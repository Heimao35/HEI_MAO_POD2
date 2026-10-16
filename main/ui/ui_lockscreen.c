/**
 * @file ui_lockscreen.c
 * @brief 锁屏页面实现
 *
 * 全屏覆盖层，显示背景图 + 大字体时间 + 小字体日期。
 * 用户上滑解锁（由 ui_manager 的手势检测驱动调用 ui_lockscreen_hide）。
 */

#include "ui_lockscreen.h"
#include "ui_statusbar.h"
#include <stdio.h>

static lv_obj_t *s_lock_page  = NULL; /**< 锁屏页面容器（全屏） */
static lv_obj_t *s_time_label = NULL; /**< 大字体时间 label */
static lv_obj_t *s_date_label = NULL; /**< 小字体日期 label */
static bool      s_visible    = true; /**< 锁屏是否可见 */

/**
 * @brief 创建锁屏页面
 *
 * 布局：全屏黑色背景（后续替换为用户图片），
 * 中上方大字体时间，其下小字体日期。
 *
 * @param parent 父对象
 */
void ui_lockscreen_create(lv_obj_t *parent)
{
    s_lock_page = lv_obj_create(parent);
    lv_obj_set_size(s_lock_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_align(s_lock_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_lock_page, lv_color_hex(0x0D0D1A), 0);
    lv_obj_set_style_bg_opa(s_lock_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_lock_page, 0, 0);
    lv_obj_set_style_radius(s_lock_page, 0, 0);
    lv_obj_set_style_pad_all(s_lock_page, 0, 0);
    lv_obj_remove_flag(s_lock_page, LV_OBJ_FLAG_SCROLLABLE);
    /* 允许触摸事件冒泡到父屏幕，以便 ui_manager 的手势检测能收到事件 */
    lv_obj_add_flag(s_lock_page, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 大字体时间 —— 位于屏幕中上方约 35% 高度处
     * 修改 y 偏移量可调节时间显示位置 */
    s_time_label = lv_label_create(s_lock_page);
    lv_label_set_text(s_time_label, "12:00");
    lv_obj_set_style_text_color(s_time_label, lv_color_white(), 0);
    /* 使用 montserrat_26 作为最大可用字体，后续可替换为更大字体 */
    lv_obj_set_style_text_font(s_time_label, &lv_font_montserrat_26, 0);
    lv_obj_align(s_time_label, LV_ALIGN_TOP_MID, 0, UI_VER_RES * 35 / 100);

    /* 小字体日期 —— 紧跟时间下方，间距 8px */
    s_date_label = lv_label_create(s_lock_page);
    lv_label_set_text(s_date_label, "2026-01-01");
    lv_obj_set_style_text_color(s_date_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(s_date_label, &lv_font_montserrat_16, 0);
    lv_obj_align_to(s_date_label, s_time_label, LV_ALIGN_OUT_BOTTOM_MID, 0, 8);

    s_visible = true;
}

/**
 * @brief 刷新锁屏时间和日期
 */
void ui_lockscreen_update(void)
{
    if (!s_lock_page || !s_visible) return;

    ui_device_state_t *st = ui_get_device_state();
    char buf[32];

    snprintf(buf, sizeof(buf), "%02d:%02d", st->hour, st->minute);
    lv_label_set_text(s_time_label, buf);

    snprintf(buf, sizeof(buf), "%04d-%02d-%02d", st->year, st->month, st->day);
    lv_label_set_text(s_date_label, buf);
}

/**
 * @brief 动画结束回调 —— 显示完成后确保对象可见
 */
static void show_anim_ready(lv_anim_t *a)
{
    (void)a;
    s_visible = true;
    /* 锁屏下状态栏不显示时间 */
    ui_statusbar_show_time(false);
}

/**
 * @brief 动画结束回调 —— 隐藏完成后真正隐藏对象
 */
static void hide_anim_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_lock_page, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    /* 解锁后恢复状态栏时间显示 */
    ui_statusbar_show_time(true);
}

/**
 * @brief 动画执行回调 —— 设置锁屏页面 Y 坐标实现上滑效果
 */
static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

/**
 * @brief 显示锁屏（从上方滑入）
 *
 * 动画：从 -UI_VER_RES 滑到 0，渐慢曲线。
 * 修改 UI_ANIM_DURATION_MS 可调节动画快慢。
 */
void ui_lockscreen_show(void)
{
    if (!s_lock_page) return;

    lv_obj_remove_flag(s_lock_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_lock_page);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lock_page);
    lv_anim_set_values(&a, -(int32_t)UI_VER_RES, 0);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_completed_cb(&a, show_anim_ready);
    lv_anim_start(&a);

    s_visible = true;
    ui_statusbar_show_time(false);
}

/**
 * @brief 隐藏锁屏（向上滑出）
 *
 * 动画：从 0 滑到 -UI_VER_RES，渐慢曲线。
 */
void ui_lockscreen_hide(void)
{
    if (!s_lock_page || !s_visible) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_lock_page);
    lv_anim_set_values(&a, 0, -(int32_t)UI_VER_RES);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_completed_cb(&a, hide_anim_ready);
    lv_anim_start(&a);
}

/**
 * @brief 查询锁屏是否可见
 * @return true=可见
 */
bool ui_lockscreen_is_visible(void)
{
    return s_visible;
}
