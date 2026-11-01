/* Ebook page with a RAM backend double: home card, horizontal library,
 * reader controls and page-turn animation, without SD/JPEG/TXT. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* Host LVGL only enables the sizes used by the other UI tests. */
#define lv_font_montserrat_48 lv_font_montserrat_26
#include "../../main/ui/ui_ebook.c"

/* ---------------- backend double ---------------- */
static ebook_ops_t ops;
static uint32_t mock_count = 3;
static unsigned scans, opens, pages, closes, covers, homes;
static uint32_t last_open_book, last_open_token;
static uint32_t last_page, last_page_token;
static uint32_t last_cover_slot, last_cover_book, last_cover_token;
static uint32_t last_home_token;
static const uint64_t mock_progress_key = 0x123456789abcdef0ULL;
static uint32_t mock_saved_page;
static unsigned progress_sets;
static uint32_t last_saved_page;

esp_err_t ebook_init(void) { return ESP_OK; }
void ebook_set_ops(const ebook_ops_t *value, void *ctx)
{
    (void)ctx;
    if (value) ops = *value;
}
void ebook_request_scan(void) { ++scans; }
uint32_t ebook_get_count(void) { return mock_count; }
bool ebook_has_cover(uint32_t index) { return index == 0; }
bool ebook_get_title(uint32_t index, char *out, size_t cap)
{
    if (index >= mock_count) return false;
    snprintf(out, cap, "书%d", (int)index);
    return true;
}
void ebook_request_home_cover(uint32_t book, uint32_t generation, uint32_t token)
{
    (void)book;
    (void)generation;
    (void)token;
    ++homes;
    last_home_token = token;
}
void ebook_request_cover(uint32_t book, uint32_t slot, uint32_t token)
{
    ++covers;
    last_cover_book = book;
    last_cover_slot = slot;
    last_cover_token = token;
}
void ebook_open(uint32_t book, uint32_t token)
{
    ++opens;
    last_open_book = book;
    last_open_token = token;
}
void ebook_request_page(uint32_t page, uint32_t token)
{
    ++pages;
    last_page = page;
    last_page_token = token;
}
void ebook_close(void) { ++closes; }

bool player_settings_get_ebook_progress(uint64_t key, uint32_t *page,
                                        uint32_t *pages)
{
    if (key != mock_progress_key || !progress_sets) return false;
    *page = mock_saved_page;
    *pages = 5;
    return true;
}
void player_settings_set_ebook_progress(uint64_t key, uint32_t page,
                                        uint32_t pages)
{
    assert(key == mock_progress_key);
    assert(page < pages);
    ++progress_sets;
    last_saved_page = page;
    mock_saved_page = page;
}

/* ---------------- platform doubles ---------------- */
static bool statusbar_visible = true;
ui_mode_t ui_manager_get_current_mode(void) { return UI_MODE_EBOOK; }
bool ui_lockscreen_is_visible(void) { return false; }
void ui_statusbar_set_visible(bool visible) { statusbar_visible = visible; }
uint32_t sd_card_get_generation(void) { return 7; }
bool sd_card_is_present(void) { return true; }
const lv_font_t *ui_font_16(void) { return &lv_font_montserrat_16; }
const lv_font_t *ui_font_18(void) { return &lv_font_montserrat_18; }
const lv_font_t *ui_font_22(void) { return &lv_font_montserrat_22; }

/* ---------------- LVGL harness ---------------- */
static uint32_t ticks;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;
static lv_indev_t *s_indev;

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = pointer_point;
    data->state = pointer_state;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static void advance(unsigned ms)
{
    for (unsigned n = 0; n < ms; n += 10) {
        ticks += 10;
        lv_tick_inc(10);
        lv_timer_handler();
    }
    lv_obj_update_layout(lv_screen_active());
}

static void press_release(lv_point_t from, lv_point_t to)
{
    pointer_point = from;
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(s_indev);
    advance(20);
    pointer_point = to;
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(s_indev);
    advance(60);
}

static void click_obj(lv_obj_t *obj)
{
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    lv_point_t p = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
    press_release(p, p);
}

static uint8_t cover_pixels[EBOOK_COVER_W * EBOOK_COVER_H * 2];
static uint8_t home_pixels[EBOOK_HOME_W * EBOOK_HOME_H * 2];

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    lv_init();
    lv_display_t *display = lv_display_create(UI_HOR_RES, UI_VER_RES);
    static uint8_t draw[UI_HOR_RES * 20 * 2];
    lv_display_set_buffers(display, draw, NULL, sizeof(draw),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);

    ui_ebook_create(lv_screen_active());
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    s_indev = pointer;
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *card = lv_obj_get_child(s_home, 0);
    assert(s_page && card && s_library && s_reader && s_lib_back);
    assert(lv_obj_get_style_text_font(lv_obj_get_child(card, 1), 0) == ui_font_22());
    assert(strcmp(lv_label_get_text(lv_obj_get_child(card, 1)), "书库") == 0);
    assert(lv_obj_has_flag(s_library, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_reader, LV_OBJ_FLAG_HIDDEN));
    puts("PASS ebook home card exists with 22px 书库 title");

    ui_ebook_on_mode_enter();
    assert(scans == 1);
    assert(statusbar_visible);
    ops.library(3, false, 7, NULL);
    ops.scan_done(EBOOK_SCAN_OK, 7, NULL);
    assert(homes == 1 && last_home_token != 0);
    advance(200);
    assert(s_count == 3 && s_ready && !s_items_dirty);
    assert(s_items[0] && s_items[2] && !s_items[3]);
    assert(strcmp(lv_label_get_text(s_item_titles[0]), "书0") == 0);
    assert(lv_obj_get_style_text_font(s_item_titles[0], 0) == ui_font_18());
    assert(lv_obj_get_x(s_items[0]) == 0);
    assert(lv_obj_get_x(s_items[1]) == (int32_t)LIB_ITEM_STRIDE);
    assert(strcmp(lv_label_get_text(s_lib_count), "共 3 本") == 0);
    assert(lv_obj_has_flag(s_home_status, LV_OBJ_FLAG_HIDDEN));
    ops.home_cover(0, home_pixels, EBOOK_HOME_W, EBOOK_HOME_H, 7, true, NULL);
    assert(!lv_obj_has_flag(s_home_img, LV_OBJ_FLAG_HIDDEN));
    puts("PASS scan builds horizontal items, count and home cover");

    click_obj(card);
    assert(s_lib_transition);
    advance(UI_ANIM_DURATION_MS + 100);
    assert(s_library_visible && lv_obj_get_x(s_library) == 0);
    assert(lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN));
    assert(covers >= 3);
    assert(last_cover_book < 3);
    ops.cover(last_cover_slot, last_cover_book, cover_pixels,
              EBOOK_COVER_W, EBOOK_COVER_H, last_cover_token, true, NULL);
    assert(!lv_obj_has_flag(s_item_imgs[last_cover_book], LV_OBJ_FLAG_HIDDEN));
    puts("PASS home card slides in the library and covers are published");

    click_obj(s_items[1]);
    assert(s_reader_active);
    assert(opens == 1 && last_open_book == 1);
    assert(!statusbar_visible);
    assert(!lv_obj_has_flag(s_reader, LV_OBJ_FLAG_HIDDEN));
    ops.opened(1, 5, mock_progress_key, last_open_token, EBOOK_OPEN_OK, NULL);
    assert(pages == 1 && last_page == 0);
    assert(progress_sets == 0);
    ops.page(0, "第一页正文", last_page_token, true, NULL);
    assert(!s_reader_loading);
    assert(progress_sets == 1 && last_saved_page == 0);
    assert(strcmp(lv_label_get_text(s_rpages[s_reader_current]), "第一页正文") == 0);
    assert(lv_obj_get_style_text_font(s_rpages[s_reader_current], 0) == ui_font_18());
    assert(s_reader_controls_visible && statusbar_visible);
    assert(strstr(lv_label_get_text(s_reader_page_no), "1 / 5") != NULL);
    puts("PASS cover click opens the 18px reader and shows controls");

    /* Tap blank area: controls + status bar hide together. */
    press_release((lv_point_t){230, 250}, (lv_point_t){230, 250});
    advance(READER_CONTROLS_FADE_MS + 50);
    assert(!s_reader_controls_visible && !statusbar_visible);
    press_release((lv_point_t){230, 250}, (lv_point_t){230, 250});
    advance(READER_CONTROLS_FADE_MS + 50);
    assert(s_reader_controls_visible && statusbar_visible);
    puts("PASS tap toggles reader controls and status bar together");

    /* Left swipe requests the next page; callback starts the slide. */
    lv_point_t from = {320, 250}, to = {120, 250};
    press_release(from, to);
    assert(pages == 2 && last_page == 1);
    ops.page(1, "第二页正文", last_page_token, true, NULL);
    assert(s_reader_sliding);
    advance(READER_SLIDE_MS + 50);
    assert(!s_reader_sliding && s_reader_page == 1);
    assert(progress_sets == 2 && last_saved_page == 1);
    assert(strcmp(lv_label_get_text(s_rpages[s_reader_current]), "第二页正文") == 0);
    assert(strstr(lv_label_get_text(s_reader_page_no), "2 / 5") != NULL);
    puts("PASS horizontal swipe cross-slides to the next page");

    /* Back returns to the library and releases the book text. */
    press_release((lv_point_t){230, 250}, (lv_point_t){230, 250});
    advance(READER_CONTROLS_FADE_MS + 50);
    assert(s_reader_controls_visible);
    lv_obj_t *back = lv_obj_get_child(s_reader_controls, 1);
    click_obj(back);
    assert(!s_reader_active && closes == 1);
    assert(lv_obj_has_flag(s_reader, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(s_library, LV_OBJ_FLAG_HIDDEN));
    assert(statusbar_visible);
    puts("PASS reader back restores the library and status bar");

    /* 再次打开同一本书：模拟 NVS 中已保存第 1 页，应直接请求该页。 */
    click_obj(s_items[1]);
    assert(s_reader_active);
    ops.opened(1, 5, mock_progress_key, last_open_token, EBOOK_OPEN_OK, NULL);
    assert(pages == 3 && last_page == 1);
    ops.page(1, "继续阅读", last_page_token, true, NULL);
    assert(strcmp(lv_label_get_text(s_rpages[s_reader_current]), "继续阅读") == 0);
    assert(strstr(lv_label_get_text(s_reader_page_no), "2 / 5") != NULL);
    lv_obj_t *back2 = lv_obj_get_child(s_reader_controls, 1);
    click_obj(back2);
    assert(!s_reader_active && closes == 2);
    puts("PASS reopening the book resumes at the saved page");

    click_obj(s_lib_back);
    /* 书库滑出期间首页必须已经可见，而不是等动画结束才出现。 */
    assert(!lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN));
    advance(UI_ANIM_DURATION_MS + 100);
    assert(!s_library_visible && lv_obj_has_flag(s_library, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN));
    puts("PASS library back reveals home during the slide-out");

    /* Mode enter reuses the ready list (generation unchanged). */
    unsigned before = scans;
    ui_ebook_on_mode_leave();
    ui_ebook_on_mode_enter();
    assert(scans == before && statusbar_visible);
    assert(!lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN));
    puts("PASS mode re-entry keeps the scanned library");

    puts("ALL EBOOK UI TESTS PASSED");
    return 0;
}
