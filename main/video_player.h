/**
 * @file video_player.h
 * @brief 无音频裸 MJPEG 视频播放器的后台状态机。
 *
 * 设计边界：
 * - 只接收 460x460、无音频的裸 MJPEG；不做容器解复用、不做音频解码。
 * - 所有 SD/JPEG 工作都在单个 worker 内完成，UI 回调由 worker 在 LVGL
 *   锁内同步调用；UI 的公开函数只投递标量请求，不等待 SD/JPEG/LVGL。
 * - 播放使用硬件 JPEG 解码到两块 464x464 RGB565 缓冲，交替交给 UI
 *   canvas。464 是 460 按 JPEG 协议对齐到 16 的结果，右边/下边多出的
 *   4 像素由 460x460 的页面裁剪掉。
 * - 进入播放前会暂停音乐并关闭其 SD 文件；退出视频模式时释放视频解码
 *   缓冲和 JPEG 引擎，避免和锁屏 MJPEG 同时占用硬件。
 */

#ifndef VIDEO_PLAYER_H
#define VIDEO_PLAYER_H

#include "esp_err.h"
#include "video_library.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIDEO_PLAYER_WIDTH       460U
#define VIDEO_PLAYER_HEIGHT      460U
#define VIDEO_PLAYER_PAD_WIDTH   464U
#define VIDEO_PLAYER_PAD_HEIGHT  464U
#define VIDEO_PLAYER_THUMB_SIZE  176U

typedef enum {
    VIDEO_STATE_IDLE = 0,   /**< 没有打开视频 */
    VIDEO_STATE_LOADING,    /**< 正在暂停音乐/打开文件/解首帧 */
    VIDEO_STATE_PLAYING,    /**< 正常播放 */
    VIDEO_STATE_PAUSED,     /**< 用户暂停，保留当前帧 */
    VIDEO_STATE_ENDED,      /**< 已到文件末尾，保留最后一帧 */
    VIDEO_STATE_ERROR,      /**< 打开/解码失败，message 描述原因 */
} video_state_t;

/**
 * UI 回调集合。全部由 worker 在 LVGL 锁内调用，回调中禁止等待 SD、
 * 再次获取 LVGL 锁或长时间阻塞；只更新控件/复制字符串。
 */
typedef struct {
    void (*status)(const char *text, void *ctx);
    void (*library)(uint32_t count, bool truncated, uint32_t generation, void *ctx);
    void (*page)(const video_page_t *page, uint32_t generation, void *ctx);
    void (*thumbnail)(uint32_t slot, uint32_t index, const uint8_t *pixels,
                      uint16_t w, uint16_t h, uint32_t generation, void *ctx);
    void (*frame)(const uint8_t *pixels, uint16_t w, uint16_t h,
                  uint32_t sequence, void *ctx);
    void (*state)(video_state_t state, uint32_t index,
                  const char *message, void *ctx);
} video_player_ops_t;

/** @brief 创建命令队列和 worker。由 app_main 在 LVGL 锁外调用一次。 */
esp_err_t video_player_init(void);

/** @brief 注册 UI 回调。由 UI 创建时调用一次；回调指针生命周期到关机。 */
void video_player_set_ops(const video_player_ops_t *ops, void *ctx);

/* 以下函数均可从 LVGL 回调调用：只做非阻塞队列投递。 */
void video_player_scan(void);
void video_player_load_page(uint32_t first);
void video_player_play(uint32_t index);
void video_player_pause(void);
void video_player_resume(void);
void video_player_toggle(void);
void video_player_seek(uint16_t per_mille);
void video_player_stop(void);
/** @brief 离开视频模式/锁屏/关机：停止并释放解码器、缩略图缓冲。 */
void video_player_release(void);

bool video_player_is_active(void);
/** @brief worker 当前没有 SD/JPEG 工作且不在播放，可用于锁屏/关机屏障。 */
bool video_player_is_idle(void);
video_state_t video_player_get_state(void);
uint32_t video_player_get_index(void);
uint16_t video_player_get_progress(void);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_PLAYER_H */
