#pragma once

#include "esp_err.h"

/* Starts AXP2101 on I2C1 (SDA48/SCL47, address 0x34), IRQ10.
 * Call after UI creation, without holding the LVGL lock. The first worker
 * iteration sets DCDC4=1.8V and ALDO3=3.3V, verifies and enables both outputs,
 * and samples battery/VBUS. Other rails, charger setpoints and power-key
 * policy are preserved. Power configuration retries independently of battery
 * monitoring; already-configured outputs are not cycled by monitor retries.
 * A successful return means the task was created, not that physical output
 * voltages/current capability have been measured. */
esp_err_t pmic_axp2101_init(void);
