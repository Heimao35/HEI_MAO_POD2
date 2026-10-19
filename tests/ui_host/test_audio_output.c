#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
static ssize_t checked_audio_read(int fd,void *buf,size_t bytes);
#define read checked_audio_read
#include "../../main/audio_output.c"
#undef read
static bool sd_locked,ui_locked,mounted=true,tx_enabled,hp_present;
static bool decoder_fail,quiesce_in_poll,switch_in_decode,decoder_header_seen,oversize_output;
static TickType_t ticks;
static uint32_t generation=1,rate=192000;
static unsigned bits=32,channels=2,opens,closes,polls,configures;
static unsigned played_configs,channels_created,channels_deleted,clock_reconfigs;
static unsigned direct_reads,preload_cursor,sd_lock_failures;
static bool fresh_request_in_prepare,enable_fail_once;
static uint32_t last_rate;
static ui_device_state_t device_state;
static i2s_event_callbacks_t dma_callbacks;
static uint8_t dma[AUDIO_DMA_COUNT][AUDIO_DMA_FRAMES*8];
static void(*detach_callback)(void);
static const char *fixture="audio-worker-fixture.bin";
static uint64_t fixture_size=42+4+5*1024*1024+(192000*8/8192*8192);
static bool fixture_flac=true,mock_mp3;
static unsigned decode_cost_ms;
/* Simulate actual sample-clock cadence, not eight descriptors per C call.
 * This model fails when the worker sleeps 100ms without a DMA notification. */
static unsigned dma_slot,notify_count,dma_credit,wait_calls,dma_wakes;
static void advance_ms(unsigned ms)
{
    while(ms--) {
        ++ticks;
        if(!tx_enabled)continue;
        dma_credit+=rate;
        while(dma_credit>=AUDIO_DMA_FRAMES*1000U) {
            dma_credit-=AUDIO_DMA_FRAMES*1000U;
            i2s_event_data_t e={.dma_buf=dma[dma_slot],.size=sizeof(dma[dma_slot])};
            dma_callbacks.on_sent((void*)1,&e,NULL);
            dma_slot=(dma_slot+1)%AUDIO_DMA_COUNT;
        }
    }
}
void vTaskDelay(unsigned ms) {advance_ms(ms);}
uint32_t ulTaskNotifyTake(BaseType_t clear,TickType_t ms)
{
    ++wait_calls;
    while(!notify_count && ms--)advance_ms(1);
    unsigned result=notify_count;
    if(clear)notify_count=0;
    else if(notify_count)--notify_count;
    return result;
}
TickType_t xTaskGetTickCount(void) {return ticks;}
void xTaskNotifyGive(TaskHandle_t task) {assert(task);++notify_count;}
void vTaskNotifyGiveFromISR(TaskHandle_t task,BaseType_t *wake)
{assert(task);++notify_count;++dma_wakes;*wake=pdTRUE;}
BaseType_t xTaskCreatePinnedToCore(void(*fn)(void*),const char*n,uint32_t stack,void*arg,UBaseType_t p,TaskHandle_t*t,int core)
{(void)fn;(void)n;(void)arg;assert(stack>=20480 && p==5 && core==0);*t=(void*)3;return pdPASS;}
static ssize_t checked_audio_read(int fd,void *buf,size_t bytes)
{
    assert(sd_locked && !ui_locked && buf==s_io);
    assert(((uintptr_t)buf%IO_ALIGNMENT)==0 && bytes%SECTOR_BYTES==0 && bytes<=READ_BYTES);
    assert((lseek(fd,0,SEEK_CUR)%SECTOR_BYTES)==0);
    ++direct_reads;return read(fd,buf,bytes);
}
bool pmic_axp2101_outputs_ready(void) {return true;}
esp_err_t gpio_config(const gpio_config_t *c) {assert(c->pin_bit_mask==1ULL<<53);return ESP_OK;}
esp_err_t gpio_isr_handler_add(gpio_num_t pin,void(*cb)(void*),void *arg) {(void)cb;(void)arg;assert(pin==53);return ESP_OK;}
esp_err_t gpio_isr_handler_remove(gpio_num_t p) {(void)p;return ESP_OK;}
int gpio_get_level(gpio_num_t pin) {assert(pin==53);return 0;}
bool lvgl_port_lock(uint32_t wait) {(void)wait;assert(!sd_locked && !ui_locked);ui_locked=true;return true;}
void lvgl_port_unlock(void) {assert(ui_locked);ui_locked=false;}
ui_device_state_t *ui_get_device_state(void) {assert(ui_locked);return &device_state;}
bool sd_card_io_lock(TickType_t wait)
{
    assert(!sd_locked && !ui_locked);
    if(sd_lock_failures) {--sd_lock_failures;advance_ms(wait);return false;}
    sd_locked=true;return true;
}
void sd_card_io_unlock(void) {assert(sd_locked);sd_locked=false;}
bool sd_card_is_mounted(void) {return mounted;}
bool sd_card_is_present(void) {return mounted;}
uint32_t sd_card_get_generation(void) {return generation;}
esp_err_t sd_card_set_audio_detach_callback(void(*cb)(void)) {detach_callback=cb;return ESP_OK;}
esp_err_t music_library_get_item(uint32_t index,music_item_t *item)
{assert(sd_locked && index<4);memset(item,0,sizeof(*item));strcpy(item->audio,fixture);return ESP_OK;}
esp_err_t music_library_make_path(const music_item_t *item,const char *f,char *out,size_t cap)
{(void)item;assert(sd_locked && strlen(f)<cap);strcpy(out,f);return ESP_OK;}
esp_err_t audio_player_probe(const char *path,audio_file_info_t *info)
{assert(sd_locked && !strcmp(path,fixture));memset(info,0,sizeof(*info));info->file_size=fixture_size;info->is_flac=fixture_flac;info->channels=channels;info->bits_per_sample=bits;info->duration_ms=1000;return ESP_OK;}
esp_err_t cs43131_init(void) {assert(!ui_locked && !sd_locked);return ESP_OK;}
esp_err_t cs43131_prepare(uint32_t hz) {
    assert(!ui_locked && !sd_locked);last_rate=hz;++played_configs;
    if(fresh_request_in_prepare) {fresh_request_in_prepare=false;audio_output_set_playing(true);return ESP_FAIL;}
    return ESP_OK;
}
esp_err_t cs43131_start_hp(void) {vTaskDelay(15);return ESP_OK;}
esp_err_t cs43131_stop(void) {assert(!ui_locked && !sd_locked);vTaskDelay(150);return ESP_OK;}
void cs43131_abort(void) {}
esp_err_t cs43131_volume(uint8_t v,bool m) {assert(!ui_locked && !sd_locked && v<=100 && m==(v==0));return ESP_OK;}
esp_err_t cs43131_poll(bool *hp)
{
    assert(!ui_locked && !sd_locked);++polls;*hp=hp_present;
    if(quiesce_in_poll) {quiesce_in_poll=false;assert(!audio_output_quiesce(true));}
    return ESP_OK;
}
esp_err_t i2s_new_channel(const i2s_chan_config_t *c,i2s_chan_handle_t *tx,i2s_chan_handle_t *rx)
{assert(!rx && c->role==I2S_ROLE_SLAVE && !c->auto_clear_before_cb && !c->auto_clear_after_cb);*tx=(void*)1;memset(dma,0,sizeof(dma));++channels_created;return ESP_OK;}
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t tx,const i2s_std_config_t *c)
{assert(tx && c->gpio_cfg.bclk==51 && c->gpio_cfg.ws==50 && c->gpio_cfg.dout==49 && c->gpio_cfg.mclk==-1);++configures;return ESP_OK;}
esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t tx,const i2s_std_clk_config_t *c)
{assert(tx && c && !tx_enabled);last_rate=c->sample_rate_hz;++clock_reconfigs;return ESP_OK;}
esp_err_t i2s_channel_preload_data(i2s_chan_handle_t tx,const void *src,size_t size,size_t *loaded)
{
    assert(tx && !tx_enabled && src && size==AUDIO_DMA_FRAMES*8 && loaded);
    assert(preload_cursor+size<=sizeof(dma));
    for(size_t i=0;i<size;++i)assert(!((const uint8_t*)src)[i]);
    memcpy((uint8_t*)dma+preload_cursor,src,size);
    preload_cursor+=size;*loaded=size;return ESP_OK;
}
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t tx,const i2s_event_callbacks_t *c,void*arg)
{(void)arg;assert(tx);dma_callbacks=*c;return ESP_OK;}
esp_err_t i2s_channel_enable(i2s_chan_handle_t tx) {
    assert(tx && !tx_enabled && preload_cursor==sizeof(dma));
    for(size_t i=0;i<sizeof(dma);++i)assert(!((uint8_t*)dma)[i]);
    if(enable_fail_once) {enable_fail_once=false;return ESP_FAIL;}
    assert(audio_pcm_queued(&s_pcm)>=AUDIO_PCM_HIGH_BYTES || s_decode_eof);
    tx_enabled=true;dma_slot=dma_credit=0;return ESP_OK;
}
esp_err_t i2s_channel_disable(i2s_chan_handle_t tx) {assert(tx && tx_enabled);tx_enabled=false;preload_cursor=0;return ESP_OK;}
esp_err_t i2s_del_channel(i2s_chan_handle_t tx) {assert(tx && !tx_enabled);++channels_deleted;return ESP_OK;}
esp_audio_err_t esp_mp3_dec_register(void) {return ESP_AUDIO_ERR_OK;}
esp_audio_err_t esp_flac_dec_register(void) {return ESP_AUDIO_ERR_OK;}
esp_audio_err_t esp_audio_simple_dec_open(esp_audio_simple_dec_cfg_t *cfg,esp_audio_simple_dec_handle_t *h)
{assert(cfg->dec_type==(fixture_flac?ESP_AUDIO_SIMPLE_DEC_TYPE_FLAC:ESP_AUDIO_SIMPLE_DEC_TYPE_MP3) && !s_decoder);*h=(void*)5;decoder_header_seen=!fixture_flac;++opens;return ESP_AUDIO_ERR_OK;}
void esp_audio_simple_dec_close(esp_audio_simple_dec_handle_t h) {assert(h);++closes;}
esp_audio_err_t esp_audio_simple_dec_get_info(esp_audio_simple_dec_handle_t h,esp_audio_simple_dec_info_t *info)
{assert(h);*info=(esp_audio_simple_dec_info_t){.sample_rate=rate,.bits_per_sample=bits,.channel=channels};return ESP_AUDIO_ERR_OK;}
esp_audio_err_t esp_audio_simple_dec_process(esp_audio_simple_dec_handle_t h,esp_audio_simple_dec_raw_t *in,esp_audio_simple_dec_out_t *out)
{
    assert(h && !sd_locked && !ui_locked);
    if(decoder_fail)return ESP_AUDIO_ERR_FAIL;
    if(oversize_output) {out->needed_size=OUTPUT_MAX_BYTES+1;return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;}
    if(!decoder_header_seen) {
        assert(in->len>=42 && !memcmp(in->buffer,"fLaC",4) && (in->buffer[4]&0x80));
        in->consumed=42;out->decoded_size=0;decoder_header_seen=true;return ESP_AUDIO_ERR_OK;
    }
    advance_ms(decode_cost_ms);
    if(mock_mp3) {
        /* 384 encoded bytes -> 1152 stereo16 frames; models MP3 expansion. */
        assert(in->len>=384);
        if(out->len<4608) {out->needed_size=4608;return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;}
        memset(out->buffer,0x35,4608);out->decoded_size=4608;in->consumed=384;
        return ESP_AUDIO_ERR_OK;
    }
    unsigned frame_bytes=channels*(bits/8);
    unsigned n=(in->len<8192?in->len:8192)/frame_bytes*frame_bytes;
    if(n>out->len) {out->needed_size=n;return ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;}
    memcpy(out->buffer,in->buffer,n);out->decoded_size=n;in->consumed=n;
    if(switch_in_decode) {switch_in_decode=false;audio_output_select(1,generation,true);}
    return ESP_AUDIO_ERR_OK;
}
static void steps(unsigned n) {while(n--) {output_iteration();advance_ms(1);}}
int main(void)
{
    FILE *fp=fopen(fixture,"wb");assert(fp);uint8_t chunk[8192];memset(chunk,0x21,sizeof(chunk));
    uint8_t header[42]={0};memcpy(header,"fLaC",4);header[7]=34;
    assert(fwrite(header,1,sizeof(header),fp)==sizeof(header));
    uint8_t picture[4]={0x86,0x50,0,0}; /*5MiB embedded PICTURE, skipped by offset */
    assert(fwrite(picture,1,4,fp)==4);assert(fseeko(fp,42+4+5*1024*1024,SEEK_SET)==0);
    for(unsigned i=0;i<192000*8/sizeof(chunk);++i)assert(fwrite(chunk,1,sizeof(chunk),fp)==sizeof(chunk));
    /* file size is a multiple of8192 for complete mock PCM frames */
    fclose(fp);
    assert(audio_output_init()==ESP_OK);
    /* I2S/GDMA must stay unallocated while the boot SD catalogue is scanned. */
    assert(channels_created==0);
    s_output_capacity=64; /* exercise real needed_size growth on the first frame */
    hp_present=true;audio_output_select(0,generation,false);steps(3);
    assert(!audio_output_is_playing() && !audio_output_is_active() && !tx_enabled && device_state.headphone_inserted);
    assert(!s_file); /* restored selection doesn't leave a file open or autoplay */
    /* Cold artwork can own SD for several attempts; keep request pending. */
    sd_lock_failures=3;audio_output_toggle();steps(2);
    assert(audio_output_is_play_requested() && !audio_output_is_playing());
    steps(40);
    assert(audio_output_is_playing() && tx_enabled && last_rate==192000 && audio_output_time_ms()>0);
    audio_output_toggle();steps(3);uint32_t paused=audio_output_time_ms();
    assert(!tx_enabled && !audio_output_is_active());steps(20);assert(audio_output_time_ms()==paused);
    audio_output_toggle();steps(24);assert(audio_output_is_playing() && audio_output_time_ms()>=paused);
    puts("PASS real worker boot restore,192k32bit pause/resume and bounded5MiB FLAC PICTURE skip");
    ticks+=100;quiesce_in_poll=true;output_iteration(); /* request arrives during I2C while BUSY */
    assert(!(atomic_load(&s_quiesce)&QUIESCE_ACK));
    output_iteration();assert(audio_output_quiesce(true) && !tx_enabled && !s_file);
    unsigned old_polls=polls;steps(3);assert(polls==old_polls);
    assert(audio_output_sleep(true));audio_output_quiesce(false);steps(3);assert(polls==old_polls);
    audio_output_sleep(false);
    /* An ACK can be safely reused by a new owner before worker claims BUSY. */
    assert(audio_output_quiesce(true));steps(2);assert(polls==old_polls);
    audio_output_quiesce(false);steps(24);assert(tx_enabled && polls>old_polls);
    puts("PASS quiesce arriving in I2C, no I/O after ACK, independent shutdown/sleep ownership and exact resume");
    assert(s_output_capacity>=8192);
    oversize_output=true;steps(15);oversize_output=false;
    assert(!audio_output_is_playing() && !tx_enabled && !s_decoder && !audio_output_take_finished());
    unsigned old_opens=opens;audio_output_toggle();steps(24);assert(opens>old_opens && tx_enabled);
    switch_in_decode=true;steps(36);assert(s_current.index==1 && audio_output_is_playing());
    puts("PASS oversized output is rejected, decoder can retry, selection during decode discards old data");
    sd_card_io_lock(0);detach_callback();mounted=false;++generation;sd_card_io_unlock();
    steps(3);assert(!s_file && !s_decoder && !tx_enabled && !audio_output_is_active());
    mounted=true;rate=22050;audio_output_select(0,generation,true);steps(8);
    assert(!tx_enabled && !audio_output_take_finished());
    rate=44100;audio_output_select(0,generation,true);steps(24);assert(tx_enabled && last_rate==44100);
    puts("PASS unmount closes stream before VFS teardown; unsupported rate pauses without false EOS");
    for(unsigned i=0;i<3000 && !atomic_load(&s_finished);++i)steps(1);
    assert(audio_output_take_finished() && !audio_output_take_finished() && !tx_enabled);
    assert(audio_output_time_ms()==(uint64_t)(192000*8/8192*8192/8)*1000/rate);
    assert(opens>=closes);
    assert(close_session());remove(fixture);
    /* Real input reader: large ID3v2 and ID3v1 never enter decoder input. */
    const char *tagged="audio-worker-mp3-fixture.bin";
    FILE *tag=fopen(tagged,"wb");assert(tag);
    uint32_t tag_size=10*1024*1024;
    uint8_t id3[10]={'I','D','3',4,0,0,(tag_size>>21)&127,(tag_size>>14)&127,(tag_size>>7)&127,tag_size&127};
    assert(fwrite(id3,1,10,tag)==10 && fseeko(tag,10+tag_size,SEEK_SET)==0);
    uint8_t payload[4096];memset(payload,0x55,sizeof(payload));
    assert(fwrite(payload,1,sizeof(payload),tag)==sizeof(payload));
    uint8_t tail_tag[128]={0};memcpy(tail_tag,"TAG",3);assert(fwrite(tail_tag,1,128,tag)==128);fclose(tag);
    sd_card_io_lock(0);s_file=fopen(tagged,"rb");assert(s_file);setvbuf(s_file,NULL,_IONBF,0);
    s_file_size=10+tag_size+sizeof(payload)+128;
    assert(skip_mp3_metadata()==ESP_OK && s_file_offset==10+tag_size && s_file_size==10+tag_size+sizeof(payload));
    sd_card_io_unlock();
    assert(read_input()==ESP_OK && s_raw.len==sizeof(payload) && s_input_eof && !memcmp(s_raw.buffer,payload,sizeof(payload)));
    assert(close_session());remove(tagged);
    puts("PASS10MiB ID3v2 seek and ID3v1 tail exclusion in actual stream reader");
    /* Test sector staging with nonuniform bytes, unaligned payload, parser
     * leftovers, a lock timeout and an actual close/reopen. */
    fixture="audio-aligned-fixture.bin";
    FILE *pattern=fopen(fixture,"wb");assert(pattern);
    const unsigned begin=123,length=300*1024+37;
    for(unsigned i=0;i<begin+length+128;++i)assert(fputc((i*13U+i/97U)&255,pattern)!=EOF);
    fclose(pattern);fixture_size=begin+length+128;
    sd_card_io_lock(0);s_file=fopen(fixture,"rb");assert(s_file);setvbuf(s_file,NULL,_IONBF,0);sd_card_io_unlock();
    s_file_offset=begin;s_file_size=begin+length;
    unsigned next=begin,reads_before=direct_reads,rounds=0;
    while(!s_input_eof || s_raw.len) {
        if(!s_input_eof)assert(read_input()==ESP_OK);
        for(unsigned i=0;i<s_raw.len;++i)assert(s_raw.buffer[i]==(uint8_t)((next+i)*13U+(next+i)/97U));
        unsigned consume=s_input_eof?s_raw.len:s_raw.len-17;
        s_raw.buffer+=consume;s_raw.len-=consume;next+=consume;
        if(++rounds==2) {
            uint8_t *held=s_raw.buffer;unsigned len=s_raw.len;
            sd_lock_failures=1;assert(read_input()==ESP_ERR_TIMEOUT && s_raw.buffer==held && s_raw.len==len);
            assert(close_file()); /* retain staged data and exact logical offset */
        }
        if(s_input_eof && !s_raw.len)break;
    }
    assert(next==begin+length && direct_reads-reads_before<=(length+READ_BYTES-1)/READ_BYTES+1);
    assert(close_session());remove(fixture);
    puts("PASS aligned16KiB direct reads, patterned bytes, unaligned ends,17-byte parser remainder, timeout and reopen");
    /* Three reported formats plus192k32bit were exercised with actual worker
     * and sample-paced DMA. Decoder arithmetic is a hardware double. */
    for(unsigned scenario=0;scenario<4;++scenario) {
        rate=(uint32_t[]){44100,96000,48000,192000}[scenario];
        bits=(unsigned[]){16,24,16,32}[scenario];channels=2;
        mock_mp3=scenario==2;fixture_flac=!mock_mp3;
        decode_cost_ms=mock_mp3?4:1;
        fixture="audio-paced-fixture.bin";
        FILE *paced=fopen(fixture,"wb");assert(paced);
        unsigned payload_bytes=mock_mp3?384*60:rate*channels*(bits/8);
        fixture_size=(fixture_flac?42:0)+payload_bytes;
        if(fixture_flac) {
            uint8_t h[42]={0};memcpy(h,"fLaC",4);h[4]=0x80;h[7]=34;
            assert(fwrite(h,1,42,paced)==42);
        }
        while(payload_bytes--)assert(fputc(0x32,paced)!=EOF);
        fclose(paced);
        audio_output_select(0,generation,true);
        if(scenario==0) {
            fresh_request_in_prepare=true;
            for(unsigned step=0;step<300 && fresh_request_in_prepare;++step)steps(1);
            assert(!fresh_request_in_prepare && audio_output_is_play_requested() && !s_decoder);
        }
        if(scenario==1) {
            enable_fail_once=true;
            for(unsigned step=0;step<300 && enable_fail_once;++step)steps(1);
            assert(!enable_fail_once && !audio_output_is_play_requested());
            assert(s_dma_primed && preload_cursor==sizeof(dma));
            audio_output_set_playing(true);
        }
        unsigned blocked_before=wait_calls,wakes_before=dma_wakes;
        bool started=false,ended=false;
        for(unsigned step=0;step<5000;++step) {
            output_iteration();advance_ms(1);
            if(tx_enabled)started=true;
            if(!s_decode_eof)assert(!atomic_load(&s_pcm.underrun_frames));
            if(atomic_load(&s_finished)) {ended=true;break;}
        }
        assert(started && ended && !tx_enabled && wait_calls>blocked_before && dma_wakes>wakes_before);
        assert(audio_output_take_finished() && !audio_output_take_finished());
        uint32_t expected_ms=mock_mp3?60*1152*1000/rate:1000;
        assert(audio_output_time_ms()==expected_ms);
        assert(close_session());remove(fixture);
        printf("PASS paced %luHz/%ubit/%s: startup reserve, no pre-EOF underrun, blocked producer and exact samples\n",
            (unsigned long)rate,bits,mock_mp3?"MP3 model":"FLAC model");
    }
    /* A short track never reaches48KiB; it must still start and drain. */
    mock_mp3=false;fixture_flac=true;rate=44100;bits=16;decode_cost_ms=1;
    fixture="audio-short-fixture.bin";FILE *short_file=fopen(fixture,"wb");assert(short_file);
    uint8_t h[42]={0};memcpy(h,"fLaC",4);h[4]=0x80;h[7]=34;
    assert(fwrite(h,1,42,short_file)==42);
    uint8_t short_pcm[400]={0};assert(fwrite(short_pcm,1,sizeof(short_pcm),short_file)==sizeof(short_pcm));
    fclose(short_file);fixture_size=42+sizeof(short_pcm);
    audio_output_select(0,generation,true);
    for(unsigned i=0;i<1000 && !atomic_load(&s_finished);++i)steps(1);
    assert(audio_output_take_finished() && audio_output_time_ms()==100*1000/rate);
    assert(close_session());remove(fixture);
    puts("PASS short EOF starts below prebuffer threshold and completes");
    assert(channels_created==1 && channels_deleted==0 && clock_reconfigs>0 && direct_reads>0);
    assert(i2s_del_channel(s_tx)==ESP_OK);
    puts("PASS one reserved I2S channel, clock changes while stopped, clean DMA reuse and request version during failure");
    heap_caps_free(s_io);heap_caps_free(s_ring);heap_caps_free(s_input);heap_caps_free(s_output);
    assert(wait_calls>0 && dma_wakes>0);
    puts("PASS actual sample-clock DMA cadence wakes blocked producer; startup prebuffer, backpressure and exact EOS");
    return 0;
}
