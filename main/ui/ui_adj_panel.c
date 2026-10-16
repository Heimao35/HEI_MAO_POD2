/**
 * @file ui_adj_panel.c
 * @brief 声光调节栏实现
 *
 * 结构：
 *   s_overlay  全屏透明遮罩（点击收起）
 *   s_panel    右侧面板容器，宽 UI_ADJ_PANEL_WIDTH，透明背景
 *     ├─ 左列：亮度图标 + 亮度滑条 + 自动亮度按钮
 *     └─ 右列：音量图标 + 音量滑条
 */

#include "ui_adj_panel.h"
#include <stdio.h>

static lv_obj_t *s_overlay     = NULL;
static lv_obj_t *s_panel       = NULL;
static lv_obj_t *s_bright_slider = NULL; /**< 亮度滑条 */
static lv_obj_t *s_vol_slider  = NULL;   /**< 音量滑条 */
static lv_obj_t *s_auto_btn    = NULL;   /**< 自动亮度按钮 */
static lv_obj_t *s_auto_icon   = NULL;   /**< 自动亮度按钮中的图标 */
static bool s_visible = false;

static ui_brightness_cb_t s_br_cb  = NULL;
static ui_volume_cb_t     s_vol_cb = NULL;

/** @brief 遮罩点击回调 */
static void overlay_click_cb(lv_event_t *e)
{
    (void)e;
    ui_adj_panel_hide();
}

/** @brief 亮度滑条值变更回调 */
static void bright_slider_cb(lv_event_t *e)
{
    (void)e;
    ui_device_state_t *st = ui_get_device_state();
    if (st->auto_brightness) return; /* 自动模式下忽略手动调节 */

    int32_t val = lv_slider_get_value(s_bright_slider);
    st->brightness = (uint8_t)val;
    if (s_br_cb) s_br_cb((uint8_t)val);
}

/** @brief 音量滑条值变更回调 */
static void vol_slider_cb(lv_event_t *e)
{
    (void)e;
    int32_t val = lv_slider_get_value(s_vol_slider);
    ui_get_device_state()->volume = (uint8_t)val;
    if (s_vol_cb) s_vol_cb((uint8_t)val);
}

/** @brief 自动亮度按钮点击回调 —— 切换自动/手动亮度模式 */
static void auto_bright_cb(lv_event_t *e)
{
    (void)e;
    ui_device_state_t *st = ui_get_device_state();
    st->auto_brightness = !st->auto_brightness;

    if (st->auto_brightness) {
        /* 自动模式：禁用滑条，图标变为 EYE_OPEN 表示"感应中" */
        lv_obj_add_state(s_bright_slider, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_auto_btn, UI_COLOR_ACCENT, 0);
        lv_label_set_text(s_auto_icon, LV_SYMBOL_EYE_OPEN);
    } else {
        /* 手动模式：启用滑条 */
        lv_obj_remove_state(s_bright_slider, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_auto_btn, lv_color_hex(0x3A3A5C), 0);
        lv_label_set_text(s_auto_icon, LV_SYMBOL_EYE_CLOSE);
    }
}

/**
 * @brief 创建一个纵向滑条（用于亮度和音量）
 *
 * 滑条高度 200px、宽度 40px，修改这两个值会影响滑条视觉粗细和长度。
 * 范围 0-100，圆角风格。
 *
 * @param parent 父容器
 * @param cb     值变更回调
 * @return 创建的 slider 对象
 */
static lv_obj_t *create_vertical_slider(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *slider = lv_slider_create(parent);
    /* 滑条宽 40px 高 200px，增大宽度使滑条更粗 */
    lv_obj_set_size(slider, 40, 200);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(slider, UI_COLOR_SLIDER_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 20, LV_PART_MAIN);

    lv_obj_set_style_bg_color(slider, UI_COLOR_SLIDER_ACTIVE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 20, LV_PART_INDICATOR);

    lv_obj_set_style_bg_color(slider, lv_color_white(), LV_PART_KNOB);
    /* 旋钮 padding 4px，增大使旋钮更大 */
    lv_obj_set_style_pad_all(slider, 4, LV_PART_KNOB);

    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return slider;
}

/**
 * @brief 创建声光调节栏
 *
 * 面板宽度 UI_ADJ_PANEL_WIDTH (屏幕宽 1/2)，全屏高度，透明背景。
 * 内部两列：左列亮度，右列音量。
 *
 * @param parent 父对象
 */
void ui_adj_panel_create(lv_obj_t *parent)
{
    /* ---- 全屏遮罩 ---- */
    s_overlay = lv_obj_create(parent);
    lv_obj_set_size(s_overlay, UI_HOR_RES, UI_VER_RES);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_overlay, 80, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    /* ---- 右侧面板 ---- */
    s_panel = lv_obj_create(parent);
    lv_obj_set_size(s_panel, UI_ADJ_PANEL_WIDTH, UI_VER_RES);
    /* 初始位置在屏幕右侧外 */
    lv_obj_set_pos(s_panel, (int32_t)UI_HOR_RES, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 两列容器 —— 水平排列 */
    lv_obj_t *cols = lv_obj_create(s_panel);
    lv_obj_set_size(cols, LV_PCT(100), UI_VER_RES);
    lv_obj_center(cols);
    lv_obj_set_style_bg_opa(cols, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cols, 0, 0);
    lv_obj_set_style_pad_all(cols, 0, 0);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(cols, LV_OBJ_FLAG_SCROLLABLE);

    /* ===== 左列：亮度 ===== */
    lv_obj_t *br_col = lv_obj_create(cols);
    lv_obj_set_size(br_col, 70, UI_VER_RES - 40);
    lv_obj_set_style_bg_opa(br_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(br_col, 0, 0);
    lv_obj_set_style_pad_all(br_col, 0, 0);
    lv_obj_set_flex_flow(br_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(br_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(br_col, 8, 0);
    lv_obj_remove_flag(br_col, LV_OBJ_FLAG_SCROLLABLE);

    /* 亮度图标 —— 使用 TINT(太阳)符号 */
    lv_obj_t *br_icon = lv_label_create(br_col);
    lv_label_set_text(br_icon, LV_SYMBOL_TINT);
    lv_obj_set_style_text_color(br_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(br_icon, &lv_font_montserrat_20, 0);

    /* 亮度滑条 */
    s_bright_slider = create_vertical_slider(br_col, bright_slider_cb);

    /* 自动亮度按钮 —— 圆形 40x40 */
    s_auto_btn = lv_obj_create(br_col);
    lv_obj_set_size(s_auto_btn, 40, 40);
    lv_obj_set_style_radius(s_auto_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_auto_btn, lv_color_hex(0x3A3A5C), 0);
    lv_obj_set_style_bg_opa(s_auto_btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_auto_btn, 0, 0);
    lv_obj_remove_flag(s_auto_btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_auto_btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_auto_btn, auto_bright_cb, LV_EVENT_CLICKED, NULL);

    s_auto_icon = lv_label_create(s_auto_btn);
    lv_label_set_text(s_auto_icon, LV_SYMBOL_EYE_CLOSE);
    lv_obj_set_style_text_color(s_auto_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_auto_icon, &lv_font_montserrat_14, 0);
    lv_obj_center(s_auto_icon);

    /* ===== 右列：音量 ===== */
    lv_obj_t *vol_col = lv_obj_create(cols);
    lv_obj_set_size(vol_col, 70, UI_VER_RES - 40);
    lv_obj_set_style_bg_opa(vol_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_col, 0, 0);
    lv_obj_set_style_pad_all(vol_col, 0, 0);
    lv_obj_set_flex_flow(vol_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(vol_col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(vol_col, 8, 0);
    lv_obj_remove_flag(vol_col, LV_OBJ_FLAG_SCROLLABLE);

    /* 音量图标 */
    lv_obj_t *vol_icon = lv_label_create(vol_col);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(vol_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(vol_icon, &lv_font_montserrat_20, 0);

    /* 音量滑条 */
    s_vol_slider = create_vertical_slider(vol_col, vol_slider_cb);

    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

/** @brief 面板滑入/滑出动画 X 坐标执行回调 */
static void panel_anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

/** @brief 面板隐藏动画完成回调 */
static void panel_hide_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

/**
 * @brief 展开声光调节栏（从右侧滑入）
 */
void ui_adj_panel_show(void)
{
    if (!s_panel || s_visible) return;

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);

    /* 同步滑条到当前设备状态 */
    ui_adj_panel_update();

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    /* 从屏幕右侧外滑入到右对齐位置 */
    lv_anim_set_values(&a, (int32_t)UI_HOR_RES,
                       (int32_t)(UI_HOR_RES - UI_ADJ_PANEL_WIDTH));
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, panel_anim_x_cb);
    lv_anim_start(&a);

    s_visible = true;
}

/**
 * @brief 收起声光调节栏（向右侧滑出）
 */
void ui_adj_panel_hide(void)
{
    if (!s_panel || !s_visible) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_values(&a, (int32_t)(UI_HOR_RES - UI_ADJ_PANEL_WIDTH),
                       (int32_t)UI_HOR_RES);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, panel_anim_x_cb);
    lv_anim_set_completed_cb(&a, panel_hide_ready);
    lv_anim_start(&a);
}

/** @brief 查询是否展开 */
bool ui_adj_panel_is_visible(void)
{
    return s_visible;
}

/**
 * @brief 用当前设备状态刷新滑条位置
 */
void ui_adj_panel_update(void)
{
    ui_device_state_t *st = ui_get_device_state();
    if (s_bright_slider) lv_slider_set_value(s_bright_slider, st->brightness, LV_ANIM_OFF);
    if (s_vol_slider)    lv_slider_set_value(s_vol_slider, st->volume, LV_ANIM_OFF);
}

/**
 * @brief 注册亮度和音量变更回调
 */
void ui_adj_panel_set_callbacks(ui_brightness_cb_t br_cb, ui_volume_cb_t vol_cb)
{
    s_br_cb  = br_cb;
    s_vol_cb = vol_cb;
}
