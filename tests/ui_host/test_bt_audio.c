/* Production bt_audio facade with stub transport and a single-threaded mutex
 * double. Verifies user intent, link-ready auto-connect, scan-result caching,
 * connected-name lookup, pairing memory and error fallback without SDIO. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../main/bt_audio.c"

/* -------------------- FreeRTOS mutex doubles -------------------- */
static int lock_depth;
SemaphoreHandle_t xSemaphoreCreateMutex(void) { return (void *)1; }
BaseType_t xSemaphoreTake(SemaphoreHandle_t semaphore, TickType_t ticks)
{
    (void)semaphore; (void)ticks;
    assert(lock_depth == 0);
    ++lock_depth;
    return pdTRUE;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t semaphore)
{
    (void)semaphore;
    assert(lock_depth == 1);
    --lock_depth;
    return pdTRUE;
}

/* -------------------- bt_link transport doubles -------------------- */
static bool link_enabled;
static int scan_start_calls, scan_stop_calls, connect_calls;
static uint8_t last_connect_bda[6];
static esp_err_t connect_result;

void bt_link_set_enabled(bool enabled) { link_enabled = enabled; }
bool bt_link_is_ready(void) { return true; }

void bt_link_scan_start(void) { ++scan_start_calls; }
void bt_link_scan_stop(void) { ++scan_stop_calls; }

esp_err_t bt_link_connect(const uint8_t bda[6])
{
    ++connect_calls;
    memcpy(last_connect_bda, bda, 6);
    return connect_result;
}

void bt_link_disconnect(void) {}
void bt_link_media_start(void) {}
void bt_link_media_stop(void) {}
static uint8_t last_volume;
void bt_link_set_volume(uint8_t volume) { last_volume = volume; }
static unsigned forget_calls;
static uint8_t forgotten_bda[6];
void bt_link_forget(const uint8_t bda[6])
{
    ++forget_calls;
    memcpy(forgotten_bda, bda, 6);
}

static const uint8_t bda_a[6] = {0xA1, 0xA2, 0xA3, 0xA4, 0xA5, 0xA6};
static const uint8_t bda_b[6] = {0xB1, 0xB2, 0xB3, 0xB4, 0xB5, 0xB6};

int main(void)
{
    assert(bt_audio_init() == ESP_OK);
    assert(!bt_audio_is_enabled());
    assert(bt_audio_get_state() == BT_AUDIO_STATE_OFF);
    assert(!bt_audio_link_ready());
    assert(!bt_audio_is_connected());
    bt_audio_device_t devs[BT_AUDIO_MAX_DEVICES];
    assert(bt_audio_get_devices(devs, BT_AUDIO_MAX_DEVICES) == 0);
    uint8_t pair[6];
    char pair_name[BT_AUDIO_NAME_MAX];
    assert(!bt_audio_get_paired(pair, pair_name, sizeof(pair_name)));
    puts("PASS default off: no link, devices or pairing");

    /* 未打开时扫描/连接请求不得到达传输层。 */
    bt_audio_scan_start();
    assert(scan_start_calls == 0 && !bt_audio_is_enabled());
    assert(bt_audio_connect(bda_a) == ESP_ERR_INVALID_STATE);
    puts("PASS disabled facade rejects scan/connect without touching the link");

    /* 打开后记录意图并通知链路；链路未 ready 时不自动连接。 */
    bt_audio_set_paired(bda_a, "客厅音箱");
    bt_audio_set_enabled(true);
    assert(bt_audio_is_enabled() && link_enabled);
    assert(bt_audio_get_state() == BT_AUDIO_STATE_ON);
    assert(connect_calls == 0);
    puts("PASS enabling records intent and waits for link-ready");

    /* READY 后自动连接最近配对设备。 */
    bt_audio_link_set_ready(true);
    assert(bt_audio_link_ready());
    assert(connect_calls == 1);
    assert(memcmp(last_connect_bda, bda_a, 6) == 0);
    assert(bt_audio_get_state() == BT_AUDIO_STATE_CONNECTING);
    puts("PASS link-ready auto-connects the stored paired device");

    /* U4 确认 A2DP 连接后状态栏/UI 才能看到已连接与名称。 */
    bt_audio_link_connected(bda_a);
    assert(bt_audio_is_connected());
    assert(bt_audio_get_state() == BT_AUDIO_STATE_CONNECTED);
    char name[BT_AUDIO_NAME_MAX];
    assert(bt_audio_get_connected_name(name, sizeof(name)) > 0);
    assert(strcmp(name, "客厅音箱") == 0);
    assert(bt_audio_get_paired(pair, pair_name, sizeof(pair_name)));
    assert(memcmp(pair, bda_a, 6) == 0 && strcmp(pair_name, "客厅音箱") == 0);
    puts("PASS confirmed connection exposes name and persists pairing in RAM");

    /* 已连接时不允许再发起搜索或第二个连接。 */
    int starts = scan_start_calls;
    bt_audio_scan_start();
    assert(scan_start_calls == starts);
    assert(bt_audio_connect(bda_b) == ESP_ERR_INVALID_STATE);
    puts("PASS connected state blocks scan and a second A2DP sink");

    /* 断开后回到已开启未连接；配对记录保留。 */
    bt_audio_link_disconnected();
    assert(!bt_audio_is_connected());
    assert(bt_audio_get_state() == BT_AUDIO_STATE_ON);
    assert(bt_audio_get_paired(pair, pair_name, sizeof(pair_name)));
    puts("PASS disconnect keeps pairing and returns to on/unconnected");

    /* 扫描结果按 BDA 去重并更新 RSSI/名称。 */
    bt_audio_scan_start();
    assert(scan_start_calls == starts + 1);
    assert(bt_audio_get_state() == BT_AUDIO_STATE_SCANNING);
    bt_audio_link_scan_add(bda_b, -40, "第一台");
    bt_audio_link_scan_add(bda_b, -55, "第一台-新名称");
    bt_audio_link_scan_add(bda_a, -70, "客厅音箱");
    assert(bt_audio_get_devices(devs, BT_AUDIO_MAX_DEVICES) == 2);
    assert(memcmp(devs[0].bda, bda_b, 6) == 0 && devs[0].rssi == -55);
    assert(strcmp(devs[0].name, "第一台-新名称") == 0);
    assert(memcmp(devs[1].bda, bda_a, 6) == 0);
    puts("PASS scan cache deduplicates BDA and updates name/RSSI");

    /* 手动连接失败保持错误态，便于用户重试。 */
    connect_result = ESP_OK;
    assert(bt_audio_connect(bda_b) == ESP_OK);
    assert(bt_audio_get_state() == BT_AUDIO_STATE_CONNECTING);
    /* P4 等待 U4 停止扫描时，U4 可能先报 UNCONNECTED；该中间态不能把
     * UI 从 CONNECTING 拉回 ON。 */
    bt_audio_link_bt_state(LINK_BT_UNCONNECTED);
    assert(bt_audio_get_state() == BT_AUDIO_STATE_CONNECTING);
    bt_audio_link_error(2);
    assert(bt_audio_get_state() == BT_AUDIO_STATE_ERROR);
    puts("PASS manual connect timeout is surfaced as an error");

    /* 自动重连失败只回到“已开启/未连接”，不锁定错误态。 */
    bt_audio_set_enabled(false);
    assert(!bt_audio_is_enabled() && !link_enabled);
    assert(bt_audio_get_state() == BT_AUDIO_STATE_OFF);
    assert(bt_audio_get_devices(devs, BT_AUDIO_MAX_DEVICES) == 0);
    bt_audio_set_enabled(true);
    bt_audio_link_set_ready(false);
    bt_audio_link_set_ready(true);
    assert(connect_calls >= 3); /* re-enable auto-connect attempt */
    bt_audio_link_error(2);
    assert(bt_audio_get_state() == BT_AUDIO_STATE_ON);
    puts("PASS auto-connect failure falls back to on/unconnected");

    /* 链路丢失会取消连接状态但保留配对。 */
    bt_audio_link_connected(bda_a);
    assert(bt_audio_is_connected());
    bt_audio_link_set_ready(false);
    assert(!bt_audio_link_ready());
    assert(bt_audio_get_state() == BT_AUDIO_STATE_ON);
    assert(!bt_audio_is_connected());
    assert(bt_audio_get_paired(pair, pair_name, sizeof(pair_name)));
    assert(memcmp(pair, bda_a, 6) == 0);
    puts("PASS link loss clears connection but keeps the paired device");

    /* 取消配对：清除 RAM 记录并把 BDA 交给 U4 删除绑定。 */
    unsigned before_forget = forget_calls;
    bt_audio_forget_paired();
    assert(forget_calls == before_forget + 1);
    assert(memcmp(forgotten_bda, bda_a, 6) == 0);
    assert(!bt_audio_get_paired(pair, pair_name, sizeof(pair_name)));
    /* 已无配对记录时再次调用不应重复发送命令。 */
    bt_audio_forget_paired();
    assert(forget_calls == before_forget + 1);
    puts("PASS unpair clears the single pairing record and asks U4 to remove the bond");
    return 0;
}
