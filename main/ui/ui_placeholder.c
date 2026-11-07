/**
 * @file ui_placeholder.c
 * @brief 拍照模式占位页面实现
 *
 * 页面层级与其它模式页完全一致：
 *   固定壁纸层 → 各模式页面（含本页）→ 锁屏 → 面板 → 状态栏
 * 因此模式切换动画、锁屏遮挡（ui_manager 会隐藏当前模式页）、边缘手势
 * 和状态栏都复用现有逻辑，这里不新增任何定时器、任务或硬件访问。
 *
 * 只有拍照模式使用本模块：图标用 ui_camera_icon_create() 画矢量相机
 * （LVGL 内置符号字体没有相机字符）。USB 声卡模式已由 ui_usb_audio.c
 * 实现，不再经过这里。
 */

#include "ui_placeholder.h"
#include "ui_fonts.h"

/** 已创建的占位页对象。目前只有拍照一个模式。 */
static lv_obj_t *s_page;

/**
 * @brief 创建拍照占位页面
 *
 * 页面是不透明的全屏对象：与音乐页相同的 UI_COLOR_PAGE_BG，不依赖底下
 * 的固定壁纸层，也不会因为用户更换壁纸而改变观感。
 *
 * 注意保留 EVENT_BUBBLE：页面本身没有任何可点击控件，如果吞掉触摸，
 * 左边缘右滑打开模式栏、状态栏下滑打开设置都会被挡住。这里只让页面
 * 参与事件冒泡，不注册任何自己的手势回调。
 */
void ui_placeholder_create(lv_obj_t *parent)
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

    /* 72px 宽的矢量相机图标（高 63px），描边宽度自动换算为 3px。 */
    ui_camera_icon_create(col, 72);

    lv_obj_t *title = lv_label_create(col);
    lv_label_set_text(title, "拍照");
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    /* 使用 18px 全字库：它包含常用的中文，不需要用户再为 22px 子集
     * 字库补充“拍/照/功/能/开/发/中”等新字。 */
    lv_obj_set_style_text_font(title, ui_font_18(), 0);

    lv_obj_t *hint = lv_label_create(col);
    lv_label_set_text(hint, "功能开发中");
    lv_obj_set_style_text_color(hint, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_font(hint, ui_font_18(), 0);

    s_page = page;
}

lv_obj_t *ui_placeholder_get_page(ui_mode_t mode)
{
    return mode == UI_MODE_CAMERA ? s_page : NULL;
}
