#include "u4_ota.h"

#include <string.h>

#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"

#include "link_protocol.h"
#include "sdio_link.h"

static const char *TAG = "u4_ota";

static esp_ota_handle_t s_handle;
static const esp_partition_t *s_part;
static uint32_t s_image_size;
static uint32_t s_next_offset;
static bool s_active;
static bool s_verified;

static void send_ack(uint8_t opcode, uint8_t status, uint32_t next)
{
    link_send_fw_ack(opcode, status, next);
}

static uint32_t get_u32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void clear_state(bool abort_handle)
{
    if (abort_handle && s_handle) esp_ota_abort(s_handle);
    s_handle = 0;
    s_part = NULL;
    s_image_size = 0;
    s_next_offset = 0;
    s_active = false;
    s_verified = false;
}

static void handle_info(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    uint8_t slot = 0;
    if (running && running->subtype >= ESP_PARTITION_SUBTYPE_APP_OTA_MIN)
        slot = (uint8_t)(running->subtype - ESP_PARTITION_SUBTYPE_APP_OTA_MIN);
    const esp_app_desc_t *desc = esp_app_get_description();
    link_send_fw_info(slot, LINK_FW_FLAG_OTA_CAPABLE,
                      desc ? desc->version : "");
}

static void handle_begin(const uint8_t *payload, size_t len)
{
    if (len < LINK_FW_DATA_HEADER) {
        send_ack(LINK_CMD_FW_BEGIN, LINK_FW_STATUS_ERROR, 0);
        return;
    }
    uint32_t size = get_u32(payload + 1);
    const esp_partition_t *part = esp_ota_get_next_update_partition(NULL);
    if (!part || size == 0 || size > part->size) {
        ESP_LOGE(TAG, "FW_BEGIN rejected: part=%p size=%u", part,
                 (unsigned)size);
        send_ack(LINK_CMD_FW_BEGIN, LINK_FW_STATUS_ERROR, 0);
        return;
    }

    clear_state(true);
    esp_err_t err = esp_ota_begin(part, OTA_WITH_SEQUENTIAL_WRITES, &s_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
        s_handle = 0;
        send_ack(LINK_CMD_FW_BEGIN, LINK_FW_STATUS_ERROR, 0);
        return;
    }
    s_part = part;
    s_image_size = size;
    s_next_offset = 0;
    s_active = true;
    s_verified = false;
    ESP_LOGI(TAG, "OTA begin: %s at 0x%08x, %u bytes",
             part->label, (unsigned)part->address, (unsigned)size);
    send_ack(LINK_CMD_FW_BEGIN, LINK_FW_STATUS_OK, 0);
}

static void handle_data(const uint8_t *payload, size_t len)
{
    if (!s_active || !s_handle || len < LINK_FW_DATA_HEADER) {
        send_ack(LINK_CMD_FW_DATA, LINK_FW_STATUS_ERROR, s_next_offset);
        return;
    }
    uint32_t offset = get_u32(payload + 1);
    size_t data_len = len - LINK_FW_DATA_HEADER;
    if (offset != s_next_offset) {
        send_ack(LINK_CMD_FW_DATA, LINK_FW_STATUS_BAD_OFFSET, s_next_offset);
        return;
    }
    if (s_next_offset + data_len > s_image_size) {
        send_ack(LINK_CMD_FW_DATA, LINK_FW_STATUS_ERROR, s_next_offset);
        return;
    }

    /* payload points into the SDIO receive buffer (internal DMA RAM); the
     * flash driver can read it directly while the cache is off. */
    esp_err_t err = esp_ota_write(s_handle, payload + LINK_FW_DATA_HEADER,
                                  data_len);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_write at %u failed: %s", (unsigned)offset,
                 esp_err_to_name(err));
        send_ack(LINK_CMD_FW_DATA, LINK_FW_STATUS_ERROR, s_next_offset);
        return;
    }
    s_next_offset += (uint32_t)data_len;
    send_ack(LINK_CMD_FW_DATA, LINK_FW_STATUS_OK, s_next_offset);
}

static void handle_end(void)
{
    if (!s_active || s_next_offset != s_image_size || !s_handle) {
        send_ack(LINK_CMD_FW_END, LINK_FW_STATUS_ERROR, s_next_offset);
        return;
    }
    esp_err_t err = esp_ota_end(s_handle);
    s_handle = 0;
    s_active = false;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(err));
        s_verified = false;
        send_ack(LINK_CMD_FW_END, LINK_FW_STATUS_ERROR, s_next_offset);
        return;
    }
    s_verified = true;
    ESP_LOGI(TAG, "OTA image verified (%u bytes)", (unsigned)s_next_offset);
    send_ack(LINK_CMD_FW_END, LINK_FW_STATUS_OK, s_next_offset);
}

static void handle_commit(void)
{
    if (!s_verified || !s_part) {
        send_ack(LINK_CMD_FW_COMMIT, LINK_FW_STATUS_ERROR, 0);
        return;
    }
    esp_err_t err = esp_ota_set_boot_partition(s_part);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition failed: %s",
                 esp_err_to_name(err));
        send_ack(LINK_CMD_FW_COMMIT, LINK_FW_STATUS_ERROR, 0);
        return;
    }
    ESP_LOGI(TAG, "OTA committed; next boot from %s", s_part->label);
    send_ack(LINK_CMD_FW_COMMIT, LINK_FW_STATUS_OK, 0);
    /* Reboot is controlled by the P4 via CHIP_PU after it receives this ACK;
     * resetting here could drop the ACK before the event task sends it. */
    s_part = NULL;
}

bool u4_ota_handle_cmd(const uint8_t *payload, size_t len)
{
    if (!payload || len == 0) return false;
    switch (payload[0]) {
    case LINK_CMD_FW_INFO:
        handle_info();
        return true;
    case LINK_CMD_FW_BEGIN:
        handle_begin(payload, len);
        return true;
    case LINK_CMD_FW_DATA:
        handle_data(payload, len);
        return true;
    case LINK_CMD_FW_END:
        handle_end();
        return true;
    case LINK_CMD_FW_COMMIT:
        handle_commit();
        return true;
    case LINK_CMD_FW_ABORT:
        clear_state(true);
        send_ack(LINK_CMD_FW_ABORT, LINK_FW_STATUS_OK, 0);
        return true;
    default:
        return false;
    }
}

void u4_ota_mark_valid_if_pending(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) return;
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK) return;
    if (state != ESP_OTA_IMG_PENDING_VERIFY) return;

    esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Running OTA image confirmed: %s", running->label);
    } else {
        ESP_LOGE(TAG, "Failed to confirm OTA image: %s", esp_err_to_name(err));
    }
}
