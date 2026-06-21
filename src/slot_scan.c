/*
 * slot_scan.c - see slot_scan.h
 *
 * binary_info layout (from pico/binary_info/defs.h):
 *
 *   somewhere in the first 256 bytes of the image:
 *     offset 0x00 : BINARY_INFO_MARKER_START (0x7188ebf2)
 *     offset 0x04 : uint32_t __binary_info_start (absolute address into image)
 *     offset 0x08 : uint32_t __binary_info_end
 *     offset 0x0c : uint32_t __address_mapping_table (unused here)
 *     offset 0x10 : BINARY_INFO_MARKER_END   (0xe71aa390)
 *
 *   [bi_start, bi_end) is an array of pointers (32-bit absolute) to entries.
 *   Each entry begins with binary_info_core_t { uint16_t type; uint16_t tag; }.
 *   The program-name entry is a binary_info_id_and_string_t:
 *     core   (4 bytes: type=ID_AND_STRING, tag='R'|'P'<<8)
 *     id     (4 bytes: BINARY_INFO_ID_RP_PROGRAM_NAME == 0x02031c86)
 *     value  (4 bytes: pointer to NUL-terminated string in the same image)
 *
 * Because each emulator was linked for its own slot, all pointers above are
 * already correct absolute XIP addresses we can dereference directly.
 */
#include "slot_scan.h"

#include <string.h>
#include "boot_config.h"
#include "app_launch.h"

#define BI_MARKER_START          0x7188ebf2u
#define BI_MARKER_END            0xe71aa390u
#define BI_TYPE_ID_AND_STRING    6u
#define BI_TAG_RP                ((((uint16_t)'P') << 8) | (uint16_t)'R')
#define BI_ID_RP_PROGRAM_NAME    0x02031c86u

/* Is `addr` a plausible XIP pointer that lies within the slot at `base`? */
static inline bool in_slot(uint32_t addr, uint32_t base)
{
    return addr >= base && addr < base + SLOT_SIZE;
}

static bool find_bi_header(uint32_t base, uint32_t *out_bi_start, uint32_t *out_bi_end)
{
    /* 5-word header: marker_start, bi_start, bi_end, map, marker_end. The SDK
     * documents it as "within the first 256 bytes" but in practice on RP2350
     * builds it lands further in (e.g. slot+0x124 = 292 bytes for the standard
     * NVIC table size). Search the first 4 KB to be safe; that's still tens of
     * microseconds via XIP and bounds the cost. */
    const uint32_t *p   = (const uint32_t *)base;
    const uint32_t *end = (const uint32_t *)(base + 4096u - 20u);
    for (; p <= end; p++) {
        if (p[0] != BI_MARKER_START)  continue;
        if (p[4] != BI_MARKER_END)    continue;   /* offset +0x10 from p[0] */
        uint32_t bi_s = p[1];
        uint32_t bi_e = p[2];
        if (!in_slot(bi_s, base) || !in_slot(bi_e, base) || bi_s > bi_e)
            return false;
        if (((bi_e - bi_s) & 3u) != 0)
            return false;
        *out_bi_start = bi_s;
        *out_bi_end   = bi_e;
        return true;
    }
    return false;
}

bool slot_read_program_name(uint32_t base, char *name, unsigned cap)
{
    if (cap > 0) name[0] = '\0';

    uint32_t bi_s, bi_e;
    if (!find_bi_header(base, &bi_s, &bi_e))
        return false;

    /* Walk the array of pointers and look for the program-name entry. */
    const uint32_t *pp = (const uint32_t *)bi_s;
    const uint32_t *pe = (const uint32_t *)bi_e;
    for (; pp < pe; pp++) {
        uint32_t entry_addr = *pp;
        if (!in_slot(entry_addr, base))
            continue;
        const uint8_t *e = (const uint8_t *)entry_addr;
        uint16_t type = (uint16_t)(e[0] | (e[1] << 8));
        uint16_t tag  = (uint16_t)(e[2] | (e[3] << 8));
        if (type != BI_TYPE_ID_AND_STRING || tag != BI_TAG_RP)
            continue;
        uint32_t id = (uint32_t)e[4] | ((uint32_t)e[5] << 8)
                    | ((uint32_t)e[6] << 16) | ((uint32_t)e[7] << 24);
        if (id != BI_ID_RP_PROGRAM_NAME)
            continue;
        uint32_t str_addr = (uint32_t)e[8] | ((uint32_t)e[9] << 8)
                          | ((uint32_t)e[10] << 16) | ((uint32_t)e[11] << 24);
        if (!in_slot(str_addr, base))
            return false;
        /* Copy out the name, bounded by both cap and the slot. */
        const char *s = (const char *)str_addr;
        unsigned i = 0;
        uint32_t max = (base + SLOT_SIZE) - str_addr;
        if (max > cap - 1u) max = cap - 1u;
        while (i < max && s[i] != '\0') { name[i] = s[i]; i++; }
        name[i] = '\0';
        return i > 0;
    }
    return false;
}

unsigned slot_scan(slot_info_t *out, unsigned count)
{
    unsigned found = 0;
    for (unsigned i = 0; i < count; i++) {
        uint32_t base = SLOT_BASE(i);
        out[i].base    = base;
        out[i].present = app_launch_present_at(base);
        out[i].name[0] = '\0';
        if (out[i].present) {
            slot_read_program_name(base, out[i].name, SLOT_NAME_MAX);
            found++;
        }
    }
    return found;
}
