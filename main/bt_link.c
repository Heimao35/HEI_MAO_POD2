/**
 * @file bt_link.c
 * @brief P4 SDMMC slot1 + ESSL 板间链路 worker。
 *
 * 硬件连接（与 docs/BLUETOOTH_U4.md 一致）：
 *   P4 GPIO18 -> U4 CLK,  GPIO19 -> CMD,
 *   GPIO14/15/16/17 -> DAT0/1/2/3（P4 侧矩阵路由，slot0 的 SD 卡不受影响）。
 * U4 已烧写 XPD_SDIO_* eFuse 固定 VDD_SDIO=3.3 V，因此 DAT2 上拉可保留，
 * 链路按 4-bit 初始化。U4 始终由 ALDO3 供电，本模块不驱动 CHIP_PU，
 * 避免 P4 每次启动都复位蓝牙协处理器。
 *
 * 协议：
 * - 主机 -> U4：LINK_MSG_CMD / LINK_MSG_PCM，经 essl_send_packet 写入 U4
 *   sdio_slave 接收缓冲区；单包不超过 1024 字节（两个 SDIO 块）。
 * - U4 -> 主机：LINK_MSG_EVENT 经 slave 发送 FIFO 到达；U4 在入队后拉
 *   host interrupt 0。ESSL 只提供字节流，因此这里维护 8 字节头 + CRC8
 *   的流式解析器和 4 字节对齐填充。
 * - HELLO 在链路打开后发送；U4 的 BT 协议栈尚未就绪时可能返回错误，
 *   worker 每秒重试，直到收到 LINK_EVT_READY。
 *
 * 当前阶段不做 PCM 转发（音频路由后续接入）；本文件已经读取并记录
 * AUDIO_RATE 事件，便于下一阶段按实际 SBC 采样率重采样。
 */

#include "bt_link.h"

#include <stdatomic.h>
#include <stddef.h>
#include <string.h>

#include "bt_audio.h"
#include "bt_audio_stream.h"
#include "link_protocol.h"
#include "u4_power.h"
#include "driver/gpio.h"
#include "driver/sdmmc_host.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_serial_slave_link/essl_sdio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "sdmmc_cmd.h"

static const char *TAG = "bt_link";

/* ============================ 硬件配置 ============================ */

#define BT_LINK_PIN_CLK GPIO_NUM_18
#define BT_LINK_PIN_CMD GPIO_NUM_19
#define BT_LINK_PIN_D0  GPIO_NUM_14
#define BT_LINK_PIN_D1  GPIO_NUM_15
#define BT_LINK_PIN_D2  GPIO_NUM_16
#define BT_LINK_PIN_D3  GPIO_NUM_17

/* 与 U4 sdio_slave_recv 的 buffer 大小一致；ESSL 用它计算 TX buffer 数。 */
#define BT_LINK_RECV_BUFFER BT_LINK_RECV_BUFFER_SIZE

#define BT_LINK_TASK_STACK  6144
#define BT_LINK_TASK_PRIO   5
#define BT_LINK_WAIT_MS     20
/* U4 由 ALDO3（额定 300 mA）供电，蓝牙 PHY 校准/发射峰值可能触发
 * brownout。启动和重开链路都留出 3 秒，让 U4 完成 BT 初始化；SDIO
 * 失败时也先完全松开总线，避免持续轮询叠加负载。 */
#define BT_LINK_OPEN_RETRY_MS 3000
#define BT_LINK_HELLO_RETRY_MS 1000
#define BT_LINK_REG_POLL_MS 500
#define BT_LINK_REG_FAIL_LIMIT 3
#define BT_LINK_MAX_SEND_FAILS 20

/* ============================ 命令队列 ============================ */

enum {
    BT_LINK_MSG_SCAN_START = 1,
    BT_LINK_MSG_SCAN_STOP,
    BT_LINK_MSG_CONNECT,
    BT_LINK_MSG_DISCONNECT,
    BT_LINK_MSG_MEDIA_START,
    BT_LINK_MSG_MEDIA_STOP,
    BT_LINK_MSG_VOLUME,
};

typedef struct {
    uint8_t cmd;
    uint8_t bda[6];
} bt_link_msg_t;

/* ============================ worker 状态 ============================ */

static QueueHandle_t s_queue;
static TaskHandle_t s_task;
static atomic_bool s_ready;      /**< 已收到 U4 的 READY 事件 */
static atomic_bool s_enabled;    /**< 用户开关意图（由 bt_audio 设置） */

static sdmmc_card_t *s_card;
static sdmmc_host_t s_host;
static essl_handle_t s_essl;
static bool s_slot_inited;
static bool s_hello_confirmed;
static bool s_applied_enabled;
/* 扫描/连接时序：U4 的 CONNECT 处理会 cancel_discovery() 后立即 connect，
 * 询问尚未停止时容易导致 SDP/AVDTP 超时。这里先发 SCAN_STOP，等 U4 的
 * STATE 事件确认已离开 DISCOVERING 后再发 CONNECT；事件丢失时 2 秒兜底。 */
static bool s_scan_active;
static int s_bt_state = LINK_BT_IDLE;
static bool s_connect_pending;
static bool s_stop_seen;
static uint8_t s_pending_bda[6];
static int64_t s_connect_not_before_us;
static int64_t s_connect_deadline_us;
static int64_t s_last_hello_us;
static int64_t s_last_reg_us;
static int64_t s_last_open_log_us;
static int64_t s_u4_reset_us;
static uint16_t s_tx_seq;
static unsigned s_send_failures;
static unsigned s_reg_failures;
static uint16_t s_audio_rate;
static bool s_media_streaming;
static int64_t s_last_pcm_us;
static int64_t s_last_media_start_us;
static int64_t s_pcm_diag_us;
static int s_media_state = LINK_MEDIA_IDLE;
static int64_t s_media_retry_us;
static unsigned s_media_retry_count;
static uint8_t s_volume_127 = 127;

/* U4 发送的事件包 ≤ 128 字节；4 KiB 足够吸收一次扫描结果突发。 */
static uint8_t s_rx_stream[4096];
static size_t s_rx_len;

/* SDMMC DMA 要求缓冲区在 DRAM 且满足缓存行/字对齐。使用
 * DRAM_DMA_ALIGNED_ATTR 与官方 SDIO host 示例一致，避免 P4 上缓存行
 * 未对齐时频繁走 bounce buffer。 */
static DRAM_DMA_ALIGNED_ATTR uint8_t s_tx_buf[BT_LINK_TX_MAX];
static DRAM_DMA_ALIGNED_ATTR uint8_t s_rx_chunk[256];

/* ============================ 小工具 ============================ */

static void send_cmd(uint8_t opcode, const void *arg, size_t arg_len)
{
    if (!s_essl || arg_len > 32) return;

    uint8_t payload[1 + 32];
    payload[0] = opcode;
    if (arg_len) memcpy(payload + 1, arg, arg_len);

    link_header_t *hdr = (link_header_t *)s_tx_buf;
    size_t len = 1 + arg_len;
    hdr->magic = LINK_MAGIC;
    hdr->type = LINK_MSG_CMD;
    hdr->len = (uint16_t)len;
    hdr->seq = s_tx_seq++;
    hdr->flags = 0;
    memcpy(s_tx_buf + LINK_HEADER_BYTES, payload, len);
    hdr->crc = link_crc_packet(s_tx_buf, s_tx_buf + LINK_HEADER_BYTES, len);

    size_t total = LINK_HEADER_BYTES + len;
    size_t padded = (total + 3U) & ~(size_t)3U;
    if (padded > total) memset(s_tx_buf + total, 0, padded - total);
    if (padded > sizeof(s_tx_buf)) return;

    esp_err_t err = essl_send_packet(s_essl, s_tx_buf, padded, 100);
    if (err == ESP_OK) {
        s_send_failures = 0;
    } else {
        ++s_send_failures;
        ESP_LOGW(TAG, "send cmd %u failed: %s", (unsigned)opcode,
                 esp_err_to_name(err));
        /* 连接命令失败时不能让 UI 永远停在 CONNECTING。 */
        if (opcode == LINK_CMD_CONNECT)
            bt_audio_link_error(0xFF);
    }
}

static void send_hello(void)
{
    send_cmd(LINK_CMD_HELLO, NULL, 0);
    s_last_hello_us = esp_timer_get_time();
    s_hello_confirmed = false;
}

/* ============================ 事件解析 ============================ */

static void handle_event(const uint8_t *payload, size_t len)
{
    if (len < 1) return;

    switch (payload[0]) {
    case LINK_EVT_READY: {
        uint8_t version = len >= 2 ? payload[1] : 0;
        if (version != LINK_PROTO_VERSION) {
            ESP_LOGW(TAG, "protocol version %u != %u", (unsigned)version,
                     (unsigned)LINK_PROTO_VERSION);
            break;
        }
        if (!s_hello_confirmed) {
            s_hello_confirmed = true;
            ESP_LOGI(TAG, "U4 link ready (protocol v%u)", (unsigned)version);
        }
        atomic_store(&s_ready, true);
        /* 由 bt_audio 在 ready 回调里决定“自动连接已配对设备”或“补发
         * 扫描”：若这里直接开始搜索，会与紧接着的 CONNECT 命令冲突。 */
        bt_audio_link_set_ready(true);
        break;
    }
    case LINK_EVT_STATE:
        if (len >= 3) {
            s_bt_state = payload[1];
            if (s_bt_state == LINK_BT_DISCOVERING) {
                s_scan_active = true;
            } else {
                s_scan_active = false;
                s_stop_seen = true; /* 只有离开 DISCOVERING 才算停止完成 */
                if (s_bt_state == LINK_BT_CONNECTED)
                    s_connect_pending = false; /* 已连接就不再补发 CONNECT */
            }
            /* 先更新本地时序标志，再让 bt_audio 决定 UI 状态。 */
            bt_audio_link_bt_state(payload[1]);
            ESP_LOGD(TAG, "U4 state bt=%u media=%u",
                     (unsigned)payload[1], (unsigned)payload[2]);
        }
        break;
    case LINK_EVT_SCAN_RESULT: {
        if (len < 1 + 6 + 1 + 4 + 1) break;
        const uint8_t *p = payload + 1;
        int8_t rssi = (int8_t)p[6];
        uint8_t name_len = p[11];
        if (1 + 6 + 1 + 4 + 1 + name_len > len)
            name_len = (uint8_t)(len - (1 + 6 + 1 + 4 + 1));
        char name[BT_AUDIO_NAME_MAX];
        size_t copy = name_len < sizeof(name) - 1 ? name_len : sizeof(name) - 1;
        memcpy(name, p + 12, copy);
        name[copy] = '\0';
        bt_audio_link_scan_add(p, rssi, name);
        break;
    }
    case LINK_EVT_CONNECTED: {
        /* 新连接从干净的 PCM 缓冲开始，避免重连时发送上一次会话残留。 */
        s_media_streaming = false;
        bt_stream_reset();
        if (len >= 7) bt_audio_link_connected(payload + 1);

        /* 连接成功就启动 A2DP 媒体流，即使当前没有音乐。U4 数据回调会
         * 用静音填充空 ring，保持 AVDTP 流活跃；否则部分耳机在 5 秒左右
         * 检测到 source 未推流会主动断开。 */
        if (atomic_load(&s_enabled)) {
            int64_t now = esp_timer_get_time();
            /* PCM 响度由 P4 侧数字增益控制；音箱端 AVRCP 固定在最大，
             * 避免同时衰减。命令失败会由 U4 打印。 */
            uint8_t sink_max = 127;
            s_volume_127 = sink_max;
            send_cmd(LINK_CMD_VOLUME, &sink_max, 1);
            ESP_LOGI(TAG, "AVRCP sink volume -> 127 (P4 PCM gain controls loudness)");
            send_cmd(LINK_CMD_MEDIA_START, NULL, 0);
            s_media_state = LINK_MEDIA_IDLE;
            s_media_streaming = true;
            s_media_retry_count = 0;
            s_media_retry_us = now + 700000LL;
            s_last_media_start_us = now;
            s_last_pcm_us = now;
            ESP_LOGI(TAG, "A2DP media stream started (idle silence until PCM)");
        }
        break;
    }
    case LINK_EVT_DISCONNECTED:
        ESP_LOGW(TAG, "U4 disconnected: media_state=%d streaming=%d queued=%u dropped=%u",
                 s_media_state, s_media_streaming ? 1 : 0,
                 (unsigned)bt_stream_queued(),
                 (unsigned)bt_stream_dropped_bytes());
        s_media_streaming = false;
        s_media_state = LINK_MEDIA_IDLE;
        s_media_retry_us = 0;
        bt_stream_reset();
        bt_audio_link_disconnected();
        break;
    case LINK_EVT_MEDIA:
        if (len >= 2) {
            s_media_state = payload[1];
            if (payload[1] == LINK_MEDIA_STARTED) {
                s_media_streaming = true;
                s_media_retry_count = 0;
                ESP_LOGI(TAG, "U4 media STARTED");
            } else if (payload[1] == LINK_MEDIA_IDLE) {
                s_media_streaming = false;
                ESP_LOGW(TAG, "U4 media IDLE");
            }
        }
        break;
    case LINK_EVT_ERROR:
        if (len >= 2) {
            /* 1/5 表示 U4 的蓝牙任务尚未就绪/命令队列满：HELLO 会自动
             * 重试，属于启动早期正常现象，不把 UI 拉进错误态。 */
            if (payload[1] == 1 || payload[1] == 5) {
                ESP_LOGD(TAG, "U4 early error %u, HELLO will retry",
                         (unsigned)payload[1]);
            } else {
                bt_audio_link_error(payload[1]);
            }
        }
        break;
    case LINK_EVT_AUDIO_RATE:
        if (len >= 3) {
            s_audio_rate = (uint16_t)(payload[1] | (payload[2] << 8));
            bt_stream_set_target_rate(s_audio_rate);
            ESP_LOGI(TAG, "A2DP rate %u Hz", (unsigned)s_audio_rate);
        }
        break;
    default:
        ESP_LOGW(TAG, "unknown event %u", (unsigned)payload[0]);
        break;
    }
}

/**
 * @brief 把接收字节追加到流缓冲；溢出时丢弃整段并重新同步。
 *
 * 事件突发很小，正常不会触发。丢弃而不是覆盖前半段，避免把半个包
 * 当成下一包继续解析。
 */
static void rx_append(const uint8_t *data, size_t len)
{
    if (len > sizeof(s_rx_stream) - s_rx_len) {
        ESP_LOGW(TAG, "RX stream overflow, dropping %u bytes",
                 (unsigned)s_rx_len);
        s_rx_len = 0;
    }
    memcpy(s_rx_stream + s_rx_len, data, len);
    s_rx_len += len;
}

/**
 * @brief 从字节流中提取完整事件包。
 *
 * 每个 U4 包在 SDIO 上按 4 字节对齐填充，因此流中包起点始终对齐；
 * 但为了从丢字节/复位中恢复，遇到非法 magic/CRC 时逐字节前移重新
 * 寻找下一个 0xA5。
 */
static void rx_parse(void)
{
    size_t off = 0;
    while (s_rx_len - off >= LINK_HEADER_BYTES) {
        const uint8_t *p = s_rx_stream + off;
        if (p[0] != LINK_MAGIC) {
            ++off;
            continue;
        }
        uint16_t payload_len = (uint16_t)(p[2] | (p[3] << 8));
        if (payload_len > LINK_MAX_PAYLOAD) {
            ++off;
            continue;
        }
        size_t need = LINK_HEADER_BYTES + payload_len;
        size_t padded = (need + 3U) & ~(size_t)3U;
        if (s_rx_len - off < padded) break; /* 等下一个 SDIO 包补齐 */
        if (p[6] != link_crc_packet(p, p + LINK_HEADER_BYTES, payload_len)) {
            ++off;
            continue;
        }
        if (p[1] == LINK_MSG_EVENT)
            handle_event(p + LINK_HEADER_BYTES, payload_len);
        else
            ESP_LOGW(TAG, "unexpected RX type %u", (unsigned)p[1]);
        off += padded;
    }

    if (off) {
        memmove(s_rx_stream, s_rx_stream + off, s_rx_len - off);
        s_rx_len -= off;
    }
}

/** 取走 U4 当前已加载的全部事件字节。 */
static void rx_poll(void)
{
    for (;;) {
        uint32_t available = 0;
        if (essl_get_rx_data_size(s_essl, &available, 0) != ESP_OK) return;
        if (!available) break;

        size_t want = available < sizeof(s_rx_chunk) ? available : sizeof(s_rx_chunk);
        size_t got = 0;
        esp_err_t err = essl_get_packet(s_essl, s_rx_chunk, want, &got, 0);
        if ((err != ESP_OK && err != ESP_ERR_NOT_FINISHED) || got == 0) {
            if (err != ESP_ERR_NOT_FOUND)
                ESP_LOGW(TAG, "essl_get_packet: %s", esp_err_to_name(err));
            break;
        }
        rx_append(s_rx_chunk, got);
        if (got < want) break;
    }
    rx_parse();
}

/* ============================ 链路初始化 ============================ */

static void link_close(void)
{
    atomic_store(&s_ready, false);
    bt_audio_link_set_ready(false);
    s_hello_confirmed = false;
    s_scan_active = false;
    s_bt_state = LINK_BT_IDLE;
    s_connect_pending = false;
    s_media_streaming = false;
    s_media_state = LINK_MEDIA_IDLE;
    bt_stream_reset();
    s_rx_len = 0;
    s_send_failures = 0;
    s_reg_failures = 0;

    if (s_essl) {
        essl_sdio_deinit_dev(s_essl);
        s_essl = NULL;
    }
    if (s_slot_inited) {
        sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_1);
        s_slot_inited = false;
    }
    if (s_card) {
        heap_caps_free(s_card);
        s_card = NULL;
    }
}

/**
 * @brief 初始化 SDMMC slot1 与 ESSL；失败返回错误，由 worker 重试。
 *
 * 20 MHz 与 SD 卡使用的频率一致，避免共享 SDMMC 时钟被重新配置。
 * 4-bit、无卡检测，使用板上的外部上拉；GPIO 矩阵仅配置实际使用的
 * 6 根线，d4..d7 设为 NC，避免与 slot0 的 SD 卡引脚冲突。
 */
static esp_err_t link_open(void)
{
    esp_err_t err;

    if (!s_card) {
        s_host = (sdmmc_host_t)SDMMC_HOST_DEFAULT();
        s_host.slot = SDMMC_HOST_SLOT_1;
        s_host.flags = SDMMC_HOST_FLAG_4BIT | SDMMC_HOST_FLAG_ALLOC_ALIGNED_BUF;
        /* 10 MHz 对 PCM（约 1.5 Mbps）绰绰有余，同时降低 U4 SDIO
         * 引脚翻转电流，给 ALDO3 留出蓝牙峰值余量。 */
        s_host.max_freq_khz = 10000;

        sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
        slot.clk = BT_LINK_PIN_CLK;
        slot.cmd = BT_LINK_PIN_CMD;
        slot.d0 = BT_LINK_PIN_D0;
        slot.d1 = BT_LINK_PIN_D1;
        slot.d2 = BT_LINK_PIN_D2;
        slot.d3 = BT_LINK_PIN_D3;
        slot.d4 = GPIO_NUM_NC;
        slot.d5 = GPIO_NUM_NC;
        slot.d6 = GPIO_NUM_NC;
        slot.d7 = GPIO_NUM_NC;
        slot.cd = SDMMC_SLOT_NO_CD;
        slot.wp = SDMMC_SLOT_NO_WP;
        slot.width = 4;
        /* 板上应已有 10–90 kΩ SDIO 上拉；同时打开内部弱上拉作为调试
         * 兜底，不会改变 4-bit 选择或 U4 的 strapping（eFuse 已固定
         * VDD_SDIO=3.3 V）。 */
        slot.flags = SDMMC_SLOT_FLAG_INTERNAL_PULLUP;

        err = sdmmc_host_init();
        if (err != ESP_OK) return err;
        err = sdmmc_host_init_slot(SDMMC_HOST_SLOT_1, &slot);
        if (err != ESP_OK) return err;
        s_slot_inited = true;

        s_card = heap_caps_calloc(1, sizeof(*s_card),
                                  MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        if (!s_card) {
            sdmmc_host_deinit_slot(SDMMC_HOST_SLOT_1);
            s_slot_inited = false;
            return ESP_ERR_NO_MEM;
        }
        ESP_LOGI(TAG, "SDIO host on slot1: CLK=18 CMD=19 D0..D3=14..17, 20 MHz 4-bit");
    }

    err = sdmmc_card_init(&s_host, s_card);
    if (err != ESP_OK) return err;

    essl_sdio_config_t config = {
        .card = s_card,
        .recv_buffer_size = BT_LINK_RECV_BUFFER,
    };
    essl_handle_t handle = NULL;
    err = essl_sdio_init_dev(&handle, &config);
    if (err != ESP_OK) return err;

    err = essl_init(handle, 1000);
    if (err != ESP_OK) {
        essl_sdio_deinit_dev(handle);
        return err;
    }

    s_essl = handle;
    s_rx_len = 0;
    s_tx_seq = 1;
    s_scan_active = false;
    s_bt_state = LINK_BT_IDLE;
    s_connect_pending = false;
    s_send_failures = 0;
    s_reg_failures = 0;
    s_audio_rate = 0;
    s_media_streaming = false;
    s_last_pcm_us = 0;
    bt_stream_reset();
    s_last_hello_us = 0;
    s_last_reg_us = 0;

    /* 先读协议版本；U4 的 link_init 在 BT 初始化之前完成，正常情况下
     * 此时应为 1。版本不对不立即关闭链路，交给后续 HELLO/周期检查。 */
    uint8_t version = 0;
    esp_err_t verr = essl_read_reg(handle, LINK_REG_VERSION, &version, 100);
    if (verr == ESP_OK && version == LINK_PROTO_VERSION)
        ESP_LOGI(TAG, "U4 SDIO slave detected, protocol v%u", (unsigned)version);
    else
        ESP_LOGW(TAG, "U4 version read failed/unexpected (%s, v%u)",
                 esp_err_to_name(verr), (unsigned)version);

    send_hello();
    return ESP_OK;
}

/* ============================ 周期寄存器检查 ============================ */

/**
 * @brief 定期读版本/标志，发现 U4 复位或链路失效时退化为未就绪。
 *
 * 单次 CMD52 失败不关闭链路（可能只是瞬时 CRC），连续多次失败或版本
 * 变回 0 才认为 U4 重启，并重新握手。读取 flags 主要用于日志确认
 * HELLO 已让 U4 关闭测试音。
 */
static void poll_registers(int64_t now)
{
    if (now - s_last_reg_us < (int64_t)BT_LINK_REG_POLL_MS * 1000) return;
    s_last_reg_us = now;

    uint8_t version = 0;
    esp_err_t err = essl_read_reg(s_essl, LINK_REG_VERSION, &version, 100);
    if (err != ESP_OK || version != LINK_PROTO_VERSION) {
        ++s_reg_failures;
        if (s_hello_confirmed || atomic_load(&s_ready)) {
            ESP_LOGW(TAG, "U4 link lost (version=%u, %s)",
                     (unsigned)version, esp_err_to_name(err));
            atomic_store(&s_ready, false);
            bt_audio_link_set_ready(false);
            s_hello_confirmed = false;
        }
        /* 把重试基准设为当前时刻：主循环 1 秒后重发 HELLO，避免 0 值
         * 让重发条件永远不成立。 */
        s_last_hello_us = now;
        return;
    }
    s_reg_failures = 0;

    if (s_hello_confirmed) {
        uint8_t flags = 0;
        if (essl_read_reg(s_essl, LINK_REG_FLAGS, &flags, 100) == ESP_OK) {
            if (!(flags & LINK_FLAG_HOST_PRESENT)) {
                /* host present 未置位说明 U4 没收到合法 HELLO，重试。 */
                s_hello_confirmed = false;
                s_last_hello_us = now;
            }
        } else {
            ++s_reg_failures;
        }
    }
}

/* ============================ 命令处理 ============================ */

static void handle_msg(const bt_link_msg_t *msg)
{
    bool enabled = atomic_load(&s_enabled);

    switch (msg->cmd) {
    case BT_LINK_MSG_SCAN_START:
        if (!enabled) break;
        s_scan_active = true;
        if (s_hello_confirmed)
            send_cmd(LINK_CMD_SCAN_START, NULL, 0);
        break;
    case BT_LINK_MSG_SCAN_STOP:
        s_scan_active = false;
        if (s_hello_confirmed)
            send_cmd(LINK_CMD_SCAN_STOP, NULL, 0);
        break;
    case BT_LINK_MSG_CONNECT:
        if (!enabled) break;
        if (s_scan_active || s_bt_state == LINK_BT_DISCOVERING) {
            /* 不要在询问停止前发起 A2DP 连接；先停扫描并等待 U4 的
             * 非 DISCOVERING 状态事件，最多等 2 秒。 */
            if (s_hello_confirmed)
                send_cmd(LINK_CMD_SCAN_STOP, NULL, 0);
            s_scan_active = false;
            s_connect_pending = true;
            memcpy(s_pending_bda, msg->bda, 6);
            s_stop_seen = false;
            int64_t now = esp_timer_get_time();
            s_connect_not_before_us = now + 100000;      /* 100 ms */
            s_connect_deadline_us = now + 2000000;       /* 2 s 兜底 */
        } else if (s_hello_confirmed) {
            send_cmd(LINK_CMD_CONNECT, msg->bda, 6);
        }
        break;
    case BT_LINK_MSG_DISCONNECT:
        if (s_hello_confirmed)
            send_cmd(LINK_CMD_DISCONNECT, NULL, 0);
        break;
    case BT_LINK_MSG_MEDIA_START:
        if (!enabled) break;
        if (s_hello_confirmed)
            send_cmd(LINK_CMD_MEDIA_START, NULL, 0);
        break;
    case BT_LINK_MSG_MEDIA_STOP:
        if (s_hello_confirmed)
            send_cmd(LINK_CMD_MEDIA_STOP, NULL, 0);
        break;
    case BT_LINK_MSG_VOLUME: {
        uint8_t volume = msg->bda[0] > 127 ? 127 : msg->bda[0];
        s_volume_127 = volume;
        /* 没有 A2DP 连接时 AVRCP 音量命令无意义，等连接/推流时补发。 */
        if (enabled && s_hello_confirmed && bt_audio_is_connected()) {
            ESP_LOGI(TAG, "AVRCP volume %u", (unsigned)volume);
            send_cmd(LINK_CMD_VOLUME, &volume, 1);
        }
        break;
    }
    default:
        break;
    }
}

/** 用户关闭蓝牙：停止搜索、断开连接和媒体流，但保持 SDIO 链路。 */
static void apply_disabled(void)
{
    s_scan_active = false;
    s_connect_pending = false;
    s_media_streaming = false;
    s_media_state = LINK_MEDIA_IDLE;
    bt_stream_reset();
    if (!s_hello_confirmed) return;
    send_cmd(LINK_CMD_MEDIA_STOP, NULL, 0);
    send_cmd(LINK_CMD_DISCONNECT, NULL, 0);
}

/**
 * @brief 按 U4 的 PCM credit 发送已重采样的 16-bit 立体声数据。
 *
 * credit 以 256 字节为单位，表示 U4 16 KiB PCM ring 的空闲空间。每轮
 * 最多读一次寄存器，然后连续发送到本次 credit 用完；下一轮再读。
 * A2DP 媒体流在连接后保持活跃（无 PCM 时由 U4 填静音），本函数在
 * U4 未确认 STARTED 时快速重发 MEDIA_START。
 */
static void send_pcm(int64_t now)
{
    if (!s_essl || !s_hello_confirmed || !bt_audio_is_connected()) {
        if (s_media_streaming) {
            send_cmd(LINK_CMD_MEDIA_STOP, NULL, 0);
            s_media_streaming = false;
        }
        return;
    }

    /* U4 尚未确认 STARTED 时快速重发 MEDIA_START（700 ms，超过 6 次
     * 后退到 2 s）。重连后 A2DP 就绪时序可能与首次配对不同，单次发送
     * 容易落在 CHECK_SRC_RDY 失败窗口，耳机就会把空闲 source 断开。 */
    if (s_media_state != LINK_MEDIA_STARTED && now >= s_media_retry_us) {
        send_cmd(LINK_CMD_MEDIA_START, NULL, 0);
        s_media_streaming = true;
        ++s_media_retry_count;
        s_media_retry_us = now + (s_media_retry_count < 6 ? 700000LL : 2000000LL);
        ESP_LOGI(TAG, "media start retry %u (U4 state %d)",
                 s_media_retry_count, s_media_state);
    }

    if (bt_stream_queued() >= 4U) {
        uint8_t credit = 0;
        if (essl_read_reg(s_essl, LINK_REG_PCM_CREDIT, &credit, 50) == ESP_OK) {
            size_t allow = (size_t)credit * 256U;
            while (allow >= 16U) {
                size_t want = allow - LINK_HEADER_BYTES;
                if (want > 1016U) want = 1016U;
                want &= ~(size_t)3U;
                if (!want) break;

                size_t got = bt_stream_read(s_tx_buf + LINK_HEADER_BYTES, want);
                if (!got) break;

                link_header_t *hdr = (link_header_t *)s_tx_buf;
                hdr->magic = LINK_MAGIC;
                hdr->type = LINK_MSG_PCM;
                hdr->len = (uint16_t)got;
                hdr->seq = s_tx_seq++;
                hdr->flags = 0;
                hdr->crc = link_crc_packet(s_tx_buf,
                                           s_tx_buf + LINK_HEADER_BYTES, got);
                size_t total = LINK_HEADER_BYTES + got;
                size_t padded = (total + 3U) & ~(size_t)3U;
                if (padded > total) memset(s_tx_buf + total, 0, padded - total);

                if (essl_send_packet(s_essl, s_tx_buf, padded, 50) != ESP_OK) {
                    ++s_send_failures;
                    break;
                }
                s_send_failures = 0;
                allow -= got;
                s_last_pcm_us = now;

            }
        }
    }

    /* 5 秒诊断：帮助区分“没有 PCM 生产者”和“U4 不消费导致丢帧”。 */
    if (now - s_pcm_diag_us > 5000000LL) {
        s_pcm_diag_us = now;
        if (s_media_streaming || bt_stream_queued())
            ESP_LOGI(TAG, "PCM queued=%u dropped=%u streaming=%d",
                     (unsigned)bt_stream_queued(),
                     (unsigned)bt_stream_dropped_bytes(),
                     s_media_streaming ? 1 : 0);
    }
}

static void link_task(void *arg)
{
    (void)arg;
    int64_t last_open_attempt = 0;

    /* U4 刚释放 CHIP_PU 时会经历 ROM/应用启动和蓝牙 RF 初始化，峰值
     * 电流最高。先等它完成启动再探测 SDIO，避免主机总线活动叠加到
     * 本来已紧张的 ALDO3 上。 */
    vTaskDelay(pdMS_TO_TICKS(1500));
    s_u4_reset_us = esp_timer_get_time();

    for (;;) {
        bool enabled = atomic_load(&s_enabled);
        if (enabled != s_applied_enabled) {
            s_applied_enabled = enabled;
            if (!enabled) apply_disabled();
        }

        if (!s_essl) {
            int64_t now = esp_timer_get_time();
            if (now - last_open_attempt >= (int64_t)BT_LINK_OPEN_RETRY_MS * 1000) {
                last_open_attempt = now;
                esp_err_t err = link_open();
                if (err != ESP_OK) {
                    if (now - s_last_open_log_us > 10000000LL) {
                        s_last_open_log_us = now;
                        ESP_LOGW(TAG, "Open U4 SDIO link failed: %s (retrying)",
                                 esp_err_to_name(err));
                    }
                    /* U4 持续无响应（典型为蓝牙初始化 Brownout 反复重启）
                     * 时给一次干净的 CHIP_PU 复位。1 秒低电平只复位 U4，
                     * 不关闭 ALDO3；若供电本身不足，可能再次 Brownout，
                     * 这里每 10 秒最多重试一次并持续记录。 */
                    if (now - s_u4_reset_us > 10000000LL) {
                        ESP_LOGW(TAG, "U4 unresponsive for 10 s; pulsing CHIP_PU");
                        u4_power_pulse_reset(1000);
                        s_u4_reset_us = esp_timer_get_time();
                        last_open_attempt = s_u4_reset_us;
                    }
                } else {
                    s_u4_reset_us = now;
                    ESP_LOGI(TAG, "U4 SDIO link opened");
                }
            }
            vTaskDelay(pdMS_TO_TICKS(50));
            continue;
        }

        /* 先处理主机命令，减少打开设置页/点击设备后的延迟。 */
        bt_link_msg_t msg;
        while (xQueueReceive(s_queue, &msg, 0) == pdTRUE) handle_msg(&msg);

        esp_err_t err = essl_wait_int(s_essl, BT_LINK_WAIT_MS);
        if (err == ESP_OK) {
            uint32_t raw = 0, st = 0;
            if (essl_get_intr(s_essl, &raw, &st, 0) == ESP_OK && raw)
                essl_clear_intr(s_essl, raw, 0);
        }
        /* 即使没有中断也轮询一次，防止漏中断后事件滞留。 */
        rx_poll();

        int64_t now = esp_timer_get_time();

        /* 扫描停止后的待连接：优先等 U4 的非 DISCOVERING 状态事件，
         * 事件丢失则 2 秒后仍然尝试，避免 UI 永远停在 CONNECTING。 */
        if (s_connect_pending && s_hello_confirmed &&
            now >= s_connect_not_before_us &&
            (s_stop_seen || now >= s_connect_deadline_us)) {
            send_cmd(LINK_CMD_CONNECT, s_pending_bda, 6);
            s_connect_pending = false;
            ESP_LOGI(TAG, "discovery stopped, connecting now");
        }

        send_pcm(now);
        poll_registers(now);

        if (!s_hello_confirmed && s_last_hello_us &&
            now - s_last_hello_us > (int64_t)BT_LINK_HELLO_RETRY_MS * 1000) {
            send_hello();
        }

        if (s_reg_failures >= BT_LINK_MAX_SEND_FAILS ||
            s_send_failures >= BT_LINK_MAX_SEND_FAILS) {
            ESP_LOGW(TAG, "Too many SDIO errors, reopening link");
            link_close();
            last_open_attempt = 0;
        }
    }
}

/* ============================ 公共接口 ============================ */

esp_err_t bt_link_init(void)
{
    if (s_task) return ESP_OK;

    s_queue = xQueueCreate(8, sizeof(bt_link_msg_t));
    if (!s_queue) return ESP_ERR_NO_MEM;

    esp_err_t stream_ret = bt_stream_init();
    if (stream_ret != ESP_OK)
        ESP_LOGW(TAG, "Bluetooth PCM stream unavailable: %s",
                 esp_err_to_name(stream_ret));

    BaseType_t ok = xTaskCreate(link_task, "bt_link", BT_LINK_TASK_STACK, NULL,
                                BT_LINK_TASK_PRIO, &s_task);
    if (ok != pdPASS) {
        vQueueDelete(s_queue);
        s_queue = NULL;
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}

void bt_link_set_enabled(bool enabled)
{
    atomic_store(&s_enabled, enabled);
}

bool bt_link_is_ready(void)
{
    return atomic_load(&s_ready);
}

static bool push_msg(uint8_t cmd, const uint8_t *bda)
{
    if (!s_queue) return false;
    bt_link_msg_t msg = {.cmd = cmd};
    if (bda) memcpy(msg.bda, bda, 6);
    if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
        ESP_LOGW(TAG, "command queue full, cmd %u dropped", (unsigned)cmd);
        return false;
    }
    return true;
}

void bt_link_scan_start(void)
{
    (void)push_msg(BT_LINK_MSG_SCAN_START, NULL);
}

void bt_link_scan_stop(void)
{
    (void)push_msg(BT_LINK_MSG_SCAN_STOP, NULL);
}

esp_err_t bt_link_connect(const uint8_t bda[6])
{
    if (!bda) return ESP_ERR_INVALID_ARG;
    if (!s_queue) return ESP_ERR_INVALID_STATE;
    if (!atomic_load(&s_ready)) return ESP_ERR_INVALID_STATE;
    if (!push_msg(BT_LINK_MSG_CONNECT, bda)) return ESP_ERR_TIMEOUT;
    return ESP_OK;
}

void bt_link_disconnect(void)
{
    push_msg(BT_LINK_MSG_DISCONNECT, NULL);
}

void bt_link_media_start(void)
{
    push_msg(BT_LINK_MSG_MEDIA_START, NULL);
}

void bt_link_media_stop(void)
{
    push_msg(BT_LINK_MSG_MEDIA_STOP, NULL);
}

void bt_link_set_volume(uint8_t volume)
{
    if (!s_queue) return;
    bt_link_msg_t msg = {.cmd = BT_LINK_MSG_VOLUME};
    msg.bda[0] = volume > 127 ? 127 : volume;
    (void)xQueueSend(s_queue, &msg, 0);
}
