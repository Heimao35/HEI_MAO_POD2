/**
 * @file bt_audio_stream.h
 * @brief 送往 U4/A2DP 的 PCM 环形缓冲与重采样（P4 侧）。
 *
 * 音频源（音乐解码 worker、视频音轨 worker）在把 PCM 交给本机 I2S ring
 * 的同时，调用 bt_stream_write() 复制一份到这里的 16 KiB 环形缓冲：
 * - 输入可为 16/24/32-bit、单/双声道、44.1/48/96/192 kHz；
 * - 输出固定为 16-bit 立体声、U4 当前 A2DP SBC 采样率（44.1/48 kHz）；
 * - 传输 worker 用 bt_stream_read() 取数据并按 credit 发 LINK_MSG_PCM。
 *
 * 线程模型：
 * - 写入来自音频/视频 worker，读取来自 bt_link worker，两者用互斥量
 *   保护重采样状态和环形缓冲；
 * - 缓冲有界，BT 链路拥塞时丢弃新数据并计数，绝不阻塞本机播放；
 * - 复位（切歌、暂停、seek、断开）清空缓冲和重采样状态。
 */

#ifndef BT_AUDIO_STREAM_H
#define BT_AUDIO_STREAM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 初始化 16 KiB 内部 PCM 缓冲和互斥量；不访问蓝牙/SDIO。 */
esp_err_t bt_stream_init(void);

/** 清空缓冲、重采样器和丢帧计数（切歌/暂停/seek/断开时调用）。 */
void bt_stream_reset(void);

/**
 * @brief 设置目标采样率（U4 的 A2DP SBC 速率）。
 *
 * 只接受 44100/48000，其他值回退 48000。速率变化会清空缓冲，避免
 * 两种采样率的数据混在一起。
 */
void bt_stream_set_target_rate(uint32_t rate);

/** 当前目标采样率（仅诊断/日志用）。 */
uint32_t bt_stream_target_rate(void);

/**
 * @brief 设置 P4 侧 PCM 数字音量（0..100，muted 为 true 时静音）。
 *
 * 为兼容不执行 AVRCP 绝对音量的音箱，实际响度由 P4 对 16-bit PCM
 * 做增益控制；U4/音箱的 AVRCP 音量在连接时固定为最大。
 */
void bt_stream_set_volume(uint8_t volume, bool muted);

/**
 * @brief 转换并重采样一段源 PCM，写入环形缓冲。
 *
 * @param pcm   源 PCM 起始地址
 * @param frames 源帧数
 * @param bits  16/24/32
 * @param channels 1/2
 * @param rate  源采样率
 * @return 实际处理的源帧数（可能因缓冲满而丢弃部分输出）
 *
 * @note 可在音频/视频 worker 调用，内部有界互斥；不会访问 SDIO/Flash。
 *       仅在蓝牙已连接时由调用方调用，未连接时不应写入。
 */
size_t bt_stream_write(const void *pcm, size_t frames, unsigned bits,
                       unsigned channels, uint32_t rate);

/**
 * @brief 取出目标格式 PCM（16-bit 立体声，≤ max_bytes）。
 * @return 实际取出的字节数，保证为 4 的倍数；0 表示暂无数据。
 */
size_t bt_stream_read(uint8_t *out, size_t max_bytes);

/** 当前可读字节数。 */
size_t bt_stream_queued(void);

/** 累计因缓冲满而丢弃的字节数（诊断用）。 */
uint32_t bt_stream_dropped_bytes(void);

#ifdef __cplusplus
}
#endif

#endif /* BT_AUDIO_STREAM_H */
