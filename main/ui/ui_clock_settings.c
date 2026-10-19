/**
 * @file ui_clock_settings.c
 * @brief RTC 时钟设置子页：三组吸附滚轮、右侧渐慢进出、退出自动保存。
 *
 * 维护约定：设置主页仍在下层保持原位置，不改其顶部展开/收起手势。
 * 此页不冒泡页面手势，滚轮的上下拖动只改变当前字段。所有 LVGL 访问
 * 由调用方/定时器在 LVGL 锁内执行；RTC 接口只复制 RAM，绝不在这里等 I2C。
 */
#include "ui_clock_settings.h"
#include "ui_fonts.h"
#include "rtc_clock.h"
#include <stdio.h>

/* 三行采用固定且居中的区域；仅新建子页使用这些坐标，不牵动原设置页、
 * 状态栏、锁屏时间等已有位置。每个滚轮显示三项，中项为当前吸附选择。 */
#define ROW_W 400
#define ROW_H 118
#define ROW_FIRST_Y 66
#define ROW_GAP 120
static lv_obj_t *s_clock_page, *s_back, *s_hint;
static lv_obj_t *s_hour, *s_minute, *s_year, *s_month, *s_day, *s_weekday;
static lv_timer_t *s_timer;
static bool s_clock_visible, s_loading, s_dirty, s_saving, s_lock_close;
static uint32_t s_save_id;

static void anim_x(void *obj,int32_t x) { lv_obj_set_x(obj,x); }
static void hidden(lv_anim_t *anim)
{
    (void)anim;
    lv_obj_add_flag(s_clock_page,LV_OBJ_FLAG_HIDDEN);
    s_clock_visible=false;
    if(!s_saving) lv_timer_pause(s_timer);
}
static void slide_out(void)
{
    /* 从当前位置启动，快速返回/电源锁屏打断滑入时也不会跳到 x=0。
     * 这里只隐藏子页，不改状态栏：设置主页负责其原有的显隐生命周期。 */
    lv_anim_delete(s_clock_page,anim_x);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a,s_clock_page);
    lv_anim_set_values(&a,lv_obj_get_x(s_clock_page),UI_HOR_RES);
    lv_anim_set_duration(&a,UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a,UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a,anim_x);
    lv_anim_set_completed_cb(&a,hidden);
    lv_anim_start(&a);
}
static void set_enabled(bool enabled)
{
    lv_obj_t *objects[]={s_hour,s_minute,s_year,s_month,s_day,s_weekday,s_back};
    for(unsigned i=0;i<sizeof(objects)/sizeof(objects[0]);++i) {
        if(enabled) lv_obj_remove_state(objects[i],LV_STATE_DISABLED);
        else lv_obj_add_state(objects[i],LV_STATE_DISABLED);
    }
}
static void options(lv_obj_t *roller,unsigned first,unsigned last,unsigned digits)
{
    /* 最大列表为 100 年（500 字节以内），不按文件/曲库大小分配内存。
     * NORMAL 模式避免无限滚轮为循环显示复制多份文本。 */
    char text[512];size_t used=0;
    for(unsigned n=first;n<=last;++n)
        used+=(size_t)snprintf(text+used,sizeof(text)-used,"%0*u%s",digits,n,n==last?"":"\n");
    lv_roller_set_options(roller,text,LV_ROLLER_MODE_NORMAL);
}
static void clamp_day(void)
{
    unsigned selected=lv_roller_get_selected(s_day);
    unsigned year=2000+lv_roller_get_selected(s_year);
    unsigned month=1+lv_roller_get_selected(s_month);
    unsigned days=rtc_clock_days_in_month(year,month);
    /* 年/月改变后重新生成有效日列表。保留原日；跨月不存在时夹到月末，
     * 禁止把 2 月 31 日等无效值交给 RTC，也不让程序触发事件标为用户修改。 */
    s_loading=true;
    options(s_day,1,days,2);
    lv_roller_set_selected(s_day,selected<days?selected:days-1,LV_ANIM_OFF);
    s_loading=false;
}
static void changed(lv_event_t *event)
{
    if(s_loading || s_saving) return;
    lv_obj_t *target=lv_event_get_target(event);
    if(target==s_year || target==s_month) clamp_day();
    /* 星期是芯片独立计数器，按需求可单独选择，不用日期计算值覆盖它。 */
    s_dirty=true;
    lv_label_set_text(s_hint,"返回时自动保存");
}
static rtc_datetime_t selected_time(void)
{
    return (rtc_datetime_t){.year=2000+lv_roller_get_selected(s_year),
        .month=1+lv_roller_get_selected(s_month),.day=1+lv_roller_get_selected(s_day),
        .hour=lv_roller_get_selected(s_hour),.minute=lv_roller_get_selected(s_minute),
        .weekday=(lv_roller_get_selected(s_weekday)+1)%7,.second=0};
}
static bool save_edits(void)
{
    if(s_saving) return true;
    /* 无改动直接返回，保留 RTC 秒数。未校准时的编辑默认值也不能因为
     * 用户只是查看后返回，就悄悄写入并冒充真实时间。 */
    if(!s_dirty) return false;
    rtc_datetime_t time=selected_time();
    esp_err_t ret=rtc_clock_request_set(&time,&s_save_id);
    if(ret!=ESP_OK) {
        lv_label_set_text(s_hint,"保存失败，请重试");
        return false;
    }
    s_saving=true;
    set_enabled(false);
    lv_label_set_text(s_hint,"正在保存...");
    lv_timer_resume(s_timer);
    return true;
}
static void back_clicked(lv_event_t *event)
{
    (void)event;
    if(s_saving) return;
    if(!s_dirty) { slide_out();return; }
    /* 普通返回必须等硬件 ACK+读回核对，失败留在原页可重试；不能在
     * 触摸回调里阻塞，也不能提前改公共时间使用户误以为已经保存。 */
    (void)save_edits();
}
void ui_clock_settings_update(void)
{
    rtc_clock_snapshot_t snapshot;
    rtc_clock_get_snapshot(&snapshot);
    ui_device_state_t *st=ui_get_device_state();
    st->time_valid=snapshot.valid;
    if(snapshot.valid) {
        st->year=snapshot.time.year;st->month=snapshot.time.month;st->day=snapshot.time.day;
        st->hour=snapshot.time.hour;st->minute=snapshot.time.minute;
        st->weekday=snapshot.time.weekday;
    }
    if(s_saving && snapshot.completed_id==s_save_id) {
        s_saving=false;set_enabled(true);
        if(snapshot.set_result==ESP_OK) {
            s_dirty=false;
            lv_label_set_text(s_hint,"返回时自动保存");
            if(s_clock_visible && !s_lock_close) slide_out();
        } else lv_label_set_text(s_hint,"保存失败，请重试");
        if(!s_clock_visible) lv_timer_pause(s_timer);
    }
}
static void clock_timer(lv_timer_t *timer)
{
    (void)timer;
    /* 页面内 100ms 仅查 RAM/完成状态，不做 I2C、不逐秒改滚轮；用户
     * 正在选的值不受后台真实时间推进覆盖。隐藏且保存结束后暂停此定时器。 */
    ui_clock_settings_update();
}
static lv_obj_t *row(lv_obj_t *parent,int y)
{
    lv_obj_t *obj=lv_obj_create(parent);
    lv_obj_set_size(obj,ROW_W,ROW_H);
    lv_obj_set_pos(obj,(UI_HOR_RES-ROW_W)/2,y);
    lv_obj_set_style_bg_opa(obj,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(obj,0,0);
    lv_obj_set_style_pad_all(obj,0,0);
    lv_obj_remove_flag(obj,LV_OBJ_FLAG_SCROLLABLE);
    return obj;
}
static void caption(lv_obj_t *parent,const char *text,int center_x,int y)
{
    lv_obj_t *label=lv_label_create(parent);
    lv_label_set_text(label,text);
    lv_obj_set_style_text_font(label,ui_font_16(),0);
    lv_obj_set_style_text_color(label,UI_COLOR_TEXT_SECONDARY,0);
    lv_obj_set_style_text_align(label,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_width(label,50);
    lv_obj_set_pos(label,center_x-25,y);
}
static lv_obj_t *roller(lv_obj_t *parent,int x,int width,bool chinese)
{
    lv_obj_t *obj=lv_roller_create(parent);
    lv_obj_set_width(obj,width);
    lv_obj_set_pos(obj,x,20);
    lv_obj_set_style_bg_opa(obj,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(obj,0,0);
    lv_obj_set_style_pad_all(obj,0,0);
    lv_obj_set_style_text_font(obj,chinese?ui_font_22():&lv_font_montserrat_26,0);
    lv_obj_set_style_text_font(obj,chinese?ui_font_22():&lv_font_montserrat_26,LV_PART_SELECTED);
    lv_obj_set_style_text_line_space(obj,chinese?8:4,0);
    lv_obj_set_style_text_color(obj,UI_COLOR_TEXT_MUTED,0);
    lv_obj_set_style_bg_color(obj,UI_COLOR_TEXT,LV_PART_SELECTED);
    lv_obj_set_style_bg_opa(obj,LV_OPA_20,LV_PART_SELECTED);
    lv_obj_set_style_text_color(obj,UI_COLOR_TEXT,LV_PART_SELECTED);
    lv_obj_set_style_radius(obj,8,LV_PART_SELECTED);
    lv_obj_set_style_border_width(obj,0,LV_PART_SELECTED);
    lv_roller_set_visible_row_count(obj,3);
    /* LVGL roller 自带竖直滚动及 release 后吸附；关闭链式/手势冒泡，
     * 防止滚到边缘后触发下层设置页收起、歌词滚动或歌曲切换。 */
    lv_obj_remove_flag(obj,LV_OBJ_FLAG_EVENT_BUBBLE|LV_OBJ_FLAG_GESTURE_BUBBLE|
        LV_OBJ_FLAG_SCROLL_CHAIN_VER|LV_OBJ_FLAG_SCROLL_CHAIN_HOR);
    lv_obj_add_event_cb(obj,changed,LV_EVENT_VALUE_CHANGED,NULL);
    return obj;
}
void ui_clock_settings_create(lv_obj_t *parent)
{
    s_loading=true;
    s_clock_page=lv_obj_create(parent);
    lv_obj_set_size(s_clock_page,UI_HOR_RES,UI_VER_RES);
    lv_obj_set_pos(s_clock_page,UI_HOR_RES,0);
    lv_obj_set_style_bg_color(s_clock_page,UI_COLOR_PAGE_BG_DARK,0);
    lv_obj_set_style_bg_opa(s_clock_page,LV_OPA_COVER,0);
    lv_obj_set_style_border_width(s_clock_page,0,0);
    lv_obj_set_style_radius(s_clock_page,0,0);
    lv_obj_set_style_pad_all(s_clock_page,0,0);
    lv_obj_remove_flag(s_clock_page,LV_OBJ_FLAG_SCROLLABLE|LV_OBJ_FLAG_EVENT_BUBBLE|
        LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_clock_page,LV_OBJ_FLAG_HIDDEN);
    lv_obj_t *title=lv_label_create(s_clock_page);
    lv_label_set_text(title,"时钟设置");
    lv_obj_set_style_text_font(title,ui_font_22(),0);
    lv_obj_set_style_text_color(title,UI_COLOR_TEXT,0);
    lv_obj_align(title,LV_ALIGN_TOP_MID,0,16);
    /* 返回按钮只画 LVGL 自带箭头，不依赖中文字库的图标收录。44px 的
     * 触摸目标位于左上角，与标题分离；只处理 CLICKED，不占用滚轮拖动。 */
    s_back=lv_button_create(s_clock_page);
    lv_obj_set_size(s_back,44,44);lv_obj_set_pos(s_back,12,8);
    lv_obj_set_style_bg_opa(s_back,LV_OPA_TRANSP,0);
    lv_obj_set_style_border_width(s_back,0,0);
    lv_obj_set_style_shadow_width(s_back,0,0);
    lv_obj_t *arrow=lv_label_create(s_back);
    lv_label_set_text(arrow,LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow,&lv_font_montserrat_26,0);
    lv_obj_set_style_text_color(arrow,UI_COLOR_TEXT,0);lv_obj_center(arrow);
    lv_obj_add_event_cb(s_back,back_clicked,LV_EVENT_CLICKED,NULL);

    lv_obj_t *time=row(s_clock_page,ROW_FIRST_Y);
    caption(time,"时",135,0);caption(time,"分",265,0);
    s_hour=roller(time,85,100,false);s_minute=roller(time,215,100,false);
    lv_obj_t *colon=lv_label_create(time);lv_label_set_text(colon,":");
    lv_obj_set_style_text_font(colon,&lv_font_montserrat_26,0);
    lv_obj_set_style_text_color(colon,UI_COLOR_TEXT,0);
    lv_obj_set_pos(colon,195,54);
    options(s_hour,0,23,2);options(s_minute,0,59,2);

    lv_obj_t *date=row(s_clock_page,ROW_FIRST_Y+ROW_GAP);
    caption(date,"年",85,0);caption(date,"月",212,0);caption(date,"日",327,0);
    s_year=roller(date,20,130,false);s_month=roller(date,174,76,false);s_day=roller(date,289,76,false);
    options(s_year,2000,2099,4);options(s_month,1,12,2);options(s_day,1,31,2);

    lv_obj_t *week=row(s_clock_page,ROW_FIRST_Y+2*ROW_GAP);
    /* 周字固定，仅“一..天”滚动；UI 顺序为周一至周天，写入时再转换
     * 芯片的 0=周日编码。星期独立于日期，以符合用户可单独设置的要求。 */
    caption(week,"周",150,58);
    s_weekday=roller(week,185,96,true);
    lv_roller_set_options(s_weekday,"一\n二\n三\n四\n五\n六\n天",LV_ROLLER_MODE_NORMAL);

    s_hint=lv_label_create(s_clock_page);
    lv_obj_set_style_text_font(s_hint,ui_font_16(),0);
    lv_obj_set_style_text_color(s_hint,UI_COLOR_TEXT_CAPTION,0);
    lv_label_set_text(s_hint,"返回时自动保存");
    lv_obj_align(s_hint,LV_ALIGN_BOTTOM_MID,0,-8);
    s_timer=lv_timer_create(clock_timer,100,NULL);lv_timer_pause(s_timer);
    s_loading=false;
}
void ui_clock_settings_show(void)
{
    if(!s_clock_page || s_clock_visible) return;
    rtc_clock_snapshot_t snapshot;rtc_clock_get_snapshot(&snapshot);
    s_loading=true;s_lock_close=false;
    if(!s_saving && !s_dirty) {
        /* 首次无效时只给编辑器提供占位起点，公共时间仍是未知；不会
         * 自动写默认日期，更不能把编译日期当成用户的真实时间。 */
        rtc_datetime_t t=snapshot.valid?snapshot.time:
            (rtc_datetime_t){.year=2026,.month=1,.day=1,.weekday=4};
        lv_roller_set_selected(s_hour,t.hour,LV_ANIM_OFF);
        lv_roller_set_selected(s_minute,t.minute,LV_ANIM_OFF);
        lv_roller_set_selected(s_year,t.year-2000,LV_ANIM_OFF);
        lv_roller_set_selected(s_month,t.month-1,LV_ANIM_OFF);
        options(s_day,1,rtc_clock_days_in_month(t.year,t.month),2);
        lv_roller_set_selected(s_day,t.day-1,LV_ANIM_OFF);
        lv_roller_set_selected(s_weekday,(t.weekday+6)%7,LV_ANIM_OFF);
        lv_label_set_text(s_hint,snapshot.valid?"返回时自动保存":
            (snapshot.online?"未校准，请设置时间":"时钟未就绪"));
    }
    s_loading=false;
    lv_anim_delete(s_clock_page,anim_x);
    lv_obj_set_x(s_clock_page,UI_HOR_RES);
    lv_obj_remove_flag(s_clock_page,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_clock_page);
    s_clock_visible=true;lv_timer_resume(s_timer);
    lv_anim_t a;lv_anim_init(&a);lv_anim_set_var(&a,s_clock_page);
    lv_anim_set_values(&a,UI_HOR_RES,0);lv_anim_set_duration(&a,UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a,UI_ANIM_PATH);lv_anim_set_exec_cb(&a,anim_x);lv_anim_start(&a);
}
bool ui_clock_settings_is_visible(void) { return s_clock_visible; }
void ui_clock_settings_close_for_lock(void)
{
    if(!s_clock_page || !s_clock_visible) return;
    s_lock_close=true;
    (void)save_edits();
    slide_out();
}
