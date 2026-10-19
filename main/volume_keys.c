#include "volume_keys.h"
#include "screen_power.h"
#include "audio_output.h"
#include "system_power.h"
#include "ui/ui_adj_panel.h"
#include "driver/gpio.h"
#include "esp_lvgl_port.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdbool.h>

#define VOLUME_UP_GPIO GPIO_NUM_35
#define VOLUME_DOWN_GPIO GPIO_NUM_34
#define KEY_ACTIVE_LEVEL 0
#define KEY_SCAN_MS 10U
#define KEY_DEBOUNCE_MS 30U
#define KEY_HOLD_MS 400U
#define KEY_REPEAT_MS 100U
#define VOLUME_STEP 5

typedef struct {
    bool raw, stable, armed, repeating;
    TickType_t changed, repeated;
} volume_key_t;

static volume_key_t s_keys[2];
static TaskHandle_t s_task;
static int s_pending;

/* 启动或熄屏且无音频任务时，不把已经按住的键当作新按下。
 * 音频准备/播放阻止 Light-sleep，此时熄屏只停止显示，音量键应继续工作。
 * 音量键仍不是唤醒源；真正睡眠/关机阶段丢弃事件，恢复后先释放再接受。 */
static void reset_keys(TickType_t now)
{
    for (unsigned i = 0; i < 2; ++i) {
        s_keys[i] = (volume_key_t){ .raw = true, .stable = true, .changed = now };
    }
    s_pending = 0;
}

static int scan_key(volume_key_t *key, bool down, TickType_t now)
{
    if (down != key->raw) { key->raw = down; key->changed = now; }
    if (down != key->stable && now - key->changed >= pdMS_TO_TICKS(KEY_DEBOUNCE_MS)) {
        key->stable = down;
        if (!down) { key->armed = true; key->repeating = false; return 0; }
        if (key->armed) { key->repeated = now; key->repeating = false; return 1; }
    }
    /* 原始输入已经释放时不连发；释放抖动稳定后才允许下一次新按下。 */
    if (down && key->stable && key->armed &&
        now - key->repeated >= pdMS_TO_TICKS(key->repeating ? KEY_REPEAT_MS : KEY_HOLD_MS)) {
        key->repeated = now;
        key->repeating = true;
        return 1;
    }
    return 0;
}

/* 与显示电源任务采用同一音频活跃快照（含准备阶段），而不是用屏幕
 * 亮灭代替系统是否仍运行。只在屏幕暗时增加此例外，原亮屏按键逻辑不变。
 * 熄屏时电源流程活跃则不接受新音量，避免保存后又被静默修改。 */
static bool can_adjust_volume(void)
{
    return !screen_power_is_dark() ||
           (audio_output_is_active() && !system_power_is_active());
}

static void poll_keys(void)
{
    TickType_t now = xTaskGetTickCount();
    if (!can_adjust_volume()) { reset_keys(now); return; }
    bool up = gpio_get_level(VOLUME_UP_GPIO) == KEY_ACTIVE_LEVEL;
    bool down = gpio_get_level(VOLUME_DOWN_GPIO) == KEY_ACTIVE_LEVEL;
    if (up && down) {
        /* 两键同时按住不相互竞争，也不积累稍后突然执行的音量事件。
         * 必须分别释放后才重新激活，避免松开其中一键时触发另一键连发。 */
        reset_keys(now);
        return;
    }
    int delta = (scan_key(&s_keys[0], up, now) - scan_key(&s_keys[1], down, now)) * VOLUME_STEP;
    s_pending += delta;
    if (s_pending > 100) s_pending = 100;
    if (s_pending < -100) s_pending = -100;
    /* GPIO 轮询不占 LVGL 锁。仅有事件时短暂获取；忙时保留增量，下轮再发。
     * 锁内重新判断，处理等待锁期间的熄屏、暂停和关机切换。熄屏分支仅
     * 同步音量/静音及后端请求：不展开面板、不更新显示活动、不重启刷新。
     * lvgl_port_stop() 只暂停 LVGL 定时处理，状态/对象仍须在其锁内修改。 */
    if (s_pending && lvgl_port_lock(5)) {
        if (can_adjust_volume()) {
            if (screen_power_is_dark()) ui_adj_panel_adjust_volume_silent(s_pending);
            else ui_adj_panel_adjust_volume(s_pending);
        }
        s_pending = 0;
        lvgl_port_unlock();
    }
}

static void key_task(void *arg)
{
    (void)arg;
    reset_keys(xTaskGetTickCount());
    for (;;) {
        poll_keys();
        vTaskDelay(pdMS_TO_TICKS(KEY_SCAN_MS));
    }
}

esp_err_t volume_keys_init(void)
{
    if (s_task) return ESP_OK;
    const gpio_config_t cfg = {
        .pin_bit_mask = (1ULL << VOLUME_UP_GPIO) | (1ULL << VOLUME_DOWN_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    esp_err_t ret = gpio_config(&cfg);
    if (ret != ESP_OK) return ret;
    /* 常驻只保留两个消抖状态与 2KB 任务栈，不分配事件队列/图像缓冲。
     * 显式 Light-sleep 会暂停本任务，10ms 轮询不会阻止现有睡眠入口。 */
    if (xTaskCreate(key_task, "volume_keys", 2048, NULL, 2, &s_task) != pdPASS) return ESP_ERR_NO_MEM;
    return ESP_OK;
}
