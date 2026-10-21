#include <inttypes.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_attr.h"
#include "esp_err.h"
#include "esp_log.h"
#include "esp_check.h"
#include "driver/i2c_master.h"
#include "esp_lcd_panel_io.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_mipi_dsi.h"
#include "driver/gpio.h"
#include "esp_ldo_regulator.h"
#include "esp_lvgl_port.h"
#include "screen_power.h"
#include "ambient_light.h"
#include "system_power.h"
#include "player_settings.h"
#include "volume_keys.h"
#include "motion_sensor.h"

#include "esp_lcd_co5300.h"
#include "co5300_init_cmds.h"
#include "esp_lcd_touch_cst820.h"

#include "ui_manager.h"
#include "ui_adj_panel.h"
#include "ui_lockscreen.h"
#include "sd_card.h"
#include "lock_animation.h"
#include "pmic_axp2101.h"
#include "music_player_adapter.h"
#include "audio_output.h"
#include "ui_fonts.h"
#include "ui_boot.h"

/* LCD size */
#define EXAMPLE_LCD_H_RES   (460)
#define EXAMPLE_LCD_V_RES   (460)

/* MIPI DSI 时序：16 MHz / (568 * 472) ≈ 59.7 Hz；lane 480 Mbps 实测会导致不断重启 */
#define EXAMPLE_LCD_DPI_CLK_MHZ        (16)
#define EXAMPLE_LCD_LANE_BIT_RATE_MBPS (320)

#if LV_COLOR_DEPTH == 16
#define MIPI_DPI_PX_FORMAT (LCD_COLOR_PIXEL_FORMAT_RGB565)
#define BSP_LCD_COLOR_DEPTH (16)
#define LV_COLOR_FORMAT LV_COLOR_FORMAT_RGB565
#elif LV_COLOR_DEPTH == 24
#define MIPI_DPI_PX_FORMAT (LCD_COLOR_PIXEL_FORMAT_RGB888)
#define BSP_LCD_COLOR_DEPTH (24)
#define LV_COLOR_FORMAT LV_COLOR_FORMAT_RGB888
#endif

// “VDD_MIPI_DPHY”应供电 2.5V，可从内部 LDO 稳压器或外部 LDO 芯片获取电源
#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN 3 // LDO_VO3 连接至 VDD_MIPI_DPHY
#define EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV 2500
#define EXAMPLE_PIN_NUM_LCD_RST  -1
#define EXAMPLE_PIN_NUM_LCD_TE   (GPIO_NUM_23)

/* Touch settings */
#define EXAMPLE_TOUCH_I2C_NUM       (I2C_NUM_0)
#define EXAMPLE_TOUCH_I2C_CLK_HZ    (400 * 1000)

/* LCD touch pins */
#define EXAMPLE_TOUCH_I2C_SCL       (GPIO_NUM_8)
#define EXAMPLE_TOUCH_I2C_SDA       (GPIO_NUM_7)
#define EXAMPLE_TOUCH_RST           (GPIO_NUM_5)
#define EXAMPLE_TOUCH_INT           (GPIO_NUM_6)

static const char *TAG = "EXAMPLE";

/* LCD IO and panel */
static esp_lcd_panel_handle_t lcd_panel = NULL;
static esp_lcd_panel_io_handle_t io_handle = NULL;
static esp_lcd_touch_handle_t touch_handle = NULL;

/* LVGL display and touch */
static lv_display_t *lvgl_disp = NULL;
static lv_indev_t *lvgl_touch_indev = NULL;

/* TE 脉冲计数，仅用于核对面板内部刷新率与 DPI 输出帧率是否一致 */
static volatile uint32_t s_te_count = 0;

static void IRAM_ATTR lcd_te_isr_handler(void *arg)
{
    s_te_count++;
}

static esp_err_t app_lcd_te_init(void)
{
    const gpio_config_t te_cfg = {
        .pin_bit_mask = 1ULL << EXAMPLE_PIN_NUM_LCD_TE,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_ENABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    ESP_RETURN_ON_ERROR(gpio_config(&te_cfg), TAG, "TE gpio config failed");
    esp_err_t ret = gpio_install_isr_service(0);
    ESP_RETURN_ON_FALSE(ret == ESP_OK || ret == ESP_ERR_INVALID_STATE, ret, TAG, "install gpio isr service failed");
    return gpio_isr_handler_add(EXAMPLE_PIN_NUM_LCD_TE, lcd_te_isr_handler, NULL);
}

static void app_lcd_te_report(void)
{
    const uint32_t h_total = EXAMPLE_LCD_H_RES + 72 + 4 + 32;
    const uint32_t v_total = EXAMPLE_LCD_V_RES + 4 + 2 + 6;
    const float dpi_fps = EXAMPLE_LCD_DPI_CLK_MHZ * 1.0e6f / (h_total * v_total);

    uint32_t start = s_te_count;
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGI(TAG, "TE: %"PRIu32" Hz, DPI output: %.1f Hz", s_te_count - start, dpi_fps);
}

static void example_bsp_enable_dsi_phy_power(void)
{
    // 打开 MIPI DSI PHY 的电源，使其从“无电”状态进入“关机”状态
    esp_ldo_channel_handle_t ldo_mipi_phy = NULL;
#ifdef EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN
    esp_ldo_channel_config_t ldo_mipi_phy_config = {
        .chan_id = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_CHAN,
        .voltage_mv = EXAMPLE_MIPI_DSI_PHY_PWR_LDO_VOLTAGE_MV,
    };
    ESP_ERROR_CHECK(esp_ldo_acquire_channel(&ldo_mipi_phy_config, &ldo_mipi_phy));
    ESP_LOGI(TAG, "MIPI DSI PHY Powered on");
#endif
}

static esp_err_t app_lcd_init(void)
{
    esp_err_t ret = ESP_OK;

    example_bsp_enable_dsi_phy_power();

    // 首先创建 MIPI DSI 总线，它还将初始化 DSI PHY
    esp_lcd_dsi_bus_handle_t mipi_dsi_bus;
    esp_lcd_dsi_bus_config_t bus_config = {                    \
        .bus_id = 0,                                           \
        .num_data_lanes = 1,                                   \
        .phy_clk_src = 0,                                      \
        .lane_bit_rate_mbps = EXAMPLE_LCD_LANE_BIT_RATE_MBPS,  \
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_dsi_bus(&bus_config, &mipi_dsi_bus), err, TAG, "LCD init failed");

    ESP_LOGI(TAG, "Install MIPI DSI LCD control panel");
    // 我们使用DBI接口发送LCD命令和参数
    esp_lcd_dbi_io_config_t dbi_config = CO5300_PANEL_IO_DBI_CONFIG();

    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_io_dbi(mipi_dsi_bus, &dbi_config, &io_handle), err, TAG, "LCD init failed");

    // 创建CO5300控制面板
    esp_lcd_dpi_panel_config_t dpi_config = {                 \
        .dpi_clk_src = MIPI_DSI_DPI_CLK_SRC_DEFAULT,          \
        .dpi_clock_freq_mhz = EXAMPLE_LCD_DPI_CLK_MHZ,        \
        .virtual_channel = 0,                                 \
        .pixel_format = MIPI_DPI_PX_FORMAT,                   \
        .num_fbs = 2,                                         \
        .video_timing = {                                     \
            .h_size = EXAMPLE_LCD_H_RES,                      \
            .v_size = EXAMPLE_LCD_V_RES,                      \
            .hsync_back_porch = 72,                           \
            .hsync_pulse_width = 4,                           \
            .hsync_front_porch = 32,                          \
            .vsync_back_porch = 4,                            \
            .vsync_pulse_width = 2,                           \
            .vsync_front_porch = 6,                           \
        },                                                    \
        .flags.use_dma2d = true,                              \
    };

    co5300_vendor_config_t vendor_config = {
        .init_cmds = lcd_init_cmds,
        .init_cmds_size = lcd_init_cmds_size,
        .flags.use_mipi_interface = 1,
        .mipi_config = {
            .dsi_bus = mipi_dsi_bus,
            .dpi_config = &dpi_config,
        },
    };
    const esp_lcd_panel_dev_config_t panel_config = {
        .reset_gpio_num = EXAMPLE_PIN_NUM_LCD_RST,
        .rgb_ele_order = LCD_RGB_ELEMENT_ORDER_RGB,
        .bits_per_pixel = BSP_LCD_COLOR_DEPTH,
        .vendor_config = &vendor_config,
    };
    ESP_GOTO_ON_ERROR(esp_lcd_new_panel_co5300(io_handle, &panel_config, &lcd_panel), err, TAG, "AMOLED init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_reset(lcd_panel), err, TAG, "AMOLED init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_init(lcd_panel), err, TAG, "AMOLED init failed");
    ESP_GOTO_ON_ERROR(esp_lcd_panel_co5300_set_brightness(lcd_panel, 80), err, TAG, "AMOLED init failed");

    return ret;

err:
    if (lcd_panel) {
        esp_lcd_panel_del(lcd_panel);
    }
    return ret;
}

static esp_err_t app_touch_init(void)
{
    /* Initilize I2C */
    const i2c_master_bus_config_t i2c_bus_config = {
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .i2c_port = EXAMPLE_TOUCH_I2C_NUM,
        .scl_io_num = EXAMPLE_TOUCH_I2C_SCL,
        .sda_io_num = EXAMPLE_TOUCH_I2C_SDA,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t i2c_bus;
    ESP_ERROR_CHECK(i2c_new_master_bus(&i2c_bus_config, &i2c_bus));

    esp_lcd_panel_io_handle_t tp_io_handle = NULL;
    esp_lcd_panel_io_i2c_config_t tp_io_config = ESP_LCD_TOUCH_IO_I2C_CST820_CONFIG();
    tp_io_config.scl_speed_hz = EXAMPLE_TOUCH_I2C_CLK_HZ;

    ESP_ERROR_CHECK(esp_lcd_new_panel_io_i2c(i2c_bus, &tp_io_config, &tp_io_handle));

    /* Initialize touch HW */
    const esp_lcd_touch_config_t tp_cfg = {
        .x_max = EXAMPLE_LCD_H_RES,
        .y_max = EXAMPLE_LCD_V_RES,
        .rst_gpio_num = EXAMPLE_TOUCH_RST,
        .int_gpio_num = EXAMPLE_TOUCH_INT,
        .levels = {
            .reset = 0,
            .interrupt = 0,
        },
        .flags = {
            .swap_xy = 0,
            .mirror_x = 0,
            .mirror_y = 0,
        },
    };
    return esp_lcd_touch_new_i2c_cst820(tp_io_handle, &tp_cfg, &touch_handle);
}

static esp_err_t app_lvgl_init(void)
{
    /* Initialize LVGL */
    lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    /* 音频解码和I2S中断固定core0，UI固定core1，避免MP3和高采样率解码
     * 的短时突发抢占渲染。只改变任务归属，页面布局、手势、动画保持原样。 */
    lvgl_cfg.task_affinity = 1;
    ESP_RETURN_ON_ERROR(lvgl_port_init(&lvgl_cfg), TAG, "LVGL port initialization failed");

    /* Add LCD screen */
    ESP_LOGD(TAG, "Add LCD screen");
    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle = io_handle,
        .panel_handle = lcd_panel,
        .buffer_size = EXAMPLE_LCD_H_RES * EXAMPLE_LCD_V_RES,
        .double_buffer = true,
        .hres = EXAMPLE_LCD_H_RES,
        .vres = EXAMPLE_LCD_V_RES,
        .monochrome = false,
        .color_format = LV_COLOR_FORMAT,
        .rotation = {
            .swap_xy = false,
            .mirror_x = false,
            .mirror_y = false,
        },
        .flags = {
            .buff_dma = false,
            .buff_spiram = true,
            .sw_rotate = false,// true: 软件; false: 硬件
            .swap_bytes = false,
            .full_refresh = false,
            .direct_mode = true,
        }
    };

    const lvgl_port_display_dsi_cfg_t dpi_cfg = {
        .flags = {
            .avoid_tearing = true,
        }
    };

    lvgl_disp = lvgl_port_add_disp_dsi(&disp_cfg, &dpi_cfg);

    /* Add touch input (for selected screen) */
    const lvgl_port_touch_cfg_t touch_cfg = {
        .disp = lvgl_disp,
        .handle = touch_handle,
    };
    lvgl_touch_indev = lvgl_port_add_touch(&touch_cfg);

    return ESP_OK;
}

/* 普通初始化阶段在 LVGL 锁外运行，只有状态发布/刷新占用锁。
 * 字库回调则已经持锁，直接调用 ui_boot 接口，避免嵌套等待。 */
static void app_boot_progress(uint8_t progress, const char *stage)
{
    lvgl_port_lock(0);
    ui_boot_set_progress(progress,stage);
    lvgl_port_unlock();
}

static void app_font_progress(unsigned completed, unsigned total,
                              const char *filename, void *user_data)
{
    (void)user_data;
    /* 用户只需要知道仍在启动，不暴露内部字库文件名。 */
    const char *stage=filename ? "Loading fonts..." : "Fonts ready";
    /* 字库阶段占启动条的 10..70 区间，只表示已完成的阶段，
     * 不表示实际字节数或预计剩余时间；大字库内部允许短暂停留。 */
    ui_boot_set_progress((uint8_t)(10+completed*60/total),stage);
}

void app_main(void)
{
    /* LCD HW initialization */
    ESP_ERROR_CHECK(app_lcd_init());
    ESP_ERROR_CHECK(app_lcd_te_init());
    app_lcd_te_report();

    /* Touch initialization */
    ESP_ERROR_CHECK(app_touch_init());

    /* LVGL initialization */
    ESP_ERROR_CHECK(app_lvgl_init());

    /* 第一幅提示画面不依赖 NVS、字库或 SD。启动完成前暂停触摸输入，
     * 避免用户在加载阶段的触摸被底层锁屏/播放器当成正常手势。 */
    lvgl_port_lock(0);
    lv_indev_enable(lvgl_touch_indev,false);
    ui_boot_show(lvgl_disp);
    ui_boot_set_progress(2,"Loading settings...");
    lvgl_port_unlock();

    /* 设置在 UI 创建前从 Flash 读取；失败保留 NVS 原数据并沿用默认设置。 */
    esp_err_t settings_ret=player_settings_init();
    if (settings_ret!=ESP_OK) ESP_LOGW(TAG,"Settings unavailable: %s",esp_err_to_name(settings_ret));

    /* Show LVGL objects */
    lvgl_port_lock(0);
    player_settings_restore_ui();

    /* 正常页面使用外部字体；开机提示始终用内置字体。先刷新挂载提示，
     * 逐字库加载时再由同步回调推进阶段条，期间不释放共同 LVGL 锁。 */
    ui_boot_set_progress(10,"Mounting fonts...");
    esp_err_t font_ret = ui_fonts_init_with_progress(app_font_progress,NULL);
    if (font_ret != ESP_OK) ESP_LOGW(TAG, "Custom fonts unavailable: %s", esp_err_to_name(font_ret));

    ui_boot_set_progress(70,"Creating interface...");
    /* 初始化自定义 UI 系统（替换原有 demo） */
    ui_manager_init();
    ui_adj_panel_update(); /* 同步恢复的音量/亮度及静音/自动亮度图标。 */

    /* 注册亮度/音量回调：UI只投递请求，显示/音频任务分别写硬件。 */
    ui_adj_panel_set_callbacks(screen_power_set_brightness, audio_output_set_volume);
    /* 恢复的静音/音量先投递给音频worker，DAC上电始终保持静音到时钟和PCM就绪。 */
    audio_output_set_volume(ui_get_device_state()->muted?0:ui_get_device_state()->volume);
    ui_adj_panel_set_auto_brightness_callback(screen_power_set_auto_brightness);

    ui_boot_set_progress(74,"Starting power monitor...");
    lvgl_port_unlock();

    /* PMIC readings run on I2C1 in a separate worker. Start after UI objects
     * exist, outside the LVGL lock; an unavailable PMIC leaves an unknown
     * battery indication and is retried without blocking media startup. */
    pmic_axp2101_set_key_callback(system_power_key_short);
    pmic_axp2101_set_key_edge_callback(system_power_key_edge);
    esp_err_t pmic_ret = pmic_axp2101_init();
    if (pmic_ret != ESP_OK) ESP_LOGE(TAG, "PMIC monitor init failed: %s", esp_err_to_name(pmic_ret));

    app_boot_progress(78,"Checking SD card...");
    /* Initial SD mount, followed by interrupt/poll-based hot-plug detection. */
    /* 放在 UI 初始化之后，确保 lockscreen 等 UI 对象已创建 */
    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) ESP_LOGE(TAG, "SD init failed: %s", esp_err_to_name(sd_ret));

    app_boot_progress(82,"Preparing lock animation...");
    /* 动画资源只在本次启动导入一次，此时音频/屏幕电源任务尚未启动。
     * 全部SD读取和Flash擦写均在LVGL锁外，且不受曲库扫描是否成功影响。
     * 导入完成即从Flash持久资源的只读缓存重播，无卡也可使用上次动画。
     * 运行中更换SD文件不会触发导入；用户替换000.mjpeg后重新开机更新。 */
    esp_err_t animation_ret=lock_animation_init();
    if (animation_ret!=ESP_OK) ESP_LOGW(TAG,"Lock animation unavailable: %s",esp_err_to_name(animation_ret));
    app_boot_progress(88,"Starting audio...");

    /* 解码/I2S与封面/LRC加载使用不同任务，避免JPEG/LVGL阻塞音频。
     * DAC任务自行等待PMIC供电就绪，全部初始化与文件/I2C在LVGL锁外。 */
    esp_err_t audio_ret=audio_output_init();
    if(audio_ret!=ESP_OK)ESP_LOGE(TAG,"Audio output init failed: %s",esp_err_to_name(audio_ret));

    app_boot_progress(92,"Starting music library...");
    /* Background SD catalogue/cover/LRC loader. */
    esp_err_t ret = music_player_adapter_init();
    if (ret != ESP_OK) ESP_LOGE(TAG, "Music integration failed: %s", esp_err_to_name(ret));

    app_boot_progress(95,"Starting display controls...");
    /* 字体和 SD 初始挂载结束后才启动十秒计时；触摸 IRQ 包装器保留
     * LVGL 正常输入，仅在熄屏时截获唤醒。所有硬件操作均在锁外执行。 */
    /* ALS初始化和ADC校准在LVGL锁外；随后由显示电源任务低频采样，
     * 不额外创建会与Light-sleep并发的ADC任务。失败时保留原手动亮度。 */
    esp_err_t light_ret = ambient_light_init();
    if (light_ret != ESP_OK) ESP_LOGW(TAG, "Ambient light init failed: %s", esp_err_to_name(light_ret));
    esp_err_t power_ret = screen_power_init(lvgl_disp, lvgl_touch_indev, lcd_panel, touch_handle);
    if (power_ret != ESP_OK) ESP_LOGE(TAG, "Screen power init failed: %s", esp_err_to_name(power_ret));

    app_boot_progress(98,"Starting system controls...");
    esp_err_t system_ret=system_power_init();
    if (system_ret!=ESP_OK) ESP_LOGE(TAG,"System power init failed: %s",esp_err_to_name(system_ret));

    /* BMI270复用现有I2C1；8KiB功能配置由独立低优先级worker分块加载，
     * 不持有LVGL锁、不阻塞开机提示/音频/PMIC。先准备好显示与关机接口，
     * 再允许传感器发布温度、当天步数和经过状态寄存器确认的抬腕事件。 */
    esp_err_t motion_ret = motion_sensor_init();
    if (motion_ret != ESP_OK) ESP_LOGW(TAG, "Motion sensor init failed: %s", esp_err_to_name(motion_ret));

    /* 音量键与触摸滑条共享设备音量和回调；UI 和熄屏状态机就绪后再接入。 */
    esp_err_t keys_ret = volume_keys_init();
    if (keys_ret != ESP_OK) ESP_LOGE(TAG, "Volume keys init failed: %s", esp_err_to_name(keys_ret));

    /* 所有同步启动阶段已处理完（可选外设失败仍沿用既有降级/重试），
     * 不等待后台曲库扫描。只移除临时提示层，不清空任何正常页面。
     * 吃掉启动期间仍按住的触摸；最后重置活动时间，十秒锁屏计时从
     * 用户真正看到锁屏时开始，避免长时间导入动画资源后立即熄屏。 */
    lvgl_port_lock(0);
    ui_boot_set_progress(100,"Ready");
    ui_boot_hide();
    lv_indev_enable(lvgl_touch_indev,true);
    lv_indev_wait_release(lvgl_touch_indev);
    lv_display_trigger_activity(lvgl_disp);
    lvgl_port_unlock();

    /* 提示层退出后再投递原有 MJPEG 重播请求，保证用户从第一帧看到
     * 动画，而不是动画已经在黑色提示层下面播放。资源仍来自 Flash/
     * PSRAM 缓存，运行时不重新读取 SD，原动画帧率/布局均不变。 */
    ui_lockscreen_start_boot_anim();
}
