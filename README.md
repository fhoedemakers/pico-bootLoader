# pico-bootLoader

**Turns an RP2350 board into a self-contained multi-application device: switch
between the programs on its SD card from an on-screen menu, without ever
connecting it to a PC again.**

An RP2350 board normally runs a single program — to run a different one you
have to plug it into a computer, hold BOOTSEL, and copy a new `.uf2` over.
pico-bootLoader removes that loop. Put all your applications on the board's SD
card once; from then on, every power-on greets you with a menu. Browse it with
a game controller or USB keyboard — in a **graphical mode** with full-screen
artwork per application, or a plain **text mode** — press a button, and the
selected application is flashed automatically (with a live progress bar) and
started. If it's the one already in flash, it starts instantly. No PC, no
BOOTSEL button, no cables.

<!-- TODO: add a photo/screenshot of the graphical picker here -->

It was built so the RP2350 retro-emulator family (pico-infonesPlus,
pico-genesisPlus, pico-pcePlus, pico-smsplus, pico-peanutGB, …) can live
together on one console-like device — power on, pick a system, play. But
nothing about the loader is emulator-specific: any application built for the
application partition can be on the menu (Doom is), and making your own app
bootable is a three-line CMake change plus one line in a config file (see
[Building your own app](#building-your-own-app-for-the-bootloader)).

## Features

- Two menu modes, toggled with **SELECT** and remembered across boots
  (persisted in `<BASEDIR>/.guimode`).
- Sub-second launch of the resident app: a VTOR jump, no flash op, no SD I/O.
- Auto-reflash on drift: if the `.uf2` on SD differs from the resident copy
  (CRC mismatch — e.g. you dropped a newer build on the card), launching it
  reflashes first.
- Optional auxiliary data `.uf2` per app for large read-only payloads (Doom's
  WAD, for example) flashed alongside the application.
- Idle screensaver after 30 seconds — two styles, rendered from your own
  images; any button wakes it.
- Input devices, all active simultaneously: USB game controllers (HID and
  XInput), USB keyboard, NES/SNES controller ports, Wii Classic controller.
- Configurable through an optional [`/boot.txt`](boot.txt) on the SD card.
- The bootloader only ever *jumps* to applications — it never hands over the
  boot vector — so any reset or power-cycle always returns to the menu.

## Supported hardware

RP2350 only: the partition scheme and UF2 family checks are RP2350-specific.
Board selection is the compile-time `HW_CONFIG` value (from
[`pico_shared/BoardConfigs.cmake`](pico_shared/BoardConfigs.cmake)); it also
names the SD card folder the loader reads apps from (`<BASEDIR>/<HW_CONFIG>/`).

| HW_CONFIG | Board | Prebuilt loader binary |
|---|---|---|
| 1 | Pimoroni Pico DV Demo Base (Pico 2 / Pico 2 W) | `pico-bootLoader_PimoroniDVI_pico2_arm.uf2` / `..._pico2_w_arm.uf2` |
| 2 | Adafruit DVI breakout + SD breakout (or PCB) (Pico 2 / Pico 2 W) | `pico-bootLoader_AdafruitDVISD_pico2_arm.uf2` / `..._pico2_w_arm.uf2` |
| 5 | Adafruit Metro RP2350 | `pico-bootLoader_AdafruitMetroRP2350_arm.uf2` |
| 6 | Waveshare RP2350-Zero with PCB | `pico-bootLoader_WaveShareRP2350ZeroWithPCB_arm.uf2` |
| 7 | Waveshare RP2350-PiZero | `pico-bootLoader_WaveShareRP2350PiZero_arm_piousb.uf2` |
| 8 | Adafruit Fruit Jam (HSTX) | `pico-bootLoader_AdafruitFruitJam_arm_piousb.uf2` |
| 9 | Waveshare RP2350-USB-A | `pico-bootLoader_WaveShare2350USBA_arm_piousb.uf2` |
| 13 | Murmulator M2 | `pico-bootLoader_MurmulatorM2_arm.uf2` |
| 14 | Adafruit Feather RP2350 (TLV320DAC3100 audio) | `pico-bootLoader_AdafruitFeatherRP2350_TLV320DAC3100_arm_piousb.uf2` |

Video is DVI/HDMI on all boards: the Fruit Jam (config 8) drives it through
the RP2350 HSTX peripheral, the others through PicoDVI. One SD card serves
both kinds — artwork is cached in both pixel formats (see
[Artwork](#artwork-menu-images-and-screensaver)).

## Quick start

1. Flash the loader for your board (from
   [Releases](https://github.com/fhoedemakers/pico-bootLoader/releases), or
   build it yourself — see [Building the bootloader](#building-the-bootloader)):
   hold BOOTSEL, plug in USB, copy the `.uf2` over.
2. Prepare a FAT32/exFAT SD card: download the ready-made `sdcard.zip` from the
   content repository (see [Prebuilt app collection](#prebuilt-app-collection))
   and unpack it onto the card, or assemble your own layout as described below.
3. Insert the card, power on. The picker appears.

Controls:

| Action | Gamepad | USB keyboard |
|---|---|---|
| Move through the list (text mode) | D-pad UP / DOWN | ↑ / ↓ |
| Slide between apps (graphical mode) | D-pad LEFT / RIGHT | ← / → |
| Launch the selected app | A | Z |
| Toggle text ↔ graphical | SELECT | A |
| Wake from screensaver | any button | any mapped key |

NES/SNES and Wii Classic controllers use their own SELECT/A buttons; the
on-screen footer hints adapt to the connected pad. Inside a `pico_shared`
based app, **SELECT+START** opens its menu, which offers *Return to emulator
selection* to reboot back into the picker.

## SD card layout

```
/boot.txt                            optional configuration (defaults apply if absent)
/emu/                                BASEDIR (default /emu, override in boot.txt)
/emu/<HW_CONFIG>/*.uf2               applications for this board (e.g. /emu/8/)
/emu/emulators.txt                   the index / allow-list (filename set by INDEX)
/emu/assets/<image_key>.png|.jpg     menu artwork, auto-converted on first use
/emu/assets/screensaver/*.png|.jpg   screensaver images (optional)
/emu/.guimode                        persisted menu mode (auto-created)
```

Apps live in a subfolder named after the board's `HW_CONFIG` number, so one
card can carry builds for several boards side by side.

## Configuring with `boot.txt`

An optional file in the **root** of the SD card. If absent, the defaults
below apply. A commented sample ships in the repo root: [`boot.txt`](boot.txt).

Syntax:

- One `KEY=VALUE` per line; whitespace around `=` and the value is trimmed.
- Lines starting with `#` or `;` are comments; blank lines are ignored.
- Keys are case-insensitive. `SCREENSAVER` values are case-insensitive;
  `BASEDIR`/`INDEX` values are filesystem paths and case-sensitive.
- A malformed file (unknown key, duplicate key, missing `=` or value) shows an
  on-screen error at boot — fix the file and reset.

| Key | Default | Meaning |
|---|---|---|
| `BASEDIR` | `/emu` | Absolute SD path (must start with `/`, max 63 chars) under which everything lives: app folders, the index, artwork, screensaver images. |
| `INDEX` | `emulators.txt` | Bare filename (no slashes) of the index file inside `BASEDIR`. |
| `SCREENSAVER` | see note | `STARFIELD` — images fly outward from the screen centre, growing toward the camera. `BLOCKS` — images float and bounce off the edges; the on-screen set is re-picked every 15 s. |

> **Screensaver default:** when `/boot.txt` is *absent* the default is
> `STARFIELD`; when the file is *present* but the key is omitted, it is
> `BLOCKS`. Set the key explicitly if you care which one you get.

## The index file (allow-list)

`<BASEDIR>/<INDEX>` — by default `/emu/emulators.txt` — decides **what
appears in the menu**. Only `.uf2` files whose embedded program name matches a
row are listed; anything else in the app folder is ignored. One row per app:

```
<program_name>;<image_key>;<display_name>[;<aux_uf2>]
```

```
# program_name  ; image_key ; display_name              ; optional aux data uf2
piconesPlus     ; nes       ; Nintendo Entertainment System
picogenesisPlus ; md        ; Sega Genesis/Mega Drive
doom_tiny_usb   ; doom      ; Doom!                     ; doom1-whx-for-fruitjam.uf2
```

- Fields are separated by `;`, whitespace is trimmed, `#` starts a comment
  line. Maximum 16 rows.
- **`program_name`** (max 32 chars) — matched case-insensitively against the
  name each `.uf2` embeds via `pico_set_program_name()` (read from its
  `binary_info`, without flashing anything). The `.uf2` *filename* is
  irrelevant — rename files freely.
- **`image_key`** (max 16 chars) — basename of the menu artwork:
  `<BASEDIR>/assets/<image_key>.png` (or `.jpg`/`.jpeg`).
- **`display_name`** (max 40 chars) — the label shown in the picker.
- **`aux_uf2`** (optional, max 64 chars) — filename of a companion **data**
  `.uf2` in the same `<BASEDIR>/<HW_CONFIG>/` folder, flashed alongside the
  app. Used for large read-only payloads such as Doom's WAD (see
  [Auxiliary data images](#auxiliary-data-images)).

## Artwork (menu images and screensaver)

Drop ordinary images on the card — the bootloader converts them itself:

- **Menu artwork:** `<BASEDIR>/assets/<image_key>.png|.jpg|.jpeg`, one per
  index row, shown full-screen in graphical mode.
- **Screensaver images:** any `*.png|.jpg|.jpeg` in
  `<BASEDIR>/assets/screensaver/` (names don't matter; more images = more
  variety).

On first use each source image is converted and cached next to it as
`<name>.444` (RGB444, used by PicoDVI boards) **and** `<name>.555` (RGB555,
used by HSTX boards) — both are always written, so the same card works in
every supported board. Never author the `.444`/`.555` files by hand, and if
you replace a source image under the same name, delete its stale `.444`/`.555`
files so they regenerate.

Constraints:

| | Menu artwork | Screensaver |
|---|---|---|
| Accepted formats | PNG, baseline JPEG | PNG, baseline JPEG |
| Max source size | 1280 × 960 | 1280 × 960 |
| Rendered as | scaled down (never up) to fit, letterboxed on black to exactly 320 × 240 | scaled down to fit 80 × 60 |
| Recommendation | 4:3 aspect (e.g. 320 × 240 or 640 × 480) to avoid black bars | small and legible — it moves around the screen |

Not supported (skipped and moved to an `unsupported/` subfolder so they are
not retried every boot): **progressive** JPEG, **interlaced** PNG, **16-bit**
PNG, oversized images. Re-export as baseline/non-interlaced 8-bit and copy
again.

Boards **with PSRAM** convert images lazily, as they first appear on screen.
Boards **without PSRAM** convert everything in one batch during boot — the
first boot after adding images takes noticeably longer; after that the cache
makes it instant.

## Building your own app for the bootloader

The flash map (defined in
[`pico_shared/BootPartition.cmake`](pico_shared/BootPartition.cmake), mirrored
in [`src/boot_config.h`](src/boot_config.h)):

```
0x10000000  Bootloader          1 MB    <- bootrom always runs this
0x10100000  Application         15 MB   <- your app is flashed and started here
0x11000000  end (16 MB flash)
```

A normal Pico SDK app links at `0x10000000`; for the bootloader it must be
relinked to `0x10100000`. Three steps:

1. **Give it a name** — the loader identifies apps by it:

   ```cmake
   pico_set_program_name(${projectname} "my_app")
   ```

2. **Relink into the app partition.** Add near the end of your
   `CMakeLists.txt` (after the target exists, before
   `pico_add_extra_outputs`), with `BootPartition.cmake` taken from the
   [`pico_shared`](https://github.com/fhoedemakers/pico_shared) repo:

   ```cmake
   if(BUILD_FOR_BOOTLOADER)
       include("pico_shared/BootPartition.cmake")
       frens_offset_for_bootloader(${projectname})
   endif()
   ```

3. **Build as a secure-Arm RP2350 image** (the default SDK Arm build — the
   loader only flashes UF2 blocks with family `RP2350 ARM_S`, `0xe48bff59`):

   ```sh
   mkdir build && cd build
   cmake -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 \
         -DPICO_PLATFORM=rp2350-arm-s -DBUILD_FOR_BOOTLOADER=ON ..
   make -j
   ```

   Apps based on `pico_shared` can simply use its build script:
   `./bld.sh -2 -c <HW_CONFIG> -b` (the `-b` switch passes
   `-DBUILD_FOR_BOOTLOADER=ON`).

Then copy the `.uf2` to `<BASEDIR>/<HW_CONFIG>/` on the card, add an index row
with its program name, and drop an `assets/<image_key>.png` for the graphical
menu.

The loader validates before it erases anything: UF2 magic, family ID, page
alignment, and that every block lands inside the application partition. A
standalone build still linked at `0x10000000` is rejected on-screen — it can
never overwrite the bootloader.

### Detecting the bootloader and returning to the menu

Optional, but friendly: an app can detect it was started by the loader and
offer a "back to menu" action. The protocol is two watchdog scratch registers,
which survive `watchdog_reboot()` but clear on power-cycle:

- `scratch[6] == 0xB007ED01` — set by the loader just before starting the app
  ("you were launched from the bootloader").
- `scratch[7] = 0xB007BACE` — set by the app, followed by a watchdog reboot
  ("show me the picker instead of resuming").

With `pico_shared` this is `Frens::isLaunchedFromBootloader()` and
`Frens::rebootToBootloader()` ([`pico_shared/FrensHelpers.h`](pico_shared/FrensHelpers.h));
its SELECT+START menu shows *Return to emulator selection* automatically.
Without `pico_shared`, the raw equivalent is:

```c
#include "hardware/watchdog.h"

bool launched_by_loader = (watchdog_hw->scratch[6] == 0xB007ED01u);

void return_to_menu(void) {
    watchdog_hw->scratch[7] = 0xB007BACEu;
    watchdog_reboot(0, 0, 0);
}
```

### Auxiliary data images

For payloads too big to embed (game data, filesystems), a second `.uf2` with
family `RP2350 DATA` (`0xe48bff58`) can target free flash above the app. Name
it in the index row's 4th field and the loader flashes it alongside the app,
skipping the write when the CRC already matches. Doom ships this way: the
engine (`doom_tiny_usb.uf2`, ARM_S) plus the WAD
(`doom1-whx-for-fruitjam.uf2`, DATA).

## Building the bootloader

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DHW_CONFIG=8 \
      -DPICO_PLATFORM=rp2350-arm-s -DENABLE_PIO_USB=1 -DUSE_PICO_EXTRAS_I2S=0 ..
make -j
# -> build/pico-bootLoader.uf2  (flash via BOOTSEL)
```

Or use the wrapper: `./bld.sh -2 -c <HW_CONFIG>` (add `-w` for Pico 2 W).
`./buildAll.sh` builds every supported board into `releases/` (requires
`picotool`). The result is a few hundred KB — well inside the 1 MB
bootloader region.

## How it works

On every boot the loader:

1. **Resume check** — on PSRAM-less boards an emulator watchdog-reboots to
   flash a selected ROM; if that flag is set and a valid app is resident, the
   loader jumps straight back without showing the menu (unless the app
   explicitly requested the picker via the scratch-register protocol above).
2. Reads `/boot.txt`, the index file, and scans `<BASEDIR>/<HW_CONFIG>/`.
3. Reads each `.uf2`'s program name from its `binary_info` on disk (~100 ms
   per file, nothing is flashed) and the resident app's name via XIP.
4. Filters against the index, then **highlights and pre-selects** the entry
   matching the resident app, so a single press of A relaunches it instantly
   via a VTOR jump.
5. If the SD copy's CRC differs from the resident image (a newer build was
   dropped on the card), launching that entry reflashes it first — with a
   live progress bar — then starts it.

Because the handoff is a jump and the boot vector is never given away, the
bootloader can never be locked out: reset or power-cycle always brings the
menu back, and flashing a broken app costs nothing but another selection.

On boards without PSRAM the graphical menu keeps the slide animation by
allocating the incoming image at 160 × 120 and upscaling 2× during the slide,
then reloading it at full 320 × 240 once it lands — a brief blocky-to-sharp
snap unique to no-PSRAM hardware.

## Prebuilt app collection

Ready-built emulators (NES, Genesis/Mega Drive, PC Engine, Game Boy, Master
System/Game Gear, Videopac/Odyssey², …), Doom, and all menu artwork are
distributed as a ready-to-copy `sdcard.zip` from the companion content
repository:
<!-- TODO: update the link once the content repository exists -->
**[pico-bootLoader-sdcard](https://github.com/fhoedemakers/pico-bootLoader-sdcard)**.
Its CI builds each emulator from its own repository (on the `bootloader`
branch) and packs the SD card image, so binaries stay out of git history.
Until that repo is live, prebuilt sets for several boards are still in this
repo under [`emu/`](emu/).
