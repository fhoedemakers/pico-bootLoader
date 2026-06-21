/*
 * app_launch.c - see app_launch.h
 */
#include "app_launch.h"

#include "boot_config.h"
#include "pico/stdlib.h"
/* CMSIS core symbols (SCB, NVIC, SysTick, __set_MSP, ...) come from the RP2350
 * CMSIS device header. It is not pulled in transitively here, so include it
 * explicitly (the cmsis_core library supplies the include path). */
#include "RP2350.h"

#ifndef BOOT_USE_ROM_CHAIN
#define BOOT_USE_ROM_CHAIN 0
#endif

bool app_launch_present_at(uint32_t base)
{
    const uint32_t *vt = (const uint32_t *)base;
    uint32_t sp    = vt[0];
    uint32_t reset = vt[1];

    /* Erased flash reads as all-ones. */
    if (sp == 0xFFFFFFFFu || reset == 0xFFFFFFFFu)
        return false;
    /* Initial stack pointer must land in SRAM and be word aligned. */
    if (sp < SRAM_BASE || sp > SRAM_END_ADDR || (sp & 0x3u))
        return false;
    /* Reset vector must point inside this image's slot, with the Thumb bit. */
    if (reset < base || reset >= base + SLOT_SIZE || !(reset & 0x1u))
        return false;
    return true;
}

bool app_launch_present(void)
{
    return app_launch_present_at(APP_BASE_ADDR);
}

#if BOOT_USE_ROM_CHAIN
/* --------- RP2350 bootrom chain-image path --------- */
#include "pico/bootrom.h"

void app_launch_run_at(uint32_t base)
{
    if (!app_launch_present_at(base))
        return;

    stdio_flush();

    /* Scratch RAM for the ROM loader. 4 KB is plenty for an unsigned image. */
    static uint8_t workarea[4096] __attribute__((aligned(256)));

    /* Re-run the ROM image loader on the chosen slot. On success it does not
     * return. A negative return is a BOOTROM_ERROR_* code; -4 (NOT_PERMITTED)
     * usually means the image/partition permissions or signing don't allow a
     * chain in this configuration -- in that case fall back to the menu. */
    (void)rom_chain_image(workarea, sizeof(workarea), base, SLOT_SIZE);
    /* If we get here the chain was refused; let the caller keep running. */
}

#else
/* --------- Classic vector-table jump path (default) --------- */

void app_launch_run_at(uint32_t base)
{
    if (!app_launch_present_at(base))
        return;

    const uint32_t *vt = (const uint32_t *)base;
    uint32_t app_sp    = vt[0];
    uint32_t app_reset = vt[1];

    stdio_flush();

    /* From here on we tear down the bootloader's runtime so the application
     * starts from a clean slate. Order matters. */
    __disable_irq();

    /* Stop and clear SysTick. */
    SysTick->CTRL = 0;
    SysTick->VAL  = 0;

    /* Disable and clear all NVIC interrupts the bootloader may have enabled. */
    for (unsigned i = 0; i < (sizeof(NVIC->ICER) / sizeof(NVIC->ICER[0])); i++) {
        NVIC->ICER[i] = 0xFFFFFFFFu;
        NVIC->ICPR[i] = 0xFFFFFFFFu;
    }

    /* Point the CPU at the application's vector table. */
    SCB->VTOR = base;

    /* ARMv8-M: clear the main stack-limit so the app can set its own, switch to
     * the MSP in privileged thread mode, then load the app's stack pointer. */
    __set_MSPLIM(0);
    __set_CONTROL(0);
    __ISB();
    __set_MSP(app_sp);
    __DSB();
    __ISB();

    /* Re-enable interrupts; the application's runtime init will configure
     * priorities. (SDK >= 2.1.0 also re-enables IRQs during runtime init.) */
    __enable_irq();

    /* Branch to the application's reset handler. Never returns. */
    ((void (*)(void))app_reset)();

    __builtin_unreachable();
}
#endif

void app_launch_run(void)
{
    app_launch_run_at(APP_BASE_ADDR);
}
