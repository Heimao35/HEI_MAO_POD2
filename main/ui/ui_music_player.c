/**
 * @file ui_music_player.c
 * @brief 音乐播放器页面实现（重构版）
 *
 * 页面结构：
 *   s_page (全屏容器)
 *     ├─ s_title_lbl   歌曲标题（与歌曲列表联动）
 *     ├─ s_cover_area  封面/歌词切换区域 (COVER_SIZE x COVER_SIZE 正方形)
 *     ├─ s_ctrl_panel  播放控制台 (屏幕等宽，底边对齐屏幕底部)
 *     │   ├─ 按钮行：上一首 / 暂停继续 / 下一首 / 播放模式
 *     │   └─ 进度条(只读 lv_bar) + 时长
 *     └─ s_playlist   歌曲列表（全屏覆盖，可滚动）
 *
 * 切歌时封面有左右滑入滑出动画。
 * 播放/暂停、播放模式按钮点击后会改变颜色以反映状态。
 * 进度条使用 lv_bar（只读），无旋钮，不可手动拖拽。
 * 歌曲列表与标题联动，高亮当前播放歌曲。
 */

#include "ui_music_player.h"
#include "../cover_loader.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- 封面正方形尺寸 ---- */
#define COVER_SIZE      240
/* ---- 控制台宽度（与屏幕等宽） ---- */
#define CTRL_PANEL_W    UI_HOR_RES


/* ---- 内部对象 ---- */
static lv_obj_t *s_page        = NULL;
static lv_obj_t *s_cover_area  = NULL;
static lv_obj_t *s_cover_img   = NULL;
static lv_obj_t *s_cover_canvas = NULL;
static lv_obj_t *s_lyric_scroll = NULL;
static lv_obj_t *s_lyric_label = NULL;
static lv_obj_t *s_ctrl_panel  = NULL;
static lv_obj_t *s_btn_prev    = NULL;
static lv_obj_t *s_btn_toggle  = NULL;
static lv_obj_t *s_btn_next    = NULL;
static lv_obj_t *s_btn_mode    = NULL;
static lv_obj_t *s_progress    = NULL; /**< lv_bar 只读进度条 */
static lv_obj_t *s_time_lbl    = NULL;
static lv_obj_t *s_title_lbl   = NULL;
static lv_obj_t *s_playlist    = NULL;

/* 歌曲列表中每个条目的对象指针，用于高亮当前歌曲 */
/*
 * The list keeps the original flex/scroll layout, but creates one row per
 * scanned song. Only LVGL row pointers are retained here; each row owns its
 * own title label, so no second full title array is kept in RAM.
 */
static lv_obj_t **s_pl_items = NULL;
static uint32_t s_pl_item_count = 0;
static uint32_t s_library_count = 0;
static uint32_t s_lyric_line_count = 0;
static ui_music_backend_t s_backend = {0};
static lv_timer_t *s_player_timer = NULL;
static uint32_t s_lyric_last_input = 0;
static bool s_lyric_user_scrolling = false;
static bool s_lyric_touch_active = false;
static int32_t s_lyric_follow_target = -1;

static bool s_show_lyric  = false;
static bool s_is_playing  = false;  /**< 当前是否播放中 */
static bool s_single_loop = false;  /**< true=单曲循环, false=顺序播放 */
static int  s_current_idx = 0;      /**< 当前播放歌曲索引 */

/* 播放控制回调指针 */
static ui_music_ctrl_cb_t s_cb_prev   = NULL;
static ui_music_ctrl_cb_t s_cb_next   = NULL;
static ui_music_ctrl_cb_t s_cb_toggle = NULL;
static ui_music_ctrl_cb_t s_cb_mode   = NULL;

/* ---- 封面切歌动画状态 ---- */
static bool s_cover_animating = false;
static bool s_cover_waiting_song = false;
static uint32_t s_cover_pending_index = UINT32_MAX;
static bool s_cover_pending_slide_left = true;
static bool s_cover_swipe_suppressed = false;
static lv_point_t s_cover_press_point; /**< 按下坐标：在 CLICKED 前识别拖动，避免一次操作触发两种动作。 */

/* ---- 前向声明 ---- */
static void update_playlist_highlight(void);
static void switch_to_song(int idx);
static bool switch_to_song_animated(int idx, bool slide_left);
static void start_cover_slide_in(bool slide_left);
static bool request_song(uint32_t index);
static uint32_t ui_song_count(void) { return s_library_count; }
static void lyric_press_cb(lv_event_t *e);
static void lyric_release_cb(lv_event_t *e);
static void lyric_scroll_cb(lv_event_t *e);
static void player_timer_cb(lv_timer_t *timer);
static int32_t lyric_center_padding(void);

/** @brief 上一首按钮：动画期间忽略重复切歌，也不重复通知控制回调。 */
static void prev_btn_cb(lv_event_t *e)
{
    (void)e;
    if (ui_song_count() == 0) return;
    int idx = s_current_idx - 1;
    if (idx < 0) idx = (int)ui_song_count() - 1;
    if (!switch_to_song_animated(idx, false)) return;
    if (s_cb_prev) s_cb_prev();
}

/** @brief 下一首按钮点击 —— 带动画切换到下一首歌曲 */
static void next_btn_cb(lv_event_t *e)
{
    (void)e;
    if (ui_song_count() == 0) return;
    int idx = s_current_idx + 1;
    if (idx >= (int)ui_song_count()) idx = 0;
    if (!switch_to_song_animated(idx, true)) return;
    if (s_cb_next) s_cb_next();
}

/**
 * @brief 暂停/继续按钮点击 —— 切换播放状态并更新按钮样式
 *
 * 播放中：按钮背景变为高亮色(UI_COLOR_ACCENT)，图标为 PAUSE
 * 暂停中：按钮背景恢复为默认深色，图标为 PLAY
 */
static void toggle_btn_cb(lv_event_t *e)
{
    (void)e;
    s_is_playing = !s_is_playing;
    lv_obj_t *icon = lv_obj_get_child(s_btn_toggle, 0);
    if (icon) {
        lv_label_set_text(icon, s_is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
    /* 播放中按钮高亮，暂停时恢复 */
    lv_obj_set_style_bg_color(s_btn_toggle,
        s_is_playing ? UI_COLOR_ACCENT : lv_color_hex(0x3A3A5C), 0);
    if (s_cb_toggle) s_cb_toggle();
}

/**
 * @brief 播放模式按钮点击 —— 在单曲循环和顺序播放间切换
 *
 * 单曲循环：图标 LOOP，按钮高亮
 * 顺序播放：图标 SHUFFLE(此处借用表示顺序)，按钮恢复
 */
static void mode_btn_cb(lv_event_t *e)
{
    (void)e;
    s_single_loop = !s_single_loop;
    lv_obj_t *icon = lv_obj_get_child(s_btn_mode, 0);
    if (icon) {
        lv_label_set_text(icon, s_single_loop ? LV_SYMBOL_LOOP : LV_SYMBOL_NEXT);
    }
    /* 单曲循环时高亮，顺序播放时恢复 */
    lv_obj_set_style_bg_color(s_btn_mode,
        s_single_loop ? UI_COLOR_ACCENT : lv_color_hex(0x3A3A5C), 0);
    if (s_cb_mode) s_cb_mode();
}

/** @brief 封面区域点击 —— 切换封面/歌词 */
/*
 * LVGL 在未进入滚动状态的横向拖动结束后仍可能发送 CLICKED。
 * RELEASED 先按位移过滤拖动，CLICKED 仅负责真正的轻触切换；
 * 上下首及列表展开/收起仍交给 ui_manager.c 原有的全局手势处理。
 */
static void cover_press_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev) lv_indev_get_point(indev, &s_cover_press_point);
    s_cover_swipe_suppressed = false;
}

static void cover_release_cb(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t point;
    lv_indev_get_point(indev, &point);
    int32_t dx = point.x - s_cover_press_point.x;
    int32_t dy = point.y - s_cover_press_point.y;
    int32_t abs_dx = dx < 0 ? -dx : dx;
    int32_t abs_dy = dy < 0 ? -dy : dy;
    s_cover_swipe_suppressed = abs_dx >= 40 || abs_dy >= 40;
}

static void cover_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_cover_swipe_suppressed || s_cover_animating) {
        s_cover_swipe_suppressed = false;
        return;
    }
    s_show_lyric = !s_show_lyric;
    if (s_show_lyric) {
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_lyric_scroll, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lyric_scroll, LV_OBJ_FLAG_HIDDEN);
    }
}

/** @brief 歌曲列表条目点击 —— 切换到对应歌曲 */
static void playlist_item_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx < 0 || idx >= (int)ui_song_count() || s_cover_animating) return;
    switch_to_song(idx);
    request_song((uint32_t)idx);
}

/**
 * @brief 切换到指定索引的歌曲，更新标题和列表高亮
 * @param idx 歌曲索引 (0-based)
 */
static void switch_to_song(int idx)
{
    if (idx < 0 || idx >= (int)ui_song_count()) return;
    s_current_idx = idx;
    update_playlist_highlight();
}

/** @brief 封面 X 坐标动画执行回调 */
static void cover_anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

/** @brief 封面滑出动画完成回调 —— 更新内容后从另一侧滑入 */
static void cover_slide_out_ready(lv_anim_t *a)
{
    int32_t packed = (int32_t)(intptr_t)a->user_data;
    int idx = packed >> 1;
    bool slide_left = packed & 1;

    s_current_idx = idx;
    s_cover_pending_index = (uint32_t)idx;
    s_cover_pending_slide_left = slide_left;
    s_cover_waiting_song = true;
    update_playlist_highlight();

    /* 此时旧图已完全离屏。SD/JPEG/LRC 在 worker 中异步加载，
     * 容器保持离屏；收到同一首的 song_ready 后才开始滑入，不能提前换图。 */
    if (!request_song((uint32_t)idx)) {
        ui_music_player_song_ready((uint32_t)idx);
    }
}

/**
 * @brief 带滑动动画的切歌
 * @param idx        目标歌曲索引
 * @param slide_left true=封面向左滑出（下一首），false=向右滑出（上一首）
 */
static void cover_slide_in_ready(lv_anim_t *a)
{
    (void)a;
    s_cover_animating = false;
    s_cover_waiting_song = false;
    s_cover_pending_index = UINT32_MAX;
}

/** 新媒体已经在 LVGL 锁内发布，才从屏幕另一侧滑入；沿用原时长和曲线。 */
static void start_cover_slide_in(bool slide_left)
{
    int32_t in_start = slide_left ? (int32_t)UI_HOR_RES : -(int32_t)UI_HOR_RES;
    int32_t in_end = ((int32_t)UI_HOR_RES - COVER_SIZE) / 2;
    lv_obj_set_x(s_cover_area, in_start);
    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, s_cover_area);
    lv_anim_set_values(&a_in, in_start, in_end);
    lv_anim_set_duration(&a_in, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a_in, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a_in, cover_anim_x_cb);
    lv_anim_set_completed_cb(&a_in, cover_slide_in_ready);
    lv_anim_start(&a_in);
}

static bool switch_to_song_animated(int idx, bool slide_left)
{
    if (idx < 0 || idx >= (int)ui_song_count() || s_cover_animating) return false;
    s_cover_animating = true;
    int32_t out_target = slide_left ? -(int32_t)UI_HOR_RES : (int32_t)UI_HOR_RES;
    int32_t packed = (idx << 1) | (slide_left ? 1 : 0);
    lv_anim_t a_out;
    lv_anim_init(&a_out);
    lv_anim_set_var(&a_out, s_cover_area);
    lv_anim_set_values(&a_out, lv_obj_get_x(s_cover_area), out_target);
    lv_anim_set_duration(&a_out, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a_out, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a_out, cover_anim_x_cb);
    lv_anim_set_user_data(&a_out, (void *)(intptr_t)packed);
    lv_anim_set_completed_cb(&a_out, cover_slide_out_ready);
    lv_anim_start(&a_out);
    return true;
}

/**
 * @brief 刷新歌曲列表中的高亮状态
 * 当前播放歌曲背景变为 UI_COLOR_ACCENT，其余恢复默认色。
 */
static void update_playlist_highlight(void)
{
    for (uint32_t i = 0; i < s_pl_item_count; i++) {
        if (!s_pl_items[i]) continue;
        if (i == s_current_idx) {
            lv_obj_set_style_bg_color(s_pl_items[i], UI_COLOR_ACCENT, 0);
            lv_obj_set_style_bg_opa(s_pl_items[i], LV_OPA_COVER, 0);
        } else {
            lv_obj_set_style_bg_color(s_pl_items[i], lv_color_hex(0x1A1A3E), 0);
            lv_obj_set_style_bg_opa(s_pl_items[i], LV_OPA_80, 0);
        }
    }
}

/**
 * @brief 创建控制台按钮（统一样式）
 * @param parent  父容器
 * @param symbol  LVGL 符号
 * @param cb      点击回调
 * @return 按钮对象
 *
 * 按钮尺寸 56x56，增大此值使按钮更大更易点击
 */
static lv_obj_t *create_ctrl_btn(lv_obj_t *parent, const char *symbol,
                                  lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 72, 72);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A5C), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 10);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/**
 * @brief 创建音乐播放器页面
 *
 * 封面区域和控制台等宽 (PLAYER_PANEL_W=280px)。
 * 进度条使用 lv_bar（只读，无旋钮，不可拖拽）。
 * 歌曲列表可上下滚动，高亮当前播放歌曲。
 *
 * @param parent 父对象
 */
void ui_music_player_create(lv_obj_t *parent)
{
    /* ===== 页面容器（全屏） ===== */
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_align(s_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0x0D0D2B), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* ===== 歌曲标题 —— 默认显示第一首测试歌曲 ===== */
    s_title_lbl = lv_label_create(s_page);
    lv_label_set_text(s_title_lbl, "Waiting for SD card");
    lv_obj_set_style_text_color(s_title_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    /* Keep the original title position; constrain width so long names scroll horizontally. */
    lv_obj_set_width(s_title_lbl, UI_HOR_RES - 32);
    /* 固定的是标题视口宽度：短歌名按视口居中，溢出时 LVGL 自动从左侧滚动。
     * 不要让标签宽度随文本伸长，否则短标题/长标题会使用不同的对齐基准。 */
    lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_title_lbl, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_HEIGHT + 6);

    /* ===== 封面/歌词切换区域（正方形 COVER_SIZE x COVER_SIZE） ===== */
    s_cover_area = lv_obj_create(s_page);
    lv_obj_set_size(s_cover_area, COVER_SIZE, COVER_SIZE);
    lv_obj_set_pos(s_cover_area, ((int32_t)UI_HOR_RES - (int32_t)COVER_SIZE) / 2,
                   UI_STATUSBAR_HEIGHT + 30);
    lv_obj_set_style_bg_opa(s_cover_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cover_area, 0, 0);
    lv_obj_set_style_pad_all(s_cover_area, 0, 0);
    lv_obj_remove_flag(s_cover_area, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cover_area, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_cover_area, cover_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_cover_area, cover_release_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_cover_area, cover_click_cb, LV_EVENT_CLICKED, NULL);

    /* 封面占位矩形（正方形，铺满 cover_area） */
    s_cover_img = lv_obj_create(s_cover_area);
    lv_obj_set_size(s_cover_img, COVER_SIZE, COVER_SIZE);
    lv_obj_center(s_cover_img);
    lv_obj_set_style_radius(s_cover_img, 16, 0);
    lv_obj_set_style_bg_color(s_cover_img, lv_color_hex(0x4A3080), 0);
    lv_obj_set_style_bg_opa(s_cover_img, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cover_img, 0, 0);
    lv_obj_remove_flag(s_cover_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* Canvas 只承接后台已解码的双缓冲像素，不改变原封面容器的位置和动画。 */
    s_cover_canvas = lv_canvas_create(s_cover_img);
    lv_obj_set_size(s_cover_canvas, COVER_SIZE, COVER_SIZE);
    lv_obj_center(s_cover_canvas);
    lv_obj_add_flag(s_cover_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_cover_canvas, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cover_icon = lv_label_create(s_cover_img);
    lv_label_set_text(cover_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(cover_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(cover_icon, &lv_font_montserrat_26, 0);
    lv_obj_center(cover_icon);

    /*
     * Lyrics remain in the original cover area and stay transparent. The
     * scroll viewport is exactly 240x240, so it cannot overlap the console.
     * A scroll event records user activity; the player timer follows the
     * current lyric again after two seconds without interaction.
     */
    s_lyric_scroll = lv_obj_create(s_cover_area);
    lv_obj_set_size(s_lyric_scroll, COVER_SIZE, COVER_SIZE);
    lv_obj_center(s_lyric_scroll);
    lv_obj_set_style_bg_opa(s_lyric_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_lyric_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_lyric_scroll, 0, 0);
    lv_obj_add_flag(s_lyric_scroll, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_lyric_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_lyric_scroll, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_lyric_scroll, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_lyric_scroll, lyric_scroll_cb, LV_EVENT_SCROLL, NULL);
    lv_obj_add_event_cb(s_lyric_scroll, lyric_press_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_lyric_scroll, lyric_release_cb, LV_EVENT_RELEASED, NULL);

    s_lyric_label = lv_label_create(s_lyric_scroll);
    lv_label_set_text(s_lyric_label, "Lyrics will\nappear here");
    lv_obj_set_width(s_lyric_label, COVER_SIZE - 16);
    lv_obj_set_style_text_color(s_lyric_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(s_lyric_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_lyric_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_lyric_label, LV_LABEL_LONG_WRAP);
    /* 上下各补半个视口减半行字高：scroll_y=0 时首行即在中央，
     * 同时末行也有足够的底部空间滚到中央。留白属于透明歌词内容，
     * 视口仍为原有的 240x240，文字不会覆盖下方控制台。 */
    int32_t lyric_pad = lyric_center_padding();
    lv_obj_set_style_pad_top(s_lyric_label, lyric_pad, 0);
    lv_obj_set_style_pad_bottom(s_lyric_label, lyric_pad, 0);
    lv_obj_set_pos(s_lyric_label, 8, 0);
    lv_obj_add_flag(s_lyric_scroll, LV_OBJ_FLAG_HIDDEN);

    /* ===== 播放控制台（与屏幕等宽，底边对齐屏幕底部） ===== */
    s_ctrl_panel = lv_obj_create(s_page);
    lv_obj_set_size(s_ctrl_panel, CTRL_PANEL_W, 120);
    lv_obj_align(s_ctrl_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(s_ctrl_panel, 0, 0);
    lv_obj_set_style_bg_color(s_ctrl_panel, lv_color_hex(0x1A1A3E), 0);
    lv_obj_set_style_bg_opa(s_ctrl_panel, UI_PANEL_BG_OPA, 0);
    lv_obj_set_style_border_width(s_ctrl_panel, 0, 0);
    lv_obj_set_style_pad_all(s_ctrl_panel, 8, 0);
    lv_obj_set_flex_flow(s_ctrl_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ctrl_panel, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_ctrl_panel, 6, 0);
    lv_obj_remove_flag(s_ctrl_panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_ctrl_panel, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 按钮行 */
    lv_obj_t *btn_row = lv_obj_create(s_ctrl_panel);
    lv_obj_set_size(btn_row, LV_PCT(100), 76);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(btn_row, 24, 0);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_row, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_btn_prev   = create_ctrl_btn(btn_row, LV_SYMBOL_PREV,  prev_btn_cb);
    s_btn_toggle = create_ctrl_btn(btn_row, LV_SYMBOL_PLAY,  toggle_btn_cb);
    s_btn_next   = create_ctrl_btn(btn_row, LV_SYMBOL_NEXT,  next_btn_cb);
    s_btn_mode   = create_ctrl_btn(btn_row, LV_SYMBOL_NEXT,  mode_btn_cb);

    /* 进度条行 */
    lv_obj_t *prog_row = lv_obj_create(s_ctrl_panel);
    lv_obj_set_size(prog_row, LV_PCT(100), 24);
    lv_obj_set_style_bg_opa(prog_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(prog_row, 0, 0);
    lv_obj_set_style_pad_all(prog_row, 0, 0);
    lv_obj_remove_flag(prog_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(prog_row, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 进度条 —— 使用 lv_bar（只读，无旋钮，不可手动拖拽） */
    s_progress = lv_bar_create(prog_row);
    lv_obj_set_size(s_progress, LV_PCT(78), 6);
    lv_obj_set_style_bg_color(s_progress, UI_COLOR_SLIDER_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress, UI_COLOR_SLIDER_ACTIVE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress, 3, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress, 3, LV_PART_INDICATOR);
    lv_bar_set_range(s_progress, 0, 1000);
    lv_bar_set_value(s_progress, 0, LV_ANIM_OFF);
    lv_obj_align(s_progress, LV_ALIGN_LEFT_MID, 10, 0);

    /* 时长 label */
    s_time_lbl = lv_label_create(prog_row);
    lv_label_set_text(s_time_lbl, "0:00");
    lv_obj_set_style_text_color(s_time_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_time_lbl, LV_ALIGN_RIGHT_MID, -10, 0);

    /* ===== 歌曲列表（全屏，可上下滚动，默认隐藏） ===== */
    s_playlist = lv_obj_create(s_page);
    lv_obj_set_size(s_playlist, UI_HOR_RES, UI_VER_RES);
    lv_obj_align(s_playlist, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_playlist, lv_color_hex(0x0D0D2B), 0);
    lv_obj_set_style_bg_opa(s_playlist, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_playlist, 0, 0);
    lv_obj_set_style_radius(s_playlist, 0, 0);
    lv_obj_set_style_pad_top(s_playlist, UI_STATUSBAR_HEIGHT + 10, 0);
    lv_obj_set_style_pad_bottom(s_playlist, 10, 0);
    lv_obj_set_style_pad_left(s_playlist, 16, 0);
    lv_obj_set_style_pad_right(s_playlist, 16, 0);
    lv_obj_set_flex_flow(s_playlist, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(s_playlist, 6, 0);
    /* 允许上下滚动查看歌曲列表 */
    lv_obj_add_flag(s_playlist, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_playlist, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_playlist, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(s_playlist, LV_OBJ_FLAG_EVENT_BUBBLE);

    /*
     * Do not create demo rows here. The media worker calls
     * ui_music_player_set_library() after scanning and creates the same row
     * style for every real SD-card song. The original full-screen container,
     * flex layout, vertical scrolling, and playlist slide animation stay
     * unchanged.
     */
    lv_obj_add_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);
    /* Periodically updates the existing progress row and lyric follow position. */
    s_player_timer = lv_timer_create(player_timer_cb, 200, NULL);
}

lv_obj_t *ui_music_player_get_page(void) { return s_page; }

static void playlist_anim_y_cb(void *obj, int32_t v) { lv_obj_set_y((lv_obj_t *)obj, v); }

static void playlist_hide_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);
}

void ui_music_player_toggle_playlist(bool show)
{
    if (!s_playlist) return;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_playlist);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, playlist_anim_y_cb);
    if (show) {
        lv_obj_remove_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);
        lv_obj_move_foreground(s_playlist);
        lv_anim_set_values(&a, (int32_t)UI_VER_RES, 0);
    } else {
        lv_anim_set_values(&a, 0, (int32_t)UI_VER_RES);
        lv_anim_set_completed_cb(&a, playlist_hide_ready);
    }
    lv_anim_start(&a);
}

void ui_music_player_update_progress(uint32_t current_sec, uint32_t total_sec, bool is_playing)
{
    if (!s_progress || !s_time_lbl) return;
    int32_t val = 0;
    if (total_sec > 0) val = (int32_t)((uint64_t)current_sec * 1000 / total_sec);
    lv_bar_set_value(s_progress, val, LV_ANIM_ON);
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu:%02lu",
             (unsigned long)(total_sec / 60), (unsigned long)(total_sec % 60));
    lv_label_set_text(s_time_lbl, buf);

    /* 同步播放状态到按钮样式 */
    if (is_playing != s_is_playing) {
        s_is_playing = is_playing;
        lv_obj_t *icon = lv_obj_get_child(s_btn_toggle, 0);
        if (icon) lv_label_set_text(icon, s_is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
        lv_obj_set_style_bg_color(s_btn_toggle,
            s_is_playing ? UI_COLOR_ACCENT : lv_color_hex(0x3A3A5C), 0);
    }
}

void ui_music_player_set_song(const char *title)
{
    if (s_title_lbl && title) lv_label_set_text(s_title_lbl, title);
}

void ui_music_player_set_callbacks(ui_music_ctrl_cb_t on_prev, ui_music_ctrl_cb_t on_next,
                                   ui_music_ctrl_cb_t on_toggle, ui_music_ctrl_cb_t on_mode)
{
    s_cb_prev = on_prev; s_cb_next = on_next;
    s_cb_toggle = on_toggle; s_cb_mode = on_mode;
}

bool ui_music_player_is_playlist_visible(void)
{
    if (!s_playlist) return false;
    return !lv_obj_has_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);
}
bool ui_music_player_is_lyric_visible(void)
{
    return s_show_lyric;
}
bool ui_music_player_is_lyric_point(int32_t x, int32_t y)
{
    if (!s_show_lyric || !s_lyric_scroll ||
        lv_obj_has_flag(s_lyric_scroll, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_point_t point = {.x = x, .y = y};
    return lv_obj_hit_test(s_lyric_scroll, &point);
}

void ui_music_player_next_animated(void)
{
    if (ui_song_count() == 0) return;
    int idx = s_current_idx + 1;
    if (idx >= (int)ui_song_count()) idx = 0;
    if (!switch_to_song_animated(idx, true)) return;
    if (s_cb_next) s_cb_next();
}

void ui_music_player_prev_animated(void)
{
    if (ui_song_count() == 0) return;
    int idx = s_current_idx - 1;
    if (idx < 0) idx = (int)ui_song_count() - 1;
    if (!switch_to_song_animated(idx, false)) return;
    if (s_cb_prev) s_cb_prev();
}

static bool request_song(uint32_t index)
{
    if (!s_backend.select || index >= s_library_count) return false;
    s_backend.select(index);
    return true;
}

bool ui_music_player_can_show_song(uint32_t index)
{
    /* 滑出阶段丢弃先前点击发起的迟到加载；只允许离屏等待的目标发布。
     * 滑出完成还会重新投递目标，故丢弃不会让动画永久等待。 */
    return !s_cover_animating ||
           (s_cover_waiting_song && index == s_cover_pending_index);
}

void ui_music_player_song_ready(uint32_t index)
{
    /* 必须在 show_song / set_lyrics / commit 之后、同一个 LVGL 锁内调用。 */
    if (!s_cover_waiting_song || index != s_cover_pending_index) return;
    s_cover_waiting_song = false;
    start_cover_slide_in(s_cover_pending_slide_left);
}

void ui_music_player_bind(const ui_music_backend_t *backend)
{
    s_backend = backend ? *backend : (ui_music_backend_t){0};
}

void ui_music_player_set_library(uint32_t count, const char *status)
{
    /*
     * Rebuild only the list children. The playlist object itself is retained,
     * so its original padding, flex flow, scroll behavior, and slide animation
     * are untouched. A rescan therefore changes data, not page geometry.
     */
    if (!s_playlist) return;
    /* 拔卡/重扫可能发生在任何动画阶段，必须取消等待并恢复原始坐标。 */
    lv_anim_delete(s_cover_area, cover_anim_x_cb);
    s_cover_animating = false;
    s_cover_waiting_song = false;
    s_cover_pending_index = UINT32_MAX;
    lv_obj_set_x(s_cover_area, ((int32_t)UI_HOR_RES - COVER_SIZE) / 2);
    lv_obj_add_flag(s_cover_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lv_obj_get_child(s_cover_img, 1), LV_OBJ_FLAG_HIDDEN);
    ui_music_player_set_lyrics("", 0, false);
    ui_music_player_update_progress(0, 0, false);
    lv_obj_clean(s_playlist);
    free(s_pl_items);
    s_pl_items = NULL;
    s_pl_item_count = 0;
    s_library_count = count;
    s_current_idx = 0;

    if (count > 0) {
        s_pl_items = calloc(count, sizeof(*s_pl_items));
        if (!s_pl_items) {
            s_library_count = 0;
            lv_label_set_text(s_title_lbl, "Music list memory error");
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                lv_obj_t *item = lv_obj_create(s_playlist);
                if (!item) break;
                lv_obj_set_size(item, LV_PCT(100), 48);
                lv_obj_set_style_bg_color(item, lv_color_hex(0x1A1A3E), 0);
                lv_obj_set_style_bg_opa(item, LV_OPA_80, 0);
                lv_obj_set_style_radius(item, 10, 0);
                lv_obj_set_style_border_width(item, 0, 0);
                lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
                lv_obj_add_event_cb(item, playlist_item_cb, LV_EVENT_CLICKED,
                                    (void *)(uintptr_t)i);

                lv_obj_t *title = lv_label_create(item);
                lv_label_set_text(title, "Loading...");
                lv_obj_set_width(title, UI_HOR_RES - 56);
                /* 条目仍是原来的 48px 高、左侧对齐；限制文本宽度并使用横向
                 * 循环滚动，长歌名不换行，不改变列表的滚动和点击选歌逻辑。 */
                lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
                lv_obj_set_style_text_color(title, lv_color_white(), 0);
                lv_obj_set_style_text_font(title, &lv_font_montserrat_16, 0);
                lv_obj_align(title, LV_ALIGN_LEFT_MID, 8, 0);
                s_pl_items[i] = item;
                s_pl_item_count++;
            }
            s_library_count = s_pl_item_count;
            update_playlist_highlight();
        }
    }

    lv_obj_scroll_to_y(s_playlist, 0, LV_ANIM_OFF);
    lv_label_set_text(s_title_lbl, count ? "Loading..." : (status ? status : "No songs"));
    if (s_backend.page && s_library_count) s_backend.page(0);
}

void ui_music_player_set_page(const music_page_t *page)
{
    if (!page || !s_pl_items) return;
    /* Pages are only a bounded transport format from the SD worker; every
     * song already has a normal row in the original scrolling list. */
    for (uint32_t i = 0; i < page->count; ++i) {
        uint32_t index = page->first + i;
        if (index >= s_pl_item_count || !s_pl_items[index]) continue;
        lv_obj_t *title = lv_obj_get_child(s_pl_items[index], 0);
        if (title) lv_label_set_text(title, page->names[i]);
    }
    update_playlist_highlight();
}

void ui_music_player_show_song(uint32_t index, const char *title,
                               const uint8_t *cover, uint32_t duration_sec)
{
    s_current_idx = (int)index;
    if (title) lv_label_set_text(s_title_lbl, title);
    if (cover && s_cover_canvas) {
        lv_canvas_set_buffer(s_cover_canvas, (void *)cover, COVER_SIZE, COVER_SIZE,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_remove_flag(s_cover_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *icon = lv_obj_get_child(s_cover_img, 1);
        if (icon) lv_obj_add_flag(icon, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_cover_canvas);
    } else {
        lv_obj_add_flag(s_cover_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_t *icon = lv_obj_get_child(s_cover_img, 1);
        if (icon) lv_obj_remove_flag(icon, LV_OBJ_FLAG_HIDDEN);
    }
    ui_music_player_update_progress(0, duration_sec, false);
    update_playlist_highlight();
}

void ui_music_player_set_lyrics(const char *text, uint32_t line_count, bool truncated)
{
    (void)truncated;
    /* 多时间标签可复用解析器文字，但展开后的 UI 副本有独立的 64KiB 上限。
     * 只定位真正显示的行，避免被截去的尾部时间索引指向不存在的文本。 */
    uint32_t visible_lines = text && text[0] ? 1 : 0;
    if (visible_lines) {
        for (const char *p = text; *p; ++p) if (*p == '\n') ++visible_lines;
    }
    s_lyric_line_count = line_count < visible_lines ? line_count : visible_lines;
    lv_label_set_text(s_lyric_label, text && text[0] ? text : "Lyrics will\nappear here");
    lv_obj_update_layout(s_lyric_label);
    s_lyric_user_scrolling = false;
    s_lyric_last_input = lv_tick_get();
    s_lyric_touch_active = false;
    s_lyric_follow_target = -1;
    lv_obj_scroll_to_y(s_lyric_scroll, 0, LV_ANIM_OFF);
}

static void lyric_press_cb(lv_event_t *e)
{
    (void)e;
    /* 按住时不回跟，终止尚未完成的自动滚动，释放后重新计两秒。 */
    lv_obj_scroll_to_y(s_lyric_scroll, lv_obj_get_scroll_y(s_lyric_scroll), LV_ANIM_OFF);
    s_lyric_follow_target = -1;
    s_lyric_touch_active = true;
    s_lyric_user_scrolling = true;
    s_lyric_last_input = lv_tick_get();
}

static void lyric_release_cb(lv_event_t *e)
{
    (void)e;
    s_lyric_touch_active = false;
    s_lyric_last_input = lv_tick_get();
}

static void lyric_scroll_cb(lv_event_t *e)
{
    (void)e;
    /* LVGL also emits SCROLL during our animated auto-follow. Only pointer
     * activity counts as user interaction and restarts the two-second delay. */
    if (!s_lyric_touch_active) return;
    s_lyric_user_scrolling = true;
    s_lyric_last_input = lv_tick_get();
}

static int32_t lyric_center_padding(void)
{
    const lv_font_t *font = lv_obj_get_style_text_font(s_lyric_label, LV_PART_MAIN);
    int32_t line_height = font ? lv_font_get_line_height(font) : 16;
    return (COVER_SIZE - line_height) / 2;
}

/* Convert the parser's LRC row index to an UTF-8 character index for LVGL. */
static uint32_t lyric_line_char_index(uint32_t line)
{
    const char *text = lv_label_get_text(s_lyric_label);
    uint32_t row = 0;
    uint32_t char_index = 0;
    for (const unsigned char *p = (const unsigned char *)text; *p && row < line; ++p) {
        if (*p == '\n') { ++row; ++char_index; }
        else if ((*p & 0xC0) != 0x80) ++char_index;
    }
    return char_index;
}

static void player_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_backend.get_time_ms && s_backend.get_duration_sec) {
        uint32_t time_ms = s_backend.get_time_ms();
        uint32_t duration_sec = s_backend.get_duration_sec();
        /* Before DAC/decoder integration there is no backend play-state
         * callback. Preserve the original button interaction locally; once
         * a real callback is supplied it becomes authoritative. */
        bool playing = s_backend.is_playing ? s_backend.is_playing() : s_is_playing;
        ui_music_player_update_progress(time_ms / 1000, duration_sec, playing);

        if (s_show_lyric && s_backend.get_lyric_line && s_lyric_line_count &&
            !s_lyric_touch_active && lv_tick_elaps(s_lyric_last_input) >= 2000) {
            uint32_t line = s_backend.get_lyric_line(time_ms);
            /* 第一个时间标签之前也保持首行在中央。 */
            if (line == UINT32_MAX) line = 0;
            if (line >= s_lyric_line_count) line = s_lyric_line_count - 1;
            if (line < s_lyric_line_count) {
                lv_point_t lyric_pos;
                lv_label_get_letter_pos(s_lyric_label, lyric_line_char_index(line), &lyric_pos);
                int32_t pad_top = lv_obj_get_style_pad_top(s_lyric_label, LV_PART_MAIN);
                int32_t line_height = lv_font_get_line_height(
                    lv_obj_get_style_text_font(s_lyric_label, LV_PART_MAIN));
                int32_t target = pad_top + lyric_pos.y - (COVER_SIZE - line_height) / 2;
                /* scroll_bottom 是当前位置的剩余距离，必须加 scroll_y 才是绝对上限。
                 * 不能按固定行高乘 LRC 行号：长句会自动换行，需查询真实字形坐标。 */
                int32_t bottom = lv_obj_get_scroll_y(s_lyric_scroll) +
                                 lv_obj_get_scroll_bottom(s_lyric_scroll);
                if (bottom < 0) bottom = 0;
                if (target < 0) target = 0;
                if (target > bottom) target = bottom;
                /* 仅目标改变或手动浏览结束后发起，避免 200ms 定时器不断重启动画。 */
                if (s_lyric_follow_target != target || s_lyric_user_scrolling) {
                    lv_obj_scroll_to_y(s_lyric_scroll, target, LV_ANIM_ON);
                    s_lyric_follow_target = target;
                }
                s_lyric_user_scrolling = false;
            }
        }
    }
}
