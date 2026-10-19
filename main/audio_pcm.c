#include "audio_pcm.h"
#include <string.h>
#include "esp_attr.h"
_Static_assert(ATOMIC_INT_LOCK_FREE==2,"DMA counters must be lock free");
void audio_pcm_reset(audio_pcm_t *s,uint8_t *ring)
{
    memset(s,0,sizeof(*s));s->ring=ring;
    atomic_init(&s->head,0);atomic_init(&s->tail,0);
    atomic_init(&s->sent,0);atomic_init(&s->submitted,0);atomic_init(&s->feed,false);
    atomic_init(&s->underrun_frames,0);
}
size_t audio_pcm_push(audio_pcm_t *s,const uint8_t *pcm,size_t frames,unsigned bits,unsigned channels)
{
    if((bits!=16 && bits!=24 && bits!=32)||(channels!=1 && channels!=2))return 0;
    unsigned head=atomic_load_explicit(&s->head,memory_order_relaxed);
    unsigned tail=atomic_load_explicit(&s->tail,memory_order_acquire);
    size_t free_frames=(AUDIO_RING_BYTES-(unsigned)(head-tail))/8;
    if(frames>free_frames)frames=free_frames;
    /* 按格式在循环外分支；32bit双声道已经是总线格式，只需最多两次复制。
     * 避免每个样本重新遍历字节；保留源数据的全部有效位。 */
    if(bits==32 && channels==2) {
        size_t bytes=frames*8;
        size_t first=AUDIO_RING_BYTES-(head&(AUDIO_RING_BYTES-1));
        if(first>bytes)first=bytes;
        memcpy(s->ring+(head&(AUDIO_RING_BYTES-1)),pcm,first);
        if(bytes>first)memcpy(s->ring,pcm+first,bytes-first);
    } else {
        unsigned width=bits/8;
        for(size_t f=0;f<frames;++f) {
            uint32_t stereo[2];
            for(unsigned c=0;c<channels;++c) {
                const uint8_t *p=pcm+(f*channels+c)*width;
                uint32_t value=(uint32_t)p[0]|(uint32_t)p[1]<<8;
                if(bits>=24)value|=(uint32_t)p[2]<<16;
                if(bits==32)value|=(uint32_t)p[3]<<24;
                /* 无符号移位避免负样本左移的未定义行为。 */
                stereo[c]=value<<(32-bits);
            }
            if(channels==1)stereo[1]=stereo[0];
            memcpy(s->ring+((head+f*8)&(AUDIO_RING_BYTES-1)),stereo,8);
        }
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
