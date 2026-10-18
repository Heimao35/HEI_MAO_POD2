/* Real LVGL hit testing/dragging and production key debounce/repeat; only
 * board GPIO, power snapshot and task creation are replaced. */
#include <assert.h>
#include <stdio.h>
#include "../../main/ui/ui_adj_panel.c"
#include "../../main/volume_keys.c"

static ui_device_state_t state = {.brightness=80, .volume=50};
static bool locked_page, dark, ui_locked, allow_lock=true;
static int up_level=1, down_level=1;
static TickType_t ticks;
static unsigned vol_calls, br_calls, other_clicks;
static uint8_t backend_volume;
ui_device_state_t *ui_get_device_state(void) { return &state; }
bool ui_lockscreen_is_visible(void) { return locked_page; }
const lv_font_t *ui_font_22(void) { return &lv_font_montserrat_22; }
bool screen_power_is_dark(void) { return dark; }
bool lvgl_port_lock(uint32_t timeout)
{ (void)timeout; assert(!ui_locked); ui_locked=allow_lock; return allow_lock; }
void lvgl_port_unlock(void) { assert(ui_locked); ui_locked=false; }
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(unsigned ms) { ticks+=ms; }
int gpio_get_level(gpio_num_t pin)
{ assert(pin==35 || pin==34); return pin==35 ? up_level : down_level; }
esp_err_t gpio_config(const gpio_config_t *cfg)
{
    assert(cfg->pin_bit_mask == ((1ULL<<35)|(1ULL<<34)));
    assert(cfg->mode==GPIO_MODE_INPUT && cfg->pull_up_en==GPIO_PULLUP_ENABLE);
    assert(!cfg->pull_down_en && cfg->intr_type==GPIO_INTR_DISABLE);
    return ESP_OK;
}
BaseType_t xTaskCreate(void (*fn)(void *), const char *name, uint32_t stack,
                      void *arg, UBaseType_t priority, TaskHandle_t *task)
{ (void)fn;(void)name;(void)arg;(void)priority;assert(stack==2048);*task=(void *)1;return pdPASS; }
static void volume_backend(uint8_t v) { ++vol_calls; backend_volume=v; }
static void bright_backend(uint8_t v) { (void)v; ++br_calls; }
static void other_cb(lv_event_t *e) { (void)e; ++other_clicks; }
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{ (void)a;(void)p;lv_display_flush_ready(d); }
static lv_point_t point;
static lv_indev_state_t pointer_state;
static void read_pointer(lv_indev_t *i,lv_indev_data_t *d)
{ (void)i;d->point=point;d->state=pointer_state; }
static void advance(unsigned ms)
{
    for(unsigned n=0;n<ms;n+=10) { lv_tick_inc(10);lv_timer_handler(); }
    lv_obj_update_layout(lv_screen_active());
}
static void touch(lv_indev_t *i,int x,int y,lv_indev_state_t state_)
{ point=(lv_point_t){x,y};pointer_state=state_;lv_indev_read(i);advance(20); }
static void key_time(unsigned ms)
{ for(unsigned n=0;n<ms;n+=10) { ticks+=10;poll_keys(); } }
int main(void)
{
    lv_init();
    lv_display_t *d=lv_display_create(460,460);
    static uint8_t pixels[460*20*2];
    lv_display_set_buffers(d,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    lv_obj_t *root=lv_screen_active();
    lv_obj_remove_flag(root,LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *button=lv_button_create(root);
    lv_obj_set_pos(button,10,100);lv_obj_set_size(button,440,300);
    lv_obj_add_event_cb(button,other_cb,LV_EVENT_CLICKED,NULL);
    ui_adj_panel_create(root);
    ui_adj_panel_set_callbacks(bright_backend,volume_backend);
    lv_indev_t *i=lv_indev_create();lv_indev_set_type(i,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(i,read_pointer);
    ui_adj_panel_adjust_volume(5);advance(400);
    assert(state.volume==55 && backend_volume==55 && vol_calls==1 && !br_calls);
    assert(ui_adj_panel_is_visible() && !ui_adj_panel_is_modal());
    assert(lv_obj_has_flag(s_bright_col,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_overlay,LV_OBJ_FLAG_HIDDEN));
    assert(!lv_anim_get(s_overlay,overlay_anim_opa_cb));
    lv_point_t p={20,200};assert(lv_indev_search_obj(root,&p)==button);
    touch(i,20,200,LV_INDEV_STATE_PRESSED);touch(i,20,200,LV_INDEV_STATE_RELEASED);
    assert(other_clicks==1);
    p=(lv_point_t){350,200};assert(lv_indev_search_obj(root,&p)==button);
    /* A blank point INSIDE the narrow transparent panel also passes through. */
    p=(lv_point_t){350,390};assert(lv_indev_search_obj(root,&p)==button);
    lv_area_t slider;lv_obj_get_coords(s_vol_slider,&slider);
    int x=(slider.x1+slider.x2)/2;
    touch(i,x,slider.y2-40,LV_INDEV_STATE_PRESSED);
    for(int n=0;n<6;n++) touch(i,x,slider.y2-40-n*20,LV_INDEV_STATE_PRESSED);
    assert(s_volume_touch && vol_calls>1 && state.volume==backend_volume);
    advance(6000);assert(!s_closing && ui_adj_panel_is_visible());
    touch(i,x,slider.y2-140,LV_INDEV_STATE_RELEASED);
    assert(!s_volume_touch);
    advance(4500);assert(!s_closing);
    advance(600);assert(s_closing);
    /* A key during the exit animation cancels its stale hide callback. */
    ui_adj_panel_adjust_volume(-5);advance(500);
    assert(ui_adj_panel_is_visible() && !s_closing);
    advance(5100);advance(400);assert(!ui_adj_panel_is_visible());
    puts("PASS non-modal hit testing, real slider drag, held-touch timeout, reopen during exit");

    ui_adj_panel_show();advance(500);
    assert(ui_adj_panel_is_modal() && !lv_obj_has_flag(s_bright_col,LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(s_overlay,LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_width(s_panel)==UI_ADJ_PANEL_WIDTH);
    advance(6000);assert(ui_adj_panel_is_modal() && !s_closing);
    p=(lv_point_t){20,200};assert(lv_indev_search_obj(root,&p)==s_overlay);
    ui_adj_panel_adjust_volume(200);advance(400);
    assert(state.volume==100 && !ui_adj_panel_is_modal());
    ui_adj_panel_adjust_volume(-200);assert(state.volume==0);
    mute_btn_cb(NULL);assert(s_muted);
    ui_adj_panel_adjust_volume(5);assert(state.volume==5 && !s_muted);
    ui_adj_panel_show();advance(500);assert(ui_adj_panel_is_modal());
    ui_adj_panel_hide();advance(500);
    locked_page=true;ui_adj_panel_show();advance(400);
    assert(lv_obj_get_style_bg_opa(s_overlay,0)==LV_OPA_TRANSP);
    ui_adj_panel_hide();advance(500);locked_page=false;
    puts("PASS full gesture panel, modal overlay, no full-panel timeout, transitions, clamping/mute");

    assert(volume_keys_init()==ESP_OK);
    state.volume=50;reset_keys(0);ticks=0;key_time(50);
    unsigned before=vol_calls;
    up_level=0;key_time(20);up_level=1;key_time(40);assert(vol_calls==before);
    up_level=0;key_time(40);assert(state.volume==55);
    key_time(350);assert(state.volume==55);
    key_time(60);assert(state.volume==60);
    key_time(100);assert(state.volume==65);
    up_level=1;key_time(40);
    down_level=0;key_time(40);assert(state.volume==60);
    down_level=1;key_time(40);
    before=vol_calls;up_level=down_level=0;key_time(800);assert(vol_calls==before);
    down_level=1;key_time(500);assert(vol_calls==before);
    up_level=1;key_time(50);
    allow_lock=false;up_level=0;key_time(40);assert(s_pending==5);
    allow_lock=true;key_time(10);assert(s_pending==0 && state.volume==65);
    dark=true;key_time(100);dark=false;key_time(500);assert(state.volume==65);
    up_level=1;key_time(50);up_level=0;key_time(40);assert(state.volume==70);
    up_level=1;key_time(50);
    /* Unsigned tick differences remain valid across wrap. */
    ticks=UINT32_MAX-20;up_level=0;poll_keys();key_time(40);assert(state.volume==75);
    assert(!br_calls && !ui_locked);
    puts("PASS GPIO pin config, debounce, repeat, chords, LVGL contention, dark suppression, tick wrap");
    puts("ALL VOLUME TESTS PASSED");
    return 0;
}
