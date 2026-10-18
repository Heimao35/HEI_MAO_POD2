/* Exercise production PMIC scheduling/register accesses and the actual LVGL
 * status bar. The register model applies W1C and can inject events during ACK;
 * no PMIC, SD card, external network or hardware display is accessed. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/pmic_axp2101.c"
#include "../../main/ui/ui_common.c"
#include "../../main/ui/ui_statusbar.c"

static uint8_t registers[256];
static bool ui_locked, in_isr, unavailable, task_failure, handler_installed;
static bool inject_remove_on_ack, repeated_irq, force_irq_low;
static int fail_read_reg = -1;
static int fail_write_reg = -1, ignored_write_reg = -1;
static unsigned rail_writes;
static uint8_t rail_write_order[32];
static unsigned transactions, writes, notifications, buses, devices;
static unsigned module_writes, irq_writes, status_reads;
static void (*irq_handler)(void *);

const lv_font_t *ui_font_14(void) { return &lv_font_montserrat_14; }
const lv_font_t *ui_font_16(void) { return &lv_font_montserrat_16; }

bool lvgl_port_lock(uint32_t timeout)
{ (void)timeout; assert(!ui_locked && !in_isr); ui_locked = true; return true; }
void lvgl_port_unlock(void) { assert(ui_locked); ui_locked = false; }

esp_err_t i2c_new_master_bus(const i2c_master_bus_config_t *cfg, i2c_master_bus_handle_t *bus)
{
    assert(cfg->i2c_port == 1 && cfg->sda_io_num == 48 && cfg->scl_io_num == 47);
    assert(!buses); ++buses; *bus = (void *)1; return ESP_OK;
}
esp_err_t i2c_del_master_bus(i2c_master_bus_handle_t bus)
{ assert(bus == (void *)1 && buses && !devices); --buses; return ESP_OK; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus, const i2c_device_config_t *cfg,
                                    i2c_master_dev_handle_t *device)
{
    assert(bus == (void *)1 && cfg->device_address == 0x34 && cfg->scl_speed_hz == 100000);
    ++devices; *device = (void *)2; return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t device)
{ assert(device == (void *)2 && devices); --devices; return ESP_OK; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t device, const uint8_t *write,
                                     size_t write_size, uint8_t *read, size_t read_size, int timeout_ms)
{
    assert(!ui_locked && !in_isr && device == (void *)2);
    assert(write_size == 1 && read_size == 1 && timeout_ms == 100);
    ++transactions;
    if (unavailable || *write == fail_read_reg) return ESP_ERR_TIMEOUT;
    if (*write >= 0x48 && *write <= 0x4A) ++status_reads;
    *read = registers[*write]; return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t device, const uint8_t *write,
                             size_t size, int timeout_ms)
{
    assert(!ui_locked && !in_isr && device == (void *)2 && size == 2 && timeout_ms == 100);
    ++transactions; ++writes;
    if (unavailable) return ESP_ERR_TIMEOUT;
    uint8_t reg = write[0], value = write[1];
    if (reg == fail_write_reg) return ESP_ERR_TIMEOUT;
    if (reg == 0x85 || reg == 0x94 || reg == 0x80 || reg == 0x90) {
        assert(rail_writes < sizeof(rail_write_order));
        rail_write_order[rail_writes++] = reg;
        uint8_t mask = reg == 0x85 ? 0x7F : reg == 0x94 ? 0x1F : reg == 0x80 ? 0x08 : 0x04;
        uint8_t target = reg == 0x85 ? 0x64 : reg == 0x94 ? 0x1C : mask;
        assert((value & mask) == target);
        assert((value & (uint8_t)~mask) == (registers[reg] & (uint8_t)~mask));
        if (reg == 0x80 || reg == 0x90) {
            /* Neither newly enabled rail may precede verification of the two
             * voltage fields. Other channel enables must keep their values. */
            assert((registers[0x85] & 0x7F) == 0x64 && (registers[0x94] & 0x1F) == 0x1C);
        }
        if (reg != ignored_write_reg) registers[reg] = value;
    } else if (reg == 0x18) {
        ++module_writes;
        assert(value == (registers[reg] | (1U << 3)));
        registers[reg] = value;
    } else if (reg >= 0x40 && reg <= 0x42) {
        registers[reg] = value;
    } else {
        /* Only the two requested rails, gauge and IRQ fields may be written.
         * Charger settings, other rails, model/reset and key policy are forbidden. */
        assert(reg >= 0x48 && reg <= 0x4A);
        assert(value && !(value & ~s_irq_masks[reg - 0x48]));
        ++irq_writes;
        registers[reg] &= (uint8_t)~value;
        if (reg == 0x49 && inject_remove_on_ack) {
            inject_remove_on_ack = false;
            registers[0x49] |= 1U << 6;
            registers[0x00] &= (uint8_t)~(1U << 5);
            registers[0x01] = (2U << 5) | 5;
        }
        if (repeated_irq && reg == 0x49) registers[reg] |= 1U << 7;
    }
    return ESP_OK;
}
esp_err_t gpio_config(const gpio_config_t *cfg)
{
    assert(cfg->pin_bit_mask == (1ULL << 10) && cfg->mode == GPIO_MODE_INPUT);
    assert(cfg->intr_type == GPIO_INTR_NEGEDGE && cfg->pull_up_en);
    return ESP_OK;
}
esp_err_t gpio_install_isr_service(int flags)
{ assert(flags == 0); return ESP_ERR_INVALID_STATE; /* TE/SD already installed. */ }
esp_err_t gpio_isr_handler_add(gpio_num_t gpio, void (*handler)(void *), void *arg)
{ assert(gpio == 10 && !arg && !handler_installed); handler_installed = true; irq_handler = handler; return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t gpio)
{ assert(gpio == 10 && handler_installed); handler_installed = false; return ESP_OK; }
int gpio_get_level(gpio_num_t gpio)
{
    assert(gpio == 10);
    if (force_irq_low) return 0;
    for (unsigned i = 0; i < 3; ++i)
        if (registers[0x48+i] & registers[0x40+i]) return 0;
    return 1;
}
void xTaskNotifyGive(TaskHandle_t task) { assert(!in_isr && task == (void *)3); }
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken)
{ assert(in_isr && task == (void *)3); ++notifications; *woken = pdTRUE; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{ (void)clear; (void)ticks; return 0; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *task)
{
    assert(fn == monitor_task && !strcmp(name, "pmic") && stack == 4096 && !arg && priority == 2);
    if (task_failure) return pdFALSE;
    *task = (void *)3; return pdPASS;
}
static void trigger_irq(void)
{ in_isr = true; irq_handler(NULL); in_isr = false; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
static void check_right_bounds(void)
{
    lv_obj_update_layout(s_bar);
    lv_area_t bar, parent, pct, icon, charge;
    lv_obj_get_coords(s_bar, &bar);
    lv_obj_get_coords(lv_obj_get_parent(s_batt_icon), &parent);
    lv_obj_get_coords(s_batt_pct, &pct);
    lv_obj_get_coords(s_batt_icon, &icon);
    lv_obj_get_coords(s_charge, &charge);
    assert(pct.x1 >= parent.x1 && pct.x2 < icon.x1);
    assert(icon.x2 <= parent.x2 && icon.y1 >= bar.y1 && icon.y2 <= bar.y2);
    if (!lv_obj_has_flag(s_charge, LV_OBJ_FLAG_HIDDEN))
        assert(icon.x2 < charge.x1 && charge.x2 <= parent.x2 && charge.y2 <= bar.y2);
}
static unsigned key_presses;
static void key_event(void)
{
    assert(!in_isr && !ui_locked && !(registers[0x49] & (1U << 3)));
    ++key_presses;
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(460, 460);
    static uint8_t pixels[460*20*2];
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    ui_statusbar_create(lv_screen_active());
    ui_device_state_t *state = ui_get_device_state();
    assert(!state->power_valid && !state->battery_pct_valid);
    assert(!strcmp(lv_label_get_text(s_batt_pct), "--%"));
    assert(lv_obj_has_flag(s_charge, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_style_text_font(s_batt_icon, 0) == &lv_font_montserrat_26);

    task_failure = true;
    assert(pmic_axp2101_init() == ESP_ERR_NO_MEM);
    assert(!buses && !devices && !handler_installed && !s_bus && !s_device && !s_worker);
    task_failure = false;
    assert(pmic_axp2101_init() == ESP_OK);
    assert(pmic_axp2101_init() == ESP_ERR_INVALID_STATE);
    assert(buses == 1 && devices == 1 && handler_installed);
    puts("PASS separate I2C1 pins/address, shared GPIO ISR service and failed-init resource cleanup");

    registers[0x85] = 0xA2;
    registers[0x94] = 0xA5;
    registers[0x80] = 0x65; /* DCDC4 off; DCDC1/3 and mode flags untouched. */
    registers[0x90] = 0xA9; /* ALDO3 off; unrelated LDOs remain enabled. */
    registers[0x18] = 0x03; /* gauge disabled; charger and watchdog bits must survive. */
    registers[0x00] = (1U << 5) | (1U << 3);
    registers[0x01] = (1U << 5) | 2;
    registers[0xA4] = 68;
    registers[0x49] = (1U << 7) | (1U << 3); /* startup-low VBUS + owned short-key event. */
    assert(monitor_step() == ESP_OK);
    assert(state->power_valid && state->battery_present && state->battery_pct_valid);
    assert(state->vbus_present && state->charging && state->battery_pct == 68);
    assert(registers[0x18] == 0x0B && module_writes == 1);
    assert(s_outputs_ready && rail_writes == 4);
    const uint8_t expected_order[] = {0x85, 0x94, 0x80, 0x90};
    assert(!memcmp(rail_write_order, expected_order, sizeof(expected_order)));
    assert(registers[0x85] == 0xE4 && registers[0x94] == 0xBC);
    assert(registers[0x80] == 0x6D && registers[0x90] == 0xAD);
    assert(registers[0x49] == 0);
    assert(!strcmp(lv_label_get_text(s_batt_pct), "68%"));
    assert(!lv_obj_has_flag(s_charge, LV_OBJ_FLAG_HIDDEN));
    assert(lv_color_eq(lv_obj_get_style_text_color(s_charge, 0), lv_color_hex(0xFFD700)));
    check_right_bounds();
    puts("PASS voltage-before-enable startup, field readback and preservation of unrelated rail enables/mode bits");
    puts("PASS startup snapshot, gauge-enable preserves other modules, owned RW1C flags, real SOC and golden charging icon");

    registers[0xA4] = 100;
    registers[0x01] = 4; /* standby, charging complete */
    registers[0x4A] = 1U << 4;
    unsigned before = transactions;
    trigger_irq();
    assert(notifications == 1 && transactions == before);
    assert(monitor_step() == ESP_OK);
    assert(state->vbus_present && !state->charging && state->battery_pct == 100);
    assert(!strcmp(lv_label_get_text(s_batt_pct), "100%"));
    assert(lv_obj_has_flag(s_charge, LV_OBJ_FLAG_HIDDEN));
    check_right_bounds();
    /* Also validate the widest combination: 100% while still charging. */
    registers[0x01] = (1U << 5) | 3;
    assert(monitor_step() == ESP_OK);
    check_right_bounds();
    puts("PASS charge-done keeps VBUS present, ISR performs no I2C, enlarged icon and 100%-plus-lightning fit original bounds");

    registers[0x49] |= 1U << 7;
    inject_remove_on_ack = true;
    assert(monitor_step() == ESP_OK);
    assert(!state->vbus_present && !state->charging && !(registers[0x49] & 0xC0));
    assert(lv_obj_has_flag(s_charge, LV_OBJ_FLAG_HIDDEN));
    puts("PASS VBUS removal arriving during IRQ ACK is drained and sampled from current state");

    registers[0x00] |= 1U << 5;
    registers[0x01] = (1U << 5) | 2;
    registers[0xA4] = 255;
    assert(monitor_step() == ESP_OK);
    assert(state->power_valid && state->vbus_present && state->charging && !state->battery_pct_valid);
    assert(!strcmp(lv_label_get_text(s_batt_pct), "--%"));
    registers[0x00] = 1U << 5; /* external supply, no battery: never show stale SOC/charge. */
    assert(monitor_step() == ESP_OK);
    assert(state->vbus_present && !state->battery_present && !state->charging && !state->battery_pct_valid);
    assert(lv_obj_has_flag(s_charge, LV_OBJ_FLAG_HIDDEN));
    puts("PASS invalid SOC remains unknown; battery removal preserves VBUS and clears charging/percentage");

    registers[0x00] |= 1U << 3;
    registers[0xA4] = 42;
    assert(monitor_step() == ESP_OK && state->charging);
    fail_read_reg = 0x01; /* failure after status1 read must not publish a partial snapshot. */
    assert(monitor_step() == ESP_ERR_TIMEOUT);
    assert(!state->power_valid && !state->vbus_present && !state->charging && !state->battery_pct_valid);
    assert(!strcmp(lv_label_get_text(s_batt_pct), "--%"));
    fail_read_reg = -1;
    assert(monitor_step() == ESP_OK && state->battery_pct == 42 && state->charging);
    assert(module_writes == 1); /* already-enabled gauge never reset/re-enabled unnecessarily. */
    unavailable = true;
    assert(monitor_step() == ESP_ERR_TIMEOUT && !state->power_valid);
    unavailable = false;
    assert(monitor_step() == ESP_OK && state->power_valid);
    assert(rail_writes == 4); /* Battery monitor retries never rewrite or cycle good rails. */
    puts("PASS partial/disconnected I2C reads invalidate stale UI; retry preserves gauge and already-started rails");

    registers[0x49] |= 1U << 7;
    repeated_irq = true;
    unsigned acks_before = irq_writes;
    assert(monitor_step() == ESP_OK);
    assert(irq_writes - acks_before == PMIC_IRQ_DRAIN_MAX);
    repeated_irq = false;
    assert(monitor_step() == ESP_OK);
    force_irq_low = true;
    unsigned reads_before = status_reads;
    assert(monitor_step() == ESP_OK && status_reads - reads_before == 3);
    force_irq_low = false;
    puts("PASS repeating/stuck IRQ remains bounded; register write ownership remains restricted");

    /* A failed readback must stop before either new enable, while preserving
     * working battery/VBUS monitoring. The retry must reuse the first already-
     * written voltage, rather than power-cycling or rewriting good fields. */
    s_outputs_ready = false;
    registers[0x85] = 0xA2; registers[0x94] = 0xA5;
    registers[0x80] = 0x65; registers[0x90] = 0xA9;
    unsigned rails_before = rail_writes;
    ignored_write_reg = 0x94;
    assert(monitor_step() == ESP_ERR_INVALID_RESPONSE);
    assert(!s_outputs_ready && state->power_valid && state->battery_pct == 42);
    assert(rail_writes == rails_before + 2);
    assert(registers[0x80] == 0x65 && registers[0x90] == 0xA9);
    ignored_write_reg = -1;
    assert(monitor_step() == ESP_OK && s_outputs_ready);
    assert(rail_writes == rails_before + 5); /* first voltage is not rewritten. */
    assert(registers[0x80] == 0x6D && registers[0x90] == 0xAD);
    puts("PASS mismatched voltage readback defers both enables; retry finishes without losing battery status");

    /* Boot/partial retry with already-correct fields must issue no rail writes,
     * including when both outputs are already on from a previous startup. */
    s_outputs_ready = false;
    rails_before = rail_writes;
    assert(monitor_step() == ESP_OK && s_outputs_ready && rail_writes == rails_before);
    s_outputs_ready = false;
    registers[0x80] = 0x65; registers[0x90] = 0xA9;
    fail_write_reg = 0x80;
    assert(monitor_step() == ESP_ERR_TIMEOUT && !s_outputs_ready);
    assert(state->power_valid && registers[0x90] == 0xA9);
    fail_write_reg = -1;
    assert(monitor_step() == ESP_OK && s_outputs_ready);
    assert(rail_writes == rails_before + 2 && registers[0x80] == 0x6D && registers[0x90] == 0xAD);
    s_outputs_ready = false;
    fail_read_reg = 0x85;
    rails_before = rail_writes;
    assert(monitor_step() == ESP_ERR_TIMEOUT && state->power_valid && !s_outputs_ready);
    fail_read_reg = -1;
    assert(monitor_step() == ESP_OK && s_outputs_ready && rail_writes == rails_before);
    puts("PASS already-on rails remain on; failed power I2C operations recover independently of battery monitoring");

    pmic_axp2101_set_key_callback(key_event);
    registers[0x49] |= 1U << 3;
    assert(monitor_step() == ESP_OK && key_presses == 1);
    assert(registers[0x41] == 0xF8);
    assert(monitor_step() == ESP_OK && key_presses == 1);
    registers[0x49] |= 1U << 3;
    fail_write_reg = 0x49;
    assert(monitor_step() == ESP_ERR_TIMEOUT && key_presses == 1);
    fail_write_reg = -1;
    assert(monitor_step() == ESP_OK && key_presses == 2);
    registers[0x49] |= 1U << 2; /* Long key remains unowned. */
    assert(monitor_step() == ESP_OK && key_presses == 2);
    assert(registers[0x49] & (1U << 2));
    assert(pmic_axp2101_pause_monitor(true));
    atomic_store(&s_monitor_busy, true);
    assert(!pmic_axp2101_pause_monitor(true));
    atomic_store(&s_monitor_busy, false);
    assert(pmic_axp2101_pause_monitor(false));
    puts("PASS short-key delivered once after ACK, retry safe, long-key policy unchanged, monitor sleep handshake");

    lv_deinit();
    puts("ALL POWER MONITOR TESTS PASSED");
    return 0;
}
