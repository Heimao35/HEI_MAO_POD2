#include "screen_power.h"
#include "ambient_light.h"
#include "ui/ui_adj_panel.h"
#include "system_power.h"
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
#define EVENT_TOUCH  (1U << 0) /* 仅为触摸候选，必须读CST820确认 */
#define EVENT_KEY    (1U << 1)
#define EVENT_WAKE_REQUEST (1U << 2) /* 已由业务逻辑确认的主动唤醒 */
#define TOUCH_PROBE_MIN_MS 20U

static const char *TAG = "screen_power";
static lv_display_t *s_display;
static lv_indev_t *s_input;
static esp_lcd_panel_handle_t s_panel;
static esp_lcd_touch_handle_t s_touch;
static esp_lcd_touch_interrupt_callback_t s_original_touch_irq;
static TaskHandle_t s_task;
static atomic_uint s_events;
static atomic_bool s_dark;
static atomic_uchar s_brightness = 80;
static atomic_bool s_brightness_pending;
/* UI只在极短临界区投递模式/手动命令；ADC和DCS不持有此锁或LVGL锁。
 * generation用于丢弃采样期间发生手动拖动/模式切换后已过期的自动结果。 */
static portMUX_TYPE s_brightness_mux = portMUX_INITIALIZER_UNLOCKED;
static bool s_auto_brightness;
static uint32_t s_brightness_generation;
static bool s_auto_ui_dirty;
static uint32_t s_auto_ui_generation;
static bool s_stream_stopped;
static atomic_bool s_shutdown_display, s_display_suspended;
static bool s_sleep_fault_logged;
static bool s_fading_out;
static TickType_t s_fade_started;
static uint8_t s_fade_from;
static bool s_touch_probed, s_touch_fault_logged, s_touch_rejected_logged;
static bool s_touch_probe_pending;
static TickType_t s_touch_probe_at;
/* s_brightness 保存用户偏好，s_panel_brightness 保存实际已写入亮度。
 * 渐灭只改变后者，唤醒仍恢复到用户偏好；都只由电源任务写硬件。 */
static uint8_t s_panel_brightness = 80;

/* 任务上下文事件统一唤醒等待中的电源任务。软件请求与原始触摸IRQ必须
 * 分开：电源键按下/关机失败恢复不需要也不能依赖触摸控制器存在触点。 */
static void post_event(unsigned event)
{
    atomic_fetch_or(&s_events, event);
    if (s_task) xTaskNotifyGive(s_task);
}

void screen_power_request_wake(void)
{ post_event(EVENT_WAKE_REQUEST); }
void screen_power_shutdown_display(bool off)
{
    atomic_store(&s_shutdown_display,off);
    if (!off) screen_power_request_wake();
}
bool screen_power_display_suspended(void)
{ return atomic_load(&s_display_suspended); }

bool screen_power_is_dark(void)
{
    return atomic_load(&s_dark);
}

void screen_power_key_short_press(void)
{
    /* No UI/I2C in PMIC IRQ consumer. The power task serializes transitions. */
    post_event(EVENT_KEY);
}

static void IRAM_ATTR touch_irq(esp_lcd_touch_handle_t touch)
{
    /* ISR只登记候选并通知任务，绝不读I2C/等待LVGL。亮屏时仍使用原
     * esp_lvgl_port回调；熄屏时立即唤醒检查任务，避免100ms固定轮询
     * 把快速点按拖到松手后才读，造成“需要长按触摸才能唤醒”。 */
    if (atomic_load(&s_dark)) {
        atomic_fetch_or(&s_events, EVENT_TOUCH);
        BaseType_t woken = pdFALSE;
        if (s_task) vTaskNotifyGiveFromISR(s_task, &woken);
        if (woken) portYIELD_FROM_ISR();
    } else if (s_original_touch_irq) s_original_touch_irq(touch);
}

void screen_power_set_brightness(uint8_t brightness)
{
    if (brightness < 5) brightness = 5;
    if (brightness > 100) brightness = 100;
    /* 此入口仅代表用户手动操作。自动渐变不再绕回此回调，否则UI锁竞争
     * 会让硬件中间值无法提交。临界区只交接标量，绝不调用ADC/DCS/LVGL。 */
    portENTER_CRITICAL(&s_brightness_mux);
    s_auto_brightness = false;
    ++s_brightness_generation;
    atomic_store(&s_brightness, brightness);
    atomic_store(&s_brightness_pending, true);
    portEXIT_CRITICAL(&s_brightness_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

void screen_power_set_auto_brightness(bool enabled)
{
    /* A按钮事件已经持有LVGL锁；这里只投递模式，不能等待显示任务。
     * 关闭后即使UI暂时没机会刷新，也立即停止自动渐变并保留实际亮度。 */
    portENTER_CRITICAL(&s_brightness_mux);
    s_auto_brightness = enabled;
    ++s_brightness_generation;
    portEXIT_CRITICAL(&s_brightness_mux);
    if (s_task) xTaskNotifyGive(s_task);
}

static esp_err_t write_panel_brightness(uint8_t brightness)
{
    esp_err_t ret = esp_lcd_panel_co5300_set_brightness(s_panel, brightness);
    if (ret == ESP_OK) s_panel_brightness = brightness;
    return ret;
}

/* 只在熄屏的电源任务调用；此时LVGL输入和计时已停止，所以不会和
 * 正常触摸读取并发。所有阻塞I2C均在ISR/LVGL锁/SD锁之外完成。
 * IRQ低电平或EXT1标志只能说明需要检查控制器，不能证明手指按下。
 * 每次重新read_data后再get_data，不使用熄屏前留下的坐标缓存；失败、
 * 零触点或越界数据均保持黑屏。持续低电平/IRQ风暴最多每20ms读一次。 */
static bool touch_confirms_wake(void)
{
    TickType_t now = xTaskGetTickCount();
    if (s_touch_probed && now - s_touch_probe_at < pdMS_TO_TICKS(TOUCH_PROBE_MIN_MS))
        return false;
    s_touch_probe_pending = false;
    s_touch_probed = true;
    s_touch_probe_at = now;
    esp_lcd_touch_point_data_t point = {0};
    uint8_t count = 0;
    esp_err_t ret = esp_lcd_touch_read_data(s_touch);
    if (ret == ESP_OK) ret = esp_lcd_touch_get_data(s_touch, &point, &count, 1);
    if (ret != ESP_OK) {
        if (!s_touch_fault_logged)
            ESP_LOGW(TAG, "Touch wake check failed: %s; keeping screen off", esp_err_to_name(ret));
        s_touch_fault_logged = true;
        return false;
    }
    s_touch_fault_logged = false;
    bool touched = count != 0 && point.x < UI_HOR_RES && point.y < UI_VER_RES;
    if (!touched && !s_touch_rejected_logged) {
        ESP_LOGI(TAG, "Touch wake hint without valid contact; keeping screen off");
        s_touch_rejected_logged = true;
    }
    return touched;
}

/* 调用方持有 LVGL 锁。超时与锁屏下短按共用这条路径：先停止画面和
 * 输入活动，再让显示链路继续输出固定画面，直到亮度渐灭完毕才停 DMA。
 * s_dark 在渐灭开始时即置位，使此期间触摸也能取消熄屏，不会丢失操作。 */
static void begin_screen_off_locked(void)
{
    s_touch_probed = false;
    s_touch_probe_pending = false;
    s_touch_rejected_logged = false;
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
    if (system_power_is_active()) return;
    if (lvgl_port_lock(ambient_light_is_transitioning() ? 1 : 50)) {
        /* 渐变期间不为了检查锁屏超时长等LVGL，否则会拖慢锁外硬件步进。
         * 只在锁屏计时。LVGL 的显示器活动时间涵盖全屏输入，而不只是在
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
        atomic_store(&s_display_suspended,true);
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
    if (s_touch_probe_pending || system_power_is_active() || music_player_adapter_is_playing() || !ui_lockscreen_anim_is_idle()) return;
    /* The production adapter may have a real decoder worker in the future.
     * Its SD accesses are covered by the same mutex; this check only makes
     * the present metadata-only path explicit. */
    /* 低电平候选已在screen_power_step中读触摸数据。仍为低时不进入
     * ANY_LOW睡眠，避免立刻被同一电平唤醒；也不再给自己排无条件亮屏
     * 事件/通知，防止持续低电平导致I2C忙循环。 */
    if (gpio_get_level(TOUCH_IRQ_GPIO) == 0) return;
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
        if (!system_power_is_active() && !atomic_load(&s_events) && gpio_get_level(PMIC_IRQ_GPIO) != 0 &&
            gpio_get_level(TOUCH_IRQ_GPIO) != 0 && !music_player_adapter_is_playing()) {
            esp_sleep_enable_timer_wakeup(1000000ULL);
            esp_err_t ret = esp_light_sleep_start();
            if (ret != ESP_OK && !s_sleep_fault_logged)
                ESP_LOGW(TAG, "Light-sleep unavailable: %s; screen remains off", esp_err_to_name(ret));
            s_sleep_fault_logged = ret != ESP_OK;
            uint64_t wake_pins = ret == ESP_OK ? esp_sleep_get_ext1_wakeup_status() : 0;
            /* EXT1 temporarily owns these RTC-capable pins. Return them to
             * digital GPIO before normal touch/PMIC edge IRQs are used again. */
            /* v5.5.4的EXT1会把唤醒pad设为HOLD；先解除HOLD，再切回
             * 数字GPIO。仅deinit无法解除冻结的MUX/输入配置：触摸可能
             * 被误读为低电平、PMIC下降沿ISR也可能失效而退化到5秒轮询。
             * 即使睡眠被拒绝也执行清理，避免部分准备留下pad保持状态。 */
            rtc_gpio_hold_dis(TOUCH_IRQ_GPIO);
            rtc_gpio_hold_dis(PMIC_IRQ_GPIO);
            rtc_gpio_deinit(TOUCH_IRQ_GPIO);
            rtc_gpio_deinit(PMIC_IRQ_GPIO);
            if ((wake_pins & (1ULL << TOUCH_IRQ_GPIO)) || gpio_get_level(TOUCH_IRQ_GPIO) == 0)
                post_event(EVENT_TOUCH);

        }
        sd_card_io_unlock();
    }
    pmic_axp2101_pause_monitor(false);
}

/* 硬件渐变不能依赖LVGL锁：歌词/字体绘制可能占锁超过250ms，旧实现
 * 会丢失整个过渡的中间值，解锁时按已经过去的时间直接写终点。
 * 现在自动模式由UI回调投递，ADC/渐变/DCS均由显示任务锁外推进；
 * generation复核保证采样期间手动操作后，迟到结果不能覆盖用户命令。 */
static void update_ambient_brightness(void)
{
    if (screen_power_is_dark() || system_power_is_active()) {
        ambient_light_reset();
        return;
    }
    portENTER_CRITICAL(&s_brightness_mux);
    bool automatic = s_auto_brightness;
    uint32_t generation = s_brightness_generation;
    portEXIT_CRITICAL(&s_brightness_mux);
    if (!automatic) { ambient_light_reset(); return; }
    uint8_t target;
    /* 从已经成功发送的物理亮度起步；UI滑条允许稍后同步，不能当作硬件值。 */
    if (!ambient_light_poll(s_panel_brightness, &target)) return;
    if (system_power_is_active()) return;
    portENTER_CRITICAL(&s_brightness_mux);
    if (s_auto_brightness && s_brightness_generation == generation) {
        atomic_store(&s_brightness, target);
        atomic_store(&s_brightness_pending, true);
        s_auto_ui_dirty = true;
        s_auto_ui_generation = generation;
    }
    portEXIT_CRITICAL(&s_brightness_mux);
}

/* UI同步只是观察者。拿不到锁就保留dirty，下轮更新最新实际亮度，不能阻断
 * 硬件渐变，也不重放积压的滑条动画。必须锁内发布状态，且不触发用户活动。
 * 手动/模式事件只可能在LVGL锁内发生，因此锁内复核代次后不会覆盖新操作。 */
static void publish_ambient_brightness(void)
{
    if (!s_auto_ui_dirty || screen_power_is_dark() || system_power_is_active()) return;
    if (!lvgl_port_lock(1)) return;
    portENTER_CRITICAL(&s_brightness_mux);
    bool current = s_auto_brightness && s_auto_ui_generation == s_brightness_generation;
    portEXIT_CRITICAL(&s_brightness_mux);
    if (current && ui_get_device_state()->auto_brightness) {
        ui_get_device_state()->brightness = s_panel_brightness;
        ui_adj_panel_update_brightness();
    }
    s_auto_ui_dirty = false;
    lvgl_port_unlock();
}

/* One serialized transition per iteration, also used by host regressions. */
static void screen_power_step(void)
{
    unsigned events = atomic_exchange(&s_events, 0);
    /* 确认关机后复用渐灭/DPI 停止逻辑，但不进入 Light-sleep，也不接受
     * 唤醒打断持久化。失败时清除请求后再沿原唤醒路径恢复。 */
    if (atomic_load(&s_shutdown_display)) {
        ambient_light_reset();
        if (!atomic_load(&s_dark)) {
            if (lvgl_port_lock(50)) {
                begin_screen_off_locked();
                lvgl_port_unlock();
            }
        } else if (s_fading_out) fade_screen_off_step();
        return;
    }
    update_ambient_brightness();
    if (!atomic_load(&s_dark) && atomic_exchange(&s_brightness_pending, false)) {
        if (write_panel_brightness(atomic_load(&s_brightness)) != ESP_OK)
            atomic_store(&s_brightness_pending, true); /* 失败不把建议值冒充实际亮度。 */
    }
    publish_ambient_brightness();
    if (atomic_load(&s_dark)) {
        bool explicit_wake = (events & (EVENT_KEY | EVENT_WAKE_REQUEST)) != 0;
        bool touch_wake = false;
        if ((events & EVENT_TOUCH) || gpio_get_level(TOUCH_IRQ_GPIO) == 0)
            s_touch_probe_pending = true;
        if (!explicit_wake && s_touch_probe_pending) touch_wake = touch_confirms_wake();
        /* CPU从EXT1醒来不等于用户要求亮屏；仅确认触点或独立电源键/
         * 软件请求才能恢复显示。渐灭途中也执行相同检查，不被空IRQ打断。 */
        if (explicit_wake || touch_wake) {
            if (s_stream_stopped) {
                esp_err_t ret = player_dpi_resume(s_panel);
                if (ret != ESP_OK) {
                    ESP_LOGE(TAG, "DPI resume failed: %s", esp_err_to_name(ret));
                    /* 已确认的触摸需重试DPI恢复时升级为主动唤醒请求，
                     * 不能因为下一轮手指已抬起而遗失这次有效操作。 */
                    post_event(EVENT_WAKE_REQUEST);
                    return;
                }
            }
            /* 渐灭中也能唤醒，此时 DPI 尚未停，无需重复 resume。 */
            s_touch_probe_pending = false;
            s_fading_out = false;
            s_stream_stopped = false;
            atomic_store(&s_display_suspended,false);
            gpio_intr_enable(LCD_TE_GPIO);
            restore_ui();
            ESP_LOGI(TAG, "Screen awake (%s); lock MJPEG restarted without page slide",
                     touch_wake ? "touch data" : (events & EVENT_KEY) ? "power key" : "request");
        } else if (s_fading_out) {
            fade_screen_off_step();
        } else if (s_stream_stopped) {
            dark_idle();
        }
    } else if (events & EVENT_KEY) {
        if (system_power_is_active()) return;
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
        /* 有候选立即读触摸，平时仍用原周期推进渐灭/锁屏超时。RTC触摸
         * 唤醒后也会post_event，因此不额外等100ms才确认一次点按。 */
        /* 自动亮度只有跨档的250ms线性过渡用20ms步进，稳定后仍100ms轮询。
         * ADC自身限速100ms；中间步只写小型亮度命令，不新增全屏动画/遮罩。 */
        bool fast_step = s_fading_out || s_touch_probe_pending || ambient_light_is_transitioning();
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(fast_step ? SCREEN_FADE_STEP_MS : 100U));
    }
}

esp_err_t screen_power_init(lv_display_t *display, lv_indev_t *input,
                            esp_lcd_panel_handle_t panel, esp_lcd_touch_handle_t touch)
{
    if (!display || !input || !panel || !touch || s_task) return ESP_ERR_INVALID_ARG;
    s_display = display;
    s_input = input;
    s_panel = panel;
    s_touch = touch;
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
    portENTER_CRITICAL(&s_brightness_mux);
    s_auto_brightness = ui_get_device_state()->auto_brightness;
    ++s_brightness_generation;
    portEXIT_CRITICAL(&s_brightness_mux);
    atomic_store(&s_brightness_pending,true); /* 应用恢复的偏好，不沿用面板上电80%。 */
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
