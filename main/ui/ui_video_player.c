/**
 * @file ui_video_player.c
 * @brief 视频播放器页面：2x2 预览翻页、全屏播放和控制台。
 *
 * 页面层级（都在 s_page 内，s_page 由 ui_manager 做模式切换）：
 *   s_list_view  预览列表（2x2，左右滑动翻页）
 *   s_player_view 全屏播放器（黑底，覆盖状态栏）
 *     s_canvas     464x464 RGB565，页面 460x460 裁掉右侧/下侧各 4 像素
 *     s_controls   返回键 + 底部播放/暂停 + 可拖动进度条
 *
 * 重要约束：
 * - 这里不做任何 SD/JPEG 读取，也不调用音频解码；所有文件、缩略图和
 *   视频帧都来自 video_player worker。UI 回调已经持有 LVGL 锁。
 * - 播放时隐藏状态栏，但保留屏幕边缘手势，所以模式栏/声光栏仍可拉出。
 * - 左侧/右侧边缘 30px 内的滑动留给 ui_manager 打开面板；只有非边缘
 *   的横向滑动才用于翻页/切换视频。
 * - 控制台 3 秒无操作淡出；滑动进度条时不自动隐藏。
 */

#include "ui_video_player.h"
#include "ui_fonts.h"
#include "ui_statusbar.h"
#include "ui_lockscreen.h"
#include "video_player.h"
#include "sd_card.h"
#include "ui_manager.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include "esp_log.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_video";

/* ============================ 预览列表布局 ============================ */
/*
 * 屏幕 460x460，状态栏 38px。2x2 预览在状态栏下方：
 *   行 0 y=42，行 1 y=250；每格 212x200，水平间距 12，垂直间距 8。
 * 每格上方 176x176 预览图，下方 24px 文件名（18 号全字库）。
 * 4 个格子刚好覆盖 42..450，底部留 10px。
 */
#define TILE_W             212
#define TILE_H             200
#define TILE_X0            12
#define TILE_Y0            (UI_STATUSBAR_HEIGHT + 4)
#define TILE_GAP_X         12
#define TILE_GAP_Y         8
#define PREVIEW_SIZE       176
#define PREVIEW_X          ((TILE_W - PREVIEW_SIZE) / 2)
#define NAME_Y             PREVIEW_SIZE
#define NAME_H             24

#define LIST_SLIDE_MS      200
#define PLAYER_SLIDE_MS    220
#define CONTROLS_FADE_MS   220
#define CONTROLS_HIDE_MS   3000
#define PLAYER_BAR_H       76
#define PLAY_BTN_SIZE      56
#define PLAYER_FRAME_BYTES (VIDEO_PLAYER_PAD_WIDTH * VIDEO_PLAYER_PAD_HEIGHT * 2U)

static lv_obj_t *s_page;
static lv_obj_t *s_list_view;
static lv_obj_t *s_grid;
static lv_obj_t *s_status_label;
/* 播放器切换时用于盖住旧画面的独立黑底和旧帧快照。 */
static lv_obj_t *s_player_backdrop;
static lv_obj_t *s_switch_canvas;
static uint8_t *s_switch_pixels;
static const uint8_t *s_last_frame;
static int32_t s_switch_entry_x;
static lv_obj_t *s_tiles[VIDEO_PAGE_SIZE];
static lv_obj_t *s_tile_images[VIDEO_PAGE_SIZE];
static lv_obj_t *s_tile_placeholders[VIDEO_PAGE_SIZE];
static lv_obj_t *s_tile_names[VIDEO_PAGE_SIZE];
static lv_image_dsc_t s_thumb_dsc[VIDEO_PAGE_SIZE];
static uint32_t s_tile_index[VIDEO_PAGE_SIZE];

static uint32_t s_page_first;
static uint32_t s_page_count;
static uint32_t s_total_count;
static uint32_t s_library_generation;
static bool s_library_ready;
static bool s_page_loaded;
static bool s_page_transitioning;
static bool s_page_request_pending;
static uint32_t s_last_page_attempt;
static uint32_t s_last_scan_attempt;
static int32_t s_page_direction;
static uint32_t s_page_target_first;
static bool s_tile_swiped;
static lv_point_t s_list_press;
static bool s_list_press_valid;

/* ============================ 播放器控件 ============================ */
static lv_obj_t *s_player_view;
static lv_obj_t *s_canvas;
static lv_obj_t *s_player_status;
static lv_obj_t *s_controls;
static lv_obj_t *s_back_btn;
static lv_obj_t *s_play_btn;
static lv_obj_t *s_play_icon;
static lv_obj_t *s_slider;
static lv_obj_t *s_slider_touch; /**< 视觉滑条上方的 44px 透明拖动层 */
static lv_timer_t *s_ui_timer;

static bool s_player_active;
static bool s_player_transitioning;
static uint32_t s_current_index = UINT32_MAX;
static uint32_t s_switch_target = UINT32_MAX;
static lv_point_t s_player_press;
static bool s_player_press_valid;
static bool s_controls_visible;
static bool s_slider_dragging;
static uint32_t s_last_activity;
static bool s_canvas_has_buffer;
static int32_t s_controls_opa;

static void controls_show(void);
static void controls_hide(void);
static void controls_activity(void);
static void ui_timer_cb(lv_timer_t *timer);

/* ============================ 小工具 ============================ */

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void set_list_status(const char *text, bool visible)
{
    if (!s_status_label) return;
    if (text) lv_label_set_text(s_status_label, text);
    set_hidden(s_status_label, !visible);
}

/**
 * @brief 请求一页视频索引，避免 100ms 定时器重复投递同一个请求。
 *
 * 真正的 SD 读取由 worker 完成；这里只设置 pending 标志并更新提示。
 * 页面回调或明确的失败提示会清除 pending。
 */
static void request_list_page(uint32_t first)
{
    if (s_page_request_pending) return;
    s_page_request_pending = true;
    s_last_page_attempt = lv_tick_get();
    set_list_status("正在加载...", true);
    video_player_load_page(first);
}

static void set_player_status(const char *text)
{
    if (!s_player_status) return;
    if (!text || !text[0]) {
        set_hidden(s_player_status, true);
        return;
    }
    lv_label_set_text(s_player_status, text);
    set_hidden(s_player_status, false);
}

static void set_play_icon(bool playing)
{
    if (!s_play_icon) return;
    lv_label_set_text(s_play_icon, playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY);
}

static bool is_edge_start(const lv_point_t *p)
{
    return p->x < UI_EDGE_THRESHOLD || p->x > UI_HOR_RES - UI_EDGE_THRESHOLD;
}

/** @brief 底部控制栏区域：在这里按下时只允许控件操作，不参与视频滑动切换。 */
static bool is_control_bar_point(const lv_point_t *p)
{
    return p->y >= UI_VER_RES - PLAYER_BAR_H;
}

/** @brief 触摸被其它控件/系统打断时清除未完成的滑动状态。 */
static void gesture_press_lost_cb(lv_event_t *event)
{
    (void)event;
    s_list_press_valid = false;
    s_player_press_valid = false;
}

/* ============================ 预览缩略图 ============================ */

static void clear_tile_preview(unsigned slot)
{
    if (slot >= VIDEO_PAGE_SIZE) return;
    s_thumb_dsc[slot].data = NULL;
    s_thumb_dsc[slot].data_size = 0;
    /* 复用同一个描述符指针时，必须丢弃 LVGL 按 src 指针建立的图像缓存，
     * 否则换页后可能继续显示上一页的缩略图。 */
    lv_image_cache_drop(&s_thumb_dsc[slot]);
    set_hidden(s_tile_images[slot], true);
    set_hidden(s_tile_placeholders[slot], false);
}

static void clear_all_previews(void)
{
    for (unsigned i = 0; i < VIDEO_PAGE_SIZE; ++i) clear_tile_preview(i);
}

static void set_tile_preview(unsigned slot, const uint8_t *pixels,
                             uint16_t w, uint16_t h)
{
    if (slot >= VIDEO_PAGE_SIZE || !pixels ||
        w != VIDEO_PLAYER_THUMB_SIZE || h != VIDEO_PLAYER_THUMB_SIZE) {
        clear_tile_preview(slot);
        return;
    }
    s_thumb_dsc[slot].header.magic = LV_IMAGE_HEADER_MAGIC;
    s_thumb_dsc[slot].header.cf = LV_COLOR_FORMAT_RGB565;
    s_thumb_dsc[slot].header.flags = 0;
    s_thumb_dsc[slot].header.w = w;
    s_thumb_dsc[slot].header.h = h;
    s_thumb_dsc[slot].header.stride = w * 2;
    s_thumb_dsc[slot].data_size = (uint32_t)w * h * 2;
    s_thumb_dsc[slot].data = pixels;
    lv_image_cache_drop(&s_thumb_dsc[slot]);
    lv_image_set_src(s_tile_images[slot], &s_thumb_dsc[slot]);
    set_hidden(s_tile_images[slot], false);
    set_hidden(s_tile_placeholders[slot], true);
    lv_obj_invalidate(s_tile_images[slot]);
}

/* ============================ 播放器控制台 ============================ */

static void controls_fade_exec(void *obj, int32_t value)
{
    s_controls_opa = value;
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void controls_fade_done(lv_anim_t *anim)
{
    (void)anim;
    lv_obj_add_flag(s_controls, LV_OBJ_FLAG_HIDDEN);
    s_controls_visible = false;
}

/**
 * @brief 显示控制台并重置 3 秒计时。
 *
 * 只改 s_controls 的透明度，不改变播放器布局、canvas 或视频帧缓冲。
 * 透明度作用于整个控制层，返回键、底部栏和进度条一起淡入。
 */
static void controls_show(void)
{
    if (!s_player_active || s_player_transitioning || !s_controls) return;
    lv_anim_delete(s_controls, controls_fade_exec);
    lv_obj_remove_flag(s_controls, LV_OBJ_FLAG_HIDDEN);
    /* 从当前透明度继续，避免已经显示时再次淡入造成闪烁。 */
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_controls);
    lv_anim_set_values(&a, s_controls_opa, LV_OPA_COVER);
    lv_anim_set_duration(&a, CONTROLS_FADE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, controls_fade_exec);
    lv_anim_start(&a);
    s_controls_visible = true;
    s_last_activity = lv_tick_get();
}

/** @brief 3 秒无操作后淡出；拖动进度条时绝不隐藏。 */
static void controls_hide(void)
{
    if (!s_controls || !s_controls_visible || s_slider_dragging) return;
    /* 立刻把逻辑状态设为“不可见”，否则 100ms 定时器会在 220ms 淡出
     * 完成前反复重启动画，控制台可能永远淡不出去。 */
    s_controls_visible = false;
    lv_anim_delete(s_controls, controls_fade_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_controls);
    lv_anim_set_values(&a, s_controls_opa, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, CONTROLS_FADE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, controls_fade_exec);
    lv_anim_set_completed_cb(&a, controls_fade_done);
    lv_anim_start(&a);
}

/** @brief 切换视频/返回列表时立即撤销控制台，不等待淡出。 */
static void controls_hide_immediate(void)
{
    if (!s_controls) return;
    lv_anim_delete(s_controls, controls_fade_exec);
    s_controls_opa = LV_OPA_TRANSP;
    lv_obj_set_style_opa(s_controls, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_controls, LV_OBJ_FLAG_HIDDEN);
    s_controls_visible = false;
}

static void controls_activity(void)
{
    s_last_activity = lv_tick_get();
}

/** @brief 点击视频区域：控制台可见则隐藏，不可见则唤出。 */
static void controls_toggle(void)
{
    if (s_controls_visible) controls_hide();
    else controls_show();
}

/* ============================ 播放器切换动画 ============================ */

static void player_anim_x_cb(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

/** @brief 旧帧快照滑出完成：隐藏快照层，不碰正在滑入的播放器。 */
static void switch_canvas_done(lv_anim_t *anim)
{
    (void)anim;
    if (!s_switch_canvas) return;
    lv_obj_add_flag(s_switch_canvas, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(s_switch_canvas, 0);
}

static void player_in_done(lv_anim_t *anim)
{
    (void)anim;
    lv_obj_set_x(s_player_view, 0);
    if (s_switch_canvas) {
        lv_obj_add_flag(s_switch_canvas, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_x(s_switch_canvas, 0);
    }
    s_player_transitioning = false;
    /* 切换后如果已经暂停/结束/出错，自动把控制台显示出来；
     * 正常播放保持全屏，由用户点击唤出。 */
    video_state_t state = video_player_get_state();
    if (state == VIDEO_STATE_PAUSED || state == VIDEO_STATE_ENDED ||
        state == VIDEO_STATE_ERROR) {
        controls_show();
    }
}

static void player_out_done(lv_anim_t *anim);

/**
 * @brief 切换到上一个/下一个视频。
 *
 * 有旧帧快照时做真正的交叉滑动：旧画面从 0 滑到屏幕一侧，同时新播放器
 * 从另一侧滑入。这样不会出现“旧画面先滑出、黑屏等待、再滑入”的断层。
 * 没有快照（例如首帧还没到）时退回原来的顺序滑出/滑入。
 */
static void switch_video(int32_t direction)
{
    if (!s_player_active || s_player_transitioning || direction == 0) return;
    uint32_t target;
    if (direction > 0) {
        if (s_current_index + 1 >= s_total_count) return;
        target = s_current_index + 1;
    } else {
        if (s_current_index == 0) return;
        target = s_current_index - 1;
    }

    s_player_transitioning = true;
    s_switch_target = target;
    /* 先切到新索引，旧视频迟到的帧/状态回调会被索引过滤掉。 */
    s_current_index = target;
    s_switch_entry_x = direction > 0 ? (int32_t)UI_HOR_RES : -(int32_t)UI_HOR_RES;
    controls_hide_immediate();
    set_player_status("正在切换视频...");
    if (s_player_backdrop) set_hidden(s_player_backdrop, false);

    bool cross_slide = s_last_frame && s_switch_pixels && s_switch_canvas;
    if (cross_slide) {
        /* 在 LVGL 锁内复制当前显示缓冲；解码器下一帧会写另一块缓冲，
         * 不会在复制过程中覆盖这一帧。 */
        memcpy(s_switch_pixels, s_last_frame, PLAYER_FRAME_BYTES);
        lv_canvas_set_buffer(s_switch_canvas, (void *)s_switch_pixels,
                             VIDEO_PLAYER_PAD_WIDTH, VIDEO_PLAYER_PAD_HEIGHT,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_set_pos(s_switch_canvas, 0, 0);
        set_hidden(s_switch_canvas, false);
        lv_obj_move_foreground(s_switch_canvas);
        lv_obj_invalidate(s_switch_canvas);
    }

    /* 旧 canvas 不能继续显示；新首帧到达后 video_frame_cb 会重新打开它。 */
    set_hidden(s_canvas, true);
    s_canvas_has_buffer = false;
    s_last_frame = NULL;

    lv_anim_delete(s_player_view, player_anim_x_cb);
    if (cross_slide) {
        lv_anim_delete(s_switch_canvas, player_anim_x_cb);
        lv_obj_set_x(s_player_view, s_switch_entry_x);

        /* 新视频立刻开始加载，首帧到达时播放器已经在滑入。 */
        video_player_play(target);

        lv_anim_t old_anim;
        lv_anim_init(&old_anim);
        lv_anim_set_var(&old_anim, s_switch_canvas);
        /* 快照 canvas 是 464px 宽，必须滑出 464px 才完全离开屏幕；
         * 否则最后 4px 旧画面会停在左/右边缘。 */
        lv_anim_set_values(&old_anim, 0,
                           direction > 0 ? -(int32_t)VIDEO_PLAYER_PAD_WIDTH
                                         : (int32_t)VIDEO_PLAYER_PAD_WIDTH);
        lv_anim_set_duration(&old_anim, PLAYER_SLIDE_MS);
        lv_anim_set_path_cb(&old_anim, UI_ANIM_PATH);
        lv_anim_set_exec_cb(&old_anim, player_anim_x_cb);
        lv_anim_set_completed_cb(&old_anim, switch_canvas_done);
        lv_anim_start(&old_anim);

        lv_anim_t new_anim;
        lv_anim_init(&new_anim);
        lv_anim_set_var(&new_anim, s_player_view);
        lv_anim_set_values(&new_anim, s_switch_entry_x, 0);
        lv_anim_set_duration(&new_anim, PLAYER_SLIDE_MS);
        lv_anim_set_path_cb(&new_anim, UI_ANIM_PATH);
        lv_anim_set_exec_cb(&new_anim, player_anim_x_cb);
        lv_anim_set_completed_cb(&new_anim, player_in_done);
        lv_anim_start(&new_anim);
        return;
    }

    /* 无快照的兜底：保留原来的顺序滑出/滑入。 */
    lv_obj_set_x(s_player_view, 0);
    lv_anim_t out_anim;
    lv_anim_init(&out_anim);
    lv_anim_set_var(&out_anim, s_player_view);
    lv_anim_set_values(&out_anim, 0, -s_switch_entry_x);
    lv_anim_set_duration(&out_anim, PLAYER_SLIDE_MS);
    lv_anim_set_path_cb(&out_anim, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&out_anim, player_anim_x_cb);
    lv_anim_set_completed_cb(&out_anim, player_out_done);
    lv_anim_start(&out_anim);
}

static void player_out_done(lv_anim_t *anim)
{
    (void)anim;
    set_player_status("正在切换视频...");
    lv_obj_set_x(s_player_view, s_switch_entry_x);

    uint32_t target = s_switch_target;
    s_current_index = target;
    video_player_play(target);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_player_view);
    lv_anim_set_values(&a, s_switch_entry_x, 0);
    lv_anim_set_duration(&a, PLAYER_SLIDE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, player_anim_x_cb);
    lv_anim_set_completed_cb(&a, player_in_done);
    lv_anim_start(&a);
}

/* ============================ 预览列表翻页 ============================ */

static void grid_anim_x_cb(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void page_in_done(lv_anim_t *anim)
{
    (void)anim;
    lv_obj_set_x(s_grid, 0);
    s_page_transitioning = false;
}

static void page_out_done(lv_anim_t *anim)
{
    (void)anim;
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(s_grid, s_page_direction > 0 ? UI_HOR_RES : -UI_HOR_RES);
    /* 翻页优先于更早的加载请求；旧页回调会被 target_first 检查丢弃。 */
    s_page_request_pending = false;
    request_list_page(s_page_target_first);
}

/** @brief 请求上一/下一页；没有下一页时保持当前页不动。 */
static void request_page(int32_t direction)
{
    if (!s_library_ready || s_page_transitioning || s_player_active ||
        s_page_count == 0 || direction == 0) return;

    uint32_t target;
    if (direction > 0) {
        if (s_page_first + VIDEO_PAGE_SIZE >= s_total_count) return;
        target = s_page_first + VIDEO_PAGE_SIZE;
    } else {
        if (s_page_first < VIDEO_PAGE_SIZE) return;
        target = s_page_first - VIDEO_PAGE_SIZE;
    }

    s_page_transitioning = true;
    s_page_direction = direction;
    s_page_target_first = target;
    s_tile_swiped = true; /* 抑制本次手势结束后的 tile CLICKED */

    lv_anim_delete(s_grid, grid_anim_x_cb);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_grid);
    lv_anim_set_values(&a, lv_obj_get_x(s_grid),
                       direction > 0 ? -(int32_t)UI_HOR_RES : (int32_t)UI_HOR_RES);
    lv_anim_set_duration(&a, LIST_SLIDE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, grid_anim_x_cb);
    lv_anim_set_completed_cb(&a, page_out_done);
    lv_anim_start(&a);
}

static void list_pressed_cb(lv_event_t *event)
{
    (void)event;
    /* tile 和 list 都可能设置 EVENT_BUBBLE；第一个到达的 press 记录起点，
     * 后续冒泡事件直接忽略，避免一次手势被处理两次。 */
    if (s_list_press_valid || s_player_active || s_page_transitioning) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_indev_get_point(indev, &s_list_press);
    s_list_press_valid = true;
    s_tile_swiped = false;
}

static void list_released_cb(lv_event_t *event)
{
    (void)event;
    if (!s_list_press_valid) return;
    s_list_press_valid = false;
    if (s_player_active || s_page_transitioning) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_list_press.x;
    int32_t dy = p.y - s_list_press.y;
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;
    /* 任何明显拖动都抑制随后的 tile CLICKED，避免竖向拖动误播放。 */
    if (adx > 12 || ady > 12) s_tile_swiped = true;
    if (adx <= 50 || adx <= ady || is_edge_start(&s_list_press)) return;

    /* 非边缘的横向滑动翻页；边缘留给 ui_manager 打开模式/声光栏。 */
    request_page(dx < 0 ? 1 : -1);
}

/* ============================ 列表/播放器切换 ============================ */

static void clear_list_page(void)
{
    for (unsigned i = 0; i < VIDEO_PAGE_SIZE; ++i) {
        clear_tile_preview(i);
        s_tile_index[i] = UINT32_MAX;
        if (s_tile_names[i]) lv_label_set_text(s_tile_names[i], "");
        set_hidden(s_tiles[i], true);
    }
    s_page_count = 0;
    s_page_loaded = false;
}

static void populate_page(const video_page_t *page)
{
    s_page_first = page->first;
    s_page_count = page->count;
    s_page_loaded = true;
    for (unsigned slot = 0; slot < VIDEO_PAGE_SIZE; ++slot) {
        if (slot >= page->count) {
            s_tile_index[slot] = UINT32_MAX;
            lv_label_set_text(s_tile_names[slot], "");
            clear_tile_preview(slot);
            set_hidden(s_tiles[slot], true);
            continue;
        }
        set_hidden(s_tiles[slot], false);
        s_tile_index[slot] = page->first + slot;
        lv_label_set_text(s_tile_names[slot], page->names[slot]);
        clear_tile_preview(slot);
    }
}

static void return_to_list(void)
{
    s_player_active = false;
    s_player_transitioning = false;
    s_player_press_valid = false;
    s_slider_dragging = false;
    s_canvas_has_buffer = false;
    s_last_frame = NULL;
    lv_anim_delete(s_player_view, player_anim_x_cb);
    if (s_switch_canvas) {
        lv_anim_delete(s_switch_canvas, player_anim_x_cb);
        set_hidden(s_switch_canvas, true);
        lv_obj_set_x(s_switch_canvas, 0);
    }
    if (s_player_backdrop) set_hidden(s_player_backdrop, true);
    s_switch_target = UINT32_MAX;
    set_hidden(s_canvas, true);
    set_player_status(NULL);
    controls_hide_immediate();
    set_hidden(s_player_view, true);
    lv_obj_set_x(s_player_view, 0);
    set_hidden(s_list_view, false);
    ui_statusbar_set_visible(true);
    video_player_stop(); /* 返回列表只停止播放，保留解码器给缩略图使用 */
}

static void play_index(uint32_t index)
{
    if (s_player_active || s_page_transitioning || index >= s_total_count) return;
    s_player_active = true;
    s_player_transitioning = false;
    s_current_index = index;
    s_switch_target = UINT32_MAX;
    s_canvas_has_buffer = false;
    s_last_frame = NULL;
    s_slider_dragging = false;
    s_list_press_valid = false;
    if (s_switch_canvas) {
        lv_anim_delete(s_switch_canvas, player_anim_x_cb);
        set_hidden(s_switch_canvas, true);
        lv_obj_set_x(s_switch_canvas, 0);
    }
    if (s_player_backdrop) set_hidden(s_player_backdrop, false);
    lv_slider_set_value(s_slider, 0, LV_ANIM_OFF);
    set_hidden(s_canvas, true);
    set_hidden(s_list_view, true);
    set_hidden(s_player_view, false);
    lv_obj_move_foreground(s_player_view);
    lv_obj_set_x(s_player_view, 0);
    set_player_status("正在打开视频...");
    ui_statusbar_set_visible(false);
    controls_show();
    video_player_play(index);
}

static void back_clicked_cb(lv_event_t *event)
{
    (void)event;
    return_to_list();
}

static void play_clicked_cb(lv_event_t *event)
{
    (void)event;
    controls_activity();
    video_state_t state = video_player_get_state();
    if (state == VIDEO_STATE_ERROR || state == VIDEO_STATE_IDLE) {
        video_player_play(s_current_index);
    } else {
        video_player_toggle();
    }
}

/* ============================ 播放器手势 ============================ */

static void player_pressed_cb(lv_event_t *event)
{
    (void)event;
    if (!s_player_active || s_player_transitioning) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    /* 底部控制栏区域只处理播放/暂停/进度条/空白点击，不启动视频
     * 左右切换手势，避免拖进度条时误切视频。 */
    if (is_control_bar_point(&p)) {
        s_player_press_valid = false;
        return;
    }
    s_player_press = p;
    s_player_press_valid = true;
}

static void player_released_cb(lv_event_t *event)
{
    (void)event;
    if (!s_player_active || s_player_transitioning || !s_player_press_valid ||
        s_slider_dragging) return;
    s_player_press_valid = false;
    /* 控制台子控件的事件不会冒泡到这里；只处理视频空白区域。 */
    if (lv_event_get_target(event) != s_player_view) return;

    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_player_press.x;
    int32_t dy = p.y - s_player_press.y;
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;

    if (adx > 50 && adx > ady) {
        /* 边缘横向滑动交给 ui_manager 打开模式/声光栏。 */
        if (!is_edge_start(&s_player_press)) switch_video(dx < 0 ? 1 : -1);
        return;
    }
    /* 轻点视频：控制台可见则收起，否则唤出。 */
    if (adx < 12 && ady < 12) controls_toggle();
}

/* ============================ 进度条 ============================ */
/*
 * 视觉与触摸分开：
 * - s_slider 保持原来的细轨道/小圆点样式，本身不可点击；
 * - s_slider_touch 是覆盖其上的透明 340x44 对象，负责 press/pressing/
 *   released 和坐标换算。这样外观回到原样，命中区域仍有 44px 高。
 */
static uint16_t slider_value_from_x(int32_t x)
{
    lv_area_t area;
    lv_obj_get_coords(s_slider_touch, &area);
    int32_t width = lv_area_get_width(&area);
    if (width <= 1) return 0;
    int32_t value = (x - area.x1) * 1000 / (width - 1);
    if (value < 0) value = 0;
    if (value > 1000) value = 1000;
    return (uint16_t)value;
}

/** @brief 把当前触摸点换算为 0..1000 并更新视觉滑块。 */
static void slider_touch_apply(void)
{
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    lv_slider_set_value(s_slider, slider_value_from_x(p.x), LV_ANIM_OFF);
}

static void slider_touch_pressed_cb(lv_event_t *event)
{
    (void)event;
    s_slider_dragging = true;
    /* 如果播放器先收到过一次 press，在这里立即作废，松手不会切换视频。 */
    s_player_press_valid = false;
    slider_touch_apply();
    controls_activity();
}

static void slider_touch_pressing_cb(lv_event_t *event)
{
    (void)event;
    if (!s_slider_dragging) return;
    slider_touch_apply();
    controls_activity();
}

static void slider_touch_released_cb(lv_event_t *event)
{
    (void)event;
    if (!s_slider_dragging) return;
    slider_touch_apply();
    s_slider_dragging = false;
    uint16_t value = (uint16_t)lv_slider_get_value(s_slider);
    controls_activity();
    video_player_seek(value);
}

/** @brief 触摸被系统/弹窗打断时不要卡在“正在拖动”状态。 */
static void slider_touch_press_lost_cb(lv_event_t *event)
{
    (void)event;
    s_slider_dragging = false;
    controls_activity();
}

/* ============================ 后台回调 ============================ */

static void video_status_cb(const char *text, void *ctx)
{
    (void)ctx;
    if (s_player_active) {
        set_player_status(text);
        controls_activity();
    } else {
        /* 扫描/翻页/读取失败的提示已经到达，允许定时器稍后重试；
         * 不能永远保持 pending，否则列表会卡在“正在加载”。 */
        s_page_request_pending = false;
        if (s_page_transitioning) {
            s_page_transitioning = false;
            lv_anim_delete(s_grid, grid_anim_x_cb);
            lv_obj_set_x(s_grid, 0);
            set_hidden(s_grid, false);
        }
        set_list_status(text, true);
    }
}

static void video_library_cb(uint32_t count, bool truncated, uint32_t generation,
                             void *ctx)
{
    (void)ctx;
    if (truncated) ESP_LOGW(TAG, "Video index truncated; showing the indexed prefix");
    s_total_count = count;
    s_library_generation = generation;
    s_library_ready = true;
    s_page_transitioning = false;
    s_page_request_pending = false;
    lv_anim_delete(s_grid, grid_anim_x_cb);
    lv_obj_set_x(s_grid, 0);

    if (count == 0) {
        clear_list_page();
        set_hidden(s_grid, true);
        set_list_status("未找到视频\n请将 460x460 无音频 MJPEG\n"
                        "放入 HEI_MAO_POD_SYS/MEDIA/video", true);
        return;
    }

    if (s_page_first >= count) s_page_first = 0;
    clear_list_page();
    set_hidden(s_grid, true);
    request_list_page(s_page_first);
}

static void video_page_cb(const video_page_t *page, uint32_t generation, void *ctx)
{
    (void)ctx;
    if (s_player_active) return;
    if (s_page_transitioning && page->first != s_page_target_first) return;

    s_library_generation = generation;
    s_page_request_pending = false;
    populate_page(page);
    set_list_status(NULL, false);
    set_hidden(s_grid, false);

    if (s_page_transitioning) {
        int32_t start = s_page_direction > 0 ? (int32_t)UI_HOR_RES : -(int32_t)UI_HOR_RES;
        lv_obj_set_x(s_grid, start);
        lv_anim_delete(s_grid, grid_anim_x_cb);
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, s_grid);
        lv_anim_set_values(&a, start, 0);
        lv_anim_set_duration(&a, LIST_SLIDE_MS);
        lv_anim_set_path_cb(&a, UI_ANIM_PATH);
        lv_anim_set_exec_cb(&a, grid_anim_x_cb);
        lv_anim_set_completed_cb(&a, page_in_done);
        lv_anim_start(&a);
    } else {
        lv_obj_set_x(s_grid, 0);
    }
}

static void video_thumbnail_cb(uint32_t slot, uint32_t index, const uint8_t *pixels,
                               uint16_t w, uint16_t h, uint32_t generation,
                               void *ctx)
{
    (void)ctx;
    if (slot >= VIDEO_PAGE_SIZE || s_player_active) return;
    if (!pixels) {
        clear_tile_preview(slot);
        /* index=UINT32_MAX 是 video_player 释放缩略图缓冲前的通知；
         * 说明当前页的图片已不可再用，下次进入视频模式必须重新加载。 */
        if (index == UINT32_MAX) s_page_loaded = false;
        return;
    }
    if (generation != s_library_generation ||
        index != s_tile_index[slot] || slot >= s_page_count) return;
    set_tile_preview(slot, pixels, w, h);
}

static void video_frame_cb(const uint8_t *pixels, uint16_t w, uint16_t h,
                           uint32_t sequence, void *ctx)
{
    (void)sequence;
    (void)ctx;
    if (!s_player_active) return;
    /* 只接受当前索引的帧。切换视频时旧 worker 可能还会迟交一帧，不能
     * 让它在滑入的新播放器里重新显示旧内容。 */
    if (video_player_get_index() != s_current_index) return;
    if (!pixels || w != VIDEO_PLAYER_PAD_WIDTH || h != VIDEO_PLAYER_PAD_HEIGHT) {
        s_last_frame = NULL;
        s_canvas_has_buffer = false;
        set_hidden(s_canvas, true);
        return;
    }
    s_last_frame = pixels;
    lv_canvas_set_buffer(s_canvas, (void *)pixels,
                         VIDEO_PLAYER_PAD_WIDTH, VIDEO_PLAYER_PAD_HEIGHT,
                         LV_COLOR_FORMAT_RGB565);
    s_canvas_has_buffer = true;
    set_hidden(s_canvas, false);
    lv_obj_invalidate(s_canvas);
}

static void video_state_cb(video_state_t state, uint32_t index,
                           const char *message, void *ctx)
{
    (void)ctx;
    if (!s_player_active) return;
    /* 交叉滑动期间忽略旧视频迟到的状态，也忽略 IDLE；只接受目标视频。 */
    if (s_player_transitioning &&
        (index == UINT32_MAX || index != s_switch_target)) return;
    if (index != UINT32_MAX) s_current_index = index;

    switch (state) {
    case VIDEO_STATE_LOADING:
        set_play_icon(true);
        set_player_status(message && message[0] ? message : "正在打开视频...");
        controls_show();
        break;
    case VIDEO_STATE_PLAYING:
        set_play_icon(true);
        set_player_status(NULL);
        if (!s_controls_visible) controls_show();
        controls_activity();
        break;
    case VIDEO_STATE_PAUSED:
        set_play_icon(false);
        set_player_status(NULL);
        controls_show();
        controls_activity();
        break;
    case VIDEO_STATE_ENDED:
        set_play_icon(false);
        set_player_status("播放结束");
        controls_show();
        controls_activity();
        break;
    case VIDEO_STATE_ERROR:
        set_play_icon(false);
        set_player_status(message && message[0] ? message : "视频播放失败");
        controls_show();
        controls_activity();
        break;
    case VIDEO_STATE_IDLE:
    default:
        break;
    }
}

static const video_player_ops_t s_video_ops = {
    .status = video_status_cb,
    .library = video_library_cb,
    .page = video_page_cb,
    .thumbnail = video_thumbnail_cb,
    .frame = video_frame_cb,
    .state = video_state_cb,
};

/* ============================ 页面创建 ============================ */

static lv_obj_t *create_tile(lv_obj_t *parent, unsigned slot)
{
    int32_t x = TILE_X0 + (slot % 2) * (TILE_W + TILE_GAP_X);
    int32_t y = TILE_Y0 + (slot / 2) * (TILE_H + TILE_GAP_Y);
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, TILE_W, TILE_H);
    lv_obj_set_pos(tile, x, y);
    lv_obj_set_style_bg_color(tile, UI_COLOR_LIST_ROW_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    /* 让边缘手势能从 tile 冒泡到屏幕；tile 自己的 press/release 先处理，
     * list 的重复事件由 s_list_press_valid 拦截。 */
    lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *img = lv_image_create(tile);
    lv_obj_set_pos(img, PREVIEW_X, 0);
    lv_obj_set_size(img, PREVIEW_SIZE, PREVIEW_SIZE);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);

    /* 占位图标使用内置视频符号；18 号全字库只用于文件名。 */
    lv_obj_t *ph = lv_label_create(tile);
    lv_label_set_text(ph, LV_SYMBOL_VIDEO);
    lv_obj_set_style_text_font(ph, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(ph, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_pos(ph, PREVIEW_X, 58);
    lv_obj_set_size(ph, PREVIEW_SIZE, 60);
    lv_obj_set_style_text_align(ph, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_remove_flag(ph, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *name = lv_label_create(tile);
    lv_obj_set_pos(name, 0, NAME_Y);
    lv_obj_set_size(name, TILE_W, NAME_H);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(name, ui_font_18(), 0);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_add_event_cb(tile, list_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(tile, list_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(tile, gesture_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
    /* CLICKED 在 ui_video_player_create 里注册，user_data 记录槽位。 */
    return tile;
}

static void tile_clicked_cb(lv_event_t *event)
{
    if (s_tile_swiped) {
        s_tile_swiped = false;
        return;
    }
    if (s_player_active || s_page_transitioning) return;
    unsigned slot = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (slot >= VIDEO_PAGE_SIZE || slot >= s_page_count) return;
    uint32_t index = s_tile_index[slot];
    if (index < s_total_count) play_index(index);
}

static lv_obj_t *create_back_button(lv_obj_t *parent)
{
    /* 与时钟设置/姿态感应子页一致：44x44 透明按钮 + Montserrat 26 左箭头。
     * 视频内容可能很亮，所以控制台顶部另有一条 40% 黑色 scrim，按钮本身
     * 的尺寸、颜色、位置和点击范围不变。 */
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_pos(btn, 12, 8);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 6);
    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(arrow, UI_COLOR_TEXT, 0);
    lv_obj_center(arrow);
    lv_obj_add_event_cb(btn, back_clicked_cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

void ui_video_player_create(lv_obj_t *parent)
{
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

    /* ---------------- 预览列表 ---------------- */
    s_list_view = lv_obj_create(s_page);
    lv_obj_set_size(s_list_view, UI_HOR_RES, UI_VER_RES);
    lv_obj_align(s_list_view, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_opa(s_list_view, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list_view, 0, 0);
    lv_obj_set_style_pad_all(s_list_view, 0, 0);
    lv_obj_remove_flag(s_list_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_list_view, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_list_view, list_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_list_view, list_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_list_view, gesture_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);

    s_grid = lv_obj_create(s_list_view);
    lv_obj_set_size(s_grid, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_grid, 0, 0);
    lv_obj_set_style_bg_opa(s_grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_grid, 0, 0);
    lv_obj_set_style_pad_all(s_grid, 0, 0);
    lv_obj_remove_flag(s_grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_grid, LV_OBJ_FLAG_EVENT_BUBBLE);
    set_hidden(s_grid, true);

    for (unsigned i = 0; i < VIDEO_PAGE_SIZE; ++i) {
        s_tiles[i] = create_tile(s_grid, i);
        s_tile_images[i] = lv_obj_get_child(s_tiles[i], 0);
        s_tile_placeholders[i] = lv_obj_get_child(s_tiles[i], 1);
        s_tile_names[i] = lv_obj_get_child(s_tiles[i], 2);
        s_tile_index[i] = UINT32_MAX;
        set_hidden(s_tiles[i], true);
        lv_obj_add_event_cb(s_tiles[i], tile_clicked_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);
    }

    s_status_label = lv_label_create(s_list_view);
    lv_obj_set_width(s_status_label, UI_HOR_RES - 40);
    lv_obj_set_style_text_align(s_status_label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status_label, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_status_label, ui_font_18(), 0);
    lv_label_set_text(s_status_label, "");
    lv_obj_align(s_status_label, LV_ALIGN_CENTER, 0, 10);
    set_hidden(s_status_label, true);

    /* 播放器黑底：交叉滑动时旧帧滑出、新帧滑入，中间不能露出列表底色。 */
    s_player_backdrop = lv_obj_create(s_page);
    lv_obj_set_size(s_player_backdrop, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_player_backdrop, 0, 0);
    lv_obj_set_style_bg_color(s_player_backdrop, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_player_backdrop, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_player_backdrop, 0, 0);
    lv_obj_set_style_radius(s_player_backdrop, 0, 0);
    lv_obj_set_style_pad_all(s_player_backdrop, 0, 0);
    lv_obj_remove_flag(s_player_backdrop, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_player_backdrop, LV_OBJ_FLAG_CLICKABLE);
    set_hidden(s_player_backdrop, true);

    /* ---------------- 全屏播放器 ---------------- */
    s_player_view = lv_obj_create(s_page);
    lv_obj_set_size(s_player_view, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_player_view, 0, 0);
    lv_obj_set_style_bg_color(s_player_view, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_player_view, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_player_view, 0, 0);
    lv_obj_set_style_radius(s_player_view, 0, 0);
    lv_obj_set_style_pad_all(s_player_view, 0, 0);
    lv_obj_remove_flag(s_player_view, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_player_view, LV_OBJ_FLAG_CLICKABLE);
    /* 播放器覆盖状态栏，但仍让边缘手势冒泡给 ui_manager。 */
    lv_obj_add_flag(s_player_view, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_player_view, player_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_player_view, player_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_player_view, gesture_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
    set_hidden(s_player_view, true);

    /* canvas 使用 464x464 缓冲：解码器对 460x460 的 420/422 输出会补齐到
     * 16 的倍数。页面只有 460x460，默认裁剪右侧/下侧各 4 像素；这样
     * 不需要每帧再做 460 次逐行 memcpy。 */
    s_canvas = lv_canvas_create(s_player_view);
    lv_obj_set_size(s_canvas, VIDEO_PLAYER_PAD_WIDTH, VIDEO_PLAYER_PAD_HEIGHT);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_canvas, LV_OBJ_FLAG_HIDDEN);

    s_player_status = lv_label_create(s_player_view);
    lv_obj_set_width(s_player_status, UI_HOR_RES - 40);
    lv_obj_set_style_text_align(s_player_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_player_status, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_player_status, ui_font_18(), 0);
    lv_label_set_text(s_player_status, "");
    lv_obj_center(s_player_status);
    set_hidden(s_player_status, true);

    /* 旧帧快照层：和播放器同尺寸，切换时盖在上面滑出；平时隐藏。 */
    s_switch_canvas = lv_canvas_create(s_page);
    lv_obj_set_size(s_switch_canvas, VIDEO_PLAYER_PAD_WIDTH, VIDEO_PLAYER_PAD_HEIGHT);
    lv_obj_set_pos(s_switch_canvas, 0, 0);
    lv_obj_remove_flag(s_switch_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_switch_canvas, LV_OBJ_FLAG_SCROLLABLE);
    set_hidden(s_switch_canvas, true);
    s_switch_pixels = lv_malloc(PLAYER_FRAME_BYTES);
    if (!s_switch_pixels)
        ESP_LOGW(TAG, "Video switch snapshot unavailable; using sequential slide");

    /* 控制台：全屏透明层；自己没有点击行为，空白区域让点击穿到
     * s_player_view 以唤出/隐藏控制台，子按钮/滑条各自接收触摸。 */
    s_controls = lv_obj_create(s_player_view);
    lv_obj_set_size(s_controls, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_controls, 0, 0);
    lv_obj_set_style_bg_opa(s_controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_controls, 0, 0);
    lv_obj_set_style_pad_all(s_controls, 0, 0);
    lv_obj_remove_flag(s_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_controls, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(s_controls, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_controls, LV_OBJ_FLAG_HIDDEN);

    /* 顶部 scrim 只为白色返回箭头在亮画面上提供可读对比。 */
    lv_obj_t *top_scrim = lv_obj_create(s_controls);
    lv_obj_set_size(top_scrim, UI_HOR_RES, 64);
    lv_obj_set_pos(top_scrim, 0, 0);
    lv_obj_set_style_bg_color(top_scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(top_scrim, LV_OPA_40, 0);
    lv_obj_set_style_border_width(top_scrim, 0, 0);
    lv_obj_set_style_radius(top_scrim, 0, 0);
    lv_obj_remove_flag(top_scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(top_scrim, LV_OBJ_FLAG_SCROLLABLE);

    s_back_btn = create_back_button(s_controls);

    lv_obj_t *bottom = lv_obj_create(s_controls);
    lv_obj_set_size(bottom, UI_HOR_RES, PLAYER_BAR_H);
    lv_obj_set_pos(bottom, 0, UI_VER_RES - PLAYER_BAR_H);
    lv_obj_set_style_bg_color(bottom, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(bottom, LV_OPA_60, 0);
    lv_obj_set_style_border_width(bottom, 0, 0);
    lv_obj_set_style_radius(bottom, 0, 0);
    lv_obj_set_style_pad_all(bottom, 0, 0);
    lv_obj_remove_flag(bottom, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(bottom, LV_OBJ_FLAG_SCROLLABLE);

    s_play_btn = lv_button_create(bottom);
    lv_obj_set_size(s_play_btn, PLAY_BTN_SIZE, PLAY_BTN_SIZE);
    lv_obj_set_pos(s_play_btn, 16, (PLAYER_BAR_H - PLAY_BTN_SIZE) / 2);
    lv_obj_set_style_radius(s_play_btn, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(s_play_btn, UI_COLOR_CONTROL_BG, 0);
    lv_obj_set_style_bg_opa(s_play_btn, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_play_btn, 0, 0);
    lv_obj_set_style_shadow_width(s_play_btn, 0, 0);
    lv_obj_set_ext_click_area(s_play_btn, 6);
    s_play_icon = lv_label_create(s_play_btn);
    lv_label_set_text(s_play_icon, LV_SYMBOL_PAUSE);
    lv_obj_set_style_text_font(s_play_icon, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_color(s_play_icon, UI_COLOR_TEXT, 0);
    lv_obj_center(s_play_icon);
    lv_obj_add_event_cb(s_play_btn, play_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* 进度条视觉保持以前的样式：6px 细轨道 + 18px 白色圆点。对象本身
     * 不接收触摸，真正拖动由上方独立的透明 44px 触摸层完成。 */
    s_slider = lv_slider_create(bottom);
    lv_obj_set_size(s_slider, 340, 20);
    lv_obj_set_pos(s_slider, 84, (PLAYER_BAR_H - 20) / 2);
    lv_slider_set_range(s_slider, 0, 1000);
    lv_slider_set_value(s_slider, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_slider, UI_COLOR_SLIDER_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_height(s_slider, 6, LV_PART_MAIN);
    lv_obj_set_style_radius(s_slider, 3, LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_slider, UI_COLOR_SLIDER_ACTIVE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_COVER, LV_PART_INDICATOR);
    lv_obj_set_style_height(s_slider, 6, LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_slider, 3, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_slider, UI_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_set_style_bg_opa(s_slider, LV_OPA_COVER, LV_PART_KNOB);
    lv_obj_set_style_width(s_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_height(s_slider, 18, LV_PART_KNOB);
    lv_obj_set_style_radius(s_slider, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_remove_flag(s_slider, LV_OBJ_FLAG_CLICKABLE);

    /* 透明拖动层：44px 高、与滑条同宽，覆盖圆点和轨道。创建在 slider
     * 之后，触摸优先由它接收，不依赖细小滑条本身的命中测试。 */
    s_slider_touch = lv_obj_create(bottom);
    lv_obj_set_size(s_slider_touch, 340, 44);
    lv_obj_set_pos(s_slider_touch, 84, (PLAYER_BAR_H - 44) / 2);
    lv_obj_set_style_bg_opa(s_slider_touch, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_slider_touch, 0, 0);
    lv_obj_set_style_pad_all(s_slider_touch, 0, 0);
    lv_obj_remove_flag(s_slider_touch, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_slider_touch, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_slider_touch, slider_touch_pressed_cb,
                        LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_slider_touch, slider_touch_pressing_cb,
                        LV_EVENT_PRESSING, NULL);
    lv_obj_add_event_cb(s_slider_touch, slider_touch_released_cb,
                        LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_slider_touch, slider_touch_press_lost_cb,
                        LV_EVENT_PRESS_LOST, NULL);

    /* 100ms 定时器只做轻量 RAM 读取和滑块同步；不访问 SD/JPEG。 */
    s_ui_timer = lv_timer_create(ui_timer_cb, 100, NULL);
    if (!s_ui_timer) ESP_LOGW(TAG, "Video UI timer unavailable");

    video_player_set_ops(&s_video_ops, NULL);
}

/* ============================ 主循环辅助 ============================ */

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_player_active) {
        video_state_t state = video_player_get_state();
        if (state == VIDEO_STATE_PLAYING) set_play_icon(true);
        else if (state == VIDEO_STATE_PAUSED || state == VIDEO_STATE_ENDED ||
                 state == VIDEO_STATE_ERROR) set_play_icon(false);

        if (!s_slider_dragging) {
            uint16_t progress = video_player_get_progress();
            if ((uint16_t)lv_slider_get_value(s_slider) != progress)
                lv_slider_set_value(s_slider, progress, LV_ANIM_OFF);
        }
        if (s_controls_visible && !s_slider_dragging &&
            lv_tick_elaps(s_last_activity) >= CONTROLS_HIDE_MS) {
            controls_hide();
        }
        return;
    }

    /* 队列满/代次交错等极端情况下，页面请求可能没有回调；5 秒后允许
     * 重新发起，避免列表永久停在“正在加载”。 */
    if (s_page_request_pending &&
        lv_tick_elaps(s_last_page_attempt) >= 5000) {
        s_page_request_pending = false;
    }

    /* 列表页检测 SD 代次变化：换卡/拔卡后旧索引和缩略图不再可信。
     * 这里只投递 release+scan，不在 LVGL 定时器里做任何文件 I/O。 */
    bool video_visible = ui_manager_get_current_mode() == UI_MODE_VIDEO &&
                         !ui_lockscreen_is_visible();
    if (video_visible &&
        (!s_library_ready ||
         sd_card_get_generation() != s_library_generation) &&
        lv_tick_elaps(s_last_scan_attempt) >= 5000) {
        s_library_ready = false;
        s_total_count = 0;
        s_page_count = 0;
        s_page_loaded = false;
        s_page_transitioning = false;
        s_last_scan_attempt = lv_tick_get();
        clear_all_previews();
        clear_list_page();
        set_hidden(s_grid, true);
        set_list_status("正在扫描视频...", true);
        video_player_release();
        video_player_scan();
        return;
    }

    /* 锁屏期间资源被释放，解锁后当前模式仍是视频，但不会再次触发
     * on_mode_enter。这里在锁屏结束后补一次当前预览页请求；1 秒节流
     * 避免读取失败时每 100ms 重试。 */
    if (s_library_ready && s_total_count > 0 && !s_page_loaded &&
        !s_page_request_pending && !s_page_transitioning &&
        ui_manager_get_current_mode() == UI_MODE_VIDEO &&
        !ui_lockscreen_is_visible() &&
        lv_tick_elaps(s_last_page_attempt) >= 1000) {
        request_list_page(s_page_first);
    }
}

/* ============================ 模式生命周期 ============================ */

void ui_video_player_on_mode_enter(void)
{
    s_player_active = false;
    s_player_transitioning = false;
    s_page_transitioning = false;
    s_page_request_pending = false;
    s_player_press_valid = false;
    s_list_press_valid = false;
    s_slider_dragging = false;
    s_tile_swiped = false;
    s_switch_target = UINT32_MAX;
    s_last_frame = NULL;
    lv_anim_delete(s_player_view, player_anim_x_cb);
    if (s_switch_canvas) {
        lv_anim_delete(s_switch_canvas, player_anim_x_cb);
        set_hidden(s_switch_canvas, true);
        lv_obj_set_x(s_switch_canvas, 0);
    }
    if (s_player_backdrop) set_hidden(s_player_backdrop, true);
    set_hidden(s_player_view, true);
    set_hidden(s_list_view, false);
    controls_hide_immediate();
    ui_statusbar_set_visible(true);
    lv_obj_set_x(s_player_view, 0);
    lv_anim_delete(s_grid, grid_anim_x_cb);
    lv_obj_set_x(s_grid, 0);
    set_hidden(s_grid, true);

    if (!s_library_ready || sd_card_get_generation() != s_library_generation) {
        s_library_ready = false;
        s_total_count = 0;
        s_page_count = 0;
        s_page_loaded = false;
        clear_all_previews();
        clear_list_page();
        s_last_scan_attempt = lv_tick_get();
        set_list_status("正在扫描视频...", true);
        video_player_scan();
        return;
    }
    if (s_total_count == 0) {
        set_list_status("未找到视频\n请将 460x460 无音频 MJPEG\n"
                        "放入 HEI_MAO_POD_SYS/MEDIA/video", true);
        return;
    }
    if (s_page_loaded) {
        set_hidden(s_grid, false);
        set_list_status(NULL, false);
    } else {
        request_list_page(s_page_first);
    }
}

void ui_video_player_on_mode_leave(void)
{
    s_player_active = false;
    s_player_transitioning = false;
    s_page_transitioning = false;
    s_page_request_pending = false;
    s_player_press_valid = false;
    s_list_press_valid = false;
    s_slider_dragging = false;
    s_canvas_has_buffer = false;
    s_switch_target = UINT32_MAX;
    s_last_frame = NULL;
    lv_anim_delete(s_player_view, player_anim_x_cb);
    if (s_switch_canvas) {
        lv_anim_delete(s_switch_canvas, player_anim_x_cb);
        set_hidden(s_switch_canvas, true);
        lv_obj_set_x(s_switch_canvas, 0);
    }
    if (s_player_backdrop) set_hidden(s_player_backdrop, true);
    set_hidden(s_canvas, true);
    set_player_status(NULL);
    controls_hide_immediate();
    set_hidden(s_player_view, true);
    lv_obj_set_x(s_player_view, 0);
    set_hidden(s_list_view, false);
    lv_anim_delete(s_grid, grid_anim_x_cb);
    lv_obj_set_x(s_grid, 0);

    /* 先让 UI 不再引用缩略图，再让 worker 释放 PSRAM/JPEG 引擎。 */
    clear_all_previews();
    s_page_loaded = false;
    s_page_count = 0;
    video_player_stop();
    video_player_release();
    ui_statusbar_set_visible(true);
}

void ui_video_player_close_for_lock(void)
{
    s_player_active = false;
    s_player_transitioning = false;
    s_page_transitioning = false;
    s_page_request_pending = false;
    s_player_press_valid = false;
    s_list_press_valid = false;
    s_slider_dragging = false;
    s_canvas_has_buffer = false;
    s_switch_target = UINT32_MAX;
    s_last_frame = NULL;
    lv_anim_delete(s_player_view, player_anim_x_cb);
    if (s_switch_canvas) {
        lv_anim_delete(s_switch_canvas, player_anim_x_cb);
        set_hidden(s_switch_canvas, true);
        lv_obj_set_x(s_switch_canvas, 0);
    }
    if (s_player_backdrop) set_hidden(s_player_backdrop, true);
    set_hidden(s_canvas, true);
    set_player_status(NULL);
    controls_hide_immediate();
    set_hidden(s_player_view, true);
    lv_obj_set_x(s_player_view, 0);
    /* 锁屏会盖住本页；先恢复列表视图，解锁后定时器才能补加载预览页。 */
    set_hidden(s_list_view, false);
    lv_anim_delete(s_grid, grid_anim_x_cb);
    lv_obj_set_x(s_grid, 0);
    set_hidden(s_grid, true);

    clear_all_previews();
    s_page_loaded = false;
    s_page_count = 0;
    video_player_stop();
    video_player_release();
    ui_statusbar_set_visible(true);
}

lv_obj_t *ui_video_player_get_page(void)
{
    return s_page;
}

bool ui_video_player_is_player_active(void)
{
    return s_player_active;
}
