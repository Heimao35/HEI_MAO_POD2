/**
 * @file ui_bluetooth_settings.c
 * @brief 蓝牙设置子页：顶部开关 + 已配对设备卡片 + 可滚动扫描结果。
 *
 * 维护约定（与时钟/姿态感应子页保持一致）：
 * - 本页只是设置页的视觉子页，不建立新的屏幕/显示对象，不改变设置主页
 *   的两列网格、顶部展开和底部收起手势；
 * - 页面放在主屏幕内、初始位于屏幕右侧，点击设置项后从右侧滑入，
 *   左上角箭头从当前位置滑回右侧；关闭时暂停本页定时器并停止搜索；
 * - 本页不直接访问 SDIO、Bluetooth 协议栈、I2C 或 Flash：开关、扫描
 *   结果和连接状态都来自 bt_audio.h。用户操作也只向该后端投递请求；
 * - 蓝牙开关状态按要求不写 NVS，上电默认关闭；已配对设备与扫描结果都
 *   使用 18 号全字库，长名称用 LV_LABEL_LONG_DOT 省略，避免撑破卡片/行；
 * - 扫描结果由 250ms LVGL 定时器轮询后端快照，不回调、不在后台任务里
 *   直接操作 LVGL。页面隐藏后定时器暂停，不产生空转。
 */

#include "ui_bluetooth_settings.h"
#include "bt_audio.h"
#include "ui_fonts.h"
#include "ui_settings.h"

#include <stdio.h>
#include <string.h>

/* ============================ 页面尺寸 ============================ */
/* 页面从上到下：标题 → 蓝牙开关（置顶） → 已配对设备卡片 → 状态文字
 * → 扫描结果列表。为了让出已配对卡片的位置，蓝牙子页的开关比姿态
 * 感应子页缩小：轨道 110x56、行高 76；设置主页和姿态页的开关尺寸
 * 不受影响。 */
#define SWITCH_ROW_W   380
#define SWITCH_ROW_H   76
#define SWITCH_ROW_X   ((UI_HOR_RES - SWITCH_ROW_W) / 2)
#define SWITCH_ROW_Y   60
#define SWITCH_W       110
#define SWITCH_H       56

#define PAIRED_ROW_X   40
#define PAIRED_ROW_Y   144
#define PAIRED_ROW_W   380
#define PAIRED_ROW_H   64

/* 状态文字在已配对卡片下方，18 号字（会拼接设备名），居中；
 * 扫描结果列表从状态文字下面开始。 */
#define STATUS_Y       212
#define DEVICE_LIST_X         30
#define DEVICE_LIST_Y         238
#define DEVICE_LIST_W         (UI_HOR_RES - DEVICE_LIST_X * 2)
#define DEVICE_LIST_H         (UI_VER_RES - DEVICE_LIST_Y - 10)

/* 单行设备条目高度与行距；列表可上下滚动，滚动条隐藏以保持深色版面。 */
#define DEVICE_ROW_H   64
#define DEVICE_ROW_GAP 8

/* 页面最多显示多少个扫描结果。后端上限为 BT_AUDIO_MAX_DEVICES(16)，
 * 这里与之相同；超出的结果不创建对象，避免无限增长。 */
#define BT_UI_ROWS     BT_AUDIO_MAX_DEVICES

/** 连接请求失败等临时提示的显示时长（毫秒）。 */
#define NOTICE_MS      2000

/* ============================ 页面对象 ============================ */
static lv_obj_t *s_page;                 /**< 全屏子页 */
static lv_obj_t *s_back;                 /**< 左上返回按钮 */
static lv_obj_t *s_paired_row;           /**< 顶部已配对设备卡片 */
static lv_obj_t *s_paired_icon;          /**< 卡片左侧蓝牙图标 */
static lv_obj_t *s_paired_label;         /**< 已配对设备名称/占位文字 */
static lv_obj_t *s_power_switch;         /**< 蓝牙电源开关 */
static lv_obj_t *s_status;               /**< 连接/就绪状态文字 */
static lv_obj_t *s_list;                 /**< 可滚动设备列表容器 */
static lv_obj_t *s_empty;                /**< 列表为空时的提示文字 */
static lv_timer_t *s_timer;              /**< 250ms 状态/结果轮询 */
static bool s_visible;                   /**< 页面是否处于可见（含滑入中） */

/* 顶部卡片缓存最近一次从 bt_audio 读到的配对设备。后端只保存一台，
 * 因此这里不需要列表；刷新时只在名称/地址变化后调用 set_text。 */
static bool s_paired_valid;
static uint8_t s_paired_bda[6];
static char s_paired_display[BT_AUDIO_NAME_MAX + 16];

/* 设备行池。LVGL 对象只在第一次需要时创建，之后只改文字/BDA/位置，
 * 不随每次扫描重建，避免拖动列表时对象被删除导致触摸丢失。 */
typedef struct {
    lv_obj_t *row;                       /**< 可点击的整行容器 */
    lv_obj_t *label;                     /**< 18 号设备名 */
    uint8_t bda[6];                      /**< 该行当前对应的设备地址 */
    char name[BT_AUDIO_NAME_MAX];        /**< 当前显示文本，用于避免重复 set_text */
} bt_ui_row_t;

static bt_ui_row_t s_rows[BT_UI_ROWS];

/* 临时提示：例如连接请求被后端拒绝时，覆盖普通状态文字显示 2 秒。
 * 使用 lv_tick_get()/lv_tick_elaps()，不额外创建定时器。 */
static char s_notice[64];
static uint32_t s_notice_tick;

/* 完整刷新在文件后部定义；顶部卡片点击回调会先调用它。 */
static void refresh_all(void);

/* ============================ 动画与返回 ============================ */

/** @brief 子页 X 位移动画执行回调；只改位置，不改布局。 */
static void anim_x(void *obj, int32_t x)
{
    lv_obj_set_x((lv_obj_t *)obj, x);
}

/** @brief 滑出完成：隐藏子页并清可见标志。 */
static void hidden(lv_anim_t *anim)
{
    (void)anim;
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    s_visible = false;
    /* 子页开关可能改变了蓝牙电源意图。退出动画结束后设置主页重新
     * 露出，此时同步蓝牙入口按钮的反白/深灰外观；只改颜色，不改
     * 设置主页布局或动画。 */
    ui_settings_refresh_bluetooth();
}

/**
 * @brief 从当前 X 位置滑出到屏幕右侧。
 *
 * 与时钟/姿态子页相同：不把起点写死为 0，用户在滑入途中点击返回时，
 * 动画才能从实际位置连续退出。350ms、ease-out、UI_ANIM_PATH 均复用
 * UI 公共参数，不引入新的动画曲线。
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

/* 行点击回调在行对象创建函数之后定义，这里先声明，供创建时注册。 */
static void device_clicked_cb(lv_event_t *event);

/* ============================ 状态刷新 ============================ */

/** @brief 设置一条覆盖普通状态的临时提示，2 秒后自动恢复。 */
static void set_notice(const char *text)
{
    if (!text) text = "";
    snprintf(s_notice, sizeof(s_notice), "%s", text);
    s_notice_tick = lv_tick_get();
}

/**
 * @brief 根据后端状态更新开关和状态文字。
 *
 * 状态文字优先级：临时提示 > 已关闭 > 链路未就绪 > 具体状态。
 * “链路未就绪”必须单独显示：开关可以打开，但 U4/SDIO 还没握手，
 * 此时绝不能显示“未连接”或“搜索中”来暗示蓝牙已经可用。
 */
static void refresh_status(void)
{
    /* 开关位置始终跟随用户意图。程序化 add/remove_state 不会触发
     * LV_EVENT_VALUE_CHANGED，因此不会反向调用开关回调。 */
    bool on = bt_audio_is_enabled();
    if (on) lv_obj_add_state(s_power_switch, LV_STATE_CHECKED);
    else    lv_obj_remove_state(s_power_switch, LV_STATE_CHECKED);

    const char *text = "";
    bool highlight = false; /* true 时用白色强调已连接 */
    /* 名称最长 BT_AUDIO_NAME_MAX-1 字节，加上“已连接：”前缀；缓冲需
     * 大于临时提示，避免长中文设备名被截断。 */
    char buf[BT_AUDIO_NAME_MAX + 16];

    if (s_notice[0] && lv_tick_elaps(s_notice_tick) < NOTICE_MS) {
        /* 临时提示保留，不覆盖开关位置。 */
        text = s_notice;
    } else if (s_notice[0]) {
        /* 过期后清空，后续正常刷新不再命中。 */
        s_notice[0] = '\0';
    }

    if (!text[0]) {
        bt_audio_state_t state = bt_audio_get_state();
        if (state == BT_AUDIO_STATE_OFF) {
            text = "蓝牙已关闭";
        } else if (!bt_audio_link_ready()) {
            /* 用户已打开，但 P4 与 U4 的 SDIO 链路尚未握手。 */
            text = "蓝牙协处理器未就绪";
        } else {
            switch (state) {
            case BT_AUDIO_STATE_CONNECTED: {
                /* 名称由链路 worker 更新，必须复制到本地再格式化：
                 * 直接持有 bt_audio 内部指针可能在 250ms 轮询期间被改写。 */
                char name[BT_AUDIO_NAME_MAX];
                bt_audio_get_connected_name(name, sizeof(name));
                snprintf(buf, sizeof(buf), "已连接：%s",
                         name[0] ? name : "蓝牙设备");
                text = buf;
                highlight = true;
                break;
            }
            case BT_AUDIO_STATE_CONNECTING:
                text = "正在连接…";
                break;
            case BT_AUDIO_STATE_SCANNING:
                text = "正在搜索附近设备…";
                break;
            case BT_AUDIO_STATE_ERROR:
                text = "蓝牙连接异常";
                break;
            case BT_AUDIO_STATE_ON:
            default:
                text = "已开启，未连接";
                break;
            }
        }
    }

    lv_label_set_text(s_status, text);
    lv_obj_set_style_text_color(s_status,
                                highlight ? UI_COLOR_TEXT : UI_COLOR_TEXT_SECONDARY, 0);
}

/**
 * @brief 列表为空时的提示语；有待显示设备时隐藏提示。
 *
 * 提示语区分“没打开/链路未就绪/正在搜索/确实没找到”，避免把不同原因
 * 都显示成“未找到设备”。
 */
static void refresh_empty_hint(size_t device_count)
{
    if (device_count > 0) {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    bt_audio_state_t state = bt_audio_get_state();
    /* 已连接/正在连接时列表为空是正常的（A2DP Source 此时不扫描），
     * 不显示“未发现设备”以免与上方“已连接”状态矛盾。 */
    if (state == BT_AUDIO_STATE_CONNECTED || state == BT_AUDIO_STATE_CONNECTING) {
        lv_obj_add_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
        return;
    }

    const char *hint;
    if (state == BT_AUDIO_STATE_OFF) hint = "蓝牙已关闭，打开开关后开始搜索";
    else if (!bt_audio_link_ready()) hint = "蓝牙链路未就绪";
    else if (state == BT_AUDIO_STATE_SCANNING) hint = "正在搜索附近的设备…";
    else hint = "未发现蓝牙设备";

    lv_label_set_text(s_empty, hint);
    lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_HIDDEN);
    /* 列表可能刚被用户滚到底后结果被清空；回到顶部，避免提示文字
     * 停在可视区之外看起来像“空白页”。 */
    lv_obj_scroll_to_y(s_list, 0, LV_ANIM_OFF);
}

/**
 * @brief 第一次需要显示某一行时创建 LVGL 对象。
 *
 * 行容器是唯一可点击对象；内部文字不接触摸，拖动仍交给列表容器滚动。
 * 行对象创建在列表内、使用绝对位置（不启用 flex），因此增删结果不会
 * 重新排列已有行，也不会改变行高/字号。
 */
static void create_row_objects(unsigned index)
{
    bt_ui_row_t *item = &s_rows[index];

    item->row = lv_obj_create(s_list);
    lv_obj_set_size(item->row, DEVICE_LIST_W, DEVICE_ROW_H);
    lv_obj_set_pos(item->row, 0, (int32_t)index * (DEVICE_ROW_H + DEVICE_ROW_GAP));
    lv_obj_set_style_bg_color(item->row, UI_COLOR_LIST_ROW_BG, 0);
    lv_obj_set_style_bg_opa(item->row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(item->row, 0, 0);
    lv_obj_set_style_radius(item->row, 12, 0);
    lv_obj_set_style_pad_all(item->row, 0, 0);
    lv_obj_remove_flag(item->row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(item->row, LV_OBJ_FLAG_CLICKABLE);
    /* 用户数据保存行号而不是 BDA 指针：行池是静态数组，回调发生时
     * 仍有效；重新扫描只更新同一行的 bda 内容，不重新注册事件。 */
    lv_obj_add_event_cb(item->row, device_clicked_cb, LV_EVENT_CLICKED,
                        (void *)(uintptr_t)index);

    /* 18 号全字库：覆盖常见文字和符号，也是用户明确要求的设备名
     * 字号。label 宽度固定为行宽减左右 16px，长名称用 DOT 省略，
     * 不允许撑宽行或覆盖滚动条区域。 */
    item->label = lv_label_create(item->row);
    lv_obj_set_style_text_font(item->label, ui_font_18(), 0);
    lv_obj_set_style_text_color(item->label, UI_COLOR_TEXT, 0);
    lv_label_set_long_mode(item->label, LV_LABEL_LONG_DOT);
    lv_obj_set_width(item->label, DEVICE_LIST_W - 32);
    lv_obj_align(item->label, LV_ALIGN_LEFT_MID, 16, 0);
    lv_label_set_text(item->label, "");
    lv_obj_remove_flag(item->label, LV_OBJ_FLAG_CLICKABLE);

    item->bda[0] = 0;
    item->name[0] = '\0';
    lv_obj_add_flag(item->row, LV_OBJ_FLAG_HIDDEN);
}

/**
 * @brief 发起连接并显示失败原因（设备列表行和已配对卡片共用）。
 *
 * 只投递异步请求；真正的 U4 时序由 bt_audio/bt_link 处理。错误提示
 * 只在这里转换成用户可读文字，不阻塞触摸回调。
 */
static void request_connect(const uint8_t bda[6])
{
    /* 已连接或正在连接时不再重复投递，避免把“已有请求”误报成未就绪。 */
    bt_audio_state_t state = bt_audio_get_state();
    if (state == BT_AUDIO_STATE_CONNECTED || state == BT_AUDIO_STATE_CONNECTING)
        return;

    esp_err_t err = bt_audio_connect(bda);
    if (err == ESP_ERR_INVALID_STATE) {
        /* 未打开或链路未就绪：明确告诉用户，而不是静默失败。 */
        set_notice("蓝牙未就绪，无法连接");
    } else if (err == ESP_ERR_NOT_SUPPORTED) {
        set_notice("蓝牙链路尚未接入");
    } else if (err != ESP_OK) {
        set_notice("连接请求失败");
    } else {
        /* 后端已接受请求：清除可能残留的旧错误提示，让状态机随后的
         * CONNECTING/CONNECTED 立即显示。这里不能再设 2 秒提示，
         * 否则会遮住刚刚建立的“已连接”状态。 */
        s_notice[0] = '\0';
    }
    refresh_status();
}

/**
 * @brief 点击设备行：把地址交给共享连接请求函数。
 *
 * 本回调只发请求，不阻塞、不等待 U4 应答；结果由 250ms 定时器从后端
 * 状态机读取。地址从行池复制，不保存 U4 缓冲区指针。
 */
static void device_clicked_cb(lv_event_t *event)
{
    unsigned index = (unsigned)(uintptr_t)lv_event_get_user_data(event);
    if (index >= BT_UI_ROWS || !s_rows[index].row) return;
    request_connect(s_rows[index].bda);
}

/**
 * @brief 刷新顶部“已配对设备”卡片。
 *
 * 后端只保留最近一次成功配对的设备。有记录时显示名称；名称为空时
 * 退回显示 BDA；没有记录时显示占位文字并变灰。点击卡片的动作由
 * paired_clicked_cb() 处理，本节只更新文字/颜色，不改变布局。
 */
static void refresh_paired(void)
{
    if (!s_paired_label) return;

    bool valid = false;
    uint8_t bda[6] = {0};
    char name[BT_AUDIO_NAME_MAX] = {0};
    valid = bt_audio_get_paired(bda, name, sizeof(name));

    char display[sizeof(s_paired_display)];
    if (valid) {
        if (name[0]) {
            snprintf(display, sizeof(display), "%s", name);
        } else {
            snprintf(display, sizeof(display),
                     "%02X:%02X:%02X:%02X:%02X:%02X",
                     bda[0], bda[1], bda[2], bda[3], bda[4], bda[5]);
        }
    } else {
        snprintf(display, sizeof(display), "暂无已配对设备");
    }

    if (strcmp(display, s_paired_display) != 0) {
        snprintf(s_paired_display, sizeof(s_paired_display), "%s", display);
        lv_label_set_text(s_paired_label, s_paired_display);
    }

    if (valid != s_paired_valid ||
        (valid && memcmp(bda, s_paired_bda, sizeof(bda)) != 0)) {
        s_paired_valid = valid;
        if (valid) memcpy(s_paired_bda, bda, sizeof(bda));
        else memset(s_paired_bda, 0, sizeof(s_paired_bda));
    }

    /* 有记录时白色表示可操作；无记录时灰色，点击回调也会直接返回。 */
    lv_obj_set_style_text_color(s_paired_label,
                                valid ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_color(s_paired_icon,
                                valid ? UI_COLOR_TEXT : UI_COLOR_TEXT_MUTED, 0);
}

/**
 * @brief 点击已配对设备：重新连接这一台，或在蓝牙关闭时先打开蓝牙。
 *
 * - 蓝牙关闭：只打开开关；链路就绪后由 bt_audio 的自动重连逻辑连接
 *   已配对设备，不在 UI 里等待硬件，也不启动扫描。
 * - 已连接：无需重复请求，直接返回。
 * - 已打开但未连接：发送 CONNECT；若链路未就绪，request_connect()
 *   会显示现有提示。
 */
static void paired_clicked_cb(lv_event_t *event)
{
    (void)event;
    if (!s_paired_valid) return;

    if (!bt_audio_is_enabled()) {
        bt_audio_set_enabled(true);
        set_notice("正在打开蓝牙…");
        refresh_all();
        return;
    }
    if (bt_audio_is_connected()) return;
    request_connect(s_paired_bda);
}

/**
 * @brief 从后端复制扫描结果并刷新行池。
 *
 * 只在地址或名称变化时调用 lv_label_set_text；扫描刷新每 250ms 一次，
 * 相同结果不产生多余布局/重绘。设备数减少时隐藏多余行，不删除对象，
 * 下次扫描增多可直接复用。
 *
 * 后端可能返回空名称的设备；UI 显示“(未知设备)”，不因名称为空而
 * 跳过设备，用户仍可按地址配对。
 */
static void refresh_devices(void)
{
    bt_audio_device_t devices[BT_AUDIO_MAX_DEVICES];
    /* 关闭或链路未就绪时一律不显示旧结果：后端可能还没来得及清空
     * 自己的数组，UI 不能把上一轮扫描的名称继续挂在页面上误导用户。 */
    size_t count = 0;
    if (bt_audio_is_enabled() && bt_audio_link_ready())
        count = bt_audio_get_devices(devices, BT_AUDIO_MAX_DEVICES);
    if (count > BT_UI_ROWS) count = BT_UI_ROWS;

    for (size_t i = 0; i < count; ++i) {
        bt_ui_row_t *item = &s_rows[i];
        if (!item->row) create_row_objects((unsigned)i);

        const char *display = devices[i].name[0] ? devices[i].name : "(未知设备)";
        if (memcmp(item->bda, devices[i].bda, sizeof(item->bda)) != 0 ||
            strcmp(item->name, display) != 0) {
            memcpy(item->bda, devices[i].bda, sizeof(item->bda));
            snprintf(item->name, sizeof(item->name), "%s", display);
            lv_label_set_text(item->label, item->name);
        }
        lv_obj_remove_flag(item->row, LV_OBJ_FLAG_HIDDEN);
    }

    for (size_t i = count; i < BT_UI_ROWS; ++i) {
        if (s_rows[i].row) lv_obj_add_flag(s_rows[i].row, LV_OBJ_FLAG_HIDDEN);
    }

    refresh_empty_hint(count);
}

/** @brief 后端状态与扫描结果的一次完整刷新。 */
static void refresh_all(void)
{
    refresh_paired();
    refresh_status();
    refresh_devices();
}

/** @brief 250ms 页面定时器：只读 RAM 状态，不阻塞 SDIO/协议栈。 */
static void page_timer_cb(lv_timer_t *timer)
{
    (void)timer;
    refresh_all();
}

/* ============================ 用户交互 ============================ */

/**
 * @brief 电源开关变化：记录开关意图并立即开始/停止搜索。
 *
 * 开关状态不写 NVS（按需求上电默认关闭）。后端只发请求，真正的
 * U4 启动/关闭由链路 worker 异步执行；这里不等待硬件，也不改状态栏。
 */
static void power_switch_changed_cb(lv_event_t *event)
{
    lv_obj_t *sw = lv_event_get_target(event);
    bool on = lv_obj_has_state(sw, LV_STATE_CHECKED);

    bt_audio_set_enabled(on);
    if (on) bt_audio_scan_start();
    else    bt_audio_scan_stop();

    /* 关闭时后端已清空设备/连接状态；打开且链路就绪时搜索需要时间，
     * 先刷新一次让开关和提示立即响应，后续由定时器继续更新。 */
    refresh_all();
}

/** @brief 左上返回：停止搜索并滑出；不修改设置主页和状态栏。 */
static void back_clicked_cb(lv_event_t *event)
{
    (void)event;
    ui_bluetooth_settings_hide();
}

/* ============================ 页面构建 ============================ */

/**
 * @brief 创建“左侧文字 + 右侧开关”的电源行。
 *
 * 颜色/圆角与姿态感应子页一致，但轨道缩小为 110x56、行高 76，给下方
 * 的已配对设备卡片留空间。开关由 bt_audio 状态驱动，不参与 NVS 快照。
 */
static void create_power_row(lv_obj_t *parent)
{
    lv_obj_t *row = lv_obj_create(parent);
    lv_obj_set_size(row, SWITCH_ROW_W, SWITCH_ROW_H);
    lv_obj_set_pos(row, SWITCH_ROW_X, SWITCH_ROW_Y);
    lv_obj_set_style_bg_opa(row, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *label = lv_label_create(row);
    lv_label_set_text(label, "蓝牙");
    lv_obj_set_style_text_font(label, ui_font_22(), 0);
    lv_obj_set_style_text_color(label, UI_COLOR_TEXT, 0);
    lv_obj_align(label, LV_ALIGN_LEFT_MID, 12, 0);

    s_power_switch = lv_switch_create(row);
    lv_obj_set_size(s_power_switch, SWITCH_W, SWITCH_H);
    lv_obj_align(s_power_switch, LV_ALIGN_RIGHT_MID, -12, 0);
    lv_obj_set_style_bg_color(s_power_switch, UI_COLOR_CONTROL_BG, LV_PART_MAIN);
    lv_obj_set_style_bg_opa(s_power_switch, LV_OPA_COVER, LV_PART_MAIN);
    lv_obj_set_style_radius(s_power_switch, LV_RADIUS_CIRCLE, LV_PART_MAIN);
    lv_obj_set_style_pad_all(s_power_switch, 0, LV_PART_MAIN);
    /* 关闭：不画选中指示层，露出深灰轨道；打开：白色铺满。 */
    lv_obj_set_style_bg_opa(s_power_switch, LV_OPA_TRANSP, LV_PART_INDICATOR);
    lv_obj_set_style_bg_color(s_power_switch, UI_COLOR_TEXT,
                              LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_bg_opa(s_power_switch, LV_OPA_COVER,
                            LV_PART_INDICATOR | LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_power_switch, LV_RADIUS_CIRCLE, LV_PART_INDICATOR);
    /* 旋钮在两种轨道上都保持对比。 */
    lv_obj_set_style_bg_color(s_power_switch, UI_COLOR_TEXT, LV_PART_KNOB);
    lv_obj_set_style_bg_color(s_power_switch, UI_COLOR_PAGE_BG_DARK,
                              LV_PART_KNOB | LV_STATE_CHECKED);
    lv_obj_set_style_radius(s_power_switch, LV_RADIUS_CIRCLE, LV_PART_KNOB);
    lv_obj_set_style_shadow_width(s_power_switch, 0, LV_PART_KNOB);
    /* 创建时默认关闭；refresh_status() 会按后端状态再同步一次。 */
    lv_obj_add_event_cb(s_power_switch, power_switch_changed_cb,
                        LV_EVENT_VALUE_CHANGED, NULL);
}

/**
 * @brief 创建顶部“已配对设备”卡片。
 *
 * 位置在蓝牙开关下方、扫描列表上方；后端只保存一台设备，所以只做
 * 一个可点击行，不建列表。名称使用 18 号全字库（设备名可能包含任意
 * 中文/符号），无记录时显示灰色占位文字并禁用点击效果。
 *
 * 卡片本身是点击目标；内部图标/文字不接触摸。点击回调只投递重连
 * 请求或打开蓝牙，不阻塞 UI。
 */
static void create_paired_row(lv_obj_t *parent)
{
    s_paired_row = lv_obj_create(parent);
    lv_obj_set_size(s_paired_row, PAIRED_ROW_W, PAIRED_ROW_H);
    lv_obj_set_pos(s_paired_row, PAIRED_ROW_X, PAIRED_ROW_Y);
    lv_obj_set_style_bg_color(s_paired_row, UI_COLOR_LIST_ROW_BG, 0);
    lv_obj_set_style_bg_opa(s_paired_row, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_paired_row, 0, 0);
    lv_obj_set_style_radius(s_paired_row, 12, 0);
    lv_obj_set_style_pad_all(s_paired_row, 0, 0);
    lv_obj_remove_flag(s_paired_row, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_paired_row, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(s_paired_row, paired_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* 内置 Montserrat 的蓝牙符号，不依赖自制字库。 */
    s_paired_icon = lv_label_create(s_paired_row);
    lv_label_set_text(s_paired_icon, LV_SYMBOL_BLUETOOTH);
    lv_obj_set_style_text_font(s_paired_icon, &lv_font_montserrat_20, 0);
    lv_obj_set_style_text_color(s_paired_icon, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_align(s_paired_icon, LV_ALIGN_LEFT_MID, 16, 0);

    s_paired_label = lv_label_create(s_paired_row);
    lv_obj_set_width(s_paired_label, PAIRED_ROW_W - 52 - 16);
    lv_obj_set_style_text_font(s_paired_label, ui_font_18(), 0);
    lv_obj_set_style_text_color(s_paired_label, UI_COLOR_TEXT_MUTED, 0);
    lv_label_set_long_mode(s_paired_label, LV_LABEL_LONG_DOT);
    lv_label_set_text(s_paired_label, "暂无已配对设备");
    lv_obj_align(s_paired_label, LV_ALIGN_LEFT_MID, 52, 0);
    lv_obj_remove_flag(s_paired_label, LV_OBJ_FLAG_CLICKABLE);

    /* main 已在 UI 创建前从 NVS 恢复配对记录；这里立即套用一次，
     * 不等待页面首次打开或 250ms 定时器。 */
    refresh_paired();
}

/**
 * @brief 创建设备列表容器。
 *
 * 只允许垂直滚动，滚动条关闭以保持深色页面干净；列表不把滚动链传给
 * 下方设置主页，避免在列表到顶/到底后继续拖动而收起设置页。
 */
static void create_device_list(lv_obj_t *parent)
{
    s_list = lv_obj_create(parent);
    lv_obj_set_size(s_list, DEVICE_LIST_W, DEVICE_LIST_H);
    lv_obj_set_pos(s_list, DEVICE_LIST_X, DEVICE_LIST_Y);
    lv_obj_set_style_bg_opa(s_list, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_list, 0, 0);
    lv_obj_set_style_pad_all(s_list, 0, 0);
    lv_obj_add_flag(s_list, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_scroll_dir(s_list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(s_list, LV_SCROLLBAR_MODE_OFF);
    lv_obj_remove_flag(s_list, LV_OBJ_FLAG_SCROLL_CHAIN_VER |
                               LV_OBJ_FLAG_SCROLL_CHAIN_HOR);

    /* 空列表提示：不拦截触摸，不参与行池布局；有设备时隐藏。
     * 宽度固定，文字自动换行，行距/字号与列表状态文字区分。 */
    s_empty = lv_label_create(s_list);
    lv_obj_set_width(s_empty, DEVICE_LIST_W - 40);
    lv_obj_set_style_text_font(s_empty, ui_font_16(), 0);
    lv_obj_set_style_text_color(s_empty, UI_COLOR_TEXT_MUTED, 0);
    lv_obj_set_style_text_align(s_empty, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(s_empty, LV_LABEL_LONG_WRAP);
    lv_label_set_text(s_empty, "蓝牙已关闭，打开开关后开始搜索");
    lv_obj_align(s_empty, LV_ALIGN_TOP_MID, 0, 80);
    lv_obj_remove_flag(s_empty, LV_OBJ_FLAG_CLICKABLE);
}

void ui_bluetooth_settings_create(lv_obj_t *parent)
{
    /* 全屏子页与时钟/姿态子页同层：覆盖在设置主页上方，但仍是主屏幕
     * 的子对象。初始放在屏幕右侧并隐藏，show() 只做位移动画。 */
    s_page = lv_obj_create(parent);
    lv_obj_set_size(s_page, UI_HOR_RES, UI_VER_RES);
    lv_obj_set_pos(s_page, UI_HOR_RES, 0);
    lv_obj_set_style_bg_color(s_page, UI_COLOR_PAGE_BG_DARK, 0);
    lv_obj_set_style_bg_opa(s_page, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_page, 0, 0);
    lv_obj_set_style_radius(s_page, 0, 0);
    lv_obj_set_style_pad_all(s_page, 0, 0);
    /* 不向外冒泡事件/手势：返回动画或列表拖动不能触发设置页收起、
     * 锁屏解锁或播放器切歌。 */
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_SCROLLABLE |
                               LV_OBJ_FLAG_EVENT_BUBBLE |
                               LV_OBJ_FLAG_GESTURE_BUBBLE);
    lv_obj_add_flag(s_page, LV_OBJ_FLAG_HIDDEN);

    /* 标题与时钟/姿态子页保持相同字号、颜色和顶部位置。 */
    lv_obj_t *title = lv_label_create(s_page);
    lv_label_set_text(title, "蓝牙");
    lv_obj_set_style_text_font(title, ui_font_22(), 0);
    lv_obj_set_style_text_color(title, UI_COLOR_TEXT, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 16);

    /* 返回键只画 LVGL 自带左箭头，44x44 触摸目标，与另外两个子页一致。 */
    s_back = lv_button_create(s_page);
    lv_obj_set_size(s_back, 44, 44);
    lv_obj_set_pos(s_back, 12, 8);
    lv_obj_set_style_bg_opa(s_back, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(s_back, 0, 0);
    lv_obj_set_style_shadow_width(s_back, 0, 0);
    lv_obj_t *arrow = lv_label_create(s_back);
    lv_label_set_text(arrow, LV_SYMBOL_LEFT);
    lv_obj_set_style_text_font(arrow, &lv_font_montserrat_26, 0);
    lv_obj_set_style_text_color(arrow, UI_COLOR_TEXT, 0);
    lv_obj_center(arrow);
    lv_obj_add_event_cb(s_back, back_clicked_cb, LV_EVENT_CLICKED, NULL);

    /* 页面纵向顺序：开关置顶 → 已配对设备卡片 → 状态文字 → 扫描列表。
     * 全部使用绝对坐标，不启用 flex，避免扫描增删结果时移动上面的卡片。 */
    create_power_row(s_page);
    create_paired_row(s_page);

    /* 状态文字：连接/搜索/未就绪都只在这里显示，不改变状态栏。
     * 连接成功后文字转白，与“未连接”等次要信息区分。 */
    s_status = lv_label_create(s_page);
    lv_obj_set_width(s_status, DEVICE_LIST_W);
    /* 固定单行宽度：过长的“已连接：设备名”用省略号收尾，不换行
     * 覆盖下方列表，也不改变列表起始位置。
     * 状态行使用 18 号全字库而不是 16 号子集：连接后这里会拼接扫描
     * 到的设备名，设备名可能包含任意中文/符号，只有 18 号全字库能
     * 保证不出现乱码或缺字。18px 行高仍在列表上边界之内。 */
    lv_label_set_long_mode(s_status, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_font(s_status, ui_font_18(), 0);
    lv_obj_set_style_text_color(s_status, UI_COLOR_TEXT_SECONDARY, 0);
    lv_obj_set_style_text_align(s_status, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_status, "蓝牙已关闭");
    lv_obj_set_pos(s_status, DEVICE_LIST_X, STATUS_Y);

    create_device_list(s_page);

    /* 创建时不读取 NVS、不启动扫描：上电默认关闭。250ms 定时器只在
     * 页面可见期间运行，隐藏后暂停，避免无意义的后台轮询。 */
    s_visible = false;
    s_notice[0] = '\0';
    s_timer = lv_timer_create(page_timer_cb, 250, NULL);
    lv_timer_pause(s_timer);
}

void ui_bluetooth_settings_show(void)
{
    if (!s_page || s_visible) return;

    lv_anim_delete(s_page, anim_x);
    /* 与另外两个子页一致：每次从右侧边界重新开始。 */
    lv_obj_set_x(s_page, UI_HOR_RES);
    lv_obj_remove_flag(s_page, LV_OBJ_FLAG_HIDDEN);
    lv_obj_move_foreground(s_page);
    s_visible = true;

    /* 进入页面即请求搜索。后端只在“已打开且链路就绪”时真正进入
     * SCANNING；未打开时保持关闭，空列表会提示用户先打开开关。 */
    s_notice[0] = '\0';
    bt_audio_scan_start();
    refresh_all();
    lv_timer_resume(s_timer);

    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, s_page);
    lv_anim_set_values(&a, UI_HOR_RES, 0);
    lv_anim_set_duration(&a, UI_ANIM_DURATION_MS);
    lv_anim_set_path_cb(&a, UI_ANIM_PATH);
    lv_anim_set_exec_cb(&a, anim_x);
    lv_anim_start(&a);
}

void ui_bluetooth_settings_hide(void)
{
    if (!s_page || !s_visible) return;
    /* 先停搜索和轮询，再播退出动画；已建立的 A2DP 连接不因离开页面
     * 断开，音乐/视频可以继续从蓝牙输出。 */
    lv_timer_pause(s_timer);
    bt_audio_scan_stop();
    slide_out();
}

void ui_bluetooth_settings_close_for_lock(void)
{
    /* 锁屏只收起浮层并停止搜索；不改变蓝牙电源意图，也不等待后台。 */
    if (!s_page || !s_visible) return;
    lv_timer_pause(s_timer);
    bt_audio_scan_stop();
    slide_out();
}

bool ui_bluetooth_settings_is_visible(void)
{
    return s_visible;
}
