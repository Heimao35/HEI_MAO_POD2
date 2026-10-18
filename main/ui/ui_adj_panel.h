/**
 * @file ui_adj_panel.h
 * @brief 声光调节栏 —— 从屏幕右侧边缘左滑展开
 *
 * 展开后占屏幕右侧 1/2 宽度，透明背景。
 * 左滑条：亮度调节 + 上方亮度图标 + 下方自动亮度按钮
 * 右滑条：音量调节 + 上方音量图标
 *
 * 手势展开时在栏外点击或右滑可收起。锁屏页面下也可打开此栏。
 * 实体音量键展开为无亮度/无遮罩的单列，不拦截下层触摸和页面手势；
 * 按键/滑条/静音按钮五秒无操作后自动收起，持续拖动时不计超时。
 */

#ifndef UI_ADJ_PANEL_H
#define UI_ADJ_PANEL_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建声光调节栏
 * @param parent 父对象（主屏幕）
 */
void ui_adj_panel_create(lv_obj_t *parent);

/** @brief 展开声光调节栏（从右侧滑入） */
void ui_adj_panel_show(void);

/** @brief 实体音量键：增减 0..100 音量，展开非模态单列，五秒无音量操作收起。
 * 必须持有 LVGL 锁；此调用不执行 GPIO/I2C/文件操作。 */
void ui_adj_panel_adjust_volume(int delta);

/** @brief 仅手势完整面板阻止页面手势，按键单列允许下层正常交互。 */
bool ui_adj_panel_is_modal(void);

/** @brief 收起声光调节栏（向右侧滑出） */
void ui_adj_panel_hide(void);

/** @brief 查询是否展开 */
bool ui_adj_panel_is_visible(void);

/** @brief 用当前设备状态刷新滑条位置和图标 */
void ui_adj_panel_update(void);

/** 亮度变更回调类型，参数为新亮度 0-100 */
typedef void (*ui_brightness_cb_t)(uint8_t brightness);
/** 音量变更回调类型，参数为新音量 0-100 */
typedef void (*ui_volume_cb_t)(uint8_t volume);

/**
 * @brief 注册亮度和音量变更回调
 * @param br_cb 亮度回调（用户拖动亮度条时触发）
 * @param vol_cb 音量回调（用户拖动音量条时触发）
 */
void ui_adj_panel_set_callbacks(ui_brightness_cb_t br_cb, ui_volume_cb_t vol_cb);

#ifdef __cplusplus
}
#endif

#endif /* UI_ADJ_PANEL_H */
