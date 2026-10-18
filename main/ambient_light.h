#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* ALS-PT19: 3.3V -> sensor -> GPIO22/VOUT -> 10k -> GND.
 * 初始化在 LVGL 锁外执行；之后只由 screen_power 任务串行访问，无新任务/ISR/DMA。
 * 不提供 lux 读数：板上光路和器件离散性尚未标定，曲线只用于显示亮度控制。 */
esp_err_t ambient_light_init(void);
/* 手动/熄屏/关机时取消旧过渡；不读硬件，不改变用户自动亮度开关。 */
void ambient_light_reset(void);
/* 最多每100ms读一次并直接选择10%..100%十档，250ms直线过渡到目标。
 * 过渡期间由显示任务约每20ms调用；只返回变化值，不重复发布同一亮度。
 * 仅亮屏自动模式使用；ADC/校准必须在LVGL锁和ISR外，失败保留当前亮度。 */
bool ambient_light_poll(uint8_t current_brightness, uint8_t *suggested_brightness);
/* 显示任务据此在过渡时用20ms步进，稳定后恢复100ms轮询。仅同一任务访问。 */
bool ambient_light_is_transitioning(void);
