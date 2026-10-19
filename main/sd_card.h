#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>
#include "freertos/FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif

#ifndef SD_MOUNT_POINT
#define SD_MOUNT_POINT "/sdcard"
#endif
#define SD_SYS_DIR     SD_MOUNT_POINT "/HEI_MAO_POD_SYS"

esp_err_t sd_card_init(void);
bool      sd_card_is_mounted(void);
bool      sd_card_is_present(void);
uint32_t  sd_card_get_generation(void);
/* Media worker must hold this lock across ALL fopen/read/close operations.
 * Never acquire it from the LVGL task. Unmount waits until handles are closed. */
bool      sd_card_io_lock(TickType_t timeout);
void      sd_card_io_unlock(void);
void      sd_card_poll_detect(void);
/* Request a detect-task-owned remount after an SD I/O timeout.
 * The next mount uses the conservative SD clock. */
void      sd_card_request_safe_remount(void);
/* 一次注册；卸载持有SD锁时关闭音频流。回调内不可重新取锁或访问UI。 */
esp_err_t sd_card_set_audio_detach_callback(void (*callback)(void));

#ifdef __cplusplus
}
#endif

#endif /* SD_CARD_H */
