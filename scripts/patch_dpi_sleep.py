"""Generate a project-local DMA-boundary suspend and synchronized resume extension for IDF 5.5.4.

Fail on changed anchors rather than silently compile a partially patched DMA
callback after an SDK upgrade. Never edit the installed SDK or its dependencies.
"""
import pathlib
import sys

source, extension, output = map(pathlib.Path, sys.argv[1:])
text = source.read_text(encoding="utf-8")

def replace_once(old, new):
    global text
    if text.count(old) != 1:
        raise SystemExit(f"DPI sleep extension: SDK anchor changed: {old[:80]!r}")
    text = text.replace(old, new, 1)

replace_once('#include <sys/param.h>', '#include <sys/param.h>\n#include <stdatomic.h>\n#include "freertos/FreeRTOS.h"\n#include "freertos/task.h"')
replace_once('    esp_lcd_panel_t base;         // Base class of generic lcd panel',
             '    atomic_uint player_frame_seq; // Real bridge VSYNC events, not DMA completions\n'
             '    atomic_uint player_sleep_state; // 0=run, 1=request, 3=ISR stopping, 2=stopped\n'
             '    esp_lcd_panel_t base;         // Base class of generic lcd panel')
replace_once('    // restart the DMA transfer, keep refreshing the LCD', '''    unsigned expected = 1;
    bool player_stopping = atomic_compare_exchange_strong(
        &dpi_panel->player_sleep_state, &expected, 3);
    if (player_stopping) {
        /* DMA completion only means pixels reached the bridge FIFO. It is
         * NOT a physical VSYNC boundary. Brightness is already zero here;
         * resume must reset that FIFO and the scan state before rearming. */
        mipi_dsi_hal_context_t *hal = &dpi_panel->bus->hal;
        mipi_dsi_brg_ll_enable_interrupt(hal->bridge,
            MIPI_DSI_BRG_LL_EVENT_UNDERRUN | MIPI_DSI_BRG_LL_EVENT_VSYNC, false);
        mipi_dsi_brg_ll_enable_dpi_output(hal->bridge, false);
        mipi_dsi_brg_ll_update_dpi_config(hal->bridge);
        mipi_dsi_host_ll_enable_video_mode(hal->host, false);
    } else {
    // restart the DMA transfer, keep refreshing the LCD''')
# Deliver on_refresh_done from the DMA transfer-done ISR. This is the point
# where the driver actually latches cur_fb_index and rearms the display DMA;
# the bridge VSYNC can arrive before that switch commits, which would let the
# LVGL port start drawing into the buffer the panel is still scanning.
replace_once('    dw_gdma_channel_enable_ctrl(chan, true);', '''    dw_gdma_channel_enable_ctrl(chan, true);
    /* Project extension: cur_fb_index has just been latched and the DMA has
     * been rearmed for the next frame buffer. Release the LVGL port only now,
     * not on VSYNC, so it can never start drawing into the buffer that the
     * panel or its FIFO is still consuming. */
    if (dpi_panel->on_refresh_done) {
        if (dpi_panel->on_refresh_done(&dpi_panel->base, NULL, dpi_panel->user_ctx)) {
            yield_needed = true;
        }
    }
    }''')
replace_once('''#if !MIPI_DSI_BRG_LL_EVENT_VSYNC
    // the DMA descriptor is large enough to carry a whole frame buffer, so this event can also be treated as a fake "vsync end"
    if (dpi_panel->on_refresh_done) {
        if (dpi_panel->on_refresh_done(&dpi_panel->base, NULL, dpi_panel->user_ctx)) {
            yield_needed = true;
        }
    }
#endif
''', '')
replace_once('''    return yield_needed;
}

void mipi_dsi_bridge_isr_handler''', '''    if (player_stopping)
        atomic_store_explicit(&dpi_panel->player_sleep_state, 2, memory_order_release);
    return yield_needed;
}

void mipi_dsi_bridge_isr_handler''')
# A real bridge VSYNC is still required for the first visible frame after restart.
# P4 rev < 3.0 lacks both this event and the bridge reset used by the extension;
# on rev >= 3.0 the event now only advances the wake barrier sequence.
replace_once('''    if (intr_status & MIPI_DSI_BRG_LL_EVENT_VSYNC) {
        if (dpi_panel->on_refresh_done) {
            if (dpi_panel->on_refresh_done(&dpi_panel->base, NULL, dpi_panel->user_ctx)) {
                portYIELD_FROM_ISR();
            }
        }
    }''', '''    if (intr_status & MIPI_DSI_BRG_LL_EVENT_VSYNC) {
        if (atomic_load_explicit(&dpi_panel->player_sleep_state, memory_order_acquire) == 0)
            atomic_fetch_add_explicit(&dpi_panel->player_frame_seq, 1, memory_order_release);
    }''')
text += '\n' + extension.read_text(encoding='utf-8-sig')
output.parent.mkdir(parents=True, exist_ok=True)
if not output.exists() or output.read_text(encoding='utf-8') != text:
    output.write_text(text, encoding='utf-8')
