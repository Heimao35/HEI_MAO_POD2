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
#define LOW_BATTERY_DELAY_MS 30000U
#define LOW_BATTERY_RETRY_MS 5000U
static const char *TAG="system_power";
static TaskHandle_t s_worker;
static atomic_bool s_held, s_dialog, s_finalizing, s_popup_requested, s_latched, s_woke_on_press;
static atomic_uint s_pressed_at, s_request;
static atomic_bool s_saving;
static atomic_bool s_input_ready, s_short_ready;
/* PMIC -> 电源任务只有一个原子字，避免把不同采样的 SOC/VBUS 拼接。
 * 只在 PMIC 任务投递，不依赖 LVGL 状态，也保留初始化前收到的状态。 */
enum { BAT_VALID=1U<<8, BAT_PRESENT=1U<<9, BAT_PCT_VALID=1U<<10,
       BAT_VBUS=1U<<11, BAT_SEEN=1U<<12 };
static atomic_uint s_battery_sample;
static atomic_bool s_low_active;
static atomic_uint s_low_action;
/* 以下只由 system_power worker 操作。关闭弹窗只改 dismissed，
 * started 永不被重复 IRQ、相同 SOC、UI 刷新或按钮关闭重置。 */
static bool s_low_dismissed, s_low_prompted, s_low_immediate, s_low_retry;
static TickType_t s_low_started, s_low_retry_at;
static unsigned s_low_seconds=UINT32_MAX;

void system_power_update_battery(bool valid, bool present, bool pct_valid,
                                 uint8_t percent, bool vbus_present)
{
    unsigned sample=BAT_SEEN | percent;
    if(valid) sample|=BAT_VALID;
    if(present) sample|=BAT_PRESENT;
    if(pct_valid && percent<=100) sample|=BAT_PCT_VALID;
    if(vbus_present) sample|=BAT_VBUS;
    atomic_store(&s_battery_sample,sample);
    /* worker 每20ms检查，不另建任务，也不从 PMIC 等待它结束；因此
     * 不会与最后向 PMIC owner 投递断电命令形成双向等待。 */
}
static bool battery_is_low(unsigned sample)
{
    return (sample&(BAT_VALID|BAT_PRESENT|BAT_PCT_VALID|BAT_VBUS))==
        (BAT_VALID|BAT_PRESENT|BAT_PCT_VALID) && (sample&0xffU)<=1;
}
static bool battery_recovered(unsigned sample)
{
    return (sample&BAT_VALID) && ((sample&BAT_VBUS) || !(sample&BAT_PRESENT) ||
        ((sample&BAT_PCT_VALID) && (sample&0xffU)>1));
}

bool system_power_is_active(void)
{
    return atomic_load(&s_held) || atomic_load(&s_dialog) || atomic_load(&s_finalizing) ||
           atomic_load(&s_popup_requested) || atomic_load(&s_saving) || atomic_load(&s_low_active);
}

void system_power_key_edge(bool pressed)
{
    /* PMIC在SD/显示初始化前已启动。开机阶段的按下/松手只负责上电，
     * 不得预先排队成锁屏/熄屏事件；UI电源任务就绪后才接受新一轮按键。 */
    if (!atomic_load(&s_input_ready) || atomic_load(&s_low_active)) return;
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
    if (!atomic_load(&s_input_ready) || atomic_load(&s_low_active) ||
        !atomic_exchange(&s_short_ready,false)) return;
    if (atomic_exchange(&s_woke_on_press,false) || atomic_load(&s_latched) ||
        atomic_load(&s_dialog) || atomic_load(&s_finalizing)) return;
    screen_power_key_short_press();
}
void system_power_request(system_power_action_t action)
{
    if (!atomic_load(&s_dialog) || atomic_load(&s_finalizing)) return;
    unsigned expected=SYSTEM_POWER_NONE;
    if(atomic_load(&s_low_active)) {
        /* 低电提示无重启/取消关机选项。CANCEL 在此仅表示“关闭提示”，
         * 只向 worker 投递动作，不更改倒计时和硬件长按策略。 */
        if(action==SYSTEM_POWER_OFF || action==SYSTEM_POWER_CANCEL)
            atomic_compare_exchange_strong(&s_low_action,&expected,action);
        return;
    }
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
    if(!lvgl_port_lock(50)) return;
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
static void finish(system_power_action_t action, bool low_battery)
{
    atomic_store(&s_finalizing,true);
    if(!low_battery || !screen_power_is_dark())
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
        /* 低电路径必须由 PMIC owner 最后新读电量/VBUS 再断电；手动
         * 关机/重启保持原行为，插线不会取消用户明确请求的手动动作。 */
        ret = low_battery ? pmic_axp2101_poweroff_low_battery() :
            (action==SYSTEM_POWER_RESTART ? pmic_axp2101_restart() : pmic_axp2101_poweroff());
        /* ACK只代表PMIC收到命令；正常情况下整板供电随后消失/重新建立。
         * 若800ms后本次程序仍执行，恢复屏幕报告失败，绝不偷偷降级成
         * 仅P4软件重启。两路动作都保留十秒硬件强制关机兜底。 */
        if (ret==ESP_OK) { vTaskDelay(pdMS_TO_TICKS(800)); ret=ESP_FAIL; }
    }
    ESP_LOGE(TAG,"Shutdown/restart aborted: %s",esp_err_to_name(ret));
    screen_power_shutdown_display(false);
    music_player_adapter_pause(false);
    atomic_store(&s_finalizing,false);
    set_status(low_battery?"关机失败，将自动重试":"操作失败，请重试或取消",false);
}

/* 返回 true 表示低电流程接管本轮，不与手动弹窗/重启竞争。
 * 未知采样不启动关机，也不把已经启动的计时清零；只有明确恢复的
 * 电池/电量/VBUS 快照可解除低电流程。终态还会做一次硬件新读校验。 */
static bool low_battery_step(void)
{
    unsigned sample=atomic_load(&s_battery_sample);
    TickType_t now=xTaskGetTickCount();
    if(atomic_load(&s_low_active) && battery_recovered(sample)) {
        /* 插入有效 VBUS（包括充满后未充电）即撤销低电关机。隐藏尚在
         * 显示的低电提示，恢复正常触摸/按键；不能留下占用手势的遮罩。 */
        if(!lvgl_port_lock(50)) return true;
        ui_power_dialog_hide();
        lv_display_trigger_activity(lv_display_get_default());
        lvgl_port_unlock();
        atomic_store(&s_dialog,false);
        atomic_store(&s_low_action,SYSTEM_POWER_NONE);
        atomic_store(&s_low_active,false);
        return false;
    }
    if(!atomic_load(&s_low_active)) {
        if(!battery_is_low(sample)) return false;
        s_low_started=now;
        s_low_dismissed=s_low_prompted=s_low_retry=false;
        s_low_immediate=screen_power_is_dark();
        s_low_seconds=UINT32_MAX;
        atomic_store(&s_low_action,SYSTEM_POWER_NONE);
        atomic_store(&s_request,SYSTEM_POWER_NONE);
        atomic_store(&s_popup_requested,false);
        atomic_store(&s_held,false);
        atomic_store(&s_short_ready,false);
        atomic_store(&s_low_active,true);
    }
    unsigned action=atomic_exchange(&s_low_action,SYSTEM_POWER_NONE);
    if(action==SYSTEM_POWER_OFF) s_low_immediate=true;
    if(action==SYSTEM_POWER_CANCEL) {
        /* 关闭只隐藏 UI，不改变 started/immediate。取锁失败保留关闭
         * 请求，避免点击恰好遇到渲染忙而丢失；期限仍照常计算。 */
        s_low_dismissed=true;
        if(lvgl_port_lock(50)) {
            ui_power_dialog_hide();
            lvgl_port_unlock();
            atomic_store(&s_dialog,false);
        } else atomic_store(&s_low_action,SYSTEM_POWER_CANCEL);
    }
    TickType_t elapsed=now-s_low_started; /* 无符号差支持 tick 回绕 */
    bool due=s_low_immediate || elapsed>=pdMS_TO_TICKS(LOW_BATTERY_DELAY_MS);
    if(due && battery_is_low(sample) &&
       (!s_low_retry || action==SYSTEM_POWER_OFF ||
        (TickType_t)(now-s_low_retry_at)>=pdMS_TO_TICKS(LOW_BATTERY_RETRY_MS))) {
        s_low_retry=true;
        finish(SYSTEM_POWER_OFF,true);
        s_low_retry_at=xTaskGetTickCount();
        return true;
    }
    if(!s_low_dismissed && !screen_power_is_dark()) {
        unsigned seconds=elapsed>=pdMS_TO_TICKS(LOW_BATTERY_DELAY_MS) ? 0 :
            (unsigned)((pdMS_TO_TICKS(LOW_BATTERY_DELAY_MS)-elapsed+
                        pdMS_TO_TICKS(1000)-1)/pdMS_TO_TICKS(1000));
        if((!s_low_prompted || seconds!=s_low_seconds) && lvgl_port_lock(50)) {
            ui_power_dialog_show_low_battery(seconds);
            if(due) ui_power_dialog_set_status(s_low_retry?"关机失败，将自动重试":"正在确认电量，请稍候",false);
            atomic_store(&s_dialog,true);
            s_low_prompted=true;s_low_seconds=seconds;
            lvgl_port_unlock();
        }
    }
    return true;
}

static void system_power_step(void)
{
    if(!atomic_load(&s_input_ready)) return;
    if(low_battery_step()) return;
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
    } else if (action==SYSTEM_POWER_OFF || action==SYSTEM_POWER_RESTART) finish(action,false);
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
