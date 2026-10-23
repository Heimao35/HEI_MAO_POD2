/* Production settings grid with the Bluetooth entry: short click toggles the
 * power intent, long press opens the Bluetooth subpage and must suppress the
 * CLICKED that LVGL emits on release. ui_settings.c is compiled as a separate
 * translation unit here (its static page objects are reached through the
 * screen child tree) so this test does not need the Bluetooth page's statics. */
#include <assert.h>
#include <stdio.h>

#include "lvgl.h"

const lv_font_t *ui_font_14(void) { return &lv_font_montserrat_14; }
const lv_font_t *ui_font_22(void) { return &lv_font_montserrat_22; }

static bool status_visible = true;
static unsigned clock_shown, motion_shown, bt_shown;
static unsigned bt_set_calls;
static bool bt_enabled;

void ui_statusbar_set_visible(bool visible) { status_visible = visible; }
void ui_clock_settings_show(void) { ++clock_shown; }
void ui_motion_settings_show(void) { ++motion_shown; }
void ui_bluetooth_settings_show(void) { ++bt_shown; }
void bt_audio_set_enabled(bool enabled) { ++bt_set_calls; bt_enabled = enabled; }
bool bt_audio_is_enabled(void) { return bt_enabled; }

#include "../../main/ui/ui_settings.h"

static uint32_t ticks;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;

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

static void press_at(lv_indev_t *indev, int x, int y)
{
    pointer_point = (lv_point_t){x, y};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(indev);
    advance(20);
}

static void release(lv_indev_t *indev)
{
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);
    advance(50);
}

static void tap_obj(lv_indev_t *indev, lv_obj_t *obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    press_at(indev, (area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
    release(indev);
}

static void long_press_obj(lv_indev_t *indev, lv_obj_t *obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    press_at(indev, (area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
    /* Default LVGL long-press threshold is 400ms; hold well past it and
     * release, which still produces CLICKED and must be discarded. */
    advance(700);
    release(indev);
    advance(100);
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(460, 460);
    static uint8_t pixels[460 * 20 * 2];
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);

    ui_settings_create(lv_screen_active());
    ui_settings_show();
    advance(400);
    assert(ui_settings_is_visible());
    assert(!status_visible);

    /* Only the settings page exists here, so it is screen child 0; its grid
     * is child 1, the Bluetooth item is grid child 1 and the round button is
     * that item's child 0 (same tree used by test_clock.c). */
    lv_obj_t *settings_page = lv_obj_get_child(lv_screen_active(), 0);
    lv_obj_t *grid = lv_obj_get_child(settings_page, 1);
    lv_obj_t *bt_item = lv_obj_get_child(grid, 1);
    lv_obj_t *bt_button = lv_obj_get_child(bt_item, 0);
    lv_obj_t *bt_icon = lv_obj_get_child(bt_button, 0);
    lv_obj_t *bt_caption = lv_obj_get_child(bt_item, 1);

    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    lv_obj_scroll_to_view(bt_button, LV_ANIM_OFF);
    advance(40);

    /* Entry starts in the off palette: dark circle, white icon, gray caption. */
    assert(lv_color_eq(lv_obj_get_style_bg_color(bt_button, 0), UI_COLOR_CONTROL_BG));
    assert(lv_color_eq(lv_obj_get_style_text_color(bt_icon, 0), UI_COLOR_TEXT));
    assert(lv_color_eq(lv_obj_get_style_text_color(bt_caption, 0), UI_COLOR_TEXT_CAPTION));

    /* Short click toggles the power intent and must not open the subpage.
     * The entry itself must invert to the on palette immediately. */
    assert(!bt_enabled);
    tap_obj(pointer, bt_button);
    assert(bt_enabled);
    assert(bt_set_calls == 1);
    assert(bt_shown == 0);
    assert(ui_settings_is_visible());
    assert(lv_color_eq(lv_obj_get_style_bg_color(bt_button, 0), UI_COLOR_TEXT));
    assert(lv_color_eq(lv_obj_get_style_text_color(bt_icon, 0), UI_COLOR_ON_ACCENT));
    assert(lv_color_eq(lv_obj_get_style_text_color(bt_caption, 0), UI_COLOR_TEXT));
    tap_obj(pointer, bt_button);
    assert(!bt_enabled && bt_set_calls == 2 && bt_shown == 0);
    assert(lv_color_eq(lv_obj_get_style_bg_color(bt_button, 0), UI_COLOR_CONTROL_BG));
    assert(lv_color_eq(lv_obj_get_style_text_color(bt_icon, 0), UI_COLOR_TEXT));
    assert(lv_color_eq(lv_obj_get_style_text_color(bt_caption, 0), UI_COLOR_TEXT_CAPTION));
    puts("PASS short click toggles Bluetooth and inverts the entry palette");

    /* Long press opens the subpage and the release CLICKED must not also
     * toggle the switch (the pre-press state stays untouched). */
    unsigned calls = bt_set_calls;
    long_press_obj(pointer, bt_button);
    assert(bt_shown == 1);
    assert(bt_set_calls == calls);
    assert(!bt_enabled);
    assert(ui_settings_is_visible());
    puts("PASS long press opens the page and suppresses the release click");

    /* The next ordinary click after a long press must toggle again: the
     * pressed callback clears the stale consumption flag. */
    tap_obj(pointer, bt_button);
    assert(bt_enabled && bt_set_calls == calls + 1);
    assert(bt_shown == 1);
    puts("PASS a later short click is not swallowed by the prior long press");

    /* Other settings entries keep their original click behavior. */
    lv_obj_t *clock_button = lv_obj_get_child(lv_obj_get_child(grid, 3), 0);
    lv_obj_scroll_to_view(clock_button, LV_ANIM_OFF);
    advance(40);
    tap_obj(pointer, clock_button);
    assert(clock_shown == 1);
    lv_obj_t *motion_button = lv_obj_get_child(lv_obj_get_child(grid, 2), 0);
    lv_obj_scroll_to_view(motion_button, LV_ANIM_OFF);
    advance(40);
    tap_obj(pointer, motion_button);
    assert(motion_shown == 1);
    puts("PASS clock/motion entries keep their original click behavior");

    /* The subpage switch can change power while the grid is covered. Hide and
     * reopen the settings page to verify the entry resynchronizes its palette. */
    assert(bt_enabled);
    bt_enabled = false; /* simulate the subpage switch changing the intent */
    ui_settings_hide();
    advance(400);
    assert(!ui_settings_is_visible());
    ui_settings_show();
    advance(400);
    assert(lv_color_eq(lv_obj_get_style_bg_color(bt_button, 0), UI_COLOR_CONTROL_BG));
    assert(lv_color_eq(lv_obj_get_style_text_color(bt_icon, 0), UI_COLOR_TEXT));
    puts("PASS reopening settings resynchronizes the Bluetooth entry palette");
    return 0;
}
