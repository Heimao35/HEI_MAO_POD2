/**
 * @file ui_lockscreen.h
 * @brief 锁屏页面模块
 *
 * 上电默认展示此页面。用户在页面任意位置上滑可解锁退出。
 * 锁屏页面显示：
 *   - Flash持久保存、开机载入只读缓存的MJPEG动态背景
 *   - 大字体时间（中上方）
 *   - 小字体日期（时间下方）
 *   - 状态栏仍显示但不显示时间
 * 锁屏页面下只允许打开声光调节栏，不允许打开模式切换栏和设置页面。
 * 可通过短按电源键重新进入锁屏。
 */

#ifndef UI_LOCKSCREEN_H
#define UI_LOCKSCREEN_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建锁屏页面
 * @param parent 父对象（通常是主屏幕）
 */
void ui_lockscreen_create(lv_obj_t *parent);

/**
 * @brief 刷新锁屏页面的时间和日期显示
 * @note  从全局设备状态读取时间，建议每秒调用一次
 */
void ui_lockscreen_update(void);

/**
 * @brief 发布当天的步数到锁屏右下角。
 * @param valid false 表示尚无有效传感器数据，显示“-- 步”，不伪造 0。
 * @param steps 当天步数（不是上电以来的累计值）。
 * @note  调用方必须持有 LVGL 锁。日期与跨日清零由 BMI270 后台统一
 *        管理，UI 不维护第二个跨日计数器；熄屏时也继续每日换算。
 */
void ui_lockscreen_set_steps(bool valid, uint32_t steps);

/**
 * @brief 发布温度到步数上方，右对齐显示。
 * @param valid false 表示无有效温度，显示“--℃”。
 * @param temp_c_x10 摄氏度×10（例如 235 表示 23.5℃，-52 表示 -5.2℃）。
 * @note  调用方必须持有 LVGL 锁。当前只做显示，不读取 I2C；BMI270 的
 *        片内温度传感器测的是芯片结温，不是环境温度，有效性由发布方判断。
 */
void ui_lockscreen_set_temperature(bool valid, int16_t temp_c_x10);

/**
 * @brief 进入锁屏或恢复已有锁屏，并从头重播 MJPEG。
 * @note  仅从已隐藏的锁屏进入时播放页面滑入；亮屏唤醒不重复滑入。
 *        调用方须持有 LVGL 锁。
 */
void ui_lockscreen_show(void);

/**
 * @brief 隐藏锁屏页面（带滑出动画）
 */
void ui_lockscreen_hide(void);

/**
 * @brief 查询锁屏页面是否当前可见
 * @return true=锁屏页面正在显示
 */
bool ui_lockscreen_is_visible(void);

/**
 * @brief 从头重播锁屏 MJPEG 动画（开机Flash资源就绪、再次锁屏、熄屏唤醒均调用）
 */
void ui_lockscreen_start_boot_anim(void);
/** 取消旧代次，不等待 I/O；电源任务在锁外等待 idle 后才允许浅睡眠。 */
void ui_lockscreen_stop_anim(void);
bool ui_lockscreen_anim_is_idle(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_LOCKSCREEN_H */
