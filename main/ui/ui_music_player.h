/**
 * @file ui_music_player.h
 * @brief 音乐播放器模式页面
 *
 * 页面布局（从上到下）：
 *   - 背景：歌曲封面模糊并混合白色后铺满全屏，切歌时交叉淡变
 *   - 中上：清晰封面图（点击可切换歌词/封面）
 *   - 下方：透明播放控制台，按键使用独立的半透明白色背景
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
    /* 后台准备/预缓冲期间仍保留用户的播放意图。按钮展示与再次点击使用它，
     * 实际进度仍由DMA完成计数驱动；错误、暂停和自然结束后此状态归false。 */
    bool (*is_play_requested)(void);
    /* 显式传递按钮目标状态，避免异步准备中二次toggle反转尚未生效的请求。 */
    void (*set_playing)(bool playing);
    /* DMA自然播放结束事件。UI定时器消费后复用现有切歌动画/单曲模式。 */
    bool (*take_finished)(void);
} ui_music_backend_t;

/* 开机/重新挂载后恢复歌曲选择。须持有 LVGL 锁，先同步 UI 当前索引，
 * 再由适配器排队加载；不播放切歌动画、不调用后端、不开始播放。 */
void ui_music_player_restore_selection(uint32_t index);
void ui_music_player_bind(const ui_music_backend_t *backend);
/** 自然结束处理共用入口，须持有 LVGL 锁。亮屏定时器传 true 保留原
 * 动画；熄屏媒体任务传 false，不依赖暂停的 LVGL 动画/定时器完成。
 * 只处理后端自然结束，不根据时间估计或错误主动开始播放。 */
void ui_music_player_service_finished(bool animate);
void ui_music_player_set_library(uint32_t count, const char *status);
void ui_music_player_set_page(const music_page_t *page);
void ui_music_player_show_song(uint32_t index, const char *title,
                               const uint8_t *cover, uint32_t duration_sec);
/** 以下接口均须持有 LVGL 锁；仅允许当前选中歌曲发布数据。
 * set_cover / show_song 借用封面参数，内部为显示对象独立持有引用。 */
bool ui_music_player_can_show_song(uint32_t index);
void ui_music_player_set_cover(uint32_t index, const uint8_t *cover);
/** 须持有 LVGL 锁，借用后台生成的 460×460 RGB565 图像并独立持有显示引用。
 * NULL 取消旧的待显示图，继续显示已有背景；重建曲库时统一清空。 */
void ui_music_player_set_background(uint32_t index, const uint8_t *pixels);
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
