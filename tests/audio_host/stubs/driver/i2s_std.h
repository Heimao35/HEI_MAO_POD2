#pragma once
#include "esp_err.h"
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
typedef void *i2s_chan_handle_t;
typedef struct {void *dma_buf;size_t size;} i2s_event_data_t;
typedef struct {bool(*on_sent)(i2s_chan_handle_t,i2s_event_data_t*,void*);} i2s_event_callbacks_t;
typedef struct {int role;unsigned dma_desc_num,dma_frame_num;bool auto_clear_before_cb,auto_clear_after_cb;} i2s_chan_config_t;
typedef struct {uint32_t sample_rate_hz;} i2s_std_clk_config_t;
typedef struct {struct {uint32_t sample_rate_hz;} clk_cfg;struct {int unused;} slot_cfg;struct {int mclk,bclk,ws,dout,din;} gpio_cfg;} i2s_std_config_t;
#define I2S_NUM_AUTO -1
#define I2S_ROLE_SLAVE 0
#define I2S_GPIO_UNUSED -1
#define I2S_DATA_BIT_WIDTH_32BIT 32
#define I2S_SLOT_MODE_STEREO 2
#define I2S_CHANNEL_DEFAULT_CONFIG(id,role_) {.role=role_}
#define I2S_STD_CLK_DEFAULT_CONFIG(rate) {.sample_rate_hz=rate}
#define I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(bits,mode) {.unused=0}
esp_err_t i2s_new_channel(const i2s_chan_config_t*,i2s_chan_handle_t*,i2s_chan_handle_t*);
esp_err_t i2s_channel_init_std_mode(i2s_chan_handle_t,const i2s_std_config_t*);
esp_err_t i2s_channel_reconfig_std_clock(i2s_chan_handle_t,const i2s_std_clk_config_t*);
esp_err_t i2s_channel_register_event_callback(i2s_chan_handle_t,const i2s_event_callbacks_t*,void*);
esp_err_t i2s_channel_preload_data(i2s_chan_handle_t,const void*,size_t,size_t*);
esp_err_t i2s_channel_enable(i2s_chan_handle_t);
esp_err_t i2s_channel_disable(i2s_chan_handle_t);
esp_err_t i2s_del_channel(i2s_chan_handle_t);
