/* Project extension compiled inside ESP-IDF v5.5.4's DPI translation unit.
 * The source patch adds a DMA-boundary state handshake; no private struct
 * layout is duplicated and neither SDK installation nor managed files change. */
esp_err_t player_dpi_suspend(esp_lcd_panel_handle_t panel)
{
    esp_lcd_dpi_panel_t *dpi = __containerof(panel, esp_lcd_dpi_panel_t, base);
    unsigned expected = 0;
    if (!atomic_compare_exchange_strong(&dpi->player_sleep_state, &expected, 1))
        return expected == 2 ? ESP_OK : ESP_ERR_INVALID_STATE;
    TickType_t started = xTaskGetTickCount();
    while (atomic_load_explicit(&dpi->player_sleep_state, memory_order_acquire) != 2) {
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(200)) {
            expected = 1;
            /* Only cancel an unclaimed request. State 3 means the ISR is
             * already stopping hardware; wait for its very short completion. */
            if (atomic_compare_exchange_strong(&dpi->player_sleep_state, &expected, 0))
                return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

/* 此扩展依赖 rev3 的显示桥复位和真实 VSYNC。旧芯片上的 LL reset
 * 是空函数，不能用一次成功返回掩盖 FIFO 未复位；升级/换芯片需复核。 */
#if HAL_CONFIG(CHIP_SUPPORT_MIN_REV) < 300
#error "DPI sleep restart requires ESP32-P4 revision >= 3.0"
#endif

/* 只保存显示桥运行配置，绝不整块回写寄存器映像：FIFO 深度、IRQ
 * 状态/清除、配置更新等含只读或写触发语义。以下字段对应 v5.5.4
 * DPI 初始化/色彩转换使用的配置，复位后保持时序、颜色和 DMA 模式。
 * 这几十字节在电源任务栈上，不新分配帧缓冲，也不重新创建 LVGL。 */
#define PLAYER_BRG_CONFIG_REGS(X) \
    X(dma_req_cfg) X(raw_num_cfg) X(raw_buf_credit_ctl) X(pixel_type) \
    X(dma_block_interval) X(dma_req_interval) X(dpi_lcd_ctl) \
    X(dpi_rsv_dpi_data) X(dpi_v_cfg0) X(dpi_v_cfg1) X(dpi_h_cfg0) \
    X(dpi_h_cfg1) X(dpi_misc_config) X(host_trigger_rev) \
    X(blk_raw_num_cfg) X(dma_frame_interval) X(host_ctrl) \
    X(dma_flow_ctrl) X(raw_buf_almost_empty_thrd) X(yuv_cfg)

static void player_dpi_reset_stream(esp_lcd_dpi_panel_t *dpi)
{
    mipi_dsi_hal_context_t *hal = &dpi->bus->hal;
#define PLAYER_SAVE_REG(name) uint32_t saved_##name = hal->bridge->name.val;
    PLAYER_BRG_CONFIG_REGS(PLAYER_SAVE_REG)
#undef PLAYER_SAVE_REG
    /* DMA 已停且亮度为零。只复位主控内的显示桥/Host，不碰屏幕 RESET、
     * D-PHY、PMIC 电源或帧缓冲地址。旧实现关闭 DPI 时可能留下未发送像素；
     * 仅从帧首重开 DMA 会把旧 FIFO 尾巴接到新帧前，造成持续的行/列错位。 */
    mipi_dsi_host_ll_power_on_off(hal->host, false);
    mipi_dsi_brg_ll_enable(hal->bridge, false);
    mipi_dsi_brg_ll_reset(hal->bridge);
#define PLAYER_RESTORE_REG(name) hal->bridge->name.val = saved_##name;
    PLAYER_BRG_CONFIG_REGS(PLAYER_RESTORE_REG)
#undef PLAYER_RESTORE_REG
    /* 不恢复旧的 dpi_en/中断标志；先清场再按 DMA -> Host -> DPI 启动。 */
    mipi_dsi_brg_ll_enable_dpi_output(hal->bridge, false);
    mipi_dsi_brg_ll_enable_interrupt(hal->bridge,
        MIPI_DSI_BRG_LL_EVENT_UNDERRUN | MIPI_DSI_BRG_LL_EVENT_VSYNC, false);
    mipi_dsi_brg_ll_clear_interrupt_status(hal->bridge,
        MIPI_DSI_BRG_LL_EVENT_UNDERRUN | MIPI_DSI_BRG_LL_EVENT_VSYNC);
    mipi_dsi_brg_ll_enable(hal->bridge, true);
    mipi_dsi_brg_ll_update_dpi_config(hal->bridge);
}
#undef PLAYER_BRG_CONFIG_REGS

esp_err_t player_dpi_resume(esp_lcd_panel_handle_t panel)
{
    esp_lcd_dpi_panel_t *dpi = __containerof(panel, esp_lcd_dpi_panel_t, base);
    unsigned state = atomic_load_explicit(&dpi->player_sleep_state, memory_order_acquire);
    if (state != 2 && state != 0) return ESP_ERR_INVALID_STATE;
    mipi_dsi_hal_context_t *hal = &dpi->bus->hal;
    unsigned first_frame = atomic_load_explicit(&dpi->player_frame_seq, memory_order_acquire);
    if (state == 2) {
        player_dpi_reset_stream(dpi);
        dw_gdma_link_list_handle_t link = dpi->link_lists[dpi->cur_fb_index];
        dw_gdma_block_markers_t markers = {.is_valid = true, .is_last = true};
        dw_gdma_lli_set_block_markers(dw_gdma_link_list_get_item(link, 0), markers);
        dw_gdma_channel_use_link_list(dpi->dma_chan, link);
        atomic_store_explicit(&dpi->player_sleep_state, 0, memory_order_release);
        dw_gdma_channel_enable_ctrl(dpi->dma_chan, true);
        mipi_dsi_host_ll_enable_video_mode(hal->host, true);
        mipi_dsi_host_ll_set_clock_lane_state(hal->host, MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO);
        mipi_dsi_host_ll_power_on_off(hal->host, true);
        /* 此计数由桥的真实 VSYNC ISR 推进，不能用 DMA done 代替。
         * 即使上层无 refresh 回调，唤醒屏障也必须临时能收到 VSYNC。 */
        mipi_dsi_brg_ll_enable_interrupt(hal->bridge,
            MIPI_DSI_BRG_LL_EVENT_UNDERRUN | MIPI_DSI_BRG_LL_EVENT_VSYNC, true);
        mipi_dsi_brg_ll_enable_dpi_output(hal->bridge, true);
        mipi_dsi_brg_ll_update_dpi_config(hal->bridge);
    }
    /* 保持黑屏直到新时序输出了完整帧。两次新 VSYNC 排除启动边沿，
     * 不在 LVGL 锁内等待，也不重新播放页面滑入。超时仍保持亮度为零；
     * 下次调用仅重试等待，不在运行中的 DMA 下再次复位显示桥。 */
    TickType_t started = xTaskGetTickCount();
    while ((unsigned)(atomic_load_explicit(&dpi->player_frame_seq, memory_order_acquire) - first_frame) < 2U) {
        if (xTaskGetTickCount() - started >= pdMS_TO_TICKS(200)) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    mipi_dsi_brg_ll_enable_interrupt(hal->bridge, MIPI_DSI_BRG_LL_EVENT_VSYNC,
                                    dpi->on_refresh_done != NULL);
    return ESP_OK;
}
