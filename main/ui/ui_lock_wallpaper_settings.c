/**
 * @file ui_lock_wallpaper_settings.c
 * @brief “锁屏和壁纸”设置子页实现（三张卡片 + 防误触开关）。
 *
 * 页面层级与维护约束：
 * - 与 ui_clock_settings.c / ui_motion_settings.c 同级：主屏幕内的全屏
 *   子页，初始位于屏幕右侧并隐藏；show() 只做位移动画，不创建/销毁
 *   对象，也不改变设置主页的两列网格、顶部展开/底部收起手势；
 * - 页面不向外冒泡事件/手势，避免卡片点击或返回动画触发下层设置页
 *   收起、锁屏解锁或播放器切歌；
 * - 三张卡片（从上到下）：防误触、锁屏设置、壁纸设置。防误触右侧是
 *   开关；另外两张整卡可点击。锁屏设置本轮只做占位，点击仅有按下
 *   反馈，不跳转、不写任何设置；壁纸设置打开壁纸选择页；
 * - 防误触开关只维护用户意图（RAM bool），并立即把结果交给
 *   screen_power 的原子标志；持久化沿用 player_settings 的统一快照，
 *   本页不直接写 Flash、不访问 I2C/SD；
 * - UI 文案使用已有 14/16/18/22 号字库；卡片标题用 22 号，保证中文
 *   与原有设置页字号一致。
 */

#include "ui_lock_wallpaper_settings.h"

#include "ui_fonts.h"
#include "ui_wallpaper_picker.h"
#include "screen_power.h"

/* 卡片尺寸参照姿态感应子页的行宽（380px），高度缩小到 88px 以容纳
 * 三张卡片并给标题/返回键留出空间：
 *   112..200、220..308、328..416，底部仍有 44px 余量。
 * 圆角 18 与图片查看器首页卡片一致，颜色沿用列表行底色。 */
#define CARD_W        380
#define CARD_H        88
#define CARD_X        ((UI_HOR_RES - CARD_W) / 2)
#define CARD_FIRST_Y  112
#define CARD_GAP      20
#define CARD_RADIUS   18
#define CARD_TEXT_X   24

/* 防误触开关：比姿态感应页的 140x70 略小，才能在 88px 高的卡片内
 * 保留上下各 16px 的呼吸空间；颜色/选中逻辑与姿态页完全相同。 */
#define SWITCH_W      110
#define SWITCH_H      56

static lv_obj_t *s_page;
static lv_obj_t *s_touch_switch;
static bool s_visible;
/* 防误触的用户意图。false = 熄屏后触摸仍可唤醒（沿用原行为），
 * 开机由 restore() 套用 NVS 快照。 */
static bool s_anti_touch;

/** @brief 子页 X 位移动画执行回调：只改位置，不改布局。 */
static void anim_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

/** @brief 滑出完成：隐藏子页并清零可见标志。 */
static void hidden(lv_anim_t *anim)
{
    (void)anim;
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
}

/**
 * @brief 从当前 X 位置滑出到屏幕右侧。
 *
 * 不把起点写死为 0：用户在滑入途中触发锁屏时，动画从实际位置继续，
 * 不会先跳到 0 再滑出。350ms / ease-out 与其他设置子页一致。
 */
static void slide_out(void)
{
    lv_anim_delete(s_page, anim_x);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_page);
    lv_anim_set_values(&a, lv_obj_get_x(s_page), UI_HOR_RES);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_set_completed_cb(&a, hidden);
    lv_anim_start(&a);
}

/**
 * @brief 防误触开关变化：更新 RAM 意图并立即通知屏幕电源任务。
 *
 * 开关的选中状态由 LVGL CHECKABLE 行为维护，这里只读取结果，避免
 * 手动取反和重复点击造成状态不一致。screen_power 只记录原子标志，
 * 下一次熄屏进入 Light-sleep 时才调整触摸 EXT1 唤醒源；本回调不
 * 阻塞、不访问 Flash。
 */
static void touch_switch_changed(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    s_anti_touch = lv_obj_has_state(sw, LV_STATE_CHECKED);
    screen_power_set_touch_wake(!s_anti_touch);
}

/** @brief “壁纸设置”整卡点击：打开壁纸选择页（滑入覆盖本页）。 */
static void wallpaper_clicked(lv_event_t *event)
{
    (void)event;
    ui_wallpaper_picker_show();
}

/**
 * @brief “锁屏设置”整卡点击占位。
 *
 * 本轮需求只要求可点击；不跳转、不修改任何锁屏行为。保留独立回调
 * 便于后续接入时替换实现，也避免把壁纸入口误接到这张卡片上。
 */
static void lock_settings_clicked(lv_event_t *event)
{
    (void)event;
}

static void back_clicked(lv_event_t *event)
{
    (void)event;
    ui_lock_wallpaper_settings_hide();
}

/**
 * @brief 创建一张 380x88 的卡片。
 *
 * 卡片使用列表行底色 + 18px 圆角，文字 22px 白色、左侧 24px 居中。
 * clickable 为 true 时整卡接收点击，并应用统一的按下提亮/白色描边；
 * 防误触卡片不设 CLICKABLE，只有右侧开关是点击目标，避免用户点到
 * 文字区域就误切换。
 */
static lv_obj_t *create_card(lv_obj_t *parent, int32_t y, const char *text,
                             bool clickable)
{
    lv_obj_t *card = lv_obj_create(parent);
    lv_obj_set_size(card, CARD_W, CARD_H);
    lv_obj_set_pos(card, CARD_X, y);
    lv_obj_set_style_bg_color(card, UI_COLOR_LIST_ROW_BG, 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(card, 0, 0);
    lv_obj_set_style_radius(card, CARD_RADIUS, 0);
    lv_obj_set_style_pad_all(card, 0, 0);
    /* 基类对象默认带 CLICKABLE；先统一移除，只有真正可点击的卡片再
     * 加回来。防误触卡片仅由右侧开关响应，点文字区域不会误切换。 */
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);
    if (clickable) {
        lv_obj_add_flag(card, LV_OBJ_FLAG_CLICKABLE);
        ui_apply_pressed_style(card);
    }

    lv_obj_t *label = lv_label_create(card);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_22(), 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, CARD_TEXT_X, 0);
    lv_obj_remove_flag(label, LV_OBJ_FLAG_CLICKABLE);
    return card;
}

/**
 * @brief 给可点击卡片右侧加一个箭头，提示“可进入”。
 *
 * 只使用内置 Montserrat 的 LV_SYMBOL_RIGHT，不依赖中文/图标字库中
 * 是否收录特殊字符；箭头不接触摸，点击仍由整卡处理。
 */
static void add_chevron(lv_obj_t *card)
{
    lv_obj_t *arrow = lv_label_create(card);
    lv_label_set_text(arrow, LV_SYMBOL_RIGHT);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(arrow, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_align(arrow, LV_ALIGN_RIGHT_MID, -24, 0);
    lv_obj_remove_flag(arrow, LV_OBJ_FLAG_CLICKABLE);
}

/**
 * @brief 创建防误触开关。
 *
 * 颜色显式覆盖默认主题，规则与姿态感应子页一致：
 * - 轨道（MAIN）关闭时为中性深灰；
 * - 选中指示层（INDICATOR）关闭透明、打开铺满白色；
 * - 旋钮在深色轨道上为白色，在白色轨道上反转为页面深色。
 * 不改 LVGL 开关自带的点击、选中和滑动动画。
 */
static lv_obj_t *create_touch_switch(lv_obj_t *parent)
{
    lv_obj_t *sw = lv_switch_create(parent);
    lv_obj_set_size(sw, SWITCH_W, SWITCH_H);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -16, 0);
    lv_obj_set_style_bg_color(sw, UI_COLOR_CONTROL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sw, 0, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, UI_COLOR_TEXT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(sw, UI_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, UI_COLOR_PAGE_BG_DARK, LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(sw, touch_switch_changed, LV_EVENT_VALUE_CHANGED, NULL);
    return sw;
}

void ui_lock_wallpaper_settings_create(lv_obj_t *parent)
{
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_page, UI_HOR_RES, 0);
    lv_obj_set_style_bg_color(s_page, UI_COLOR_PAGE_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE |
                              LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);

    /* 标题/返回键与时钟、姿态感应子页完全一致：22px 标题居中于顶部
     * 16px，44x44 返回键位于左上角，只画内置左箭头。 */
    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "锁屏和壁纸");
    lv_obj_set_style_text_font(title, ui_font_22(), 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    lv_obj_t *back = lv_button_create(s_page);
    lv_obj_set_size(back, 44, 44);
    lv_obj_set_pos(back, 12, 8);
    lv_obj_set_style_bg_opa(back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(back, 0, 0);
    lv_obj_set_style_shadow_width(back, 0, 0);
    lv_obj_t *arrow = lv_label_create(back);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(arrow, UI_COLOR_TEXT, 0);
    lv_obj_center(arrow);
    lv_obj_add_event_cb(back, back_clicked, LV_EVENT_CLICKED, NULL);

    /* 第一张：防误触。整卡不接触摸，只有开关响应。 */
    lv_obj_t *touch_card = create_card(s_page, CARD_FIRST_Y, "防误触", false);
    s_touch_switch = create_touch_switch(touch_card);

    /* 第二张：锁屏设置占位。可点击但不执行动作。 */
    lv_obj_t *lock_card = create_card(s_page, CARD_FIRST_Y + CARD_H + CARD_GAP,
                                      "锁屏设置", true);
    add_chevron(lock_card);
    lv_obj_add_event_cb(lock_card, lock_settings_clicked, LV_EVENT_CLICKED, NULL);

    /* 第三张：壁纸设置，点击进入图片选择页。 */
    lv_obj_t *wall_card = create_card(s_page, CARD_FIRST_Y + 2 * (CARD_H + CARD_GAP),
                                      "壁纸设置", true);
    add_chevron(wall_card);
    lv_obj_add_event_cb(wall_card, wallpaper_clicked, LV_EVENT_CLICKED, NULL);

    /* 创建时保持关闭；main 在 ui_manager_init() 之后、同一 LVGL 锁内
     * 调用 restore() 套用 NVS 快照。页面创建本身不发 I2C/Flash。 */
    s_anti_touch = false;
    s_visible = false;
}

void ui_lock_wallpaper_settings_restore(bool anti_touch)
{
    if (!s_page) return;
    s_anti_touch = anti_touch;
    /* 程序化设置 CHECKED 不会触发 LV_EVENT_VALUE_CHANGED（该事件只由
     * 用户交互产生），因此不会重复进入 touch_switch_changed。 */
    if (anti_touch) lv_obj_add_state(s_touch_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(s_touch_switch, LV_STATE_CHECKED);
    /* 通知屏幕电源任务：防误触打开时熄屏状态不使用触摸唤醒。 */
    screen_power_set_touch_wake(!anti_touch);
}

bool ui_lock_wallpaper_settings_anti_touch_enabled(void)
{
    return s_anti_touch;
}

void ui_lock_wallpaper_settings_show(void)
{
    if (!s_page || s_visible) return;
    lv_anim_delete(s_page, anim_x);
    /* 与时钟/姿态子页一致：每次从右侧边界重新开始，不依赖上次滑出位置。 */
    lv_obj_set_x(s_page, UI_HOR_RES);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_page);
    s_visible = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_page);
    lv_anim_set_values(&a, UI_HOR_RES, 0);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_start(&a);
}

void ui_lock_wallpaper_settings_hide(void)
{
    if (!s_page || !s_visible) return;
    slide_out();
}

void ui_lock_wallpaper_settings_close_for_lock(void)
{
    /* 锁屏只负责收起浮层；不保存、不等待后台操作。 */
    if (!s_page || !s_visible) return;
    slide_out();
}

bool ui_lock_wallpaper_settings_is_visible(void)
{
    return s_visible;
}
