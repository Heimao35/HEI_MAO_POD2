/* Runs the production player and gesture code with the project's real LVGL.
 * Only unrelated panels and the SD selection backend are replaced. Including
 * the .c files lets regressions inspect private state without test-only APIs
 * or changed visibility in firmware code. No SD or physical display is used. */
#include <assert.h>
#include <stdio.h>
#include "../../main/ui/ui_music_player.c"
#include "../../main/ui/ui_manager.c"

bool ui_settings_is_visible(void) { return false; }
bool ui_lockscreen_is_visible(void) { return false; }
bool ui_mode_panel_is_visible(void) { return false; }
bool ui_adj_panel_is_visible(void) { return false; }
void ui_settings_show(void) {} void ui_settings_hide(void) {}
void ui_lockscreen_hide(void) {}
void ui_mode_panel_show(void) {} void ui_mode_panel_hide(void) {}
void ui_adj_panel_show(void) {} void ui_adj_panel_hide(void) {}

void ui_statusbar_update(void) {} void ui_lockscreen_update(void) {}
void ui_video_player_create(lv_obj_t *p) { (void)p; }
void ui_image_viewer_create(lv_obj_t *p) { (void)p; }
void ui_ebook_create(lv_obj_t *p) { (void)p; }
lv_obj_t *ui_video_player_get_page(void) { return NULL; }
lv_obj_t *ui_image_viewer_get_page(void) { return NULL; }
lv_obj_t *ui_ebook_get_page(void) { return NULL; }
void ui_lockscreen_create(lv_obj_t *p) { (void)p; }
void ui_lockscreen_show(void) {}
void ui_mode_panel_create(lv_obj_t *p) { (void)p; }
void ui_mode_panel_set_callback(ui_mode_switch_cb_t cb) { (void)cb; }
void ui_adj_panel_create(lv_obj_t *p) { (void)p; }
void ui_settings_create(lv_obj_t *p) { (void)p; }
void ui_statusbar_create(lv_obj_t *p) { (void)p; }
void ui_statusbar_show_time(bool show) { (void)show; }

static uint32_t selected = UINT32_MAX, requests, clock_ms, lyric_line;
static void select_test(uint32_t index) { selected = index; ++requests; }
static uint32_t get_clock(void) { return clock_ms; }
static uint32_t get_duration(void) { return 300; }
static uint32_t get_line(uint32_t ms) { (void)ms; return lyric_line; }
static lv_point_t input_point;
static lv_indev_state_t input_state;
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{ (void)indev; data->point = input_point; data->state = input_state; }
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{ (void)a; (void)p; lv_display_flush_ready(d); }
static void advance(uint32_t ms)
{
    for (uint32_t i = 0; i < ms; i += 10) {
        lv_tick_inc(10); lv_timer_handler();
    }
    lv_obj_update_layout(s_page);
}
static void input(lv_indev_t *indev, int x, int y, lv_indev_state_t state)
{
    input_point = (lv_point_t){x,y}; input_state = state;
    lv_indev_read(indev); advance(20);
}
static void drag(lv_indev_t *indev, int x, int y, int dx, int dy)
{
    input(indev,x,y,LV_INDEV_STATE_PRESSED);
    for(int i=1;i<=6;i++) input(indev,x+dx*i/6,y+dy*i/6,LV_INDEV_STATE_PRESSED);
    input(indev,x+dx,y+dy,LV_INDEV_STATE_RELEASED);
}
static int line_y(uint32_t row)
{
    lv_point_t pos;
    lv_label_get_letter_pos(s_lyric_label, lyric_line_char_index(row), &pos);
    return lv_obj_get_y(s_lyric_label) +
           lv_obj_get_style_pad_top(s_lyric_label,LV_PART_MAIN) + pos.y -
           lv_obj_get_scroll_y(s_lyric_scroll) +
           lv_font_get_line_height(lv_obj_get_style_text_font(s_lyric_label,0))/2;
}
int main(void)
{
    lv_init();
    lv_display_t *display=lv_display_create(460,460);
    static uint8_t pixels[460*20*2];
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    lv_obj_t *root=lv_screen_active();
    lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    ui_music_player_create(root);
    lv_obj_add_event_cb(root,gesture_pressed_cb,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(root,gesture_released_cb,LV_EVENT_RELEASED,NULL);
    ui_music_backend_t backend={.select=select_test,.get_time_ms=get_clock,
        .get_duration_sec=get_duration,.get_lyric_line=get_line};
    ui_music_player_bind(&backend);
    ui_music_player_set_library(12,"test");
    ui_music_player_show_song(0,"Short",NULL,300);
    advance(40);
    assert(s_pl_item_count==12);
    assert(lv_obj_get_style_text_align(s_title_lbl,0)==LV_TEXT_ALIGN_CENTER);
    lv_point_t first;
    lv_label_get_letter_pos(s_title_lbl,0,&first);
    assert(first.x>100); /* A short title really gets a centered letter position. */
    lv_obj_t *row_title=lv_obj_get_child(s_pl_items[0],0);
    assert(lv_label_get_long_mode(row_title)==LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(row_title,"A long song name with many words that must stay on one line and scroll horizontally across the row instead of wrapping");
    advance(40);
    assert(lv_obj_get_height(row_title)==lv_font_get_line_height(lv_obj_get_style_text_font(row_title,0)));
    puts("PASS centered short title and normal SD rows with single-line marquee");

    ui_music_player_next_animated();
    advance(170);
    assert(requests==0 && !ui_music_player_can_show_song(1));
    assert(lv_obj_get_x(s_cover_area)>-UI_HOR_RES);
    ui_music_player_next_animated(); /* ignored during outgoing animation */
    advance(210);
    assert(requests==1 && selected==1 && s_cover_waiting_song);
    assert(lv_obj_get_x(s_cover_area)<=-COVER_SIZE);
    advance(1000); /* slow SD cannot bring the old cover back */
    assert(lv_obj_get_x(s_cover_area)<=-COVER_SIZE);
    ui_music_player_song_ready(2); assert(s_cover_waiting_song);
    ui_music_player_show_song(1,"Next",NULL,300);
    ui_music_player_song_ready(1); advance(400);
    assert(!s_cover_animating && lv_obj_get_x(s_cover_area)==110);
    ui_music_player_prev_animated(); advance(400);
    ui_music_player_set_library(1,"reinserted");
    assert(!s_cover_animating && lv_obj_get_x(s_cover_area)==110);
    ui_music_player_next_animated(); advance(400);
    assert(selected==0 && s_cover_waiting_song); /* same-song transition */
    ui_music_player_song_ready(0); advance(400);
    puts("PASS offscreen async handoff, stale completion, repeated switch and rescan cancellation");

    ui_music_player_set_lyrics("first\na long second lyric that wraps into multiple visual lines in this small viewport\nthird\nfourth\nfifth\nsixth\nlast",7,false);
    cover_click_cb(NULL); advance(40);
    assert(s_show_lyric && line_y(0)==COVER_SIZE/2);
    lyric_line=6; advance(2600);
    assert(abs(line_y(6)-COVER_SIZE/2)<=1);
    lyric_line=1; advance(800);
    assert(abs(line_y(1)-COVER_SIZE/2)<=1); /* real wrapped line positions */
    lyric_press_cb(NULL);
    lv_obj_scroll_to_y(s_lyric_scroll,0,LV_ANIM_OFF);
    advance(2500); assert(lv_obj_get_scroll_y(s_lyric_scroll)==0);
    lyric_release_cb(NULL); advance(1500);
    assert(lv_obj_get_scroll_y(s_lyric_scroll)==0);
    advance(1100); assert(abs(line_y(1)-COVER_SIZE/2)<=1);
    puts("PASS first/last centered lyrics, wrapping, no follow while held and two-second return");

    ui_music_player_set_library(12,"test");
    ui_music_player_show_song(0,"Gesture",NULL,300);
    if(s_show_lyric) cover_click_cb(NULL);
    lv_indev_t *indev=lv_indev_create();
    lv_indev_set_type(indev,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev,read_pointer);
    lv_indev_set_mode(indev,LV_INDEV_MODE_EVENT);
    advance(40);
    drag(indev,300,170,-100,0);
    assert(!s_show_lyric && s_cover_animating);
    advance(400); ui_music_player_song_ready(selected); advance(400);
    input(indev,230,170,LV_INDEV_STATE_PRESSED);
    input(indev,230,170,LV_INDEV_STATE_RELEASED);
    assert(s_show_lyric);
    ui_music_player_set_lyrics("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten",10,false);
    drag(indev,230,220,0,-90);
    assert(s_show_lyric && !ui_music_player_is_playlist_visible());
    assert(!s_lyric_touch_active);
    drag(indev,350,320,0,-80); advance(400);
    assert(ui_music_player_is_playlist_visible());
    drag(indev,230,10,0,100); advance(400);
    assert(!ui_music_player_is_playlist_visible());
    puts("PASS real pointer gestures: swipe does not click, lyrics scroll, playlist opens outside and closes from top");
    puts("ALL PLAYER UI TESTS PASSED");
    return 0;
}
