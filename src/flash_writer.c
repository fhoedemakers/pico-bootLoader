/*
 * flash_writer.c - see flash_writer.h
 */
#include "flash_writer.h"

#include <string.h>
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "hardware/regs/addressmap.h" /* XIP_BASE */

/* Round x down / up to the flash sector boundary (4096 bytes). */
static inline uint32_t sector_down(uint32_t x) { return x & ~(FLASH_SECTOR_SIZE - 1u); }
static inline uint32_t sector_up(uint32_t x)   { return (x + FLASH_SECTOR_SIZE - 1u) & ~(FLASH_SECTOR_SIZE - 1u); }

void flash_writer_erase(uint32_t offset, uint32_t len)
{
    uint32_t start = sector_down(offset);
    uint32_t end   = sector_up(offset + len);
    uint32_t ints = save_and_disable_interrupts();
    flash_range_erase(start, end - start);
    restore_interrupts(ints);
}

void flash_writer_program(uint32_t offset, const uint8_t *data, uint32_t len)
{
    /* flash_range_program requires page-aligned length; callers pad to 256. */
    uint32_t ints = save_and_disable_interrupts();
    flash_range_program(offset, data, len);
    restore_interrupts(ints);
}

bool flash_writer_verify(uint32_t offset, const uint8_t *data, uint32_t len)
{
    const uint8_t *flash = (const uint8_t *)(XIP_BASE + offset);
    return memcmp(flash, data, len) == 0;
}
