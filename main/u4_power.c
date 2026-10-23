/**
 * @file u4_power.c
 * @brief U4 CHIP_PU（P4 GPIO54）复位控制。
 *
 * 引脚：U4 CHIP_PU 接 P4 IO54，板上有外部上拉；P4 不驱动时 U4 正常
 * 上电启动。P4 启动早期主动拉低可把 U4 的蓝牙射频初始化推迟到 P4
 * 主要电源负载稳定之后。
 *
 * 注意：这里只控制 U4 的 EN，不关闭 AXP2101 ALDO3。U4 的 3.3 V 轨
 * 始终由 PMIC 保持；若 ALDO3 本身限流/过热，本模块无法解决。
 */

#include "u4_power.h"

#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define U4_EN_GPIO GPIO_NUM_54

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
    if (gpio_config(&cfg) == ESP_OK) {
        gpio_set_level(U4_EN_GPIO, level);
        s_configured = true;
    }
}

void u4_power_hold_reset(void)
{
    configure_output(0);
}

void u4_power_release(void)
{
    if (!s_configured) configure_output(1);
    else gpio_set_level(U4_EN_GPIO, 1);
}

void u4_power_pulse_reset(unsigned low_ms)
{
    if (!s_configured) configure_output(0);
    else gpio_set_level(U4_EN_GPIO, 0);
    vTaskDelay(pdMS_TO_TICKS(low_ms));
    gpio_set_level(U4_EN_GPIO, 1);
}
