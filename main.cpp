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
 *   3. Otherwise: init display/SD/USB via the framework, list the emulator
 *      .uf2 files under /emu/<HW_CONFIG>/, identify which one is currently
 *      in the application partition by matching binary_info program names,
 *      and show a picker.
 *
 * Picker semantics (single-slot model):
 *   - One flat list of every .uf2 on SD.
 *   - The entry whose program_name matches the in-flash image is highlighted
 *     as "in flash" (and selected by default).
 *   - Pressing B on the in-flash entry: launch it directly (no flash, VTOR
 *     jump). Pressing B on any other entry: show a "Flashing..." screen for
 *     a brief moment so the user reads it, then flash and launch.
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
#include "program_name.h"
#include "emulators_txt.h"
#include "gui.h"
#include <hardware/divider.h>
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

// Brief on-screen pause (ms) so the "Flashing..." notice is readable before
// core1 is reset and the HDMI signal drops out.
#define FLASH_NOTICE_MS  1500

#define LOG(fmt, ...) printf("[emuLoader] " fmt "\n", ##__VA_ARGS__)

namespace {

#define PROG_NAME_MAX 32
#define IMAGE_KEY_MAX 16
#define DISPLAY_NAME_MAX 40

struct SdEmu {
    char filename[ROMLISTER_MAXPATH];        // basename
    char label[PROG_NAME_MAX];               // shown in the menu (program_name preferred)
    char prog_name[PROG_NAME_MAX];           // binary_info match key ("" if not extractable)
    char image_key[IMAGE_KEY_MAX];           // emulators.txt column 2 ("md", "nes", ...)
    char display_name[DISPLAY_NAME_MAX];     // emulators.txt column 3 (human-readable)
};

char  g_emuDir[32];                   // "/emu/8"
SdEmu g_emus[32];
int   g_emu_count = 0;
char  g_flash_prog_name[PROG_NAME_MAX] = {0};   // currently-flashed image's program name
int   g_flash_idx = -1;                         // index into g_emus, or -1 if none matches

// User-visible mode toggle (SELECT). Persisted to /emu/.guimode across reboots.
#define EMULATORS_TXT_PATH "/emu/emulators.txt"
#define GUI_MODE_PATH      "/emu/.guimode"
#define GUI_SLIDE_PX_PER_FRAME 20            // 320 / 20 = 16 frames ≈ 270 ms

// Fallback label derived from filename when binary_info parsing fails:
//   "picogenesisPlus_AdafruitFruitJam_arm_piousb.uf2" -> "picogenesisPlus"
void makeLabelFromFilename(const char *fname, char *out, size_t n)
{
    char tmp[ROMLISTER_MAXPATH];
    strncpy(tmp, fname, sizeof(tmp) - 1);
    tmp[sizeof(tmp) - 1] = '\0';

    char *dot = strrchr(tmp, '.');
    if (dot && strcasecmp(dot, ".uf2") == 0) *dot = '\0';
    char *us = strchr(tmp, '_');
    if (us) *us = '\0';

    strncpy(out, tmp, n - 1);
    out[n - 1] = '\0';
}

void centerText(int row, const char *text, int fg, int bg)
{
    int len = (int)strlen(text);
    int x = (SCREEN_COLS - len) / 2;
    if (x < 0) x = 0;
    putText(x, row, text, fg, bg);
}

// Paint one menu frame. The flashed entry (g_flash_idx) gets a leading marker
// to identify it as "the one currently in flash"; the selected row is shown
// inverted. Both compose when the flashed row is also the selected row.
void drawMenu(int sel, int top, int visible)
{
    ClearScreen(COL_BG);
    centerText(0, "P I C O   e m u L o a d e r", COL_FG, COL_BG);

    char hdr[SCREEN_COLS + 1];
    snprintf(hdr, sizeof(hdr), "Config %d   %s", HW_CONFIG, g_emuDir);
    centerText(1, hdr, COL_FG, COL_BG);

    char bar[SCREEN_COLS + 1];
    memset(bar, ' ', SCREEN_COLS);
    bar[SCREEN_COLS] = '\0';

    for (int i = 0; i < visible && (top + i) < g_emu_count; i++) {
        int idx = top + i;
        bool seld    = (idx == sel);
        bool flashed = (idx == g_flash_idx);
        int fg = seld ? COL_BG : COL_FG;
        int bg = seld ? COL_FG : COL_BG;
        int row = STARTROW + i;
        putText(0, row, bar, fg, bg);
        // Prefer the friendly name from emulators.txt; fall back to the
        // binary_info / filename-derived label when the txt has no row for
        // this prog_name.
        const char *name = g_emus[idx].display_name[0] ?
                           g_emus[idx].display_name : g_emus[idx].label;
        char line[SCREEN_COLS + 1];
        // Two prefix characters: selection marker, then flashed marker.
        // ">" = cursor; "*" = currently in flash. Either or both, or neither.
        snprintf(line, sizeof(line), "%c %c %s",
                 seld ? '>' : ' ',
                 flashed ? '*' : ' ',
                 name);
        putText(1, row, line, fg, bg);
    }

    centerText(SCREEN_ROWS - 4, "*  = in flash (no flash on B)", COL_FG, COL_BG);
    centerText(SCREEN_ROWS - 3, "UP / DOWN : choose   SELECT : graphical", COL_FG, COL_BG);
    centerText(SCREEN_ROWS - 2, "B : start", COL_FG, COL_BG);
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
// like a runnable image.
void logAppPartitionState(const char *when)
{
    const uint32_t *vt = (const uint32_t *)APP_BASE_ADDR;
    uint32_t sp    = vt[0];
    uint32_t reset = vt[1];
    bool present = app_launch_present();
    LOG("App partition %s: SP=0x%08X  reset=0x%08X  app_launch_present=%d",
        when, (unsigned)sp, (unsigned)reset, (int)present);
}

void flashProgress(const char *phase, uint32_t done, uint32_t total)
{
    bool isWriting = (strcmp(phase, "Writing") == 0);
    bool boundary  = (done == 0 || done == total);
    if (boundary || !isWriting || (done & 0x3F) == 0) {
        unsigned pct = (total > 0) ? (unsigned)(((uint64_t)done * 100) / total) : 0;
        LOG("  %s %u / %u  (%u%%)", phase, (unsigned)done, (unsigned)total, pct);
    }
}

// Read the SD directory, parse each emulator's program_name from its binary_info,
// build g_emus[], and locate the in-flash entry (g_flash_idx).
void scanEmulators()
{
    Frens::RomLister lister(32 * 1024, ".uf2");
    lister.list(g_emuDir);
    int count = (int)lister.Count();
    auto *entries = lister.GetEntries();

    int cap = (int)(sizeof(g_emus) / sizeof(g_emus[0]));
    if (count > cap) {
        LOG("WARNING: %d entries found, capping list at %d", count, cap);
        count = cap;
    }

    g_emu_count = 0;
    for (int i = 0; i < count; i++) {
        SdEmu &e = g_emus[g_emu_count];
        strncpy(e.filename, entries[i].Path, sizeof(e.filename) - 1);
        e.filename[sizeof(e.filename) - 1] = '\0';

        char full[FF_MAX_LFN];
        snprintf(full, sizeof(full), "%s/%s", g_emuDir, e.filename);

        e.prog_name[0]    = '\0';
        e.image_key[0]    = '\0';
        e.display_name[0] = '\0';
        bool ok = program_name_from_uf2_file(full, e.prog_name, sizeof(e.prog_name));
        if (ok && e.prog_name[0]) {
            strncpy(e.label, e.prog_name, sizeof(e.label) - 1);
            e.label[sizeof(e.label) - 1] = '\0';
        } else {
            makeLabelFromFilename(e.filename, e.label, sizeof(e.label));
            LOG("  binary_info parse FAILED for %s; fallback label=\"%s\"",
                e.filename, e.label);
        }
        // Pull the friendly name + image key from emulators.txt by prog_name.
        // Missing rows just leave both fields empty (graphical mode skips them).
        if (e.prog_name[0]) {
            emulators_txt_lookup(e.prog_name,
                                 e.image_key,    sizeof(e.image_key),
                                 e.display_name, sizeof(e.display_name));
        }
        LOG("  [%2d] %-40s  prog_name=\"%s\"  img_key=\"%s\"  display=\"%s\"",
            g_emu_count, e.filename, e.prog_name, e.image_key, e.display_name);
        g_emu_count++;
    }
    LOG("Found %d emulator UF2(s) in %s.", g_emu_count, g_emuDir);

    g_flash_prog_name[0] = '\0';
    g_flash_idx = -1;
    if (app_launch_present()) {
        if (program_name_from_xip(APP_BASE_ADDR, APP_PARTITION_SIZE,
                                  g_flash_prog_name, sizeof(g_flash_prog_name))) {
            LOG("In-flash program_name: \"%s\"", g_flash_prog_name);
            for (int i = 0; i < g_emu_count; i++) {
                if (g_emus[i].prog_name[0] &&
                    strcmp(g_emus[i].prog_name, g_flash_prog_name) == 0) {
                    g_flash_idx = i;
                    break;
                }
            }
            LOG("In-flash match: %s (idx=%d)",
                g_flash_idx >= 0 ? g_emus[g_flash_idx].filename : "(no match)",
                g_flash_idx);
        } else {
            LOG("In-flash image present but binary_info parse failed.");
        }
    } else {
        LOG("No valid image currently in flash.");
    }
}

// Launch the already-flashed emulator. No flash op; just reset core1 and jump.
void launchInFlash()
{
    LOG("Launching in-flash emulator: %s", g_flash_prog_name);
    if (!app_launch_present()) {
        LOG("ERROR: app_launch_present()=false at launch time; refusing.");
        showMessage("App partition is empty.", "Pick an emulator to flash.", nullptr);
        idleFor(2500);
        return;
    }
    multicore_reset_core1();   // bootloader's HSTX driver lives on core1; quiesce
    stdio_flush();
    app_launch_run();          // VTOR jump; no return on success
    LOG("app_launch_run() returned unexpectedly.");
}

// Flash the SD .uf2 at g_emus[idx] into the app partition and launch it.
void flashAndLaunch(int idx)
{
    char full[FF_MAX_LFN];
    snprintf(full, sizeof(full), "%s/%s", g_emuDir, g_emus[idx].filename);
    LOG("Flash & launch: [%d] %s", idx, full);

    // Pre-flight while the display is alive.
    LOG("Pre-flight validating UF2 (pass 1, no flash writes)...");
    uf2_load_stats_t st;
    uf2_load_result_t vr = uf2_validate_file(full, &st);
    LOG("  result: %s", uf2_load_result_str(vr));
    if (vr != UF2_LOAD_OK) {
        LOG("REJECTED: %s", uf2_load_result_str(vr));
        showMessage("Cannot flash this file:", g_emus[idx].filename, uf2_load_result_str(vr));
        idleFor(3000);
        return;
    }

    // Show the notice and give core1 a moment to push it to the display, then
    // pause so the user can actually read it before HSTX goes dark for the
    // flash op.
    showMessage("Flashing", g_emus[idx].label, "Do not power off.");
    DrawScreen(-1);
    idleFor(FLASH_NOTICE_MS);

    LOG("Pre-flight OK. Committing to flash; resetting core1 and erasing/programming.");
    multicore_reset_core1();
    LOG("core1 reset; beginning flash sequence");

    uf2_load_result_t r = uf2_load_file(full, &st, flashProgress);
    if (r == UF2_LOAD_OK) {
        LOG("Flash OK: %u blocks written to 0x%08X..0x%08X",
            (unsigned)st.programmed_blocks,
            (unsigned)st.lowest_addr, (unsigned)st.highest_addr);
        logAppPartitionState("after flash");
        if (app_launch_present()) {
            LOG("Launching %s; bye!", g_emus[idx].label);
            stdio_flush();
            app_launch_run();   // no return on success
            LOG("app_launch_run() returned unexpectedly.");
        } else {
            LOG("ERROR: app_launch_present()=false after flash; rebooting.");
        }
    } else {
        LOG("Flash FAILED: %s (after %u programmed, %u skipped)",
            uf2_load_result_str(r),
            (unsigned)st.programmed_blocks, (unsigned)st.skipped_blocks);
    }

    // Reboot to recover a clean state; the resume check's app_launch_present()
    // guard prevents jumping into a half-written partition.
    LOG("Rebooting bootloader to recover a clean state.");
    stdio_flush();
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
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
    if (watchdog_enable_caused_reboot() && app_launch_present()) {
        LOG("Resume path: watchdog_enable=true and app image valid");
        LOG("Jumping to app reset vector at 0x%08X (no return on success)",
            (unsigned)((const uint32_t *)APP_BASE_ADDR)[1]);
        stdio_flush();
        app_launch_run();
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

    if (!sdOk) {
        LOG("FATAL: SD card mount failed; cannot list emulators.");
        showMessage("SD card not found.", "Insert a card and reset.", nullptr);
        for (;;) { tuh_task(); DrawScreen(-1); sleep_ms(16); }
    }

    // Pre-load the emulators.txt table so scanEmulators() can attach a friendly
    // display name and image_key to each SdEmu entry as it goes. Missing or
    // unreadable file is non-fatal -- text mode still shows the prog_name
    // fallback labels and graphical mode just has nothing to show.
    LOG("Loading %s...", EMULATORS_TXT_PATH);
    emulators_txt_load(EMULATORS_TXT_PATH);

    // Parse program_name from every .uf2 on SD and from the in-flash image.
    // Briefly tell the user what's happening (this can take a second or two
    // while we seek through 5+ files on slow SD cards).
    showMessage("Scanning emulators...", g_emuDir, nullptr);
    DrawScreen(-1);
    LOG("Scanning %s for *.uf2 and parsing binary_info...", g_emuDir);
    scanEmulators();

    if (g_emu_count == 0) {
        LOG("FATAL: no .uf2 files in %s.", g_emuDir);
        char m[48];
        snprintf(m, sizeof(m), "No emulators in %s", g_emuDir);
        showMessage(m, "Copy emulator .uf2 files there", "and reset.");
        for (;;) { tuh_task(); DrawScreen(-1); sleep_ms(16); }
    }

    // --- PICKER LOOP --------------------------------------------------------
    LOG("Entering picker loop. D-pad: navigate, A: start, SELECT: toggle graphical.");
    const int visible = ENDROW - STARTROW + 1;
    int sel = (g_flash_idx >= 0) ? g_flash_idx : 0;
    int top = 0;
    if (sel >= visible) top = sel - visible + 1;
    uint32_t prevButtons = 0;
    using Btn = io::GamePadState::Button;

    // Graphical-mode state. Buffers are allocated lazily on first entry so
    // text-only sessions don't pay the ~300 KB cost.
    bool graphical_mode = false;
    bool buffers_ready  = false;
    int  slide_p        = 0;        // 0..SCREENWIDTH, pixels of the new image visible
    int  slide_dir      = 0;        // +1 = new enters from right, -1 = from left, 0 = idle

    auto load_image_into = [](int idx, uint16_t *dest) {
        if (idx >= 0 && idx < g_emu_count && g_emus[idx].image_key[0]) {
            if (gui_load_image(g_emus[idx].image_key, dest)) return;
        }
        gui_fill_solid(dest, 0);    // black placeholder on missing/invalid asset
    };

    auto enter_graphical = [&]() {
        if (!buffers_ready) buffers_ready = gui_buffers_alloc();
        if (!buffers_ready) {
            LOG("GUI buffer allocation failed; staying in text mode.");
            graphical_mode = false;
            return;
        }
        load_image_into(sel, gui_buf_cur());
        slide_p   = 0;
        slide_dir = 0;
    };

    // Restore the last mode the user left us in (file lives on the SD card).
    graphical_mode = gui_load_mode(GUI_MODE_PATH);
    LOG("Initial menu mode: %s", graphical_mode ? "graphical" : "text");
    if (graphical_mode) enter_graphical();

    for (;;) {
        tuh_task();
        Frens::PaceFrames60fps(false, true);
        auto count =
#if !HSTX
        dvi_->getFrameCounter();
#else
        hstx_getframecounter();
#endif
        auto onOff = hw_divider_s32_quotient_inlined(count, 60) & 1;
        Frens::blinkLed(onOff);
        uint32_t btns = io::getCurrentGamePadState(0).buttons |
                        io::getCurrentGamePadState(1).buttons;
        uint32_t pushed = btns & ~prevButtons;
        prevButtons = btns;

        // SELECT: toggle modes regardless. Persist so next boot lands the same way.
        if (pushed & Btn::SELECT) {
            graphical_mode = !graphical_mode;
            LOG("SELECT -> mode=%s", graphical_mode ? "graphical" : "text");
            gui_save_mode(GUI_MODE_PATH, graphical_mode);
            if (graphical_mode) enter_graphical();
        }

        // Mode-specific navigation.
        if (graphical_mode && buffers_ready) {
            if (slide_dir == 0) {
                // Idle: a fresh LEFT/RIGHT kicks off a slide to the neighbour.
                // Wraps around at the ends so the user can keep cycling.
                if ((pushed & Btn::RIGHT) && g_emu_count > 1) {
                    sel = (sel + 1) % g_emu_count;
                    LOG("RIGHT -> sel=%d (%s)", sel, g_emus[sel].label);
                    load_image_into(sel, gui_buf_next());
                    slide_dir = +1;
                    slide_p   = 0;
                } else if ((pushed & Btn::LEFT) && g_emu_count > 1) {
                    sel = (sel + g_emu_count - 1) % g_emu_count;
                    LOG("LEFT -> sel=%d (%s)", sel, g_emus[sel].label);
                    load_image_into(sel, gui_buf_next());
                    slide_dir = -1;
                    slide_p   = 0;
                }
            } else {
                // Mid-slide: advance progress. Ignore further LEFT/RIGHT until done.
                slide_p += GUI_SLIDE_PX_PER_FRAME;
                if (slide_p > SCREENWIDTH) slide_p = SCREENWIDTH;
            }
        } else {
            // Text mode (also the fallback when GUI buffers can't be allocated).
            if (pushed & Btn::UP) {
                if (sel > 0) { sel--; LOG("UP -> sel=%d (%s)", sel, g_emus[sel].label); }
            }
            if (pushed & Btn::DOWN) {
                if (sel < g_emu_count - 1) {
                    sel++; LOG("DOWN -> sel=%d (%s)", sel, g_emus[sel].label);
                }
            }
            if (top > sel)              top = sel;
            if (sel >= top + visible)   top = sel - visible + 1;
        }

        // A always launches the selected entry.
        if (pushed & Btn::A) {
            LOG("A pressed. sel=%d (%s) flashed_idx=%d",
                sel, g_emus[sel].label, g_flash_idx);
            if (sel == g_flash_idx) {
                launchInFlash();
            } else {
                flashAndLaunch(sel);
            }
            // If we get here, either the launch was refused or flash failed;
            // both paths reboot the bootloader so we shouldn't actually reach
            // this. Force a fresh button read just in case.
            prevButtons = ~0u;
        }

        // Render.
        if (graphical_mode && buffers_ready) {
            gui_draw_frame(gui_buf_cur(),
                           slide_dir != 0 ? gui_buf_next() : nullptr,
                           slide_p, slide_dir);
            if (slide_dir != 0 && slide_p >= SCREENWIDTH) {
                // Slide complete: the just-revealed image becomes the new current.
                gui_swap_buffers();
                slide_dir = 0;
                slide_p   = 0;
            }
        } else {
            drawMenu(sel, top, visible);
            DrawScreen(-1);
        }
    }
}
