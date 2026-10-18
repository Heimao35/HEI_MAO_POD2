#include "screen_power.h"
#include "display_dpi_sleep.h"
#include "pmic_axp2101.h"
#include "sd_card.h"
#include "music_player_adapter.h"
#include "ui/ui_manager.h"
#include "ui/ui_lockscreen.h"
#include "ui/ui_common.h"
#include "esp_lcd_co5300.h"
#include "esp_lvgl_port.h"
#include "esp_sleep.h"
#include "driver/gpio.h"
#include "driver/rtc_io.h"
#include "esp_attr.h"
#include "esp_log.h"
#include "freertos/task.h"
#include <stdatomic.h>

#define TOUCH_IRQ_GPIO GPIO_NUM_6
#define PMIC_IRQ_GPIO GPIO_NUM_10
#define LCD_TE_GPIO GPIO_NUM_23 /* main.c: EXAMPLE_PIN_NUM_LCD_TE, diagnostic pulse only */
#define LOCK_IDLE_MS 10000U
/* 渐灭由电源任务使用 FreeRTOS 时间推进；LVGL 停止刷新后仍然有效。
 * 约 300ms、每 20ms 一步，仅发送小型 DCS 亮度命令，不绘制全屏遮罩。 */
#define SCREEN_FADE_MS 300U
#define SCREEN_FADE_STEP_MS 20U
#define EVENT_TOUCH  (1U << 0)
#define EVENT_KEY    (1U << 1)

static const char *TAG = "screen_power";
static lv_display_t *s_display;
static lv_indev_t *s_input;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_interrupt_callback_t s_original_touch_irq;
static TaskHandle_t s_task;
static atomic_uint s_events;
static atomic_bool s_dark;
static atomic_uchar s_brightness = 80;
static atomic_bool s_brightness_pending;
static bool s_stream_stopped;
static bool s_sleep_fault_logged;
static bool s_fading_out;
static TickType_t s_fade_started;
static uint8_t s_fade_from;
/* s_brightness 保存用户偏好，s_panel_brightness 保存实际已写入亮度。
 * 渐灭只改变后者，唤醒仍恢复到用户偏好；都只由电源任务写硬件。 */
static uint8_t s_panel_brightness = 80;

bool screen_power_is_dark(void)
{
    return atomic_load(&s_dark);
}

void screen_power_key_short_press(void)
{
    /* No UI/I2C in PMIC IRQ consumer. The power task serializes transitions. */
    atomic_fetch_or(&s_events, EVENT_KEY);
}

static void IRAM_ATTR touch_irq(esp_lcd_touch_handle_t touch)
{
    /* Preserve esp_lvgl_port's callback/user_data. Its ordinary event-mode
     * input path remains unchanged while awake; dark touches only request wake.
     * The GPIO ISR never reads the touch I2C bus or acquires the LVGL lock. */
    if (atomic_load(&s_dark)) atomic_fetch_or(&s_events, EVENT_TOUCH);
    else if (s_original_touch_irq) s_original_touch_irq(touch);
}

void screen_power_set_brightness(uint8_t brightness)
{
    if (brightness < 5) brightness = 5;
    atomic_store(&s_brightness, brightness);
    /* Slider callbacks execute under LVGL lock: queue only; serialize DCS
     * commands with screen-off/wake on the power worker, outside the lock. */
    atomic_store(&s_brightness_pending, true);
}

static esp_err_t write_panel_brightness(uint8_t brightness)
{
    esp_err_t ret = esp_lcd_panel_co5300_set_brightness(s_panel, brightness);
    if (ret == ESP_OK) s_panel_brightness = brightness;
    return ret;
}

/* 调用方持有 LVGL 锁。超时与锁屏下短按共用这条路径：先停止画面和
 * 输入活动，再让显示链路继续输出固定画面，直到亮度渐灭完毕才停 DMA。
 * s_dark 在渐灭开始时即置位，使此期间触摸也能取消熄屏，不会丢失操作。 */
static void begin_screen_off_locked(void)
{
    atomic_store(&s_dark, true);
    lv_indev_enable(s_input, false);
    ui_lockscreen_stop_anim();
    lvgl_port_stop();
    s_fade_from = s_panel_brightness;
    s_fade_started = xTaskGetTickCount();
    s_fading_out = true;
}

static void check_lock_timeout(void)
{
    if (lvgl_port_lock(50)) {
        /* 只在锁屏计时。LVGL 的显示器活动时间涵盖全屏输入，而不只是在
         * 主屏冒泡成功的手势；持续按住/滑动也不会被当成十秒无操作。 */
        if (ui_lockscreen_is_visible() &&
            lv_display_get_inactive_time(s_display) >= LOCK_IDLE_MS &&
            lv_indev_get_state(s_input) != LV_INDEV_STATE_PRESSED)
            begin_screen_off_locked();
        lvgl_port_unlock();
    }
}

static void restore_ui(void)
{
    if (lvgl_port_lock(0)) {
        /* 触摸唤醒只负责亮屏；等待本次手指释放后再接受手势，防止同一
         * 次上滑既唤醒又解锁。锁屏对象一直保留，show 仅重播 MJPEG，
         * 不重播页面滑入动画；也不重新初始化 UI、字体或媒体库。 */
        lv_indev_reset(s_input, NULL);
        lv_indev_wait_release(s_input);
        ui_manager_lock();
        lv_obj_invalidate(lv_screen_active());
        lvgl_port_resume();
        /* Flush the new lock page while still dark; the previous page must
         * not flash briefly when brightness is restored. */
        lv_refr_now(s_display);
        atomic_store(&s_dark, false);
        lv_indev_enable(s_input, true);
        lvgl_port_unlock();
    }
    /* Commands run outside LVGL lock, and only after DPI has resumed. */
    write_panel_brightness(atomic_load(&s_brightness));
    /* Force one input read even if the wake finger was already released.
     * Otherwise event-mode input might swallow the user's next fresh tap. */
    lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, s_input);
}

static void fade_screen_off_step(void)
{
    TickType_t duration = pdMS_TO_TICKS(SCREEN_FADE_MS);
    TickType_t elapsed = xTaskGetTickCount() - s_fade_started;
    uint32_t remaining = elapsed < duration ? duration - elapsed : 0;
    /* 整数二次渐慢曲线：先明显变暗，再柔和地降至零。向上取整确保
     * 渐变结束前至少为 1%，结束才写 0；无需 LVGL 动画或额外帧缓冲。
     * 使用 64 位中间值，避免较高 tick 频率时平方乘法溢出。 */
    uint64_t denominator = (uint64_t)duration * duration;
    uint8_t brightness = (uint8_t)(((uint64_t)s_fade_from * remaining * remaining +
                                   denominator - 1) / denominator);
    esp_err_t ret = ESP_OK;
    if (brightness != s_panel_brightness) ret = write_panel_brightness(brightness);
    if (ret == ESP_OK && remaining) return;

    s_fading_out = false;
    /* 亮度到零之前绝不停止 DPI 或进入浅睡眠，否则渐变会被截断。
     * 一次状态机只推进一步，渐灭中触摸或短按可在下一步立即唤醒。 */
    if (ret == ESP_OK) ret = player_dpi_suspend(s_panel);
    if (ret == ESP_OK) {
        s_stream_stopped = true;
        gpio_intr_disable(LCD_TE_GPIO);
        ESP_LOGI(TAG, "Lock screen faded out; DPI refresh off");
    } else {
        ESP_LOGW(TAG, "Screen-off deferred: %s", esp_err_to_name(ret));
        restore_ui();
    }
}

static void dark_idle(void)
{
    /* Playing must retain CPU execution for future decoder/I2S output. Idle
     * uses bounded Light-sleep intervals, retaining PSRAM/UI and allowing SD
     * hotplug and PMIC monitoring to run between intervals. Never Deep-sleep:
     * it cannot resume the existing UI and is inappropriate for this lock. */
    if (music_player_adapter_is_playing() || !ui_lockscreen_anim_is_idle()) return;
    /* The production adapter may have a real decoder worker in the future.
     * Its SD accesses are covered by the same mutex; this check only makes
     * the present metadata-only path explicit. */
    if (gpio_get_level(TOUCH_IRQ_GPIO) == 0) {
        atomic_fetch_or(&s_events, EVENT_TOUCH);
        return;
    }
    /* A pending PMIC notification first needs the monitor to drain it. VBUS,
     * charging and SOC events can wake the CPU without turning on the screen. */
    if (gpio_get_level(PMIC_IRQ_GPIO) == 0) return;
    if (!pmic_axp2101_pause_monitor(true)) {
        pmic_axp2101_pause_monitor(false);
        return;
    }
    /* SD mutex covers open/read/close, JPEG cover decode, mount and unmount.
     * Acquire without waiting and outside LVGL; never sleep across live SD DMA.
     * Before SD init, the missing mutex also prevents an early sleep. */
    if (sd_card_io_lock(0)) {
        if (!atomic_load(&s_events) && gpio_get_level(PMIC_IRQ_GPIO) != 0 &&
            gpio_get_level(TOUCH_IRQ_GPIO) != 0 && !music_player_adapter_is_playing()) {
            esp_sleep_enable_timer_wakeup(1000000ULL);
            esp_err_t ret = esp_light_sleep_start();
            if (ret != ESP_OK && !s_sleep_fault_logged)
                ESP_LOGW(TAG, "Light-sleep unavailable: %s; screen remains off", esp_err_to_name(ret));
            s_sleep_fault_logged = ret != ESP_OK;
            uint64_t wake_pins = ret == ESP_OK ? esp_sleep_get_ext1_wakeup_status() : 0;
            /* EXT1 temporarily owns these RTC-capable pins. Return them to
             * digital GPIO before normal touch/PMIC edge IRQs are used again. */
            rtc_gpio_deinit(TOUCH_IRQ_GPIO);
            rtc_gpio_deinit(PMIC_IRQ_GPIO);
            if ((wake_pins & (1ULL << TOUCH_IRQ_GPIO)) || gpio_get_level(TOUCH_IRQ_GPIO) == 0)
                atomic_fetch_or(&s_events, EVENT_TOUCH);

        }
        sd_card_io_unlock();
    }
    pmic_axp2101_pause_monitor(false);
}

/* One serialized transition per iteration, also used by host regressions. */
static void screen_power_step(void)
{
    unsigned events = atomic_exchange(&s_events, 0);
    if (!atomic_load(&s_dark) && atomic_exchange(&s_brightness_pending, false))
        write_panel_brightness(atomic_load(&s_brightness));
    if (atomic_load(&s_dark)) {
        if (events & (EVENT_TOUCH | EVENT_KEY)) {
            if (s_stream_stopped) {
                esp_err_t ret = player_dpi_resume(s_panel);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "DPI resume failed: %s", esp_err_to_name(ret));
                    atomic_fetch_or(&s_events, events);
                    return;
                }
            }
            /* 渐灭中也能唤醒，此时 DPI 尚未停，无需重复 resume。 */
            s_fading_out = false;
            s_stream_stopped = false;
            gpio_intr_enable(LCD_TE_GPIO);
            restore_ui();
            ESP_LOGI(TAG, "Screen awake; lock MJPEG restarted without page slide");
        } else if (s_fading_out) {
            fade_screen_off_step();
        } else if (s_stream_stopped) {
            dark_idle();
        }
    } else if (events & EVENT_KEY) {
        if (lvgl_port_lock(0)) {
            /* 未锁屏：进入锁屏并滑入；锁屏亮屏：短按直接开始渐灭。
             * 不再次调用 lock/show，所以不会把短按误当成重新进入锁屏。 */
            if (ui_lockscreen_is_visible()) begin_screen_off_locked();
            else ui_manager_lock();
            lvgl_port_unlock();
        }
    } else {
        check_lock_timeout();
    }
}

static void power_task(void *arg)
{
    (void)arg;
    for (;;) {
        screen_power_step();
        vTaskDelay(pdMS_TO_TICKS(s_fading_out ? SCREEN_FADE_STEP_MS : 100U));
    }
}

esp_err_t screen_power_init(lv_display_t *display, lv_indev_t *input,
                            esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch)
{
    if (!display || !input || !panel || !touch || s_task) return ESP_ERR_INVALID_ARG;
    s_display = display;
    s_input = input;
    s_panel = panel;
    s_original_touch_irq = touch->config.interrupt_callback;
    /* GPIO6/GPIO10 are RTC GPIOs on P4. EXT1 does not replace their normal
     * negative-edge IRQ configuration with a level IRQ while the UI is awake.
     * Keep peripheral retention enabled for the current DSI/I2C drivers. */
    esp_err_t ret = esp_sleep_enable_ext1_wakeup_io(
        (1ULL << TOUCH_IRQ_GPIO) | (1ULL << PMIC_IRQ_GPIO), ESP_EXT1_WAKEUP_ANY_LOW);
    if (ret != ESP_OK) return ret;
    ret = esp_lcd_touch_register_interrupt_callback(touch, touch_irq);
    if (ret != ESP_OK) return ret;
    lvgl_port_lock(0);
    s_panel_brightness = ui_get_device_state()->brightness;
    atomic_store(&s_brightness, s_panel_brightness);
    lv_display_trigger_activity(display);
    lvgl_port_unlock();
    /* xTaskCreate uses internal RAM for stack: esp_light_sleep_start rejects
     * a PSRAM stack while caches are disabled during entry/exit. */
    if (xTaskCreate(power_task, "screen_power", 4096, NULL, 3, &s_task) != pdPASS) {
        esp_lcd_touch_register_interrupt_callback(touch, s_original_touch_irq);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "Lock timeout=10s, touch wake=GPIO6, PMIC key wake=GPIO10");
    return ESP_OK;
}
