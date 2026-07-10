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
| **Doom** (native port, not emulated) | `doom_tiny` | [fruitjam-doom](https://github.com/fhoedemakers/fruitjam-doom) | <img width="1920" height="1080" alt="Screenshot 2026-07-10 12-29-53" src="https://github.com/user-attachments/assets/2a101f0d-39d4-493e-bf73-4452737f723a" />  |

The following emulators need a bios in `/bios` on SD:
- *Nintendo Entertainment System* : For Famicom Dsik System games `fds-bios.rom`
- *Philips Videopac / Magnavox Odyssey²*: `o2rom.bin`
- *PCEngine CD* : `Super CD-ROM System (Japan) (v3.0).pce` or another variant.

*Doom* currently runs on the Adafruit Fruit Jam (HW_CONFIG 8) only; support for
further boards is expected to follow. It is distributed as the engine `.uf2`
together with a companion WAD data image (see [Auxiliary data
images](#auxiliary-data-images)).

*PCEngine CD* needs PSRAM

Additional emulators may be added over time.



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
| Launch the selected application | A | Z |
| Toggle text / graphical mode | SELECT | A |
| Wake from screensaver | any button | any mapped key |

The chosen menu mode is remembered across boots. Inside a running emulator built
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
/boot.txt                            optional configuration (defaults apply if absent)
/emu/                                BASEDIR (default /emu, override in boot.txt)
/emu/<HW_CONFIG>/*.uf2               applications for this board (e.g. /emu/8/)
/emu/emulators.txt                   the index / allow-list (name set by INDEX)
/emu/assets/<image_key>.png|.jpg     menu artwork, converted on first use
/emu/assets/screensaver/*.png|.jpg   screensaver images (optional)
/emu/.guimode                        persisted menu mode (created automatically)
```

Applications live in a subfolder named after the board's `HW_CONFIG` number, so
one card can carry builds for several boards side by side.

### Configuration (`boot.txt`)

An optional file in the **root** of the SD card. If it is absent, the defaults
below apply. A commented sample ships in the repository root:
[`boot.txt`](boot.txt).

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

> **Screensaver default.** When `/boot.txt` is *absent* the default is
> `STARFIELD`; when the file is *present* but the key is omitted, it is
> `BLOCKS`. Set the key explicitly if the choice matters.

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
doom_tiny       ; doom      ; Doom!                     ; doom1-whx-for-fruitjam.uf2
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

- **Menu artwork** — `<BASEDIR>/assets/<image_key>.png|.jpg|.jpeg`, one per index
  row, shown full-screen in graphical mode.
- **Screensaver images** — any `*.png|.jpg|.jpeg` in
  `<BASEDIR>/assets/screensaver/` (file names do not matter; more images give
  more variety).

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
Boards **without PSRAM** convert everything in one batch during boot — the first
boot after adding images takes noticeably longer, after which the cache makes it
immediate.

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

- **Menu image** — place `assets/<image_key>.png` (or `.jpg`/`.jpeg`) in
  `<BASEDIR>/assets/`, using the `image_key` from the index row. It is shown
  full-screen in graphical mode.
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
plus the WAD (`doom1-whx-for-fruitjam.uf2`, DATA).

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
(requires `picotool`). The resulting binary is a few hundred KB, well inside the
512 KB bootloader region.

To build the emulators and the *Doom* port themselves,
[`build_emulators.sh`](build_emulators.sh) clones each source repository and
produces the bootloader-format `.uf2` files, placing them under `emu/<HW_CONFIG>/`.

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
