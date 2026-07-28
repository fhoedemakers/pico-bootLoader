/*
 * themes.h - Artwork themes for the pico-bootLoader picker.
 *
 * Menu artwork lives in numbered theme folders:
 *
 *   <BASEDIR>/assets/themes/0/<image_key>.png   default theme (also the fallback)
 *   <BASEDIR>/assets/themes/1..9/               optional alternative themes
 *
 * <image_key> is column 2 of the index file (see emulators_txt.h). Theme 0 is
 * special: it is the default, and it is what gui.cpp falls back to when the
 * active theme has no artwork for a given key.
 *
 * Screensaver images are NOT themed -- they stay in <BASEDIR>/assets/screensaver
 * and are reached through screensaver_set_asset_dir(), which builds its own path.
 *
 * Migration: cards written for earlier releases keep their artwork loose in
 * <BASEDIR>/assets. themes_migration_needed() detects that layout and
 * themes_migrate_default() moves the image files down into themes/0, cache
 * files included so nothing has to be re-converted. The move is crash-safe --
 * see themes_migrate_default().
 */
#ifndef THEMES_H
#define THEMES_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define THEME_MAX 10   /* themes 0..9 */

/* Remember "<base>/assets" and "<base>/assets/themes". Call once at boot,
 * right after gui_set_asset_dir(). NULL / empty input is a silent no-op. */
void themes_init(const char *base_dir);

/* True when a migration is pending: the themes/0 folder is missing, holds no
 * files, or a .migrating marker from an interrupted run is still present.
 * Cheap enough (one f_stat + one f_opendir + a few f_readdir) to call before
 * deciding whether to put a notice on screen. */
bool themes_migration_needed(void);

/* Move every loose image file in <base>/assets into <base>/assets/themes/0.
 *
 * Only names ending in .png/.jpg/.jpeg/.444/.555 are moved -- unrelated files
 * the user parked there are left alone. Subdirectories (screensaver/,
 * unsupported/, themes/) are skipped via the directory attribute, not by name.
 *
 * Crash-safe: a .migrating marker is written before the first rename and
 * removed after the last, so an interrupted run is detected and resumed on the
 * next boot even though themes/0 is no longer empty.
 *
 * Returns the number of files moved, or -1 on error. */
int themes_migrate_default(void);

/* Rescan <base>/assets/themes for subdirectories named "0".."9". Returns the
 * bitmask of present themes (bit N = theme N exists) and caches it. */
uint32_t themes_scan(void);

uint32_t themes_mask(void);           /* cached result of the last themes_scan() */
int      themes_count(void);          /* number of themes present */
bool     themes_exists(int n);
int      themes_lowest(void);         /* lowest present theme, or -1 if none */

/* Next present theme in the given direction (+1 / -1), wrapping and skipping
 * absent slots. Returns cur unchanged when fewer than two themes exist. */
int      themes_next(int cur, int dir);

int      themes_active(void);
void     themes_set_active(int n);    /* clamped to a theme that exists */

/* Write "<base>/assets/themes/<n>" into out. Caller-owned buffer (>= 80 bytes)
 * on purpose: callers need to hold two theme paths at once. */
void     themes_dir_of(int n, char *out, size_t out_sz);

/* Batch-convert any PNG/JPG lacking its .444/.555 cache in EVERY present theme.
 * Boards without PSRAM must call this at boot: the converter needs ~53 KB of
 * SRAM heap that stops being available once the GUI slide buffers are
 * allocated, so a theme converted lazily at switch time would fail. */
void     themes_convert_all(void);

#ifdef __cplusplus
}
#endif

#endif /* THEMES_H */
