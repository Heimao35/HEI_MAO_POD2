#pragma once
#include "esp_err.h"
typedef struct touch *esp_lcd_touch_handle_t;
typedef void (*esp_lcd_touch_interrupt_callback_t)(esp_lcd_touch_handle_t);
struct touch { struct { esp_lcd_touch_interrupt_callback_t interrupt_callback; void *user_data; } config; };
esp_err_t esp_lcd_touch_register_interrupt_callback(esp_lcd_touch_handle_t, esp_lcd_touch_interrupt_callback_t);

#include <stdint.h>
typedef struct { uint16_t x,y,strength; uint8_t track_id; } esp_lcd_touch_point_data_t;
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t touch, esp_lcd_touch_point_data_t *out,
                                 uint8_t *count, uint8_t capacity);
