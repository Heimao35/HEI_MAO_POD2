#pragma once
#include "esp_err.h"
/* Starts the single SD media worker and binds the UI. No DAC/decoder is started. */
esp_err_t music_player_adapter_init(void);

/* 请求当前挂载的未完成/失败曲库补扫。可在 UI 回调内调用；不执行 I/O，
 * 已成功扫描的曲库（包括空曲库）保持不变，由后台合并重复请求。 */
void music_player_adapter_request_scan_if_needed(void);
