#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include "../../main/audio_mp3.c"

static const esp_audio_dec_ops_t *registered;
static TickType_t ticks;
static esp_audio_err_t register_result=ESP_AUDIO_ERR_OK,decode_result=ESP_AUDIO_ERR_OK;
static esp_audio_dec_in_raw_t *expected_raw;
static esp_audio_dec_out_frame_t *expected_out;
static esp_audio_dec_info_t *expected_info;
static unsigned calls;
TickType_t xTaskGetTickCount(void) {return ticks;}
esp_audio_err_t esp_audio_dec_register(esp_audio_type_t type,const esp_audio_dec_ops_t *ops)
{assert(type==ESP_AUDIO_TYPE_MP3);registered=ops;return register_result;}
esp_audio_err_t esp_mp3_dec_open(void *cfg,uint32_t size,void **h)
{assert(!cfg && !size);*h=(void*)7;return ESP_AUDIO_ERR_OK;}
esp_audio_err_t esp_mp3_dec_reset(void *h)
{assert(h==(void*)7);return ESP_AUDIO_ERR_OK;}
esp_audio_err_t esp_mp3_dec_close(void *h)
{assert(h==(void*)7);return ESP_AUDIO_ERR_OK;}
esp_audio_err_t esp_mp3_dec_decode(void *h,esp_audio_dec_in_raw_t *raw,
                                   esp_audio_dec_out_frame_t *out,esp_audio_dec_info_t *info)
{
    assert(h==(void*)7 && raw==expected_raw && out==expected_out && info==expected_info);
    ++calls;ticks+=3;
    /* The timing wrapper must preserve ALL official output/error semantics. */
    raw->consumed=decode_result==ESP_AUDIO_ERR_OK?raw->len:0;
    out->needed_size=4608;out->decoded_size=decode_result==ESP_AUDIO_ERR_OK?4:0;
    if(out->decoded_size) {out->buffer[0]=1;out->buffer[1]=2;out->buffer[2]=3;out->buffer[3]=4;}
    *info=(esp_audio_dec_info_t){.sample_rate=48000,.bits_per_sample=16,.channel=2};
    return decode_result;
}
int main(void)
{
    register_result=ESP_AUDIO_ERR_MEM_LACK;
    assert(audio_mp3_register()==ESP_AUDIO_ERR_MEM_LACK);
    register_result=ESP_AUDIO_ERR_OK;
    assert(audio_mp3_register()==ESP_AUDIO_ERR_OK && registered);
    assert(registered->open==esp_mp3_dec_open && registered->reset==esp_mp3_dec_reset &&
           registered->close==esp_mp3_dec_close);
    void *h=NULL;assert(registered->open(NULL,0,&h)==ESP_AUDIO_ERR_OK && h==(void*)7);
    uint8_t in[3073]={0},out[8194]={0};
    esp_audio_dec_in_raw_t raw={.buffer=in+1,.len=3072};
    esp_audio_dec_out_frame_t frame={.buffer=out+1,.len=8192};
    esp_audio_dec_info_t info={0};
    expected_raw=&raw;expected_out=&frame;expected_info=&info;
    assert(registered->decode(h,&raw,&frame,&info)==ESP_AUDIO_ERR_OK);
    assert(raw.buffer==in+1 && raw.len==3072 && raw.consumed==3072);
    assert(frame.buffer==out+1 && frame.len==8192 && frame.decoded_size==4);
    assert(!out[0] && !out[5] && out[1]==1 && out[4]==4 && info.sample_rate==48000);
    decode_result=ESP_AUDIO_ERR_BUFF_NOT_ENOUGH;
    assert(registered->decode(h,&raw,&frame,&info)==decode_result);
    assert(raw.consumed==0 && frame.needed_size==4608 && !frame.decoded_size);
    decode_result=ESP_AUDIO_ERR_FAIL;
    assert(registered->decode(h,&raw,&frame,&info)==decode_result);
    /* Unsigned tick subtraction must also handle a tick-counter wrap. */
    ticks=(TickType_t)-2;
    assert(registered->decode(h,&raw,&frame,&info)==decode_result);
    audio_mp3_stats_t stats=audio_mp3_take_stats();
    assert(stats.calls==4 && calls==4 && stats.core_ticks==12 && stats.max_core_ticks==3);
    assert(!audio_mp3_take_stats().calls);
    assert(registered->reset(h)==ESP_AUDIO_ERR_OK && registered->close(h)==ESP_AUDIO_ERR_OK);
    puts("PASS transparent MP3 lifecycle/buffer/error forwarding and core-only timing with tick wrap");
    return 0;
}
