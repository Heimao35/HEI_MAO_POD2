#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "audio_pcm.h"
static uint8_t ring[AUDIO_RING_BYTES],dma[AUDIO_DMA_COUNT][AUDIO_DMA_FRAMES*8];
static audio_pcm_t stream;
static void reset(void) {audio_pcm_reset(&stream,ring);memset(dma,0,sizeof(dma));atomic_store(&stream.feed,true);}
int main(void)
{
    reset();
    uint8_t s16[]={0,0x80,0xff,0x7f,0xff,0xff,0,0};
    assert(audio_pcm_push(&stream,s16,2,16,2)==2);
    assert(atomic_load(&stream.sent)==0 && !audio_pcm_drained(&stream));
    audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));
    uint32_t *words=(uint32_t*)dma[0];
    assert(words[0]==0x80000000 && words[1]==0x7fff0000 && words[2]==0xffff0000 && words[3]==0);
    assert(!audio_pcm_drained(&stream)); /* queued is not sent */
    audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));
    assert(atomic_load(&stream.sent)==2 && audio_pcm_drained(&stream));
    for(unsigned i=0;i<20;++i)audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));
    assert(atomic_load(&stream.sent)==2); /* underrun silence never advances clock */
    for(unsigned i=0;i<sizeof(dma[0]);++i)assert(!dma[0][i]);
    puts("PASS signed16 packing and DMA completion clock, underrun silence never counts");
    reset();uint8_t s24[]={0,0,0x80,0xff,0xff,0x7f};
    assert(audio_pcm_push(&stream,s24,2,24,1)==2);
    audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));
    assert(words[0]==0x80000000 && words[1]==0x80000000 && words[2]==0x7fffff00 && words[3]==0x7fffff00);
    reset();uint32_t s32[]={0x81234567,0x7abcdef0};
    assert(audio_pcm_push(&stream,(uint8_t*)s32,1,32,2)==1);
    audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));assert(words[0]==s32[0] && words[1]==s32[1]);
    puts("PASS packed24 mono duplication and bit-exact32 stereo");
    reset();uint8_t *large=malloc(AUDIO_RING_BYTES+8);assert(large);
    for(unsigned i=0;i<AUDIO_RING_BYTES+8;++i)large[i]=(uint8_t)(i*7);
    assert(audio_pcm_push(&stream,large,AUDIO_RING_BYTES/8+1,32,2)==AUDIO_RING_BYTES/8);
    assert(!audio_pcm_push(&stream,large,1,32,2));
    audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));assert(!memcmp(dma[0],large,sizeof(dma[0])));
    assert(audio_pcm_push(&stream,large,1,32,2)==1); /* wrap into newly free space */
    atomic_store(&stream.feed,false);
    unsigned tail=atomic_load(&stream.tail);
    audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));
    assert(atomic_load(&stream.sent)==AUDIO_DMA_FRAMES && atomic_load(&stream.tail)==tail);
    atomic_store(&stream.feed,true);
    audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));
    assert(!memcmp(dma[0],large+sizeof(dma[0]),sizeof(dma[0])));
    puts("PASS bounded ring backpressure, wrap and pause preserves unconsumed PCM");
    reset();unsigned start=UINT32_MAX-7;
    atomic_store(&stream.head,start);atomic_store(&stream.tail,start);
    atomic_store(&stream.submitted,UINT32_MAX);atomic_store(&stream.sent,UINT32_MAX);
    assert(audio_pcm_push(&stream,(uint8_t*)s32,1,32,2)==1);
    audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));assert(!audio_pcm_drained(&stream));
    audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));assert(audio_pcm_drained(&stream));
    puts("PASS ring and32bit frame-counter wrap");
    reset();unsigned near_end=AUDIO_RING_BYTES-16;
    atomic_store(&stream.head,near_end);atomic_store(&stream.tail,near_end);
    uint8_t split[40];for(unsigned i=0;i<sizeof(split);++i)split[i]=(uint8_t)(0x80+i);
    assert(audio_pcm_push(&stream,split,5,32,2)==5);
    assert(audio_pcm_queued(&stream)==sizeof(split));
    memset(dma[0],0xff,sizeof(dma[0]));
    audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));
    assert(!memcmp(dma[0],split,sizeof(split)) && !audio_pcm_queued(&stream));
    for(unsigned i=sizeof(split);i<sizeof(dma[0]);++i)assert(!dma[0][i]);
    assert(atomic_load(&stream.underrun_frames)==AUDIO_DMA_FRAMES-5);
    atomic_store(&stream.feed,false);audio_pcm_dma(&stream,dma[0],sizeof(dma[0]));
    assert(atomic_load(&stream.underrun_frames)==AUDIO_DMA_FRAMES-5);
    free(large);puts("PASS32bit two-span copy wraps bit-exactly; partial DMA zeros stale tail, pause not an underrun");
    return 0;
}
