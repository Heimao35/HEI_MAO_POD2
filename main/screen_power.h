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

/* Lock-free snapshot for input workers; includes brightness fade-out. */
bool screen_power_is_dark(void);

/* 系统电源任务与显示任务通过原子请求交接，不在持有 LVGL 锁时等待。
 * request_wake是已确认的软件主动唤醒，独立于需要I2C确认的原始触摸IRQ。 */
void screen_power_request_wake(void);
void screen_power_shutdown_display(bool off);
bool screen_power_display_suspended(void);
