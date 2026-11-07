/**
 * @file ui_placeholder.c
 * @brief USB 声卡 / 拍照模式的占位页面实现
 *
 * 页面层级与其它模式页完全一致：
 *   固定壁纸层 → 各模式页面（含本文件的两个占位页）→ 锁屏 → 面板 → 状态栏
 * 因此模式切换动画、锁屏遮挡（ui_manager 会隐藏当前模式页）、边缘手势
 * 和状态栏都复用现有逻辑，这里不新增任何定时器、任务或硬件访问。
 *
 * 两种模式共用一个创建函数，差别只有“标题”和“图标来源”：
 *   - USB 声卡：LVGL 内置符号表已有 USB 字符（0xF287），直接用字体图标；
 *   - 拍照：内置字体没有相机字符，用 ui_camera_icon_create() 画矢量图标。
 * 以后实现真实功能时，把对应页面换成独立模块即可，模式切换代码不用改。
 */

#include "ui_placeholder.h"
#include "ui_fonts.h"

/** 一个占位页面的静态配置 */
typedef struct {
    ui_mode_t   mode;        /**< 对应的模式枚举 */
    const char *title;       /**< 页面中央的模式名称 */
    bool        camera_icon; /**< true=矢量相机图标，false=USB 字体符号 */
} placeholder_cfg_t;

/** 按模式切换栏中的顺序排列，方便对照。 */
static const placeholder_cfg_t s_cfg[] = {
    { UI_MODE_USB_AUDIO, "USB 声卡", false },
    { UI_MODE_CAMERA,    "拍照",     true  },
};
#define PLACEHOLDER_COUNT (sizeof(s_cfg) / sizeof(s_cfg[0]))

/** 已创建的占位页对象，与 s_cfg 一一对应。 */
static lv_obj_t *s_pages[PLACEHOLDER_COUNT];

/**
 * @brief 创建一个占位页面
 *
 * 页面是不透明的全屏对象：这两种模式没有壁纸需求，所以使用与音乐页
 * 相同的 UI_COLOR_PAGE_BG，不依赖底下的固定壁纸层，也不会因为用户
 * 更换壁纸而改变观感。
 *
 * 注意保留 EVENT_BUBBLE：页面本身没有任何可点击控件，如果吞掉触摸，
 * 左边缘右滑打开模式栏、状态栏下滑打开设置都会被挡住。这里只让页面
 * 参与事件冒泡，不注册任何自己的手势回调。
 */
static lv_obj_t *create_page(lv_obj_t *parent, const placeholder_cfg_t *cfg)
{
    lv_obj_t *page = lv_obj_create(parent);
    lv_obj_set_size(page, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(page, 0, 0);
    lv_obj_set_style_bg_color(page, UI_COLOR_PAGE_BG, 0);
    lv_obj_set_style_bg_opa(page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(page, 0, 0);
    lv_obj_set_style_radius(page, 0, 0);
    lv_obj_set_style_pad_all(page, 0, 0);
    lv_obj_remove_flag(page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(page, LV_OBJ_FLAG_EVENT_BUBBLE);
    /* 与其它模式页一致：创建后先隐藏，由 ui_manager 决定何时显示。 */
    lv_obj_add_flag(page, LV_OBJ_FLAG_HIDDEN);

    /* 居中的纵向内容列：图标 → 标题 → 提示。列本身不接触摸、不滚动，
     * 尺寸随内容自适应，因此页面仍是纯展示、不拦截任何输入。 */
    lv_obj_t *col = lv_obj_create(page);
    lv_obj_set_size(col, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_center(col);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 0, 0);
    /* 图标与标题、标题与提示之间的间距；调整只影响占位页排版。 */
    lv_obj_set_style_pad_row(col, 14, 0);
    lv_obj_remove_flag(col, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(col, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);

    if (cfg->camera_icon) {
        /* 72px 宽的矢量相机图标（高 63px），描边宽度自动换算为 3px。 */
        ui_camera_icon_create(col, 72);
    } else {
        /* 48px 字体符号；内置 montserrat_48 的符号表包含 USB(0xF287)。 */
        lv_obj_t *ico = lv_label_create(col);
        lv_label_set_text(ico, LV_SYMBOL_USB);
        lv_obj_set_style_text_color(ico, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_font(ico, &lv_font_montserrat_48, 0);
    }

    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, cfg->title);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    /* 使用 18px 全字库：它包含常用的中文，不需要用户再为 22px 子集
     * 字库补充“声/卡/拍/照/功/能/开/发/中”等新字。 */
    lv_obj_set_style_text_font(title, ui_font_18(), 0);

    lv_obj_t *hint = lv_label_create(col);
    lv_label_set_text(hint, "功能开发中");
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hint, ui_font_18(), 0);

    return page;
}

void ui_placeholder_create(lv_obj_t *parent)
{
    for (unsigned i = 0; i < PLACEHOLDER_COUNT; ++i)
        s_pages[i] = create_page(parent, &s_cfg[i]);
}

lv_obj_t *ui_placeholder_get_page(ui_mode_t mode)
{
    for (unsigned i = 0; i < PLACEHOLDER_COUNT; ++i) {
        if (s_cfg[i].mode == mode) return s_pages[i];
    }
    return NULL;
}
