/*
 * uf2_loader.c - see uf2_loader.h
 *
 * Strategy (two passes over the file):
 *   Pass 1  - validate every block and compute the exact flash address range
 *             the image occupies, so we erase only what we need.
 *   (erase) - erase that range once (sector granularity).
 *   Pass 2  - program each in-range block, then read it back to verify.
 *
 * The pure per-block decision (in range? right family? where does it go?) lives
 * in uf2_classify_block() in uf2_format.h and is covered by host unit tests.
 */
#include "uf2_loader.h"

#include <string.h>
#include "boot_config.h"
#include "uf2_format.h"
#include "storage.h"
#include "flash_writer.h"
#include "hardware/flash.h" /* FLASH_PAGE_SIZE */

#ifndef EXPECTED_UF2_FAMILY
#define EXPECTED_UF2_FAMILY UF2_FAMILY_RP2350_ARM_S
#endif

/* Runtime upper bound for the app partition. Defaults to the build-time
 * APP_END_ADDR; main() narrows it to the real chip's capacity once the SD/PSRAM
 * init has run and storage_get_flash_capacity() is known good. */
static uint32_t g_app_end_addr = APP_END_ADDR;

void uf2_loader_set_app_end_addr(uint32_t app_end_addr)
{
    if (app_end_addr > APP_BASE_ADDR && app_end_addr <= APP_END_ADDR)
        g_app_end_addr = app_end_addr;
}

/* Read exactly sizeof(uf2_block_t) bytes. Returns 1 on a full block, 0 on clean
 * EOF, -1 on error or a short (corrupt) trailing block. */
static int read_block(uf2_block_t *blk)
{
    uint8_t *p = (uint8_t *)blk;
    uint32_t got_total = 0;
    while (got_total < UF2_BLOCK_SIZE) {
        uint32_t got = 0;
        if (!storage_read(p + got_total, UF2_BLOCK_SIZE - got_total, &got))
            return -1;
        if (got == 0)
            return (got_total == 0) ? 0 : -1; /* EOF mid-block == corrupt */
        got_total += got;
    }
    return 1;
}

const char *uf2_load_result_str(uf2_load_result_t r)
{
    switch (r) {
    case UF2_LOAD_OK:                  return "OK";
    case UF2_LOAD_OPEN_FAILED:         return "could not open file";
    case UF2_LOAD_NO_MATCHING_BLOCKS:  return "no blocks for this chip/partition";
    case UF2_LOAD_READ_ERROR:          return "read error";
    case UF2_LOAD_BAD_FILE:            return "corrupt or misaligned UF2";
    case UF2_LOAD_VERIFY_FAILED:       return "flash verify mismatch";
    case UF2_LOAD_TOO_LARGE:           return "image exceeds available flash";
    default:                           return "unknown";
    }
}

uf2_load_result_t uf2_load_file(const char *name,
                                uf2_load_stats_t *stats,
                                uf2_progress_cb progress)
{
    uf2_load_stats_t st = {0};
    st.lowest_addr = 0xFFFFFFFFu;

    if (!storage_open(name))
        return UF2_LOAD_OPEN_FAILED;

    /* ---------- Pass 1: validate + measure ---------- */
    uf2_block_t blk;
    int rc;
    while ((rc = read_block(&blk)) == 1) {
        st.total_blocks++;
        uint32_t off, len;
        uf2_class_t c = uf2_classify_block(&blk, EXPECTED_UF2_FAMILY,
                                           APP_BASE_ADDR, g_app_end_addr, XIP_BASE,
                                           &off, &len);
        switch (c) {
        case UF2_CLS_PROGRAM: {
            if (off % FLASH_PAGE_SIZE != 0) { storage_close(); return UF2_LOAD_BAD_FILE; }
            uint32_t abs_lo = XIP_BASE + off;
            uint32_t abs_hi = abs_lo + len;
            if (abs_lo < st.lowest_addr)  st.lowest_addr  = abs_lo;
            if (abs_hi > st.highest_addr) st.highest_addr = abs_hi;
            st.programmed_blocks++;
            break;
        }
        case UF2_CLS_OUT_OF_RANGE:
            /* A block whose payload would land at or past the real flash end
             * means the image is too big for this chip. Refuse rather than
             * silently truncating. Blocks below APP_BASE_ADDR (e.g. a UF2 still
             * linked to the bootloader region) stay as a soft skip. */
            if (blk.target_addr >= APP_BASE_ADDR) {
                storage_close();
                return UF2_LOAD_TOO_LARGE;
            }
            st.skipped_blocks++;
            break;
        case UF2_CLS_SKIP:
            st.skipped_blocks++;
            break;
        case UF2_CLS_BAD_MAGIC:
        case UF2_CLS_BAD_PAYLOAD:
            storage_close();
            return UF2_LOAD_BAD_FILE;
        }
    }
    if (rc < 0)               { storage_close(); return UF2_LOAD_READ_ERROR; }
    if (st.programmed_blocks == 0) { storage_close(); return UF2_LOAD_NO_MATCHING_BLOCKS; }

    /* ---------- Erase exactly the range we will write ----------
     * Loop one sector at a time so the progress callback fires per sector
     * (~50 ms on RP2350 + QSPI). A single big flash_range_erase would only
     * give us two ticks (before/after) and leave the on-screen bar frozen
     * for many seconds on a 1-2 MB image. The per-sector bootrom overhead
     * is negligible vs the erase time itself. */
    {
        uint32_t off_lo  = st.lowest_addr  - XIP_BASE;
        uint32_t off_hi  = st.highest_addr - XIP_BASE;
        uint32_t sec_lo  = off_lo & ~(FLASH_SECTOR_SIZE - 1u);
        uint32_t sec_hi  = (off_hi + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u);
        uint32_t total   = (sec_hi - sec_lo) / FLASH_SECTOR_SIZE;
        uint32_t done    = 0;
        if (progress) progress(UF2_PROGRESS_ERASE, 0, total);
        for (uint32_t off = sec_lo; off < sec_hi; off += FLASH_SECTOR_SIZE) {
            flash_writer_erase(off, FLASH_SECTOR_SIZE);
            if (progress) progress(UF2_PROGRESS_ERASE, ++done, total);
        }
    }

    /* ---------- Pass 2: program + verify ---------- */
    if (!storage_rewind()) { storage_close(); return UF2_LOAD_READ_ERROR; }

    uint32_t done = 0;
    while ((rc = read_block(&blk)) == 1) {
        uint32_t off, len;
        if (uf2_classify_block(&blk, EXPECTED_UF2_FAMILY,
                               APP_BASE_ADDR, g_app_end_addr, XIP_BASE,
                               &off, &len) != UF2_CLS_PROGRAM)
            continue;

        /* Pad to a full 256-byte page (flash programs whole pages). */
        uint8_t page[FLASH_PAGE_SIZE];
        memset(page, 0xFF, sizeof(page));
        memcpy(page, blk.data, len);

        flash_writer_program(off, page, FLASH_PAGE_SIZE);
        if (!flash_writer_verify(off, page, FLASH_PAGE_SIZE)) {
            storage_close();
            return UF2_LOAD_VERIFY_FAILED;
        }
        if (progress) progress(UF2_PROGRESS_WRITE, ++done, st.programmed_blocks);
    }
    storage_close();
    if (rc < 0) return UF2_LOAD_READ_ERROR;

    if (stats) *stats = st;
    return UF2_LOAD_OK;
}

uf2_load_result_t uf2_validate_file(const char *name, uf2_load_stats_t *stats)
{
    uf2_load_stats_t st = {0};
    st.lowest_addr = 0xFFFFFFFFu;

    if (!storage_open(name))
        return UF2_LOAD_OPEN_FAILED;

    /* Pass 1 only: validate + measure, never touch flash. */
    uf2_block_t blk;
    int rc;
    while ((rc = read_block(&blk)) == 1) {
        st.total_blocks++;
        uint32_t off, len;
        uf2_class_t c = uf2_classify_block(&blk, EXPECTED_UF2_FAMILY,
                                           APP_BASE_ADDR, g_app_end_addr, XIP_BASE,
                                           &off, &len);
        switch (c) {
        case UF2_CLS_PROGRAM: {
            if (off % FLASH_PAGE_SIZE != 0) { storage_close(); return UF2_LOAD_BAD_FILE; }
            uint32_t abs_lo = XIP_BASE + off;
            uint32_t abs_hi = abs_lo + len;
            if (abs_lo < st.lowest_addr)  st.lowest_addr  = abs_lo;
            if (abs_hi > st.highest_addr) st.highest_addr = abs_hi;
            st.programmed_blocks++;
            break;
        }
        case UF2_CLS_OUT_OF_RANGE:
            if (blk.target_addr >= APP_BASE_ADDR) {
                storage_close();
                return UF2_LOAD_TOO_LARGE;
            }
            st.skipped_blocks++;
            break;
        case UF2_CLS_SKIP:
            st.skipped_blocks++;
            break;
        case UF2_CLS_BAD_MAGIC:
        case UF2_CLS_BAD_PAYLOAD:
            storage_close();
            return UF2_LOAD_BAD_FILE;
        }
    }
    storage_close();
    if (rc < 0)                     return UF2_LOAD_READ_ERROR;
    if (st.programmed_blocks == 0)  return UF2_LOAD_NO_MATCHING_BLOCKS;

    if (stats) *stats = st;
    return UF2_LOAD_OK;
}
