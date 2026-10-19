#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define AUDIO_RING_BYTES (64U*1024U)
#define AUDIO_DMA_COUNT 8U
#define AUDIO_DMA_FRAMES 256U
/* 3840字节/描述符：低于v5.5.4 P4对齐后的4032字节上限，并保持缓存行对齐。
 * 高低水位形成滞回，避免高优先级解码任务在满队列上轮询。 */
#define AUDIO_PCM_LOW_BYTES (AUDIO_RING_BYTES / 2U)
#define AUDIO_PCM_HIGH_BYTES (AUDIO_RING_BYTES * 3U / 4U)
/* 有界SPSC：任务写、DMA ISR读。ring和本结构必须在内部SRAM。
 * 每帧固定双声道32bit；samples/sent计数自然回绕，差值始终受队列容量约束。 */
typedef struct {
    uint8_t *ring;
    atomic_uint head,tail,sent,submitted;
    atomic_uint underrun_frames; /* 仅feed=true时统计欠载，不计启动静音/暂停 */
    atomic_bool feed;
    struct {void *address;uint32_t frames;} dma[AUDIO_DMA_COUNT];
} audio_pcm_t;
void audio_pcm_reset(audio_pcm_t *s,uint8_t *ring);
size_t audio_pcm_push(audio_pcm_t *s,const uint8_t *pcm,size_t frames,unsigned bits,unsigned channels);
/* ISR中只复制有限字节/清零及无锁32bit计数，不解码、不触碰文件或I2C。 */
void audio_pcm_dma(audio_pcm_t *s,void *dma,size_t bytes);
bool audio_pcm_drained(audio_pcm_t *s);
size_t audio_pcm_queued(const audio_pcm_t *s);
