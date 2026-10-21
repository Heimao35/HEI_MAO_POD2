#pragma once
#include <stdint.h>
#include "esp_err.h"
#define ESP_EXT1_WAKEUP_ANY_LOW 0
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t pins, int mode);
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us);
esp_err_t esp_light_sleep_start(void);
uint64_t esp_sleep_get_ext1_wakeup_status(void);

esp_err_t esp_sleep_disable_ext1_wakeup_io(uint64_t pins);
