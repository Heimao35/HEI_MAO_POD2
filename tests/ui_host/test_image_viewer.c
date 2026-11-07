/* Production image-viewer UI with a stub image_viewer backend. Verifies the
 * two folder cards, four-column virtualized grid, fullscreen controls,
 * status-bar hiding and the swipe/back gestures without touching SD or JPEG. */
#include <assert.h>
#include <stdio.h>
#include <string.h>

/* The host LVGL build only mounts the fonts used by the test; the card
 * placeholder uses 48px in firmware, so alias it to the 26px host font. */
#define lv_font_montserrat_48 lv_font_montserrat_26
#include "../../main/ui/ui_image_viewer.c"

/* ---------------- backend doubles ---------------- */
static image_viewer_ops_t ops;
static unsigned scan_requests, thumb_requests, full_requests;
static unsigned clear_thumbs_calls, cancel_full_calls;
static uint32_t thumb_slot[GRID_SLOTS];
static uint32_t thumb_index[GRID_SLOTS];
static uint32_t thumb_token[GRID_SLOTS];
static uint32_t last_full_index;
static uint32_t last_full_token;
static image_folder_t last_full_folder;

void image_viewer_set_ops(const image_viewer_ops_t *value, void *ctx)
{
    (void)ctx;
    ops = *value;
}

void image_viewer_request_scan(void) { ++scan_requests; }

void image_viewer_request_thumbnail(image_folder_t folder, uint32_t index,
                                    uint32_t slot, uint32_t token)
{
    (void)folder;
    assert(slot < GRID_SLOTS);
    ++thumb_requests;
    thumb_slot[slot] = slot;
    thumb_index[slot] = index;
    thumb_token[slot] = token;
}

void image_viewer_request_full(image_folder_t folder, uint32_t index,
                               uint32_t token)
{
    ++full_requests;
    last_full_folder = folder;
    last_full_index = index;
    last_full_token = token;
}

void image_viewer_cancel_full(void) { ++cancel_full_calls; }
void image_viewer_clear_thumbs(void) { ++clear_thumbs_calls; }
/* 本地补丁：离开模式销毁worker、进入时重建；本测试只验证UI调用顺序。 */
static unsigned worker_exits;
void image_viewer_request_worker_exit(void) { ++worker_exits; }
esp_err_t image_viewer_ensure_started(void) { return ESP_OK; }
static bool full_active_calls;
void image_viewer_set_full_active(bool active) { full_active_calls = active; }

/* ---------------- platform doubles ---------------- */
static ui_mode_t mock_mode = UI_MODE_IMAGE;
static bool statusbar_visible = true;
static uint32_t mock_generation = 7;

uint32_t sd_card_get_generation(void) { return mock_generation; }
bool sd_card_is_present(void) { return true; }
ui_mode_t ui_manager_get_current_mode(void) { return mock_mode; }
bool ui_lockscreen_is_visible(void) { return false; }
void ui_statusbar_set_visible(bool visible) { statusbar_visible = visible; }
const lv_font_t *ui_font_18(void) { return &lv_font_montserrat_18; }
const lv_font_t *ui_font_22(void) { return &lv_font_montserrat_22; }
/* 壁纸模块由独立测试覆盖；本页只验证“首页最底层会创建一个 canvas”。
 * 返回 NULL 表示没有壁纸，页面其余布局与手势不应受影响。 */
lv_obj_t *wallpaper_create_canvas(lv_obj_t *parent) { (void)parent; return NULL; }

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

static void click_obj(lv_indev_t *indev, lv_obj_t *obj)
{
    lv_area_t area;
    lv_obj_get_coords(obj, &area);
    lv_point_t point = {(area.x1 + area.x2) / 2, (area.y1 + area.y2) / 2};
    press_release(indev, point, point);
}

static void swipe(lv_indev_t *indev, lv_point_t from, lv_point_t to)
{
    press_release(indev, from, to);
}

/* Dummy decoded pixels returned by the backend callbacks. */
static uint8_t home_cam_pixels[IMAGE_HOME_W * IMAGE_HOME_H * 2];
static uint8_t thumb_pixels[IMAGE_THUMB_W * IMAGE_THUMB_H * 2];
static uint8_t full_pixels[IMAGE_FULL_PAD_W * IMAGE_FULL_PAD_H * 2];

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

    ui_image_viewer_create(lv_screen_active());
    lv_indev_t *pointer = lv_indev_create();
    lv_indev_set_type(pointer, LV_INDEV_TYPE_POINTER);
    lv_indev_set_read_cb(pointer, read_pointer);
    lv_indev_set_mode(pointer, LV_INDEV_MODE_EVENT);

    lv_obj_t *card_cam = lv_obj_get_child(s_home, 0);
    lv_obj_t *card_pictures = lv_obj_get_child(s_home, 1);
    lv_obj_update_layout(lv_screen_active());

    /* 首页两张卡片：几乎全宽、各约半屏高，名称为22号全字库。 */
    assert(lv_obj_get_width(card_cam) == 440);
    assert(lv_obj_get_height(card_cam) == 196);
    assert(lv_obj_get_y(card_cam) == UI_STATUSBAR_HEIGHT + 10);
    assert(lv_obj_get_y(card_pictures) ==
           lv_obj_get_y(card_cam) + 196 + HOME_CARD_GAP);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(card_cam, 1)), "相机") == 0);
    assert(strcmp(lv_label_get_text(lv_obj_get_child(card_pictures, 1)), "其他图片") == 0);
    assert(lv_obj_get_style_text_font(lv_obj_get_child(card_cam, 1), 0) == ui_font_22());
    assert(lv_obj_get_style_text_font(lv_obj_get_child(card_pictures, 1), 0) == ui_font_22());
    assert(lv_obj_has_flag(s_overview, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_x(s_overview) == UI_HOR_RES);
    assert(lv_obj_has_flag(s_full, LV_OBJ_FLAG_HIDDEN));
    puts("PASS home cards, names, fonts and initial hidden pages");

    /* 模式进入：未扫描时请求后台扫描并显示18号状态。 */
    ui_image_viewer_on_mode_enter();
    assert(scan_requests == 1);
    assert(!lv_obj_has_flag(s_home_status, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(s_home_status), "正在扫描图片...") == 0);
    assert(lv_obj_get_style_text_font(s_home_status, 0) == ui_font_18());
    puts("PASS mode entry requests scan and shows status");

    /* 扫描结果：cam 5张，pictures 0张；只有 cam 有首页预览。 */
    assert(ops.library != NULL && ops.home_preview != NULL && ops.scan_done != NULL);
    ops.library(IMAGE_FOLDER_CAM, 5, false, mock_generation, NULL);
    ops.library(IMAGE_FOLDER_PICTURES, 0, false, mock_generation, NULL);
    ops.home_preview(IMAGE_FOLDER_CAM, home_cam_pixels, IMAGE_HOME_W,
                     IMAGE_HOME_H, mock_generation, true, NULL);
    ops.home_preview(IMAGE_FOLDER_PICTURES, NULL, IMAGE_HOME_W,
                     IMAGE_HOME_H, mock_generation, false, NULL);
    ops.scan_done(IMAGE_SCAN_OK, mock_generation, NULL);
    assert(lv_obj_has_flag(s_home_status, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(s_home_imgs[IMAGE_FOLDER_CAM], LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_home_phs[IMAGE_FOLDER_CAM], LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_home_imgs[IMAGE_FOLDER_PICTURES], LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(s_home_phs[IMAGE_FOLDER_PICTURES], LV_OBJ_FLAG_HIDDEN));
    puts("PASS scan result fills cam preview and leaves pictures placeholder");

    /* 按下卡片应有提亮+白色描边；拖动释放后自动恢复，不误进文件夹。 */
    lv_area_t card_area;
    lv_obj_get_coords(card_cam, &card_area);
    lv_point_t card_center = {(card_area.x1 + card_area.x2) / 2,
                              (card_area.y1 + card_area.y2) / 2};
    pointer_point = card_center;
    pointer_state = LV_INDEV_STATE_PRESSED;
    lv_indev_read(pointer);
    advance(20);
    assert(lv_obj_has_state(card_cam, LV_STATE_PRESSED));
    assert(lv_color_eq(lv_obj_get_style_bg_color(card_cam, 0),
                       UI_COLOR_PRESSED_BG));
    assert(lv_obj_get_style_border_width(card_cam, 0) == 2);
    pointer_point = (lv_point_t){card_center.x + 160, card_center.y};
    pointer_state = LV_INDEV_STATE_RELEASED;
    lv_indev_read(pointer);
    advance(60);
    assert(!lv_obj_has_state(card_cam, LV_STATE_PRESSED));
    assert(lv_obj_get_style_border_width(card_cam, 0) == 0);
    assert(!s_overview_visible);
    puts("PASS home card pressed feedback appears and clears without a stray click");

    /* 在卡片左边缘右滑是打开模式栏的手势，不能同时触发卡片 CLICKED。 */
    swipe(pointer, (lv_point_t){20, 150}, (lv_point_t){140, 150});
    advance(400);
    assert(!s_overview_visible);
    assert(!lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN));
    puts("PASS home edge swipe is suppressed instead of opening the folder");

    /* 点击相机卡片：右侧滑入总览，滑入完成后隐藏首页。 */
    click_obj(pointer, card_cam);
    assert(s_overview_visible);
    advance(400);
    assert(lv_obj_get_x(s_overview) == 0);
    assert(lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_overview_status, LV_OBJ_FLAG_HIDDEN));
    /* 5张图按4列排列：前4张在第一行，第5张第二行；32个tile中只显示5个。 */
    lv_obj_t *tile0 = lv_obj_get_child(s_overview_content, 0);
    lv_obj_t *tile4 = lv_obj_get_child(s_overview_content, 4);
    lv_obj_t *tile5 = lv_obj_get_child(s_overview_content, 5);
    assert(lv_obj_get_x(tile0) == GRID_X0 && lv_obj_get_y(tile0) == GRID_Y0);
    assert(lv_obj_get_x(tile4) == GRID_X0 &&
           lv_obj_get_y(tile4) == GRID_Y0 + GRID_ROW_H);
    assert(lv_obj_has_flag(tile5, LV_OBJ_FLAG_HIDDEN));
    assert(thumb_requests >= 5);
    /* 第0张缩略图回调（使用UI实际请求的token）。 */
    ops.thumbnail(0, thumb_pixels, IMAGE_THUMB_W, IMAGE_THUMB_H,
                  thumb_token[0], true, NULL);
    assert(!lv_obj_has_flag(s_tile_imgs[0], LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_tile_phs[0], LV_OBJ_FLAG_HIDDEN));
    puts("PASS overview slides in, lays out four columns and publishes thumbnail");

    /* 点击第一张：进入全屏并隐藏状态栏。 */
    click_obj(pointer, tile0);
    assert(s_full_active && !statusbar_visible);
    assert(full_active_calls);
    assert(lv_obj_has_flag(s_overview, LV_OBJ_FLAG_HIDDEN));
    assert(full_requests == 1 && last_full_index == 0 &&
           last_full_folder == IMAGE_FOLDER_CAM);
    assert(!lv_obj_has_flag(s_full_status, LV_OBJ_FLAG_HIDDEN));
    /* 回调前名称使用18号全字库（加载中由状态提示负责，名称为空）。 */
    assert(lv_obj_get_style_text_font(s_full_name, 0) == ui_font_18());
    ops.full_image(full_pixels, IMAGE_FULL_PAD_W, IMAGE_FULL_PAD_H,
                   IMAGE_FOLDER_CAM, 0, "测试图片_a.jpg", last_full_token, true,
                   IMAGE_ERROR_NONE, NULL);
    assert(!lv_obj_has_flag(s_full_canvas, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_full_status, LV_OBJ_FLAG_HIDDEN));
    assert(strcmp(lv_label_get_text(s_full_name), "测试图片_a.jpg") == 0);
    puts("PASS fullscreen opens, hides status bar and shows name at 18px font");

    /* 左滑切到下一张；加载期间保留旧图，回调后更新名称。 */
    swipe(pointer, (lv_point_t){200, 230}, (lv_point_t){80, 230});
    assert(full_requests == 2 && last_full_index == 1);
    assert(s_full_loading);
    ops.full_image(full_pixels, IMAGE_FULL_PAD_W, IMAGE_FULL_PAD_H,
                   IMAGE_FOLDER_CAM, 1, "测试图片_b.jpg", last_full_token, true,
                   IMAGE_ERROR_NONE, NULL);
    assert(!s_full_loading);
    assert(s_full_sliding);
    assert(strcmp(lv_label_get_text(s_full_name), "测试图片_b.jpg") == 0);
    /* 等交叉滑动结束：旧图滑出、新图成为当前canvas。 */
    advance(FULL_SWITCH_MS + 100);
    assert(!s_full_sliding);
    assert(!lv_obj_has_flag(s_full_canvas, LV_OBJ_FLAG_HIDDEN));
    puts("PASS fullscreen horizontal swipe cross-slides to the next image");

    /* 点击空白切换控制台；控制台可见时返回键可点击。 */
    click_obj(pointer, s_full);
    advance(FULL_CONTROLS_FADE_MS + 50);
    lv_obj_t *back = lv_obj_get_child(s_full_controls, 1);
    if (!s_controls_visible) click_obj(pointer, s_full);
    advance(FULL_CONTROLS_FADE_MS + 50);
    assert(s_controls_visible);
    click_obj(pointer, back);
    assert(!s_full_active && statusbar_visible);
    assert(!full_active_calls);
    assert(!lv_obj_has_flag(s_overview, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_has_flag(s_full, LV_OBJ_FLAG_HIDDEN));
    puts("PASS tap controls and back button return to overview with status bar");

    /* 对角/带纵向分量的滑动不能误触发返回；只有干净右滑才返回。 */
    swipe(pointer, (lv_point_t){150, 300}, (lv_point_t){230, 240});
    advance(400);
    assert(s_overview_visible);
    /* 总览非边缘右滑返回首页。 */
    swipe(pointer, (lv_point_t){200, 230}, (lv_point_t){330, 230});
    advance(400);
    assert(!s_overview_visible);
    assert(lv_obj_has_flag(s_overview, LV_OBJ_FLAG_HIDDEN));
    assert(!lv_obj_has_flag(s_home, LV_OBJ_FLAG_HIDDEN));
    puts("PASS overview right swipe slides back to home");

    /* 模式离开应取消在途请求并恢复状态栏。 */
    ui_image_viewer_on_mode_leave();
    assert(statusbar_visible);
    assert(cancel_full_calls > 0);
    assert(clear_thumbs_calls > 0);
    puts("ALL IMAGE VIEWER UI TESTS PASSED");
    return 0;
}
