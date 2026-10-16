/**
 * @file ui_mode_panel.h
 * @brief 模式切换栏 —— 从屏幕左侧边缘右滑展开
 *
 * 展开后占屏幕左侧约 1/4 宽度，纵向排列圆角矩形按钮，
 * 每个按钮代表一个模式（音乐/视频/图片/电子书）。
 * 可上下滚动，点击按钮切换模式，在栏外点击或左滑收起。
 */

#ifndef UI_MODE_PANEL_H
#define UI_MODE_PANEL_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** 模式切换回调类型，参数为目标模式枚举值 */
typedef void (*ui_mode_switch_cb_t)(ui_mode_t mode);

/**
 * @brief 创建模式切换栏
 * @param parent 父对象（主屏幕）
 */
void ui_mode_panel_create(lv_obj_t *parent);

/**
 * @brief 展开模式切换栏（带从左滑入动画）
 */
void ui_mode_panel_show(void);

/**
 * @brief 收起模式切换栏（带向左滑出动画）
 */
void ui_mode_panel_hide(void);

/**
 * @brief 查询模式切换栏是否展开
 * @return true=已展开
 */
bool ui_mode_panel_is_visible(void);

/**
 * @brief 注册模式切换回调（用户点击按钮后触发）
 * @param cb 回调函数，参数为目标 ui_mode_t
 */
void ui_mode_panel_set_callback(ui_mode_switch_cb_t cb);

#ifdef __cplusplus
}
#endif

#endif /* UI_MODE_PANEL_H */
