#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch.h"
#include "lvgl.h"

/* Start after SD/media initialization, without the LVGL lock. Touch controller
 * and PMIC remain powered. This module never changes PMIC rail/key policy. */
esp_err_t screen_power_init(lv_display_t *display, lv_indev_t *input,
                            esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch);
/* PMIC task callback: queues the event; never waits for LVGL or hardware. */
void screen_power_key_short_press(void);
/* Existing brightness slider callback. Zero is reserved for screen-off;
 * restores the user's last nonzero brightness rather than a hardcoded value. */
void screen_power_set_brightness(uint8_t brightness);
/* UI自动亮度A开关回调：仅排队，不等待LVGL/ADC/DCS。硬件渐变独立于UI绘制。 */
void screen_power_set_auto_brightness(bool enabled);

/* “锁屏和壁纸 → 防误触”开关：enabled=false 表示熄屏后触摸不得唤醒屏幕。
 * 只写原子标志，可在UI回调/初始化任意阶段调用（屏幕电源任务尚未创建
 * 也安全）。真正的EXT1触摸唤醒源调整在熄屏进入Light-sleep前完成，
 * 电源键和抬手亮屏不受影响。 */
void screen_power_set_touch_wake(bool enabled);

/* Lock-free snapshot for input workers; includes brightness fade-out. */
bool screen_power_is_dark(void);

/* 系统电源任务与显示任务通过原子请求交接，不在持有 LVGL 锁时等待。
 * request_wake是已确认的软件主动唤醒，独立于需要I2C确认的原始触摸IRQ。 */
void screen_power_request_wake(void);
void screen_power_shutdown_display(bool off);
bool screen_power_display_suspended(void);
