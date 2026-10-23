/**
 * @file ui_settings.h
 * @brief 设置页面 —— 从屏幕上边缘下滑展开，占满全屏
 *
 * 页面内放置圆形按钮（两列排列），可上下滚动。
 * 当前按钮：WiFi设置、蓝牙、姿态感应设置、时钟设置、锁屏页面设置。
 * - 蓝牙：短按切换蓝牙开关；长按进入蓝牙设置子页（开关 + 扫描列表）；
 * - 姿态感应/时钟：点击进入各自的独立子页；
 * - 其余条目保留原入口/占位行为。
 * 点击按钮可进入对应详细设置子页面；在屏幕下边缘上滑可收起设置页面。
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

/**
 * @brief 按 bt_audio 的电源意图刷新蓝牙入口按钮的开/关外观。
 *
 * 开启时按钮反白、关闭时恢复深灰；只改颜色，不改尺寸、网格、手势或
 * 图标字形。设置页短按、设置页展开以及蓝牙子页关闭后调用；状态未变化
 * 时不重复设置样式。调用方需持有 LVGL 锁。
 */
void ui_settings_refresh_bluetooth(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_SETTINGS_H */
