/*
 * setup.c - see setup.h
 */
#include "setup.h"

#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <stdint.h>

#include "ff.h"
#include "boot_config.h"
#include "uf2_loader.h"

const char *setup_result_str(setup_result_t r)
{
    switch (r) {
    case SETUP_OK:               return "OK";
    case SETUP_NO_LAYOUT:        return "layout.txt not found";
    case SETUP_BAD_LAYOUT:       return "layout.txt malformed";
    case SETUP_UF2_NOT_FOUND:    return "UF2 file missing on SD";
    case SETUP_UF2_WRONG_SLOT:   return "UF2 linked for wrong slot";
    case SETUP_FLASH_FAILED:     return "flash erase/program failed";
    default:                     return "unknown";
    }
}

/* Strip leading whitespace; return pointer to first non-space char. */
static char *lstrip(char *s)
{
    while (*s == ' ' || *s == '\t') s++;
    return s;
}

setup_result_t setup_read_layout(const char *emu_dir,
                                 setup_entry_t *out, int *out_count)
{
    char path[64];
    snprintf(path, sizeof(path), "%s/layout.txt", emu_dir);

    FIL fil;
    if (f_open(&fil, path, FA_READ) != FR_OK)
        return SETUP_NO_LAYOUT;

    int n = 0;
    char line[128];
    while (f_gets(line, sizeof(line), &fil)) {
        char *p = lstrip(line);
        if (*p == '#' || *p == '\n' || *p == '\r' || *p == '\0') continue;

        int slot = -1;
        char fname[SETUP_FNAME_MAX] = {0};
        /* Expect: "<slot> <filename>"; filename takes the rest of the line. */
        if (sscanf(p, "%d %79s", &slot, fname) != 2) { f_close(&fil); return SETUP_BAD_LAYOUT; }
        if (slot < 0 || slot >= (int)SLOT_COUNT)     { f_close(&fil); return SETUP_BAD_LAYOUT; }
        if (n >= SETUP_MAX_ENTRIES)                   { f_close(&fil); return SETUP_BAD_LAYOUT; }

        out[n].slot = slot;
        strncpy(out[n].filename, fname, SETUP_FNAME_MAX - 1);
        out[n].filename[SETUP_FNAME_MAX - 1] = '\0';
        n++;
    }
    f_close(&fil);

    if (out_count) *out_count = n;
    return SETUP_OK;
}

setup_result_t setup_flash_all(const char *emu_dir,
                               const setup_entry_t *entries, int count,
                               setup_progress_cb progress)
{
    /* ---- Pre-flight every entry (no flash writes) ---- */
    for (int i = 0; i < count; i++) {
        char full[128];
        snprintf(full, sizeof(full), "%s/%s", emu_dir, entries[i].filename);

        uint32_t base = SLOT_BASE(entries[i].slot);
        uf2_load_stats_t st;
        uf2_load_result_t r = uf2_validate_file_to(full, base, SLOT_SIZE, &st);
        if (r == UF2_LOAD_OPEN_FAILED)
            return SETUP_UF2_NOT_FOUND;
        if (r != UF2_LOAD_OK)
            /* The most common failure is target_addr outside the slot range,
             * i.e. the UF2 was built for a different slot than layout.txt says. */
            return SETUP_UF2_WRONG_SLOT;
    }

    /* ---- Commit: flash each slot in turn ---- */
    for (int i = 0; i < count; i++) {
        char full[128];
        snprintf(full, sizeof(full), "%s/%s", emu_dir, entries[i].filename);
        uint32_t base = SLOT_BASE(entries[i].slot);

        if (progress) progress(entries[i].slot, "Starting", 0, 1, entries[i].filename);

        uf2_load_stats_t st;
        uf2_load_result_t r = uf2_load_file_to(full, base, SLOT_SIZE, &st, NULL);
        if (r != UF2_LOAD_OK)
            return SETUP_FLASH_FAILED;

        if (progress) progress(entries[i].slot, "Done", 1, 1, entries[i].filename);
    }
    return SETUP_OK;
}
