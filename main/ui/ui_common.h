/**
 * @file ui_common.h
 * @brief UI 公共定义 —— 屏幕尺寸、颜色主题、动画参数、页面枚举、全局状态结构体
 *
 * 所有 UI 模块共享此头文件，修改此处的数值会影响全局 UI 外观与行为。
 */

#ifndef UI_COMMON_H
#define UI_COMMON_H

#include "esp_lvgl_port.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

/* ======================== 屏幕尺寸 ======================== */
#define UI_HOR_RES  460   /**< 屏幕水平分辨率 (像素) */
#define UI_VER_RES  460   /**< 屏幕垂直分辨率 (像素) */

/* ======================== 动画参数 ======================== */
/** 页面切换 / 面板展开收起的默认动画时长 (ms)，增大使动画更慢 */
#define UI_ANIM_DURATION_MS     350
/** 快速动画时长 (ms)，用于小幅度过渡如图标淡入淡出 */
#define UI_ANIM_FAST_MS         200
/** 动画运动曲线：渐慢曲线 (ease-out)，使运动在结束时减速，视觉上更灵动 */
#define UI_ANIM_PATH            lv_anim_path_ease_out

/* ======================== 颜色主题 ======================== */
/* 全系统使用白色主色与中性黑灰，颜色只在此定义，避免页面/交互状态
 * 各自残留蓝紫色。只调整 RGB，不改变尺寸、透明度或动画。
 * 白色实心选中背景必须搭配 ON_ACCENT 黑字；浅色阅读页使用独立深色前景。
 * 歌曲封面、模糊封面、图片/视频内容不属于 UI 调色板，不做去色。 */
#define UI_COLOR_TEXT           lv_color_white()
#define UI_COLOR_TEXT_SECONDARY lv_color_hex(0xCCCCCC)
#define UI_COLOR_TEXT_MUTED     lv_color_hex(0x888888)
#define UI_COLOR_TEXT_CAPTION   lv_color_hex(0xAAAAAA)
#define UI_COLOR_PAGE_BG        lv_color_hex(0x141414)
#define UI_COLOR_PAGE_BG_DARK   lv_color_hex(0x101010)
#define UI_COLOR_CONTROL_BG     lv_color_hex(0x303030)
#define UI_COLOR_LIST_ROW_BG    lv_color_hex(0x262626)
#define UI_COLOR_ACCENT         lv_color_white()
#define UI_COLOR_ON_ACCENT      lv_color_black()
#define UI_COLOR_READING_BG     lv_color_white()
#define UI_COLOR_READING_INK    lv_color_hex(0x333333)
/** 状态栏白色文字，搭配原有半透明黑色背景。 */
#define UI_COLOR_STATUS_TEXT    UI_COLOR_TEXT
/** 充电状态保留金色，作为唯一的设备状态提示色。 */
#define UI_COLOR_CHARGE        lv_color_hex(0xFFD700)
/** 面板保持原有不透明度，仅将偏蓝背景改为中性深灰。 */
#define UI_PANEL_BG_OPA         (LV_OPA_70)
#define UI_PANEL_BG_COLOR       lv_color_hex(0x1A1A1A)
#define UI_COLOR_SLIDER_ACTIVE  UI_COLOR_ACCENT
#define UI_COLOR_SLIDER_BG      lv_color_hex(0x484848)
/** 按键/卡片按下时的中性提亮色；与白色描边配合，不改变布局。 */
#define UI_COLOR_PRESSED_BG     lv_color_hex(0x3A3A3A)
/**
 * 图片/电子书“首页”卡片的不透明度。
 *
 * 卡片底色仍是 UI_COLOR_LIST_ROW_BG，但允许用户壁纸从卡片后面透出：
 * 70% 不透明度在深色照片上仍能保证 22px 白色标题清晰，同时能明显看到
 * 背景图；调整此值即可整体改变两处首页卡片的通透程度。只影响首页
 * 卡片，不改变总览/书架/阅读页和播放器的既有背景。
 */
#define UI_HOME_CARD_BG_OPA     LV_OPA_70

/**
 * @brief 为可点击对象添加统一按下反馈：中性提亮 + 2px 白色描边。
 *
 * 只写 PRESSED 状态样式，不修改尺寸、位置、圆角或正常状态颜色；
 * 释放或 LV_EVENT_PRESS_LOST 后 LVGL 自动恢复。用于模式按钮、设置
 * 按钮、声光按钮、视频预览 tile 以及关机/配对确认弹窗按键。
 */
static inline void ui_apply_pressed_style(lv_obj_t *obj)
{
    if (!obj) return;
    lv_obj_set_style_bg_color(obj, UI_COLOR_PRESSED_BG, LV_STATE_PRESSED);
    /* 弹窗按键常态是 20% 白色半透明，若按住时继续半透明，提亮色几乎
     * 不可见；按下瞬间改为不透明填充。其它控件常态本就不透明，此行
     * 不改变它们的外观。 */
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, LV_STATE_PRESSED);
    lv_obj_set_style_border_width(obj, 2, LV_STATE_PRESSED);
    lv_obj_set_style_border_color(obj, UI_COLOR_TEXT, LV_STATE_PRESSED);
    lv_obj_set_style_border_opa(obj, LV_OPA_80, LV_STATE_PRESSED);
}

/**
 * @brief 用基础图形绘制一个“相机”图标（圆角机身 + 顶部凸起 + 圆形镜头）。
 *
 * 不用字体图标的原因：LVGL 内置 Montserrat/FontAwesome 字符集里没有
 * 相机字符（工程使用的 lv_font_montserrat_24/48 的符号表只包含
 * LV_SYMBOL_* 那一组，FontAwesome 的 camera(F030) 不在其中）。设置页的
 * 时钟表盘此前也是出于同样原因改为图形绘制，这里沿用同一做法。
 *
 * 图标完全由白色描边 + 透明填充构成，颜色取 UI_COLOR_TEXT，与模式栏里
 * 其它字体符号的观感一致；所有子对象都不接触摸，点击事件仍然交给父
 * 按钮/页面处理。尺寸按传入宽度等比换算，因此同一个函数可以同时用于
 * 80px 模式按钮（约 40px 图标）和占位页的大图标。
 *
 * @param parent 父对象（模式按钮、占位页容器等）
 * @param width  图标宽度（像素）；高度为宽度的 7/8，描边约 width/24
 * @return 图标容器对象；尺寸已设为 width x height，位置由调用者决定
 */
static inline lv_obj_t *ui_camera_icon_create(lv_obj_t *parent, int32_t width)
{
    /* 先算出机身、凸起和镜头的比例，避免调用处再写一套像素常量。 */
    const int32_t w      = width;
    const int32_t h      = (w * 7) / 8;   /* 整个图标的包围盒高度 */
    const int32_t body_y = h / 5;         /* 机身顶边，上面留给凸起 */
    const int32_t body_h = h - body_y;    /* 机身高度 */
    const int32_t lens   = (w * 5) / 10;  /* 镜头直径 */
    const int32_t bw     = (w + 12) / 24; /* 描边宽度：40px→2，72px→3 */

    lv_obj_t *icon = lv_obj_create(parent);
    lv_obj_set_size(icon, w, h);
    lv_obj_set_style_bg_opa(icon, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_width(icon, 0, 0);
    lv_obj_set_style_pad_all(icon, 0, 0);
    lv_obj_remove_flag(icon, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* 顶部凸起（取景器/闪光灯）：比机身窄，坐在机身上沿。先创建它，
     * 后面创建的机身边线会盖住接缝，形成连续的水平轮廓。 */
    lv_obj_t *hump = lv_obj_create(icon);
    lv_obj_set_size(hump, w / 3, body_y + bw);
    lv_obj_set_pos(hump, w / 8, 0);
    lv_obj_set_style_radius(hump, bw, 0);
    lv_obj_set_style_bg_opa(hump, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(hump, UI_COLOR_TEXT, 0);
    lv_obj_set_style_border_width(hump, bw, 0);
    lv_obj_remove_flag(hump, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* 机身：占满宽度，圆角随尺寸缩放。 */
    lv_obj_t *body = lv_obj_create(icon);
    lv_obj_set_size(body, w, body_h);
    lv_obj_set_pos(body, 0, body_y);
    lv_obj_set_style_radius(body, w / 8, 0);
    lv_obj_set_style_bg_opa(body, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(body, UI_COLOR_TEXT, 0);
    lv_obj_set_style_border_width(body, bw, 0);
    lv_obj_remove_flag(body, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    /* 镜头：位于机身正中，纯圆描边。 */
    lv_obj_t *lens_obj = lv_obj_create(icon);
    lv_obj_set_size(lens_obj, lens, lens);
    lv_obj_set_pos(lens_obj, (w - lens) / 2, body_y + (body_h - lens) / 2);
    lv_obj_set_style_radius(lens_obj, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(lens_obj, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(lens_obj, UI_COLOR_TEXT, 0);
    lv_obj_set_style_border_width(lens_obj, bw, 0);
    lv_obj_remove_flag(lens_obj, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    return icon;
}

/* ======================== 状态栏参数 ======================== */
/** 状态栏高度 (像素)，同时也是下拉手势的检测区域高度，增大使状态栏更高 */
#define UI_STATUSBAR_HEIGHT     38
/** 状态栏仅背景混合黑色，20% 不透明度；文字、图标不随背景一起变淡。 */
#define UI_STATUSBAR_BG_COLOR   lv_color_black()
#define UI_STATUSBAR_BG_OPA     LV_OPA_20

/* ======================== 面板参数 ======================== */
/** 模式切换栏宽度 = 屏幕宽度的 1/4 */
#define UI_MODE_PANEL_WIDTH     (UI_HOR_RES / 4)
/** 声光调节栏宽度 = 屏幕宽度的 1/2 */
#define UI_ADJ_PANEL_WIDTH      (UI_HOR_RES / 2)

/* ======================== 边缘手势检测区域 ======================== */
/** 屏幕边缘多少像素以内算作"边缘滑动"起点，增大使手势更容易触发 */
#define UI_EDGE_THRESHOLD       30

/* ======================== 页面 / 模式枚举 ======================== */

/**
 * @brief 应用模式枚举 —— 对应模式切换栏中的各个模式
 */
typedef enum {
    UI_MODE_MUSIC = 0,    /**< 音乐播放器模式 */
    UI_MODE_VIDEO,        /**< 视频播放器模式 */
    UI_MODE_IMAGE,        /**< 图片查看器模式 */
    UI_MODE_EBOOK,        /**< 电子书模式 */
    /* 以下两个模式本轮只提供入口和占位页，不包含实际功能。
     * 追加在末尾可以保持既有模式的下标不变：模式切换动画用下标决定
     * 上下滑动方向，新增项只会出现在电子书之后，原有四页的动画方向
     * 与顺序完全不受影响。 */
    UI_MODE_USB_AUDIO,    /**< USB 声卡模式（占位页，未实现音频功能） */
    UI_MODE_CAMERA,       /**< 拍照模式（占位页，未实现拍照/录像） */
    UI_MODE_MAX           /**< 模式总数，用于数组大小 */
} ui_mode_t;

/**
 * @brief 全局设备状态结构体
 *
 * 各硬件驱动更新此结构体的字段，UI 层读取并刷新显示。
 * 所有字段应在 LVGL 锁内访问以避免竞态。
 */
typedef struct {
    /* 连接状态 —— true 表示已连接/已插入 */
    bool wifi_connected;
    bool bt_connected;
    bool headphone_inserted;
    bool sd_card_inserted;
    /* 电源状态由 PMIC worker 在 LVGL 锁内整体发布。VBUS 接入与正在充电
     * 分开：充满、未装电池等情况下即便有外部电源，也不显示充电闪电。 */
    bool power_valid;         /**< 最近一次 PMIC 状态读取成功 */
    bool battery_present;     /**< PMIC 检测到电池 */
    bool battery_pct_valid;   /**< 已读取有效 SOC；false 时显示 --%，不用演示值 */
    bool vbus_present;        /**< PMIC 报告 VBUS good，代表有效外部输入 */
    bool charging;            /**< 电池实际处于充电过程 */

    /* 数值状态 */
    uint8_t battery_pct;      /**< 电池电量百分比 0-100 */
    uint8_t brightness;       /**< 当前亮屏亮度5-100；自动模式下由光感更新，不含熄屏渐灭值 */
    uint8_t manual_brightness; /**< 最近手动亮度，单独持久化，避免光照变化触发NVS写入 */
    uint8_t volume;           /**< 当前音量 0-100 */
    bool muted;              /**< 静音独立于音量数值保存，供 UI 和 NVS 共用 */
    bool auto_brightness;     /**< 自动亮度是否开启 */

    /* 时间 (由 RTC 或 NTP 更新) */
    bool time_valid;         /**< RTC 读数有效且无 OS/STOP；未知时显示占位文字 */
    uint8_t weekday;          /**< PCF85063TP: 0=周日，1..6=周一..周六 */
    uint8_t hour;             /**< 24 小时制：0-23 */
    uint8_t minute;           /**< 分钟：0-59 */
    uint16_t year;
    uint8_t month;
    uint8_t day;
} ui_device_state_t;

/**
 * @brief 获取全局设备状态指针 (单例)
 * @return 指向全局 ui_device_state_t 的指针
 */
ui_device_state_t *ui_get_device_state(void);

/**
 * @brief 获取 LVGL 主屏幕对象
 * @return 当前活动屏幕的 lv_obj_t 指针
 */
lv_obj_t *ui_get_screen(void);

#ifdef __cplusplus
}
#endif

#endif /* UI_COMMON_H */
