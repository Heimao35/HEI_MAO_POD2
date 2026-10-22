/**
 * @file video_wav.h
 * @brief 视频同名 WAV 音轨的流式读取（PCM，直连 FatFs）。
 *
 * 只接受 RIFF/WAVE、PCM（format 1）、单/双声道 16/24/32bit。采样率是否
 * 被 DAC 支持由 audio_output 在打开时再次校验；不支持时视频静音播放。
 * 所有文件操作都由调用方持有 sd_card_io_lock()；WAV 与视频一样可能超过
 * 2 GiB，因此不使用 32 位有符号 off_t 的 VFS。
 */
#ifndef VIDEO_WAV_H
#define VIDEO_WAV_H

#include "esp_err.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct video_wav video_wav_t;

/** @brief 打开并解析 WAV 头；成功后可顺序读取 data 区。调用方持有 SD 锁。 */
esp_err_t video_wav_open(video_wav_t **out, const char *path);

/** @brief 关闭文件并释放对象。调用方持有 SD 锁。 */
void video_wav_close(video_wav_t *wav);

/** @brief SD 卸载回调：只关闭 FIL，保留对象，后续 read 返回 0。 */
void video_wav_detach(video_wav_t *wav);

uint32_t video_wav_rate(const video_wav_t *wav);
uint8_t video_wav_channels(const video_wav_t *wav);
uint8_t video_wav_bits(const video_wav_t *wav);
uint64_t video_wav_frames(const video_wav_t *wav);

/** @brief 读取最多 frames 帧原始 PCM；返回实际帧数（0=EOF/错误）。 */
size_t video_wav_read(video_wav_t *wav, void *pcm, size_t frames);

/** @brief 定位到指定帧（数据区起点为 0）。调用方持有 SD 锁。 */
bool video_wav_seek(video_wav_t *wav, uint64_t frame);

#ifdef __cplusplus
}
#endif

#endif /* VIDEO_WAV_H */
