#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file u4_ota.h
 * @brief U4 侧 SDIO 固件升级接收器（v2 协议）。
 *
 * 数据由 P4 通过 LINK_CMD_FW_* 命令下发；FW_DATA 直接使用 SDIO 从机接收
 * 缓冲（内部 DMA RAM）调用 esp_ota_write()，不额外拷贝。命令处理在 SDIO
 * 接收任务中同步执行，P4 使用停等 ACK 保证不会在 Flash 擦写期间继续发包。
 */

/** 新镜像首次启动时，在 NVS/SDIO/Bluetooth 初始化完成后确认镜像有效。 */
void u4_ota_mark_valid_if_pending(void);

/** 若 payload[0] 是 OTA 命令则处理并返回 true；其它命令返回 false。 */
bool u4_ota_handle_cmd(const uint8_t *payload, size_t len);
