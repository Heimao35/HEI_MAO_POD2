/**
 * @file ui_mode_panel.c
 * @brief 模式切换栏实现
 *
 * 结构：
 *   s_overlay  全屏透明遮罩（点击收起面板）
 *   s_panel    左侧面板容器，宽 UI_MODE_PANEL_WIDTH
 *     └─ 纵向排列的圆角矩形按钮，每个代表一个模式
 *
 * 动画：从屏幕左侧外滑入/滑出，渐慢曲线。
 */

#include "ui_mode_panel.h"

static lv_obj_t *s_overlay = NULL; /**< 全屏遮罩，用于点击空白区域收起 */
static lv_obj_t *s_panel   = NULL; /**< 左侧面板容器 */
static bool s_visible = false;
static ui_mode_switch_cb_t s_switch_cb = NULL;

/** 模式信息表：符号 + 模式枚举 */
static const struct {
    const char *symbol;
    ui_mode_t   mode;
} s_modes[] = {
    { LV_SYMBOL_AUDIO,  UI_MODE_MUSIC },  /* 音乐 */
    { LV_SYMBOL_VIDEO,  UI_MODE_VIDEO },  /* 视频 */
    { LV_SYMBOL_IMAGE,  UI_MODE_IMAGE },  /* 图片 */
    { LV_SYMBOL_FILE,   UI_MODE_EBOOK },  /* 电子书 */
};
#define MODE_COUNT (sizeof(s_modes) / sizeof(s_modes[0]))

/**
 * @brief 遮罩点击回调 —— 点击面板外区域收起面板
 */
static void overlay_click_cb(lv_event_t *e)
{
    (void)e;
    ui_mode_panel_hide();
}

/**
 * @brief 模式按钮点击回调 —— 通知 ui_manager 切换模式
 */
static void mode_btn_cb(lv_event_t *e)
{
    ui_mode_t mode = (ui_mode_t)(uintptr_t)lv_event_get_user_data(e);
    if (s_switch_cb) s_switch_cb(mode);
    ui_mode_panel_hide();
}

/**
 * @brief 创建模式切换栏
 *
 * 面板宽度 UI_MODE_PANEL_WIDTH (屏幕宽 1/4)，全屏高度。
 * 按钮尺寸 80x80，圆角 16px，纵向间距 12px。
 * 修改按钮尺寸 / 间距会影响面板内按钮排列密度。
 *
 * @param parent 父对象
 */
void ui_mode_panel_create(lv_obj_t *parent)
{
    /* ---- 全屏遮罩 ---- */
    s_overlay = lv_obj_create(parent);
    lv_obj_set_size(s_overlay, UI_HOR_RES, UI_VER_RES);
    lv_obj_align(s_overlay, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_overlay, lv_color_black(), 0);
    /* 遮罩透明度 80/255，增大使遮罩更暗 */
    lv_obj_set_style_bg_opa(s_overlay, 80, 0);
    lv_obj_set_style_border_width(s_overlay, 0, 0);
    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(s_overlay, overlay_click_cb, LV_EVENT_CLICKED, NULL);
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);

    /* ---- 左侧面板 ---- */
    s_panel = lv_obj_create(parent);
    lv_obj_set_size(s_panel, UI_MODE_PANEL_WIDTH, UI_VER_RES);
    /* 初始位置在屏幕左侧外 */
    lv_obj_set_pos(s_panel, -(int32_t)UI_MODE_PANEL_WIDTH, 0);
    lv_obj_set_style_bg_color(s_panel, UI_PANEL_BG_COLOR, 0);
    lv_obj_set_style_bg_opa(s_panel, UI_PANEL_BG_OPA, 0);
    lv_obj_set_style_border_width(s_panel, 0, 0);
    lv_obj_set_style_radius(s_panel, 0, 0);
    lv_obj_set_style_pad_top(s_panel, UI_STATUSBAR_HEIGHT + 20, 0);
    lv_obj_set_style_pad_bottom(s_panel, 20, 0);
    lv_obj_set_style_pad_left(s_panel, 10, 0);
    lv_obj_set_style_pad_right(s_panel, 10, 0);
    lv_obj_set_flex_flow(s_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_panel, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    /* 按钮纵向间距 12px，增大使按钮排列更稀疏 */
    lv_obj_set_style_pad_row(s_panel, 12, 0);
    /* 允许上下滚动（模式较多时） */
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_panel, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_panel, LV_SCROLLBAR_MODE_OFF);

    /* ---- 创建模式按钮 ---- */
    for (size_t i = 0; i < MODE_COUNT; i++) {
        lv_obj_t *btn = lv_obj_create(s_panel);
        /* 按钮尺寸 80x80，修改此值改变按钮大小 */
        lv_obj_set_size(btn, 80, 80);
        /* 圆角 16px，增大使按钮更圆润 */
        lv_obj_set_style_radius(btn, 16, 0);
        lv_obj_set_style_bg_color(btn, UI_COLOR_CONTROL_BG, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);

        lv_obj_t *lbl = lv_label_create(btn);
        lv_label_set_text(lbl, s_modes[i].symbol);
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
        lv_obj_center(lbl);

        lv_obj_add_event_cb(btn, mode_btn_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)s_modes[i].mode);
    }

    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

/** @brief 面板滑入/滑出动画 X 坐标执行回调 */
static void panel_anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

/** @brief 遮罩透明度动画执行回调 */
static void overlay_anim_opa_cb(void *obj, int32_t v)
{
    lv_obj_set_style_bg_opa((lv_obj_t *)obj, (lv_opa_t)v, 0);
}

/** @brief 遮罩淡出动画完成回调 */
static void overlay_hide_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
}

/** @brief 面板隐藏动画完成回调 */
static void panel_hide_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

/**
 * @brief 展开模式切换栏（从左侧滑入）
 */
void ui_mode_panel_show(void)
{
    if (!s_panel || s_visible) return;

    lv_obj_remove_flag(s_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_style_bg_opa(s_overlay, 0, 0);
    lv_obj_move_foreground(s_overlay);
    lv_obj_remove_flag(s_panel, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_panel);

    /* 遮罩淡入动画（比面板滑动稍长，使渐变更平滑） */
    lv_anim_t a_ov;
    lv_anim_init(&a_ov);
    lv_anim_set_var(&a_ov, s_overlay);
    lv_anim_set_values(&a_ov, 0, 80);
    lv_anim_set_duration(&a_ov, UI_ANIM_DURATION_MS + 100);
    lv_anim_set_path_cb(&a_ov, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a_ov, overlay_anim_opa_cb);
    lv_anim_start(&a_ov);

    /* 面板滑入动画 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_values(&a, -(int32_t)UI_MODE_PANEL_WIDTH, 0);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, panel_anim_x_cb);
    lv_anim_start(&a);

    s_visible = true;
}

/**
 * @brief 收起模式切换栏（向左侧滑出）
 */
void ui_mode_panel_hide(void)
{
    if (!s_panel || !s_visible) return;

    /* 遮罩淡出动画 */
    lv_anim_t a_ov;
    lv_anim_init(&a_ov);
    lv_anim_set_var(&a_ov, s_overlay);
    lv_anim_set_values(&a_ov, 80, 0);
    lv_anim_set_duration(&a_ov, UI_ANIM_DURATION_MS + 100);
    lv_anim_set_path_cb(&a_ov, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a_ov, overlay_anim_opa_cb);
    lv_anim_set_completed_cb(&a_ov, overlay_hide_ready);
    lv_anim_start(&a_ov);

    /* 面板滑出动画 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_panel);
    lv_anim_set_values(&a, 0, -(int32_t)UI_MODE_PANEL_WIDTH);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, panel_anim_x_cb);
    lv_anim_set_completed_cb(&a, panel_hide_ready);
    lv_anim_start(&a);
}

/**
 * @brief 查询面板是否展开
 * @return true=已展开
 */
bool ui_mode_panel_is_visible(void)
{
    return s_visible;
}

/**
 * @brief 注册模式切换回调
 * @param cb 回调函数
 */
void ui_mode_panel_set_callback(ui_mode_switch_cb_t cb)
{
    s_switch_cb = cb;
}
