/**
 * @file usb_audio.h
 * @brief USB 声卡模式（UAC2 扬声器）生命周期
 *
 * 使用 espressif/usb_device_uac 组件把 ESP32-P4 的 USB 2.0 HS 口（芯片
 * 第 49/50 脚 USB_DM/USB_DP）枚举成 96 kHz / 24 bit / 立体声扬声器。
 *
 * 进入/退出时机：
 *   - 模式页面进入时调用 usb_audio_mode_enter()，worker 开始等待 VBUS；
 *   - 只有 PMIC 报告 VBUS good（有线插入）时才初始化/连接 TinyUSB；
 *   - 主机完成枚举（tud_ready()，即收到 SET_CONFIGURATION）才算“握手成功”，
 *     此时才暂停本地音乐、打开 PCM 会话、把充电电流降到 300mA；
 *   - 拔线或退出模式时关闭 PCM、恢复 800mA、软断开 USB。
 *
 * 所有函数只设置 RAM 状态/任务通知，不等待 I2C、USB 或音频；可在 LVGL
 * 锁内调用。真正的 TinyUSB/USB PHY 初始化和音频会话由独立 worker 完成。
 */

#ifndef USB_AUDIO_H
#define USB_AUDIO_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/** 创建 USB 声卡 worker（不初始化 USB；进入模式且检测到 VBUS 后才初始化）。 */
esp_err_t usb_audio_init(void);

/** 进入 USB 声卡模式：等待 VBUS 与主机握手。 */
void usb_audio_mode_enter(void);

/** 退出 USB 声卡模式：停止会话、恢复充电电流并软断开 USB。 */
void usb_audio_mode_leave(void);

/** 主机已枚举完成（RAM 原子快照，供日志/界面查询）。 */
bool usb_audio_host_ready(void);

/**
 * 声卡页的 USB 线状态（RAM，不访问 I2C）：
 * 0 = 没有检测到 VBUS（未插线）；1 = 有 VBUS 但主机尚未枚举；
 * 2 = 主机已枚举（可播放）。供界面提示“请插入 USB 数据线/等待主机”。
 */
uint8_t usb_audio_cable_status(void);

#ifdef __cplusplus
}
#endif

#endif /* USB_AUDIO_H */
