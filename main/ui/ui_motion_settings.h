/**
 * @file ui_motion_settings.h
 * @brief 姿态感应设置子页（步计/抬手亮屏开关）。
 *
 * UI 通过 RAM 请求接入 BMI270 后台驱动：
 * - 本页不访问 I2C、不加载配置；GPIO 中断由独立驱动负责；
 * - 两个开关维护用户意图，变化时向后台投递请求；开关值由现有
 *   player_settings 快照链路（电源 worker 3 秒合并保存）写入 NVS，
 *   本页自身不做任何 Flash I/O；
 * - 开机由 main 在 ui_manager_init() 之后调用 ui_motion_settings_restore()
 *   把 NVS 中的选择套用到开关视觉和传感器请求；
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

/**
 * @brief 开机套用 NVS 中保存的开关意图。
 * @param steps 计步开关目标状态
 * @param wake  抬手亮屏开关目标状态
 * @note  必须在 ui_motion_settings_create() 之后、持有 LVGL 锁时调用。
 *        只更新开关视觉和 RAM 意图，并像用户点击一样投递一次合并请求，
 *        不进行 I2C/Flash 操作；程序化修改选中状态不会触发
 *        LV_EVENT_VALUE_CHANGED，因此不会重复进入点击回调。
 */
void ui_motion_settings_restore(bool steps, bool wake);

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
 * @brief 读取“计步”开关的用户意图。
 * @note  返回 RAM 状态；变化后由电源 worker 的快照链路合并写入 NVS，
 *        只表示用户意图，后台异步应用到芯片。无保存值时默认关闭。
 */
bool ui_motion_settings_steps_enabled(void);

/**
 * @brief 读取“抬手亮屏”开关的用户意图。
 * @note  同上；真实唤醒必须由 BMI270 状态位确认，不把引脚电平直接当手势。
 */
bool ui_motion_settings_wake_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_MOTION_SETTINGS_H */
