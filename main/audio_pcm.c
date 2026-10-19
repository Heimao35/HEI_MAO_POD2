#include "audio_pcm.h"
#include <string.h>
#include "esp_attr.h"
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"DMA counters must be lock free");

/* 按连续区段转换，格式分支放在样本循环外。输入按字节读取，允许解码器
 * 返回非对齐地址；输出为内部 SRAM 中 4 字节对齐的 32bit I2S 槽。
 * 无符号左移保留全部有效位，不截断 24/32bit，不做音量乘法或重采样。 */
static uint32_t sample16(const uint8_t *p)
{return (uint32_t)p[0]<<16 | (uint32_t)p[1]<<24;}
static uint32_t sample24(const uint8_t *p)
{return (uint32_t)p[0]<<8 | (uint32_t)p[1]<<16 | (uint32_t)p[2]<<24;}
static uint32_t sample32(const uint8_t *p)
{return (uint32_t)p[0] | (uint32_t)p[1]<<8 | (uint32_t)p[2]<<16 | (uint32_t)p[3]<<24;}
static void pack_contiguous(uint8_t *dst,const uint8_t *pcm,size_t frames,
                            unsigned bits,unsigned channels)
{
    uint32_t *out=(uint32_t *)dst;
    if(channels==2) {
        if(bits==16) {
            for(size_t f=0;f<frames;++f,pcm+=4,out+=2) {
                out[0]=sample16(pcm);out[1]=sample16(pcm+2);
            }
        } else { /* 32bit stereo 已在调用处直接复制，此处只有 packed 24bit。 */
            for(size_t f=0;f<frames;++f,pcm+=6,out+=2) {
                out[0]=sample24(pcm);out[1]=sample24(pcm+3);
            }
        }
    } else if(bits==16) {
        for(size_t f=0;f<frames;++f,pcm+=2,out+=2)out[0]=out[1]=sample16(pcm);
    } else if(bits==24) {
        for(size_t f=0;f<frames;++f,pcm+=3,out+=2)out[0]=out[1]=sample24(pcm);
    } else {
        for(size_t f=0;f<frames;++f,pcm+=4,out+=2)out[0]=out[1]=sample32(pcm);
    }
}
void audio_pcm_reset(audio_pcm_t *s,uint8_t *ring)
{
    memset(s,0,sizeof(*s));s->ring=ring;
    atomic_init(&s->head,0);atomic_init(&s->tail,0);
    atomic_init(&s->sent,0);atomic_init(&s->submitted,0);atomic_init(&s->feed,false);
    atomic_init(&s->underrun_frames,0);
    atomic_init(&s->min_queued_bytes,AUDIO_RING_BYTES);
}
size_t audio_pcm_push(audio_pcm_t *s,const uint8_t *pcm,size_t frames,unsigned bits,unsigned channels)
{
    if((bits!=16 && bits!=24 && bits!=32)||(channels!=1 && channels!=2))return 0;
    unsigned head=atomic_load_explicit(&s->head,memory_order_relaxed);
    unsigned tail=atomic_load_explicit(&s->tail,memory_order_acquire);
    size_t free_frames=(AUDIO_RING_BYTES-(unsigned)(head-tail))/8;
    if(frames>free_frames)frames=free_frames;
    size_t offset=head&(AUDIO_RING_BYTES-1);
    size_t first_frames=(AUDIO_RING_BYTES-offset)/8;
    if(first_frames>frames)first_frames=frames;
    if(bits==32 && channels==2) {
        memcpy(s->ring+offset,pcm,first_frames*8);
        if(frames>first_frames)memcpy(s->ring,pcm+first_frames*8,(frames-first_frames)*8);
    } else {
        pack_contiguous(s->ring+offset,pcm,first_frames,bits,channels);
        if(frames>first_frames)
            pack_contiguous(s->ring,pcm+first_frames*channels*(bits/8),
                            frames-first_frames,bits,channels);
    }
    atomic_fetch_add_explicit(&s->submitted,frames,memory_order_relaxed);
    atomic_store_explicit(&s->head,head+frames*8,memory_order_release);
    return frames;
}
void IRAM_ATTR audio_pcm_dma(audio_pcm_t *s,void *dma,size_t bytes)
{
    unsigned slot=0;
    while(slot<AUDIO_DMA_COUNT && s->dma[slot].address && s->dma[slot].address!=dma)++slot;
    if(slot==AUDIO_DMA_COUNT) {memset(dma,0,bytes);return;}
    s->dma[slot].address=dma;
    atomic_fetch_add_explicit(&s->sent,s->dma[slot].frames,memory_order_release);
    s->dma[slot].frames=0;
    if(!atomic_load_explicit(&s->feed,memory_order_acquire)) {
        memset(dma,0,bytes);return;
    }
    unsigned tail=atomic_load_explicit(&s->tail,memory_order_relaxed);
    unsigned head=atomic_load_explicit(&s->head,memory_order_acquire);
    size_t take=(unsigned)(head-tail);
    if(take>bytes)take=bytes;
    take&=~(size_t)7;
    /* 正常满块无需先清零再覆盖；只清欠载尾部，暂停不消耗ring。 */
    if(take<bytes) {
        memset((uint8_t*)dma+take,0,bytes-take);
        atomic_fetch_add_explicit(&s->underrun_frames,(bytes-take)/8,memory_order_relaxed);
    }
    size_t first=AUDIO_RING_BYTES-(tail&(AUDIO_RING_BYTES-1));
    if(first>take)first=take;
    memcpy(dma,s->ring+(tail&(AUDIO_RING_BYTES-1)),first);
    if(take>first)memcpy((uint8_t*)dma+first,s->ring,take-first);
    /* 只有 ISR 降低水位；任务每个诊断窗口 exchange 重置。记录 DMA 消费后
     * 的最低值，避免任务只在补完数据后采样而漏掉短暂见底。窗口边界允许
     * 一次采样落在任一侧，不增加 ISR 循环、锁或日志。 */
    unsigned left=(unsigned)(head-tail)-take;
    if(left<atomic_load_explicit(&s->min_queued_bytes,memory_order_relaxed))
        atomic_store_explicit(&s->min_queued_bytes,left,memory_order_relaxed);
    s->dma[slot].frames=take/8;
    atomic_store_explicit(&s->tail,tail+take,memory_order_release);
}
bool audio_pcm_drained(audio_pcm_t *s)
{
    return atomic_load_explicit(&s->sent,memory_order_acquire)==atomic_load(&s->submitted);
}

size_t IRAM_ATTR audio_pcm_queued(const audio_pcm_t *s)
{
    unsigned tail=atomic_load_explicit(&s->tail,memory_order_acquire);
    unsigned head=atomic_load_explicit(&s->head,memory_order_acquire);
    return (unsigned)(head-tail);
}
