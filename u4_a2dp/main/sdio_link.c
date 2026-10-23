/*
 * SDIO slave transport for the ESP32-U4WDH Bluetooth coprocessor.
 * See sdio_link.h and link_protocol.h.
 */
#include "sdio_link.h"
#include "link_protocol.h"

#include <math.h>
#include <string.h>
#include <stdatomic.h>

#include "driver/sdio_slave.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

static const char *TAG = "u4_link";

#define LINK_RING_BYTES      (16U * 1024U)     /* power of two; ~85 ms at 48 kHz */
#define LINK_RECV_BUFS       8U
#define LINK_EVENT_SLOTS     8U
#define LINK_EVENT_MAX       128U
#define LINK_EVENT_QUEUE     16U
#define LINK_TASK_STACK      4096U
#define LINK_RECV_PRIO       5U
#define LINK_EVENT_PRIO      4U

/* Receive buffers must be DMA capable and at least the negotiated size. */
static DMA_ATTR uint8_t s_recv_buf[LINK_RECV_BUFS][LINK_MAX_PACKET];
/* Event buffers are handed to the SDIO driver until it reports finished. */
static DMA_ATTR uint8_t s_event_buf[LINK_EVENT_SLOTS][LINK_EVENT_MAX];
static size_t s_event_len[LINK_EVENT_SLOTS];
static atomic_uint s_event_used;
static atomic_uint s_event_seq;

static QueueHandle_t s_event_q;
static link_cmd_cb_t s_cmd_cb;
static uint8_t *s_ring;
static atomic_uint s_head;
static atomic_uint s_tail;
static atomic_bool s_host_present;
static atomic_bool s_tone_enabled = true;
static atomic_uint s_rate = 48000;
static uint32_t s_tone_phase;
static int16_t s_sine[64];
static bool s_link_ready;
static int s_last_bt_state = -1;
static int s_last_media_state = -1;
static uint32_t s_dropped_pcm;

static void send_event(uint8_t code, const void *payload, size_t len);

static void update_credit(void)
{
    if (!s_link_ready) return;
    unsigned head = atomic_load_explicit(&s_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&s_tail, memory_order_relaxed);
    size_t free_bytes = LINK_RING_BYTES - (size_t)(head - tail);
    unsigned credit = (unsigned)(free_bytes / 256U);
    if (credit > 255U) credit = 255U;
    sdio_slave_write_reg(LINK_REG_PCM_CREDIT, (uint8_t)credit);
}

static size_t pcm_write(const uint8_t *src, size_t len)
{
    unsigned head = atomic_load_explicit(&s_head, memory_order_relaxed);
    unsigned tail = atomic_load_explicit(&s_tail, memory_order_acquire);
    size_t free_bytes = LINK_RING_BYTES - (size_t)(head - tail);
    if (len > free_bytes) {
        s_dropped_pcm += (uint32_t)(len - free_bytes);
        len = free_bytes;
        len &= ~(size_t)3U; /* keep 16-bit stereo frame alignment */
    }
    size_t off = head & (LINK_RING_BYTES - 1U);
    size_t first = LINK_RING_BYTES - off;
    if (first > len) first = len;
    if (first) memcpy(s_ring + off, src, first);
    if (len > first) memcpy(s_ring, src + first, len - first);
    atomic_store_explicit(&s_head, head + (unsigned)len, memory_order_release);
    update_credit();
    return len;
}

size_t link_pcm_read(void *dst, size_t bytes)
{
    if (!dst || !bytes || !s_ring) return 0;
    bytes &= ~(size_t)3U; /* keep 16-bit stereo frame alignment */
    unsigned head = atomic_load_explicit(&s_head, memory_order_acquire);
    unsigned tail = atomic_load_explicit(&s_tail, memory_order_relaxed);
    size_t avail = (size_t)(head - tail);
    if (avail > bytes) avail = bytes;
    size_t off = tail & (LINK_RING_BYTES - 1U);
    size_t first = LINK_RING_BYTES - off;
    if (first > avail) first = avail;
    if (first) memcpy(dst, s_ring + off, first);
    if (avail > first) memcpy((uint8_t *)dst + first, s_ring, avail - first);
    atomic_store_explicit(&s_tail, tail + (unsigned)avail, memory_order_release);
    update_credit();
    return avail;
}

bool link_host_present(void)
{
    return atomic_load(&s_host_present);
}

void link_fill_silence_or_tone(uint8_t *dst, size_t bytes)
{
    if (!dst || !bytes) return;
    if (atomic_load(&s_host_present) || !atomic_load(&s_tone_enabled)) {
        memset(dst, 0, bytes);
        return;
    }

    unsigned rate = atomic_load(&s_rate);
    if (!rate) rate = 48000;
    uint32_t step = (uint32_t)((uint64_t)440U * 4294967296ULL / rate);
    size_t frames = bytes / 4U;
    int16_t *out = (int16_t *)dst;
    for (size_t i = 0; i < frames; ++i) {
        int16_t v = (int16_t)(((int32_t)s_sine[(s_tone_phase >> 26) & 63U] * 2500) / 32767);
        out[0] = v;
        out[1] = v;
        out += 2;
        s_tone_phase += step;
    }
    size_t done = frames * 4U;
    if (done < bytes) memset(dst + done, 0, bytes - done);
}

void link_set_audio_rate(uint32_t rate)
{
    if (rate != 44100 && rate != 48000) rate = 48000;
    atomic_store(&s_rate, rate);
    s_tone_phase = 0;
    if (s_link_ready) {
        uint8_t code = rate == 48000 ? LINK_AUDIO_RATE_48K : LINK_AUDIO_RATE_44K;
        sdio_slave_write_reg(LINK_REG_AUDIO_RATE, code);
    }
    uint16_t hz = (uint16_t)rate;
    send_event(LINK_EVT_AUDIO_RATE, &hz, sizeof(hz));
}

static int event_alloc(void)
{
    unsigned used = atomic_load(&s_event_used);
    for (;;) {
        for (int i = 0; i < (int)LINK_EVENT_SLOTS; ++i) {
            unsigned bit = 1U << i;
            if (!(used & bit)) {
                unsigned next = used | bit;
                if (atomic_compare_exchange_weak(&s_event_used, &used, next))
                    return i;
                break;
            }
        }
        if (used == (1U << LINK_EVENT_SLOTS) - 1U) return -1;
    }
}

static void event_free(int slot)
{
    atomic_fetch_and(&s_event_used, ~(1U << slot));
}

static void send_event(uint8_t code, const void *payload, size_t len)
{
    if (!s_link_ready || !atomic_load(&s_host_present)) return;
    if (!s_event_q) return;
    size_t cap = LINK_EVENT_MAX - LINK_HEADER_BYTES - 1U;
    if (len > cap) len = cap;
    int slot = event_alloc();
    if (slot < 0) return;
    uint8_t *buf = s_event_buf[slot];
    link_header_t *hdr = (link_header_t *)buf;
    hdr->magic = LINK_MAGIC;
    hdr->type = LINK_MSG_EVENT;
    hdr->len = (uint16_t)(1U + len);
    hdr->seq = (uint16_t)atomic_fetch_add(&s_event_seq, 1);
    hdr->flags = 0;
    buf[LINK_HEADER_BYTES] = code;
    if (len) memcpy(buf + LINK_HEADER_BYTES + 1, payload, len);
    hdr->crc = link_crc_packet(buf, buf + LINK_HEADER_BYTES, hdr->len);
    /* SDIO transfers are padded to 32-bit boundaries; the padding is not part
     * of header.len and not covered by the CRC. */
    size_t total = LINK_HEADER_BYTES + hdr->len;
    size_t padded = (total + 3U) & ~(size_t)3U;
    if (padded > LINK_EVENT_MAX) padded = LINK_EVENT_MAX;
    if (padded > total) memset(buf + total, 0, padded - total);
    s_event_len[slot] = padded;
    if (xQueueSend(s_event_q, &slot, pdMS_TO_TICKS(20)) != pdTRUE)
        event_free(slot);
}

void link_send_ready(void)
{
    uint8_t ver = LINK_PROTO_VERSION;
    send_event(LINK_EVT_READY, &ver, sizeof(ver));
}

void link_update_bt_state(int bt_state, int media_state)
{
    if (s_link_ready) {
        sdio_slave_write_reg(LINK_REG_BT_STATE, (uint8_t)bt_state);
        sdio_slave_write_reg(LINK_REG_MEDIA_STATE, (uint8_t)media_state);
    }
    if (bt_state != s_last_bt_state || media_state != s_last_media_state) {
        s_last_bt_state = bt_state;
        s_last_media_state = media_state;
        uint8_t payload[2] = {(uint8_t)bt_state, (uint8_t)media_state};
        send_event(LINK_EVT_STATE, payload, sizeof(payload));
        if (media_state == LINK_MEDIA_STARTED || media_state == LINK_MEDIA_IDLE)
            send_event(LINK_EVT_MEDIA, &payload[1], 1);
        if (bt_state == LINK_BT_CONNECTED) {
            /* main.c sends the address explicitly; keep state event generic */
        }
    }
}

void link_send_scan_result(const uint8_t bda[6], const char *name,
                           int8_t rssi, uint32_t cod)
{
    struct __attribute__((packed)) {
        uint8_t bda[6];
        int8_t rssi;
        uint32_t cod;
        uint8_t name_len;
        char name[32];
    } ev;
    memset(&ev, 0, sizeof(ev));
    memcpy(ev.bda, bda, 6);
    ev.rssi = rssi;
    ev.cod = cod;
    size_t n = name ? strlen(name) : 0;
    if (n > sizeof(ev.name)) n = sizeof(ev.name);
    ev.name_len = (uint8_t)n;
    if (n) memcpy(ev.name, name, n);
    send_event(LINK_EVT_SCAN_RESULT, &ev, 6 + 1 + 4 + 1 + n);
}

void link_send_connected(const uint8_t bda[6])
{
    send_event(LINK_EVT_CONNECTED, bda, 6);
}

void link_send_disconnected(void)
{
    send_event(LINK_EVT_DISCONNECTED, NULL, 0);
}

void link_send_error(uint8_t code)
{
    send_event(LINK_EVT_ERROR, &code, 1);
}

static void mark_host_present(void)
{
    if (!atomic_exchange(&s_host_present, true)) {
        atomic_store(&s_tone_enabled, false);
        if (s_link_ready)
            sdio_slave_write_reg(LINK_REG_FLAGS, LINK_FLAG_HOST_PRESENT);
        ESP_LOGI(TAG, "P4 host detected; test tone disabled");
    }
}

static void handle_packet(const uint8_t *buf, size_t len)
{
    if (!buf || len < LINK_HEADER_BYTES) return;
    const link_header_t *hdr = (const link_header_t *)buf;
    size_t actual = (size_t)LINK_HEADER_BYTES + hdr->len;
    if (hdr->magic != LINK_MAGIC || hdr->len > LINK_MAX_PAYLOAD ||
        len < actual || len > actual + 3U)
        return; /* host pads the transfer to a 32-bit boundary */
    if (hdr->crc != link_crc_packet(buf, buf + LINK_HEADER_BYTES, hdr->len))
        return;

    if (hdr->type == LINK_MSG_CMD || hdr->type == LINK_MSG_PCM)
        mark_host_present();

    if (hdr->type == LINK_MSG_PCM) {
        if (hdr->len >= 4 && !(hdr->len & 1U))
            pcm_write(buf + LINK_HEADER_BYTES, hdr->len);
    } else if (hdr->type == LINK_MSG_CMD) {
        if (hdr->len && s_cmd_cb)
            s_cmd_cb(buf + LINK_HEADER_BYTES, hdr->len);
    }
}

static void link_recv_task(void *arg)
{
    (void)arg;
    bool discard = false;
    for (;;) {
        sdio_slave_buf_handle_t handle = NULL;
        esp_err_t ret = sdio_slave_recv_packet(&handle, portMAX_DELAY);
        if (ret == ESP_ERR_NOT_FINISHED) {
            /* Host packet larger than one buffer: drop it entirely. */
            discard = true;
            sdio_slave_recv_load_buf(handle);
            continue;
        }
        if (ret != ESP_OK) {
            discard = false;
            continue;
        }
        if (!discard) {
            size_t len = 0;
            uint8_t *buf = sdio_slave_recv_get_buf(handle, &len);
            handle_packet(buf, len);
        }
        discard = false;
        sdio_slave_recv_load_buf(handle);
    }
}

static void link_event_task(void *arg)
{
    (void)arg;
    uint8_t slot;
    for (;;) {
        if (xQueueReceive(s_event_q, &slot, portMAX_DELAY) != pdTRUE) continue;
        void *done_arg = NULL;
        if (sdio_slave_send_queue(s_event_buf[slot], s_event_len[slot],
                                  (void *)(uintptr_t)slot, portMAX_DELAY) == ESP_OK) {
            /* Signal before waiting for the host to drain the packet; the
             * host interrupt is what makes the P4 read the send FIFO. */
            sdio_slave_send_host_int(0);
            sdio_slave_send_get_finished(&done_arg, portMAX_DELAY);
        }
        event_free(slot);
    }
}

void link_init(link_cmd_cb_t cmd_cb)
{
    if (s_link_ready) return;
    s_cmd_cb = cmd_cb;
    atomic_store(&s_host_present, false);
    atomic_store(&s_tone_enabled, true);
    atomic_store(&s_rate, 48000);
    s_tone_phase = 0;
    s_dropped_pcm = 0;
    atomic_init(&s_head, 0);
    atomic_init(&s_tail, 0);
    atomic_init(&s_event_used, 0);
    atomic_init(&s_event_seq, 1);
    s_last_bt_state = s_last_media_state = -1;

    for (unsigned i = 0; i < 64; ++i)
        s_sine[i] = (int16_t)(sinf(6.28318530718f * (float)i / 64.0f) * 32767.0f);

    s_ring = heap_caps_malloc(LINK_RING_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_event_q = xQueueCreate(LINK_EVENT_QUEUE, sizeof(uint8_t));
    if (!s_ring || !s_event_q) {
        ESP_LOGE(TAG, "PCM ring/event queue allocation failed; Bluetooth stays standalone");
        return;
    }

    sdio_slave_config_t config = {
        .sending_mode = SDIO_SLAVE_SEND_PACKET,
        .send_queue_size = LINK_EVENT_QUEUE,
        .recv_buffer_size = LINK_MAX_PACKET,
        .event_cb = NULL,
        .flags = 0, /* eFuse fixed VDD_SDIO=3.3V, DAT2 stays enabled */
    };
    esp_err_t ret = sdio_slave_initialize(&config);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "sdio_slave_initialize failed: %s; Bluetooth stays standalone",
                 esp_err_to_name(ret));
        return;
    }
    for (unsigned i = 0; i < LINK_RECV_BUFS; ++i) {
        sdio_slave_buf_handle_t h = sdio_slave_recv_register_buf(s_recv_buf[i]);
        if (!h || sdio_slave_recv_load_buf(h) != ESP_OK) {
            ESP_LOGE(TAG, "SDIO receive buffer %u registration failed", i);
            return;
        }
    }
    sdio_slave_set_host_intena(SDIO_SLAVE_HOSTINT_BIT0 |
                               SDIO_SLAVE_HOSTINT_SEND_NEW_PACKET);
    sdio_slave_start();

    s_link_ready = true;
    sdio_slave_write_reg(LINK_REG_VERSION, LINK_PROTO_VERSION);
    sdio_slave_write_reg(LINK_REG_BT_STATE, LINK_BT_IDLE);
    sdio_slave_write_reg(LINK_REG_MEDIA_STATE, LINK_MEDIA_IDLE);
    sdio_slave_write_reg(LINK_REG_AUDIO_RATE, LINK_AUDIO_RATE_48K);
    sdio_slave_write_reg(LINK_REG_FLAGS, LINK_FLAG_TEST_TONE);
    update_credit();

    if (xTaskCreate(link_recv_task, "u4_link_rx", LINK_TASK_STACK, NULL,
                    LINK_RECV_PRIO, NULL) != pdPASS ||
        xTaskCreate(link_event_task, "u4_link_tx", LINK_TASK_STACK, NULL,
                    LINK_EVENT_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "SDIO link task creation failed");
        s_link_ready = false;
        return;
    }
    ESP_LOGI(TAG, "SDIO link ready: %u B PCM ring, %u B packets",
             (unsigned)LINK_RING_BYTES, (unsigned)LINK_MAX_PACKET);
}
