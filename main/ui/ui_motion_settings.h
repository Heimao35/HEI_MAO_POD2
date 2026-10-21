/**
 * @file ui_motion_settings.h
 * @brief 姿态感应设置子页（步计/抬手亮屏开关）。
 *
 * 当前只实现 UI 和开关的 RAM 状态，尚未接入 BMI270 驱动：
 * - 本页不访问 I2C、不加载配置、不产生任何传感器中断；
 * - 两个开关仅保存本次运行的选择，供后续姿态模块查询；
 * - 页面沿用时钟设置子页的右侧 350ms ease-out 进出、左上箭头返回，
 *   不改设置主页、状态栏和现有手势。
 *
 * 所有接口都要求调用方持有 LVGL 锁。
 */

#ifndef UI_MOTION_SETTINGS_H
#define UI_MOTION_SETTINGS_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建姿态感应设置子页（初始隐藏，位于主屏幕内）。 */
void ui_motion_settings_create(lv_obj_t *parent);

/** @brief 从右侧滑入子页。重复调用或页面已可见时不重播动画。 */
void ui_motion_settings_show(void);

/** @brief 从当前位置滑出到右侧；完成后再隐藏页面。 */
void ui_motion_settings_hide(void);

/**
 * @brief 进入锁屏前收起子页，不阻塞原有锁屏动画。
 * @note  与 ui_clock_settings_close_for_lock() 的职责一致；姿态页没有
 *        需要同步保存的数据，只做位移动画。
 */
void ui_motion_settings_close_for_lock(void);

/** @brief 子页是否可见（用于 ui_manager 拦截下层页面手势）。 */
bool ui_motion_settings_is_visible(void);

/**
 * @brief 读取“计步”开关状态。
 * @note  仅为本次运行的 RAM 状态，尚未持久化；后续 BMI270 模块在初始化
 *        或启用计步功能时读取。默认关闭。
 */
bool ui_motion_settings_steps_enabled(void);

/**
 * @brief 读取“抬手亮屏”开关状态。
 * @note  同上；抬手亮屏的具体策略尚未实现，本接口只发布用户选择。
 */
bool ui_motion_settings_wake_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MOTION_SETTINGS_H */
