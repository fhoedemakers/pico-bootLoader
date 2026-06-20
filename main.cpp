/*
 * main.cpp - Pico emuLoader: a resident .uf2 bootloader frontend for the
 *            RP2350 retro-emulator family (shared pico_shared framework).
 *
 * Boot flow:
 *   1. The RP2350 bootrom always runs this image (it owns the start of flash).
 *   2. RESUME CHECK: if we got here because an emulator deliberately rebooted
 *      itself to flash a ROM (no-PSRAM path: watchdog_enable), jump straight
 *      back into the already-flashed emulator instead of showing the menu.
 *      A physical reset / power-cycle does NOT set that flag, so it lands here
 *      and shows the menu -- exactly the requested behaviour.
 *   3. Otherwise: init display/SD/USB via the framework, list the emulators
 *      available for this board config under /emu/<HW_CONFIG>/, and show a
 *      picker. Pressing B flashes the chosen emulator into the application
 *      partition and jumps to it.
 *
 * The flash map (bootloader region + app partition) lives in
 * pico_shared/BootPartition.cmake and src/boot_config.h (single source).
 *
 * Serial output: all diagnostics go to UART (PICO_DEFAULT_UART, pins 44/45 on
 * Fruit Jam, 115200-8N1). Tag every line with "[emuLoader] " so they stand out
 * when the freshly-launched emulator starts speaking.
 */
#include <cstdio>
#include <cstring>
#include <strings.h>

#include "pico/stdlib.h"
#include "pico/multicore.h"
#include "pico/version.h"
#include "hardware/clocks.h"
#include "hardware/watchdog.h"
#include "hardware/vreg.h"

#include "FrensHelpers.h"
#include "RomLister.h"
#include "menu.h"
#include "settings.h"
#include "gamepad.h"
#include "tusb.h"

extern "C" {
#include "boot_config.h"
#include "uf2_loader.h"
#include "app_launch.h"
#include "storage.h"
}

// DrawScreen() has external linkage in pico_shared/menu.cpp but is not declared
// in menu.h. It renders the 40x30 charcell screenBuffer into the active video
// framebuffer for one frame. We drive it directly from our own picker loop.
void DrawScreen(int selectedRow, int w = 0, int h = 0, uint16_t *imagebuffer = nullptr,
                int imagex = 0, int imagey = 0);

// splash() is normally provided by each emulator (it is emulator-specific art).
// The bootloader shows no splash, but menu.cpp references the symbol, so give
// the linker a harmless definition.
void splash() {}

#define CPUFREQ_KHZ 252000

#ifndef HW_CONFIG
#define HW_CONFIG 0
#endif

#define COL_FG  DEFAULT_FGCOLOR   // dark text
#define COL_BG  DEFAULT_BGCOLOR   // light background

// Every diagnostic line gets the same prefix so it's easy to grep for
// bootloader output across the emulator's own chatty stdout.
#define LOG(fmt, ...) printf("[emuLoader] " fmt "\n", ##__VA_ARGS__)

namespace {

char g_emuDir[32];                       // "/emu/8"
char g_labels[64][ROMLISTER_MAXPATH];    // friendly names parallel to entries

// Derive a friendly label from a UF2 filename:
//   "picogenesisPlus_AdafruitFruitJam_arm_piousb.uf2" -> "genesisPlus"
//   "PicoPeanutGB_AdafruitFruitJam_arm_piousb.uf2"    -> "PeanutGB"
void makeLabel(const char *fname, char *out, size_t n)
{
    char tmp[ROMLISTER_MAXPATH];
    strncpy(tmp, fname, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *dot = strrchr(tmp, '.');
    if (dot && strcasecmp(dot, ".uf2") == 0) *dot = '\0';
    char *us = strchr(tmp, '_');
    if (us) *us = '\0';

    char *s = tmp;
    if (strncasecmp(s, "pico", 4) == 0 && s[4] != '\0') s += 4;

    strncpy(out, s, n - 1);
    out[n - 1] = '\0';
}

void centerText(int row, const char *text, int fg, int bg)
{
    int len = (int)strlen(text);
    int x = (SCREEN_COLS - len) / 2;
    if (x < 0) x = 0;
    putText(x, row, text, fg, bg);
}

// Paint one menu frame into screenBuffer from the current selection state.
void drawMenu(Frens::RomLister::RomEntry *entries, int count, int sel, int top, int visible)
{
    ClearScreen(COL_BG);
    centerText(0, "P I C O   e m u L o a d e r", COL_FG, COL_BG);

    char hdr[SCREEN_COLS + 1];
    snprintf(hdr, sizeof(hdr), "Config %d   %s", HW_CONFIG, g_emuDir);
    centerText(1, hdr, COL_FG, COL_BG);

    char bar[SCREEN_COLS + 1];
    memset(bar, ' ', SCREEN_COLS);
    bar[SCREEN_COLS] = '\0';

    for (int i = 0; i < visible && (top + i) < count; i++) {
        int idx = top + i;
        bool seld = (idx == sel);
        int fg = seld ? COL_BG : COL_FG;   // invert the highlighted row
        int bg = seld ? COL_FG : COL_BG;
        int row = STARTROW + i;
        putText(0, row, bar, fg, bg);                 // paint the full-width bar
        char line[SCREEN_COLS + 1];
        snprintf(line, sizeof(line), "%c %s", seld ? '>' : ' ', g_labels[idx]);
        putText(1, row, line, fg, bg);
    }

    centerText(SCREEN_ROWS - 3, "UP / DOWN : choose", COL_FG, COL_BG);
    centerText(SCREEN_ROWS - 2, "B : flash & launch", COL_FG, COL_BG);
}

void showMessage(const char *l1, const char *l2, const char *l3)
{
    ClearScreen(COL_BG);
    if (l1) centerText(12, l1, COL_FG, COL_BG);
    if (l2) centerText(14, l2, COL_FG, COL_BG);
    if (l3) centerText(16, l3, COL_FG, COL_BG);
}

// Pump USB + render for a fixed time (display stays live).
void idleFor(int ms)
{
    for (int i = 0; i < ms / 16; i++) {
        tuh_task();
        DrawScreen(-1);
        sleep_ms(16);
    }
}

// Why did we boot? Useful breadcrumb for diagnosing the resume-vs-menu split.
void logBootCause()
{
    bool wd          = watchdog_caused_reboot();
    bool wd_enable   = watchdog_enable_caused_reboot();
    LOG("Boot cause: watchdog=%d  watchdog_enable=%d  (menu-triggered reboot if both true)",
        (int)wd, (int)wd_enable);
}

// Inspect the application partition's vector table and report whether it looks
// like a runnable image. Useful for diagnosing "why didn't it resume?" cases.
void logAppPartitionState(const char *when)
{
    const uint32_t *vt = (const uint32_t *)APP_BASE_ADDR;
    uint32_t sp    = vt[0];
    uint32_t reset = vt[1];
    bool present = app_launch_present();
    LOG("App partition %s: SP=0x%08X  reset=0x%08X  app_launch_present=%d",
        when, (unsigned)sp, (unsigned)reset, (int)present);
}

// uf2_loader progress callback: UART-only. The HSTX display is driven by
// core1 in this framework, and we reset core1 just before flashing -- so the
// on-screen picture goes blank for the duration of the flash regardless of
// what we paint into the framebuffer. UART output is throttled to once per
// 64 blocks plus the boundary events (start/end), so it does not bottleneck
// the flash loop.
void flashProgress(const char *phase, uint32_t done, uint32_t total)
{
    bool isWriting = (strcmp(phase, "Writing") == 0);
    bool boundary  = (done == 0 || done == total);
    if (boundary || !isWriting || (done & 0x3F) == 0) {
        unsigned pct = (total > 0) ? (unsigned)(((uint64_t)done * 100) / total) : 0;
        LOG("  %s %u / %u  (%u%%)", phase, (unsigned)done, (unsigned)total, pct);
    }
}

} // namespace

int main()
{
    Frens::setClocksAndStartStdio(CPUFREQ_KHZ, VREG_VOLTAGE_1_30);

    // --- BANNER -------------------------------------------------------------
    LOG("---- Pico emuLoader booting ----");
    LOG("Build: %s %s   SDK: " PICO_SDK_VERSION_STRING, __DATE__, __TIME__);
    LOG("HW_CONFIG=%d  sys_clk=%lu Hz  vreg=1.30V",
        HW_CONFIG, (unsigned long)clock_get_hz(clk_sys));
    LOG("Flash map: bootloader [0x%08X..0x%08X)  app [0x%08X..0x%08X)",
        (unsigned)XIP_BASE,     (unsigned)APP_BASE_ADDR,
        (unsigned)APP_BASE_ADDR,(unsigned)APP_END_ADDR);
    LOG("App partition size: %u bytes (%u KB)",
        (unsigned)APP_PARTITION_SIZE, (unsigned)(APP_PARTITION_SIZE / 1024));
    logBootCause();
    logAppPartitionState("at boot");

    // --- RESUME CHECK -------------------------------------------------------
    // A menu-triggered reboot inside an emulator (no-PSRAM ROM load) sets the
    // SDK watchdog-enable flag; a physical reset does not. If an emulator asked
    // to be resumed and a valid image is present, jump straight back to it so
    // the emulator can finish flashing/starting its ROM. Leaves the watchdog
    // scratch untouched so the emulator still sees watchdog_enable_caused_reboot.
    if (watchdog_enable_caused_reboot() && app_launch_present()) {
        LOG("Resume path: watchdog_enable=true and app image valid");
        LOG("Jumping to app reset vector at 0x%08X (no return on success)",
            (unsigned)((const uint32_t *)APP_BASE_ADDR)[1]);
        stdio_flush();
        app_launch_run();   // no return on success
        LOG("Resume refused (no valid image); falling through to menu.");
    } else {
        LOG("No resume: showing emulator picker.");
    }

    // --- FULL INIT (display/SD/USB/input via the shared framework) ----------
    LOG("Initializing shared framework (display/SD/USB/audio)...");
    FrensSettings::initSettings(FrensSettings::emulators::MULTI);
    char dummyRom[FF_MAX_LFN];
    dummyRom[0] = '\0';
    bool sdOk = Frens::initAll(dummyRom, CPUFREQ_KHZ, 0, 0, 256, false, false);
    LOG("initAll done. SD mounted=%d  PSRAM=%d  framebufferUsed=%d",
        (int)sdOk, (int)Frens::isPsramEnabled(), (int)Frens::isFrameBufferUsed());
    if (sdOk) {
        char fstype[16] = {0};
        Frens::getFsInfo(fstype, sizeof(fstype));
        LOG("Filesystem: %s", fstype);
    }

    screenBuffer = (charCell *)Frens::f_malloc(screenbufferSize);
    LOG("Allocated %u-byte screenBuffer at %p", (unsigned)screenbufferSize, screenBuffer);

    snprintf(g_emuDir, sizeof(g_emuDir), "/emu/%d", HW_CONFIG);
    LOG("Scanning %s for *.uf2...", g_emuDir);

    // List the emulators for this board config.
    Frens::RomLister lister(32 * 1024, ".uf2");
    int count = 0;
    Frens::RomLister::RomEntry *entries = nullptr;
    if (sdOk) {
        lister.list(g_emuDir);
        count = (int)lister.Count();
        entries = lister.GetEntries();
        if (count > (int)(sizeof(g_labels) / sizeof(g_labels[0]))) {
            LOG("WARNING: %d entries found, capping list at %u (recompile to raise)",
                count, (unsigned)(sizeof(g_labels) / sizeof(g_labels[0])));
            count = (int)(sizeof(g_labels) / sizeof(g_labels[0]));
        }
        for (int i = 0; i < count; i++) {
            makeLabel(entries[i].Path, g_labels[i], ROMLISTER_MAXPATH);
            LOG("  [%2d] %-40s  label=\"%s\"", i, entries[i].Path, g_labels[i]);
        }
        LOG("Found %d emulator UF2(s) in %s.", count, g_emuDir);
    }

    if (!sdOk) {
        LOG("FATAL: SD card mount failed; cannot list emulators.");
        showMessage("SD card not found.", "Insert a card and reset.", nullptr);
        for (;;) { tuh_task(); DrawScreen(-1); sleep_ms(16); }
    }
    if (count == 0) {
        LOG("FATAL: no .uf2 files in %s. Copy emulator UF2s there.", g_emuDir);
        char m[48];
        snprintf(m, sizeof(m), "No emulators in %s", g_emuDir);
        showMessage(m, "Copy emulator .uf2 files there", "and reset.");
        for (;;) { tuh_task(); DrawScreen(-1); sleep_ms(16); }
    }

    // --- PICKER LOOP --------------------------------------------------------
    LOG("Entering picker loop. D-pad: navigate, B: flash & launch.");
    const int visible = ENDROW - STARTROW + 1;
    int sel = 0, top = 0;
    uint32_t prevButtons = 0;
    using Btn = io::GamePadState::Button;

    for (;;) {
        tuh_task();

        uint32_t btns = io::getCurrentGamePadState(0).buttons |
                        io::getCurrentGamePadState(1).buttons;
        uint32_t pushed = btns & ~prevButtons;
        prevButtons = btns;

        if (pushed & Btn::UP)   {
            if (sel > 0) { sel--; LOG("UP -> sel=%d (%s)", sel, g_labels[sel]); }
        }
        if (pushed & Btn::DOWN) {
            if (sel < count - 1) { sel++; LOG("DOWN -> sel=%d (%s)", sel, g_labels[sel]); }
        }
        if (top > sel)              top = sel;
        if (sel >= top + visible)   top = sel - visible + 1;

        if (pushed & Btn::B) {
            char full[FF_MAX_LFN];
            snprintf(full, sizeof(full), "%s/%s", g_emuDir, entries[sel].Path);
            LOG("B pressed. Selected [%d] %s", sel, full);

            // Pre-flight while the display is alive: reject anything that has no
            // in-range program blocks (e.g. a standalone UF2 linked at 0x10000000).
            LOG("Pre-flight validating UF2 (pass 1, no flash writes)...");
            uf2_load_stats_t st;
            uf2_load_result_t vr = uf2_validate_file(full, &st);
            LOG("  result: %s", uf2_load_result_str(vr));
            LOG("  blocks: total=%u  programmable=%u  skipped=%u",
                (unsigned)st.total_blocks, (unsigned)st.programmed_blocks,
                (unsigned)st.skipped_blocks);
            if (st.programmed_blocks) {
                unsigned span = st.highest_addr - st.lowest_addr;
                LOG("  span:   0x%08X..0x%08X  (%u bytes, %u%% of partition)",
                    (unsigned)st.lowest_addr, (unsigned)st.highest_addr,
                    span, (unsigned)(100u * span / APP_PARTITION_SIZE));
            }
            if (vr != UF2_LOAD_OK) {
                LOG("REJECTED: %s", uf2_load_result_str(vr));
                showMessage("Cannot flash this file:", entries[sel].Path, uf2_load_result_str(vr));
                idleFor(3000);
                prevButtons = ~0u;   // require a fresh press after the message
                continue;
            }

            // Commit: paint the "Flashing..." notice WHILE core1 is still
            // alive (we need a few frames of it on screen before resetting
            // core1 kills HSTX servicing), then quiesce core1 and flash.
            // The screen will go blank for the ~1-2 seconds of the actual
            // flash op -- progress is reported on UART instead. Live on-screen
            // progress would need the framework's core1 to register a
            // multicore_lockout_victim so we can park-and-resume it per flash
            // op instead of resetting it; that's a pico_shared change.
            LOG("Pre-flight OK. Committing to flash; resetting core1 and erasing/programming.");
            showMessage("Flashing...", g_labels[sel], "Do not power off.");
            DrawScreen(-1);
            sleep_ms(500);   // let core1 push the notice out a few frames

            multicore_reset_core1();   // stop HSTX servicing before erasing flash
            LOG("core1 reset; beginning flash sequence");

            uf2_load_result_t r = uf2_load_file(full, &st, flashProgress);
            if (r == UF2_LOAD_OK) {
                LOG("Flash OK: %u blocks written to 0x%08X..0x%08X",
                    (unsigned)st.programmed_blocks,
                    (unsigned)st.lowest_addr, (unsigned)st.highest_addr);
                logAppPartitionState("after flash");
                if (!app_launch_present()) {
                    LOG("ERROR: app_launch_present()=false after flash; refusing jump and rebooting.");
                } else {
                    LOG("Launching %s; bye!", g_labels[sel]);
                    stdio_flush();
                    app_launch_run();      // no return on success
                    LOG("app_launch_run() returned unexpectedly; rebooting.");
                }
            } else {
                LOG("Flash FAILED: %s (after %u programmed, %u skipped)",
                    uf2_load_result_str(r),
                    (unsigned)st.programmed_blocks, (unsigned)st.skipped_blocks);
            }

            // Flash failed, or the launch was refused: reboot to recover the
            // menu cleanly (the resume check's app_launch_present() guard stops
            // it from jumping into a half-written partition).
            LOG("Rebooting bootloader to recover a clean state.");
            stdio_flush();
            watchdog_reboot(0, 0, 0);
            for (;;) tight_loop_contents();
        }

        drawMenu(entries, count, sel, top, visible);
        DrawScreen(-1);
    }
}
