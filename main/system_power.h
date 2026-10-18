#pragma once
#include "esp_err.h"
#include <stdbool.h>
/* 统一终态动作；UI 回调只投递请求，不做 I2C/NVS/等待 worker。 */
typedef enum { SYSTEM_POWER_NONE, SYSTEM_POWER_OFF, SYSTEM_POWER_RESTART, SYSTEM_POWER_CANCEL } system_power_action_t;
esp_err_t system_power_init(void);
void system_power_key_edge(bool pressed);
void system_power_key_short(void);
void system_power_request(system_power_action_t action);
/* 按住电源键、弹窗和保存流程期间禁止锁屏超时/睡眠/再次短按。 */
bool system_power_is_active(void);
