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
 * Program the named file into the region [base, base + size). Erases only the
 * sectors the image actually touches. The two convenience wrappers below
 * default to the legacy single application partition (== slot 0).
 */
uf2_load_result_t uf2_load_file_to(const char *name,
                                   uint32_t base, uint32_t size,
                                   uf2_load_stats_t *stats,
                                   uf2_progress_cb progress);

uf2_load_result_t uf2_validate_file_to(const char *name,
                                       uint32_t base, uint32_t size,
                                       uf2_load_stats_t *stats);

/* Convenience: validate/load against the legacy partition (slot 0). */
uf2_load_result_t uf2_load_file(const char *name,
                                uf2_load_stats_t *stats,
                                uf2_progress_cb progress);
uf2_load_result_t uf2_validate_file(const char *name, uf2_load_stats_t *stats);

const char *uf2_load_result_str(uf2_load_result_t r);

#endif /* UF2_LOADER_H */
