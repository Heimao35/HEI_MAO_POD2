/**
 * @file ui_ebook.c
 * @brief 电子书模式 UI：书库入口、横向书架、全屏阅读页。
 *
 * 页面层级（s_page 内）：
 *   s_home     一张 440x196 卡片：左侧第一本书封面，右侧“书库”
 *   s_library  横向书架：封面 + 18px 书名，LVGL 横向滚动并吸附
 *   s_reader   全屏黑底白字阅读页；左右滑动翻页，点击唤出控制台
 *
 * 约束：
 * - 本文件不访问 SD/JPEG/TXT；所有扫描、封面解码、打开和取页都来自
 *   ebook worker，回调在 LVGL 锁内执行，这里只更新控件。
 * - 书名和正文固定 18 号全字库；首页/书库标题“书库”用 22 号，状态
 *   提示用 16 号，页码用内置 Montserrat 14（只有数字/斜杠）。
 * - 阅读页把控制台和状态栏绑定在一起：控制台显示时状态栏也显示；控制
 *   台淡出完成后隐藏状态栏。返回键/书名放在状态栏下方，避免重叠。
 * - 所有新布局参数集中在文件头，便于和图片查看器/视频播放器保持一致的
 *   边距、圆角和动画时长；修改正文宽高/行距必须同步 ebook.h。
 */
#include "ui_ebook.h"

#include "ebook.h"
#include "player_settings.h"
#include "sd_card.h"
#include "ui_fonts.h"
#include "ui_lockscreen.h"
#include "ui_manager.h"
#include "ui_statusbar.h"

#include "esp_log.h"
#include "src/misc/cache/instance/lv_image_cache.h"
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *TAG = "ui_ebook";

/* ============================ 布局参数 ============================ */
/* 首页卡片沿用图片查看器首页：10px 外边距、196px 高、18px 圆角。 */
#define HOME_CARD_X          10
#define HOME_CARD_Y          (UI_STATUSBAR_HEIGHT + 10)
#define HOME_CARD_W          (UI_HOR_RES - 2 * HOME_CARD_X)
#define HOME_CARD_H          196
#define HOME_CARD_RADIUS     18
#define HOME_PREVIEW_SIZE    148
#define HOME_PREVIEW_X       16
#define HOME_PREVIEW_Y       ((HOME_CARD_H - HOME_PREVIEW_SIZE) / 2)
#define HOME_PREVIEW_RADIUS  12
#define HOME_TITLE_X         (HOME_PREVIEW_X + HOME_PREVIEW_SIZE + 16)
#define HOME_TITLE_W         (HOME_CARD_W - HOME_TITLE_X - 16)

/* 书库：状态栏下留出 44px 返回键行，封面 140x200，三列加间距正好铺满
 * 460px；滚动容器左右各 16px 内边距，LVGL 的 START 吸附会把每个封面
 * 对齐到 16px 起始线。 */
#define LIB_HEADER_Y         42
#define LIB_SCROLL_Y         104
#define LIB_ITEM_W           140
#define LIB_ITEM_H           228
#define LIB_ITEM_GAP         16
#define LIB_ITEM_STRIDE      (LIB_ITEM_W + LIB_ITEM_GAP)
#define LIB_TITLE_Y          (EBOOK_COVER_H + 6)
#define LIB_TITLE_H          22
#define LIB_PAD              16

/* 阅读页：正文区域必须与 ebook.h 的分页参数一致。 */
#define READER_TEXT_X        20
#define READER_TEXT_Y        24
#define READER_SLIDE_MS      220
#define READER_CONTROLS_FADE_MS 220
#define READER_CONTROLS_HIDE_MS 3000
#define READER_TOP_H         110
#define READER_BACK_Y        46
#define READER_OPEN_TIMEOUT_MS  15000
#define READER_PAGE_TIMEOUT_MS  2000

/* ============================ 页面对象 ============================ */
static lv_obj_t *s_page;
static lv_obj_t *s_home;
static lv_obj_t *s_home_prev;
static lv_obj_t *s_home_img;
static lv_obj_t *s_home_ph;
static lv_obj_t *s_home_status;
static lv_image_dsc_t s_home_dsc;

static lv_obj_t *s_library;
static lv_obj_t *s_lib_scroll;
static lv_obj_t *s_lib_status;
static lv_obj_t *s_lib_count;
static lv_obj_t *s_lib_back;

static lv_obj_t *s_items[EBOOK_MAX_BOOKS];
static lv_obj_t *s_item_imgs[EBOOK_MAX_BOOKS];
static lv_obj_t *s_item_phs[EBOOK_MAX_BOOKS];
static lv_obj_t *s_item_titles[EBOOK_MAX_BOOKS];
static uint8_t s_item_state[EBOOK_MAX_BOOKS]; /* 0无 1加载中 2已显示 3失败 */

static lv_obj_t *s_reader;
static lv_obj_t *s_rpages[2];
static lv_obj_t *s_reader_status;
static lv_obj_t *s_reader_page_no;
static lv_obj_t *s_reader_controls;
static lv_obj_t *s_reader_title;

static lv_timer_t *s_timer;

/* ============================ 运行状态 ============================ */
static bool s_ready;
static bool s_scan_pending;
static bool s_truncated;
static ebook_scan_result_t s_scan_result = EBOOK_SCAN_OK;
static uint32_t s_count;
static uint32_t s_generation;
static uint32_t s_sd_generation;
static uint32_t s_last_scan_tick;
static bool s_items_dirty;

static bool s_library_visible;
static bool s_lib_transition;
static bool s_lib_press_valid;
static bool s_lib_swiped;
static lv_point_t s_lib_press;

static uint32_t s_cover_owner[EBOOK_COVER_SLOTS];
static uint32_t s_cover_token[EBOOK_COVER_SLOTS];
static lv_image_dsc_t s_cover_dsc[EBOOK_COVER_SLOTS];
static uint32_t s_home_token;
static uint32_t s_home_generation;

static bool s_reader_active;
static bool s_reader_loading;
static bool s_reader_sliding;
static bool s_reader_page_pending;
static uint32_t s_reader_book;
static uint32_t s_reader_pages;
static uint32_t s_reader_page;
static uint32_t s_reader_target;
static uint64_t s_reader_progress_key; /**< 当前书的 NVS 阅读进度 key */
static uint32_t s_reader_open_token;
static uint32_t s_reader_page_token;
static uint32_t s_reader_open_tick;
static uint32_t s_reader_page_tick;
static int s_reader_current;
static bool s_reader_press_valid;
static lv_point_t s_reader_press;
static bool s_reader_controls_visible;
static int32_t s_reader_controls_opa;
static uint32_t s_reader_last_activity;

static void request_scan(void);
static void update_status(void);
static void update_cover_window(void);
static void open_reader(uint32_t book);
static void close_reader(bool show_library);
static void reader_controls_show(void);
static void reader_controls_hide(void);
static void reader_controls_hide_now(void);

/* ============================ 小工具 ============================ */

static void set_hidden(lv_obj_t *obj, bool hidden)
{
    if (!obj) return;
    if (hidden) lv_obj_add_flag(obj, LV_OBJ_FLAG_HIDDEN);
    else lv_obj_remove_flag(obj, LV_OBJ_FLAG_HIDDEN);
}

static bool edge_start(const lv_point_t *p)
{
    return p->x < UI_EDGE_THRESHOLD ||
           p->x > UI_HOR_RES - UI_EDGE_THRESHOLD;
}

static void set_rgb565_dsc(lv_image_dsc_t *dsc, const uint8_t *pixels,
                           uint16_t w, uint16_t h)
{
    dsc->header.magic = LV_IMAGE_HEADER_MAGIC;
    dsc->header.cf = LV_COLOR_FORMAT_RGB565;
    dsc->header.flags = 0;
    dsc->header.w = w;
    dsc->header.h = h;
    dsc->header.stride = (uint32_t)w * 2U;
    dsc->data_size = (uint32_t)w * h * 2U;
    dsc->data = pixels;
    lv_image_cache_drop(dsc);
}

static lv_obj_t *create_back_button(lv_obj_t *parent, int32_t y,
                                    lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_size(btn, 44, 44);
    lv_obj_set_pos(btn, 12, y);
    lv_obj_set_style_bg_opa(btn, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(btn, 0, 0);
    lv_obj_set_style_shadow_width(btn, 0, 0);
    lv_obj_set_ext_click_area(btn, 6);
    lv_obj_t *arrow = lv_label_create(btn);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(arrow, UI_COLOR_TEXT, 0);
    lv_obj_center(arrow);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    return btn;
}

/* ============================ 首页卡片 ============================ */

static void home_pressed_cb(lv_event_t *event)
{
    (void)event;
    if (s_lib_press_valid) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_indev_get_point(indev, &s_lib_press);
    s_lib_press_valid = true;
    s_lib_swiped = false;
}

static void home_released_cb(lv_event_t *event)
{
    (void)event;
    if (!s_lib_press_valid) return;
    s_lib_press_valid = false;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_lib_press.x;
    int32_t dy = p.y - s_lib_press.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > 12 || dy > 12) s_lib_swiped = true;
}

static void home_press_lost_cb(lv_event_t *event)
{
    (void)event;
    s_lib_press_valid = false;
    s_lib_swiped = true;
}

static void library_anim_x_cb(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void library_show_done(lv_anim_t *a)
{
    (void)a;
    s_lib_transition = false;
    s_library_visible = true;
    /* 书架不透明：滑入完成后隐藏首页，避免滚动时重复绘制被遮住的卡片。 */
    set_hidden(s_home, true);
    update_cover_window();
}

static void library_hide_done(lv_anim_t *a)
{
    (void)a;
    s_lib_transition = false;
    s_library_visible = false;
    set_hidden(s_library, true);
    lv_obj_set_x(s_library, UI_HOR_RES);
    set_hidden(s_home, false);
}

static void open_library(void)
{
    if (s_library_visible || s_lib_transition || s_reader_active) return;
    s_lib_transition = true;
    set_hidden(s_home, false);
    set_hidden(s_library, false);
    lv_obj_move_foreground(s_library);
    lv_obj_set_x(s_library, UI_HOR_RES);
    update_cover_window();

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_library);
    lv_anim_set_values(&a, UI_HOR_RES, 0);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, library_anim_x_cb);
    lv_anim_set_completed_cb(&a, library_show_done);
    lv_anim_start(&a);
}

static void close_library(void)
{
    if (!s_library_visible || s_lib_transition || s_reader_active) return;
    s_lib_transition = true;
    /* 书库是不透明页面：必须先恢复首页，再让书库向右滑出，滑动过程中
     * 首页始终在下面可见。否则首页要等 completed_cb 才出现，形成“动画
     * 播完才闪出主页面”的错误观感（与图片查看器 close_overview 一致）。 */
    set_hidden(s_home, false);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_library);
    lv_anim_set_values(&a, 0, UI_HOR_RES);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, library_anim_x_cb);
    lv_anim_set_completed_cb(&a, library_hide_done);
    lv_anim_start(&a);
}

static void home_clicked_cb(lv_event_t *event)
{
    (void)event;
    if (s_lib_swiped) {
        s_lib_swiped = false;
        return;
    }
    if (edge_start(&s_lib_press)) return;
    if (s_reader_active || s_lib_transition) return;
    open_library();
}

/* ============================ 书库 ============================ */

static void lib_back_cb(lv_event_t *event)
{
    (void)event;
    close_library();
}

static void lib_scroll_cb(lv_event_t *event)
{
    (void)event;
    update_cover_window();
}

static void item_pressed_cb(lv_event_t *event)
{
    (void)event;
    if (s_lib_press_valid) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_indev_get_point(indev, &s_lib_press);
    s_lib_press_valid = true;
    s_lib_swiped = false;
}

static void item_released_cb(lv_event_t *event)
{
    (void)event;
    if (!s_lib_press_valid) return;
    s_lib_press_valid = false;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_lib_press.x;
    int32_t dy = p.y - s_lib_press.y;
    if (dx < 0) dx = -dx;
    if (dy < 0) dy = -dy;
    if (dx > 12 || dy > 12) s_lib_swiped = true;
}

static void item_clicked_cb(lv_event_t *event)
{
    if (s_lib_swiped) {
        s_lib_swiped = false;
        return;
    }
    /* 边缘滑动是 ui_manager 打开模式/声光栏的手势，不能在书架里误开书。 */
    if (edge_start(&s_lib_press)) return;
    uint32_t book = (uint32_t)(uintptr_t)lv_event_get_user_data(event);
    if (book >= s_count) return;
    open_reader(book);
}

static void item_press_lost_cb(lv_event_t *event)
{
    (void)event;
    s_lib_press_valid = false;
    s_lib_swiped = true;
}

static void create_item(uint32_t index, const char *title)
{
    lv_obj_t *item = lv_obj_create(s_lib_scroll);
    lv_obj_set_size(item, LIB_ITEM_W, LIB_ITEM_H);
    lv_obj_set_pos(item, (int32_t)index * LIB_ITEM_STRIDE, 0);
    lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(item, 0, 0);
    lv_obj_set_style_pad_all(item, 0, 0);
    lv_obj_set_style_radius(item, 10, 0);
    lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);
    ui_apply_pressed_style(item);
    lv_obj_add_event_cb(item, item_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(item, item_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(item, item_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(item, item_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);
    s_items[index] = item;

    lv_obj_t *cover = lv_obj_create(item);
    lv_obj_set_size(cover, EBOOK_COVER_W, EBOOK_COVER_H);
    lv_obj_set_pos(cover, 0, 0);
    lv_obj_set_style_bg_color(cover, UI_COLOR_CONTROL_BG, 0);
    lv_obj_set_style_bg_opa(cover, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(cover, 0, 0);
    lv_obj_set_style_radius(cover, 10, 0);
    lv_obj_set_style_clip_corner(cover, true, 0);
    lv_obj_set_style_pad_all(cover, 0, 0);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(cover, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *img = lv_image_create(cover);
    lv_obj_set_size(img, EBOOK_COVER_W, EBOOK_COVER_H);
    lv_obj_set_pos(img, 0, 0);
    lv_obj_add_flag(img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(img, LV_OBJ_FLAG_CLICKABLE);
    s_item_imgs[index] = img;

    lv_obj_t *ph = lv_label_create(cover);
    lv_label_set_text(ph, LV_SYMBOL_FILE);
    lv_obj_set_style_text_font(ph, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(ph, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_center(ph);
    lv_obj_remove_flag(ph, LV_OBJ_FLAG_CLICKABLE);
    s_item_phs[index] = ph;

    /* 书名固定 18 号全字库；长书名单行省略，不换行改变书架高度。 */
    lv_obj_t *label = lv_label_create(item);
    lv_obj_set_size(label, LIB_ITEM_W, LIB_TITLE_H);
    lv_obj_set_pos(label, 0, LIB_TITLE_Y);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(label, ui_font_18(), 0);
    lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    lv_label_set_text(label, title);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    s_item_titles[index] = label;

    s_item_state[index] = 0;
}

static void rebuild_items(void)
{
    s_items_dirty = false;
    for (unsigned i = 0; i < EBOOK_MAX_BOOKS; ++i) {
        if (s_items[i]) {
            lv_obj_delete(s_items[i]);
            s_items[i] = NULL;
        }
        s_item_imgs[i] = NULL;
        s_item_phs[i] = NULL;
        s_item_titles[i] = NULL;
        s_item_state[i] = 0;
    }
    for (unsigned i = 0; i < EBOOK_COVER_SLOTS; ++i) {
        s_cover_owner[i] = UINT32_MAX;
        ++s_cover_token[i];
    }
    for (uint32_t i = 0; i < s_count; ++i) {
        char title[EBOOK_TITLE_MAX];
        if (!ebook_get_title(i, title, sizeof(title))) title[0] = '\0';
        create_item(i, title);
    }
    lv_obj_scroll_to_x(s_lib_scroll, 0, LV_ANIM_OFF);
    update_cover_window();
}

static void update_cover_window(void)
{
    if (!s_library_visible || s_count == 0) return;
    int32_t scroll_x = lv_obj_get_scroll_x(s_lib_scroll);
    if (scroll_x < 0) scroll_x = 0;
    int32_t first = scroll_x / LIB_ITEM_STRIDE;
    int32_t start = first > 0 ? first - 1 : 0;
    int32_t end = first + 5; /* 一屏约 3 本，前后各多预取一本 */
    if (end > (int32_t)s_count) end = (int32_t)s_count;
    for (int32_t i = start; i < end; ++i) {
        uint32_t book = (uint32_t)i;
        uint32_t slot = book % EBOOK_COVER_SLOTS;
        if (s_cover_owner[slot] == book) {
            if (s_item_state[book] == 0) {
                s_item_state[book] = 1;
                ++s_cover_token[slot];
                ebook_request_cover(book, slot, s_cover_token[slot]);
            }
            continue;
        }
        /* 槽位被另一本书占用：先让旧书解除对这个描述符的引用。 */
        uint32_t old = s_cover_owner[slot];
        if (old != UINT32_MAX && old < EBOOK_MAX_BOOKS) {
            if (s_item_imgs[old]) {
                lv_image_set_src(s_item_imgs[old], NULL);
                lv_obj_add_flag(s_item_imgs[old], LV_OBJ_FLAG_HIDDEN);
            }
            if (s_item_phs[old])
                lv_obj_remove_flag(s_item_phs[old], LV_OBJ_FLAG_HIDDEN);
            s_item_state[old] = 0;
        }
        s_cover_owner[slot] = book;
        s_item_state[book] = 1;
        ++s_cover_token[slot];
        ebook_request_cover(book, slot, s_cover_token[slot]);
    }
}

/* ============================ 阅读页 ============================ */

static void reader_controls_fade_exec(void *obj, int32_t opa)
{
    s_reader_controls_opa = opa;
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)opa, 0);
}

static void reader_controls_fade_done(lv_anim_t *a)
{
    (void)a;
    if (!s_reader_active) return;
    if (!s_reader_controls_visible) {
        lv_obj_add_flag(s_reader_controls, LV_OBJ_FLAG_HIDDEN);
        /* 控制台完全淡出后才隐藏状态栏，两者作为一组消失。 */
        ui_statusbar_set_visible(false);
    }
}

static void reader_controls_show(void)
{
    if (!s_reader_active || !s_reader_controls) return;
    lv_anim_delete(s_reader_controls, reader_controls_fade_exec);
    lv_obj_remove_flag(s_reader_controls, LV_OBJ_FLAG_HIDDEN);
    ui_statusbar_set_visible(true);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_reader_controls);
    lv_anim_set_values(&a, s_reader_controls_opa, LV_OPA_COVER);
    lv_anim_set_duration(&a, READER_CONTROLS_FADE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, reader_controls_fade_exec);
    lv_anim_set_completed_cb(&a, reader_controls_fade_done);
    lv_anim_start(&a);
    s_reader_controls_visible = true;
    s_reader_last_activity = lv_tick_get();
}

static void reader_controls_hide(void)
{
    if (!s_reader_controls || !s_reader_controls_visible) return;
    s_reader_controls_visible = false;
    lv_anim_delete(s_reader_controls, reader_controls_fade_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_reader_controls);
    lv_anim_set_values(&a, s_reader_controls_opa, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, READER_CONTROLS_FADE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, reader_controls_fade_exec);
    lv_anim_set_completed_cb(&a, reader_controls_fade_done);
    lv_anim_start(&a);
}

static void reader_controls_hide_now(void)
{
    if (!s_reader_controls) return;
    lv_anim_delete(s_reader_controls, reader_controls_fade_exec);
    s_reader_controls_visible = false;
    s_reader_controls_opa = LV_OPA_TRANSP;
    lv_obj_set_style_opa(s_reader_controls, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_reader_controls, LV_OBJ_FLAG_HIDDEN);
}

static void reader_update_page_no(void)
{
    if (!s_reader_page_no) return;
    if (!s_reader_pages) {
        set_hidden(s_reader_page_no, true);
        return;
    }
    char text[32];
    snprintf(text, sizeof(text), "%u / %u",
             (unsigned)(s_reader_page + 1U), (unsigned)s_reader_pages);
    lv_label_set_text(s_reader_page_no, text);
    set_hidden(s_reader_page_no, false);
}

static void reader_set_status(const char *text)
{
    if (!s_reader_status) return;
    if (!text || !text[0]) {
        set_hidden(s_reader_status, true);
        return;
    }
    lv_label_set_text(s_reader_status, text);
    set_hidden(s_reader_status, false);
}

static void reader_slide_exec(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

static void reader_slide_done(lv_anim_t *a)
{
    (void)a;
    int old = s_reader_current;
    int now = 1 - old;
    set_hidden(s_rpages[old], true);
    lv_obj_set_x(s_rpages[old], READER_TEXT_X);
    lv_obj_set_x(s_rpages[now], READER_TEXT_X);
    s_reader_current = now;
    s_reader_page = s_reader_target;
    s_reader_sliding = false;
    reader_update_page_no();
    /* 翻页完成后更新 RAM 进度；3 秒合并保存由电源 worker 负责。 */
    if (s_reader_progress_key)
        player_settings_set_ebook_progress(s_reader_progress_key,
                                           s_reader_page, s_reader_pages);
}

/**
 * @brief 新页滑入、旧页滑出（与图片全屏/视频切换同一条 ease-out 曲线）。
 *
 * dir=+1 表示下一页：新页从右侧 460px 滑入；dir=-1 表示上一页。
 */
static void reader_start_slide(const char *text, int32_t dir)
{
    int old = s_reader_current;
    int now = 1 - old;
    s_reader_sliding = true;
    lv_label_set_text(s_rpages[now], text);
    lv_obj_set_x(s_rpages[now], READER_TEXT_X + dir * UI_HOR_RES);
    set_hidden(s_rpages[now], false);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_rpages[old]);
    lv_anim_set_values(&a, READER_TEXT_X, READER_TEXT_X - dir * UI_HOR_RES);
    lv_anim_set_duration(&a, READER_SLIDE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, reader_slide_exec);
    lv_anim_start(&a);

    lv_anim_init(&a);
    lv_anim_set_var(&a, s_rpages[now]);
    lv_anim_set_values(&a, READER_TEXT_X + dir * UI_HOR_RES, READER_TEXT_X);
    lv_anim_set_duration(&a, READER_SLIDE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, reader_slide_exec);
    lv_anim_set_completed_cb(&a, reader_slide_done);
    lv_anim_start(&a);
}

static void reader_back_cb(lv_event_t *event)
{
    (void)event;
    close_reader(true);
}

static void reader_request_page(uint32_t page)
{
    s_reader_target = page;
    s_reader_page_pending = true;
    s_reader_page_tick = lv_tick_get();
    ++s_reader_page_token;
    ebook_request_page(page, s_reader_page_token);
}

static void reader_toggle_controls(void)
{
    if (s_reader_controls_visible) reader_controls_hide();
    else reader_controls_show();
}

static void reader_pressed_cb(lv_event_t *event)
{
    (void)event;
    if (!s_reader_active || s_reader_sliding || s_reader_press_valid) return;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_indev_get_point(indev, &s_reader_press);
    s_reader_press_valid = true;
}

static void reader_released_cb(lv_event_t *event)
{
    (void)event;
    if (!s_reader_active || s_reader_sliding || !s_reader_press_valid) return;
    s_reader_press_valid = false;
    lv_indev_t *indev = lv_indev_active();
    if (!indev) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    int32_t dx = p.x - s_reader_press.x;
    int32_t dy = p.y - s_reader_press.y;
    int32_t adx = dx < 0 ? -dx : dx;
    int32_t ady = dy < 0 ? -dy : dy;

    if (adx > 50 && adx > ady && !edge_start(&s_reader_press)) {
        if (s_reader_loading || s_reader_page_pending || !s_reader_pages)
            return;
        uint32_t target = s_reader_page;
        if (dx < 0) {
            if (target + 1U >= s_reader_pages) return;
            ++target;
        } else {
            if (target == 0) return;
            --target;
        }
        /* 翻页时先收起控制台和状态栏，画面保持干净；新页到达后再滑入。 */
        reader_controls_hide_now();
        ui_statusbar_set_visible(false);
        reader_request_page(target);
        return;
    }
    if (adx < 12 && ady < 12) {
        if (s_reader_loading && !s_reader_pages) {
            reader_controls_show();
            return;
        }
        reader_toggle_controls();
    }
}

static void reader_press_lost_cb(lv_event_t *event)
{
    (void)event;
    s_reader_press_valid = false;
}

static void close_reader(bool show_library)
{
    if (!s_reader_active) return;
    s_reader_active = false;
    s_reader_loading = false;
    s_reader_sliding = false;
    s_reader_page_pending = false;
    ++s_reader_open_token;
    ++s_reader_page_token;
    s_reader_progress_key = 0;
    ebook_close();
    lv_anim_delete(s_rpages[0], reader_slide_exec);
    lv_anim_delete(s_rpages[1], reader_slide_exec);
    set_hidden(s_rpages[0], true);
    set_hidden(s_rpages[1], true);
    lv_obj_set_x(s_rpages[0], READER_TEXT_X);
    lv_obj_set_x(s_rpages[1], READER_TEXT_X);
    reader_set_status(NULL);
    set_hidden(s_reader_page_no, true);
    reader_controls_hide_now();
    set_hidden(s_reader, true);
    if (show_library && s_library_visible) set_hidden(s_library, false);
    else set_hidden(s_home, false);
    ui_statusbar_set_visible(true);
}

static void open_reader(uint32_t book)
{
    if (s_reader_active || !s_ready || book >= s_count) return;
    s_reader_active = true;
    s_reader_book = book;
    s_reader_pages = 0;
    s_reader_page = 0;
    s_reader_target = 0;
    s_reader_current = 0;
    s_reader_loading = true;
    s_reader_sliding = false;
    s_reader_page_pending = false;
    s_reader_press_valid = false;
    s_reader_open_tick = lv_tick_get();
    s_reader_progress_key = 0;
    ++s_reader_open_token;
    ++s_reader_page_token;
    set_hidden(s_rpages[0], true);
    set_hidden(s_rpages[1], true);
    lv_obj_set_x(s_rpages[0], READER_TEXT_X);
    lv_obj_set_x(s_rpages[1], READER_TEXT_X);
    reader_controls_hide_now();
    set_hidden(s_reader_page_no, true);
    reader_set_status("正在打开...");
    ui_statusbar_set_visible(false);

    char title[EBOOK_TITLE_MAX];
    if (!ebook_get_title(book, title, sizeof(title))) title[0] = '\0';
    lv_label_set_text(s_reader_title, title);

    set_hidden(s_reader, false);
    lv_obj_move_foreground(s_reader);
    ebook_open(book, s_reader_open_token);
}

/* ============================ 后台回调 ============================ */

static void library_cb(uint32_t count, bool truncated, uint32_t generation,
                       void *ctx)
{
    (void)ctx;
    if (count > EBOOK_MAX_BOOKS) count = EBOOK_MAX_BOOKS;
    s_count = count;
    s_truncated = truncated;
    s_generation = generation;
    s_ready = true;
    s_items_dirty = true;
    /* 新扫描的第一本书可能已换：先恢复占位图，避免继续显示旧封面。 */
    set_hidden(s_home_img, true);
    set_hidden(s_home_ph, false);
    s_home_token = 0;
    s_home_generation = 0;
    update_status();
    if (count > 0 && ebook_has_cover(0)) {
        ++s_home_token;
        s_home_generation = generation;
        ebook_request_home_cover(0, generation, s_home_token);
    }
}

static void scan_done_cb(ebook_scan_result_t result, uint32_t generation,
                         void *ctx)
{
    (void)generation;
    (void)ctx;
    s_scan_pending = false;
    s_scan_result = result;
    /* 失败时允许 5 秒后重试；成功（包括空书库）才保持 ready。 */
    if (result != EBOOK_SCAN_OK) s_ready = false;
    update_status();
}

static void home_cover_cb(uint32_t book, const uint8_t *pixels, uint16_t w,
                          uint16_t h, uint32_t generation, bool ok, void *ctx)
{
    (void)ctx;
    if (book != 0 || generation != s_generation ||
        generation != s_home_generation ||
        !s_home_token) return;
    if (!ok || !pixels || w != EBOOK_HOME_W || h != EBOOK_HOME_H) {
        set_hidden(s_home_img, true);
        set_hidden(s_home_ph, false);
        return;
    }
    set_rgb565_dsc(&s_home_dsc, pixels, w, h);
    lv_image_set_src(s_home_img, &s_home_dsc);
    set_hidden(s_home_img, false);
    set_hidden(s_home_ph, true);
}

static void cover_cb(uint32_t slot, uint32_t book, const uint8_t *pixels,
                     uint16_t w, uint16_t h, uint32_t token, bool ok,
                     void *ctx)
{
    (void)ctx;
    if (slot >= EBOOK_COVER_SLOTS || book >= s_count ||
        s_cover_owner[slot] != book || token != s_cover_token[slot])
        return;
    if (!ok || !pixels || w != EBOOK_COVER_W || h != EBOOK_COVER_H) {
        if (book < EBOOK_MAX_BOOKS) s_item_state[book] = 3;
        return;
    }
    set_rgb565_dsc(&s_cover_dsc[slot], pixels, w, h);
    lv_obj_t *img = s_item_imgs[book];
    lv_obj_t *ph = s_item_phs[book];
    if (img) {
        lv_image_set_src(img, &s_cover_dsc[slot]);
        lv_obj_remove_flag(img, LV_OBJ_FLAG_HIDDEN);
    }
    if (ph) lv_obj_add_flag(ph, LV_OBJ_FLAG_HIDDEN);
    if (book < EBOOK_MAX_BOOKS) s_item_state[book] = 2;
}

static const char *open_error_text(ebook_open_result_t result)
{
    switch (result) {
    case EBOOK_OPEN_UTF8: return "仅支持 UTF-8 TXT";
    case EBOOK_OPEN_TOO_LARGE: return "文件过大";
    case EBOOK_OPEN_NO_CARD: return "未找到 SD 卡";
    case EBOOK_OPEN_BUSY: return "SD 卡忙，请重试";
    case EBOOK_OPEN_TOO_MANY_PAGES: return "文件过大";
    default: return "打开失败";
    }
}

static void opened_cb(uint32_t book, uint32_t pages, uint64_t progress_key,
                      uint32_t token, ebook_open_result_t result, void *ctx)
{
    (void)book;
    (void)ctx;
    if (!s_reader_active || token != s_reader_open_token) return;
    if (result != EBOOK_OPEN_OK) {
        s_reader_loading = false;
        reader_set_status(open_error_text(result));
        reader_controls_show();
        return;
    }
    s_reader_pages = pages;
    s_reader_loading = true;
    s_reader_progress_key = progress_key;
    /* 同一本书且 TXT/排版未变化时从上次页码继续；越界或 key 变化则从
     * 第一页开始。进度表在 player_settings 的 RAM 快照里，读它不碰
     * Flash；真正写 NVS 仍由电源 worker 的合并保存完成。 */
    uint32_t start = 0, saved_pages = 0, saved_page = 0;
    if (progress_key && pages &&
        player_settings_get_ebook_progress(progress_key, &saved_page,
                                           &saved_pages) &&
        saved_page < pages)
        start = saved_page;
    s_reader_page = start;
    reader_set_status("正在打开...");
    reader_request_page(start);
}

static void page_cb(uint32_t page, const char *text, uint32_t token, bool ok,
                    void *ctx)
{
    (void)ctx;
    if (!s_reader_active || token != s_reader_page_token) return;
    if (!ok || !text) {
        s_reader_page_pending = false;
        s_reader_loading = false;
        reader_set_status("页面读取失败");
        reader_controls_show();
        return;
    }
    if (s_reader_loading) {
        s_reader_loading = false;
        s_reader_page_pending = false;
        lv_label_set_text(s_rpages[s_reader_current], text);
        lv_obj_set_x(s_rpages[s_reader_current], READER_TEXT_X);
        set_hidden(s_rpages[s_reader_current], false);
        reader_set_status(NULL);
        s_reader_page = page;
        reader_update_page_no();
        if (s_reader_progress_key)
            player_settings_set_ebook_progress(s_reader_progress_key, page,
                                               s_reader_pages);
        reader_controls_show();
        return;
    }
    s_reader_page_pending = false;
    if (s_reader_sliding || page != s_reader_target) return;
    int32_t dir = s_reader_target >= s_reader_page ? 1 : -1;
    reader_start_slide(text, dir);
}

static const ebook_ops_t s_ops = {
    .library = library_cb,
    .scan_done = scan_done_cb,
    .home_cover = home_cover_cb,
    .cover = cover_cb,
    .opened = opened_cb,
    .page = page_cb,
};

/* ============================ 状态/扫描 ============================ */

static void update_status(void)
{
    const char *text = NULL;
    if (s_scan_pending) {
        text = "正在扫描书籍...";
    } else if (!s_ready) {
        if (s_scan_result == EBOOK_SCAN_NO_CARD) text = "未找到 SD 卡";
        else if (s_scan_result == EBOOK_SCAN_ERROR) text = "书籍扫描失败";
        else text = "正在扫描书籍...";
    } else if (s_count == 0) {
        text = "暂无书籍";
    }

    if (text) {
        lv_label_set_text(s_home_status, text);
        set_hidden(s_home_status, false);
    } else {
        set_hidden(s_home_status, true);
    }

    if (s_count > 0) {
        set_hidden(s_lib_status, true);
        char count_text[48];
        snprintf(count_text, sizeof(count_text), "共 %u%s 本",
                 (unsigned)s_count, s_truncated ? "+" : "");
        lv_label_set_text(s_lib_count, count_text);
        set_hidden(s_lib_count, false);
    } else {
        if (s_ready && s_scan_result == EBOOK_SCAN_OK)
            lv_label_set_text(s_lib_status,
                              "暂无书籍\n请将 TXT 与封面放入 BOOKS/书名/");
        else
            lv_label_set_text(s_lib_status, text ? text : "正在扫描书籍...");
        set_hidden(s_lib_status, false);
        set_hidden(s_lib_count, true);
    }
}

static void request_scan(void)
{
    if (s_scan_pending) return;
    s_scan_pending = true;
    s_ready = false;
    s_scan_result = EBOOK_SCAN_OK;
    s_last_scan_tick = lv_tick_get();
    update_status();
    ebook_request_scan();
}

static bool ebook_page_active(void)
{
    return ui_manager_get_current_mode() == UI_MODE_EBOOK &&
           !ui_lockscreen_is_visible() &&
           !lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

/* ============================ 定时器 ============================ */

static void ui_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    if (s_reader_active) {
        if (s_reader_controls_visible && !s_reader_loading &&
            !s_reader_sliding &&
            lv_tick_elaps(s_reader_last_activity) >= READER_CONTROLS_HIDE_MS)
            reader_controls_hide();
        /* 打开/取页都由 worker 完成；超时后恢复控制台，避免卡在加载画面。 */
        if (s_reader_loading && !s_reader_pages &&
            lv_tick_elaps(s_reader_open_tick) >= READER_OPEN_TIMEOUT_MS) {
            s_reader_loading = false;
            reader_set_status("打开失败");
            reader_controls_show();
        }
        if (s_reader_page_pending &&
            lv_tick_elaps(s_reader_page_tick) >= READER_PAGE_TIMEOUT_MS) {
            s_reader_page_pending = false;
            reader_set_status("页面读取失败");
            reader_controls_show();
        }
    }

    if (!ebook_page_active()) return;

    uint32_t generation = sd_card_get_generation();
    if (generation != s_sd_generation) {
        s_sd_generation = generation;
        if (s_reader_active) close_reader(true);
        request_scan();
        return;
    }
    if (s_scan_pending &&
        lv_tick_elaps(s_last_scan_tick) >= 15000) {
        s_scan_pending = false;
        s_scan_result = EBOOK_SCAN_ERROR;
        update_status();
    }
    if (!s_ready && !s_scan_pending && sd_card_is_present() &&
        lv_tick_elaps(s_last_scan_tick) >= 5000)
        request_scan();

    if (s_items_dirty) rebuild_items();
    if (s_library_visible && !s_lib_transition) update_cover_window();
}

/* ============================ 页面创建 ============================ */

static void create_home(lv_obj_t *parent)
{
    s_home = lv_obj_create(parent);
    lv_obj_set_size(s_home, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_home, 0, 0);
    lv_obj_set_style_bg_opa(s_home, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_home, 0, 0);
    lv_obj_set_style_pad_all(s_home, 0, 0);
    lv_obj_remove_flag(s_home, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_home, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *card = lv_obj_create(s_home);
    lv_obj_set_size(card, HOME_CARD_W, HOME_CARD_H);
    lv_obj_set_pos(card, HOME_CARD_X, HOME_CARD_Y);
    lv_obj_set_style_bg_color(card, UI_COLOR_LIST_ROW_BG, 0);
    /* 与图片查看器首页相同的通透度：保留原卡片底色和 22px 白字，
     * 只让用户壁纸从卡片后透出；书架/阅读页不受影响。 */
    lv_obj_set_style_bg_opa(card, UI_HOME_CARD_BG_OPA, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, HOME_CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
    ui_apply_pressed_style(card);
    lv_obj_add_flag(card, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(card, home_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(card, home_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(card, home_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
    lv_obj_add_event_cb(card, home_clicked_cb, LV_EVENT_CLICKED, NULL);

    s_home_prev = lv_obj_create(card);
    lv_obj_set_size(s_home_prev, HOME_PREVIEW_SIZE, HOME_PREVIEW_SIZE);
    lv_obj_set_pos(s_home_prev, HOME_PREVIEW_X, HOME_PREVIEW_Y);
    lv_obj_set_style_bg_color(s_home_prev, UI_COLOR_CONTROL_BG, 0);
    lv_obj_set_style_bg_opa(s_home_prev, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_home_prev, 0, 0);
    lv_obj_set_style_radius(s_home_prev, HOME_PREVIEW_RADIUS, 0);
    lv_obj_set_style_clip_corner(s_home_prev, true, 0);
    lv_obj_set_style_pad_all(s_home_prev, 0, 0);
    lv_obj_remove_flag(s_home_prev, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_home_prev, LV_OBJ_FLAG_CLICKABLE);

    s_home_img = lv_image_create(s_home_prev);
    lv_obj_set_size(s_home_img, EBOOK_HOME_W, EBOOK_HOME_H);
    lv_obj_set_pos(s_home_img,
                   (HOME_PREVIEW_SIZE - (int32_t)EBOOK_HOME_W) / 2, 0);
    lv_obj_add_flag(s_home_img, LV_OBJ_FLAG_HIDDEN);
    lv_obj_remove_flag(s_home_img, LV_OBJ_FLAG_CLICKABLE);

    s_home_ph = lv_label_create(s_home_prev);
    lv_label_set_text(s_home_ph, LV_SYMBOL_FILE);
    lv_obj_set_style_text_font(s_home_ph, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_home_ph, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_center(s_home_ph);
    lv_obj_remove_flag(s_home_ph, LV_OBJ_FLAG_CLICKABLE);

    lv_obj_t *title = lv_label_create(card);
    lv_obj_set_width(title, HOME_TITLE_W);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, ui_font_22(), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_DOT);
    lv_label_set_text(title, "书库");
    lv_obj_align(title, LV_ALIGN_LEFT_MID, HOME_TITLE_X, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    s_home_status = lv_label_create(s_home);
    lv_obj_set_width(s_home_status, 400);
    lv_obj_set_style_text_align(s_home_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_home_status, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_home_status, ui_font_16(), 0);
    lv_obj_set_style_bg_color(s_home_status, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_home_status, LV_OPA_70, 0);
    lv_obj_set_style_radius(s_home_status, 10, 0);
    lv_obj_set_style_pad_all(s_home_status, 10, 0);
    lv_label_set_long_mode(s_home_status, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_home_status, "");
    lv_obj_align(s_home_status, LV_ALIGN_CENTER, 0, 0);
    set_hidden(s_home_status, true);
}

static void create_library(lv_obj_t *parent)
{
    s_library = lv_obj_create(parent);
    lv_obj_set_size(s_library, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_library, UI_HOR_RES, 0);
    lv_obj_set_style_bg_color(s_library, UI_COLOR_PAGE_BG, 0);
    lv_obj_set_style_bg_opa(s_library, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_library, 0, 0);
    lv_obj_set_style_radius(s_library, 0, 0);
    lv_obj_set_style_pad_all(s_library, 0, 0);
    lv_obj_remove_flag(s_library, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_library, LV_OBJ_FLAG_EVENT_BUBBLE);
    set_hidden(s_library, true);

    s_lib_back = create_back_button(s_library, LIB_HEADER_Y, lib_back_cb);
    lv_obj_t *title = lv_label_create(s_library);
    lv_obj_set_width(title, UI_HOR_RES - 64 - 16);
    lv_obj_set_style_text_align(title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, ui_font_22(), 0);
    lv_label_set_text(title, "书库");
    lv_obj_align_to(title, s_lib_back, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_remove_flag(title, LV_OBJ_FLAG_CLICKABLE);

    s_lib_scroll = lv_obj_create(s_library);
    lv_obj_set_size(s_lib_scroll, UI_HOR_RES, LIB_ITEM_H);
    lv_obj_set_pos(s_lib_scroll, 0, LIB_SCROLL_Y);
    lv_obj_set_style_bg_opa(s_lib_scroll, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_lib_scroll, 0, 0);
    lv_obj_set_style_pad_left(s_lib_scroll, LIB_PAD, 0);
    lv_obj_set_style_pad_right(s_lib_scroll, LIB_PAD, 0);
    lv_obj_set_style_pad_top(s_lib_scroll, 0, 0);
    lv_obj_set_style_pad_bottom(s_lib_scroll, 0, 0);
    lv_obj_set_scroll_dir(s_lib_scroll, LV_DIR_HOR);
    lv_obj_set_scrollbar_mode(s_lib_scroll, LV_SCROLLBAR_MODE_OFF);
    /* START 吸附把每个 item 的左边缘对齐到内容起始线（含 16px 内边距），
     * 松手后自动停到整本封面，形成书架的“吸附”手感。 */
    lv_obj_set_scroll_snap_x(s_lib_scroll, LV_SCROLL_SNAP_START);
    lv_obj_add_flag(s_lib_scroll, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_lib_scroll, lib_scroll_cb, LV_EVENT_SCROLL, NULL);

    s_lib_status = lv_label_create(s_library);
    lv_obj_set_width(s_lib_status, UI_HOR_RES - 40);
    lv_obj_set_style_text_align(s_lib_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_lib_status, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_lib_status, ui_font_16(), 0);
    lv_label_set_long_mode(s_lib_status, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_lib_status, "");
    lv_obj_align(s_lib_status, LV_ALIGN_CENTER, 0, 30);
    set_hidden(s_lib_status, true);

    s_lib_count = lv_label_create(s_library);
    lv_obj_set_width(s_lib_count, UI_HOR_RES - 40);
    lv_obj_set_style_text_align(s_lib_count, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_lib_count, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_lib_count, ui_font_16(), 0);
    lv_label_set_text(s_lib_count, "");
    lv_obj_align(s_lib_count, LV_ALIGN_BOTTOM_MID, 0, -10);
    set_hidden(s_lib_count, true);
}

static void create_reader(lv_obj_t *parent)
{
    s_reader = lv_obj_create(parent);
    lv_obj_set_size(s_reader, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_reader, 0, 0);
    lv_obj_set_style_bg_color(s_reader, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(s_reader, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_reader, 0, 0);
    lv_obj_set_style_radius(s_reader, 0, 0);
    lv_obj_set_style_pad_all(s_reader, 0, 0);
    lv_obj_remove_flag(s_reader, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_reader, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(s_reader, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_reader, reader_pressed_cb, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(s_reader, reader_released_cb, LV_EVENT_RELEASED, NULL);
    lv_obj_add_event_cb(s_reader, reader_press_lost_cb, LV_EVENT_PRESS_LOST, NULL);
    set_hidden(s_reader, true);

    for (unsigned i = 0; i < 2; ++i) {
        lv_obj_t *label = lv_label_create(s_reader);
        lv_obj_set_size(label, EBOOK_TEXT_W, EBOOK_TEXT_H);
        lv_obj_set_pos(label, READER_TEXT_X, READER_TEXT_Y);
        lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(label, ui_font_18(), 0);
        lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_LEFT, 0);
        lv_obj_set_style_text_line_space(label, EBOOK_LINE_SPACE, 0);
        lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
        lv_label_set_text(label, "");
        lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
        set_hidden(label, true);
        s_rpages[i] = label;
    }

    s_reader_status = lv_label_create(s_reader);
    lv_obj_set_width(s_reader_status, 400);
    lv_obj_set_style_text_align(s_reader_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_reader_status, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_font(s_reader_status, ui_font_16(), 0);
    lv_label_set_long_mode(s_reader_status, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_reader_status, "");
    lv_obj_align(s_reader_status, LV_ALIGN_CENTER, 0, 0);
    set_hidden(s_reader_status, true);

    s_reader_page_no = lv_label_create(s_reader);
    lv_obj_set_width(s_reader_page_no, 200);
    lv_obj_set_style_text_align(s_reader_page_no, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_color(s_reader_page_no, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(s_reader_page_no, &lv_font_montserrat_14, 0);
    lv_label_set_text(s_reader_page_no, "");
    lv_obj_align(s_reader_page_no, LV_ALIGN_BOTTOM_MID, 0, -10);
    set_hidden(s_reader_page_no, true);

    /* 控制台：顶部 scrim + 返回键 + 书名。整体淡入淡出，返回键和书名
     * 从状态栏下方开始，状态栏/控制台同时出现时不会互相遮挡。 */
    s_reader_controls = lv_obj_create(s_reader);
    lv_obj_set_size(s_reader_controls, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_reader_controls, 0, 0);
    lv_obj_set_style_bg_opa(s_reader_controls, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_reader_controls, 0, 0);
    lv_obj_set_style_pad_all(s_reader_controls, 0, 0);
    lv_obj_remove_flag(s_reader_controls, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(s_reader_controls, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_style_opa(s_reader_controls, LV_OPA_TRANSP, 0);
    lv_obj_add_flag(s_reader_controls, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t *scrim = lv_obj_create(s_reader_controls);
    lv_obj_set_size(scrim, UI_HOR_RES, READER_TOP_H);
    lv_obj_set_pos(scrim, 0, 0);
    lv_obj_set_style_bg_color(scrim, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(scrim, LV_OPA_40, 0);
    lv_obj_set_style_border_width(scrim, 0, 0);
    lv_obj_set_style_radius(scrim, 0, 0);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(scrim, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *back = create_back_button(s_reader_controls, READER_BACK_Y,
                                        reader_back_cb);
    s_reader_title = lv_label_create(s_reader_controls);
    lv_obj_set_width(s_reader_title, UI_HOR_RES - 64 - 16);
    lv_obj_set_style_text_align(s_reader_title, LV_TEXT_ALIGN_LEFT, 0);
    lv_obj_set_style_text_color(s_reader_title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(s_reader_title, ui_font_18(), 0);
    lv_label_set_long_mode(s_reader_title, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_reader_title, "");
    lv_obj_align_to(s_reader_title, back, LV_ALIGN_OUT_RIGHT_MID, 8, 0);
    lv_obj_remove_flag(s_reader_title, LV_OBJ_FLAG_CLICKABLE);
}

void ui_ebook_create(lv_obj_t *parent)
{
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_page, 0, 0);
    lv_obj_set_style_bg_color(s_page, UI_COLOR_PAGE_BG, 0);
    /* 页面背景透明：壁纸层固定在模式页最底层（ui_manager 创建），
     * 首页直接透出壁纸；书架和阅读页各自有不透明背景，不受影响。
     * 没有壁纸时看到的是壁纸层的原页面底色，外观与改动前一致。 */
    lv_obj_set_style_bg_opa(s_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_EVENT_BUBBLE);

    create_home(s_page);
    create_library(s_page);
    create_reader(s_page);

    for (unsigned i = 0; i < EBOOK_COVER_SLOTS; ++i) {
        s_cover_owner[i] = UINT32_MAX;
        s_cover_token[i] = i + 1U;
    }
    for (unsigned i = 0; i < EBOOK_MAX_BOOKS; ++i) s_item_state[i] = 0;

    s_reader_current = 0;
    s_reader_controls_opa = LV_OPA_TRANSP;
    s_sd_generation = sd_card_get_generation();
    ebook_set_ops(&s_ops, NULL);
    s_timer = lv_timer_create(ui_timer_cb, 100, NULL);
    if (!s_timer) ESP_LOGW(TAG, "Ebook UI timer unavailable");
}

lv_obj_t *ui_ebook_get_page(void)
{
    return s_page;
}

/* ============================ 模式生命周期 ============================ */

void ui_ebook_on_mode_enter(void)
{
    /* 离开电子书模式时worker已被销毁；按需重建任务。书库表/封面缓存
     * 保留，重建后直接复用，不重新扫描SD。 */
    esp_err_t start_ret = ebook_ensure_started();
    if (start_ret != ESP_OK)
        ESP_LOGW(TAG, "Ebook worker start deferred: %s", esp_err_to_name(start_ret));
    if (s_reader_active) close_reader(false);
    s_library_visible = false;
    s_lib_transition = false;
    lv_anim_delete(s_library, library_anim_x_cb);
    set_hidden(s_library, true);
    lv_obj_set_x(s_library, UI_HOR_RES);
    set_hidden(s_reader, true);
    reader_controls_hide_now();
    set_hidden(s_home, false);
    ui_statusbar_set_visible(true);
    lv_obj_scroll_to_x(s_lib_scroll, 0, LV_ANIM_OFF);

    s_sd_generation = sd_card_get_generation();
    if (!s_ready || s_generation != s_sd_generation) request_scan();
    else update_status();
}

void ui_ebook_on_mode_leave(void)
{
    if (s_reader_active) close_reader(false);
    lv_anim_delete(s_library, library_anim_x_cb);
    s_library_visible = false;
    s_lib_transition = false;
    set_hidden(s_library, true);
    lv_obj_set_x(s_library, UI_HOR_RES);
    set_hidden(s_reader, true);
    reader_controls_hide_now();
    set_hidden(s_home, false);
    /* 关闭阅读页之后销毁worker：省下12KiB任务栈，书库表与封面缓存保留。
     * 只置标志并唤醒，不在这里等待任务退出（当前处于LVGL回调）。 */
    ebook_request_worker_exit();
    ui_statusbar_set_visible(true);
}

void ui_ebook_close_for_lock(void)
{
    if (s_reader_active) close_reader(true);
    reader_controls_hide_now();
    ui_statusbar_set_visible(true);
}

bool ui_ebook_is_reader_active(void)
{
    return s_reader_active;
}
