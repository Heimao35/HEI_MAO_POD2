/**
 * @file ui_motion_settings.c
 * @brief 姿态感应设置子页：两项大尺寸开关，样式复用时钟设置子页的进出动画。
 *
 * 维护约定：
 * - 本页只是设置页的视觉子页，不建立新的屏幕/显示对象，也不改变设置主页
 *   的两列网格、顶部展开和底部收起手势；
 * - 与 ui_clock_settings.c 一样，页面放在主屏幕内、初始位于屏幕右侧，
 *   点击设置项后从右侧滑入，左上角箭头从当前位置滑回右侧；
 * - 本页没有滚轮、定时器、任务、文件或 I2C 访问；
 * - 两个开关维护用户意图，并向 BMI270 worker 投递 RAM 请求；
 *   所有硬件配置在 LVGL 锁外完成，不修改本页的布局、动画或触摸逻辑；
 * - 开关值接入现有 player_settings 快照：player_settings_capture() 在
 *   电源 worker 的 LVGL 锁内读取下面两个 RAM 标志，经 3 秒合并后写入
 *   NVS；本页自身不写 Flash。开机由 main 调用 restore() 恢复。
 */

#include "ui_motion_settings.h"
#include "ui_fonts.h"
#include "motion_sensor.h"
#include "ui_lockscreen.h"

/* 两个条目作为一个整体居中：行宽 380、行高 96、间距 36。
 * 120 和 252 两行覆盖 120..348，视觉中心约 234，接近 460 屏幕的中心，
 * 同时给顶部标题和左上返回箭头留出空间。 */
#define ROW_W        380
#define ROW_H        96
#define ROW_X        ((UI_HOR_RES - ROW_W) / 2)
#define ROW_FIRST_Y  120
#define ROW_GAP      132

/* 开关尺寸只影响本页：页面内容少，采用 140x70 的大轨道，让 22 号文字
 * 与开关比例协调；旋钮直径由 LVGL 按对象高度计算（70px）。 */
#define SWITCH_W     140
#define SWITCH_H     70

static lv_obj_t *s_motion_page;
static lv_obj_t *s_motion_back;
static lv_obj_t *s_motion_steps_switch;
static lv_obj_t *s_motion_wake_switch;
static bool s_motion_visible;

/* 开关的 RAM 状态（用户意图）。创建时默认关闭；开机由 restore() 套用
 * NVS 快照，之后用户点击立即更新。状态不代表芯片已通过初始化/通信验证，
 * 持久化由既有 player_settings 快照链路统一完成，UI 不直接写 Flash。 */
static bool s_motion_steps_enabled;
static bool s_motion_wake_enabled;

/** @brief 子页 X 位移动画执行回调。与时钟子页一致，只改位置，不改布局。 */
static void anim_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

/** @brief 滑出完成：隐藏子页并把可见标志清零。 */
static void hidden(lv_anim_t *anim)
{
    (void)anim;
    lv_obj_add_flag(s_motion_page, LV_OBJ_FLAG_HIDDEN);
    s_motion_visible = false;
}

/**
 * @brief 从当前 X 位置滑出到屏幕右侧。
 *
 * 不把起点写死为 0：用户在滑入途中点击返回时，动画应从实际位置继续，
 * 避免页面先跳到 0 再滑出。350ms、ease-out 与时钟子页/设置主页一致。
 */
static void slide_out(void)
{
    lv_anim_delete(s_motion_page, anim_x);
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_motion_page);
    lv_anim_set_values(&a, lv_obj_get_x(s_motion_page), UI_HOR_RES);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_set_completed_cb(&a, hidden);
    lv_anim_start(&a);
}

/**
 * @brief 开关状态变化：更新 RAM 意图并投递后台请求，不做 I2C/Flash。
 *
 * user_data 为 0 表示计步开关，1 表示抬手亮屏开关。开关自身由 LVGL
 * 的 CHECKABLE 行为维护选中状态，这里只读取结果，避免手动取反造成
 * 与动画/重复点击不一致。这里只向 BMI270 worker 投递原子 RAM 请求，
 * 真正的 I2C 配置由传感器任务在 LVGL 锁外完成。保存沿用现有设置链路：
 * player_settings_capture() 随后读取这两个 RAM 标志，由电源 worker 在
 * 停止调整 3 秒后合并写入 NVS，因此本回调不直接访问 Flash。
 */
static void switch_changed_cb(lv_event_t *e)
{
    lv_obj_t *sw = lv_event_get_target(e);
    bool checked = lv_obj_has_state(sw, LV_STATE_CHECKED);
    if ((uintptr_t)lv_event_get_user_data(e) == 0) s_motion_steps_enabled = checked;
    else s_motion_wake_enabled = checked;
    /* 温度不依赖这两个开关，后台始终低频读取。关闭计步后立即隐藏旧步数，
     * 避免 I2C 正忙时仍显示正在计步；再次开启保留本次运行中当天的步数。
     * 抬腕开关只改变传感器功能/唤醒策略，绝不直接点亮屏幕或重播动画。 */
    if (!s_motion_steps_enabled) ui_lockscreen_set_steps(false, 0);
    motion_sensor_set_features(s_motion_steps_enabled, s_motion_wake_enabled);
}

/** @brief 返回键点击：只调用无参的收起函数，避免直接转换函数指针类型。 */
static void back_clicked_cb(lv_event_t *e)
{
    (void)e;
    ui_motion_settings_hide();
}

/**
 * @brief 创建一个“左侧文字 + 右侧大开关”的条目。
 *
 * 行容器本身取消 CLICKABLE/SCROLLABLE：只有开关是点击目标，避免把
 * 整行做成一个看不见的大按钮，导致用户点到文字区域也误切换功能。
 * 文字与开关都在行内垂直居中，行整体水平居中（ROW_X）。
 *
 * 开关颜色显式覆盖默认主题：
 * - 轨道（MAIN）关闭时为中性深灰 0x303030；
 * - 选中指示层（INDICATOR）关闭时透明，打开时铺满白色；
 * - 旋钮（KNOB）在深色轨道上为白色，在白色轨道上反转为页面深色，
 *   保证两种状态都能看清旋钮位置。
 * 这里只改颜色/尺寸，不改变 LVGL 开关自带的点击、选中和滑动动画。
 */
static void create_row(lv_obj_t *parent, int32_t y, const char *text,
                       lv_obj_t **switch_out, uintptr_t index)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, ROW_W, ROW_H);
    lv_obj_set_pos(row, ROW_X, y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* 22 号是用户允许的最大字号；文字颜色沿用普通页面的白色。 */
    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_font(label, ui_font_22(), 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);

    lv_obj_t *sw = lv_switch_create(row);
    lv_obj_set_size(sw, SWITCH_W, SWITCH_H);
    lv_obj_align(sw, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(sw, UI_COLOR_CONTROL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(sw, 0, LV_PART_MAIN);
    /* 关闭：不画选中指示层，露出深灰轨道。 */
    lv_obj_set_style_bg_opa(sw, LV_OPA_TRANSP, LV_PART_INDICATOR);
    /* 打开：白色选中层铺满轨道。 */
    lv_obj_set_style_bg_color(sw, UI_COLOR_TEXT, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(sw, LV_OPA_COVER, LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    /* 旋钮在两种底色上保持对比。 */
    lv_obj_set_style_bg_color(sw, UI_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_set_style_bg_color(sw, UI_COLOR_PAGE_BG_DARK, LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_radius(sw, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(sw, 0, LV_PART_KNOB);
    lv_obj_add_event_cb(sw, switch_changed_cb, LV_EVENT_VALUE_CHANGED,
                        (void *)index);

    *switch_out = sw;
}

void ui_motion_settings_create(lv_obj_t *parent)
{
    /* 全屏子页与时钟设置子页同层：覆盖在设置主页上方，但仍是主屏幕的子对象。
     * 初始放在屏幕右侧并隐藏，show() 只做位移动画，不创建/销毁对象。 */
    s_motion_page = lv_obj_create(parent);
    lv_obj_set_size(s_motion_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_motion_page, UI_HOR_RES, 0);
    lv_obj_set_style_bg_color(s_motion_page, UI_COLOR_PAGE_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_motion_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_motion_page, 0, 0);
    lv_obj_set_style_radius(s_motion_page, 0, 0);
    lv_obj_set_style_pad_all(s_motion_page, 0, 0);
    /* 子页不向外冒泡事件/手势：返回动画或开关点击不能触发设置页收起、
     * 锁屏解锁或播放器切歌。页面本身不滚动，内容固定居中。 */
    lv_obj_remove_flag(s_motion_page, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_EVENT_BUBBLE |
                              LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_motion_page, LV_OBJ_FLAG_HIDDEN);

    /* 标题与时钟设置子页保持完全相同的字号、颜色和顶部位置。 */
    lv_obj_t *title = lv_label_create(s_motion_page);
    lv_label_set_text(title, "姿态感应");
    lv_obj_set_style_text_font(title, ui_font_22(), 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    /* 返回键只画 LVGL 自带左箭头，不依赖中文/图标字库；44x44 触摸目标
     * 与时钟子页相同，位于左上角。 */
    s_motion_back = lv_button_create(s_motion_page);
    lv_obj_set_size(s_motion_back, 44, 44);
    lv_obj_set_pos(s_motion_back, 12, 8);
    lv_obj_set_style_bg_opa(s_motion_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_motion_back, 0, 0);
    lv_obj_set_style_shadow_width(s_motion_back, 0, 0);
    lv_obj_t *arrow = lv_label_create(s_motion_back);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(arrow, UI_COLOR_TEXT, 0);
    lv_obj_center(arrow);
    lv_obj_add_event_cb(s_motion_back, back_clicked_cb, LV_EVENT_CLICKED, NULL);

    create_row(s_motion_page, ROW_FIRST_Y, "计步", &s_motion_steps_switch, 0);
    create_row(s_motion_page, ROW_FIRST_Y + ROW_GAP, "抬手亮屏", &s_motion_wake_switch, 1);

    /* 创建时先按“未选中”初始化；main 在 ui_manager_init() 之后、同一
     * LVGL 锁内调用 ui_motion_settings_restore() 套用 NVS 中保存的选择。
     * 创建页面本身不发 I2C/Flash；传感器离线也不会擅自把用户开关改成关闭。 */
    s_motion_steps_enabled = false;
    s_motion_wake_enabled = false;
    s_motion_visible = false;
}

void ui_motion_settings_restore(bool steps, bool wake)
{
    /* 防御：页面尚未创建时没有可更新的对象；生产的开机顺序由 main 保证
     * （ui_manager_init() 之后立即调用），不会走到这个分支。 */
    if (!s_motion_page) return;
    s_motion_steps_enabled = steps;
    s_motion_wake_enabled = wake;
    /* 程序化设置选中状态不会触发 LV_EVENT_VALUE_CHANGED（该事件只由
     * 用户交互产生），因此不会重复进入 switch_changed_cb。开关旋钮的
     * 位置/过渡由 LVGL 主题根据状态自动处理，不改本页布局。 */
    if (steps) lv_obj_add_state(s_motion_steps_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(s_motion_steps_switch, LV_STATE_CHECKED);
    if (wake) lv_obj_add_state(s_motion_wake_switch, LV_STATE_CHECKED);
    else lv_obj_remove_state(s_motion_wake_switch, LV_STATE_CHECKED);
    /* 与用户关闭计步的行为一致：恢复为关闭时立即清空锁屏步数，不能在
     * 下次传感器轮询前显示上次运行的旧数字。 */
    if (!steps) ui_lockscreen_set_steps(false, 0);
    /* 传感器 worker 的启动晚于 UI（见 main），此调用只写原子 RAM 请求；
     * 没有 worker 时通知是空操作，worker 首次轮询会读取该请求并配置芯片。
     * 抬起亮屏同理：只有芯片成功配置后 motion_sensor_wake_enabled() 才为真。 */
    motion_sensor_set_features(steps, wake);
}

void ui_motion_settings_show(void)
{
    if (!s_motion_page || s_motion_visible) return;
    lv_anim_delete(s_motion_page, anim_x);
    /* 与时钟子页一致：每次从右侧边界重新开始，不依赖上次滑出后的位置。 */
    lv_obj_set_x(s_motion_page, UI_HOR_RES);
    lv_obj_remove_flag(s_motion_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_motion_page);
    s_motion_visible = true;

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_motion_page);
    lv_anim_set_values(&a, UI_HOR_RES, 0);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_start(&a);
}

void ui_motion_settings_hide(void)
{
    if (!s_motion_page || !s_motion_visible) return;
    slide_out();
}

void ui_motion_settings_close_for_lock(void)
{
    /* 锁屏只负责收起浮层；不保存、不等待任何后台操作。 */
    if (!s_motion_page || !s_motion_visible) return;
    slide_out();
}

bool ui_motion_settings_is_visible(void)
{
    return s_motion_visible;
}

bool ui_motion_settings_steps_enabled(void)
{
    return s_motion_steps_enabled;
}

bool ui_motion_settings_wake_enabled(void)
{
    return s_motion_wake_enabled;
}
