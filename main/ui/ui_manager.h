/**
 * @file ui_manager.h
 * @brief UI 管理器 —— 页面切换、边缘手势检测、定时刷新
 *
 * 职责：
 *   1. 初始化所有 UI 模块并设定 z-order
 *   2. 检测屏幕边缘手势（左/右/上/下滑）并触发对应面板或页面
 *   3. 管理模式页面切换（带滑入滑出动画，只播放一次）
 *   4. 创建 LVGL 定时器周期刷新状态栏和锁屏时间
 */

#ifndef UI_MANAGER_H
#define UI_MANAGER_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 初始化整个 UI 系统
 *
 * 按顺序创建所有页面和面板，设定层级关系，
 * 注册手势检测和定时刷新。调用前需已完成 LVGL 初始化。
 */
void ui_manager_init(void);

/**
 * @brief 切换到指定模式页面（带动画）
 * @param mode 目标模式枚举值
 *
 * 当前页面滑出 + 目标页面滑入，动画只播放一次，
 * 不会经过中间模式页面。
 */
void ui_manager_switch_mode(ui_mode_t mode);

/**
 * @brief 获取当前活跃模式
 * @return 当前 ui_mode_t 枚举值
 */
ui_mode_t ui_manager_get_current_mode(void);

/**
 * @brief 进入锁屏（短按电源键时调用）
 */
void ui_manager_lock(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MANAGER_H */
