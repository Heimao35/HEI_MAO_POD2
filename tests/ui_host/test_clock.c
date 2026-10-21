/* Production RTC and settings UI; real LVGL, byte-accurate PCF85063TP model.
 * Hardware doubles reject I2C in UI/ISR, model OS, STOP, 12h and write errors. */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/rtc_clock.c"
#include "../../main/ui/ui_common.c"
#include "../../main/ui/ui_clock_settings.c"
#include "../../main/ui/ui_settings.c"

static uint8_t regfile[11];
static TickType_t ticks;
static bool under_ui,in_isr,device_added,fail_read,drop_calendar,rtc_irq_added;
static unsigned transactions,write_calls,fail_write_call,notify_count,isr_notify_count;
static void (*irq_function)(void *);
static unsigned last_write_reg[32],last_write_value[32],recorded;
static bool status_visible=true;
const lv_font_t *ui_font_14(void) { return &lv_font_montserrat_14; }
const lv_font_t *ui_font_16(void) { return &lv_font_montserrat_16; }
const lv_font_t *ui_font_18(void) { return &lv_font_montserrat_18; }
const lv_font_t *ui_font_22(void) { return &lv_font_montserrat_22; }
void ui_statusbar_set_visible(bool visible) { status_visible=visible; }
/* Settings-page regression includes the real ui_settings.c; the motion page is
 * out of scope here, so its entry point is a no-op double. */
void ui_motion_settings_show(void) {}
void pmic_axp2101_notify_service(void) { assert(!in_isr);++notify_count; }
void pmic_axp2101_notify_service_from_isr(void) { assert(in_isr);++isr_notify_count; }
TickType_t xTaskGetTickCount(void) { return ticks; }
esp_err_t i2c_master_get_bus_handle(int port,i2c_master_bus_handle_t *bus)
{ assert(!under_ui && port==1);*bus=(void *)1;return ESP_OK; }
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t bus,
    const i2c_device_config_t *cfg,i2c_master_dev_handle_t *dev)
{
    assert(!under_ui && bus==(void *)1 && !device_added);
    assert(cfg->device_address==0x51 && cfg->scl_speed_hz==100000);
    device_added=true;*dev=(void *)3;return ESP_OK;
}
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t dev)
{ assert(!under_ui && dev==(void *)3 && device_added);device_added=false;return ESP_OK; }
esp_err_t gpio_config(const gpio_config_t *cfg)
{ assert(cfg->pin_bit_mask==(1ULL<<11) && cfg->intr_type==GPIO_INTR_NEGEDGE);return ESP_OK; }
esp_err_t gpio_isr_handler_add(gpio_num_t pin,void (*handler)(void *),void *arg)
{ assert(pin==11 && !arg);irq_function=handler;rtc_irq_added=true;return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t pin)
{ assert(pin==11);rtc_irq_added=false;return ESP_OK; }
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t dev,const uint8_t *tx,
    size_t tx_size,uint8_t *rx,size_t rx_size,int timeout)
{
    assert(!under_ui && !in_isr && dev==(void *)3 && tx_size==1 && timeout==100);
    assert(*tx+rx_size<=sizeof(regfile));++transactions;
    if(fail_read) return ESP_ERR_TIMEOUT;
    memcpy(rx,regfile+*tx,rx_size);return ESP_OK;
}
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t dev,const uint8_t *tx,size_t size,int timeout)
{
    assert(!under_ui && !in_isr && dev==(void *)3 && size>=2 && timeout==100);
    assert(tx[0]+size-1<=sizeof(regfile));++transactions;++write_calls;
    if(recorded<32) {last_write_reg[recorded]=tx[0];last_write_value[recorded++]=tx[1];}
    if(write_calls==fail_write_call) return ESP_ERR_TIMEOUT;
    if(drop_calendar && tx[0]==4) return ESP_OK;
    memcpy(regfile+tx[0],tx+1,size-1);return ESP_OK;
}
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;
static void read_pointer(lv_indev_t *indev,lv_indev_data_t *data)
{ (void)indev;data->point=pointer_point;data->state=pointer_state; }
static void flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{ (void)area;(void)pixels;lv_display_flush_ready(display); }
static void advance(unsigned ms)
{
    for(unsigned n=0;n<ms;n+=10) {ticks+=10;lv_tick_inc(10);lv_timer_handler();}
    lv_obj_update_layout(lv_screen_active());
}
static void input(lv_indev_t *indev,int x,int y,lv_indev_state_t state)
{
    pointer_point=(lv_point_t){x,y};pointer_state=state;
    under_ui=true;lv_indev_read(indev);under_ui=false;advance(20);
}
static void drag(lv_indev_t *indev,int x,int y,int dy)
{
    input(indev,x,y,LV_INDEV_STATE_PRESSED);
    for(int i=1;i<=6;++i) input(indev,x,y+dy*i/6,LV_INDEV_STATE_PRESSED);
    input(indev,x,y+dy,LV_INDEV_STATE_RELEASED);advance(500);
}
static void read_again(void) { ticks+=1000;rtc_clock_service(); }
static void choose(lv_obj_t *roller,unsigned index)
{
    under_ui=true;
    lv_roller_set_selected(roller,index,LV_ANIM_OFF);
    lv_obj_send_event(roller,LV_EVENT_VALUE_CHANGED,NULL);
    under_ui=false;
}
static rtc_clock_snapshot_t snapshot(void)
{ rtc_clock_snapshot_t value;rtc_clock_get_snapshot(&value);return value; }
static void model_time(rtc_datetime_t time)
{
    regfile[0]=1; /* Preserve 12.5pF CAP_SEL. */
    regfile[4]=bcd(time.second);regfile[5]=bcd(time.minute);regfile[6]=bcd(time.hour);
    regfile[7]=bcd(time.day);regfile[8]=time.weekday;
    regfile[9]=bcd(time.month);regfile[10]=bcd(time.year-2000);
}
static void write_time(rtc_datetime_t time)
{
    unsigned before=transactions;uint32_t id;
    under_ui=true;assert(rtc_clock_request_set(&time,&id)==ESP_OK);under_ui=false;
    assert(transactions==before && snapshot().set_pending);
    assert(rtc_clock_request_set(&time,&id)==ESP_ERR_INVALID_STATE);
    rtc_clock_service();
    assert(!snapshot().set_pending && snapshot().set_result==ESP_OK && snapshot().valid);
}
int main(void)
{
    assert(rtc_clock_days_in_month(2000,2)==29 && rtc_clock_days_in_month(2100,2)==28);
    rtc_datetime_t t={.year=2024,.month=2,.day=29,.weekday=4,.hour=23,.minute=59,.second=59};
    assert(rtc_clock_datetime_valid(&t));t.year=2023;assert(!rtc_clock_datetime_valid(&t));t.year=2024;
    uint32_t id;assert(rtc_clock_request_set(&t,&id)==ESP_ERR_INVALID_STATE);
    assert(rtc_clock_init()==ESP_OK && device_added && rtc_irq_added);
    assert(rtc_clock_init()==ESP_ERR_INVALID_STATE);
    model_time(t);regfile[1]=0x0a; /* preserve CLKOUT=2, clear TF, enable MI */
    rtc_clock_service();assert(snapshot().valid && snapshot().time.second==59);
    assert(regfile[0]==1 && regfile[1]==0x22);
    unsigned accesses=transactions;rtc_clock_service();assert(accesses==transactions);
    in_isr=true;irq_function(NULL);in_isr=false;
    assert(isr_notify_count==1 && accesses==transactions);
    regfile[4]|=0x80;read_again();assert(!snapshot().valid && snapshot().online);
    assert(regfile[4]&0x80); /* Reading does not mask oscillator loss. */
    model_time(t);regfile[0]|=0x20;read_again();assert(!snapshot().valid);
    model_time(t);regfile[5]=0x5a;read_again();assert(!snapshot().valid);
    model_time(t);regfile[7]=0x30;read_again();assert(!snapshot().valid); /* Feb30 */
    model_time(t);regfile[8]=7;read_again();assert(!snapshot().valid);
    model_time(t);regfile[0]|=2;regfile[6]=0x32;read_again();
    assert(snapshot().valid && snapshot().time.hour==12); /* 12 PM */
    regfile[6]=0x12;read_again();assert(snapshot().valid && snapshot().time.hour==0);
    regfile[6]=0x21;read_again();assert(snapshot().time.hour==13);
    fail_read=true;read_again();assert(!snapshot().online && !snapshot().valid);fail_read=false;
    recorded=0;write_time(t);
    assert(regfile[0]==1 && regfile[4]==0x59 && regfile[6]==0x23 && regfile[10]==0x24);
    assert(last_write_reg[0]==0 && (last_write_value[0]&0x20));
    assert(last_write_reg[1]==4 && last_write_reg[2]==0 && !(last_write_value[2]&0x20));
    t.day=28;t.hour=8;t.second=0;
    fail_write_call=write_calls+2; /* fail the calendar burst, resume still required */
    assert(rtc_clock_request_set(&t,&id)==ESP_OK);rtc_clock_service();
    assert(snapshot().set_result==ESP_ERR_TIMEOUT && !snapshot().valid && !(regfile[0]&0x20));
    fail_write_call=0;read_again();assert(!snapshot().valid); /* partial set stays unknown */
    write_time(t);assert(snapshot().valid);
    fail_write_call=write_calls+3; /* failed resume, next save must recover STOP */
    assert(rtc_clock_request_set(&t,&id)==ESP_OK);rtc_clock_service();
    assert(snapshot().set_result==ESP_ERR_TIMEOUT && (regfile[0]&0x20));
    fail_write_call=0;write_time(t);assert(!(regfile[0]&0x20));
    t.hour=9;drop_calendar=true;
    assert(rtc_clock_request_set(&t,&id)==ESP_OK);rtc_clock_service();
    assert(snapshot().set_result==ESP_ERR_INVALID_RESPONSE && !snapshot().valid);
    drop_calendar=false;write_time(t);
    s_configured=false;s_config_once=false;regfile[2]=0x80;regfile[1]=0x37;
    read_again();assert(regfile[1]==7 && regfile[2]==0x80); /* no MODE=1 minute IRQ */

    lv_init();lv_display_t *display=lv_display_create(460,460);
    static uint8_t pixels[460*20*2];
    lv_display_set_buffers(display,pixels,NULL,sizeof(pixels),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    lv_obj_remove_flag(lv_screen_active(),LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_border_width(lv_screen_active(),0,0);
    lv_obj_set_style_pad_all(lv_screen_active(),0,0);
    ui_settings_create(lv_screen_active());ui_clock_settings_create(lv_screen_active());
    lv_indev_t *pointer=lv_indev_create();lv_indev_set_type(pointer,LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer,read_pointer);
    lv_indev_set_mode(pointer,LV_INDEV_MODE_EVENT);
    ui_settings_show();advance(400);assert(!status_visible && ui_settings_is_visible());
    /* Enter through actual settings clock button, rather than only invoking
     * the page API. This also checks its dial children do not steal clicks. */
    lv_obj_t *grid=lv_obj_get_child(s_page,1);
    lv_obj_t *clock_button=lv_obj_get_child(lv_obj_get_child(grid,3),0);
    lv_obj_scroll_to_view(clock_button,LV_ANIM_OFF);advance(40);
    lv_area_t area;lv_obj_get_coords(clock_button,&area);
    int bx=(area.x1+area.x2)/2,by=(area.y1+area.y2)/2;
    input(pointer,bx,by,LV_INDEV_STATE_PRESSED);input(pointer,bx,by,LV_INDEV_STATE_RELEASED);
    assert(ui_clock_settings_is_visible());advance(400);
    assert(lv_obj_get_x(s_clock_page)==0 && !status_visible);
    assert(lv_roller_get_selected(s_hour)==9 && lv_roller_get_selected(s_year)==24);
    assert(lv_roller_get_option_count(s_day)==29 && lv_roller_get_selected(s_weekday)==3);
    unsigned before=transactions;
    under_ui=true;back_clicked(NULL);under_ui=false;advance(400);
    assert(!ui_clock_settings_is_visible() && transactions==before); /* unchanged does not reset seconds */
    ui_clock_settings_show();advance(400);
    /* Verify all roller viewports stay within their rows and above hint,
     * and real pointer drag changes selection then settles on one option. */
    lv_obj_t *rollers[]={s_hour,s_minute,s_year,s_month,s_day,s_weekday};
    for(unsigned n=0;n<6;++n) {
        lv_area_t r;lv_obj_get_coords(rollers[n],&r);
        assert(r.x1>=0 && r.x2<460 && r.y1>=60 && r.y2<430);
    }
    lv_obj_get_coords(s_minute,&area);
    unsigned prior=lv_roller_get_selected(s_minute);
    drag(pointer,(area.x1+area.x2)/2,(area.y1+area.y2)/2,30);
    assert(lv_roller_get_selected(s_minute)!=prior && s_dirty && ui_clock_settings_is_visible());
    choose(s_day,28);choose(s_year,23);assert(lv_roller_get_selected(s_day)==27);
    assert(lv_roller_get_option_count(s_day)==28); /* leap to nonleap clamps */
    choose(s_month,3);choose(s_day,29);choose(s_month,1);
    assert(lv_roller_get_selected(s_day)==27);
    choose(s_weekday,6);choose(s_hour,17);choose(s_minute,42);
    before=transactions;
    under_ui=true;back_clicked(NULL);under_ui=false;
    assert(transactions==before && s_saving && ui_clock_settings_is_visible());
    assert(lv_obj_has_state(s_hour,LV_STATE_DISABLED));
    rtc_clock_service();under_ui=true;ui_clock_settings_update();under_ui=false;advance(400);
    assert(!s_saving && !ui_clock_settings_is_visible());
    assert(snapshot().time.hour==17 && snapshot().time.minute==42 && snapshot().time.weekday==0);
    assert(ui_get_device_state()->time_valid && ui_get_device_state()->hour==17);
    ui_clock_settings_show();advance(400);choose(s_hour,18);
    under_ui=true;back_clicked(NULL);under_ui=false;
    fail_read=true;rtc_clock_service();fail_read=false;ui_clock_settings_update();
    assert(!s_saving && s_dirty && ui_clock_settings_is_visible());
    assert(!strcmp(lv_label_get_text(s_hint),"保存失败，请重试"));
    under_ui=true;back_clicked(NULL);under_ui=false;rtc_clock_service();ui_clock_settings_update();advance(400);
    assert(!ui_clock_settings_is_visible() && snapshot().time.hour==18);
    ui_clock_settings_show();advance(400);choose(s_minute,11);
    under_ui=true;ui_clock_settings_close_for_lock();under_ui=false;advance(400);
    assert(!ui_clock_settings_is_visible() && s_saving);
    rtc_clock_service();ui_clock_settings_update();
    assert(!s_saving && snapshot().time.minute==11);
    regfile[4]|=0x80;read_again();ui_clock_settings_update();
    assert(!ui_get_device_state()->time_valid);
    ui_clock_settings_show();advance(400);assert(!strcmp(lv_label_get_text(s_hint),"未校准，请设置时间"));
    before=transactions;back_clicked(NULL);advance(400);assert(transactions==before && !s_saving);
    rtc_clock_deinit();assert(!device_added && !rtc_irq_added);
    puts("RTC calendar/BCD/OS/STOP/12h/failure/readback and real LVGL clock settings passed");
    return 0;
}
