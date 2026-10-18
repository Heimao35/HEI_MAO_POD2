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
    uint8_t brightness;       /**< 当前亮度 0-100 */
    uint8_t volume;           /**< 当前音量 0-100 */
    bool muted;              /**< 静音独立于音量数值保存，供 UI 和 NVS 共用 */
    bool auto_brightness;     /**< 自动亮度是否开启 */

    /* 时间 (由 RTC 或 NTP 更新) */
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
