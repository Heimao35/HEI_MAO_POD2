#include "pmic_axp2101.h"
#include "driver/i2c_master.h"
#include "driver/gpio.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ui/ui_common.h"
#include "ui/ui_statusbar.h"
#include <stdatomic.h>

static const char *TAG = "axp2101";

#define PMIC_SDA_GPIO       GPIO_NUM_48
#define PMIC_SCL_GPIO       GPIO_NUM_47
#define PMIC_IRQ_GPIO       GPIO_NUM_10
#define PMIC_I2C_ADDRESS    0x34
#define PMIC_I2C_TIMEOUT_MS 100
#define PMIC_POLL_MS        5000
#define PMIC_RETRY_MS       2000
#define PMIC_IRQ_DRAIN_MAX  4

/* AXP2101 datasheet rev 1.4: pp.31-34, 41-45, 50, 52-54 and 56.
 * Register ownership is limited to the gauge/IRQs and the two user-requested
 * supplies. Charger setpoints and all other rails keep their existing values. */
#define REG_STATUS1         0x00
#define REG_STATUS2         0x01
#define REG_MODULE_ENABLE   0x18
#define REG_IRQ_ENABLE0     0x40
#define REG_IRQ_STATUS0     0x48
#define REG_DCDC_ENABLE     0x80
#define REG_DCDC4_VOLTAGE   0x85
#define REG_LDO_ENABLE      0x90
#define REG_ALDO3_VOLTAGE   0x94
#define REG_BATTERY_PERCENT 0xA4
#define DCDC4_VOLTAGE_MASK  0x7F
#define ALDO3_VOLTAGE_MASK  0x1F
/* DCDC4: 1.20V is code 70, then 20mV/step => 1.80V is code 100.
 * ALDO3: 0.5V + code * 0.1V => 3.3V is code 28. */
#define DCDC4_1800MV        0x64
#define ALDO3_3300MV        0x1C
#define DCDC4_ENABLE        (1U << 3)
#define ALDO3_ENABLE        (1U << 2)
#define GAUGE_ENABLE        (1U << 3)
#define STATUS_VBUS_GOOD    (1U << 5)
#define STATUS_BAT_PRESENT  (1U << 3)

/* Only handled notification sources are enabled. Otherwise the factory-
 * enabled power-key/thermal IRQs can hold the shared IRQ line low forever.
 * These masks control notifications, not the PMIC's hardware protection,
 * charging algorithm or power-key shutdown behavior. When adding key/fault
 * handling, extend both these masks and the IRQ consumer together.
 * 0: SOC warning levels + Gauge New SOC; 1: VBUS/battery + short/down/up power key;
 * 2: charging start/done. Other pending bits are left untouched. */
static const uint8_t s_irq_masks[3] = {0xD0, 0xFB, 0x18};
#define IRQ_POWER_KEY_SHORT (1U << 3)
static TaskHandle_t s_worker;
static void (*s_key_callback)(void);
static void (*s_key_edge_callback)(bool pressed);
enum {
    POWER_ACTION_IDLE,
    POWER_ACTION_OFF_REQUESTED,
    POWER_ACTION_RESTART_REQUESTED,
    POWER_ACTION_EXECUTING,
    POWER_ACTION_DONE,
};
static atomic_uint s_power_action_state;
static atomic_int s_power_action_result;
/* PMIC 上电/重启留下的 IRQ 不属于本次运行中的新按键。只在首次成功
 * 读取并清除按键状态时建立基线，普通监测 I2C 重试不能重新吞掉用户按键。 */
static bool s_key_events_armed;

void pmic_axp2101_set_key_edge_callback(void (*callback)(bool))
{
    s_key_edge_callback = callback;
}

/* 关机/整机重启共用唯一 PMIC worker 的命令入口。请求种类直接编码在
 * 同一原子字中，不能先发布请求、再填写参数，以免双核任务领取错误动作。 */
static esp_err_t request_power_action(unsigned requested)
{
    if (!s_worker) return ESP_ERR_INVALID_STATE;
    unsigned expected = POWER_ACTION_IDLE;
    if (!atomic_compare_exchange_strong(&s_power_action_state, &expected, requested))
        return ESP_ERR_INVALID_STATE;
    xTaskNotifyGive(s_worker);
    /* 尚未执行的超时请求必须撤销，避免界面恢复后迟到的命令突然断电。
     * 已领取的命令只包含两个各100ms超时的I2C事务，等待其明确结果；
     * 不能宣称撤销已经送达芯片的断电/重启动作。 */
    for (unsigned i = 0;; ++i) {
        unsigned state = atomic_load(&s_power_action_state);
        if (state == POWER_ACTION_DONE) {
            esp_err_t ret = atomic_load(&s_power_action_result);
            atomic_store(&s_power_action_state, POWER_ACTION_IDLE);
            return ret;
        }
        if (i >= 100 && state == requested) {
            expected = requested;
            if (atomic_compare_exchange_strong(&s_power_action_state, &expected, POWER_ACTION_IDLE))
                return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t pmic_axp2101_poweroff(void)
{ return request_power_action(POWER_ACTION_OFF_REQUESTED); }

esp_err_t pmic_axp2101_restart(void)
{ return request_power_action(POWER_ACTION_RESTART_REQUESTED); }

static atomic_bool s_monitor_paused;
static atomic_bool s_monitor_busy;

void pmic_axp2101_set_key_callback(void (*callback)(void))
{
    /* Install before init starts the worker. Callback only queues an event. */
    s_key_callback = callback;
}

bool pmic_axp2101_pause_monitor(bool pause)
{
    bool was_paused = atomic_exchange(&s_monitor_paused, pause);
    if (was_paused && !pause && s_worker) xTaskNotifyGive(s_worker);
    return !atomic_load(&s_monitor_busy);
}

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_device;

static bool s_configured;
/* Rail startup has an independent lifetime from battery sampling. A monitor
 * retry must not cycle chips that have already been powered successfully. */
static bool s_outputs_ready;
static bool s_output_fault_logged;
static bool s_fault_logged;

typedef struct {
    bool valid;
    bool battery_present;
    bool battery_pct_valid;
    bool vbus_present;
    bool charging;
    uint8_t battery_pct;
} power_sample_t;

static esp_err_t read_register(uint8_t reg, uint8_t *value)
{
    return i2c_master_transmit_receive(s_device, &reg, 1, value, 1,
                                       PMIC_I2C_TIMEOUT_MS);
}

static esp_err_t write_register(uint8_t reg, uint8_t value)
{
    const uint8_t bytes[] = {reg, value};
    return i2c_master_transmit(s_device, bytes, sizeof(bytes), PMIC_I2C_TIMEOUT_MS);
}

/* Read/modify/write only a named field, then check its readback. This helper
 * is for configuration registers, never RW1C interrupt status. Already-correct
 * values require no write, so a partial startup can be retried without toggling
 * existing outputs or disturbing adjacent supply enables. */
static esp_err_t update_field_checked(uint8_t reg, uint8_t mask, uint8_t target)
{
    uint8_t before;
    esp_err_t ret = read_register(reg, &before);
    if (ret != ESP_OK) return ret;
    if ((before & mask) == target) return ESP_OK;
    ret = write_register(reg, (before & (uint8_t)~mask) | target);
    if (ret != ESP_OK) return ret;
    uint8_t after;
    ret = read_register(reg, &after);
    if (ret != ESP_OK) return ret;
    if ((after & mask) != target) {
        ESP_LOGW(TAG, "Power reg 0x%02X readback mismatch: expected 0x%02X, got 0x%02X (mask 0x%02X)",
                 (unsigned)reg, (unsigned)target, (unsigned)after, (unsigned)mask);
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

static esp_err_t initialize_power_outputs(void)
{
    if (s_outputs_ready) return ESP_OK;
    /* Configure and verify BOTH voltage fields before enabling either output.
     * If a rail was already on at boot, adjust its setpoint without an off/on
     * cycle. Readback confirms register configuration, not measured pin voltage.
     * ALDO3 is rated at 300mA: this does not establish sufficient current for
     * ESP32-U4WDH radio operation (ESP32 supply recommendation is >=500mA). */
    esp_err_t ret = update_field_checked(REG_DCDC4_VOLTAGE, DCDC4_VOLTAGE_MASK, DCDC4_1800MV);
    if (ret == ESP_OK) ret = update_field_checked(REG_ALDO3_VOLTAGE, ALDO3_VOLTAGE_MASK, ALDO3_3300MV);
    if (ret == ESP_OK) ret = update_field_checked(REG_DCDC_ENABLE, DCDC4_ENABLE, DCDC4_ENABLE);
    if (ret == ESP_OK) ret = update_field_checked(REG_LDO_ENABLE, ALDO3_ENABLE, ALDO3_ENABLE);
    if (ret == ESP_OK) {
        s_outputs_ready = true;
        s_output_fault_logged = false;
        ESP_LOGI(TAG, "Power outputs configured: DCDC4=1800mV ON, ALDO3=3300mV ON (register readback verified)");
    } else {
        if (!s_output_fault_logged)
            ESP_LOGW(TAG, "Power output initialization failed: %s; retrying independently of battery monitor", esp_err_to_name(ret));
        s_output_fault_logged = true;
    }
    return ret;
}

static esp_err_t configure_monitor(void)
{
    /* 原厂手册 pp.36/38：OFFLEVEL=10s；使能硬件长按并选择断电。
     * 保留 ONLEVEL、IRQLEVEL、过温保护；软件独立计时六秒弹窗。 */
    esp_err_t ret = update_field_checked(0x27, 0x0C, 0x0C);
    if (ret == ESP_OK) ret = update_field_checked(0x22, 0x03, 0x02);
    if (ret != ESP_OK) return ret;
    uint8_t modules;
    ret = read_register(REG_MODULE_ENABLE, &modules);
    if (ret != ESP_OK) return ret;
    if (!(modules & GAUGE_ENABLE)) {
        ret = write_register(REG_MODULE_ENABLE, modules | GAUGE_ENABLE);
        if (ret != ESP_OK) return ret;
    }
    for (unsigned bank = 0; bank < 3; ++bank) {
        ret = write_register(REG_IRQ_ENABLE0 + bank, s_irq_masks[bank]);
        if (ret != ESP_OK) return ret;
    }
    return ESP_OK;
}

static esp_err_t acknowledge_irqs(void)
{
    /* RW1C: write back only captured, owned bits; never use read/modify/write
     * on an IRQ status register. An event arriving during clearing is drained
     * again, with a fixed bound to avoid monopolizing CPU on a stuck IRQ pin.
     * Always inspect once, even on a poll: IRQ may already be low at startup. */
    for (unsigned pass = 0; pass < PMIC_IRQ_DRAIN_MAX; ++pass) {
        uint8_t pending[3];
        bool handled = false;
        for (unsigned bank = 0; bank < 3; ++bank) {
            esp_err_t ret = read_register(REG_IRQ_STATUS0 + bank, &pending[bank]);
            if (ret != ESP_OK) return ret;
            pending[bank] &= s_irq_masks[bank];
        }
        for (unsigned bank = 0; bank < 3; ++bank) {
            if (pending[bank]) {
                esp_err_t ret = write_register(REG_IRQ_STATUS0 + bank, pending[bank]);
                if (ret != ESP_OK) return ret;
                handled = true;
            }
            /* 0x49[3] is the PMIC's debounced short-press event. Dispatch only
             * after successful RW1C acknowledgement, so an I2C retry cannot
             * deliver the same press twice. Never perform UI or sleep work
             * in these callbacks. */
            if (bank == 1) {
                /* 首次快照可能同时带着开机用的按下/释放/短按。先W1C
                 * 消费但不转发；若ACK失败保持未就绪，下轮继续清理。
                 * 之后真正的电量/VBUS事件和新按键仍按原路径处理。 */
                if (!s_key_events_armed) {
                    s_key_events_armed = true;
                    continue;
                }
                /* 边沿位不是实时电平。合并时按下再释放，不推测丢失的
                 * 持续时间；硬件十秒断电完全独立于软件计时。 */
                if ((pending[bank] & 0x02) && s_key_edge_callback) s_key_edge_callback(true);
                if ((pending[bank] & 0x01) && s_key_edge_callback) s_key_edge_callback(false);
                if ((pending[bank] & IRQ_POWER_KEY_SHORT) && s_key_callback) s_key_callback();
            }
        }
        if (!handled || gpio_get_level(PMIC_IRQ_GPIO) != 0) break;
    }
    return ESP_OK;
}

static esp_err_t read_power_sample(power_sample_t *sample)
{
    uint8_t status1, status2;
    esp_err_t ret = read_register(REG_STATUS1, &status1);
    if (ret != ESP_OK) return ret;
    ret = read_register(REG_STATUS2, &status2);
    if (ret != ESP_OK) return ret;

    sample->battery_present = (status1 & STATUS_BAT_PRESENT) != 0;
    sample->vbus_present = (status1 & STATUS_VBUS_GOOD) != 0;
    if (sample->battery_present) {
        ret = read_register(REG_BATTERY_PERCENT, &sample->battery_pct);
        if (ret != ESP_OK) return ret;
        /* An out-of-range gauge value is unknown, never clamped into a
         * convincing 100%. VBUS/charging remain usable if SOC is unavailable. */
        sample->battery_pct_valid = sample->battery_pct <= 100;
    }
    const unsigned direction = (status2 >> 5) & 3U;
    const unsigned phase = status2 & 7U;
    /* An inserted supply is not proof of charging: exclude done/not-charging,
     * reserved states and battery absence even if VBUS remains valid. */
    sample->charging = sample->battery_present && sample->vbus_present &&
                       direction == 1 && phase <= 3;
    sample->valid = true;
    return ESP_OK;
}

static void publish_power_sample(const power_sample_t *sample)
{
    /* All I2C transactions have finished before entering LVGL. Publish one
     * coherent snapshot so the UI never mixes an old charge flag with a new
     * percentage. The normal UI timer can read these fields under its lock.
     * Redraw only after a change, avoiding extra invalidation while lyrics
     * or full-screen cover backgrounds are being rendered. */
    if (!lvgl_port_lock(50)) return;
    ui_device_state_t *state = ui_get_device_state();
    const bool pct_valid = sample->valid && sample->battery_pct_valid;
    const uint8_t pct = pct_valid ? sample->battery_pct : 0;
    bool changed = state->power_valid != sample->valid ||
                   state->battery_present != sample->battery_present ||
                   state->battery_pct_valid != pct_valid ||
                   state->vbus_present != sample->vbus_present ||
                   state->charging != sample->charging || state->battery_pct != pct;
    state->power_valid = sample->valid;
    state->battery_present = sample->battery_present;
    state->battery_pct_valid = pct_valid;
    state->vbus_present = sample->vbus_present;
    state->charging = sample->charging;
    state->battery_pct = pct;
    if (changed) {
        ui_statusbar_update();
        ESP_LOGI(TAG, "Battery present=%d SOC=%u%% valid=%d VBUS=%d charging=%d",
                 sample->battery_present, (unsigned)pct, pct_valid,
                 sample->vbus_present, sample->charging);
    }
    lvgl_port_unlock();
}

/* One bounded worker iteration, also exercised by the host regressions.
 * An I2C failure publishes unknown instead of leaving a stale charging icon;
 * the next scheduled attempt re-establishes the monitor without resetting
 * the gauge or changing charger settings. Rail initialization is independent:
 * failure still allows battery/VBUS publication and schedules the same retry. */
static esp_err_t monitor_step(void)
{
    const esp_err_t output_ret = initialize_power_outputs();
    esp_err_t ret = ESP_OK;
    power_sample_t sample = {0};
    if (!s_configured) {
        ret = configure_monitor();
        if (ret == ESP_OK) s_configured = true;
    }
    if (ret == ESP_OK) ret = acknowledge_irqs();
    if (ret == ESP_OK) ret = read_power_sample(&sample);
    if (ret != ESP_OK) {
        s_configured = false;
        sample = (power_sample_t){0};
        if (!s_fault_logged) ESP_LOGW(TAG, "Monitor unavailable: %s; retrying", esp_err_to_name(ret));
        s_fault_logged = true;
    } else if (s_fault_logged) {
        ESP_LOGI(TAG, "Monitor communication recovered");
        s_fault_logged = false;
    }
    publish_power_sample(&sample);
    return ret != ESP_OK ? ret : output_ret;
}

static void IRAM_ATTR pmic_irq_handler(void *arg)
{
    (void)arg;
    /* GPIO ISR does no I2C, allocation, logging or LVGL work. Notifications
     * coalesce IRQ bursts; the task drains status and samples the current state. */
    BaseType_t woken = pdFALSE;
    if (s_worker) vTaskNotifyGiveFromISR(s_worker, &woken);
    if (woken) portYIELD_FROM_ISR();
}

/* 原厂手册 pp.21/32：0x10[0] Soft PWROFF；0x10[1] 整机
 * POWOFF/POWON并复位相关寄存器。两命令互斥；高六位完整保留。
 * 重启不使用esp_restart，不改变十秒长按仍为强制关机的硬件策略。 */
static void service_power_action(void)
{
    unsigned requested = atomic_load(&s_power_action_state);
    if (requested != POWER_ACTION_OFF_REQUESTED && requested != POWER_ACTION_RESTART_REQUESTED) return;
    unsigned expected = requested;
    if (!atomic_compare_exchange_strong(&s_power_action_state, &expected, POWER_ACTION_EXECUTING)) return;
    uint8_t value;
    esp_err_t ret = read_register(0x10, &value);
    const uint8_t command = requested == POWER_ACTION_RESTART_REQUESTED ? 0x02 : 0x01;
    if (ret == ESP_OK) ret = write_register(0x10, (value & (uint8_t)~0x03) | command);
    atomic_store(&s_power_action_result, ret);
    atomic_store(&s_power_action_state, POWER_ACTION_DONE);
}

static void monitor_task(void *arg)
{
    (void)arg;
    for (;;) {
        esp_err_t ret = ESP_OK;
        service_power_action();
        atomic_store(&s_monitor_busy, true);
        if (!atomic_load(&s_monitor_paused)) ret = monitor_step();
        atomic_store(&s_monitor_busy, false);
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ret == ESP_OK ? PMIC_POLL_MS : PMIC_RETRY_MS));
    }
}

esp_err_t pmic_axp2101_init(void)
{
    if (s_bus || s_worker) return ESP_ERR_INVALID_STATE;
    const i2c_master_bus_config_t bus_cfg = {
        .i2c_port = I2C_NUM_1,
        .sda_io_num = PMIC_SDA_GPIO,
        .scl_io_num = PMIC_SCL_GPIO,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        /* Board needs external pull-ups. Internal pull-ups only supplement
         * them; PMIC datasheet recommends SDA/SCL 2.2k and IRQ 4.7k. */
        .flags.enable_internal_pullup = true,
    };
    esp_err_t ret = i2c_new_master_bus(&bus_cfg, &s_bus);
    if (ret != ESP_OK) return ret;
    const i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PMIC_I2C_ADDRESS,
        .scl_speed_hz = 100000,
    };
    bool irq_added = false;
    ret = i2c_master_bus_add_device(s_bus, &dev_cfg, &s_device);
    if (ret != ESP_OK) goto fail;
    const gpio_config_t irq_cfg = {
        .pin_bit_mask = 1ULL << PMIC_IRQ_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_NEGEDGE,
    };
    ret = gpio_config(&irq_cfg);
    if (ret != ESP_OK) goto fail;
    /* TE and SD detection already share the GPIO ISR service. Never uninstall
     * it during PMIC cleanup, otherwise those peripherals lose their handlers. */
    ret = gpio_install_isr_service(0);
    if (ret != ESP_OK && ret != ESP_ERR_INVALID_STATE) goto fail;
    ret = gpio_isr_handler_add(PMIC_IRQ_GPIO, pmic_irq_handler, NULL);
    if (ret != ESP_OK) goto fail;
    irq_added = true;
    if (xTaskCreate(monitor_task, "pmic", 4096, NULL, 2, &s_worker) != pdPASS) {
        ret = ESP_ERR_NO_MEM;
        goto fail;
    }
    ESP_LOGI(TAG, "Monitor started: SDA48/SCL47 IRQ10 address=0x34");
    return ESP_OK;

fail:
    if (irq_added) gpio_isr_handler_remove(PMIC_IRQ_GPIO);
    if (s_device) i2c_master_bus_rm_device(s_device);
    i2c_del_master_bus(s_bus);
    s_device = NULL;
    s_bus = NULL;
    return ret;
}
