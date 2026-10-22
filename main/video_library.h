/**
 * @file video_library.h
 * @brief 视频目录的有限磁盘索引与分页读取。
 *
 * 视频数量可能很多，UI 不能为每个文件创建一个 LVGL 对象。本模块沿用
 * music_library 的做法：扫描时把固定大小记录写入 SD 上的可丢弃索引，
 * UI 每次只取 4 条（2x2 预览页）。索引有字节上限，超出只标记截断，
 * 不无限占用 PSRAM 或阻塞开机。
 */

#ifndef VIDEO_LIBRARY_H
#define VIDEO_LIBRARY_H

#include "esp_err.h"
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define VIDEO_NAME_MAX       256
#define VIDEO_PAGE_SIZE      4
/* 16 MiB / 264 B ≈ 64k 个文件；超出后只保留前面的记录并报告截断。 */
#define VIDEO_INDEX_MAX_BYTES (16U * 1024U * 1024U)

/* 两种目录布局（扫描时自动识别，UI 显示名不变）：
 * - 文件夹布局：MEDIA/video/<name>/<name>.mjpeg|.mjpg + 可选 <name>.wav
 * - 旧式平铺：  MEDIA/video/<文件名>.mjpeg|.mjpg，无音轨
 * name 对文件夹布局是文件夹名（不含扩展名），对旧式布局是完整文件名。 */
#define VIDEO_KIND_FILE      0U
#define VIDEO_KIND_FOLDER    1U

typedef struct {
    char name[VIDEO_NAME_MAX]; /* 显示名：文件夹名或旧式文件名 */
    uint32_t size;             /* 0；播放/缩略图阶段按需 stat */
    uint8_t kind;              /* VIDEO_KIND_* */
    uint8_t ext;               /* 0=.mjpeg 1=.mjpg */
    uint8_t has_audio;         /* 文件夹布局且存在同名 .wav */
} video_item_t;

typedef struct {
    uint32_t first;
    uint32_t count;
    /* UI 继续只读 names/sizes；items 供 worker 拼路径和判断音轨。 */
    char names[VIDEO_PAGE_SIZE][VIDEO_NAME_MAX];
    uint32_t sizes[VIDEO_PAGE_SIZE];
    video_item_t items[VIDEO_PAGE_SIZE];
} video_page_t;

/**
 * @brief 扫描 MEDIA/video，重建索引。
 * @param cancel 可选的取消标志；锁屏/关机时置位，扫描会尽快退出。
 * @note 调用方必须持有 sd_card_io_lock()。扫描过程中每隔若干目录项
 *       让出 CPU，避免长目录把 LVGL 或音频饿死。
 */
esp_err_t video_library_scan(atomic_bool *cancel);

void video_library_reset(void);
uint32_t video_library_get_count(void);
bool video_library_is_truncated(void);

/** @brief 读取一页（最多 VIDEO_PAGE_SIZE 条）。调用方持有 SD 锁。 */
esp_err_t video_library_get_page(uint32_t first, video_page_t *out);

/** @brief 读取单条记录。调用方持有 SD 锁。 */
esp_err_t video_library_get_item(uint32_t index, video_item_t *out);

/** @brief 由记录拼出视频文件的绝对 VFS 路径。 */
esp_err_t video_library_make_video_path(const video_item_t *item,
                                        char *out, size_t size);

/** @brief 拼出同名 WAV 的绝对路径；没有音轨时返回 ESP_ERR_NOT_FOUND。 */
esp_err_t video_library_make_audio_path(const video_item_t *item,
                                        char *out, size_t size);

/** @brief 拼出可选帧索引 .vix 的绝对路径（文件夹布局）。 */
esp_err_t video_library_make_index_path(const video_item_t *item,
                                        char *out, size_t size);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_LIBRARY_H */
