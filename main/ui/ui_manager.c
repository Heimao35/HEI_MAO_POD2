/**
 * @file ui_manager.c
 * @brief UI 管理器实现 —— 页面切换、边缘手势检测、定时刷新
 *
 * 层级关系（从底到顶）：
 *   1. 模式页面（音乐/视频/图片/电子书）—— 同级互斥，同一时间只显示一个
 *   2. 锁屏页面 —— 覆盖模式页面
 *   3. 模式切换栏（左侧面板）/ 声光调节栏（右侧面板）
 *   4. 设置页面 —— 全屏覆盖
 *   5. 状态栏 —— 始终最顶层（设置页面展开时隐藏）
 *
 * 手势检测通过在主屏幕上注册 LVGL 输入事件回调实现。
 */

#include "ui_manager.h"
#include "ui_power_dialog.h"
#include "ui_statusbar.h"
#include "ui_lockscreen.h"
#include "ui_music_player.h"
#include "ui_video_player.h"
#include "ui_image_viewer.h"
#include "ui_ebook.h"
#include "ui_mode_panel.h"
#include "ui_adj_panel.h"
#include "ui_settings.h"
#include "ui_clock_settings.h"
#include "ui_motion_settings.h"
#include "esp_log.h"

static const char *TAG = "ui_manager";

/* ---- 当前模式 ---- */
static ui_mode_t s_current_mode = UI_MODE_MUSIC;

/* ---- 各模式页面指针数组，按 ui_mode_t 索引 ---- */
static lv_obj_t *s_mode_pages[UI_MODE_MAX] = {NULL};

/* ---- 手势检测状态 ---- */
static lv_point_t s_press_point;   /**< 按下时的坐标 */
static bool s_gesture_detected;    /**< 本次触摸是否已识别手势 */

/** 手势滑动最小距离 (像素)，低于此值不触发手势 */
#define GESTURE_MIN_DIST  40

/* ---- 页面切换动画辅助 ---- */

/** @brief 页面滑出动画完成后隐藏页面 */
static void page_hide_ready(lv_anim_t *a)
{
    lv_obj_t *obj = (lv_obj_t *)a->var;
    lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

/** @brief 页面 Y 坐标动画执行回调（模式切换用上下滑动） */
static void page_anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

/**
 * @brief 模式切换回调 —— 由模式切换栏按钮触发
 * @param mode 目标模式
 */
static void on_mode_switch(ui_mode_t mode)
{
    ui_manager_switch_mode(mode);
}

/**
 * @brief 手势处理：按下事件 —— 记录起始坐标
 */
static void gesture_pressed_cb(lv_event_t *e)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_indev_get_point(indev, &s_press_point);
    s_gesture_detected = false;
}

/**
 * @brief 手势处理：释放事件 —— 判断手势方向并触发动作
 *
 * 手势规则：
 *   - 起点在状态栏区域且向下滑 → 展开设置页面
 *   - 起点在左边缘且向右滑 → 展开模式切换栏
 *   - 起点在右边缘且向左滑 → 展开声光调节栏
 *   - 锁屏页面下向上滑 → 解锁
 *   - 音乐页面下向上滑 → 展开歌曲列表
 *   - 设置页面下从底部上滑 → 收起设置页面
 */
static void gesture_released_cb(lv_event_t *e)
{
    (void)e;
    /* 关机弹窗是唯一新增模态层；取消后恢复原手势判定和页面动画。 */
    if (ui_power_dialog_is_visible()) return;
    /* 子页滚轮/开关独占触摸，不让边缘/底部滑动穿透到原设置页或播放器。
     * 姿态感应子页没有滚轮，但同样必须独占，否则在返回动画途中从底部
     * 上滑会把下层的设置主页收起，留下一个悬空的子页。 */
    if (ui_clock_settings_is_visible() || ui_motion_settings_is_visible()) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;

    lv_point_t rel_point;
    lv_indev_get_point(indev, &rel_point);

    int32_t dx = rel_point.x - s_press_point.x;
    int32_t dy = rel_point.y - s_press_point.y;
    int32_t abs_dx = dx > 0 ? dx : -dx;
    int32_t abs_dy = dy > 0 ? dy : -dy;

    /* 滑动距离不够则忽略 */
    if (abs_dx < GESTURE_MIN_DIST && abs_dy < GESTURE_MIN_DIST) return;

    bool is_horizontal = abs_dx > abs_dy;

    /* ---- 设置页面展开时：从底部边缘上滑收起（不能用任意位置上滑，否则无法滚动页面） ---- */
    if (ui_settings_is_visible()) {
        if (!is_horizontal && dy < -GESTURE_MIN_DIST &&
            s_press_point.y > (int32_t)(UI_VER_RES - UI_EDGE_THRESHOLD)) {
            ui_settings_hide();
        }
        return;
    }

    /* ---- 锁屏页面下：上滑解锁，右边缘左滑开声光调节栏 ---- */
    if (ui_lockscreen_is_visible()) {
        if (ui_adj_panel_is_modal()) {
            if (is_horizontal && dx > GESTURE_MIN_DIST) {
                ui_adj_panel_hide();
            }
            return;
        }
        if (!is_horizontal && dy < -GESTURE_MIN_DIST) {
            ui_lockscreen_hide();
            return;
        }
        if (is_horizontal && dx < -GESTURE_MIN_DIST &&
            s_press_point.x > (int32_t)(UI_HOR_RES - UI_EDGE_THRESHOLD)) {
            ui_adj_panel_show();
            return;
        }
        return;
    }

    /* ---- 歌曲列表展开时：上边缘下滑收起，边缘手势仍可展开面板 ---- */
    if (s_current_mode == UI_MODE_MUSIC && ui_music_player_is_playlist_visible()) {
        /* 从上边缘下滑 → 收起歌曲列表 */
        if (!is_horizontal && dy > GESTURE_MIN_DIST &&
            s_press_point.y < (int32_t)UI_EDGE_THRESHOLD) {
            ui_music_player_toggle_playlist(false);
            return;
        }
        /* 左边缘右滑 → 展开模式切换栏 */
        if (is_horizontal && dx > GESTURE_MIN_DIST &&
            s_press_point.x < (int32_t)UI_EDGE_THRESHOLD) {
            ui_mode_panel_show();
            return;
        }
        /* 右边缘左滑 → 展开声光调节栏 */
        if (is_horizontal && dx < -GESTURE_MIN_DIST &&
            s_press_point.x > (int32_t)(UI_HOR_RES - UI_EDGE_THRESHOLD)) {
            ui_adj_panel_show();
            return;
        }
        return; /* 歌曲列表展开时不处理其他手势 */
    }

    /* 完整声光栏保持原手势互斥；按键音量栏不拦截锁屏解锁、模式切换、
     * 设置下拉或播放器手势。实际滑条触摸由控件接收，不冒泡到这里。 */
    if (ui_mode_panel_is_visible() || ui_adj_panel_is_modal()) return;

    /* ---- 从状态栏区域下滑 → 展开设置页面 ---- */
    if (!is_horizontal && dy > GESTURE_MIN_DIST &&
        s_press_point.y < (int32_t)UI_STATUSBAR_HEIGHT) {
        ui_settings_show();
        return;
    }

    /* ---- 左边缘右滑 → 展开模式切换栏 ---- */
    if (is_horizontal && dx > GESTURE_MIN_DIST &&
        s_press_point.x < (int32_t)UI_EDGE_THRESHOLD) {
        ui_mode_panel_show();
        return;
    }

    /* ---- 右边缘左滑 → 展开声光调节栏 ---- */
    if (is_horizontal && dx < -GESTURE_MIN_DIST &&
        s_press_point.x > (int32_t)(UI_HOR_RES - UI_EDGE_THRESHOLD)) {
        ui_adj_panel_show();
        return;
    }

    /* ---- 音乐播放器下的手势 ---- */
    /*
     * 歌词区域内的竖向拖动由 LVGL 原生滚动接管，不展开歌曲列表。
     * 按手势起点判断，避免手指滑出视口后突然展开列表；横向手势
     * 仍保留原有的上一首/下一首操作，其他区域展开/收起规则不变。
     */
    if (s_current_mode == UI_MODE_MUSIC &&
        ui_music_player_is_lyric_point(s_press_point.x, s_press_point.y) && !is_horizontal) {
        return;
    }

    if (s_current_mode == UI_MODE_MUSIC) {
        /* 非边缘上滑 → 展开歌曲列表 */
        if (!is_horizontal && dy < -GESTURE_MIN_DIST &&
            s_press_point.x > (int32_t)UI_EDGE_THRESHOLD &&
            s_press_point.x < (int32_t)(UI_HOR_RES - UI_EDGE_THRESHOLD)) {
            ui_music_player_toggle_playlist(true);
            return;
        }
        /* 非边缘左滑 → 下一首（封面向左滑出） */
        if (is_horizontal && dx < -GESTURE_MIN_DIST &&
            s_press_point.x > (int32_t)UI_EDGE_THRESHOLD &&
            s_press_point.x < (int32_t)(UI_HOR_RES - UI_EDGE_THRESHOLD)) {
            ui_music_player_next_animated();
            return;
        }
        /* 非边缘右滑 → 上一首（封面向右滑出） */
        if (is_horizontal && dx > GESTURE_MIN_DIST &&
            s_press_point.x > (int32_t)UI_EDGE_THRESHOLD &&
            s_press_point.x < (int32_t)(UI_HOR_RES - UI_EDGE_THRESHOLD)) {
            ui_music_player_prev_animated();
            return;
        }
    }
}

/**
 * @brief LVGL 定时器回调 —— 每秒刷新状态栏和锁屏时间
 */
static void periodic_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    ui_clock_settings_update(); /* RAM 快照，无 I2C；先发布再绘制真实时间。 */
    ui_statusbar_update();
    ui_lockscreen_update();
}

/**
 * @brief 初始化整个 UI 系统
 *
 * 创建顺序决定了 z-order（后创建的在上层）：
 *   1. 各模式页面（只显示当前模式，其余隐藏）
 *   2. 锁屏页面（默认可见，覆盖模式页面）
 *   3. 模式切换栏、声光调节栏（默认隐藏）
 *   4. 设置页面（默认隐藏）
 *   5. 状态栏（始终最顶层）
 *   6. 注册手势检测和定时刷新
 */
void ui_manager_init(void)
{
    lv_obj_t *scr = lv_screen_active();

#if LV_USE_THEME_DEFAULT
    /* 沿用当前显示器、字体及原主题明暗配置，仅替换主/辅颜色。
     * 默认主题也必须同步，否则未显式设色的旋钮/焦点框仍会继承蓝绿色。
     * 不替换成另一套主题，避免引入新的尺寸、间距或状态动画。 */
    lv_display_t *display = lv_obj_get_display(scr);
    lv_theme_t *theme = lv_theme_default_init(display, UI_COLOR_ACCENT,
                                              UI_COLOR_TEXT_SECONDARY,
                                              LV_THEME_DEFAULT_DARK,
                                              lv_theme_get_font_normal(scr));
    lv_display_set_theme(display, theme);
#endif

    /* 主屏幕样式：黑色背景，无滚动 */
    lv_obj_set_style_bg_color(scr, lv_color_black(), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    /* ---- 1. 创建各模式页面 ---- */
    ui_music_player_create(scr);
    s_mode_pages[UI_MODE_MUSIC] = ui_music_player_get_page();

    ui_video_player_create(scr);
    s_mode_pages[UI_MODE_VIDEO] = ui_video_player_get_page();

    ui_image_viewer_create(scr);
    s_mode_pages[UI_MODE_IMAGE] = ui_image_viewer_get_page();

    ui_ebook_create(scr);
    s_mode_pages[UI_MODE_EBOOK] = ui_ebook_get_page();

    /* 默认只显示音乐页面，其余隐藏 */
    for (int i = 0; i < UI_MODE_MAX; i++) {
        if (i != (int)s_current_mode && s_mode_pages[i]) {
            lv_obj_add_flag(s_mode_pages[i], LV_OBJ_FLAG_HIDDEN);
        }
    }

    /* ---- 2. 锁屏页面（默认可见） ---- */
    ui_lockscreen_create(scr);

    /* ---- 3. 面板 ---- */
    ui_mode_panel_create(scr);
    ui_mode_panel_set_callback(on_mode_switch);

    ui_adj_panel_create(scr);

    /* ---- 4. 设置页面 ---- */
    ui_settings_create(scr);
    ui_clock_settings_create(scr);
    /* 姿态感应设置页与时钟设置页同级，均为覆盖在设置主页上的全屏子页。
     * 创建顺序在状态栏之前，保持原有 z-order；显示时再 move_foreground，
     * 不改变各模式页面、锁屏、面板和状态栏的既有顺序。 */
    ui_motion_settings_create(scr);

    /* ---- 5. 状态栏（最顶层） ---- */
    ui_statusbar_create(scr);
    /* 锁屏默认不显示时间 */
    ui_statusbar_show_time(false);

    /* ---- 6. 手势检测 —— 在主屏幕上捕获按下和释放事件 ---- */
    lv_obj_add_event_cb(scr, gesture_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, gesture_released_cb, LV_EVENT_RELEASED, NULL);

    /* ---- 7. 定时刷新（每 1000ms 刷新状态栏和锁屏时间） ---- */
    lv_timer_create(periodic_timer_cb, 1000, NULL);

    ESP_LOGI(TAG, "UI manager initialized");
}

/**
 * @brief 切换到指定模式页面
 *
 * 当前页面向左滑出 + 目标页面从右滑入（如果目标在当前右边）
 * 或当前页面向右滑出 + 目标页面从左滑入（如果目标在当前左边）。
 * 动画只播放一次，不经过中间页面。
 *
 * @param mode 目标模式
 */
void ui_manager_switch_mode(ui_mode_t mode)
{
    if (mode == s_current_mode || mode >= UI_MODE_MAX) return;

    lv_obj_t *old_page = s_mode_pages[s_current_mode];
    lv_obj_t *new_page = s_mode_pages[mode];
    if (!old_page || !new_page) return;

    /* 判断滑动方向：目标模式序号大于当前 → 向上滑出（新页面从下方滑入） */
    bool slide_up = (mode > s_current_mode);
    int32_t out_target = slide_up ? -(int32_t)UI_VER_RES : (int32_t)UI_VER_RES;
    int32_t in_start   = slide_up ? (int32_t)UI_VER_RES : -(int32_t)UI_VER_RES;

    /* 旧页面滑出动画 */
    lv_anim_t a_out;
    lv_anim_init(&a_out);
    lv_anim_set_var(&a_out, old_page);
    lv_anim_set_values(&a_out, 0, out_target);
    lv_anim_set_duration(&a_out, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a_out, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a_out, page_anim_y_cb);
    lv_anim_set_completed_cb(&a_out, page_hide_ready);
    lv_anim_start(&a_out);

    /* 新页面滑入动画 */
    lv_obj_remove_flag(new_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_y(new_page, in_start);

    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, new_page);
    lv_anim_set_values(&a_in, in_start, 0);
    lv_anim_set_duration(&a_in, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a_in, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a_in, page_anim_y_cb);
    lv_anim_start(&a_in);

    s_current_mode = mode;
    ESP_LOGI(TAG, "Switched to mode %d", (int)mode);
}

/**
 * @brief 获取当前活跃模式
 * @return 当前 ui_mode_t
 */
ui_mode_t ui_manager_get_current_mode(void)
{
    return s_current_mode;
}

/**
 * @brief 进入锁屏
 */
void ui_manager_lock(void)
{
    /* 电源键锁屏时收起原有浮层，防止设置页/面板继续盖住锁屏。
     * 仅复用原收起动画，不改变这些页面自身的手势、布局及动画曲线。 */
    ui_clock_settings_close_for_lock();
    ui_motion_settings_close_for_lock();
    ui_settings_hide();
    ui_mode_panel_hide();
    ui_adj_panel_hide();
    ui_lockscreen_show();
    ui_statusbar_set_visible(true);
    lv_display_trigger_activity(lv_display_get_default());
}
