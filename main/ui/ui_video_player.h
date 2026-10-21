/**
 * @file ui_video_player.h
 * @brief 视频播放器页面：2x2 预览翻页、全屏播放和控制台。
 *
 * 页面本身不访问 SD/JPEG/NVS；文件扫描、缩略图生成和视频解码全部由
 * video_player 后台 worker 完成。UI 只通过回调更新控件。
 */

#ifndef UI_VIDEO_PLAYER_H
#define UI_VIDEO_PLAYER_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建视频页面（列表 + 全屏播放器，播放器初始隐藏）。 */
void ui_video_player_create(lv_obj_t *parent);

/** @brief 获取页面容器，供 ui_manager 做模式切换动画。 */
lv_obj_t *ui_video_player_get_page(void);

/** @brief 全屏播放器是否正在显示（用于阻止状态栏下拉/设置页盖住视频）。 */
bool ui_video_player_is_player_active(void);

/**
 * @brief 切换到视频模式时调用（ui_manager，持有 LVGL 锁）。
 *
 * 显示列表、恢复状态栏；首次进入或 SD 代次变化时请求后台扫描，
 * 否则直接请求当前预览页。不会自动播放视频。
 */
void ui_video_player_on_mode_enter(void);

/**
 * @brief 离开视频模式时调用（ui_manager，持有 LVGL 锁）。
 *
 * 停止播放、释放视频解码器/缩略图缓冲、恢复状态栏。页面切换动画仍由
 * ui_manager 负责，本函数只处理媒体资源和内部视图状态。
 */
void ui_video_player_on_mode_leave(void);

/**
 * @brief 锁屏前调用（ui_manager，持有 LVGL 锁）。
 *
 * 与离开模式类似，但保留当前预览页索引，方便解锁后重新加载；必须
 * 先停止视频，锁屏 MJPEG 才能独占硬件 JPEG 解码器。
 */
void ui_video_player_close_for_lock(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_VIDEO_PLAYER_H */
