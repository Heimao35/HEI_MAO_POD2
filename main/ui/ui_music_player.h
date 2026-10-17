/**
 * @file ui_music_player.h
 * @brief 音乐播放器模式页面
 *
 * 页面布局（从上到下）：
 *   - 背景：歌曲封面模糊图铺满全屏（当前用纯色占位，见实现说明）
 *   - 中上：清晰封面图（点击可切换歌词/封面）
 *   - 下方：圆角矩形播放控制台
 *     - 上一首 / 暂停继续 / 下一首 / 播放模式
 *     - 进度条 + 歌曲总时长
 *
 * 手势：
 *   - 点击封面：切换歌词/封面显示
 *   - 非边缘左右滑：切换上一首/下一首
 *   - 歌词视口外上滑：展开歌曲列表
 *   - 歌曲列表中上边缘下滑：收起歌曲列表
 */

#ifndef UI_MUSIC_PLAYER_H
#define UI_MUSIC_PLAYER_H

#include "ui_common.h"
#include "../music_library.h"
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建音乐播放器页面
 * @param parent 父对象
 */
void ui_music_player_create(lv_obj_t *parent);

typedef struct {
    void (*select)(uint32_t index);
    void (*page)(uint32_t first);
    void (*rescan)(void);
    uint32_t (*get_time_ms)(void);
    uint32_t (*get_duration_sec)(void);
    uint32_t (*get_lyric_line)(uint32_t time_ms);
    bool (*is_playing)(void);
} ui_music_backend_t;

void ui_music_player_bind(const ui_music_backend_t *backend);
void ui_music_player_set_library(uint32_t count, const char *status);
void ui_music_player_set_page(const music_page_t *page);
void ui_music_player_show_song(uint32_t index, const char *title,
                               const uint8_t *cover, uint32_t duration_sec);
/** 后台发布前检查动画阶段；以下接口均须持有 LVGL 锁。 */
bool ui_music_player_can_show_song(uint32_t index);
/** 歌名/封面/歌词发布完成（失败时发布占位也须通知），释放离屏等待。 */
void ui_music_player_song_ready(uint32_t index);
void ui_music_player_set_lyrics(const char *text, uint32_t line_count, bool truncated);

/**
 * @brief 获取音乐播放器页面容器（用于页面切换管理）
 * @return lv_obj_t* 页面容器指针
 */
lv_obj_t *ui_music_player_get_page(void);

/**
 * @brief 刷新播放状态显示（进度条、时间、播放/暂停图标）
 * @param current_sec  当前播放位置（秒）
 * @param total_sec    歌曲总时长（秒）
 * @param is_playing   是否正在播放
 */
void ui_music_player_update_progress(uint32_t current_sec, uint32_t total_sec, bool is_playing);

/**
 * @brief 设置歌曲信息（封面后续接入，当前只更新标题）
 * @param title 歌曲标题字符串
 */
void ui_music_player_set_song(const char *title);

/**
 * @brief 显示/隐藏歌曲列表
 * @param show true=展开歌曲列表, false=收起
 */
void ui_music_player_toggle_playlist(bool show);

/** 播放控制回调类型 —— 由硬件层注册 */
typedef void (*ui_music_ctrl_cb_t)(void);

/**
 * @brief 注册播放控制回调
 * @param on_prev  上一首回调
 * @param on_next  下一首回调
 * @param on_toggle 暂停/继续回调
 * @param on_mode  播放模式切换回调
 */
void ui_music_player_set_callbacks(ui_music_ctrl_cb_t on_prev,
                                   ui_music_ctrl_cb_t on_next,
                                   ui_music_ctrl_cb_t on_toggle,
                                   ui_music_ctrl_cb_t on_mode);

/**
 * @brief 查询歌曲列表是否当前展开
 * @return true=歌曲列表可见
 */
bool ui_music_player_is_playlist_visible(void);

/** Returns true while the cover area is showing the vertically scrollable lyrics. */
bool ui_music_player_is_lyric_visible(void);
/** Tests whether a screen coordinate is inside the visible lyric viewport. */
bool ui_music_player_is_lyric_point(int32_t x, int32_t y);

/**
 * @brief 带滑动动画切换到下一首歌曲（用于手势触发）
 */
void ui_music_player_next_animated(void);

/**
 * @brief 带滑动动画切换到上一首歌曲（用于手势触发）
 */
void ui_music_player_prev_animated(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MUSIC_PLAYER_H */
