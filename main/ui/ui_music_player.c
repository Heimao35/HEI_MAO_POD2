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
#include <stdio.h>
#include <string.h>

/* ---- 封面正方形尺寸 ---- */
#define COVER_SIZE      240
/* ---- 控制台宽度（与屏幕等宽） ---- */
#define CTRL_PANEL_W    UI_HOR_RES

/* ---- 测试歌曲列表 ---- */
static const char *s_demo_songs[] = {
    "Song 1", "Song 2", "Song 3", "Song 4", "Song 5",
    "Song 6", "Song 7", "Song 8"
};
#define DEMO_SONG_COUNT (sizeof(s_demo_songs) / sizeof(s_demo_songs[0]))

/* ---- 内部对象 ---- */
static lv_obj_t *s_page        = NULL;
static lv_obj_t *s_cover_area  = NULL;
static lv_obj_t *s_cover_img   = NULL;
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
static lv_obj_t *s_pl_items[DEMO_SONG_COUNT] = {NULL};

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
static bool s_cover_animating = false; /**< 封面动画进行中则忽略新切歌 */

/* ---- 前向声明 ---- */
static void update_playlist_highlight(void);
static void switch_to_song(int idx);
static void switch_to_song_animated(int idx, bool slide_left);

/* ---- 按钮事件回调 ---- */

/** @brief 上一首按钮点击 —— 带动画切换到上一首歌曲 */
static void prev_btn_cb(lv_event_t *e)
{
    (void)e;
    int idx = s_current_idx - 1;
    if (idx < 0) idx = (int)DEMO_SONG_COUNT - 1;
    switch_to_song_animated(idx, false);
    if (s_cb_prev) s_cb_prev();
}

/** @brief 下一首按钮点击 —— 带动画切换到下一首歌曲 */
static void next_btn_cb(lv_event_t *e)
{
    (void)e;
    int idx = s_current_idx + 1;
    if (idx >= (int)DEMO_SONG_COUNT) idx = 0;
    switch_to_song_animated(idx, true);
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
static void cover_click_cb(lv_event_t *e)
{
    (void)e;
    s_show_lyric = !s_show_lyric;
    if (s_show_lyric) {
        lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(s_lyric_label, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(s_cover_img, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(s_lyric_label, LV_OBJ_FLAG_HIDDEN);
    }
}

/** @brief 歌曲列表条目点击 —— 切换到对应歌曲 */
static void playlist_item_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    switch_to_song(idx);
}

/**
 * @brief 切换到指定索引的歌曲，更新标题和列表高亮
 * @param idx 歌曲索引 (0-based)
 */
static void switch_to_song(int idx)
{
    if (idx < 0 || idx >= (int)DEMO_SONG_COUNT) return;
    s_current_idx = idx;
    lv_label_set_text(s_title_lbl, s_demo_songs[idx]);
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
    /* 取出目标索引和方向（编码在 user_data 中） */
    int32_t packed = (int32_t)(intptr_t)a->user_data;
    int idx = packed >> 1;
    bool slide_left = packed & 1;

    /* 更新歌曲信息 */
    s_current_idx = idx;
    lv_label_set_text(s_title_lbl, s_demo_songs[idx]);
    update_playlist_highlight();

    /* 封面从反方向滑入 */
    int32_t in_start = slide_left ? (int32_t)UI_HOR_RES : -(int32_t)UI_HOR_RES;
    lv_obj_set_x(s_cover_area, in_start);

    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, s_cover_area);
    lv_anim_set_values(&a_in, in_start, ((int32_t)UI_HOR_RES - (int32_t)COVER_SIZE) / 2);
    lv_anim_set_duration(&a_in, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a_in, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a_in, cover_anim_x_cb);
    lv_anim_start(&a_in);
    s_cover_animating = false;
}

/**
 * @brief 带滑动动画的切歌
 * @param idx        目标歌曲索引
 * @param slide_left true=封面向左滑出（下一首），false=向右滑出（上一首）
 */
static void switch_to_song_animated(int idx, bool slide_left)
{
    if (idx < 0 || idx >= (int)DEMO_SONG_COUNT) return;
    if (s_cover_animating) return;
    s_cover_animating = true;

    int32_t out_target = slide_left ? -(int32_t)UI_HOR_RES : (int32_t)UI_HOR_RES;
    int32_t packed = (idx << 1) | (slide_left ? 1 : 0);

    lv_anim_t a_out;
    lv_anim_init(&a_out);
    lv_anim_set_var(&a_out, s_cover_area);
    lv_anim_set_values(&a_out, ((int32_t)UI_HOR_RES - (int32_t)COVER_SIZE) / 2, out_target);
    lv_anim_set_duration(&a_out, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a_out, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a_out, cover_anim_x_cb);
    lv_anim_set_user_data(&a_out, (void *)(intptr_t)packed);
    lv_anim_set_completed_cb(&a_out, cover_slide_out_ready);
    lv_anim_start(&a_out);
}

/**
 * @brief 刷新歌曲列表中的高亮状态
 * 当前播放歌曲背景变为 UI_COLOR_ACCENT，其余恢复默认色。
 */
static void update_playlist_highlight(void)
{
    for (int i = 0; i < (int)DEMO_SONG_COUNT; i++) {
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
    lv_label_set_text(s_title_lbl, s_demo_songs[0]);
    lv_obj_set_style_text_color(s_title_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
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
    lv_obj_t *cover_icon = lv_label_create(s_cover_img);
    lv_label_set_text(cover_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(cover_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(cover_icon, &lv_font_montserrat_26, 0);
    lv_obj_center(cover_icon);

    /* 歌词 label（默认隐藏） */
    s_lyric_label = lv_label_create(s_cover_area);
    lv_label_set_text(s_lyric_label, "Lyrics will\nappear here");
    lv_obj_set_style_text_color(s_lyric_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(s_lyric_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_lyric_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_lyric_label);
    lv_obj_add_flag(s_lyric_label, LV_OBJ_FLAG_HIDDEN);

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

    for (int i = 0; i < (int)DEMO_SONG_COUNT; i++) {
        lv_obj_t *item = lv_obj_create(s_playlist);
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

        lv_obj_t *lbl = lv_label_create(item);
        lv_label_set_text(lbl, s_demo_songs[i]);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);

        s_pl_items[i] = item;
    }
    /* 高亮第一首歌曲 */
    update_playlist_highlight();
    lv_obj_add_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);
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
    if (total_sec > 0) val = (int32_t)(current_sec * 1000 / total_sec);
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

void ui_music_player_next_animated(void)
{
    int idx = s_current_idx + 1;
    if (idx >= (int)DEMO_SONG_COUNT) idx = 0;
    switch_to_song_animated(idx, true);
    if (s_cb_next) s_cb_next();
}

void ui_music_player_prev_animated(void)
{
    int idx = s_current_idx - 1;
    if (idx < 0) idx = (int)DEMO_SONG_COUNT - 1;
    switch_to_song_animated(idx, false);
    if (s_cb_prev) s_cb_prev();
}
