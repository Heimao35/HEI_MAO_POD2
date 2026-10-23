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
#include "audio_mp3.h"
#include "bt_audio.h"
#include "bt_audio_stream.h"
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
static bool s_flac_prefix,s_mp3_session;
static uint32_t s_last_sent;
static esp_audio_simple_dec_handle_t s_decoder;
static esp_audio_simple_dec_raw_t s_raw;
static esp_audio_simple_dec_info_t s_format;
static size_t s_pcm_bytes,s_pcm_offset;
static bool s_input_eof,s_decode_eof,s_dac_ready,s_tx_on,s_format_ready;
static i2s_chan_handle_t s_tx;
static TickType_t s_poll_at,s_retry_at;
static bool s_have_retry,s_hp_dirty,s_hp;
/* 视频 PCM 模式：共用同一 I2S/DAC/ring，但生产者是视频 worker。
 * s_video_cmd 与音乐命令共用 s_cmd_mux；serial 只用于 open/close，
 * play/flush/eof 用独立原子量，避免频繁重配 I2S。 */
typedef struct {uint32_t serial;bool active,play;uint32_t rate;uint8_t channels,bits;} video_command_t;
static video_command_t s_video_cmd;
static uint32_t s_video_serial;              /* worker 已处理的 serial */
static bool s_video_worker_active;           /* worker-only：stop_tx 不更新音乐进度 */
static TickType_t s_video_diag_at;           /* worker-only 5s 视频PCM诊断 */
static unsigned s_video_volume=UINT32_MAX;   /* worker-only 音量缓存 */
static atomic_uint s_video_ack;              /* worker 已回应到 serial */
static atomic_bool s_video_failed;           /* 本会话配置失败 */
static atomic_bool s_video_eof_req;
static atomic_bool s_video_started;          /* TX 已启动 */
static atomic_uint s_video_flush_req,s_video_flush_ack;
static atomic_uint s_video_rate_pub,s_video_frame_bytes_pub,s_video_bits_pub,s_video_ch_pub;
/* 只在音频任务中计时、每 5s 汇总，ISR 仅维护内部 SRAM 中的无锁计数。
 * max 是单次最大墙钟耗时（包括抢占），work 是本窗口各阶段累计墙钟耗时；
 * decoded 是这些解码调用产出的音频时长，不能把单次 67ms 直接当成实时率。
 * 例如一次产出 8192 帧/96kHz = 85.3ms，67ms 解码本身仍快于实时，但需要
 * 足够储备覆盖这个突发；若 work 长期超过 decoded，再考虑解码器吞吐问题。 */
static TickType_t s_diag_at,s_read_max,s_decode_max,s_push_max,s_step_max,s_poll_max;
static uint64_t s_read_ticks,s_decode_ticks,s_push_ticks,s_decoded_frames;
static uint32_t s_diag_underrun,s_decode_peak_frames;
/* A2DP 已连接时本机 DAC 静音，但 I2S/DMA 继续运行以维持视频主时钟和
 * 音乐进度；蓝牙断开后恢复用户音量。 */
static unsigned dac_volume(void)
{
    return bt_audio_is_connected()?0:atomic_load(&s_volume);
}
static TickType_t record_max(TickType_t *maximum,TickType_t start)
{
    TickType_t elapsed=xTaskGetTickCount()-start;
    if(elapsed>*maximum)*maximum=elapsed;
    return elapsed;
}
static void reset_diagnostics(void)
{
    s_diag_at=xTaskGetTickCount();
    s_diag_underrun=atomic_load(&s_pcm.underrun_frames);
    atomic_store(&s_pcm.min_queued_bytes,AUDIO_RING_BYTES);
    s_read_max=s_decode_max=s_push_max=s_step_max=s_poll_max=0;
    s_read_ticks=s_decode_ticks=s_push_ticks=s_decoded_frames=0;
    s_decode_peak_frames=0;
    audio_mp3_take_stats();
}
static void report_diagnostics(void)
{
    TickType_t now=xTaskGetTickCount();
    if(!s_tx_on || (TickType_t)(now-s_diag_at)<pdMS_TO_TICKS(5000))return;
    uint32_t missing=atomic_load(&s_pcm.underrun_frames);
    unsigned minimum=atomic_exchange(&s_pcm.min_queued_bytes,AUDIO_RING_BYTES);
    ESP_LOGI(TAG,"Audio: %luHz, queued=%uB, min=%uB, underrun=%lu frames/%lums, max read/decode/push/step/poll=%lu/%lu/%lu/%lu/%lu ms",
        (unsigned long)s_format.sample_rate,(unsigned)audio_pcm_queued(&s_pcm),minimum,
        (unsigned long)(missing-s_diag_underrun),
        (unsigned long)((now-s_diag_at)*portTICK_PERIOD_MS),
        (unsigned long)(s_read_max*portTICK_PERIOD_MS),
        (unsigned long)(s_decode_max*portTICK_PERIOD_MS),
        (unsigned long)(s_push_max*portTICK_PERIOD_MS),
        (unsigned long)(s_step_max*portTICK_PERIOD_MS),
        (unsigned long)(s_poll_max*portTICK_PERIOD_MS));
    ESP_LOGI(TAG,"Audio work: read/decode/push=%lu/%lu/%lu ms, decoded=%lu ms, peak decode=%lu frames/%lu ms",
        (unsigned long)(s_read_ticks*portTICK_PERIOD_MS),
        (unsigned long)(s_decode_ticks*portTICK_PERIOD_MS),
        (unsigned long)(s_push_ticks*portTICK_PERIOD_MS),
        (unsigned long)(s_decoded_frames*1000/s_format.sample_rate),
        (unsigned long)s_decode_peak_frames,(unsigned long)(s_decode_max*portTICK_PERIOD_MS));
    audio_mp3_stats_t mp3=audio_mp3_take_stats();
    if(s_mp3_session)ESP_LOGI(TAG,"MP3 core: calls=%lu, work=%lu ms, max=%lu ms",
        (unsigned long)mp3.calls,(unsigned long)(mp3.core_ticks*portTICK_PERIOD_MS),
        (unsigned long)(mp3.max_core_ticks*portTICK_PERIOD_MS));
    s_diag_underrun=missing;s_diag_at=now;
    s_read_max=s_decode_max=s_push_max=s_step_max=s_poll_max=0;
    s_read_ticks=s_decode_ticks=s_push_ticks=s_decoded_frames=0;
    s_decode_peak_frames=0;
}

static command_t command(void)
{
    portENTER_CRITICAL(&s_cmd_mux);command_t c=s_cmd;portEXIT_CRITICAL(&s_cmd_mux);return c;
}
static video_command_t video_command(void)
{
    portENTER_CRITICAL(&s_cmd_mux);
    video_command_t v=s_video_cmd;
    portEXIT_CRITICAL(&s_cmd_mux);
    return v;
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
    atomic_store(&s_finished,false);
    portEXIT_CRITICAL(&s_cmd_mux);notify();
}
void audio_output_set_playing(bool play)
{
    portENTER_CRITICAL(&s_cmd_mux);
    ++s_cmd.request;
    s_cmd.play=play && s_cmd.index!=UINT32_MAX;
    atomic_store(&s_finished,false);
    portEXIT_CRITICAL(&s_cmd_mux);notify();
}
void audio_output_set_volume(uint8_t volume)
{
    volume=volume>100?100:volume;
    atomic_store(&s_volume,volume);
    /* 用户音量同时作为 A2DP 绝对音量意图保存；BT 未连接时只记录，
     * 连接成功后由 bt_audio 补发。 */
    bt_audio_set_volume(volume,volume==0);
    notify();
}
uint32_t audio_output_time_ms(void) {return atomic_load(&s_position);}
uint32_t audio_output_duration_sec(void) {return atomic_load(&s_duration);}
bool audio_output_is_play_requested(void) {return command().play;}
bool audio_output_is_playing(void) {return atomic_load(&s_playing) && command().play;}
bool audio_output_is_active(void)
{
    /* 自然结束等待续播的事件也需要 CPU；显式播放/暂停/选歌均清除此
     * 事件。不能在媒体 worker 尚未接力前误把曲间空隙当成空闲睡眠。 */
    return s_task && (atomic_load(&s_active)||command().play||atomic_load(&s_finished));
}
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
    /* ISR最多搬运2048字节，不读取PSRAM/SD/I2C。达到低水位时通知生产者，
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
    /* 暂停/停止时清空蓝牙侧残留 PCM，恢复播放不能从旧位置继续。 */
    bt_stream_reset();
    if(s_tx_on) {
        /* mute ramp期间DAC继续供时钟，已经排入DMA的有效样本仍计入进度。
         * 新ring数据停止进入DMA，所以暂停后保留的PCM不会被静音消耗。 */
        esp_err_t ret=cs43131_stop();
        if(ret!=ESP_OK) {ESP_LOGW(TAG,"DAC stop: %s",esp_err_to_name(ret));cs43131_abort();s_dac_ready=false;}
        i2s_channel_disable(s_tx);s_tx_on=false;
    }
    /* 视频会话的帧数不属于音乐进度，不能改写音乐进度条/续播位置。 */
    if(!s_video_worker_active) update_position();
    atomic_store(&s_playing,false);atomic_store(&s_active,false);
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
    audio_pcm_reset(&s_pcm,s_ring);
    bt_stream_reset();
    reset_diagnostics();
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
    s_mp3_session=!info.is_flac;
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
 * s_io仅为同步读取的staging，此处无SD读取正在进行，可借用其前2048字节。 */
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
        unsigned volume=dac_volume();
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
    reset_diagnostics();
    return ESP_OK;
}
/* 解码结果一旦可用就交给 ring，避免慢解码返回后先绕回 I2C/日志/UI
 * 轮询一圈。未放入的尾部继续保存在有上限的 PSRAM 输出块，绝不整曲缓存。 */
static size_t push_decoded(void)
{
    size_t frame_bytes=s_format.channel*(s_format.bits_per_sample/8);
    size_t frames=(s_pcm_bytes-s_pcm_offset)/frame_bytes;
    const uint8_t *source=s_output+s_pcm_offset;
    TickType_t before=xTaskGetTickCount();
    size_t pushed=audio_pcm_push(&s_pcm,source,frames,
                                 s_format.bits_per_sample,s_format.channel);
    s_push_ticks+=record_max(&s_push_max,before);
    /* 仅蓝牙已连接时复制一份到重采样/发送缓冲；本机 I2S ring 仍是
     * 主路径，蓝牙拥塞不会反压或改变解码节奏。 */
    if(pushed && bt_audio_is_connected())
        bt_stream_write(source,pushed,s_format.bits_per_sample,
                        s_format.channel,s_format.sample_rate);
    s_pcm_offset+=pushed*frame_bytes;
    return pushed;
}
static esp_err_t decode_step(bool *progress)
{
    *progress=false;
    if(s_pcm_offset<s_pcm_bytes) {
        *progress=push_decoded()!=0;
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
        esp_err_t r=read_input();s_read_ticks+=record_max(&s_read_max,before);
        if(r!=ESP_OK)return r;
    }
    s_raw.consumed=0;
    esp_audio_simple_dec_out_t out={.buffer=s_output,.len=s_output_capacity};
    TickType_t before=xTaskGetTickCount();
    esp_audio_err_t err=esp_audio_simple_dec_process(s_decoder,&s_raw,&out);
    TickType_t elapsed=xTaskGetTickCount()-before;
    s_decode_ticks+=elapsed;
    bool peak=elapsed>=s_decode_max;
    if(peak) {s_decode_max=elapsed;s_decode_peak_frames=0;}
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
        size_t frames=out.decoded_size/(s_format.channel*(s_format.bits_per_sample/8));
        s_decoded_frames+=frames;
        if(peak)s_decode_peak_frames=frames;
        s_pcm_bytes=out.decoded_size;s_pcm_offset=0;
        push_decoded();
    }
    *progress=s_raw.consumed||out.decoded_size;
    if(!*progress) {
        if(s_input_eof && !s_raw.len) {s_decode_eof=true;return ESP_OK;}
        if(!s_input_eof && s_raw.len<INPUT_BYTES) {
            before=xTaskGetTickCount();
            esp_err_t r=read_input();s_read_ticks+=record_max(&s_read_max,before);return r;
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
        if(plugged!=s_hp) {
            bool unplugged=s_hp && !plugged;
            s_hp=plugged;s_hp_dirty=true;
            /* 只响应成功读取后的“已插入 -> 已拔出”，初次检测为未插入
             * 不算拔出事件。复用明确暂停请求，撤销正在播放或等待准备的
             * 意图；本轮 output_step 随后走原暂停路径，保留解码器/进度。
             * 必须在 LVGL 锁外处理，界面繁忙不能推迟暂停。插入只更新
             * 状态栏，绝不恢复播放，也不唤醒屏幕或展开任何控件。 */
            if(unplugged) {
                if(bt_audio_is_connected()) {
                    /* 蓝牙优先于耳机：拔出只更新状态栏，暂停/恢复本机
                     * DAC 交给蓝牙连接状态；A2DP 继续输出。 */
                    ESP_LOGI(TAG,"Headphone removed; Bluetooth output continues");
                } else if(video_command().active) {
                    /* 视频音轨同样遵守拔出暂停；视频 worker 通过
                     * audio_output_video_play_requested() 同步界面状态。 */
                    portENTER_CRITICAL(&s_cmd_mux);
                    s_video_cmd.play=false;
                    portEXIT_CRITICAL(&s_cmd_mux);
                    notify();
                    ESP_LOGI(TAG,"Headphone removed; video audio paused");
                } else {
                    audio_output_set_playing(false);
                    ESP_LOGI(TAG,"Headphone removed; playback paused");
                }
            }
        }
    } else if(s_tx_on)fail_current(ret,polled);
    /* UI只是状态观察者：锁忙时延后发布；不在锁内读I2C或调整布局。 */
    if(s_hp_dirty && lvgl_port_lock(1)) {
        ui_get_device_state()->headphone_inserted=s_hp;
        s_hp_dirty=false;lvgl_port_unlock();
    }
}
/* ------------------------------------------------------------------ */
/* 视频 PCM 模式：生产者是视频 worker，音频 worker 只负责 I2S/DAC/音量、
 * TX 启停和 ring 复位。音乐命令在视频会话期间被忽略；进入前 video_player
 * 已暂停音乐并等待其文件关闭。 */

static esp_err_t start_video_tx(void)
{
    esp_err_t ret=cs43131_prepare(s_format.sample_rate);
    if(ret==ESP_OK)ret=clear_dma();
    if(ret==ESP_OK)ret=i2s_channel_enable(s_tx);
    if(ret==ESP_OK) {
        s_dma_primed=false;
        s_tx_on=true;
        ret=cs43131_start_hp();
    }
    if(ret==ESP_OK) {
        atomic_store_explicit(&s_pcm.feed,true,memory_order_release);
        unsigned volume=dac_volume();
        s_video_volume=volume;
        ret=cs43131_volume(volume,volume==0);
    }
    if(ret!=ESP_OK) {
        cs43131_abort();s_dac_ready=false;
        if(s_tx_on) {i2s_channel_disable(s_tx);s_tx_on=false;}
        atomic_store(&s_active,false);
        return ret;
    }
    atomic_store(&s_active,true);
    ESP_LOGI(TAG,"Video PCM started: %luHz %ubit %uch",
        (unsigned long)s_format.sample_rate,s_format.bits_per_sample,s_format.channel);
    return ESP_OK;
}

static void video_step(const video_command_t *v)
{
    if(v->serial!=s_video_serial) {
        /* open/close：整个拆除过程都按视频会话处理，stop_tx 不得改写音乐进度。 */
        s_video_serial=v->serial;
        s_video_worker_active=true;
        if(s_tx_on)stop_tx();
        if(s_decoder||s_file)close_session();
        else {audio_pcm_reset(&s_pcm,s_ring);s_sent_total=0;s_last_sent=0;}
        atomic_store(&s_video_started,false);
        atomic_store(&s_video_eof_req,false);
        atomic_store(&s_video_failed,false);
        if(v->active) {
            memset(&s_format,0,sizeof(s_format));
            s_format.sample_rate=v->rate;
            s_format.channel=v->channels;
            s_format.bits_per_sample=v->bits;
            s_video_volume=UINT32_MAX;
            esp_err_t ret=configure_tx();
            if(ret!=ESP_OK) {
                atomic_store(&s_video_failed,true);
                ESP_LOGW(TAG,"Video PCM unsupported: %s",esp_err_to_name(ret));
            } else {
                atomic_store(&s_video_rate_pub,v->rate);
                atomic_store(&s_video_frame_bytes_pub,(unsigned)v->channels*(v->bits/8));
                atomic_store(&s_video_bits_pub,v->bits);
                atomic_store(&s_video_ch_pub,v->channels);
                s_video_diag_at=xTaskGetTickCount();
                reset_diagnostics();
            }
        } else {
            s_video_worker_active=false;
            memset(&s_format,0,sizeof(s_format));
        }
        atomic_store(&s_video_ack,v->serial);
        return;
    }
    if(!v->active) {
        s_video_worker_active=false;
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(20));
        return;
    }
    if(atomic_load(&s_video_failed)) {ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(50));return;}
    /* 拖动进度条：丢弃旧PCM并把音频时间归零，生产者在ack后从新位置重新填充。
     * 放在 DAC 就绪检查之前，初始化期间拖动也能及时得到 ack。 */
    unsigned flush=atomic_load(&s_video_flush_req);
    if(flush!=atomic_load(&s_video_flush_ack)) {
        if(s_tx_on)stop_tx();
        audio_pcm_reset(&s_pcm,s_ring);
        s_sent_total=0;s_last_sent=0;
        atomic_store(&s_video_started,false);
        atomic_store(&s_video_flush_ack,flush);
    }
    /* DAC 上电/初始化可能晚于视频进入；未就绪时不启动 TX，音频时间保持 0，
     * 视频停在首帧等待，不会出现无音频却继续播放的静音画面。 */
    if(!s_dac_ready) {
        TickType_t now=xTaskGetTickCount();
        if(pmic_axp2101_outputs_ready() && (!s_have_retry || (TickType_t)(now-s_retry_at)>=pdMS_TO_TICKS(1000))) {
            s_retry_at=now;s_have_retry=true;
            esp_err_t ret=cs43131_init();
            if(ret==ESP_OK) {s_dac_ready=true;s_hp_dirty=true;}
            else ESP_LOGW(TAG,"DAC init deferred: %s",esp_err_to_name(ret));
        }
        if(!s_dac_ready) {ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(50));return;}
    }
    if(!v->play) {
        if(s_tx_on)stop_tx();
        atomic_store(&s_video_started,false);
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(50));
        return;
    }
    size_t queued=audio_pcm_queued(&s_pcm);
    if(!s_tx_on && queued && (queued>=AUDIO_PCM_HIGH_BYTES || atomic_load(&s_video_eof_req))) {
        if(start_video_tx()!=ESP_OK) {ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(50));return;}
        atomic_store(&s_video_started,true);
    }
    if(!s_tx_on || queued>=AUDIO_PCM_HIGH_BYTES) poll_headphone();
    if(s_tx_on) {
        unsigned volume=dac_volume();
        if(volume!=s_video_volume && cs43131_volume(volume,volume==0)==ESP_OK)
            s_video_volume=volume;
    }
    TickType_t now_diag=xTaskGetTickCount();
    if((TickType_t)(now_diag-s_video_diag_at)>=pdMS_TO_TICKS(5000)) {
        s_video_diag_at=now_diag;
        ESP_LOGI(TAG,"Video PCM: queued=%uB, sent=%lu frames, underrun=%lu frames",
            (unsigned)audio_pcm_queued(&s_pcm),
            (unsigned long)atomic_load(&s_pcm.sent),
            (unsigned long)atomic_load(&s_pcm.underrun_frames));
    }
    /* 与音乐路径相同的低水位唤醒；生产者由视频 worker 自己按需补充。 */
    if(s_tx_on && queued>=AUDIO_PCM_HIGH_BYTES)
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));
    else
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(5));
}

esp_err_t audio_output_video_open(const audio_output_pcm_cfg_t *cfg)
{
    bt_stream_reset();
    if(!s_task || !cfg) return ESP_ERR_INVALID_STATE;
    if(!supported_rate(cfg->sample_rate) ||
       (cfg->channels!=1 && cfg->channels!=2) ||
       (cfg->bits!=16 && cfg->bits!=24 && cfg->bits!=32))
        return ESP_ERR_NOT_SUPPORTED;
    portENTER_CRITICAL(&s_cmd_mux);
    ++s_video_cmd.serial;
    s_video_cmd.active=true;s_video_cmd.play=true;
    s_video_cmd.rate=cfg->sample_rate;
    s_video_cmd.channels=cfg->channels;
    s_video_cmd.bits=cfg->bits;
    uint32_t serial=s_video_cmd.serial;
    portEXIT_CRITICAL(&s_cmd_mux);
    /* s_cmd.play 已在视频进入前由调用方置 false；这里只清视频专用请求。 */
    atomic_store(&s_video_eof_req,false);
    notify();
    for(unsigned i=0;i<200;++i) {
        if(atomic_load(&s_video_ack)==serial) {
            if(atomic_load(&s_video_failed)) {audio_output_video_close();return ESP_ERR_NOT_SUPPORTED;}
            return ESP_OK;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    /* 超时：撤销本次请求，不能让音频 worker 在无人使用的情况下继续占用
     * 视频模式而阻塞音乐。若期间已有新命令，保持新命令不变。 */
    portENTER_CRITICAL(&s_cmd_mux);
    if(s_video_cmd.serial==serial) {s_video_cmd.active=false;s_video_cmd.play=false;}
    portEXIT_CRITICAL(&s_cmd_mux);
    notify();
    return ESP_ERR_TIMEOUT;
}

void audio_output_video_close(void)
{
    bt_stream_reset();
    if(!s_task) return;
    portENTER_CRITICAL(&s_cmd_mux);
    if(!s_video_cmd.active) {portEXIT_CRITICAL(&s_cmd_mux);return;}
    ++s_video_cmd.serial;
    s_video_cmd.active=false;s_video_cmd.play=false;
    uint32_t serial=s_video_cmd.serial;
    portEXIT_CRITICAL(&s_cmd_mux);
    atomic_store(&s_video_eof_req,false);
    notify();
    for(unsigned i=0;i<200;++i) {
        if(atomic_load(&s_video_ack)==serial) return;
        /* 关机/Light-sleep 屏障期间 worker 不会处理命令；active/serial 已
         * 更新，屏障释放后仍会完成拆除，无需在这里等满1秒。 */
        if(atomic_load(&s_quiesce)&REQUEST_MASK) return;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
}

void audio_output_video_play(bool play)
{
    if(!s_task) return;
    if(!play) bt_stream_reset();
    portENTER_CRITICAL(&s_cmd_mux);
    if(!s_video_cmd.active) {portEXIT_CRITICAL(&s_cmd_mux);return;}
    s_video_cmd.play=play;
    portEXIT_CRITICAL(&s_cmd_mux);
    notify();
}

void audio_output_video_flush(void)
{
    if(!s_task || !video_command().active) return;
    uint32_t request=atomic_fetch_add(&s_video_flush_req,1)+1;
    notify();
    for(unsigned i=0;i<200;++i) {
        if(atomic_load(&s_video_flush_ack)==request) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    /* seek/刷新后不能让蓝牙继续播放旧位置的数据。 */
    bt_stream_reset();
}

void audio_output_video_eof(void)
{
    atomic_store(&s_video_eof_req,true);notify();
}

size_t audio_output_video_write(const void *pcm,size_t frames,uint32_t timeout_ms)
{
    if(!pcm || !frames) return 0;
    unsigned bits=atomic_load(&s_video_bits_pub);
    unsigned channels=atomic_load(&s_video_ch_pub);
    unsigned frame_bytes=atomic_load(&s_video_frame_bytes_pub);
    unsigned rate=atomic_load(&s_video_rate_pub);
    if(!frame_bytes || !video_command().active) return 0;
    TickType_t start=xTaskGetTickCount();
    size_t done=0;
    while(done<frames) {
        const uint8_t *source=(const uint8_t*)pcm+done*frame_bytes;
        size_t n=audio_pcm_push(&s_pcm,source,frames-done,bits,channels);
        if(n && bt_audio_is_connected() && rate)
            bt_stream_write(source,n,bits,channels,rate);
        done+=n;
        if(done==frames) break;
        if((TickType_t)(xTaskGetTickCount()-start)>=pdMS_TO_TICKS(timeout_ms)) break;
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    return done;
}

uint64_t audio_output_video_time_us(void)
{
    unsigned rate=atomic_load(&s_video_rate_pub);
    if(!rate) return 0;
    return (uint64_t)atomic_load_explicit(&s_pcm.sent,memory_order_acquire)
           *1000000ULL/rate;
}

bool audio_output_video_started(void) {return atomic_load(&s_video_started);}
bool audio_output_video_play_requested(void)
{
    video_command_t v=video_command();
    return v.active&&v.play;
}

static void output_step(void)
{
    /* 视频会话独占 I2S/DAC；关闭请求也要由 video_step 处理完再回到音乐路径。 */
    video_command_t video=video_command();
    if(video.active || s_video_worker_active || video.serial!=s_video_serial) {
        video_step(&video);
        return;
    }
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
    if(!s_tx_on || audio_pcm_queued(&s_pcm)>=AUDIO_PCM_HIGH_BYTES)poll_headphone();
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
    /* 冷启动、切歌和暂停恢复先达到高水位，再打开DAC。短歌曲在EOF时
     * 允许不足此水位启动，不能因达不到预缓冲量永远不播放。 */
    if(!s_tx_on && queued && (queued>=AUDIO_PCM_HIGH_BYTES || s_decode_eof)) {
        esp_err_t ret=start_tx();if(ret!=ESP_OK) {fail_current(ret,c);return;}
    }
    if(s_tx_on) {
        static unsigned applied=UINT32_MAX;
        unsigned volume=dac_volume();
        if(volume!=applied) {
            esp_err_t ret=cs43131_volume(volume,volume==0);
            if(ret!=ESP_OK) {fail_current(ret,c);return;}
            applied=volume;
        }
    }
    /* 保留接近满队列的储备，只在满队列附近阻塞，提前由 DMA 通知唤醒。
     * 96kHz 下 32KiB 只够约 43ms，而实测一次解码可达 67ms，不能继续等到
     * 半满再补充。通知带计数，DMA/选歌/关机事件不会在检查与等待间丢失。 */
    if(s_tx_on && queued>=AUDIO_PCM_HIGH_BYTES) {
        ulTaskNotifyTake(pdTRUE,pdMS_TO_TICKS(100));return;
    }
    TickType_t step_before=xTaskGetTickCount();
    bool progress;esp_err_t ret=decode_step(&progress);
    record_max(&s_step_max,step_before);
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
    if(audio_mp3_register()!=ESP_AUDIO_ERR_OK || esp_flac_dec_register()!=ESP_AUDIO_ERR_OK) {
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
