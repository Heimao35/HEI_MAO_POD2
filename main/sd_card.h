#ifndef SD_CARD_H
#define SD_CARD_H

#include "esp_err.h"
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define SD_MOUNT_POINT "/sdcard"
#define SD_SYS_DIR     SD_MOUNT_POINT "/HEI_MAO_POD_SYS"

esp_err_t sd_card_init(void);
bool      sd_card_is_mounted(void);
void      sd_card_poll_detect(void);

#ifdef __cplusplus
}
#endif

#endif /* SD_CARD_H */
