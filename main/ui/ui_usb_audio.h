/**
 * @file ui_usb_audio.h
 * @brief USB 声卡模式页面
 *
 * 页面背景与视频/图片/电子书首页一致：本页自身透明，由 ui_manager 创建
 * 的固定壁纸层提供背景。页面中央有一条与屏幕等宽的控制台，显示模式名称
 * 和固定音频规格（96kHz/24bit）；点击页面任意位置切换控制台显示/隐藏，
 * 动画时长与曲线沿用视频播放控制台（220ms ease-out 透明度渐变）。
 *
 * 页面的显示/隐藏由 ui_manager 的模式切换控制；进入/离开模式时转交给
 * usb_audio_mode_enter()/usb_audio_mode_leave() 管理 USB 与音频会话。
 */

#ifndef UI_USB_AUDIO_H
#define UI_USB_AUDIO_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 创建 USB 声卡页面（默认隐藏，控制台默认可见）。 */
void ui_usb_audio_create(lv_obj_t *parent);

/** 获取页面对象，供 ui_manager 放入模式页数组。 */
lv_obj_t *ui_usb_audio_get_page(void);

/** 进入模式：开始等待 VBUS 与主机握手。在 LVGL 锁内调用，只投递通知。 */
void ui_usb_audio_on_mode_enter(void);

/** 离开模式：停止 USB 音频、恢复充电电流并软断开。只投递通知。 */
void ui_usb_audio_on_mode_leave(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_USB_AUDIO_H */
