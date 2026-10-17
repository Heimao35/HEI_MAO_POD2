#include "music_background.h"
#include "cover_loader.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdlib.h>

#define SMALL MUSIC_BACKGROUND_SMALL_SIDE
#define SMALL_BYTES (SMALL * SMALL * 3U)
#define OUTPUT_BYTES (MUSIC_BACKGROUND_WIDTH * MUSIC_BACKGROUND_HEIGHT * 2U)
_Static_assert(COVER_WIDTH == SMALL * 3 && COVER_HEIGHT == SMALL * 3,
               "Background downsampling requires a 240x240 cover");
_Static_assert(MUSIC_BACKGROUND_WHITE_OPA <= 255, "Invalid white mix");

typedef struct {
    uint8_t *pixels;
    uint32_t generation, index, refs;
    uint64_t age;
    bool valid;
} background_slot_t;
static background_slot_t s_slots[MUSIC_BACKGROUND_SLOTS];
static uint8_t *s_work; /* 两张 RGB888 小图共 37.5KiB，整个任务复用。 */
static uint64_t s_age;
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;

esp_err_t music_background_init(void)
{
    if (s_work) return ESP_OK;
    for (int i = 0; i < MUSIC_BACKGROUND_SLOTS; ++i) {
        s_slots[i].pixels = heap_caps_malloc(OUTPUT_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!s_slots[i].pixels) { music_background_destroy(); return ESP_ERR_NO_MEM; }
    }
    s_work = heap_caps_malloc(2U * SMALL_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_work) { music_background_destroy(); return ESP_ERR_NO_MEM; }
    return ESP_OK;
}

const uint8_t *music_background_acquire(uint32_t generation, uint32_t index)
{
    const uint8_t *pixels = NULL;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MUSIC_BACKGROUND_SLOTS; ++i) {
        background_slot_t *slot = &s_slots[i];
        if (slot->valid && slot->generation == generation && slot->index == index) {
            ++slot->refs;
            slot->age = ++s_age;
            pixels = slot->pixels;
            break;
        }
    }
    portEXIT_CRITICAL(&s_lock);
    return pixels;
}

void music_background_retain(const uint8_t *pixels)
{
    if (!pixels) return;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MUSIC_BACKGROUND_SLOTS; ++i)
        if (s_slots[i].pixels == pixels) { ++s_slots[i].refs; break; }
    portEXIT_CRITICAL(&s_lock);
}

void music_background_release(const uint8_t *pixels)
{
    if (!pixels) return;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MUSIC_BACKGROUND_SLOTS; ++i)
        if (s_slots[i].pixels == pixels && s_slots[i].refs) { --s_slots[i].refs; break; }
    portEXIT_CRITICAL(&s_lock);
}

void music_background_invalidate(void)
{
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MUSIC_BACKGROUND_SLOTS; ++i) s_slots[i].valid = false;
    portEXIT_CRITICAL(&s_lock);
}

static background_slot_t *reserve_slot(void)
{
    background_slot_t *oldest = NULL;
    portENTER_CRITICAL(&s_lock);
    for (int i = 0; i < MUSIC_BACKGROUND_SLOTS; ++i) {
        background_slot_t *slot = &s_slots[i];
        if (slot->refs) continue;
        if (!oldest || !slot->valid || slot->age < oldest->age) oldest = slot;
        if (!slot->valid) break;
    }
    if (oldest) { oldest->valid = false; oldest->refs = 1; }
    portEXIT_CRITICAL(&s_lock);
    return oldest;
}

static int clamp_small(int x) { return x < 0 ? 0 : (x >= SMALL ? SMALL - 1 : x); }

/* 3×3 面积平均优于简单隔点采样：减少纹理缩小时的混叠。
 * 小图使用 RGB888 运算，避免多次模糊造成 RGB565 累积量化损失。 */
static void downsample(const uint16_t *cover, uint8_t *out)
{
    for (int y = 0; y < SMALL; ++y) {
        for (int x = 0; x < SMALL; ++x) {
            unsigned r = 0, g = 0, b = 0;
            for (int dy = 0; dy < 3; ++dy) {
                for (int dx = 0; dx < 3; ++dx) {
                    uint16_t c = cover[(y * 3 + dy) * COVER_WIDTH + x * 3 + dx];
                    unsigned cr = (c >> 11) & 31, cg = (c >> 5) & 63, cb = c & 31;
                    r += (cr << 3) | (cr >> 2);
                    g += (cg << 2) | (cg >> 4);
                    b += (cb << 3) | (cb >> 2);
                }
            }
            *out++ = (uint8_t)((r + 4) / 9);
            *out++ = (uint8_t)((g + 4) / 9);
            *out++ = (uint8_t)((b + 4) / 9);
        }
    }
}

/* 每行维护滑动窗口的和，半径增大也不会变成逐像素的二维卷积。
 * 边缘重复最外侧像素，避免全屏四边出现黑边。两张小图交替读写。 */
static void blur_axis(const uint8_t *src, uint8_t *dst, bool vertical)
{
    const int radius = MUSIC_BACKGROUND_BLUR_RADIUS;
    const unsigned diameter = radius * 2 + 1;
    int step = vertical ? SMALL * 3 : 3;
    for (int line = 0; line < SMALL; ++line) {
        for (int channel = 0; channel < 3; ++channel) {
            int start = (vertical ? line * 3 : line * SMALL * 3) + channel;
            unsigned sum = 0;
            for (int k = -radius; k <= radius; ++k) sum += src[start + clamp_small(k) * step];
            for (int p = 0; p < SMALL; ++p) {
                dst[start + p * step] = (uint8_t)((sum + diameter / 2) / diameter);
                sum += src[start + clamp_small(p + radius + 1) * step];
                sum -= src[start + clamp_small(p - radius) * step];
            }
        }
    }
}

/* 固定点双线性放大并烘焙白色遮罩，最终只保留可直接绘制的原生 RGB565。
 * UI 不必每帧重复缩放、模糊、或覆盖一张全屏半透明白色矩形。 */
static void expand_and_tint(const uint8_t *src, uint16_t *out)
{
    for (unsigned y = 0; y < MUSIC_BACKGROUND_HEIGHT; ++y) {
        unsigned fy = y * (SMALL - 1) * 256U / (MUSIC_BACKGROUND_HEIGHT - 1);
        unsigned sy = fy >> 8, wy = fy & 255U;
        unsigned ny = sy + (sy + 1 < SMALL);
        for (unsigned x = 0; x < MUSIC_BACKGROUND_WIDTH; ++x) {
            unsigned fx = x * (SMALL - 1) * 256U / (MUSIC_BACKGROUND_WIDTH - 1);
            unsigned sx = fx >> 8, wx = fx & 255U;
            unsigned nx = sx + (sx + 1 < SMALL);
            unsigned rgb[3];
            for (unsigned c = 0; c < 3; ++c) {
                unsigned top = src[(sy * SMALL + sx) * 3 + c] * (256U - wx) +
                               src[(sy * SMALL + nx) * 3 + c] * wx;
                unsigned bottom = src[(ny * SMALL + sx) * 3 + c] * (256U - wx) +
                                  src[(ny * SMALL + nx) * 3 + c] * wx;
                unsigned value = (top * (256U - wy) + bottom * wy + 32768U) >> 16;
                rgb[c] = (value * (255U - MUSIC_BACKGROUND_WHITE_OPA) +
                          255U * MUSIC_BACKGROUND_WHITE_OPA + 127U) / 255U;
            }
            *out++ = (uint16_t)(((rgb[0] >> 3) << 11) | ((rgb[1] >> 2) << 5) | (rgb[2] >> 3));
        }
    }
}

esp_err_t music_background_generate(const uint8_t *cover, uint32_t generation,
                                     uint32_t index, const uint8_t **pixels)
{
    if (!pixels) return ESP_ERR_INVALID_ARG;
    *pixels = NULL;
    if (!cover) return ESP_ERR_INVALID_ARG;
    if (!s_work) return ESP_ERR_INVALID_STATE;
    *pixels = music_background_acquire(generation, index);
    if (*pixels) return ESP_OK;
    background_slot_t *slot = reserve_slot();
    if (!slot) return ESP_ERR_INVALID_STATE;
    /* 单 worker 串行生成，处理期间不持有 LVGL / SD / 缓存锁。
     * 输出槽先撤销旧键并占用，任何读者都不能取得生成了一半的图像。 */
    downsample((const uint16_t *)cover, s_work);
    for (int pass = 0; pass < MUSIC_BACKGROUND_BLUR_PASSES; ++pass) {
        blur_axis(s_work, s_work + SMALL_BYTES, false);
        blur_axis(s_work + SMALL_BYTES, s_work, true);
    }
    expand_and_tint(s_work, (uint16_t *)slot->pixels);
    portENTER_CRITICAL(&s_lock);
    slot->generation = generation;
    slot->index = index;
    slot->age = ++s_age;
    slot->valid = true;
    portEXIT_CRITICAL(&s_lock);
    *pixels = slot->pixels; /* reserve_slot 的引用交给调用者。 */
    return ESP_OK;
}

void music_background_destroy(void)
{
    for (int i = 0; i < MUSIC_BACKGROUND_SLOTS; ++i) {
        free(s_slots[i].pixels);
        s_slots[i] = (background_slot_t){0};
    }
    free(s_work);
    s_work = NULL;
    s_age = 0;
}
