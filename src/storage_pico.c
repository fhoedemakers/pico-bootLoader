/*
 * storage_pico.c - storage.h implemented on top of the FatFs volume that
 *                  pico_shared already mounts (Frens::initSDCard / f_mount).
 *
 * The bootloader reuses the framework's SD/FatFs stack, so there is no second
 * SD driver here: we only open / stream the chosen UF2 file. The menu builds
 * full paths (e.g. "/emu/8/foo.uf2"), which f_open accepts directly, so the
 * directory-listing half of storage.h is handled in main.cpp via RomLister and
 * left as a thin stub below.
 */
#include "storage.h"

#include <string.h>
#include "ff.h"

static FIL  s_file;
static bool s_file_open = false;

/* The volume is already mounted by Frens::initSDCard() during initAll(). */
bool storage_mount(void)   { return true; }
void storage_unmount(void) { if (s_file_open) storage_close(); }

/* Listing is done in main.cpp with pico_shared's RomLister; not used here. */
bool storage_list_uf2(storage_entry_t *out, int max, int *count)
{
    (void)out; (void)max;
    if (count) *count = 0;
    return false;
}

bool storage_open(const char *name)
{
    if (s_file_open) storage_close();
    if (f_open(&s_file, name, FA_READ) != FR_OK) return false;
    s_file_open = true;
    return true;
}

bool storage_read(void *buf, uint32_t len, uint32_t *read)
{
    if (!s_file_open) return false;
    UINT br = 0;
    FRESULT fr = f_read(&s_file, buf, len, &br);
    if (read) *read = br;
    return fr == FR_OK;
}

bool storage_rewind(void)
{
    if (!s_file_open) return false;
    return f_lseek(&s_file, 0) == FR_OK;
}

void storage_close(void)
{
    if (s_file_open) { f_close(&s_file); s_file_open = false; }
}
