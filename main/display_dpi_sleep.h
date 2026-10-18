#pragma once
#include "esp_lcd_panel_ops.h"
/* Called only by the screen-power worker, after LVGL has stopped flushing.
 * Retains the panel, framebuffer addresses and LVGL objects across sleep. */
esp_err_t player_dpi_suspend(esp_lcd_panel_handle_t panel);
/* Resets the bridge FIFO/Host scan state, retains the selected framebuffer,
 * and waits for two fresh physical VSYNC events while brightness remains zero.
 * A timeout can be retried without resetting an already-running DMA stream. */
esp_err_t player_dpi_resume(esp_lcd_panel_handle_t panel);
