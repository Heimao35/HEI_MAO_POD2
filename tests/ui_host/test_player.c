/* Runs the production player and gesture code with the project's real LVGL.
 * Only unrelated panels and the SD selection backend are replaced. Including
 * the .c files lets regressions inspect private state without test-only APIs
 * or changed visibility in firmware code. No SD or physical display is used. */
#include <assert.h>
#include <stdio.h>
#include "../../main/ui/ui_music_player.c"
#include "../../main/ui/ui_manager.c"

/* Fonts are mounted by the ESP-IDF startup, outside this host UI harness.
 * Keep the same sizes here so row bounds and gesture tests remain realistic. */
const lv_font_t *ui_font_14(void) { return &lv_font_montserrat_14; }
const lv_font_t *ui_font_16(void) { return &lv_font_montserrat_16; }
const lv_font_t *ui_font_18(void) { return &lv_font_montserrat_18; }
const lv_font_t *ui_font_22(void) { return &lv_font_montserrat_22; }

bool ui_settings_is_visible(void) { return false; }
bool ui_clock_settings_is_visible(void) { return false; }
void ui_clock_settings_create(lv_obj_t *parent) { (void)parent; }
void ui_clock_settings_update(void) {}
void ui_clock_settings_close_for_lock(void) {}
bool ui_power_dialog_is_visible(void) { return false; }
bool ui_lockscreen_is_visible(void) { return false; }
bool ui_mode_panel_is_visible(void) { return false; }
static bool test_adj_visible, test_adj_modal;
bool ui_adj_panel_is_visible(void) { return test_adj_visible; }
bool ui_adj_panel_is_modal(void) { return test_adj_visible && test_adj_modal; }
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
void ui_statusbar_set_visible(bool visible) { (void)visible; }
void ui_statusbar_show_time(bool show) { (void)show; }

/* Track display references independently of the JPEG cache tested by media_host. */
static uint8_t test_covers[2][240*240*2];
static unsigned cover_refs[2];
void cover_loader_retain(const uint8_t *pixels)
{
    if (!pixels) return;
    for (unsigned i=0;i<2;i++) if (pixels==test_covers[i]) { ++cover_refs[i]; return; }
    assert(!"unexpected cover");
}
void cover_loader_release(const uint8_t *pixels)
{
    if (!pixels) return;
    for (unsigned i=0;i<2;i++) if (pixels==test_covers[i]) {
        assert(cover_refs[i]); --cover_refs[i]; return;
    }
    assert(!"unexpected cover");
}
/* Background ownership is independent of clear cover ownership. Real RGB565
 * buffers let the actual LVGL renderer exercise the two full-screen canvases. */
static uint8_t test_backgrounds[3][460*460*2];
static unsigned background_refs[3];
void music_background_retain(const uint8_t *pixels)
{
    if (!pixels) return;
    for (unsigned i=0;i<3;i++) if (pixels==test_backgrounds[i]) { ++background_refs[i]; return; }
    assert(!"unexpected background");
}
void music_background_release(const uint8_t *pixels)
{
    if (!pixels) return;
    for (unsigned i=0;i<3;i++) if (pixels==test_backgrounds[i]) {
        assert(background_refs[i]); --background_refs[i]; return;
    }
    assert(!"unexpected background");
}
static uint32_t selected = UINT32_MAX, requests, clock_ms, lyric_line;
static bool cached_cover,test_finished,test_play_intent;
static bool take_finished_test(void) {bool value=test_finished;test_finished=false;return value;}
static void set_playing_test(bool playing) {test_play_intent=playing;}
static bool play_requested_test(void) {return test_play_intent;}
static bool output_not_started_test(void) {return false;}
static void select_test(uint32_t index)
{
    selected = index; ++requests;
    if (cached_cover) ui_music_player_set_cover(index,test_covers[1]);
}
static uint32_t get_clock(void) { return clock_ms; }
static uint32_t get_duration(void) { return 300; }
static uint32_t get_line(uint32_t ms) { (void)ms; return lyric_line; }
/* Check every row, so a stale pause icon on any inactive song is caught. */
static void assert_playlist_actions(unsigned current, bool playing)
{
    for (unsigned i=0;i<s_pl_item_count;i++) {
        const char *expected=i==current && playing ? LV_SYMBOL_PAUSE : LV_SYMBOL_PLAY;
        assert(!strcmp(lv_label_get_text(playlist_item_icon(s_pl_items[i])),expected));
    }
    assert(!strcmp(lv_label_get_text(playlist_item_icon(s_pl_items[current])),
                   lv_label_get_text(lv_obj_get_child(s_btn_toggle,0))));
}
static lv_point_t input_point;
static lv_indev_state_t input_state;
static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{ (void)indev; data->point = input_point; data->state = input_state; }
/* Sample a background-only screen coordinate to verify actual compositing,
 * in addition to object flags and reference counts. */
static uint16_t rendered_background;
static void flush(lv_display_t *d, const lv_area_t *a, uint8_t *p)
{
    if (a->x1<=20 && a->x2>=20 && a->y1<=200 && a->y2>=200)
        rendered_background=((const uint16_t *)p)[(200-a->y1)*(a->x2-a->x1+1)+20-a->x1];
    lv_display_flush_ready(d);
}
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
    assert(s_playlist_empty_lbl);
    assert(!lv_obj_has_flag(s_playlist_empty_lbl,LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(s_playlist_empty_lbl),"请插入 SD 卡"));
    assert(lv_obj_has_flag(s_playlist_empty_lbl,LV_OBJ_FLAG_IGNORE_LAYOUT));
    assert(!lv_obj_has_flag(s_playlist_empty_lbl,LV_OBJ_FLAG_CLICKABLE));
    puts("PASS initial no-SD playlist message before backend publication");
    lv_obj_add_event_cb(root,gesture_pressed_cb,LV_EVENT_PRESSED,NULL);
    lv_obj_add_event_cb(root,gesture_released_cb,LV_EVENT_RELEASED,NULL);
    ui_music_backend_t backend={.select=select_test,.get_time_ms=get_clock,
        .get_duration_sec=get_duration,.get_lyric_line=get_line};
    ui_music_player_bind(&backend);
    ui_music_player_set_library(12,"test");
    ui_music_player_show_song(0,"Short",NULL,300);
    advance(40);
    /* Real click bounds include each button's 10px extension: moving the
     * content must not make a bottom-edge lyric gesture hit a control button. */
    lv_area_t cover_bounds, button_click, progress_bounds, time_bounds;
    lv_obj_get_coords(s_cover_area,&cover_bounds);
    lv_obj_t *controls[]={s_btn_prev,s_btn_toggle,s_btn_next,s_btn_mode};
    for (unsigned i=0;i<4;i++) {
        lv_obj_get_click_area(controls[i],&button_click);
        assert(cover_bounds.y2<button_click.y1);
    }
    lv_obj_get_coords(s_progress,&progress_bounds);
    lv_obj_get_coords(s_time_lbl,&time_bounds);
    assert(progress_bounds.x2<time_bounds.x1);
    assert(s_pl_item_count==12);
    assert(lv_obj_get_style_text_align(s_title_lbl,0)==LV_TEXT_ALIGN_CENTER);
    lv_point_t first;
    lv_label_get_letter_pos(s_title_lbl,0,&first);
    assert(first.x>100); /* A short title really gets a centered letter position. */
    assert(lv_obj_has_flag(s_playlist_empty_lbl,LV_OBJ_FLAG_HIDDEN));
    assert_playlist_actions(0,false);
    /* Only the playing row shows PAUSE; all inactive rows remain PLAY through
     * local toggles, timer refreshes, backend updates and selection changes. */
    for (unsigned i=0;i<4;i++) {
        toggle_btn_cb(NULL);
        assert_playlist_actions(0,i%2==0);
        advance(240); /* No backend play-state: the UI action remains stable. */
        assert_playlist_actions(0,i%2==0);
    }
    ui_music_player_update_progress(0,300,true);
    assert_playlist_actions(0,true);
    ui_music_player_update_progress(0,300,false);
    assert_playlist_actions(0,false);
    switch_to_song(1);
    assert_playlist_actions(1,false);
    ui_music_player_update_progress(0,300,true);
    assert_playlist_actions(1,true);
    ui_music_player_update_progress(0,300,false);
    assert_playlist_actions(1,false);
    switch_to_song(0);
    ui_music_player_show_song(0,"Short",NULL,300);
    assert_playlist_actions(0,false);
    puts("PASS only playing playlist row shows PAUSE; paused/inactive rows show PLAY");
    lv_obj_t *row_title=playlist_item_title(s_pl_items[0]);
    lv_obj_update_layout(s_playlist);
    lv_area_t icon_bounds, title_bounds;
    lv_obj_get_coords(playlist_item_icon(s_pl_items[0]),&icon_bounds);
    lv_obj_get_coords(row_title,&title_bounds);
    assert(icon_bounds.x2<title_bounds.x1);
    assert(lv_label_get_long_mode(row_title)==LV_LABEL_LONG_SCROLL_CIRCULAR);
    lv_label_set_text(row_title,"A long song name with many words that must stay on one line and scroll horizontally across the row instead of wrapping");
    advance(40);
    assert(lv_obj_get_height(row_title)==lv_font_get_line_height(lv_obj_get_style_text_font(row_title,0)));
    puts("PASS centered short title and normal SD rows with single-line marquee");

    assert(lv_obj_get_child(s_page,0)==s_background_base);
    assert(lv_obj_get_child(s_page,1)==s_background_overlay);
    assert(!lv_obj_has_flag(s_background_base,LV_OBJ_FLAG_CLICKABLE));
    assert(!lv_obj_has_flag(s_background_overlay,LV_OBJ_FLAG_SCROLLABLE));
    memset(test_backgrounds[1],0xff,sizeof(test_backgrounds[1]));
    memset(test_backgrounds[2],0x88,sizeof(test_backgrounds[2]));
    ui_music_player_set_background(0,test_backgrounds[0]);
    assert(!s_background_current && s_background_incoming==test_backgrounds[0]);
    advance(80);
    int32_t background_opa=lv_obj_get_style_image_opa(s_background_overlay,0);
    assert(background_opa>0 && background_opa<LV_OPA_COVER);
    advance(160);
    assert(s_background_current==test_backgrounds[0] && !s_background_incoming);
    assert(rendered_background==0);
    assert(background_refs[0]==1 && lv_obj_has_flag(s_background_overlay,LV_OBJ_FLAG_HIDDEN));
    ui_music_player_set_background(0,test_backgrounds[0]); /* Same image does not restart a fade. */
    assert(!lv_anim_get(s_background_overlay,background_opa_cb));
    ui_music_player_set_background(0,test_backgrounds[1]); advance(80);
    background_opa=lv_obj_get_style_image_opa(s_background_overlay,0);
    assert(background_opa>0 && background_opa<LV_OPA_COVER);
    assert(lv_obj_get_style_image_opa(s_background_base,0)==LV_OPA_COVER);
    assert(rendered_background>0 && rendered_background<0xffff);
    ui_music_player_set_background(0,test_backgrounds[2]);
    assert(s_background_pending==test_backgrounds[2] && background_refs[2]==1);
    ui_music_player_set_background(0,test_backgrounds[0]);
    assert(!background_refs[2] && background_refs[0]==2);
    assert(lv_obj_get_style_image_opa(s_background_overlay,0)==background_opa);
    ui_music_player_set_background(0,NULL); /* Latest uncached selection releases pending only. */
    assert(!s_background_pending && background_refs[0]==1 && s_background_current==test_backgrounds[0]);
    ui_music_player_set_background(0,test_backgrounds[2]);
    advance(160);
    assert(s_background_current==test_backgrounds[1] && s_background_incoming==test_backgrounds[2]);
    assert(!background_refs[0] && background_refs[1]==1 && background_refs[2]==1);
    advance(240);
    assert(s_background_current==test_backgrounds[2] && !s_background_incoming);
    assert(rendered_background==0x8888);
    assert(!background_refs[1] && background_refs[2]==1);
    ui_music_player_set_background(1,test_backgrounds[0]); /* stale result */
    assert(!background_refs[0]);
    ui_music_player_set_background(0,test_backgrounds[1]);
    ui_music_player_set_background(0,test_backgrounds[0]);
    ui_music_player_set_library(12,"test"); /* Reset while base + incoming + pending are held. */
    for (unsigned i=0;i<3;i++) assert(!background_refs[i]);
    assert(!lv_anim_get(s_background_overlay,background_opa_cb));
    assert(!s_background_current && !s_background_incoming && !s_background_pending);
    advance(240);
    assert(lv_obj_has_flag(s_background_base,LV_OBJ_FLAG_HIDDEN));
    puts("PASS background crossfade, bounded latest pending, stale result rejection and reset ownership");

    /* Exercise actual LVGL animation ticks, not just the mode flag. Only one
     * content subtree is shown, and the existing cover reference is reused. */
    ui_music_player_set_cover(0,test_covers[0]);
    ui_music_player_set_lyrics("first\nsecond\nthird\nfourth\nfifth\nsixth",6,false);
    advance(40);
    cover_click_cb(NULL);
    assert(s_show_lyric && !s_content_lyric && cover_refs[0]==1);
    assert(lv_obj_has_flag(s_lyric_scroll,LV_OBJ_FLAG_HIDDEN));
    advance(40);
    assert(s_content_opa>0 && s_content_opa<LV_OPA_COVER);
    assert(lv_obj_get_style_image_opa(s_cover_canvas,0)==s_content_opa);
    advance(100);
    assert(s_content_lyric && s_content_opa>0 && s_content_opa<LV_OPA_COVER);
    assert(lv_obj_has_flag(s_cover_img,LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(s_lyric_scroll,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_style_text_opa(s_lyric_label,0)==s_content_opa);
    advance(120);
    assert(s_content_opa==LV_OPA_COVER && !lv_anim_get(s_cover_area,content_fade_exec));
    assert(cover_refs[0]==1 && !s_outgoing_pixels);

    /* Reverse during fade-out: remain on the same content and fade back in
     * from the sampled opacity, without a jump or queued second transition. */
    cover_click_cb(NULL); advance(40);
    int32_t partial_opa=s_content_opa;
    assert(!s_show_lyric && s_content_lyric && partial_opa>0 && partial_opa<LV_OPA_COVER);
    assert(ui_music_player_is_lyric_point(230,170));
    cover_click_cb(NULL);
    assert(s_content_opa==partial_opa && s_show_lyric);
    advance(200);
    assert(s_content_lyric && s_content_opa==LV_OPA_COVER);

    /* Reverse during fade-in after the midpoint; the latest target wins. */
    cover_click_cb(NULL); advance(130);
    partial_opa=s_content_opa;
    assert(!s_content_lyric && partial_opa>0 && partial_opa<LV_OPA_COVER);
    cover_click_cb(NULL);
    assert(s_content_opa==partial_opa);
    advance(220);
    assert(s_show_lyric && s_content_lyric && s_content_opa==LV_OPA_COVER);

    lyric_press_cb(NULL);
    lv_obj_scroll_to_y(s_lyric_scroll,60,LV_ANIM_OFF);
    int32_t saved_scroll=lv_obj_get_scroll_y(s_lyric_scroll);
    assert(saved_scroll>0);
    cover_click_cb(NULL); advance(240);
    cover_click_cb(NULL); advance(240);
    assert(lv_obj_get_scroll_y(s_lyric_scroll)==saved_scroll);
    lyric_release_cb(NULL);

    /* Missing pixels never hold the mode switch. A late publication changes
     * the canvas buffer, preserving the opacity of an in-progress fade-in. */
    ui_music_player_set_cover(0,NULL);
    cover_click_cb(NULL); advance(240);
    assert(!s_content_lyric && s_content_opa==LV_OPA_COVER);
    assert(lv_obj_has_flag(s_cover_canvas,LV_OBJ_FLAG_HIDDEN));
    cover_click_cb(NULL); advance(240);
    cover_click_cb(NULL); advance(130);
    partial_opa=s_content_opa;
    ui_music_player_set_cover(0,test_covers[0]);
    assert(s_content_opa==partial_opa && lv_obj_get_style_image_opa(s_cover_canvas,0)==partial_opa);
    advance(120);
    assert(!s_show_lyric && !s_content_lyric && s_content_opa==LV_OPA_COVER);
    puts("PASS sequential opacity fades, reversal in both phases, scroll retention and late cover");

    /* A song switch cancels a fade before taking the outgoing snapshot.
     * Request cover while lyrics are fading in; the latest target is cover. */
    cover_click_cb(NULL); advance(130);
    assert(s_content_lyric && s_content_opa<LV_OPA_COVER);
    cover_click_cb(NULL);
    cached_cover=true;
    ui_music_player_next_animated();
    assert(requests==1 && selected==1 && s_current_idx==1);
    assert(!lv_anim_get(s_cover_area,content_fade_exec));
    assert(!s_show_lyric && !s_content_lyric && s_content_opa==LV_OPA_COVER);
    assert(lv_obj_get_style_text_opa(s_lyric_label,0)==LV_OPA_COVER);
    assert(lv_obj_get_style_image_opa(s_cover_canvas,0)==LV_OPA_COVER);
    assert(!ui_music_player_can_show_song(0) && ui_music_player_can_show_song(1));
    assert(s_cover_pixels==test_covers[1] && s_outgoing_pixels==test_covers[0]);
    assert(cover_refs[0]==1 && cover_refs[1]==1);
    advance(80);
    assert(lv_obj_get_x(s_outgoing_area)<110 && lv_obj_get_x(s_outgoing_area)>-COVER_SIZE);
    assert(lv_obj_get_x(s_cover_area)<UI_HOR_RES && lv_obj_get_x(s_cover_area)>110);
    assert(!lv_obj_has_flag(s_outgoing_canvas,LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(s_cover_canvas,LV_OBJ_FLAG_HIDDEN));
    /* A stale completion must never replace the old snapshot or incoming cover. */
    ui_music_player_show_song(0,"Stale",test_covers[0],123);
    assert(s_cover_pixels==test_covers[1] && s_outgoing_pixels==test_covers[0]);
    ui_music_player_next_animated(); assert(requests==1); /* ignored only during animation */
    advance(320);
    assert(!s_cover_animating && lv_obj_get_x(s_cover_area)==110);
    assert(cover_refs[0]==0 && cover_refs[1]==1);
    assert(lv_obj_has_flag(s_outgoing_area,LV_OBJ_FLAG_HIDDEN));
    puts("PASS simultaneous cached-cover slides, separate image ownership and stale result rejection");

    cached_cover=false;
    ui_music_player_next_animated();
    assert(selected==2 && s_current_idx==2 && !s_cover_pixels);
    assert(lv_obj_has_flag(s_cover_canvas,LV_OBJ_FLAG_HIDDEN));
    advance(400); /* No ready notification; animation must finish anyway. */
    assert(!s_cover_animating && lv_obj_get_x(s_cover_area)==110);
    ui_music_player_next_animated(); assert(selected==3 && s_current_idx==3);
    ui_music_player_set_cover(2,test_covers[0]); assert(!s_cover_pixels);
    advance(400);
    ui_music_player_set_cover(3,test_covers[0]); /* Late cover appears in place. */
    assert(s_cover_pixels==test_covers[0] && lv_obj_get_x(s_cover_area)==110);
    ui_music_player_prev_animated(); advance(80);
    ui_music_player_set_library(1,"reinserted");
    assert(!s_cover_animating && lv_obj_get_x(s_cover_area)==110);
    assert(cover_refs[0]==0 && cover_refs[1]==0);
    ui_music_player_next_animated(); assert(selected==0);
    advance(400); assert(!s_cover_animating);
    puts("PASS immediate uncached switch, late cover, repeated slow loads, rescan cleanup and single-song transition");

    ui_music_player_set_lyrics("first\na long second lyric that wraps into multiple visual lines in this small viewport\nthird\nfourth\nfifth\nsixth\nlast",7,false);
    cover_click_cb(NULL); advance(240);
    /* Odd font line heights can place the center between pixels; allow rounding. */
    assert(s_show_lyric && abs(line_y(0)-COVER_SIZE/2)<=1);
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

    ui_music_player_next_animated();
    assert(!strcmp(lv_label_get_text(s_outgoing_lyrics),"first\na long second lyric that wraps into multiple visual lines in this small viewport\nthird\nfourth\nfifth\nsixth\nlast"));
    ui_music_player_set_lyrics("new lyrics",1,false);
    assert(strstr(lv_label_get_text(s_outgoing_lyrics),"first")!=NULL);
    assert(lv_obj_get_style_text_font(s_outgoing_lyrics,0)==lv_obj_get_style_text_font(s_lyric_label,0));
    assert(lv_obj_get_style_text_line_space(s_outgoing_lyrics,0)==lv_obj_get_style_text_line_space(s_lyric_label,0));
    advance(400);
    puts("PASS outgoing lyric snapshot retains previous text and styling while new lyrics load");

    cover_click_cb(NULL); advance(40);
    assert(lv_anim_get(s_cover_area,content_fade_exec));
    ui_music_player_set_library(12,"test");
    assert(!lv_anim_get(s_cover_area,content_fade_exec));
    assert(s_content_lyric==s_show_lyric && s_content_opa==LV_OPA_COVER);
    ui_music_player_show_song(0,"Gesture",NULL,300);
    ui_music_player_set_background(0,test_backgrounds[0]);
    if(s_show_lyric) cover_click_cb(NULL);
    advance(200);
    lv_indev_t *indev=lv_indev_create();
    lv_indev_set_type(indev,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(indev,read_pointer);
    lv_indev_set_mode(indev,LV_INDEV_MODE_EVENT);
    advance(40);
    drag(indev,300,170,-100,0);
    assert(!s_show_lyric && s_cover_animating);
    advance(400);
    /* The strip above the moved cover must no longer toggle its mode. The
     * newly occupied bottom strip must both toggle and scroll as normal. */
    int old_top=UI_STATUSBAR_HEIGHT+30;
    int new_bottom_strip=old_top+COVER_SIZE+8;
    input(indev,230,old_top+8,LV_INDEV_STATE_PRESSED);
    input(indev,230,old_top+8,LV_INDEV_STATE_RELEASED);
    assert(!s_show_lyric && !s_cover_animating);
    input(indev,230,new_bottom_strip,LV_INDEV_STATE_PRESSED);
    input(indev,230,new_bottom_strip,LV_INDEV_STATE_RELEASED);
    assert(s_show_lyric);
    advance(200);
    ui_music_player_set_lyrics("one\ntwo\nthree\nfour\nfive\nsix\nseven\neight\nnine\nten",10,false);
    drag(indev,230,new_bottom_strip,0,-90);
    assert(s_show_lyric && !ui_music_player_is_playlist_visible());
    assert(!s_lyric_touch_active);
    /* 硬件按键栏可见时仍允许下层原有手势；完整声光栏仍然互斥。 */
    test_adj_visible=true;test_adj_modal=true;
    drag(indev,230,330,0,-80); advance(400);
    assert(!ui_music_player_is_playlist_visible());
    test_adj_modal=false;
    drag(indev,230,330,0,-80); advance(400);
    assert(ui_music_player_is_playlist_visible());
    drag(indev,230,10,0,100); advance(400);
    assert(!ui_music_player_is_playlist_visible());
    test_adj_visible=false;
    puts("PASS real pointer gestures: lyrics scroll, playlist gestures and non-modal volume coexist");
    ui_music_player_set_library(0,"请插入 SD 卡");
    assert(s_pl_item_count==0 && s_library_count==0);
    assert(!lv_obj_has_flag(s_playlist_empty_lbl,LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(s_playlist_empty_lbl),"请插入 SD 卡"));
    ui_music_player_toggle_playlist(true);
    advance(400);
    assert(lv_obj_is_visible(s_playlist_empty_lbl));
    ui_music_player_set_library(0,"未找到歌曲");
    assert(!strcmp(lv_label_get_text(s_playlist_empty_lbl),"未找到歌曲"));
    ui_music_player_set_library(0,"正在扫描 SD 卡...");
    assert(!strcmp(lv_label_get_text(s_playlist_empty_lbl),"正在扫描 SD 卡..."));
    ui_music_player_set_library(2,"reinserted");
    assert(lv_obj_has_flag(s_playlist_empty_lbl,LV_OBJ_FLAG_HIDDEN));
    assert(s_pl_item_count==2);
    ui_music_player_set_library(0,NULL);
    assert(!lv_obj_has_flag(s_playlist_empty_lbl,LV_OBJ_FLAG_HIDDEN));
    assert(!strcmp(lv_label_get_text(s_playlist_empty_lbl),"未找到歌曲"));
    ui_music_player_set_library(3,"restored");
    ui_music_player_restore_selection(2);
    assert(s_current_idx==2 && ui_music_player_can_show_song(2));
    ui_music_player_show_song(2,"restored song",NULL,123);
    assert(!strcmp(lv_label_get_text(s_title_lbl),"restored song"));
    ui_music_player_restore_selection(99); assert(s_current_idx==2);
    ui_music_player_set_library(0,NULL);
    puts("PASS empty playlist status, no SD, scan results and reinserted card");
    for (unsigned i=0;i<3;i++) assert(!background_refs[i]);
    /* EOS复用既有动画；异步准备期的按钮传递明确目标状态。 */
    ui_music_player_set_library(3,"ready");
    ui_music_player_restore_selection(0);
    s_backend.take_finished=take_finished_test;
    s_backend.set_playing=set_playing_test;
    s_is_playing=false;toggle_btn_cb(NULL);assert(test_play_intent);
    s_is_playing=false;toggle_btn_cb(NULL);assert(test_play_intent);
    /* A real request can remain pending while the decoder/artwork is cold.
     * Timer and late metadata must not turn the button back to PLAY. */
    s_backend.is_play_requested=play_requested_test;
    s_backend.is_playing=output_not_started_test;
    test_play_intent=true;s_is_playing=false;
    clock_ms=2345;player_timer_cb(NULL);
    assert(s_is_playing && !strcmp(lv_label_get_text(lv_obj_get_child(s_btn_toggle,0)),LV_SYMBOL_PAUSE));
    ui_music_player_show_song(0,"late artwork",NULL,300);
    assert(test_play_intent && s_is_playing);
    assert(lv_bar_get_value(s_progress)>0); /* late art did not reset real progress */
    s_is_playing=false; /* model a stale local frame immediately after selecting */
    toggle_btn_cb(NULL);assert(!test_play_intent && !s_is_playing);
    player_timer_cb(NULL);assert(!s_is_playing);
    toggle_btn_cb(NULL);assert(test_play_intent);
    player_timer_cb(NULL);assert(s_is_playing);
    test_play_intent=false;player_timer_cb(NULL);assert(!s_is_playing); /* failure/EOF */
    s_backend.is_play_requested=NULL;s_backend.is_playing=NULL;
    puts("PASS pending playback survives timer and late metadata; rapid click cancels current request");
    s_single_loop=true;
    unsigned before_eos=requests;
    test_finished=true;player_timer_cb(NULL);
    assert(requests==before_eos+1 && !test_finished);
    s_cover_animating=true;test_finished=true;before_eos=requests;
    player_timer_cb(NULL);assert(test_finished && requests==before_eos);
    s_cover_animating=false;s_single_loop=false;
    player_timer_cb(NULL);assert(!test_finished && requests==before_eos+1);
    advance(1500);
    puts("PASS explicit asynchronous play intent, single-loop EOS and deferred sequential EOS animation");
    /* No timer-handler/animation advances during these calls: model the
     * stopped LVGL task while the screen remains physically dark. */
    ui_music_player_set_library(3,"ready");ui_music_player_restore_selection(2);
    s_single_loop=false;test_finished=true;before_eos=requests;
    ui_music_player_service_finished(false);
    assert(!test_finished && requests==before_eos+1 && selected==0 && s_current_idx==0);
    assert(!s_cover_animating && !lv_anim_get(s_cover_area,cover_anim_x_cb));
    ui_music_player_service_finished(false);assert(requests==before_eos+1); /* one-shot */
    test_finished=true;ui_music_player_service_finished(false);
    assert(selected==1 && s_current_idx==1 && !s_cover_animating);
    s_single_loop=true;test_finished=true;before_eos=requests;
    ui_music_player_service_finished(false);
    assert(selected==1 && requests==before_eos+1 && !s_cover_animating);
    /* Current mode is read at EOS: a mode toggle immediately affects the next
     * dark completion, without an independent or stale copy in the worker. */
    mode_btn_cb(NULL);assert(!s_single_loop);
    test_finished=true;ui_music_player_service_finished(false);assert(selected==2);
    /* A cover slide paused halfway by screen-off must not block continuation
     * forever, and wake must not replay its stale completion into the new art. */
    ui_music_player_next_animated();assert(s_cover_animating && lv_anim_get(s_cover_area,cover_anim_x_cb));
    test_finished=true;ui_music_player_service_finished(false);
    assert(!s_cover_animating && !s_outgoing_pixels);
    assert(!lv_anim_get(s_cover_area,cover_anim_x_cb) && !lv_anim_get(s_outgoing_area,cover_anim_x_cb));
    assert(lv_obj_has_flag(s_outgoing_area,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_style_x(s_cover_area,0)==(UI_HOR_RES-COVER_SIZE)/2);
    ui_music_player_set_library(1,"ready");test_finished=true;before_eos=requests;
    ui_music_player_service_finished(false);assert(selected==0 && requests==before_eos+1);
    ui_music_player_set_library(0,"empty");test_finished=true;before_eos=requests;
    ui_music_player_service_finished(false);assert(test_finished && requests==before_eos);
    test_finished=false;
    puts("PASS dark sequential/repeat/single-song EOS without LVGL ticks, active slide cleanup and empty library");
    puts("ALL PLAYER UI TESTS PASSED");
    return 0;
}
