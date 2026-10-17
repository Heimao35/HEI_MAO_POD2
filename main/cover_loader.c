#include "cover_loader.h"
#include "driver/jpeg_decode.h"
#include "esp_log.h"
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>

#define PIXEL_BYTES (COVER_WIDTH * COVER_HEIGHT * 2U)
static jpeg_decoder_handle_t s_decoder;
static uint8_t *s_buffers[2];
static size_t s_sizes[2];
static const uint8_t *s_displayed;
static const char *TAG = "cover_loader";

esp_err_t cover_loader_init(void)
{
    if (s_decoder) return ESP_OK;
    jpeg_decode_engine_cfg_t cfg = {.intr_priority = 0, .timeout_ms = 250};
    esp_err_t ret = jpeg_new_decoder_engine(&cfg, &s_decoder);
    if (ret != ESP_OK) return ret;
    jpeg_decode_memory_alloc_cfg_t mem = {.buffer_direction = JPEG_DEC_ALLOC_OUTPUT_BUFFER};
    for (int i = 0; i < 2; ++i) {
        s_buffers[i] = jpeg_alloc_decoder_mem(PIXEL_BYTES, &mem, &s_sizes[i]);
        if (!s_buffers[i]) { cover_loader_destroy(); return ESP_ERR_NO_MEM; }
    }
    return ESP_OK;
}

esp_err_t cover_loader_load(const char *path, const uint8_t **pixels)
{
    if (!pixels) return ESP_ERR_INVALID_ARG;
    *pixels = NULL;
    if (!path || !path[0]) return ESP_ERR_NOT_FOUND;
    esp_err_t ret = cover_loader_init();
    if (ret != ESP_OK) return ret;
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
        int slot = s_displayed == s_buffers[0] ? 1 : 0;
        jpeg_decode_cfg_t cfg = {.output_format = JPEG_DECODE_OUT_FORMAT_RGB565,
                                 /* IDF 5.5.4: RGB selects byte-swapped/big-endian RGB565.
                                  * LVGL needs native little-endian RGB565 (BGR setting). */
                                 .rgb_order = JPEG_DEC_RGB_ELEMENT_ORDER_BGR};
        uint32_t out_size = 0;
        ret = jpeg_decoder_process(s_decoder, &cfg, compressed, read, s_buffers[slot],
                                   s_sizes[slot], &out_size);
        if (ret == ESP_OK && out_size != PIXEL_BYTES) ret = ESP_ERR_INVALID_SIZE;
        if (ret == ESP_OK) *pixels = s_buffers[slot];
    }
    free(compressed);
    return ret;
}

void cover_loader_commit(const uint8_t *pixels) { s_displayed = pixels; }
void cover_loader_destroy(void)
{
    if (s_decoder) jpeg_del_decoder_engine(s_decoder);
    s_decoder = NULL;
    for (int i = 0; i < 2; ++i) { free(s_buffers[i]); s_buffers[i] = NULL; s_sizes[i] = 0; }
    s_displayed = NULL;
}
