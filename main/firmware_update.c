#include "firmware_update.h"

#include <ctype.h>
#include <dirent.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "bt_audio.h"
#include "u4_ota_host.h"
#include "esp_app_desc.h"
#include "esp_app_format.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "pmic_axp2101.h"
#include "sd_card.h"
#include "ui/ui_fw_update_dialog.h"

static const char *TAG = "fw_update";

/* SD 卡系统目录下的 BIN 文件夹；只扫描这一层，不递归，避免误读其它文件。
 * SD_SYS_DIR 由 sd_card.h 统一定义为 /sdcard/HEI_MAO_POD_SYS，避免两处
 * 路径字符串不同步。实际路径：SD 卡根目录 /HEI_MAO_POD_SYS/BIN。 */
#define FW_BIN_DIR          SD_SYS_DIR "/BIN"
/* 同时满足 esp_flash_write 的 8 KiB 直接写块和 SD 的 DMA 对齐要求。 */
#define FW_CHUNK_BYTES      (8 * 1024)
/* VFS/FatFs 读取链与 esp_ota_* 共用；给到与媒体 worker 相同的 12 KiB，
 * 避免更新中途栈溢出重启（那会中断一次本可安全失败的写入）。 */
#define FW_TASK_STACK       (12 * 1024)
#define FW_SCAN_LOCK_MS     5000
#define FW_UPDATE_LOCK_MS   5000

/* app 镜像结构：24 字节 image header + 8 字节首段头，descriptor 位于 0x20。
 * 这个偏移由 esp_image_format.c 固定，不能用文件名/自定义头替代。 */
#define FW_APP_DESC_OFFSET  (sizeof(esp_image_header_t) + sizeof(esp_image_segment_header_t))

#define FW_EVT_CHOICE       BIT0
#define FW_EVT_DONE         BIT1
#define FW_EVT_DISMISS      BIT2

static EventGroupHandle_t s_events;
static atomic_int s_state = FW_UPDATE_STATE_IDLE;
static atomic_int s_phase = FW_UPDATE_PHASE_NONE;
static atomic_uint s_progress;
static atomic_int s_error = FW_UPDATE_ERR_NONE;
static bool s_user_confirmed;
static bool s_boot_checked;
static fw_update_info_t s_info;

/* ============================== 版本比较 ============================== */

/**
 * @brief 解析最多 4 段十进制版本号（1 / 1.2 / 1.2.3 / 1.2.3.4）。
 *
 * 只接受纯数字和点：带 -dirty、+build 等后缀的版本不参与自动选择，
 * 避免把开发中的临时构建误判为更高版本。发布固件应使用 version.txt
 * 里的干净版本号。
 */
static bool version_parse(const char *text, uint32_t out[4])
{
    memset(out, 0, 4 * sizeof(uint32_t));
    if (!text || !*text) return false;

    unsigned part = 0;
    uint32_t value = 0;
    bool have_digit = false;
    for (const char *p = text; ; ++p) {
        if (*p >= '0' && *p <= '9') {
            if (value > 100000U) return false;
            value = value * 10U + (uint32_t)(*p - '0');
            have_digit = true;
        } else if (*p == '.' || *p == '\0') {
            if (!have_digit || part >= 4) return false;
            out[part++] = value;
            value = 0;
            have_digit = false;
            if (*p == '\0') break;
        } else {
            return false;
        }
    }
    return part > 0;
}

static int version_cmp(const uint32_t a[4], const uint32_t b[4])
{
    for (unsigned i = 0; i < 4; ++i) {
        if (a[i] != b[i]) return a[i] < b[i] ? -1 : 1;
    }
    return 0;
}

/* descriptor 的 char 字段是定长 32/16 字节，不保证以 NUL 结尾。 */
static void copy_field(char *dst, size_t dst_size, const char *src, size_t src_size)
{
    size_t n = 0;
    while (n < src_size && src[n]) n++;
    if (n >= dst_size) n = dst_size - 1;
    memcpy(dst, src, n);
    dst[n] = '\0';
}

static bool has_bin_suffix(const char *name)
{
    size_t len = strlen(name);
    if (len < 4) return false;
    const char *ext = name + len - 4;
    return ext[0] == '.' &&
           tolower((unsigned char)ext[1]) == 'b' &&
           tolower((unsigned char)ext[2]) == 'i' &&
           tolower((unsigned char)ext[3]) == 'n';
}

/* VFS read 允许短读；描述符只有几百字节，循环补齐即可。 */
static bool read_full(int fd, void *buf, size_t len)
{
    uint8_t *p = buf;
    while (len) {
        ssize_t n = read(fd, p, len);
        if (n <= 0) return false;
        p += n;
        len -= (size_t)n;
    }
    return true;
}

/* ============================== 镜像检查 ============================== */

/**
 * @brief 读取一个候选 .bin 的头部，确认它是本工程的 P4 app 镜像。
 *
 * 只读 app descriptor，不需要把 1.4 MB 文件读进内存；真正的完整校验
 * 由 esp_ota_end() 在写入完成后执行（bin 末尾自带 SHA-256）。
 * 调用方必须已持有 sd_card_io_lock。
 */
static bool read_candidate(const char *path, const esp_app_desc_t *running,
                           const esp_partition_t *target, fw_update_info_t *out)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < (off_t)(FW_APP_DESC_OFFSET + sizeof(esp_app_desc_t))) {
        return false;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;

    uint8_t buf[FW_APP_DESC_OFFSET + sizeof(esp_app_desc_t)];
    bool ok = read_full(fd, buf, sizeof(buf));
    close(fd);
    if (!ok) return false;

    esp_image_header_t hdr;
    esp_app_desc_t desc;
    memcpy(&hdr, buf, sizeof(hdr));
    memcpy(&desc, buf + FW_APP_DESC_OFFSET, sizeof(desc));

    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC || hdr.chip_id != ESP_CHIP_ID_ESP32P4) {
        ESP_LOGD(TAG, "Skip %s: not an ESP32-P4 app image", path);
        return false;
    }
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGD(TAG, "Skip %s: no app descriptor (bootloader/merged image?)", path);
        return false;
    }
    /* 两个字段都由同一工程构建产生，32 字节定长比较；避免把 U4 的
     * a2dp_source.bin 或其它工程镜像写进 P4 的应用槽。 */
    if (memcmp(desc.project_name, running->project_name, sizeof(desc.project_name)) != 0) {
        ESP_LOGD(TAG, "Skip %s: project name mismatch", path);
        return false;
    }
    if ((uint32_t)st.st_size > target->size) {
        ESP_LOGW(TAG, "Skip %s: %u bytes exceeds target partition %u bytes",
                 path, (unsigned)st.st_size, (unsigned)target->size);
        return false;
    }

    char candidate_version[33];
    char running_version[33];
    uint32_t candidate_ver[4];
    uint32_t running_ver[4];
    copy_field(candidate_version, sizeof(candidate_version),
               desc.version, sizeof(desc.version));
    copy_field(running_version, sizeof(running_version),
               running->version, sizeof(running->version));

    if (!version_parse(candidate_version, candidate_ver)) {
        ESP_LOGW(TAG, "Skip %s: version '%s' is not numeric", path, candidate_version);
        return false;
    }
    if (!version_parse(running_version, running_ver)) {
        /* 旧构建可能是 git describe 字符串；视为 0.0.0，让带 version.txt
         * 的新镜像可以升级，但不影响正常数字版本比较。 */
        memset(running_ver, 0, sizeof(running_ver));
    }
    if (version_cmp(candidate_ver, running_ver) <= 0) {
        ESP_LOGI(TAG, "Skip %s: version %s is not newer than %s",
                 path, candidate_version, running_version);
        return false;
    }

    memset(out, 0, sizeof(*out));
    copy_field(out->version, sizeof(out->version), desc.version, sizeof(desc.version));
    copy_field(out->date, sizeof(out->date), desc.date, sizeof(desc.date));
    snprintf(out->path, sizeof(out->path), "%s", path);
    out->size = (uint32_t)st.st_size;
    ESP_LOGI(TAG, "Candidate: %s (%s, %s), %u bytes",
             path, out->version, out->date, (unsigned)out->size);
    return true;
}

/**
 * @brief 读取 U4 app bin 的头部，确认它是 a2dp_source 的 ESP32 镜像。
 *
 * U4 的 app 分区是 1.75 MiB；只读 descriptor，真正的镜像校验由 U4 自己
 * 的 esp_ota_end() 完成。cached_version 是 P4 上次 FW_INFO/缓存版本，
 * 为空/非数字时无法比较，交给 U4 在线查询后决定是否真的需要传输。
 */
static bool read_u4_candidate(const char *path, const char *cached_version,
                              fw_update_info_t *out)
{
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode) ||
        st.st_size < (off_t)(FW_APP_DESC_OFFSET + sizeof(esp_app_desc_t)) ||
        st.st_size > 0x1C0000) {
        return false;
    }

    int fd = open(path, O_RDONLY);
    if (fd < 0) return false;
    uint8_t buf[FW_APP_DESC_OFFSET + sizeof(esp_app_desc_t)];
    bool ok = read_full(fd, buf, sizeof(buf));
    close(fd);
    if (!ok) return false;

    esp_image_header_t hdr;
    esp_app_desc_t desc;
    memcpy(&hdr, buf, sizeof(hdr));
    memcpy(&desc, buf + FW_APP_DESC_OFFSET, sizeof(desc));

    if (hdr.magic != ESP_IMAGE_HEADER_MAGIC || hdr.chip_id != ESP_CHIP_ID_ESP32)
        return false;
    if (desc.magic_word != ESP_APP_DESC_MAGIC_WORD) return false;
    if (memcmp(desc.project_name, "a2dp_source", 12) != 0) return false;

    char candidate_version[33];
    uint32_t candidate_ver[4];
    copy_field(candidate_version, sizeof(candidate_version),
               desc.version, sizeof(desc.version));
    if (!version_parse(candidate_version, candidate_ver)) {
        ESP_LOGW(TAG, "Skip U4 %s: version '%s' is not numeric",
                 path, candidate_version);
        return false;
    }

    uint32_t cached_ver[4];
    if (cached_version && version_parse(cached_version, cached_ver) &&
        version_cmp(candidate_ver, cached_ver) <= 0) {
        ESP_LOGI(TAG, "Skip U4 %s: version %s not newer than cached %s",
                 path, candidate_version, cached_version);
        return false;
    }

    copy_field(out->u4_version, sizeof(out->u4_version),
               desc.version, sizeof(desc.version));
    copy_field(out->u4_date, sizeof(out->u4_date),
               desc.date, sizeof(desc.date));
    snprintf(out->u4_path, sizeof(out->u4_path), "%s", path);
    out->u4_size = (uint32_t)st.st_size;
    out->has_u4 = true;
    ESP_LOGI(TAG, "U4 candidate: %s (%s, %s), %u bytes",
             path, out->u4_version, out->u4_date, (unsigned)out->u4_size);
    return true;
}

/**
 * @brief 扫描 /sdcard/HEI_MAO_POD_SYS/BIN 下的 .bin 文件。
 *
 * 同时识别 P4（chip_id 0x0012）和 U4（chip_id 0x0000，a2dp_source）。
 * P4 选择高于当前运行版本的最高版本；U4 与最近一次 FW_INFO 缓存比较，
 * 缓存未知时仍列为候选，最终由 U4 在线查询版本决定是否传输。
 * 只读 descriptor；整个扫描持有 SD 锁，时间在毫秒级。
 */
static bool scan_for_update(fw_update_info_t *out)
{
    if (!sd_card_is_mounted() || !sd_card_is_present()) {
        ESP_LOGI(TAG, "SD card not mounted; skip firmware check");
        return false;
    }
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    const esp_app_desc_t *running = esp_app_get_description();
    char cached_u4[BT_AUDIO_U4_VERSION_MAX];
    bool have_cached_u4 = bt_audio_get_u4_version(cached_u4, sizeof(cached_u4)) &&
                          cached_u4[0];

    if (!sd_card_io_lock(pdMS_TO_TICKS(FW_SCAN_LOCK_MS))) {
        ESP_LOGW(TAG, "SD lock timeout while scanning firmware");
        return false;
    }

    DIR *dir = opendir(FW_BIN_DIR);
    if (!dir) {
        sd_card_io_unlock();
        ESP_LOGI(TAG, "No %s directory; skip firmware check", FW_BIN_DIR);
        return false;
    }

    bool found_p4 = false, found_u4 = false;
    fw_update_info_t best_p4 = {0}, best_u4 = {0};
    uint32_t best_p4_ver[4] = {0}, best_u4_ver[4] = {0};
    struct dirent *entry;
    while (sd_card_is_present() && (entry = readdir(dir)) != NULL) {
        if (entry->d_name[0] == '.') continue;
        if (!has_bin_suffix(entry->d_name)) continue;

        char path[sizeof(best_p4.path)];
        int n = snprintf(path, sizeof(path), "%s/%s", FW_BIN_DIR, entry->d_name);
        if (n <= 0 || (size_t)n >= sizeof(path)) continue;

        fw_update_info_t candidate;
        if (target && running &&
            read_candidate(path, running, target, &candidate)) {
            uint32_t ver[4];
            if (version_parse(candidate.version, ver) &&
                (!found_p4 || version_cmp(ver, best_p4_ver) > 0)) {
                best_p4 = candidate;
                memcpy(best_p4_ver, ver, sizeof(best_p4_ver));
                found_p4 = true;
            }
        }

        candidate = (fw_update_info_t){0};
        if (read_u4_candidate(path, have_cached_u4 ? cached_u4 : NULL,
                              &candidate)) {
            uint32_t ver[4];
            if (version_parse(candidate.u4_version, ver) &&
                (!found_u4 || version_cmp(ver, best_u4_ver) > 0)) {
                best_u4 = candidate;
                memcpy(best_u4_ver, ver, sizeof(best_u4_ver));
                found_u4 = true;
            }
        }
    }
    closedir(dir);
    sd_card_io_unlock();

    if (!found_p4 && !found_u4) return false;
    memset(out, 0, sizeof(*out));
    if (found_p4) {
        out->has_p4 = true;
        out->version[0] = 0;
        copy_field(out->version, sizeof(out->version), best_p4.version,
                   sizeof(best_p4.version));
        copy_field(out->date, sizeof(out->date), best_p4.date,
                   sizeof(best_p4.date));
        snprintf(out->path, sizeof(out->path), "%s", best_p4.path);
        out->size = best_p4.size;
        if (running)
            copy_field(out->current_version, sizeof(out->current_version),
                       running->version, sizeof(running->version));
        ESP_LOGI(TAG, "Selected P4 %s (%s -> %s)",
                 out->path, out->current_version, out->version);
    }
    if (found_u4) {
        out->has_u4 = true;
        if (have_cached_u4)
            snprintf(out->u4_current_version, sizeof(out->u4_current_version),
                     "%s", cached_u4);
        else
            snprintf(out->u4_current_version, sizeof(out->u4_current_version),
                     "?");
        copy_field(out->u4_version, sizeof(out->u4_version), best_u4.u4_version,
                   sizeof(best_u4.u4_version));
        copy_field(out->u4_date, sizeof(out->u4_date), best_u4.u4_date,
                   sizeof(best_u4.u4_date));
        snprintf(out->u4_path, sizeof(out->u4_path), "%s", best_u4.u4_path);
        out->u4_size = best_u4.u4_size;
        ESP_LOGI(TAG, "Selected U4 %s (%s -> %s)",
                 out->u4_path, out->u4_current_version, out->u4_version);
    }
    return true;
}

/* ============================== 状态/错误 ============================== */

fw_update_state_t firmware_update_get_state(void)
{
    return (fw_update_state_t)atomic_load(&s_state);
}

fw_update_phase_t firmware_update_get_phase(void)
{
    return (fw_update_phase_t)atomic_load(&s_phase);
}

uint8_t firmware_update_get_progress(void)
{
    return (uint8_t)atomic_load(&s_progress);
}

const fw_update_info_t *firmware_update_get_info(void)
{
    return &s_info;
}

bool firmware_update_is_active(void)
{
    return firmware_update_get_state() != FW_UPDATE_STATE_IDLE;
}

const char *firmware_update_error_text(void)
{
    static const char *const messages[] = {
        [FW_UPDATE_ERR_NONE] = "",
        [FW_UPDATE_ERR_NO_PARTITION] = "找不到可写入的 OTA 分区",
        [FW_UPDATE_ERR_NO_MEM] = "内部内存不足",
        [FW_UPDATE_ERR_SD_LOCK] = "SD 卡忙，请重试",
        [FW_UPDATE_ERR_OPEN] = "无法打开固件文件",
        [FW_UPDATE_ERR_FILE_CHANGED] = "固件文件已被修改",
        [FW_UPDATE_ERR_READ] = "读取 SD 卡失败",
        [FW_UPDATE_ERR_OTA_BEGIN] = "无法开始写入 Flash",
        [FW_UPDATE_ERR_OTA_WRITE] = "写入 Flash 失败",
        [FW_UPDATE_ERR_OTA_END] = "固件校验失败",
        [FW_UPDATE_ERR_SET_BOOT] = "无法切换启动分区",
        [FW_UPDATE_ERR_TASK] = "无法创建更新任务",
        [FW_UPDATE_ERR_U4] = "协处理器更新失败",
        [FW_UPDATE_ERR_U4_UNSUPPORTED] = "U4 固件不支持 SDIO 更新，请先用 UART 烧录",
    };
    int err = atomic_load(&s_error);
    if (err < 0 || err >= (int)(sizeof(messages) / sizeof(messages[0]))) err = 0;
    return messages[err];
}

void firmware_update_confirm(void)
{
    s_user_confirmed = true;
    if (s_events) xEventGroupSetBits(s_events, FW_EVT_CHOICE);
}

void firmware_update_cancel(void)
{
    s_user_confirmed = false;
    if (s_events) xEventGroupSetBits(s_events, FW_EVT_CHOICE);
}

void firmware_update_dismiss(void)
{
    if (s_events) xEventGroupSetBits(s_events, FW_EVT_DISMISS);
}

/* ============================== OTA 任务 ============================== */

/**
 * @brief 独立更新任务：内部 RAM 栈 + 内部 DMA 缓冲。
 *
 * 不能使用 worker_task_create()（PSRAM 栈）：本项目虽然开启了
 * CONFIG_SPIRAM_XIP_FROM_PSRAM（SPI1 写 Flash 时外部 memory cache 可以
 * 保持工作），但 esp_ota_end()/esp_ota_set_boot_partition() 的校验路径会
 * 调用 esp_image_verify() -> bootloader_mmap()，后者需要操作 MMU/外部
 * memory cache；项目规则也明确禁止 PSRAM 栈任务调用 esp_ota_* 与 flash API。
 * 数据源则必须是内部 RAM：esp_flash_write() 对非 DRAM 缓冲会退化成每次
 * 32 字节拷贝，1.5 MB 会非常慢。
 *
 * 本任务不调用 LVGL，只更新原子状态和事件组；UI 由 100 ms 定时器轮询
 * 快照。唯一的 Flash 写入例外是 U4 版本的小缓存 player/u4ver（在 U4
 * 查询/更新后立即落盘，避免随后的 PMIC 重启丢失显示值）。SD 锁在整个
 * 文件传输期间持有：开机阶段没有音频播放，且该锁能阻止卸载路径在文件
 * 句柄仍存活时卸载 VFS。
 */
static void u4_ota_progress_cb(uint8_t pct, void *ctx)
{
    (void)ctx;
    atomic_store(&s_progress, pct);
}

static void update_task(void *arg)
{
    (void)arg;

    if (s_info.has_u4) {
        atomic_store(&s_phase, FW_UPDATE_PHASE_U4);
        atomic_store(&s_progress, 0);
        u4_ota_result_t u4res = {0};
        esp_err_t u4err = u4_ota_host_update(s_info.u4_path, s_info.u4_size,
                                             s_info.u4_version,
                                             u4_ota_progress_cb, NULL, &u4res);
        if (u4err != ESP_OK) {
            ESP_LOGE(TAG, "U4 update failed: %s", esp_err_to_name(u4err));
            atomic_store(&s_error, u4err == ESP_ERR_NOT_SUPPORTED
                                       ? FW_UPDATE_ERR_U4_UNSUPPORTED
                                       : FW_UPDATE_ERR_U4);
            goto fail;
        }
        ESP_LOGI(TAG, "U4 phase done: updated=%d version=%s",
                 u4res.updated ? 1 : 0, u4res.version);
        if (!s_info.has_p4) {
            /* U4-only update: no P4 slot commit and no PMIC restart. */
            atomic_store(&s_progress, 100);
            atomic_store(&s_state, FW_UPDATE_STATE_SUCCESS);
            if (s_events) xEventGroupSetBits(s_events, FW_EVT_DONE);
            vTaskDelete(NULL);
            return;
        }
    }

    atomic_store(&s_phase, FW_UPDATE_PHASE_P4);
    atomic_store(&s_progress, 0);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part) {
        atomic_store(&s_error, FW_UPDATE_ERR_NO_PARTITION);
        goto fail;
    }

    uint8_t *buf = heap_caps_aligned_alloc(128, FW_CHUNK_BYTES,
                                           MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        /* 对齐分配失败时退回普通内部堆：只影响 SD 单块 DMA 效率，
         * 不影响 esp_ota_write 的内部 RAM 要求。 */
        buf = heap_caps_malloc(FW_CHUNK_BYTES,
                               MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    }
    if (!buf) {
        ESP_LOGE(TAG, "No internal 8 KiB buffer; DMA free=%u largest=%u",
                 (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DMA));
        atomic_store(&s_error, FW_UPDATE_ERR_NO_MEM);
        goto fail;
    }

    if (!sd_card_io_lock(pdMS_TO_TICKS(FW_UPDATE_LOCK_MS))) {
        atomic_store(&s_error, FW_UPDATE_ERR_SD_LOCK);
        heap_caps_free(buf);
        goto fail;
    }

    int fd = open(s_info.path, O_RDONLY);
    if (fd < 0) {
        atomic_store(&s_error, FW_UPDATE_ERR_OPEN);
        sd_card_io_unlock();
        heap_caps_free(buf);
        goto fail;
    }

    struct stat st;
    if (stat(s_info.path, &st) != 0 || !S_ISREG(st.st_mode) ||
        (uint32_t)st.st_size != s_info.size) {
        atomic_store(&s_error, FW_UPDATE_ERR_FILE_CHANGED);
        close(fd);
        sd_card_io_unlock();
        heap_caps_free(buf);
        goto fail;
    }

    ESP_LOGI(TAG, "Writing %s (%s) to %s at 0x%08x, %u bytes",
             s_info.path, s_info.version, part->label,
             (unsigned)part->address, (unsigned)s_info.size);

    esp_ota_handle_t handle = 0;
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        atomic_store(&s_error, FW_UPDATE_ERR_OTA_BEGIN);
        close(fd);
        sd_card_io_unlock();
        heap_caps_free(buf);
        goto fail;
    }

    uint32_t written = 0;
    while (written < s_info.size) {
        size_t want = s_info.size - written;
        if (want > FW_CHUNK_BYTES) want = FW_CHUNK_BYTES;

        ssize_t n = read(fd, buf, want);
        if (n <= 0) {
            atomic_store(&s_error, FW_UPDATE_ERR_READ);
            goto fail_ota;
        }
        err = esp_ota_write(handle, buf, (size_t)n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write failed at %u: %s",
                     (unsigned)written, esp_err_to_name(err));
            atomic_store(&s_error, FW_UPDATE_ERR_OTA_WRITE);
            goto fail_ota;
        }
        written += (uint32_t)n;
        atomic_store(&s_progress, (unsigned)((uint64_t)written * 100U / s_info.size));
    }

    close(fd);
    sd_card_io_unlock();
    heap_caps_free(buf);
    buf = NULL;

    /* 完整镜像校验：检查段校验和与 bin 末尾的 SHA-256。失败时目标槽
     * 保持未选中，当前启动分区仍是旧固件。 */
    err = esp_ota_end(handle);
    handle = 0;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        atomic_store(&s_error, FW_UPDATE_ERR_OTA_END);
        goto fail;
    }

    err = esp_ota_set_boot_partition(part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(err));
        atomic_store(&s_error, FW_UPDATE_ERR_SET_BOOT);
        goto fail;
    }

    ESP_LOGI(TAG, "Firmware verified; next boot from %s", part->label);
    atomic_store(&s_progress, 100);
    atomic_store(&s_state, FW_UPDATE_STATE_SUCCESS);
    if (s_events) xEventGroupSetBits(s_events, FW_EVT_DONE);
    vTaskDelete(NULL);
    return;

fail_ota:
    if (handle) esp_ota_abort(handle);
    close(fd);
    sd_card_io_unlock();
    heap_caps_free(buf);
    goto fail;

fail:
    atomic_store(&s_state, FW_UPDATE_STATE_FAILED);
    if (s_events) xEventGroupSetBits(s_events, FW_EVT_DONE);
    vTaskDelete(NULL);
}

/* 新镜像第一次启动：所有关键初始化已完成后才确认，之后的普通重启
 * 不再处于 PENDING_VERIFY，rollback 只覆盖首次启动失败。 */
bool firmware_update_mark_valid_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return true;

    esp_ota_img_states_t state;
    esp_err_t err = esp_ota_get_state_partition(running, &state);
    if (err != ESP_OK) {
        /* factory 分区没有 otadata 记录，返回 NOT_SUPPORTED 是正常的。 */
        return true;
    }
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return true;

    err = esp_ota_mark_app_valid_cancel_rollback();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to confirm running OTA image: %s", esp_err_to_name(err));
        return false;
    }
    ESP_LOGI(TAG, "Running OTA image confirmed");
    return true;
}

/* ============================== 开机入口 ============================== */

void firmware_update_boot_check(void)
{
    if (s_boot_checked) return;
    s_boot_checked = true;

    fw_update_info_t info;
    if (!scan_for_update(&info)) return;

    s_info = info;
    atomic_store(&s_error, FW_UPDATE_ERR_NONE);
    atomic_store(&s_phase, FW_UPDATE_PHASE_NONE);
    atomic_store(&s_progress, 0);
    atomic_store(&s_state, FW_UPDATE_STATE_PROMPT);

    s_events = xEventGroupCreate();
    if (!s_events) {
        ESP_LOGE(TAG, "Failed to create firmware update event group");
        atomic_store(&s_state, FW_UPDATE_STATE_IDLE);
        return;
    }

    lvgl_port_lock(0);
    ui_fw_update_dialog_show_prompt(&s_info);
    lvgl_port_unlock();

    /* 等待用户在 LVGL 任务里点击确认/取消。主任务只阻塞在这个事件组，
     * 不持有 LVGL 锁，UI 定时器仍可刷新和重置熄屏计时。 */
    EventBits_t bits = xEventGroupWaitBits(s_events, FW_EVT_CHOICE,
                                           pdTRUE, pdFALSE, portMAX_DELAY);
    if (!(bits & FW_EVT_CHOICE) || !s_user_confirmed) {
        lvgl_port_lock(0);
        ui_fw_update_dialog_hide();
        lvgl_port_unlock();
        atomic_store(&s_state, FW_UPDATE_STATE_IDLE);
        vEventGroupDelete(s_events);
        s_events = NULL;
        ESP_LOGI(TAG, "Firmware update cancelled");
        return;
    }

    atomic_store(&s_progress, 0);
    atomic_store(&s_state, FW_UPDATE_STATE_RUNNING);
    if (xTaskCreatePinnedToCore(update_task, "fw_update", FW_TASK_STACK,
                                NULL, 3, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create firmware update task");
        atomic_store(&s_error, FW_UPDATE_ERR_TASK);
        atomic_store(&s_state, FW_UPDATE_STATE_FAILED);
        xEventGroupSetBits(s_events, FW_EVT_DONE);
    }
    xEventGroupWaitBits(s_events, FW_EVT_DONE, pdTRUE, pdFALSE, portMAX_DELAY);

    if (firmware_update_get_state() == FW_UPDATE_STATE_SUCCESS) {
        if (!s_info.has_p4) {
            /* U4-only update: no P4 slot was committed, so do not reboot.
             * Show the completion for a moment and continue normal boot. */
            vTaskDelay(pdMS_TO_TICKS(1500));
            lvgl_port_lock(0);
            ui_fw_update_dialog_hide();
            lvgl_port_unlock();
            atomic_store(&s_state, FW_UPDATE_STATE_IDLE);
            atomic_store(&s_phase, FW_UPDATE_PHASE_NONE);
            vEventGroupDelete(s_events);
            s_events = NULL;
            return;
        }
        /* 让 UI 至少显示一帧“更新完成，正在重启”，再交给 PMIC owner
         * 执行整机重启。这里不能用 esp_restart()：项目规定重启必须走
         * AXP2101 reg0x10 bit1，保留电源路径和十秒硬件兜底。 */
        vTaskDelay(pdMS_TO_TICKS(800));
        esp_err_t ret = pmic_axp2101_restart();
        if (ret != ESP_OK) {
            ESP_LOGE(TAG, "PMIC restart failed: %s", esp_err_to_name(ret));
        }
        /* 正常路径下 PMIC 会切断/重建整板供电，执行不到这里。若仍返回，
         * 说明自动重启失败；镜像已经提交，提示用户手动断电重启。 */
        vTaskDelay(pdMS_TO_TICKS(800));
        atomic_store(&s_state, FW_UPDATE_STATE_RESTART_FAILED);
    }

    /* 失败或重启失败：对话框显示“返回”，用户确认后继续正常开机。
     * 取消路径不会走到这里。 */
    xEventGroupWaitBits(s_events, FW_EVT_DISMISS, pdTRUE, pdFALSE, portMAX_DELAY);

    lvgl_port_lock(0);
    ui_fw_update_dialog_hide();
    lvgl_port_unlock();
    atomic_store(&s_state, FW_UPDATE_STATE_IDLE);
    atomic_store(&s_phase, FW_UPDATE_PHASE_NONE);
    vEventGroupDelete(s_events);
    s_events = NULL;

    if (atomic_load(&s_error) != FW_UPDATE_ERR_NONE) {
        ESP_LOGW(TAG, "Firmware update ended with error: %s", firmware_update_error_text());
    }
}
