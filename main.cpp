/*
 * main.cpp - Pico emuLoader: a resident .uf2 bootloader frontend for the
 *            RP2350 retro-emulator family (shared pico_shared framework).
 *
 * Boot flow:
 *   1. The RP2350 bootrom always runs this image (it owns the start of flash).
 *   2. RESUME CHECK: if we got here because a no-PSRAM emulator deliberately
 *      rebooted itself to flash a ROM (watchdog_enable), jump straight back to
 *      slot 0 / APP_BASE_ADDR. A physical reset / power-cycle does NOT set
 *      that flag, so it lands here and shows the menu.
 *   3. Init the framework, then RUNTIME-DETECT board capability:
 *      slots_supported = isPsramEnabled() && flash_capacity >= 16 MB.
 *      - slots_supported = true  -> scan slots 0..6 for resident emulators
 *        (binary_info parsed for program names), show pinned + SD entries
 *        side-by-side. Pinned launches via VTOR jump (instant, no flash).
 *      - slots_supported = false -> legacy path: list SD, flash-on-launch
 *        every time (today's bootloader behaviour, byte-for-byte).
 *
 * The flash map (boot region + slot table) lives in
 * pico_shared/BootPartition.cmake and src/boot_config.h (single source).
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

// pico_shared/FrensHelpers.cpp defines this in `namespace Frens` but doesn't
// expose it in the header. Forward-declare so we can query flash size at boot.
namespace Frens { unsigned storage_get_flash_capacity(); }

extern "C" {
#include "boot_config.h"
#include "uf2_loader.h"
#include "uf2_format.h"
#include "app_launch.h"
#include "slot_scan.h"
#include "setup.h"
#include "storage.h"
}

void DrawScreen(int selectedRow, int w = 0, int h = 0, uint16_t *imagebuffer = nullptr,
                int imagex = 0, int imagey = 0);

void splash() {}  // emulator-specific stub for menu.cpp

#define CPUFREQ_KHZ 252000

#ifndef HW_CONFIG
#define HW_CONFIG 0
#endif

#define COL_FG  DEFAULT_FGCOLOR
#define COL_BG  DEFAULT_BGCOLOR

#define LOG(fmt, ...) printf("[emuLoader] " fmt "\n", ##__VA_ARGS__)

namespace {

// ----- entry model for the unified picker -------------------------------------
enum EntryKind { ENTRY_HEADER, ENTRY_PINNED, ENTRY_SD, ENTRY_SETUP };

struct Entry {
    EntryKind kind;
    int       slot;                              // pinned/sd: target slot; header/setup: -1
    char      label[40];                         // displayed
    char      filename[ROMLISTER_MAXPATH];       // SD basename for ENTRY_SD; "" otherwise
};

static inline bool entry_is_selectable(const Entry &e) { return e.kind != ENTRY_HEADER; }

constexpr int  MAX_ENTRIES = 32;
Entry          g_entries[MAX_ENTRIES];
int            g_entry_count = 0;
char           g_emuDir[32];                     // "/emu/8"
bool           g_slots_supported = false;
slot_info_t    g_slot_table[SLOT_COUNT];

// Last-launched slot is recorded just before the VTOR jump so the resume path
// can return to the right slot after a no-PSRAM watchdog reboot. The
// watchdog-scratch survives a software reboot; a power-cycle clears it.
// (Slot mode requires PSRAM so this is dormant on Fruit Jam; wired up now for
// future no-PSRAM configs that grow to >= 16 MB.)
// scratch[7] is reserved for our use; the SDK touches scratch[0..6] for various
// flags but leaves scratch[7] alone.
#define RESUME_SCRATCH 7
#define RESUME_MAGIC   0xE5E5E5E0u      /* low nibble = slot index, top 28 bits = magic */
static inline void resume_record_slot(int slot) {
    watchdog_hw->scratch[RESUME_SCRATCH] = RESUME_MAGIC | (uint32_t)(slot & 0xF);
}
static inline int resume_consume_slot(void) {
    uint32_t v = watchdog_hw->scratch[RESUME_SCRATCH];
    watchdog_hw->scratch[RESUME_SCRATCH] = 0;
    if ((v & ~0xFu) != RESUME_MAGIC) return -1;
    int s = (int)(v & 0xF);
    if (s < 0 || s >= (int)SLOT_COUNT) return -1;
    return s;
}

// ----- helpers ----------------------------------------------------------------
void makeLabelFromFilename(const char *fname, char *out, size_t n)
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

void showMessage(const char *l1, const char *l2, const char *l3)
{
    ClearScreen(COL_BG);
    if (l1) centerText(12, l1, COL_FG, COL_BG);
    if (l2) centerText(14, l2, COL_FG, COL_BG);
    if (l3) centerText(16, l3, COL_FG, COL_BG);
}

void idleFor(int ms)
{
    for (int i = 0; i < ms / 16; i++) { tuh_task(); DrawScreen(-1); sleep_ms(16); }
}

// Return slot index 0..SLOT_COUNT-1 if `target_addr` falls inside one, else -1.
int slot_for_addr(uint32_t target_addr)
{
    if (target_addr < APP_BASE_ADDR || target_addr >= SLOTS_END_ADDR) return -1;
    return (int)((target_addr - APP_BASE_ADDR) / SLOT_SIZE);
}

// Peek the first valid program block of a UF2 file and return its target_addr,
// or 0xFFFFFFFF on failure. Used to figure out which slot an SD UF2 wants.
uint32_t peek_uf2_target(const char *path)
{
    if (!storage_open(path)) return 0xFFFFFFFFu;
    uf2_block_t blk;
    uint32_t got = 0;
    uint32_t target = 0xFFFFFFFFu;
    while (storage_read(&blk, sizeof(blk), &got) && got == sizeof(blk)) {
        if (blk.magic_start0 != UF2_MAGIC_START0) continue;
        if (blk.magic_start1 != UF2_MAGIC_START1) continue;
        if (blk.magic_end    != UF2_MAGIC_END)    continue;
        if (blk.flags & UF2_FLAG_NOT_MAIN_FLASH)  continue;
        if (blk.flags & UF2_FLAG_FAMILY_ID_PRESENT) {
            if (blk.file_size_or_family != UF2_FAMILY_RP2350_ARM_S) continue;
        }
        target = blk.target_addr;
        break;
    }
    storage_close();
    return target;
}

// ----- entry builders ---------------------------------------------------------
void push_header(const char *text)
{
    if (g_entry_count >= MAX_ENTRIES) return;
    Entry &e = g_entries[g_entry_count++];
    e.kind = ENTRY_HEADER;
    e.slot = -1;
    strncpy(e.label, text, sizeof(e.label) - 1);
    e.label[sizeof(e.label) - 1] = '\0';
    e.filename[0] = '\0';
}

int add_pinned_entries()
{
    int added = 0;
    for (unsigned i = 0; i < SLOT_COUNT; i++) {
        if (!g_slot_table[i].present) continue;
        if (g_entry_count >= MAX_ENTRIES) break;
        if (added == 0) push_header("PLAY");
        Entry &e = g_entries[g_entry_count++];
        e.kind = ENTRY_PINNED;
        e.slot = (int)i;
        const char *nm = g_slot_table[i].name[0] ? g_slot_table[i].name : "(unknown)";
        snprintf(e.label, sizeof(e.label), "%-15s slot %u", nm, i);
        e.filename[0] = '\0';
        added++;
    }
    return added;
}

int add_sd_entries()
{
    Frens::RomLister lister(32 * 1024, ".uf2");
    lister.list(g_emuDir);
    int count = (int)lister.Count();
    auto *entries = lister.GetEntries();

    int added = 0;
    for (int i = 0; i < count; i++) {
        if (g_entry_count >= MAX_ENTRIES - 2) break;  // leave room for SETUP header+item

        char full[FF_MAX_LFN];
        snprintf(full, sizeof(full), "%s/%s", g_emuDir, entries[i].Path);
        uint32_t target = peek_uf2_target(full);

        // Decide what slot this SD UF2 targets and how to label it.
        int target_slot = -1;
        char suffix[24];
        if (g_slots_supported) {
            target_slot = (target == 0xFFFFFFFFu) ? -1 : slot_for_addr(target);
            if (target_slot < 0) {
                snprintf(suffix, sizeof(suffix), "?");
            } else if (g_slot_table[target_slot].present) {
                // Pinned: show what would be overwritten so the user sees it.
                const char *cur = g_slot_table[target_slot].name[0]
                                  ? g_slot_table[target_slot].name : "?";
                snprintf(suffix, sizeof(suffix), "replace %s", cur);
            } else {
                snprintf(suffix, sizeof(suffix), "-> slot %d", target_slot);
            }
        } else {
            // Legacy mode: only slot 0 / APP_BASE_ADDR-targeted UF2s are valid.
            snprintf(suffix, sizeof(suffix),
                     (target == APP_BASE_ADDR) ? "flash" : "?");
        }

        if (added == 0) push_header(g_slots_supported ? "INSTALL FROM SD" : "FROM SD");
        char shortname[20];
        makeLabelFromFilename(entries[i].Path, shortname, sizeof(shortname));

        Entry &e = g_entries[g_entry_count++];
        e.kind = ENTRY_SD;
        e.slot = target_slot;
        snprintf(e.label, sizeof(e.label), "%-15s %s", shortname, suffix);
        strncpy(e.filename, entries[i].Path, sizeof(e.filename) - 1);
        e.filename[sizeof(e.filename) - 1] = '\0';
        added++;
    }
    return added;
}

void add_setup_entry()
{
    if (g_entry_count >= MAX_ENTRIES - 1) return;
    push_header("SETUP");
    Entry &e = g_entries[g_entry_count++];
    e.kind = ENTRY_SETUP;
    e.slot = -1;
    snprintf(e.label, sizeof(e.label), "Rebuild flash from layout.txt");
    e.filename[0] = '\0';
}

// ----- rendering --------------------------------------------------------------
void drawMenu(int sel, int top, int visible)
{
    ClearScreen(COL_BG);
    centerText(0, "P I C O   e m u L o a d e r", COL_FG, COL_BG);

    char hdr[SCREEN_COLS + 1];
    snprintf(hdr, sizeof(hdr), "%s  %s",
             g_slots_supported ? "slot mode" : "legacy mode", g_emuDir);
    centerText(1, hdr, COL_FG, COL_BG);

    char bar[SCREEN_COLS + 1];
    memset(bar, ' ', SCREEN_COLS);
    bar[SCREEN_COLS] = '\0';

    for (int i = 0; i < visible && (top + i) < g_entry_count; i++) {
        int idx = top + i;
        const Entry &e = g_entries[idx];
        int row = STARTROW + i;

        if (e.kind == ENTRY_HEADER) {
            // Section label: never highlighted, never selectable.
            char line[SCREEN_COLS + 1];
            snprintf(line, sizeof(line), "  %s", e.label);
            putText(0, row, bar,  COL_FG, COL_BG);
            putText(0, row, line, COL_FG, COL_BG);
            continue;
        }

        bool seld = (idx == sel);
        int fg = seld ? COL_BG : COL_FG;
        int bg = seld ? COL_FG : COL_BG;
        putText(0, row, bar, fg, bg);
        char line[SCREEN_COLS + 1];
        snprintf(line, sizeof(line), "%c   %s",
                 seld ? '>' : ' ', e.label);
        putText(1, row, line, fg, bg);
    }
    centerText(SCREEN_ROWS - 3, "UP / DOWN : choose", COL_FG, COL_BG);
    centerText(SCREEN_ROWS - 2, "B : select", COL_FG, COL_BG);
}

// Move selection by `dir` (+1 or -1) skipping headers. Returns the new index,
// or the original if no further selectable entry exists in that direction.
int move_selection(int cur, int dir)
{
    for (int i = cur + dir; i >= 0 && i < g_entry_count; i += dir) {
        if (entry_is_selectable(g_entries[i])) return i;
    }
    return cur;
}

// First selectable entry index (start position), or -1 if none.
int first_selectable()
{
    for (int i = 0; i < g_entry_count; i++) {
        if (entry_is_selectable(g_entries[i])) return i;
    }
    return -1;
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

void setupProgress(int slot, const char *phase, uint32_t done, uint32_t total,
                   const char *filename)
{
    (void)done; (void)total;
    LOG("  setup slot %d: %s %s", slot, phase, filename);
}

// ----- actions ----------------------------------------------------------------
void action_launch_pinned(int slot)
{
    uint32_t base = SLOT_BASE(slot);
    LOG("Launching pinned slot %d at 0x%08X (%s)", slot, (unsigned)base,
        g_slot_table[slot].name);
    if (!app_launch_present_at(base)) {
        LOG("ERROR: slot %d no longer presents a valid image; refusing to launch.", slot);
        showMessage("Slot is empty.", "Run 'Setup / Rebuild flash'", nullptr);
        idleFor(3000);
        return;
    }
    resume_record_slot(slot);
    /* Reset core1 BEFORE the VTOR jump. The bootloader's framework launches
     * core1 to service HSTX; if we leave that running, the emulator's own
     * hstx_init() calls multicore_launch_core1 -> multicore_reset_core1, which
     * PSM-resets a core that may be mid-XIP-fetch and hangs the QMI. Resetting
     * here costs a brief HDMI signal blank (the emulator re-launches core1 a
     * few hundred ms into its own boot) and avoids the race. */
    multicore_reset_core1();
    stdio_flush();
    app_launch_run_at(base);
    /* Should not reach here. */
}

void action_flash_sd(const Entry &e)
{
    char full[FF_MAX_LFN];
    snprintf(full, sizeof(full), "%s/%s", g_emuDir, e.filename);
    LOG("SD selected: %s", full);

    uint32_t target = peek_uf2_target(full);
    if (target == 0xFFFFFFFFu) {
        showMessage("Could not read UF2:", e.filename, "(no valid block)");
        idleFor(3000);
        return;
    }
    LOG("  UF2 target_addr = 0x%08X", (unsigned)target);

    uint32_t base, size;
    int slot;
    if (g_slots_supported) {
        slot = slot_for_addr(target);
        if (slot < 0) {
            LOG("REJECTED: target_addr 0x%08X is outside slot range", (unsigned)target);
            showMessage("UF2 target out of range:", e.filename, "(rebuild needed)");
            idleFor(3000);
            return;
        }
        base = SLOT_BASE(slot);
        size = SLOT_SIZE;
        if (g_slot_table[slot].present) {
            LOG("  WARNING: slot %d currently holds '%s' -- will overwrite.",
                slot, g_slot_table[slot].name);
            showMessage("Overwriting slot:", g_slot_table[slot].name, "Press B again to confirm");
            // Simple confirm: poll for B-press release/press
            uint32_t prev = ~0u;
            for (int t = 0; t < 5000 / 16; t++) {
                tuh_task();
                uint32_t btns = io::getCurrentGamePadState(0).buttons
                              | io::getCurrentGamePadState(1).buttons;
                uint32_t pushed = btns & ~prev;
                prev = btns;
                if (pushed & io::GamePadState::Button::B) { goto confirmed; }
                if (pushed & io::GamePadState::Button::SELECT) {
                    LOG("  user cancelled overwrite of slot %d", slot);
                    return;
                }
                DrawScreen(-1); sleep_ms(16);
            }
            LOG("  confirmation timeout; aborting overwrite of slot %d", slot);
            return;
        confirmed:
            LOG("  user confirmed overwrite of slot %d", slot);
        }
    } else {
        // Legacy mode: only accept slot-0 / APP_BASE_ADDR UF2s.
        if (target != APP_BASE_ADDR) {
            LOG("REJECTED: legacy bootloader can only flash UF2s linked at 0x%08X",
                (unsigned)APP_BASE_ADDR);
            showMessage("Needs PSRAM + 16 MB flash", e.filename, "(slot-only UF2)");
            idleFor(3000);
            return;
        }
        slot = 0;                 // virtual: "the only app partition"
        base = APP_BASE_ADDR;
        size = APP_PARTITION_SIZE;
    }

    // Pre-flight while display is alive.
    uf2_load_stats_t st;
    uf2_load_result_t vr = uf2_validate_file_to(full, base, size, &st);
    LOG("  pre-flight: %s  blocks=%u/%u/%u  span=0x%08X..0x%08X",
        uf2_load_result_str(vr),
        (unsigned)st.total_blocks, (unsigned)st.programmed_blocks, (unsigned)st.skipped_blocks,
        (unsigned)st.lowest_addr, (unsigned)st.highest_addr);
    if (vr != UF2_LOAD_OK) {
        showMessage("Cannot flash:", e.filename, uf2_load_result_str(vr));
        idleFor(3000);
        return;
    }

    // Commit.
    char msg[40];
    snprintf(msg, sizeof(msg), "Flashing slot %d...", slot);
    showMessage(msg, e.filename, "Do not power off.");
    DrawScreen(-1);
    sleep_ms(500);
    multicore_reset_core1();
    LOG("core1 reset; flashing");
    uf2_load_result_t r = uf2_load_file_to(full, base, size, &st, flashProgress);
    if (r == UF2_LOAD_OK) {
        LOG("Flash OK; jumping to slot %d (base 0x%08X)", slot, (unsigned)base);
        if (g_slots_supported) resume_record_slot(slot);
        stdio_flush();
        app_launch_run_at(base);
    }
    LOG("Flash FAILED: %s; rebooting bootloader", uf2_load_result_str(r));
    stdio_flush();
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

void action_setup()
{
    LOG("Setup: reading layout.txt and reflashing every pinned slot");
    setup_entry_t plan[SETUP_MAX_ENTRIES];
    int n = 0;
    setup_result_t r = setup_read_layout(g_emuDir, plan, &n);
    if (r != SETUP_OK) {
        LOG("Setup failed before any flash: %s", setup_result_str(r));
        showMessage("Setup failed:", setup_result_str(r), nullptr);
        idleFor(4000);
        return;
    }
    LOG("Setup plan: %d entries", n);
    for (int i = 0; i < n; i++)
        LOG("  slot %d <- %s", plan[i].slot, plan[i].filename);

    showMessage("Setup: flashing all slots", "This may take a minute.", "Do not power off.");
    DrawScreen(-1);
    sleep_ms(500);
    multicore_reset_core1();

    r = setup_flash_all(g_emuDir, plan, n, setupProgress);
    LOG("Setup result: %s", setup_result_str(r));

    // Reboot back into the bootloader either way; on success the menu will
    // now show all newly-pinned slots.
    stdio_flush();
    watchdog_reboot(0, 0, 0);
    for (;;) tight_loop_contents();
}

} // namespace

int main()
{
    Frens::setClocksAndStartStdio(CPUFREQ_KHZ, VREG_VOLTAGE_1_30);

    LOG("---- Pico emuLoader booting ----");
    LOG("Build: %s %s   SDK: " PICO_SDK_VERSION_STRING, __DATE__, __TIME__);
    LOG("HW_CONFIG=%d  sys_clk=%lu Hz", HW_CONFIG, (unsigned long)clock_get_hz(clk_sys));
    LOG("Flash layout: bootloader [0x10000000..0x%08X)  %u slots x %u KB starting 0x%08X",
        (unsigned)APP_BASE_ADDR, SLOT_COUNT, (unsigned)(SLOT_SIZE / 1024), (unsigned)APP_BASE_ADDR);
    LOG("Boot cause: watchdog=%d  watchdog_enable=%d",
        (int)watchdog_caused_reboot(), (int)watchdog_enable_caused_reboot());

    // Resume check: if a slot deliberately rebooted itself, jump back into it.
    if (watchdog_enable_caused_reboot()) {
        int resume_slot = resume_consume_slot();
        if (resume_slot < 0) {
            LOG("Resume request without a recorded slot; checking slot 0 fallback.");
            if (app_launch_present_at(APP_BASE_ADDR)) {
                LOG("Resume into slot 0 (legacy/no-PSRAM ROM-load handshake).");
                stdio_flush();
                app_launch_run_at(APP_BASE_ADDR);
            }
        } else {
            uint32_t base = SLOT_BASE(resume_slot);
            LOG("Resume into slot %d at 0x%08X", resume_slot, (unsigned)base);
            if (app_launch_present_at(base)) {
                stdio_flush();
                app_launch_run_at(base);
            }
            LOG("Resume refused: slot %d no longer presents a valid image.", resume_slot);
        }
    } else {
        LOG("No resume: cold boot or RUN reset.");
    }

    FrensSettings::initSettings(FrensSettings::emulators::MULTI);
    char dummyRom[FF_MAX_LFN]; dummyRom[0] = '\0';
    bool sdOk = Frens::initAll(dummyRom, CPUFREQ_KHZ, 0, 0, 256, false, false);

    uint32_t flash_bytes = Frens::storage_get_flash_capacity();
    bool psram_ok = Frens::isPsramEnabled();
    g_slots_supported = psram_ok && flash_bytes >= 16u * 1024u * 1024u;
    LOG("Capability: PSRAM=%d  flash=%u KB  -> slots_supported=%d",
        (int)psram_ok, (unsigned)(flash_bytes / 1024), (int)g_slots_supported);

    screenBuffer = (charCell *)Frens::f_malloc(screenbufferSize);
    snprintf(g_emuDir, sizeof(g_emuDir), "/emu/%d", HW_CONFIG);

    if (g_slots_supported) {
        unsigned found = slot_scan(g_slot_table, SLOT_COUNT);
        LOG("Slot scan: %u of %u slots populated", found, SLOT_COUNT);
        for (unsigned i = 0; i < SLOT_COUNT; i++)
            LOG("  slot %u @ 0x%08X  present=%d  name='%s'",
                i, (unsigned)g_slot_table[i].base, (int)g_slot_table[i].present,
                g_slot_table[i].name);
    }

    if (sdOk) {
        if (g_slots_supported) add_pinned_entries();
        add_sd_entries();
        if (g_slots_supported) add_setup_entry();
    }
    LOG("Menu entries: %d", g_entry_count);

    if (!sdOk) {
        showMessage("SD card not found.", "Insert a card and reset.", nullptr);
        for (;;) { tuh_task(); DrawScreen(-1); sleep_ms(16); }
    }
    if (g_entry_count == 0) {
        char m[48];
        snprintf(m, sizeof(m), "No emulators in %s", g_emuDir);
        showMessage(m, "Drop emulator .uf2 files there", "and reset.");
        for (;;) { tuh_task(); DrawScreen(-1); sleep_ms(16); }
    }

    // ----- picker loop --------------------------------------------------------
    const int visible = ENDROW - STARTROW + 1;
    int sel = first_selectable();
    if (sel < 0) sel = 0;   // shouldn't happen — we already errored on empty entries
    int top = 0;
    uint32_t prevButtons = 0;
    using Btn = io::GamePadState::Button;

    for (;;) {
        tuh_task();
        uint32_t btns   = io::getCurrentGamePadState(0).buttons
                        | io::getCurrentGamePadState(1).buttons;
        uint32_t pushed = btns & ~prevButtons;
        prevButtons = btns;

        if (pushed & Btn::UP) {
            int n = move_selection(sel, -1);
            if (n != sel) { sel = n; LOG("UP   -> sel=%d (%s)", sel, g_entries[sel].label); }
        }
        if (pushed & Btn::DOWN) {
            int n = move_selection(sel, +1);
            if (n != sel) { sel = n; LOG("DOWN -> sel=%d (%s)", sel, g_entries[sel].label); }
        }
        if (top > sel)              top = sel;
        if (sel >= top + visible)   top = sel - visible + 1;

        if (pushed & Btn::B) {
            const Entry &e = g_entries[sel];
            LOG("B    -> sel=%d  kind=%d  '%s'", sel, (int)e.kind, e.label);
            switch (e.kind) {
            case ENTRY_PINNED: action_launch_pinned(e.slot); break;
            case ENTRY_SD:     action_flash_sd(e);            break;
            case ENTRY_SETUP:  action_setup();                break;
            case ENTRY_HEADER: /* not selectable */           break;
            }
            prevButtons = ~0u;   // require a fresh press if we returned
            continue;
        }

        drawMenu(sel, top, visible);
        DrawScreen(-1);
    }
}
