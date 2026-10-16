/**
 * @file ui_music_player.c
 * @brief 音乐播放器页面实现
 *
 * 页面结构：
 *   s_page (全屏容器，深色背景占位，后续替换为模糊封面)
 *     ├─ s_cover_img  清晰封面占位（圆角矩形，点击切换歌词）
 *     ├─ s_lyric_label 歌词 label（默认隐藏，点击封面区域切换）
 *     ├─ s_ctrl_panel 播放控制台（圆角矩形半透明背景）
 *     │   ├─ 按钮行：上一首 / 暂停继续 / 下一首 / 播放模式
 *     │   ├─ 进度条
 *     │   └─ 时长 label
 *     └─ s_playlist   歌曲列表（全屏覆盖，默认隐藏）
 */

#include "ui_music_player.h"
#include <stdio.h>

/* ---- 内部对象 ---- */
static lv_obj_t *s_page        = NULL; /**< 音乐播放器页面容器 */
static lv_obj_t *s_cover_area  = NULL; /**< 封面/歌词切换区域 */
static lv_obj_t *s_cover_img   = NULL; /**< 封面占位 (当前用带颜色的矩形) */
static lv_obj_t *s_lyric_label = NULL; /**< 歌词 label */
static lv_obj_t *s_ctrl_panel  = NULL; /**< 播放控制台容器 */
static lv_obj_t *s_btn_prev    = NULL; /**< 上一首按钮 */
static lv_obj_t *s_btn_toggle  = NULL; /**< 暂停/继续按钮 */
static lv_obj_t *s_btn_next    = NULL; /**< 下一首按钮 */
static lv_obj_t *s_btn_mode    = NULL; /**< 播放模式按钮 */
static lv_obj_t *s_progress    = NULL; /**< 进度条 */
static lv_obj_t *s_time_lbl    = NULL; /**< 时长显示 label */
static lv_obj_t *s_title_lbl   = NULL; /**< 歌曲标题 label */
static lv_obj_t *s_playlist    = NULL; /**< 歌曲列表容器 */

static bool s_show_lyric = false; /**< 当前是否显示歌词 (false=封面) */

/* 播放控制回调指针 */
static ui_music_ctrl_cb_t s_cb_prev   = NULL;
static ui_music_ctrl_cb_t s_cb_next   = NULL;
static ui_music_ctrl_cb_t s_cb_toggle = NULL;
static ui_music_ctrl_cb_t s_cb_mode   = NULL;

/* ---- 按钮事件回调 ---- */

/** @brief 上一首按钮点击回调 */
static void prev_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_cb_prev) s_cb_prev();
}

/** @brief 暂停/继续按钮点击回调 */
static void toggle_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_cb_toggle) s_cb_toggle();
}

/** @brief 下一首按钮点击回调 */
static void next_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_cb_next) s_cb_next();
}

/** @brief 播放模式按钮点击回调 */
static void mode_btn_cb(lv_event_t *e)
{
    (void)e;
    if (s_cb_mode) s_cb_mode();
}

/**
 * @brief 封面区域点击回调 —— 切换封面/歌词显示
 */
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

/**
 * @brief 创建一个控制台圆形按钮（统一样式）
 * @param parent  父容器
 * @param symbol  按钮上显示的 LVGL 符号
 * @param cb      点击回调函数
 * @return 创建的按钮对象
 *
 * 按钮尺寸 48x48，修改此值会影响控制台按钮大小。
 */
static lv_obj_t *create_ctrl_btn(lv_obj_t *parent, const char *symbol,
                                  lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    /* 按钮尺寸 48x48 像素，调整此值改变按钮大小 */
    lv_obj_set_size(btn, 48, 48);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x3A3A5C), 0);
    lv_obj_set_style_bg_opa(btn, LV_OPA_80, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_20, 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/**
 * @brief 创建音乐播放器页面
 *
 * 页面全屏，深色渐变背景（后续替换为模糊封面图）。
 * 封面占位 180x180 圆角矩形，位于屏幕中上方。
 * 控制台位于封面下方，含按钮行 + 进度条 + 时长。
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
    /* 允许触摸事件冒泡到父屏幕，以便 ui_manager 的手势检测能收到事件 */
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* ===== 歌曲标题 ===== */
    s_title_lbl = lv_label_create(s_page);
    lv_label_set_text(s_title_lbl, "No Song");
    lv_obj_set_style_text_color(s_title_lbl, lv_color_white(), 0);
    lv_obj_set_style_text_font(s_title_lbl, &lv_font_montserrat_16, 0);
    lv_obj_align(s_title_lbl, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_HEIGHT + 8);

    /* ===== 封面/歌词切换区域 ===== */
    s_cover_area = lv_obj_create(s_page);
    /* 封面区域尺寸 200x200，修改此值会改变封面显示大小 */
    lv_obj_set_size(s_cover_area, 200, 200);
    lv_obj_align(s_cover_area, LV_ALIGN_TOP_MID, 0, UI_STATUSBAR_HEIGHT + 40);
    lv_obj_set_style_bg_opa(s_cover_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_cover_area, 0, 0);
    lv_obj_set_style_pad_all(s_cover_area, 0, 0);
    lv_obj_remove_flag(s_cover_area, LV_OBJ_FLAG_SCROLLABLE);
    /* 封面区域事件冒泡，让手势检测能收到 PRESSED/RELEASED */
    lv_obj_add_flag(s_cover_area, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_cover_area, cover_click_cb, LV_EVENT_CLICKED, NULL);

    /* 封面占位矩形（紫色渐变，后续替换为真实封面图） */
    s_cover_img = lv_obj_create(s_cover_area);
    lv_obj_set_size(s_cover_img, 180, 180);
    lv_obj_center(s_cover_img);
    /* 圆角半径 16px，增大使封面更圆润 */
    lv_obj_set_style_radius(s_cover_img, 16, 0);
    lv_obj_set_style_bg_color(s_cover_img, lv_color_hex(0x4A3080), 0);
    lv_obj_set_style_bg_opa(s_cover_img, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cover_img, 0, 0);
    lv_obj_remove_flag(s_cover_img, LV_OBJ_FLAG_SCROLLABLE);
    /* 封面占位不拦截点击，让事件冒泡到 cover_area 处理封面/歌词切换 */
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* 封面中间放一个音乐符号作为占位 */
    lv_obj_t *cover_icon = lv_label_create(s_cover_img);
    lv_label_set_text(cover_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(cover_icon, lv_color_white(), 0);
    lv_obj_set_style_text_font(cover_icon, &lv_font_montserrat_26, 0);
    lv_obj_center(cover_icon);

    /* 歌词 label（默认隐藏，点击封面区域切换显示） */
    s_lyric_label = lv_label_create(s_cover_area);
    lv_label_set_text(s_lyric_label, "Lyrics will\nappear here");
    lv_obj_set_style_text_color(s_lyric_label, lv_color_hex(0xCCCCCC), 0);
    lv_obj_set_style_text_font(s_lyric_label, &lv_font_montserrat_16, 0);
    lv_obj_set_style_text_align(s_lyric_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_center(s_lyric_label);
    lv_obj_add_flag(s_lyric_label, LV_OBJ_FLAG_HIDDEN);

    /* ===== 播放控制台 ===== */
    s_ctrl_panel = lv_obj_create(s_page);
    /* 控制台宽度占屏幕 85%，高度 140px，修改此值改变控制台尺寸 */
    lv_obj_set_size(s_ctrl_panel, UI_HOR_RES * 85 / 100, 140);
    lv_obj_align(s_ctrl_panel, LV_ALIGN_BOTTOM_MID, 0, -30);
    lv_obj_set_style_radius(s_ctrl_panel, 20, 0);
    lv_obj_set_style_bg_color(s_ctrl_panel, lv_color_hex(0x1A1A3E), 0);
    lv_obj_set_style_bg_opa(s_ctrl_panel, UI_PANEL_BG_OPA, 0);
    lv_obj_set_style_border_width(s_ctrl_panel, 0, 0);
    lv_obj_set_style_pad_all(s_ctrl_panel, 10, 0);
    lv_obj_set_flex_flow(s_ctrl_panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_ctrl_panel, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_ctrl_panel, LV_OBJ_FLAG_SCROLLABLE);
    /* 控制台事件冒泡，让边缘手势能穿透 */
    lv_obj_add_flag(s_ctrl_panel, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 按钮行容器 */
    lv_obj_t *btn_row = lv_obj_create(s_ctrl_panel);
    lv_obj_set_size(btn_row, LV_PCT(100), 56);
    lv_obj_set_style_bg_opa(btn_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn_row, 0, 0);
    lv_obj_set_style_pad_all(btn_row, 0, 0);
    lv_obj_set_flex_flow(btn_row, LV_FLEX_FLOW_ROW);
    /* 按钮间距 20px，增大使按钮间距更宽 */
    lv_obj_set_style_pad_column(btn_row, 20, 0);
    lv_obj_set_flex_align(btn_row, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(btn_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(btn_row, LV_OBJ_FLAG_EVENT_BUBBLE);

    s_btn_prev   = create_ctrl_btn(btn_row, LV_SYMBOL_PREV,    prev_btn_cb);
    s_btn_toggle = create_ctrl_btn(btn_row, LV_SYMBOL_PAUSE,   toggle_btn_cb);
    s_btn_next   = create_ctrl_btn(btn_row, LV_SYMBOL_NEXT,    next_btn_cb);
    s_btn_mode   = create_ctrl_btn(btn_row, LV_SYMBOL_LOOP,    mode_btn_cb);

    /* 进度条行 */
    lv_obj_t *prog_row = lv_obj_create(s_ctrl_panel);
    lv_obj_set_size(prog_row, LV_PCT(100), 30);
    lv_obj_set_style_bg_opa(prog_row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(prog_row, 0, 0);
    lv_obj_set_style_pad_all(prog_row, 0, 0);
    lv_obj_remove_flag(prog_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(prog_row, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 进度条 —— 高度 6px，修改可调节进度条粗细 */
    s_progress = lv_slider_create(prog_row);
    lv_obj_set_width(s_progress, LV_PCT(75));
    lv_obj_set_style_bg_color(s_progress, UI_COLOR_SLIDER_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress, UI_COLOR_SLIDER_ACTIVE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_progress, lv_color_white(), LV_PART_KNOB);
    /* 滑块高度 6px */
    lv_obj_set_style_bg_opa(s_progress, LV_OPA_COVER, LV_PART_MAIN);
    lv_slider_set_range(s_progress, 0, 1000);
    lv_slider_set_value(s_progress, 0, LV_ANIM_OFF);
    lv_obj_align(s_progress, LV_ALIGN_LEFT_MID, 0, 0);

    /* 时长 label */
    s_time_lbl = lv_label_create(prog_row);
    lv_label_set_text(s_time_lbl, "0:00");
    lv_obj_set_style_text_color(s_time_lbl, lv_color_hex(0xAAAAAA), 0);
    lv_obj_set_style_text_font(s_time_lbl, &lv_font_montserrat_12, 0);
    lv_obj_align(s_time_lbl, LV_ALIGN_RIGHT_MID, 0, 0);

    /* ===== 歌曲列表（全屏覆盖，默认隐藏，上滑展开） ===== */
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
    /* 添加几个示例歌曲条目 */
    const char *demo_songs[] = {"Song 1", "Song 2", "Song 3", "Song 4", "Song 5"};
    for (int i = 0; i < 5; i++) {
        lv_obj_t *item = lv_obj_create(s_playlist);
        lv_obj_set_size(item, LV_PCT(100), 44);
        lv_obj_set_style_bg_color(item, lv_color_hex(0x1A1A3E), 0);
        lv_obj_set_style_bg_opa(item, LV_OPA_80, 0);
        lv_obj_set_style_radius(item, 10, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);

        lv_obj_t *lbl = lv_label_create(item);
        lv_label_set_text(lbl, demo_songs[i]);
        lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
        lv_obj_set_style_text_font(lbl, &lv_font_montserrat_16, 0);
        lv_obj_align(lbl, LV_ALIGN_LEFT_MID, 8, 0);
    }
    lv_obj_add_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 获取音乐播放器页面容器
 * @return lv_obj_t* 页面容器
 */
lv_obj_t *ui_music_player_get_page(void)
{
    return s_page;
}

/**
 * @brief 歌曲列表滑入/滑出动画的 Y 坐标执行回调
 */
static void playlist_anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

/**
 * @brief 歌曲列表隐藏动画完成回调
 */
static void playlist_hide_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_playlist, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 显示/隐藏歌曲列表（带滑入滑出动画）
 * @param show true=从底部滑入展开, false=向底部滑出收起
 */
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

/**
 * @brief 刷新播放进度、时间显示和播放/暂停图标
 * @param current_sec 当前播放秒数
 * @param total_sec   总时长秒数
 * @param is_playing  是否正在播放
 */
void ui_music_player_update_progress(uint32_t current_sec, uint32_t total_sec,
                                     bool is_playing)
{
    if (!s_progress || !s_time_lbl) return;

    /* 更新进度条 (范围 0-1000) */
    int32_t val = 0;
    if (total_sec > 0) {
        val = (int32_t)(current_sec * 1000 / total_sec);
    }
    lv_slider_set_value(s_progress, val, LV_ANIM_ON);

    /* 更新时长文字 MM:SS */
    char buf[16];
    snprintf(buf, sizeof(buf), "%lu:%02lu",
             (unsigned long)(total_sec / 60), (unsigned long)(total_sec % 60));
    lv_label_set_text(s_time_lbl, buf);

    /* 更新播放/暂停图标 */
    lv_obj_t *icon = lv_obj_get_child(s_btn_toggle, 0);
    if (icon) {
        lv_label_set_text(icon, is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
    }
}

/**
 * @brief 设置歌曲标题
 * @param title 歌曲标题字符串
 */
void ui_music_player_set_song(const char *title)
{
    if (s_title_lbl && title) {
        lv_label_set_text(s_title_lbl, title);
    }
}

/**
 * @brief 注册播放控制回调函数
 * @param on_prev  上一首
 * @param on_next  下一首
 * @param on_toggle 暂停/继续
 * @param on_mode  播放模式切换
 */
void ui_music_player_set_callbacks(ui_music_ctrl_cb_t on_prev,
                                   ui_music_ctrl_cb_t on_next,
                                   ui_music_ctrl_cb_t on_toggle,
                                   ui_music_ctrl_cb_t on_mode)
{
    s_cb_prev   = on_prev;
    s_cb_next   = on_next;
    s_cb_toggle = on_toggle;
    s_cb_mode   = on_mode;
}
