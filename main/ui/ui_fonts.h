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
 * are created. The returned font pointers remain valid for the whole process
 * lifetime; UI objects must not keep them after a future unload is introduced.
 */
esp_err_t ui_fonts_init(void);

/** Custom fonts with an LVGL same-size fallback already attached. */
const lv_font_t *ui_font_14(void);
const lv_font_t *ui_font_16(void);
const lv_font_t *ui_font_18(void);
const lv_font_t *ui_font_22(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_FONTS_H */
