/**
 * @file ui_lockscreen.h
 * @brief 锁屏页面模块
 *
 * 上电默认展示此页面。用户在页面任意位置上滑可解锁退出。
 * 锁屏页面显示：
 *   - 用户设置的背景图片（后续可能支持 MJPEG 动态背景）
 *   - 大字体时间（中上方）
 *   - 小字体日期（时间下方）
 *   - 状态栏仍显示但不显示时间
 * 锁屏页面下只允许打开声光调节栏，不允许打开模式切换栏和设置页面。
 * 可通过短按电源键重新进入锁屏。
 */

#ifndef UI_LOCKSCREEN_H
#define UI_LOCKSCREEN_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建锁屏页面
 * @param parent 父对象（通常是主屏幕）
 */
void ui_lockscreen_create(lv_obj_t *parent);

/**
 * @brief 刷新锁屏页面的时间和日期显示
 * @note  从全局设备状态读取时间，建议每秒调用一次
 */
void ui_lockscreen_update(void);

/**
 * @brief 显示锁屏页面（带滑入动画）
 */
void ui_lockscreen_show(void);

/**
 * @brief 隐藏锁屏页面（带滑出动画）
 */
void ui_lockscreen_hide(void);

/**
 * @brief 查询锁屏页面是否当前可见
 * @return true=锁屏页面正在显示
 */
bool ui_lockscreen_is_visible(void);

/**
 * @brief 启动锁屏 MJPEG 开机动画（SD 卡挂载后调用）
 */
void ui_lockscreen_start_boot_anim(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_LOCKSCREEN_H */
