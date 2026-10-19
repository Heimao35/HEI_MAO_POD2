#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Starts AXP2101 on I2C1 (SDA48/SCL47, address 0x34), IRQ10.
 * Call after UI creation, without holding the LVGL lock. The first worker
 * iteration sets DCDC4=1.8V and ALDO3=3.3V, verifies and enables both outputs,
 * and samples battery/VBUS. Other rails, charger setpoints and power-key
 * policy is set to 10s hardware poweroff; short/down/up IRQs are enabled.
 * Power configuration retries independently of battery
 * monitoring; already-configured outputs are not cycled by monitor retries.
 * A successful return means the task was created, not that physical output
 * voltages/current capability have been measured. */
esp_err_t pmic_axp2101_init(void);

/* Install before init. Called from monitor task after a debounced short press;
 * callback must only post an event, without blocking on UI or I/O. */
void pmic_axp2101_set_key_callback(void (*callback)(void));
/* Stop starting new I2C samples. true means the last sample has finished;
 * call outside LVGL lock and only enter sleep after this returns true. */
bool pmic_axp2101_pause_monitor(bool pause);

/* Install before init; debounced PWRON edges, callback queues RAM state only. */
void pmic_axp2101_set_key_edge_callback(void (*callback)(bool pressed));
/* Blocking terminal command, outside LVGL lock; executed by PMIC owner task. */
esp_err_t pmic_axp2101_poweroff(void);
/* 低电关机专用：owner 重新读取电量/电池/VBUS，只有有效 <=1% 且无
 * VBUS 才发 Soft PWROFF；条件恢复/I2C 失败时拒绝断电。 */
esp_err_t pmic_axp2101_poweroff_low_battery(void);

/* AXP2101整机POWOFF/POWON（0x10[1]），与关机共用PMIC所有者任务。
 * 在LVGL锁外调用；成功只代表I2C命令成功，不是供电波形验证。 */
esp_err_t pmic_axp2101_restart(void);

/* RAM快照：DAC必须等待两路供电配置验证成功后才释放RESET。 */
bool pmic_axp2101_outputs_ready(void);

/* Shared I2C1 owner scheduling for RTC requests/INT11. RAM notifications only;
 * the ISR form may only be called from a GPIO ISR. Not new wake sources. */
void pmic_axp2101_notify_service(void);
void pmic_axp2101_notify_service_from_isr(void);
