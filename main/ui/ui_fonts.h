#ifndef UI_FONTS_H
#define UI_FONTS_H

#include "lvgl.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/**
 * Mount the embedded font partition and load each custom font once.
 *
 * This must be called after LVGL has been initialized and before UI objects
 * that use these custom fonts are created. A built-in-font boot layer is safe
 * before this call. The returned font pointers remain valid for the whole process
 * lifetime; UI objects must not keep them after a future unload is introduced.
 */
esp_err_t ui_fonts_init(void);

/**
 * 可选的同步进度回调：在每个文件加载前报告已完成的尝试数，全部处理完
 * 后 filename=NULL。total 为字库文件总数；失败后采用内置字体也算该阶段
 * 完成。它不报告单个文件内的字节/耗时百分比。
 * 回调在调用者的 LVGL 锁内执行，禁止再次等待锁或转交并发 LVGL 操作。
 */
typedef void (*ui_fonts_progress_cb_t)(unsigned completed, unsigned total,
                                      const char *filename, void *user_data);
esp_err_t ui_fonts_init_with_progress(ui_fonts_progress_cb_t progress, void *user_data);

/** Custom fonts with an LVGL same-size fallback already attached. */
const lv_font_t *ui_font_14(void);
const lv_font_t *ui_font_16(void);
const lv_font_t *ui_font_18(void);
const lv_font_t *ui_font_22(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_FONTS_H */
