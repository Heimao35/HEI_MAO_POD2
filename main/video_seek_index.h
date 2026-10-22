/**
 * @file video_seek_index.h
 * @brief 裸 MJPEG 的可选帧位置索引（.vix），用于拖动时精确对齐音画。
 *
 * 裸 MJPEG 没有逐帧时间戳，且通常是变码率：按文件字节比例 seek 得到的
 * 帧时间与音轨按时间比例 seek 的位置并不一致。PC 端工具
 * scripts/mjpeg_index.py 用 ffprobe 生成稀疏帧位置表（每秒一个检查点）：
 *
 *   HMVX, version, frame_us, total_frames, interval, count, file_size,
 *   然后 count 个小端 uint32 文件偏移（第 k 项对应第 k*interval 帧）
 *
 * 索引很小（1 小时 24fps 约 3600 项、14 KiB）。文件大小不匹配、版本或
 * 内容不一致时整个索引视为无效，播放器回退到原来的字节比例 seek。
 */
#ifndef VIDEO_SEEK_INDEX_H
#define VIDEO_SEEK_INDEX_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct video_seek_index video_seek_index_t;

/**
 * @brief 打开并校验索引。调用方持有 SD 锁。
 * @param path       .vix 的 VFS 路径
 * @param video_size 对应视频的实际字节数；与索引头不一致时拒绝
 * @note 索引文件很小，读入 PSRAM 后立即关闭 FILE。
 */
esp_err_t video_seek_index_open(video_seek_index_t **out, const char *path,
                                uint32_t video_size);
void video_seek_index_close(video_seek_index_t *index);

uint32_t video_seek_index_frame_us(const video_seek_index_t *index);
uint32_t video_seek_index_total_frames(const video_seek_index_t *index);
uint32_t video_seek_index_interval(const video_seek_index_t *index);

/**
 * @brief 查找目标帧之前最近的检查点。
 * @param[in]  frame           目标帧号（会被裁剪到有效范围）
 * @param[out] checkpoint_frame 检查点对应的帧号（<= frame）
 * @param[out] byte_offset     该帧的 JPEG SOI 文件偏移
 * @return false 表示索引无效/为空。
 */
bool video_seek_index_lookup(const video_seek_index_t *index, uint32_t frame,
                             uint32_t *checkpoint_frame, uint64_t *byte_offset);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_SEEK_INDEX_H */
