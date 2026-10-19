#pragma once
#include "ui_common.h"

/* All UI entry points require the LVGL lock. Hardware writes are queued only. */
void ui_clock_settings_create(lv_obj_t *parent);
void ui_clock_settings_show(void);
bool ui_clock_settings_is_visible(void);
/* Publishes the RAM RTC snapshot to common UI time; also handles save results. */
void ui_clock_settings_update(void);
/* Physical lock entry also counts as leaving: enqueue edited time, then hide
 * without delaying the original lock-screen slide/replay. */
void ui_clock_settings_close_for_lock(void);
