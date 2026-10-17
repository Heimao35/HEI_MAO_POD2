#pragma once
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#define COVER_WIDTH 240
#define COVER_HEIGHT 240
#define COVER_JPEG_MAX_BYTES (256U * 1024U)
/* Worker-only, double buffered. Never writes the currently displayed buffer.
 * The worker must publish a buffer under the LVGL lock before its next load. */
esp_err_t cover_loader_init(void);
esp_err_t cover_loader_load(const char *path, const uint8_t **pixels);
void cover_loader_commit(const uint8_t *pixels);
void cover_loader_destroy(void); /* UI must detach its canvas first. */
