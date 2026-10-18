#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
#define GPIO_NUM_34 34
#define GPIO_NUM_35 35
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *config);
int gpio_get_level(gpio_num_t gpio);
