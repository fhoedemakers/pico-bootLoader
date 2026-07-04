/*
 * screensaver.h - Idle screensaver for the pico-bootLoader picker.
 *
 * After 30 s of input idle the picker calls screensaver_init() to load small
 * images from <BASEDIR>/assets/screensaver/ (default BASEDIR = "/emu";
 * overridable at runtime via screensaver_set_asset_dir() from /boot.txt),
 * then screensaver_run_one_frame() each frame to advance the sprites and
 * paint the active display backend (HSTX framebuffer, picoDVI framebuffer,
 * or picoDVI line stream). Any button press exits and the picker calls
 * screensaver_free() to release the per-sprite buffers.
 *
 * Visual mode (BLOCKS = bouncing sprites, STARFIELD = pin-hole depth
 * projection) is selectable at runtime via screensaver_set_mode(). Default
 * is STARFIELD.
 *
 * Images use the same raw 16-bit format gui_load_image() expects (4-byte
 * width/height header then w*h*2 pixel bytes) but with arbitrary dimensions;
 * any file wider than 55 px or taller than 60 px is skipped.
 */
#ifndef SCREENSAVER_H
#define SCREENSAVER_H

#include <stdbool.h>

#include "sd_boot_ini.h"   /* ss_mode_t */

#ifdef __cplusplus
extern "C" {
#endif

/* Override the asset base directory. Setter appends "/assets/screensaver".
 * NULL / empty input is a silent no-op. Default base is "/emu". */
void screensaver_set_asset_dir(const char *base);

/* Choose visual mode. Default is SS_MODE_STARFIELD. */
void screensaver_set_mode(ss_mode_t m);

/* Scan the asset dir, pick random images that fit the size limits, allocate
 * per-sprite RAM, randomise position/velocity. Returns true when at least
 * one sprite was successfully loaded. Idempotent only in the sense of
 * "safe to call again after screensaver_free()". */
bool screensaver_init(void);

/* Advance physics one step and composite the sprites onto the active display
 * backend. */
void screensaver_run_one_frame(void);

/* Release every sprite buffer and reset internal state. Safe to call when
 * init was never called or returned false. */
void screensaver_free(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREENSAVER_H */
