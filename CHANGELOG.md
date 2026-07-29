# CHANGELOG

A resident .uf2 bootloader / front-end for the RP2350 retro-emulator family (pico-infonesPlus, pico-pcePlus, pico-genesisPlus, pico-smsplus, pico-peanutGB, …).

## v0.2

### Added

- **Artwork themes.** The graphical menu can carry up to ten sets of artwork in
  `<BASEDIR>/assets/themes/0` … `themes/9`. Press **UP** or **DOWN** in the
  graphical menu to cycle through the themes present on the card; the choice is
  saved immediately and restored on the next boot. A theme need not be
  complete — any application it has no image for falls back to theme 0.
- **On-screen help.** Press **START** in either menu mode for a full-screen
  summary of the controls, the meaning of the `*` and `!` markers, and the
  current mode, theme, board configuration and index file.
- **`GUI` and `THEME` keys in `boot.txt`**, holding the menu mode and the active
  theme.

### Changed

- **Menu artwork moved** from `<BASEDIR>/assets` to
  `<BASEDIR>/assets/themes/0`. Existing cards are migrated automatically on the
  first boot — the cached `.444`/`.555` files move too, so nothing is
  re-converted, and the `screensaver/` folder is left where it is. The move is
  resumable if it is interrupted.
- **`boot.txt` is now written by the bootloader.** Only the `GUI=` and `THEME=`
  lines are rewritten; comments, ordering, spacing and every other key are
  preserved. The file is created on the first change if it does not exist,
  carrying the effective value of all keys. Updates go through a temporary file
  that is re-parsed before being renamed into place, so a power cut cannot
  corrupt the configuration. A card that cannot be written is not an error: the
  change applies for the session and the help screen says it was not saved.
- **The `.guimode` file is gone**, folded into `boot.txt`'s `GUI` key. An
  existing `.guimode` is read once, migrated, and deleted.
- Menu footer and text-mode hints updated for the new controls.
- Boards without PSRAM now convert artwork for *every* theme at boot rather than
  one folder, since the converter cannot run once the menu is up. The first boot
  after adding a theme is correspondingly slower.
- **`pico-bootLoader_sdcard.zip` is a release asset again.** The archive carries
  the emulators, the *Doom* port and its WAD, the artwork themes, the screensaver
  sprites, a sample `boot.txt` and a new `versions.txt` recording which release
  tag each emulator was built from. Unpack it at the root of the card.
- **Release tags now say what changed.** `v0.N` is a bootloader firmware release
  (re-flash the board and refresh the card); `v0.N.M` ships the same firmware with
  refreshed emulator binaries, so only `/emu` on the card needs replacing. Each
  release lists the emulator versions its archive contains.
- **Doom's WAD is named `doom1-whx.uf2`** in `emulators.txt`, matching what the
  build actually produces. Earlier cards declared `doom1-whx-for-fruitjam.uf2`,
  which the loader could never find, so it launched *Doom* without flashing the
  WAD.

## General Info

[Binaries for each configuration and PCB design are at the end of this page](#downloads___).

## Getting started

For board-by-board wiring, supported display modes and more refer to the [pico-infonesPlus documentation](https://github.com/fhoedemakers/pico-infonesPlus#setup). The set of supported boards and their pinouts is identical between the two projects.

1. **Flash the bootloader.** Download the loader `.uf2` for your board. Hold
   BOOTSEL, connect the board over USB, and copy the `.uf2` onto the
   `RP2350` drive. See [Supported](https://github.com/fhoedemakers/pico-bootLoaders#supported-hardware) Hardware in the README.
2. **Prepare the SD card.** Download `pico-bootLoader_sdcard.zip` from the same
   Releases page and unpack it onto a FAT32- or exFAT-formatted card. The
   archive contains the emulators, the *Doom* port, the menu artwork, and a
   sample configuration file.
3. **Run it.** Insert the card and power on the board. The menu appears.

## Metadata
Download the full metadata pack [here](https://1drv.ms/u/c/db8991463e5b8b0c/IQD1kFD0-j94QoCW3Tw447AaAVq2XdkmUH4T40Bcmtf9ZL0?e=8eNkSe) and extract it to the root of the SD card. This should create a `/metadata` folder on the card.

<a name="downloads___"></a>


