#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
#define GPIO_NUM_10 10
#define GPIO_NUM_47 47
#define GPIO_NUM_48 48
#define GPIO_MODE_INPUT 1
#define GPIO_PULLUP_ENABLE 1
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_NEGEDGE 2
typedef struct {
    uint64_t pin_bit_mask;
    int mode, pull_up_en, pull_down_en, intr_type;
} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t gpio, void (*handler)(void *), void *arg);
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio);
int gpio_get_level(gpio_num_t gpio);

#define GPIO_MODE_OUTPUT 2
esp_err_t gpio_set_level(gpio_num_t pin,int level);
