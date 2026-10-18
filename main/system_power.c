#include "system_power.h"
#include "screen_power.h"
#include "pmic_axp2101.h"
#include "player_settings.h"
#include "music_player_adapter.h"
#include "sd_card.h"
#include "ui/ui_power_dialog.h"
#include "ui/ui_lockscreen.h"
#include "esp_lvgl_port.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <stdatomic.h>

#define KEY_DIALOG_MS 6000U
#define QUIESCE_MS 2500U
#define SAVE_DELAY_MS 3000U
static const char *TAG="system_power";
static TaskHandle_t s_worker;
static atomic_bool s_held, s_dialog, s_finalizing, s_popup_requested, s_latched, s_woke_on_press;
static atomic_uint s_pressed_at, s_request;
static atomic_bool s_saving;
static atomic_bool s_input_ready, s_short_ready;

bool system_power_is_active(void)
{
    return atomic_load(&s_held) || atomic_load(&s_dialog) || atomic_load(&s_finalizing) ||
           atomic_load(&s_popup_requested) || atomic_load(&s_saving);
}

void system_power_key_edge(bool pressed)
{
    /* PMIC在SD/显示初始化前已启动。开机阶段的按下/松手只负责上电，
     * 不得预先排队成锁屏/熄屏事件；UI电源任务就绪后才接受新一轮按键。 */
    if (!atomic_load(&s_input_ready)) return;
    TickType_t now=xTaskGetTickCount();
    if (pressed) {
        /* 只接受第一次下降沿，重复 IRQ 不能把六秒起点往后推。
         * 从熄屏开始按住也立即唤醒，再由同一计时到六秒弹窗。 */
        if (!atomic_load(&s_held)) {
            atomic_store(&s_short_ready,false);
            atomic_store(&s_pressed_at,now);
            atomic_store(&s_latched,false);
            atomic_store(&s_woke_on_press,screen_power_is_dark());
            atomic_store(&s_held,true);
            if (atomic_load(&s_woke_on_press)) screen_power_request_wake();
        }
    } else if (atomic_exchange(&s_held,false)) {
        /* 只有本次运行观测到完整按下/释放，才允许随后短按通知触发UI。
         * 上电按住在初始化后才松手、孤立短按和重复通知均不能熄屏。 */
        atomic_store(&s_short_ready,true);
        if ((TickType_t)(now-atomic_load(&s_pressed_at))>=pdMS_TO_TICKS(KEY_DIALOG_MS) &&
            !atomic_exchange(&s_latched,true)) atomic_store(&s_popup_requested,true);
    }
}
void system_power_key_short(void)
{
    /* PMIC 短按在松手时送达：熄屏按下已经亮屏，不再让松手短按熄屏。
     * 长按后即使同时收到短按事件也不能改变页面/关闭弹窗。 */
    if (!atomic_load(&s_input_ready) || !atomic_exchange(&s_short_ready,false)) return;
    if (atomic_exchange(&s_woke_on_press,false) || atomic_load(&s_latched) ||
        atomic_load(&s_dialog) || atomic_load(&s_finalizing)) return;
    screen_power_key_short_press();
}
void system_power_request(system_power_action_t action)
{
    if (!atomic_load(&s_dialog) || atomic_load(&s_finalizing)) return;
    unsigned expected=SYSTEM_POWER_NONE;
    atomic_compare_exchange_strong(&s_request,&expected,action);
}
static bool capture(player_settings_t *snapshot)
{
    if (!lvgl_port_lock(50)) return false;
    player_settings_capture(snapshot);
    lvgl_port_unlock();
    return true;
}
static void set_status(const char *text, bool busy)
{
    lvgl_port_lock(0);
    ui_power_dialog_set_status(text,busy);
    lvgl_port_unlock();
}
static esp_err_t quiesce(void)
{
    TickType_t started=xTaskGetTickCount();
    if (lvgl_port_lock(0)) {
        ui_lockscreen_stop_anim();
        lvgl_port_unlock();
    }
    /* 所有等待均在 LVGL 锁外。先停新请求，再等已经开始的 SD/JPEG 工作
     * 结束；超时直接返回可恢复状态，不能强杀持有文件/锁的任务。 */
    while (!music_player_adapter_pause(true) || !ui_lockscreen_anim_is_idle()) {
        if ((TickType_t)(xTaskGetTickCount()-started)>=pdMS_TO_TICKS(QUIESCE_MS)) return ESP_ERR_TIMEOUT;
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    esp_err_t ret=music_player_adapter_capture_selection();
    if (ret!=ESP_OK) return ret;
    /* SD 文件均在各自 worker 的临界区内关闭；在这里过一道空闲屏障。
     * SD 热插拔任务仍可更新状态，不卸载/重格式化用户卡。 */
    if (sd_card_is_mounted()) {
        if (!sd_card_io_lock(pdMS_TO_TICKS(500))) return ESP_ERR_TIMEOUT;
        sd_card_io_unlock();
    }
    return ESP_OK;
}
static void finish(system_power_action_t action)
{
    atomic_store(&s_finalizing,true);
    set_status(action==SYSTEM_POWER_RESTART?"正在保存，即将重启":"正在保存，即将关机",true);
    esp_err_t ret=quiesce();
    player_settings_t snapshot;
    if (ret==ESP_OK) {
        screen_power_shutdown_display(true);
        TickType_t started=xTaskGetTickCount();
        while (!screen_power_display_suspended()) {
            if ((TickType_t)(xTaskGetTickCount()-started)>=pdMS_TO_TICKS(2000)) { ret=ESP_ERR_TIMEOUT; break; }
            vTaskDelay(pdMS_TO_TICKS(20));
        }
    }
    /* 渐灭开始后实体音量键已停止发布；最后再抓一次用户设置，避免在
     * 等待媒体空闲时按键产生的新音量落在旧快照之后。亮度取偏好值，
     * 不取实际渐灭到零的面板亮度。 */
    if (ret==ESP_OK && !capture(&snapshot)) ret=ESP_ERR_TIMEOUT;
    if (ret==ESP_OK) ret=player_settings_save(&snapshot);
    if (ret==ESP_OK) {
        ESP_LOGI(TAG,"Settings committed and verified; action=%d",action);
        ret = action==SYSTEM_POWER_RESTART ? pmic_axp2101_restart() : pmic_axp2101_poweroff();
        /* ACK只代表PMIC收到命令；正常情况下整板供电随后消失/重新建立。
         * 若800ms后本次程序仍执行，恢复屏幕报告失败，绝不偷偷降级成
         * 仅P4软件重启。两路动作都保留十秒硬件强制关机兜底。 */
        if (ret==ESP_OK) { vTaskDelay(pdMS_TO_TICKS(800)); ret=ESP_FAIL; }
    }
    ESP_LOGE(TAG,"Shutdown/restart aborted: %s",esp_err_to_name(ret));
    screen_power_shutdown_display(false);
    music_player_adapter_pause(false);
    atomic_store(&s_finalizing,false);
    set_status("操作失败，请重试或取消",false);
}

static void system_power_step(void)
{
    TickType_t now=xTaskGetTickCount();
    if (atomic_load(&s_held) &&
        (TickType_t)(now-atomic_load(&s_pressed_at))>=pdMS_TO_TICKS(KEY_DIALOG_MS) &&
        !atomic_exchange(&s_latched,true)) atomic_store(&s_popup_requested,true);
    if (atomic_load(&s_popup_requested) && !atomic_load(&s_finalizing)) {
        if (screen_power_is_dark()) { screen_power_request_wake(); return; }
        if (!lvgl_port_lock(50)) return;
        if (!atomic_load(&s_dialog)) {
            atomic_store(&s_dialog,true);
            ui_power_dialog_show();
        }
        atomic_store(&s_popup_requested,false);
        lvgl_port_unlock();
    }
    unsigned action=atomic_exchange(&s_request,SYSTEM_POWER_NONE);
    if (action==SYSTEM_POWER_CANCEL) {
        lvgl_port_lock(0);
        ui_power_dialog_hide();
        lv_display_trigger_activity(lv_display_get_default());
        lvgl_port_unlock();
        atomic_store(&s_dialog,false);
        /* 不重置 s_latched：同一次持续按住取消后不反复弹窗。
         * 不碰 PMIC OFFLEVEL/使能，继续按到十秒仍硬件断电。 */
    } else if (action==SYSTEM_POWER_OFF || action==SYSTEM_POWER_RESTART) finish(action);
}
static void power_worker(void *arg)
{
    (void)arg;
    player_settings_t previous={0}, current={0};
    bool have_previous=false, dirty=false;
    TickType_t changed=0;
    for (;;) {
        system_power_step();
        /* RAM 每 100ms 采样（此任务周期20ms，下方分频），停止调整3s后合并
         * 保存；强制断电最多丢失尚未提交的最近修改，不能保证崩溃时能写 Flash。
         * 正常关机/重启走 finish()，无条件等待最终提交，不受此延迟限制。 */
        static unsigned divider;
        if (++divider>=5) {
            divider=0;
            if (!atomic_load(&s_finalizing) && capture(&current)) {
                TickType_t now=xTaskGetTickCount();
                if (!have_previous || !player_settings_equal(&current,&previous)) {
                    previous=current; have_previous=true; dirty=true; changed=now;
                }
                if (dirty && !system_power_is_active() && !screen_power_is_dark() &&
                    (TickType_t)(now-changed)>=pdMS_TO_TICKS(SAVE_DELAY_MS)) {
                    /* 先公布保存活动，再复查熄屏；显示任务进入 Light-sleep
                     * 前也复查活动，防止 Flash 提交与浅睡眠同时开始。 */
                    atomic_store(&s_saving,true);
                    if (!screen_power_is_dark()) {
                        esp_err_t ret=player_settings_save(&current);
                        if (ret!=ESP_OK) ESP_LOGW(TAG,"Settings save deferred: %s",esp_err_to_name(ret));
                        dirty=ret!=ESP_OK; changed=now;
                    }
                    atomic_store(&s_saving,false);
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}
esp_err_t system_power_init(void)
{
    if (s_worker) return ESP_ERR_INVALID_STATE;
    /* 内部 RAM 栈，避免 Flash 提交期间栈位于 PSRAM。 */
    if (xTaskCreate(power_worker,"system_power",8192,NULL,3,&s_worker)!=pdPASS) return ESP_ERR_NO_MEM;
    atomic_store(&s_input_ready,true);
    return ESP_OK;
}
