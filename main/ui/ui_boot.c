#include "ui_boot.h"

static lv_display_t *s_display;
static lv_obj_t *s_overlay;
static lv_obj_t *s_stage;
static lv_obj_t *s_bar;
static uint8_t s_progress;

void ui_boot_show(lv_display_t *display)
{
    if(s_overlay || !display) return;
    s_display=display;
    s_progress=0;

    /* 使用 top layer 的临时覆盖层，不切换/清空 active screen。
     * 后续 ui_manager 可以在下面创建原有页面，开机结束时仅删除本层；
     * 因而不会把原页面对象一起删除，也不会改动锁屏/播放器的坐标。
     * 不透明黑底沿用系统白色主调，避免显示正在构建的页面和状态栏。 */
    s_overlay=lv_obj_create(lv_display_get_layer_top(display));
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay,LV_PCT(100),LV_PCT(100));
    lv_obj_set_pos(s_overlay,0,0);
    lv_obj_set_style_bg_color(s_overlay,lv_color_black(),0);
    lv_obj_set_style_bg_opa(s_overlay,LV_OPA_COVER,0);
    lv_obj_add_flag(s_overlay,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_overlay,LV_OBJ_FLAG_SCROLLABLE |
        LV_OBJ_FLAG_GESTURE_BUBBLE | LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 字体显式指定为固件内置 Montserrat，不能调用 ui_font_*，否则
     * 尚未挂载的外部字体会反过来成为显示开机提示的前置条件。 */
    lv_obj_t *title=lv_label_create(s_overlay);
    lv_obj_set_style_text_font(title,&lv_font_montserrat_22,0);
    lv_obj_set_style_text_color(title,lv_color_white(),0);
    lv_label_set_text(title,"Starting...");
    lv_obj_align(title,LV_ALIGN_CENTER,0,-30);

    /* 只显示阶段进度，不显示百分比，也不做插值动画：某个字库内部
     * 没有进度回调，不能用时间插值伪装实际读取进度。每次更新均立即
     * 绘制，不依赖被同步字库加载阻塞的 LVGL 定时器。 */
    s_bar=lv_bar_create(s_overlay);
    lv_obj_remove_style_all(s_bar);
    lv_obj_set_size(s_bar,260,6);
    lv_obj_align(s_bar,LV_ALIGN_CENTER,0,10);
    lv_bar_set_range(s_bar,0,100);
    lv_obj_set_style_radius(s_bar,3,LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar,lv_color_white(),LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_bar,LV_OPA_20,LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar,3,LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_bar,lv_color_white(),LV_PART_INDICATOR);
    lv_obj_set_style_bg_opa(s_bar,LV_OPA_COVER,LV_PART_INDICATOR);
    lv_obj_remove_flag(s_bar,LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    s_stage=lv_label_create(s_overlay);
    lv_obj_set_width(s_stage,400);
    lv_obj_set_style_text_align(s_stage,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_font(s_stage,&lv_font_montserrat_14,0);
    lv_obj_set_style_text_color(s_stage,lv_color_white(),0);
    lv_obj_set_style_text_opa(s_stage,LV_OPA_70,0);
    lv_label_set_text(s_stage,"Initializing...");
    lv_obj_align(s_stage,LV_ALIGN_CENTER,0,42);

    /* app_main 随后可能长时间持有 LVGL 锁；仅创建对象不能保证
     * LVGL worker 已刷新首帧，必须在开始耗时加载前主动提交画面。 */
    lv_refr_now(s_display);
}

void ui_boot_set_progress(uint8_t progress, const char *stage)
{
    if(!s_overlay) return;
    if(progress>100) progress=100;
    if(progress>s_progress) s_progress=progress;
    lv_bar_set_value(s_bar,s_progress,LV_ANIM_OFF);
    if(stage) lv_label_set_text(s_stage,stage);
    lv_refr_now(s_display);
}

void ui_boot_hide(void)
{
    if(!s_overlay) return;
    lv_obj_delete(s_overlay);
    s_overlay=s_stage=s_bar=NULL;
    s_progress=0;
    lv_display_t *display=s_display;
    s_display=NULL;
    /* 不清空 top layer：其他模块的弹窗可以是本层的兄弟对象。 */
    lv_refr_now(display);
}
