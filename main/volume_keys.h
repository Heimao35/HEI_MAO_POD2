#pragma once
#include "esp_err.h"

/* GPIO35 音量+、GPIO34 音量-。按键按下接地，内部上拉；主 UI 和
 * screen_power_init 完成后在 LVGL 锁外调用。无 GPIO ISR、无音频 I/O。 */
esp_err_t volume_keys_init(void);
