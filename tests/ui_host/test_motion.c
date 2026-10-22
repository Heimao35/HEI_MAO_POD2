/* Real motion-settings page with LVGL's software renderer and event input.
 * Sensor request double verifies both switches post their combined intent
 * without changing slide/back behavior or making synchronous I2C calls. */
#include <assert.h>
#include <stdio.h>
#include "../../main/ui/ui_motion_settings.c"

/* Fonts are mounted by firmware startup; use the same built-in size here so
 * layout/click coordinates match the production page. */
const lv_font_t *ui_font_22(void) { return &lv_font_montserrat_22; }

static bool requested_steps, requested_wake;
static unsigned request_count;
void motion_sensor_set_features(bool steps, bool wake)
{ requested_steps=steps; requested_wake=wake; ++request_count; }
void ui_lockscreen_set_steps(bool valid, uint32_t steps)
{ assert(!valid && !steps); }
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

static void click(lv_indev_t *indev, int x, int y)
{
    pointer_point = (lv_point_t){x, y};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(indev);
    advance(20);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);
    advance(40);
}

static void click_obj(lv_indev_t *indev, lv_obj_t *obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    click(indev, (area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2);
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

    /* 页面尚未创建时恢复调用必须安全忽略（main 只在 UI 创建后调用）。 */
    ui_motion_settings_restore(true, true);
    assert(!requested_steps && !requested_wake && request_count == 0);

    ui_motion_settings_create(lv_screen_active());
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);

    /* The page starts off-screen and hidden; only show() animates it in.
     * Coordinates are only valid after LVGL has laid the hidden page out. */
    lv_obj_update_layout(lv_screen_active());
    assert(lv_obj_has_flag(s_motion_page, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_x(s_motion_page) == 460);
    assert(!ui_motion_settings_is_visible());
    ui_motion_settings_show();
    advance(400);
    assert(ui_motion_settings_is_visible());
    assert(!lv_obj_has_flag(s_motion_page, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_x(s_motion_page) == 0);
    puts("PASS motion page slides in from the right and becomes visible");

    /* Both functions default off and the two switches must be independent. */
    assert(!ui_motion_settings_steps_enabled());
    assert(!ui_motion_settings_wake_enabled());
    click_obj(pointer, s_motion_steps_switch);
    assert(ui_motion_settings_steps_enabled());
    assert(!ui_motion_settings_wake_enabled());
    click_obj(pointer, s_motion_wake_switch);
    assert(ui_motion_settings_steps_enabled());
    assert(ui_motion_settings_wake_enabled());
    assert(requested_steps && requested_wake && request_count==2);
    click_obj(pointer, s_motion_steps_switch);
    assert(!requested_steps && requested_wake && request_count==3);
    click_obj(pointer, s_motion_steps_switch);
    assert(requested_steps && requested_wake && request_count==4);
    puts("PASS large switches post independent combined requests without hardware access");

    /* Back button uses the same 350 ms right-side exit as the clock subpage. */
    click_obj(pointer, s_motion_back);
    advance(400);
    assert(!ui_motion_settings_is_visible());
    assert(lv_obj_has_flag(s_motion_page, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_x(s_motion_page) == 460);
    puts("PASS back arrow slides out and hides the page");

    /* Re-entering must preserve the session's switch choices and page position. */
    ui_motion_settings_show();
    advance(400);
    assert(ui_motion_settings_is_visible());
    assert(lv_obj_get_x(s_motion_page) == 0);
    assert(ui_motion_settings_steps_enabled());
    assert(ui_motion_settings_wake_enabled());
    puts("PASS reopening keeps the RAM switch state");

    /* 开机从 NVS 恢复：程序化设置选中状态不会触发 VALUE_CHANGED，
     * 每次恢复只允许投递一次合并请求，视觉必须与恢复值一致。 */
    unsigned requests = request_count;
    ui_motion_settings_restore(false, false);
    assert(!ui_motion_settings_steps_enabled() && !ui_motion_settings_wake_enabled());
    assert(!lv_obj_has_state(s_motion_steps_switch, LV_STATE_CHECKED));
    assert(!lv_obj_has_state(s_motion_wake_switch, LV_STATE_CHECKED));
    assert(!requested_steps && !requested_wake && request_count == requests + 1);
    ui_motion_settings_restore(true, true);
    assert(ui_motion_settings_steps_enabled() && ui_motion_settings_wake_enabled());
    assert(lv_obj_has_state(s_motion_steps_switch, LV_STATE_CHECKED));
    assert(lv_obj_has_state(s_motion_wake_switch, LV_STATE_CHECKED));
    assert(requested_steps && requested_wake && request_count == requests + 2);
    /* 恢复后用户点击仍按真实选中状态取反并投递新请求。 */
    click_obj(pointer, s_motion_wake_switch);
    assert(ui_motion_settings_steps_enabled() && !ui_motion_settings_wake_enabled());
    assert(requested_steps && !requested_wake && request_count == requests + 3);
    puts("PASS boot restore applies persisted switches without extra UI events");

    /* Lock-screen entry closes the floating page without touching switches. */
    ui_motion_settings_close_for_lock();
    advance(400);
    assert(!ui_motion_settings_is_visible());
    assert(lv_obj_has_flag(s_motion_page, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_x(s_motion_page) == 460);
    puts("PASS close_for_lock hides the page without changing settings");
    return 0;
}
