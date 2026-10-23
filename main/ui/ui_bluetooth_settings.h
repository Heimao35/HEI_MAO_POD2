/**
 * @file ui_bluetooth_settings.h
 * @brief 蓝牙设置子页：电源开关 + 可滚动设备列表。
 *
 * 页面与时钟/姿态感应设置子页同风格：
 * - 全屏深色页，初始位于屏幕右侧并隐藏，show()/hide() 做 350ms ease-out
 *   水平位移动画，左上角 44x44 箭头返回；
 * - 只是设置主页的视觉子页，不改变设置主页的网格、顶部展开/底部收起手势，
 *   也不改变状态栏：状态栏显隐仍由设置主页负责；
 * - 页面不冒泡事件/手势，返回动画或列表拖动不会触发下层设置页收起。
 *
 * 数据来源与持久化：
 * - 开关、扫描结果和连接状态全部来自 bt_audio.h 的应用层接口，本页只
 *   轮询显示并投递用户请求；不直接访问 SDIO、Bluetooth 协议栈或 NVS。
 * - 蓝牙开关状态按用户要求不写入 NVS，上电默认关闭；本页只维护本次
 *   运行的开关位置。
 * - 进入页面时请求开始搜索；离开页面时请求停止搜索。设备名使用 18 号
 *   全字库，长名称以省略号截断。
 *
 * 所有接口都要求调用方持有 LVGL 锁。
 */

#ifndef UI_BLUETOOTH_SETTINGS_H
#define UI_BLUETOOTH_SETTINGS_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建蓝牙设置子页（初始隐藏，位于主屏幕内）。 */
void ui_bluetooth_settings_create(lv_obj_t *parent);

/** @brief 从右侧滑入子页，并请求开始搜索。重复调用不会重播动画。 */
void ui_bluetooth_settings_show(void);

/** @brief 停止搜索并从当前位置滑出到右侧；隐藏完成后清可见标志。 */
void ui_bluetooth_settings_hide(void);

/**
 * @brief 进入锁屏前收起子页。
 * @note  与时钟/姿态子页一致，只做退出动画并停止搜索；蓝牙连接本身
 *        不因锁屏断开，音乐可以继续在后台播放。
 */
void ui_bluetooth_settings_close_for_lock(void);

/** @brief 子页是否可见（用于 ui_manager 拦截下层页面手势）。 */
bool ui_bluetooth_settings_is_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_BLUETOOTH_SETTINGS_H */
