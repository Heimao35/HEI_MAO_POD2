#pragma once

#include "ui_common.h"
#include "../firmware_update.h"

/**
 * @file ui_fw_update_dialog.h
 * @brief 开机固件更新的确认/进度/失败提示框。
 *
 * 视觉完全沿用 ui_power_dialog：top layer 60% 黑色遮罩、382x204 深灰卡片、
 * 18px 中文字体、166x44 半透明白色按键和统一按下反馈。只在本轮开机出现，
 * 不改变设置页、锁屏、播放器等任何既有页面的布局/手势/动画。
 *
 * 所有接口都必须在 LVGL 锁内调用。对话框自己的 100ms 定时器只做原子状态
 * 轮询、标签/进度条刷新和熄屏活动重置，不执行 SD/Flash/I2C 操作。
 */

/** 发现新固件：显示当前/新版本、大小和“确认/取消”两个按键。 */
void ui_fw_update_dialog_show_prompt(const fw_update_info_t *info);
/** 用户确认后立即切到进度视图，随后由定时器读取真实百分比。 */
void ui_fw_update_dialog_show_progress(void);
/** 更新失败：显示错误文本和“返回”按键。 */
void ui_fw_update_dialog_show_error(const char *text);
/** 更新成功：显示“正在重启”，不提供按键。 */
void ui_fw_update_dialog_show_success(void);
/** 更新成功但 PMIC 自动重启失败：提示手动重启并提供“返回”。 */
void ui_fw_update_dialog_show_manual_restart(void);
/** 删除遮罩、卡片和定时器；可重复调用。 */
void ui_fw_update_dialog_hide(void);
