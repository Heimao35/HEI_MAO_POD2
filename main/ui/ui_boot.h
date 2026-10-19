#ifndef UI_BOOT_H
#define UI_BOOT_H

#include "lvgl.h"
#include <stdint.h>

/**
 * 临时开机提示层，完全使用固件内置字体，不依赖 fontfs/SD。
 * 所有接口均须在 LVGL 锁内调用；show/progress 会主动刷新，确保下一段
 * 同步初始化长时间持锁时，用户仍能看到最近一次启动状态。
 * 此模块不创建定时器、动画、任务或额外帧缓冲。
 */
void ui_boot_show(lv_display_t *display);
/** 阶段进度 0..100，单调递增；stage 是英文状态，内部复制其内容。 */
void ui_boot_set_progress(uint8_t progress, const char *stage);
/** 删除临时对象并刷新既有界面；不修改其布局、手势或动画。 */
void ui_boot_hide(void);

#endif
