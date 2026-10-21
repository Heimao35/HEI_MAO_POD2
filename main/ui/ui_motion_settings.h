/**
 * @file ui_motion_settings.h
 * @brief 姿态感应设置子页（步计/抬手亮屏开关）。
 *
 * UI 通过 RAM 请求接入 BMI270 后台驱动：
 * - 本页不访问 I2C、不加载配置；GPIO 中断由独立驱动负责；
 * - 两个开关保存本次运行的选择，变化时向后台投递请求；
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
 * @note  仅为本次运行的 RAM 状态，尚未持久化；只表示用户意图，
 *        后台异步应用到芯片。默认关闭。
 */
bool ui_motion_settings_steps_enabled(void);

/**
 * @brief 读取“抬手亮屏”开关状态。
 * @note  同上；真实唤醒必须由 BMI270 状态位确认，不把引脚电平直接当手势。
 */
bool ui_motion_settings_wake_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MOTION_SETTINGS_H */
