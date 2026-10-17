#pragma once
#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>
#include "esp_err.h"
#define LYRIC_MAX_LINES 1024
#define LYRIC_TEXT_BYTES (64U * 1024U)
#define LYRIC_MAX_FILE_BYTES (256U * 1024U)
#define LYRIC_PAGE_LINES 6
#define LYRIC_PAGE_BYTES 4096
/* Single worker owns the parser; UI receives a bounded text copy, not parser pointers. */
esp_err_t lyric_parser_load(const char *path);
void lyric_parser_destroy(void);
uint32_t lyric_parser_get_line_count(void);
bool lyric_parser_is_truncated(void);
void lyric_parser_get_page(uint32_t page, char *text, size_t capacity);
const char *lyric_parser_get_current_text(uint32_t time_ms);
uint32_t lyric_parser_get_current_line(uint32_t time_ms);
size_t lyric_parser_copy_all(char *out, size_t capacity);
