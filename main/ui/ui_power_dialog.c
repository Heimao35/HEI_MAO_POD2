#include "ui_power_dialog.h"
#include "ui_fonts.h"

static lv_obj_t *s_overlay, *s_status, *s_buttons[3];

static void action_cb(lv_event_t *event)
{
    /* 点击只投递枚举；NVS 提交、等待 SD/JPEG 和 PMIC I2C 全部在电源任务，
     * 不能堵住 LVGL。由任务统一处理重复点击和终态切换。 */
    system_power_request((system_power_action_t)(uintptr_t)lv_event_get_user_data(event));
}
static void create_dialog(void)
{
    /* 独立顶层全屏命中区：弹窗打开期间拦截下层触摸，取消后删除遮罩，
     * 原页面坐标、手势、封面与歌词动画完全沿用。无全屏模糊或额外帧缓冲。 */
    s_overlay=lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(s_overlay);
    lv_obj_set_size(s_overlay,UI_HOR_RES,UI_VER_RES);
    lv_obj_set_style_bg_color(s_overlay,lv_color_black(),0);
    lv_obj_set_style_bg_opa(s_overlay,LV_OPA_60,0);
    lv_obj_add_flag(s_overlay,LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(s_overlay,LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_t *card=lv_obj_create(s_overlay);
    lv_obj_set_size(card,382,204);
    lv_obj_center(card);
    lv_obj_remove_flag(card,LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_set_style_bg_color(card,lv_color_hex(0x242424),0);
    lv_obj_set_style_bg_opa(card,LV_OPA_COVER,0);
    lv_obj_set_style_border_color(card,lv_color_white(),0);
    lv_obj_set_style_border_opa(card,LV_OPA_30,0);
    lv_obj_set_style_border_width(card,1,0);
    lv_obj_set_style_radius(card,18,0);
    lv_obj_set_style_pad_all(card,12,0);
    lv_obj_set_style_text_color(card,lv_color_white(),0);
    /* 新中文统一使用已有 18px 全字库，避免小字号固定字库缺字。 */
    lv_obj_set_style_text_font(card,ui_font_18(),0);
    s_status=lv_label_create(card);
    lv_obj_set_width(s_status,350);
    lv_obj_set_style_text_align(s_status,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_align(s_status,LV_ALIGN_TOP_MID,0,10);
    lv_obj_t *hint=lv_label_create(card);
    lv_label_set_text(hint,"持续按住电源键 10 秒将强制关机");
    lv_obj_set_width(hint,350);
    lv_obj_set_style_text_align(hint,LV_TEXT_ALIGN_CENTER,0);
    lv_obj_set_style_text_opa(hint,LV_OPA_70,0);
    lv_obj_align(hint,LV_ALIGN_TOP_MID,0,66);
    const char *labels[]={"确认关机","重启","取消"};
    const system_power_action_t actions[]={SYSTEM_POWER_OFF,SYSTEM_POWER_RESTART,SYSTEM_POWER_CANCEL};
    for (unsigned i=0;i<3;++i) {
        s_buttons[i]=lv_button_create(card);
        lv_obj_set_size(s_buttons[i],106,44);
        lv_obj_align(s_buttons[i],LV_ALIGN_BOTTOM_LEFT,(int32_t)i*120,0);
        lv_obj_set_style_bg_color(s_buttons[i],lv_color_white(),0);
        lv_obj_set_style_bg_opa(s_buttons[i],LV_OPA_20,0);
        lv_obj_set_style_shadow_width(s_buttons[i],0,0);
        lv_obj_remove_flag(s_buttons[i],LV_OBJ_FLAG_EVENT_BUBBLE | LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_add_event_cb(s_buttons[i],action_cb,LV_EVENT_CLICKED,(void *)(uintptr_t)actions[i]);
        lv_obj_t *label=lv_label_create(s_buttons[i]);
        lv_label_set_text(label,labels[i]); lv_obj_center(label);
    }
}
void ui_power_dialog_show(void)
{
    if (!s_overlay) create_dialog();
    lv_obj_remove_flag(s_overlay,LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_overlay);
    ui_power_dialog_set_status("正在关机，请选择操作",false);
}
void ui_power_dialog_hide(void)
{
    if (s_overlay) lv_obj_add_flag(s_overlay,LV_OBJ_FLAG_HIDDEN);
}
void ui_power_dialog_set_status(const char *text, bool busy)
{
    if (!s_overlay) return;
    lv_label_set_text(s_status,text);
    /* 保存期间禁用全部按钮，避免半途取消后却继续断电。失败后任务恢复按钮，
     * 保留原 UI 以允许重试或取消，不以保存失败为由静默丢弃用户设置。 */
    for (unsigned i=0;i<3;++i) {
        if (busy) lv_obj_add_state(s_buttons[i],LV_STATE_DISABLED);
        else lv_obj_remove_state(s_buttons[i],LV_STATE_DISABLED);
    }
}
bool ui_power_dialog_is_visible(void)
{ return s_overlay && !lv_obj_has_flag(s_overlay,LV_OBJ_FLAG_HIDDEN); }
