#pragma once
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef struct {
    uint64_t file_size;
    uint64_t total_samples;
    uint64_t duration_ms;
    uint32_t sample_rate;
    uint8_t bits_per_sample;
    uint8_t channels;
    bool is_flac;
    bool metadata_known;
} audio_file_info_t;

/* Metadata only: opens/closes the file, never decodes or loads audio payload.
 * FLAC STREAMINFO uses 64-bit sample counts and retains 32-bit/192-kHz formats.
 * MP3 uses Xing/VBRI frame counts when present, otherwise its real header bitrate
 * provides an estimate; DMA sample counts provide the actual playback position.
 * Call on the media worker while holding the SD I/O lock. */
esp_err_t audio_player_probe(const char *path, audio_file_info_t *info);
