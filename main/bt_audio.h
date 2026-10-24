/**
 * @file bt_audio.h
 * @brief P4 侧蓝牙音频应用层接口（UI 与 U4 板间链路的唯一接触面）。
 *
 * 设计边界：
 * - 蓝牙协议栈和 A2DP 实际运行在 ESP32-U4WDH 协处理器上；P4 不直接
 *   初始化 Bluedroid，也不在本地做 SBC 编码。P4 只维护用户意图、把
 *   UI/音频数据经 SDIO 发给 U4，并接收设备列表/连接状态。
 * - 本文件只描述应用层语义，不暴露 SDIO、寄存器或 link_protocol.h 的
 *   细节；后续 P4 SDIO 主机 worker 作为唯一实现者调用下面接口发布状态。
 * - 当前阶段（UI 对接）已实现电源意图、扫描/连接请求和只读状态机，
 *   板间链路尚未接入：link_ready 保持 false，扫描不会产生设备，
 *   连接请求返回 ESP_ERR_INVALID_STATE。UI 会明确显示“链路未就绪”，
 *   不会伪造设备或已连接状态。
 * - 蓝牙开关状态按用户要求不写入 NVS，每次上电默认关闭；配对设备信息
 *   的持久化属于后续链路阶段，不在本模块里另建 NVS 存储。
 *
 * 线程模型：
 * - 以下接口可从 LVGL 任务或后台 worker 调用，模块内部使用原子量同步；
 *   返回设备列表的接口会复制到调用方缓冲区，不返回内部指针。
 * - 不提供回调，避免在 SDIO 中断/worker 上下文中直接操作 LVGL。UI 以
 *   250ms 定时器轮询本接口；状态栏沿用现有 1s 周期刷新。
 */

#ifndef BT_AUDIO_H
#define BT_AUDIO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/** UI 一次最多显示/缓存多少个扫描结果（U4 侧缓冲区上限为 16）。 */
#define BT_AUDIO_MAX_DEVICES 16
/** 设备名最大字节数（含结尾 '\0'）；U4 扫描事件只携带 32 字节名称，
 * 这里留一倍余量，后续若扩展协议不需要同步修改 UI。 */
#define BT_AUDIO_NAME_MAX 64

/**
 * @brief 蓝牙应用层状态。
 *
 * OFF       用户关闭；不保持 U4 连接（后续由链路 worker 决定是否复位 U4）。
 * ON        用户已打开，链路就绪但当前未连接、未扫描。
 * SCANNING  正在搜索附近设备。
 * CONNECTING 已向某台设备发起配对/连接，等待结果。
 * CONNECTED A2DP 链路已建立，音频可以送往蓝牙。
 * ERROR     最近一次启动/连接失败；具体原因由链路层日志记录。
 */
typedef enum {
    BT_AUDIO_STATE_OFF = 0,
    BT_AUDIO_STATE_ON,
    BT_AUDIO_STATE_SCANNING,
    BT_AUDIO_STATE_CONNECTING,
    BT_AUDIO_STATE_CONNECTED,
    BT_AUDIO_STATE_ERROR,
} bt_audio_state_t;

/** 扫描到的设备描述。bda 为 Classic BT 6 字节地址（小端顺序与 U4 一致）。 */
typedef struct {
    uint8_t bda[6];
    int8_t rssi;
    char name[BT_AUDIO_NAME_MAX];
} bt_audio_device_t;

/**
 * @brief 初始化应用层状态机（不访问 SDIO/不启动蓝牙）。
 *
 * 必须在 UI 创建前调用，确保页面读取到确定的“关闭”初始状态。
 * 上电默认关闭；开关状态不做持久化。
 *
 * @return ESP_OK 或内存/同步原语初始化错误
 */
esp_err_t bt_audio_init(void);

/**
 * @brief 记录用户的蓝牙开关意图。
 *
 * 立即更新对 UI 可见的开关/状态，实际启动/关闭 U4 链路由后续 worker
 * 异步完成。关闭时同时丢弃扫描结果与连接状态；不会写 NVS。
 *
 * @param enabled true 打开，false 关闭
 */
void bt_audio_set_enabled(bool enabled);

/** @brief 用户意图是否为“打开”（不代表链路已就绪）。 */
bool bt_audio_is_enabled(void);

/**
 * @brief 与 U4 的板间链路是否完成握手。
 *
 * 由 SDIO 链路 worker 在收到 U4 的 LINK_EVT_READY 后置位；初始化失败、
 * U4 复位或版本不匹配时清零。UI 用它区分“用户已打开但后端未就绪”和
 * 真正的未连接状态，避免显示虚假结果。
 */
bool bt_audio_link_ready(void);

/** @brief 获取当前应用层状态（原子读取，可在任意任务调用）。 */
bt_audio_state_t bt_audio_get_state(void);

/**
 * @brief 请求开始搜索附近设备。
 *
 * 仅在已打开且链路就绪时生效；调用会清空上一轮结果。重复调用等价于
 * 一次新的搜索请求。UI 在进入蓝牙设置页或打开开关后调用。
 */
void bt_audio_scan_start(void);

/** @brief 请求停止搜索；已连接状态不受影响。 */
void bt_audio_scan_stop(void);

/**
 * @brief 复制当前扫描结果。
 *
 * @param out 调用方缓冲区，可为 NULL 仅查询数量
 * @param max 最多复制多少个元素
 * @return 实际复制的元素数量（0 表示没有结果/链路未就绪）
 */
size_t bt_audio_get_devices(bt_audio_device_t *out, size_t max);

/**
 * @brief 请求与指定设备配对/连接。
 *
 * 异步执行；结果通过 bt_audio_get_state()/bt_audio_is_connected() 轮询。
 * 若已有连接，后续实现应先断开旧设备（A2DP Source 同一时间只支持一个
 * Sink）。配对成功后由链路层记住设备地址，供下次打开蓝牙自动重连。
 *
 * @param bda 6 字节 Classic BT 地址
 * @return ESP_OK 表示请求已接受；ESP_ERR_INVALID_STATE 表示未打开或链路
 *         未就绪；ESP_ERR_NOT_SUPPORTED 表示当前构建尚未接入 SDIO 主机
 */
esp_err_t bt_audio_connect(const uint8_t bda[6]);

/** @brief 当前是否已建立 A2DP 连接（只反映链路层确认过的状态）。 */
bool bt_audio_is_connected(void);

/**
 * @brief 更新 A2DP 绝对音量意图（0..100，muted 为 true 时发送 0）。
 *
 * 由音量滑条/物理按键经 audio_output_set_volume 调用；连接后通过 U4
 * 的 AVRCP 发送 0..127。本机 DAC 的音量叠加“蓝牙连接时静音”策略，
 * 与本函数的用户意图分开保存。
 */
void bt_audio_set_volume(uint8_t volume, bool muted);

/**
 * @brief 复制已连接设备名称（线程安全）。
 *
 * @param out 输出缓冲区；未连接时写入空字符串
 * @param size 缓冲区大小
 * @return 写入的字节数（不含结尾 NUL）
 */
size_t bt_audio_get_connected_name(char *out, size_t size);

/**
 * @brief 记录最近一次成功配对的设备，供下次打开蓝牙自动连接。
 *
 * 由链路 worker 在收到 U4 的 LINK_EVT_CONNECTED 后调用；只更新 RAM，
 * 最终由现有 player_settings 快照链路写入 NVS。也可在开机时用 NVS
 * 中读出的值调用。bda 全 0 视为无效并清除记录。
 */
void bt_audio_set_paired(const uint8_t bda[6], const char *name);

/**
 * @brief 读取配对设备（线程安全，只访问 RAM）。
 * @return true 表示存在有效配对记录；false 时输出保持清零
 */
bool bt_audio_get_paired(uint8_t bda[6], char *name, size_t name_size);

/**
 * @brief 取消最近配对设备：清除 P4 记录并通知 U4 删除蓝牙绑定。
 *
 * 只清除一条配对记录；若该设备当前已连接，U4 会先断开 A2DP 再删除
 * 绑定。NVS 由现有 player_settings 快照在 3 秒内合并保存。
 */
void bt_audio_forget_paired(void);

/* =====================================================================
 * 以下为链路内部发布接口：只允许 bt_link.c 的 worker 调用。
 * 这些函数只更新 RAM/原子状态，不访问 LVGL、不阻塞；UI 通过上面的
 * 只读接口轮询，因此不会在 SDIO 任务上下文里操作界面。
 * ===================================================================== */

/** U4 完成 HELLO 应答/链路失效时更新就绪状态；就绪且已打开时会自动
 *  尝试连接已配对设备。 */
void bt_audio_link_set_ready(bool ready);

/** 收到新的搜索请求/开始搜索时清空上一轮结果。 */
void bt_audio_link_scan_clear(void);

/** 追加/更新一条扫描结果（按 BDA 去重，最多 BT_AUDIO_MAX_DEVICES 条）。 */
void bt_audio_link_scan_add(const uint8_t bda[6], int8_t rssi, const char *name);

/** 发布 U4 的 A2DP 状态机（LINK_BT_*），用于扫描/连接中的过渡状态。 */
void bt_audio_link_bt_state(int bt_state);

/** U4 确认 A2DP 已连接：更新状态、名称并记住配对地址。 */
void bt_audio_link_connected(const uint8_t bda[6]);

/** U4 报告断开/连接失败后的清理。 */
void bt_audio_link_disconnected(void);

/** U4 返回错误码（link_protocol.h LINK_EVT_ERROR）。 */
void bt_audio_link_error(uint8_t code);

#ifdef __cplusplus
}
#endif

#endif /* BT_AUDIO_H */
