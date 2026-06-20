# pico_emuLoader

A resident `.uf2` **bootloader / front-end** for the RP2350 retro-emulator family
(pico-infonesPlus, pico-pcePlus, pico-genesisPlus, pico-smsplus, pico-peanutGB, …),
built on the same shared framework (`pico_shared`, `pico_lib`, `tusb_xinput`).

On every power-on the RP2350 bootrom runs the bootloader (it owns the start of
flash). It shows an on-screen menu of the emulators available for the current
board config — read from the SD card at **`/emu/<HW_CONFIG>/*.uf2`** — and on the
**B button** it flashes the chosen emulator into the application partition and
jumps to it. Because the bootloader only *jumps* (it never hands over the boot
vector), any reset / power-cycle returns to the menu.

Scope today: **HW_CONFIG 8 (Adafruit Fruit Jam, HSTX)**. Other configs / picoDVI later.

## Flash memory map (single source of truth)

Defined in [`pico_shared/BootPartition.cmake`](pico_shared/BootPartition.cmake) and
mirrored in [`src/boot_config.h`](src/boot_config.h):

```
0x10000000  Bootloader (this app)      1 MB     <- bootrom always runs this
0x10100000  Application partition      15 MB    <- emulator UF2s land here, jumped to
0x11000000  end (16 MB Fruit Jam flash)
```

## Building the bootloader

```sh
mkdir build && cd build
cmake -DCMAKE_BUILD_TYPE=Release -DPICO_BOARD=pico2 -DHW_CONFIG=8 \
      -DPICO_PLATFORM=rp2350-arm-s -DENABLE_PIO_USB=1 -DUSE_PICO_EXTRAS_I2S=0 ..
make -j
# -> build/emuLoader.uf2   (flash via BOOTSEL)
```

## Preparing the SD card

Copy the **relinked** emulator UF2s into `/emu/<HW_CONFIG>/` on the SD card, e.g.
`/emu/8/`. The repo's [`uf2/8/`](uf2/8/) holds prebuilt ones. Only UF2s linked to
the application partition (`0x10100000`) can be flashed; a standalone build (still
linked at `0x10000000`) is rejected on-screen so it can never overwrite the
bootloader.

## Relinking an emulator for the bootloader

Each emulator is normally linked at `0x10000000`. To produce a partition build,
add this near the end of the emulator's `CMakeLists.txt` (after the target is
created and linked, before `pico_add_extra_outputs`):

```cmake
if(BUILD_FOR_BOOTLOADER)
    include("pico_shared/BootPartition.cmake")
    frens_offset_for_bootloader(${projectname})
endif()
```

then build with `-DBUILD_FOR_BOOTLOADER=ON` and copy the resulting `.uf2` into
`uf2/<HW_CONFIG>/`. `BootPartition.cmake` lives in the shared `pico_shared` repo;
the framework already computes its runtime flash addresses (FlashParams, and the
no-PSRAM ROM-in-flash region) relative to `__flash_binary_end`, so relinking moves
them into the partition automatically.

Done for HW_CONFIG 8: **all five** — piconesPlus, picopcePlus, picogenesisPlus, picosmsPlus, PicoPeanutGB.

## ROM-load reboot interaction (no-PSRAM boards, future)

On PSRAM-less boards an emulator reboots itself (`watchdog_enable`) to flash a
selected ROM. The bootloader's first action is a **resume check**: if
`watchdog_enable_caused_reboot()` is set and a valid image is present, it jumps
straight back into the emulator (no menu) so the emulator can finish flashing its
ROM. A physical reset / power-cycle does not set that flag, so it shows the menu.
On HW_CONFIG 8 (PSRAM) ROMs load to PSRAM and this path is dormant.
