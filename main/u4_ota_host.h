#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @file u4_ota_host.h
 * @brief P4 侧通过 SDIO 更新 U4（ESP32-U4WDH）固件。
 *
 * 调用约束：
 * - 必须在用户确认后、蓝牙开关为关闭（默认开机状态）时调用；
 * - 在固件更新任务中同步执行，阻塞到 U4 更新/重启/版本复核完成；
 * - 自己持有 SD 锁读取 U4 bin，自己初始化/关闭 SDMMC slot1 + ESSL，
 *   期间不使用 bt_link 的 worker（BT 默认关闭，worker 不碰链路）；
 * - 成功后 U4 被重新置于 CHIP_PU 复位，保持“蓝牙默认关闭”的上电状态。
 *
 * 第一次把 U4 升级到支持 SDIO OTA 的版本仍必须用 UART 烧录；本函数会
 * 通过 FW_INFO 检测旧协议并返回 ESP_ERR_NOT_SUPPORTED。
 */

typedef struct {
    bool updated;               /**< 是否实际传输并提交了镜像 */
    bool ota_capable;           /**< U4 是否支持 v2 FW_INFO/OTA */
    uint8_t slot;               /**< 更新后运行的 U4 槽位（0/1） */
    char version[32];           /**< 更新后/当前运行的 U4 版本 */
} u4_ota_result_t;

/**
 * @brief 更新 U4 到指定 bin。
 *
 * @param path       U4 app bin 的 VFS 路径（/sdcard/...）
 * @param size       扫描时记录的 bin 大小
 * @param bin_version 扫描时读取的版本字符串
 * @param progress   可选进度回调（0..100），在更新任务上下文调用
 * @param ctx        进度回调上下文
 * @param out        可选结果（运行版本/槽位/是否实际更新）
 *
 * @return ESP_OK 表示 U4 已在目标版本或成功升级并复核；
 *         ESP_ERR_NOT_SUPPORTED 表示当前 U4 固件不支持 SDIO OTA；
 *         其它错误见日志。
 */
esp_err_t u4_ota_host_update(const char *path, uint32_t size,
                             const char *bin_version,
                             void (*progress)(uint8_t pct, void *ctx),
                             void *ctx,
                             u4_ota_result_t *out);
