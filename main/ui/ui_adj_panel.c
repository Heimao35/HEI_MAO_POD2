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
#include "ui_lockscreen.h"
#include <stdio.h>
#include "ui_fonts.h"

static lv_obj_t *s_overlay       = NULL;
static lv_obj_t *s_panel         = NULL;
static lv_obj_t *s_bright_slider = NULL;
static lv_obj_t *s_vol_slider    = NULL;
static lv_obj_t *s_auto_btn      = NULL;
static lv_obj_t *s_auto_icon     = NULL;
static lv_obj_t *s_mute_btn      = NULL;  /**< 静音切换按钮 */
static lv_obj_t *s_mute_icon     = NULL;  /**< 静音按钮图标 */
/* 两种展开来源共用现有滑条和滑入曲线。手势模式仍显示亮度+音量并使用
 * 原遮罩；实体键模式只显示音量，透明容器不参与命中，空白处可直接操作
 * 下层页面。所有对象、计时和状态均只在 LVGL 线程/锁内访问。 */
static lv_obj_t *s_bright_col;
static lv_timer_t *s_volume_timer;
static bool s_volume_only;
static bool s_closing;
static bool s_volume_touch;
static uint32_t s_volume_activity;
#define VOLUME_IDLE_MS 5000U
#define VOLUME_PANEL_WIDTH (UI_ADJ_PANEL_WIDTH / 2)
static bool s_visible = false;
static bool s_muted   = false;            /**< 是否静音 */

static ui_brightness_cb_t s_br_cb  = NULL;
static ui_volume_cb_t     s_vol_cb = NULL;
static ui_auto_brightness_cb_t s_auto_cb = NULL;

/* 白色选中态必须反转图标颜色；所有点击和拖动取消路径统一配对恢复。
 * 此函数只设颜色，不修改自动亮度、静音、滑条数值或回调执行顺序。 */
static void set_toggle_colors(lv_obj_t *btn, lv_obj_t *icon, bool active)
{
    lv_obj_set_style_bg_color(btn, active ? UI_COLOR_ACCENT : UI_COLOR_CONTROL_BG, 0);
    lv_obj_set_style_text_color(icon, active ? UI_COLOR_ON_ACCENT : UI_COLOR_TEXT, 0);
}

static void volume_activity(void)
{
    s_volume_activity = lv_tick_get();
}

/* 即便拖动到上下限、不再产生 VALUE_CHANGED，持续按住也不能超时收起。
 * RELEASED/PRESS_LOST 后重新计五秒；触摸事件不冒泡到页面手势处理。 */
static void volume_touch_cb(lv_event_t *e)
{
    lv_event_code_t code = lv_event_get_code(e);
    if (code == LV_EVENT_PRESSED || code == LV_EVENT_PRESSING) s_volume_touch = true;
    else if (code == LV_EVENT_RELEASED || code == LV_EVENT_PRESS_LOST) s_volume_touch = false;
    else return;
    volume_activity();
}

static void volume_timeout_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_volume_only && s_visible && !s_closing && !s_volume_touch &&
        lv_tick_elaps(s_volume_activity) >= VOLUME_IDLE_MS) ui_adj_panel_hide();
}

static void overlay_click_cb(lv_event_t *e) { (void)e; ui_adj_panel_hide(); }

static void bright_slider_cb(lv_event_t *e)
{
    (void)e;
    ui_device_state_t *st = ui_get_device_state();
    /* 自动亮度开启时手动拖滑条 → 自动关闭自动亮度 */
    if (st->auto_brightness) {
        st->auto_brightness = false;
        /* 手动拖动立即撤销显示任务中的自动模式，无需等滑条重新绘制。 */
        if (s_auto_cb) s_auto_cb(false);
        set_toggle_colors(s_auto_btn, s_auto_icon, false);
    }
    int32_t val = lv_slider_get_value(s_bright_slider);
    st->brightness = (uint8_t)val;
    /* 手动拖动仍退出自动模式；单独记住手动偏好，光感调整不会覆盖它。
     * 不改变原滑条布局/事件传播/手势，硬件亮度继续由显示电源任务串行写入。 */
    st->manual_brightness = (uint8_t)val;
    if (s_br_cb) s_br_cb((uint8_t)val);
}

/* 音量状态和后端请求的共同入口；调用方持有 LVGL 锁。
 * 与展开面板/记录触摸活动分开，使熄屏播放时按键仍可调音量，且不会
 * 启动动画、延长屏幕或调节栏活动时间。静音解除策略与原滑条一致。 */
static void apply_volume_value(uint8_t val)
{
    /* 静音时手动拖滑条 → 自动解除静音 */
    if (s_muted && val > 0) {
        s_muted = false;
        ui_get_device_state()->muted = false;
        set_toggle_colors(s_mute_btn, s_mute_icon, false);
        lv_label_set_text(s_mute_icon, LV_SYMBOL_VOLUME_MID);
    }
    ui_get_device_state()->volume = (uint8_t)val;
    if (s_vol_cb) s_vol_cb((uint8_t)val);
}

static void vol_slider_cb(lv_event_t *e)
{
    (void)e;
    volume_activity();
    apply_volume_value((uint8_t)lv_slider_get_value(s_vol_slider));
}

/** @brief 自动亮度按钮点击 —— 切换自动/手动，按钮图标为字母 A */
static void auto_bright_cb(lv_event_t *e)
{
    (void)e;
    ui_device_state_t *st = ui_get_device_state();
    st->auto_brightness = !st->auto_brightness;
    /* 硬件渐变与UI绘制解耦后，开关必须主动通知后端，而不是由后端拿
     * LVGL锁轮询。仅投递布尔值，不改变布局/手势，也不在此读取ADC。 */
    if (s_auto_cb) s_auto_cb(st->auto_brightness);
    if (st->auto_brightness) {
        set_toggle_colors(s_auto_btn, s_auto_icon, true);
    } else {
        /* 用户明确关闭自动模式时，以当前可见亮度作为新的手动偏好。
         * 这只在点击开关时发生；光照引起的连续调节不会反复写NVS。 */
        st->manual_brightness = st->brightness;
        set_toggle_colors(s_auto_btn, s_auto_icon, false);
    }
}

/** @brief 静音按钮点击 —— 切换静音模式 */
static void mute_btn_cb(lv_event_t *e)
{
    (void)e;
    volume_activity();
    s_muted = !s_muted;
    ui_get_device_state()->muted = s_muted;
    if (s_muted) {
        set_toggle_colors(s_mute_btn, s_mute_icon, true);
        lv_label_set_text(s_mute_icon, LV_SYMBOL_MUTE);
        /* 静音时将音量设为 0 */
        lv_slider_set_value(s_vol_slider, 0, LV_ANIM_ON);
        ui_get_device_state()->volume = 0;
        if (s_vol_cb) s_vol_cb(0);
    } else {
        set_toggle_colors(s_mute_btn, s_mute_icon, false);
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
    lv_obj_set_style_bg_color(btn, UI_COLOR_CONTROL_BG, 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, text);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lbl, ui_font_22(), 0);
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
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

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
    lv_obj_remove_flag(cols, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* ===== 左列：亮度 ===== */
    lv_obj_t *br_col = lv_obj_create(cols);
    s_bright_col = br_col;
    lv_obj_set_size(br_col, 80, UI_VER_RES - 20);
    lv_obj_set_style_bg_opa(br_col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(br_col, 0, 0);
    lv_obj_set_style_pad_all(br_col, 0, 0);
    lv_obj_set_style_pad_top(br_col, UI_STATUSBAR_HEIGHT + 10, 0);
    lv_obj_set_flex_flow(br_col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(br_col, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(br_col, 10, 0);
    lv_obj_remove_flag(br_col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 亮度图标 —— EYE_OPEN 代表亮度/可见度（LVGL 无太阳图标） */
    lv_obj_t *br_icon = lv_label_create(br_col);
    lv_label_set_text(br_icon, LV_SYMBOL_EYE_OPEN);
    lv_obj_set_style_text_color(br_icon, UI_COLOR_TEXT, 0);
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
    lv_obj_remove_flag(vol_col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    /* 音量图标 */
    lv_obj_t *vol_icon = lv_label_create(vol_col);
    lv_label_set_text(vol_icon, LV_SYMBOL_VOLUME_MAX);
    lv_obj_set_style_text_color(vol_icon, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(vol_icon, &lv_font_montserrat_22, 0);

    s_vol_slider = create_vertical_slider(vol_col, vol_slider_cb);
    lv_obj_add_event_cb(s_vol_slider, volume_touch_cb, LV_EVENT_ALL, NULL);

    /* 静音按钮 —— 与自动亮度按钮样式相同 */
    s_mute_btn = create_round_btn(vol_col, LV_SYMBOL_VOLUME_MID, mute_btn_cb, &s_mute_icon);
    lv_obj_add_event_cb(s_mute_btn, volume_touch_cb, LV_EVENT_ALL, NULL);
    s_volume_timer = lv_timer_create(volume_timeout_cb, 100, NULL);
    lv_timer_pause(s_volume_timer);

    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

static void panel_anim_x_cb(void *obj, int32_t v) { lv_obj_set_x((lv_obj_t *)obj, v); }

static void overlay_anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

static void overlay_hide_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

static void panel_hide_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    s_closing = false;
    s_volume_touch = false;
}

/* 从当前 X 开始滑动，连续按键只刷新数值/超时，不重复播放入场动画。
 * 显示来源改变或收起中再次按键时，取消旧完成回调，避免旧回调隐藏新面板。 */
static void animate_panel(int32_t target, bool hide)
{
    lv_anim_delete(s_panel, panel_anim_x_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_values(&a, lv_obj_get_x(s_panel), target);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, panel_anim_x_cb);
    if (hide) lv_anim_set_completed_cb(&a, panel_hide_ready);
    lv_anim_start(&a);
}

static void show_panel(bool volume_only)
{
    if (!s_panel) return;
    if (s_visible && !s_closing && s_volume_only == volume_only) return;
    bool on_lockscreen = ui_lockscreen_is_visible();
    s_volume_only = volume_only;
    s_closing = false;
    lv_anim_delete(s_overlay, overlay_anim_opa_cb);

    if (volume_only) {
        /* 按键展开完全不显示遮罩，也不启动其渐变；仅控件本身接收触摸。
         * 隐藏亮度列后缩成单列，避免保留一个不可见的半屏触摸占位。 */
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_bright_col, LV_OBJ_FLAG_HIDDEN);
        lv_timer_resume(s_volume_timer);
        lv_timer_reset(s_volume_timer);
        volume_activity();
    } else {
        lv_timer_pause(s_volume_timer);
        lv_obj_remove_flag(s_bright_col, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_overlay);
        lv_obj_set_style_bg_opa(s_overlay, LV_OPA_TRANSP, 0);
        if (!on_lockscreen) {
            lv_anim_t a_ov;
            lv_anim_init(&a_ov);
            lv_anim_set_var(&a_ov, s_overlay);
            lv_anim_set_values(&a_ov, 0, 80);
            lv_anim_set_duration(&a_ov, UI_ANIM_DURATION_MS + 100);
            lv_anim_set_path_cb(&a_ov, lv_anim_path_linear);
            lv_anim_set_exec_cb(&a_ov, overlay_anim_opa_cb);
            lv_anim_start(&a_ov);
        }
    }
    int32_t width = volume_only ? VOLUME_PANEL_WIDTH : UI_ADJ_PANEL_WIDTH;
    lv_obj_set_width(s_panel, width);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);
    ui_adj_panel_update();
    animate_panel((int32_t)UI_HOR_RES - width, false);
    s_visible = true;
}

void ui_adj_panel_show(void) { show_panel(false); }

/* 两种按键路径共用同一滑条值、上下限、静音策略和音量回调。
 * show_feedback=false 供熄屏播放使用：仅在暂停的 LVGL 对象上同步数值，
 * 方便唤醒后正确显示；不展开/移动面板，不恢复刷新，不触发活动时间。
 * 亮屏路径保持原有单列滑入、无遮罩、五秒自动收起和活动提示行为。 */
static void adjust_volume(int delta, bool show_feedback)
{
    if (!s_panel) return;
    int value = ui_get_device_state()->volume + delta;
    if (value < 0) value = 0;
    if (value > 100) value = 100;
    if (show_feedback) {
        show_panel(true);
        volume_activity();
    }
    lv_slider_set_value(s_vol_slider, value, LV_ANIM_OFF);
    apply_volume_value((uint8_t)value);
    if (show_feedback) lv_display_trigger_activity(lv_display_get_default());
}

void ui_adj_panel_adjust_volume(int delta) { adjust_volume(delta, true); }
void ui_adj_panel_adjust_volume_silent(int delta) { adjust_volume(delta, false); }

void ui_adj_panel_hide(void)
{
    if (!s_panel || !s_visible || s_closing) return;
    s_closing = true;
    lv_timer_pause(s_volume_timer);
    lv_anim_delete(s_overlay, overlay_anim_opa_cb);
    if (s_volume_only || ui_lockscreen_is_visible()) {
        lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_anim_t a_ov;
        lv_anim_init(&a_ov);
        lv_anim_set_var(&a_ov, s_overlay);
        lv_anim_set_values(&a_ov, lv_obj_get_style_bg_opa(s_overlay, 0), 0);
        lv_anim_set_duration(&a_ov, UI_ANIM_DURATION_MS + 100);
        lv_anim_set_path_cb(&a_ov, lv_anim_path_linear);
        lv_anim_set_exec_cb(&a_ov, overlay_anim_opa_cb);
        lv_anim_set_completed_cb(&a_ov, overlay_hide_ready);
        lv_anim_start(&a_ov);
    }
    animate_panel((int32_t)UI_HOR_RES, true);
}

bool ui_adj_panel_is_visible(void) { return s_visible; }
bool ui_adj_panel_is_modal(void) { return s_visible && !s_volume_only; }

void ui_adj_panel_update(void)
{
    ui_device_state_t *st = ui_get_device_state();
    /* 开机从 NVS 恢复后同时刷新滑条和按钮，避免逻辑已静音但图标未选中。
     * 此处只渲染，不发送回调，不触发持久化写入或自动亮度切换。 */
    s_muted = st->muted;
    if (s_auto_btn) set_toggle_colors(s_auto_btn, s_auto_icon, st->auto_brightness);
    if (s_mute_btn) {
        set_toggle_colors(s_mute_btn, s_mute_icon, s_muted);
        lv_label_set_text(s_mute_icon, s_muted ? LV_SYMBOL_MUTE : LV_SYMBOL_VOLUME_MID);
    }
    if (s_bright_slider) lv_slider_set_value(s_bright_slider, st->brightness, LV_ANIM_OFF);
    if (s_vol_slider)    lv_slider_set_value(s_vol_slider, st->volume, LV_ANIM_OFF);
}

/* 光感发布只同步亮度滑条，必须持有LVGL锁。set_value不发送VALUE_CHANGED，
 * 因而不会触发手动回调、关闭自动亮度，也不会影响正在操作的音量/静音控件。
 * 关闭自动模式时保留当前亮度；下次拖动再记录新的手动偏好。 */
void ui_adj_panel_update_brightness(void)
{
    if (s_bright_slider)
        lv_slider_set_value(s_bright_slider, ui_get_device_state()->brightness, LV_ANIM_OFF);
}

void ui_adj_panel_set_auto_brightness_callback(ui_auto_brightness_cb_t callback)
{
    s_auto_cb = callback;
}

void ui_adj_panel_set_callbacks(ui_brightness_cb_t br_cb, ui_volume_cb_t vol_cb)
{
    s_br_cb = br_cb; s_vol_cb = vol_cb;
}
