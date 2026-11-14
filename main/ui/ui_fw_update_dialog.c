#include "ui_fw_update_dialog.h"

#include "ui_fonts.h"

#include <stdbool.h>
#include <stdio.h>

/**
 * 开机固件更新对话框。
 *
 * 这里刻意复制 ui_power_dialog 的尺寸/颜色/按键样式，而不是复用它的
 * 三键对象：关机弹窗的三个按钮、低电模式和电源任务绑定关系不应为了
 * OTA 再增加分支。新对话框只在开机阶段出现一次，完成或取消后整个
 * 对象（含定时器）删除，不占用运行期 LVGL 对象。
 */

typedef enum {
    FW_DIALOG_HIDDEN = 0,
    FW_DIALOG_PROMPT,
    FW_DIALOG_PROGRESS,
    FW_DIALOG_ERROR,
    FW_DIALOG_SUCCESS,
    FW_DIALOG_MANUAL_RESTART,
} fw_dialog_mode_t;

static lv_obj_t *s_overlay;
static lv_obj_t *s_title;
static lv_obj_t *s_body;
static lv_obj_t *s_hint;
static lv_obj_t *s_bar;
static lv_obj_t *s_pct;
static lv_obj_t *s_btn_confirm;
static lv_obj_t *s_btn_cancel;
static lv_obj_t *s_btn_back;
static lv_timer_t *s_timer;
static fw_dialog_mode_t s_mode;

static void update_timer_cb(lv_timer_t *timer);

/* ============================== 按键回调 ============================== */

static void confirm_cb(lv_event_t *event)
{
    (void)event;
    /* 只投递选择；真正的 OTA 任务由 firmware_update.c 创建。先切到
     * 进度视图，避免用户点击后仍停留在确认框上重复点击。 */
    firmware_update_confirm();
    ui_fw_update_dialog_show_progress();
}

static void cancel_cb(lv_event_t *event)
{
    (void)event;
    firmware_update_cancel();
    /* 只隐藏，不在这里删除对象：事件回调仍在该对象的事件链中，
     * 由 boot_check 在事件组返回后用 hide() 统一删除。 */
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void back_cb(lv_event_t *event)
{
    (void)event;
    firmware_update_dismiss();
    if (s_overlay) lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/* ============================== 视图切换 ============================== */

static void set_buttons_visible(bool confirm, bool cancel, bool back)
{
    if (!s_btn_confirm) return;
    if (confirm) lv_obj_remove_flag(s_btn_confirm, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_btn_confirm, LV_OBJ_FLAG_HIDDEN);
    if (cancel) lv_obj_remove_flag(s_btn_cancel, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_btn_cancel, LV_OBJ_FLAG_HIDDEN);
    if (back) lv_obj_remove_flag(s_btn_back, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_add_flag(s_btn_back, LV_OBJ_FLAG_HIDDEN);
}

static void set_progress_visible(bool visible)
{
    if (!s_bar) return;
    if (visible) {
        lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_pct, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_pct, LV_OBJ_FLAG_HIDDEN);
    }
}

static void create_dialog(void)
{
    if (s_overlay) return;

    /* 与关机弹窗相同的顶层全屏命中区：拦截下层触摸，不改动任何页面。 */
    s_overlay = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, LV_OPA_60, 0);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE |
        LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);

    lv_obj_t *card = lv_obj_create(s_overlay);
    lv_obj_set_size(card, 382, 204);
    lv_obj_center(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE |
        LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_color(card, lv_color_hex(0x242424), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(card, lv_color_white(), 0);
    lv_obj_set_style_border_opa(card, LV_OPA_30, 0);
    lv_obj_set_style_border_width(card, 1, 0);
    lv_obj_set_style_radius(card, 18, 0);
    lv_obj_set_style_pad_all(card, 12, 0);
    lv_obj_set_style_text_color(card, lv_color_white(), 0);
    lv_obj_set_style_text_font(card, ui_font_18(), 0);

    s_title = lv_label_create(card);
    lv_obj_set_width(s_title, 350);
    lv_obj_set_style_text_align(s_title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_title, LV_ALIGN_TOP_MID, 0, 2);
    lv_label_set_text(s_title, "发现新固件");

    s_body = lv_label_create(card);
    lv_obj_set_width(s_body, 350);
    lv_obj_set_style_text_align(s_body, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_body, LV_LABEL_LONG_WRAP);
    lv_obj_align(s_body, LV_ALIGN_TOP_MID, 0, 30);

    /* 进度条沿用开机提示条的白色细条尺寸；深色卡片上保持同一视觉语言。 */
    s_bar = lv_bar_create(card);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar, 320, 10);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 92);
    lv_bar_set_range(s_bar, 0, 100);
    lv_obj_set_style_radius(s_bar, 5, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_white(), LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 5, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar, lv_color_white(), LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_pct = lv_label_create(card);
    lv_obj_set_width(s_pct, 350);
    lv_obj_set_style_text_align(s_pct, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_pct, LV_ALIGN_TOP_MID, 0, 108);
    lv_label_set_text(s_pct, "0%");

    s_hint = lv_label_create(card);
    lv_obj_set_width(s_hint, 350);
    lv_obj_set_style_text_align(s_hint, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_opa(s_hint, LV_OPA_70, 0);
    lv_obj_align(s_hint, LV_ALIGN_TOP_MID, 0, 112);

    s_btn_confirm = lv_button_create(card);
    lv_obj_set_size(s_btn_confirm, 166, 44);
    lv_obj_align(s_btn_confirm, LV_ALIGN_BOTTOM_LEFT, 0, 0);
    lv_obj_set_style_bg_color(s_btn_confirm, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_btn_confirm, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(s_btn_confirm, 0, 0);
    ui_apply_pressed_style(s_btn_confirm);
    lv_obj_remove_flag(s_btn_confirm, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_btn_confirm, confirm_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *confirm_label = lv_label_create(s_btn_confirm);
    lv_label_set_text(confirm_label, "确认");
    lv_obj_center(confirm_label);

    s_btn_cancel = lv_button_create(card);
    lv_obj_set_size(s_btn_cancel, 166, 44);
    lv_obj_align(s_btn_cancel, LV_ALIGN_BOTTOM_LEFT, 180, 0);
    lv_obj_set_style_bg_color(s_btn_cancel, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_btn_cancel, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(s_btn_cancel, 0, 0);
    ui_apply_pressed_style(s_btn_cancel);
    lv_obj_remove_flag(s_btn_cancel, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_btn_cancel, cancel_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(s_btn_cancel);
    lv_label_set_text(cancel_label, "取消");
    lv_obj_center(cancel_label);

    s_btn_back = lv_button_create(card);
    lv_obj_set_size(s_btn_back, 166, 44);
    lv_obj_align(s_btn_back, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_bg_color(s_btn_back, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(s_btn_back, LV_OPA_20, 0);
    lv_obj_set_style_shadow_width(s_btn_back, 0, 0);
    ui_apply_pressed_style(s_btn_back);
    lv_obj_remove_flag(s_btn_back, LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_event_cb(s_btn_back, back_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *back_label = lv_label_create(s_btn_back);
    lv_label_set_text(back_label, "返回");
    lv_obj_center(back_label);
}

static void ensure_timer(void)
{
    if (!s_timer) s_timer = lv_timer_create(update_timer_cb, 100, NULL);
}

/* ============================== 定时刷新 ============================== */

static void update_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* 开机阶段屏幕电源任务已经在运行；更新/等待期间持续重置 LVGL
     * 无操作计时，避免 10 秒后自动熄屏而看不到进度。 */
    lv_display_trigger_activity(NULL);

    fw_update_state_t state = firmware_update_get_state();
    if (state == FW_UPDATE_STATE_RUNNING) {
        if (s_mode != FW_DIALOG_PROGRESS) ui_fw_update_dialog_show_progress();
        uint8_t pct = firmware_update_get_progress();
        lv_bar_set_value(s_bar, pct, LV_ANIM_OFF);
        lv_label_set_text_fmt(s_pct, "%u%%", (unsigned)pct);
    } else if (state == FW_UPDATE_STATE_SUCCESS) {
        if (s_mode != FW_DIALOG_SUCCESS) ui_fw_update_dialog_show_success();
        lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
        lv_label_set_text(s_pct, "100%");
    } else if (state == FW_UPDATE_STATE_FAILED) {
        if (s_mode != FW_DIALOG_ERROR)
            ui_fw_update_dialog_show_error(firmware_update_error_text());
    } else if (state == FW_UPDATE_STATE_RESTART_FAILED) {
        if (s_mode != FW_DIALOG_MANUAL_RESTART)
            ui_fw_update_dialog_show_manual_restart();
    }
}

/* ============================== 公共接口 ============================== */

void ui_fw_update_dialog_show_prompt(const fw_update_info_t *info)
{
    if (!info) return;
    create_dialog();
    s_mode = FW_DIALOG_PROMPT;

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);

    lv_label_set_text(s_title, "发现新固件");
    lv_label_set_text_fmt(s_body,
        "当前版本：%s\n新版本：%s\n大小：%u KB",
        info->current_version, info->version,
        (unsigned)((info->size + 1023U) / 1024U));
    lv_label_set_text(s_hint, "更新期间请勿断电");
    lv_obj_remove_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    set_progress_visible(false);
    set_buttons_visible(true, true, false);
    ensure_timer();

    /* 主任务随后会阻塞在事件组上；先强制提交一帧，保证确认框在
     * 任何同步等待之前已经真正显示。 */
    lv_refr_now(NULL);
}

void ui_fw_update_dialog_show_progress(void)
{
    if (!s_overlay) return;
    s_mode = FW_DIALOG_PROGRESS;

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    lv_label_set_text(s_title, "正在更新固件");
    lv_label_set_text(s_body, "正在写入 Flash，请勿断电");
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    set_progress_visible(true);
    set_buttons_visible(false, false, false);
    lv_bar_set_value(s_bar, firmware_update_get_progress(), LV_ANIM_OFF);
    lv_label_set_text_fmt(s_pct, "%u%%", (unsigned)firmware_update_get_progress());
}

void ui_fw_update_dialog_show_error(const char *text)
{
    if (!s_overlay) return;
    s_mode = FW_DIALOG_ERROR;

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    lv_label_set_text(s_title, "更新失败");
    lv_label_set_text(s_body, (text && *text) ? text : "未知错误");
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    set_progress_visible(false);
    set_buttons_visible(false, false, true);
}

void ui_fw_update_dialog_show_success(void)
{
    if (!s_overlay) return;
    s_mode = FW_DIALOG_SUCCESS;

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    lv_label_set_text(s_title, "更新完成");
    lv_label_set_text(s_body, "正在重启...");
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    set_progress_visible(true);
    set_buttons_visible(false, false, false);
    lv_bar_set_value(s_bar, 100, LV_ANIM_OFF);
    lv_label_set_text(s_pct, "100%");
}

void ui_fw_update_dialog_show_manual_restart(void)
{
    if (!s_overlay) return;
    s_mode = FW_DIALOG_MANUAL_RESTART;

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    lv_label_set_text(s_title, "更新完成");
    lv_label_set_text(s_body, "自动重启失败，请手动重启");
    lv_obj_add_flag(s_hint, LV_OBJ_FLAG_HIDDEN);
    set_progress_visible(false);
    set_buttons_visible(false, false, true);
}

void ui_fw_update_dialog_hide(void)
{
    if (s_timer) {
        lv_timer_delete(s_timer);
        s_timer = NULL;
    }
    if (s_overlay) {
        lv_obj_delete(s_overlay);
        s_overlay = NULL;
        s_title = s_body = s_hint = s_bar = s_pct = NULL;
        s_btn_confirm = s_btn_cancel = s_btn_back = NULL;
        lv_refr_now(NULL);
    }
    s_mode = FW_DIALOG_HIDDEN;
}
