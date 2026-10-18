/* Exercise the real screen power state machine with real LVGL activity/input
 * state. Hardware doubles assert that I2C and sleep never hold the UI lock. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "src/indev/lv_indev_private.h"
#include "../../main/screen_power.c"

void vTaskDelay(unsigned ticks) { (void)ticks; }

static bool power_active;
bool system_power_is_active(void) { return power_active; }
static bool ui_locked, locked = true, anim_idle = true, playing;
static bool sd_available = true, sd_locked, pmic_idle = true, pmic_paused;
static int touch_level = 1, pmic_level = 1;
static unsigned stops, resumes, suspend_calls, resume_calls, locks, cancels;
static unsigned sleep_calls, original_irqs, te_disabled, rtc_restores;
static uint64_t held_pads;
static unsigned hold_releases;
static unsigned touch_reads, task_notifications, isr_notifications;
static bool in_touch_isr;
static uint8_t touch_points;
static uint16_t touch_x=100, touch_y=120;
static esp_err_t touch_read_result, touch_get_result;
static uint8_t stale_touch_points;
void xTaskNotifyGive(TaskHandle_t task)
{ assert(!in_touch_isr && task==(void *)2); ++task_notifications; }
void vTaskNotifyGiveFromISR(TaskHandle_t task, BaseType_t *woken)
{ assert(in_touch_isr && task==(void *)2); ++isr_notifications; *woken=pdTRUE; }
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{ (void)ticks; assert(clear==pdTRUE); return 0; }
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t touch)
{
    assert(!ui_locked && !sd_locked && !pmic_paused && !in_touch_isr && touch==s_touch);
    assert(atomic_load(&s_dark) && !s_input->enabled);
    ++touch_reads;
    if(touch_read_result==ESP_OK) stale_touch_points=touch_points;
    return touch_read_result;
}
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t touch, esp_lcd_touch_point_data_t *out,
                                uint8_t *count, uint8_t capacity)
{
    assert(!ui_locked && !in_touch_isr && touch==s_touch && capacity==1);
    *count=stale_touch_points;
    *out=(esp_lcd_touch_point_data_t){.x=touch_x,.y=touch_y};
    stale_touch_points=0;
    return touch_get_result;
}
static void touch_edge(esp_lcd_touch_handle_t touch)
{
    in_touch_isr=true; touch->config.interrupt_callback(touch); in_touch_isr=false;
}
static unsigned brightness_calls;
static TickType_t test_ticks;
static uint8_t brightness_history[512];
TickType_t xTaskGetTickCount(void) { return test_ticks; }
static uint8_t brightness_value;
static esp_err_t suspend_result, resume_result;
static uint64_t wake_status;
static bool inject_sleep_touch, inject_sleep_key;
static ui_device_state_t state = {.brightness = 65};

bool lvgl_port_lock(uint32_t timeout)
{ (void)timeout; assert(!ui_locked); ui_locked = true; return true; }
void lvgl_port_unlock(void) { assert(ui_locked); ui_locked = false; }
esp_err_t lvgl_port_stop(void) { assert(ui_locked); ++stops; return ESP_OK; }
esp_err_t lvgl_port_resume(void) { assert(ui_locked); ++resumes; return ESP_OK; }
esp_err_t lvgl_port_task_wake(int event, void *param)
{ assert(!ui_locked && event == LVGL_PORT_EVENT_TOUCH && param == s_input); return ESP_OK; }
ui_device_state_t *ui_get_device_state(void) { assert(ui_locked); return &state; }
bool ui_lockscreen_is_visible(void) { assert(ui_locked); return locked; }
void ui_lockscreen_stop_anim(void) { assert(ui_locked); ++cancels; }
bool ui_lockscreen_anim_is_idle(void) { return anim_idle; }
void ui_manager_lock(void)
{ assert(ui_locked); locked = true; ++locks; lv_display_trigger_activity(s_display); }
bool music_player_adapter_is_playing(void) { return playing; }
bool pmic_axp2101_pause_monitor(bool pause)
{ assert(!ui_locked); pmic_paused = pause; return pmic_idle; }
bool sd_card_io_lock(TickType_t ticks)
{ assert(!ui_locked && !sd_locked && !ticks); sd_locked = sd_available; return sd_locked; }
void sd_card_io_unlock(void) { assert(sd_locked); sd_locked = false; }
int gpio_get_level(gpio_num_t pin)
{
    /* Frozen RTC mux appears low to the digital GPIO path. Old code would
     * spuriously queue TOUCH and PMIC edges would stay unavailable. */
    if (held_pads & (1ULL << pin)) return 0;
    return pin == 6 ? touch_level : pmic_level;
}
esp_err_t gpio_intr_enable(gpio_num_t pin) { assert(pin == 23); te_disabled = false; return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t pin) { assert(pin == 23); te_disabled = true; return ESP_OK; }
esp_err_t rtc_gpio_deinit(gpio_num_t pin)
{
    assert(pin == 6 || pin == 10);
    assert(!(held_pads & (1ULL << pin))); /* HOLD must be disabled before changing mux. */
    ++rtc_restores; return ESP_OK;
}
esp_err_t rtc_gpio_hold_dis(gpio_num_t pin)
{ assert(pin==6 || pin==10); held_pads &= ~(1ULL<<pin); ++hold_releases; return ESP_OK; }
esp_err_t esp_sleep_enable_ext1_wakeup_io(uint64_t mask, int mode)
{ assert(mask == ((1ULL << 6) | (1ULL << 10)) && mode == ESP_EXT1_WAKEUP_ANY_LOW); return ESP_OK; }
esp_err_t esp_sleep_enable_timer_wakeup(uint64_t us)
{ assert(!ui_locked && sd_locked && us == 1000000ULL); return ESP_OK; }
uint64_t esp_sleep_get_ext1_wakeup_status(void) { return wake_status; }
esp_err_t esp_light_sleep_start(void)
{
    assert(!ui_locked && sd_locked && pmic_paused && atomic_load(&s_dark));
    assert(s_stream_stopped && te_disabled && !playing && anim_idle);
    ++sleep_calls;
    held_pads=(1ULL<<6)|(1ULL<<10);
    wake_status = inject_sleep_touch ? 1ULL << 6 : inject_sleep_key ? 1ULL << 10 : 0;
    return ESP_OK;
}
esp_err_t esp_lcd_panel_co5300_set_brightness(esp_lcd_panel_handle_t panel, uint8_t value)
{
    assert(!ui_locked && panel == (void *)1);
    assert(brightness_calls < sizeof(brightness_history));
    brightness_history[brightness_calls++] = value;
    brightness_value = value;
    return ESP_OK;
}
esp_err_t player_dpi_suspend(esp_lcd_panel_handle_t panel)
{ assert(!ui_locked && panel == (void *)1 && atomic_load(&s_dark)); ++suspend_calls; return suspend_result; }
esp_err_t player_dpi_resume(esp_lcd_panel_handle_t panel)
{ assert(!ui_locked && panel == (void *)1 && atomic_load(&s_dark)); ++resume_calls; return resume_result; }
esp_err_t esp_lcd_touch_register_interrupt_callback(esp_lcd_touch_handle_t touch,
                                                     esp_lcd_touch_interrupt_callback_t callback)
{ touch->config.interrupt_callback = callback; return ESP_OK; }
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                       void *arg, UBaseType_t priority, TaskHandle_t *task)
{ assert(fn == power_task && !strcmp(name, "screen_power") && stack == 4096 && priority == 3 && !arg); *task = (void *)2; return pdPASS; }
static void original_touch_irq(esp_lcd_touch_handle_t touch)
{ assert(touch->config.user_data == (void *)3); ++original_irqs; }
static void input_read(lv_indev_t *input, lv_indev_data_t *data)
{ (void)input; data->state = LV_INDEV_STATE_RELEASED; }
static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{ (void)area; (void)pixels; lv_display_flush_ready(display); }
/* In production the LVGL tick is stopped while fading. Advance only RTOS
 * time here: a successful gradient must not depend on LVGL's paused clock. */
static void finish_fade(void)
{
    unsigned previous_sleep = sleep_calls;
    uint8_t previous_brightness = brightness_value;
    unsigned samples = 0;
    while (s_fading_out) {
        test_ticks += pdMS_TO_TICKS(20);
        screen_power_step();
        if (s_fading_out) {
            assert(brightness_value > 0 && brightness_value <= previous_brightness);
            previous_brightness = brightness_value;
        }
        assert(sleep_calls == previous_sleep && ++samples <= 15);
    }
}
static void expire(void)
{
    lv_display_trigger_activity(s_display);
    lv_tick_inc(10000);
    screen_power_step();
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(460, 460);
    static uint8_t pixels[460 * 20 * 2];
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_indev_t *input = lv_indev_create();
    lv_indev_set_type(input, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(input, input_read);
    struct touch touch = {.config = {original_touch_irq, (void *)3}};
    assert(screen_power_init(display, input, (void *)1, &touch) == ESP_OK);
    lvgl_port_lock(0); screen_power_set_brightness(65); lvgl_port_unlock();
    assert(!brightness_calls); screen_power_step(); assert(brightness_value == 65);
    touch_edge(&touch);
    assert(original_irqs == 1 && !atomic_load(&s_events));
    lv_tick_inc(9999); screen_power_step();
    assert(!atomic_load(&s_dark));
    lv_tick_inc(1); screen_power_step();
    assert(atomic_load(&s_dark) && s_fading_out && !s_stream_stopped);
    assert(brightness_value == 65 && stops == 1 && !suspend_calls && cancels == 1 && !te_disabled);
    uint32_t frozen_lv_tick = lv_tick_get();
    unsigned fade_start = brightness_calls;
    finish_fade();
    assert(lv_tick_get() == frozen_lv_tick); /* fade uses RTOS clock */
    assert(s_stream_stopped && brightness_value == 0 && suspend_calls == 1 && te_disabled);
    assert(brightness_calls - fade_start >= 10);
    assert(brightness_history[fade_start] < 65 && brightness_history[fade_start] > 0);
    for (unsigned i = fade_start + 1; i < brightness_calls; ++i)
        assert(brightness_history[i] <= brightness_history[i - 1]);
    puts("PASS ten-second timeout freezes UI, fades actual brightness over 300ms, then stops DPI before sleep");

    screen_power_set_brightness(72);
    assert(brightness_value == 0); /* preserve desired brightness without lighting screen */
    touch_points=0;
    playing = true; screen_power_step(); assert(!sleep_calls);
    playing = false; anim_idle = false; screen_power_step(); assert(!sleep_calls);
    anim_idle = true; sd_available = false; screen_power_step(); assert(!sleep_calls && !pmic_paused);
    sd_available = true; pmic_idle = false; screen_power_step(); assert(!sleep_calls && !pmic_paused);
    pmic_idle = true; pmic_level = 0; screen_power_step(); assert(!sleep_calls);
    pmic_level = 1; screen_power_step(); assert(sleep_calls == 1 && !sd_locked && !pmic_paused);
    assert(atomic_load(&s_dark)); /* timer/monitoring wake must not turn on display */
    puts("PASS playback, JPEG, SD and PMIC sleep guards unchanged; timer keeps screen dark");

    inject_sleep_touch = true; touch_points=1; screen_power_step();
    assert(atomic_load(&s_events) & EVENT_TOUCH);
    screen_power_step();
    assert(!atomic_load(&s_dark) && !s_stream_stopped && locks == 1);
    assert(resumes == 1 && resume_calls == 1 && brightness_value == 72 && !te_disabled);
    assert(rtc_restores == 4 && hold_releases==4 && !held_pads);
    puts("PASS touch RTC wake resumes existing UI, consumes wake touch and restores saved brightness");

    locked = false; expire(); assert(!atomic_load(&s_dark));
    screen_power_key_short_press(); screen_power_step();
    assert(locked && locks == 2 && !atomic_load(&s_dark));
    /* A second key press while locked must start fade immediately, without
     * calling lock/show or waiting another ten seconds. */
    screen_power_key_short_press(); screen_power_step();
    assert(atomic_load(&s_dark) && s_fading_out && locks == 2 && !s_stream_stopped);
    finish_fade();
    touch_edge(&touch);
    assert(original_irqs == 1); screen_power_step();
    assert(!atomic_load(&s_dark) && locks == 3 && brightness_value == 72);
    puts("PASS key locks unlocked UI; next key on visible lock fades off without re-entering lock; touch wakes");

    screen_power_key_short_press(); screen_power_step(); finish_fade();
    inject_sleep_touch = false; inject_sleep_key = true;
    screen_power_step(); screen_power_step();
    assert(atomic_load(&s_dark)); /* A PMIC IRQ alone does not imply short key. */
    screen_power_key_short_press(); screen_power_step();
    assert(!atomic_load(&s_dark) && locks == 4);
    puts("PASS VBUS/SOC IRQ does not light screen; decoded short key wakes after fade");

    /* Cancel halfway: no DMA suspend/resume and no sleep are allowed while
     * brightness is still falling. Both touch and a new short key cancel. */
    unsigned dma_before = suspend_calls, dma_resume_before = resume_calls;
    unsigned sleeps_before = sleep_calls;
    screen_power_key_short_press(); screen_power_step();
    test_ticks += pdMS_TO_TICKS(100); screen_power_step();
    assert(s_fading_out && brightness_value > 0 && brightness_value < 72);
    touch_points=1; touch_edge(&touch); screen_power_step();
    assert(!s_fading_out && !atomic_load(&s_dark) && brightness_value == 72 && locks == 5);
    assert(suspend_calls == dma_before && resume_calls == dma_resume_before && sleep_calls == sleeps_before);
    screen_power_key_short_press(); screen_power_step();
    test_ticks += pdMS_TO_TICKS(100); screen_power_step();
    screen_power_key_short_press(); screen_power_step();
    assert(!s_fading_out && !atomic_load(&s_dark) && brightness_value == 72 && locks == 6);
    assert(suspend_calls == dma_before && resume_calls == dma_resume_before && sleep_calls == sleeps_before);
    puts("PASS touch or key cancels fade immediately, restores brightness and never enters sleep mid-gradient");

    suspend_result = ESP_ERR_TIMEOUT; expire(); finish_fade();
    assert(!atomic_load(&s_dark) && !s_stream_stopped && brightness_value == 72 && locks == 7);
    suspend_result = ESP_OK; expire(); finish_fade(); assert(atomic_load(&s_dark));
    resume_result = ESP_FAIL; screen_power_key_short_press(); screen_power_step();
    assert(atomic_load(&s_dark) && atomic_load(&s_events));
    resume_result = ESP_OK; screen_power_step(); assert(!atomic_load(&s_dark) && locks == 8);
    puts("PASS DMA suspend/resume failure handling remains recoverable after a gradual fade");

    /* A low user brightness and a wrapping RTOS clock must still reach zero
     * monotonically and restore that user's value, rather than a default. */
    screen_power_set_brightness(5); screen_power_step(); assert(brightness_value == 5);
    test_ticks = UINT32_MAX - 10U;
    screen_power_key_short_press(); screen_power_step(); finish_fade();
    assert(atomic_load(&s_dark) && brightness_value == 0);
    screen_power_key_short_press(); screen_power_step();
    assert(!atomic_load(&s_dark) && brightness_value == 5);
    puts("PASS 5-percent brightness and RTOS tick wrap preserve gradient and saved wake brightness");
    power_active=true;
    lv_tick_inc(11000); screen_power_step(); assert(!screen_power_is_dark());
    unsigned no_sleep=sleep_calls;
    screen_power_shutdown_display(true); screen_power_step();
    assert(screen_power_is_dark() && s_fading_out);
    finish_fade(); assert(screen_power_display_suspended() && brightness_value==0);
    screen_power_request_wake(); screen_power_step();
    assert(screen_power_display_suspended() && screen_power_is_dark() && sleep_calls==no_sleep);
    screen_power_shutdown_display(false); screen_power_step();
    assert(!screen_power_is_dark() && !screen_power_display_suspended() && brightness_value==5);
    power_active=false;
    puts("PASS power dialog prevents idle screen-off; shutdown fades/stops DPI, ignores touch, never sleeps and recovers on failure");
    /* Repeat both ways of turning off, with empty IRQ pulses and RTC hints.
     * The old code unconditionally restored DPI on any one of these inputs. */
    inject_sleep_touch=false; inject_sleep_key=false; touch_points=0;
    unsigned wake_before=resume_calls;
    for (unsigned pass=0; pass<2; ++pass) {
        touch_level=pmic_level=1;
        if (pass==0) { screen_power_key_short_press(); screen_power_step(); }
        else expire();
        assert(s_fading_out && screen_power_is_dark());
        stale_touch_points=1; /* Must be replaced by a fresh hardware read. */
        touch_edge(&touch); finish_fade();
        assert(screen_power_display_suspended() && resume_calls==wake_before && brightness_value==0);
        touch_level=0;
        unsigned probes=touch_reads;
        for (unsigned n=0;n<5;++n) {
            test_ticks+=20; screen_power_step();
            assert(screen_power_is_dark() && brightness_value==0 && resume_calls==wake_before);
        }
        assert(touch_reads==probes+5);
        touch_level=1;
        inject_sleep_touch=true; screen_power_step(); screen_power_step();
        assert(screen_power_is_dark() && resume_calls==wake_before);
        inject_sleep_touch=false;
        screen_power_request_wake(); screen_power_step();
        assert(!screen_power_is_dark() && resume_calls==++wake_before);
    }
    puts("PASS key/idle screen-off survives zero-contact IRQ pulses, held-low GPIO, stale cache and empty RTC hints");

    screen_power_key_short_press(); screen_power_step(); finish_fade();
    unsigned probes=touch_reads;
    touch_level=0; touch_read_result=ESP_ERR_TIMEOUT; stale_touch_points=1;
    test_ticks+=20; screen_power_step();
    assert(screen_power_is_dark() && touch_reads==probes+1 && stale_touch_points==1);
    touch_read_result=ESP_OK; touch_points=1; touch_x=999;
    test_ticks+=20; screen_power_step(); assert(screen_power_is_dark());
    touch_x=100; touch_get_result=ESP_FAIL;
    test_ticks+=20; screen_power_step(); assert(screen_power_is_dark());
    touch_get_result=ESP_OK; touch_points=0;
    test_ticks+=20; screen_power_step(); assert(screen_power_is_dark());
    /* A real pulse immediately after an empty hint is deferred, never dropped;
     * it is still validated once the 20ms probe bound expires even with pin high. */
    touch_level=1; touch_points=1; touch_edge(&touch); screen_power_step();
    assert(screen_power_is_dark() && s_touch_probe_pending);
    test_ticks+=20; screen_power_step();
    assert(!screen_power_is_dark() && !s_touch_probe_pending);
    assert(isr_notifications && task_notifications);
    puts("PASS I2C/get-data failures and invalid coordinates stay dark; subsequent real pulse survives probe throttling");

    touch_points=0;
    screen_power_key_short_press(); screen_power_step(); finish_fade();
    touch_read_result=ESP_ERR_TIMEOUT; touch_level=0;
    screen_power_key_short_press(); screen_power_step(); assert(!screen_power_is_dark());
    screen_power_key_short_press(); screen_power_step(); finish_fade();
    screen_power_request_wake(); screen_power_step(); assert(!screen_power_is_dark());
    touch_read_result=ESP_OK; touch_level=1;
    puts("PASS power key and explicit wake remain independent of an absent/faulted touch controller");
    lv_deinit();
    puts("ALL SCREEN POWER TESTS PASSED");
    return 0;
}
