/**
 * @file ui_adj_panel.c
 * @brief 声光调节栏实现（重构版）
 *
 * 结构：
 *   s_overlay  全屏透明遮罩（点击收起）
 *   s_panel    右侧面板，宽 UI_ADJ_PANEL_WIDTH，透明背景
 *     ├─ 左列：亮度图标(EYE_OPEN) + 亮度滑条(56x280) + 自动亮度按钮(字母A)
 *     └─ 右列：音量图标 + 音量滑条(56x280) + 静音按钮
 *
 * 滑条加粗到 56px 宽、280px 高，旋钮隐藏(pad=0,opa=0)使任意位置可拖拽。
 * 两列使用 flex START 对齐，使图标和滑条顶部对齐。
 */

#include "ui_adj_panel.h"
#include <stdio.h>

static lv_obj_t *s_overlay       = NULL;
static lv_obj_t *s_panel         = NULL;
static lv_obj_t *s_bright_slider = NULL;
static lv_obj_t *s_vol_slider    = NULL;
static lv_obj_t *s_auto_btn      = NULL;
static lv_obj_t *s_auto_icon     = NULL;
static lv_obj_t *s_mute_btn      = NULL;  /**< 静音切换按钮 */
static lv_obj_t *s_mute_icon     = NULL;  /**< 静音按钮图标 */
static bool s_visible = false;
static bool s_muted   = false;            /**< 是否静音 */

static ui_brightness_cb_t s_br_cb  = NULL;
static ui_volume_cb_t     s_vol_cb = NULL;

static void overlay_click_cb(lv_event_t *e) { (void)e; ui_adj_panel_hide(); }

static void bright_slider_cb(lv_event_t *e)
{
    (void)e;
    ui_device_state_t *st = ui_get_device_state();
    /* 自动亮度开启时手动拖滑条 → 自动关闭自动亮度 */
    if (st->auto_brightness) {
        st->auto_brightness = false;
        lv_obj_remove_state(s_bright_slider, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_auto_btn, lv_color_hex(0x3A3A5C), 0);
    }
    int32_t val = lv_slider_get_value(s_bright_slider);
    st->brightness = (uint8_t)val;
    if (s_br_cb) s_br_cb((uint8_t)val);
}

static void vol_slider_cb(lv_event_t *e)
{
    (void)e;
    int32_t val = lv_slider_get_value(s_vol_slider);
    /* 静音时手动拖滑条 → 自动解除静音 */
    if (s_muted && val > 0) {
        s_muted = false;
        lv_obj_set_style_bg_color(s_mute_btn, lv_color_hex(0x3A3A5C), 0);
        lv_label_set_text(s_mute_icon, LV_SYMBOL_VOLUME_MID);
    }
    ui_get_device_state()->volume = (uint8_t)val;
    if (s_vol_cb) s_vol_cb((uint8_t)val);
}

/** @brief 自动亮度按钮点击 —— 切换自动/手动，按钮图标为字母 A */
static void auto_bright_cb(lv_event_t *e)
{
    (void)e;
    ui_device_state_t *st = ui_get_device_state();
    st->auto_brightness = !st->auto_brightness;
    if (st->auto_brightness) {
        lv_obj_add_state(s_bright_slider, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_auto_btn, UI_COLOR_ACCENT, 0);
    } else {
        lv_obj_remove_state(s_bright_slider, LV_STATE_DISABLED);
        lv_obj_set_style_bg_color(s_auto_btn, lv_color_hex(0x3A3A5C), 0);
    }
}

/** @brief 静音按钮点击 —— 切换静音模式 */
static void mute_btn_cb(lv_event_t *e)
{
    (void)e;
    s_muted = !s_muted;
    if (s_muted) {
        lv_obj_set_style_bg_color(s_mute_btn, UI_COLOR_ACCENT, 0);
        lv_label_set_text(s_mute_icon, LV_SYMBOL_MUTE);
        /* 静音时将音量设为 0 */
        lv_slider_set_value(s_vol_slider, 0, LV_ANIM_ON);
        ui_get_device_state()->volume = 0;
        if (s_vol_cb) s_vol_cb(0);
    } else {
        lv_obj_set_style_bg_color(s_mute_btn, lv_color_hex(0x3A3A5C), 0);
        lv_label_set_text(s_mute_icon, LV_SYMBOL_VOLUME_MID);
        /* 取消静音恢复到 50 */
        lv_slider_set_value(s_vol_slider, 50, LV_ANIM_ON);
        ui_get_device_state()->volume = 50;
        if (s_vol_cb) s_vol_cb(50);
    }
}

/**
 * @brief 创建纵向滑条（加粗加长版）
 *
 * 滑条宽 56px 高 280px。旋钮隐藏（透明+padding=0），使用户在滑条任意位置
 * 上下滑动即可调节，无需命中小圆球。ext_click_area 扩大触摸区域。
 *
 * @param parent 父容器
 * @param cb     值变更回调
 * @return 创建的 slider 对象
 */
static lv_obj_t *create_vertical_slider(lv_obj_t *parent, lv_event_cb_t cb)
{
    lv_obj_t *slider = lv_slider_create(parent);
    /* 宽 56px 高 280px，增大宽度使滑条更粗，增大高度使滑条更长 */
    lv_obj_set_size(slider, 56, 280);
    lv_slider_set_range(slider, 0, 100);
    lv_slider_set_value(slider, 50, LV_ANIM_OFF);

    lv_obj_set_style_bg_color(slider, UI_COLOR_SLIDER_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(slider, 28, LV_PART_MAIN);

    lv_obj_set_style_bg_color(slider, UI_COLOR_SLIDER_ACTIVE, LV_PART_INDICATOR);
    lv_obj_set_style_radius(slider, 28, LV_PART_INDICATOR);

    /* 隐藏旋钮：透明 + padding=0，用户在滑条任意位置滑动即可调节 */
    lv_obj_set_style_bg_opa(slider, LV_OPA_TRANSP, LV_PART_KNOB);
    lv_obj_set_style_pad_all(slider, 0, LV_PART_KNOB);
    /* 扩大触摸区域，使整个滑条范围都可响应拖拽 */
    lv_obj_set_ext_click_area(slider, 20);

    lv_obj_add_event_cb(slider, cb, LV_EVENT_VALUE_CHANGED, NULL);
    return slider;
}

/**
 * @brief 创建圆形按钮（用于自动亮度和静音）
 * @param parent 父容器
 * @param text   按钮中的文字或符号
 * @param cb     点击回调
 * @param out_icon 输出：按钮内 label 指针（用于后续更新图标）
 * @return 按钮对象
 *
 * 按钮尺寸 50x50，增大此值使按钮更大更易点击
 */
static lv_obj_t *create_round_btn(lv_obj_t *parent, const char *text,
                                   lv_event_cb_t cb, lv_obj_t **out_icon)
{
    lv_obj_t *btn = lv_obj_create(parent);
    lv_obj_set_size(btn, 64, 64);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A5C), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_22, 0);
    lv_obj_center(lbl);
    if (out_icon) *out_icon = lbl;
    return btn;
}

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
    lv_obj_set_pos(s_panel, (int32_t)UI_HOR_RES, 0);
    lv_obj_set_style_bg_opa(s_panel, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_all(s_panel, 0, 0);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);

    /* 两列容器 —— flex START 使图标和滑条顶部对齐 */
    lv_obj_t *cols = lv_obj_create(s_panel);
    lv_obj_set_size(cols, LV_PCT(100), UI_VER_RES);
    lv_obj_center(cols);
    lv_obj_set_style_bg_opa(cols, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(cols, 0, 0);
    lv_obj_set_style_pad_all(cols, 0, 0);
    lv_obj_set_flex_flow(cols, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(cols, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(cols, LV_OBJ_FLAG_SCROLLABLE);

    /* ===== 左列：亮度 ===== */
    lv_obj_t *br_col = lv_obj_create(cols);
    lv_obj_set_size(br_col, 80, UI_VER_RES - 20);
    lv_obj_set_style_bg_opa(br_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(br_col, 0, 0);
    lv_obj_set_style_pad_all(br_col, 0, 0);
    lv_obj_set_style_pad_top(br_col, UI_STATUSBAR_HEIGHT + 10, 0);
    lv_obj_set_flex_flow(br_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(br_col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(br_col, 10, 0);
    lv_obj_remove_flag(br_col, LV_OBJ_FLAG_SCROLLABLE);

    /* 亮度图标 —— EYE_OPEN 代表亮度/可见度（LVGL 无太阳图标） */
    lv_obj_t *br_icon = lv_label_create(br_col);
    lv_label_set_text(br_icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(br_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(br_icon, &lv_font_montserrat_22, 0);

    s_bright_slider = create_vertical_slider(br_col, bright_slider_cb);

    /* 自动亮度按钮 —— 图标为字母 A */
    s_auto_btn = create_round_btn(br_col, "A", auto_bright_cb, &s_auto_icon);

    /* ===== 右列：音量 ===== */
    lv_obj_t *vol_col = lv_obj_create(cols);
    lv_obj_set_size(vol_col, 80, UI_VER_RES - 20);
    lv_obj_set_style_bg_opa(vol_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(vol_col, 0, 0);
    lv_obj_set_style_pad_all(vol_col, 0, 0);
    lv_obj_set_style_pad_top(vol_col, UI_STATUSBAR_HEIGHT + 10, 0);
    lv_obj_set_flex_flow(vol_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(vol_col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(vol_col, 10, 0);
    lv_obj_remove_flag(vol_col, LV_OBJ_FLAG_SCROLLABLE);

    /* 音量图标 */
    lv_obj_t *vol_icon = lv_label_create(vol_col);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(vol_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(vol_icon, &lv_font_montserrat_22, 0);

    s_vol_slider = create_vertical_slider(vol_col, vol_slider_cb);

    /* 静音按钮 —— 与自动亮度按钮样式相同 */
    s_mute_btn = create_round_btn(vol_col, LV_SYMBOL_VOLUME_MID, mute_btn_cb, &s_mute_icon);

    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

static void panel_anim_x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }

static void panel_hide_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

void ui_adj_panel_show(void)
{
    if (!s_panel || s_visible) return;
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    ui_adj_panel_update();

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_values(&a, (int32_t)UI_HOR_RES,
                       (int32_t)(UI_HOR_RES - UI_ADJ_PANEL_WIDTH));
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, panel_anim_x_cb);
    lv_anim_start(&a);
    s_visible = true;
}

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

bool ui_adj_panel_is_visible(void) { return s_visible; }

void ui_adj_panel_update(void)
{
    ui_device_state_t *st = ui_get_device_state();
    if (s_bright_slider) lv_slider_set_value(s_bright_slider, st->brightness, LV_ANIM_OFF);
    if (s_vol_slider)    lv_slider_set_value(s_vol_slider, st->volume, LV_ANIM_OFF);
}

void ui_adj_panel_set_callbacks(ui_brightness_cb_t br_cb, ui_volume_cb_t vol_cb)
{
    s_br_cb = br_cb; s_vol_cb = vol_cb;
}
