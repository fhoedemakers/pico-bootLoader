/*
 * emulators_txt.h - Parse /emu/emulators.txt on the SD card.
 *
 * File format: one record per line, three fields separated by ';':
 *
 *   <program_name>;<image_key>;<display_name>
 *
 * Example:
 *   picogenesisPlus;md;Sega Genesis/Mega Drive
 *
 * - program_name matches the value extracted from each UF2's binary_info
 *   (see program_name.h). Comparison is case-insensitive.
 * - image_key picks the artwork file under /emu/assets/<key>.444 (PicoDVI)
 *   or .555 (HSTX).
 * - display_name is the human-readable label shown in the text menu.
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

/* Look up by program_name. Writes the matching image_key and display_name into
 * the caller's buffers (always NUL-terminated). Returns true on match. */
bool emulators_txt_lookup(const char *prog_name,
                          char *image_key, size_t key_sz,
                          char *display_name, size_t name_sz);

#ifdef __cplusplus
}
#endif

#endif /* EMULATORS_TXT_H */
