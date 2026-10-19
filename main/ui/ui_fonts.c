#include "ui_fonts.h"

#include <stdbool.h>
#include <stdio.h>
#include "esp_log.h"
#include "esp_spiffs.h"

#define FONTFS_PARTITION_LABEL "fontfs"
#define FONTFS_BASE_PATH       "/fontfs"
#define FONTFS_LVGL_LETTER     "F:"

static const char *TAG = "ui_fonts";
static bool s_fontfs_mounted;
static bool s_initialized;
static lv_font_t *s_font_14;
static lv_font_t *s_font_16;
static lv_font_t *s_font_18;
static lv_font_t *s_font_22;

/*
 * LVGL's POSIX driver prepends CONFIG_LV_FS_POSIX_PATH to the path after the
 * driver letter. Thus F:UI_FONT_18.bin resolves to /fontfs/UI_FONT_18.bin.
 * Keep the path construction in one place so a future partition mount change
 * cannot silently make only one of the font sizes fail.
 */
static lv_font_t *load_font(const char *file_name, const lv_font_t *fallback)
{
    char path[64];
    snprintf(path, sizeof(path), "%s%s", FONTFS_LVGL_LETTER, file_name);

    lv_font_t *font = lv_binfont_create(path);
    if(font == NULL) {
        ESP_LOGW(TAG, "Unable to load %s; using built-in fallback", file_name);
        return (lv_font_t *)fallback;
    }

    /* Fallback is per size: this preserves line height and baseline metrics. */
    font->fallback = fallback;
    ESP_LOGI(TAG, "Loaded %s (line height %d)", file_name, font->line_height);
    return font;
}

esp_err_t ui_fonts_init(void)
{
    return ui_fonts_init_with_progress(NULL,NULL);
}

esp_err_t ui_fonts_init_with_progress(ui_fonts_progress_cb_t progress, void *user_data)
{
    if(s_initialized) return ESP_OK;

    /* Never format this partition automatically: it contains build-time assets. */
    const esp_vfs_spiffs_conf_t conf = {
        .base_path = FONTFS_BASE_PATH,
        .partition_label = FONTFS_PARTITION_LABEL,
        .max_files = 4,
        .format_if_mount_failed = false,
    };

    esp_err_t err = esp_vfs_spiffs_register(&conf);
    if(err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGE(TAG, "Font partition mount failed: %s", esp_err_to_name(err));
        return err;
    }
    s_fontfs_mounted = true;

    size_t total = 0;
    size_t used = 0;
    if(esp_spiffs_info(FONTFS_PARTITION_LABEL, &total, &used) == ESP_OK) {
        ESP_LOGI(TAG, "Font partition: %u/%u bytes used", (unsigned)used, (unsigned)total);
    }

    /*
     * lv_binfont_create parses the binary file into LVGL-owned allocations.
     * Loading once avoids multiplying the approximately 1.13 MiB 18 px font
     * when multiple pages use it. ESP-IDF's LVGL allocator is configured for
     * this project, so the allocation can use the available external RAM.
     */
    const struct {
        const char *name;
        const lv_font_t *fallback;
        lv_font_t **destination;
    } fonts[] = {
        {"UI_FONT_14.bin", &lv_font_montserrat_14, &s_font_14},
        {"UI_FONT_16.bin", &lv_font_montserrat_16, &s_font_16},
        {"UI_FONT_18.bin", &lv_font_montserrat_18, &s_font_18},
        {"UI_FONT_22.bin", &lv_font_montserrat_22, &s_font_22},
    };
    const unsigned count=sizeof(fonts)/sizeof(fonts[0]);
    for(unsigned i=0;i<count;++i) {
        /* 启动状态先刷新，再执行同步解析；不解锁去并行操作 LVGL，
         * 继续遵守当前 font loader/界面的共同锁约定。 */
        if(progress) progress(i,count,fonts[i].name,user_data);
        *fonts[i].destination=load_font(fonts[i].name,fonts[i].fallback);
    }
    if(progress) progress(count,count,NULL,user_data);

    s_initialized = true;
    ESP_LOGI(TAG, "Custom UI fonts initialized%s", s_fontfs_mounted ? "" : " without partition");
    return ESP_OK;
}

const lv_font_t *ui_font_14(void) { return s_font_14 ? s_font_14 : &lv_font_montserrat_14; }
const lv_font_t *ui_font_16(void) { return s_font_16 ? s_font_16 : &lv_font_montserrat_16; }
const lv_font_t *ui_font_18(void) { return s_font_18 ? s_font_18 : &lv_font_montserrat_18; }
const lv_font_t *ui_font_22(void) { return s_font_22 ? s_font_22 : &lv_font_montserrat_22; }
