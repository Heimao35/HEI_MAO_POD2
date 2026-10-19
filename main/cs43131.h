#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* 唯一调用者为audio_output任务；所有I2C均在LVGL/SD锁及ISR外执行。
 * I2C1复用PMIC总线，不创建第二个GPIO47/48控制器。
 * 外部24.576MHz有源时钟输入XTI/MCLK，保持无源晶振驱动下电。 */
esp_err_t cs43131_init(void);
esp_err_t cs43131_prepare(uint32_t sample_rate);
esp_err_t cs43131_start_hp(void);
esp_err_t cs43131_stop(void);
esp_err_t cs43131_volume(uint8_t percent, bool mute);
esp_err_t cs43131_poll(bool *headphone);
void cs43131_abort(void);
