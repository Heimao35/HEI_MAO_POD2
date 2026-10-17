/**
 * @file ui_settings.c
 * @brief 设置页面实现
 *
 * 全屏覆盖页面，深色背景，内含两列圆形按钮网格。
 * 展开时隐藏状态栏，收起时恢复状态栏。
 */

#include "ui_settings.h"
#include "ui_statusbar.h"

static lv_obj_t *s_page = NULL; /**< 设置页面容器 */
static bool s_visible = false;

/** 设置项信息表 */
static const struct {
    const char *symbol;  /**< 按钮图标符号 */
    const char *label;   /**< 按钮下方文字 */
} s_items[] = {
    { LV_SYMBOL_WIFI,     "WiFi"     },
    { LV_SYMBOL_BLUETOOTH,"Bluetooth"},
    { LV_SYMBOL_GPS,      "Gyro"     },  /* 姿态感应用 GPS/指南针图标代替 */
    { LV_SYMBOL_SETTINGS, "Clock"    },  /* 时钟设置 */
    { LV_SYMBOL_IMAGE,    "Lock Scr" },  /* 锁屏页面设置 */
};
#define ITEM_COUNT (sizeof(s_items) / sizeof(s_items[0]))

/**
 * @brief 设置项按钮点击回调（当前仅打印日志，后续跳转子页面）
 */
static void item_click_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    (void)idx;
    /* TODO: 跳转到对应的详细设置子页面 */
}

/**
 * @brief 创建设置页面
 *
 * 页面全屏，顶部留 50px 放标题，下方为可上下滚动的两列按钮网格。
 * 每个 item 容器宽度 (屏幕宽-80)/2 ≈ 190px，确保严格两列排列。
 * 按钮直径 100px，图标字体 26px，修改此值改变按钮大小。
 *
 * @param parent 父对象
 */
void ui_settings_create(lv_obj_t *parent)
{
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_page, 0, -(int32_t)UI_VER_RES);
    lv_obj_set_style_bg_color(s_page, UI_COLOR_PAGE_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "Settings");
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_22, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    /* 可上下滚动的按钮网格容器 */
    lv_obj_t *grid = lv_obj_create(s_page);
    lv_obj_set_size(grid, UI_HOR_RES - 20, UI_VER_RES - 50);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 10, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 16, 0);
    lv_obj_set_style_pad_column(grid, 16, 0);
    /* 允许上下滚动查看更多设置按钮 */
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 每个 item 容器宽度：严格两列排列 = (grid宽 - padding*2 - 列间距) / 2
     * item 宽度 180px，确保一行只能放两个 */
    const int32_t item_w = 180;
    const int32_t item_h = 140;

    for (size_t i = 0; i < ITEM_COUNT; i++) {
        lv_obj_t *item = lv_obj_create(grid);
        lv_obj_set_size(item, item_w, item_h);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);

        /* 圆形按钮 100x100，增大此值使按钮更大 */
        lv_obj_t *btn = lv_obj_create(item);
        lv_obj_set_size(btn, 100, 100);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, UI_COLOR_CONTROL_BG, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(btn, item_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        /* 图标字体 26px，增大使图标更醒目 */
        lv_obj_t *ico = lv_label_create(btn);
        lv_label_set_text(ico, s_items[i].symbol);
        lv_obj_set_style_text_color(ico, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(ico, &lv_font_montserrat_26, 0);
        lv_obj_center(ico);

        /* 按钮下方文字 */
        lv_obj_t *lbl = lv_label_create(item);
        lv_label_set_text(lbl, s_items[i].label);
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_CAPTION, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_14, 0);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
    }

    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

/** @brief 动画 Y 坐标执行回调 */
static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

/** @brief 展开动画完成回调 */
static void show_ready(lv_anim_t *a)
{
    (void)a;
    s_visible = true;
    ui_statusbar_set_visible(false);
}

/** @brief 收起动画完成回调 */
static void hide_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    ui_statusbar_set_visible(true);
}

/** @brief 展开设置页面（从顶部下滑） */
void ui_settings_show(void)
{
    if (!s_page || s_visible) return;

    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_page);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_page);
    lv_anim_set_values(&a, -(int32_t)UI_VER_RES, 0);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_completed_cb(&a, show_ready);
    lv_anim_start(&a);

    s_visible = true;
    ui_statusbar_set_visible(false);
}

/** @brief 收起设置页面（向顶部上滑） */
void ui_settings_hide(void)
{
    if (!s_page || !s_visible) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_page);
    lv_anim_set_values(&a, 0, -(int32_t)UI_VER_RES);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_completed_cb(&a, hide_ready);
    lv_anim_start(&a);
}

/** @brief 查询设置页面是否展开 */
bool ui_settings_is_visible(void)
{
    return s_visible;
}
