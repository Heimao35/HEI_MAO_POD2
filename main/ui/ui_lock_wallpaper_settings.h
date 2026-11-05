/**
 * @file ui_lock_wallpaper_settings.h
 * @brief “锁屏和壁纸”设置子页：防误触开关、锁屏设置占位、壁纸设置入口。
 *
 * 页面样式与时钟/姿态感应子页一致：从屏幕右侧滑入，左上角箭头滑出，
 * 不改变设置主页的网格、手势和状态栏生命周期。
 */
#ifndef UI_LOCK_WALLPAPER_SETTINGS_H
#define UI_LOCK_WALLPAPER_SETTINGS_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建设置子页（不访问硬件/SD；开关默认关闭，由 restore 套用 NVS）。 */
void ui_lock_wallpaper_settings_create(lv_obj_t *parent);

/** @brief 从右侧滑入；由设置主页“锁屏和壁纸”入口调用。 */
void ui_lock_wallpaper_settings_show(void);

/** @brief 从当前位置滑出到屏幕右侧。 */
void ui_lock_wallpaper_settings_hide(void);

/** @brief 锁屏前收起；不保存、不等待后台操作。 */
void ui_lock_wallpaper_settings_close_for_lock(void);

/** @brief 子页是否可见（ui_manager 用于阻断下层页面手势）。 */
bool ui_lock_wallpaper_settings_is_visible(void);

/**
 * @brief 套用持久化的防误触开关意图。
 *
 * 开机在 ui_manager_init() 之后、同一把 LVGL 锁内调用；只更新开关
 * 视觉和 screen_power 的原子标志，不做 Flash/I2C/SD。开关视觉恢复
 * 不会触发 LV_EVENT_VALUE_CHANGED，因此不会重复写设置。
 *
 * @param anti_touch true 表示打开防误触（熄屏时触摸不唤醒）。
 */
void ui_lock_wallpaper_settings_restore(bool anti_touch);

/** @brief 读取当前防误触意图；player_settings_capture() 在 LVGL 锁内读取。 */
bool ui_lock_wallpaper_settings_anti_touch_enabled(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_LOCK_WALLPAPER_SETTINGS_H */
