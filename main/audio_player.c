#include "audio_player.h"
#include <stdio.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

esp_err_t audio_player_probe(const char *path, audio_file_info_t *info)
{
    if (!path || !info) return ESP_ERR_INVALID_ARG;
    memset(info, 0, sizeof(*info));
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) || st.st_size <= 0) return ESP_FAIL;
    info->file_size = (uint64_t)st.st_size;
    FILE *fp = fopen(path, "rb");
    if (!fp) return ESP_FAIL;
    uint8_t h[42];
    size_t n = fread(h, 1, sizeof(h), fp);
    bool read_error = ferror(fp);
    fclose(fp);
    if (read_error || !n) return ESP_FAIL;
    const char *ext = strrchr(path, '.');
    info->is_flac = ext && !strcasecmp(ext, ".flac");
    if (!info->is_flac) {
        /* This is a readability probe, NOT MP3 bitstream validation. No fake
         * 128-kbps duration estimate, no PCM allocation, no playback clock. */
        return ext && !strcasecmp(ext, ".mp3") ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    /* Native FLAC, RFC 9639 section 8.2: first block must be 34-byte STREAMINFO. */
    if (n != sizeof(h) || memcmp(h, "fLaC", 4) || (h[4] & 0x7f) != 0 ||
        h[5] != 0 || h[6] != 0 || h[7] != 34) return ESP_ERR_INVALID_RESPONSE;
    uint64_t packed = 0;
    for (unsigned i = 18; i < 26; ++i) packed = (packed << 8) | h[i];
    info->sample_rate = (uint32_t)(packed >> 44);
    info->channels = (uint8_t)((packed >> 41) & 7) + 1;
    info->bits_per_sample = (uint8_t)((packed >> 36) & 31) + 1;
    info->total_samples = packed & UINT64_C(0xfffffffff);
    if (!info->sample_rate || info->bits_per_sample < 4) return ESP_ERR_INVALID_RESPONSE;
    info->duration_ms = info->total_samples * UINT64_C(1000) / info->sample_rate;
    info->metadata_known = true;
    return ESP_OK;
}
