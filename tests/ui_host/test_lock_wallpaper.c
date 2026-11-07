/* “锁屏和壁纸”子页 + 壁纸选择页的真实 UI 回归。
 *
 * 两个生产 .c 作为独立编译单元链接（它们各有同名的 static 状态），本文件
 * 只提供字体/屏幕电源/图片后台/壁纸存储的替身，然后通过公开 API 和 LVGL
 * 对象树验证：
 *  - 三张卡片的顺序/尺寸/文案，防误触开关的恢复与点击；
 *  - “壁纸设置”打开选择页并临时接管 image_viewer 回调；
 *  - 四列网格的槽位/位置/缩略图 token；
 *  - 全屏预览无控制台、确认/取消按钮状态和保存结果轮询；
 *  - 右滑返回、锁屏关闭时恢复图片查看器回调。
 * 不访问真实 SD/JPEG/Flash/LVGL 显示设备。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>

#include "image_viewer.h"
#include "ui_common.h"
#include "ui_lock_wallpaper_settings.h"
#include "ui_wallpaper_picker.h"
#include "wallpaper.h"

/* ---------------- 字体/屏幕电源 ---------------- */
const lv_font_t *ui_font_14(void) { return &lv_font_montserrat_14; }
const lv_font_t *ui_font_16(void) { return &lv_font_montserrat_16; }
const lv_font_t *ui_font_18(void) { return &lv_font_montserrat_18; }
const lv_font_t *ui_font_22(void) { return &lv_font_montserrat_22; }

static bool touch_wake_allowed = true;
void screen_power_set_touch_wake(bool enabled) { touch_wake_allowed = enabled; }

/* ---------------- 状态栏 ---------------- */
static lv_obj_t *status_bar;
static bool statusbar_visible = true;
void ui_statusbar_set_visible(bool visible) { statusbar_visible = visible; }
lv_obj_t *ui_statusbar_get_obj(void) { return status_bar; }

/* ---------------- image_viewer 后台替身 ---------------- */
static image_viewer_ops_t ops;
static unsigned scans, thumb_requests, full_requests, clears, cancels;
static unsigned backend_detach_count, backend_attach_count;
static uint32_t thumb_token[IMAGE_MAX_THUMB_SLOTS];
static uint32_t last_full_index, last_full_token;

void image_viewer_set_ops(const image_viewer_ops_t *value, void *ctx)
{
    (void)ctx;
    ops = value ? *value : (image_viewer_ops_t){0};
}
void image_viewer_request_scan(void) { ++scans; }
/* 本地补丁：壁纸选择页复用图片后台，但图片worker可能在离开图片模式时
 * 被销毁；打开选择页前会调用 ensure_started()，这里桩成功即可。 */
esp_err_t image_viewer_ensure_started(void) { return ESP_OK; }
void image_viewer_request_thumbnail(image_folder_t folder, uint32_t index,
                                    uint32_t slot, uint32_t token)
{
    assert(folder == IMAGE_FOLDER_PICTURES && slot < IMAGE_MAX_THUMB_SLOTS);
    (void)index;
    ++thumb_requests;
    thumb_token[slot] = token;
}
void image_viewer_request_full(image_folder_t folder, uint32_t index,
                               uint32_t token)
{
    assert(folder == IMAGE_FOLDER_PICTURES);
    ++full_requests;
    last_full_index = index;
    last_full_token = token;
}
void image_viewer_cancel_full(void) { ++cancels; }
void image_viewer_clear_thumbs(void) { ++clears; }
void image_viewer_set_full_active(bool active) { (void)active; }
void ui_image_viewer_backend_detach(void) { ++backend_detach_count; }
void ui_image_viewer_backend_attach(void) { ++backend_attach_count; }

/* ---------------- SD/壁纸存储替身 ---------------- */
static uint32_t sd_generation = 9;
uint32_t sd_card_get_generation(void) { return sd_generation; }
bool sd_card_is_present(void) { return true; }

static wallpaper_state_t wall_state = WALLPAPER_STATE_IDLE;
static unsigned commits, acks;
static const uint8_t *committed_pixels;
esp_err_t wallpaper_commit(const uint8_t *pixels)
{
    assert(pixels);
    ++commits;
    committed_pixels = pixels;
    wall_state = WALLPAPER_STATE_SAVING;
    return ESP_OK;
}
wallpaper_state_t wallpaper_status(void) { return wall_state; }
void wallpaper_ack_result(void)
{
    ++acks;
    wall_state = WALLPAPER_STATE_IDLE;
}

/* ---------------- LVGL 测试工具 ---------------- */
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
static void press_release(lv_indev_t *indev, lv_point_t from, lv_point_t to)
{
    pointer_point = from;
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(indev);
    advance(20);
    pointer_point = to;
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(indev);
    advance(60);
}
static void tap(lv_indev_t *indev, lv_obj_t *obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    lv_point_t point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    press_release(indev, point, point);
}
static bool hidden(lv_obj_t *obj) { return lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN); }
static const char *text_of(lv_obj_t *obj) { return lv_label_get_text(obj); }

/* 后台回调使用的假图像；只需要非空指针和尺寸。 */
static uint8_t thumb_pixels[IMAGE_THUMB_W * IMAGE_THUMB_H * 2];
static uint8_t full_pixels[IMAGE_FULL_PAD_H * IMAGE_FULL_PAD_W * 2];

int main(void)
{
    setvbuf(stdout, NULL, _IONBF, 0);
    lv_init();
    lv_display_t *display = lv_display_create(UI_HOR_RES, UI_VER_RES);
    static uint8_t disp_pixels[UI_HOR_RES * 20 * 2];
    lv_display_set_buffers(display, disp_pixels, NULL, sizeof(disp_pixels),
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);
    lv_obj_remove_flag(lv_screen_active(), LV_OBJ_FLAG_SCROLLABLE);

    /* 创建顺序与 ui_manager_init 一致：子页在前，壁纸选择页随后，
     * 状态栏替身最后。 */
    ui_lock_wallpaper_settings_create(lv_screen_active());
    ui_wallpaper_picker_create(lv_screen_active());
    status_bar = lv_obj_create(lv_screen_active());
    lv_obj_set_size(status_bar, UI_HOR_RES, UI_STATUSBAR_HEIGHT);
    lv_obj_update_layout(lv_screen_active());

    lv_obj_t *lock_page = lv_obj_get_child(lv_screen_active(), 0);
    lv_obj_t *picker_page = lv_obj_get_child(lv_screen_active(), 1);
    assert(hidden(lock_page) && hidden(picker_page));
    assert(lv_obj_get_x(picker_page) == UI_HOR_RES);

    /* ---- 三张卡片：防误触 / 锁屏设置 / 壁纸设置 ---- */
    assert(lv_obj_get_child_count(lock_page) == 5);
    lv_obj_t *title = lv_obj_get_child(lock_page, 0);
    assert(!strcmp(text_of(title), "锁屏和壁纸"));
    assert(lv_obj_get_style_text_font(title, 0) == ui_font_22());
    lv_obj_t *touch_card = lv_obj_get_child(lock_page, 2);
    lv_obj_t *lock_card = lv_obj_get_child(lock_page, 3);
    lv_obj_t *wall_card = lv_obj_get_child(lock_page, 4);
    lv_obj_t *cards[] = {touch_card, lock_card, wall_card};
    const char *names[] = {"防误触", "锁屏设置", "壁纸设置"};
    const int32_t tops[] = {112, 220, 328};
    for (unsigned i = 0; i < 3; ++i) {
        lv_obj_t *label = lv_obj_get_child(cards[i], 0);
        assert(!strcmp(text_of(label), names[i]));
        assert(lv_obj_get_style_text_font(label, 0) == ui_font_22());
        assert(lv_obj_get_x(cards[i]) == 40 && lv_obj_get_width(cards[i]) == 380);
        assert(lv_obj_get_height(cards[i]) == 88 &&
               lv_obj_get_y(cards[i]) == tops[i]);
    }
    lv_obj_t *sw = lv_obj_get_child(touch_card, 1);
    assert(lv_obj_check_type(sw, &lv_switch_class));
    assert(lv_obj_get_width(sw) == 110 && lv_obj_get_height(sw) == 56);
    assert(!lv_obj_has_flag(touch_card, LV_OBJ_FLAG_CLICKABLE));
    assert(lv_obj_has_flag(lock_card, LV_OBJ_FLAG_CLICKABLE));
    assert(lv_obj_has_flag(wall_card, LV_OBJ_FLAG_CLICKABLE));
    assert(!strcmp(text_of(lv_obj_get_child(lock_card, 1)), LV_SYMBOL_RIGHT));
    assert(!strcmp(text_of(lv_obj_get_child(wall_card, 1)), LV_SYMBOL_RIGHT));
    puts("PASS three cards ordered/labelled; only the two entries are clickable");

    /* ---- 防误触开关：NVS 恢复、点击写屏幕电源标志 ---- */
    ui_lock_wallpaper_settings_restore(true);
    assert(lv_obj_has_state(sw, LV_STATE_CHECKED) && !touch_wake_allowed);
    assert(ui_lock_wallpaper_settings_anti_touch_enabled());
    ui_lock_wallpaper_settings_restore(false);
    assert(!lv_obj_has_state(sw, LV_STATE_CHECKED) && touch_wake_allowed);
    assert(!ui_lock_wallpaper_settings_anti_touch_enabled());

    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);

    ui_lock_wallpaper_settings_show();
    advance(450);
    assert(!hidden(lock_page) && lv_obj_get_x(lock_page) == 0 &&
           ui_lock_wallpaper_settings_is_visible());

    tap(pointer, sw);
    assert(lv_obj_has_state(sw, LV_STATE_CHECKED) && !touch_wake_allowed);
    assert(ui_lock_wallpaper_settings_anti_touch_enabled());
    tap(pointer, sw);
    assert(!lv_obj_has_state(sw, LV_STATE_CHECKED) && touch_wake_allowed);
    /* 锁屏设置占位卡可点击但不跳转。 */
    tap(pointer, lock_card);
    assert(!ui_wallpaper_picker_is_visible());
    puts("PASS anti-touch switch toggles the screen-power flag; lock card stays a placeholder");

    /* ---- 打开壁纸选择页并接管图片后台 ---- */
    unsigned scans_before = scans;
    tap(pointer, wall_card);
    assert(ui_wallpaper_picker_is_visible());
    assert(scans == scans_before + 1 && backend_detach_count == 1);
    assert(ops.library && ops.scan_done && ops.thumbnail && ops.full_image);
    assert(!hidden(picker_page) && statusbar_visible);
    advance(450);
    assert(lv_obj_get_x(picker_page) == 0);
    puts("PASS wallpaper entry takes over the image backend and shows the grid");

    /* ---- 扫描结果：只关心 PICTURES，10 张按四列铺开 ---- */
    ops.library(IMAGE_FOLDER_CAM, 7, false, sd_generation, NULL);
    assert(thumb_requests == 0);
    ops.library(IMAGE_FOLDER_PICTURES, 10, false, sd_generation, NULL);
    ops.scan_done(IMAGE_SCAN_OK, sd_generation, NULL);
    assert(thumb_requests == 10);
    /* 后台回调只设置对象位置；LVGL 在下一轮布局后才更新 coords，
     * 与页面真实运行时的下一帧一致。 */
    advance(20);
    lv_obj_t *scroll = lv_obj_get_child(picker_page, 0);
    lv_obj_t *content = lv_obj_get_child(scroll, 0);
    assert(lv_obj_get_child_count(content) == 32);
    lv_obj_t *tile0 = lv_obj_get_child(content, 0);
    lv_obj_t *tile4 = lv_obj_get_child(content, 4);
    lv_obj_t *tile9 = lv_obj_get_child(content, 9);
    lv_obj_t *tile10 = lv_obj_get_child(content, 10);
    assert(!hidden(tile0) && lv_obj_get_x(tile0) == 12 && lv_obj_get_y(tile0) == 8);
    assert(!hidden(tile4) && lv_obj_get_x(tile4) == 12 &&
           lv_obj_get_y(tile4) == 8 + 112);
    /* 第 9 张（0 基）是第二行第 2 列：x=12+112、y=8+2*112。 */
    assert(!hidden(tile9) && lv_obj_get_x(tile9) == 12 + 112 &&
           lv_obj_get_y(tile9) == 8 + 2 * 112);
    assert(hidden(tile10));
    assert(lv_obj_get_height(content) == UI_VER_RES - UI_STATUSBAR_HEIGHT);
    puts("PASS scan fills the same 4-column/32-slot window as the image overview");

    /* 缩略图 token 必须来自本页的请求（高位段），旧图片页的 token 不会命中。 */
    lv_obj_t *tile0_img = lv_obj_get_child(tile0, 0);
    lv_obj_t *tile0_ph = lv_obj_get_child(tile0, 1);
    ops.thumbnail(0, thumb_pixels, IMAGE_THUMB_W, IMAGE_THUMB_H,
                  thumb_token[0], true, NULL);
    assert(!hidden(tile0_img) && hidden(tile0_ph));
    assert(lv_image_get_src(tile0_img) != NULL);
    assert(thumb_token[0] >= 0x40000000U);
    puts("PASS thumbnail result binds to the picker token high range");

    /* ---- 点击缩略图：无控制台，只有确认/关闭 ---- */
    tap(pointer, tile0);
    assert(full_requests == 1 && last_full_index == 0);
    assert(hidden(scroll) && !statusbar_visible);
    lv_obj_t *full = lv_obj_get_child(picker_page, 3);
    assert(!hidden(full));
    lv_obj_t *canvas = lv_obj_get_child(full, 0);
    lv_obj_t *full_status = lv_obj_get_child(full, 1);
    lv_obj_t *confirm = lv_obj_get_child(full, 2);
    lv_obj_t *cancel = lv_obj_get_child(full, 3);
    assert(hidden(canvas));
    assert(!strcmp(text_of(full_status), "正在加载..."));
    assert(lv_obj_has_state(confirm, LV_STATE_DISABLED));
    assert(!lv_obj_has_state(cancel, LV_STATE_DISABLED));
    assert(lv_obj_get_width(confirm) == 160 && lv_obj_get_height(confirm) == 52);
    assert(!strcmp(text_of(lv_obj_get_child(confirm, 0)), "确认"));
    assert(!strcmp(text_of(lv_obj_get_child(cancel, 0)), "关闭"));
    puts("PASS full preview has no console; confirm disabled while loading, cancel available");

    ops.full_image(full_pixels, IMAGE_FULL_PAD_W, IMAGE_FULL_PAD_H,
                   IMAGE_FOLDER_PICTURES, 0, "wall.jpg", last_full_token,
                   true, IMAGE_ERROR_NONE, NULL);
    assert(!hidden(canvas) && lv_canvas_get_buf(canvas) == full_pixels);
    assert(hidden(full_status));
    assert(!lv_obj_has_state(confirm, LV_STATE_DISABLED) &&
           !lv_obj_has_state(cancel, LV_STATE_DISABLED));
    puts("PASS decoded 464x464 pixels are published directly to the preview canvas");

    /* ---- 确认：提交像素，等待后台结果后恢复按键 ---- */
    unsigned commits_before = commits, acks_before = acks;
    tap(pointer, confirm);
    assert(commits == commits_before + 1 && committed_pixels == full_pixels);
    assert(lv_obj_has_state(confirm, LV_STATE_DISABLED) &&
           lv_obj_has_state(cancel, LV_STATE_DISABLED));
    assert(!strcmp(text_of(full_status), "正在保存壁纸..."));
    wall_state = WALLPAPER_STATE_SAVED;
    advance(120);
    assert(acks == acks_before + 1);
    assert(!strcmp(text_of(full_status), "壁纸已设置"));
    assert(!lv_obj_has_state(confirm, LV_STATE_DISABLED) &&
           !lv_obj_has_state(cancel, LV_STATE_DISABLED));
    puts("PASS confirm submits pixels and polls the wallpaper worker result");

    tap(pointer, cancel);
    assert(hidden(full) && !hidden(scroll) && statusbar_visible);
    puts("PASS cancel closes the preview and returns to the grid");

    /* ---- 右滑返回：交接后台并滑出 ---- */
    unsigned attaches_before = backend_attach_count;
    press_release(pointer, (lv_point_t){200, 200}, (lv_point_t){320, 200});
    advance(450);
    assert(!ui_wallpaper_picker_is_visible() && hidden(picker_page));
    assert(backend_attach_count == attaches_before + 1);
    assert(ui_lock_wallpaper_settings_is_visible());
    puts("PASS right swipe returns to the lock/wallpaper page and restores the image backend");

    /* ---- 再次打开：列表/缩略图缓存命中，不再重新扫描目录 ---- */
    unsigned scans_cached = scans, thumbs_cached = thumb_requests;
    ui_wallpaper_picker_show();
    advance(450);
    assert(ui_wallpaper_picker_is_visible());
    /* SD 代次未变且上次扫描已完成：不发起新的目录扫描，只为当前窗口
     * 重发缩略图请求（后台 s_thumb_mem 命中时不再读 SD/JPEG）。 */
    assert(scans == scans_cached);
    assert(thumb_requests == thumbs_cached + 10);
    puts("PASS reopening the picker reuses the cached list and only rebinds thumbnails");

    /* ---- 锁屏关闭：立即隐藏，不等待动画 ---- */
    ui_wallpaper_picker_close_for_lock();
    assert(!ui_wallpaper_picker_is_visible() && hidden(picker_page));
    assert(backend_attach_count == attaches_before + 2);
    puts("PASS lock close cancels preview/thumbnails and restores the image backend immediately");

    lv_deinit();
    puts("ALL LOCK/WALLPAPER TESTS PASSED");
    return 0;
}
