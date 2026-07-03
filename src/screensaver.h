/*
 * screensaver.h - Idle screensaver for the pico-bootLoader picker.
 *
 * After 30 s of input idle the picker calls screensaver_init() to load up to
 * 5 small images from /emu/assets/screensaver/, then screensaver_run_one_frame()
 * each frame to advance the bouncing sprites and paint the active display
 * backend (HSTX framebuffer, picoDVI framebuffer, or picoDVI line stream).
 * Any button press exits and the picker calls screensaver_free() to release
 * the per-sprite buffers.
 *
 * Images use the same raw 16-bit format gui_load_image() expects (4-byte
 * width/height header then w*h*2 pixel bytes) but with arbitrary dimensions;
 * any file wider than 55 px or taller than 60 px is skipped.
 */
#ifndef SCREENSAVER_H
#define SCREENSAVER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Scan /emu/assets/screensaver, pick up to 5 random images that fit the size
 * limits, allocate per-sprite RAM, randomise position/velocity. Returns true
 * when at least one sprite was successfully loaded. Idempotent only in the
 * sense of "safe to call again after screensaver_free()". */
bool screensaver_init(void);

/* Advance physics one step and composite the sprites onto the active display
 * backend. Cheap (5 small sprites, ~33 KB pixel data max). */
void screensaver_run_one_frame(void);

/* Release every sprite buffer and reset internal state. Safe to call when
 * init was never called or returned false. */
void screensaver_free(void);

#ifdef __cplusplus
}
#endif

#endif /* SCREENSAVER_H */
