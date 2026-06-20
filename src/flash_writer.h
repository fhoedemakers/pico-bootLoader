/*
 * flash_writer.h - Thin, interrupt-safe wrapper around the SDK flash API.
 *
 * All offsets are XIP-relative (i.e. absolute_address - XIP_BASE), matching the
 * convention used by flash_range_erase() / flash_range_program().
 *
 * On the RP2350 these SDK calls route through the bootrom, which performs the
 * "exit XIP -> talk to the QSPI flash -> restore XIP" dance for us. We only
 * have to guarantee nothing else touches flash meanwhile, which on this
 * single-core bootloader just means disabling interrupts for the duration.
 */
#ifndef FLASH_WRITER_H
#define FLASH_WRITER_H

#include <stdint.h>
#include <stdbool.h>

/* Erase [offset, offset+len). offset and len are rounded to the 4 KB sector. */
void flash_writer_erase(uint32_t offset, uint32_t len);

/* Program len bytes at offset. len must be a multiple of the 256 B page size;
 * the caller (uf2_loader) pads partial pages before calling. */
void flash_writer_program(uint32_t offset, const uint8_t *data, uint32_t len);

/* Verify that flash at offset matches data for len bytes (reads back via XIP). */
bool flash_writer_verify(uint32_t offset, const uint8_t *data, uint32_t len);

#endif /* FLASH_WRITER_H */
