/**
 * @file ui_wallpaper_picker.h
 * @brief 壁纸选择页：复用图片查看器“其他图片”的四列缩略图浏览方式。
 *
 * 页面行为：
 * - 从“锁屏和壁纸 → 壁纸设置”进入，从右侧滑入；
 * - 四列 100px 缩略图、上下滚动、8 行环形窗口，视觉与图片查看器总览
 *   完全相同；状态栏保持可见（与图片总览一致），网格从状态栏下方开始；
 * - 右滑返回“锁屏和壁纸”子页（与图片总览右滑返回首页一致）；
 * - 点击缩略图打开全屏图片，不显示控制台，只在图片下方左右两侧显示
 *   “取消 / 确认”两个圆角矩形按键；确认后把当前图片写入 Flash 壁纸槽，
 *   之后视频列表、图片首页和电子书首页都会以它为背景，无 SD 卡也保留。
 *
 * 后台复用 image_viewer 的扫描/缩略图/全屏解码；打开本页时临时接管
 * 其 UI 回调，关闭时恢复图片查看器自己的回调。本模块不直接读 SD/JPEG。
 */
#ifndef UI_WALLPAPER_PICKER_H
#define UI_WALLPAPER_PICKER_H

#include "ui_common.h"

#ifdef __cplusplus
extern "C" {
#endif

/** @brief 创建页面与 100ms 状态轮询定时器（不访问 SD/Flash）。 */
void ui_wallpaper_picker_create(lv_obj_t *parent);

/** @brief 从右侧滑入并请求一次图片列表扫描。 */
void ui_wallpaper_picker_show(void);

/**
 * @brief 右滑返回“锁屏和壁纸”子页：交接后台回调并滑出。
 *
 * 只由本页的右滑手势调用；全屏预览中不会触发（需要先点“取消”）。
 */
void ui_wallpaper_picker_hide(void);

/** @brief 锁屏/设置页收起时立即关闭：取消在途解码并恢复图片查看器回调。 */
void ui_wallpaper_picker_close_for_lock(void);

/** @brief 页面是否可见（滑动退出期间仍为 true）。 */
bool ui_wallpaper_picker_is_visible(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_WALLPAPER_PICKER_H */
