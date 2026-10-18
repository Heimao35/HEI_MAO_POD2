#pragma once
#include "esp_err.h"
typedef struct touch *esp_lcd_touch_handle_t;
typedef void (*esp_lcd_touch_interrupt_callback_t)(esp_lcd_touch_handle_t);
struct touch { struct { esp_lcd_touch_interrupt_callback_t interrupt_callback; void *user_data; } config; };
esp_err_t esp_lcd_touch_register_interrupt_callback(esp_lcd_touch_handle_t, esp_lcd_touch_interrupt_callback_t);
