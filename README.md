# pico_emuLoader

A resident `.uf2` **bootloader / front-end** for the RP2350 retro-emulator family
(pico-infonesPlus, pico-pcePlus, pico-genesisPlus, pico-smsplus, pico-peanutGB, …),
built on the same shared framework (`pico_shared`, `pico_lib`, `tusb_xinput`).

On every power-on the RP2350 bootrom runs the bootloader (it owns the start of
flash). It shows an on-screen menu and, on the **B button**, either:

- **launches a pinned emulator instantly** via a VTOR jump (no flash op), or
- **flashes a chosen SD UF2** into the right partition and then launches it.

Because the bootloader only *jumps* (never hands over the boot vector), any
reset / power-cycle returns to the menu.

## Two modes, selected at boot time per board

The bootloader detects PSRAM and flash size at startup and picks one of two modes:

| Detected            | Mode                | Behaviour                                                              |
|---------------------|---------------------|------------------------------------------------------------------------|
| PSRAM + ≥ 16 MB     | **slot mode**       | 7 × 2 MB pinned slots + Setup option; instant launch from pinned slots |
| anything else       | **legacy mode**     | Today's behaviour: list SD, flash-on-launch every time                 |

One bootloader binary per HW_CONFIG (still needed for display/SD pins) — it
self-adapts at boot. Both modes share the same `main.cpp`.

## Flash memory map

Single source of truth in [`pico_shared/BootPartition.cmake`](pico_shared/BootPartition.cmake)
+ [`src/boot_config.h`](src/boot_config.h):

```
0x10000000  bootloader              1 MB
0x10100000  slot 0                  2 MB   <- == legacy single-partition base
0x10300000  slot 1                  2 MB
0x10500000  slot 2                  2 MB
0x10700000  slot 3                  2 MB
0x10900000  slot 4                  2 MB
0x10B00000  slot 5                  2 MB
0x10D00000  slot 6                  2 MB
0x10F00000  reserved (1 MB)
```

**The slot-0 unification**: slot 0's base is exactly the legacy
single-partition base. So a UF2 built with `-DBUILD_FOR_BOOTLOADER=ON` (no slot
index) **doubles as both** a legacy-mode UF2 (flash-on-launch) AND a slot-0
pinned UF2 — same image, no rebuild needed.

## Building the bootloader

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DHW_CONFIG=8 \
      -DPICO_PLATFORM=rp2350-arm-s -DENABLE_PIO_USB=1 -DUSE_PICO_EXTRAS_I2S=0 ..
make -j
# -> build/emuLoader.uf2   (flash via BOOTSEL)
```

## Building an emulator for a slot

In each emulator's `CMakeLists.txt` (on its `bootloader` branch):

```cmake
if(BUILD_FOR_BOOTLOADER)
    include("pico_shared/BootPartition.cmake")
    if(DEFINED BUILD_FOR_BOOTLOADER_SLOT)
        frens_link_to_pinned_slot(${projectname} ${BUILD_FOR_BOOTLOADER_SLOT})
    else()
        frens_offset_for_bootloader(${projectname})       # legacy / slot-0
    endif()
endif()
```

Build commands:

```sh
# Slot-0 / legacy (works in both modes):
cmake -DBUILD_FOR_BOOTLOADER=ON ..
# Pinned slot N (slot-mode boards only):
cmake -DBUILD_FOR_BOOTLOADER=ON -DBUILD_FOR_BOOTLOADER_SLOT=2 ..
```

Copy the resulting `.uf2` into [`uf2/<HW>/`](uf2/) on the SD card. The
framework already computes runtime flash addresses (FlashParams, no-PSRAM
ROM-in-flash) relative to `__flash_binary_end`, so relinking moves them
automatically.

## Preparing the SD card

```
/emu/<HW>/
  layout.txt                          <- optional, only consumed by Setup
  piconesPlus_slot1_AdafruitFruitJam_arm_piousb.uf2
  picopcePlus_AdafruitFruitJam_arm_piousb.uf2
  ...
```

`layout.txt` format (used by the **Setup / Rebuild flash** menu item):

```
# slot   uf2-filename
0        picopcePlus_AdafruitFruitJam_arm_piousb.uf2
1        piconesPlus_slot1_AdafruitFruitJam_arm_piousb.uf2
# lines starting with # are comments; missing slots are left alone
```

Setup pre-flights every UF2 (target_addr must match the slot it's assigned to)
before touching flash, so a misbuilt UF2 rejects cleanly. On success the
bootloader reboots and the menu shows the new pinned emulators — program names
read directly from each image's SDK `binary_info` (no per-emulator descriptor
needed).

## Resume after no-PSRAM ROM-load reboot (future configs)

When an emulator on a no-PSRAM board reboots itself to flash a ROM, the
bootloader's first action records that the chosen slot was running (via a
watchdog scratch register) and resumes that slot — physical reset / power-cycle
clears the scratch and the menu reappears. Dormant on HW_CONFIG 8 (PSRAM
present, ROMs go to RAM) but wired up now.

## Discovery of pinned-slot names

Every emulator's `CMakeLists.txt` already calls `pico_set_program_name(...)`,
which the SDK encodes via its **binary_info** system. Each image carries a
20-byte header within its first 256 bytes pointing to an array of binary_info
entries; the bootloader reads the program-name entry directly from the slot
via XIP. So no per-emulator descriptor / macro is needed — the menu always
shows the real `projectname` you set in CMake.
