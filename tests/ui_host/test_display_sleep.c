/* Run the production DPI restart extension against a register/FIFO model.
 * DMA completion deliberately precedes physical VSYNC, as on the board. */
#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <limits.h>
#include <stdatomic.h>

typedef int esp_err_t;
#define ESP_OK 0
#define ESP_ERR_INVALID_STATE 1
#define ESP_ERR_TIMEOUT 2
#define HAL_CONFIG(x) 300
#define pdMS_TO_TICKS(x) (x)
#define MIPI_DSI_BRG_LL_EVENT_UNDERRUN 1U
#define MIPI_DSI_BRG_LL_EVENT_VSYNC 2U
#define MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO 0
#define __containerof(p, t, f) ((t *)((char *)(p) - offsetof(t, f)))
typedef uint32_t TickType_t;
typedef struct { uint32_t val; } reg_t;
typedef struct {
    reg_t dma_req_cfg, raw_num_cfg, raw_buf_credit_ctl, pixel_type;
    reg_t dma_block_interval, dma_req_interval, dpi_lcd_ctl, dpi_rsv_dpi_data;
    reg_t dpi_v_cfg0, dpi_v_cfg1, dpi_h_cfg0, dpi_h_cfg1, dpi_misc_config;
    reg_t host_trigger_rev, blk_raw_num_cfg, dma_frame_interval, host_ctrl;
    reg_t dma_flow_ctrl, raw_buf_almost_empty_thrd, yuv_cfg;
    unsigned fifo, pending, interrupts;
    bool enabled, output;
} bridge_t;
typedef struct { bool powered, video; } host_t;
typedef struct { bridge_t *bridge; host_t *host; } mipi_dsi_hal_context_t;
typedef struct { mipi_dsi_hal_context_t hal; } bus_t;
typedef struct { int unused; } panel_t;
typedef panel_t *esp_lcd_panel_handle_t;
typedef unsigned dw_gdma_link_list_handle_t;
typedef struct { bool is_valid, is_last; } dw_gdma_block_markers_t;
typedef struct {
    atomic_uint player_sleep_state, player_frame_seq;
    panel_t base;
    bus_t *bus;
    unsigned cur_fb_index, link_lists[2], dma_chan;
    void *on_refresh_done;
} esp_lcd_dpi_panel_t;

static esp_lcd_dpi_panel_t dpi;
static bridge_t bridge;
static host_t host;
static bus_t bus;
static TickType_t ticks;
static unsigned resets, host_resets, dma_starts, selected_link, waits;
static bool generate_vsync = true;
static TickType_t xTaskGetTickCount(void) { return ticks; }
static void vTaskDelay(unsigned dt)
{
    ticks += dt;
    ++waits;
    /* Every DMA starts immediately, but neither that nor a stale pending
     * interrupt is allowed to satisfy the production visible-frame barrier. */
    assert(dma_starts && resets && bridge.fifo == 0);
    assert(atomic_load(&dpi.player_sleep_state) == 0);
    if (generate_vsync && (ticks & 1U) == 0 &&
        (bridge.interrupts & MIPI_DSI_BRG_LL_EVENT_VSYNC))
        atomic_fetch_add(&dpi.player_frame_seq, 1);
}
static void mipi_dsi_brg_ll_enable(bridge_t *b, bool en) { b->enabled = en; }
static void mipi_dsi_host_ll_power_on_off(host_t *h, bool en)
{ h->powered = en; if (!en) ++host_resets; }
static void mipi_dsi_brg_ll_reset(bridge_t *b)
{
    assert(!host.powered && !b->enabled);
    ++resets;
    /* Include loss of configuration in the reset model: restart must restore
     * color/timing/flow fields, not just purge the leftover pixels. */
    memset(b, 0, sizeof(*b));
}
static void mipi_dsi_brg_ll_enable_dpi_output(bridge_t *b, bool en)
{ if (en) assert(host.powered && dma_starts); b->output = en; }
static void mipi_dsi_brg_ll_update_dpi_config(bridge_t *b) { (void)b; }
static void mipi_dsi_brg_ll_clear_interrupt_status(bridge_t *b, unsigned mask)
{ b->pending &= ~mask; }
static void mipi_dsi_brg_ll_enable_interrupt(bridge_t *b, unsigned mask, bool en)
{ if (en) b->interrupts |= mask; else b->interrupts &= ~mask; }
static void mipi_dsi_host_ll_enable_video_mode(host_t *h, bool en) { h->video = en; }
static void mipi_dsi_host_ll_set_clock_lane_state(host_t *h, int state)
{ (void)h; assert(state == MIPI_DSI_LL_CLOCK_LANE_STATE_AUTO); }
static unsigned dw_gdma_link_list_get_item(unsigned link, unsigned index)
{ assert(!index); return link; }
static void dw_gdma_lli_set_block_markers(unsigned item, dw_gdma_block_markers_t markers)
{ assert(item == 22 && markers.is_valid && markers.is_last); }
static void dw_gdma_channel_use_link_list(unsigned channel, unsigned link)
{ assert(channel == 7 && link == 22); selected_link = link; }
static void dw_gdma_channel_enable_ctrl(unsigned channel, bool en)
{
    assert(channel == 7 && en && selected_link == 22 && !host.powered);
    assert(bridge.enabled && !bridge.output && bridge.fifo == 0 && !bridge.pending);
    assert(bridge.dpi_h_cfg0.val == 0x12345678 && bridge.dpi_v_cfg0.val == 0x98765432);
    assert(bridge.pixel_type.val == 0x4321 && bridge.dma_flow_ctrl.val == 0x99);
    ++dma_starts;
}

#include "../../main/display_dpi_sleep.inc"

static void prepare(void)
{
    memset(&dpi, 0, sizeof(dpi));
    memset(&bridge, 0, sizeof(bridge));
    memset(&host, 0, sizeof(host));
    bus.hal = (mipi_dsi_hal_context_t){&bridge, &host};
    dpi.bus = &bus;
    dpi.cur_fb_index = 1; /* Must preserve the displayed framebuffer, not force fb0. */
    dpi.link_lists[0] = 11;
    dpi.link_lists[1] = 22;
    dpi.dma_chan = 7;
    atomic_init(&dpi.player_sleep_state, 2);
    atomic_init(&dpi.player_frame_seq, 17);
    bridge.dpi_h_cfg0.val = 0x12345678;
    bridge.dpi_v_cfg0.val = 0x98765432;
    bridge.pixel_type.val = 0x4321;
    bridge.dma_flow_ctrl.val = 0x99;
    bridge.fifo = 91; /* Old frame tail left by the previous stop. */
    bridge.pending = MIPI_DSI_BRG_LL_EVENT_VSYNC | MIPI_DSI_BRG_LL_EVENT_UNDERRUN;
    ticks = waits = resets = host_resets = dma_starts = selected_link = 0;
    generate_vsync = true;
}

int main(void)
{
    prepare();
    assert(player_dpi_resume(&dpi.base) == ESP_OK);
    assert(resets == 1 && host_resets == 1 && dma_starts == 1);
    assert(dpi.cur_fb_index == 1 && bridge.fifo == 0 && waits >= 4);
    assert(atomic_load(&dpi.player_frame_seq) == 19 && bridge.output && host.powered);
    assert(!(bridge.interrupts & MIPI_DSI_BRG_LL_EVENT_VSYNC)); /* no UI callback */
    puts("PASS restart clears leftover pixels, restores timing/color, retains framebuffer and waits for real VSYNC");

    prepare();
    dpi.on_refresh_done = &dpi;
    atomic_store(&dpi.player_frame_seq, UINT_MAX);
    ticks = UINT32_MAX - 2U;
    assert(player_dpi_resume(&dpi.base) == ESP_OK);
    assert(atomic_load(&dpi.player_frame_seq) == 1U);
    assert(bridge.interrupts & MIPI_DSI_BRG_LL_EVENT_VSYNC);
    puts("PASS frame/tick wrap and existing refresh IRQ subscription survive restart");

    prepare();
    generate_vsync = false;
    assert(player_dpi_resume(&dpi.base) == ESP_ERR_TIMEOUT);
    assert(ticks >= 200 && resets == 1 && dma_starts == 1);
    generate_vsync = true;
    assert(player_dpi_resume(&dpi.base) == ESP_OK);
    assert(resets == 1 && dma_starts == 1); /* no reset under a running DMA */
    puts("PASS absent physical VSYNC times out despite DMA start; retry waits without resetting a live stream");

    prepare();
    atomic_store(&dpi.player_sleep_state, 1);
    assert(player_dpi_resume(&dpi.base) == ESP_ERR_INVALID_STATE);
    assert(!resets && !dma_starts);
    puts("PASS in-progress suspend cannot be resumed prematurely");
    return 0;
}
