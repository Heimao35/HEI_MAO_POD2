/**
 * @file jpeg_codec.c
 * @brief 共享 JPEG decoder engine 与跨模块串行化。
 */

#include "jpeg_codec.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "jpeg_codec";

#define JPEG_CODEC_TIMEOUT_MS  250U
#define JPEG_CODEC_RETRY_US    1000000LL

static SemaphoreHandle_t s_lock;
static jpeg_decoder_handle_t s_decoder;
static int64_t s_retry_at_us;

esp_err_t jpeg_codec_init(void)
{
    if (s_lock) return ESP_OK;
    s_lock = xSemaphoreCreateMutex();
    return s_lock ? ESP_OK : ESP_ERR_NO_MEM;
}

esp_err_t jpeg_codec_process(const jpeg_decode_cfg_t *cfg,
                             const uint8_t *input, uint32_t input_len,
                             uint8_t *output, uint32_t output_cap,
                             uint32_t *output_len,
                             uint32_t timeout_ms)
{
    if (!cfg || !input || !input_len || !output || !output_cap || !output_len)
        return ESP_ERR_INVALID_ARG;
    if (!s_lock) return ESP_ERR_INVALID_STATE;
    if (xSemaphoreTake(s_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE)
        return ESP_ERR_TIMEOUT;

    esp_err_t ret = ESP_OK;
    if (!s_decoder) {
        int64_t now = esp_timer_get_time();
        if (now < s_retry_at_us) {
            ret = ESP_ERR_INVALID_STATE;
            goto done;
        }
        jpeg_decode_engine_cfg_t engine_cfg = {
            .intr_priority = 0,
            .timeout_ms = JPEG_CODEC_TIMEOUT_MS,
        };
        ret = jpeg_new_decoder_engine(&engine_cfg, &s_decoder);
        if (ret != ESP_OK) {
            s_decoder = NULL;
            s_retry_at_us = now + JPEG_CODEC_RETRY_US;
            ESP_LOGW(TAG, "JPEG decoder unavailable: %s; retry in 1 s",
                     esp_err_to_name(ret));
            goto done;
        }
        ESP_LOGI(TAG, "Shared JPEG decoder ready");
    }

    ret = jpeg_decoder_process(s_decoder, cfg, input, input_len,
                               output, output_cap, output_len);

done:
    xSemaphoreGive(s_lock);
    return ret;
}
