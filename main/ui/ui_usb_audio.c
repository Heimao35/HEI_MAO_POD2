/**
 * @file ui_usb_audio.c
 * @brief USB 声卡模式页面实现
 *
 * 结构（从底到顶）：
 *   固定壁纸层（ui_manager 创建）
 *   └─ s_page（本页，透明、全屏、EVENT_BUBBLE）
 *       └─ s_console（460x120 横向矩形控制台，垂直居中）
 *           ├─ 标题 “USB声卡模式” 22px
 *           └─ 规格 “96kHz/24bit” 18px
 *
 * 交互：
 *   - 控制台自己和标签都不接触摸，点击整页任意位置都能到达 s_page；
 *   - 点击切换控制台显隐，动画与视频播放器控制台一致：220ms ease-out
 *     透明度渐变，隐藏动画结束后才设置 HIDDEN；
 *   - 页面保留 EVENT_BUBBLE，左边缘右滑打开模式栏、状态栏下滑打开设置
 *     等全局手势仍由主屏幕接收，不被本页吞掉。
 *
 * 本页不显示连接状态：USB/TinyUSB 的枚举、充电电流和音频会话由
 * usb_audio.c 的 worker 负责，UI 线程只投递进入/离开通知。
 */

#include "ui_usb_audio.h"
#include "ui_fonts.h"
#include "usb_audio.h"

/* 与视频播放器控制台相同的淡入淡出时长（ui_video_player.c 的
 * CONTROLS_FADE_MS）。只复制数值，不引用视频模块内部符号。 */
#define USB_CONSOLE_FADE_MS 220
/* 控制台高度：两行文字（22px 标题 + 18px 规格）加行距后的最小高度，
 * 增大只会加高控制台，不影响页面其它元素。 */
#define USB_CONSOLE_HEIGHT  120

static lv_obj_t *s_page;
static lv_obj_t *s_console;
static lv_opa_t s_console_opa = LV_OPA_COVER;
static bool s_console_visible = true;

/* ---------------- 控制台显隐动画 ---------------- */

static void console_fade_exec(void *obj, int32_t value)
{
    s_console_opa = (lv_opa_t)value;
    lv_obj_set_style_opa((lv_obj_t *)obj, (lv_opa_t)value, 0);
}

static void console_fade_done(lv_anim_t *anim)
{
    (void)anim;
    /* 只有隐藏动画完成后才真正隐藏；中途反向显示不会残留 HIDDEN。 */
    if (!s_console_visible) lv_obj_add_flag(s_console, LV_OBJ_FLAG_HIDDEN);
}

static void console_show(void)
{
    if (!s_console || s_console_visible) return;
    lv_anim_delete(s_console, console_fade_exec);
    lv_obj_remove_flag(s_console, LV_OBJ_FLAG_HIDDEN);
    s_console_visible = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_console);
    /* 从当前透明度继续，避免快速连点时的闪烁。 */
    lv_anim_set_values(&a, s_console_opa, LV_OPA_COVER);
    lv_anim_set_duration(&a, USB_CONSOLE_FADE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, console_fade_exec);
    lv_anim_start(&a);
}

static void console_hide(void)
{
    if (!s_console || !s_console_visible) return;
    /* 先置逻辑状态，隐藏完成回调据此决定是否 HIDDEN。 */
    s_console_visible = false;
    lv_anim_delete(s_console, console_fade_exec);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_console);
    lv_anim_set_values(&a, s_console_opa, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, USB_CONSOLE_FADE_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, console_fade_exec);
    lv_anim_set_completed_cb(&a, console_fade_done);
    lv_anim_start(&a);
}

/** 点击页面任意位置：控制台可见则收起，否则唤出。 */
static void page_click_cb(lv_event_t *e)
{
    (void)e;
    if (s_console_visible) console_hide();
    else console_show();
}

/* ---------------- 页面创建 ---------------- */

void ui_usb_audio_create(lv_obj_t *parent)
{
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_page, 0, 0);
    /* 背景透明：与视频/图片/电子书首页相同，固定壁纸层直接透出。
     * 没有壁纸时看到的是壁纸层的 UI_COLOR_PAGE_BG 纯色。 */
    lv_obj_set_style_bg_opa(s_page, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    /* 全局手势（边缘模式栏/状态栏下拉）需要事件冒泡到主屏幕。 */
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_event_cb(s_page, page_click_cb, LV_EVENT_CLICKED, NULL);

    /* ---- 横向控制台：与屏幕等宽、垂直居中 ---- */
    s_console = lv_obj_create(s_page);
    lv_obj_set_size(s_console, UI_HOR_RES, USB_CONSOLE_HEIGHT);
    lv_obj_align(s_console, LV_ALIGN_CENTER, 0, 0);
    /* 深色矩形：无圆角、无边框，与“控制台”定位一致；80% 不透明度
     * 让壁纸仍隐约可见，文字保持清晰。 */
    lv_obj_set_style_bg_color(s_console, UI_COLOR_PAGE_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_console, LV_OPA_80, 0);
    lv_obj_set_style_border_width(s_console, 0, 0);
    lv_obj_set_style_radius(s_console, 0, 0);
    lv_obj_set_style_pad_all(s_console, 0, 0);
    /* 不接触摸：点击控制台等于点击页面，仍然切换显隐。 */
    lv_obj_remove_flag(s_console, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    /* 两行文字纵向排列并整体居中；pad_row 为标题与规格的间距。 */
    lv_obj_set_flex_flow(s_console, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_console, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_row(s_console, 10, 0);

    lv_obj_t *title = lv_label_create(s_console);
    lv_label_set_text(title, "USB声卡模式");
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    /* 标题使用 22px 字库（与其它子页标题一致）；新增的“声/卡/模/式”
     * 需要存在于 font/UI_FONT_22.bin，否则会显示缺字符占位。 */
    lv_obj_set_style_text_font(title, ui_font_22(), 0);

    lv_obj_t *spec = lv_label_create(s_console);
    lv_label_set_text(spec, "96kHz/24bit");
    lv_obj_set_style_text_color(spec, UI_COLOR_TEXT_SECONDARY, 0);
    /* 规格是纯 ASCII，18px 全字库即可，无需额外字模。 */
    lv_obj_set_style_text_font(spec, ui_font_18(), 0);

    /* 与其它模式页一致：创建后隐藏，由 ui_manager 的模式切换显示。 */
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
}

lv_obj_t *ui_usb_audio_get_page(void) { return s_page; }

void ui_usb_audio_on_mode_enter(void)
{
    /* 每次进入声卡模式都把控制台恢复为完全显示，不继承上一次的显隐状态：
     * 取消可能仍在进行的淡入/淡出动画后直接设为可见。此函数在LVGL锁内
     * 被 ui_manager 的模式切换调用，可以直接操作对象。 */
    if (s_console) {
        lv_anim_delete(s_console, console_fade_exec);
        s_console_visible = true;
        s_console_opa = LV_OPA_COVER;
        lv_obj_remove_flag(s_console, LV_OBJ_FLAG_HIDDEN);
        lv_obj_set_style_opa(s_console, LV_OPA_COVER, 0);
    }
    usb_audio_mode_enter();
}

void ui_usb_audio_on_mode_leave(void)
{
    usb_audio_mode_leave();
}
