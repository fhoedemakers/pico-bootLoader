/*
 * main.cpp - Pico bootLoader: a resident .uf2 bootloader frontend for the
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
 *      .uf2 files under <BASEDIR>/<HW_CONFIG>/ (BASEDIR defaults to /emu;
 *      overridable via an optional /boot.txt INI on the SD root), identify
 *      which one is currently in the application partition by matching
 *      binary_info program names, and show a picker.
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
 * Fruit Jam, 115200-8N1). Tag every line with "[bootLoader] " so they stand out
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
#include "image_convert.h"   // C++ linkage; keep out of the extern "C" block

extern "C" {
#include "boot_config.h"
#include "uf2_loader.h"
#include "uf2_diag.h"
#include "uf2_format.h"
#include "app_launch.h"
#include "storage.h"
#include "program_name.h"
#include "emulators_txt.h"
#include "uf2_crc.h"
#include "gui.h"
#include "themes.h"
#include "screensaver.h"
#include "progress_bar.h"
#include "sd_boot_ini.h"
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

// End of usable flash for app images: build-time partition end, clamped to
// the real chip size reported by the JEDEC ID. Both the loader's runtime
// bound (set in main) and the menu's size gate (scanEmulators) derive from
// this, so the picker never lists an image the loader would refuse.
static uint32_t appFlashEnd()
{
    uint32_t cap = Frens::storage_get_flash_capacity();   // cached after first call
    return (cap >= FLASH_TOTAL_SIZE) ? APP_END_ADDR : (XIP_BASE + cap);
}

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

// On picoDVI we have to stop core1 before flashing (see flashAndLaunch
// comment), so the screen goes dark for the whole flash op. Hold the
// "screen will go blank" notice on screen long enough for the user to
// actually read it.
#define PICO_DVI_FLASH_NOTICE_MS  3500

#define LOG(fmt, ...) printf("[bootLoader] " fmt "\n", ##__VA_ARGS__)

namespace {

#define PROG_NAME_MAX 32
#define IMAGE_KEY_MAX 16
#define DISPLAY_NAME_MAX 40
#define AUX_UF2_MAX 64

struct SdEmu {
    char filename[ROMLISTER_MAXPATH];        // basename
    char label[PROG_NAME_MAX];               // shown in the menu (program_name preferred)
    char prog_name[PROG_NAME_MAX];           // binary_info match key ("" if not extractable)
    char image_key[IMAGE_KEY_MAX];           // emulators.txt column 2 ("md", "nes", ...)
    char display_name[DISPLAY_NAME_MAX];     // emulators.txt column 3 (human-readable)
    char aux_uf2[AUX_UF2_MAX];               // emulators.txt column 4 (basename of aux .uf2; "" = none)
};

char  g_emuDir[80];                   // "<BASEDIR>/<HW_CONFIG>"
SdEmu g_emus[32];
int   g_emu_count = 0;                // entries actually shown (matched in emulators.txt)
int   g_emu_seen  = 0;                // total .uf2 files found in g_emuDir, pre-filter
char  g_flash_prog_name[PROG_NAME_MAX] = {0};   // currently-flashed image's program name
int   g_flash_idx = -1;                         // index into g_emus, or -1 if none matches
// True when the in-flash image has the same program_name as g_emus[g_flash_idx]
// but its CRC32 differs from the SD .uf2 -- meaning the user dropped a newer
// build onto the card. Pressing A on that entry takes the flash path instead
// of the in-flash launch.
bool  g_flash_drift = false;

// Paths built at boot from /boot.txt (or its defaults). Consumed everywhere
// the old EMULATORS_TXT_PATH / GUI_MODE_PATH macros used to be.
char  g_index_path[144];              // "<BASEDIR>/<INDEX>"
char  g_guimode_path[80];             // "<BASEDIR>/.guimode"

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
    centerText(0, "RP2350 bootloader " SWVERSION, COL_FG, COL_BG);

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
        // Two prefix characters: selection marker, then flash-state marker.
        // ">" = cursor; "*" = currently in flash and matches SD;
        // "!" = currently in flash but SD copy differs (will reflash on launch).
        char flashMark = ' ';
        if (flashed) flashMark = g_flash_drift ? '!' : '*';
        snprintf(line, sizeof(line), "%c %c %s",
                 seld ? '>' : ' ',
                 flashMark,
                 name);
        putText(1, row, line, fg, bg);
    }

    // buttonLabel1 is the label of the button that triggers Btn::A on the
    // attached pad (e.g. "A" on NES, "B" on XInput, "O" on PlayStation).
    char buttonLabel1[2];
    char buttonLabel2[2];
    getButtonLabels(buttonLabel1, buttonLabel2);

    char hint[SCREEN_COLS + 1];
    centerText(SCREEN_ROWS - 4, "* in flash    ! SD differs (reflash)",
               COL_FG, COL_BG);
    centerText(SCREEN_ROWS - 3, "UP / DOWN : choose   SELECT : graphical", COL_FG, COL_BG);
    // '_' rather than spaces for the gap: putText collapses whitespace runs,
    // so a space-padded string would render shorter than centerText measured
    // it with strlen() and the line would sit off-centre. putText turns '_'
    // back into a literal space (menu.cpp:598).
    snprintf(hint, sizeof(hint), "%s : start____START : help", buttonLabel1);
    centerText(SCREEN_ROWS - 2, hint, COL_FG, COL_BG);
}

void showMessage(const char *l1, const char *l2, const char *l3)
{
    ClearScreen(COL_BG);
    if (l1) centerText(12, l1, COL_FG, COL_BG);
    if (l2) centerText(14, l2, COL_FG, COL_BG);
    if (l3) centerText(16, l3, COL_FG, COL_BG);
}

// --- Graphical error-screen rendering --------------------------------------
//
// The error screen is composed in two layers per frame:
//   1. Charcell layer (via DrawScreen): title in a red bar, message body in an
//      ASCII-bordered box, "Press RESET" line. Carries all the words.
//   2. Direct-framebuffer overlay (overlayErrorDecor below): diagonal yellow/
//      black caution-tape bands top + bottom, a big yellow disk with a red
//      "!" punched through the top stripe band. Pure pixel writes -- nothing
//      goes through the charcell pipeline. Drawn AFTER DrawScreen so it sits
//      on top of (and overwrites) any charcell content in those bands.
//
// Both layers are repainted every frame in the fatalErrorScreen pump loop, so
// nothing has to persist between frames.

// NesMenuPalette indices used by the charcell layer. 0x16 / 0x06 are NES reds,
// 0x30 is white. Picked to match the bright FB-direct red so the layers don't
// clash visually.
#define COL_ERR_BG  0x06
#define COL_ERR_FG  0x30

// 16-bit pixel colours for the FB-direct overlay. Two encodings depending on
// the active backend -- the same split progress_bar.cpp uses.
//   HSTX  : RGB555  (bit layout 0RRRRR GGGGG BBBBB)
//   !HSTX : RGB444  packed as 0x0RGB (this codebase's PicoDVI encoding)
#if HSTX
#define ERRPX_YELLOW  0x7FE0u
#define ERRPX_BLACK   0x0000u
#define ERRPX_RED     0x7C00u
#else
#define ERRPX_YELLOW  0x0FF0u
#define ERRPX_BLACK   0x0000u
#define ERRPX_RED     0x0F00u
#endif

// Get the active backend's 320x240 framebuffer. Returns nullptr when there
// isn't one (PicoDVI line-stream mode without a framebuffer) -- the caller
// silently skips overlay in that case and the user still gets the charcell
// layer alone.
static uint16_t *getActiveFramebuffer()
{
#if HSTX
    return (uint16_t *)hstx_getframebuffer();
#else
  #if FRAMEBUFFERISPOSSIBLE
    if (!Frens::isFrameBufferUsed()) return nullptr;
    return Frens::framebuffer;
  #else
    return nullptr;
  #endif
#endif
}

// Caution-tape stripe band: diagonal yellow/black stripes filling a horizontal
// strip [y0, y0+h). Stripe width fixed at 12 px which gives a clear "caution"
// look without going dizzy.
static void drawStripeBand(uint16_t *fb, int y0, int h)
{
    const int W = 320;
    for (int dy = 0; dy < h; dy++) {
        int y = y0 + dy;
        uint16_t *row = fb + y * W;
        for (int x = 0; x < W; x++) {
            row[x] = (((x + y) / 12) & 1) ? ERRPX_YELLOW : ERRPX_BLACK;
        }
    }
}

// Big yellow "!" disk centered horizontally at x=cx, vertically at y=cy.
// Sits half-embedded in the top stripe band so the icon visually breaks
// through it -- the whole point of having it there.
static void drawWarningDisk(uint16_t *fb, int cx, int cy, int r)
{
    const int W = 320;
    const int rSq        = r * r;
    const int rSqInner   = (r - 3) * (r - 3);
    // Yellow disk (with black outline ring carved out of the same loop).
    for (int y = cy - r; y <= cy + r; y++) {
        if (y < 0 || y >= 240) continue;
        for (int x = cx - r; x <= cx + r; x++) {
            int dx = x - cx, dy = y - cy;
            int d  = dx * dx + dy * dy;
            if (d > rSq) continue;
            fb[y * W + x] = (d >= rSqInner) ? ERRPX_BLACK : ERRPX_YELLOW;
        }
    }
    // Red "!" inside: a 5-px-wide bar above a 5x5 dot below.
    auto fillRect = [&](int x0, int y0, int x1, int y1, uint16_t col) {
        for (int y = y0; y <= y1; y++) {
            if (y < 0 || y >= 240) continue;
            for (int x = x0; x <= x1; x++) {
                if (x < 0 || x >= W) continue;
                fb[y * W + x] = col;
            }
        }
    };
    fillRect(cx - 2, cy - 14, cx + 2, cy + 4,  ERRPX_RED);  // tall bar
    fillRect(cx - 2, cy + 8,  cx + 2, cy + 13, ERRPX_RED);  // dot
}

// Direct-framebuffer overlay. Called after DrawScreen() each frame so it
// stays on top of the charcell pipeline.
static void overlayErrorDecor()
{
    uint16_t *fb = getActiveFramebuffer();
    if (!fb) return;

    drawStripeBand(fb, 0,   16);     // top caution-tape band
    drawStripeBand(fb, 224, 16);     // bottom caution-tape band
    drawWarningDisk(fb, 160, 16, 22);  // breaks through the top band
}

// Charcell layout. Avoids rows 0..1 and 28..29 since those get overwritten
// by the stripe overlay. Title and "Press RESET" use the same red bg as the
// FB-direct red so the layers blend rather than clash.
// Paint a full-width solid background row.
//
// putText() collapses consecutive whitespace -- a 40-space string ends up
// writing only one cell, leaving the rest of the row untouched. Use '_' so
// each cell is treated as a non-space character; putText converts it back to
// a literal space at write time (menu.cpp:598), giving us a proper solid-bg
// row with no visible characters.
void solidBar(int row, int fg, int bg)
{
    char bar[SCREEN_COLS + 1];
    memset(bar, '_', SCREEN_COLS);
    bar[SCREEN_COLS] = '\0';
    putText(0, row, bar, fg, bg);
}

void drawErrorScreen(const char *title, const char *l1, const char *l2, const char *l3)
{
    ClearScreen(COL_BG);

    // Title bar: solid red row with white centered title. Sits below the
    // bottom edge of the warning disk (which lands around scanline 38 ~= row 4).
    solidBar(5, COL_ERR_FG, COL_ERR_BG);
    if (title) centerText(5, title, COL_ERR_FG, COL_ERR_BG);

    // ASCII box around the three message lines (rows 10..14).
    const int boxW = 36;
    const int boxX = (SCREEN_COLS - boxW) / 2;
    char border[SCREEN_COLS + 1];
    border[0] = '+';
    for (int i = 1; i < boxW - 1; i++) border[i] = '-';
    border[boxW - 1] = '+';
    border[boxW] = '\0';

    putText(boxX, 10, border, COL_FG, COL_BG);
    for (int r = 11; r <= 13; r++) {
        putText(boxX,            r, "|", COL_FG, COL_BG);
        putText(boxX + boxW - 1, r, "|", COL_FG, COL_BG);
    }
    putText(boxX, 14, border, COL_FG, COL_BG);

    const char *lines[3] = { l1, l2, l3 };
    for (int i = 0; i < 3; i++) {
        if (lines[i]) centerText(11 + i, lines[i], COL_FG, COL_BG);
    }

    // "Press RESET" bar: another solid red row above the bottom stripe band.
    solidBar(21, COL_ERR_FG, COL_ERR_BG);
    centerText(21, "Press RESET to retry", COL_ERR_FG, COL_ERR_BG);
}

// Render the error screen and never return.
//
// The screen is entirely static (no animation, no input), so on backends with
// a persistent framebuffer (HSTX, PicoDVI in framebuffer mode) we paint once
// and then just pump USB. Redrawing every frame causes visible flicker: the
// stripe rows go briefly blank between DrawScreen (which writes charcell bg
// to those rows) and overlayErrorDecor (which paints stripes on top), and
// the DMA scanout catches that gap.
//
// PicoDVI in line-stream mode has no framebuffer; every scanline is rebuilt
// from screenBuffer on the fly, so DrawScreen must run every frame. The
// overlay does nothing there (no framebuffer to write to) -- but DrawScreen
// alone never tears because the line pipeline is single-pass.
[[noreturn]] void fatalErrorScreen(const char *title,
                                   const char *l1, const char *l2, const char *l3)
{
    LOG("FATAL: %s", title ? title : "(no title)");
    if (l1) LOG("       %s", l1);
    if (l2) LOG("       %s", l2);
    if (l3) LOG("       %s", l3);
    drawErrorScreen(title, l1, l2, l3);

    bool persistentFB = (getActiveFramebuffer() != nullptr);

    // One-shot paint. DrawScreen pushes the charcell layer into the FB (or
    // the line pipeline on line-stream); overlay then sits on top on FB-mode.
    DrawScreen(-1);
    if (persistentFB) overlayErrorDecor();

    for (;;) {
        tuh_task();
        if (!persistentFB) DrawScreen(-1);  // line-stream needs every frame
        sleep_ms(16);
    }
}

// One frame's worth of input, merged from every source the board has, in
// io::GamePadState::Button bits. Paces to 60 fps, pumps USB, blinks the
// onboard LED, and hot-plug-probes the Wii pad -- i.e. this is the whole
// per-frame housekeeping, not just the button read.
//
// Shared by the picker loop and the help screen; edge detection
// (pushed = btns & ~prev) stays with each caller since they track their own
// prevButtons. The nespad_read_start() / _finish() bracket keeps its original
// spacing: the frame-counter and LED work sits between them so the PIO shift
// register has time to clock out before we block on the result.
uint32_t readPads()
{
    using Btn = io::GamePadState::Button;

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
    // io::GamePadState::Button bits the callers check via Btn::*.
#if NES_PIN_CLK != -1 || NES_PIN_CLK_1 != -1
    auto nesToBtn = [](uint8_t s) -> uint32_t {
        // nespad_states is LSB-first wire order (A clocked out first lands
        // in bit 0): 0x01=A, 0x02=B, 0x04=Select, 0x08=Start, 0x10=Up,
        // 0x20=Down, 0x40=Left, 0x80=Right. The header comment in
        // pico_shared/nespad.cpp claims the reverse and is wrong --
        // infonesPlus ORs nespad_states[] straight into a bitmask with
        // A=1<<0..RIGHT=1<<7, which only works under this layout.
        uint32_t b = 0;
        if (s & 0x01) b |= Btn::A;
        if (s & 0x02) b |= Btn::B;
        if (s & 0x04) b |= Btn::SELECT;
        if (s & 0x08) b |= Btn::START;
        if (s & 0x10) b |= Btn::UP;
        if (s & 0x20) b |= Btn::DOWN;
        if (s & 0x40) b |= Btn::LEFT;
        if (s & 0x80) b |= Btn::RIGHT;
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
    return btns;
}

// --- Help screen ------------------------------------------------------------

// NesMenuPalette indices. 0x16 is a NES red for section headings; the title /
// footer bars reuse a blue-on-white pairing so they read as chrome rather than
// as an error (which owns the red bars).
#define COL_HELP_HDR   0x16
#define COL_BAR_FG     0x30
#define COL_BAR_BG     0x02

// Static rows of the help page. Two columns because putText() collapses
// whitespace runs -- a single string with padding between key and description
// would render with the gap squeezed to one space. col 1 marks a section
// heading (drawn in COL_HELP_HDR), col 3 / col 18 are the key / description
// columns. Rows needing runtime state are painted separately below.
struct HelpLine { uint8_t row, col; const char *text; };

const HelpLine HELP_BODY[] = {
    { 2,  1, "TEXT MODE" },
    { 3,  3, "UP / DOWN" },      { 3,  18, "select application" },
    { 5,  3, "SELECT" },         { 5,  18, "switch to graphics" },
    { 6,  3, "START" },          { 6,  18, "this help screen" },

    { 8,  1, "GRAPHICAL MODE" },
    { 9,  3, "LEFT / RIGHT" },   { 9,  18, "select application" },
    { 10, 3, "UP / DOWN" },      { 10, 18, "change artwork theme" },
    { 12, 3, "SELECT" },         { 12, 18, "switch to text mode" },
    { 13, 3, "START" },          { 13, 18, "this help screen" },

    { 15, 1, "LIST MARKERS" },
    { 16, 3, "*" },              { 16, 18, "in flash, up to date" },
    { 17, 3, "!" },              { 17, 18, "in flash, SD differs" },
    { 18, 18, "- starts by reflashing" },

    { 20, 1, "STATUS" },
    { 26, 3, "Screensaver: after 30 s idle." },
    { 27, 3, "Any button wakes it." },
};

// Paint the help page into the charcell layer. Pure charcell on purpose: it
// renders identically on all three backends and reuses the screenBuffer that
// is already allocated, so the screen costs no extra RAM.
void drawHelpScreen(bool graphical_mode, const char *index_file, bool cfg_save_failed)
{
    ClearScreen(COL_BG);

    char btn1[2], btn2[2];
    getButtonLabels(btn1, btn2);

    solidBar(0, COL_BAR_FG, COL_BAR_BG);
    centerText(0, "HELP - RP2350 bootloader " SWVERSION, COL_BAR_FG, COL_BAR_BG);

    for (const HelpLine &h : HELP_BODY) {
        putText(h.col, h.row, h.text, h.col == 1 ? COL_HELP_HDR : COL_FG, COL_BG);
    }

    // The launch button's label follows the attached pad ("A" on NES, "B" on
    // XInput, "O" on DualShock, "Z" on a keyboard), so these two rows can't
    // live in the static table.
    for (int row : { 4, 11 }) {
        putText(3,  row, btn1,                 COL_FG, COL_BG);
        putText(18, row, "start selected app", COL_FG, COL_BG);
    }

    char val[28];
    putText(3, 21, "Mode",  COL_FG, COL_BG);
    putText(18, 21, graphical_mode ? "graphical" : "text", COL_FG, COL_BG);

    // "3 (2 of 4)" -- the theme number (what THEME= in boot.txt holds), then
    // its position among the themes that exist. Listing every present theme
    // instead ran to 28 characters with all ten on the card, overflowing both
    // this buffer and the 22-cell description column. Worst case here is
    // "9 (10 of 10)", 12 characters. Single spaces only: putText collapses
    // whitespace runs, so wider padding would not survive anyway.
    {
        const int active = themes_active();
        int pos = 0;
        for (int t = 0; t <= active && t < THEME_MAX; t++) {
            if (themes_exists(t)) pos++;
        }
        snprintf(val, sizeof(val), "%d (%d of %d)", active, pos, themes_count());
        putText(3,  22, "Theme", COL_FG, COL_BG);
        putText(18, 22, val,     COL_FG, COL_BG);
    }

    putText(3,  23, "Config", COL_FG, COL_BG);
    putText(18, 23, g_emuDir, COL_FG, COL_BG);
    putText(3,  24, "Index",  COL_FG, COL_BG);
    putText(18, 24, index_file ? index_file : "", COL_FG, COL_BG);

    if (cfg_save_failed) {
        putText(3, 25, "Settings not saved - SD write failed",
                COL_ERR_FG, COL_ERR_BG);
    }

    solidBar(28, COL_BAR_FG, COL_BAR_BG);
    snprintf(val, sizeof(val), "Press START or %s to return", btn1);
    centerText(28, val, COL_BAR_FG, COL_BAR_BG);
}

// Show the help page until the user dismisses it.
//
// Unlike fatalErrorScreen(), this redraws every frame on every backend. The
// flicker rule documented above applies to screens that alternate a charcell
// pass with an FB-direct pass -- the gap between the two is what tears. The
// help page has no FB-direct layer, so a full-screen charcell rewrite is
// idempotent and safe, exactly as the picker's own text mode already does.
void showHelpScreen(bool graphical_mode, const char *index_file, bool cfg_save_failed)
{
    using Btn = io::GamePadState::Button;

    drawHelpScreen(graphical_mode, index_file, cfg_save_failed);

    // ~0u so the START press that opened this screen isn't immediately read
    // as the press that closes it.
    uint32_t prev = ~0u;
    for (;;) {
        uint32_t btns   = readPads();
        uint32_t pushed = btns & ~prev;
        prev = btns;

        DrawScreen(-1);

        if (pushed & (Btn::START | Btn::A | Btn::B | Btn::SELECT)) break;
    }
}

// --- Rejected-.uf2 error screen ---------------------------------------------
//
// Shown when flashAndLaunch()'s pre-flight validation refuses a file. That
// check runs before anything is erased, so unlike fatalErrorScreen() this page
// is dismissible and drops the user straight back into the picker.
//
// Layout and styling follow drawHelpScreen(): pure charcell, section headings
// in col 1, body in col 3. Pure charcell means it renders identically on HSTX,
// PicoDVI-framebuffer and PicoDVI-line-stream, costs nothing beyond the
// screenBuffer that is already allocated, and -- having no FB-direct layer --
// is safe to redraw every frame (see the flicker note above fatalErrorScreen).

#define ERR_COL_BODY 3   // section body
#define ERR_COL_ITEM 5   // indented sub-item (a command line, a family name)

// putText() collapses runs of whitespace AND maps '_' to a space
// (menu.cpp:589-599). Both are fatal here: this screen prints CMake flags such
// as -DBUILD_FOR_BOOTLOADER=ON that the user is meant to type back verbatim,
// underscores and all. Write the cells directly so what is on screen is exactly
// what was passed in.
void putTextRaw(int x, int y, const char *text, int fg, int bg)
{
    if (!text || x < 0 || y < 0 || y >= SCREEN_ROWS) return;
    for (int i = 0; text[i] && x + i < SCREEN_COLS; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (ch < 32 || ch > 126) ch = ' ';
        charCell &cell = screenBuffer[y * SCREEN_COLS + x + i];
        cell.charvalue = (char)ch;
        cell.fgcolor   = (uint8_t)fg;
        cell.bgcolor   = (uint8_t)bg;
    }
}

struct ErrLine { uint8_t col; const char *text; };

void drawUf2ErrorScreen(const char *filename, const uf2_diag_t *d, bool isAux)
{
    ClearScreen(COL_BG);

    char btn1[2], btn2[2];
    getButtonLabels(btn1, btn2);

    solidBar(0, COL_ERR_FG, COL_ERR_BG);
    centerText(0, isAux ? "CANNOT FLASH THIS DATA FILE" : "CANNOT FLASH THIS FILE",
               COL_ERR_FG, COL_ERR_BG);

    putText(1, 2, "FILE", COL_HELP_HDR, COL_BG);
    char shown[SCREEN_COLS + 1];
    ic_truncate_for_display(filename ? filename : "(unknown)", shown,
                            SCREEN_COLS - ERR_COL_BODY - 1);
    putTextRaw(ERR_COL_BODY, 3, shown, COL_FG, COL_BG);

    // Scratch for the lines that carry addresses. Declared here because the
    // tables below hold pointers only, so these must outlive them.
    char a0[SCREEN_COLS + 1], a1[SCREEN_COLS + 1], a2[SCREEN_COLS + 1];

    ErrLine problem[5] = {};
    ErrLine fix[9]     = {};
    int np = 0, nf = 0;
    bool docRef = false;

    auto P = [&](int col, const char *t) { if (np < 5) problem[np++] = { (uint8_t)col, t }; };
    auto F = [&](int col, const char *t) { if (nf < 9) fix[nf++]     = { (uint8_t)col, t }; };

    // How to produce an image this loader accepts. Kept in sync with the README
    // section "Creating a bootable build of your own application".
    auto appBuildRecipe = [&]() {
        F(ERR_COL_BODY, "Rebuild the application with:");
        F(ERR_COL_ITEM, "-DBUILD_FOR_BOOTLOADER=ON");
        F(ERR_COL_ITEM, "-DPICO_PLATFORM=rp2350-arm-s");
        F(ERR_COL_BODY, "and call, in its CMakeLists.txt:");
        F(ERR_COL_ITEM, "frens_offset_for_bootloader()");
        F(ERR_COL_BODY, "");
        F(ERR_COL_BODY, "Projects built on pico_shared can");
        F(ERR_COL_BODY, "use: ./bld.sh -2 -c <CONFIG> -b");
        docRef = true;
    };

    // The aux blob is a data image emitted by the application's own build (the
    // Doom WAD), so there is no separate recipe to hand the user.
    auto auxAdvice = [&]() {
        F(ERR_COL_BODY, "This is a data file, produced by");
        F(ERR_COL_BODY, "the application's own build. Copy");
        F(ERR_COL_BODY, "it again from the release, or");
        F(ERR_COL_BODY, "rebuild the application.");
    };

    switch (d->reason) {
    case UF2_DIAG_WRONG_LINK_ADDR:
        // The headline case: a standalone build still linked at 0x10000000.
        snprintf(a1, sizeof(a1), "0x%08X-0x%08X.",
                 (unsigned)d->region_base, (unsigned)(d->region_end - 1));
        if (d->have_extent) {
            snprintf(a0, sizeof(a0), "Image is linked at 0x%08X.",
                     (unsigned)d->image_base);
            P(ERR_COL_BODY, a0);
        } else {
            P(ERR_COL_BODY, "Image is linked below the app");
            P(ERR_COL_BODY, "partition.");
        }
        P(ERR_COL_BODY, "The loader can only write to");
        P(ERR_COL_ITEM, a1);
        isAux ? auxAdvice() : appBuildRecipe();
        break;

    case UF2_DIAG_WRONG_FAMILY:
        snprintf(a0, sizeof(a0), "%s (0x%08X)",
                 uf2_family_name(d->family), (unsigned)d->family);
        snprintf(a1, sizeof(a1), "%s (0x%08X)",
                 uf2_family_name(d->expected_family), (unsigned)d->expected_family);
        P(ERR_COL_BODY, "This file is built for:");
        P(ERR_COL_ITEM, a0);
        P(ERR_COL_BODY, "The loader needs:");
        P(ERR_COL_ITEM, a1);
        isAux ? auxAdvice() : appBuildRecipe();
        break;

    case UF2_DIAG_TOO_LARGE:
        snprintf(a1, sizeof(a1), "Usable flash ends at 0x%08X.",
                 (unsigned)d->region_end);
        if (d->have_extent) {
            snprintf(a0, sizeof(a0), "Image ends at 0x%08X.", (unsigned)d->image_end);
            P(ERR_COL_BODY, a0);
            P(ERR_COL_BODY, a1);
            if (d->image_end > d->region_end) {
                snprintf(a2, sizeof(a2), "%u KB too big for this board.",
                         (unsigned)((d->image_end - d->region_end + 1023) / 1024));
                P(ERR_COL_BODY, a2);
            }
        } else {
            P(ERR_COL_BODY, "The image does not fit in this");
            P(ERR_COL_BODY, "board's flash.");
            P(ERR_COL_BODY, a1);
        }
        F(ERR_COL_BODY, "Use a board with more flash, or");
        F(ERR_COL_BODY, "a smaller build of this");
        F(ERR_COL_BODY, isAux ? "data file." : "application.");
        break;

    case UF2_DIAG_CORRUPT:
        P(ERR_COL_BODY, "This is not a valid UF2 file");
        P(ERR_COL_BODY, "(bad magic, or blocks that are");
        P(ERR_COL_BODY, "misaligned or truncated).");
        F(ERR_COL_BODY, "Copy the file to the SD card");
        F(ERR_COL_BODY, "again - the copy on the card is");
        F(ERR_COL_BODY, "damaged or incomplete.");
        break;

    case UF2_DIAG_UNREADABLE:
        // Covers both a real SD read error and a file that ends mid-block,
        // which is what an interrupted copy to the card looks like.
        P(ERR_COL_BODY, "The file could not be read in");
        P(ERR_COL_BODY, "full - it is damaged, or was");
        P(ERR_COL_BODY, "copied to the card incompletely.");
        F(ERR_COL_BODY, "Check the SD card, then copy the");
        F(ERR_COL_BODY, "file again.");
        break;

    default:
        P(ERR_COL_BODY, "The loader rejected it:");
        P(ERR_COL_ITEM, uf2_load_result_str(d->result));
        isAux ? auxAdvice() : appBuildRecipe();
        break;
    }

    putText(1, 5, "PROBLEM", COL_HELP_HDR, COL_BG);
    for (int i = 0; i < np; i++) {
        putTextRaw(problem[i].col, 6 + i, problem[i].text, COL_FG, COL_BG);
    }

    if (nf > 0) {
        putText(1, 11, "HOW TO FIX", COL_HELP_HDR, COL_BG);
        for (int i = 0; i < nf; i++) {
            putTextRaw(fix[i].col, 12 + i, fix[i].text, COL_FG, COL_BG);
        }
    }

    if (docRef) {
        putTextRaw(ERR_COL_BODY, 25, "More: the pico-bootLoader README,", COL_FG, COL_BG);
        putTextRaw(ERR_COL_BODY, 26, "\"Creating a bootable build\".", COL_FG, COL_BG);
    }

    char foot[SCREEN_COLS + 1];
    solidBar(28, COL_BAR_FG, COL_BAR_BG);
    snprintf(foot, sizeof(foot), "Press START or %s to return", btn1);
    centerText(28, foot, COL_BAR_FG, COL_BAR_BG);
}

// Show the rejection page until the user dismisses it. Same pump as
// showHelpScreen(): pure charcell, so redrawing every frame is idempotent.
void showUf2ErrorScreen(const char *filename, const uf2_diag_t *d, bool isAux)
{
    using Btn = io::GamePadState::Button;

    drawUf2ErrorScreen(filename, d, isAux);

    // ~0u so the A press that started the launch isn't immediately read as the
    // press that dismisses this screen.
    uint32_t prev = ~0u;
    for (;;) {
        uint32_t btns   = readPads();
        uint32_t pushed = btns & ~prev;
        prev = btns;

        DrawScreen(-1);

        if (pushed & (Btn::START | Btn::A | Btn::B | Btn::SELECT)) break;
    }
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
//
// LED heartbeat: on picoDVI HW configs the DVI receiver loses sync during the
// ~50 ms-per-sector erase windows even with the full SRAM audit -- HSTX's
// HW-accelerated IRQ is microseconds, picoDVI's PIO encoder isn't. The
// progress bar is invisible while the screen is dark, so toggle the onboard
// LED here too: it's a direct gpio_put on core0 between flash calls (XIP is
// restored at each callback boundary), and gives the user some "still alive"
// feedback during the dark stretch. On HSTX configs the screen also stays up
// so the LED is just bonus.
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

    // LED heartbeat -- toggle on every accepted callback so the user sees
    // activity even while the picoDVI signal is gone.
    static bool led_on = false;
    led_on = !led_on;
    Frens::blinkLed(led_on);
}

// Read the SD directory, parse each emulator's program_name from its binary_info,
// build g_emus[], and locate the in-flash entry (g_flash_idx).
//
// Filtering: an .uf2 file is added to g_emus[] only if its program_name has a
// matching row in /emu/emulators.txt. Files that aren't listed (third-party
// builds, test images, .uf2 files for unrelated tools) are silently skipped --
// the picker should show only emulators that the maintainer of this card has
// curated as launchable.
void scanEmulators()
{
    g_emu_count = 0;
    g_emu_seen  = 0;

    // f_stat first: RomLister::list() silently falls back to chdir("/") when
    // its target directory doesn't exist (RomLister.cpp:118) and then lists
    // the root, which would show whatever the user has at the top of the SD
    // (e.g. 10 unrelated .uf2 files) as if they were emulators for this
    // config. Skip the lister entirely when the config dir is missing so
    // the post-scan path lands on the right "nothing here" error.
    FILINFO fi;
    FRESULT fr = f_stat(g_emuDir, &fi);
    bool dir_ok = (fr == FR_OK) && (fi.fattrib & AM_DIR);
    if (!dir_ok) {
        LOG("Config dir %s not present (f_stat=%d, attr=0x%02x); leaving emu list empty.",
            g_emuDir, (int)fr, (unsigned)fi.fattrib);
    } else {
        Frens::RomLister lister(32 * 1024, ".uf2");
        lister.list(g_emuDir);
        int count = (int)lister.Count();
        auto *entries = lister.GetEntries();

        int cap = (int)(sizeof(g_emus) / sizeof(g_emus[0]));
        if (count > cap) {
            LOG("WARNING: %d entries found, capping list at %d", count, cap);
            count = cap;
        }

        g_emu_seen  = count;
        int skipped = 0;
        uint32_t flash_end = appFlashEnd();
        for (int i = 0; i < count; i++) {
            SdEmu &e = g_emus[g_emu_count];
            strncpy(e.filename, entries[i].Path, sizeof(e.filename) - 1);
            e.filename[sizeof(e.filename) - 1] = '\0';

            char full[FF_MAX_LFN];
            snprintf(full, sizeof(full), "%s/%s", g_emuDir, e.filename);

            e.prog_name[0]    = '\0';
            e.image_key[0]    = '\0';
            e.display_name[0] = '\0';
            e.aux_uf2[0]      = '\0';
            bool ok = program_name_from_uf2_file(full, e.prog_name, sizeof(e.prog_name));
            if (!ok || !e.prog_name[0]) {
                LOG("  SKIP %s (binary_info parse failed; no program_name)", e.filename);
                skipped++;
                continue;
            }
            strncpy(e.label, e.prog_name, sizeof(e.label) - 1);
            e.label[sizeof(e.label) - 1] = '\0';

            // The emulators.txt match is mandatory: an unlisted .uf2 is not
            // shown. This is how Frank curates which emulators are "supported"
            // on this card -- the bootloader trusts the txt as the allow-list.
            bool matched = emulators_txt_lookup(e.prog_name,
                                                e.image_key,    sizeof(e.image_key),
                                                e.display_name, sizeof(e.display_name),
                                                e.aux_uf2,      sizeof(e.aux_uf2));
            if (!matched) {
                LOG("  SKIP %s (prog_name=\"%s\" not in emulators.txt)",
                    e.filename, e.prog_name);
                skipped++;
                continue;
            }

            // Size gate: hide entries whose image (or companion data image)
            // extends past the end of the actual flash chip. An extent probe
            // failure is NOT a skip -- the flash-time validate in
            // flashAndLaunch() remains the backstop for odd files.
            uint32_t lo, hi;
            if (uf2_extent_from_file_family(full, UF2_FAMILY_RP2350_ARM_S,
                                            &lo, &hi) && hi > flash_end) {
                LOG("  SKIP %s (image 0x%08X-0x%08X exceeds flash end 0x%08X, %u KB over)",
                    e.filename, (unsigned)lo, (unsigned)hi, (unsigned)flash_end,
                    (unsigned)((hi - flash_end + 1023) / 1024));
                skipped++;
                continue;
            }
            if (e.aux_uf2[0]) {
                char auxFull[FF_MAX_LFN];
                snprintf(auxFull, sizeof(auxFull), "%s/%s", g_emuDir, e.aux_uf2);
                if (uf2_extent_from_file_family(auxFull, UF2_FAMILY_RP2350_DATA,
                                                &lo, &hi) && hi > flash_end) {
                    LOG("  SKIP %s (aux %s at 0x%08X-0x%08X exceeds flash end 0x%08X)",
                        e.filename, e.aux_uf2, (unsigned)lo, (unsigned)hi,
                        (unsigned)flash_end);
                    skipped++;
                    continue;
                }
            }
            LOG("  [%2d] %-40s  prog_name=\"%s\"  img_key=\"%s\"  display=\"%s\"%s%s",
                g_emu_count, e.filename, e.prog_name, e.image_key, e.display_name,
                e.aux_uf2[0] ? "  aux=" : "", e.aux_uf2[0] ? e.aux_uf2 : "");
            g_emu_count++;
        }
        LOG("Listed %d of %d .uf2 file(s) in %s (%d skipped).",
            g_emu_count, g_emu_seen, g_emuDir, skipped);
    }

    g_flash_prog_name[0] = '\0';
    g_flash_idx = -1;
    g_flash_drift = false;
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

    // If we matched the in-flash image to an SD entry by program_name,
    // CRC32-compare the two to detect "user dropped a new build on the card".
    // If the bytes differ, set g_flash_drift so the picker reflashes on launch.
    if (g_flash_idx >= 0) {
        char full[FF_MAX_LFN];
        snprintf(full, sizeof(full), "%s/%s", g_emuDir, g_emus[g_flash_idx].filename);

        uf2_fingerprint_t fp = {0};
        if (uf2_fingerprint_from_file(full, &fp)) {
            uint32_t flash_crc = 0;
            if (uf2_fingerprint_from_xip(fp.image_base, fp.image_size, &flash_crc)) {
                if (flash_crc != fp.crc) {
                    g_flash_drift = true;
                    LOG("DRIFT: SD CRC=0x%08X  flash CRC=0x%08X  -> reflash on launch",
                        (unsigned)fp.crc, (unsigned)flash_crc);
                } else {
                    LOG("In-flash image matches SD copy (CRC 0x%08X, %u bytes).",
                        (unsigned)fp.crc, (unsigned)fp.image_size);
                }
            } else {
                LOG("WARN: XIP fingerprint failed (base=0x%08X size=%u)",
                    (unsigned)fp.image_base, (unsigned)fp.image_size);
            }
        } else {
            LOG("WARN: SD fingerprint failed for %s", full);
        }
    }
}

// AuxState tells the launch dispatcher how to handle a row's aux blob.
//   NO_AUX  : emulators.txt has no 4th column for this row -- ignore aux entirely.
//   MATCH   : SD file's CRC matches the bytes already at its target XIP address.
//   DRIFT   : SD and flash differ (or the flash region is blank) -- reflash needed.
//   ERROR   : couldn't fingerprint the SD file -- log and proceed without flashing.
enum AuxState { AUX_NO_AUX, AUX_MATCH, AUX_DRIFT, AUX_ERROR };

static AuxState computeAuxDrift(int idx, uf2_fingerprint_t *out_fp)
{
    if (out_fp) *out_fp = {};
    if (idx < 0 || idx >= g_emu_count) return AUX_NO_AUX;
    const char *aux = g_emus[idx].aux_uf2;
    if (!aux[0]) return AUX_NO_AUX;

    char full[FF_MAX_LFN];
    snprintf(full, sizeof(full), "%s/%s", g_emuDir, aux);

    uf2_fingerprint_t fp = {0};
    if (!uf2_fingerprint_from_file_family(full, UF2_FAMILY_RP2350_DATA, &fp)) {
        LOG("WARN: aux fingerprint failed for %s -- launching without flashing it", full);
        return AUX_ERROR;
    }
    if (out_fp) *out_fp = fp;

    uint32_t flash_crc = 0;
    if (!uf2_fingerprint_from_xip(fp.image_base, fp.image_size, &flash_crc)) {
        LOG("WARN: aux XIP fingerprint failed (base=0x%08X size=%u)",
            (unsigned)fp.image_base, (unsigned)fp.image_size);
        return AUX_DRIFT;   // safest: reflash
    }
    if (flash_crc == fp.crc) {
        LOG("Aux blob already in flash (CRC 0x%08X, %u bytes at 0x%08X); skipping.",
            (unsigned)fp.crc, (unsigned)fp.image_size, (unsigned)fp.image_base);
        return AUX_MATCH;
    }
    LOG("Aux drift: SD CRC=0x%08X  flash CRC=0x%08X  -> reflash on launch",
        (unsigned)fp.crc, (unsigned)flash_crc);
    return AUX_DRIFT;
}

// Do the final "hand off to the emulator" sequence: quiesce I2C/core1, mark
// the handshake register, and VTOR-jump. Never returns on success.
[[noreturn]] void handoffToApp(const char *label)
{
    LOG("Launching %s; bye!", label ? label : "(unknown)");
    stdio_flush();
#if WII_PIN_SDA >= 0 and WII_PIN_SCL >= 0
    wiipad_end();
#endif
    multicore_reset_core1();   // hand HSTX over; emulator brings its own driver up
    Frens::markLaunchedFromBootloader();
    app_launch_run();          // VTOR jump; no return on success
    LOG("app_launch_run() returned unexpectedly.");
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

// Launch the already-flashed emulator. No flash op; just quiesce and jump.
void launchInFlash()
{
    LOG("Launching in-flash emulator: %s", g_flash_prog_name);
    if (!app_launch_present()) {
        LOG("ERROR: app_launch_present()=false at launch time; refusing.");
        showMessage("App partition is empty.", "Pick an emulator to flash.", nullptr);
        idleFor(2500);
        return;
    }
    handoffToApp(g_flash_prog_name);
}

// Flash paths (emulator .uf2 and optional aux .uf2) into flash and launch the
// emulator. Either or both flashes can be requested:
//   flashEmu=true  : write g_emus[idx].filename to the app partition.
//   flashAux=true  : write g_emus[idx].aux_uf2 at *auxFp's target address.
// If both are false, callers should use launchInFlash() instead -- but this
// function still handles that case gracefully by just jumping.
void flashAndLaunch(int idx, bool flashEmu, bool flashAux, const uf2_fingerprint_t *auxFp)
{
    char full[FF_MAX_LFN];
    snprintf(full, sizeof(full), "%s/%s", g_emuDir, g_emus[idx].filename);
    LOG("Flash & launch: [%d] %s  flashEmu=%d flashAux=%d",
        idx, full, (int)flashEmu, (int)flashAux);

    uf2_load_stats_t st;

    // Pre-flight the emulator UF2 first (display still alive). Skip if the
    // caller just wants the aux flashed.
    if (flashEmu) {
        LOG("Pre-flight validating emulator UF2 (pass 1, no flash writes)...");
        uf2_load_result_t vr = uf2_validate_file(full, &st);
        LOG("  result: %s", uf2_load_result_str(vr));
        if (vr != UF2_LOAD_OK) {
            // Nothing has been erased yet, so this is recoverable: diagnose why
            // the file was refused and hold a dismissible page up until the user
            // acknowledges it, rather than flashing a terse notice for 3 s and
            // dropping back into the picker as if nothing happened.
            uf2_diag_t d;
            uf2_diagnose(full, vr, APP_BASE_ADDR, appFlashEnd(),
                         UF2_FAMILY_RP2350_ARM_S, &d);
            LOG("REJECTED: %s -> %s (image 0x%08X-0x%08X%s, family 0x%08X, "
                "first out-of-range block 0x%08X)",
                uf2_load_result_str(vr), uf2_diag_reason_str(d.reason),
                (unsigned)d.image_base, (unsigned)d.image_end,
                d.have_extent ? "" : ", unknown", (unsigned)d.family,
                (unsigned)st.lowest_out_of_range);
            showUf2ErrorScreen(g_emus[idx].filename, &d, false);
            return;
        }
    }

    // Pre-flight the aux UF2 too, using its own target range (derived from
    // the fingerprint pass, so we don't re-scan the file).
    char auxFull[FF_MAX_LFN] = {0};
    if (flashAux) {
        if (!auxFp || !g_emus[idx].aux_uf2[0]) {
            LOG("BUG: flashAux=true but no aux fingerprint / column");
            return;
        }
        snprintf(auxFull, sizeof(auxFull), "%s/%s", g_emuDir, g_emus[idx].aux_uf2);
        LOG("Pre-flight validating aux UF2 %s at [0x%08X..0x%08X)...",
            auxFull, (unsigned)auxFp->image_base,
            (unsigned)(auxFp->image_base + auxFp->image_size));
        uf2_load_stats_t astp;
        uf2_load_result_t vr = uf2_validate_file_ex(auxFull,
            auxFp->image_base, auxFp->image_base + auxFp->image_size,
            UF2_FAMILY_RP2350_DATA, &astp);
        LOG("  result: %s", uf2_load_result_str(vr));
        if (vr != UF2_LOAD_OK) {
            uf2_diag_t d;
            uf2_diagnose(auxFull, vr,
                         auxFp->image_base, auxFp->image_base + auxFp->image_size,
                         UF2_FAMILY_RP2350_DATA, &d);
            LOG("REJECTED aux: %s -> %s (image 0x%08X-0x%08X%s, family 0x%08X)",
                uf2_load_result_str(vr), uf2_diag_reason_str(d.reason),
                (unsigned)d.image_base, (unsigned)d.image_end,
                d.have_extent ? "" : ", unknown", (unsigned)d.family);
            showUf2ErrorScreen(g_emus[idx].aux_uf2, &d, true);
            return;
        }
    }

    // Build a short header line naming what's being flashed. The user cares
    // about "Flashing" more than which one; keep the extra line short.
    // Prefer display_name so this repaint matches the instant acknowledgment
    // the picker loop already put up at A-press time (no visible text swap).
    const char *line1 = "Flashing";
    const char *line2 = g_emus[idx].display_name[0] ?
                        g_emus[idx].display_name : g_emus[idx].label;
    const char *line3 = "Do not power off.";
    if (flashAux && !flashEmu) {
        line1 = "Flashing WAD";
    } else if (flashAux && flashEmu) {
        line3 = "WAD + emulator...";
    }

#if HSTX
    // HSTX path: hardware-accelerated TMDS encoding gives microsecond IRQs
    // on core1, so it keeps running cleanly through every flash erase/write
    // window. The live progress bar updates in real time and the audit in
    // framework-flash-while-running.md keeps core1 SRAM-only throughout.
    showMessage(line1, line2, line3);
    DrawScreen(-1);
    idleFor(FLASH_NOTICE_MS);

    // Paint an empty 0% bar so the user sees the bar's box appear before any
    // flash writes start. Colours are compile-time literals (see PB_COL_*),
    // so the flash callback never has to dereference flash for them.
    progress_bar_draw(0, 100, PB_COL_FILL, PB_COL_EMPTY, PB_COL_BORDER);

    LOG("Pre-flight OK. core1 left running; beginning flash sequence.");
#else
    // picoDVI path: software TMDS encoding on core1 (PIO encoder) is too
    // timing-sensitive to survive multi-second flash ops -- the DVI
    // receiver drops sync inside the first sector erase even with a full
    // SRAM audit of core1. Stop core1 entirely before flashing so it can't
    // hardfault on anything; the screen goes intentionally dark and the
    // LED heartbeat in flashProgress() carries progress for the user.
    showMessage("Screen will go blank.",
                LED_GPIO_PIN == -1 ? "" :"Watch LED for progress.",
                "Be patient...");
    DrawScreen(-1);
    idleFor(PICO_DVI_FLASH_NOTICE_MS);

    LOG("Pre-flight OK. picoDVI: stopping core1 before flash sequence.");
    multicore_reset_core1();
#endif

    // Aux blob first (small, and we want it in place before the emulator
    // boots and reads from it). See plan for the ordering rationale.
    if (flashAux) {
        LOG("Flashing aux blob %s ...", auxFull);
        uf2_load_stats_t ast;
        uf2_load_result_t r = uf2_load_file_ex(auxFull,
            auxFp->image_base, auxFp->image_base + auxFp->image_size,
            UF2_FAMILY_RP2350_DATA, &ast, flashProgress);
        if (r != UF2_LOAD_OK) {
            LOG("Aux flash FAILED: %s (after %u programmed, %u skipped)",
                uf2_load_result_str(r),
                (unsigned)ast.programmed_blocks, (unsigned)ast.skipped_blocks);
            stdio_flush();
            watchdog_reboot(0, 0, 0);
            for (;;) tight_loop_contents();
        }
        LOG("Aux flash OK: %u blocks written to 0x%08X..0x%08X",
            (unsigned)ast.programmed_blocks,
            (unsigned)ast.lowest_addr, (unsigned)ast.highest_addr);
#if HSTX
        // Reset the bar to 0% for the next phase so the second flash starts
        // fresh instead of finishing full-on-full.
        if (flashEmu) progress_bar_draw(0, 100, PB_COL_FILL, PB_COL_EMPTY, PB_COL_BORDER);
#endif
    }

    if (flashEmu) {
        LOG("Flashing emulator %s ...", full);
        uf2_load_result_t r = uf2_load_file(full, &st, flashProgress);
        if (r != UF2_LOAD_OK) {
            LOG("Flash FAILED: %s (after %u programmed, %u skipped)",
                uf2_load_result_str(r),
                (unsigned)st.programmed_blocks, (unsigned)st.skipped_blocks);
            stdio_flush();
            watchdog_reboot(0, 0, 0);
            for (;;) tight_loop_contents();
        }
        LOG("Flash OK: %u blocks written to 0x%08X..0x%08X",
            (unsigned)st.programmed_blocks,
            (unsigned)st.lowest_addr, (unsigned)st.highest_addr);
    }

#if HSTX
    // Force a final 100% paint before we tear down core1.
    progress_bar_draw(100, 100, PB_COL_FILL, PB_COL_EMPTY, PB_COL_BORDER);
#endif
    logAppPartitionState("after flash");
    if (app_launch_present()) {
        handoffToApp(g_emus[idx].label);
    }
    LOG("ERROR: app_launch_present()=false after flash; rebooting.");

    // The image flashed and verified but has no usable vector table, so we are
    // about to reboot into the menu. Say so first: an unexplained bounce back
    // to the picker right after a full progress bar is exactly the "it silently
    // did nothing" experience this screen work exists to remove.
    //
    // HSTX only -- the picoDVI path reset core1 before flashing, so there is no
    // display left to draw on and the LED heartbeat is all the user gets.
#if HSTX
    showMessage("Flashed, but the app will",
                "not start. Returning to menu.",
                "Rebuild it for the bootloader.");
    DrawScreen(-1);
    idleFor(4000);
#endif

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
    LOG("---- Pico bootLoader booting ----");
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
    // If the previously-running emulator asked to return to the picker
    // (Frens::rebootToBootloader() before its watchdog_reboot), honour that
    // request and fall through to the menu even though watchdog_enable
    // would otherwise trigger the resume jump.
    bool returnRequested = Frens::consumeReturnToBootloaderRequest();
    if (returnRequested) {
        LOG("Return-to-loader requested by app; skipping resume jump.");
    }
    if (!returnRequested && watchdog_enable_caused_reboot() && app_launch_present()) {
        LOG("Resume path: watchdog_enable=true and app image valid");
        LOG("Jumping to app reset vector at 0x%08X (no return on success)",
            (unsigned)((const uint32_t *)APP_BASE_ADDR)[1]);
        stdio_flush();
        Frens::markLaunchedFromBootloader();
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
    // useFrameBuffer=true: HSTX always has its own FB; on RP2350 PicoDVI
    // (FRAMEBUFFERISPOSSIBLE) this picks the framebuffer path instead of
    // line-streaming, which is what the FB-direct overlays (progress bar,
    // error screen decor) need to have something to write to. RP2040 is
    // out of scope -- FRAMEBUFFERISPOSSIBLE is false there and the flag
    // is ignored at the !HSTX && FRAMEBUFFERISPOSSIBLE gate in
    // FrensHelpers.cpp:1639.
    //
    // audiobufferSize=1024 (not 256): in PicoDVI framebuffer mode the
    // emulators all use 1024; pico-infonesPlus main.cpp explicitly notes
    // "When using framebuffer, AUDIOBUFFERSIZE must be increased to 1024".
    // (256 was once blamed for an intermittent startup deadlock here --
    // core1 wedged in the DVI DMA IRQ, core0 in PaceFrames60fps waiting
    // for vsync. Real cause found 2026-07: our global -Os left
    // std::lock_guard & friends out of line in flash, so the DMA IRQ
    // fetched flash code every scanline and, under core0 XIP/SD traffic,
    // could miss its control-block reload deadline, silently killing the
    // DMA chain. Fixed in pico_lib (SpinLockGuard etc.); the buffer size
    // only shifted the timing.)
    bool sdOk = Frens::initAll(dummyRom, CPUFREQ_KHZ, 0, 0, 1024, false, true);
    LOG("initAll done. SD mounted=%d  PSRAM=%d  framebufferUsed=%d",
        (int)sdOk, (int)Frens::isPsramEnabled(), (int)Frens::isFrameBufferUsed());

    // Force 1:1 scaling so the full 320x240 framebuffer is shown. The global
    // scaleMode8_7_ defaults to true and is read by the DVI core1 render loop
    // to pick convertScanBuffer12bppScaled16_7 (clips 34 src px off the left)
    // vs convertScanBuffer12bpp (1:1). applyScreenMode returns the new value
    // -- we must assign it to the extern, like pico-infonesPlus does.
    scaleMode8_7_ = Frens::applyScreenMode(ScreenMode::NOSCANLINE_1_1);
    if (sdOk) {
        char fstype[16] = {0};
        Frens::getFsInfo(fstype, sizeof(fstype));
        LOG("Filesystem: %s", fstype);
    }

    // Clamp the loader's app-partition end to the real chip capacity. The
    // build-time APP_END_ADDR assumes a 16 MB chip (Fruit Jam); on a smaller
    // chip we'd otherwise let an over-large image march past real flash.
    {
        uint32_t end = appFlashEnd();
        uf2_loader_set_app_end_addr(end);
        LOG("Flash capacity (JEDEC): %u bytes  app-end clamp: 0x%08X",
            (unsigned)Frens::storage_get_flash_capacity(), (unsigned)end);
    }
    screenBuffer = (charCell *)Frens::f_malloc(screenbufferSize);
    LOG("Allocated %u-byte screenBuffer at %p", (unsigned)screenbufferSize, screenBuffer);

    if (!sdOk) {
        fatalErrorScreen("SD CARD NOT FOUND",
                         "Insert a FAT-formatted card",
                         "and reset.",
                         nullptr);
    }

    // Load /boot.txt. Missing file -> defaults ("/emu", "emulators.txt",
    // STARFIELD). Present but malformed -> fatal, so the user knows the
    // config didn't take effect instead of silently reverting to defaults.
    sd_boot_ini_t ini;
    char ini_err[64] = {0};
    LOG("Loading /boot.txt (optional)...");
    if (sd_boot_ini_load("/boot.txt", &ini, ini_err, sizeof(ini_err)) != SD_BOOT_INI_OK) {
        fatalErrorScreen("BOOT.TXT INVALID",
                         ini_err,
                         "Fix /boot.txt and reset.",
                         nullptr);
    }
    LOG("boot_ini: BASEDIR=%s INDEX=%s SCREENSAVER=%s GUI=%d THEME=%u",
        ini.base_dir, ini.index_file,
        ini.screensaver == SS_MODE_STARFIELD ? "STARFIELD" : "BLOCKS",
        (int)ini.gui_graphical, (unsigned)ini.theme);

    snprintf(g_emuDir,       sizeof(g_emuDir),       "%s/%d",      ini.base_dir, HW_CONFIG);
    snprintf(g_index_path,   sizeof(g_index_path),   "%s/%s",      ini.base_dir, ini.index_file);
    snprintf(g_guimode_path, sizeof(g_guimode_path), "%s/.guimode", ini.base_dir);
    gui_set_asset_dir(ini.base_dir);
    screensaver_set_asset_dir(ini.base_dir);
    screensaver_set_mode(ini.screensaver);
    themes_init(ini.base_dir);

    // Legacy .guimode -> /boot.txt GUI=. Only when boot.txt doesn't already
    // carry the key, so an explicit GUI= always wins. Runs once ever: the file
    // is deleted as soon as the value is safely in boot.txt.
    {
        FILINFO gi;
        if (!ini.seen_gui && f_stat(g_guimode_path, &gi) == FR_OK) {
            ini.gui_graphical = gui_load_mode(g_guimode_path);
            LOG("Migrating %s -> /boot.txt GUI=%d", g_guimode_path, (int)ini.gui_graphical);
            if (sd_boot_ini_save("/boot.txt", &ini)) f_unlink(g_guimode_path);
            else LOG("boot.txt write failed; keeping %s for now", g_guimode_path);
        }
    }

    // Cards written before themes existed keep their artwork loose in
    // <BASEDIR>/assets. Move it into themes/0 so there is always a theme 0.
    if (themes_migration_needed()) {
        showMessage("Updating SD card layout...", "assets -> assets/themes/0", nullptr);
        DrawScreen(-1);
        themes_migrate_default();
    }
    themes_scan();
    {
        int theme = ini.theme;
        if (!themes_exists(theme)) {
            // Configured theme was deleted from the card (or never existed).
            int fallback = themes_exists(0) ? 0 : themes_lowest();
            theme = (fallback >= 0) ? fallback : 0;
            LOG("Theme %u not on card; using %d", (unsigned)ini.theme, theme);
            ini.theme = (uint8_t)theme;
        }
        themes_set_active(theme);

        char td[80];
        themes_dir_of(theme, td, sizeof(td));
        gui_set_theme_dir(td);
        if (themes_exists(0)) {
            themes_dir_of(0, td, sizeof(td));
            gui_set_theme_fallback_dir(td);
        } else {
            // No theme 0 to fall back to -- misses go straight to black.
            gui_set_theme_fallback_dir(nullptr);
        }
        LOG("themes: mask=0x%03X count=%d active=%d",
            (unsigned)themes_mask(), themes_count(), theme);
    }

    // Pre-load the emulators.txt (or user-renamed) index. It's the allow-list
    // that scanEmulators() filters SD .uf2 files against, so a missing or
    // empty file means we'd hide every emulator and confuse the user with a
    // "no matching emulators" screen. Name the real cause directly instead.
    LOG("Loading %s...", g_index_path);
    if (!emulators_txt_load(g_index_path)) {
        fatalErrorScreen("INDEX FILE MISSING",
                         "Place the index file at",
                         g_index_path,
                         "and reset.");
    }

    // Parse program_name from every .uf2 on SD and from the in-flash image.
    // Briefly tell the user what's happening (this can take a second or two
    // while we seek through 5+ files on slow SD cards).
    showMessage("Scanning emulators...", g_emuDir, nullptr);
    DrawScreen(-1);
    LOG("Scanning %s for *.uf2 and parsing binary_info...", g_emuDir);
    scanEmulators();

    if (g_emu_count == 0) {
        char m[48];
        if (g_emu_seen == 0) {
            // No .uf2 files at all in the config dir.
            snprintf(m, sizeof(m), "Nothing in %s", g_emuDir);
            fatalErrorScreen("NO EMULATORS FOUND",
                             m,
                             "Copy emulator .uf2 files there",
                             "and reset.");
        } else {
            // .uf2 files exist but none are listed in the index -- either
            // the file is missing, or its entries don't match any prog_name.
            snprintf(m, sizeof(m), "%d UF2 file(s) found in %s",
                     g_emu_seen, g_emuDir);
            fatalErrorScreen("NO MATCHING EMULATORS",
                             m,
                             "but none listed in",
                             g_index_path);
        }
    }

    // Materialise .444/.555 caches for any PNG/JPG images on the card. On
    // boards without PSRAM this must happen NOW: the converter needs ~60 KB
    // of contiguous SRAM heap (dominated by the PNG decoder state) and that
    // stops being available once the GUI slide buffers (~190 KB) are
    // allocated below. PSRAM boards keep the existing behaviour -- picker
    // tiles convert lazily on first view and the screensaver batches at
    // first activation -- since their scratch lives in lwmem, not here.
    //
    // Every present theme is converted, not just the active one: switching
    // theme at runtime must not need the converter, which by then has no heap
    // left to run in.
    if (!Frens::isPsramEnabled()) {
        themes_convert_all();
        screensaver_convert_batch();
    }

    // --- PICKER LOOP --------------------------------------------------------
    LOG("Entering picker loop. D-pad: navigate, A: start, SELECT: toggle graphical, START: help.");
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

    // Screensaver state. Activates after 30 s of no input; loads up to 5
    // small bouncing images from /emu/assets/screensaver/ and exits on any
    // button press. `ss_unavailable` latches when the first init attempt
    // finds nothing usable so we don't keep retrying every frame.
    constexpr uint32_t SS_IDLE_THRESHOLD = 30 * 60;   // 30 s @ 60 fps
    uint32_t idle_frames    = 0;
    bool     ss_active      = false;
    bool     ss_unavailable = false;

    // Persist GUI= / THEME= to /boot.txt. Failure is deliberately non-fatal:
    // a write-protected or full card must not stop the picker from working,
    // so the change still takes effect for this session and the help screen
    // reports that it wasn't saved.
    bool cfg_save_failed = false;
    auto persist_cfg = [&]() {
        if (sd_boot_ini_save("/boot.txt", &ini)) {
            cfg_save_failed = false;
        } else {
            LOG("boot.txt write FAILED; setting kept in RAM only");
            cfg_save_failed = true;
        }
    };

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

    // Restore the last mode the user left us in (/boot.txt GUI= key).
    graphical_mode = ini.gui_graphical;
    LOG("Initial menu mode: %s", graphical_mode ? "graphical" : "text");
    if (graphical_mode) enter_graphical();

    for (;;) {
        uint32_t btns   = readPads();
        uint32_t pushed = btns & ~prevButtons;
        prevButtons = btns;

        // Idle counter feeds the screensaver. Tick only while no button is
        // pressed -- a held button is not "idle".
        if (btns) idle_frames = 0;
        else      idle_frames++;

        if (ss_active) {
            if (btns) {
                // Any press exits. The press itself must NOT also drive
                // navigation, A-launch, or SELECT-mode-toggle this frame --
                // the user just meant "wake up". Setting prevButtons = ~0u
                // prevents *future* frames from seeing this press as a fresh
                // edge, and the `continue` below stops this frame's already-
                // computed `pushed` (which still contains the wake button)
                // from reaching the rest of the loop.
                screensaver_free();
                ss_active   = false;
                idle_frames = 0;
                prevButtons = ~0u;
                continue;
            } else {
                screensaver_run_one_frame();
                continue;   // skip nav + normal render while running
            }
        } else if (!ss_unavailable && idle_frames >= SS_IDLE_THRESHOLD) {
            if (screensaver_init()) ss_active = true;
            else                    ss_unavailable = true;
        }

        // START: help screen. Checked before SELECT so opening help can never
        // also toggle the menu mode on the same frame.
        if (pushed & Btn::START) {
            showHelpScreen(graphical_mode, ini.index_file, cfg_save_failed);
            prevButtons = ~0u;   // swallow the press that dismissed it
            idle_frames = 0;
            continue;            // repaint from scratch next frame
        }

        // SELECT: toggle modes regardless. Persist so next boot lands the same way.
        if (pushed & Btn::SELECT) {
            graphical_mode = !graphical_mode;
            LOG("SELECT -> mode=%s", graphical_mode ? "graphical" : "text");
            ini.gui_graphical = graphical_mode;
            persist_cfg();
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

                // UP/DOWN cycles the artwork theme (they do nothing else in
                // graphical mode). Only themes actually present on the card
                // are reachable. No slide: this repaints the SAME entry, so a
                // transition would be staging the neighbour buffer for nothing.
                bool tdn = (pushed & Btn::DOWN) != 0;
                bool tup = (pushed & Btn::UP)   != 0;
                if ((tdn || tup) && themes_count() > 1) {
                    int t = themes_next(themes_active(), tdn ? +1 : -1);
                    if (t != themes_active()) {
                        themes_set_active(t);
                        char td[80];
                        themes_dir_of(t, td, sizeof(td));
                        gui_set_theme_dir(td);
                        LOG("%s -> theme=%d", tdn ? "DOWN" : "UP", t);
                        ini.theme = (uint8_t)t;
                        persist_cfg();
                        load_image_into(sel, gui_buf_cur(), false);
                        slide_p   = 0;
                        slide_dir = 0;
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
            LOG("A pressed. sel=%d (%s) flashed_idx=%d emu_drift=%d aux=\"%s\"",
                sel, g_emus[sel].label, g_flash_idx, (int)g_flash_drift,
                g_emus[sel].aux_uf2);
            bool emuDrift = (sel != g_flash_idx) || g_flash_drift;

            // Acknowledge the press on screen BEFORE any SD I/O.
            // computeAuxDrift() CRC-walks a possibly multi-MB aux .uf2 and
            // flashAndLaunch() pre-flight-validates the whole emulator .uf2;
            // on a slow card that is several seconds during which a frozen
            // menu reads as a hang. emuDrift comes from state cached at scan
            // time, so we already know whether this press flashes or just
            // launches and can show the right message immediately.
            const char *dispName = g_emus[sel].display_name[0] ?
                                   g_emus[sel].display_name : g_emus[sel].label;
            if (emuDrift) showMessage("Flashing", dispName, "Do not power off.");
            else          showMessage("Starting", dispName, nullptr);
            DrawScreen(-1);

            uf2_fingerprint_t auxFp;
            AuxState auxState = computeAuxDrift(sel, &auxFp);
            bool flashAux = (auxState == AUX_DRIFT);

            if (emuDrift || flashAux) {
                flashAndLaunch(sel, emuDrift, flashAux, flashAux ? &auxFp : nullptr);
            } else {
                launchInFlash();
            }
            // If we get here, either the launch was refused or flash failed;
            // both paths reboot the bootloader so we shouldn't actually reach
            // this. Force a fresh button read just in case.
            prevButtons = ~0u;
        }

        // Render.
        if (graphical_mode && buffers_ready) {
            // Rebuilt every frame so the A-button label tracks pad hot-plug
            // (NES "A", XInput "B", DualShock "O", ...). gui only caches
            // the pointer so the stack buffer must live across the call,
            // which it does -- we set it immediately before gui_draw_frame.
            char bl1[2], bl2[2];
            getButtonLabels(bl1, bl2);
            char footer[SCREEN_COLS + 1];
            snprintf(footer, sizeof(footer),
                     "L/R:app  U/D:theme  %s:go  START:help", bl1);
            gui_set_footer(footer);
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
