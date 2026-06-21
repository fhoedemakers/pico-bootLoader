/*
 * slot_scan.h - Walk the pinned slots, find resident emulators, read their
 *               program name out of the SDK's binary_info table.
 *
 * Every emulator's CMakeLists already calls pico_set_program_name(), which the
 * SDK encodes via the binary_info system. Each image carries a 20-byte header
 * (BINARY_INFO_MARKER_START + bi_start/end pointers) within its first 256
 * bytes (documented in pico/binary_info/defs.h), pointing into the image's
 * .rodata at an array of binary_info entries. The bootloader reads those
 * directly via XIP -- no per-emulator descriptor needed.
 */
#ifndef SLOT_SCAN_H
#define SLOT_SCAN_H

#include <stdint.h>
#include <stdbool.h>

#define SLOT_NAME_MAX 32

typedef struct {
    uint32_t base;                  /* slot base address (absolute, XIP)         */
    bool     present;                /* app_launch_present_at(base) succeeded     */
    char     name[SLOT_NAME_MAX];    /* program_name read from binary_info, "" if unavailable */
} slot_info_t;

/*
 * Walk slots 0 .. (count-1), fill out[] (must be sized for count). Returns the
 * number of slots that look programmed (present == true).
 *
 * Slot 0 == APP_BASE_ADDR — interchangeable with a legacy single-partition
 * image, so a legacy build is detected as a resident slot 0.
 */
unsigned slot_scan(slot_info_t *out, unsigned count);

/*
 * Public: read an image's program name (from its binary_info) into `name`
 * (capacity `cap`, always NUL-terminated). Returns false if no parseable
 * binary_info header is found in the first 256 bytes of `base` or the name
 * pointer doesn't resolve into the slot. Exposed so the menu / setup can also
 * peek into an SD-loaded image once it's been flashed.
 */
bool slot_read_program_name(uint32_t base, char *name, unsigned cap);

#endif /* SLOT_SCAN_H */
