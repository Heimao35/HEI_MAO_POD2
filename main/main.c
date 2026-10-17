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

#include "esp_lcd_co5300.h"
#include "co5300_init_cmds.h"
#include "esp_lcd_touch_cst820.h"

#include "ui_manager.h"
#include "ui_adj_panel.h"
#include "ui_lockscreen.h"
#include "sd_card.h"
#include "music_player_adapter.h"
#include "ui_fonts.h"

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
    const lvgl_port_cfg_t lvgl_cfg = ESP_LVGL_PORT_INIT_CONFIG();
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
            .sw_rotate = true,// true: 软件; false: 硬件
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

static void on_brightness_change(uint8_t val)
{
    if (val < 5) val = 5;
    esp_lcd_panel_co5300_set_brightness(lcd_panel, val);
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

    /* Show LVGL objects */
    lvgl_port_lock(0);

    /* Mount and load the build-time font assets before any label is created. */
    esp_err_t font_ret = ui_fonts_init();
    if (font_ret != ESP_OK) ESP_LOGW(TAG, "Custom fonts unavailable: %s", esp_err_to_name(font_ret));

    /* 初始化自定义 UI 系统（替换原有 demo） */
    ui_manager_init();

    /* 注册亮度回调，滑条拖动时实际调节屏幕亮度 */
    ui_adj_panel_set_callbacks(on_brightness_change, NULL);

    lvgl_port_unlock();

    /* Initial SD mount, followed by interrupt/poll-based hot-plug detection. */
    /* 放在 UI 初始化之后，确保 lockscreen 等 UI 对象已创建 */
    esp_err_t sd_ret = sd_card_init();
    if (sd_ret != ESP_OK) ESP_LOGE(TAG, "SD init failed: %s", esp_err_to_name(sd_ret));

    /* Background SD catalogue/cover/LRC loader. No fake decoder or DAC task. */
    esp_err_t ret = music_player_adapter_init();
    if (ret != ESP_OK) ESP_LOGE(TAG, "Music integration failed: %s", esp_err_to_name(ret));

    /* 如果 SD 卡已挂载且锁屏可见，启动开机动画 */
    if (sd_card_is_mounted()) {
        lvgl_port_lock(0);
        ui_lockscreen_start_boot_anim();
        lvgl_port_unlock();
    }
}
