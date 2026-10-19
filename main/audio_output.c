#include "audio_output.h"
#include "esp_attr.h"
#include "audio_pcm.h"
#include "audio_player.h"
#include "cs43131.h"
#include "music_library.h"
#include "sd_card.h"
#include "pmic_axp2101.h"
#include "driver/i2s_std.h"
#include "driver/gpio.h"
#include "esp_audio_simple_dec.h"
#include "esp_mp3_dec.h"
#include "esp_flac_dec.h"
#include "esp_heap_caps.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "ui/ui_common.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdio.h>
#include <string.h>
#include <stdatomic.h>
#include <unistd.h>
#include <errno.h>

#define INPUT_BYTES (32U*1024U)
#define READ_BYTES (16U*1024U)
#define SECTOR_BYTES 512U
#define IO_ALIGNMENT 64U
#define OUTPUT_MAX_BYTES (65536U*2U*4U) /* FLAC最大块65535帧，双声道32bit */
#define IRQ_GPIO 53
#define QUIESCE_REQUEST 1U
#define QUIESCE_ACK 2U
#define WORKER_BUSY 4U
#define SLEEP_REQUEST 8U
#define REQUEST_MASK (QUIESCE_REQUEST|SLEEP_REQUEST)
static const char *TAG="audio_output";
static TaskHandle_t s_task;
static portMUX_TYPE s_cmd_mux=portMUX_INITIALIZER_UNLOCKED;
typedef struct {uint32_t serial,index,generation,request;bool play;} command_t;
static command_t s_cmd={.index=UINT32_MAX};
static atomic_uint s_quiesce,s_volume=50,s_position,s_duration;
static atomic_bool s_playing,s_active,s_finished,s_detached;
static command_t s_current={.index=UINT32_MAX};
static DRAM_ATTR audio_pcm_t s_pcm; /* ISR触及的数据及ring均驻留内部SRAM */
static uint8_t *s_ring,*s_input,*s_output,*s_io;
static uint32_t s_tx_rate;
static bool s_dma_primed;
static uint64_t s_read_base;
static size_t s_read_size;
static size_t s_output_capacity;
static FILE *s_file; /* 仅在SD锁内访问，包括卸载回调；不将指针交给UI。 */
static uint64_t s_file_size,s_file_offset,s_sent_total;
static uint8_t s_flac_header[42];
static bool s_flac_prefix;
static uint32_t s_last_sent;
static esp_audio_simple_dec_handle_t s_decoder;
static esp_audio_simple_dec_raw_t s_raw;
static esp_audio_simple_dec_info_t s_format;
static size_t s_pcm_bytes,s_pcm_offset;
static bool s_input_eof,s_decode_eof,s_dac_ready,s_tx_on,s_format_ready;
static i2s_chan_handle_t s_tx;
static TickType_t s_poll_at,s_retry_at;
static bool s_have_retry,s_hp_dirty,s_hp;
/* 任务内统计只记录阶段最大经过时间，不在ISR输出日志；5秒汇总一次。
 * 区分SD等待、解码耗时和真实PCM欠载，这些耗时不等同于CPU占用率。 */
static TickType_t s_diag_at,s_read_max,s_decode_max,s_poll_max;
static uint32_t s_diag_underrun;
static void record_max(TickType_t *maximum,TickType_t start)
{
    TickType_t elapsed=xTaskGetTickCount()-start;
    if(elapsed>*maximum)*maximum=elapsed;
}
static void report_diagnostics(void)
{
    TickType_t now=xTaskGetTickCount();
    if(!s_tx_on || (TickType_t)(now-s_diag_at)<pdMS_TO_TICKS(5000))return;
    uint32_t missing=atomic_load(&s_pcm.underrun_frames);
    ESP_LOGI(TAG,"Audio: %luHz, queued=%uB, underrun=%lu frames/5s, max read/decode/poll=%lu/%lu/%lu ms",
        (unsigned long)s_format.sample_rate,(unsigned)audio_pcm_queued(&s_pcm),
        (unsigned long)(missing-s_diag_underrun),
        (unsigned long)(s_read_max*portTICK_PERIOD_MS),
        (unsigned long)(s_decode_max*portTICK_PERIOD_MS),
        (unsigned long)(s_poll_max*portTICK_PERIOD_MS));
    s_diag_underrun=missing;s_diag_at=now;
    s_read_max=s_decode_max=s_poll_max=0;
}

static command_t command(void)
{
    portENTER_CRITICAL(&s_cmd_mux);command_t c=s_cmd;portEXIT_CRITICAL(&s_cmd_mux);return c;
}
static void notify(void) {if(s_task)xTaskNotifyGive(s_task);}
void audio_output_select(uint32_t index,uint32_t generation,bool autoplay)
{
    portENTER_CRITICAL(&s_cmd_mux);
    ++s_cmd.serial;++s_cmd.request;s_cmd.index=index;s_cmd.generation=generation;s_cmd.play=autoplay && index!=UINT32_MAX;
    atomic_store(&s_position,0);atomic_store(&s_duration,0);atomic_store(&s_finished,false);
    atomic_store(&s_playing,false);
    portEXIT_CRITICAL(&s_cmd_mux);notify();
}
void audio_output_toggle(void)
{
    portENTER_CRITICAL(&s_cmd_mux);
    ++s_cmd.request;
    if(s_cmd.index!=UINT32_MAX)s_cmd.play=!s_cmd.play;
    if(s_cmd.play)atomic_store(&s_finished,false);
    portEXIT_CRITICAL(&s_cmd_mux);notify();
}
void audio_output_set_playing(bool play)
{
    portENTER_CRITICAL(&s_cmd_mux);
    ++s_cmd.request;
    s_cmd.play=play && s_cmd.index!=UINT32_MAX;
    if(s_cmd.play)atomic_store(&s_finished,false);
    portEXIT_CRITICAL(&s_cmd_mux);notify();
}
void audio_output_set_volume(uint8_t volume)
{atomic_store(&s_volume,volume>100?100:volume);notify();}
uint32_t audio_output_time_ms(void) {return atomic_load(&s_position);}
uint32_t audio_output_duration_sec(void) {return atomic_load(&s_duration);}
bool audio_output_is_play_requested(void) {return command().play;}
bool audio_output_is_playing(void) {return atomic_load(&s_playing) && command().play;}
bool audio_output_is_active(void)
{return s_task && (atomic_load(&s_active)||command().play);}
bool audio_output_take_finished(void) {return atomic_exchange(&s_finished,false);}
static bool request_quiescence(unsigned owner,bool pause)
{
    if(!s_task)return true;
    if(pause) {
        unsigned before=atomic_fetch_or(&s_quiesce,owner);notify();
        return (before&QUIESCE_ACK)!=0;
    }
    /* 只释放自己的所有权。ACK由worker以CAS撤销，避免新请求在ACK撤销与
     * 开始I/O的缝隙中误判空闲；睡眠恢复不能撤销正在执行的关机屏障。 */
    atomic_fetch_and(&s_quiesce,~owner);notify();return true;
}
bool audio_output_quiesce(bool pause) {return request_quiescence(QUIESCE_REQUEST,pause);}
bool audio_output_sleep(bool pause) {return request_quiescence(SLEEP_REQUEST,pause);}
static void IRAM_ATTR dac_irq(void *arg)
{
    (void)arg;BaseType_t wake=pdFALSE;
    if(s_task)vTaskNotifyGiveFromISR(s_task,&wake);
    if(wake)portYIELD_FROM_ISR();
}
static bool IRAM_ATTR dma_sent(i2s_chan_handle_t handle,i2s_event_data_t *event,void *arg)
{
    (void)handle;(void)arg;
    /* ISR最多搬运3840字节，不读取PSRAM/SD/I2C。达到低水位时通知生产者，
     * 满队列上的任务真正阻塞，避免taskYIELD后继续抢占低优先级任务。
     * 由I2S驱动根据返回值执行ISR调度。 */
    audio_pcm_dma(&s_pcm,event->dma_buf,event->size);
    BaseType_t wake=pdFALSE;
    if(s_task && atomic_load_explicit(&s_pcm.feed,memory_order_acquire) &&
       audio_pcm_queued(&s_pcm)<=AUDIO_PCM_LOW_BYTES)
        vTaskNotifyGiveFromISR(s_task,&wake);
    return wake==pdTRUE;
}
static void detach_file(void)
{
    if(s_file) {fclose(s_file);s_file=NULL;}
    atomic_store(&s_detached,true);
}
static bool close_file(void)
{
    if(!sd_card_io_lock(pdMS_TO_TICKS(100)))return false;
    if(s_file) {fclose(s_file);s_file=NULL;}
    sd_card_io_unlock();return true;
}
static bool card_current(void)
{
    return sd_card_is_present() && sd_card_is_mounted() &&
           sd_card_get_generation()==s_current.generation && !atomic_load(&s_detached);
}
static void update_position(void)
{
    uint32_t now=atomic_load_explicit(&s_pcm.sent,memory_order_acquire);
    s_sent_total+=(uint32_t)(now-s_last_sent);s_last_sent=now;
    if(!s_format.sample_rate)return;
    uint64_t ms=s_sent_total*1000/s_format.sample_rate;
    portENTER_CRITICAL(&s_cmd_mux);
    if(s_cmd.serial==s_current.serial)atomic_store(&s_position,ms>UINT32_MAX?UINT32_MAX:(uint32_t)ms);
    portEXIT_CRITICAL(&s_cmd_mux);
}
static void stop_tx(void)
{
    atomic_store_explicit(&s_pcm.feed,false,memory_order_release);
    if(s_tx_on) {
        /* mute ramp期间DAC继续供时钟，已经排入DMA的有效样本仍计入进度。
         * 新ring数据停止进入DMA，所以暂停后保留的PCM不会被静音消耗。 */
        esp_err_t ret=cs43131_stop();
        if(ret!=ESP_OK) {ESP_LOGW(TAG,"DAC stop: %s",esp_err_to_name(ret));cs43131_abort();s_dac_ready=false;}
        i2s_channel_disable(s_tx);s_tx_on=false;
    }
    update_position();atomic_store(&s_playing,false);atomic_store(&s_active,false);
}
static bool close_session(void)
{
    stop_tx();
    if(!close_file())return false;
    if(s_decoder) {esp_audio_simple_dec_close(s_decoder);s_decoder=NULL;}
    memset(&s_raw,0,sizeof(s_raw));memset(&s_format,0,sizeof(s_format));
    s_pcm_bytes=s_pcm_offset=0;s_file_offset=0;s_sent_total=0;s_last_sent=0;
    s_read_size=0;
    s_input_eof=s_decode_eof=s_format_ready=s_flac_prefix=false;
    s_diag_underrun=0;s_diag_at=xTaskGetTickCount();
    s_read_max=s_decode_max=s_poll_max=0;
    audio_pcm_reset(&s_pcm,s_ring);
    return true;
}
/* 只保留fLaC+STREAMINFO供解码器使用；流式跳过PICTURE/SEEKTABLE等块。
 * 元数据块可达16MiB，不能让通用parser为一张内嵌图片缓存整块。这里只用
 * 42+4字节固定存储并按文件偏移跳过，绝不改写SD文件；独立JPG照常用于UI。
 * 调用时持有SD锁；限制块数和偏移，损坏文件不能无限占锁或seek越界。 */
static esp_err_t skip_flac_metadata(void)
{
    if(fseeko(s_file,0,SEEK_SET)!=0 || fread(s_flac_header,1,42,s_file)!=42 ||
       memcmp(s_flac_header,"fLaC",4) || (s_flac_header[4]&0x7f) ||
       s_flac_header[5] || s_flac_header[6] || s_flac_header[7]!=34)return ESP_ERR_INVALID_RESPONSE;
    bool last=(s_flac_header[4]&0x80)!=0;
    uint64_t offset=42;
    for(unsigned block=0;!last && block<1024;++block) {
        uint8_t h[4];
        if(offset+4>s_file_size || fread(h,1,4,s_file)!=4)return ESP_ERR_INVALID_RESPONSE;
        uint32_t size=(uint32_t)h[1]<<16|h[2]<<8|h[3];
        if((h[0]&0x7f)==127 || offset+4+size>s_file_size)return ESP_ERR_INVALID_RESPONSE;
        offset+=4+size;last=(h[0]&0x80)!=0;
        if(fseeko(s_file,(off_t)offset,SEEK_SET)!=0)return ESP_FAIL;
    }
    if(!last)return ESP_ERR_NOT_SUPPORTED;
    s_flac_header[4]|=0x80; /* virtual stream: STREAMINFO is the last metadata block */
    s_flac_prefix=true;s_file_offset=offset;
    return ESP_OK;
}

/* MP3的APIC封面通常在ID3v2内。跳过整段tag，不让它进入解码器缓存；
 * ID3v1尾标签也不当成待解码音频。文件偏移仍是物理64位偏移，方便暂停恢复。 */
static esp_err_t skip_mp3_metadata(void)
{
    uint8_t h[10];uint64_t offset=0;
    if(fseeko(s_file,0,SEEK_SET)!=0 || fread(h,1,10,s_file)!=10)return ESP_ERR_INVALID_RESPONSE;
    if(!memcmp(h,"ID3",3)) {
        if((h[6]|h[7]|h[8]|h[9])&0x80)return ESP_ERR_INVALID_RESPONSE;
        offset=10+((uint32_t)h[6]<<21|h[7]<<14|h[8]<<7|h[9]);
        if(h[3]==4 && (h[5]&0x10))offset+=10;
    }
    if(s_file_size>=128 && fseeko(s_file,(off_t)(s_file_size-128),SEEK_SET)==0 &&
       fread(h,1,3,s_file)==3 && !memcmp(h,"TAG",3))s_file_size-=128;
    if(offset>=s_file_size)return ESP_ERR_INVALID_RESPONSE;
    if(fseeko(s_file,(off_t)offset,SEEK_SET)!=0)return ESP_FAIL;
    s_file_offset=offset;return ESP_OK;
}

static esp_err_t open_session(command_t c)
{
    if(!sd_card_io_lock(pdMS_TO_TICKS(100)))return ESP_ERR_TIMEOUT;
    esp_err_t ret=ESP_ERR_INVALID_STATE;
    music_item_t item;char path[MUSIC_PATH_MAX];audio_file_info_t info={0};
    if(sd_card_is_present() && sd_card_is_mounted() && sd_card_get_generation()==c.generation) {
        ret=music_library_get_item(c.index,&item);
        if(ret==ESP_OK)ret=music_library_make_path(&item,item.audio,path,sizeof(path));
        if(ret==ESP_OK)ret=audio_player_probe(path,&info);
        if(ret==ESP_OK && info.is_flac && (info.channels>2 ||
           (info.bits_per_sample!=16 && info.bits_per_sample!=24 && info.bits_per_sample!=32)))
            ret=ESP_ERR_NOT_SUPPORTED;
        if(ret==ESP_OK) {
            s_file=fopen(path,"rb");
            if(!s_file)ret=ESP_FAIL;
            else {
                /* 先禁用stdio预读，后续read(fileno)与定位共享同一物理位置。
                 * 热路径按扇区边界读入独立DMA staging，不让parser余量影响对齐。 */
                setvbuf(s_file,NULL,_IONBF,0);
                s_file_size=info.file_size;s_file_offset=0;
                ret=info.is_flac?skip_flac_metadata():skip_mp3_metadata();
                if(!c.play || ret!=ESP_OK) {fclose(s_file);s_file=NULL;}
            }
        }
    }
    sd_card_io_unlock();
    if(ret!=ESP_OK)return ret;
    s_current=c;
    atomic_store(&s_detached,false);
    esp_audio_simple_dec_cfg_t cfg={.dec_type=info.is_flac?ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC:ESP_AUDIO_SIMPLE_DEC_TYPE_MP3};
    esp_audio_err_t err=esp_audio_simple_dec_open(&cfg,&s_decoder);
    if(err!=ESP_AUDIO_ERR_OK)return ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&s_cmd_mux);
    if(s_cmd.serial==c.serial) {
        atomic_store(&s_duration,(uint32_t)(info.duration_ms/1000));
        atomic_store(&s_position,0);
    }
    portEXIT_CRITICAL(&s_cmd_mux);
    return ESP_OK;
}
static esp_err_t read_input(void)
{
    if(s_input_eof)return ESP_OK;
    if(!sd_card_io_lock(pdMS_TO_TICKS(5)))return ESP_ERR_TIMEOUT;
    esp_err_t ret=ESP_OK;
    if(!card_current())ret=ESP_ERR_INVALID_STATE;
    if(ret==ESP_OK && !s_file) {
        /* 普通暂停/关机失败恢复后，从已读取的64位文件偏移接续；解码器和
         * 未消费的压缩数据保留，不近似跳转MP3帧或丢失FLAC状态。 */
        music_item_t item;char path[MUSIC_PATH_MAX];
        ret=music_library_get_item(s_current.index,&item);
        if(ret==ESP_OK)ret=music_library_make_path(&item,item.audio,path,sizeof(path));
        if(ret==ESP_OK) {
            s_file=fopen(path,"rb");
            if(!s_file)ret=ESP_FAIL;
            else setvbuf(s_file,NULL,_IONBF,0);
        }
    }
    if(ret==ESP_OK) {
        if(s_raw.len)memmove(s_input,s_raw.buffer,s_raw.len);
        if(s_flac_prefix) {
            if(s_raw.len+sizeof(s_flac_header)>INPUT_BYTES) {sd_card_io_unlock();return ESP_ERR_INVALID_SIZE;}
            memcpy(s_input+s_raw.len,s_flac_header,sizeof(s_flac_header));
            s_raw.len+=sizeof(s_flac_header);s_flac_prefix=false;
        }
        /* ID3/FLAC帧通常不在扇区边界；直接读到parser的非对齐PSRAM会触发
         * v5.5.4 sdmmc_read_sectors()逐扇区临时DMA中转。物理位置向下对齐，
         * 一次读16KiB到64字节对齐的内部DMA块，再复制逻辑音频范围。
         * 重读的扇区头最多511字节；尾标签和扇区补齐数据绝不交给解码器。
         * parser输入固定32KiB，staging固定16KiB，大文件不增加内存占用。 */
        while(s_raw.len<INPUT_BYTES && s_file_offset<s_file_size) {
            /* 保留本次没有用完的staging尾部，避免32KiB输入只差几字节时
             * 又读取整个16KiB，也避免下轮重读这些已经取到的扇区。 */
            if(!s_read_size || s_file_offset<s_read_base ||
               s_file_offset-s_read_base>=s_read_size) {
                uint64_t base=s_file_offset&~(uint64_t)(SECTOR_BYTES-1);
                uint64_t remaining=s_file_size-base;
                size_t amount=remaining>=READ_BYTES?READ_BYTES:
                    ((size_t)remaining+SECTOR_BYTES-1)&~(size_t)(SECTOR_BYTES-1);
                if(fseeko(s_file,(off_t)base,SEEK_SET)!=0) {ret=ESP_FAIL;break;}
                ssize_t n;
                do {n=read(fileno(s_file),s_io,amount);} while(n<0 && errno==EINTR);
                if(n<=0 || (size_t)n<=s_file_offset-base) {ret=ESP_FAIL;break;}
                s_read_base=base;s_read_size=(size_t)n;
            }
            size_t skip=(size_t)(s_file_offset-s_read_base);
            size_t take=s_read_size-skip;
            if(take>INPUT_BYTES-s_raw.len)take=INPUT_BYTES-s_raw.len;
            if(take>s_file_size-s_file_offset)take=(size_t)(s_file_size-s_file_offset);
            memcpy(s_input+s_raw.len,s_io+skip,take);
            s_file_offset+=take;s_raw.len+=take;
        }
        s_input_eof=s_file_offset>=s_file_size;
        s_raw.buffer=s_input;s_raw.eos=s_input_eof;
    }
    sd_card_io_unlock();return ret;
}
static bool supported_rate(uint32_t rate)
{
    return rate==32000 || rate==44100 || rate==48000 || rate==88200 ||
           rate==96000 || rate==176400 || rate==192000;
}
/* Create I2S/GDMA only when PCM is first prepared, so boot SD scanning does not
 * overlap the driver allocation. The created channel remains shared by later songs. */
static esp_err_t create_tx(uint32_t sample_rate)
{
    if(s_tx)return ESP_OK;
    i2s_chan_config_t channel=I2S_CHANNEL_DEFAULT_CONFIG(I2S_NUM_AUTO,I2S_ROLE_SLAVE);
    channel.dma_desc_num=AUDIO_DMA_COUNT;channel.dma_frame_num=AUDIO_DMA_FRAMES;
    channel.auto_clear_before_cb=false;channel.auto_clear_after_cb=false;
    esp_err_t ret=i2s_new_channel(&channel,&s_tx,NULL);
    if(ret!=ESP_OK)return ret;
    i2s_std_config_t std={
        .clk_cfg=I2S_STD_CLK_DEFAULT_CONFIG(sample_rate),
        .slot_cfg=I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_32BIT,I2S_SLOT_MODE_STEREO),
        .gpio_cfg={.mclk=I2S_GPIO_UNUSED,.bclk=51,.ws=50,.dout=49,.din=I2S_GPIO_UNUSED},
    };
    ret=i2s_channel_init_std_mode(s_tx,&std);
    if(ret!=ESP_OK) {
        /* init_std_mode may have allocated partial DMA/mode resources. Delete the
         * channel and clear the handle so the next play request can create a clean
         * channel instead of reconfiguring a half-initialized handle. */
        i2s_del_channel(s_tx);
        s_tx=NULL;
        s_tx_rate=0;
        return ret;
    }
    i2s_event_callbacks_t callbacks={.on_sent=dma_sent};
    ret=i2s_channel_register_event_callback(s_tx,&callbacks,NULL);
    if(ret!=ESP_OK) {
        i2s_del_channel(s_tx);
        s_tx=NULL;
        s_tx_rate=0;
        return ret;
    }
    s_tx_rate=sample_rate;
    return ESP_OK;
}
static esp_err_t configure_tx(void)
{
    if(!supported_rate(s_format.sample_rate) ||
       (s_format.bits_per_sample!=16 && s_format.bits_per_sample!=24 && s_format.bits_per_sample!=32) ||
       (s_format.channel!=1 && s_format.channel!=2))return ESP_ERR_NOT_SUPPORTED;
    esp_err_t ret=create_tx(s_format.sample_rate);
    if(ret!=ESP_OK)return ret;
    if(s_tx_rate!=s_format.sample_rate) {
        i2s_std_clk_config_t clock=I2S_STD_CLK_DEFAULT_CONFIG(s_format.sample_rate);
        ret=i2s_channel_reconfig_std_clock(s_tx,&clock);
        if(ret!=ESP_OK)return ret;
        s_tx_rate=s_format.sample_rate;
    }
    ESP_LOGI(TAG,"PCM %luHz %ubit %uch ->32bit stereo, DAC master/P4 slave",
        (unsigned long)s_format.sample_rate,s_format.bits_per_sample,s_format.channel);
    return ESP_OK;
}
/* 保留通道也保留了DMA存储。每次启动先用静音覆盖全部8块，避免上一首
 * 残留样本在新的DMA回调补齐前发出；不能直接清空含待解码数据的s_input。
 * s_io仅为同步读取的staging，此处无SD读取正在进行，可借用其前3840字节。 */
static esp_err_t clear_dma(void)
{
    if(s_dma_primed)return ESP_OK;
    const size_t block=AUDIO_DMA_FRAMES*8U;
    s_read_size=0; /* 下面借用staging，物理文件偏移仍保留，之后可重新读取。 */
    memset(s_io,0,block);
    for(unsigned i=0;i<AUDIO_DMA_COUNT;++i) {
        size_t loaded=0;
        esp_err_t ret=i2s_channel_preload_data(s_tx,s_io,block,&loaded);
        if(ret!=ESP_OK)return ret;
        if(loaded!=block)return ESP_ERR_INVALID_SIZE;
    }
    s_dma_primed=true;
    return ESP_OK;
}
static esp_err_t start_tx(void)
{
    command_t latest=command();
    if(latest.serial!=s_current.serial || !latest.play)return ESP_OK;
    atomic_store(&s_active,true);
    esp_err_t ret=cs43131_prepare(s_format.sample_rate);
    if(ret==ESP_OK)ret=clear_dma();
    if(ret==ESP_OK)ret=i2s_channel_enable(s_tx);
    if(ret==ESP_OK) {
        s_dma_primed=false; /* enable之后，disable会重置SDK的preload游标。 */
        s_tx_on=true;
        ret=cs43131_start_hp();
    }
    latest=command();
    if(ret==ESP_OK && (latest.serial!=s_current.serial || !latest.play)) {stop_tx();return ESP_OK;}
    if(ret==ESP_OK) {
        atomic_store_explicit(&s_pcm.feed,true,memory_order_release);
        unsigned volume=atomic_load(&s_volume);
        ret=cs43131_volume(volume,volume==0);
    }
    if(ret!=ESP_OK) {
        cs43131_abort();s_dac_ready=false;
        if(s_tx_on) {i2s_channel_disable(s_tx);s_tx_on=false;}
        atomic_store(&s_active,false);return ret;
    }
    portENTER_CRITICAL(&s_cmd_mux);
    if(s_cmd.serial==s_current.serial && s_cmd.play)atomic_store(&s_playing,true);
    portEXIT_CRITICAL(&s_cmd_mux);
    s_diag_at=xTaskGetTickCount();
    return ESP_OK;
}
static esp_err_t decode_step(bool *progress)
{
    *progress=false;
    if(s_pcm_offset<s_pcm_bytes) {
        size_t frame_bytes=s_format.channel*(s_format.bits_per_sample/8);
        size_t frames=(s_pcm_bytes-s_pcm_offset)/frame_bytes;
        size_t pushed=audio_pcm_push(&s_pcm,s_output+s_pcm_offset,frames,s_format.bits_per_sample,s_format.channel);
        s_pcm_offset+=pushed*frame_bytes;*progress=pushed!=0;
        return ESP_OK;
    }
    /* 带eos=true的最后输入交给parser，全部消费且输出送入ring后再结束。 */
    if(!s_raw.len && s_input_eof)s_decode_eof=true;
    if(s_decode_eof) {
        if(!s_format_ready)return ESP_ERR_INVALID_RESPONSE;
        if(audio_pcm_drained(&s_pcm)) {
            stop_tx();update_position();
            portENTER_CRITICAL(&s_cmd_mux);
            if(s_cmd.serial==s_current.serial && s_cmd.play) {
                s_cmd.play=false;atomic_store(&s_finished,true);
                atomic_store(&s_duration,(uint32_t)(s_sent_total/s_format.sample_rate));
            }
            portEXIT_CRITICAL(&s_cmd_mux);
        }
        return ESP_OK;
    }
    if(!s_raw.len && !s_input_eof) {
        TickType_t before=xTaskGetTickCount();
        esp_err_t r=read_input();record_max(&s_read_max,before);
        if(r!=ESP_OK)return r;
    }
    s_raw.consumed=0;
    esp_audio_simple_dec_out_t out={.buffer=s_output,.len=s_output_capacity};
    TickType_t before=xTaskGetTickCount();
    esp_audio_err_t err=esp_audio_simple_dec_process(s_decoder,&s_raw,&out);
    record_max(&s_decode_max,before);
    if(err==ESP_AUDIO_ERR_BUFF_NOT_ENOUGH) {
        if(out.needed_size<=s_output_capacity || out.needed_size>OUTPUT_MAX_BYTES)return ESP_ERR_INVALID_SIZE;
        uint8_t *grown=heap_caps_realloc(s_output,out.needed_size,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
        if(!grown)return ESP_ERR_NO_MEM;
        s_output=grown;s_output_capacity=out.needed_size;*progress=true;return ESP_OK;
    }
    if(err!=ESP_AUDIO_ERR_OK)return ESP_FAIL;
    if(s_raw.consumed>s_raw.len || out.decoded_size>s_output_capacity)return ESP_ERR_INVALID_RESPONSE;
    s_raw.buffer+=s_raw.consumed;s_raw.len-=s_raw.consumed;
    if(out.decoded_size) {
        esp_audio_simple_dec_info_t format={0};
        if(esp_audio_simple_dec_get_info(s_decoder,&format)!=ESP_AUDIO_ERR_OK)return ESP_FAIL;
        if(!s_format_ready) {
            s_format=format;esp_err_t r=configure_tx();if(r!=ESP_OK)return r;
            s_format_ready=true;
        } else if(format.sample_rate!=s_format.sample_rate || format.channel!=s_format.channel ||
                  format.bits_per_sample!=s_format.bits_per_sample)return ESP_ERR_NOT_SUPPORTED;
        if(out.decoded_size%(s_format.channel*(s_format.bits_per_sample/8)))return ESP_ERR_INVALID_SIZE;
        s_pcm_bytes=out.decoded_size;s_pcm_offset=0;
    }
    *progress=s_raw.consumed||out.decoded_size;
    if(!*progress) {
        if(s_input_eof && !s_raw.len) {s_decode_eof=true;return ESP_OK;}
        if(!s_input_eof && s_raw.len<INPUT_BYTES) {
            before=xTaskGetTickCount();
            esp_err_t r=read_input();record_max(&s_read_max,before);return r;
        }
        return ESP_ERR_INVALID_RESPONSE; /* malformed data must not busy-loop */
    }
    return ESP_OK;
}
static void fail_current(esp_err_t ret,command_t failed)
{
    ESP_LOGE(TAG,"Playback failed: %s (index=%lu)",esp_err_to_name(ret),(unsigned long)s_current.index);
    close_session();
    portENTER_CRITICAL(&s_cmd_mux);
    /* SD/I2C/静音停止期间仍可收到同一首歌的新播放操作。只撤销产生该
     * 错误的请求版本，不丢弃用户在阻塞期间新提交的播放/暂停意图。 */
    if(s_cmd.serial==s_current.serial && s_cmd.request==failed.request)s_cmd.play=false;
    portEXIT_CRITICAL(&s_cmd_mux);
}
static void poll_headphone(void)
{
    TickType_t now=xTaskGetTickCount();
    TickType_t elapsed=now-s_poll_at;
    if(elapsed<pdMS_TO_TICKS(20) || (elapsed<pdMS_TO_TICKS(100) && gpio_get_level(IRQ_GPIO)))return;
    s_poll_at=now;bool plugged;
    command_t polled=command();
    TickType_t before=xTaskGetTickCount();
    esp_err_t ret=cs43131_poll(&plugged);record_max(&s_poll_max,before);
    if(ret==ESP_OK) {
        if(plugged!=s_hp) {s_hp=plugged;s_hp_dirty=true;}
    } else if(s_tx_on)fail_current(ret,polled);
    /* UI只是状态观察者：锁忙时延后发布；不在锁内读I2C或调整布局。 */
    if(s_hp_dirty && lvgl_port_lock(1)) {
        ui_get_device_state()->headphone_inserted=s_hp;
        s_hp_dirty=false;lvgl_port_unlock();
    }
}
static void output_step(void)
{
    if(!s_dac_ready) {
        TickType_t now=xTaskGetTickCount();
        if(pmic_axp2101_outputs_ready() && (!s_have_retry || (TickType_t)(now-s_retry_at)>=pdMS_TO_TICKS(1000))) {
            s_retry_at=now;s_have_retry=true;
            esp_err_t ret=cs43131_init();
            if(ret==ESP_OK) {s_dac_ready=true;s_hp_dirty=true;}
            else ESP_LOGW(TAG,"DAC init deferred: %s",esp_err_to_name(ret));
        }
        if(!s_dac_ready) {ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));return;}
    }
    /* 低水位时优先补PCM；I2C耳机轮询等音频储备充足后再执行。
     * IRQ仍能通知任务；首次初始化和暂停时不受水位限制。 */
    if(!s_tx_on || audio_pcm_queued(&s_pcm)>=AUDIO_PCM_LOW_BYTES)poll_headphone();
    update_position();report_diagnostics();
    command_t c=command();
    if(c.serial!=s_current.serial) {
        if(!close_session()) {ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(20));return;}
        s_current=c;
        if(c.index!=UINT32_MAX) {
            esp_err_t ret=open_session(c);
            if(ret==ESP_ERR_TIMEOUT) {s_current.serial=c.serial-1;return;}
            if(ret!=ESP_OK) {fail_current(ret,c);return;}
        }
    }
    if(s_decoder && !card_current()) {fail_current(ESP_ERR_INVALID_STATE,c);close_session();return;}
    if(c.play && !s_decoder && c.index!=UINT32_MAX) {
        esp_err_t ret=open_session(c);
        if(ret==ESP_ERR_TIMEOUT)return;
        if(ret!=ESP_OK) {fail_current(ret,c);return;}
    }
    command_t latest=command();
    if(latest.serial!=c.serial || latest.play!=c.play)return;
    if(!c.play || !s_decoder) {
        if(s_tx_on)stop_tx();
        if(s_file)close_file();
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(50));return;
    }
    if(s_decode_eof && !s_tx_on && audio_pcm_drained(&s_pcm)) {
        /* 文件自然结束后再次点击播放：重新打开同一首；不复用EOF解码器。 */
        audio_output_select(c.index,c.generation,true);return;
    }
    size_t queued=audio_pcm_queued(&s_pcm);
    /* 冷启动、切歌和暂停恢复先积累48KiB，再打开DAC。短歌曲在EOF时
     * 允许不足此水位启动，不能因达不到预缓冲量永远不播放。 */
    if(!s_tx_on && queued && (queued>=AUDIO_PCM_HIGH_BYTES || s_decode_eof)) {
        esp_err_t ret=start_tx();if(ret!=ESP_OK) {fail_current(ret,c);return;}
    }
    if(s_tx_on) {
        static unsigned applied=UINT32_MAX;
        unsigned volume=atomic_load(&s_volume);
        if(volume!=applied) {
            esp_err_t ret=cs43131_volume(volume,volume==0);
            if(ret!=ESP_OK) {fail_current(ret,c);return;}
            applied=volume;
        }
    }
    /* 高低水位滞回：储备充足时释放CPU，DMA消耗到半满后再补PCM。
     * 通知带计数，判断与阻塞之间到达的DMA/选歌/关机通知不会丢失。
     * taskYIELD只让同优先级任务运行，不能替代这里的阻塞。 */
    if(s_tx_on && queued>=AUDIO_PCM_HIGH_BYTES) {
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));return;
    }
    bool progress;esp_err_t ret=decode_step(&progress);
    if(ret!=ESP_OK && ret!=ESP_ERR_TIMEOUT) {fail_current(ret,c);return;}
    if(!progress || ret==ESP_ERR_TIMEOUT)
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(2));

}
static void output_iteration(void)
{
    unsigned state=atomic_load(&s_quiesce);
    if(state&REQUEST_MASK) {
        if(!(state&QUIESCE_ACK)) {
            atomic_fetch_or(&s_quiesce,WORKER_BUSY);
            stop_tx();bool closed=close_file();
            atomic_fetch_and(&s_quiesce,~WORKER_BUSY);
            unsigned expected=atomic_load(&s_quiesce);
            if(closed && (expected&REQUEST_MASK))
                atomic_compare_exchange_strong(&s_quiesce,&expected,expected|QUIESCE_ACK);
        }
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(20));
        return;
    }
    if(state==QUIESCE_ACK) {
        atomic_compare_exchange_strong(&s_quiesce,&state,0);return;
    }
    /* 请求/ACK/BUSY共用一个CAS入口。ACK发布后不再开始任何I2C/SD/DMA，
     * 直到调用者显式解除屏障；不能用两个bool判断空闲以免双核睡眠竞态。 */
    unsigned expected=0;
    if(!atomic_compare_exchange_strong(&s_quiesce,&expected,WORKER_BUSY))return;
    output_step();
    atomic_fetch_and(&s_quiesce,~WORKER_BUSY);
}
static void output_task(void *arg)
{
    (void)arg;
    for(;;)output_iteration();
}
esp_err_t audio_output_init(void)
{
    if(s_task)return ESP_ERR_INVALID_STATE;
    esp_err_t ret=ESP_ERR_NO_MEM;
    bool hook_added=false,irq_added=false;
    s_io=heap_caps_aligned_alloc(IO_ALIGNMENT,READ_BYTES,
        MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT);
    s_ring=heap_caps_malloc(AUDIO_RING_BYTES,MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT);
    s_input=heap_caps_malloc(INPUT_BYTES,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    s_output_capacity=32*1024;
    s_output=heap_caps_malloc(s_output_capacity,MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT);
    if(!s_io || !s_ring || !s_input || !s_output)goto fail;
    audio_pcm_reset(&s_pcm,s_ring);
    if(esp_mp3_dec_register()!=ESP_AUDIO_ERR_OK || esp_flac_dec_register()!=ESP_AUDIO_ERR_OK) {
        ret=ESP_ERR_NOT_SUPPORTED;goto fail;
    }
    ret=sd_card_set_audio_detach_callback(detach_file);
    if(ret!=ESP_OK)goto fail;
    hook_added=true;
    gpio_config_t irq={.pin_bit_mask=1ULL<<IRQ_GPIO,.mode=GPIO_MODE_INPUT,
        .pull_up_en=GPIO_PULLUP_ENABLE,.intr_type=GPIO_INTR_NEGEDGE};
    ret=gpio_config(&irq);if(ret!=ESP_OK)goto fail;
    ret=gpio_isr_handler_add(IRQ_GPIO,dac_irq,NULL);if(ret!=ESP_OK)goto fail;
    irq_added=true;
    if(xTaskCreatePinnedToCore(output_task,"audio_out",24576,NULL,5,&s_task,0)!=pdPASS) {
        ret=ESP_ERR_NO_MEM;goto fail;
    }
    return ESP_OK;
fail:
    if(irq_added)gpio_isr_handler_remove(IRQ_GPIO);
    if(hook_added)sd_card_set_audio_detach_callback(NULL);
    if(s_tx) {i2s_del_channel(s_tx);s_tx=NULL;}
    heap_caps_free(s_io);heap_caps_free(s_ring);heap_caps_free(s_input);heap_caps_free(s_output);
    s_io=s_ring=s_input=s_output=NULL;return ret;
}
