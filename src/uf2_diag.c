/*
 * uf2_diag.c - see uf2_diag.h
 *
 * The only file walking done here is via uf2_extent_from_file_family() from
 * uf2_crc.h, which reads the first 12 and last 8 blocks and reports the address
 * range that family's program blocks cover. The scan path already trusts it for
 * the same purpose (main.cpp's size gate), so no second UF2 walker is
 * introduced.
 */
#include "uf2_diag.h"

#include <string.h>

#include "uf2_crc.h"
#include "uf2_format.h"

/* Families worth naming when the expected one produced nothing. Order is not
 * significant among these -- a block that carries a family ID matches at most
 * one of them. */
static const uint32_t OTHER_FAMILIES[] = {
    UF2_FAMILY_RP2040,
    UF2_FAMILY_RP2350_RISCV,
    UF2_FAMILY_RP2350_ARM_NS,
    UF2_FAMILY_RP2350_DATA,
    UF2_FAMILY_RP2XXX_ABS,
};

const char *uf2_family_name(uint32_t family)
{
    switch (family) {
    case UF2_FAMILY_RP2040:        return "RP2040";
    case UF2_FAMILY_RP2XXX_ABS:    return "RP2xxx absolute";
    case UF2_FAMILY_RP2350_DATA:   return "RP2350 data";
    case UF2_FAMILY_RP2350_ARM_S:  return "RP2350 secure Arm";
    case UF2_FAMILY_RP2350_RISCV:  return "RP2350 RISC-V";
    case UF2_FAMILY_RP2350_ARM_NS: return "RP2350 non-secure Arm";
    default:                       return "unknown";
    }
}

const char *uf2_diag_reason_str(uf2_diag_reason_t reason)
{
    switch (reason) {
    case UF2_DIAG_WRONG_LINK_ADDR: return "linked outside the app partition";
    case UF2_DIAG_TOO_LARGE:       return "image too large for this board";
    case UF2_DIAG_WRONG_FAMILY:    return "built for the wrong chip/architecture";
    case UF2_DIAG_CORRUPT:         return "corrupt UF2";
    case UF2_DIAG_UNREADABLE:      return "unreadable";
    default:                       return "undetermined";
    }
}

void uf2_diagnose(const char *path, uf2_load_result_t r,
                  uint32_t region_base, uint32_t region_end,
                  uint32_t expected_family, uf2_diag_t *out)
{
    if (!out) return;
    memset(out, 0, sizeof(*out));
    out->result          = r;
    out->reason          = UF2_DIAG_UNKNOWN;
    out->expected_family = expected_family;
    out->region_base     = region_base;
    out->region_end      = region_end;

    switch (r) {
    case UF2_LOAD_OPEN_FAILED:
    case UF2_LOAD_READ_ERROR:
        /* Nothing to probe -- the card or the file is the problem. */
        out->reason = UF2_DIAG_UNREADABLE;
        return;

    case UF2_LOAD_BAD_FILE:
        out->reason = UF2_DIAG_CORRUPT;
        return;

    case UF2_LOAD_TOO_LARGE:
    case UF2_LOAD_NO_MATCHING_BLOCKS:
    case UF2_LOAD_WRONG_ADDRESS:
        break; /* worth probing; handled below */

    default:
        return; /* OK / VERIFY_FAILED never reach the pre-flight error path */
    }

    /* Probe the family the loader wanted FIRST. uf2_crc.c's block filter accepts
     * a block that carries no UF2_FLAG_FAMILY_ID_PRESENT for any family, so a
     * family-less UF2 matches every probe. Asking for the expected family up
     * front means the other families are only consulted once we know the blocks
     * really do carry a family ID that isn't ours. */
    uint32_t base = 0, end = 0;
    if (uf2_extent_from_file_family(path, expected_family, &base, &end)) {
        out->have_extent = true;
        out->image_base  = base;
        out->image_end   = end;
        out->family      = expected_family;
        /* Right family, wrong place: below the partition means it was linked
         * for a standalone build (0x10000000); at or above the end means it
         * simply does not fit this board's flash. */
        out->reason = (base < region_base) ? UF2_DIAG_WRONG_LINK_ADDR
                                           : UF2_DIAG_TOO_LARGE;
        return;
    }

    /* The loader already established that this image sits below the partition.
     * Trust that even if the extent probe could not pin the range down. */
    if (r == UF2_LOAD_WRONG_ADDRESS) {
        out->reason = UF2_DIAG_WRONG_LINK_ADDR;
        out->family = expected_family;
        return;
    }

    for (unsigned i = 0; i < sizeof(OTHER_FAMILIES) / sizeof(OTHER_FAMILIES[0]); i++) {
        if (OTHER_FAMILIES[i] == expected_family) continue;
        if (!uf2_extent_from_file_family(path, OTHER_FAMILIES[i], &base, &end)) continue;
        out->reason      = UF2_DIAG_WRONG_FAMILY;
        out->family      = OTHER_FAMILIES[i];
        out->have_extent = true;
        out->image_base  = base;
        out->image_end   = end;
        return;
    }

    /* No family produced a usable extent: not a UF2 we can make sense of. */
    out->reason = UF2_DIAG_UNKNOWN;
}
