/**
 * @file ui_video_player.h
 * @brief 视频播放器模式页面（框架占位）
 *
 * 当前仅搭建基础框架，后续填充具体功能。
 * 页面中间显示一个视频图标和提示文字。
 */

#ifndef UI_VIDEO_PLAYER_H
#define UI_VIDEO_PLAYER_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建视频播放器页面 */
void ui_video_player_create(lv_obj_t *parent);

/** @brief 获取视频播放器页面容器（用于页面切换管理） */
lv_obj_t *ui_video_player_get_page(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_VIDEO_PLAYER_H */
