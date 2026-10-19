#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <math.h>
#include "esp_heap_caps.h"
/* Intercept the actual adapter allocation: no internal-memory fallback is allowed. */
#undef heap_caps_malloc
#undef heap_caps_free
static unsigned allocs,frees,live,peak;
static int fail_alloc;
static void *mp3_test_alloc(size_t n,unsigned caps)
{
    assert(caps==(MALLOC_CAP_SPIRAM|MALLOC_CAP_8BIT));
    assert(n<=12*1024);
    if(fail_alloc)return NULL;
    void *p=test_heap_alloc(64,n);assert(p);
    ++allocs;++live;if(live>peak)peak=live;
    return p;
}
static void mp3_test_free(void *p) {assert(p && live);--live;++frees;test_heap_free(p);}
#define heap_caps_malloc mp3_test_alloc
#define heap_caps_free mp3_test_free
#include "../../main/audio_mp3.c"

static const esp_audio_dec_ops_t *registered;
static TickType_t ticks;
static esp_audio_err_t register_result=ESP_AUDIO_ERR_OK;
TickType_t xTaskGetTickCount(void) {return ticks++;}
esp_audio_err_t esp_audio_dec_register(esp_audio_type_t type,const esp_audio_dec_ops_t *ops)
{assert(type==ESP_AUDIO_TYPE_MP3);registered=ops;return register_result;}

static uint8_t *load(const char *name,const char *ext,size_t *size)
{
    char path[512];snprintf(path,sizeof(path),"%s/%s.%s",MP3_FIXTURE_DIR,name,ext);
    FILE *f=fopen(path,"rb");assert(f);assert(!fseek(f,0,SEEK_END));
    long n=ftell(f);assert(n>0);rewind(f);
    uint8_t *data=malloc((size_t)n);assert(data);
    assert(fread(data,1,(size_t)n,f)==(size_t)n);fclose(f);*size=(size_t)n;return data;
}
/* Independent MPEG header length calculation imitates the existing simple parser:
 * feed EXACTLY one full compressed frame, rather than minimp3's own stream finder. */
static size_t frame_bytes(const uint8_t *p,size_t left)
{
    assert(left>=4 && p[0]==0xff && (p[1]&0xe0)==0xe0);
    unsigned version=(p[1]>>3)&3,layer=(p[1]>>1)&3;
    assert(version!=1 && layer==1);
    static const unsigned rate[]={44100,48000,32000};
    static const unsigned br1[]={0,32,40,48,56,64,80,96,112,128,160,192,224,256,320};
    static const unsigned br2[]={0,8,16,24,32,40,48,56,64,80,96,112,128,144,160};
    unsigned ri=(p[2]>>2)&3,bi=p[2]>>4;assert(ri<3 && bi>0 && bi<15);
    unsigned hz=rate[ri]/(version==3?1:version==2?2:4);
    size_t n=(version==3?144000:72000)*(version==3?br1[bi]:br2[bi])/hz+((p[2]>>1)&1);
    assert(n<=left);return n;
}
static size_t decode_all(void *h,uint8_t *input,size_t bytes,uint8_t *result,int split,
                          unsigned rate,unsigned channels)
{
    size_t off=0,produced=0;unsigned before=allocs;
    while(off<bytes) {
        size_t n=split?frame_bytes(input+off,bytes-off):bytes-off;
        /* Use intentionally unaligned source/output buffers. */
        uint8_t source[16385],pcm[4609];assert(n<sizeof(source));memcpy(source+1,input+off,n);
        memset(pcm,0xa5,sizeof(pcm));
        esp_audio_dec_in_raw_t raw={.buffer=source+1,.len=(uint32_t)n};
        esp_audio_dec_out_frame_t out={.buffer=pcm+1,.len=4608};
        esp_audio_dec_info_t info={0};
        assert(registered->decode(h,&raw,&out,&info)==ESP_AUDIO_ERR_OK);
        assert(raw.consumed && raw.consumed<=n && out.decoded_size<=4608);
        assert(info.sample_rate==rate && info.channel==channels && info.bits_per_sample==16);
        assert(pcm[0]==0xa5 && out.decoded_size==info.frame_size);
        memcpy(result+produced,pcm+1,out.decoded_size);produced+=out.decoded_size;off+=raw.consumed;
    }
    assert(allocs==before);return produced;
}
static void fixture(const char *name,unsigned rate,unsigned channels)
{
    size_t bytes,refbytes;uint8_t *input=load(name,"mp3",&bytes),*ref=load(name,"pcm",&refbytes);
    uint8_t *actual=malloc(refbytes+4608),*stream=malloc(refbytes+4608);assert(actual && stream);
    void *h=NULL;assert(registered->open(NULL,0,&h)==ESP_AUDIO_ERR_OK && h);
    /* Capacity negotiation must NOT advance reservoir/synthesis state. */
    uint8_t small[8];esp_audio_dec_in_raw_t raw={.buffer=input,.len=(uint32_t)frame_bytes(input,bytes)};
    esp_audio_dec_out_frame_t out={.buffer=small,.len=sizeof(small)};esp_audio_dec_info_t info={0};
    for(unsigned i=0;i<3;++i) {
        raw.consumed=123;out.decoded_size=456;
        assert(registered->decode(h,&raw,&out,&info)==ESP_AUDIO_ERR_BUFF_NOT_ENOUGH);
        assert(!raw.consumed && !out.decoded_size && out.needed_size==4608);
    }
    size_t n=decode_all(h,input,bytes,actual,1,rate,channels);assert(n==refbytes);
    /* Independent FFmpeg reference: decoder rounding differences <= 3 LSB. */
    int peakdiff=0;double sq=0;
    for(size_t i=0;i<n;i+=2) {
        int a=(int16_t)(actual[i]|actual[i+1]<<8),b=(int16_t)(ref[i]|ref[i+1]<<8);
        int d=a-b;int ad=d<0?-d:d;if(ad>peakdiff)peakdiff=ad;sq+=(double)d*d;
    }
    printf("%s: PCM=%luB, peak difference=%d, rms=%g LSB\n",name,(unsigned long)n,peakdiff,sqrt(sq/(n/2)));
    assert(peakdiff<=3 && sq/(n/2)<1.0);
    /* Explicit reset removes previous song state; output matches on every repeat.
     * Leaving the same handle alive throughout each track exercises reservoir continuity. */
    for(unsigned i=0;i<4;++i) {
        assert(registered->reset(h)==ESP_AUDIO_ERR_OK);
        assert(decode_all(h,input,bytes,stream,0,rate,channels)==n);
        assert(!memcmp(actual,stream,n));
    }
    assert(registered->close(h)==ESP_AUDIO_ERR_OK && !live);
    free(input);free(ref);free(actual);free(stream);
}
int main(void)
{
    register_result=ESP_AUDIO_ERR_MEM_LACK;assert(audio_mp3_register()==register_result);
    register_result=ESP_AUDIO_ERR_OK;assert(audio_mp3_register()==ESP_AUDIO_ERR_OK && registered);
    void *h=(void*)7;fail_alloc=1;
    assert(registered->open(NULL,0,&h)==ESP_AUDIO_ERR_MEM_LACK && !h);fail_alloc=0;
    assert(registered->open(NULL,0,NULL)==ESP_AUDIO_ERR_INVALID_PARAMETER);
    assert(registered->reset(NULL)==ESP_AUDIO_ERR_INVALID_PARAMETER);
    assert(registered->close(NULL)==ESP_AUDIO_ERR_INVALID_PARAMETER);
    fixture("48k_stereo_cbr",48000,2);
    fixture("44k_stereo_vbr",44100,2);
    fixture("32k_mono",32000,1);
    fixture("24k_mono_mpeg2",24000,1);
    assert(peak==1 && allocs==frees && !live);
    assert(registered->open(NULL,0,&h)==ESP_AUDIO_ERR_OK);
    uint8_t zeros[4608]={0},garbage[16]={0};
    esp_audio_dec_in_raw_t raw={.buffer=garbage,.len=16};
    esp_audio_dec_out_frame_t out={.buffer=zeros,.len=sizeof(zeros)};esp_audio_dec_info_t info={0};
    assert(registered->decode(NULL,&raw,&out,&info)==ESP_AUDIO_ERR_INVALID_PARAMETER);
    raw.frame_recover=ESP_AUDIO_DEC_RECOVERY_PLC;
    assert(registered->decode(h,&raw,&out,&info)==ESP_AUDIO_ERR_NOT_SUPPORT);
    raw.frame_recover=ESP_AUDIO_DEC_RECOVERY_NONE;raw.buffer=NULL;
    assert(registered->decode(h,&raw,&out,&info)==ESP_AUDIO_ERR_INVALID_PARAMETER);
    raw.len=0;assert(registered->decode(h,&raw,&out,&info)==ESP_AUDIO_ERR_OK && !raw.consumed);
    raw.buffer=garbage;raw.len=sizeof(garbage);
    /* Garbage must not fabricate PCM or access outside the buffers. */
    assert(registered->decode(h,&raw,&out,&info)!=ESP_AUDIO_ERR_BUFF_NOT_ENOUGH);
    assert(!out.decoded_size && raw.consumed<=sizeof(garbage));
    assert(registered->close(h)==ESP_AUDIO_ERR_OK);
    audio_mp3_stats_t stats=audio_mp3_take_stats();assert(stats.calls && stats.core_ticks==stats.calls);
    assert(stats.max_core_ticks==1 && !audio_mp3_take_stats().calls);
    assert(allocs==frees && !live);
    puts("PASS real MP3 frame/stream decode, FFmpeg PCM reference, reservoir/reset, bounded PSRAM, negotiation/errors");
    return 0;
}
