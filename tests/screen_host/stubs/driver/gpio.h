#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
#define GPIO_NUM_6 6
#define GPIO_NUM_10 10
#define GPIO_NUM_23 23
int gpio_get_level(gpio_num_t pin);
esp_err_t gpio_intr_enable(gpio_num_t pin);
esp_err_t gpio_intr_disable(gpio_num_t pin);
