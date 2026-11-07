/**
 * @file ui_placeholder.h
 * @brief 拍照模式的占位页面
 *
 * 本轮只提供模式入口和占位页，不初始化摄像头、不占用硬件 JPEG 解码器。
 * USB 声卡模式已由独立模块 ui_usb_audio.c 实现，不再使用本模块。
 *
 * 页面与音乐/视频/图片/电子书页面同级：全屏、创建后默认隐藏，由
 * ui_manager 统一负责上下滑动切换、锁屏遮挡和手势判定。页面本身只显示
 * 相机图标、模式名称和“功能开发中”提示，不处理任何触摸事件（保留
 * EVENT_BUBBLE，边缘手势仍由主屏幕捕获）。
 *
 * 以后实现真实功能时，建议把该模式换成独立模块（例如 ui_camera.c）并让
 * ui_manager 改调那个模块的 create/get_page；届时删除本文件即可，模式
 * 枚举和模式切换逻辑不需要再改动。
 */

#ifndef UI_PLACEHOLDER_H
#define UI_PLACEHOLDER_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * @brief 创建拍照占位页面（默认隐藏）
 *
 * 必须在 ui_manager_init() 中、状态栏之前调用，以保持既有的 z-order：
 * 占位页位于壁纸层/其它模式页之上、锁屏和面板之下。
 *
 * @param parent 父对象（LVGL 主屏幕）
 */
void ui_placeholder_create(lv_obj_t *parent);

/**
 * @brief 获取拍照占位页对象
 *
 * @param mode 只接受 UI_MODE_CAMERA
 * @return 页面对象；其它模式或尚未创建时返回 NULL
 */
lv_obj_t *ui_placeholder_get_page(ui_mode_t mode);

#ifdef __cplusplus
}
#endif

#endif /* UI_PLACEHOLDER_H */
