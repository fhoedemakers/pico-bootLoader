/*
 * sd_boot_ini.h - Parse /boot.txt on the SD root.
 *
 * File format (INI-style, optional). One key=value per line.
 * Lines starting with '#' or ';' are comments. Blank lines and CRLF tolerated.
 *
 *   BASEDIR=/uf2                  (default "/emu")
 *   INDEX=uf2list.txt             (default "emulators.txt")
 *   SCREENSAVER=STARFIELD         (BLOCKS or STARFIELD; default: see below)
 *
 * BASEDIR is the SD-root folder under which the bootloader looks for the
 * per-HW_CONFIG subdir, the allow-list, the assets folder, and .guimode.
 * INDEX is the filename of the allow-list inside BASEDIR.
 * SCREENSAVER selects the idle visual mode.
 *
 * Screensaver default is asymmetric:
 *   - When /boot.txt is absent altogether -> STARFIELD (historical default).
 *   - When /boot.txt is present but has no SCREENSAVER key -> BLOCKS.
 *   - Explicit SCREENSAVER= wins.
 *
 * Return values:
 *   SD_BOOT_INI_OK    - out[] is populated (either defaults, or parsed).
 *   SD_BOOT_INI_ERROR - a parse rule failed. err[] holds a single-line
 *                       diagnostic ready to display on a fatal screen.
 */
#ifndef SD_BOOT_INI_H
#define SD_BOOT_INI_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    SS_MODE_BLOCKS    = 0,   /* bouncing sprites */
    SS_MODE_STARFIELD = 1,   /* pin-hole depth projection */
} ss_mode_t;

typedef struct {
    char      base_dir[64];    /* no trailing slash, e.g. "/emu" */
    char      index_file[64];  /* bare filename, e.g. "emulators.txt" */
    ss_mode_t screensaver;
} sd_boot_ini_t;

typedef enum {
    SD_BOOT_INI_OK,
    SD_BOOT_INI_ERROR,
} sd_boot_ini_status_t;

sd_boot_ini_status_t sd_boot_ini_load(const char *path,
                                      sd_boot_ini_t *out,
                                      char *err, size_t err_sz);

#ifdef __cplusplus
}
#endif

#endif /* SD_BOOT_INI_H */
