/**
 * @file ui_music_player.c
 * @brief 音乐播放器页面实现（重构版）
 *
 * 页面结构：
 *   s_page (全屏容器)
 *     ├─ s_background_base / overlay  全屏模糊背景与淡变层（不交互）
 *     ├─ s_title_lbl   歌曲标题（与歌曲列表联动）
 *     ├─ s_cover_area  封面/歌词切换区域 (COVER_SIZE x COVER_SIZE 正方形)
 *     ├─ s_ctrl_panel  透明播放控制台 (屏幕等宽，底边对齐屏幕底部)
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
#include "../music_background.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "ui_fonts.h"

/* ---- 封面正方形尺寸 ---- */
#define COVER_SIZE      240
/* 封面/歌词使用短促的分段淡变；与左右切歌的 350ms 动画互相独立。 */
#define CONTENT_FADE_OUT_MS  70U
#define CONTENT_FADE_IN_MS   90U
#define BACKGROUND_FADE_MS   180U
_Static_assert(UI_HOR_RES == MUSIC_BACKGROUND_WIDTH && UI_VER_RES == MUSIC_BACKGROUND_HEIGHT,
               "Background buffers must match the music page");
/* ---- 控制台宽度（与屏幕等宽） ---- */
#define CTRL_PANEL_W    UI_HOR_RES
/* 标题与封面/歌词整体下移，仍保留到控制按键（含扩展点击区）的间隙。
 * 封面与歌词共用父容器，移动父容器即可同步绘制、点击和滚动命中区域；
 * 滑出快照在切歌时复制父容器的位置，不另设一组容易失配的 Y 坐标。 */
#define MUSIC_CONTENT_Y_OFFSET   16
#define MUSIC_PROGRESS_HEIGHT    8
#define MUSIC_PROGRESS_LEFT_PAD  20 /* 原为 10px，保持长度与时长位置不变。 */
/* 控制台仅保留透明的布局/事件容器，按键保留独立的半透明白色背景。
 * 只改变背景，不能用整体 opa，否则按键、时间和进度也会一起消失。 */
#define CTRL_PANEL_BG_OPA        LV_OPA_TRANSP
#define CTRL_BTN_BG_OPA          LV_OPA_20
#define CTRL_BTN_ACTIVE_BG_OPA   LV_OPA_40
#define CTRL_BTN_PRESSED_BG_OPA  LV_OPA_30
#define CTRL_BTN_ACTIVE_PRESSED_BG_OPA LV_OPA_50


/* ---- 内部对象 ---- */
static lv_obj_t *s_page        = NULL;
/* 两张全屏 Canvas 固定在所有前景控件之下，不接受点击/滚动。
 * 当前底图不透明，仅新图做一次透明度混合，避免交叉淡变中途变暗。
 * pending 只保留最新待显示图；三份引用对应固定三个后台缓存槽。 */
static lv_obj_t *s_background_base;
static lv_obj_t *s_background_overlay;
static const uint8_t *s_background_current;
static const uint8_t *s_background_incoming;
static const uint8_t *s_background_pending;
static uint32_t s_background_pending_index;
static lv_obj_t *s_cover_area  = NULL;
static lv_obj_t *s_cover_img   = NULL;
static lv_obj_t *s_cover_canvas = NULL;
/* 滑出层只保存上一首的显示快照；当前层可立即接收下一首的数据。
 * 两层独立持有封面引用，后台预读不得复用仍在动画中的像素缓冲。 */
static lv_obj_t *s_outgoing_area = NULL;
static lv_obj_t *s_outgoing_canvas = NULL;
static lv_obj_t *s_outgoing_lyrics = NULL;
static const uint8_t *s_cover_pixels;
static const uint8_t *s_outgoing_pixels;
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
static lv_obj_t *s_playlist_empty_lbl = NULL;

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

/* s_show_lyric 为最近点击的目标模式，s_content_lyric 为当前实际绘制模式。
 * 淡出结束才更换实际模式，连续点击可从当前透明度反向，无需排队或复制内容。 */
static bool s_show_lyric  = false;
static bool s_content_lyric = false;
static int32_t s_content_opa = LV_OPA_COVER;
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
static bool s_cover_swipe_suppressed = false;
static lv_point_t s_cover_press_point; /**< 按下坐标：在 CLICKED 前识别拖动，避免一次操作触发两种动作。 */

/* ---- 前向声明 ---- */
static void background_opa_cb(void *obj, int32_t opa);
static uint32_t ui_song_count(void);
static void content_fade_start(void);
static void switch_to_song(int idx);
static bool switch_to_song_animated(int idx, bool slide_left);
static void update_playlist_highlight(void);
static void playlist_create_empty_label(void);
static void lyric_press_cb(lv_event_t *e);
static void lyric_release_cb(lv_event_t *e);
static void lyric_scroll_cb(lv_event_t *e);
static int32_t lyric_center_padding(void);
static void player_timer_cb(lv_timer_t *timer);
static bool request_song(uint32_t index);
static lv_obj_t *playlist_item_title(lv_obj_t *item);
static lv_obj_t *playlist_item_icon(lv_obj_t *item);

/* 主控制台和当前歌曲条目共用同一图标规则，避免两处各自取反。
 * 图标表示可执行的操作：播放中可以暂停，暂停中可以继续播放。 */
static const char *playback_action_symbol(void)
{
    return s_is_playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
}

static void background_opa_cb(void *obj, int32_t opa)
{
    lv_obj_set_style_image_opa((lv_obj_t *)obj, (lv_opa_t)opa, 0);
}

static uint32_t ui_song_count(void)
{
    return s_library_count;
}

static void background_clear_pending(void)
{
    music_background_release(s_background_pending);
    s_background_pending = NULL;
}

static void background_fade_ready(lv_anim_t *a)
{
    (void)a;
    /* 新图已经全不透明：先把它接管到底层，再隐藏上层并释放旧图。
     * 固定两个控件的前后顺序，避免误把背景移动到歌名、歌词或控制台之上。 */
    const uint8_t *old = s_background_current;
    s_background_current = s_background_incoming;
    s_background_incoming = NULL;
    lv_canvas_set_buffer(s_background_base, (void *)s_background_current,
                         UI_HOR_RES, UI_VER_RES, LV_COLOR_FORMAT_RGB565);
    lv_obj_remove_flag(s_background_base, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_background_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_background_base);
    music_background_release(old);

    /* 不在半透明帧强制换图。当前过渡完成后再接续最新背景，最多等待
     * 一段 180ms；较旧的 pending 会被新请求替换，歌曲切换始终不等待。 */
    const uint8_t *pending = s_background_pending;
    uint32_t index = s_background_pending_index;
    s_background_pending = NULL;
    if (pending) {
        ui_music_player_set_background(index, pending);
        music_background_release(pending);
    }
}

void ui_music_player_set_background(uint32_t index, const uint8_t *pixels)
{
    if (!ui_music_player_can_show_song(index)) return;
    if (!pixels || pixels == s_background_incoming ||
        (!s_background_incoming && pixels == s_background_current)) {
        /* 新选歌未命中缓存：只撤销待显示图，保留已显示/过渡中的背景。
         * 缺图、慢卡和后台生成均不会让歌曲或封面切换等待。 */
        background_clear_pending();
        return;
    }
    music_background_retain(pixels); /* 参数借用，显示/等待各自独立持有。 */
    if (s_background_incoming) {
        background_clear_pending();
        s_background_pending = pixels;
        s_background_pending_index = index;
        return;
    }
    s_background_incoming = pixels;
    lv_canvas_set_buffer(s_background_overlay, (void *)pixels, UI_HOR_RES, UI_VER_RES,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_set_style_image_opa(s_background_overlay, LV_OPA_TRANSP, 0);
    lv_obj_remove_flag(s_background_overlay, LV_OBJ_FLAG_HIDDEN);
    lv_obj_invalidate(s_background_overlay);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_background_overlay);
    lv_anim_set_values(&a, LV_OPA_TRANSP, LV_OPA_COVER);
    lv_anim_set_duration(&a, BACKGROUND_FADE_MS);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, background_opa_cb);
    lv_anim_set_completed_cb(&a, background_fade_ready);
    lv_anim_start(&a);
}

static void background_reset(void)
{
    /* 拔卡/重新挂载时隐藏控件并取消回调，再释放像素；后台缓存失效
     * 只撤销键值，不影响仍被引用的缓冲，避免显示与生成任务互相覆盖。 */
    lv_anim_delete(s_background_overlay, background_opa_cb);
    lv_obj_add_flag(s_background_base, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_background_overlay, LV_OBJ_FLAG_HIDDEN);
    music_background_release(s_background_current);
    music_background_release(s_background_incoming);
    s_background_current = NULL;
    s_background_incoming = NULL;
    background_clear_pending();
}

/* 播放/循环状态统一使用白色背景，通过不透明度区分状态。
 * 点击回调和后端状态同步都走此入口，防止同步后又恢复旧的紫色/深色。 */
static void set_ctrl_btn_active(lv_obj_t *btn, bool active)
{
    lv_obj_set_style_bg_opa(btn, active ? CTRL_BTN_ACTIVE_BG_OPA : CTRL_BTN_BG_OPA, 0);
    lv_obj_set_style_bg_opa(btn, active ? CTRL_BTN_ACTIVE_PRESSED_BG_OPA : CTRL_BTN_PRESSED_BG_OPA,
                             LV_STATE_PRESSED);
}

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
 * 播放中：提高白色背景的不透明度，图标为 PAUSE
 * 暂停中：恢复较通透的白色背景，图标为 PLAY
 */
static void toggle_btn_cb(lv_event_t *e)
{
    (void)e;
    /* 用户可能在下一次200ms状态刷新前再次点击，或在首帧封面发布前点击。
     * 从后端已接受的请求取反，不能依赖可能过期的本地按钮状态。 */
    bool requested=s_backend.is_play_requested?s_backend.is_play_requested():s_is_playing;
    s_is_playing = !requested;
    lv_obj_t *icon = lv_obj_get_child(s_btn_toggle, 0);
    if (icon) {
        lv_label_set_text(icon, playback_action_symbol());
    }
    set_ctrl_btn_active(s_btn_toggle, s_is_playing);
    /* 当前条目与控制台共用播放/暂停图标。局部点击也要立即刷新，
     * 不能等待后端定时器，因为此时 s_is_playing 已更新。 */
    update_playlist_highlight();
    /* 只提交目标状态；DAC/I2S在后台执行。加载中点击“播放”仍然是播放
     * 请求，不反转后台已经排队但尚未开始输出的状态。既有按钮样式不变。 */
    if (s_backend.set_playing) s_backend.set_playing(s_is_playing);
    else if (s_cb_toggle) s_cb_toggle();
}

/**
 * @brief 播放模式按钮点击 —— 在单曲循环和顺序播放间切换
 *
 * 单曲循环：图标 LOOP，提高白色背景的不透明度
 * 顺序播放：图标 NEXT，恢复较通透的白色背景
 */
static void mode_btn_cb(lv_event_t *e)
{
    (void)e;
    s_single_loop = !s_single_loop;
    lv_obj_t *icon = lv_obj_get_child(s_btn_mode, 0);
    if (icon) {
        lv_label_set_text(icon, s_single_loop ? LV_SYMBOL_LOOP : LV_SYMBOL_NEXT);
    }
    set_ctrl_btn_active(s_btn_mode, s_single_loop);
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

/* 仅修改当前绘制对象的图片/文字透明度：复用现有像素和 Label，不对容器
 * 使用 opa_layered，不创建中间图层，不改变歌词滚动位置或命中范围。 */
static void content_fade_exec(void *obj, int32_t opa)
{
    (void)obj;
    s_content_opa = opa;
    if (s_content_lyric)
        lv_obj_set_style_text_opa(s_lyric_label, (lv_opa_t)opa, 0);
    else
        lv_obj_set_style_image_opa(s_cover_canvas, (lv_opa_t)opa, 0);
}

static void content_fade_ready(lv_anim_t *a)
{
    (void)a;
    if (s_content_lyric == s_show_lyric) return; /* 淡入完成。 */
    /* 旧对象已完全透明：此时才隐藏它，再以透明状态显示目标对象。
     * 两种内容不同时混合绘制，且两段之间不插入等待。 */
    lv_obj_add_flag(s_content_lyric ? s_lyric_scroll : s_cover_img, LV_OBJ_FLAG_HIDDEN);
    s_content_lyric = s_show_lyric;
    content_fade_exec(NULL, LV_OPA_TRANSP);
    lv_obj_remove_flag(s_content_lyric ? s_lyric_scroll : s_cover_img, LV_OBJ_FLAG_HIDDEN);
    content_fade_start();
}

static void content_fade_start(void)
{
    /* 专用 exec 回调区分透明度和 X 坐标动画，删除时不会误删切歌动画。
     * 连续点击始终从现有透明度转向最近目标，已走过的进度不重播。 */
    lv_anim_delete(s_cover_area, content_fade_exec);
    bool fade_out = s_content_lyric != s_show_lyric;
    int32_t end = fade_out ? LV_OPA_TRANSP : LV_OPA_COVER;
    uint32_t distance = (uint32_t)abs(end - s_content_opa);
    if (!distance) {
        if (fade_out) content_fade_ready(NULL);
        return;
    }
    uint32_t full_duration = fade_out ? CONTENT_FADE_OUT_MS : CONTENT_FADE_IN_MS;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_cover_area);
    lv_anim_set_values(&a, s_content_opa, end);
    /* 部分进度反向时按剩余透明度缩短时长，向上取整保证至少 1ms。 */
    lv_anim_set_duration(&a, (full_duration * distance + LV_OPA_COVER - 1) / LV_OPA_COVER);
    lv_anim_set_path_cb(&a, lv_anim_path_linear);
    lv_anim_set_exec_cb(&a, content_fade_exec);
    lv_anim_set_completed_cb(&a, content_fade_ready);
    lv_anim_start(&a);
}

static void content_transition_finish(void)
{
    /* 切歌/重建曲库优先：结束淡变并落实最新显示模式，再拍摄滑出快照。
     * 两个对象都恢复全不透明，防止残留半透明影响后续封面或歌词发布。 */
    lv_anim_delete(s_cover_area, content_fade_exec);
    s_content_lyric = s_show_lyric;
    s_content_opa = LV_OPA_COVER;
    lv_obj_set_style_image_opa(s_cover_canvas, LV_OPA_COVER, 0);
    lv_obj_set_style_text_opa(s_lyric_label, LV_OPA_COVER, 0);
    lv_obj_add_flag(s_show_lyric ? s_cover_img : s_lyric_scroll, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_show_lyric ? s_lyric_scroll : s_cover_img, LV_OBJ_FLAG_HIDDEN);
}

static void cover_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_cover_swipe_suppressed || s_cover_animating) {
        s_cover_swipe_suppressed = false;
        return;
    }
    s_show_lyric = !s_show_lyric;
    content_fade_start();
}

/* 条目子对象固定为 [图标, 歌名]。所有读取集中在这两个辅助函数，
 * 避免新增图标后仍把 child(0) 当成歌名，导致标题或选歌内容错乱。 */
static lv_obj_t *playlist_item_icon(lv_obj_t *item)
{
    return item ? lv_obj_get_child(item, 0) : NULL;
}

static lv_obj_t *playlist_item_title(lv_obj_t *item)
{
    return item ? lv_obj_get_child(item, 1) : NULL;
}

/** @brief 歌曲列表条目点击 —— 沿用原有选歌和后台读取逻辑。 */
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
    content_transition_finish();
    s_current_idx = idx;
    /* 选歌立即生效：歌名来自已显示的列表，无需等待 SD。先清空上一首的
     * 歌词/时间/封面；缓存命中会在 select 回调内立即补上新封面。 */
    const char *title = "加载中...";
    if ((uint32_t)idx < s_pl_item_count) {
        lv_obj_t *title_obj = playlist_item_title(s_pl_items[idx]);
        if (title_obj) title = lv_label_get_text(title_obj);
    }
    lv_label_set_text(s_title_lbl, title);
    ui_music_player_set_cover((uint32_t)idx, NULL);
    ui_music_player_set_lyrics("", 0, false);
    ui_music_player_update_progress(0, 0, false);
    update_playlist_highlight();
}

/** @brief 封面 X 坐标动画执行回调 */
static void cover_anim_x_cb(void *obj, int32_t v)
{
    lv_obj_set_x((lv_obj_t *)obj, v);
}

/* 滑出层完成后先隐藏，再释放旧图；任何时候都不修改旧层的图像内容。
 * 新层动画不等待 SD/JPEG/LRC，未加载的图片仅留空，完成后原位补显。 */
static void cover_slide_out_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_outgoing_area, LV_OBJ_FLAG_HIDDEN);
    cover_loader_release(s_outgoing_pixels);
    s_outgoing_pixels = NULL;
    lv_label_set_text(s_outgoing_lyrics, "");
}

static void cover_slide_in_ready(lv_anim_t *a)
{
    (void)a;
    s_cover_animating = false;
}

/* 复制旧画面的可见内容，而不是替换当前 canvas 的像素。歌词快照保留
 * 字号、行距、透明背景和手动滚动位置，仅在切歌动画期间拥有文本副本。 */
static void snapshot_outgoing(void)
{
    s_outgoing_pixels = s_cover_pixels; /* 转移持有引用，不提前释放。 */
    s_cover_pixels = NULL;
    lv_obj_set_pos(s_outgoing_area, lv_obj_get_x(s_cover_area), lv_obj_get_y(s_cover_area));
    lv_obj_remove_flag(s_outgoing_area, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_outgoing_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_add_flag(s_outgoing_lyrics, LV_OBJ_FLAG_HIDDEN);
    if (s_show_lyric) {
        lv_label_set_text(s_outgoing_lyrics, lv_label_get_text(s_lyric_label));
        lv_obj_set_style_text_font(s_outgoing_lyrics,
                                  lv_obj_get_style_text_font(s_lyric_label, 0), 0);
        lv_obj_set_style_text_line_space(s_outgoing_lyrics,
                                        lv_obj_get_style_text_line_space(s_lyric_label, 0), 0);
        lv_obj_set_style_pad_top(s_outgoing_lyrics,
                                lv_obj_get_style_pad_top(s_lyric_label, 0), 0);
        lv_obj_set_pos(s_outgoing_lyrics, 8,
                      lv_obj_get_y(s_lyric_label) - lv_obj_get_scroll_y(s_lyric_scroll));
        lv_obj_remove_flag(s_outgoing_lyrics, LV_OBJ_FLAG_HIDDEN);
    } else if (s_outgoing_pixels) {
        lv_canvas_set_buffer(s_outgoing_canvas, (void *)s_outgoing_pixels,
                             COVER_SIZE, COVER_SIZE, LV_COLOR_FORMAT_RGB565);
        lv_obj_remove_flag(s_outgoing_canvas, LV_OBJ_FLAG_HIDDEN);
    }
}

static bool switch_to_song_animated(int idx, bool slide_left)
{
    if (idx < 0 || idx >= (int)ui_song_count() || s_cover_animating) return false;
    s_cover_animating = true;
    content_transition_finish(); /* 旧快照必须采用完整、确定的目标显示状态。 */
    snapshot_outgoing();
    switch_to_song(idx);

    /* 两段沿用原时长和曲线，同一帧启动；动画生命周期只由时长决定，
     * 不与媒体就绪绑定。慢卡、缺封面、缺歌词均不阻塞下一次切歌。 */
    lv_anim_t a_out;
    lv_anim_init(&a_out);
    lv_anim_set_var(&a_out, s_outgoing_area);
    lv_anim_set_values(&a_out, lv_obj_get_x(s_outgoing_area),
                       slide_left ? -(int32_t)UI_HOR_RES : (int32_t)UI_HOR_RES);
    lv_anim_set_duration(&a_out, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a_out, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a_out, cover_anim_x_cb);
    lv_anim_set_completed_cb(&a_out, cover_slide_out_ready);
    lv_anim_start(&a_out);

    int32_t in_start = slide_left ? (int32_t)UI_HOR_RES : -(int32_t)UI_HOR_RES;
    lv_obj_set_x(s_cover_area, in_start);
    lv_anim_t a_in;
    lv_anim_init(&a_in);
    lv_anim_set_var(&a_in, s_cover_area);
    lv_anim_set_values(&a_in, in_start, ((int32_t)UI_HOR_RES - COVER_SIZE) / 2);
    lv_anim_set_duration(&a_in, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a_in, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a_in, cover_anim_x_cb);
    lv_anim_set_completed_cb(&a_in, cover_slide_in_ready);
    lv_anim_start(&a_in);

    /* 此回调只查内存缓存并投递 worker 请求；动画已启动，不能在此等文件。 */
    request_song((uint32_t)idx);
    return true;
}

/**
 * @brief 刷新歌曲列表中的高亮状态
 * 当前播放歌曲使用白色主色背景和黑色文字，其余恢复灰底白字。
 */
static void update_playlist_highlight(void)
{
    for (uint32_t i = 0; i < s_pl_item_count; i++) {
        if (!s_pl_items[i]) continue;
        lv_obj_t *icon = playlist_item_icon(s_pl_items[i]);
        lv_obj_t *title = playlist_item_title(s_pl_items[i]);
        /* 仅当前且正在播放的歌曲显示 PAUSE（双竖线），与主控制台同步；
         * 当前歌曲已暂停以及其余所有歌曲均显示 PLAY（小三角）。
         * 这里只更新图标文字及原有选中色，不改变条目布局或点击选歌逻辑。 */
        if (icon) {
            lv_label_set_text(icon, i == (uint32_t)s_current_idx ?
                                   playback_action_symbol() : LV_SYMBOL_PLAY);
            lv_obj_set_style_text_color(icon,
                i == (uint32_t)s_current_idx ? UI_COLOR_ON_ACCENT : UI_COLOR_TEXT, 0);
        }
        if (i == (uint32_t)s_current_idx) {
            if (title) lv_obj_set_style_text_color(title, UI_COLOR_ON_ACCENT, 0);
            lv_obj_set_style_bg_color(s_pl_items[i], UI_COLOR_ACCENT, 0);
            lv_obj_set_style_bg_opa(s_pl_items[i], LV_OPA_COVER, 0);
        }
        else {
            if (title) lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
            lv_obj_set_style_bg_color(s_pl_items[i], UI_COLOR_LIST_ROW_BG, 0);
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
 * 按钮尺寸 72x72，增大此值使按钮更大更易点击
 */
static lv_obj_t *create_ctrl_btn(lv_obj_t *parent, const char *symbol,
                                  lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 72, 72);
    lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(btn, UI_COLOR_TEXT, 0);
    /* 覆盖主题按压时的黑色重着色，改用白色背景加深来保留触摸反馈。
     * 保留主题原有的状态过渡，不修改按钮尺寸、点击范围或事件逻辑。 */
    lv_obj_set_style_recolor_opa(btn, LV_OPA_TRANSP, LV_STATE_PRESSED);
    set_ctrl_btn_active(btn, false);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 10);

    lv_obj_t *lbl = lv_label_create(btn);
    lv_label_set_text(lbl, symbol);
    lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(lbl, &lv_font_montserrat_24, 0);
    lv_obj_center(lbl);

    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/**
 * @brief 创建音乐播放器页面
 *
 * 封面/歌词保持 240×240，标题与该区域同步下移；控制台保持屏幕等宽且透明。
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
    lv_obj_set_style_bg_color(s_page, UI_COLOR_PAGE_BG, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 背景最先创建，固定在所有现有内容下方；460×460 像素已由 worker
     * 完成模糊、白色遮罩混合和放大，UI 只显示，不逐帧做图像处理。
     * 禁止命中与滚动，让原有页面边缘手势、封面点击和歌词拖动照常工作。 */
    s_background_base = lv_canvas_create(s_page);
    s_background_overlay = lv_canvas_create(s_page);
    lv_obj_t *backgrounds[] = {s_background_base, s_background_overlay};
    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_set_size(backgrounds[i], UI_HOR_RES, UI_VER_RES);
        lv_obj_set_pos(backgrounds[i], 0, 0);
        lv_obj_remove_flag(backgrounds[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(backgrounds[i], LV_OBJ_FLAG_HIDDEN);
    }

    /* ===== 歌曲标题 —— 默认显示第一首测试歌曲 ===== */
    s_title_lbl = lv_label_create(s_page);
    lv_label_set_text(s_title_lbl, "等待插入 SD 卡");
    lv_obj_set_style_text_color(s_title_lbl, UI_COLOR_TEXT, 0);
    /* 顶部动态歌曲名与歌词统一使用 18 号字库，避免为歌曲名再维护
     * 一套完整的 16 号中文字库；位置、宽度、居中和横向滚动规则不变。 */
    lv_obj_set_style_text_font(s_title_lbl, ui_font_18(), 0);
    /* 保持原标题宽度与横向滚动规则，通过统一偏移与封面/歌词同步下移。 */
    lv_obj_set_width(s_title_lbl, UI_HOR_RES - 32);
    /* 固定的是标题视口宽度：短歌名按视口居中，溢出时 LVGL 自动从左侧滚动。
     * 不要让标签宽度随文本伸长，否则短标题/长标题会使用不同的对齐基准。 */
    lv_obj_set_style_text_align(s_title_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_title_lbl, LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_obj_align(s_title_lbl, LV_ALIGN_TOP_MID, 0,
                 UI_STATUSBAR_HEIGHT + 6 + MUSIC_CONTENT_Y_OFFSET);

    /* ===== 封面/歌词切换区域（正方形 COVER_SIZE x COVER_SIZE） ===== */
    s_cover_area = lv_obj_create(s_page);
    lv_obj_set_size(s_cover_area, COVER_SIZE, COVER_SIZE);
    lv_obj_set_pos(s_cover_area, ((int32_t)UI_HOR_RES - (int32_t)COVER_SIZE) / 2,
                   UI_STATUSBAR_HEIGHT + 30 + MUSIC_CONTENT_Y_OFFSET);
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
    lv_obj_set_style_bg_color(s_cover_img, UI_COLOR_CONTROL_BG, 0);
    lv_obj_set_style_bg_opa(s_cover_img, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cover_img, 0, 0);
    lv_obj_remove_flag(s_cover_img, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_cover_img, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* Canvas 承接后台缓存中已解码的像素，不改变原封面容器的位置和动画。 */
    s_cover_canvas = lv_canvas_create(s_cover_img);
    lv_obj_set_size(s_cover_canvas, COVER_SIZE, COVER_SIZE);
    lv_obj_center(s_cover_canvas);
    lv_obj_add_flag(s_cover_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_cover_canvas, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *cover_icon = lv_label_create(s_cover_img);
    lv_label_set_text(cover_icon, LV_SYMBOL_AUDIO);
    lv_obj_set_style_text_color(cover_icon, UI_COLOR_TEXT, 0);
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
    lv_label_set_text(s_lyric_label, "歌词将在这里显示");
    lv_obj_set_width(s_lyric_label, COVER_SIZE - 16);
    lv_obj_set_style_text_color(s_lyric_label, UI_COLOR_TEXT_SECONDARY, 0);
    /* 歌词单独使用 18 号字体，并增加 4 像素行间距，改善阅读体验。
     * 必须在计算上下居中留白前设置字体；自动回跟通过实际字形坐标定位，
     * 会同时考虑新字号、行间距及长句换行，无需维护固定的歌词行高。 */
    lv_obj_set_style_text_font(s_lyric_label, ui_font_18(), 0);
    lv_obj_set_style_text_line_space(s_lyric_label, 4, 0);
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

    /* 固定的滑出快照层与原封面区域同尺寸、同位置，不增加可操作控件。
     * 禁止点击/滚动，避免新旧层重叠时干扰原有手势；父容器裁剪歌词。 */
    s_outgoing_area = lv_obj_create(s_page);
    lv_obj_set_size(s_outgoing_area, COVER_SIZE, COVER_SIZE);
    lv_obj_set_style_bg_opa(s_outgoing_area, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_outgoing_area, 0, 0);
    lv_obj_set_style_pad_all(s_outgoing_area, 0, 0);
    lv_obj_remove_flag(s_outgoing_area, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_outgoing_area, LV_OBJ_FLAG_HIDDEN);
    s_outgoing_canvas = lv_canvas_create(s_outgoing_area);
    lv_obj_set_size(s_outgoing_canvas, COVER_SIZE, COVER_SIZE);
    lv_obj_remove_flag(s_outgoing_canvas, LV_OBJ_FLAG_CLICKABLE);
    s_outgoing_lyrics = lv_label_create(s_outgoing_area);
    lv_label_set_text(s_outgoing_lyrics, "");
    lv_obj_set_width(s_outgoing_lyrics, COVER_SIZE - 16);
    lv_label_set_long_mode(s_outgoing_lyrics, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(s_outgoing_lyrics, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_align(s_outgoing_lyrics, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(s_outgoing_lyrics, LV_OBJ_FLAG_CLICKABLE);

    /* ===== 播放控制台（与屏幕等宽，底边对齐屏幕底部） ===== */
    s_ctrl_panel = lv_obj_create(s_page);
    lv_obj_set_size(s_ctrl_panel, CTRL_PANEL_W, 120);
    lv_obj_align(s_ctrl_panel, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_obj_set_style_radius(s_ctrl_panel, 0, 0);
    /* 取消控制台底板，只保留原容器尺寸、Flex 排列和手势冒泡。
     * 子按键的白色背景仍独立绘制，页面模糊背景可直接透出。 */
    lv_obj_set_style_bg_color(s_ctrl_panel, UI_COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(s_ctrl_panel, CTRL_PANEL_BG_OPA, 0);
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
    /* 加粗至 8px 并右移 10px；仍在原进度条行内垂直居中。
     * 长度和右侧时间位置保持不变，二者之间仍保留间隔。 */
    lv_obj_set_size(s_progress, LV_PCT(78), MUSIC_PROGRESS_HEIGHT);
    /* 进度条也沿用控制台的白色配色，通过轨道/已播放段的明暗区分。 */
    lv_obj_set_style_bg_color(s_progress, UI_COLOR_TEXT, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_progress, UI_COLOR_TEXT, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress, LV_OPA_80, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_progress, LV_OPA_20, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress, MUSIC_PROGRESS_HEIGHT / 2, LV_PART_MAIN);
    lv_obj_set_style_radius(s_progress, MUSIC_PROGRESS_HEIGHT / 2, LV_PART_INDICATOR);
    lv_bar_set_range(s_progress, 0, 1000);
    lv_bar_set_value(s_progress, 0, LV_ANIM_OFF);
    lv_obj_align(s_progress, LV_ALIGN_LEFT_MID, MUSIC_PROGRESS_LEFT_PAD, 0);

    /* 时长 label */
    s_time_lbl = lv_label_create(prog_row);
    lv_label_set_text(s_time_lbl, "0:00");
    lv_obj_set_style_text_color(s_time_lbl, UI_COLOR_TEXT, 0);
    /* 时长由 12 号增至 16 号，提高背景上的可读性；仍在原来的
     * 24px 高进度行中垂直居中、右侧对齐，保留与左侧进度条的间距。 */
    lv_obj_set_style_text_font(s_time_lbl, ui_font_16(), 0);
    lv_obj_align(s_time_lbl, LV_ALIGN_RIGHT_MID, -10, 0);

    /* ===== 歌曲列表（全屏，可上下滚动，默认隐藏） ===== */
    s_playlist = lv_obj_create(s_page);
    lv_obj_set_size(s_playlist, UI_HOR_RES, UI_VER_RES);
    lv_obj_align(s_playlist, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_playlist, UI_COLOR_PAGE_BG, 0);
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
    playlist_create_empty_label();

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

static void playlist_create_empty_label(void)
{
    /* 空状态不参加列表 flex 排版，也不接收点击/滚动，防止占用歌曲行
     * 空间或截获原有收起手势。开机尚未挂载 SD 时也必须立即可见；
     * 后续重扫由 set_library 更新文案，并在有真实条目时隐藏它。 */
    s_playlist_empty_lbl = lv_label_create(s_playlist);
    lv_obj_add_flag(s_playlist_empty_lbl, LV_OBJ_FLAG_IGNORE_LAYOUT);
    lv_obj_set_width(s_playlist_empty_lbl, UI_HOR_RES - 32);
    lv_label_set_text(s_playlist_empty_lbl, "请插入 SD 卡");
    lv_obj_set_style_text_color(s_playlist_empty_lbl, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_playlist_empty_lbl, ui_font_18(), 0);
    lv_obj_set_style_text_align(s_playlist_empty_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(s_playlist_empty_lbl, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(s_playlist_empty_lbl, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
}

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
        if (icon) lv_label_set_text(icon, playback_action_symbol());
        set_ctrl_btn_active(s_btn_toggle, s_is_playing);
        update_playlist_highlight();
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
    /* 淡出中的歌词仍可被触摸：按实际可见对象判定，避免切回封面途中
     * 在歌词上开始的竖向拖动意外触发列表展开。
     * 命中直接取移动后对象的屏幕坐标，不能复用下移前的固定坐标范围。 */
    if (!s_content_lyric || !s_lyric_scroll ||
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
    /* 当前索引在动画开始时就已更新。媒体可在滑入途中或动画结束后
     * 发布，但旧歌曲的迟到结果不能改写当前层，更不能碰滑出快照层。 */
    return index == (uint32_t)s_current_idx;
}

void ui_music_player_restore_selection(uint32_t index)
{
    if (index >= s_library_count) return;
    /* 元数据/封面发布均检查 s_current_idx；必须在 worker 读取前同步，
     * 否则恢复非零歌曲会被当成迟到数据而丢弃。列表重建已经清理旧动画，
     * 这里只改选择与条目状态，保持页面位置、歌词手势及播放控制台不变。 */
    s_current_idx = (int)index;
    update_playlist_highlight();
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
    background_reset();
    content_transition_finish();
    /* 拔卡/重扫可发生在两层同时运动时：先停止动画并隐藏快照，
     * 然后释放两份显示引用，缓存失效时仍不会留下可绘制的旧卡像素。 */
    lv_anim_delete(s_cover_area, cover_anim_x_cb);
    lv_anim_delete(s_outgoing_area, cover_anim_x_cb);
    cover_slide_out_ready(NULL);
    ui_music_player_set_cover((uint32_t)s_current_idx, NULL);
    s_cover_animating = false;
    lv_obj_set_x(s_cover_area, ((int32_t)UI_HOR_RES - COVER_SIZE) / 2);
    lv_obj_add_flag(s_cover_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(lv_obj_get_child(s_cover_img, 1), LV_OBJ_FLAG_HIDDEN);
    ui_music_player_set_lyrics("", 0, false);
    ui_music_player_update_progress(0, 0, false);
    lv_obj_clean(s_playlist);
    s_playlist_empty_lbl = NULL;
    playlist_create_empty_label();
    /* 指针数组和条目对象使用同一个LVGL分配器；固件分配器固定在PSRAM，
     * 不让随歌曲数量增长的小块UI内存挤占JPEG/I2S内部DMA空间。
     * 重建范围和列表几何/滚动/点击回调完全保持原实现。 */
    lv_free(s_pl_items);
    s_pl_items = NULL;
    s_pl_item_count = 0;
    s_library_count = count;
    s_current_idx = 0;

    if (count > 0) {
        s_pl_items = lv_calloc(count, sizeof(*s_pl_items));
        if (!s_pl_items) {
            s_library_count = 0;
            lv_label_set_text(s_title_lbl, "歌曲列表内存错误");
        } else {
            for (uint32_t i = 0; i < count; ++i) {
                lv_obj_t *item = lv_obj_create(s_playlist);
                if (!item) break;
                lv_obj_set_size(item, LV_PCT(100), 48);
                lv_obj_set_style_bg_color(item, UI_COLOR_LIST_ROW_BG, 0);
                lv_obj_set_style_bg_opa(item, LV_OPA_80, 0);
                lv_obj_set_style_radius(item, 10, 0);
                lv_obj_set_style_border_width(item, 0, 0);
                lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
                lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
                lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
                lv_obj_add_event_cb(item, playlist_item_cb, LV_EVENT_CLICKED,
                                    (void *)(uintptr_t)i);

                /* 图标和歌名都不接受点击，由原有 48px 高条目处理选歌。
                 * 图标固定留出 28px，歌名右移后仍保持单行横向滚动；
                 * 不改变列表父容器的滚动和展开/收起动画参数。 */
                lv_obj_t *icon = lv_label_create(item);
                /* 新建条目先显示播放三角，末尾统一按真实播放状态刷新当前行。 */
                lv_label_set_text(icon, LV_SYMBOL_PLAY);
                lv_obj_set_style_text_color(icon, UI_COLOR_TEXT, 0);
                lv_obj_set_style_text_font(icon, ui_font_22(), 0);
                lv_obj_set_width(icon, 28);
                lv_obj_align(icon, LV_ALIGN_LEFT_MID, 8, 0);
                lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

                lv_obj_t *title = lv_label_create(item);
                lv_label_set_text(title, "加载中...");
                lv_obj_set_width(title, UI_HOR_RES - 72);
                lv_label_set_long_mode(title, LV_LABEL_LONG_SCROLL_CIRCULAR);
                lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
                lv_obj_set_style_text_font(title, ui_font_18(), 0);
                lv_obj_align(title, LV_ALIGN_LEFT_MID, 40, 0);
                s_pl_items[i] = item;
                s_pl_item_count++;
            }
            s_library_count = s_pl_item_count;
            update_playlist_highlight();
        }
    }

    lv_obj_scroll_to_y(s_playlist, 0, LV_ANIM_OFF);
    lv_label_set_text(s_title_lbl, count ? "加载中..." : (status ? status : "未找到歌曲"));
    if (s_playlist_empty_lbl) {
        if (s_pl_item_count == 0) {
            const char *empty_text = count ? "歌曲列表内存错误" :
                                     (status && status[0] ? status : "未找到歌曲");
            lv_label_set_text(s_playlist_empty_lbl, empty_text);
            lv_obj_remove_flag(s_playlist_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        }
        else {
            lv_obj_add_flag(s_playlist_empty_lbl, LV_OBJ_FLAG_HIDDEN);
        }
    }
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
        lv_obj_t *title = playlist_item_title(s_pl_items[index]);
        if (title) lv_label_set_text(title, page->names[i]);
    }
    update_playlist_highlight();
}

void ui_music_player_set_cover(uint32_t index, const uint8_t *cover)
{
    if (!ui_music_player_can_show_song(index)) return;
    /* 参数是借用引用；canvas 独立持有至换图/重扫/动画快照释放。
     * 先 retain 再 release，也支持缓存命中后重复发布同一张封面。 */
    cover_loader_retain(cover);
    const uint8_t *old = s_cover_pixels;
    s_cover_pixels = cover;
    if (cover) {
        lv_canvas_set_buffer(s_cover_canvas, (void *)cover, COVER_SIZE, COVER_SIZE,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_remove_flag(s_cover_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_invalidate(s_cover_canvas);
    } else {
        lv_obj_add_flag(s_cover_canvas, LV_OBJ_FLAG_HIDDEN);
    }
    /* 封面未就绪时透明留空，避免把上一首/占位底色误认为下一首封面。 */
    lv_obj_set_style_bg_opa(s_cover_img, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(lv_obj_get_child(s_cover_img, 1), LV_OBJ_FLAG_HIDDEN);
    cover_loader_release(old);
}

void ui_music_player_show_song(uint32_t index, const char *title,
                               const uint8_t *cover, uint32_t duration_sec)
{
    if (!ui_music_player_can_show_song(index)) return;
    if (title) lv_label_set_text(s_title_lbl, title);
    ui_music_player_set_cover(index, cover);
    /* 封面/LRC可晚于点击播放发布。这里只更新信息，不能把已接受的播放
     * 请求重置为暂停，否则按钮下一次点击会误提交相反目标。布局不变。 */
    bool requested=s_backend.is_play_requested?s_backend.is_play_requested():
                   (s_backend.is_playing?s_backend.is_playing():s_is_playing);
    uint32_t time_ms=s_backend.get_time_ms?s_backend.get_time_ms():0;
    ui_music_player_update_progress(time_ms/1000, duration_sec, requested);
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
    lv_label_set_text(s_lyric_label, text && text[0] ? text : "歌词将在这里显示");
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

void ui_music_player_service_finished(bool animate)
{
    /* 两个调用方共用当前索引/单曲循环状态及同一 LVGL 锁，结束事件只能
     * 被消费一次。亮屏仍延后至封面动画结束，保持原切歌效果；熄屏时
     * LVGL 已暂停，不能等待动画回调清除 s_cover_animating。 */
    uint32_t count=ui_song_count();
    if(!count || !s_backend.select || !s_backend.take_finished ||
       (animate && s_cover_animating) || !s_backend.take_finished()) return;
    if(animate) {
        if(s_single_loop) request_song((uint32_t)s_current_idx);
        else ui_music_player_next_animated();
        return;
    }
    /* 熄屏只同步保留的页面状态，不启动左右切歌动画、不触发活动时间或
     * 唤醒屏幕。取消尚未走完的左右动画并回收旧封面/歌词快照，防止
     * 唤醒后旧回调把新曲封面覆盖；纵向布局和用户的歌词/封面选择不变。 */
    lv_anim_delete(s_cover_area,cover_anim_x_cb);
    lv_anim_delete(s_outgoing_area,cover_anim_x_cb);
    cover_slide_out_ready(NULL);
    s_cover_animating=false;
    lv_obj_set_x(s_cover_area,((int32_t)UI_HOR_RES-COVER_SIZE)/2);
    uint32_t index=s_single_loop?(uint32_t)s_current_idx:((uint32_t)s_current_idx+1)%count;
    switch_to_song((int)index);
    request_song(index);
}

static void player_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    /* 自然结束与熄屏媒体 worker 共用入口；解码错误/拔卡不会产生 EOS。
     * 亮屏保持原封面切换动画与列表选中规则。 */
    ui_music_player_service_finished(true);
    if (s_backend.get_time_ms && s_backend.get_duration_sec) {
        uint32_t time_ms = s_backend.get_time_ms();
        uint32_t duration_sec = s_backend.get_duration_sec();
        /* Before DAC/decoder integration there is no backend play-state
         * callback. Preserve the original button interaction locally; once
         * a real callback is supplied it becomes authoritative. */
        /* 准备阶段没有DMA进度不代表用户取消了播放。控制台与列表图标
         * 始终使用已接受的请求；位置继续使用上面的真实已发送样本时间。 */
        bool playing = s_backend.is_play_requested ? s_backend.is_play_requested() :
                       (s_backend.is_playing ? s_backend.is_playing() : s_is_playing);
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
