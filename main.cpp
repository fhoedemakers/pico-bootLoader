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
#include "nespad.h"
#include "wiipad.h"
#include "tusb.h"

extern "C" {
#include "boot_config.h"
#include "uf2_loader.h"
#include "app_launch.h"
#include "storage.h"
#include "program_name.h"
#include "emulators_txt.h"
#include "gui.h"
#include "progress_bar.h"
#include <hardware/divider.h>
}

// Progress-bar colours, picked at compile time to match whichever 16-bit
// pixel format the active backend uses. Hard-coded so the SRAM-resident
// flashProgress callback never has to dereference a palette in flash.
//   HSTX  : RGB555 (5-5-5-1, low bit ignored)
//   !HSTX : RGB444 packed as 0x0RGB (this codebase's PicoDVI encoding,
//           see CC() in pico_shared/menu.cpp)
#if HSTX
#define PB_COL_BORDER 0x0000u   // black
#define PB_COL_EMPTY  0x7FFFu   // white (matches menu background)
#define PB_COL_FILL   0x03E0u   // pure green
#else
#define PB_COL_BORDER 0x0000u   // black
#define PB_COL_EMPTY  0x0FFFu   // white
#define PB_COL_FILL   0x00F0u   // pure green
#endif

// DrawScreen() has external linkage in pico_shared/menu.cpp but is not declared
// in menu.h. It renders the 40x30 charcell screenBuffer into the active video
// framebuffer for one frame. We drive it directly from our own picker loop.
void DrawScreen(int selectedRow, int w = 0, int h = 0, uint16_t *imagebuffer = nullptr,
                int imagex = 0, int imagey = 0);

// splash() is normally provided by each emulator (it is emulator-specific art).
// The bootloader shows no splash, but menu.cpp references the symbol, so give
// the linker a harmless definition.
void splash() {}

// Defined in pico_shared/FrensHelpers.cpp but not declared in FrensHelpers.h.
// Reads the SPI flash JEDEC capacity byte; returns the chip size in bytes.
namespace Frens { uint storage_get_flash_capacity(); }

#define CPUFREQ_KHZ 252000

#ifndef HW_CONFIG
#define HW_CONFIG 0
#endif

#define COL_FG  DEFAULT_FGCOLOR   // dark text
#define COL_BG  DEFAULT_BGCOLOR   // light background

// Brief on-screen pause (ms) so the "Flashing..." notice registers before
// the bar starts moving. The bar itself is now live during the erase so we
// don't need a long read-time -- just enough to acknowledge the press.
#define FLASH_NOTICE_MS  500

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

    // buttonLabel1 is the label of the button that triggers Btn::A on the
    // attached pad (e.g. "A" on NES, "B" on XInput, "O" on PlayStation).
    char buttonLabel1[2];
    char buttonLabel2[2];
    getButtonLabels(buttonLabel1, buttonLabel2);

    char hint[SCREEN_COLS + 1];
    snprintf(hint, sizeof(hint), "*  = in flash (no flash on %s)", buttonLabel1);
    centerText(SCREEN_ROWS - 4, hint, COL_FG, COL_BG);
    centerText(SCREEN_ROWS - 3, "UP / DOWN : choose   SELECT : graphical", COL_FG, COL_BG);
    snprintf(hint, sizeof(hint), "%s : start", buttonLabel1);
    centerText(SCREEN_ROWS - 2, hint, COL_FG, COL_BG);
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

// __not_in_flash_func: the whole callback path is SRAM-resident so we never
// have to worry about XIP state. No printf/LOG inside -- bookend logging
// happens in flashAndLaunch around uf2_load_file. The throttle (done & 0x3F)
// keeps redraw cost down for large images (otherwise ~16 K calls).
extern "C" void __not_in_flash_func(flashProgress)(int phase, uint32_t done, uint32_t total)
{
    // Combined percentage: erase contributes 0..10, write contributes 10..100.
    uint32_t pct;
    if (phase == UF2_PROGRESS_ERASE) {
        pct = (total > 0) ? (done * 10u / total) : 0;
    } else {
        uint32_t w = (total > 0) ? (done * 90u / total) : 0;
        pct = 10u + w;
        // Throttle write-phase redraws: a 2 MB image is ~8192 pages, plenty
        // of opportunity to skip frames where pct didn't move visibly.
        bool boundary = (done == 0 || done == total);
        if (!boundary && (done & 0x3F) != 0) return;
    }
    progress_bar_draw(pct, 100, PB_COL_FILL, PB_COL_EMPTY, PB_COL_BORDER);
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
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    wiipad_end();              // release I2C so the emulator can re-init it cleanly
#endif
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

    // Show the notice. Unlike before, core1 keeps running through the flash
    // op so the bar below this text updates in real time -- see
    // framework-flash-while-running.md for the SRAM-residence audit that
    // makes this safe.
    showMessage("Flashing", g_emus[idx].label, "Do not power off.");
    DrawScreen(-1);
    idleFor(FLASH_NOTICE_MS);

    // Paint an empty 0% bar so the user sees the bar's box appear before any
    // flash writes start. Colours are compile-time literals (see PB_COL_*),
    // so the flash callback never has to dereference flash for them.
    progress_bar_draw(0, 100, PB_COL_FILL, PB_COL_EMPTY, PB_COL_BORDER);

    LOG("Pre-flight OK. core1 left running; beginning flash sequence.");

    uf2_load_result_t r = uf2_load_file(full, &st, flashProgress);
    if (r == UF2_LOAD_OK) {
        // Force a final 100% paint before we tear down core1.
        progress_bar_draw(100, 100, PB_COL_FILL, PB_COL_EMPTY, PB_COL_BORDER);
        LOG("Flash OK: %u blocks written to 0x%08X..0x%08X",
            (unsigned)st.programmed_blocks,
            (unsigned)st.lowest_addr, (unsigned)st.highest_addr);
        logAppPartitionState("after flash");
        if (app_launch_present()) {
            LOG("Launching %s; bye!", g_emus[idx].label);
            stdio_flush();
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
            wiipad_end();              // free I2C for the emulator's own init
#endif
            multicore_reset_core1();   // hand HSTX over; emulator brings its own driver up
            app_launch_run();          // no return on success
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
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    wiipad_end();
#endif
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

    // Clamp the loader's app-partition end to the real chip capacity. The
    // build-time APP_END_ADDR assumes a 16 MB chip (Fruit Jam); on a smaller
    // chip we'd otherwise let an over-large image march past real flash.
    {
        uint32_t cap = Frens::storage_get_flash_capacity();
        uint32_t end = (cap >= FLASH_TOTAL_SIZE) ? APP_END_ADDR
                                                  : (XIP_BASE + cap);
        uf2_loader_set_app_end_addr(end);
        LOG("Flash capacity (JEDEC): %u bytes  app-end clamp: 0x%08X",
            (unsigned)cap, (unsigned)end);
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

    // Single loader that picks the full-res or half-res variant based on the
    // target buffer's size. Used for both cur (always full) and next (half
    // when no PSRAM, full otherwise).
    auto load_image_into = [](int idx, uint16_t *dest, bool half_res) {
        if (idx >= 0 && idx < g_emu_count && g_emus[idx].image_key[0]) {
            bool ok = half_res
                ? gui_load_image_half_res(g_emus[idx].image_key, dest)
                : gui_load_image(g_emus[idx].image_key, dest);
            if (ok) return;
        }
        if (half_res) gui_fill_solid_half_res(dest, 0);
        else          gui_fill_solid(dest, 0);
    };

    auto enter_graphical = [&]() {
        if (!buffers_ready) buffers_ready = gui_buffers_alloc();
        if (!buffers_ready) {
            LOG("GUI buffer allocation failed; staying in text mode.");
            graphical_mode = false;
            return;
        }
        load_image_into(sel, gui_buf_cur(), false);   // cur is always full res
        slide_p   = 0;
        slide_dir = 0;
    };

    // Restore the last mode the user left us in (file lives on the SD card).
    graphical_mode = gui_load_mode(GUI_MODE_PATH);
    LOG("Initial menu mode: %s", graphical_mode ? "graphical" : "text");
    if (graphical_mode) enter_graphical();

    for (;;) {
        Frens::PaceFrames60fps(false, true);
#if NES_PIN_CLK != -1
        nespad_read_start();
#endif
        auto count =
#if !HSTX
        dvi_->getFrameCounter();
#else
        hstx_getframecounter();
#endif
        auto onOff = hw_divider_s32_quotient_inlined(count, 60) & 1;
        Frens::blinkLed(onOff);
#if NES_PIN_CLK != -1
        nespad_read_finish();   // populates nespad_states[]
#endif
        tuh_task();
#if WIIPAD_DELAYED_START and WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
        // Probe once per second (onOff toggles at 60 frames) so we pick up a
        // pad that was plugged in after boot. wiipad_begin() is a no-op once
        // connected.
        if (!wiipad_is_connected() && onOff) {
            wiipad_begin();
        }
#endif
        uint32_t btns = io::getCurrentGamePadState(0).buttons |
                        io::getCurrentGamePadState(1).buttons;
        // nespad_states[] and wiipad_read() use their own bit layouts (NES
        // bus order / Wii nunchuk layout). Translate them into the same
        // io::GamePadState::Button bits the rest of this loop checks via Btn::*.
#if NES_PIN_CLK != -1 || NES_PIN_CLK_1 != -1
        auto nesToBtn = [](uint8_t s) -> uint32_t {
            // NES wire order: 0x01=Right, 0x02=Left, 0x04=Down, 0x08=Up,
            //                 0x10=Start, 0x20=Select, 0x40=B, 0x80=A.
            uint32_t b = 0;
            if (s & 0x01) b |= Btn::RIGHT;
            if (s & 0x02) b |= Btn::LEFT;
            if (s & 0x04) b |= Btn::DOWN;
            if (s & 0x08) b |= Btn::UP;
            if (s & 0x10) b |= Btn::START;
            if (s & 0x20) b |= Btn::SELECT;
            if (s & 0x40) b |= Btn::B;
            if (s & 0x80) b |= Btn::A;
            return b;
        };
#endif
#if NES_PIN_CLK != -1
        btns |= nesToBtn(nespad_states[0]);
#endif
#if NES_PIN_CLK_1 != -1
        btns |= nesToBtn(nespad_states[1]);
#endif
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
        {
            // wiipad_read() layout (see wiipad.cpp): A=1<<0, B=1<<1, SELECT=1<<2,
            // START=1<<3, UP=1<<4, DOWN=1<<5, LEFT=1<<6, RIGHT=1<<7, X=1<<8, Y=1<<9.
            uint16_t w = wiipad_read();
            if (w & (1 << 0)) btns |= Btn::A;
            if (w & (1 << 1)) btns |= Btn::B;
            if (w & (1 << 2)) btns |= Btn::SELECT;
            if (w & (1 << 3)) btns |= Btn::START;
            if (w & (1 << 4)) btns |= Btn::UP;
            if (w & (1 << 5)) btns |= Btn::DOWN;
            if (w & (1 << 6)) btns |= Btn::LEFT;
            if (w & (1 << 7)) btns |= Btn::RIGHT;
            if (w & (1 << 8)) btns |= Btn::X;
            if (w & (1 << 9)) btns |= Btn::Y;
        }
#endif
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
                // Idle: a fresh LEFT/RIGHT picks the neighbour.
                // Wraps around at the ends so the user can keep cycling.
                // gui_buf_next() is NULL on SRAM-only builds (no PSRAM) --
                // in that case we snap-load the new image instead of sliding.
                bool right = (pushed & Btn::RIGHT) != 0;
                bool left  = (pushed & Btn::LEFT)  != 0;
                if ((right || left) && g_emu_count > 1) {
                    sel = right ? (sel + 1) % g_emu_count
                                : (sel + g_emu_count - 1) % g_emu_count;
                    LOG("%s -> sel=%d (%s)",
                        right ? "RIGHT" : "LEFT", sel, g_emus[sel].label);
                    uint16_t *next_buf = gui_buf_next();
                    if (next_buf) {
                        load_image_into(sel, next_buf, gui_next_is_half_res());
                        slide_dir = right ? +1 : -1;
                        slide_p   = 0;
                    } else {
                        // No slide buffer at all: just reload cur with the new image.
                        load_image_into(sel, gui_buf_cur(), false);
                    }
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
                           slide_p, slide_dir,
                           gui_next_is_half_res());
            if (slide_dir != 0 && slide_p >= SCREENWIDTH) {
                if (gui_next_is_half_res()) {
                    // Half-res slide: don't swap (next is a 160x120 scratch).
                    // Reload cur at full res so the static display sharpens
                    // back up. Brief snap from blocky to full-res is the
                    // tradeoff for keeping the animation on no-PSRAM configs.
                    load_image_into(sel, gui_buf_cur(), false);
                } else {
                    // Both buffers full-res: just swap pointers.
                    gui_swap_buffers();
                }
                slide_dir = 0;
                slide_p   = 0;
            }
        } else {
            drawMenu(sel, top, visible);
            DrawScreen(-1);
        }
    }
}
