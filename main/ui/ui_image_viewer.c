/**
 * @file ui_image_viewer.c
 * @brief 图片查看器：两个文件夹入口、四列虚拟化总览、全屏查看和控制台。
 *
 * 页面层级（s_page 内）：
 *   s_home      两个圆角文件夹卡片（相机 / 其他图片）
 *   s_overview  四列缩略图总览；右侧滑入/滑出，非边缘右滑返回首页
 *   s_full      全屏图片；隐藏状态栏，左右滑动切换，点击唤出控制台
 *
 * 重要约束：
 * - 本文件不做 SD/JPEG 读取；所有扫描、缩略图和全屏图来自 image_viewer
 *   worker。worker 回调在 LVGL 锁内执行，这里只更新控件/描述符。
 * - 总览使用固定 32 个 tile，槽位按 8 行窗口环形复用；图片数量增加只
 *   影响总高度和索引，不会创建 O(n) 个 LVGL 对象或缩略图缓冲。
 * - 全屏图由后台双缓冲，切换时保留旧图直到新图解码完成；加载失败只
 *   显示错误文字，不用占位图冒充原图。
 * - 图片名称必须使用 18 号全字库；卡片标题使用 22 号，状态提示使用 18 号。
 */
#include "ui_image_viewer.h"

#include "image_viewer.h"
#include "sd_card.h"
#include "ui_fonts.h"
#include "ui_lockscreen.h"
#include "ui_manager.h"
#include "ui_statusbar.h"

#include "esp_log.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_image";

/* ============================ 布局参数 ============================ */
/* 首页：状态栏38px；上下各10px边距、中间10px间距，两张卡片各196px高。 */
#define HOME_CARD_X          10
#define HOME_CARD_W          (UI_HOR_RES - 2 * HOME_CARD_X)
#define HOME_CARD_H          196
#define HOME_CARD_GAP        10
#define HOME_CARD_Y0         (UI_STATUSBAR_HEIGHT + HOME_CARD_GAP)
#define HOME_CARD_RADIUS     18
#define HOME_PREVIEW_SIZE    148
#define HOME_PREVIEW_X       16
#define HOME_PREVIEW_Y       ((HOME_CARD_H - HOME_PREVIEW_SIZE) / 2)
#define HOME_PREVIEW_RADIUS  12

/* 总览：4列 x 100px，列/行间距12px，左右边距12px；可视区从状态栏下方开始。 */
#define GRID_COLS            4
#define GRID_THUMB           100
#define GRID_GAP_X           12
#define GRID_GAP_Y           12
#define GRID_X0              12
#define GRID_Y0              8
#define GRID_ROW_H           (GRID_THUMB + GRID_GAP_Y)
#define GRID_TOP             UI_STATUSBAR_HEIGHT
#define GRID_VIEW_H          (UI_VER_RES - GRID_TOP)
#define GRID_WINDOW_ROWS     8
#define GRID_SLOTS           (GRID_COLS * GRID_WINDOW_ROWS)

/* 全屏控制台：与视频播放器一致的顶部 scrim、44x44 返回键和淡出时长。 */
#define FULL_TOP_H           64
#define FULL_CONTROLS_HIDE_MS 3000
#define FULL_CONTROLS_FADE_MS 220
/* 与视频播放器 PLAYER_SLIDE_MS 一致：图片左右切换的交叉滑动时长。 */
#define FULL_SWITCH_MS       220

_Static_assert(GRID_SLOTS <= IMAGE_MAX_THUMB_SLOTS,
               "image grid needs more thumbnail slots than the backend provides");

static const char *const s_card_names[IMAGE_FOLDER_COUNT] = {
    "相机",
    "其他图片",
};

/* ============================ 页面对象 ============================ */
static lv_obj_t *s_page;
static lv_obj_t *s_home;
static lv_obj_t *s_home_imgs[IMAGE_FOLDER_COUNT];
static lv_obj_t *s_home_phs[IMAGE_FOLDER_COUNT];
static lv_image_dsc_t s_home_dsc[IMAGE_FOLDER_COUNT];
static lv_obj_t *s_home_status;

static lv_obj_t *s_overview;
static lv_obj_t *s_overview_scroll;
static lv_obj_t *s_overview_content;
static lv_obj_t *s_overview_status;
static lv_obj_t *s_overview_note;
static lv_obj_t *s_tiles[GRID_SLOTS];
static lv_obj_t *s_tile_imgs[GRID_SLOTS];
static lv_obj_t *s_tile_phs[GRID_SLOTS];
static lv_image_dsc_t s_thumb_dsc[GRID_SLOTS];
static uint32_t s_tile_index[GRID_SLOTS];
static uint32_t s_tile_token[GRID_SLOTS];
static int32_t s_group_row[GRID_WINDOW_ROWS];
static int32_t s_window_first;
static bool s_grid_valid;
static int32_t s_grid_content_h;

static lv_obj_t *s_full;
static lv_obj_t *s_full_canvas;     /**< 当前显示的图片canvas */
static lv_obj_t *s_full_canvas_alt; /**< 切换时滑入/滑出的备用canvas */
static bool s_full_canvas_has_buffer;
static bool s_full_sliding;         /**< 交叉滑动进行中，禁止再次切换 */
static int32_t s_full_switch_dir;   /**< +1=下一张从右入，-1=上一张从左入 */
static lv_obj_t *s_full_status;
static lv_obj_t *s_full_controls;
static lv_obj_t *s_full_name;

static lv_timer_t *s_timer;

/* ============================ 运行状态 ============================ */
static image_folder_t s_folder = IMAGE_FOLDER_CAM;
static uint32_t s_count[IMAGE_FOLDER_COUNT];
static uint32_t s_generation[IMAGE_FOLDER_COUNT];
static bool s_ready[IMAGE_FOLDER_COUNT];
static bool s_truncated[IMAGE_FOLDER_COUNT];
static image_scan_result_t s_scan_result = IMAGE_SCAN_OK;
static bool s_scan_pending;
static uint32_t s_last_scan_tick;
static uint32_t s_sd_generation;

static bool s_overview_visible;
static bool s_overview_transition;
static bool s_press_valid;
static lv_point_t s_press;
static bool s_swiped;

static bool s_full_active;
static bool s_full_loading;
static image_folder_t s_full_folder = IMAGE_FOLDER_CAM;
static uint32_t s_full_count;
static uint32_t s_full_index = UINT32_MAX;
static uint32_t s_full_token;

static bool s_controls_visible;
static int32_t s_controls_opa;
static uint32_t s_last_activity;

/* ============================ 小工具 ============================ */

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static void set_label(lv_obj_t *label, const char *text)
{
    if (!label) return;
    if (!text || !text[0]) {
        lv_obj_add_flag(label, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    lv_label_set_text(label, text);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_HIDDEN);
}

static bool is_edge_start(const lv_point_t *p)
{
    return p->x < UI_EDGE_THRESHOLD || p->x > UI_HOR_RES - UI_EDGE_THRESHOLD;
}

static void set_rgb565_dsc(lv_image_dsc_t *dsc, const uint8_t *pixels,
                           uint16_t w, uint16_t h)
{
    if (!pixels) {
        dsc->data = NULL;
        dsc->data_size = 0;
        return;
    }
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.flags = 0;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = w * 2;
    dsc->data_size = (uint32_t)w * h * 2;
    dsc->data = pixels;
}

/* ============================ 状态文字 ============================ */

static void update_home_status(void)
{
    const char *text = NULL;
    if (s_scan_pending) {
        text = "正在扫描图片...";
    } else if (s_scan_result == IMAGE_SCAN_NO_CARD) {
        text = "SD 卡未插入";
    } else if (s_scan_result == IMAGE_SCAN_BUSY) {
        text = "SD 卡忙，请稍后重试";
    } else if (s_scan_result == IMAGE_SCAN_ERROR) {
        text = "图片读取失败";
    } else if (s_ready[0] && s_ready[1] && s_count[0] == 0 && s_count[1] == 0) {
        text = "暂无图片";
    }
    set_label(s_home_status, text);
}

static void update_overview_status(void)
{
    const char *text = NULL;
    if (s_folder < IMAGE_FOLDER_COUNT) {
        if (s_scan_pending) {
            text = "正在扫描图片...";
        } else if (s_scan_result == IMAGE_SCAN_NO_CARD) {
            text = "SD 卡未插入";
        } else if (s_scan_result == IMAGE_SCAN_BUSY) {
            text = "SD 卡忙，请稍后重试";
        } else if (s_scan_result == IMAGE_SCAN_ERROR) {
            text = "图片读取失败";
        } else if (!s_ready[s_folder]) {
            text = "正在扫描图片...";
        } else if (s_count[s_folder] == 0) {
            text = "暂无图片";
        }
    }
    set_label(s_overview_status, text);
}

static void update_overview_note(void)
{
    bool show = s_folder < IMAGE_FOLDER_COUNT && s_ready[s_folder] &&
                s_truncated[s_folder] && s_count[s_folder] > 0;
    if (!show) {
        set_label(s_overview_note, NULL);
        return;
    }
    char text[40];
    snprintf(text, sizeof(text), "仅显示前%u张", (unsigned)IMAGE_INDEX_MAX);
    set_label(s_overview_note, text);
}

/* ============================ 缩略图槽位 ============================ */

static void hide_tile(uint32_t slot)
{
    if (slot >= GRID_SLOTS) return;
    ++s_tile_token[slot];
    s_tile_index[slot] = UINT32_MAX;
    s_thumb_dsc[slot].data = NULL;
    s_thumb_dsc[slot].data_size = 0;
    lv_image_cache_drop(&s_thumb_dsc[slot]);
    set_hidden(s_tile_imgs[slot], true);
    set_hidden(s_tile_phs[slot], false);
    set_hidden(s_tiles[slot], true);
}

static void reset_grid(void)
{
    for (uint32_t i = 0; i < GRID_SLOTS; ++i) hide_tile(i);
    s_grid_valid = false;
    s_window_first = -1;
    s_grid_content_h = -1;
    image_viewer_clear_thumbs();
}

/**
 * @brief 把一个 4 格行组放到指定绝对行；同一组只在其内容变化时重新请求。
 */
static void set_group_row(unsigned group, int32_t row, uint32_t count,
                          uint32_t rows)
{
    int32_t y = GRID_Y0 + row * GRID_ROW_H;
    for (unsigned col = 0; col < GRID_COLS; ++col) {
        uint32_t slot = group * GRID_COLS + col;
        bool valid = row >= 0 && (uint32_t)row < rows;
        uint32_t index = valid ? (uint32_t)row * GRID_COLS + col : UINT32_MAX;
        if (index >= count) valid = false;
        if (!valid) {
            hide_tile(slot);
            continue;
        }
        lv_obj_set_pos(s_tiles[slot],
                       GRID_X0 + (int32_t)col * (GRID_THUMB + GRID_GAP_X), y);
        set_hidden(s_tiles[slot], false);
        if (s_tile_index[slot] == index) continue;
        s_tile_index[slot] = index;
        ++s_tile_token[slot];
        s_thumb_dsc[slot].data = NULL;
        s_thumb_dsc[slot].data_size = 0;
        lv_image_cache_drop(&s_thumb_dsc[slot]);
        set_hidden(s_tile_imgs[slot], true);
        set_hidden(s_tile_phs[slot], false);
        image_viewer_request_thumbnail(s_folder, index, slot, s_tile_token[slot]);
    }
}

static int group_for_row(int32_t row)
{
    for (unsigned g = 0; g < GRID_WINDOW_ROWS; ++g)
        if (s_group_row[g] == row) return (int)g;
    return -1;
}

/**
 * @brief 根据当前滚动位置维护 8 行环形窗口。
 *
 * 窗口覆盖 [first, first+7]；滚动时只把移出屏幕的行组重定位到另一侧，
 * 其余 7 行（28 张缩略图）保持原对象/缓冲，不随每行滚动全部重载。
 */
static void update_grid_window(void)
{
    if (!s_overview_visible || s_folder >= IMAGE_FOLDER_COUNT) return;

    uint32_t count = s_ready[s_folder] ? s_count[s_folder] : 0;
    uint32_t rows = (count + GRID_COLS - 1) / GRID_COLS;
    int32_t content_h = 2 * GRID_Y0 + (int32_t)rows * GRID_ROW_H - GRID_GAP_Y;
    if (content_h < GRID_VIEW_H) content_h = GRID_VIEW_H;
    if (content_h != s_grid_content_h) {
        lv_obj_set_height(s_overview_content, content_h);
        s_grid_content_h = content_h;
    }

    if (!count) {
        for (uint32_t i = 0; i < GRID_SLOTS; ++i) hide_tile(i);
        s_grid_valid = false;
        s_window_first = -1;
        return;
    }

    int32_t scroll_y = lv_obj_get_scroll_y(s_overview_scroll);
    if (scroll_y < 0) scroll_y = 0;
    int32_t first = scroll_y / GRID_ROW_H;
    int32_t max_first = rows > GRID_WINDOW_ROWS ?
                        (int32_t)rows - GRID_WINDOW_ROWS : 0;
    if (first > max_first) first = max_first;
    if (first < 0) first = 0;

    if (!s_grid_valid) {
        for (unsigned g = 0; g < GRID_WINDOW_ROWS; ++g)
            set_group_row(g, first + (int32_t)g, count, rows);
        s_window_first = first;
        s_grid_valid = true;
        return;
    }

    /* 向下滚动：把顶部行组移到窗口底部；向上滚动反之。 */
    while (s_window_first < first) {
        int group = group_for_row(s_window_first);
        if (group < 0) {
            s_grid_valid = false;
            return;
        }
        set_group_row((unsigned)group, s_window_first + GRID_WINDOW_ROWS,
                      count, rows);
        ++s_window_first;
    }
    while (s_window_first > first) {
        int group = group_for_row(s_window_first + GRID_WINDOW_ROWS - 1);
        if (group < 0) {
            s_grid_valid = false;
            return;
        }
        set_group_row((unsigned)group, s_window_first - 1, count, rows);
        --s_window_first;
    }
}

/* ============================ 首页 ============================ */

static void clear_home_previews(void)
{
    for (unsigned f = 0; f < IMAGE_FOLDER_COUNT; ++f) {
        s_home_dsc[f].data = NULL;
        s_home_dsc[f].data_size = 0;
        lv_image_cache_drop(&s_home_dsc[f]);
        set_hidden(s_home_imgs[f], true);
        set_hidden(s_home_phs[f], false);
    }
}

static void request_scan(void)
{
    if (s_scan_pending) return;
    s_scan_pending = true;
    s_last_scan_tick = lv_tick_get();
    s_scan_result = IMAGE_SCAN_OK;
    for (unsigned f = 0; f < IMAGE_FOLDER_COUNT; ++f) {
        s_ready[f] = false;
        s_count[f] = 0;
        s_truncated[f] = false;
    }
    clear_home_previews();
    reset_grid();
    update_home_status();
    update_overview_status();
    update_overview_note();
    image_viewer_request_scan();
}

/* ============================ 总览 ============================ */

static void overview_anim_x_cb(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void overview_show_done(lv_anim_t *a)
{
    (void)a;
    s_overview_transition = false;
    /* 锁屏页/总览页不透明：滑入完成后隐藏首页，避免总览滚动时 LVGL
     * 仍然重复绘制被完全遮住的首页预览。解锁或滑出前会恢复。 */
    if (s_overview_visible && !s_full_active) set_hidden(s_home, true);
}

static void overview_hide_done(lv_anim_t *a)
{
    (void)a;
    set_hidden(s_overview, true);
    lv_obj_set_x(s_overview, UI_HOR_RES);
    s_overview_visible = false;
    s_overview_transition = false;
    reset_grid();
}

static void open_overview(image_folder_t folder)
{
    if (folder >= IMAGE_FOLDER_COUNT || s_overview_visible ||
        s_overview_transition || s_full_active) return;
    s_folder = folder;
    s_overview_visible = true;
    s_overview_transition = true;
    reset_grid();
    lv_obj_scroll_to_y(s_overview_scroll, 0, LV_ANIM_OFF);
    update_overview_status();
    update_overview_note();
    set_hidden(s_home, false);
    set_hidden(s_overview, false);
    lv_obj_move_foreground(s_overview);
    lv_obj_set_x(s_overview, UI_HOR_RES);
    update_grid_window();
    ui_statusbar_set_visible(true);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_overview);
    lv_anim_set_values(&a, UI_HOR_RES, 0);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, overview_anim_x_cb);
    lv_anim_set_completed_cb(&a, overview_show_done);
    lv_anim_start(&a);
}

static void close_overview(void)
{
    if (!s_overview_visible || s_overview_transition || s_full_active) return;
    s_overview_transition = true;
    /* 取消在途缩略图，滑出期间不再启动新的 SD/JPEG 解码。 */
    image_viewer_clear_thumbs();
    set_hidden(s_home, false);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_overview);
    lv_anim_set_values(&a, 0, UI_HOR_RES);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, overview_anim_x_cb);
    lv_anim_set_completed_cb(&a, overview_hide_done);
    lv_anim_start(&a);
    update_overview_status();
    update_overview_note();
}

static void overview_pressed_cb(lv_event_t *event)
{
    (void)event;
    if (!s_overview_visible || s_overview_transition || s_full_active ||
        s_press_valid) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_indev_get_point(indev, &s_press);
    s_press_valid = true;
    s_swiped = false;
}

static void overview_released_cb(lv_event_t *event)
{
    (void)event;
    if (!s_press_valid) return;
    s_press_valid = false;
    if (!s_overview_visible || s_overview_transition || s_full_active) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_press.x;
    int32_t dy = p.y - s_press.y;
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;
    if (adx > 12 || ady > 12) s_swiped = true;
    /* 非边缘右滑返回首页；边缘滑动留给 ui_manager 打开模式/声光栏。 */
    if (dx > 50 && dx > dy && !is_edge_start(&s_press))
        close_overview();
}

static void gesture_press_lost_cb(lv_event_t *event)
{
    (void)event;
    s_press_valid = false;
    s_swiped = true;
}

static void overview_scroll_cb(lv_event_t *event)
{
    (void)event;
    update_grid_window();
}

/* ============================ 首页点击/拖动判定 ============================ */

static void open_full(uint32_t index);

static void tile_clicked_cb(lv_event_t *event)
{
    if (s_swiped) {
        s_swiped = false;
        return;
    }
    if (!s_overview_visible || s_overview_transition || s_full_active) return;
    unsigned slot = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (slot >= GRID_SLOTS || s_tile_index[slot] == UINT32_MAX) return;
    open_full(s_tile_index[slot]);
}

/* 首页卡片本身可点击，但左右边缘滑动是 ui_manager 打开模式/声光栏的
 * 手势。LVGL 仍可能在滑动结束后给卡片发 CLICKED，因此像视频列表一样
 * 记录按下/释放位移：明显拖动或 PRESS_LOST 后抑制这次点击，避免边缘
 * 滑动误进文件夹。 */
static void home_pressed_cb(lv_event_t *event)
{
    (void)event;
    if (s_press_valid) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_indev_get_point(indev, &s_press);
    s_press_valid = true;
    s_swiped = false;
}

static void home_released_cb(lv_event_t *event)
{
    (void)event;
    if (!s_press_valid) return;
    s_press_valid = false;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_press.x;
    int32_t dy = p.y - s_press.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > 12 || dy > 12) s_swiped = true;
}

static void home_press_lost_cb(lv_event_t *event)
{
    (void)event;
    s_press_valid = false;
    s_swiped = true;
}

static void home_card_clicked_cb(lv_event_t *event)
{
    if (s_swiped) {
        s_swiped = false;
        return;
    }
    if (s_overview_visible || s_overview_transition || s_full_active) return;
    image_folder_t folder = (image_folder_t)(uintptr_t)lv_event_get_user_data(event);
    open_overview(folder);
}

/* ============================ 全屏控制台 ============================ */

static void controls_activity(void)
{
    s_last_activity = lv_tick_get();
}

static void controls_fade_exec(void *obj, int32_t opa)
{
    s_controls_opa = opa;
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)opa, 0);
}

static void controls_fade_done(lv_anim_t *a)
{
    (void)a;
    if (!s_controls_visible)
        lv_obj_add_flag(s_full_controls, LV_OBJ_FLAG_HIDDEN);
}

static void controls_show(void)
{
    if (!s_full_active || !s_full_controls) return;
    lv_anim_delete(s_full_controls, controls_fade_exec);
    lv_obj_remove_flag(s_full_controls, LV_OBJ_FLAG_HIDDEN);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_full_controls);
    lv_anim_set_values(&a, s_controls_opa, LV_OPA_COVER);
    lv_anim_set_duration(&a, FULL_CONTROLS_FADE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, controls_fade_exec);
    lv_anim_set_completed_cb(&a, controls_fade_done);
    lv_anim_start(&a);
    s_controls_visible = true;
    controls_activity();
}

static void controls_hide(void)
{
    if (!s_full_controls || !s_controls_visible) return;
    s_controls_visible = false;
    lv_anim_delete(s_full_controls, controls_fade_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_full_controls);
    lv_anim_set_values(&a, s_controls_opa, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, FULL_CONTROLS_FADE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, controls_fade_exec);
    lv_anim_set_completed_cb(&a, controls_fade_done);
    lv_anim_start(&a);
}

static void controls_hide_immediate(void)
{
    if (!s_full_controls) return;
    lv_anim_delete(s_full_controls, controls_fade_exec);
    s_controls_visible = false;
    s_controls_opa = LV_OPA_TRANSP;
    lv_obj_set_style_opa(s_full_controls, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_full_controls, LV_OBJ_FLAG_HIDDEN);
}

static void controls_toggle(void)
{
    if (s_controls_visible) controls_hide();
    else controls_show();
}

/* ============================ 全屏左右切换动画 ============================ */
/* 与视频播放器相同的交叉滑动：旧图向一侧滑出，新图从另一侧滑入。
 * 新图解码完成后才启动；解码期间旧图保持可见，控制台先收起。 */
static void full_canvas_anim_x_cb(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void full_slide_done(lv_anim_t *a)
{
    (void)a;
    /* 停止旧图动画、隐藏旧图并交换当前/备用canvas指针。 */
    lv_anim_delete(s_full_canvas, full_canvas_anim_x_cb);
    set_hidden(s_full_canvas, true);
    lv_obj_set_pos(s_full_canvas, 0, 0);
    lv_obj_set_pos(s_full_canvas_alt, 0, 0);
    lv_obj_t *previous = s_full_canvas;
    s_full_canvas = s_full_canvas_alt;
    s_full_canvas_alt = previous;
    s_full_canvas_has_buffer = true;
    s_full_sliding = false;
    controls_show();
    controls_activity();
}

/* ============================ 全屏手势 ============================ */

static void exit_full(void);

static void full_back_clicked_cb(lv_event_t *event)
{
    (void)event;
    exit_full();
}

static void full_pressed_cb(lv_event_t *event)
{
    (void)event;
    /* 加载/交叉滑动期间禁止再开始手势，避免覆盖目标和动画。 */
    if (!s_full_active || s_full_loading || s_full_sliding || s_press_valid)
        return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_indev_get_point(indev, &s_press);
    s_press_valid = true;
    s_swiped = false;
    controls_activity();
}

static void full_released_cb(lv_event_t *event)
{
    (void)event;
    if (!s_full_active || s_full_loading || s_full_sliding || !s_press_valid)
        return;
    s_press_valid = false;
    /* 返回键/子控件的事件不冒泡到这里；只处理图片空白区域。 */
    if (lv_event_get_target(event) != s_full) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_press.x;
    int32_t dy = p.y - s_press.y;
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;
    if (adx > 12 || ady > 12) s_swiped = true;
    if (adx > 50 && adx > ady) {
        /* 边缘横滑留给 ui_manager 打开面板；非边缘切换上一张/下一张。 */
        if (!is_edge_start(&s_press)) {
            uint32_t next;
            if (dx < 0)
                next = (s_full_index + 1U) % s_full_count;
            else
                next = (s_full_index + s_full_count - 1U) % s_full_count;
            if (next != s_full_index) {
                /* 左滑(下一张)时新图从右侧滑入、旧图向左滑出；右滑相反。 */
                s_full_switch_dir = dx < 0 ? 1 : -1;
                s_full_loading = true;
                ++s_full_token;
                set_label(s_full_status, "正在加载...");
                /* 加载可能受 SD 锁影响而较慢：控制台保持可见，用户仍可
                 * 点返回取消，不会被困在无按钮的加载画面。滑动开始前再
                 * 收起控制台，保证交叉滑动画面干净。 */
                controls_activity();
                image_viewer_request_full(s_full_folder, next, s_full_token);
            }
        }
        return;
    }
    if (adx < 12 && ady < 12) controls_toggle();
}

static void open_full(uint32_t index)
{
    if (s_full_active || s_overview_transition || !s_overview_visible) return;
    if (s_folder >= IMAGE_FOLDER_COUNT || !s_ready[s_folder] ||
        index >= s_count[s_folder]) return;
    s_full_active = true;
    s_full_folder = s_folder;
    s_full_count = s_count[s_folder];
    s_full_index = index;
    s_full_loading = true;
    s_full_canvas_has_buffer = false;
    s_full_sliding = false;
    s_full_switch_dir = 0;
    ++s_full_token;
    s_controls_visible = false;
    lv_anim_delete(s_full_controls, controls_fade_exec);
    s_controls_opa = LV_OPA_TRANSP;
    lv_obj_set_style_opa(s_full_controls, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_full_controls, LV_OBJ_FLAG_HIDDEN);
    lv_anim_delete(s_full_canvas, full_canvas_anim_x_cb);
    lv_anim_delete(s_full_canvas_alt, full_canvas_anim_x_cb);
    lv_obj_set_pos(s_full_canvas, 0, 0);
    lv_obj_set_pos(s_full_canvas_alt, 0, 0);
    set_hidden(s_full_canvas, true);
    set_hidden(s_full_canvas_alt, true);
    set_label(s_full_status, "正在加载...");
    lv_label_set_text(s_full_name, "");
    ui_statusbar_set_visible(false);
    set_hidden(s_overview, true);
    set_hidden(s_home, true);
    set_hidden(s_full, false);
    lv_obj_move_foreground(s_full);
    /* 全屏时停止总览缩略图在途解码，并让后台预取上一张/下一张。 */
    image_viewer_clear_thumbs();
    image_viewer_set_full_active(true);
    controls_show();
    controls_activity();
    image_viewer_request_full(s_full_folder, index, s_full_token);
}

static void exit_full(void)
{
    if (!s_full_active) return;
    ++s_full_token;
    image_viewer_cancel_full();
    image_viewer_set_full_active(false);
    image_viewer_clear_thumbs();
    s_full_active = false;
    s_full_loading = false;
    s_full_sliding = false;
    s_full_switch_dir = 0;
    s_full_canvas_has_buffer = false;
    s_controls_visible = false;
    lv_anim_delete(s_full_controls, controls_fade_exec);
    lv_anim_delete(s_full_canvas, full_canvas_anim_x_cb);
    lv_anim_delete(s_full_canvas_alt, full_canvas_anim_x_cb);
    s_controls_opa = LV_OPA_TRANSP;
    lv_obj_set_style_opa(s_full_controls, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_full_controls, LV_OBJ_FLAG_HIDDEN);
    set_hidden(s_full, true);
    set_hidden(s_full_canvas, true);
    set_hidden(s_full_canvas_alt, true);
    lv_obj_set_pos(s_full_canvas, 0, 0);
    lv_obj_set_pos(s_full_canvas_alt, 0, 0);
    set_label(s_full_status, NULL);
    /* 全屏是从总览打开的；返回总览并恢复状态栏。 */
    set_hidden(s_overview, false);
    lv_obj_move_foreground(s_overview);
    set_hidden(s_home, true);
    ui_statusbar_set_visible(true);
    update_grid_window();
}

/* ============================ 后台回调 ============================ */

static void library_cb(image_folder_t folder, uint32_t count, bool truncated,
                       uint32_t generation, void *ctx)
{
    (void)ctx;
    if (folder >= IMAGE_FOLDER_COUNT) return;
    bool generation_changed = s_generation[folder] != generation;
    s_count[folder] = count;
    s_truncated[folder] = truncated;
    s_generation[folder] = generation;
    s_ready[folder] = true;
    if (generation_changed && s_overview_visible)
        reset_grid();
    if (folder == s_folder && s_overview_visible) {
        /* 即使 generation 未变（重扫后文件数变化），也要按新 count 重新
         * 校验8行窗口，隐藏超出范围/标记新的可见槽位；未变化的索引会
         * 因 tile_index 相同而复用已有缩略图，不重复解码。 */
        s_grid_valid = false;
        update_grid_window();
        update_overview_note();
    }
    update_home_status();
    if (s_overview_visible) update_overview_status();
}

static void home_preview_cb(image_folder_t folder, const uint8_t *pixels,
                            uint16_t w, uint16_t h, uint32_t generation,
                            bool ok, void *ctx)
{
    (void)ctx;
    if (folder >= IMAGE_FOLDER_COUNT) return;
    if (!ok || !pixels || generation != s_generation[folder] ||
        w != IMAGE_HOME_W || h != IMAGE_HOME_H) {
        set_hidden(s_home_imgs[folder], true);
        set_hidden(s_home_phs[folder], false);
        return;
    }
    set_rgb565_dsc(&s_home_dsc[folder], pixels, w, h);
    lv_image_cache_drop(&s_home_dsc[folder]);
    lv_image_set_src(s_home_imgs[folder], &s_home_dsc[folder]);
    set_hidden(s_home_imgs[folder], false);
    set_hidden(s_home_phs[folder], true);
}

static void scan_done_cb(image_scan_result_t result, uint32_t generation,
                         void *ctx)
{
    (void)ctx;
    (void)generation;
    s_scan_pending = false;
    s_scan_result = result;
    if (result != IMAGE_SCAN_OK) {
        s_ready[0] = false;
        s_ready[1] = false;
    }
    update_home_status();
    if (s_overview_visible) {
        if (result != IMAGE_SCAN_OK) reset_grid();
        update_grid_window();
        update_overview_status();
        update_overview_note();
    }
    ESP_LOGI(TAG, "Image scan done: result=%d cam=%lu pictures=%lu",
             (int)result,
             (unsigned long)s_count[IMAGE_FOLDER_CAM],
             (unsigned long)s_count[IMAGE_FOLDER_PICTURES]);
}

static void thumbnail_cb(uint32_t slot, const uint8_t *pixels, uint16_t w,
                         uint16_t h, uint32_t token, bool ok, void *ctx)
{
    (void)ctx;
    if (slot >= GRID_SLOTS || token != s_tile_token[slot]) return;
    if (!ok || !pixels || w != IMAGE_THUMB_W || h != IMAGE_THUMB_H ||
        s_tile_index[slot] == UINT32_MAX) return;
    set_rgb565_dsc(&s_thumb_dsc[slot], pixels, w, h);
    lv_image_cache_drop(&s_thumb_dsc[slot]);
    lv_image_set_src(s_tile_imgs[slot], &s_thumb_dsc[slot]);
    set_hidden(s_tile_imgs[slot], false);
    set_hidden(s_tile_phs[slot], true);
}

static void full_image_cb(const uint8_t *pixels, uint16_t w, uint16_t h,
                          image_folder_t folder, uint32_t index,
                          const char *name, uint32_t token, bool ok,
                          image_error_t error, void *ctx)
{
    (void)ctx;
    if (!s_full_active || token != s_full_token || folder != s_full_folder)
        return;
    if (!ok || !pixels || w != IMAGE_FULL_PAD_W || h != IMAGE_FULL_PAD_H) {
        s_full_loading = false;
        s_full_sliding = false;
        s_full_switch_dir = 0;
        const char *text = "图片读取失败";
        if (error == IMAGE_ERROR_BUSY) text = "SD 卡忙，请稍后重试";
        else if (error == IMAGE_ERROR_SIZE) text = "图片尺寸需为460x460";
        else if (error == IMAGE_ERROR_FORMAT) text = "图片格式不支持";
        set_label(s_full_status, text);
        controls_show();
        controls_activity();
        return;
    }
    s_full_index = index;
    s_full_loading = false;
    set_label(s_full_status, NULL);
    if (name && name[0]) lv_label_set_text(s_full_name, name);

    if (s_full_canvas_has_buffer && s_full_switch_dir != 0) {
        /* 交叉滑动：旧图留在当前canvas，新图放进备用canvas；
         * 解码已完成后再启动动画，等待期间旧图保持可见。 */
        int32_t dir = s_full_switch_dir;
        s_full_switch_dir = 0;
        lv_canvas_set_buffer(s_full_canvas_alt, (void *)pixels, w, h,
                             LV_COLOR_FORMAT_RGB565);
        lv_obj_invalidate(s_full_canvas_alt);
        lv_obj_set_pos(s_full_canvas_alt, dir * (int32_t)IMAGE_FULL_PAD_W, 0);
        set_hidden(s_full_canvas_alt, false);
        lv_obj_set_pos(s_full_canvas, 0, 0);
        set_hidden(s_full_canvas, false);
        controls_hide_immediate();
        s_full_sliding = true;
        lv_anim_delete(s_full_canvas, full_canvas_anim_x_cb);
        lv_anim_delete(s_full_canvas_alt, full_canvas_anim_x_cb);

        lv_anim_t out;
        lv_anim_init(&out);
        lv_anim_set_var(&out, s_full_canvas);
        lv_anim_set_values(&out, 0, -dir * (int32_t)IMAGE_FULL_PAD_W);
        lv_anim_set_duration(&out, FULL_SWITCH_MS);
        lv_anim_set_path_cb(&out, UI_ANIM_PATH);
        lv_anim_set_exec_cb(&out, full_canvas_anim_x_cb);
        lv_anim_start(&out);

        lv_anim_t in;
        lv_anim_init(&in);
        lv_anim_set_var(&in, s_full_canvas_alt);
        lv_anim_set_values(&in, dir * (int32_t)IMAGE_FULL_PAD_W, 0);
        lv_anim_set_duration(&in, FULL_SWITCH_MS);
        lv_anim_set_path_cb(&in, UI_ANIM_PATH);
        lv_anim_set_exec_cb(&in, full_canvas_anim_x_cb);
        lv_anim_set_completed_cb(&in, full_slide_done);
        lv_anim_start(&in);
        return;
    }

    /* 首次打开：没有旧图，直接显示；不播放切换动画。 */
    lv_canvas_set_buffer(s_full_canvas, (void *)pixels, w, h,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(s_full_canvas);
    set_hidden(s_full_canvas, false);
    set_hidden(s_full_canvas_alt, true);
    s_full_canvas_has_buffer = true;
    controls_show();
    controls_activity();
}

static const image_viewer_ops_t s_ops = {
    .library = library_cb,
    .home_preview = home_preview_cb,
    .scan_done = scan_done_cb,
    .thumbnail = thumbnail_cb,
    .full_image = full_image_cb,
};

/* ============================ 定时器 ============================ */

static bool image_page_active(void)
{
    return ui_manager_get_current_mode() == UI_MODE_IMAGE &&
           !ui_lockscreen_is_visible() &&
           !lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_full_active && s_controls_visible && !s_full_loading &&
        !s_full_sliding &&
        lv_tick_elaps(s_last_activity) >= FULL_CONTROLS_HIDE_MS)
        controls_hide();

    if (!image_page_active()) return;

    uint32_t generation = sd_card_get_generation();
    if (generation != s_sd_generation) {
        s_sd_generation = generation;
        if (s_full_active) exit_full();
        request_scan();
        return;
    }
    /* worker 未创建/卡死时不让页面永远停在“正在扫描”：超时后按错误处理，
     * 后续5秒重试仍可恢复。正常4096张图片的扫描远小于15秒。 */
    if (s_scan_pending && lv_tick_elaps(s_last_scan_tick) >= 15000) {
        s_scan_pending = false;
        s_scan_result = IMAGE_SCAN_ERROR;
        s_ready[0] = false;
        s_ready[1] = false;
        update_home_status();
        update_overview_status();
        update_overview_note();
    }
    /* 首次扫描失败后每5秒重试；没有卡时不空转，插卡会改变 generation
     * 并立即触发新的扫描。 */
    if ((!s_ready[0] || !s_ready[1]) && !s_scan_pending &&
        sd_card_is_present() &&
        lv_tick_elaps(s_last_scan_tick) >= 5000)
        request_scan();
}

/* ============================ 模式生命周期 ============================ */

void ui_image_viewer_on_mode_enter(void)
{
    if (s_full_active) exit_full();
    if (s_overview_visible) {
        lv_anim_delete(s_overview, overview_anim_x_cb);
        s_overview_transition = false;
        s_overview_visible = false;
        set_hidden(s_overview, true);
        lv_obj_set_x(s_overview, UI_HOR_RES);
        reset_grid();
    }
    set_hidden(s_full, true);
    set_hidden(s_full_canvas, true);
    set_hidden(s_overview, true);
    lv_obj_set_x(s_overview, UI_HOR_RES);
    set_hidden(s_home, false);
    ui_statusbar_set_visible(true);
    s_sd_generation = sd_card_get_generation();
    if (!s_ready[0] || !s_ready[1] ||
        s_generation[0] != s_sd_generation ||
        s_generation[1] != s_sd_generation)
        request_scan();
    else {
        update_home_status();
        update_overview_status();
        update_overview_note();
    }
}

void ui_image_viewer_on_mode_leave(void)
{
    if (s_full_active) exit_full();
    if (s_overview_visible) {
        lv_anim_delete(s_overview, overview_anim_x_cb);
        s_overview_transition = false;
        s_overview_visible = false;
        set_hidden(s_overview, true);
        lv_obj_set_x(s_overview, UI_HOR_RES);
        set_hidden(s_home, false);
    }
    reset_grid();
    image_viewer_cancel_full();
    image_viewer_clear_thumbs();
    ui_statusbar_set_visible(true);
}

void ui_image_viewer_close_for_lock(void)
{
    /* 锁屏只收起全屏；总览状态保留在锁屏页下方，解锁后继续浏览。 */
    if (s_full_active) exit_full();
    image_viewer_cancel_full();
    image_viewer_clear_thumbs();
    ui_statusbar_set_visible(true);
}

bool ui_image_viewer_is_fullscreen_active(void)
{
    return s_full_active;
}

/* ============================ 页面创建 ============================ */

static lv_obj_t *create_back_button(lv_obj_t *parent)
{
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
    lv_obj_add_event_cb(btn, full_back_clicked_cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

static void create_home_card(lv_obj_t *parent, image_folder_t folder)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, HOME_CARD_W, HOME_CARD_H);
    lv_obj_set_pos(card, HOME_CARD_X,
                   HOME_CARD_Y0 + (int32_t)folder * (HOME_CARD_H + HOME_CARD_GAP));
    lv_obj_set_style_bg_color(card, UI_COLOR_LIST_ROW_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, HOME_CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    /* 让卡片上的边缘滑动仍能冒泡到 ui_manager 打开模式/声光栏。 */
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(card, home_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(card, home_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(card, home_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(card, home_card_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)folder);

    /* 左侧第一张图片预览；圆角容器裁切，占位图标在没有图片时显示。 */
    lv_obj_t *preview = lv_obj_create(card);
    lv_obj_set_size(preview, HOME_PREVIEW_SIZE, HOME_PREVIEW_SIZE);
    lv_obj_set_pos(preview, HOME_PREVIEW_X, HOME_PREVIEW_Y);
    lv_obj_set_style_bg_color(preview, UI_COLOR_CONTROL_BG, 0);
    lv_obj_set_style_bg_opa(preview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(preview, 0, 0);
    lv_obj_set_style_radius(preview, HOME_PREVIEW_RADIUS, 0);
    lv_obj_set_style_clip_corner(preview, true, 0);
    lv_obj_set_style_pad_all(preview, 0, 0);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(preview, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *img = lv_image_create(preview);
    lv_obj_set_size(img, HOME_PREVIEW_SIZE, HOME_PREVIEW_SIZE);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    s_home_imgs[folder] = img;

    lv_obj_t *ph = lv_label_create(preview);
    lv_label_set_text(ph, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(ph, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(ph, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_center(ph);
    lv_obj_remove_flag(ph, LV_OBJ_FLAG_CLICKABLE);
    s_home_phs[folder] = ph;

    /* 名称固定在卡片中央；左侧预览不改变文字的水平中心。 */
    lv_obj_t *name = lv_label_create(card);
    lv_obj_set_width(name, HOME_CARD_W);
    lv_obj_set_style_text_align(name, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(name, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(name, ui_font_22(), 0);
    lv_label_set_long_mode(name, LV_LABEL_LONG_DOT);
    lv_label_set_text(name, s_card_names[folder]);
    lv_obj_align(name, LV_ALIGN_CENTER, 0, 0);
    lv_obj_remove_flag(name, LV_OBJ_FLAG_CLICKABLE);
}

static void create_grid_tile(lv_obj_t *parent, unsigned slot)
{
    lv_obj_t *tile = lv_obj_create(parent);
    lv_obj_set_size(tile, GRID_THUMB, GRID_THUMB);
    lv_obj_set_style_bg_color(tile, UI_COLOR_LIST_ROW_BG, 0);
    lv_obj_set_style_bg_opa(tile, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(tile, 12, 0);
    lv_obj_set_style_clip_corner(tile, true, 0);
    lv_obj_set_style_border_width(tile, 0, 0);
    lv_obj_set_style_pad_all(tile, 0, 0);
    lv_obj_remove_flag(tile, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(tile, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(tile, overview_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(tile, overview_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(tile, gesture_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(tile, tile_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)slot);
    s_tiles[slot] = tile;

    lv_obj_t *img = lv_image_create(tile);
    lv_obj_set_size(img, GRID_THUMB, GRID_THUMB);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    s_tile_imgs[slot] = img;

    lv_obj_t *ph = lv_label_create(tile);
    lv_label_set_text(ph, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_font(ph, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(ph, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_center(ph);
    lv_obj_remove_flag(ph, LV_OBJ_FLAG_CLICKABLE);
    s_tile_phs[slot] = ph;

    s_tile_index[slot] = UINT32_MAX;
    s_tile_token[slot] = (uint32_t)slot + 1U;
    set_hidden(tile, true);
}

static void create_overview(lv_obj_t *parent)
{
    s_overview = lv_obj_create(parent);
    lv_obj_set_size(s_overview, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_overview, UI_HOR_RES, 0);
    lv_obj_set_style_bg_color(s_overview, UI_COLOR_PAGE_BG, 0);
    lv_obj_set_style_bg_opa(s_overview, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_overview, 0, 0);
    lv_obj_set_style_radius(s_overview, 0, 0);
    lv_obj_set_style_pad_all(s_overview, 0, 0);
    lv_obj_remove_flag(s_overview, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_overview, LV_OBJ_FLAG_EVENT_BUBBLE);
    set_hidden(s_overview, true);

    s_overview_scroll = lv_obj_create(s_overview);
    lv_obj_set_size(s_overview_scroll, UI_HOR_RES, GRID_VIEW_H);
    lv_obj_set_pos(s_overview_scroll, 0, GRID_TOP);
    lv_obj_set_style_bg_opa(s_overview_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overview_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_overview_scroll, 0, 0);
    lv_obj_set_scroll_dir(s_overview_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_overview_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(s_overview_scroll, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_overview_scroll, UI_COLOR_TEXT_MUTED, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_overview_scroll, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_overview_scroll, 2, LV_PART_SCROLLBAR);
    lv_obj_add_flag(s_overview_scroll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_overview_scroll, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_overview_scroll, overview_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_overview_scroll, overview_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_overview_scroll, gesture_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_overview_scroll, overview_scroll_cb, LV_EVENT_SCROLL, NULL);

    s_overview_content = lv_obj_create(s_overview_scroll);
    lv_obj_set_size(s_overview_content, UI_HOR_RES, GRID_VIEW_H);
    lv_obj_set_pos(s_overview_content, 0, 0);
    lv_obj_set_style_bg_opa(s_overview_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_overview_content, 0, 0);
    lv_obj_set_style_pad_all(s_overview_content, 0, 0);
    lv_obj_remove_flag(s_overview_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_overview_content, LV_OBJ_FLAG_CLICKABLE);

    for (unsigned i = 0; i < GRID_SLOTS; ++i) create_grid_tile(s_overview_content, i);

    /* 状态/截断提示固定在总览页上，不随缩略图滚动。 */
    s_overview_status = lv_label_create(s_overview);
    lv_obj_set_width(s_overview_status, UI_HOR_RES - 40);
    lv_obj_set_style_text_align(s_overview_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_overview_status, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_overview_status, ui_font_18(), 0);
    lv_label_set_long_mode(s_overview_status, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_overview_status, "");
    lv_obj_align(s_overview_status, LV_ALIGN_CENTER, 0, GRID_TOP / 2);
    set_hidden(s_overview_status, true);

    s_overview_note = lv_label_create(s_overview);
    lv_obj_set_width(s_overview_note, UI_HOR_RES - 24);
    lv_obj_set_style_text_align(s_overview_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_overview_note, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_overview_note, ui_font_18(), 0);
    lv_label_set_long_mode(s_overview_note, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_overview_note, "");
    lv_obj_align(s_overview_note, LV_ALIGN_BOTTOM_MID, 0, -10);
    set_hidden(s_overview_note, true);
}

static void create_full(lv_obj_t *parent)
{
    s_full = lv_obj_create(parent);
    lv_obj_set_size(s_full, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_full, 0, 0);
    lv_obj_set_style_bg_color(s_full, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_full, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_full, 0, 0);
    lv_obj_set_style_radius(s_full, 0, 0);
    lv_obj_set_style_pad_all(s_full, 0, 0);
    lv_obj_remove_flag(s_full, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_full, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_full, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_full, full_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_full, full_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_full, gesture_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
    set_hidden(s_full, true);

    /* 464x464 缓冲由 JPEG 协议 16 对齐产生；页面 460x460 裁掉右侧/下侧4像素。 */
    s_full_canvas = lv_canvas_create(s_full);
    lv_obj_set_size(s_full_canvas, IMAGE_FULL_PAD_W, IMAGE_FULL_PAD_H);
    lv_obj_set_pos(s_full_canvas, 0, 0);
    lv_obj_remove_flag(s_full_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_full_canvas, LV_OBJ_FLAG_SCROLLABLE);
    set_hidden(s_full_canvas, true);

    /* 第二块 canvas：左右切换时承载新图，与当前图做交叉滑动。
     * 两块都使用后台双缓冲的464x464指针，不做整帧拷贝。 */
    s_full_canvas_alt = lv_canvas_create(s_full);
    lv_obj_set_size(s_full_canvas_alt, IMAGE_FULL_PAD_W, IMAGE_FULL_PAD_H);
    lv_obj_set_pos(s_full_canvas_alt, 0, 0);
    lv_obj_remove_flag(s_full_canvas_alt, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_full_canvas_alt, LV_OBJ_FLAG_SCROLLABLE);
    set_hidden(s_full_canvas_alt, true);

    s_full_status = lv_label_create(s_full);
    lv_obj_set_width(s_full_status, UI_HOR_RES - 40);
    lv_obj_set_style_text_align(s_full_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_full_status, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_full_status, ui_font_18(), 0);
    lv_label_set_long_mode(s_full_status, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_full_status, "");
    lv_obj_center(s_full_status);
    set_hidden(s_full_status, true);

    /* 控制台与视频播放器同结构：顶部40%黑scrim + 44x44返回键 + 名称。 */
    s_full_controls = lv_obj_create(s_full);
    lv_obj_set_size(s_full_controls, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_full_controls, 0, 0);
    lv_obj_set_style_bg_opa(s_full_controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_full_controls, 0, 0);
    lv_obj_set_style_pad_all(s_full_controls, 0, 0);
    lv_obj_remove_flag(s_full_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_full_controls, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(s_full_controls, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_full_controls, LV_OBJ_FLAG_HIDDEN);
    s_controls_opa = LV_OPA_TRANSP;

    lv_obj_t *scrim = lv_obj_create(s_full_controls);
    lv_obj_set_size(scrim, UI_HOR_RES, FULL_TOP_H);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_40, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = create_back_button(s_full_controls);
    s_full_name = lv_label_create(s_full_controls);
    lv_obj_set_width(s_full_name, UI_HOR_RES - 64 - 12);
    lv_obj_set_style_text_align(s_full_name, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(s_full_name, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_full_name, ui_font_18(), 0);
    lv_label_set_long_mode(s_full_name, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_full_name, "");
    lv_obj_align_to(s_full_name, back, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_remove_flag(s_full_name, LV_OBJ_FLAG_CLICKABLE);
}

void ui_image_viewer_create(lv_obj_t *parent)
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

    s_home = lv_obj_create(s_page);
    lv_obj_set_size(s_home, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_home, 0, 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_home, 0, 0);
    lv_obj_set_style_pad_all(s_home, 0, 0);
    lv_obj_remove_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_EVENT_BUBBLE);

    for (unsigned f = 0; f < IMAGE_FOLDER_COUNT; ++f)
        create_home_card(s_home, (image_folder_t)f);

    /* 扫描/错误提示覆盖在卡片上方，不改变两张卡片的布局。 */
    s_home_status = lv_label_create(s_home);
    lv_obj_set_width(s_home_status, 400);
    lv_obj_set_style_text_align(s_home_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_home_status, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_home_status, ui_font_18(), 0);
    lv_obj_set_style_bg_color(s_home_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_home_status, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_home_status, 10, 0);
    lv_obj_set_style_pad_all(s_home_status, 10, 0);
    lv_label_set_long_mode(s_home_status, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_home_status, "");
    lv_obj_align(s_home_status, LV_ALIGN_CENTER, 0, 0);
    set_hidden(s_home_status, true);

    create_overview(s_page);
    create_full(s_page);

    for (unsigned f = 0; f < IMAGE_FOLDER_COUNT; ++f) {
        s_generation[f] = 0;
        s_ready[f] = false;
        s_count[f] = 0;
    }
    s_folder = IMAGE_FOLDER_CAM;
    s_window_first = -1;
    s_grid_valid = false;
    s_grid_content_h = -1;
    s_sd_generation = sd_card_get_generation();
    s_scan_result = IMAGE_SCAN_OK;
    image_viewer_set_ops(&s_ops, NULL);
    s_timer = lv_timer_create(ui_timer_cb, 100, NULL);
    if (!s_timer) ESP_LOGW(TAG, "Image viewer timer unavailable");
}

lv_obj_t *ui_image_viewer_get_page(void)
{
    return s_page;
}
