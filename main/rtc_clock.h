#pragma once

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

/* PCF85063TP: 2000..2099, 24-hour UI, weekday 0=Sunday, 1=Monday..6=Saturday.
 * Weekday is a separate hardware counter and deliberately remains user editable. */
typedef struct {
    uint16_t year;
    uint8_t month, day, weekday, hour, minute, second;
} rtc_datetime_t;

typedef struct {
    rtc_datetime_t time;
    bool online;             /* Last I2C read succeeded; not proof time is calibrated. */
    bool valid;              /* OS=0, STOP=0 and all date/BCD fields are valid. */
    bool set_pending;
    uint32_t completed_id;
    esp_err_t set_result;
} rtc_clock_snapshot_t;

/* Called once by PMIC init after I2C1 exists, before its worker starts. No new
 * bus/task/wake source is created. RTC failure must not prevent PMIC startup. */
esp_err_t rtc_clock_init(void);
/* PMIC startup rollback only, before the owner worker exists. */
void rtc_clock_deinit(void);
/* Only the PMIC worker calls service, inside its monitor BUSY/pause gate. */
void rtc_clock_service(void);
/* RAM-only APIs: safe under the LVGL lock. The worker owns every I2C access;
 * a request is complete only after run-state and calendar readback succeed. */
void rtc_clock_get_snapshot(rtc_clock_snapshot_t *snapshot);
esp_err_t rtc_clock_request_set(const rtc_datetime_t *time, uint32_t *request_id);
uint8_t rtc_clock_days_in_month(unsigned year, unsigned month);
bool rtc_clock_datetime_valid(const rtc_datetime_t *time);
