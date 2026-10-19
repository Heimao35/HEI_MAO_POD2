#pragma once
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define AUDIO_RING_BYTES (64U*1024U)
#define AUDIO_DMA_COUNT 8U
#define AUDIO_DMA_FRAMES 256U
/* 每块 256 帧 × 8 字节 = 2048 字节，8 块共 16KiB，保持原有 DMA 内存预算。
 * 旧 48/32KiB 水位在 96kHz 下仅剩约 43ms 才开始补充，覆盖不了实测 67ms
 * 解码突发。改为剩余两个 DMA 块空间时唤醒，保留约 80ms 内部 PCM 储备；
 * 高低水位相差一个 DMA 块，仍通过通知阻塞，禁止满队列忙等。
 * 这只能吸收有界突发，不能解决平均解码速度慢于实时的问题。 */
#define AUDIO_DMA_BYTES (AUDIO_DMA_FRAMES * 8U)
#define AUDIO_PCM_LOW_BYTES (AUDIO_RING_BYTES - 2U * AUDIO_DMA_BYTES)
#define AUDIO_PCM_HIGH_BYTES (AUDIO_RING_BYTES - AUDIO_DMA_BYTES)
/* 有界SPSC：任务写、DMA ISR读。ring和本结构必须在内部SRAM。
 * 每帧固定双声道32bit；samples/sent计数自然回绕，差值始终受队列容量约束。 */
typedef struct {
    uint8_t *ring;
    atomic_uint head,tail,sent,submitted;
    atomic_uint underrun_frames; /* 仅feed=true时统计欠载，不计启动静音/暂停 */
    atomic_uint min_queued_bytes; /* DMA 消费后的最低 ring 水位，每 5s 重置 */
    atomic_bool feed;
    struct {void *address;uint32_t frames;} dma[AUDIO_DMA_COUNT];
} audio_pcm_t;
/* ring 必须是至少 AUDIO_RING_BYTES 字节、4 字节对齐的内部 SRAM。 */
void audio_pcm_reset(audio_pcm_t *s,uint8_t *ring);
size_t audio_pcm_push(audio_pcm_t *s,const uint8_t *pcm,size_t frames,unsigned bits,unsigned channels);
/* ISR中只复制有限字节/清零及无锁32bit计数，不解码、不触碰文件或I2C。 */
void audio_pcm_dma(audio_pcm_t *s,void *dma,size_t bytes);
bool audio_pcm_drained(audio_pcm_t *s);
size_t audio_pcm_queued(const audio_pcm_t *s);
