/**
 * @file ui_statusbar.h
 * @brief 状态栏模块 —— 屏幕最上方的半透明黑色状态栏
 *
 * 显示内容（从左到右）：
 *   左侧：WiFi / 蓝牙 / 耳机 / SD卡 状态图标
 *   中间：时间（24小时制）
 *   右侧：电量百分比 + 电池图标（含充电闪电符号）
 *
 * 状态栏在所有页面上置顶显示，唯独设置页面展开时隐藏。
 * 锁屏页面下状态栏可见但不显示时间。
 */

#ifndef UI_STATUSBAR_H
#define UI_STATUSBAR_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建状态栏并添加到指定父对象上
 * @param parent 父对象，通常是主屏幕 (lv_screen_active())
 * @note  状态栏创建后自动置顶 (z-order 最高)
 */
void ui_statusbar_create(lv_obj_t *parent);

/**
 * @brief 根据全局设备状态刷新状态栏所有指示符和时间显示
 * @note  应在定时器回调中周期性调用（建议 1 秒一次）
 */
void ui_statusbar_update(void);

/**
 * @brief 设置状态栏可见性
 * @param visible true=显示, false=隐藏（设置页面展开时调用 false）
 */
void ui_statusbar_set_visible(bool visible);

/**
 * @brief 设置状态栏是否显示时间（锁屏页面下隐藏时间）
 * @param show true=显示时间, false=隐藏时间
 */
void ui_statusbar_show_time(bool show);

/**
 * @brief 获取状态栏对象指针（用于手势检测判断触摸起点是否在状态栏区域）
 * @return 状态栏 lv_obj_t 指针
 */
lv_obj_t *ui_statusbar_get_obj(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_STATUSBAR_H */
