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
    if (!s_lock) return ESP_ERR_NO_MEM;

    /* 提前创建共享硬件引擎，而不是等到锁屏动画（开机约 15 秒）首次
     * 解码时才惰性创建。jpeg_new_decoder_engine() 的 rx/txlink 需要
     * 128B 对齐的内部 DMA 内存；到那时音频 PCM 环、I2S DMA 和媒体
     * worker 已经占用/碎片化内部堆，实测首次上电会出现
     * "no memory for jpeg decode rxlink"，锁屏动画要等 1 秒重试后才
     * 能解码。此处紧跟在 LVGL 初始化之后、所有媒体 worker 启动之前，
     * 内部 DMA 仍完整，可以稳定拿到这块小内存。
     *
     * 失败时仍保留原惰性重试逻辑（jpeg_codec_process 每秒重试一次），
     * 不阻止开机，也不改变任何解码参数。 */
    jpeg_decode_engine_cfg_t engine_cfg = {
        .intr_priority = 0,
        .timeout_ms = JPEG_CODEC_TIMEOUT_MS,
    };
    esp_err_t ret = jpeg_new_decoder_engine(&engine_cfg, &s_decoder);
    if (ret == ESP_OK) {
        ESP_LOGI(TAG, "Shared JPEG decoder ready (early)");
    } else {
        s_decoder = NULL;
        s_retry_at_us = esp_timer_get_time() + JPEG_CODEC_RETRY_US;
        ESP_LOGW(TAG, "Early JPEG decoder init failed: %s; retry on first use",
                 esp_err_to_name(ret));
    }
    return ESP_OK;
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
