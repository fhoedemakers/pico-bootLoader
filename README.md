# pico-bootLoader



pico-bootLoader is a bootloader for RP2350 boards. Its primary purpose is to
host a collection of retro-game emulators and a port of *Doom* on a single
board and to let the user choose which one to run from an on-screen menu,
without reconnecting the board to a computer.

It is not limited to emulation, though: any RP2350 application can be made
bootable and added to the menu — see [Creating a bootable build of your own
application](#creating-a-bootable-build-of-your-own-application).

An RP2350 board normally holds a single program. Running a different one means
connecting it to a PC, holding BOOTSEL, and copying a new `.uf2` over USB.
pico-bootLoader replaces that procedure: the applications are placed on the
board's SD card once, and from then on every power-on presents a menu. The menu
is navigated with a USB game controller or a USB keyboard, in either a graphical
mode with full-screen artwork per application or a plain text mode. Selecting an
entry flashes the corresponding application (if it is not already resident) and
starts it. A hardware reset or power cycle always returns to the menu.

*Doom* is included as a native RP2350 port — it is **not** emulated.

## Video

*Click the image below to watch pico-bootLoader in action.*

[![pico-bootLoader in action](https://img.youtube.com/vi/X-7ZBaCIoeQ/maxresdefault.jpg)](https://www.youtube.com/watch?v=X-7ZBaCIoeQ)


## Bootable applications

The following emulators and the *Doom* port are supported. Each is built from
its own repository and identified by the program name embedded in its `.uf2`.

| System | Program name | Source repository | |
|---|---|---| -- |
| Nintendo Entertainment System | `piconesPlus` | [pico-infonesPlus](https://github.com/fhoedemakers/pico-infonesPlus) | <img width="1920" height="1080" alt="Screenshot 2026-07-10 12-29-44" src="https://github.com/user-attachments/assets/bcac309a-2766-4547-b28a-a49d6497cf6f" /> |
| Sega Genesis / Mega Drive | `picogenesisPlus` | [pico-genesisPlus](https://github.com/fhoedemakers/pico-genesisPlus) | <img width="1920" height="1080" alt="Screenshot 2026-07-10 12-29-48" src="https://github.com/user-attachments/assets/2c863df5-fe10-4f25-b750-fc9109de8ca3" /> |
| NEC PC Engine / PCEngine CD | `picopcePlus` | [pico-pcePlus](https://github.com/fhoedemakers/pico-pcePlus) | <img width="1920" height="1080" alt="Screenshot 2026-07-10 12-29-41" src="https://github.com/user-attachments/assets/7cf5571a-f2d5-4479-98d5-92f21357d855" /> |
| Nintendo Game Boy / Game Boy Color | `PicoPeanutGB` | [pico-peanutGB](https://github.com/fhoedemakers/pico-peanutGB) |<img width="1920" height="1080" alt="Screenshot 2026-07-10 12-29-33" src="https://github.com/user-attachments/assets/7193f45d-381b-499f-9950-b1d170663a82" />  |
| Sega Master System / Game Gear | `picosmsPlus` | [pico-smsplus](https://github.com/fhoedemakers/pico-smsplus) |<img width="1920" height="1080" alt="Screenshot 2026-07-10 12-33-32" src="https://github.com/user-attachments/assets/864730f3-e33a-49a6-8e10-fd6a656ea901" /> |
| Philips Videopac / Magnavox Odyssey² | `picoPacPlus` | [pico-pacPlus](https://github.com/fhoedemakers/pico-pacPlus) | <img width="1920" height="1080" alt="Screenshot 2026-07-10 12-38-53" src="https://github.com/user-attachments/assets/413f44e3-9eca-4106-a5a8-0dccb024b286" /> |
| **Doom** (native port, not emulated) | `doom_tiny` | [pico-doom](https://github.com/fhoedemakers/pico-doom) | <img width="1920" height="1080" alt="Screenshot 2026-07-10 12-29-53" src="https://github.com/user-attachments/assets/2a101f0d-39d4-493e-bf73-4452737f723a" />  |

The following emulators need a bios in `/bios` on SD:
- *Nintendo Entertainment System* : For Famicom Dsik System games `fds-bios.rom`
- *Philips Videopac / Magnavox Odyssey²*: `o2rom.bin`
- *PCEngine CD* : `Super CD-ROM System (Japan) (v3.0).pce` or another variant.

*Doom* runs on four boards: Adafruit Fruit Jam (HW_CONFIG 8), the Adafruit DVI +
MicroSD breakout combination (2), Murmulator M2 (13) and Adafruit Feather RP2350
+ TLV320DAC3100 (14). It ships in two variants: `doom_tiny`, the shareware
episode, distributed as the engine `.uf2` together with a companion WAD data
image (see [Auxiliary data images](#auxiliary-data-images)); and
`doom_tiny_full`, registered/Ultimate DOOM, which carries no WAD in flash and
instead reads `/roms/doom/doom.whd` from the SD card at boot, so it needs a board
with PSRAM.

*PCEngine CD* needs PSRAM

Additional emulators may be added over time.

For board-by-board wiring, supported display modes and more refer to the [pico-infonesPlus documentation](https://github.com/fhoedemakers/pico-infonesPlus#setup). The set of supported boards and their pinouts is identical between the two projects.

## How it works

The bootloader resides at the start of flash, so the RP2350 bootrom always runs
it first. On each boot it:

1. Reads the optional [`/boot.txt`](boot.txt), the index file, and scans the
   board's application folder on the SD card (`<BASEDIR>/<HW_CONFIG>/*.uf2`).
2. Reads each `.uf2`'s embedded program name from its `binary_info` on disk —
   nothing is flashed to do this — and reads the resident application's name via
   XIP.
3. Filters the files against the index (an allow-list), then displays the menu
   and pre-selects the entry that matches the application currently in flash.
4. On selection:
   - if the chosen application is already resident, it is started with a VTOR
     jump — no flash operation and no SD I/O, so the launch is near-instant;
   - otherwise the application is flashed into the application partition, with a
     progress indicator, and then started;
   - if the copy on the SD card differs from the resident image (a CRC mismatch,
     for example after a newer build was placed on the card), it is re-flashed
     before starting.

The bootloader only ever *jumps* to an application; it never transfers the boot
vector. Consequently it cannot be locked out: any reset or power cycle returns
to the menu, and flashing a defective application costs nothing more than
another selection.

The flash memory map (16 MB flash, Adafruit Fruit Jam shown) is:

```
0x10000000  Bootloader             512 KB   <- the bootrom always runs this
0x10080000  Application partition  15.5 MB  <- the selected app is flashed and started here
0x11000000  end of flash
```

## Input devices

The menu is operated with USB Human Interface Devices. All connected input
devices are active simultaneously.

- **USB game controller** — standard USB HID gamepads and XInput controllers.
- **USB keyboard** — a standard USB HID keyboard.

On boards that provide the necessary wiring, NES/SNES controller ports and a Wii
Classic controller (over I²C) are also supported and use their own buttons.

| Action | Game controller | USB keyboard |
|---|---|---|
| Move through the list (text mode) | D-pad UP / DOWN | ↑ / ↓ |
| Slide between applications (graphical mode) | D-pad LEFT / RIGHT | ← / → |
| Change artwork theme (graphical mode) | D-pad UP / DOWN | ↑ / ↓ |
| Launch the selected application | A | Z |
| Toggle text / graphical mode | SELECT | A |
| Open the help screen | START | S |
| Wake from screensaver | any button | any mapped key |

START is used for the help screen because it is the only spare button present
on every supported input device, including NES controllers.

The chosen menu mode and artwork theme are remembered across boots. Inside a running emulator built
on the shared framework, **SELECT + START** opens its menu, which offers *Return
to emulator selection* to reboot back into this menu.

## Getting started

1. **Flash the bootloader.** Download the loader `.uf2` for your board from the
   [Releases](https://github.com/fhoedemakers/pico-bootLoader/releases) page
   (see [Supported hardware](#supported-hardware) for the file names). Hold
   BOOTSEL, connect the board over USB, and copy the `.uf2` onto the
   `RP2350` drive.
2. **Prepare the SD card.** Download `pico-bootLoader_sdcard.zip` from the same
   Releases page and unpack it onto a FAT32- or exFAT-formatted card. The
   archive contains the emulators, the *Doom* port, the menu artwork, and a
   sample configuration file. Alternatively, assemble the layout yourself as
   described in [SD card layout](#sd-card-layout).
3. **Run it.** Insert the card and power on the board. The menu appears.

The Releases page provides two kinds of download: the per-board bootloader
`.uf2` binaries and the `pico-bootLoader_sdcard.zip` SD-card archive.

Release tags say which of the two changed:

| Tag | What changed | What you need to do |
| --- | --- | --- |
| `v0.N` | bootloader firmware | re-flash the board **and** refresh the SD card |
| `v0.N.M` | emulator binaries only | replace `/emu` on the SD card; no re-flash needed |

Every release lists the exact emulator versions its archive ships, and the same
table is in `/emu/versions.txt` on the card.

## Supported hardware

Only RP2350 boards are supported; the partition scheme and the UF2 family checks
are RP2350-specific. The board is selected at compile time through the
`HW_CONFIG` value (defined in
[`pico_shared/BoardConfigs.cmake`](pico_shared/BoardConfigs.cmake)). The same
number names the SD-card folder the loader reads applications from
(`<BASEDIR>/<HW_CONFIG>/`).

| HW_CONFIG | Board | Bootloader binary |
|---|---|---|
| 1 | Pimoroni Pico DV Demo Base (Pico 2 / Pico 2 W) | `pico-bootLoader_PimoroniDVI_pico2_arm.uf2` / `..._pico2_w_arm.uf2` |
| 2 | Adafruit DVI breakout + SD breakout (or custom PCB) (Pico 2 / Pico 2 W) | `pico-bootLoader_AdafruitDVISD_pico2_arm.uf2` / `..._pico2_w_arm.uf2` |
| 5 | Adafruit Metro RP2350 | `pico-bootLoader_AdafruitMetroRP2350_arm.uf2` |
| 6 | Waveshare RP2350-Zero with custom PCB | `pico-bootLoader_WaveShareRP2350ZeroWithPCB_arm.uf2` |
| 7 | Waveshare RP2350-PiZero | `pico-bootLoader_WaveShareRP2350PiZero_arm_piousb.uf2` |
| 8 | Adafruit Fruit Jam | `pico-bootLoader_AdafruitFruitJam_arm_piousb.uf2` |
| 9 | Waveshare RP2350-USB-A | `pico-bootLoader_WaveShare2350USBA_arm_piousb.uf2` |
| 13 | Murmulator M2 | `pico-bootLoader_MurmulatorM2_arm.uf2` |
| 14 | Adafruit Feather RP2350 (TLV320DAC3100 audio) | `pico-bootLoader_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2` |

Video output is DVI/HDMI on all boards. Boards whose video connector is wired
to the RP2350 HSTX pins — HW_CONFIG 2, 5, 8, 13 and 14 — drive it through the
HSTX peripheral; the others use PicoDVI. A single SD card
serves both kinds — artwork is cached in both pixel formats (see
[Artwork](#artwork)).

## SD card layout

```
/boot.txt                                  configuration (created/updated by the menu)
/emu/                                      BASEDIR (default /emu, override in boot.txt)
/emu/<HW_CONFIG>/*.uf2                     applications for this board (e.g. /emu/8/)
/emu/emulators.txt                         the index / allow-list (name set by INDEX)
/emu/versions.txt                          which version each application was built from (informational)
/emu/assets/themes/0/<image_key>.png|.jpg  default artwork theme, converted on first use
/emu/assets/themes/1..9/                   optional extra themes (UP/DOWN to switch)
/emu/assets/screensaver/*.png|.jpg         screensaver images (optional, not themed)
```

Applications live in a subfolder named after the board's `HW_CONFIG` number, so
one card can carry builds for several boards side by side.

### Configuration (`boot.txt`)

A file in the **root** of the SD card. If it is absent, the defaults below
apply. A commented sample ships in the repository root: [`boot.txt`](boot.txt).

- One `KEY=VALUE` per line; whitespace around `=` and the value is trimmed.
- Lines starting with `#` or `;` are comments; blank lines are ignored.
- Keys are case-insensitive. `SCREENSAVER` values are case-insensitive;
  `BASEDIR`/`INDEX` values are filesystem paths and case-sensitive.
- A malformed file (unknown key, duplicate key, missing `=` or value) produces
  an on-screen error at boot — correct the file and reset.

| Key | Default | Meaning |
|---|---|---|
| `BASEDIR` | `/emu` | Absolute SD path (must start with `/`, max 63 characters) under which everything lives: application folders, the index, artwork, screensaver images. |
| `INDEX` | `emulators.txt` | Bare file name (no slashes) of the index file inside `BASEDIR`. |
| `SCREENSAVER` | see note | `STARFIELD` — images fly outward from the screen centre, growing toward the camera. `BLOCKS` — images float and bounce off the edges; the on-screen set is re-picked every 15 s. The screensaver starts after ~30 s of inactivity and any button press exits it. |
| `GUI` | `1` | `0` = text menu, `1` = graphical menu. Rewritten whenever SELECT toggles the mode. Replaces the `.guimode` file used by earlier releases, which is migrated and deleted automatically. |
| `THEME` | `0` | Active artwork theme, `0`–`9` — see [Artwork themes](#artwork-themes). Rewritten whenever UP/DOWN changes the theme in graphical mode. A theme that is not on the card falls back to `0`. |

> **Screensaver default.** When `/boot.txt` is *absent* the default is
> `STARFIELD`; when the file is *present* but the key is omitted, it is
> `BLOCKS`. Set the key explicitly if the choice matters.

**The bootloader writes this file.** Changing the menu mode or the artwork
theme rewrites the corresponding `GUI=` / `THEME=` line. Nothing else is
touched: comments, blank lines, key order, spacing and any other keys are
copied through unchanged, so the file stays yours to edit. If `/boot.txt` does
not exist, the first such change creates it with the current effective value of
*every* key — `SCREENSAVER` included, so that materialising the file cannot
quietly change the screensaver through the asymmetry noted above.

Updates are written to `/boot.txt.tmp`, re-parsed to confirm they are valid,
and only then renamed into place; if a power cut interrupts the rename, the
next boot adopts the `.tmp`. A card that cannot be written to (write-protected
or full) is not an error — the change applies for the session and the help
screen reports that it was not saved.

### The index file (allow-list)

`<BASEDIR>/<INDEX>` — by default `/emu/emulators.txt` — determines what appears
in the menu. Only `.uf2` files whose embedded program name matches a row are
listed; anything else in the application folder is ignored. One row per
application:

```
<program_name>;<image_key>;<display_name>[;<aux_uf2>]
```

```
# program_name  ; image_key ; display_name              ; optional aux data uf2
piconesPlus     ; nes       ; Nintendo Entertainment System
picogenesisPlus ; md        ; Sega Genesis/Mega Drive
doom_tiny       ; doom      ; Doom!                     ; doom1-whx.uf2
```

- Fields are separated by `;`, whitespace is trimmed, and `#` starts a comment
  line. A maximum of 16 rows is allowed.
- **`program_name`** (max 32 characters) — matched case-insensitively against
  the name each `.uf2` embeds via `pico_set_program_name()` (read from its
  `binary_info`, without flashing anything). The `.uf2` *file name* is
  irrelevant; files may be renamed freely.
- **`image_key`** (max 16 characters) — basename of the menu artwork:
  `<BASEDIR>/assets/<image_key>.png` (or `.jpg`/`.jpeg`).
- **`display_name`** (max 40 characters) — the label shown in the menu.
- **`aux_uf2`** (optional, max 64 characters) — file name of a companion data
  `.uf2` in the same `<BASEDIR>/<HW_CONFIG>/` folder, flashed alongside the
  application (see [Auxiliary data images](#auxiliary-data-images)).

### Artwork

Ordinary images are placed on the card and converted by the bootloader itself:

- **Menu artwork** — `<BASEDIR>/assets/themes/<N>/<image_key>.png|.jpg|.jpeg`,
  one per index row, shown full-screen in graphical mode. `<N>` is the theme
  number; theme `0` is the default — see [Artwork themes](#artwork-themes).
- **Screensaver images** — any `*.png|.jpg|.jpeg` in
  `<BASEDIR>/assets/screensaver/` (file names do not matter; more images give
  more variety). These are **not** themed.

On first use each source image is converted and cached next to it as
`<name>.444` (RGB444, used by PicoDVI boards) and `<name>.555` (RGB555, used by
HSTX boards). Both are always written, so the same card works in every supported
board. The `.444`/`.555` files must not be authored by hand; when a source image
is replaced under the same name, its stale `.444`/`.555` files should be deleted
so they regenerate.

| | Menu artwork | Screensaver |
|---|---|---|
| Accepted formats | PNG, baseline JPEG | PNG, baseline JPEG |
| Maximum source size | 1280 × 960 | 1280 × 960 |
| Rendered as | scaled down (never up) to fit, letterboxed on black to 320 × 240 | scaled down to fit 80 × 60 |
| Recommendation | 4:3 aspect (e.g. 320 × 240 or 640 × 480) to avoid black bars | small and legible — it moves around the screen |

Progressive JPEG, interlaced PNG, 16-bit PNG, and oversized images are not
supported; they are skipped and moved to an `unsupported/` subfolder so they are
not retried on every boot. Re-export as baseline/non-interlaced 8-bit and copy
again.

Boards **with PSRAM** convert images lazily, as they first appear on screen.
Boards **without PSRAM** convert everything in one batch during boot, **for
every theme on the card, not just the active one** — the converter needs SRAM
that is no longer free once the menu is running, so a theme cannot be converted
at the moment you switch to it. The first boot after adding images takes
noticeably longer, after which the cache makes it immediate.

### Artwork themes

The graphical menu can carry up to ten sets of artwork. Each is a folder:

```
/emu/assets/themes/0/     theme 0 — the default, and the fallback
/emu/assets/themes/1/     theme 1
...                       up to theme 9
```

A theme folder holds one image per application, named after the `image_key`
from the index file — so a theme might contain `nes.png`, `md.png`, `doom.png`.
Only the folders that exist are used; the numbers need not be contiguous.

**Switching** — press UP or DOWN in the graphical menu. Only themes that are
actually on the card are reachable, so with themes `0`, `1` and `3` present,
UP/DOWN cycles `0 → 1 → 3 → 0`. The choice is saved to `THEME=` in
[`/boot.txt`](#configuration-boottxt) immediately and restored on the next boot.
UP/DOWN keep their usual meaning (choosing an application) in text mode, where
artwork is not shown.

**Incomplete themes are fine.** An application the active theme has no image for
falls back to theme 0's image, and to a black screen if theme 0 has none either.
A theme can therefore restyle just a few entries.

**Existing cards are migrated automatically.** Releases before v0.2 kept menu
artwork loose in `<BASEDIR>/assets`. On the first boot the bootloader creates
`assets/themes/0` and moves those image files into it — including the cached
`.444`/`.555` files, so nothing has to be re-converted. The `screensaver/`
folder and any other subfolder are left alone, as are files that are not
images. The move is resumable: if it is interrupted, the next boot finishes it.

### On-screen help

Press **START** in either menu mode for a full-screen summary of the controls,
the meaning of the `*` and `!` markers, and the current mode, theme, board
configuration and index file. Press START, the launch button, B or SELECT to
return. It is also where a failed configuration write is reported.

## Creating a bootable build of your own application

Any RP2350 application built for the application partition can appear in the
menu — it need not be an emulator. Making one bootable requires compiling its
`.uf2` to the loader's layout and adding it to the SD card.

### 1. Compile the `.uf2` to bootloader format

A normal Pico SDK application links at `0x10000000`. For the bootloader it must
be relinked into the application partition at `0x10080000`. Three changes to the
build are required.

**Give the application a name** — the loader identifies applications by it:

```cmake
pico_set_program_name(${projectname} "my_app")
```

**Relink into the application partition.** Add the following near the end of the
`CMakeLists.txt` (after the target exists, before `pico_add_extra_outputs`),
with `BootPartition.cmake` taken from the
[`pico_shared`](https://github.com/fhoedemakers/pico_shared) repository:

```cmake
if(BUILD_FOR_BOOTLOADER)
    include("pico_shared/BootPartition.cmake")
    frens_offset_for_bootloader(${projectname})
endif()
```

**Build as a secure-Arm RP2350 image** (the default SDK Arm build). The loader
only flashes UF2 blocks with family `RP2350 ARM_S` (`0xe48bff59`):

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 \
      -DPICO_PLATFORM=rp2350-arm-s -DBUILD_FOR_BOOTLOADER=ON ..
make -j
```

Applications based on `pico_shared` can use its build script instead:
`./bld.sh -2 -c <HW_CONFIG> -b` (the `-b` switch passes
`-DBUILD_FOR_BOOTLOADER=ON`).

The loader validates every image before erasing anything: UF2 magic, family ID,
page alignment, and that every block lands inside the application partition. A
standalone build still linked at `0x10000000` is rejected on-screen and can
never overwrite the bootloader.

### 2. Install it on the SD card

Copy the `.uf2` to `<BASEDIR>/<HW_CONFIG>/` and add a row to the index file
(`emulators.txt`) with the program name set in step 1, an `image_key`, and a
display name. See [The index file](#the-index-file-allow-list).

### 3. Add images for the menu and screensaver

- **Menu image** — place `<image_key>.png` (or `.jpg`/`.jpeg`) in
  `<BASEDIR>/assets/themes/0/`, using the `image_key` from the index row. It is
  shown full-screen in graphical mode. Add the same file name to any other
  `themes/<N>/` folder to give the application a different look in that theme;
  themes you skip fall back to this one.
- **Screensaver images** — place any `*.png|.jpg|.jpeg` in
  `<BASEDIR>/assets/screensaver/`.

Conversion is automatic; the format and size constraints are those listed under
[Artwork](#artwork).

### Auxiliary data images

For payloads too large to embed in the application (game data, filesystems), a
second `.uf2` with family `RP2350 DATA` (`0xe48bff58`) can target free flash
above the application. Name it in the index row's fourth field and the loader
flashes it alongside the application, skipping the write when the CRC already
matches. *Doom* is distributed this way: the engine (`doom_tiny.uf2`, ARM_S)
plus the WAD (`doom1-whx.uf2`, DATA).

### Detecting the bootloader and returning to the menu

Optionally, an application can detect that it was started by the loader and offer
a "return to menu" action. The protocol uses two watchdog scratch registers,
which survive `watchdog_reboot()` but are cleared on power cycle:

- `scratch[6] == 0xB007ED01` — set by the loader immediately before starting the
  application ("you were launched from the bootloader").
- `scratch[7] = 0xB007BACE` — set by the application, followed by a watchdog
  reboot ("show the menu instead of resuming").

With `pico_shared` these are `Frens::isLaunchedFromBootloader()` and
`Frens::rebootToBootloader()`
([`pico_shared/FrensHelpers.h`](pico_shared/FrensHelpers.h)); its SELECT + START
menu shows *Return to emulator selection* automatically. Without `pico_shared`,
the raw equivalent is:

```c
#include "hardware/watchdog.h"

bool launched_by_loader = (watchdog_hw->scratch[6] == 0xB007ED01u);

void return_to_menu(void) {
    watchdog_hw->scratch[7] = 0xB007BACEu;
    watchdog_reboot(0, 0, 0);
}
```

## Building the bootloader from source

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DHW_CONFIG=8 \
      -DPICO_PLATFORM=rp2350-arm-s -DENABLE_PIO_USB=1 -DUSE_PICO_EXTRAS_I2S=0 ..
make -j
# -> build/pico-bootLoader.uf2  (flash via BOOTSEL)
```

The wrapper `./bld.sh -2 -c <HW_CONFIG>` performs the same build (add `-w` for
Pico 2 W). `./buildAll.sh` builds every supported board into `releases/`
(requires `picotool`).

The image must fit the 512 KB bootloader region; the linker errors out if it
does not, and every link prints its occupancy. Most configurations sit near
260 KB (~51%), but the Pico 2 W builds pull in the CYW43 driver and land around
483 KB (~92%) — that is the configuration to check when adding code.

To build the emulators and the *Doom* port themselves,
[`build_emulators.sh`](build_emulators.sh) clones each source repository and
produces the bootloader-format `.uf2` files, placing them under `emu/<HW_CONFIG>/`.

By default each repository is built from its **latest release tag**, with the tag
stamped into that repository's own `pico_shared/menu.h` `SWVERSION` so the
emulator reports its version instead of a build date. `-B` asks interactively for
a branch instead, and `-m` builds each repository's default branch; neither
stamps a version.

> **`pico_shared` is not left at the revision the tag pins.** `bld.sh` only
> learned `-b` (`BUILD_FOR_BOOTLOADER`) in `pico_shared` `f2c8be9`, and the
> emulator release tags predate it — their pinned `pico_shared` rejects `-b`
> outright, so no bootloader-format `.uf2` can be produced from it. Tag mode
> therefore builds each emulator's tagged source against `pico_shared` `main`.
> Every other submodule stays at the revision the tag pins, and
> `emu/versions.txt` records both refs. Once the emulator repositories pin a
> `-b`-capable `pico_shared` and are re-tagged, this substitution becomes a
> no-op.

*Doom* is the exception to all of the above: [pico-doom](https://github.com/fhoedemakers/pico-doom)
has no `pico_shared`, so there is no `SWVERSION` to stamp, and it builds through
its own per-board scripts rather than `bld.sh`. It has no release tag yet, so tag
mode falls back to its `full-version` branch — the branch that carries the build
scripts for both variants, where `main` still has only the shareware half — and
records `full-version@<sha>` as the version. When pico-doom is tagged (it uses
the same `v*.*` convention), tag mode picks the tag up with no change here.

*Doom* also needs `PICO_EXTRAS_PATH` pointing at a
[pico-extras](https://github.com/raspberrypi/pico-extras) checkout, since it
resolves its whole toolchain from the environment. Without it the two Doom
variants are skipped with that reason and everything else still builds.

```bash
./build_emulators.sh -c 8            # one board, latest tags
./build_emulators.sh -c all -j 3     # every board
./build_emulators.sh -c all -j 3 -z  # ... and pack the SD-card archive
./build_emulators.sh -c 8 -B         # pick a branch interactively
```

`-z` writes [`emu/versions.txt`](emu/versions.txt) — the manifest of what each
emulator was built from, one `<program_name>;<repo>;<ref>;<pico_shared>` row per
shipped emulator — and packs
`releases/pico-bootLoader_sdcard.zip` via
[`.github/scripts/pack_sdcard.sh`](.github/scripts/pack_sdcard.sh). The packer
takes the `emu/` tree as its only source of truth and refuses to build an archive
containing an empty `.uf2`, so a half-finished build cannot ship. It requires
`-c all`, since an archive built from one board would be missing the others.

### Cutting a release

The loader `.uf2`s are built by CI; the SD-card archive is built locally, because
it needs all nine emulator toolchains. An emulator-only refresh still gets its own
release so users find out about it — that is what the `v0.N.M` form is for.

The full maintainer checklist — per-scenario steps, dry runs, verification and
rollback — is in [`RELEASING.md`](RELEASING.md). The short version:

```bash
./build_emulators.sh -c all -j 3 -z            # emulators + archive (local)
git add emu/versions.txt && git commit -m "Refresh emulator bundle" && git push
gh workflow run BuildAndRelease.yml -f tag=v0.2.1   # builds loader, creates tag, publishes
gh release upload v0.2.1 releases/pico-bootLoader_sdcard.zip
```

## Credits

- Menu and screensaver artwork is taken from **Ducalex — retro-go**
  ([github.com/ducalex/retro-go](https://github.com/ducalex/retro-go)).
- The emulator cores and the *Doom* port are the work of their upstream authors;
  see the repository links under [Bootable
  applications](#bootable-applications).
- This project was developed with the assistance of AI
  (Anthropic Claude / Claude Code).

## License

This project is licensed under the GNU General Public License, version 3. See
the [`LICENSE`](LICENSE) file for the full text.
