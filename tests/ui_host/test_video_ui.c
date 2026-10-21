/* Video page object/callback smoke test with a simulated video backend.
 * No SD/JPEG/audio device is used; the production UI file is compiled
 * directly so object ordering, page population, thumbnail descriptors,
 * canvas handoff and lifecycle calls are exercised by real LVGL. */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Host LVGL only enables the sizes used by existing UI tests. */
#define lv_font_montserrat_48 lv_font_montserrat_26

#include "../../main/ui/ui_video_player.c"

static video_player_ops_t test_ops;
static video_state_t test_state = VIDEO_STATE_IDLE;
static uint16_t test_progress;
static unsigned scans, loads, plays, stops, releases, seeks;
static uint32_t last_page, last_play, test_index;
static bool statusbar_visible = true;
static ui_mode_t test_mode = UI_MODE_VIDEO;

const lv_font_t *ui_font_18(void) { return &lv_font_montserrat_18; }
void ui_statusbar_set_visible(bool visible) { statusbar_visible = visible; }
bool ui_lockscreen_is_visible(void) { return false; }
ui_mode_t ui_manager_get_current_mode(void) { return test_mode; }
uint32_t sd_card_get_generation(void) { return 7; }

void video_player_set_ops(const video_player_ops_t *ops, void *ctx)
{
    (void)ctx;
    if (ops) test_ops = *ops;
}
void video_player_scan(void) { ++scans; }
void video_player_load_page(uint32_t first) { ++loads; last_page = first; }
void video_player_play(uint32_t index)
{
    ++plays;
    last_play = index;
    test_index = index;
    test_state = VIDEO_STATE_LOADING;
}
void video_player_pause(void) {}
void video_player_resume(void) {}
void video_player_toggle(void) {}
void video_player_seek(uint16_t per_mille)
{
    ++seeks;
    test_progress = per_mille;
}
void video_player_stop(void)
{
    ++stops;
    test_state = VIDEO_STATE_IDLE;
}
void video_player_release(void) { ++releases; }
video_state_t video_player_get_state(void) { return test_state; }
uint32_t video_player_get_index(void) { return test_index; }
uint16_t video_player_get_progress(void) { return test_progress; }

static uint8_t frame_pixels[VIDEO_PLAYER_PAD_WIDTH * VIDEO_PLAYER_PAD_HEIGHT * 2];
static uint8_t thumbs[VIDEO_PAGE_SIZE][VIDEO_PLAYER_THUMB_SIZE * VIDEO_PLAYER_THUMB_SIZE * 2];

static void flush(lv_display_t *disp, const lv_area_t *area, uint8_t *px)
{
    (void)area;
    (void)px;
    lv_display_flush_ready(disp);
}

static void advance(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
    lv_obj_update_layout(s_page);
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(460, 460);
    static uint8_t draw[460 * 20 * 2];
    lv_display_set_buffers(display, draw, NULL, sizeof(draw),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);

    ui_video_player_create(lv_screen_active());
    assert(s_page && s_list_view && s_player_view && s_canvas && s_controls);
    if (!s_switch_pixels) s_switch_pixels = malloc(PLAYER_FRAME_BYTES);
    assert(s_switch_pixels && s_switch_canvas && s_player_backdrop);
    lv_obj_update_layout(s_page);
    assert(s_slider_touch && lv_obj_get_height(s_slider_touch) >= 40);
    assert(lv_obj_get_height(s_slider) <= 20);
    lv_point_t bar_point = {100, UI_VER_RES - 10};
    lv_point_t video_point = {100, 100};
    assert(is_control_bar_point(&bar_point));
    assert(!is_control_bar_point(&video_point));
    assert(!lv_obj_has_flag(s_list_view, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_player_view, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_grid, LV_OBJ_FLAG_HIDDEN));
    for (unsigned i = 0; i < VIDEO_PAGE_SIZE; ++i) {
        assert(s_tiles[i]);
        assert(lv_obj_has_flag(s_tiles[i], LV_OBJ_FLAG_HIDDEN));
    }

    ui_video_player_on_mode_enter();
    assert(scans == 1);
    assert(statusbar_visible);

    /* Simulated scan and page publication: four tiles, 18px names. */
    assert(test_ops.library && test_ops.page && test_ops.thumbnail &&
           test_ops.frame && test_ops.state);
    test_ops.library(4, false, 7, NULL);
    assert(loads == 1 && last_page == 0);

    video_page_t page = {.first = 0, .count = 4};
    for (unsigned i = 0; i < 4; ++i) {
        snprintf(page.names[i], VIDEO_NAME_MAX, "video-%u.mjpeg", i);
        page.sizes[i] = 1000 + i;
    }
    test_ops.page(&page, 7, NULL);
    lv_obj_update_layout(s_page);
    assert(s_page_loaded && s_page_count == 4);
    for (unsigned i = 0; i < VIDEO_PAGE_SIZE; ++i) {
        assert(!lv_obj_has_flag(s_tiles[i], LV_OBJ_FLAG_HIDDEN));
        assert(s_tile_index[i] == i);
        assert(!strcmp(lv_label_get_text(s_tile_names[i]), page.names[i]));
    }

    test_ops.thumbnail(0, 0, thumbs[0], VIDEO_PLAYER_THUMB_SIZE,
                       VIDEO_PLAYER_THUMB_SIZE, 7, NULL);
    assert(!lv_obj_has_flag(s_tile_images[0], LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_tile_placeholders[0], LV_OBJ_FLAG_HIDDEN));
    assert(s_thumb_dsc[0].header.w == VIDEO_PLAYER_THUMB_SIZE);
    assert(s_thumb_dsc[0].data == thumbs[0]);

    /* Preview click starts playback; state/frame callbacks update the view. */
    play_index(2);
    assert(s_player_active);
    assert(plays == 1 && last_play == 2);
    assert(lv_obj_has_flag(s_list_view, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(s_player_view, LV_OBJ_FLAG_HIDDEN));
    assert(!statusbar_visible);

    test_ops.state(VIDEO_STATE_PLAYING, 2, NULL, NULL);
    test_ops.frame(frame_pixels, VIDEO_PLAYER_PAD_WIDTH,
                   VIDEO_PLAYER_PAD_HEIGHT, 1, NULL);
    assert(!lv_obj_has_flag(s_canvas, LV_OBJ_FLAG_HIDDEN));
    assert(s_canvas_has_buffer);
    test_progress = 500;
    ui_timer_cb(NULL);
    assert(lv_slider_get_value(s_slider) == 500);

    /* Cross-slide switch: old frame snapshot leaves while target enters.
     * Frames from the old index must not repaint the incoming player. */
    assert(s_last_frame);
    switch_video(1);
    assert(s_player_active && s_player_transitioning);
    assert(s_current_index == 3);
    assert(!lv_obj_has_flag(s_switch_canvas, LV_OBJ_FLAG_HIDDEN));
    lv_obj_update_layout(s_page);
    assert(lv_obj_get_x(s_player_view) == (int32_t)UI_HOR_RES);
    assert(lv_obj_has_flag(s_canvas, LV_OBJ_FLAG_HIDDEN));
    test_index = 2;
    test_ops.frame(frame_pixels, VIDEO_PLAYER_PAD_WIDTH,
                   VIDEO_PLAYER_PAD_HEIGHT, 2, NULL);
    assert(lv_obj_has_flag(s_canvas, LV_OBJ_FLAG_HIDDEN));
    test_index = 3;
    test_ops.frame(frame_pixels, VIDEO_PLAYER_PAD_WIDTH,
                   VIDEO_PLAYER_PAD_HEIGHT, 3, NULL);
    assert(!lv_obj_has_flag(s_canvas, LV_OBJ_FLAG_HIDDEN));
    advance(PLAYER_SLIDE_MS + 50);
    assert(!s_player_transitioning);
    assert(lv_obj_get_x(s_player_view) == 0);
    assert(lv_obj_has_flag(s_switch_canvas, LV_OBJ_FLAG_HIDDEN));

    return_to_list();
    assert(!s_player_active);
    assert(stops == 1);
    assert(!lv_obj_has_flag(s_list_view, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_player_view, LV_OBJ_FLAG_HIDDEN));
    assert(statusbar_visible);

    /* Lock/mode leave must clear image descriptors before backend release. */
    ui_video_player_close_for_lock();
    assert(releases == 1);
    assert(!s_thumb_dsc[0].data);
    assert(lv_obj_has_flag(s_grid, LV_OBJ_FLAG_HIDDEN));
    assert(statusbar_visible);

    puts("PASS video UI smoke/lifecycle");
    return 0;
}
