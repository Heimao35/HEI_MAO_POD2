#pragma once
#include <stdint.h>
#include <stdbool.h>
bool lvgl_port_lock(uint32_t timeout);
void lvgl_port_unlock(void);
