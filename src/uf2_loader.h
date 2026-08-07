/*
 * uf2_loader.h - Streams a .uf2 file from storage into the application
 *                partition: validate -> erase -> program -> verify.
 */
#ifndef UF2_LOADER_H
#define UF2_LOADER_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    UF2_LOAD_OK = 0,
    UF2_LOAD_OPEN_FAILED,
    UF2_LOAD_NO_MATCHING_BLOCKS, /* file had no blocks for our family/partition */
    UF2_LOAD_READ_ERROR,
    UF2_LOAD_BAD_FILE,           /* misaligned size / corrupt blocks            */
    UF2_LOAD_VERIFY_FAILED,
    UF2_LOAD_TOO_LARGE,          /* image extends past the chip's real flash     */
    UF2_LOAD_WRONG_ADDRESS,      /* linked below the partition (standalone build)*/
} uf2_load_result_t;

typedef struct {
    uint32_t total_blocks;     /* 512-byte records in the file                  */
    uint32_t programmed_blocks;/* blocks actually written to the partition      */
    uint32_t skipped_blocks;   /* wrong family / NOT_MAIN_FLASH / out of range  */
    uint32_t lowest_addr;      /* lowest flash address written (absolute)       */
    uint32_t highest_addr;     /* one past the highest address written          */
    uint32_t lowest_out_of_range; /* target of the block that tripped
                                   * UF2_LOAD_WRONG_ADDRESS (0 otherwise)       */
} uf2_load_stats_t;

/* Phase codes for the progress callback. Integer codes (rather than string
 * literals) so the callback path can stay SRAM-resident -- no flash reads to
 * compare against. See [[framework-flash-while-running]] for why. */
#define UF2_PROGRESS_ERASE 0
#define UF2_PROGRESS_WRITE 1

/* Optional progress callback. done/total are in arbitrary per-phase units:
 *   UF2_PROGRESS_ERASE : called twice with (0,1) and (1,1) bracketing the erase.
 *   UF2_PROGRESS_WRITE : called once per page programmed, done=1..total.
 * May be NULL. */
typedef void (*uf2_progress_cb)(int phase, uint32_t done, uint32_t total);

/*
 * Program the named file (must already be openable via storage_open) into the
 * application partition. Erases only the sectors the image actually touches.
 * Uses the built-in RP2350_ARM_S family and [APP_BASE_ADDR, g_app_end_addr)
 * range.
 */
uf2_load_result_t uf2_load_file(const char *name,
                                uf2_load_stats_t *stats,
                                uf2_progress_cb progress);

/*
 * Validate the named file WITHOUT touching flash (pass 1 only). Lets the menu
 * reject a file that has no in-range program blocks (e.g. a standalone UF2 still
 * linked at 0x10000000) while the display is still alive, before committing to
 * the destructive erase/program. Returns UF2_LOAD_OK iff there is at least one
 * programmable, page-aligned, in-range block for this chip/partition.
 */
uf2_load_result_t uf2_validate_file(const char *name, uf2_load_stats_t *stats);

/*
 * Same as uf2_load_file / uf2_validate_file, but with an explicit target range
 * [region_base, region_end) and expected_family. Used for aux blobs
 * (e.g. Doom WHX at 0x10400000, DATA family). region_base must be
 * >= APP_BASE_ADDR (we never write into the bootloader partition) and
 * region_end must be <= the runtime-clamped g_app_end_addr.
 */
uf2_load_result_t uf2_load_file_ex(const char *name,
                                   uint32_t region_base,
                                   uint32_t region_end,
                                   uint32_t expected_family,
                                   uf2_load_stats_t *stats,
                                   uf2_progress_cb progress);

uf2_load_result_t uf2_validate_file_ex(const char *name,
                                       uint32_t region_base,
                                       uint32_t region_end,
                                       uint32_t expected_family,
                                       uf2_load_stats_t *stats);

const char *uf2_load_result_str(uf2_load_result_t r);

/*
 * Set the upper bound (absolute, exclusive) the loader will accept for any
 * UF2 block. Defaults to the build-time APP_END_ADDR (which assumes a 16 MB
 * chip). Call this once at startup with min(APP_END_ADDR, XIP_BASE + real
 * flash capacity) so that on a smaller chip we refuse a too-large image
 * instead of silently truncating the tail.
 */
void uf2_loader_set_app_end_addr(uint32_t app_end_addr);

#endif /* UF2_LOADER_H */
