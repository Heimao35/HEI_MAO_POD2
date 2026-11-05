/**
 * @file ui_wallpaper_picker.c
 * @brief 壁纸选择页实现：复用图片查看器“其他图片”的浏览器 + 确认/取消。
 *
 * 页面层级（s_page 内）：
 *   s_scroll / s_content  四列缩略图总览（与图片查看器同一套几何参数）
 *   s_status / s_note     扫描/错误提示、截断提示
 *   s_full                全屏预览：canvas + 底部两个圆角矩形按键
 *
 * 重要约束：
 * - 本文件不做 SD/JPEG 读取；扫描、缩略图和全屏图全部来自 image_viewer
 *   worker。打开本页时先让图片查看器注销自己的回调，再注册本页回调；
 *   关闭时恢复图片查看器回调，二者不会同时收到同一个后台结果。
 * - 缩略图槽位固定 32 个（4 列 × 8 行窗口），滚动时环形复用，不为每张
 *   图片创建对象或缓冲；token 使用 0x40000000 起始的高位空间，与图片
 *   查看器的小 token 不相交，切换回调后迟到的旧结果会被丢弃。
 * - 全屏图没有控制台，也没有左右切图；底部“取消/确认”直接覆盖在图片
 *   下方，确认后由 wallpaper 模块把像素写入 Flash，不在这里擦写。
 * - 文案使用 18px 全字库（状态/按键）和 22px 全字库（如需标题）。
 */
#include "ui_wallpaper_picker.h"

#include "image_viewer.h"
#include "sd_card.h"
#include "ui_fonts.h"
#include "ui_image_viewer.h"
#include "ui_statusbar.h"
#include "wallpaper.h"

#include "esp_log.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_wallpaper";

/* ============================ 布局参数 ============================ */
/* 与 ui_image_viewer.c 的 GRID_* 完全一致：4 列 × 100px、列/行间距 12、
 * 左右边距 12，网格从状态栏下方开始。壁纸页只显示一个文件夹，所以
 * 不需要首页卡片区域；改这些数值时必须同步图片查看器，保持两页一致。 */
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

/* 全屏预览底部按键：160x52、圆角 16，左右各留 20px，底部 16px。
 * 图片本身仍是全屏 460x460；按键作为半透明/实色图层覆盖在画面底部，
 * 这是全屏壁纸预览下唯一能同时看到完整画面和操作按钮的布局。 */
#define BTN_W                160
#define BTN_H                52
#define BTN_RADIUS           16
#define BTN_Y                392
#define BTN_MARGIN           20

/* 与图片查看器相同的 token 高位段：图片查看器的槽位 token 从 1 开始
 * 缓慢递增，本页使用 0x40000000 以上，两者不会碰撞。 */
#define TOKEN_BASE           0x40000000U

_Static_assert(GRID_SLOTS <= IMAGE_MAX_THUMB_SLOTS,
               "wallpaper grid needs more thumbnail slots than the backend provides");

/* ============================ 页面对象 ============================ */
static lv_obj_t *s_page;
static lv_obj_t *s_scroll;
static lv_obj_t *s_content;
static lv_obj_t *s_status;
static lv_obj_t *s_note;
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
static lv_obj_t *s_canvas;
static lv_obj_t *s_full_status;
static lv_obj_t *s_confirm;
static lv_obj_t *s_cancel;

static lv_timer_t *s_timer;

/* ============================ 运行状态 ============================ */
static bool s_visible;
static uint32_t s_count;
static uint32_t s_generation;
static bool s_ready;
static bool s_truncated;
static image_scan_result_t s_scan_result = IMAGE_SCAN_OK;
static bool s_scan_pending;
static uint32_t s_last_scan_tick;
static uint32_t s_sd_generation;

static bool s_full_active;
static bool s_full_loading;
static uint32_t s_full_index = UINT32_MAX;
static uint32_t s_full_token;
static const uint8_t *s_full_pixels; /**< 后台全屏图，只在本次打开期间有效 */
static bool s_save_pending;           /**< 已提交给 wallpaper，等待结果 */

/* 页面滑入/滑出期间忽略点击和手势：避免在 350ms 动画中打开全屏预览
 * 或在关闭途中再次进入。与图片查看器总览的 transition 语义一致。 */
static bool s_transition;
static bool s_press_valid;
static bool s_swiped;
static lv_point_t s_press;
static int32_t s_press_scroll_y;

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

/* 与图片查看器相同的 RGB565 图像描述符初始化；缩略图缓冲由后台复用，
 * 这里只保存指针，不复制像素。 */
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

static void update_status(void)
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
    } else if (!s_ready) {
        text = "正在扫描图片...";
    } else if (s_count == 0) {
        text = "暂无图片";
    }
    set_label(s_status, text);
}

static void update_note(void)
{
    if (!(s_ready && s_truncated && s_count > 0)) {
        set_label(s_note, NULL);
        return;
    }
    char text[40];
    snprintf(text, sizeof(text), "仅显示前%u张", (unsigned)IMAGE_INDEX_MAX);
    set_label(s_note, text);
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
 *
 * 与图片查看器总览的窗口复用逻辑一致：滚动只重定位移出屏幕的行组，
 * 其余 7 行保留已有对象和缩略图。
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
        image_viewer_request_thumbnail(IMAGE_FOLDER_PICTURES, index, slot,
                                       s_tile_token[slot]);
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
 * 其余行（最多 28 张缩略图）保持原对象/缓冲，不随每行滚动全部重载。
 */
static void update_grid_window(void)
{
    if (!s_visible || s_full_active) return;

    uint32_t count = s_ready ? s_count : 0;
    uint32_t rows = (count + GRID_COLS - 1) / GRID_COLS;
    int32_t content_h = 2 * GRID_Y0 + (int32_t)rows * GRID_ROW_H - GRID_GAP_Y;
    if (content_h < GRID_VIEW_H) content_h = GRID_VIEW_H;
    if (content_h != s_grid_content_h) {
        lv_obj_set_height(s_content, content_h);
        s_grid_content_h = content_h;
    }

    if (!count) {
        for (uint32_t i = 0; i < GRID_SLOTS; ++i) hide_tile(i);
        s_grid_valid = false;
        s_window_first = -1;
        return;
    }

    int32_t scroll_y = lv_obj_get_scroll_y(s_scroll);
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

/* ============================ 全屏预览与保存 ============================ */

/** @brief 按当前加载/保存状态刷新底部按键的可用性。 */
static void update_full_buttons(void)
{
    bool saving = s_save_pending;
    bool can_confirm = s_full_active && !s_full_loading && s_full_pixels && !saving;
    bool can_cancel = s_full_active && !saving;
    if (can_confirm) lv_obj_remove_state(s_confirm, LV_STATE_DISABLED);
    else lv_obj_add_state(s_confirm, LV_STATE_DISABLED);
    if (can_cancel) lv_obj_remove_state(s_cancel, LV_STATE_DISABLED);
    else lv_obj_add_state(s_cancel, LV_STATE_DISABLED);
}

/** @brief 退出全屏预览，回到缩略图总览（不改变已设置的壁纸）。 */
static void exit_full(void)
{
    if (!s_full_active) return;
    ++s_full_token;
    image_viewer_cancel_full();
    s_full_active = false;
    s_full_loading = false;
    s_full_pixels = NULL;
    s_full_index = UINT32_MAX;
    set_hidden(s_full, true);
    set_hidden(s_canvas, true);
    set_label(s_full_status, NULL);
    set_hidden(s_scroll, false);
    set_hidden(s_status, false);
    set_hidden(s_note, false);
    ui_statusbar_set_visible(true);
    update_status();
    update_note();
    update_grid_window();
}

static void cancel_clicked(lv_event_t *event)
{
    (void)event;
    if (s_save_pending) return;
    exit_full();
}

/**
 * @brief 确认按键：把当前全屏图提交给 wallpaper 模块。
 *
 * wallpaper_commit 只复制像素并启动一次性 worker；Flash 擦写不在
 * LVGL 回调中执行。这里只切换按键/提示状态，真正的成功或失败由
 * 100ms 定时器轮询 wallpaper_status() 后发布。
 */
static void confirm_clicked(lv_event_t *event)
{
    (void)event;
    if (!s_full_active || s_full_loading || !s_full_pixels || s_save_pending)
        return;
    esp_err_t ret = wallpaper_commit(s_full_pixels);
    if (ret == ESP_OK) {
        s_save_pending = true;
        lv_label_set_text(s_full_status, "正在保存壁纸...");
        lv_obj_remove_flag(s_full_status, LV_OBJ_FLAG_HIDDEN);
    } else if (ret == ESP_ERR_INVALID_STATE &&
               wallpaper_status() == WALLPAPER_STATE_SAVING) {
        /* 上一次保存（可能在本页打开前提交）仍在擦写：不排队第二份写入，
         * 也不把它的结果冒充成本次图片已设置。提示稍候重试，按键保持
         * 可用；等后台空闲后再次点“确认”即可保存当前图片。 */
        lv_label_set_text(s_full_status, "壁纸正在保存，请稍候");
        lv_obj_remove_flag(s_full_status, LV_OBJ_FLAG_HIDDEN);
    } else {
        /* 存储不可用（未烧录新分区表）或内存不足：明确提示，旧壁纸不变。 */
        lv_label_set_text(s_full_status, "壁纸存储不可用");
        lv_obj_remove_flag(s_full_status, LV_OBJ_FLAG_HIDDEN);
        ESP_LOGW(TAG, "Wallpaper commit rejected: %s", esp_err_to_name(ret));
    }
    update_full_buttons();
}

/** @brief 打开一张缩略图对应的全屏预览并请求后台解码。 */
static void open_full(uint32_t index)
{
    if (s_full_active || !s_ready || index >= s_count) return;
    s_full_active = true;
    s_full_loading = true;
    s_full_index = index;
    s_full_pixels = NULL;
    ++s_full_token;
    s_save_pending = false;
    set_hidden(s_scroll, true);
    set_hidden(s_status, true);
    set_hidden(s_note, true);
    set_hidden(s_canvas, true);
    set_hidden(s_full, false);
    lv_obj_move_foreground(s_full);
    /* 与图片查看器一样：加载期间先显示提示，确认键禁用、取消键仍可用，
     * 用户不会被困在加载画面。 */
    set_label(s_full_status, "正在加载...");
    ui_statusbar_set_visible(false);
    update_full_buttons();
    image_viewer_request_full(IMAGE_FOLDER_PICTURES, index, s_full_token);
}

/* ============================ 手势 ============================ */

static void page_pressed_cb(lv_event_t *event)
{
    (void)event;
    if (!s_visible || s_transition || s_full_active || s_press_valid) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_indev_get_point(indev, &s_press);
    s_press_scroll_y = lv_obj_get_scroll_y(s_scroll);
    s_press_valid = true;
    s_swiped = false;
}

/**
 * @brief 非边缘、干净的右滑返回“锁屏和壁纸”子页。
 *
 * 与图片查看器总览的返回手势一致：内容实际滚动超过 4px（含惯性）
 * 说明用户在浏览，不返回；横向位移要明显大于纵向，避免对角滚动误判；
 * 边缘滑动留给 ui_manager 打开模式/声光栏。
 */
static void page_released_cb(lv_event_t *event)
{
    (void)event;
    if (!s_press_valid) return;
    s_press_valid = false;
    if (!s_visible || s_transition || s_full_active) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_press.x;
    int32_t dy = p.y - s_press.y;
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;
    if (adx > 12 || ady > 12) s_swiped = true;
    int32_t scroll_delta = lv_obj_get_scroll_y(s_scroll) -
                           s_press_scroll_y;
    if (scroll_delta < 0) scroll_delta = -scroll_delta;
    if (scroll_delta <= 4 && dx > 60 && dx > ady * 2 &&
        !is_edge_start(&s_press))
        ui_wallpaper_picker_hide();
}

static void gesture_press_lost_cb(lv_event_t *event)
{
    (void)event;
    s_press_valid = false;
    s_swiped = true;
}

static void scroll_cb(lv_event_t *event)
{
    (void)event;
    update_grid_window();
}

static void tile_clicked_cb(lv_event_t *event)
{
    if (s_swiped) {
        s_swiped = false;
        return;
    }
    if (!s_visible || s_transition || s_full_active) return;
    unsigned slot = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (slot >= GRID_SLOTS || s_tile_index[slot] == UINT32_MAX) return;
    open_full(s_tile_index[slot]);
}

/* ============================ 后台回调 ============================ */

static void picker_library(image_folder_t folder, uint32_t count, bool truncated,
                           uint32_t generation, void *ctx)
{
    (void)ctx;
    /* 后台会同时发布两个文件夹；本页只关心“其他图片”。 */
    if (folder != IMAGE_FOLDER_PICTURES) return;
    bool generation_changed = s_generation != generation;
    s_count = count;
    s_truncated = truncated;
    s_generation = generation;
    s_ready = true;
    if (generation_changed && s_visible) reset_grid();
    if (s_visible) {
        s_grid_valid = false;
        update_grid_window();
        update_status();
        update_note();
    }
}

static void picker_scan_done(image_scan_result_t result, uint32_t generation,
                             void *ctx)
{
    (void)ctx;
    (void)generation;
    s_scan_pending = false;
    s_scan_result = result;
    if (result != IMAGE_SCAN_OK) {
        s_ready = false;
        s_count = 0;
    }
    update_status();
    if (s_visible) {
        if (result != IMAGE_SCAN_OK) reset_grid();
        update_grid_window();
        update_note();
    }
    ESP_LOGI(TAG, "Wallpaper scan done: result=%d pictures=%u",
             (int)result, (unsigned)s_count);
}

static void picker_thumbnail(uint32_t slot, const uint8_t *pixels, uint16_t w,
                             uint16_t h, uint32_t token, bool ok, void *ctx)
{
    (void)ctx;
    if (!s_visible || slot >= GRID_SLOTS || token != s_tile_token[slot]) return;
    if (!ok || !pixels || w != IMAGE_THUMB_W || h != IMAGE_THUMB_H ||
        s_tile_index[slot] == UINT32_MAX) return;
    set_rgb565_dsc(&s_thumb_dsc[slot], pixels, w, h);
    lv_image_cache_drop(&s_thumb_dsc[slot]);
    lv_image_set_src(s_tile_imgs[slot], &s_thumb_dsc[slot]);
    set_hidden(s_tile_imgs[slot], false);
    set_hidden(s_tile_phs[slot], true);
}

static void picker_full_image(const uint8_t *pixels, uint16_t w, uint16_t h,
                              image_folder_t folder, uint32_t index,
                              const char *name, uint32_t token, bool ok,
                              image_error_t error, void *ctx)
{
    (void)ctx;
    (void)name;
    if (!s_full_active || token != s_full_token ||
        folder != IMAGE_FOLDER_PICTURES)
        return;
    if (!ok || !pixels || w != IMAGE_FULL_PAD_W || h != IMAGE_FULL_PAD_H) {
        s_full_loading = false;
        s_full_pixels = NULL;
        const char *text = "图片读取失败";
        if (error == IMAGE_ERROR_BUSY) text = "SD 卡忙，请稍后重试";
        else if (error == IMAGE_ERROR_SIZE) text = "图片尺寸需为460x460";
        else if (error == IMAGE_ERROR_FORMAT) text = "图片格式不支持";
        set_hidden(s_canvas, true);
        set_label(s_full_status, text);
        update_full_buttons();
        return;
    }
    s_full_index = index;
    s_full_loading = false;
    s_full_pixels = pixels;
    set_label(s_full_status, NULL);
    /* 与图片查看器/锁屏一致：464x464 缓冲由 460x460 页面裁掉右侧/下侧
     * 4 像素，不做逐行拷贝。 */
    lv_canvas_set_buffer(s_canvas, (void *)pixels, w, h,
                         LV_COLOR_FORMAT_RGB565);
    lv_obj_invalidate(s_canvas);
    set_hidden(s_canvas, false);
    update_full_buttons();
}

static const image_viewer_ops_t s_picker_ops = {
    .library = picker_library,
    .scan_done = picker_scan_done,
    .thumbnail = picker_thumbnail,
    .full_image = picker_full_image,
};

/* ============================ 定时器 ============================ */

static void request_scan(void)
{
    if (s_scan_pending) return;
    s_scan_pending = true;
    s_last_scan_tick = lv_tick_get();
    s_scan_result = IMAGE_SCAN_OK;
    s_ready = false;
    s_count = 0;
    s_truncated = false;
    reset_grid();
    update_status();
    update_note();
    image_viewer_request_scan();
}

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_visible) return;

    /* 保存结果由 wallpaper worker 异步发布；这里只读状态并更新提示，
     * 不在 worker 中回调 UI，页面关闭后也不会访问已隐藏的对象。 */
    if (s_save_pending) {
        wallpaper_state_t state = wallpaper_status();
        if (state == WALLPAPER_STATE_SAVED) {
            s_save_pending = false;
            set_label(s_full_status, "壁纸已设置");
            update_full_buttons();
            wallpaper_ack_result();
        } else if (state == WALLPAPER_STATE_ERROR) {
            s_save_pending = false;
            set_label(s_full_status, "壁纸保存失败");
            update_full_buttons();
            wallpaper_ack_result();
        }
    }

    if (s_full_active) return;

    uint32_t generation = sd_card_get_generation();
    if (generation != s_sd_generation) {
        s_sd_generation = generation;
        if (s_scan_pending) {
            /* 在途扫描会被后台按新代次重新完成；这里只等待回调。 */
        } else {
            request_scan();
        }
        return;
    }
    /* worker 未创建/卡死时不让页面永远停在“正在扫描”：超时后按错误处理，
     * 后续 5 秒重试仍可恢复（与图片查看器一致）。 */
    if (s_scan_pending && lv_tick_elaps(s_last_scan_tick) >= 15000) {
        s_scan_pending = false;
        s_scan_result = IMAGE_SCAN_ERROR;
        s_ready = false;
        s_count = 0;
        update_status();
        update_note();
    }
    if (!s_ready && !s_scan_pending && sd_card_is_present() &&
        lv_tick_elaps(s_last_scan_tick) >= 5000)
        request_scan();
}

/* ============================ 页面切换 ============================ */

static void anim_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

/** @brief 把图片查看器的回调交还给后台；本页关闭/锁屏/滑出时都必须调用。 */
static void detach_backend(void)
{
    /* 先注销本页的 ops 再恢复图片查看器：取消在途全屏/缩略图，避免
     * 迟到结果被发到已经隐藏的壁纸页或混进图片查看器的槽位。 */
    image_viewer_cancel_full();
    image_viewer_clear_thumbs();
    image_viewer_set_ops(NULL, NULL);
    ui_image_viewer_backend_attach();
}

static void hide_done(lv_anim_t *anim)
{
    (void)anim;
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(s_page, UI_HOR_RES);
    s_visible = false;
    s_transition = false;
    lv_timer_pause(s_timer);
}

/** @brief 滑入完成：允许点击缩略图和右滑返回。 */
static void show_done(lv_anim_t *anim)
{
    (void)anim;
    s_transition = false;
}

/** @brief 右滑返回：先交接后台回调，再按与图片总览相同的时长滑出。 */
void ui_wallpaper_picker_hide(void)
{
    if (!s_page || !s_visible) return;
    if (s_full_active) exit_full();
    detach_backend();
    ui_statusbar_set_visible(false);
    s_transition = true;
    lv_anim_delete(s_page, anim_x);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_page);
    lv_anim_set_values(&a, lv_obj_get_x(s_page), UI_HOR_RES);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_set_completed_cb(&a, hide_done);
    lv_anim_start(&a);
}

void ui_wallpaper_picker_show(void)
{
    if (!s_page || s_visible) return;
    s_visible = true;
    s_save_pending = false;
    /* 上次保存成功/失败的结果已经由上次的提示消费；清除后再进入，
     * 不会在“确认/取消”页显示旧消息。 */
    wallpaper_ack_result();
    s_full_active = false;
    s_full_loading = false;
    s_full_pixels = NULL;
    s_full_index = UINT32_MAX;
    set_hidden(s_full, true);
    set_hidden(s_canvas, true);
    set_hidden(s_scroll, false);
    set_hidden(s_status, false);
    set_hidden(s_note, false);

    /* 先切断图片查看器的回调，再注册本页回调；两者使用的 token 空间
     * 不相交，切换期间迟到的旧结果会被本页/图片页各自丢弃。 */
    ui_image_viewer_backend_detach();
    image_viewer_set_ops(&s_picker_ops, NULL);

    /* 图片列表缓存：与图片查看器一样，只有在“尚未扫描”或“SD 卡代次
     * 变化（换卡/重新挂载）”时才重新遍历目录。反复打开本页沿用上次
     * 的 count/ready，仅让后台重新发布当前窗口的缩略图：image_viewer
     * 的 s_thumb_mem 仍持有已解码的小图，命中时不再读 SD、不再解码。
     * 若这里无条件 request_scan()，后台 do_scan() 会作废全部缩略图身份，
     * 造成每次进入都重新加载——这正是要避免的。 */
    uint32_t generation = sd_card_get_generation();
    bool need_scan = !s_ready || s_generation != generation;
    s_sd_generation = generation;
    /* 上次可能在扫描途中关闭了本页；结果已交给图片查看器，本页不能再
     * 卡在 pending 上，否则新的扫描要等 15 秒超时。 */
    s_scan_pending = false;
    if (need_scan) {
        request_scan();
        /* 与图片查看器总览一致：从第一行开始，不沿用上次的滚动位置。 */
        lv_obj_scroll_to_y(s_scroll, 0, LV_ANIM_OFF);
    } else {
        /* 快速路径：不扫目录，只重建当前窗口的槽位并请求缩略图。
         * 先作废槽位再回到顶部，滚动事件会按第 0 行重新请求。 */
        reset_grid();
        lv_obj_scroll_to_y(s_scroll, 0, LV_ANIM_OFF);
        s_grid_valid = false;
        update_grid_window();
        update_status();
        update_note();
    }

    lv_anim_delete(s_page, anim_x);
    lv_obj_set_x(s_page, UI_HOR_RES);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    /* 让本页盖住“锁屏和壁纸”子页；状态栏随后重新置顶，保持与图片
     * 查看器总览相同的可见时间/电量/图标。 */
    lv_obj_move_foreground(s_page);
    lv_obj_t *bar = ui_statusbar_get_obj();
    if (bar) lv_obj_move_foreground(bar);
    ui_statusbar_set_visible(true);

    lv_timer_resume(s_timer);

    s_transition = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_page);
    lv_anim_set_values(&a, UI_HOR_RES, 0);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_set_completed_cb(&a, show_done);
    lv_anim_start(&a);
}

void ui_wallpaper_picker_close_for_lock(void)
{
    /* 锁屏立即关闭：不做滑出动画，避免锁屏 MJPEG 与缩略图/全屏解码
     * 重叠；后台保存已经提交的 Flash 擦写会继续完成，本页不再等待。 */
    if (!s_page || !s_visible) return;
    s_visible = false;
    s_transition = false;
    lv_anim_delete(s_page, anim_x);
    ++s_full_token;
    image_viewer_cancel_full();
    s_full_active = false;
    s_full_loading = false;
    s_full_pixels = NULL;
    s_full_index = UINT32_MAX;
    s_save_pending = false;
    detach_backend();
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_set_x(s_page, UI_HOR_RES);
    lv_timer_pause(s_timer);
}

bool ui_wallpaper_picker_is_visible(void)
{
    return s_visible;
}

/* ============================ 页面创建 ============================ */

static void create_tile(lv_obj_t *parent, unsigned slot)
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
    /* 与图片总览相同：按下/释放用于识别“右滑返回”，CLICKED 用于打开；
     * 明显拖动后抑制这次点击，避免滑动手势误进全屏。 */
    lv_obj_add_event_cb(tile, page_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(tile, page_released_cb, LV_EVENT_RELEASED, NULL);
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
    s_tile_token[slot] = TOKEN_BASE + slot + 1U;
    set_hidden(tile, true);
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
    set_hidden(s_full, true);

    /* 464x464 缓冲由 JPEG 解码器按 16 对齐产生；页面 460x460 裁掉
     * 右侧/下侧 4 像素，与锁屏动画/视频播放器/图片查看器完全一致。 */
    s_canvas = lv_canvas_create(s_full);
    lv_obj_set_size(s_canvas, IMAGE_FULL_PAD_W, IMAGE_FULL_PAD_H);
    lv_obj_set_pos(s_canvas, 0, 0);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE);
    set_hidden(s_canvas, true);

    s_full_status = lv_label_create(s_full);
    lv_obj_set_width(s_full_status, UI_HOR_RES - 40);
    lv_obj_set_style_text_align(s_full_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_full_status, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_full_status, ui_font_18(), 0);
    lv_label_set_long_mode(s_full_status, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_full_status, "");
    lv_obj_align(s_full_status, LV_ALIGN_CENTER, 0, 0);
    set_hidden(s_full_status, true);

    /* 确认键：白色实底 + 黑色文字，表示主操作。按下时只把底色略微
     * 压暗（不能使用统一的深色按下色，否则黑字看不清）。 */
    s_confirm = lv_button_create(s_full);
    lv_obj_set_size(s_confirm, BTN_W, BTN_H);
    lv_obj_set_pos(s_confirm, BTN_MARGIN, BTN_Y);
    lv_obj_set_style_bg_color(s_confirm, UI_COLOR_TEXT, 0);
    lv_obj_set_style_bg_opa(s_confirm, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_confirm, 0, 0);
    lv_obj_set_style_shadow_width(s_confirm, 0, 0);
    lv_obj_set_style_radius(s_confirm, BTN_RADIUS, 0);
    lv_obj_set_style_bg_color(s_confirm, UI_COLOR_TEXT_SECONDARY, LV_STATE_PRESSED);
    lv_obj_add_event_cb(s_confirm, confirm_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_add_state(s_confirm, LV_STATE_DISABLED);
    lv_obj_t *confirm_label = lv_label_create(s_confirm);
    lv_label_set_text(confirm_label, "确认");
    lv_obj_set_style_text_color(confirm_label, UI_COLOR_ON_ACCENT, 0);
    lv_obj_set_style_text_font(confirm_label, ui_font_18(), 0);
    lv_obj_center(confirm_label);

    /* 关闭键：深灰实底 + 白色文字，与播放器控制键的深色控件一致；
     * 深色底保证在任意亮度的图片上都可辨认。只退出全屏预览，不修改
     * 壁纸（与“确认”互斥）。 */
    s_cancel = lv_button_create(s_full);
    lv_obj_set_size(s_cancel, BTN_W, BTN_H);
    lv_obj_set_pos(s_cancel, UI_HOR_RES - BTN_MARGIN - BTN_W, BTN_Y);
    lv_obj_set_style_bg_color(s_cancel, UI_COLOR_CONTROL_BG, 0);
    lv_obj_set_style_bg_opa(s_cancel, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_cancel, 0, 0);
    lv_obj_set_style_shadow_width(s_cancel, 0, 0);
    lv_obj_set_style_radius(s_cancel, BTN_RADIUS, 0);
    ui_apply_pressed_style(s_cancel);
    lv_obj_add_event_cb(s_cancel, cancel_clicked, LV_EVENT_CLICKED, NULL);
    lv_obj_t *cancel_label = lv_label_create(s_cancel);
    lv_label_set_text(cancel_label, "关闭");
    lv_obj_set_style_text_color(cancel_label, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(cancel_label, ui_font_18(), 0);
    lv_obj_center(cancel_label);
}

void ui_wallpaper_picker_create(lv_obj_t *parent)
{
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_page, UI_HOR_RES, 0);
    lv_obj_set_style_bg_color(s_page, UI_COLOR_PAGE_BG, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    /* 页面自身不冒泡：右滑返回、缩略图点击和底部按键不能触发下层
     * 设置页收起或播放器切歌。 */
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE |
                              LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);

    s_scroll = lv_obj_create(s_page);
    lv_obj_set_size(s_scroll, UI_HOR_RES, GRID_VIEW_H);
    lv_obj_set_pos(s_scroll, 0, GRID_TOP);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_scroll, 0, 0);
    lv_obj_set_style_pad_all(s_scroll, 0, 0);
    lv_obj_set_scroll_dir(s_scroll, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_scroll, LV_SCROLLBAR_MODE_AUTO);
    lv_obj_set_style_width(s_scroll, 4, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_color(s_scroll, UI_COLOR_TEXT_MUTED, LV_PART_SCROLLBAR);
    lv_obj_set_style_bg_opa(s_scroll, LV_OPA_60, LV_PART_SCROLLBAR);
    lv_obj_set_style_radius(s_scroll, 2, LV_PART_SCROLLBAR);
    lv_obj_add_flag(s_scroll, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_scroll, page_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_scroll, page_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_scroll, gesture_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(s_scroll, scroll_cb, LV_EVENT_SCROLL, NULL);

    s_content = lv_obj_create(s_scroll);
    lv_obj_set_size(s_content, UI_HOR_RES, GRID_VIEW_H);
    lv_obj_set_pos(s_content, 0, 0);
    lv_obj_set_style_bg_opa(s_content, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_content, 0, 0);
    lv_obj_set_style_pad_all(s_content, 0, 0);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_content, LV_OBJ_FLAG_CLICKABLE);

    for (unsigned i = 0; i < GRID_SLOTS; ++i) create_tile(s_content, i);

    /* 扫描/错误提示固定在页面中央偏下，截断提示固定在底部；两者都不
     * 随缩略图滚动，与图片查看器总览的状态文字位置一致。 */
    s_status = lv_label_create(s_page);
    lv_obj_set_width(s_status, UI_HOR_RES - 40);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_status, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_status, ui_font_18(), 0);
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_status, "");
    lv_obj_align(s_status, LV_ALIGN_CENTER, 0, GRID_TOP / 2);
    set_hidden(s_status, true);

    s_note = lv_label_create(s_page);
    lv_obj_set_width(s_note, UI_HOR_RES - 24);
    lv_obj_set_style_text_align(s_note, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_note, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_note, ui_font_18(), 0);
    lv_label_set_long_mode(s_note, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_note, "");
    lv_obj_align(s_note, LV_ALIGN_BOTTOM_MID, 0, -10);
    set_hidden(s_note, true);

    create_full(s_page);

    for (unsigned i = 0; i < GRID_SLOTS; ++i) {
        s_tile_index[i] = UINT32_MAX;
        s_tile_token[i] = TOKEN_BASE + i + 1U;
    }
    s_window_first = -1;
    s_grid_valid = false;
    s_grid_content_h = -1;
    s_generation = 0;
    s_sd_generation = sd_card_get_generation();
    s_full_index = UINT32_MAX;
    s_timer = lv_timer_create(ui_timer_cb, 100, NULL);
    if (!s_timer) ESP_LOGW(TAG, "Wallpaper timer unavailable");
    else lv_timer_pause(s_timer);
}
