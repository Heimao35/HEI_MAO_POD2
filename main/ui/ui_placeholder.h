/**
 * @file ui_placeholder.h
 * @brief USB 声卡 / 拍照模式的占位页面
 *
 * 本轮只新增模式入口和占位页，不实现实际功能：
 *   - 不枚举/配置 USB 设备，不启动 UAC 音频，也不改动现有 CS43131 输出；
 *   - 不初始化摄像头，不占用硬件 JPEG 解码器。
 *
 * 两个页面与音乐/视频/图片/电子书页面同级：全屏、创建后默认隐藏，
 * 由 ui_manager 统一负责上下滑动切换、锁屏遮挡和手势判定。页面本身
 * 只显示一个图标、模式名称和“功能开发中”提示，不处理任何触摸事件
 * （EVENT_BUBBLE 保留，边缘手势仍由主屏幕捕获）。
 *
 * 以后实现真实功能时，建议把对应模式换成独立模块（例如 ui_usb_audio.c、
 * ui_camera.c），并让 ui_manager 改调那个模块的 create/get_page；届时
 * 删除本文件即可，模式枚举和模式切换逻辑不需要再改动。
 */

#ifndef UI_PLACEHOLDER_H
#define UI_PLACEHOLDER_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建 USB 声卡和拍照两个占位页面（默认隐藏）
 *
 * 必须在 ui_manager_init() 中、状态栏之前调用，以保持既有的 z-order：
 * 占位页位于壁纸层/其它模式页之上、锁屏和面板之下。
 *
 * @param parent 父对象（LVGL 主屏幕）
 */
void ui_placeholder_create(lv_obj_t *parent);

/**
 * @brief 获取指定模式的占位页对象
 *
 * @param mode 只接受 UI_MODE_USB_AUDIO / UI_MODE_CAMERA
 * @return 页面对象；其它模式或尚未创建时返回 NULL
 */
lv_obj_t *ui_placeholder_get_page(ui_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* UI_PLACEHOLDER_H */
