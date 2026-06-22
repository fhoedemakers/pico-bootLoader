# pico_emuLoader

A resident `.uf2` **bootloader / front-end** for the RP2350 retro-emulator family
(pico-infonesPlus, pico-pcePlus, pico-genesisPlus, pico-smsplus, pico-peanutGB, …),
built on the same shared framework (`pico_shared`, `pico_lib`, `tusb_xinput`).

On every power-on the RP2350 bootrom runs the bootloader (it owns the start of
flash). It lists every `.uf2` it finds on SD under **`/emu/<HW_CONFIG>/`** and
shows a picker. The entry whose `pico_set_program_name()` matches the image
currently sitting in the application partition is **highlighted** as *in flash*:

- B-press on the **highlighted** (in-flash) entry → launches immediately via
  VTOR jump. No flash op, no SD I/O. Sub-second.
- B-press on any **other** entry → shows a brief *"Flashing…"* notice, then
  flashes that `.uf2` into the application partition and launches it.

Because the bootloader only *jumps* (it never hands over the boot vector), any
reset / power-cycle returns to the menu.

Scope today: **HW_CONFIG 8 (Adafruit Fruit Jam, HSTX)**. Other configs later.

## Flash memory map

Defined in [`pico_shared/BootPartition.cmake`](pico_shared/BootPartition.cmake) and
mirrored in [`src/boot_config.h`](src/boot_config.h):

```
0x10000000  Bootloader (this app)      1 MB     <- bootrom always runs this
0x10100000  Application partition      15 MB    <- emulator UF2s land here, jumped to
0x11000000  end (16 MB Fruit Jam flash)
```

Single slot at `0x10100000` — one build per emulator.

## Building the bootloader

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DHW_CONFIG=8 \
      -DPICO_PLATFORM=rp2350-arm-s -DENABLE_PIO_USB=1 -DUSE_PICO_EXTRAS_I2S=0 ..
make -j
# -> build/emuLoader.uf2   (flash via BOOTSEL)
```

Result is ~220 KB, well under the 1 MB bootloader region.

## Building an emulator for the bootloader

Each emulator is normally linked at `0x10000000`. To produce a partition build
for this bootloader, add this near the end of the emulator's `CMakeLists.txt`
(after the target is created and linked, before `pico_add_extra_outputs`):

```cmake
if(BUILD_FOR_BOOTLOADER)
    include("pico_shared/BootPartition.cmake")
    frens_offset_for_bootloader(${projectname})
endif()
```

then build with `-DBUILD_FOR_BOOTLOADER=ON`:

```sh
cd <emulator-repo>            # e.g. pico-infonesPlus on its 'bootloader' branch
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DHW_CONFIG=8 \
      -DPICO_PLATFORM=rp2350-arm-s -DENABLE_PIO_USB=1 -DUSE_PICO_EXTRAS_I2S=0 \
      -DBUILD_FOR_BOOTLOADER=ON ..
make -j
```

and copy the resulting `.uf2` into `pico_emuLoader/uf2/<HW_CONFIG>/`.

Single build per emulator — there is **no** per-slot variant. (`pico_shared/bld.sh`
does not pass `-DBUILD_FOR_BOOTLOADER`; use the manual cmake invocation above
when building for the bootloader.)

Done for HW_CONFIG 8 — all five live in [`uf2/8/`](uf2/8/):
piconesPlus, picopcePlus, picogenesisPlus, picosmsPlus, PicoPeanutGB.

## Preparing the SD card

Copy the relinked emulator `.uf2`s into `/emu/<HW_CONFIG>/` on the SD card, e.g.
`/emu/8/`. The repo's [`uf2/8/`](uf2/8/) holds prebuilt ones for Fruit Jam.

Only `.uf2`s linked to the application partition (`0x10100000`) can be flashed;
a standalone build (still linked at `0x10000000`) is rejected on-screen so it
can never overwrite the bootloader itself.

The in-flash-highlight feature matches each `.uf2` to the resident image by
**program name** (`pico_set_program_name()`, parsed from each file's
`binary_info`). Renaming a `.uf2` file is fine; the match still works.

## How "highlighted" is decided

On boot, after listing SD `.uf2`s, the bootloader:

1. Parses `pico_set_program_name()` from the resident image at `0x10100000`
   via XIP (~tens of microseconds).
2. Parses the same field from each SD `.uf2` on disk by seeking through the
   UF2 blocks that contain the `binary_info` structure — no flashing, ~100 ms
   per file on a typical SD card.
3. Highlights the SD entry whose program name matches the resident one, and
   selects it by default so a single B-press launches immediately.

If the resident image cannot be parsed (e.g. flash is erased, or it's a
non-SDK image), no entry is highlighted and every choice flashes.

## ROM-load reboot interaction (no-PSRAM boards)

On PSRAM-less boards an emulator reboots itself (`watchdog_enable`) to flash a
selected ROM. The bootloader's first action is a **resume check**: if
`watchdog_enable_caused_reboot()` is set and a valid image is present, it jumps
straight back into the emulator (no menu) so the emulator can finish flashing
its ROM. A physical reset / power-cycle does not set that flag, so it shows
the menu. On HW_CONFIG 8 (PSRAM) ROMs load to PSRAM and this path is dormant.

## Branches

- `main` — the simple flash-on-every-launch bootloader (no highlight).
- `single-slot` — this branch. Adds the in-flash highlight + no-flash launch
  for the resident image.
- `slots` — earlier experiment that pinned up to 7 emulators in distinct 2 MB
  slots. Preserved for reference; the per-emulator build matrix it required
  was the reason this branch replaced it.

## Future work

The [`assets/`](assets/) folder contains per-system PNGs (nes, pce, md, sms,
gb, …) intended for a future graphical picker. The current menu already
exposes each emulator's canonical program name, which is the lookup key a
GUI renderer will use to pick the right icon.
