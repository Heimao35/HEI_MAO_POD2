/** BMI270 integration, Bosch SensorAPI v2.86.1 (vendored, BSD-3-Clause).
 * A single small worker owns the device/config pages. No sample stream,
 * FIFO, per-sample allocation, software attitude calculation or SD access.
 * Datasheet BST-BMI270-DS000-05: sections 4.4/4.7/4.10/4.12.
 */
#include "motion_sensor.h"
#include "third_party/bmi270/bmi270.h"
#include "rtc_clock.h"
#include "screen_power.h"
#include "system_power.h"
#include "ui/ui_lockscreen.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>
#include <limits.h>
#include <string.h>

#define IMU_ADDRESS 0x68
#define IMU_IRQ_GPIO 12
#define IMU_TRANSFER 64U
#define IMU_TIMEOUT_MS 100
#define IMU_POLL_MS 1000U
#define IMU_RETRY_MS 5000U
#define FEATURE_STEPS 1U
#define FEATURE_WAKE 2U
#define OWNER_SLEEP 1U
#define OWNER_SHUTDOWN 2U
#define WORK_BUSY 4U

/* Board-to-watch coordinates, DS p53. No mechanical orientation was supplied:
 * start with Bosch's identity mapping. Change ONLY this map after board tests;
 * e.g. swap X/Y and negate one axis if the sensor is rotated on the PCB.
 * These are BMI2_X/Y/Z or BMI2_NEG_X/Y/Z, not numeric axis indexes. */
static const struct bmi2_remap s_board_axes = {BMI2_X, BMI2_Y, BMI2_Z};
static const char *TAG = "motion_sensor";
static i2c_master_dev_handle_t s_device;
static struct bmi2_dev s_bmi;
static TaskHandle_t s_worker;
static atomic_uint s_requested, s_gate;
static atomic_bool s_irq_hint, s_wake_ready;
static portMUX_TYPE s_snapshot_mux = portMUX_INITIALIZER_UNLOCKED;
static motion_sensor_snapshot_t s_snapshot;
/* The fields below are worker-only. The HW counter is monotonic across page
 * switches and screen sleep. Daily totals use unsigned deltas, allowing its
 * 32-bit wrap without an enormous erroneous increment. No Flash writes per
 * step: existing motion switches and daily totals remain session RAM state. */
static bool s_configured, s_attempted, s_polled, s_fault_logged;
static bool s_raw_valid, s_ui_dirty = true;
static unsigned s_applied;
static uint32_t s_last_raw, s_day, s_daily;
static TickType_t s_attempt_at, s_poll_at, s_fault_at;

static void notify(void)
{ if (s_worker) xTaskNotifyGive(s_worker); }
void motion_sensor_set_features(bool steps, bool wrist_wake)
{
    atomic_store(&s_requested, (steps ? FEATURE_STEPS : 0) | (wrist_wake ? FEATURE_WAKE : 0));
    notify();
}
void motion_sensor_get_snapshot(motion_sensor_snapshot_t *out)
{
    if (!out) return;
    portENTER_CRITICAL(&s_snapshot_mux);
    *out = s_snapshot;
    portEXIT_CRITICAL(&s_snapshot_mux);
}
static bool pause_owner(unsigned owner, bool pause)
{
    if (pause) return !(atomic_fetch_or(&s_gate, owner) & WORK_BUSY);
    unsigned previous = atomic_fetch_and(&s_gate, ~owner);
    if (previous & owner) notify();
    return true;
}
bool motion_sensor_sleep(bool pause) { return pause_owner(OWNER_SLEEP, pause); }
bool motion_sensor_shutdown(bool pause) { return pause_owner(OWNER_SHUTDOWN, pause); }
bool motion_sensor_wake_enabled(void)
{ return atomic_load(&s_wake_ready) && (atomic_load(&s_requested) & FEATURE_WAKE); }
void motion_sensor_notify_wake(void)
{ atomic_store(&s_irq_hint, true); notify(); }

static void IRAM_ATTR imu_irq(void *arg)
{
    (void)arg;
    atomic_store(&s_irq_hint, true);
    BaseType_t woken = pdFALSE;
    if (s_worker) vTaskNotifyGiveFromISR(s_worker, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static BMI2_INTF_RETURN_TYPE sensor_read(uint8_t reg, uint8_t *data, uint32_t len, void *context)
{
    (void)context;
    if (!len || len > IMU_TRANSFER) return -1;
    return i2c_master_transmit_receive(s_device, &reg, 1, data, len, IMU_TIMEOUT_MS) == ESP_OK ?
        BMI2_INTF_RET_SUCCESS : -1;
}
static BMI2_INTF_RETURN_TYPE sensor_write(uint8_t reg, const uint8_t *data, uint32_t len, void *context)
{
    (void)context;
    if (!len || len > IMU_TRANSFER) return -1;
    uint8_t bytes[IMU_TRANSFER + 1];
    bytes[0] = reg;
    memcpy(bytes + 1, data, len);
    esp_err_t ret = i2c_master_transmit(s_device, bytes, len + 1, IMU_TIMEOUT_MS);
    /* At 100kHz each 64B config chunk occupies the shared bus for ~6ms.
     * Yield between chunks so PMIC/DAC/RTC can run; never own a bus-wide lock
     * across the 8KiB upload. No watchdog suppression or higher task priority. */
    if (len >= 32) vTaskDelay(1);
    return ret == ESP_OK ? BMI2_INTF_RET_SUCCESS : -1;
}
static void sensor_delay(uint32_t us, void *context)
{
    (void)context;
    if (us >= 1000) {
        TickType_t ticks = pdMS_TO_TICKS((us + 999) / 1000);
        vTaskDelay(ticks ? ticks : 1);
    } else esp_rom_delay_us(us);
}

static int8_t configure_sensor(void)
{
    s_bmi = (struct bmi2_dev){.intf = BMI2_I2C_INTF, .read = sensor_read,
        .write = sensor_write, .delay_us = sensor_delay, .read_write_len = IMU_TRANSFER};
    int8_t rc = bmi270_init(&s_bmi); /* Checks CHIP_ID, resets and uploads Bosch config. */
    if (rc != BMI2_OK) return rc;
    uint8_t status = 0;
    rc = bmi2_get_internal_status(&status, &s_bmi);
    if (rc != BMI2_OK || (status & 0x0f) != 1) return BMI2_E_CONFIG_LOAD;
    rc = bmi2_set_remap_axes(&s_board_axes, &s_bmi);
    if (rc != BMI2_OK) return rc;
    struct bmi2_sens_config cfg = {.type = BMI2_ACCEL};
    rc = bmi270_get_sensor_config(&cfg, 1, &s_bmi);
    if (rc != BMI2_OK) return rc;
    /* Feature engine requires >=50Hz filtered accelerometer. Gyro stays off.
     * Bosch default wrist/step tuning is kept; no unverified custom thresholds. */
    cfg.cfg.acc.odr = BMI2_ACC_ODR_50HZ;
    cfg.cfg.acc.range = BMI2_ACC_RANGE_2G;
    cfg.cfg.acc.bwp = BMI2_ACC_NORMAL_AVG4;
    cfg.cfg.acc.filter_perf = BMI2_POWER_OPT_MODE;
    rc = bmi270_set_sensor_config(&cfg, 1, &s_bmi);
    if (rc != BMI2_OK) return rc;
    struct bmi2_int_pin_config pin = {.pin_type = BMI2_INT1, .int_latch = BMI2_INT_LATCH};
    pin.pin_cfg[0] = (struct bmi2_int_pin_cfg){.lvl = BMI2_INT_ACTIVE_LOW,
        .od = BMI2_INT_OPEN_DRAIN, .output_en = BMI2_INT_OUTPUT_ENABLE,
        .input_en = BMI2_INT_INPUT_DISABLE};
    rc = bmi2_set_int_pin_config(&pin, &s_bmi);
    if (rc != BMI2_OK) return rc;
    const uint8_t temperature = BMI2_TEMP;
    rc = bmi270_sensor_enable(&temperature, 1, &s_bmi);
    if (rc != BMI2_OK) return rc;
    rc = bmi2_set_adv_power_save(BMI2_ENABLE, &s_bmi);
    if (rc == BMI2_OK) {
        /* A reset restarts HW count; retain software daily total after retry.
         * Raw zero is known here, so first valid sample is not discarded. */
        s_last_raw = 0; s_raw_valid = true; s_applied = 0;
        atomic_store(&s_irq_hint, false);
    }
    return rc;
}

static int8_t apply_features(unsigned requested)
{
    atomic_store(&s_wake_ready, false);
    uint8_t disable[3], enable[3];
    unsigned nd = 0, ne = 0;
    if ((s_applied & FEATURE_WAKE) && !(requested & FEATURE_WAKE)) disable[nd++] = BMI2_WRIST_WEAR_WAKE_UP;
    if ((s_applied & FEATURE_STEPS) && !(requested & FEATURE_STEPS)) disable[nd++] = BMI2_STEP_COUNTER;
    /* Enable accel before features; disable it only after features are off.
     * Temperature remains independently enabled (~0.78Hz with gyro off). */
    bool old_acc = s_applied != 0, new_acc = requested != 0;
    if (old_acc && !new_acc) disable[nd++] = BMI2_ACCEL;
    if (!old_acc && new_acc) enable[ne++] = BMI2_ACCEL;
    if (!(s_applied & FEATURE_STEPS) && (requested & FEATURE_STEPS)) enable[ne++] = BMI2_STEP_COUNTER;
    if (!(s_applied & FEATURE_WAKE) && (requested & FEATURE_WAKE)) enable[ne++] = BMI2_WRIST_WEAR_WAKE_UP;
    int8_t rc = nd ? bmi270_sensor_disable(disable, nd, &s_bmi) : BMI2_OK;
    if (rc == BMI2_OK && ne) rc = bmi270_sensor_enable(enable, ne, &s_bmi);
    struct bmi2_sens_int_config map = {.type = BMI2_WRIST_WEAR_WAKE_UP,
        .hw_int_pin = (requested & FEATURE_WAKE) ? BMI2_INT1 : BMI2_INT_NONE};
    if (rc == BMI2_OK) rc = bmi270_map_feat_int(&map, 1, &s_bmi);
    /* Drain stale latched status when changing switches: enabling must not
     * replay an old gesture. Only wrist is mapped; step counts are polled. */
    uint16_t ignored;
    if (rc == BMI2_OK) rc = bmi2_get_int_status(&ignored, &s_bmi);
    if (rc == BMI2_OK) {
        s_applied = requested;
        atomic_store(&s_wake_ready, (requested & FEATURE_WAKE) != 0);
    }
    return rc;
}

static uint32_t current_day(void)
{
    rtc_clock_snapshot_t clock;
    rtc_clock_get_snapshot(&clock);
    if (!clock.valid || !clock.online || clock.set_pending) return 0;
    return clock.time.year * 10000U + clock.time.month * 100U + clock.time.day;
}
static void update_steps(uint32_t raw, uint32_t day)
{
    /* Date change (including manual RTC edits) starts a new daily bucket.
     * Invalid RTC never invents a date or repeatedly clears the counter.
     * First valid date adopts current-boot steps accumulated before setting
     * the clock; subsequent dates use the latest HW sample as the baseline. */
    uint32_t delta = s_raw_valid ? raw - s_last_raw : 0;
    s_last_raw = raw; s_raw_valid = true;
    if (day && s_day && day != s_day) s_daily = 0;
    else s_daily = delta > UINT32_MAX - s_daily ? UINT32_MAX : s_daily + delta;
    if (day) s_day = day;
}
static void save_snapshot(bool online, bool step_ok, uint32_t day, bool temp_ok, int16_t temperature)
{
    motion_sensor_snapshot_t next = {.online = online,
        .steps_valid = online && step_ok && day != 0,
        .temperature_valid = online && temp_ok, .day = day, .steps = s_daily,
        .temperature_x10 = temperature};
    portENTER_CRITICAL(&s_snapshot_mux);
    if (next.online != s_snapshot.online || next.steps_valid != s_snapshot.steps_valid ||
        next.temperature_valid != s_snapshot.temperature_valid || next.day != s_snapshot.day ||
        next.steps != s_snapshot.steps || next.temperature_x10 != s_snapshot.temperature_x10)
        s_ui_dirty = true;
    s_snapshot = next;
    portEXIT_CRITICAL(&s_snapshot_mux);
}
static void publish_ui(void)
{
    if (!s_ui_dirty || !lvgl_port_lock(1)) return;
    /* Only publish into the existing labels. No alignment/font/animation,
     * backlight or activity changes: dark sampling cannot extend idle time.
     * UI callbacks can change requests before this lock; recheck the step bit
     * to avoid briefly reviving old numbers after the user turns it off. */
    motion_sensor_snapshot_t snap;
    motion_sensor_get_snapshot(&snap);
    ui_lockscreen_set_steps(snap.steps_valid && (atomic_load(&s_requested) & FEATURE_STEPS), snap.steps);
    ui_lockscreen_set_temperature(snap.temperature_valid, snap.temperature_x10);
    s_ui_dirty = false;
    lvgl_port_unlock();
}

static void sensor_step(void)
{
    unsigned gate = atomic_load(&s_gate);
    if (gate & (OWNER_SLEEP | OWNER_SHUTDOWN)) return;
    if (!atomic_compare_exchange_strong(&s_gate, &gate, gate | WORK_BUSY)) return;
    TickType_t now = xTaskGetTickCount();
    int8_t rc = BMI2_OK;
    unsigned requested = atomic_load(&s_requested);
    /* A stuck-low IRQ plus bus errors must not turn notifications into a
     * retry storm on the shared PMIC/DAC bus. Hints remain candidates; after
     * failure retry no faster than the normal 1s sampling cadence. */
    if (s_configured && s_fault_logged &&
        (TickType_t)(now - s_fault_at) < pdMS_TO_TICKS(IMU_POLL_MS)) goto done;
    if (!s_configured) {
        if (s_attempted && (TickType_t)(now - s_attempt_at) < pdMS_TO_TICKS(IMU_RETRY_MS)) goto done;
        s_attempted = true; s_attempt_at = now;
        rc = configure_sensor();
        if (rc != BMI2_OK) goto failed;
        s_configured = true; s_polled = false;
        ESP_LOGI(TAG, "BMI270 ready: 0x68 INT12, 50Hz accel, gyro off, die temperature");
    }
    if (requested != s_applied) {
        rc = apply_features(requested);
        if (rc != BMI2_OK) { s_configured = false; goto failed; }
        s_polled = false; s_ui_dirty = true;
    }
    bool hint = atomic_exchange(&s_irq_hint, false);
    bool poll = !s_polled || s_fault_logged || (TickType_t)(now - s_poll_at) >= pdMS_TO_TICKS(IMU_POLL_MS);
    if (hint || poll || gpio_get_level(IMU_IRQ_GPIO) == 0) {
        uint16_t status = 0;
        rc = bmi2_get_int_status(&status, &s_bmi); /* read-to-clear latched INT1 */
        if (rc != BMI2_OK) goto failed;
        if ((status & BMI270_WRIST_WAKE_UP_STATUS_MASK) && motion_sensor_wake_enabled() &&
            screen_power_is_dark() && !system_power_is_active())
            screen_power_request_wake(); /* Same lock-page replay path as PWRON/touch. */
    }
    if (poll) {
        s_polled = true; s_poll_at = now;
        uint8_t internal = 0;
        rc = bmi2_get_internal_status(&internal, &s_bmi);
        if (rc != BMI2_OK) goto failed;
        if ((internal & 0x0f) != 1) { s_configured = false; rc = BMI2_E_CONFIG_LOAD; goto failed; }
        uint32_t day = current_day();
        bool step_ok = false;
        /* Read even while disabled: the frozen HW value provides the exact
         * midnight baseline, so re-enabling cannot carry yesterday's tail. */
        struct bmi2_feat_sensor_data count = {.type = BMI2_STEP_COUNTER};
        rc = bmi270_get_feature_data(&count, 1, &s_bmi);
        if (rc != BMI2_OK) goto failed;
        update_steps(count.sens_data.step_counter_output, day);
        step_ok = (requested & FEATURE_STEPS) != 0;
        int16_t raw = INT16_MIN;
        rc = bmi2_get_temperature_data(&raw, &s_bmi);
        if (rc != BMI2_OK) goto failed;
        /* DS p34: 23 + raw/512 degrees, 0x8000 invalid until first sample.
         * Use signed integer rounding; preserve negative die temperatures. */
        int32_t scaled = (int32_t)raw * 10;
        int16_t temperature = 230 + (scaled >= 0 ? scaled + 256 : scaled - 256) / 512;
        save_snapshot(true, step_ok, day, raw != INT16_MIN, temperature);
        atomic_store(&s_wake_ready, (s_applied & FEATURE_WAKE) != 0);
        s_fault_logged = false;
    }
    goto done;
failed:
    atomic_store(&s_wake_ready, false);
    save_snapshot(false, false, 0, false, 0);
    if (!s_fault_logged) {
        ESP_LOGW(TAG, "BMI270 access/config failed: %d; sensor values unknown", rc);
        /* Bosch在ID不匹配时把实读值保存到dev.chip_id；直接记录这次失败
         * 的原始ID，不重新读取0x00。其他错误路径该字段可能仍是预期ID，
         * 因而仅在DEV_NOT_FOUND时打印，避免把默认值误认为成功读回。 */
        if (rc == BMI2_E_DEV_NOT_FOUND) {
            ESP_LOGW(TAG, "BMI270 CHIP_ID mismatch: I2C1 addr=0x%02X reg=0x%02X actual=0x%02X expected=0x%02X",
                (unsigned)IMU_ADDRESS, (unsigned)BMI2_CHIP_ID_ADDR,
                (unsigned)s_bmi.chip_id, (unsigned)BMI270_CHIP_ID);
            /* ERR_REG和STATUS地址连续，一次只读事务取得两字节；沿用
             * 现有worker的BUSY屏障/100ms超时，在ISR与LVGL锁外执行。
             * 只在首次ID故障时采样，避免反复占用PMIC/DAC共享总线。
             * 读取失败只报错误，不打印未初始化或伪造的寄存器数值。 */
            const uint8_t diagnostic_reg = 0x02;
            uint8_t diagnostic[2];
            esp_err_t diagnostic_ret = i2c_master_transmit_receive(s_device,
                &diagnostic_reg, 1, diagnostic, sizeof(diagnostic), IMU_TIMEOUT_MS);
            if (diagnostic_ret == ESP_OK) {
                ESP_LOGW(TAG, "BMI270 diagnostic: I2C1 addr=0x%02X ERR_REG(0x02)=0x%02X STATUS(0x03)=0x%02X",
                    (unsigned)IMU_ADDRESS, (unsigned)diagnostic[0], (unsigned)diagnostic[1]);
            } else {
                ESP_LOGW(TAG, "BMI270 diagnostic read 0x02..0x03 failed: %s (0x%X)",
                    esp_err_to_name(diagnostic_ret), (unsigned)diagnostic_ret);
            }
        }
    }
    s_fault_logged = true;
    s_fault_at = xTaskGetTickCount();
done:
    publish_ui();
    atomic_fetch_and(&s_gate, ~WORK_BUSY);
}
static void sensor_worker(void *arg)
{
    (void)arg;
    for (;;) {
        sensor_step();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(IMU_POLL_MS));
    }
}
esp_err_t motion_sensor_init(void)
{
    if (s_device || s_worker) return ESP_ERR_INVALID_STATE;
    i2c_master_bus_handle_t bus;
    esp_err_t ret = i2c_master_get_bus_handle(I2C_NUM_1, &bus);
    if (ret != ESP_OK) return ret;
    const i2c_device_config_t cfg = {.dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = IMU_ADDRESS, .scl_speed_hz = 100000};
    ret = i2c_master_bus_add_device(bus, &cfg, &s_device);
    if (ret != ESP_OK) return ret;
    const gpio_config_t irq = {.pin_bit_mask = 1ULL << IMU_IRQ_GPIO, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE};
    ret = gpio_config(&irq);
    if (ret != ESP_OK) goto fail;
    /* PMIC/touch already installed the shared service; never uninstall it. */
    ret = gpio_isr_handler_add(IMU_IRQ_GPIO, imu_irq, NULL);
    if (ret != ESP_OK) goto fail;
    if (xTaskCreate(sensor_worker, "motion_sensor", 6144, NULL, 2, &s_worker) != pdPASS) {
        gpio_isr_handler_remove(IMU_IRQ_GPIO);
        ret = ESP_ERR_NO_MEM; goto fail;
    }
    return ESP_OK;
fail:
    i2c_master_bus_rm_device(s_device); s_device = NULL;
    return ret;
}
