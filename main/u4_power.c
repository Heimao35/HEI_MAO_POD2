/**
 * @file u4_power.c
 * @brief U4 CHIP_PU（P4 GPIO54）复位控制。
 *
 * 引脚：U4 CHIP_PU 接 P4 IO54。P4 启动时拉低保持复位；蓝牙打开时
 * 由 bt_link 释放 EN，蓝牙关闭且断开完成后再次拉低以省电。这里只控制
 * EN，不关闭 ALDO3。
 *
 * 注意：这里只控制 U4 的 EN，不关闭 AXP2101 ALDO3。U4 的 3.3 V 轨
 * 始终由 PMIC 保持；若 ALDO3 本身限流/过热，本模块无法解决。
 */

#include "u4_power.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define U4_EN_GPIO GPIO_NUM_54

static const char *TAG = "u4_power";
static bool s_configured;

static void configure_output(int level)
{
    const gpio_config_t cfg = {
        .pin_bit_mask = 1ULL << U4_EN_GPIO,
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    /* 外部上拉保证 P4 复位/掉电时 U4 仍能独立启动；P4 运行时推挽
     * 输出高电平，避免 EN 浮空。 */
    esp_err_t err = gpio_config(&cfg);
    if (err == ESP_OK) {
        gpio_set_level(U4_EN_GPIO, level);
        s_configured = true;
        ESP_LOGI(TAG, "U4 CHIP_PU -> %d (GPIO54 configured)", level);
    } else {
        ESP_LOGE(TAG, "U4 CHIP_PU GPIO54 config failed: %s", esp_err_to_name(err));
    }
}

void u4_power_hold_reset(void)
{
    configure_output(0);
}

void u4_power_release(void)
{
    if (!s_configured) configure_output(1);
    else {
        gpio_set_level(U4_EN_GPIO, 1);
        ESP_LOGI(TAG, "U4 CHIP_PU released (GPIO54 high)");
    }
}

void u4_power_pulse_reset(unsigned low_ms)
{
    if (!s_configured) configure_output(0);
    else gpio_set_level(U4_EN_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(low_ms));
    gpio_set_level(U4_EN_GPIO, 1);
}
