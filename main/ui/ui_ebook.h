/**
 * @file ui_ebook.h
 * @brief 电子书模式页面：书库入口、横向书架和全屏阅读页。
 */

#ifndef UI_EBOOK_H
#define UI_EBOOK_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建电子书页面（只在 ui_manager_init 中调用一次）。 */
void ui_ebook_create(lv_obj_t *parent);

/** @brief 获取电子书模式根页面。 */
lv_obj_t *ui_ebook_get_page(void);

/** @brief 进入电子书模式：回到首页，按需重新扫描。 */
void ui_ebook_on_mode_enter(void);

/** @brief 离开电子书模式：关闭当前书，收起内部页面。 */
void ui_ebook_on_mode_leave(void);

/** @brief 锁屏：关闭阅读页并释放正文，书库/首页状态保留。 */
void ui_ebook_close_for_lock(void);

/** @brief 阅读页是否处于全屏阅读状态（用于禁止状态栏下拉设置页）。 */
bool ui_ebook_is_reader_active(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_EBOOK_H */
