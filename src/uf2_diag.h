/*
 * uf2_diag.h - Work out WHY the loader refused a .uf2, so the picker can show
 *              the user something better than the loader's internal result
 *              string.
 *
 * uf2_load_result_t answers "can I flash this?"; that is all the loader needs.
 * The user needs "what is wrong with my build, and what do I change?". The
 * overwhelmingly common case -- an application still linked at 0x10000000
 * because it was built without -DBUILD_FOR_BOOTLOADER=ON -- reaches the menu as
 * UF2_LOAD_NO_MATCHING_BLOCKS, which reads as "no blocks for this
 * chip/partition" and helps nobody.
 *
 * This module re-reads a few blocks of the rejected file to turn that into a
 * concrete diagnosis (linked at the wrong address / built for the wrong chip /
 * too big / corrupt), including the actual addresses involved.
 *
 * Only ever called on the error path, after a validation failure, with the
 * display still alive and nothing erased -- so the extra file reads cost
 * nothing that matters.
 */
#ifndef UF2_DIAG_H
#define UF2_DIAG_H

#include <stdint.h>
#include <stdbool.h>

#include "uf2_loader.h"

typedef enum {
    UF2_DIAG_WRONG_LINK_ADDR, /* valid image, but linked outside the partition */
    UF2_DIAG_TOO_LARGE,       /* image extends past the end of usable flash    */
    UF2_DIAG_WRONG_FAMILY,    /* RP2040 / RISC-V / non-secure Arm / data build */
    UF2_DIAG_CORRUPT,         /* bad magic, misaligned or truncated blocks     */
    UF2_DIAG_UNREADABLE,      /* could not open / read the file at all         */
    UF2_DIAG_UNKNOWN,         /* nothing recognisable; fall back to result str */
} uf2_diag_reason_t;

typedef struct {
    uf2_diag_reason_t reason;
    uf2_load_result_t result;      /* the loader result this was derived from  */
    bool     have_extent;          /* image_base / image_end are valid         */
    uint32_t image_base;           /* lowest address the file targets          */
    uint32_t image_end;            /* one past the highest address it targets  */
    uint32_t family;               /* detected family (WRONG_FAMILY only)      */
    uint32_t expected_family;      /* family the loader was looking for        */
    uint32_t region_base;          /* where the loader wanted the image        */
    uint32_t region_end;
} uf2_diag_t;

/*
 * Diagnose a file the loader just rejected. `r` is the result uf2_validate_file
 * (or uf2_validate_file_ex) returned; region_base/region_end/expected_family are
 * the same values that validation ran with. Never fails -- worst case the reason
 * is UF2_DIAG_UNKNOWN and the caller falls back to uf2_load_result_str().
 */
void uf2_diagnose(const char *path, uf2_load_result_t r,
                  uint32_t region_base, uint32_t region_end,
                  uint32_t expected_family, uf2_diag_t *out);

/* Human-readable name for a UF2 family ID ("RP2040", "RP2350 RISC-V", ...). */
const char *uf2_family_name(uint32_t family);

/* One-line summary of a diagnosis, for the UART log. */
const char *uf2_diag_reason_str(uf2_diag_reason_t reason);

#endif /* UF2_DIAG_H */
