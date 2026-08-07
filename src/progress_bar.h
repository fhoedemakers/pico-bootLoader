/*
 * progress_bar.h - SRAM-resident horizontal progress bar for the bootloader.
 *
 * Why it lives in its own translation unit: this function is called from
 * flashProgress() during a flash write, and any flash-resident code on
 * core1's hot path or in the callback itself can hard-fault when XIP is
 * disabled. Putting the renderer in src/progress_bar.c (and marking it
 * __not_in_flash_func) keeps the whole code path SRAM-clean.
 *
 * The bar is drawn directly into the active video framebuffer:
 *   - HSTX builds   : hstx_getframebuffer()        (SRAM static array)
 *   - PicoDVI+FB    : Frens::framebuffer           (SRAM static array)
 *   - PicoDVI line  : function is a no-op (RP2040 line-stream mode has no
 *                     persistent framebuffer to update from outside the
 *                     scanline callback).
 *
 * All colour values must be passed in by the caller (pre-fetched from
 * NesMenuPalette[] before the flash op starts), so this function never
 * reads anything from flash itself.
 */
#ifndef PROGRESS_BAR_H
#define PROGRESS_BAR_H

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Draw / update the progress bar. Pass the numerator and denominator of the
 * progress fraction in arbitrary units (e.g. numer=37, denom=100). The bar's
 * geometry is fixed; only the fill width changes.
 *
 * A "nn%" label (numer/denom as a rounded-down percentage) is drawn in a
 * fixed-width field immediately below the bar, at pixel rows 182..189.
 * Callers must leave that band free.
 *
 * col_fill   - 16-bit pixel value for the filled part of the bar
 * col_empty  - 16-bit pixel value for the empty part of the bar, and for the
 *              background of the percentage label
 * col_border - 16-bit pixel value for the 1-px border around the bar, and for
 *              the percentage label's text
 *
 * Pixel format follows whatever the active backend uses (RGB444 for
 * PicoDVI, RGB555 for HSTX). The values are written as opaque uint16_t,
 * so the caller is responsible for picking the right encoding.
 */
void progress_bar_draw(uint32_t numer, uint32_t denom,
                       uint16_t col_fill,
                       uint16_t col_empty,
                       uint16_t col_border);

#ifdef __cplusplus
}
#endif

#endif /* PROGRESS_BAR_H */
