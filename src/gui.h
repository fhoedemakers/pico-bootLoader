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
 * The artwork is otherwise un-decorated -- display name, in-flash marker,
 * etc. are intentionally absent so the picture fills the screen. The only
 * overlay drawn on top is a single 8-pixel button-hint footer at the very
 * bottom, set via gui_set_footer() and composed into each affected scanline
 * inside gui_draw_frame() so it renders uniformly across all backends
 * (including PicoDVI line-stream, which has no persistent framebuffer).
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

/* Allocate the image buffers. The cur buffer is always full resolution
 * (320x240, ~150 KB). The next buffer is full resolution when PSRAM is
 * available, or half resolution (160x120, ~38 KB) when it isn't -- the
 * smaller buffer keeps the slide animation working on SRAM-only configs
 * at the cost of a brief blocky look during the transition.
 * Idempotent. Returns false only if the cur buffer can't be allocated. */
bool gui_buffers_alloc(void);

/* True when the next buffer is at half resolution -- caller must use the
 * _half_res() loaders / fillers and tell gui_draw_frame() about it. */
bool gui_next_is_half_res(void);

/* Buffer accessors. Caller drives the slide animation and swaps with
 * gui_swap_buffers() once the new image is fully on screen. Note: swap
 * is only safe when both buffers are the same resolution (PSRAM mode);
 * in half-res mode the caller should instead reload cur at full res
 * after the slide completes. */
uint16_t *gui_buf_cur(void);
uint16_t *gui_buf_next(void);
void      gui_swap_buffers(void);

/* Load /emu/assets/<image_key>.<FILEXTFORSEARCH> into the 320x240 buffer.
 * Returns false if the file is missing, the header is wrong, or the read
 * comes up short. */
bool gui_load_image(const char *image_key, uint16_t *dest_320x240);

/* Load the same artwork but downsample 2x in each axis on the fly, writing
 * a 160x120 image into dest. Uses a single 640-byte scanline scratch on
 * the stack -- no large temporary buffer required, which is the whole
 * point of the half-res path. */
bool gui_load_image_half_res(const char *image_key, uint16_t *dest_160x120);

/* Fill a 320x240 buffer with a solid colour (used as a missing-asset
 * placeholder so we never have to crash on a bad SD layout). */
void gui_fill_solid(uint16_t *dest_320x240, uint16_t color);

/* Half-res variant of the placeholder fill (160x120). */
void gui_fill_solid_half_res(uint16_t *dest_160x120, uint16_t color);

/* Draw one frame to the active backend.
 *
 *   a, b        : current image (full 320x240) and incoming image. b may be
 *                 NULL when no slide is in progress (slide_px is then ignored).
 *   slide_px    : 0..320, number of pixels of b currently revealed.
 *   direction   : +1 = b enters from the right (RIGHT pressed),
 *                 -1 = b enters from the left  (LEFT pressed),
 *                  0 = no slide; only `a` is drawn.
 *   b_half_res  : b is a 160x120 image instead of 320x240; the function
 *                 expands it 2x per axis on the fly. Pass false if b is
 *                 full resolution or NULL.
 */
void gui_draw_frame(const uint16_t *a, const uint16_t *b,
                    int slide_px, int direction, bool b_half_res);

/* Set the one-line button-hint footer drawn at the bottom of every
 * graphical frame. Pass NULL or "" to hide the band. The caller-owned
 * string must remain valid until the next call -- gui only caches the
 * pointer. */
void gui_set_footer(const char *text);

#ifdef __cplusplus
}
#endif

#endif /* GUI_H */
