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
    default:                           return "unknown";
    }
}

uf2_load_result_t uf2_load_file_to(const char *name,
                                   uint32_t base, uint32_t size,
                                   uf2_load_stats_t *stats,
                                   uf2_progress_cb progress)
{
    uf2_load_stats_t st = {0};
    st.lowest_addr = 0xFFFFFFFFu;
    uint32_t end = base + size;

    if (!storage_open(name))
        return UF2_LOAD_OPEN_FAILED;

    /* ---------- Pass 1: validate + measure ---------- */
    uf2_block_t blk;
    int rc;
    while ((rc = read_block(&blk)) == 1) {
        st.total_blocks++;
        uint32_t off, len;
        uf2_class_t c = uf2_classify_block(&blk, EXPECTED_UF2_FAMILY,
                                           base, end, XIP_BASE,
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
        case UF2_CLS_SKIP:
        case UF2_CLS_OUT_OF_RANGE:
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

    /* ---------- Erase exactly the range we will write ---------- */
    if (progress) progress("Erasing", 0, 1);
    flash_writer_erase(st.lowest_addr - XIP_BASE, st.highest_addr - st.lowest_addr);
    if (progress) progress("Erasing", 1, 1);

    /* ---------- Pass 2: program + verify ---------- */
    if (!storage_rewind()) { storage_close(); return UF2_LOAD_READ_ERROR; }

    uint32_t done = 0;
    while ((rc = read_block(&blk)) == 1) {
        uint32_t off, len;
        if (uf2_classify_block(&blk, EXPECTED_UF2_FAMILY,
                               base, end, XIP_BASE,
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
        if (progress) progress("Writing", ++done, st.programmed_blocks);
    }
    storage_close();
    if (rc < 0) return UF2_LOAD_READ_ERROR;

    if (stats) *stats = st;
    return UF2_LOAD_OK;
}

uf2_load_result_t uf2_validate_file_to(const char *name,
                                       uint32_t base, uint32_t size,
                                       uf2_load_stats_t *stats)
{
    uf2_load_stats_t st = {0};
    st.lowest_addr = 0xFFFFFFFFu;
    uint32_t end = base + size;

    if (!storage_open(name))
        return UF2_LOAD_OPEN_FAILED;

    /* Pass 1 only: validate + measure, never touch flash. */
    uf2_block_t blk;
    int rc;
    while ((rc = read_block(&blk)) == 1) {
        st.total_blocks++;
        uint32_t off, len;
        uf2_class_t c = uf2_classify_block(&blk, EXPECTED_UF2_FAMILY,
                                           base, end, XIP_BASE,
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
        case UF2_CLS_SKIP:
        case UF2_CLS_OUT_OF_RANGE:
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

uf2_load_result_t uf2_load_file(const char *name,
                                uf2_load_stats_t *stats,
                                uf2_progress_cb progress)
{
    return uf2_load_file_to(name, APP_BASE_ADDR, APP_PARTITION_SIZE, stats, progress);
}

uf2_load_result_t uf2_validate_file(const char *name, uf2_load_stats_t *stats)
{
    return uf2_validate_file_to(name, APP_BASE_ADDR, APP_PARTITION_SIZE, stats);
}
