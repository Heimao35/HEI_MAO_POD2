/**
 * @file bt_audio_stream.c
 * @brief 蓝牙 PCM 环形缓冲 + 16 位立体声转换 + 线性重采样。
 *
 * 设计要点：
 * - 缓冲固定 16 KiB 内部 SRAM，约 85 ms @48 kHz 立体声；
 * - 生产者（音乐解码/视频音轨）与消费者（bt_link worker）用互斥量互斥；
 * - 线性插值保留跨块状态，块边界不会重新从头插值；
 * - 缓冲满时丢弃新样本并计数，绝不反压本机音频播放；
 * - 44.1↔48 kHz 使用线性插值；96/192 kHz 降采样未加低通滤波，
 *   可能有高频混叠，后续可替换为多相滤波器。
 */

#include "bt_audio_stream.h"

#include <stdatomic.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "bt_stream";

#define STREAM_RING_BYTES (16U * 1024U)
#define STREAM_RING_MASK  (STREAM_RING_BYTES - 1U)
#define STREAM_CHUNK_FRAMES 256U
/* 44.1 -> 48 的最大升采样约 1.09 倍；留 2 倍余量。 */
#define STREAM_OUT_FRAMES (STREAM_CHUNK_FRAMES * 2U)

static SemaphoreHandle_t s_lock;
static uint8_t *s_ring;
static uint32_t s_head;
static uint32_t s_tail;
static uint32_t s_src_rate;
static uint32_t s_dst_rate = 48000;
static bool s_have_prev;
static int16_t s_prev_l;
static int16_t s_prev_r;
static uint32_t s_frac; /* Q16，当前输出在 prev->cur 之间的位置 */
static atomic_uint s_dropped;
static int16_t s_chunk_out[STREAM_OUT_FRAMES * 2];

static int16_t decode_sample(const uint8_t *p, unsigned bits)
{
    if (bits == 16) {
        return (int16_t)(p[0] | (p[1] << 8));
    }
    if (bits == 24) {
        int32_t v = p[0] | (p[1] << 8) | (p[2] << 16);
        if (v & 0x800000) v |= ~0xFFFFFF; /* sign extend 24-bit */
        return (int16_t)(v >> 8);
    }
    int32_t v = p[0] | (p[1] << 8) | (p[2] << 16) | (p[3] << 24);
    return (int16_t)(v >> 16);
}

/**
 * @brief 把一个源块转换/重采样为 16-bit 立体声。
 *
 * 跨块保留 prev/frac，块边界不会丢失插值相位。src==dst 时每个输入帧
 * 产生一个输出帧；升采样在一个输入帧内产生多个输出；降采样则跨多个
 * 输入帧产生一个输出。
 */
static size_t resample_block(const uint8_t *pcm, size_t frames, unsigned bits,
                             unsigned channels, uint32_t src_rate,
                             uint32_t dst_rate, int16_t *out, size_t out_frames)
{
    uint32_t step = (uint32_t)(((uint64_t)src_rate << 16) / dst_rate);
    if (!step) step = 65536;

    const size_t stride = bits / 8U;
    const size_t frame_bytes = channels * stride;
    size_t produced = 0;

    for (size_t f = 0; f < frames; ++f) {
        const uint8_t *p = pcm + f * frame_bytes;
        int16_t l = decode_sample(p, bits);
        int16_t r = channels == 2 ? decode_sample(p + stride, bits) : l;

        if (!s_have_prev) {
            if (produced < out_frames) {
                out[produced * 2] = l;
                out[produced * 2 + 1] = r;
            }
            ++produced;
            s_prev_l = l;
            s_prev_r = r;
            s_frac = step;
            s_have_prev = true;
            continue;
        }

        while (s_frac < 65536U) {
            int32_t ol = (int32_t)(((int64_t)(l - s_prev_l) * s_frac) >> 16)
                         + s_prev_l;
            int32_t or_ = (int32_t)(((int64_t)(r - s_prev_r) * s_frac) >> 16)
                          + s_prev_r;
            if (ol > 32767) ol = 32767;
            if (ol < -32768) ol = -32768;
            if (or_ > 32767) or_ = 32767;
            if (or_ < -32768) or_ = -32768;
            if (produced < out_frames) {
                out[produced * 2] = (int16_t)ol;
                out[produced * 2 + 1] = (int16_t)or_;
            }
            ++produced;
            s_frac += step;
        }
        s_frac -= 65536U;
        s_prev_l = l;
        s_prev_r = r;
    }

    return produced < out_frames ? produced : out_frames;
}

esp_err_t bt_stream_init(void)
{
    if (s_lock) return ESP_OK;

    /* 16 KiB 放 PSRAM：链路 worker 读到 DMA 缓冲后再发送，不在 ISR
     * 访问；避免与 audio_output 的 64 KiB 内部 ring 争抢 SRAM。 */
    s_ring = heap_caps_malloc(STREAM_RING_BYTES,
                              MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_lock = xSemaphoreCreateMutex();
    if (!s_ring || !s_lock) {
        if (s_ring) heap_caps_free(s_ring);
        if (s_lock) vSemaphoreDelete(s_lock);
        s_ring = NULL;
        s_lock = NULL;
        return ESP_ERR_NO_MEM;
    }

    s_head = s_tail = 0;
    s_src_rate = 0;
    s_dst_rate = 48000;
    s_have_prev = false;
    s_prev_l = s_prev_r = 0;
    s_frac = 0;
    atomic_init(&s_dropped, 0);
    ESP_LOGI(TAG, "Bluetooth PCM stream ready: %u bytes",
             (unsigned)STREAM_RING_BYTES);
    return ESP_OK;
}

void bt_stream_reset(void)
{
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) return;
    s_head = s_tail = 0;
    s_have_prev = false;
    s_frac = 0;
    s_prev_l = s_prev_r = 0;
    atomic_store(&s_dropped, 0);
    xSemaphoreGive(s_lock);
}

void bt_stream_set_target_rate(uint32_t rate)
{
    if (rate != 44100 && rate != 48000) rate = 48000;
    if (!s_lock) return;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) return;
    if (rate != s_dst_rate) {
        s_dst_rate = rate;
        s_head = s_tail = 0;
        s_have_prev = false;
        s_frac = 0;
        ESP_LOGI(TAG, "Bluetooth PCM target rate: %u Hz", (unsigned)rate);
    }
    xSemaphoreGive(s_lock);
}

uint32_t bt_stream_target_rate(void)
{
    return s_dst_rate;
}

size_t bt_stream_write(const void *pcm, size_t frames, unsigned bits,
                       unsigned channels, uint32_t rate)
{
    if (!pcm || !frames || !s_lock || !s_ring) return 0;
    if ((bits != 16 && bits != 24 && bits != 32) ||
        (channels != 1 && channels != 2) || !rate)
        return 0;

    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) {
        atomic_fetch_add(&s_dropped, (unsigned)(frames * channels * (bits / 8U)));
        return 0;
    }

    /* 同一会话里采样率不应变化；变化时丢弃重采样相位，避免把两种
     * 时钟的样本插在一起。调用方在切歌/seek 时会先 reset。 */
    if (rate != s_src_rate) {
        s_src_rate = rate;
        s_have_prev = false;
        s_frac = 0;
    }

    const size_t stride = channels * (bits / 8U);
    size_t done = 0;
    while (done < frames) {
        size_t chunk = frames - done;
        if (chunk > STREAM_CHUNK_FRAMES) chunk = STREAM_CHUNK_FRAMES;

        size_t out_frames = resample_block((const uint8_t *)pcm + done * stride,
                                           chunk, bits, channels, rate,
                                           s_dst_rate, s_chunk_out,
                                           STREAM_OUT_FRAMES);
        size_t bytes = out_frames * 4U;

        uint32_t free_bytes = STREAM_RING_BYTES -
                              (uint32_t)(s_head - s_tail);
        if (bytes > free_bytes) {
            atomic_fetch_add(&s_dropped, (unsigned)(bytes - free_bytes));
            bytes = free_bytes;
        }
        if (bytes) {
            size_t offset = s_head & STREAM_RING_MASK;
            size_t first = STREAM_RING_BYTES - offset;
            if (first > bytes) first = bytes;
            memcpy(s_ring + offset, s_chunk_out, first);
            if (bytes > first)
                memcpy(s_ring, (const uint8_t *)s_chunk_out + first,
                       bytes - first);
            s_head += (uint32_t)bytes;
        }
        done += chunk;
    }

    xSemaphoreGive(s_lock);
    return done;
}

size_t bt_stream_read(uint8_t *out, size_t max_bytes)
{
    if (!out || !max_bytes || !s_lock) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) return 0;

    size_t available = (uint32_t)(s_head - s_tail);
    size_t take = available < max_bytes ? available : max_bytes;
    take &= ~(size_t)3U; /* 16-bit stereo frames only */
    if (take) {
        size_t offset = s_tail & STREAM_RING_MASK;
        size_t first = STREAM_RING_BYTES - offset;
        if (first > take) first = take;
        memcpy(out, s_ring + offset, first);
        if (take > first)
            memcpy(out + first, s_ring, take - first);
        s_tail += (uint32_t)take;
    }

    xSemaphoreGive(s_lock);
    return take;
}

size_t bt_stream_queued(void)
{
    if (!s_lock) return 0;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(10)) != pdTRUE) return 0;
    size_t queued = (uint32_t)(s_head - s_tail);
    xSemaphoreGive(s_lock);
    return queued;
}

uint32_t bt_stream_dropped_bytes(void)
{
    return atomic_load(&s_dropped);
}
