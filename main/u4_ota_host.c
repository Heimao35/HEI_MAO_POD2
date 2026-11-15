#include "u4_ota_host.h"

#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "link_protocol.h"
#include "bt_audio.h"
#include "bt_link.h"
#include "player_settings.h"
#include "u4_power.h"
#include "sd_card.h"

#include "driver/sdmmc_host.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_serial_slave_link/essl_sdio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "u4_ota_host";

#define U4_OTA_PIN_CLK GPIO_NUM_18
#define U4_OTA_PIN_CMD GPIO_NUM_19
#define U4_OTA_PIN_D0  GPIO_NUM_14
#define U4_OTA_PIN_D1  GPIO_NUM_15
#define U4_OTA_PIN_D2  GPIO_NUM_16
#define U4_OTA_PIN_D3  GPIO_NUM_17

#define U4_OTA_RECV_BUFFER   LINK_MAX_PACKET /* 1032, matches U4 sdio_slave */
#define U4_OTA_CHUNK         1000U
#define U4_OTA_ACK_TIMEOUT   2000
#define U4_OTA_OPEN_TIMEOUT  10000
#define U4_OTA_HELLO_RETRY   500
#define U4_OTA_OPEN_RETRY    500

typedef struct {
    sdmmc_card_t *card;
    sdmmc_host_t host;
    essl_handle_t essl;
    bool slot_inited;
} u4_link_t;

/* One OTA at a time, in the firmware-update task. The buffers live in DRAM
 * because the SDIO host driver and the U4 receive path expect DMA-safe data. */
static DRAM_DMA_ALIGNED_ATTR uint8_t s_tx[LINK_MAX_PACKET + 8];
static DRAM_DMA_ALIGNED_ATTR uint8_t s_rx_chunk[256];
static uint8_t s_rx_stream[1024];
static size_t s_rx_len;
static uint16_t s_tx_seq;

/* ============================== 版本比较 ============================== */

static bool version_parse(const char *text, uint32_t out[4])
{
    memset(out, 0, 4 * sizeof(uint32_t));
    if (!text || !*text) return false;
    unsigned part = 0;
    uint32_t value = 0;
    bool digit = false;
    for (const char *p = text; ; ++p) {
        if (*p >= '0' && *p <= '9') {
            if (value > 100000U) return false;
            value = value * 10U + (uint32_t)(*p - '0');
            digit = true;
        } else if (*p == '.' || *p == '\0') {
            if (!digit || part >= 4) return false;
            out[part++] = value;
            value = 0;
            digit = false;
            if (*p == '\0') break;
        } else {
            return false;
        }
    }
    return part > 0;
}

static int version_cmp(const char *a, const char *b)
{
    uint32_t va[4], vb[4];
    if (!version_parse(a, va)) return -1; /* unknown/invalid treated as older */
    if (!version_parse(b, vb)) return 1;
    for (unsigned i = 0; i < 4; ++i) {
        if (va[i] != vb[i]) return va[i] < vb[i] ? -1 : 1;
    }
    return 0;
}

/* ============================== 链路初始化 ============================== */

static void link_close(u4_link_t *l)
{
    if (!l) return;
    if (l->essl) {
        essl_sdio_deinit_dev(l->essl);
        l->essl = NULL;
    }
    if (l->slot_inited) {
        sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_1);
        l->slot_inited = false;
    }
    if (l->card) {
        heap_caps_free(l->card);
        l->card = NULL;
    }
    s_rx_len = 0;
}

static esp_err_t link_open(u4_link_t *l)
{
    if (!l) return ESP_ERR_INVALID_ARG;
    if (l->essl) return ESP_OK;

    if (!l->card) {
        l->host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
        l->host.slot = SDMMC_HOST_SLOT_1;
        l->host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
        /* Low clock keeps U4 SDIO current below the ALDO3/Bluetooth peaks. */
        l->host.max_freq_khz = 10000;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.clk = U4_OTA_PIN_CLK;
        slot.cmd = U4_OTA_PIN_CMD;
        slot.d0 = U4_OTA_PIN_D0;
        slot.d1 = U4_OTA_PIN_D1;
        slot.d2 = U4_OTA_PIN_D2;
        slot.d3 = U4_OTA_PIN_D3;
        slot.d4 = GPIO_NUM_NC;
        slot.d5 = GPIO_NUM_NC;
        slot.d6 = GPIO_NUM_NC;
        slot.d7 = GPIO_NUM_NC;
        slot.cd = SDMMC_SLOT_NO_CD;
        slot.wp = SDMMC_SLOT_NO_WP;
        slot.width = 4;
        slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        esp_err_t err = sdmmc_host_init();
        if (err != ESP_OK) return err;
        err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
        if (err != ESP_OK) return err;
        l->slot_inited = true;

        l->card = heap_caps_calloc(1, sizeof(*l->card),
                                   MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!l->card) {
            link_close(l);
            return ESP_ERR_NO_MEM;
        }
    }

    esp_err_t err = sdmmc_card_init(&l->host, l->card);
    if (err != ESP_OK) return err;

    essl_sdio_config_t config = {
        .card = l->card,
        .recv_buffer_size = U4_OTA_RECV_BUFFER,
    };
    essl_handle_t handle = NULL;
    err = essl_sdio_init_dev(&handle, &config);
    if (err != ESP_OK) return err;

    err = essl_init(handle, 1000);
    if (err != ESP_OK) {
        essl_sdio_deinit_dev(handle);
        return err;
    }
    l->essl = handle;
    s_rx_len = 0;
    s_tx_seq = 1;
    return ESP_OK;
}

/* ============================== 收发 ============================== */

static void rx_append(const uint8_t *data, size_t len)
{
    if (len > sizeof(s_rx_stream) - s_rx_len) {
        s_rx_len = 0;
    }
    memcpy(s_rx_stream + s_rx_len, data, len);
    s_rx_len += len;
}

static bool send_packet(u4_link_t *l, uint8_t type, const uint8_t *payload,
                        size_t len)
{
    if (!l || !l->essl || !payload || len > LINK_MAX_PAYLOAD) return false;
    link_header_t *hdr = (link_header_t *)s_tx;
    hdr->magic = LINK_MAGIC;
    hdr->type = type;
    hdr->len = (uint16_t)len;
    hdr->seq = s_tx_seq++;
    hdr->flags = 0;
    memcpy(s_tx + LINK_HEADER_BYTES, payload, len);
    hdr->crc = link_crc_packet(s_tx, s_tx + LINK_HEADER_BYTES, len);

    size_t total = LINK_HEADER_BYTES + len;
    size_t padded = (total + 3U) & ~(size_t)3U;
    if (padded > sizeof(s_tx)) return false;
    if (padded > total) memset(s_tx + total, 0, padded - total);
    return essl_send_packet(l->essl, s_tx, padded, 100) == ESP_OK;
}

static bool send_cmd(u4_link_t *l, uint8_t opcode, const void *arg, size_t arg_len)
{
    uint8_t payload[1 + 4];
    if (arg_len > 4) return false;
    payload[0] = opcode;
    if (arg_len) memcpy(payload + 1, arg, arg_len);
    return send_packet(l, LINK_MSG_CMD, payload, 1 + arg_len);
}

static bool send_fw_data(u4_link_t *l, uint32_t offset, const uint8_t *data,
                         size_t len)
{
    if (len > LINK_FW_MAX_CHUNK) return false;
    uint8_t payload[LINK_FW_DATA_HEADER + LINK_FW_MAX_CHUNK];
    payload[0] = LINK_CMD_FW_DATA;
    payload[1] = (uint8_t)(offset & 0xFF);
    payload[2] = (uint8_t)((offset >> 8) & 0xFF);
    payload[3] = (uint8_t)((offset >> 16) & 0xFF);
    payload[4] = (uint8_t)((offset >> 24) & 0xFF);
    memcpy(payload + LINK_FW_DATA_HEADER, data, len);
    return send_packet(l, LINK_MSG_CMD, payload,
                       LINK_FW_DATA_HEADER + len);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

/**
 * @brief 从 U4 事件流中等待指定事件。
 *
 * 这里直接解析 SDIO 字节流，不经过 bt_link 的 worker：U4 OTA 只在
 * 蓝牙默认关闭、worker 不占用 ESSL 时运行。
 */
static bool wait_event(u4_link_t *l, uint8_t want, uint8_t *out, size_t cap,
                       size_t *out_len, int timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    while (esp_timer_get_time() < deadline) {
        uint32_t available = 0;
        if (essl_get_rx_data_size(l->essl, &available, 0) == ESP_OK && available) {
            size_t want_rx = available < sizeof(s_rx_chunk) ? available : sizeof(s_rx_chunk);
            size_t got = 0;
            esp_err_t err = essl_get_packet(l->essl, s_rx_chunk, want_rx, &got, 0);
            if ((err == ESP_OK || err == ESP_ERR_NOT_FINISHED) && got) {
                rx_append(s_rx_chunk, got);
                /* Parse complete packets; discard unrelated events. */
                size_t off = 0;
                while (s_rx_len - off >= LINK_HEADER_BYTES) {
                    const uint8_t *p = s_rx_stream + off;
                    if (p[0] != LINK_MAGIC) { ++off; continue; }
                    uint16_t plen = (uint16_t)(p[2] | (p[3] << 8));
                    if (plen > LINK_MAX_PAYLOAD) { ++off; continue; }
                    size_t need = LINK_HEADER_BYTES + plen;
                    size_t padded = (need + 3U) & ~(size_t)3U;
                    if (s_rx_len - off < padded) break;
                    if (p[6] != link_crc_packet(p, p + LINK_HEADER_BYTES, plen)) {
                        ++off;
                        continue;
                    }
                    if (p[1] == LINK_MSG_EVENT && plen >= 1 &&
                        p[LINK_HEADER_BYTES] == want) {
                        size_t copy = plen < cap ? plen : cap;
                        memcpy(out, p + LINK_HEADER_BYTES, copy);
                        if (out_len) *out_len = copy;
                        off += padded;
                        if (off) {
                            memmove(s_rx_stream, s_rx_stream + off, s_rx_len - off);
                            s_rx_len -= off;
                        }
                        return true;
                    }
                    off += padded;
                }
                if (off) {
                    memmove(s_rx_stream, s_rx_stream + off, s_rx_len - off);
                    s_rx_len -= off;
                }
                if (got < want_rx) continue;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return false;
}

typedef struct {
    uint8_t status;
    uint32_t next_offset;
} fw_ack_t;

static bool wait_ack(u4_link_t *l, uint8_t opcode, fw_ack_t *ack, int timeout_ms)
{
    uint8_t ev[32];
    size_t len = 0;
    if (!wait_event(l, LINK_EVT_FW_ACK, ev, sizeof(ev), &len, timeout_ms))
        return false;
    /* wait_event returns the full event payload: ev[0] is the event code,
     * ev[1] the acknowledged command, ev[2] the status, ev[3..6] next. */
    if (len < 7 || ev[1] != opcode) return false;
    if (ack) {
        ack->status = ev[2];
        ack->next_offset = get_u32(ev + 3);
    }
    return true;
}

static bool query_info(u4_link_t *l, u4_ota_result_t *res)
{
    uint8_t ev[64];
    size_t len = 0;

    /* HELLO may be answered with error 1 until the U4's Bluetooth task is
     * ready; retry for up to 10 s. */
    int64_t deadline = esp_timer_get_time() + (int64_t)U4_OTA_OPEN_TIMEOUT * 1000;
    while (esp_timer_get_time() < deadline) {
        if (send_cmd(l, LINK_CMD_HELLO, NULL, 0)) {
            if (wait_event(l, LINK_EVT_READY, ev, sizeof(ev), &len, 1500))
                break;
        }
        vTaskDelay(pdMS_TO_TICKS(U4_OTA_HELLO_RETRY));
    }
    if (esp_timer_get_time() >= deadline) {
        ESP_LOGE(TAG, "U4 did not answer HELLO");
        return false;
    }

    if (!send_cmd(l, LINK_CMD_FW_INFO, NULL, 0)) return false;
    if (!wait_event(l, LINK_EVT_FW_INFO, ev, sizeof(ev), &len, 2000)) {
        ESP_LOGW(TAG, "U4 has no FW_INFO (old protocol; UART flash required)");
        return false;
    }
    if (len < 3 + 32) return false;
    res->slot = ev[1];
    res->ota_capable = (ev[2] & LINK_FW_FLAG_OTA_CAPABLE) != 0;
    size_t n = strnlen((const char *)ev + 3, 32);
    if (n >= sizeof(res->version)) n = sizeof(res->version) - 1;
    memcpy(res->version, ev + 3, n);
    res->version[n] = '\0';
    return true;
}

/* ============================== 更新 ============================== */

static esp_err_t transfer_image(u4_link_t *l, const char *path, uint32_t size,
                                void (*progress)(uint8_t, void *), void *ctx)
{
    if (!sd_card_io_lock(pdMS_TO_TICKS(5000))) return ESP_ERR_TIMEOUT;

    esp_err_t result = ESP_FAIL;
    int fd = -1;
    uint8_t *buf = NULL;

    fd = open(path, O_RDONLY);
    if (fd < 0) {
        result = ESP_ERR_NOT_FOUND;
        goto out;
    }
    struct stat st;
    if (stat(path, &st) != 0 || (uint32_t)st.st_size != size) {
        result = ESP_ERR_INVALID_SIZE;
        goto out;
    }

    buf = heap_caps_malloc(U4_OTA_CHUNK, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    if (!buf) {
        result = ESP_ERR_NO_MEM;
        goto out;
    }

    /* Stop any active A2DP stream before touching the U4 flash. */
    (void)send_cmd(l, LINK_CMD_MEDIA_STOP, NULL, 0);
    (void)send_cmd(l, LINK_CMD_DISCONNECT, NULL, 0);
    vTaskDelay(pdMS_TO_TICKS(300));

    uint8_t begin[4] = {
        (uint8_t)(size & 0xFF), (uint8_t)((size >> 8) & 0xFF),
        (uint8_t)((size >> 16) & 0xFF), (uint8_t)((size >> 24) & 0xFF),
    };
    if (!send_cmd(l, LINK_CMD_FW_BEGIN, begin, sizeof(begin))) {
        result = ESP_FAIL;
        goto abort;
    }
    fw_ack_t ack;
    if (!wait_ack(l, LINK_CMD_FW_BEGIN, &ack, U4_OTA_ACK_TIMEOUT) ||
        ack.status != LINK_FW_STATUS_OK) {
        result = ESP_FAIL;
        goto abort;
    }

    uint32_t written = 0;
    while (written < size) {
        size_t want = size - written;
        if (want > U4_OTA_CHUNK) want = U4_OTA_CHUNK;
        ssize_t n = read(fd, buf, want);
        if (n <= 0) {
            result = ESP_FAIL;
            goto abort;
        }
        bool chunk_ok = false;
        for (unsigned tries = 0; tries < 3 && !chunk_ok; ++tries) {
            if (!send_fw_data(l, written, buf, (size_t)n)) continue;
            if (!wait_ack(l, LINK_CMD_FW_DATA, &ack, U4_OTA_ACK_TIMEOUT))
                continue;
            if (ack.status == LINK_FW_STATUS_OK &&
                ack.next_offset == written + (uint32_t)n) {
                chunk_ok = true;
            } else if (ack.status == LINK_FW_STATUS_BAD_OFFSET &&
                       ack.next_offset == written + (uint32_t)n) {
                /* ACK was lost but the U4 already has this chunk. */
                chunk_ok = true;
            }
        }
        if (!chunk_ok) {
            result = ESP_FAIL;
            goto abort;
        }
        written += (uint32_t)n;
        if (progress) progress((uint8_t)((uint64_t)written * 100U / size), ctx);
    }

    if (!send_cmd(l, LINK_CMD_FW_END, NULL, 0) ||
        !wait_ack(l, LINK_CMD_FW_END, &ack, U4_OTA_ACK_TIMEOUT) ||
        ack.status != LINK_FW_STATUS_OK) {
        result = ESP_FAIL;
        goto abort;
    }
    if (!send_cmd(l, LINK_CMD_FW_COMMIT, NULL, 0) ||
        !wait_ack(l, LINK_CMD_FW_COMMIT, &ack, U4_OTA_ACK_TIMEOUT) ||
        ack.status != LINK_FW_STATUS_OK) {
        result = ESP_FAIL;
        goto abort;
    }

    result = ESP_OK;
    goto out;

abort:
    (void)send_cmd(l, LINK_CMD_FW_ABORT, NULL, 0);
out:
    if (buf) heap_caps_free(buf);
    if (fd >= 0) close(fd);
    sd_card_io_unlock();
    return result;
}

static esp_err_t open_u4_link(u4_link_t *l)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)U4_OTA_OPEN_TIMEOUT * 1000;
    while (esp_timer_get_time() < deadline) {
        esp_err_t err = link_open(l);
        if (err == ESP_OK) return ESP_OK;
        ESP_LOGW(TAG, "open U4 link: %s", esp_err_to_name(err));
        link_close(l);
        vTaskDelay(pdMS_TO_TICKS(U4_OTA_OPEN_RETRY));
    }
    return ESP_ERR_TIMEOUT;
}

esp_err_t u4_ota_host_update(const char *path, uint32_t size,
                             const char *bin_version,
                             void (*progress)(uint8_t, void *), void *ctx,
                             u4_ota_result_t *out)
{
    if (!path || !size || !bin_version) return ESP_ERR_INVALID_ARG;
    if (bt_audio_is_enabled()) {
        /* Never fight the normal bt_link worker or an active A2DP stream. */
        return ESP_ERR_INVALID_STATE;
    }

    u4_ota_result_t res = {0};
    u4_link_t link = {0};

    ESP_LOGI(TAG, "U4 update requested: %s (%s), %u bytes",
             path, bin_version, (unsigned)size);
    /* Keep the bt_link worker's initial CHIP_PU reset out of this window. */
    bt_link_u4_ota_set_active(true);
    u4_power_release();
    vTaskDelay(pdMS_TO_TICKS(1500));

    esp_err_t err = open_u4_link(&link);
    if (err != ESP_OK) goto done;

    if (!query_info(&link, &res)) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    bt_audio_set_u4_version(res.version);
    /* Persist before a possible P4 PMIC restart happens within ~1 s. */
    (void)player_settings_save_u4_version(res.version);
    ESP_LOGI(TAG, "U4 running %s (slot %u, ota=%d)", res.version,
             (unsigned)res.slot, res.ota_capable);
    if (!res.ota_capable) {
        err = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }

    if (version_cmp(res.version, bin_version) >= 0) {
        ESP_LOGI(TAG, "U4 already at %s; no transfer", res.version);
        err = ESP_OK;
        goto done;
    }

    err = transfer_image(&link, path, size, progress, ctx);
    if (err != ESP_OK) goto done;

    /* The U4 only set its boot partition; reboot it deterministically via
     * CHIP_PU, then reopen and verify the running version. */
    link_close(&link);
    u4_power_pulse_reset(1000);
    vTaskDelay(pdMS_TO_TICKS(1500));
    err = open_u4_link(&link);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "U4 did not come back after OTA; check rollback/UART");
        goto done;
    }
    u4_ota_result_t after = {0};
    if (!query_info(&link, &after)) {
        ESP_LOGE(TAG, "U4 FW_INFO missing after reboot");
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    bt_audio_set_u4_version(after.version);
    (void)player_settings_save_u4_version(after.version);
    res = after;
    if (version_cmp(after.version, bin_version) < 0) {
        ESP_LOGE(TAG, "U4 still at %s after reboot (rollback?)", after.version);
        err = ESP_ERR_INVALID_STATE;
        goto done;
    }
    res.updated = true;
    err = ESP_OK;
    ESP_LOGI(TAG, "U4 updated to %s (slot %u)", res.version, (unsigned)res.slot);

done:
    link_close(&link);
    u4_power_hold_reset();
    bt_link_u4_ota_set_active(false);
    if (out) *out = res;
    return err;
}
