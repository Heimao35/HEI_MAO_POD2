/* Real power coordinator and LVGL modal dialog. Deterministic time plus
 * hardware/media/NVS doubles: never talks to the user's board or flash. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include <setjmp.h>
#include "../../main/system_power.c"
#include "../../main/ui/ui_power_dialog.c"
static TickType_t ticks;
static bool locked,dark,suspended,display_off,media_paused,media_idle=true,anim_idle=true;
static bool fail_ui_lock, mounted=true, sd_lock=true;
static unsigned wakes,shorts,stops,saves,off_calls,restarts,captures;
static esp_err_t save_result,off_result,restart_result,capture_result;
static player_settings_t saved;
static jmp_buf terminal;
static int terminal_action;
const lv_font_t *ui_font_18(void) { return &lv_font_montserrat_18; }
bool lvgl_port_lock(uint32_t wait)
{ (void)wait; assert(!locked); if(fail_ui_lock) return false; locked=true; return true; }
void lvgl_port_unlock(void) { assert(locked); locked=false; }
TickType_t xTaskGetTickCount(void) { return ticks; }
void vTaskDelay(unsigned delay)
{ assert(!locked); ticks+=delay; if(display_off) suspended=true; }
bool screen_power_is_dark(void) { return dark; }
void screen_power_request_wake(void) { ++wakes; dark=false; }
void screen_power_key_short_press(void) { ++shorts; }
void screen_power_shutdown_display(bool off)
{ assert(!locked); display_off=off; dark=off; if(!off) suspended=false; }
bool screen_power_display_suspended(void) { assert(!locked); return suspended; }
void ui_lockscreen_stop_anim(void) { assert(locked); ++stops; }
bool ui_lockscreen_anim_is_idle(void) { assert(!locked); return anim_idle; }
bool music_player_adapter_pause(bool pause)
{ assert(!locked); media_paused=pause; return media_idle; }
esp_err_t music_player_adapter_capture_selection(void)
{ assert(media_paused && media_idle && !locked); ++captures; return capture_result; }
bool sd_card_is_mounted(void) { return mounted; }
bool sd_card_io_lock(TickType_t wait) { (void)wait; assert(!locked); return sd_lock; }
void sd_card_io_unlock(void) { assert(!locked); }
void player_settings_capture(player_settings_t *v)
{ assert(locked); *v=(player_settings_t){.brightness=37,.volume=23,.auto_brightness=true,.muted=true}; strcpy(v->song_folder,"folder"); strcpy(v->song_audio,"song.flac"); }
bool player_settings_equal(const player_settings_t *a,const player_settings_t *b)
{ return a->volume==b->volume; }
esp_err_t player_settings_save(const player_settings_t *v)
{
    assert(!locked && media_paused && suspended && display_off);
    assert(v->brightness==37 && v->muted && v->auto_brightness);
    saved=*v; ++saves; return save_result;
}
esp_err_t pmic_axp2101_poweroff(void)
{
    assert(!locked && saves && suspended && media_paused); ++off_calls;
    if(terminal_action==1) longjmp(terminal,1);
    return off_result;
}
esp_err_t pmic_axp2101_restart(void)
{
    assert(!locked && saves && suspended && media_paused); ++restarts;
    if (terminal_action==2) longjmp(terminal,2);
    return restart_result;
}
BaseType_t xTaskCreate(void (*fn)(void *),const char *name,uint32_t stack,void *arg,UBaseType_t priority,TaskHandle_t *task)
{ assert(fn==power_worker && !strcmp(name,"system_power") && stack==8192 && !arg && priority==3); *task=(void *)1; return pdPASS; }
static void flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{ (void)a;(void)p;lv_display_flush_ready(d); }
static void reset_case(void)
{
    atomic_store(&s_held,false); atomic_store(&s_dialog,false); atomic_store(&s_finalizing,false);
    atomic_store(&s_popup_requested,false); atomic_store(&s_latched,false); atomic_store(&s_woke_on_press,false);
    atomic_store(&s_short_ready,false); atomic_store(&s_saving,false);
    atomic_store(&s_input_ready,true);
    atomic_store(&s_request,0); ticks=0; locked=dark=suspended=display_off=media_paused=false;
    media_idle=anim_idle=true; mounted=sd_lock=true; fail_ui_lock=false;
    saves=off_calls=restarts=captures=0; terminal_action=0; save_result=off_result=restart_result=capture_result=ESP_OK;
    lvgl_port_lock(0); ui_power_dialog_hide(); lvgl_port_unlock();
}
static void long_press(void)
{
    system_power_key_edge(true); ticks+=6000; system_power_step();
    assert(atomic_load(&s_dialog) && ui_power_dialog_is_visible());
    system_power_key_edge(false);
}
int main(void)
{
    lv_init(); lv_display_t *d=lv_display_create(460,460);
    static uint8_t pixels[460*20*2];
    lv_display_set_buffers(d,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(d,flush);
    /* Before screen/system init, the power-on gesture cannot queue UI work. */
    system_power_key_edge(true); system_power_key_short(); system_power_key_edge(false);
    assert(!atomic_load(&s_held) && shorts==0);
    assert(system_power_init()==ESP_OK && system_power_init()==ESP_ERR_INVALID_STATE);
    system_power_key_edge(false); system_power_key_short();
    assert(!atomic_load(&s_held) && shorts==0); /* startup-held key released after init */
    reset_case();
    system_power_key_edge(true); ticks=5999; system_power_step(); assert(!ui_power_dialog_is_visible());
    ticks=6000; system_power_step(); assert(ui_power_dialog_is_visible());
    assert(!strcmp(lv_label_get_text(s_status),"正在关机，请选择操作"));
    lv_obj_update_layout(lv_layer_top());
    lv_point_t p={5,5}; assert(lv_indev_search_obj(lv_layer_top(),&p)==s_overlay);
    for(unsigned i=0;i<3;++i) {
        lv_area_t area; lv_obj_get_coords(s_buttons[i],&area);
        assert(area.x1>=0 && area.x2<460 && area.y1>=0 && area.y2<460);
        p=(lv_point_t){(area.x1+area.x2)/2,(area.y1+area.y2)/2};
        assert(lv_indev_search_obj(lv_layer_top(),&p)==s_buttons[i]);
    }
    lv_obj_send_event(s_buttons[2],LV_EVENT_CLICKED,NULL); system_power_step();
    assert(!ui_power_dialog_is_visible() && atomic_load(&s_held));
    ticks=9000; system_power_step(); assert(!ui_power_dialog_is_visible());
    system_power_key_edge(false); system_power_key_short(); assert(shorts==0);
    puts("PASS six-second threshold, modal hit testing, three buttons, cancel does not re-trigger while held");
    reset_case(); dark=true; unsigned wake_before=wakes;
    system_power_key_edge(true); assert(wakes==wake_before+1 && !dark);
    ticks=100; system_power_key_edge(false); system_power_key_short(); assert(shorts==0);
    system_power_key_edge(true); ticks=200; system_power_key_edge(false); system_power_key_short(); assert(shorts==1);
    system_power_key_short(); assert(shorts==1); /* duplicate/unpaired IRQ consumed */
    reset_case(); ticks=UINT32_MAX-3000; system_power_key_edge(true); ticks+=6000;
    system_power_key_edge(false); fail_ui_lock=true; system_power_step(); assert(atomic_load(&s_popup_requested));
    fail_ui_lock=false; system_power_step(); assert(ui_power_dialog_is_visible());
    puts("PASS dark press wakes once, normal short press remains, tick wrap/release and lock contention retain prompt");
    reset_case(); long_press(); media_idle=false;
    system_power_request(SYSTEM_POWER_OFF); system_power_step();
    assert(!saves && !off_calls && !atomic_load(&s_finalizing) && !media_paused);
    assert(!strcmp(lv_label_get_text(s_status),"操作失败，请重试或取消"));
    reset_case(); long_press(); save_result=ESP_FAIL;
    system_power_request(SYSTEM_POWER_RESTART); system_power_step();
    assert(saves==1 && !restarts && !off_calls && !dark && !media_paused);
    puts("PASS busy media timeout and NVS failure restore screen without powering off/restarting");
    reset_case(); long_press(); off_result=ESP_FAIL;
    system_power_request(SYSTEM_POWER_OFF); system_power_step();
    assert(saves==1 && off_calls==1 && !dark && !media_paused);
    reset_case(); long_press(); terminal_action=1;
    if (!setjmp(terminal)) { system_power_request(SYSTEM_POWER_OFF); system_power_step(); assert(0); }
    assert(saves==1 && off_calls==1 && !strcmp(saved.song_audio,"song.flac"));
    reset_case(); long_press(); mounted=false; terminal_action=2; /* no SD card must not prevent PMIC restart */
    if (!setjmp(terminal)) { system_power_request(SYSTEM_POWER_RESTART); system_power_step(); assert(0); }
    assert(saves==1 && restarts==1 && !off_calls);
    puts("PASS commit/readback precedes PMIC shutdown or PMIC restart; command failure recovers; no-SD restart works");
    reset_case(); long_press();
    lvgl_port_lock(0); ui_power_dialog_set_status("正在保存，即将关机",true); lvgl_port_unlock();
    for(unsigned i=0;i<3;++i) assert(lv_obj_has_state(s_buttons[i],LV_STATE_DISABLED));
    reset_case(); long_press(); restart_result=ESP_FAIL;
    system_power_request(SYSTEM_POWER_RESTART); system_power_step();
    assert(saves==1 && restarts==1 && !off_calls && !dark && !media_paused);
    reset_case(); long_press(); /* ACK without actual reset is reported as failure, never P4 fallback. */
    system_power_request(SYSTEM_POWER_RESTART); system_power_step();
    assert(saves==1 && restarts==1 && !off_calls && !dark && !atomic_load(&s_finalizing));
    puts("PASS startup/unpaired short events ignored, PMIC restart failure/no-reset ACK restores retry UI");
    puts("PASS terminal actions disable controls and original pages remain untouched");
    return 0;
}
