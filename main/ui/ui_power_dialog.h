#pragma once
#include "ui_common.h"
#include "../system_power.h"
/* 所有接口都必须持有 LVGL 锁；对话框独立挂在 top layer，不移动原页面。 */
void ui_power_dialog_show(void);
void ui_power_dialog_hide(void);
void ui_power_dialog_set_status(const char *text, bool busy);
bool ui_power_dialog_is_visible(void);
