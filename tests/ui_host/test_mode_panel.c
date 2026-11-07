/* 模式切换栏回归：真实 LVGL + 生产 ui_mode_panel.c / ui_common.c。
 *
 * 覆盖本轮新增的两个入口：
 *   - 六个按钮的顺序、80x80 尺寸与可点击性不变；
 *   - USB 声卡使用内置 USB 符号，拍照使用矢量相机图标；
 *   - 点击按钮把正确的 ui_mode_t 交给上层回调；
 *   - 面板仍可纵向滚动（六个按钮超出一屏），且不影响透明命中层。
 * 不访问 SD、USB、摄像头或物理显示。 */
#include <assert.h>
#include <stdio.h>
#include <string.h>
#include "../../main/ui/ui_common.c"
#include "../../main/ui/ui_mode_panel.c"

static ui_mode_t switched_modes[8];
static unsigned switch_count;

static void on_switch(ui_mode_t mode)
{
    assert(switch_count < sizeof(switched_modes) / sizeof(switched_modes[0]));
    switched_modes[switch_count++] = mode;
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

    ui_mode_panel_create(lv_screen_active());
    ui_mode_panel_set_callback(on_switch);
    /* LVGL 的 coords 在布局前不更新；按真实打开流程先展开再布局，之后
     * 才能用 lv_obj_get_width/height 断言 80x80。 */
    ui_mode_panel_show();
    lv_obj_update_layout(lv_screen_active());

    /* 六个模式，原有四项顺序不变，新增两项追加在末尾。 */
    assert(MODE_COUNT == 6);
    assert(s_modes[0].mode == UI_MODE_MUSIC && s_modes[1].mode == UI_MODE_VIDEO);
    assert(s_modes[2].mode == UI_MODE_IMAGE && s_modes[3].mode == UI_MODE_EBOOK);
    assert(s_modes[4].mode == UI_MODE_USB_AUDIO && s_modes[5].mode == UI_MODE_CAMERA);
    assert(lv_obj_get_child_count(s_panel) == 6);

    /* 每个按钮仍是 80x80、可点击，且只有一个图标子对象。 */
    for (unsigned i = 0; i < 6; ++i) {
        lv_obj_t *btn = lv_obj_get_child(s_panel, (int32_t)i);
        assert(lv_obj_get_width(btn) == 80 && lv_obj_get_height(btn) == 80);
        assert(lv_obj_has_flag(btn, LV_OBJ_FLAG_CLICKABLE));
        assert(lv_obj_get_child_count(btn) == 1);
    }

    /* USB 声卡：内置符号表中的 USB 字符，使用 24px 符号字体。 */
    lv_obj_t *usb_btn = lv_obj_get_child(s_panel, 4);
    lv_obj_t *usb_label = lv_obj_get_child(usb_btn, 0);
    assert(lv_obj_check_type(usb_label, &lv_label_class));
    assert(!strcmp(lv_label_get_text(usb_label), LV_SYMBOL_USB));
    assert(lv_obj_get_style_text_font(usb_label, 0) == &lv_font_montserrat_24);

    /* 拍照：内置字体没有相机字符，按钮里是用基础图形画的图标：
     * 容器 + 顶部凸起 + 机身 + 镜头，全部白色描边、透明填充、不接触摸，
     * 点击必须继续由父按钮处理。 */
    lv_obj_t *camera_btn = lv_obj_get_child(s_panel, 5);
    lv_obj_t *camera_icon = lv_obj_get_child(camera_btn, 0);
    assert(camera_icon && lv_obj_get_child_count(camera_icon) == 3);
    assert(!lv_obj_has_flag(camera_icon, LV_OBJ_FLAG_CLICKABLE));
    for (unsigned part = 0; part < 3; ++part) {
        lv_obj_t *obj = lv_obj_get_child(camera_icon, (int32_t)part);
        assert(!lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE));
        assert(lv_obj_get_style_bg_opa(obj, 0) == LV_OPA_TRANSP);
        assert(lv_obj_get_style_border_width(obj, 0) == 2);
        assert(lv_color_eq(lv_obj_get_style_border_color(obj, 0), UI_COLOR_TEXT));
    }
    /* 镜头（第三个子对象）必须是圆形。 */
    lv_obj_t *lens = lv_obj_get_child(camera_icon, 2);
    assert(lv_obj_get_style_radius(lens, 0) == LV_RADIUS_CIRCLE);
    assert(lv_obj_get_width(lens) == lv_obj_get_height(lens));

    /* 点击每个按钮都只回调一次，并带上表中对应的模式。 */
    for (unsigned i = 0; i < 6; ++i) {
        lv_obj_t *btn = lv_obj_get_child(s_panel, (int32_t)i);
        unsigned before = switch_count;
        lv_obj_send_event(btn, LV_EVENT_CLICKED, NULL);
        assert(switch_count == before + 1);
        assert(switched_modes[before] == s_modes[i].mode);
    }
    assert(switched_modes[4] == UI_MODE_USB_AUDIO && switched_modes[5] == UI_MODE_CAMERA);
    puts("PASS six mode buttons in order, USB symbol and vector camera icon, click callbacks");

    /* 六个 80px 按钮超出一屏，仍沿用原来的纵向滚动设置；滚动条保持
     * 隐藏，透明命中层仍然可点击且不绘制任何像素。 */
    assert(lv_obj_has_flag(s_panel, LV_OBJ_FLAG_SCROLLABLE));
    assert(lv_obj_get_scroll_dir(s_panel) == LV_DIR_VER);
    assert(lv_obj_get_scrollbar_mode(s_panel) == LV_SCROLLBAR_MODE_OFF);
    assert(lv_obj_get_style_bg_opa(s_overlay, 0) == LV_OPA_TRANSP);
    assert(lv_obj_has_flag(s_overlay, LV_OBJ_FLAG_CLICKABLE));
    assert(lv_obj_get_child_count(s_panel) == MODE_COUNT);
    puts("PASS panel stays vertically scrollable; transparent click-away layer unchanged");

    lv_deinit();
    puts("ALL MODE PANEL TESTS PASSED");
    return 0;
}
