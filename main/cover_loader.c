#include "cover_loader.h"
#include "jpeg_codec.h"
#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include <stdio.h>
#include <stdlib.h>
#include <stdbool.h>
#include <sys/stat.h>

#define PIXEL_BYTES (COVER_WIDTH * COVER_HEIGHT * 2U)
typedef struct {
    uint8_t *pixels;
    size_t size;
    uint32_t generation;
    uint32_t index;
    uint32_t refs;
    uint64_t age;
    bool valid;
} cover_slot_t;
static cover_slot_t s_slots[COVER_CACHE_SLOTS];
static portMUX_TYPE s_cache_lock = portMUX_INITIALIZER_UNLOCKED;
static uint64_t s_age;
static const char *TAG = "cover_loader";

esp_err_t cover_loader_init(void)
{
    if (s_slots[0].pixels) return ESP_OK;
    jpeg_decode_memory_alloc_cfg_t mem = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    for (int i = 0; i < COVER_CACHE_SLOTS; ++i) {
        s_slots[i].pixels = jpeg_alloc_decoder_mem(PIXEL_BYTES, &mem, &s_slots[i].size);
        if (!s_slots[i].pixels) { cover_loader_destroy(); return ESP_ERR_NO_MEM; }
    }
    return ESP_OK;
}

const uint8_t *cover_loader_acquire(uint32_t generation, uint32_t index)
{
    const uint8_t *pixels = NULL;
    portENTER_CRITICAL(&s_cache_lock);
    for (int i = 0; i < COVER_CACHE_SLOTS; ++i) {
        cover_slot_t *slot = &s_slots[i];
        if (slot->valid && slot->generation == generation && slot->index == index) {
            ++slot->refs;
            slot->age = ++s_age;
            pixels = slot->pixels;
            break;
        }
    }
    portEXIT_CRITICAL(&s_cache_lock);
    return pixels;
}

void cover_loader_retain(const uint8_t *pixels)
{
    if (!pixels) return;
    portENTER_CRITICAL(&s_cache_lock);
    for (int i = 0; i < COVER_CACHE_SLOTS; ++i)
        if (s_slots[i].pixels == pixels) { ++s_slots[i].refs; break; }
    portEXIT_CRITICAL(&s_cache_lock);
}

void cover_loader_release(const uint8_t *pixels)
{
    if (!pixels) return;
    portENTER_CRITICAL(&s_cache_lock);
    for (int i = 0; i < COVER_CACHE_SLOTS; ++i)
        if (s_slots[i].pixels == pixels && s_slots[i].refs) { --s_slots[i].refs; break; }
    portEXIT_CRITICAL(&s_cache_lock);
}

void cover_loader_invalidate(void)
{
    portENTER_CRITICAL(&s_cache_lock);
    for (int i = 0; i < COVER_CACHE_SLOTS; ++i) s_slots[i].valid = false;
    portEXIT_CRITICAL(&s_cache_lock);
}

/* 只复用未被任何显示对象/worker 持有的槽。先占用并撤销旧键，再离开
 * 临界区解码；UI 即使恰好查询目标歌曲，也不会读到半张图片。 */
static cover_slot_t *reserve_slot(void)
{
    cover_slot_t *oldest = NULL;
    portENTER_CRITICAL(&s_cache_lock);
    for (int i = 0; i < COVER_CACHE_SLOTS; ++i) {
        cover_slot_t *slot = &s_slots[i];
        if (slot->refs) continue;
        if (!oldest || !slot->valid || slot->age < oldest->age) oldest = slot;
        if (!slot->valid) break;
    }
    if (oldest) { oldest->valid = false; oldest->refs = 1; }
    portEXIT_CRITICAL(&s_cache_lock);
    return oldest;
}

esp_err_t cover_loader_load(const char *path, uint32_t generation, uint32_t index,
                            const uint8_t **pixels)
{
    if (!pixels) return ESP_ERR_INVALID_ARG;
    *pixels = NULL;
    if (!path || !path[0]) return ESP_ERR_NOT_FOUND;
    esp_err_t ret = cover_loader_init();
    if (ret != ESP_OK) return ret;
    *pixels = cover_loader_acquire(generation, index);
    if (*pixels) return ESP_OK;
    struct stat st;
    if (stat(path, &st) || st.st_size <= 0 || st.st_size > COVER_JPEG_MAX_BYTES)
        return ESP_ERR_INVALID_SIZE;
    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_FAIL;
    size_t allocated = 0;
    jpeg_decode_memory_alloc_cfg_t mem = {.buffer_direction = JPEG_DEC_ALLOC_INPUT_BUFFER};
    uint8_t *compressed = jpeg_alloc_decoder_mem((size_t)st.st_size, &mem, &allocated);
    if (!compressed) { fclose(fp); return ESP_ERR_NO_MEM; }
    size_t read = fread(compressed, 1, (size_t)st.st_size, fp);
    fclose(fp);
    if (read != (size_t)st.st_size) { free(compressed); return ESP_FAIL; }
    jpeg_decode_picture_info_t pic;
    ret = jpeg_decoder_get_info(compressed, read, &pic);
    if (ret == ESP_OK && (pic.width != COVER_WIDTH || pic.height != COVER_HEIGHT)) {
        ESP_LOGW(TAG, "Cover must be 240x240, got %lux%lu", (unsigned long)pic.width,
                 (unsigned long)pic.height);
        ret = ESP_ERR_INVALID_SIZE;
    }
    if (ret == ESP_OK) {
        /* 240 is a multiple of 16: no padded pixel row/stride mismatch. */
        cover_slot_t *slot = reserve_slot();
        if (!slot) { free(compressed); return ESP_ERR_INVALID_STATE; }
        jpeg_decode_cfg_t cfg = {.output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
                                 /* IDF 5.5.4: RGB selects byte-swapped/big-endian RGB565.
                                  * LVGL needs native little-endian RGB565 (BGR setting). */
                                 .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR};
        uint32_t out_size = 0;
        ret = jpeg_codec_process(&cfg, compressed, read, slot->pixels,
                                 slot->size, &out_size, 1000);
        if (ret == ESP_OK && out_size != PIXEL_BYTES) ret = ESP_ERR_INVALID_SIZE;
        if (ret == ESP_OK) {
            portENTER_CRITICAL(&s_cache_lock);
            slot->generation = generation;
            slot->index = index;
            slot->age = ++s_age;
            slot->valid = true;
            portEXIT_CRITICAL(&s_cache_lock);
            *pixels = slot->pixels; /* reserve_slot 的引用交给调用者。 */
        } else {
            cover_loader_release(slot->pixels);
        }
    }
    free(compressed);
    return ret;
}

void cover_loader_destroy(void)
{
    /* 共享 decoder engine 归 jpeg_codec 所有，不能在这里删除。 */
    for (int i = 0; i < COVER_CACHE_SLOTS; ++i) {
        free(s_slots[i].pixels);
        s_slots[i] = (cover_slot_t){0};
    }
    s_age = 0;
}
