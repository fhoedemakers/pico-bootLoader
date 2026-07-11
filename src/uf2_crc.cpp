/*
 * uf2_crc.cpp - see uf2_crc.h.
 *
 * The streaming CRC32 primitive lives in pico_shared/crc32.cpp:
 *   uint32_t update_crc32(uint32_t crc, const uint8_t* data, UINT length);
 * It pre-inverts and post-inverts internally; chaining calls (passing the
 * previous return as the next `crc`) is equivalent to one big call over the
 * concatenation, so we feed payload chunks straight through it.
 *
 * Why we can't use compute_crc32() from pico_shared: that one CRCs raw file
 * bytes (UF2 wrappers included). The in-flash bytes are payload-only, so the
 * two would never match. We walk the UF2 ourselves and feed just the payload.
 */
#include "uf2_crc.h"

extern "C" {
#include "storage.h"
#include "uf2_format.h"
}

#include <cstddef>       /* size_t referenced by crc32.h's prototypes */
#include <cstring>

#include "ff.h"          /* for UINT in crc32.h's signature */
#include "crc32.h"

/* Family-parameterised program-block filter. RP2350 SDK UF2 files start with a
 * non-program block (target_addr 0x10ffff00 with EXT_TAGS_PRESENT set) for the
 * ARM-S variant; the DATA family has no such header. Skip EXT_TAGS_PRESENT
 * unconditionally -- data-family UF2s never set it, so this stays neutral. */
static bool block_is_program(const uf2_block_t *b, uint32_t expected_family)
{
    if (b->magic_start0 != UF2_MAGIC_START0) return false;
    if (b->magic_start1 != UF2_MAGIC_START1) return false;
    if (b->magic_end    != UF2_MAGIC_END)    return false;
    if (b->flags & UF2_FLAG_NOT_MAIN_FLASH)  return false;
    if (b->flags & UF2_FLAG_EXT_TAGS_PRESENT) return false;
    if (b->flags & UF2_FLAG_FAMILY_ID_PRESENT) {
        if (b->file_size_or_family != expected_family) return false;
    }
    if (b->payload_size == 0 || b->payload_size > UF2_MAX_PAYLOAD) return false;
    return true;
}

bool uf2_fingerprint_from_file_family(const char *path,
                                      uint32_t expected_family,
                                      uf2_fingerprint_t *out)
{
    if (!out) return false;
    std::memset(out, 0, sizeof(*out));
    if (!storage_open(path)) return false;

    uint32_t crc = 0;
    uint32_t lo  = 0xFFFFFFFFu;
    uint32_t hi  = 0;
    bool any = false;

    /* Walk every block in file order. RP2350 SDK UF2 builds emit program blocks
     * in monotonically-increasing target_addr; this matches the byte order we
     * read back via XIP on the verify side. */
    uf2_block_t blk;
    for (uint32_t idx = 0; ; idx++) {
        if (!storage_seek(idx * sizeof(blk))) break;
        uint32_t got = 0;
        if (!storage_read(&blk, sizeof(blk), &got)) break;
        if (got != sizeof(blk)) break;   /* short read = EOF */
        if (blk.magic_start0 != UF2_MAGIC_START0) break;
        if (blk.magic_start1 != UF2_MAGIC_START1) break;
        if (blk.magic_end    != UF2_MAGIC_END)    break;
        if (!block_is_program(&blk, expected_family)) continue;

        if (blk.target_addr < lo) lo = blk.target_addr;
        uint32_t end = blk.target_addr + blk.payload_size;
        if (end > hi) hi = end;

        crc = update_crc32(crc, blk.data, (UINT)blk.payload_size);
        any = true;
    }
    storage_close();
    if (!any) return false;

    out->image_base = lo;
    out->image_size = hi - lo;
    out->crc        = crc;
    return true;
}

bool uf2_fingerprint_from_file(const char *path, uf2_fingerprint_t *out)
{
    return uf2_fingerprint_from_file_family(path, UF2_FAMILY_RP2350_ARM_S, out);
}

bool uf2_fingerprint_from_xip(uint32_t base, uint32_t size, uint32_t *out_crc)
{
    if (!out_crc || size == 0) return false;
    /* XIP range is already mapped; just feed the byte range through. */
    *out_crc = update_crc32(0, (const uint8_t *)(uintptr_t)base, (UINT)size);
    return true;
}

/* Read the 512-byte block at file index idx; true only on valid UF2 magic. */
static bool read_block_at(uint32_t idx, uf2_block_t *blk)
{
    if (!storage_seek(idx * sizeof(*blk))) return false;
    uint32_t got = 0;
    if (!storage_read(blk, sizeof(*blk), &got)) return false;
    if (got != sizeof(*blk)) return false;
    if (blk->magic_start0 != UF2_MAGIC_START0) return false;
    if (blk->magic_start1 != UF2_MAGIC_START1) return false;
    if (blk->magic_end    != UF2_MAGIC_END)    return false;
    return true;
}

bool uf2_extent_from_file_family(const char *path, uint32_t expected_family,
                                 uint32_t *out_base, uint32_t *out_end)
{
    if (!out_base || !out_end) return false;
    *out_base = *out_end = 0;
    if (!storage_open(path)) return false;

    uint32_t nblocks = storage_size() / UF2_BLOCK_SIZE;
    if (nblocks == 0) { storage_close(); return false; }

    /* First program block: at the front, past any header/metadata blocks
     * (same slack as program_name.c's find_marker). */
    const uint32_t FRONT_SCAN = 12;
    uf2_block_t blk;
    uint32_t base = 0, first_idx = 0;
    bool front_found = false;
    for (uint32_t idx = 0; idx < nblocks && idx < FRONT_SCAN; idx++) {
        if (!read_block_at(idx, &blk)) continue;
        if (!block_is_program(&blk, expected_family)) continue;
        base       = blk.target_addr;
        first_idx  = idx;
        front_found = true;
        break;
    }
    if (!front_found) { storage_close(); return false; }

    /* Last program block: at the back, allowing a few trailing non-program
     * blocks (other-family images appended to the same file, etc). */
    const uint32_t BACK_SCAN = 8;
    uint32_t end = 0;
    bool back_found = false;
    for (uint32_t n = 0; n < BACK_SCAN && n < nblocks; n++) {
        uint32_t idx = nblocks - 1 - n;
        if (idx < first_idx) break;
        if (!read_block_at(idx, &blk)) continue;
        if (!block_is_program(&blk, expected_family)) continue;
        end = blk.target_addr + blk.payload_size;
        back_found = true;
        break;
    }
    storage_close();

    if (!back_found || end <= base) return false;
    *out_base = base;
    *out_end  = end;
    return true;
}
