/**
 * @file ui_image_viewer.h
 * @brief 图片查看器模式页面（框架占位）
 */

#ifndef UI_IMAGE_VIEWER_H
#define UI_IMAGE_VIEWER_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建图片查看器页面 */
void ui_image_viewer_create(lv_obj_t *parent);

/** @brief 获取图片查看器页面容器 */
lv_obj_t *ui_image_viewer_get_page(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_IMAGE_VIEWER_H */
