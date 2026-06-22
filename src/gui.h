/*
 * gui.h - Graphical menu support for pico_emuLoader.
 *
 * Loads 320x240 raw 16-bit artwork from /emu/assets/<key>.{444|555} (extension
 * picked by the shared FILEXTFORSEARCH macro), holds two reusable image
 * buffers in PSRAM (current + incoming), composes a horizontal slide between
 * them per scanline, and writes the result straight into whichever framebuffer
 * the active display backend (PicoDVI line-stream / PicoDVI framebuffer / HSTX
 * framebuffer) is using.
 *
 * Pure image: nothing else is drawn on the screen in graphical mode. Text
 * overlay (display name, in-flash marker, etc.) is intentionally absent.
 *
 * Mode persistence: a one-byte file ('0' / '1') on the SD card remembers
 * whether the user last left the menu in text or graphical mode.
 */
#ifndef GUI_H
#define GUI_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Mode persistence ('0' text / '1' graphical). gui_load_mode() returns true
 * (graphical) when the file is missing -- first-boot default. */
bool gui_load_mode(const char *path);
void gui_save_mode(const char *path, bool graphical);

/* Allocate the two image buffers (current + incoming) from PSRAM-backed
 * Frens::f_malloc. Idempotent. Returns false if either allocation fails. */
bool gui_buffers_alloc(void);

/* Buffer accessors. Caller drives the slide animation and swaps with
 * gui_swap_buffers() once the new image is fully on screen. */
uint16_t *gui_buf_cur(void);
uint16_t *gui_buf_next(void);
void      gui_swap_buffers(void);

/* Load /emu/assets/<image_key>.<FILEXTFORSEARCH> into the 320x240 buffer.
 * Returns false if the file is missing, the header is wrong, or the read
 * comes up short. */
bool gui_load_image(const char *image_key, uint16_t *dest_320x240);

/* Fill a 320x240 buffer with a solid colour (used as a missing-asset
 * placeholder so we never have to crash on a bad SD layout). */
void gui_fill_solid(uint16_t *dest_320x240, uint16_t color);

/* Draw one frame to the active backend.
 *
 *   a, b        : current and incoming 320x240 images. b may be NULL when no
 *                 slide is in progress (slide_px is then ignored).
 *   slide_px    : 0..320, number of pixels of b currently revealed.
 *   direction   : +1 = b enters from the right (RIGHT pressed),
 *                 -1 = b enters from the left  (LEFT pressed),
 *                  0 = no slide; only `a` is drawn.
 */
void gui_draw_frame(const uint16_t *a, const uint16_t *b,
                    int slide_px, int direction);

#ifdef __cplusplus
}
#endif

#endif /* GUI_H */
