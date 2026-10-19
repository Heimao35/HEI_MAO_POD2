#pragma once
#include "ui_common.h"
#include "../system_power.h"
/* 所有接口都必须持有 LVGL 锁；对话框独立挂在 top layer，不移动原页面。 */
void ui_power_dialog_show(void);
/* 同一张卡片的低电模式：立即关机/关闭两键，关闭只隐藏，不取消计时。
 * seconds 来自电源任务，不另建 UI 定时器或重复创建控件。 */
void ui_power_dialog_show_low_battery(unsigned seconds);
void ui_power_dialog_hide(void);
void ui_power_dialog_set_status(const char *text, bool busy);
bool ui_power_dialog_is_visible(void);
