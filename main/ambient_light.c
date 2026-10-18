#include "ambient_light.h"
#include "esp_adc/adc_oneshot.h"
#include "esp_adc/adc_cali.h"
#include "esp_adc/adc_cali_scheme.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define LIGHT_GPIO 22 /* ESP32-P4: ADC1_CH6；通过驱动查询映射，不复用触摸I2C。 */
/* 十档映射和过渡参数集中维护。电压档距独立于ADC满量程：当前75mV一档。
 * 用户反馈整体偏暗，因此从100mV缩小到75mV，让同一光照对应更高亮度；
 * 后续按外壳透光率/实测电压只调LEVEL_MV_STEP：越小越亮，越大越暗。
 * 目标仍为10%..100%十档，采样频率和已验证的线性渐变时长保持不变。 */
#define SAMPLE_PERIOD_MS 100U
#define LEVEL_MV_STEP 75
#define LEVEL_COUNT 10
#define TRANSITION_MS 250U

static const char *TAG = "ambient_light";
static adc_oneshot_unit_handle_t s_adc;
static adc_cali_handle_t s_cali;
static adc_channel_t s_channel;
static bool s_sampled, s_have_target, s_ramping, s_read_fault;
static TickType_t s_sample_at, s_ramp_at;
static uint8_t s_from, s_target;

static uint8_t voltage_to_brightness(int mv)
{
    int level = mv / LEVEL_MV_STEP;
    if (level >= LEVEL_COUNT) level = LEVEL_COUNT - 1;
    return (uint8_t)((level + 1) * 10);
}

void ambient_light_reset(void)
{
    s_sampled = s_have_target = s_ramping = false;
}

bool ambient_light_is_transitioning(void)
{
    return s_ramping;
}

esp_err_t ambient_light_init(void)
{
    if (s_adc) return ESP_ERR_INVALID_STATE;
    adc_unit_t unit;
    esp_err_t ret = adc_oneshot_io_to_channel(LIGHT_GPIO, &unit, &s_channel);
    if (ret != ESP_OK) return ret;
    adc_oneshot_unit_init_cfg_t config = {.unit_id = unit, .ulp_mode = ADC_ULP_MODE_DISABLE};
    ret = adc_oneshot_new_unit(&config, &s_adc);
    if (ret != ESP_OK) return ret;
    adc_oneshot_chan_cfg_t channel = {.atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT};
    ret = adc_oneshot_config_channel(s_adc, s_channel, &channel);
    if (ret == ESP_OK) {
        adc_cali_curve_fitting_config_t calibration = {
            .unit_id = unit, .chan = s_channel,
            .atten = ADC_ATTEN_DB_12, .bitwidth = ADC_BITWIDTH_DEFAULT,
        };
        ret = adc_cali_create_scheme_curve_fitting(&calibration, &s_cali);
    }
    if (ret != ESP_OK) {
        /* 不将未经校准的原始ADC值冒充mV；初始化失败保留手动亮度和UI开关，
         * 不因一个可选传感器让播放器启动失败，也不在后台无限申请ADC资源。 */
        adc_oneshot_del_unit(s_adc);
        s_adc = NULL;
        return ret;
    }
    ambient_light_reset();
    ESP_LOGI(TAG, "ALS GPIO22 ADC1_CH6, calibrated 12dB; sample=100ms, 10 levels, linear ramp=250ms");
    return ESP_OK;
}

bool ambient_light_poll(uint8_t current, uint8_t *suggested)
{
    if (!s_adc || !s_cali || !suggested) return false;
    TickType_t now = xTaskGetTickCount();
    if (!s_sampled || (TickType_t)(now - s_sample_at) >= pdMS_TO_TICKS(SAMPLE_PERIOD_MS)) {
        s_sampled = true;
        s_sample_at = now;
        int raw, mv;
        /* 每100ms只做一次ADC读取+校准，直接选档；不做样本排序、均值、EMA
         * 或连续确认。仅检查驱动/校准是否成功，不把通信失败当成环境变暗。 */
        esp_err_t ret = adc_oneshot_read(s_adc, s_channel, &raw);
        if (ret == ESP_OK) ret = adc_cali_raw_to_voltage(s_cali, raw, &mv);
        if (ret != ESP_OK || mv < 0) {
            if (!s_read_fault) ESP_LOGW(TAG, "ADC sample/calibration failed; keeping brightness");
            s_read_fault = true;
            s_have_target = s_ramping = false;
            return false;
        }
        s_read_fault = false;
        uint8_t target = voltage_to_brightness(mv);
        /* 同一档不重新启动过渡，否则100ms采样会不断打断250ms直线，使它变成
         * 永远接近目标的慢速曲线。跨档时从屏幕当前值出发，直接响应新目标，
         * 无需等旧档位过渡完毕；这样反向变化也不会先走到过时的亮度。 */
        if (!s_have_target || target != s_target) {
            s_have_target = true;
            s_target = target;
            s_from = current;
            s_ramp_at = now;
            s_ramping = current != target;
        }
    }
    if (!s_have_target) return false;
    int brightness = s_target;
    if (s_ramping) {
        TickType_t elapsed = now - s_ramp_at;
        TickType_t duration = pdMS_TO_TICKS(TRANSITION_MS);
        if (elapsed >= duration) s_ramping = false;
        else {
            /* 固定时长直线插值：起点/终点不随同档采样改变。按真实经过时间
             * 计算，任务繁忙时跳过迟到的中间步，不循环补发积压亮度命令。
             * 手动/熄屏会reset，自动过渡不能覆盖用户手动值或把熄屏点亮。 */
            int delta = (int)s_target - s_from;
            int change = (int)((int64_t)delta * elapsed / duration);
            brightness = s_from + change;
        }
    }
    *suggested = (uint8_t)brightness;
    /* 目标已到时仍按current比较，使LVGL发布失败后能重试最终值，而不会
     * 因内部过渡完成标志提前清除而永久遗漏最后一档。稳定后不重复写屏幕。 */
    return *suggested != current;
}
