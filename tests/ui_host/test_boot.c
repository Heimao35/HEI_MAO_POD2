/* Real boot-layer rendering before any external font is available. Only
 * SPIFFS/binary font I/O is replaced; the font wrapper and LVGL are production. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/ui/ui_boot.c"
static lv_font_t *fake_binfont_create(const char *path);
#define lv_binfont_create fake_binfont_create
#include "../../main/ui/ui_fonts.c"
#undef lv_binfont_create

static unsigned flushes,loads,notifications;
static bool mount_fail;
static uint16_t frame[460*460];
static lv_font_t fake_fonts[4];
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{
    ++flushes;
    unsigned width=(unsigned)(area->x2-area->x1+1);
    for(int y=area->y1;y<=area->y2;++y)
        memcpy(frame+y*460+area->x1,pixels+(y-area->y1)*width*2,width*2);
    lv_display_flush_ready(display);
}
static unsigned white_bar_pixels(void)
{
    unsigned count=0;
    for(unsigned x=100;x<360;++x) if(frame[240*460+x]==0xffff) ++count;
    return count;
}
esp_err_t esp_vfs_spiffs_register(const esp_vfs_spiffs_conf_t *conf)
{
    assert(flushes>0 && s_overlay); /* prompt actually rendered before mount */
    assert(!strcmp(conf->partition_label,"fontfs") && !conf->format_if_mount_failed);
    return mount_fail ? ESP_FAIL : ESP_OK;
}
esp_err_t esp_spiffs_info(const char *label,size_t *total,size_t *used)
{
    assert(!strcmp(label,"fontfs"));*total=2*1024*1024;*used=1200000;return ESP_OK;
}
static lv_font_t *fake_binfont_create(const char *path)
{
    assert(notifications==loads+1 && s_overlay && flushes>0);
    assert(strstr(lv_label_get_text(s_stage),path+2));
    /* A synchronous loader does not service timers. The prior frame and bar
     * stay present while it works; the callback submitted that frame first. */
    assert(frame[0]==0 && white_bar_pixels()>0);
    unsigned index=loads++;
    if(index==3) return NULL; /* missing optional font -> original fallback */
    fake_fonts[index]=lv_font_montserrat_18;
    return &fake_fonts[index];
}
static void font_progress(unsigned done,unsigned total,const char *file,void *user)
{
    assert(user==(void *)&loads && total==4 && done==loads);
    ++notifications;
    char status[64];
    if(file) snprintf(status,sizeof(status),"Loading %s...",file);
    else snprintf(status,sizeof(status),"Fonts ready");
    ui_boot_set_progress((uint8_t)(10+done*60/total),status);
}
int main(void)
{
    lv_init();
    lv_display_t *display=lv_display_create(460,460);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    static uint8_t pixels[460*20*2];
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    lv_obj_t *screen=lv_screen_active();
    lv_obj_set_style_bg_color(screen,lv_color_make(0,255,0),0);
    lv_obj_set_style_bg_opa(screen,LV_OPA_COVER,0);
    lv_obj_t *underlying=lv_obj_create(screen);
    lv_obj_set_pos(underlying,50,50);
    lv_obj_set_size(underlying,100,100);
    lv_obj_update_layout(screen);
    lv_area_t before;lv_obj_get_coords(underlying,&before);
    lv_obj_t *top=lv_display_get_layer_top(display);
    lv_obj_t *sibling=lv_obj_create(top);lv_obj_set_size(sibling,1,1);
    uint32_t top_count=lv_obj_get_child_count(top);
    ui_boot_show(display);
    assert(flushes>0 && frame[0]==0 && s_overlay && s_stage && s_bar);
    assert(lv_obj_get_child_count(top)==top_count+1);
    lv_obj_t *title=lv_obj_get_child(s_overlay,0);
    assert(!strcmp(lv_label_get_text(title),"Starting..."));
    assert(lv_obj_get_style_text_font(title,0)==&lv_font_montserrat_22);
    assert(lv_obj_get_style_text_font(s_stage,0)==&lv_font_montserrat_14);
    assert(!lv_obj_has_flag(s_overlay,LV_OBJ_FLAG_GESTURE_BUBBLE));
    lv_point_t point={80,80};assert(lv_indev_search_obj(top,&point)==s_overlay);
    ui_boot_set_progress(20,"Loading settings...");
    unsigned initial_bar=white_bar_pixels();assert(initial_bar>0);
    ui_boot_set_progress(65,"Preparing animation...");
    assert(white_bar_pixels()>initial_bar);
    ui_boot_set_progress(10,NULL);assert(lv_bar_get_value(s_bar)==65);
    ui_boot_hide();
    assert(!s_overlay && !s_stage && !s_bar && !s_display);
    assert(lv_screen_active()==screen && lv_obj_get_child_count(top)==top_count);
    lv_area_t after;lv_obj_get_coords(underlying,&after);assert(!memcmp(&before,&after,sizeof(before)));
    assert(lv_indev_search_obj(top,&point)==NULL);
    assert(lv_obj_is_valid(sibling));
    puts("PASS immediate built-in-font boot frame, rendered stage bar, touch interception and normal-page/sibling preservation");

    ui_boot_show(display);ui_boot_set_progress(10,"Mounting fonts...");
    assert(ui_fonts_init_with_progress(font_progress,&loads)==ESP_OK);
    assert(loads==4 && notifications==5 && lv_bar_get_value(s_bar)==70);
    assert(s_font_22==&lv_font_montserrat_22);
    assert(s_font_18==&fake_fonts[2] && s_font_18->fallback==&lv_font_montserrat_18);
    ui_boot_set_progress(100,"Ready");assert(lv_bar_get_value(s_bar)==100);
    ui_boot_hide();
    puts("PASS synchronous font-stage callbacks render before loads and keep original built-in fallback on a missing font");

    s_initialized=false;mount_fail=true;
    ui_boot_show(display);ui_boot_set_progress(10,"Mounting fonts...");
    assert(ui_fonts_init_with_progress(font_progress,&loads)==ESP_FAIL);
    assert(loads==4 && notifications==5 && s_overlay);
    ui_boot_set_progress(100,"Ready");ui_boot_hide();ui_boot_hide();
    assert(lv_obj_get_child_count(top)==top_count && lv_obj_is_valid(sibling));
    lv_display_delete(display);
    puts("PASS failed mount still permits boot-layer completion and clean teardown");
    return 0;
}
