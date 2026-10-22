#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* BMI270: existing I2C1 SDA48/SCL47, CSB=1, SDO=0 -> 0x68; INT1=GPIO12.
 * All hardware I/O belongs to the background worker, never the UI/ISR. */
typedef struct {
    bool online, steps_valid, temperature_valid;
    uint32_t day, steps;       /* YYYYMMDD from the real RTC; current boot only. */
    int16_t temperature_x10;  /* Die temperature, NOT ambient temperature. */
} motion_sensor_snapshot_t;

esp_err_t motion_sensor_init(void);
/* RAM-only request, safe while holding the LVGL lock and also before
 * motion_sensor_init() (the worker applies the latest request on first poll).
 * The UI restores persisted switch intent at boot; without saved settings
 * both features stay off. Temperature is sampled independently. */
void motion_sensor_set_features(bool steps, bool wrist_wake);
void motion_sensor_get_snapshot(motion_sensor_snapshot_t *out);
/* Independent owner bits: releasing sleep must never release shutdown.
 * true ACK means no sensor I2C is running or can start until release. */
bool motion_sensor_sleep(bool pause);
bool motion_sensor_shutdown(bool pause);
/* Only a successfully configured, currently requested wrist feature may
 * add GPIO12 to EXT1. GPIO/RTC hints alone never turn on the display. */
bool motion_sensor_wake_enabled(void);
void motion_sensor_notify_wake(void);
