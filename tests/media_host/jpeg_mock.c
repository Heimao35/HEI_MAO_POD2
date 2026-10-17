/* Deliberately NOT a JPEG decoder: checks buffer lifetime/error paths only. */
#include "driver/jpeg_decode.h"
#include "esp_heap_caps.h"
#include <string.h>
#include <assert.h>
esp_err_t jpeg_new_decoder_engine(const jpeg_decode_engine_cfg_t *cfg, jpeg_decoder_handle_t *h)
{ (void)cfg; *h = (void *)1; return ESP_OK; }
esp_err_t jpeg_del_decoder_engine(jpeg_decoder_handle_t h) { (void)h; return ESP_OK; }
void *jpeg_alloc_decoder_mem(size_t size, const jpeg_decode_memory_alloc_cfg_t *cfg, size_t *actual)
{ (void)cfg; *actual = size; return heap_caps_malloc(size, 0); }
esp_err_t jpeg_decoder_get_info(const uint8_t *data, uint32_t len, jpeg_decode_picture_info_t *info)
{
    if (len < 9 || memcmp(data, "JPGT", 4)) return ESP_FAIL;
    info->width = data[4] * 256U + data[5]; info->height = data[6] * 256U + data[7];
    return ESP_OK;
}
esp_err_t jpeg_decoder_process(jpeg_decoder_handle_t h, const jpeg_decode_cfg_t *cfg,
 const uint8_t *data, uint32_t len, uint8_t *out, uint32_t size, uint32_t *actual)
{
    (void)h; (void)len;
    assert(cfg->rgb_order == JPEG_DEC_RGB_ELEMENT_ORDER_BGR);
    assert(size >= 240*240*2);
    *actual = 240*240*2;
    memset(out, data[8], *actual);
    return ESP_OK;
}
