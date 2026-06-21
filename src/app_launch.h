/*
 * app_launch.h - Hand control from the bootloader to the application that lives
 *                at APP_BASE_ADDR.
 *
 * Two mechanisms are provided and selected at compile time:
 *
 *   BOOT_USE_ROM_CHAIN == 0  (default)  Classic Cortex-M vector-table jump:
 *        relocate VTOR, load the app's initial SP, branch to its reset vector.
 *        Simple, well understood, no partition table required. This is the
 *        tested default.
 *
 *   BOOT_USE_ROM_CHAIN == 1             Use the RP2350 bootrom rom_chain_image()
 *        function, which re-runs the ROM's image loader on the partition. This
 *        is the "blessed" RP2350 path (handles signed/encrypted images, flash
 *        re-init, etc.) but generally expects a proper partition table and can
 *        return BOOTROM_ERROR_NOT_PERMITTED otherwise. See app_launch.c.
 */
#ifndef APP_LAUNCH_H
#define APP_LAUNCH_H

#include <stdbool.h>
#include <stdint.h>

/* Does the image at `base` look like a valid, programmed application?
 * Checks the vector table's initial SP (must be in SRAM, word-aligned) and
 * reset vector (must be inside [base, base + SLOT_SIZE) with the Thumb bit). */
bool app_launch_present_at(uint32_t base);

/* Backward-compatible: checks APP_BASE_ADDR (== slot 0 == legacy partition). */
bool app_launch_present(void);

/* Tear down the bootloader's runtime and transfer control to the application
 * whose vector table lives at `base`. Does not return on success. Returns
 * (with the bootloader still running) only if no valid image is present or
 * the chosen launch method refused. */
void app_launch_run_at(uint32_t base);

/* Backward-compatible: launches APP_BASE_ADDR. */
void app_launch_run(void);

#endif /* APP_LAUNCH_H */
