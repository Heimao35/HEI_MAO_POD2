/**
 * @file ui_video_player.c
 * @brief 视频播放器页面实现（占位框架）
 *
 * 全屏深色背景，中间放置视频图标和提示文字，后续接入 MJPEG 解码。
 */

#include "ui_video_player.h"

static lv_obj_t *s_page = NULL; /**< 视频播放器页面容器 */

/**
 * @brief 创建视频播放器占位页面
 * @param parent 父对象
 */
void ui_video_player_create(lv_obj_t *parent)
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

    /* 视频图标 */
    lv_obj_t *icon = lv_label_create(s_page);
    lv_label_set_text(icon, LV_SYMBOL_VIDEO);
    lv_obj_set_style_text_color(icon, UI_COLOR_ACCENT, 0);
    lv_obj_set_style_text_font(icon, &lv_font_montserrat_26, 0);
    lv_obj_align(icon, LV_ALIGN_CENTER, 0, -20);

    /* 提示文字 */
    lv_obj_t *tip = lv_label_create(s_page);
    lv_label_set_text(tip, "Video Player");
    lv_obj_set_style_text_color(tip, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(tip, &lv_font_montserrat_16, 0);
    lv_obj_align(tip, LV_ALIGN_CENTER, 0, 20);
}

/** @brief 获取视频播放器页面容器 */
lv_obj_t *ui_video_player_get_page(void)
{
    return s_page;
}
