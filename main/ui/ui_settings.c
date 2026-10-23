/**
 * @file ui_settings.c
 * @brief 设置页面实现
 *
 * 全屏覆盖页面，深色背景，内含两列圆形按钮网格。
 * 展开时隐藏状态栏，收起时恢复状态栏。
 */

#include "ui_settings.h"
#include "ui_statusbar.h"
#include "ui_fonts.h"
#include "ui_clock_settings.h"
#include "ui_motion_settings.h"
#include "ui_bluetooth_settings.h"
#include "bt_audio.h"

static lv_obj_t *s_page = NULL; /**< 设置页面容器 */
static bool s_visible = false;

/* 蓝牙入口按钮及其内部对象。用于在蓝牙开/关时切换按钮配色，
 * 让网格里的入口本身也能表示当前开关状态（不只依赖子页里的开关）。
 * 只保存指针，不改按钮尺寸、网格位置、图标或点击逻辑。 */
static lv_obj_t *s_bt_button = NULL;
static lv_obj_t *s_bt_icon = NULL;
static lv_obj_t *s_bt_caption = NULL;
static bool s_bt_visual_on = false;

/* 蓝牙入口的长按消费标志。
 *
 * LVGL 在 LONG_PRESSED 之后，用户松手时仍会向同一对象发送 CLICKED。
 * 如果不做区分，长按进入蓝牙页面的同时还会执行一次短按的开关切换。
 * 每次新的 PRESSED 先把标志清零，保证上一轮残留不影响下一次短按。 */
static bool s_bt_long_pressed = false;

/** 设置项信息表 */
static const struct {
    const char *symbol;  /**< 按钮图标符号 */
    const char *label;   /**< 按钮下方文字 */
} s_items[] = {
    /* 固定文字使用已经挂载的 14 号中文字库；仅替换文案，不改变
     * 原有图标、按钮尺寸、网格位置和点击逻辑，后续新增设置项也应
     * 在这里集中维护中文显示名称。 */
    { LV_SYMBOL_WIFI,     "无线网络"   },
    { LV_SYMBOL_BLUETOOTH,"蓝牙"       },
    { LV_SYMBOL_GPS,      "姿态感应"   },  /* 姿态感应用 GPS/指南针图标代替 */
    { NULL,               "时钟"       },  /* 小表盘使用矢量绘制，不依赖字库图标 */
    { LV_SYMBOL_IMAGE,    "锁屏设置"   },  /* 锁屏页面设置 */
};
#define ITEM_COUNT (sizeof(s_items) / sizeof(s_items[0]))

/**
 * @brief 把蓝牙入口按钮绘制成“开/关”两种颜色状态。
 *
 * 开：白色圆形底 + 黑色蓝牙图标 + 白色“蓝牙”文字，与蓝牙子页里
 *     白色选中轨道的大开关保持一致；
 * 关：深灰圆底 + 白色图标 + 灰色文字，与其他设置按钮完全一致。
 *
 * 只改变颜色，不改变 100px 圆形按钮的尺寸、两列网格位置、图标字形
 * 或触摸目标；也不新增动画，避免影响原有设置页手势和布局。
 */
static void bt_button_apply(bool on)
{
    if (!s_bt_button || !s_bt_icon || !s_bt_caption) return;
    if (on) {
        lv_obj_set_style_bg_color(s_bt_button, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_color(s_bt_icon, UI_COLOR_ON_ACCENT, 0);
        lv_obj_set_style_text_color(s_bt_caption, UI_COLOR_TEXT, 0);
    } else {
        lv_obj_set_style_bg_color(s_bt_button, UI_COLOR_CONTROL_BG, 0);
        lv_obj_set_style_text_color(s_bt_icon, UI_COLOR_TEXT, 0);
        lv_obj_set_style_text_color(s_bt_caption, UI_COLOR_TEXT_CAPTION, 0);
    }
}

void ui_settings_refresh_bluetooth(void)
{
    if (!s_bt_button) return;
    bool on = bt_audio_is_enabled();
    /* 状态未变化时避免重复设置样式，减少无意义的重绘。 */
    if (on == s_bt_visual_on) return;
    s_bt_visual_on = on;
    bt_button_apply(on);
}

/**
 * @brief 蓝牙入口的新按下回调：清除上一轮的长按消费标志。
 *
 * 每次短按/长按都以 PRESSED 开始，先复位标志，再等待本次手势的
 * LONG_PRESSED 或 CLICKED。这样即使某次长按后系统没有派发 CLICKED，
 * 也不会让下一次短按被误吞。
 */
static void bt_item_pressed_cb(lv_event_t *e)
{
    (void)e;
    s_bt_long_pressed = false;
}

/**
 * @brief 蓝牙入口长按：进入蓝牙设置子页。
 *
 * 长按只负责打开页面，不切换开关；松手后的 CLICKED 由
 * item_click_cb() 根据 s_bt_long_pressed 丢弃。页面样式、动画和返回
 * 行为与时钟/姿态感应子页一致，不改设置主页布局。
 */
static void bt_item_long_pressed_cb(lv_event_t *e)
{
    unsigned idx = (unsigned)(uintptr_t)lv_event_get_user_data(e);
    if (idx != 1) return;
    s_bt_long_pressed = true;
    ui_bluetooth_settings_show();
}

/**
 * @brief 设置项按钮点击回调。
 *
 * 索引 1 是“蓝牙”：短按切换电源意图（开关状态不写 NVS，上电默认
 * 关闭），长按已由 bt_item_long_pressed_cb() 打开设置页，这里必须
 * 丢弃同一次交互产生的 CLICKED，避免“进页面顺带切换开关”。
 * 索引 2 是“姿态感应”，索引 3 是“时钟”；其余条目保留原入口，
 * 后续按各自功能接入。两个已存在的子页都只是覆盖在设置主页上的
 * 全屏页面，隐藏/显示不改变设置主页的网格、状态栏和手势。
 */
static void item_click_cb(lv_event_t *e)
{
    int idx = (int)(uintptr_t)lv_event_get_user_data(e);
    if (idx == 1) {
        if (s_bt_long_pressed) {
            s_bt_long_pressed = false;
            return;
        }
        /* 只记录用户意图并更新开关；真正的 U4 链路启停由 bt_audio
         * 后端异步完成。短按不打开页面，页面内另有同样的开关。
         * 立即刷新按钮颜色，让用户在网格上就能看出开关已经切换。 */
        bt_audio_set_enabled(!bt_audio_is_enabled());
        ui_settings_refresh_bluetooth();
    }
    else if(idx==2) ui_motion_settings_show();
    else if(idx==3) ui_clock_settings_show();
    /* 其余设置项保留原入口，后续按各自功能接入。 */
}

/**
 * @brief 创建设置页面
 *
 * 页面全屏，顶部留 50px 放标题，下方为可上下滚动的两列按钮网格。
 * 每个 item 容器宽度 (屏幕宽-80)/2 ≈ 190px，确保严格两列排列。
 * 按钮直径 100px，图标字体 26px，修改此值改变按钮大小。
 *
 * @param parent 父对象
 */
void ui_settings_create(lv_obj_t *parent)
{
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_page, 0, -(int32_t)UI_VER_RES);
    lv_obj_set_style_bg_color(s_page, UI_COLOR_PAGE_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 标题 */
    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "设置");
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_set_style_text_font(title, ui_font_22(), 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    /* 可上下滚动的按钮网格容器 */
    lv_obj_t *grid = lv_obj_create(s_page);
    lv_obj_set_size(grid, UI_HOR_RES - 20, UI_VER_RES - 50);
    lv_obj_align(grid, LV_ALIGN_TOP_MID, 0, 50);
    lv_obj_set_style_bg_opa(grid, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(grid, 0, 0);
    lv_obj_set_style_pad_all(grid, 10, 0);
    lv_obj_set_flex_flow(grid, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(grid, LV_FLEX_ALIGN_SPACE_EVENLY,
                          LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
    lv_obj_set_style_pad_row(grid, 16, 0);
    lv_obj_set_style_pad_column(grid, 16, 0);
    /* 允许上下滚动查看更多设置按钮 */
    lv_obj_add_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(grid, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(grid, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(grid, LV_OBJ_FLAG_EVENT_BUBBLE);

    /* 每个 item 容器宽度：严格两列排列 = (grid宽 - padding*2 - 列间距) / 2
     * item 宽度 180px，确保一行只能放两个 */
    const int32_t item_w = 180;
    const int32_t item_h = 140;

    for (size_t i = 0; i < ITEM_COUNT; i++) {
        lv_obj_t *item = lv_obj_create(grid);
        lv_obj_set_size(item, item_w, item_h);
        lv_obj_set_style_bg_opa(item, LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(item, 0, 0);
        lv_obj_set_style_pad_all(item, 0, 0);
        lv_obj_remove_flag(item, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(item, LV_OBJ_FLAG_EVENT_BUBBLE);

        /* 圆形按钮 100x100，增大此值使按钮更大 */
        lv_obj_t *btn = lv_obj_create(item);
        lv_obj_set_size(btn, 100, 100);
        lv_obj_set_style_radius(btn, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(btn, UI_COLOR_CONTROL_BG, 0);
        lv_obj_set_style_bg_opa(btn, LV_OPA_COVER, 0);
        lv_obj_set_style_border_width(btn, 0, 0);
        lv_obj_align(btn, LV_ALIGN_TOP_MID, 0, 0);
        lv_obj_remove_flag(btn, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_add_flag(btn, LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_event_cb(btn, item_click_cb, LV_EVENT_CLICKED,
                            (void *)(uintptr_t)i);

        /* 仅蓝牙入口（索引 1）注册长按：短按切换开关，长按进入设置
         * 子页。默认长按阈值沿用 LVGL 的 400ms，不修改全局输入参数；
         * 其他设置项保持原有单击行为。 */
        if (i == 1) {
            lv_obj_add_event_cb(btn, bt_item_pressed_cb, LV_EVENT_PRESSED,
                                (void *)(uintptr_t)i);
            lv_obj_add_event_cb(btn, bt_item_long_pressed_cb,
                                LV_EVENT_LONG_PRESSED, (void *)(uintptr_t)i);
        }

        /* 图标字体 26px，增大使图标更醒目 */
        if(i==3) {
            /* 时钟图标为 40px 圆形表盘加两根指针。保持原 100px 按钮、
             * 网格位置、触摸目标不变；只替换齿轮，避免依赖二进制字库
             * 中未必包含的 FontAwesome 时钟字符。所有子对象不接触摸，
             * 点击表盘内部仍完整交给原按钮回调。线点数组需永久有效。 */
            lv_obj_t *dial=lv_obj_create(btn);
            lv_obj_set_size(dial,40,40);lv_obj_center(dial);
            lv_obj_set_style_radius(dial,LV_RADIUS_CIRCLE,0);
            lv_obj_set_style_bg_opa(dial,LV_OPA_TRANSP,0);
            lv_obj_set_style_border_color(dial,UI_COLOR_TEXT,0);
            lv_obj_set_style_border_width(dial,3,0);
            lv_obj_set_style_pad_all(dial,0,0);
            lv_obj_remove_flag(dial,LV_OBJ_FLAG_CLICKABLE|LV_OBJ_FLAG_SCROLLABLE);
            static const lv_point_precise_t hands[]={{17,6},{17,17},{26,22}};
            lv_obj_t *line=lv_line_create(dial);
            lv_line_set_points(line,hands,3);
            lv_obj_set_style_line_color(line,UI_COLOR_TEXT,0);
            lv_obj_set_style_line_width(line,3,0);
            lv_obj_set_style_line_rounded(line,true,0);
            /* 表盘 3px 边框内的内容区是 34px，中心为 (17,17)。
             * 指针跟随表盘居中，不依赖外层按钮的主题 padding。 */
            lv_obj_set_size(line,34,34);
            lv_obj_set_pos(line,0,0);
            lv_obj_remove_flag(line,LV_OBJ_FLAG_CLICKABLE);
        } else {
            lv_obj_t *ico = lv_label_create(btn);
            lv_label_set_text(ico, s_items[i].symbol);
            lv_obj_set_style_text_color(ico, UI_COLOR_TEXT, 0);
            lv_obj_set_style_text_font(ico, &lv_font_montserrat_26, 0);
            lv_obj_center(ico);
            /* 蓝牙图标在开启时反色，需要保留指针。其他图标仍由 LVGL
             * 正常管理，不额外保存。 */
            if (i == 1) s_bt_icon = ico;
        }

        /* 按钮下方文字 */
        lv_obj_t *lbl = lv_label_create(item);
        lv_label_set_text(lbl, s_items[i].label);
        lv_obj_set_style_text_color(lbl, UI_COLOR_TEXT_CAPTION, 0);
        lv_obj_set_style_text_font(lbl, ui_font_14(), 0);
        lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, 0);
        if (i == 1) {
            s_bt_button = btn;
            s_bt_caption = lbl;
        }
    }

    /* 按当前蓝牙电源意图初始化入口外观（上电默认关闭）。这里只设置
     * 颜色，不读取 NVS、不启动链路，也不改变任何按钮位置。 */
    s_bt_visual_on = bt_audio_is_enabled();
    bt_button_apply(s_bt_visual_on);

    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

/** @brief 动画 Y 坐标执行回调 */
static void anim_y_cb(void *obj, int32_t v)
{
    lv_obj_set_y((lv_obj_t *)obj, v);
}

/** @brief 展开动画完成回调 */
static void show_ready(lv_anim_t *a)
{
    (void)a;
    s_visible = true;
    ui_statusbar_set_visible(false);
}

/** @brief 收起动画完成回调 */
static void hide_ready(lv_anim_t *a)
{
    (void)a;
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    ui_statusbar_set_visible(true);
}

/** @brief 展开设置页面（从顶部下滑） */
void ui_settings_show(void)
{
    if (!s_page || s_visible) return;

    /* 蓝牙子页的开关可能在设置主页被覆盖期间改变电源意图；展开设置
     * 主页时先同步入口按钮颜色。只比较 RAM 状态，不访问后端硬件。 */
    ui_settings_refresh_bluetooth();

    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_page);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_page);
    lv_anim_set_values(&a, -(int32_t)UI_VER_RES, 0);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_completed_cb(&a, show_ready);
    lv_anim_start(&a);

    s_visible = true;
    ui_statusbar_set_visible(false);
}

/** @brief 收起设置页面（向顶部上滑） */
void ui_settings_hide(void)
{
    if (!s_page || !s_visible) return;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_page);
    lv_anim_set_values(&a, 0, -(int32_t)UI_VER_RES);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_y_cb);
    lv_anim_set_completed_cb(&a, hide_ready);
    lv_anim_start(&a);
}

/** @brief 查询设置页面是否展开 */
bool ui_settings_is_visible(void)
{
    return s_visible;
}
