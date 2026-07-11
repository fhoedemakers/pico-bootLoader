#include "progress_bar.h"

#include "FrensHelpers.h"

// Bar geometry, fixed at compile time. The "Flashing… / <name> / Do not
// power off." text from showMessage() renders at char rows 12/14/16 (pixel
// rows 96..135). Park the bar comfortably below it at y=160..179, centred
// horizontally with a 40-pixel margin on each side.
#define PB_X       40
#define PB_Y       160
#define PB_W       240
#define PB_H       20

// __not_in_flash_func: invoked from the flash callback. During the flash op,
// core1 is still alive servicing HSTX scanlines and any code on core1's path
// is in SRAM. This function on core0 is also called between bootrom flash
// calls (XIP is restored by then) but we keep it in SRAM anyway to remove
// every code-path question.
extern "C" void __not_in_flash_func(progress_bar_draw)(uint32_t numer, uint32_t denom,
                                                      uint16_t col_fill,
                                                      uint16_t col_empty,
                                                      uint16_t col_border)
{
    // Pick the framebuffer pointer for whichever display backend is active.
    uint16_t *fb;
#if HSTX
    fb = (uint16_t *)hstx_getframebuffer();
#else
  #if FRAMEBUFFERISPOSSIBLE
    if (!Frens::isFrameBufferUsed()) return;   // line-stream mode -- no FB to update
    fb = Frens::framebuffer;
  #else
    (void)numer; (void)denom; (void)col_fill; (void)col_empty; (void)col_border;
    return;                                    // RP2040 PicoDVI line-stream: no FB
  #endif
#endif
    if (!fb) return;

    // How much of the interior is filled?  Interior = BAR_W - 2 (subtract the
    // 1-px left + right border). Clamp so a >100% numer never overruns.
    uint32_t fill_px = (denom > 0) ? (numer * (PB_W - 2) / denom) : 0;
    if (fill_px > (uint32_t)(PB_W - 2)) fill_px = PB_W - 2;

    // Top + bottom borders: solid horizontal stripe.
    for (int x = 0; x < PB_W; x++) {
        fb[ PB_Y                * 320 + PB_X + x] = col_border;
        fb[(PB_Y + PB_H - 1)    * 320 + PB_X + x] = col_border;
    }

    // Interior rows: left border, filled portion, empty portion, right border.
    for (int y = 1; y < PB_H - 1; y++) {
        uint16_t *row = fb + (PB_Y + y) * 320 + PB_X;
        row[0]         = col_border;
        for (int x = 1; x <= (int)fill_px; x++)         row[x] = col_fill;
        for (int x = (int)fill_px + 1; x < PB_W - 1; x++) row[x] = col_empty;
        row[PB_W - 1]  = col_border;
    }
}
