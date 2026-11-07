/* USB 声卡页面回归：真实 LVGL + 生产 ui_usb_audio.c / ui_common.c。
 *
 * 覆盖：
 *   - 页面透明（固定壁纸层透出）、保留 EVENT_BUBBLE；
 *   - 与屏幕等宽、垂直居中的矩形控制台，无圆角/边框；
 *   - “USB声卡模式”22px、“96kHz/24bit”18px，两行居中；
 *   - 点击页面任意位置切换控制台显隐，220ms 透明度动画结束后才 HIDDEN；
 *   - 进入/离开模式转交给 usb_audio 通知。
 * 不访问 USB、音频、SD 或物理显示。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/ui/ui_common.c"
#include "../../main/ui/ui_usb_audio.c"

const lv_font_t *ui_font_18(void) { return &lv_font_montserrat_18; }
const lv_font_t *ui_font_22(void) { return &lv_font_montserrat_22; }

static unsigned enters, leaves;
void usb_audio_mode_enter(void) { ++enters; }
void usb_audio_mode_leave(void) { ++leaves; }
/* 线缆状态：本测试固定为“未插入”，状态行保持默认提示。 */
uint8_t usb_audio_cable_status(void) { return 0; }

static void advance(unsigned ms)
{
    for (unsigned i = 0; i < ms; i += 10) {
        lv_tick_inc(10);
        lv_timer_handler();
    }
}

static void flush(lv_display_t *display, const lv_area_t *area, uint8_t *pixels)
{
    (void)area;
    (void)pixels;
    lv_display_flush_ready(display);
}

int main(void)
{
    lv_init();
    lv_display_t *display = lv_display_create(UI_HOR_RES, UI_VER_RES);
    static uint8_t pixels[UI_HOR_RES * 20 * 2];
    lv_display_set_buffers(display, pixels, NULL, sizeof(pixels), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display, flush);

    ui_usb_audio_create(lv_screen_active());
    lv_obj_t *page = ui_usb_audio_get_page();
    assert(page);

    /* 页面与视频/图片/电子书首页一致：自身透明，背景由壁纸层提供；
     * 保留事件冒泡，边缘手势仍能到达主屏幕。默认隐藏由 ui_manager 显示。 */
    assert(lv_obj_get_style_bg_opa(page, 0) == LV_OPA_TRANSP);
    assert(lv_obj_has_flag(page, LV_OBJ_FLAG_EVENT_BUBBLE));
    assert(lv_obj_has_flag(page, LV_OBJ_FLAG_HIDDEN));

    /* 控制台：与屏幕等宽、垂直居中、无圆角/边框、80% 深色底。
     * 模拟 ui_manager 显示页面后再布局，LVGL 的 coords 才会更新。 */
    lv_obj_t *console = lv_obj_get_child(page, 0);
    assert(console && lv_obj_get_child_count(page) == 1);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_update_layout(page);
    assert(lv_obj_get_width(console) == UI_HOR_RES);
    assert(lv_obj_get_height(console) == 150);
    lv_area_t area;
    lv_obj_get_coords(console, &area);
    assert(area.x1 == 0 && area.x2 == UI_HOR_RES - 1);
    assert(area.y1 + area.y2 == UI_VER_RES - 1); /* 垂直居中 */
    assert(lv_obj_get_style_radius(console, 0) == 0);
    assert(lv_obj_get_style_border_width(console, 0) == 0);
    assert(lv_obj_get_style_bg_opa(console, 0) == LV_OPA_80);
    /* 控制台不接触摸：点击它等价于点击页面，仍然切换显隐。 */
    assert(!lv_obj_has_flag(console, LV_OBJ_FLAG_CLICKABLE));

    /* 两行文字：22px 标题 + 18px 规格，均使用配置的字体。 */
    lv_obj_t *title = lv_obj_get_child(console, 0);
    lv_obj_t *spec = lv_obj_get_child(console, 1);
    assert(title && spec && lv_obj_get_child_count(console) == 3);
    /* 第三行是 USB 数据线状态占位，默认提示插入数据线。 */
    lv_obj_t *status = lv_obj_get_child(console, 2);
    assert(status && !strcmp(lv_label_get_text(status), "请插入 USB 数据线"));
    assert(!strcmp(lv_label_get_text(title), "USB声卡模式"));
    assert(lv_obj_get_style_text_font(title, 0) == ui_font_22());
    assert(lv_color_eq(lv_obj_get_style_text_color(title, 0), UI_COLOR_TEXT));
    assert(!strcmp(lv_label_get_text(spec), "96kHz/24bit"));
    assert(lv_obj_get_style_text_font(spec, 0) == ui_font_18());
    puts("PASS transparent wallpaper page, full-width centered console and 22/18px texts");

    /* 第一次点击：控制台淡出，动画结束前不应立刻 HIDDEN。 */
    lv_obj_send_event(page, LV_EVENT_CLICKED, NULL);
    advance(50);
    assert(!lv_obj_has_flag(console, LV_OBJ_FLAG_HIDDEN));
    advance(250);
    assert(lv_obj_has_flag(console, LV_OBJ_FLAG_HIDDEN));
    /* 再次点击：淡入并恢复完全可见。 */
    lv_obj_send_event(page, LV_EVENT_CLICKED, NULL);
    assert(!lv_obj_has_flag(console, LV_OBJ_FLAG_HIDDEN));
    advance(250);
    assert(lv_obj_get_style_opa(console, 0) == LV_OPA_COVER);
    puts("PASS tap-to-toggle console with 220ms fade, hidden only after fade-out");

    /* 进入模式时控制台强制恢复完全显示，不继承上一次的隐藏状态。 */
    lv_obj_send_event(page, LV_EVENT_CLICKED, NULL); /* 先隐藏 */
    advance(300);
    assert(lv_obj_has_flag(console, LV_OBJ_FLAG_HIDDEN));
    ui_usb_audio_on_mode_enter();
    assert(!lv_obj_has_flag(console, LV_OBJ_FLAG_HIDDEN));
    assert(lv_obj_get_style_opa(console, 0) == LV_OPA_COVER);
    ui_usb_audio_on_mode_leave();
    assert(enters == 1 && leaves == 1);
    puts("PASS console is forced visible on every mode entry");

    lv_deinit();
    puts("ALL USB AUDIO PAGE TESTS PASSED");
    return 0;
}
