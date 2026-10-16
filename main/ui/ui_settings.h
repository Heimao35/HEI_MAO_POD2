/**
 * @file ui_settings.h
 * @brief 设置页面 —— 从屏幕上边缘下滑展开，占满全屏
 *
 * 页面内放置圆形按钮（两列排列），可上下滚动。
 * 当前按钮：WiFi设置、蓝牙设置、姿态感应设置、时钟设置、锁屏页面设置。
 * 点击按钮可进入对应详细设置子页面（当前为占位框架）。
 * 在屏幕下边缘上滑可收起设置页面。
 * 设置页面展开时状态栏隐藏。
 */

#ifndef UI_SETTINGS_H
#define UI_SETTINGS_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建设置页面 */
void ui_settings_create(lv_obj_t *parent);

/** @brief 展开设置页面（从顶部下滑动画） */
void ui_settings_show(void);

/** @brief 收起设置页面（向顶部上滑动画） */
void ui_settings_hide(void);

/** @brief 查询设置页面是否展开 */
bool ui_settings_is_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_SETTINGS_H */
