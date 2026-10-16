/**
 * @file ui_ebook.c
 * @brief 电子书页面实现（占位框架）
 *
 * 全屏浅色背景模拟阅读体验，中间放置文件图标和提示文字。
 */

#include "ui_ebook.h"

static lv_obj_t *s_page = NULL; /**< 电子书页面容器 */

/**
 * @brief 创建电子书占位页面
 * @param parent 父对象
 */
void ui_ebook_create(lv_obj_t *parent)
{
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_align(s_page, LV_ALIGN_CENTER, 0, 0);
    /* 电子书用偏暖白背景模拟纸张 */
    lv_obj_set_style_bg_color(s_page, lv_color_hex(0xF5F0E8), 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *icon = lv_label_create(s_page);
    lv_label_set_text(icon, LV_SYMBOL_FILE);
    lv_obj_set_style_text_color(icon, lv_color_hex(0x5B86E5), 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_26, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *tip = lv_label_create(s_page);
    lv_label_set_text(tip, "E-Book Reader");
    lv_obj_set_style_text_color(tip, lv_color_hex(0x333333), 0);
    lv_obj_set_style_text_font(tip, &lv_font_montserrat_16, 0);
    lv_obj_align(tip, LV_ALIGN_CENTER, 0, 20);
}

/** @brief 获取电子书页面容器 */
lv_obj_t *ui_ebook_get_page(void)
{
    return s_page;
}
