#pragma once

#include "esp_err.h"
#include <stdbool.h>

/* Starts AXP2101 on I2C1 (SDA48/SCL47, address 0x34), IRQ10.
 * Call after UI creation, without holding the LVGL lock. The first worker
 * iteration sets DCDC4=1.8V and ALDO3=3.3V, verifies and enables both outputs,
 * and samples battery/VBUS. Other rails, charger setpoints and power-key
 * shutdown policy are preserved; short-key notification is additionally enabled.
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
