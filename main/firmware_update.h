#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

/**
 * @file firmware_update.h
 * @brief SD 卡 /HEI_MAO_POD_SYS/BIN 目录的 P4 应用固件检查与 OTA 更新。
 *
 * 设计边界（与项目的电源/内存/UI 规则保持一致）：
 * - 只在开机初始化末尾检查一次，不创建常驻任务、定时器或轮询；
 * - 固件版本来自 app 镜像内部的 esp_app_desc_t，不信任文件名；
 * - 更新写入“另一个”OTA 槽，esp_ota_end() 校验成功后才切换启动分区；
 * - 更新任务使用内部 RAM 栈和内部 8 KiB 缓冲：OTA 校验会走
 *   bootloader_mmap()/MMU 映射，项目规则禁止 PSRAM 栈任务调用
 *   esp_ota_*/flash API；数据源在 DRAM 才能直接写 Flash；
 * - 读 SD 时短时间持有 sd_card_io_lock；此时处于开机阶段，音频尚未播放；
 * - 确认框和进度框都挂在 LVGL top layer，不改变正常页面布局/手势。
 *
 * 版本号来自工程根目录 version.txt（IDF 会把它写入 app descriptor）。
 * 每次发布前递增 version.txt；SD 卡上的 bin 版本必须严格更高才会提示。
 */

typedef enum {
    FW_UPDATE_STATE_IDLE = 0,
    FW_UPDATE_STATE_PROMPT,          /* 正在等待用户确认/取消 */
    FW_UPDATE_STATE_RUNNING,         /* 独立任务正在写 OTA 槽 */
    FW_UPDATE_STATE_SUCCESS,         /* 镜像已校验并切换启动分区 */
    FW_UPDATE_STATE_FAILED,          /* 更新失败，旧启动分区保持不变 */
    FW_UPDATE_STATE_RESTART_FAILED,  /* 更新成功，但 PMIC 自动重启失败 */
} fw_update_state_t;

typedef enum {
    FW_UPDATE_ERR_NONE = 0,
    FW_UPDATE_ERR_NO_PARTITION,
    FW_UPDATE_ERR_NO_MEM,
    FW_UPDATE_ERR_SD_LOCK,
    FW_UPDATE_ERR_OPEN,
    FW_UPDATE_ERR_FILE_CHANGED,
    FW_UPDATE_ERR_READ,
    FW_UPDATE_ERR_OTA_BEGIN,
    FW_UPDATE_ERR_OTA_WRITE,
    FW_UPDATE_ERR_OTA_END,
    FW_UPDATE_ERR_SET_BOOT,
    FW_UPDATE_ERR_TASK,
} fw_update_error_t;

typedef struct {
    char current_version[32];  /* 当前运行 app 的版本 */
    char version[32];          /* SD 卡候选固件的版本 */
    char date[16];             /* 候选固件的编译日期（app descriptor） */
    uint32_t size;             /* 候选固件的文件大小 */
    char path[320];            /* /sdcard/HEI_MAO_POD_SYS/BIN/xxx.bin，VFS 路径 */
} fw_update_info_t;

/**
 * @brief 开机阶段的一次性检查；发现更高版本时阻塞等待用户选择。
 *
 * 必须在 LVGL、SD 卡、system_power 都初始化完成之后调用。函数返回后：
 * - 用户取消/没有新固件：正常继续开机；
 * - 更新失败：错误已由对话框展示，用户按“返回”后继续开机；
 * - 更新成功：先调用 PMIC 整机重启；若 PMIC 拒绝/未断电，则提示手动重启。
 *
 * 本函数只应调用一次；内部有静态标志防止重复扫描。
 */
void firmware_update_boot_check(void);

/**
 * @brief 新 OTA 镜像首次启动时确认镜像有效，取消 bootloader 回滚。
 *
 * 应在所有关键初始化（显示、PMIC、SD、音频等）完成后调用。factory 启动
 * 或当前已是 VALID 状态时直接返回 true。返回 false 表示镜像仍处于
 * PENDING_VERIFY 且写 otadata 失败：调用方应跳过本轮更新检查，避免
 * esp_ota_begin() 返回 ESP_ERR_OTA_ROLLBACK_INVALID_STATE。
 */
bool firmware_update_mark_valid_if_pending(void);

/* 以下三个接口由 LVGL 对话框回调调用，只投递事件，不做 I/O/等待。 */
void firmware_update_confirm(void);
void firmware_update_cancel(void);
void firmware_update_dismiss(void);

/* 供 LVGL 定时器读取的原子快照；不要在其他任务里直接改 s_info。 */
fw_update_state_t firmware_update_get_state(void);
uint8_t firmware_update_get_progress(void);
const fw_update_info_t *firmware_update_get_info(void);
const char *firmware_update_error_text(void);

/**
 * @brief 开机更新对话框是否占用屏幕。
 *
 * system_power 用它抑制电源键弹窗/短按熄屏，避免与顶层更新对话框竞争；
 * AXP2101 的 10 秒硬件强制关机不受影响。
 */
bool firmware_update_is_active(void);
