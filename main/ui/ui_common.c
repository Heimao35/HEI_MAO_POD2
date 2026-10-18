/**
 * @file ui_common.c
 * @brief UI 公共实现 —— 全局设备状态单例、主屏幕获取
 */

#include "ui_common.h"
#include <string.h>

/** 全局设备状态（单例），初始值为全零 / false */
static ui_device_state_t s_device_state = {
    .wifi_connected    = false,
    .bt_connected      = false,
    .headphone_inserted = false,
    .sd_card_inserted  = false,
    .charging          = false,
    .power_valid       = false,
    .battery_present   = false,
    .battery_pct_valid = false,
    .vbus_present      = false,
    .battery_pct       = 0,
    .brightness        = 80,
    .manual_brightness = 80,
    .volume            = 50,
    .auto_brightness   = false,
    .hour              = 12,
    .minute            = 0,
    .year              = 2026,
    .month             = 1,
    .day               = 1,
};

/**
 * @brief 获取全局设备状态指针
 * @return 指向静态全局 ui_device_state_t 的指针，所有模块共享
 */
ui_device_state_t *ui_get_device_state(void)
{
    return &s_device_state;
}

/**
 * @brief 获取 LVGL 当前活动屏幕
 * @return lv_obj_t* 当前屏幕对象
 */
lv_obj_t *ui_get_screen(void)
{
    return lv_screen_active();
}
