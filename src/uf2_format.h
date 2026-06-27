/*
 * uf2_format.h - Microsoft UF2 block layout + RP2350 family IDs, plus a pure
 *                (no-hardware) classifier used by the loader and the host tests.
 *
 * A UF2 file is a flat sequence of 512-byte blocks. Each block carries up to
 * 476 bytes of payload (the SDK uses 256) plus a target address telling the
 * loader where in flash that payload belongs.
 *
 * Reference: https://github.com/microsoft/uf2
 */
#ifndef UF2_FORMAT_H
#define UF2_FORMAT_H

#include <stdint.h>

/* ---- Magic numbers (first two words and last word of every block) ---- */
#define UF2_MAGIC_START0 0x0A324655u /* "UF2\n"          */
#define UF2_MAGIC_START1 0x9E5D5157u
#define UF2_MAGIC_END    0x0AB16F30u

/* ---- Block flag bits ---- */
#define UF2_FLAG_NOT_MAIN_FLASH    0x00000001u /* payload is not destined for flash       */
#define UF2_FLAG_FILE_CONTAINER    0x00001000u /* block is part of an embedded file        */
#define UF2_FLAG_FAMILY_ID_PRESENT 0x00002000u /* fileSize field actually holds a familyID */
#define UF2_FLAG_MD5_PRESENT       0x00004000u
#define UF2_FLAG_EXT_TAGS_PRESENT  0x00008000u

/* ---- UF2 family IDs understood by the RP2350 bootrom ---- */
#define UF2_FAMILY_RP2040        0xe48bff56u
#define UF2_FAMILY_RP2XXX_ABS    0xe48bff57u /* "absolute": e.g. the errata-E10 fix block */
#define UF2_FAMILY_RP2350_DATA   0xe48bff58u
#define UF2_FAMILY_RP2350_ARM_S  0xe48bff59u /* secure Arm image  <- default SDK Arm build */
#define UF2_FAMILY_RP2350_RISCV  0xe48bff5au
#define UF2_FAMILY_RP2350_ARM_NS 0xe48bff5bu /* non-secure Arm image                       */

#define UF2_BLOCK_SIZE     512u
#define UF2_MAX_PAYLOAD    476u

/* One 512-byte UF2 block. */
typedef struct __attribute__((packed)) {
    uint32_t magic_start0;
    uint32_t magic_start1;
    uint32_t flags;
    uint32_t target_addr;
    uint32_t payload_size;
    uint32_t block_no;
    uint32_t num_blocks;
    uint32_t file_size_or_family; /* familyID when UF2_FLAG_FAMILY_ID_PRESENT */
    uint8_t  data[UF2_MAX_PAYLOAD];
    uint32_t magic_end;
} uf2_block_t;

#ifdef __cplusplus
static_assert(sizeof(uf2_block_t) == UF2_BLOCK_SIZE, "UF2 block must be 512 bytes");
#else
_Static_assert(sizeof(uf2_block_t) == UF2_BLOCK_SIZE, "UF2 block must be 512 bytes");
#endif

/* Result of classifying a single block against the target partition. */
typedef enum {
    UF2_CLS_PROGRAM,      /* valid, in-range, matching family -> program it     */
    UF2_CLS_SKIP,         /* valid UF2 but not for us (other family / NOT_FLASH) */
    UF2_CLS_OUT_OF_RANGE, /* targets flash outside the application partition     */
    UF2_CLS_BAD_MAGIC,    /* not a UF2 block at all                              */
    UF2_CLS_BAD_PAYLOAD   /* payload_size impossible                            */
} uf2_class_t;

/*
 * Classify a block for an application partition spanning [region_base, region_end).
 *
 * On UF2_CLS_PROGRAM, *out_flash_offset receives the XIP-relative flash offset
 * (target_addr - XIP_BASE) and *out_len receives the payload length to write.
 *
 * This function is intentionally free of any hardware dependency so it can be
 * unit-tested on a host (see test/test_uf2.c).
 */
static inline uf2_class_t uf2_classify_block(const uf2_block_t *b,
                                             uint32_t expected_family,
                                             uint32_t region_base,
                                             uint32_t region_end,
                                             uint32_t xip_base,
                                             uint32_t *out_flash_offset,
                                             uint32_t *out_len)
{
    if (b->magic_start0 != UF2_MAGIC_START0 ||
        b->magic_start1 != UF2_MAGIC_START1 ||
        b->magic_end    != UF2_MAGIC_END) {
        return UF2_CLS_BAD_MAGIC;
    }
    if (b->payload_size == 0 || b->payload_size > UF2_MAX_PAYLOAD) {
        return UF2_CLS_BAD_PAYLOAD;
    }
    if (b->flags & UF2_FLAG_NOT_MAIN_FLASH) {
        return UF2_CLS_SKIP;
    }
    if (b->flags & UF2_FLAG_FAMILY_ID_PRESENT) {
        if (b->file_size_or_family != expected_family) {
            return UF2_CLS_SKIP; /* e.g. RISC-V blocks, or the absolute errata block */
        }
    }
    /* Address range check (also rejects wrap-around). */
    uint64_t start = b->target_addr;
    uint64_t end   = start + b->payload_size;
    if (start < region_base || end > region_end || end < start) {
        return UF2_CLS_OUT_OF_RANGE;
    }
    if (out_flash_offset) *out_flash_offset = (uint32_t)(b->target_addr - xip_base);
    if (out_len)          *out_len          = b->payload_size;
    return UF2_CLS_PROGRAM;
}

#endif /* UF2_FORMAT_H */
