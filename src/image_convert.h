#pragma once

#include <cstdint>

// If <dir>/<basename>.444 and .555 both already exist, returns true immediately.
// Otherwise looks for <dir>/<basename>.png / .jpg / .jpeg (in that order), decodes
// it, downscales preserving aspect ratio into a canvas sized by (max_w, max_h),
// packs as RGB444 and RGB555, and writes both cache files atomically (.tmp +
// rename). Never upscales; smaller sources are copied 1:1.
//
//   letterbox=true  : output is exactly max_w x max_h, image centred on black.
//                     Used by the picker (fixed 320x240 tile format).
//   letterbox=false : output is the actual scaled dst_w x dst_h (both <= max_).
//                     Used by the screensaver (per-sprite dims in file header).
//
// Requires PSRAM; returns false immediately if Frens::isPsramEnabled() is false.
// Returns false on any decode / write failure.
bool image_convert_ensure(const char *dir, const char *basename,
                          uint16_t max_w, uint16_t max_h, bool letterbox);
