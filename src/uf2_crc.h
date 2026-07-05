/*
 * uf2_crc.h - CRC32 fingerprint of an RP2350 image, computed two ways:
 *
 *   uf2_fingerprint_from_file() : walks the .uf2 on SD, accumulates CRC32 over
 *                                 each program-block payload in file order
 *                                 (= target-address order on SDK builds).
 *                                 Also reports the address range the file covers.
 *   uf2_fingerprint_from_xip()  : CRCs the equivalent in-flash byte range
 *                                 directly through XIP. Trivial wrapper around
 *                                 update_crc32().
 *
 * Used by the bootloader's scanEmulators() to detect when the SD copy of an
 * emulator no longer matches the bytes currently in flash (the user dropped a
 * new build on the card), so the picker can silently reflash on launch.
 *
 * Reuses the streaming CRC32 in pico_shared/crc32.cpp (update_crc32). The
 * primitive is NOT duplicated here.
 */
#ifndef UF2_CRC_H
#define UF2_CRC_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint32_t image_base;   /* lowest program target_addr seen in the file       */
    uint32_t image_size;   /* (highest target_addr + payload_size) - image_base */
    uint32_t crc;          /* CRC32 over payload bytes in target-addr order     */
} uf2_fingerprint_t;

/* Fingerprint the RP2350 ARM-S image in a .uf2 file. Skips the errata-E10
 * header block; walks only RP2350_ARM_S program blocks. */
bool uf2_fingerprint_from_file(const char *path, uf2_fingerprint_t *out);

/* Same, but for a different UF2 family (e.g. UF2_FAMILY_RP2350_DATA for a
 * DATA-family blob). No errata-E10 filter is applied here -- data-family
 * UF2s don't carry that header. */
bool uf2_fingerprint_from_file_family(const char *path,
                                      uint32_t expected_family,
                                      uf2_fingerprint_t *out);

bool uf2_fingerprint_from_xip(uint32_t base, uint32_t size, uint32_t *out_crc);

#ifdef __cplusplus
}
#endif

#endif /* UF2_CRC_H */
