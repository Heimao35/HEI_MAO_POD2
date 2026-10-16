#include "sd_card.h"
#include "ui/ui_common.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include <sys/stat.h>
#include <string.h>

static const char *TAG = "sd_card";

#define CARD_DET_GPIO  GPIO_NUM_45
#define DEBOUNCE_MS    200

static sdmmc_card_t *s_card = NULL;
static bool s_mounted = false;
static SemaphoreHandle_t s_evt_sem = NULL;

static bool card_inserted(void)
{
    return gpio_get_level(CARD_DET_GPIO) == 0;
}

static void ensure_dir(const char *path)
{
    struct stat st;
    if (stat(path, &st) != 0) {
        mkdir(path, 0755);
    }
}

static void create_sys_dirs(void)
{
    ensure_dir(SD_SYS_DIR);
    ensure_dir(SD_SYS_DIR "/BOOKS");
    ensure_dir(SD_SYS_DIR "/MUSIC");
    ensure_dir(SD_SYS_DIR "/MEDIA");
    ensure_dir(SD_SYS_DIR "/MEDIA/cam");
    ensure_dir(SD_SYS_DIR "/MEDIA/pictures");
    ensure_dir(SD_SYS_DIR "/MEDIA/vedio");
    ensure_dir(SD_SYS_DIR "/MEDIA/lock_scr");
    ensure_dir(SD_SYS_DIR "/BIN");
    ensure_dir(SD_SYS_DIR "/.hmcache");
}

static esp_err_t do_mount(void)
{
    if (s_mounted) return ESP_OK;

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.cd = SDMMC_SLOT_NO_CD;
    slot.wp = SDMMC_SLOT_NO_WP;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 64 * 1024,
    };

    esp_err_t ret = esp_vfs_fat_sdmmc_mount(SD_MOUNT_POINT, &host, &slot,
                                             &mount_cfg, &s_card);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s", esp_err_to_name(ret));
        s_card = NULL;
        return ret;
    }

    s_mounted = true;
    sdmmc_card_print_info(stdout, s_card);
    create_sys_dirs();

    lvgl_port_lock(0);
    ui_get_device_state()->sd_card_inserted = true;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

static void do_unmount(void)
{
    if (!s_mounted) return;

    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;

    lvgl_port_lock(0);
    ui_get_device_state()->sd_card_inserted = false;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "SD card unmounted");
}

static void IRAM_ATTR card_det_isr(void *arg)
{
    (void)arg;
    BaseType_t woken = pdFALSE;
    xSemaphoreGiveFromISR(s_evt_sem, &woken);
    if (woken) portYIELD_FROM_ISR();
}

static void sd_detect_task(void *arg)
{
    (void)arg;
    for (;;) {
        xSemaphoreTake(s_evt_sem, portMAX_DELAY);
        vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

        bool inserted = card_inserted();
        if (inserted && !s_mounted) {
            ESP_LOGI(TAG, "Card inserted, mounting...");
            do_mount();
        } else if (!inserted && s_mounted) {
            ESP_LOGI(TAG, "Card removed, unmounting...");
            do_unmount();
        }
    }
}

esp_err_t sd_card_init(void)
{
    s_evt_sem = xSemaphoreCreateBinary();

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << CARD_DET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    gpio_config(&io_cfg);
    gpio_isr_handler_add(CARD_DET_GPIO, card_det_isr, NULL);

    xTaskCreate(sd_detect_task, "sd_det", 4096, NULL, 2, NULL);

    if (card_inserted()) {
        do_mount();
    }
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

void sd_card_poll_detect(void)
{
    /* kept for API compat, no longer needed with interrupt-based detection */
}
