/**
 * @file bt_link.h
 * @brief P4 <-> ESP32-U4WDH SDIO 板间链路（SDMMC slot1 + ESSL）。
 *
 * 分层：
 * - bt_audio.c 维护 UI 可见的开关/连接/扫描状态与配对信息；
 * - 本模块只负责传输：初始化 SDMMC slot1、通过 ESSL 收发 U4 的
 *   link_protocol.h 数据包、把 U4 事件发布给 bt_audio，并把 bt_audio
 *   的命令排入 worker 发送。所有 SDIO/阻塞操作都在独立 worker 中完成，
 *   绝不进入 LVGL 回调或中断上下文。
 *
 * 线程模型：
 * - bt_link_init() 只创建队列和 worker，不碰硬件；
 * - bt_link_set_enabled/scan/connect 等可从任意任务调用，只投递消息；
 * - worker 串行访问 ESSL/SDMMC，接收事件后调用 bt_audio_link_* 发布。
 *
 * 当前只实现控制面（状态、扫描、连接）与链路保活；PCM 音频转发在同一
 * worker 的后续阶段接入（LINK_MSG_PCM + LINK_REG_PCM_CREDIT）。
 */

#ifndef BT_LINK_H
#define BT_LINK_H

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 与 U4 的 sdio_slave recv_buffer_size 完全一致（8 字节头 + 1 KiB 负载）。 */
#define BT_LINK_RECV_BUFFER_SIZE 1032
/** 主机单次发送上限；1024 恰好是两个 512 字节 SDIO 块，一次 CMD53 完成。 */
#define BT_LINK_TX_MAX           1024

/** 创建链路 worker 与命令队列。不初始化 SDMMC/ESSL，失败可重试。 */
esp_err_t bt_link_init(void);

/** 记录用户开关意图；worker 在未连接时用于抑制扫描/连接。 */
void bt_link_set_enabled(bool enabled);

/** 链路是否已完成 ESSL 初始化并收到 U4 的 READY 事件。 */
bool bt_link_is_ready(void);

/** 请求开始/停止搜索（链路未就绪时暂存，READY 后补发）。 */
void bt_link_scan_start(void);
void bt_link_scan_stop(void);

/** 请求连接指定 Classic BT 地址；链路未就绪时返回错误。 */
esp_err_t bt_link_connect(const uint8_t bda[6]);

/** 请求断开当前连接（若 U4 已连接）。 */
void bt_link_disconnect(void);

/** 媒体流控制（PCM 数据到达时由 worker 自动启停，接口保留给显式控制）。 */
void bt_link_media_start(void);
void bt_link_media_stop(void);

/** 设置 A2DP 绝对音量（0..127，经 U4 的 AVRCP 发送）。 */
void bt_link_set_volume(uint8_t volume);

#ifdef __cplusplus
}
#endif

#endif /* BT_LINK_H */
