/**
 * @file ui_ebook.h
 * @brief 电子书模式页面（框架占位）
 */

#ifndef UI_EBOOK_H
#define UI_EBOOK_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建电子书页面 */
void ui_ebook_create(lv_obj_t *parent);

/** @brief 获取电子书页面容器 */
lv_obj_t *ui_ebook_get_page(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_EBOOK_H */
