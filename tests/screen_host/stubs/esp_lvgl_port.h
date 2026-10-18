#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"
bool lvgl_port_lock(uint32_t timeout);
void lvgl_port_unlock(void);
esp_err_t lvgl_port_stop(void);
esp_err_t lvgl_port_resume(void);
#define LVGL_PORT_EVENT_DISPLAY 1
#define LVGL_PORT_EVENT_TOUCH 2
esp_err_t lvgl_port_task_wake(int event, void *param);
