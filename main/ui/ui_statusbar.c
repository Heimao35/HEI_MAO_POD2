/**
 * @file ui_statusbar.c
 * @brief 状态栏实现 —— 透明背景，显示连接状态、时间、电量
 *
 * 布局：高度 UI_STATUSBAR_HEIGHT，宽度铺满屏幕，背景透明。
 * 左侧 flex 容器放 WiFi/BT/耳机/SD 图标，中间放时间 label，
 * 右侧放电量百分比 label + 电池图标 + 充电闪电。
 */

#include "ui_statusbar.h"
#include <stdio.h>

/* ---- 模块内部对象指针 ---- */
static lv_obj_t *s_bar       = NULL;  /**< 状态栏容器 */
static lv_obj_t *s_icon_wifi = NULL;  /**< WiFi 图标 label */
static lv_obj_t *s_icon_bt   = NULL;  /**< 蓝牙图标 label */
static lv_obj_t *s_icon_hp   = NULL;  /**< 耳机图标 label (用 AUDIO 符号替代) */
static lv_obj_t *s_icon_sd   = NULL;  /**< SD 卡图标 label */
static lv_obj_t *s_time_lbl  = NULL;  /**< 时间 label */
static lv_obj_t *s_batt_pct  = NULL;  /**< 电量百分比 label */
static lv_obj_t *s_batt_icon = NULL;  /**< 电池图标 label */
static lv_obj_t *s_charge    = NULL;  /**< 充电闪电图标 label */

/**
 * @brief 根据电量百分比返回对应的电池符号
 * @param pct 电量百分比 0-100
 * @return LVGL 电池符号字符串
 */
static const char *battery_symbol(uint8_t pct)
{
    if (pct > 87)  return LV_SYMBOL_BATTERY_FULL;
    if (pct > 62)  return LV_SYMBOL_BATTERY_3;
    if (pct > 37)  return LV_SYMBOL_BATTERY_2;
    if (pct > 12)  return LV_SYMBOL_BATTERY_1;
    return LV_SYMBOL_BATTERY_EMPTY;
}

/**
 * @brief 创建状态栏
 *
 * 状态栏分三段：
 *   左侧 flex 行：WiFi / BT / 耳机 / SD 图标，间距 4px
 *   中间：时间 label，居中对齐
 *   右侧 flex 行：电量百分比 + 电池图标 + 充电闪电
 *
 * @param parent 父对象（通常是屏幕根对象）
 */
void ui_statusbar_create(lv_obj_t *parent)
{
    /* ---------- 状态栏容器 ---------- */
    s_bar = lv_obj_create(parent);
    lv_obj_set_size(s_bar, UI_HOR_RES, UI_STATUSBAR_HEIGHT);
    lv_obj_align(s_bar, LV_ALIGN_TOP_MID, 0, 0);
    /* 透明背景，无边框，无滚动 */
    lv_obj_set_style_bg_opa(s_bar, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_bar, 0, 0);
    lv_obj_set_style_pad_all(s_bar, 0, 0);
    lv_obj_set_style_pad_left(s_bar, 6, 0);
    lv_obj_set_style_pad_right(s_bar, 6, 0);
    lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_SCROLLABLE);

    /* ========== 左侧图标区 ========== */
    lv_obj_t *left = lv_obj_create(s_bar);
    lv_obj_set_size(left, LV_SIZE_CONTENT, UI_STATUSBAR_HEIGHT);
    lv_obj_align(left, LV_ALIGN_LEFT_MID, 0, 0);
    lv_obj_set_style_bg_opa(left, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(left, 0, 0);
    lv_obj_set_style_pad_all(left, 0, 0);
    lv_obj_set_flex_flow(left, LV_FLEX_FLOW_ROW);
    /* 图标间距 4px，增大此值会拉开图标间距 */
    lv_obj_set_style_pad_column(left, 4, 0);
    lv_obj_set_flex_align(left, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(left, LV_OBJ_FLAG_SCROLLABLE);

    /* WiFi 图标 —— 使用 LV_SYMBOL_WIFI */
    s_icon_wifi = lv_label_create(left);
    lv_label_set_text(s_icon_wifi, LV_SYMBOL_WIFI);
    lv_obj_set_style_text_color(s_icon_wifi, UI_COLOR_STATUS_TEXT, 0);
    lv_obj_set_style_text_font(s_icon_wifi, &lv_font_montserrat_16, 0);

    /* 蓝牙图标 —— 使用 LV_SYMBOL_BLUETOOTH */
    s_icon_bt = lv_label_create(left);
    lv_label_set_text(s_icon_bt, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_color(s_icon_bt, UI_COLOR_STATUS_TEXT, 0);
    lv_obj_set_style_text_font(s_icon_bt, &lv_font_montserrat_16, 0);

    /* 耳机图标 —— LVGL 无专用耳机符号，用 LV_SYMBOL_AUDIO 代替 */
    s_icon_hp = lv_label_create(left);
    lv_label_set_text(s_icon_hp, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(s_icon_hp, UI_COLOR_STATUS_TEXT, 0);
    lv_obj_set_style_text_font(s_icon_hp, &lv_font_montserrat_16, 0);

    /* SD 卡图标 —— 使用 LV_SYMBOL_SD_CARD */
    s_icon_sd = lv_label_create(left);
    lv_label_set_text(s_icon_sd, LV_SYMBOL_SD_CARD);
    lv_obj_set_style_text_color(s_icon_sd, UI_COLOR_STATUS_TEXT, 0);
    lv_obj_set_style_text_font(s_icon_sd, &lv_font_montserrat_16, 0);

    /* ========== 中间时间 ========== */
    s_time_lbl = lv_label_create(s_bar);
    lv_label_set_text(s_time_lbl, "12:00");
    lv_obj_set_style_text_color(s_time_lbl, UI_COLOR_STATUS_TEXT, 0);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_time_lbl, LV_ALIGN_CENTER, 0, 0);

    /* ========== 右侧电量区 ========== */
    lv_obj_t *right = lv_obj_create(s_bar);
    lv_obj_set_size(right, LV_SIZE_CONTENT, UI_STATUSBAR_HEIGHT);
    lv_obj_align(right, LV_ALIGN_RIGHT_MID, 0, 0);
    lv_obj_set_style_bg_opa(right, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(right, 0, 0);
    lv_obj_set_style_pad_all(right, 0, 0);
    lv_obj_set_flex_flow(right, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(right, 2, 0);
    lv_obj_set_flex_align(right, LV_FLEX_ALIGN_END,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(right, LV_OBJ_FLAG_SCROLLABLE);

    /* 电量百分比文字 */
    s_batt_pct = lv_label_create(right);
    lv_label_set_text(s_batt_pct, "75%");
    lv_obj_set_style_text_color(s_batt_pct, UI_COLOR_STATUS_TEXT, 0);
    lv_obj_set_style_text_font(s_batt_pct, &lv_font_montserrat_14, 0);

    /* 电池图标 */
    s_batt_icon = lv_label_create(right);
    lv_label_set_text(s_batt_icon, LV_SYMBOL_BATTERY_FULL);
    lv_obj_set_style_text_color(s_batt_icon, UI_COLOR_STATUS_TEXT, 0);
    lv_obj_set_style_text_font(s_batt_icon, &lv_font_montserrat_16, 0);

    /* 充电闪电符号 —— 使用 LV_SYMBOL_CHARGE，默认隐藏 */
    s_charge = lv_label_create(right);
    lv_label_set_text(s_charge, LV_SYMBOL_CHARGE);
    lv_obj_set_style_text_color(s_charge, lv_color_hex(0xFFD700), 0);
    lv_obj_set_style_text_font(s_charge, &lv_font_montserrat_12, 0);
    lv_obj_add_flag(s_charge, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 刷新状态栏所有指示符
 *
 * 读取 ui_get_device_state() 的状态，更新图标显隐、时间文字、电量显示。
 * 建议每秒调用一次。
 */
void ui_statusbar_update(void)
{
    ui_device_state_t *st = ui_get_device_state();
    char buf[16];

    /* 左侧图标：已连接/插入则显示，否则隐藏 */
    if (st->wifi_connected)     lv_obj_remove_flag(s_icon_wifi, LV_OBJ_FLAG_HIDDEN);
    else                        lv_obj_add_flag(s_icon_wifi, LV_OBJ_FLAG_HIDDEN);

    if (st->bt_connected)       lv_obj_remove_flag(s_icon_bt, LV_OBJ_FLAG_HIDDEN);
    else                        lv_obj_add_flag(s_icon_bt, LV_OBJ_FLAG_HIDDEN);

    if (st->headphone_inserted) lv_obj_remove_flag(s_icon_hp, LV_OBJ_FLAG_HIDDEN);
    else                        lv_obj_add_flag(s_icon_hp, LV_OBJ_FLAG_HIDDEN);

    if (st->sd_card_inserted)   lv_obj_remove_flag(s_icon_sd, LV_OBJ_FLAG_HIDDEN);
    else                        lv_obj_add_flag(s_icon_sd, LV_OBJ_FLAG_HIDDEN);

    /* 中间时间 (24小时制 HH:MM) */
    snprintf(buf, sizeof(buf), "%02d:%02d", st->hour, st->minute);
    lv_label_set_text(s_time_lbl, buf);

    /* 右侧电量 */
    snprintf(buf, sizeof(buf), "%d%%", st->battery_pct);
    lv_label_set_text(s_batt_pct, buf);
    lv_label_set_text(s_batt_icon, battery_symbol(st->battery_pct));

    /* 充电闪电 */
    if (st->charging) lv_obj_remove_flag(s_charge, LV_OBJ_FLAG_HIDDEN);
    else              lv_obj_add_flag(s_charge, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 设置状态栏可见性
 * @param visible true=显示, false=隐藏
 */
void ui_statusbar_set_visible(bool visible)
{
    if (!s_bar) return;
    if (visible) lv_obj_remove_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
    else         lv_obj_add_flag(s_bar, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 控制状态栏中间的时间显示
 * @param show true=显示时间, false=隐藏时间（锁屏页面使用）
 */
void ui_statusbar_show_time(bool show)
{
    if (!s_time_lbl) return;
    if (show) lv_obj_remove_flag(s_time_lbl, LV_OBJ_FLAG_HIDDEN);
    else      lv_obj_add_flag(s_time_lbl, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 获取状态栏容器对象
 * @return lv_obj_t* 状态栏对象指针
 */
lv_obj_t *ui_statusbar_get_obj(void)
{
    return s_bar;
}
