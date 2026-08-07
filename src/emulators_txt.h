/*
 * emulators_txt.h - Parse /emu/emulators.txt on the SD card.
 *
 * File format: one record per line, three or four fields separated by ';':
 *
 *   <program_name>;<image_key>;<display_name>[;<aux_uf2>]
 *
 * Example (three columns):
 *   picogenesisPlus;md;Sega Genesis/Mega Drive
 *
 * Example (four columns - emulator with a companion data blob):
 *   doom_tiny;doom;Doom!;doom1-whx.uf2
 *
 * - program_name matches the value extracted from each UF2's binary_info
 *   (see program_name.h). Comparison is case-insensitive.
 * - image_key picks the artwork file under /emu/assets/themes/<N>/<key>.444 (PicoDVI)
 *   or .555 (HSTX).
 * - display_name is the human-readable label shown in the text menu.
 * - aux_uf2 (optional) names a second .uf2 in the same config dir that
 *   carries read-only data at its own target address (e.g. a Doom WAD
 *   packed as a DATA-family UF2). The bootloader flashes it alongside the
 *   emulator .uf2 if the in-flash bytes drift, else skips.
 *
 * Lines starting with '#' and blank lines are ignored. CR is tolerated.
 */
#ifndef EMULATORS_TXT_H
#define EMULATORS_TXT_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Read and parse the file. Safe to call once at startup. Returns false if the
 * file is missing or unreadable; in that case lookups will always miss. */
bool emulators_txt_load(const char *path);

/* Look up by program_name. Writes the matching image_key, display_name, and
 * (optional) aux_uf2 into the caller's buffers (always NUL-terminated;
 * aux_uf2 becomes "" if the row has no 4th column). Any of the output
 * pointers may be NULL. Returns true on match. */
bool emulators_txt_lookup(const char *prog_name,
                          char *image_key, size_t key_sz,
                          char *display_name, size_t name_sz,
                          char *aux_uf2, size_t aux_sz);

#ifdef __cplusplus
}
#endif

#endif /* EMULATORS_TXT_H */
