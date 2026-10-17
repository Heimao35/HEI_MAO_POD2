#pragma once
#include "esp_err.h"
/* Starts the single SD media worker and binds the UI. No DAC/decoder is started. */
esp_err_t music_player_adapter_init(void);
