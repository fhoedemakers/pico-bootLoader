/*
 * sd_boot_ini.h - Parse /boot.txt on the SD root.
 *
 * File format (INI-style, optional). One key=value per line.
 * Lines starting with '#' or ';' are comments. Blank lines and CRLF tolerated.
 *
 *   BASEDIR=/uf2                  (default "/emu")
 *   INDEX=uf2list.txt             (default "emulators.txt")
 *   SCREENSAVER=STARFIELD         (BLOCKS or STARFIELD; default: see below)
 *   GUI=1                         (0 = text menu, 1 = graphical; default 1)
 *   THEME=0                       (active artwork theme 0..9; default 0)
 *
 * BASEDIR is the SD-root folder under which the bootloader looks for the
 * per-HW_CONFIG subdir, the allow-list, and the assets folder.
 * INDEX is the filename of the allow-list inside BASEDIR.
 * SCREENSAVER selects the idle visual mode.
 * GUI and THEME are written back by the firmware -- see sd_boot_ini_save().
 *
 * Screensaver default is asymmetric:
 *   - When /boot.txt is absent altogether -> STARFIELD (historical default).
 *   - When /boot.txt is present but has no SCREENSAVER key -> BLOCKS.
 *   - Explicit SCREENSAVER= wins.
 * This is exactly why sd_boot_ini_save() writes SCREENSAVER= explicitly when it
 * has to create the file: materialising /boot.txt on a card that never had one
 * would otherwise silently flip the screensaver from STARFIELD to BLOCKS.
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
#include <stdint.h>

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
    bool      gui_graphical;   /* GUI=0|1;   default true (graphical) */
    uint8_t   theme;           /* THEME=0..9; default 0 */

    /* Provenance, filled in by the loader. Not user-visible configuration --
     * the writer and the .guimode migration both need to know what was
     * actually present in the file as opposed to defaulted. */
    bool      file_present;
    bool      seen_gui;
    bool      seen_theme;
} sd_boot_ini_t;

typedef enum {
    SD_BOOT_INI_OK,
    SD_BOOT_INI_ERROR,
} sd_boot_ini_status_t;

/* Parse `path`. Also adopts a leftover "<path>.tmp" when `path` itself is
 * missing -- that is the one window sd_boot_ini_save() cannot close by
 * itself (a power cut between unlink and rename), and letting the file
 * silently revert to "absent" would re-fire the SCREENSAVER asymmetry. */
sd_boot_ini_status_t sd_boot_ini_load(const char *path,
                                      sd_boot_ini_t *out,
                                      char *err, size_t err_sz);

/* Rewrite `path` so GUI= and THEME= reflect *ini.
 *
 * File present: only the GUI= / THEME= lines are rewritten, and keys that are
 *   absent get appended. Everything else -- comments, blank lines, ordering,
 *   spacing, CRLF, BASEDIR/INDEX/SCREENSAVER -- is copied through byte for byte.
 * File absent: the file is created carrying the EFFECTIVE value of every key,
 *   SCREENSAVER included (see the asymmetry note at the top of this header).
 *
 * The new file is built as "<path>.tmp", re-parsed with sd_boot_ini_load()
 * before being committed, and only then unlinked+renamed into place -- so a
 * formatting bug or a power cut can never leave /boot.txt in a state that
 * trips the BOOT.TXT INVALID fatal screen. On any failure the original file
 * is left untouched.
 *
 * Returns true on success. Callers should treat failure as non-fatal: a
 * write-protected or full card must not stop the picker from working. */
bool sd_boot_ini_save(const char *path, const sd_boot_ini_t *ini);

#ifdef __cplusplus
}
#endif

#endif /* SD_BOOT_INI_H */
