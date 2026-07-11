#pragma once

#include <cstdint>

// If <dir>/<basename>.444 and .555 both already exist, returns true immediately.
// Otherwise looks for <dir>/<basename>.png / .jpg / .jpeg (in that order), decodes
// it (streaming row-by-row via PNGdec / JPEGDEC), downscales preserving aspect
// ratio into a canvas sized by (max_w, max_h), packs as RGB444 and RGB555, and
// writes both cache files atomically (.tmp + rename). Never upscales; smaller
// sources are copied 1:1.
//
//   letterbox=true  : output is exactly max_w x max_h, image centred on black.
//                     Used by the picker (fixed 320x240 tile format).
//   letterbox=false : output is the actual scaled dst_w x dst_h (both <= max_).
//                     Used by the screensaver (per-sprite dims in file header).
//
// Works on both PSRAM and SRAM-only builds; peak footprint per conversion is
// ~53 KB (PNG) / ~18 KB (JPEG), so on SRAM-only boards call it while that much
// heap is still free (see main.cpp's boot-time batch).
//
// Sources that can never convert (progressive JPEG, interlaced or
// 16-bit-channel PNG, corrupt data, larger than 1280x960) are MOVED to
// <dir>/unsupported/ so scans stop retrying them on every boot. Transient
// failures (out of heap, SD I/O errors) leave the source in place.
// Returns false on any decode / write failure.
bool image_convert_ensure(const char *dir, const char *basename,
                          uint16_t max_w, uint16_t max_h, bool letterbox);

// Convert every <dir>/*.png|.jpg|.jpeg that lacks its .444/.555 cache, drawing
// a progress bar plus the current filename into the active framebuffer
// (ui_title is the heading; runs silently when no framebuffer is available).
// Returns the number of images converted (0 = nothing needed doing), or -1
// when the directory can't be opened / scratch allocation fails.
int image_convert_batch_dir(const char *dir, uint16_t max_w, uint16_t max_h,
                            bool letterbox, const char *ui_title);
