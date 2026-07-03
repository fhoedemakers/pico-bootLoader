/*
 * boot_config.h - Flash memory map for the bootLoader.
 *
 * The same numbers live in pico_shared/BootPartition.cmake (which links the
 * bootloader into the boot region and the emulators into the app partition).
 * CMake passes BOOTLOADER_SIZE / FLASH_TOTAL_SIZE here as compile definitions
 * so the C code below agrees with the linker. The #ifndef fallbacks keep this
 * header usable from host unit tests too.
 *
 * Flash layout (Adafruit Fruit Jam, 16 MB):
 *
 *   0x10000000  +-----------------------------+  <- bootrom always boots this
 *               |   Bootloader (bootLoader)   |     image (the menu/flasher).
 *               |        1 MB                  |
 *   0x10100000  +-----------------------------+  <- APP_BASE_ADDR
 *               |   Application partition      |     emulator UF2s land here.
 *               |        15 MB                 |     the bootloader jumps here.
 *   0x11000000  +-----------------------------+
 *
 * Because the bootloader sits at the very start of flash, the RP2350 bootrom
 * always runs it first. An emulator only runs when the bootloader jumps to it,
 * so every hardware reset / power cycle lands back in the menu.
 */
#ifndef BOOT_CONFIG_H
#define BOOT_CONFIG_H

#include <stdint.h>

#ifndef XIP_BASE
#define XIP_BASE 0x10000000u
#endif
#ifndef SRAM_BASE
#define SRAM_BASE 0x20000000u
#endif

/* Size reserved for the bootloader at the start of flash. Must be a multiple of
 * the 4096-byte flash sector. The bootloader pulls in the full pico_shared
 * framework (HSTX + USB host + FatFs + fonts), so 1 MB is comfortable. */
#ifndef BOOTLOADER_SIZE
#define BOOTLOADER_SIZE (1u * 1024u * 1024u)
#endif

/* Total external flash on the board. Fruit Jam = 16 MB. Override for others. */
#ifndef FLASH_TOTAL_SIZE
#define FLASH_TOTAL_SIZE (16u * 1024u * 1024u)
#endif

/* Application partition: everything after the bootloader. */
#define APP_PARTITION_OFFSET (BOOTLOADER_SIZE)                 /* XIP-relative   */
#define APP_PARTITION_SIZE   (FLASH_TOTAL_SIZE - BOOTLOADER_SIZE)
#define APP_BASE_ADDR        (XIP_BASE + APP_PARTITION_OFFSET) /* absolute (0x10100000) */
#define APP_END_ADDR         (APP_BASE_ADDR + APP_PARTITION_SIZE)

/* RP2350 has 520 KB of SRAM (0x20000000 .. 0x20082000); used to sanity-check a
 * loaded image's initial stack pointer. */
#define SRAM_END_ADDR (SRAM_BASE + (520u * 1024u))

#endif /* BOOT_CONFIG_H */
