#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include "esp_err.h"

/* FATFS UTF-8 names are bounded in bytes; overlong names are rejected, never truncated. */
#define MUSIC_NAME_MAX 256
#define MUSIC_PATH_MAX 576
#define MUSIC_PAGE_SIZE 8
#define MUSIC_INDEX_MAX_BYTES (64U * 1024U * 1024U)

typedef struct {
    char name[MUSIC_NAME_MAX]; /* folder name / display title */
    char audio[MUSIC_NAME_MAX];
    char cover[MUSIC_NAME_MAX];
    char lyric[MUSIC_NAME_MAX];
} music_item_t;
typedef struct {
    uint32_t first, count;
    char names[MUSIC_PAGE_SIZE][MUSIC_NAME_MAX];
} music_page_t;

/* All APIs are worker-only, with sd_card_io_lock held. No persistent open files.
 * Index is rebuilt on mount/rescan; RAM does not grow with the number of songs.
 * Files with missing sidecars remain selectable and display a fallback. */
esp_err_t music_library_scan(void);
void music_library_reset(void);
uint32_t music_library_get_count(void);
bool music_library_is_truncated(void);
esp_err_t music_library_get_item(uint32_t index, music_item_t *out);
esp_err_t music_library_get_page(uint32_t first, music_page_t *out);
esp_err_t music_library_make_path(const music_item_t *item, const char *file,
                                  char *out, size_t size);
