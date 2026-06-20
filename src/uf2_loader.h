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
} uf2_load_result_t;

typedef struct {
    uint32_t total_blocks;     /* 512-byte records in the file                  */
    uint32_t programmed_blocks;/* blocks actually written to the partition      */
    uint32_t skipped_blocks;   /* wrong family / NOT_MAIN_FLASH / out of range  */
    uint32_t lowest_addr;      /* lowest flash address written (absolute)       */
    uint32_t highest_addr;     /* one past the highest address written          */
} uf2_load_stats_t;

/* Optional progress callback: phase is "Erasing"/"Writing"/"Verifying",
 * done/total are block counts. May be NULL. */
typedef void (*uf2_progress_cb)(const char *phase, uint32_t done, uint32_t total);

/*
 * Program the named file (must already be openable via storage_open) into the
 * application partition. Erases only the sectors the image actually touches.
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

const char *uf2_load_result_str(uf2_load_result_t r);

#endif /* UF2_LOADER_H */
