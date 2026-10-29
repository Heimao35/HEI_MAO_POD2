/**
 * @file ui_image_viewer.h
 * @brief 图片查看器模式页面：两个文件夹入口、四列总览、全屏查看。
 */

#ifndef UI_IMAGE_VIEWER_H
#define UI_IMAGE_VIEWER_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建图片查看器页面（不访问 SD/JPEG，实际扫描在模式进入时投递）。 */
void ui_image_viewer_create(lv_obj_t *parent);

/** @brief 获取图片查看器页面容器 */
lv_obj_t *ui_image_viewer_get_page(void);

/** @brief 切换到图片模式：回到首页并在需要时请求扫描。 */
void ui_image_viewer_on_mode_enter(void);

/** @brief 离开图片模式：立即退出全屏/总览并取消后台解码请求。 */
void ui_image_viewer_on_mode_leave(void);

/** @brief 锁屏前收起全屏和未完成的解码；总览状态保留到解锁。 */
void ui_image_viewer_close_for_lock(void);

/** @brief 是否正在全屏查看图片（ui_manager 用于抑制状态栏下拉设置）。 */
bool ui_image_viewer_is_fullscreen_active(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_IMAGE_VIEWER_H */
