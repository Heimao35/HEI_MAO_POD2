/* Production Bluetooth settings subpage with LVGL's software renderer and
 * event input. The bt_audio facade is a RAM double: it verifies the page
 * posts power/scan/connect requests and renders only backend-confirmed
 * devices/connection state, without touching SDIO or Bluetooth. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "../../main/ui/ui_bluetooth_settings.c"

/* Fonts are mounted by firmware startup; use the same built-in sizes here so
 * layout/click coordinates and the 18px device-name check match production. */
const lv_font_t *ui_font_16(void) { return &lv_font_montserrat_16; }
const lv_font_t *ui_font_18(void) { return &lv_font_montserrat_18; }
const lv_font_t *ui_font_22(void) { return &lv_font_montserrat_22; }

/* ---------------- bt_audio facade double ---------------- */
static bool mock_enabled;
static bool mock_ready;
static bt_audio_state_t mock_state;
static bt_audio_device_t mock_devices[BT_AUDIO_MAX_DEVICES];
static size_t mock_device_count;
static const char *mock_connected_name;
static unsigned scan_start_calls;
static unsigned scan_stop_calls;
static unsigned connect_calls;
static uint8_t connected_bda[6];
static esp_err_t connect_result;
static bool mock_paired_valid;
static uint8_t mock_paired_bda[6];
static char mock_paired_name[BT_AUDIO_NAME_MAX];

esp_err_t bt_audio_init(void) { return ESP_OK; }

void bt_audio_set_enabled(bool enabled)
{
    mock_enabled = enabled;
    mock_state = enabled ? BT_AUDIO_STATE_ON : BT_AUDIO_STATE_OFF;
    if (!enabled) mock_connected_name = "";
}

bool bt_audio_is_enabled(void) { return mock_enabled; }
bool bt_audio_link_ready(void) { return mock_ready; }
bt_audio_state_t bt_audio_get_state(void) { return mock_state; }

void bt_audio_scan_start(void)
{
    ++scan_start_calls;
    if (mock_enabled && mock_ready) mock_state = BT_AUDIO_STATE_SCANNING;
}

void bt_audio_scan_stop(void)
{
    ++scan_stop_calls;
    if (mock_state == BT_AUDIO_STATE_SCANNING) mock_state = BT_AUDIO_STATE_ON;
}

size_t bt_audio_get_devices(bt_audio_device_t *out, size_t max)
{
    size_t count = mock_device_count < max ? mock_device_count : max;
    if (out && count) memcpy(out, mock_devices, count * sizeof(mock_devices[0]));
    return count;
}

esp_err_t bt_audio_connect(const uint8_t bda[6])
{
    ++connect_calls;
    memcpy(connected_bda, bda, 6);
    if (connect_result == ESP_OK) mock_state = BT_AUDIO_STATE_CONNECTING;
    return connect_result;
}

bool bt_audio_is_connected(void) { return mock_state == BT_AUDIO_STATE_CONNECTED; }

size_t bt_audio_get_connected_name(char *out, size_t size)
{
    if (!out || !size) return 0;
    snprintf(out, size, "%s", mock_connected_name ? mock_connected_name : "");
    return strlen(out);
}

static unsigned forget_calls;
void bt_audio_forget_paired(void)
{
    ++forget_calls;
    mock_paired_valid = false;
    memset(mock_paired_bda, 0, 6);
    mock_paired_name[0] = 0;
}

bool bt_audio_get_paired(uint8_t bda[6], char *name, size_t name_size)
{
    if (!bda || !name || !name_size) return false;
    memset(bda, 0, 6);
    name[0] = '\0';
    if (!mock_paired_valid) return false;
    memcpy(bda, mock_paired_bda, 6);
    snprintf(name, name_size, "%s", mock_paired_name);
    return true;
}

/* The subpage asks the settings grid to resynchronize its Bluetooth entry
 * colors after the exit animation; the settings page is out of scope here. */
static unsigned settings_refresh_calls;
void ui_settings_refresh_bluetooth(void) { ++settings_refresh_calls; }

/* ---------------- LVGL harness ---------------- */
static uint32_t ticks;
static lv_point_t pointer_point;
static lv_indev_state_t pointer_state;

static void read_pointer(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    data->point = pointer_point;
    data->state = pointer_state;
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

static void advance(unsigned ms)
{
    for (unsigned n = 0; n < ms; n += 10) {
        ticks += 10;
        lv_tick_inc(10);
        lv_timer_handler();
    }
    lv_obj_update_layout(lv_screen_active());
}

static void long_press_obj(lv_indev_t *indev, lv_obj_t *obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    pointer_point = (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(indev);
    advance(700);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);
    advance(100);
}

static void click_obj(lv_indev_t *indev, lv_obj_t *obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    pointer_point = (lv_point_t){(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(indev);
    advance(20);
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);
    advance(50);
}

static void add_device(unsigned index, const char *name, uint8_t tag)
{
    assert(index < BT_AUDIO_MAX_DEVICES);
    bt_audio_device_t *dev = &mock_devices[index];
    memset(dev, 0, sizeof(*dev));
    for (unsigned i = 0; i < 6; ++i) dev->bda[i] = (uint8_t)(tag + i);
    dev->rssi = -40 - (int8_t)index;
    snprintf(dev->name, sizeof(dev->name), "%s", name);
    if (index + 1 > mock_device_count) mock_device_count = index + 1;
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(460, 460);
    static uint8_t pixels[460 * 20 * 2];
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);

    ui_bluetooth_settings_create(lv_screen_active());
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);
    lv_obj_update_layout(lv_screen_active());

    /* Page starts off-screen/hidden; creation alone must not read NVS,
     * start scanning, or show any fake device. */
    assert(lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_x(s_page) == 460);
    assert(!ui_bluetooth_settings_is_visible());
    assert(scan_start_calls == 0 && scan_stop_calls == 0);
    assert(!lv_obj_has_state(s_power_switch, LV_STATE_CHECKED));
    puts("PASS bluetooth page starts hidden/off without side effects");

    /* Entering the page requests a scan even when the switch is off; the
     * facade keeps it off and the empty hint tells the user to enable it. */
    ui_bluetooth_settings_show();
    advance(400);
    assert(ui_bluetooth_settings_is_visible());
    assert(!lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_x(s_page) == 0);
    /* Requested layout: smaller switch on top, paired device directly below,
     * then status text, then the scanned-device list. */
    lv_obj_t *switch_row = lv_obj_get_parent(s_power_switch);
    assert(lv_obj_get_width(s_power_switch) == 110);
    assert(lv_obj_get_height(s_power_switch) == 56);
    assert(lv_obj_get_y(switch_row) == 60 && lv_obj_get_height(switch_row) == 76);
    assert(lv_obj_get_y(s_paired_row) == 144 && lv_obj_get_height(s_paired_row) == 64);
    assert(lv_obj_get_y(s_status) == 212);
    assert(lv_obj_get_y(s_list) == 238);
    assert(lv_obj_get_y(switch_row) + lv_obj_get_height(switch_row) <=
           lv_obj_get_y(s_paired_row));
    assert(lv_obj_get_y(s_paired_row) + lv_obj_get_height(s_paired_row) <=
           lv_obj_get_y(s_status));
    assert(scan_start_calls == 1);
    assert(!mock_enabled && !lv_obj_has_state(s_power_switch, LV_STATE_CHECKED));
    assert(!lv_obj_has_flag(s_empty, LV_OBJ_FLAG_HIDDEN));
    assert(strstr(lv_label_get_text(s_empty), "蓝牙已关闭") != NULL);
    /* The paired card is present even with no record, but tapping the
     * placeholder must not enable Bluetooth or send a connect request. */
    assert(strcmp(lv_label_get_text(s_paired_label), "暂无已配对设备") == 0);
    assert(lv_obj_get_style_text_font(s_paired_label, 0) == &lv_font_montserrat_18);
    click_obj(pointer, s_paired_row);
    assert(!mock_enabled && connect_calls == 0);
    puts("PASS entering the page starts a scan request and explains the off state; empty paired card is inert");

    /* The page switch posts the power intent and starts scanning; the facade
     * is still not ready, so the status must say so instead of "未连接". */
    click_obj(pointer, s_power_switch);
    advance(300);
    assert(mock_enabled);
    assert(lv_obj_has_state(s_power_switch, LV_STATE_CHECKED));
    assert(scan_start_calls == 2);
    assert(strstr(lv_label_get_text(s_status), "未就绪") != NULL);
    assert(strstr(lv_label_get_text(s_empty), "未就绪") != NULL);
    puts("PASS switch posts power intent and reports an unready link honestly");

    /* Backend becomes ready and publishes two devices. The page polls the
     * snapshot and renders names with the 18px full CJK font. */
    mock_ready = true;
    mock_state = BT_AUDIO_STATE_SCANNING;
    add_device(0, "客厅音箱", 0x11);
    add_device(1, "", 0x22); /* empty name -> explicit unknown-device text */
    advance(300);
    assert(!lv_obj_has_flag(s_rows[0].row, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(s_rows[0].label), "客厅音箱") == 0);
    assert(lv_obj_get_style_text_font(s_rows[0].label, 0) == &lv_font_montserrat_18);
    assert(!lv_obj_has_flag(s_rows[1].row, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(s_rows[1].label), "(未知设备)") == 0);
    assert(lv_obj_has_flag(s_empty, LV_OBJ_FLAG_HIDDEN));
    puts("PASS scan results render in the scroll list with 18px names");

    /* Tapping a row sends that row's BDA to the facade; the page itself does
     * not mark anything connected before the backend confirms. */
    connect_result = ESP_OK;
    click_obj(pointer, s_rows[0].row);
    advance(50);
    assert(connect_calls == 1);
    uint8_t expected[6] = {0x11, 0x12, 0x13, 0x14, 0x15, 0x16};
    assert(memcmp(connected_bda, expected, 6) == 0);
    assert(mock_state == BT_AUDIO_STATE_CONNECTING);
    puts("PASS row tap sends the selected device address");

    /* Only a backend-confirmed connection turns the status white and displays
     * the name; the list itself stays a list of scan results. */
    mock_state = BT_AUDIO_STATE_CONNECTED;
    mock_connected_name = "客厅音箱";
    advance(300);
    assert(strstr(lv_label_get_text(s_status), "已连接") != NULL);
    assert(strstr(lv_label_get_text(s_status), "客厅音箱") != NULL);
    assert(lv_color_eq(lv_obj_get_style_text_color(s_status, 0), lv_color_white()));
    puts("PASS connected state is shown only after backend confirmation");

    /* A rejected connect request becomes a temporary notice rather than a
     * silent failure; after 2s the normal state text returns. */
    mock_state = BT_AUDIO_STATE_ON;
    connect_result = ESP_ERR_INVALID_STATE;
    click_obj(pointer, s_rows[1].row);
    assert(connect_calls == 2);
    assert(strstr(lv_label_get_text(s_status), "无法连接") != NULL);
    advance(2200);
    assert(strstr(lv_label_get_text(s_status), "未连接") != NULL);
    puts("PASS rejected connect requests surface a bounded notice");

    /* The top paired card displays the single saved device and tapping it
     * reconnects that address; once connected, another tap is a no-op. */
    mock_state = BT_AUDIO_STATE_ON;
    connect_result = ESP_OK;
    mock_paired_valid = true;
    memcpy(mock_paired_bda, expected, 6);
    snprintf(mock_paired_name, sizeof(mock_paired_name), "客厅音箱");
    advance(300);
    assert(strcmp(lv_label_get_text(s_paired_label), "客厅音箱") == 0);
    assert(lv_color_eq(lv_obj_get_style_text_color(s_paired_label, 0), lv_color_white()));
    unsigned before_paired_connect = connect_calls;
    click_obj(pointer, s_paired_row);
    advance(50);
    assert(connect_calls == before_paired_connect + 1);
    assert(memcmp(connected_bda, expected, 6) == 0);
    assert(mock_state == BT_AUDIO_STATE_CONNECTING);
    mock_state = BT_AUDIO_STATE_CONNECTED;
    mock_connected_name = "客厅音箱";
    advance(300);
    click_obj(pointer, s_paired_row);
    assert(connect_calls == before_paired_connect + 1);
    puts("PASS paired card reconnects its saved device and ignores taps while connected");

    /* Long press the paired card: a modal confirmation appears; cancel keeps
     * the pairing and confirm clears it through the facade. */
    long_press_obj(pointer, s_paired_row);
    assert(!lv_obj_has_flag(s_unpair_overlay, LV_OBJ_FLAG_HIDDEN));
    assert(forget_calls == 0 && mock_paired_valid);
    click_obj(pointer, s_unpair_cancel);
    assert(lv_obj_has_flag(s_unpair_overlay, LV_OBJ_FLAG_HIDDEN));
    assert(forget_calls == 0 && mock_paired_valid);
    long_press_obj(pointer, s_paired_row);
    assert(!lv_obj_has_flag(s_unpair_overlay, LV_OBJ_FLAG_HIDDEN));
    click_obj(pointer, s_unpair_confirm);
    assert(lv_obj_has_flag(s_unpair_overlay, LV_OBJ_FLAG_HIDDEN));
    assert(forget_calls == 1 && !mock_paired_valid);
    advance(300);
    assert(strcmp(lv_label_get_text(s_paired_label), "暂无已配对设备") == 0);
    puts("PASS long press opens unpair dialog; cancel keeps pairing, confirm clears it");

    /* Back stops scanning and hides the page with the shared 350ms slide;
     * the connection/power intent is not changed by leaving the page. The
     * exit completion also asks the settings grid to refresh its entry. */
    unsigned stops = scan_stop_calls;
    unsigned refreshes = settings_refresh_calls;
    click_obj(pointer, s_back);
    advance(400);
    assert(!ui_bluetooth_settings_is_visible());
    assert(lv_obj_has_flag(s_page, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_x(s_page) == 460);
    assert(scan_stop_calls == stops + 1);
    assert(mock_enabled);
    assert(settings_refresh_calls == refreshes + 1);
    puts("PASS back arrow stops scanning, slides out and refreshes the grid entry");

    /* Re-entering resumes polling and requests a fresh scan. */
    unsigned starts = scan_start_calls;
    ui_bluetooth_settings_show();
    advance(400);
    assert(ui_bluetooth_settings_is_visible());
    assert(scan_start_calls == starts + 1);
    puts("PASS re-entering restarts the scan and page timer");
    return 0;
}
