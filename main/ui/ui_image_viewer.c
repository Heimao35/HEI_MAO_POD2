/**
 * @file ui_image_viewer.c
 * @brief 图片查看器页面实现（占位框架）
 *
 * 全屏深色背景，中间放置图片图标和提示文字，后续接入图片解码显示。
 */

#include "ui_image_viewer.h"
#include "ui_fonts.h"

static lv_obj_t *s_page = NULL; /**< 图片查看器页面容器 */

/**
 * @brief 创建图片查看器占位页面
 * @param parent 父对象
 */
void ui_image_viewer_create(lv_obj_t *parent)
{
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_align(s_page, LV_ALIGN_CENTER, 0, 0);
    lv_obj_set_style_bg_color(s_page, UI_COLOR_PAGE_BG, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_EVENT_BUBBLE);

    lv_obj_t *icon = lv_label_create(s_page);
    lv_label_set_text(icon, LV_SYMBOL_IMAGE);
    lv_obj_set_style_text_color(icon, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_26, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -20);

    lv_obj_t *tip = lv_label_create(s_page);
    lv_label_set_text(tip, "图片查看器");
    lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(tip, ui_font_16(), 0);
    lv_obj_align(tip, LV_ALIGN_CENTER, 0, 20);
}

/** @brief 获取图片查看器页面容器 */
lv_obj_t *ui_image_viewer_get_page(void)
{
    return s_page;
}
