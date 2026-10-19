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
#include <stdatomic.h>

static const char *TAG = "sd_card";

#define CARD_DET_GPIO  GPIO_NUM_45
#define DEBOUNCE_MS    200

static sdmmc_card_t *s_card = NULL;
static atomic_bool s_mounted = false;
static atomic_uint s_generation = 0;
static SemaphoreHandle_t s_io_mutex;
static SemaphoreHandle_t s_evt_sem = NULL;
static void (*s_audio_detach)(void);
static atomic_bool s_safe_remount_requested;
static atomic_bool s_safe_speed;

esp_err_t sd_card_set_audio_detach_callback(void (*callback)(void))
{
    if (!sd_card_io_lock(pdMS_TO_TICKS(1000))) return ESP_ERR_TIMEOUT;
    s_audio_detach=callback;
    sd_card_io_unlock();
    return ESP_OK;
}

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
    if (!sd_card_io_lock(portMAX_DELAY)) return ESP_ERR_INVALID_STATE;
    if (s_mounted) { sd_card_io_unlock(); return ESP_OK; }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    /* Normal cards mount at 40 MHz. After a scan-time I/O timeout,
     * the recovery remount uses 20 MHz to provide additional timing margin. */
    host.max_freq_khz = atomic_load(&s_safe_speed) ?
                        SDMMC_FREQ_DEFAULT : SDMMC_FREQ_HIGHSPEED;

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
        sd_card_io_unlock();
        return ret;
    }

    sdmmc_card_print_info(stdout, s_card);
    create_sys_dirs();
    ++s_generation;
    s_mounted = true;
    sd_card_io_unlock();

    lvgl_port_lock(0);
    ui_get_device_state()->sd_card_inserted = true;
    lvgl_port_unlock();

    ESP_LOGI(TAG, "SD card mounted at %s", SD_MOUNT_POINT);
    return ESP_OK;
}

static void do_unmount(void)
{
    if (!sd_card_io_lock(portMAX_DELAY)) return;
    if (!s_mounted) { sd_card_io_unlock(); return; }

    /* 音频唯一长开文件在同一SD锁内关闭，再销毁VFS；播放器不再持有
     * 失效FILE指针。回调只能关闭文件/发布原子标志，不等待UI或DAC。 */
    if (s_audio_detach) s_audio_detach();
    esp_vfs_fat_sdcard_unmount(SD_MOUNT_POINT, s_card);
    s_card = NULL;
    s_mounted = false;
    ++s_generation;
    sd_card_io_unlock();

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
        xSemaphoreTake(s_evt_sem, pdMS_TO_TICKS(1000));
        bool safe_remount = atomic_exchange(&s_safe_remount_requested, false);
        if (!safe_remount) vTaskDelay(pdMS_TO_TICKS(DEBOUNCE_MS));

        bool inserted = card_inserted();
        if (safe_remount && s_mounted) {
            /* The caller only posts this request. VFS and SDMMC cleanup stay
             * in the detect task, outside the scan worker and its SD lock. */
            ESP_LOGW(TAG, "Recovering SD card after an I/O timeout");
            do_unmount();
            vTaskDelay(pdMS_TO_TICKS(20));
            inserted = card_inserted();
        }
        if (inserted && !s_mounted) {
            ESP_LOGI(TAG, "Card inserted, mounting...");
            do_mount();
        } else if (!inserted && s_mounted) {
            ESP_LOGI(TAG, "Card removed, unmounting...");
            do_unmount();
            /* A physical reinsertion is a new card/session: retry its normal
             * 40 MHz operating point instead of retaining a prior recovery mode. */
            atomic_store(&s_safe_speed, false);
        }
    }
}

esp_err_t sd_card_init(void)
{
    if (s_io_mutex) return ESP_ERR_INVALID_STATE;
    s_io_mutex = xSemaphoreCreateMutex();
    s_evt_sem = xSemaphoreCreateBinary();
    atomic_store(&s_safe_remount_requested, false);
    atomic_store(&s_safe_speed, false);
    if (!s_io_mutex || !s_evt_sem) {
        if (s_io_mutex) vSemaphoreDelete(s_io_mutex);
        if (s_evt_sem) vSemaphoreDelete(s_evt_sem);
        s_io_mutex = NULL; s_evt_sem = NULL;
        return ESP_ERR_NO_MEM;
    }

    gpio_config_t io_cfg = {
        .pin_bit_mask = 1ULL << CARD_DET_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_ANYEDGE,
    };
    esp_err_t ret = gpio_config(&io_cfg);
    if (ret != ESP_OK) return ret;
    ret = gpio_isr_handler_add(CARD_DET_GPIO, card_det_isr, NULL);
    if (ret != ESP_OK) ESP_LOGW(TAG, "Card IRQ unavailable; polling enabled");

    if (card_inserted()) {
        do_mount();
    }
    if (xTaskCreate(sd_detect_task, "sd_det", 4096, NULL, 2, NULL) != pdPASS)
        return ESP_ERR_NO_MEM;
    return ESP_OK;
}

bool sd_card_is_mounted(void)
{
    return s_mounted;
}

void sd_card_poll_detect(void)
{
    /* Compatibility API: wake the detect task, never block the caller. */
    if (s_evt_sem) xSemaphoreGive(s_evt_sem);
}

void sd_card_request_safe_remount(void)
{
    if (!s_evt_sem) return;
    atomic_store(&s_safe_speed, true);
    atomic_store(&s_safe_remount_requested, true);
    xSemaphoreGive(s_evt_sem);
}

bool sd_card_is_present(void) { return card_inserted(); }
uint32_t sd_card_get_generation(void) { return s_generation; }
bool sd_card_io_lock(TickType_t timeout)
{
    return s_io_mutex && xSemaphoreTake(s_io_mutex, timeout) == pdTRUE;
}
void sd_card_io_unlock(void) { xSemaphoreGive(s_io_mutex); }
