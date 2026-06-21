/*
 * setup.h - One-shot "rebuild flash" flow for slot-mode boards.
 *
 * Reads /emu/<HW>/layout.txt (slot <-> uf2-filename mapping) and flashes each
 * row into its assigned pinned slot. Pre-flights every UF2 (target_addr must
 * fall inside the expected slot) before touching flash, so a misbuilt UF2
 * rejects cleanly without corrupting anything.
 *
 * The flow is destructive and slow (the same "core1 reset / blank screen /
 * flash" cost as today's single-shot loader, but run N times). Only invoked
 * explicitly via the menu, never automatically.
 */
#ifndef SETUP_H
#define SETUP_H

#include <stdint.h>
#include <stdbool.h>

#define SETUP_MAX_ENTRIES 8   /* upper bound on layout.txt rows we accept */
#define SETUP_FNAME_MAX   80

typedef struct {
    int  slot;                       /* 0..SLOT_COUNT-1 */
    char filename[SETUP_FNAME_MAX];  /* basename only; setup prepends /emu/<HW>/ */
} setup_entry_t;

typedef enum {
    SETUP_OK = 0,
    SETUP_NO_LAYOUT,        /* layout.txt absent or unreadable        */
    SETUP_BAD_LAYOUT,       /* malformed row / out-of-range slot      */
    SETUP_UF2_NOT_FOUND,    /* a named UF2 is missing on SD           */
    SETUP_UF2_WRONG_SLOT,   /* UF2's target_addr doesn't match slot   */
    SETUP_FLASH_FAILED,     /* erase/program/verify error during flash */
} setup_result_t;

typedef void (*setup_progress_cb)(int slot, const char *phase,
                                  uint32_t done, uint32_t total,
                                  const char *filename);

/*
 * Read /emu/<dir>/layout.txt. On success returns SETUP_OK and writes the row
 * count to *out_count. Lines: "<slot>  <uf2-filename>"; '#' starts a comment.
 */
setup_result_t setup_read_layout(const char *emu_dir,
                                 setup_entry_t *out, int *out_count);

/*
 * Flash every entry in `entries` into its assigned slot. Pre-flights each UF2
 * first; if any row fails pre-flight, returns without touching flash. Once the
 * destructive phase starts, calls `progress` once per phase/slot. Caller is
 * expected to have reset core1 BEFORE calling.
 */
setup_result_t setup_flash_all(const char *emu_dir,
                               const setup_entry_t *entries, int count,
                               setup_progress_cb progress);

const char *setup_result_str(setup_result_t r);

#endif /* SETUP_H */
