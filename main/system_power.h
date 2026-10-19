#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
/* 统一终态动作；UI 回调只投递请求，不做 I2C/NVS/等待 worker。 */
typedef enum { SYSTEM_POWER_NONE, SYSTEM_POWER_OFF, SYSTEM_POWER_RESTART, SYSTEM_POWER_CANCEL } system_power_action_t;
esp_err_t system_power_init(void);
void system_power_key_edge(bool pressed);
void system_power_key_short(void);
void system_power_request(system_power_action_t action);
/* PMIC owner 在每次采样后投递完整 RAM 快照（含失败），无需 LVGL 锁，
 * 不执行保存/I2C/等待；初始化前的最后一份快照也保留供电源任务接管。 */
void system_power_update_battery(bool valid, bool present, bool pct_valid,
                                 uint8_t percent, bool vbus_present);
/* 按住电源键、弹窗、低电倒计时（含已关闭提示）及保存流程期间
 * 禁止锁屏超时/睡眠/再次短按，避免 30 秒期限被 Light-sleep 推迟。 */
bool system_power_is_active(void);
