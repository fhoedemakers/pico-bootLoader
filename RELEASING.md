# Releasing

Maintainer checklist for publishing pico-bootLoader. Two artifacts ship, and they
have different lifecycles:

| Artifact | Built by | How it gets onto the release |
| --- | --- | --- |
| `pico-bootLoader_<board>_arm[_piousb].uf2` (11 files) | CI, on the self-hosted runner | attached automatically by the workflow |
| `pico-bootLoader_sdcard.zip` | **locally**, by `build_emulators.sh -c all -z` | `gh release upload`, by hand |

The archive is not built in CI because it needs all nine emulator toolchains and
takes hours; the loader builds take minutes. That split is why step 4 below is
manual.

## Tag convention

| Tag | Meaning | What users must do |
| --- | --- | --- |
| `v0.N` | bootloader firmware changed | re-flash the board **and** refresh the card |
| `v0.N.M` | same firmware, refreshed emulator binaries | replace `/emu` on the card only |

A three-component tag makes the workflow lead the release notes with a banner
saying the firmware is unchanged since `v0.N`, so users know not to re-flash.
Both forms are matched by the existing `push:` tag globs, and `v*.*-alpha` is
available for dry runs.

## Prerequisites

```bash
export PICO_SDK_PATH=~/pico/pico-sdk
export PICO_PIO_USB_PATH=~/pico/Pico-PIO-USB   # required for HW_CONFIG 7, 8, 9, 14
export PICO_EXTRAS_PATH=~/pico/pico-extras     # required by pico-doom only
gh auth status                                  # needs repo write
```

`zip` or `python3` must be present for the packer (either is fine).

`PICO_EXTRAS_PATH` is a pico-doom requirement, not an emulator one — pico-doom
takes its whole toolchain from the environment via its `pico-env.sh` rather than
vendoring it. Leave it unset and everything else still builds, pico-duke3D
included; the two Doom variants are reported as `SKIP` with the reason, and the
archive ships without Doom.

---

## Scenario A — bootloader firmware changed

The emulators are untouched, so the existing `pico-bootLoader_sdcard.zip` from
the previous release stays valid and does **not** need re-uploading. Note this
explicitly in the release notes, otherwise users will hunt for a missing asset.

```bash
# 1. Update CHANGELOG.md — its whole contents become the release body,
#    below the generated header and version table.
$EDITOR CHANGELOG.md
git commit -am "Changelog for v0.3" && git push

# 2. Build and publish. The release step creates the tag itself.
gh workflow run BuildAndRelease.yml -f tag=v0.3
gh run watch
```

Then follow [Verifying a release](#verifying-a-release). Pushing the tag by hand
(`git tag v0.3 && git push origin v0.3`) does exactly the same thing.

## Scenario B — an emulator was updated (firmware unchanged)

This is the common case: an emulator repo got a new tag, so the card content
changes but the board does not need re-flashing.

```bash
# 1. Build every emulator from its latest release tag and pack the archive.
#    -j 3 keeps total load near nproc; -z requires -c all.
./build_emulators.sh -c all -j 3 -z
```

Check the grand summary before going further. Every emulator should be `BUILT`
for the boards it supports. `SKIP` is expected for excluded combinations
(`picogenesisPlus` on HW 7; Doom outside boards 2, 8, 13, 14; `duke3d_game`
outside boards 2, 8, 13). Any `FAIL` or `MISSING` means the archive is
incomplete — fix it and re-run rather than shipping a partial card.

The packer refuses to build an archive containing a 0-byte `.uf2`, so a failed
link cannot ship silently. If it stops with
`refusing to build an archive that ships a broken build`, rebuild the emulator it
names.

```bash
# 2. Commit the manifest so the release notes can quote it.
#    emu/versions.txt records <program>;<repo>;<tag>;<pico_shared> per emulator.
git add emu/versions.txt
git commit -m "Refresh emulator bundle" && git push

# 3. Build the loader and publish, bumping the third component.
gh workflow run BuildAndRelease.yml -f tag=v0.2.1
gh run watch

# 4. Attach the archive to the release the workflow just created.
gh release upload v0.2.1 releases/pico-bootLoader_sdcard.zip
```

## Scenario C — both changed

Run Scenario B's steps 1–2, fold the firmware notes into `CHANGELOG.md`, then
publish with a **two-component** tag (`v0.3`, not `v0.3.0`) so users are told to
re-flash, and upload the archive as in step 4.

---

## Dry run

Two ways to rehearse without publishing:

```bash
# Build everything, release nothing (empty tag input).
gh workflow run BuildAndRelease.yml
```

```bash
# Full rehearsal against a throwaway tag, then remove it.
gh workflow run BuildAndRelease.yml -f tag=v0.2.1-alpha
gh run watch
gh release upload v0.2.1-alpha releases/pico-bootLoader_sdcard.zip
gh release delete v0.2.1-alpha --cleanup-tag
```

## Verifying a release

```bash
gh release view v0.2.1
```

- **11 loader `.uf2` assets** — one per board, plus the two `_pico2_w_` variants
  for HW_CONFIG 1 and 2.
- `pico-bootLoader_sdcard.zip` present, unless this is a Scenario A release.
- Notes open with the "SD-card content update" banner for a `v0.N.M` tag.
- The version table lists every emulator with a real tag — no `VX.X`, no blanks
  in the Version column.
- On hardware: unzip the archive at the root of a FAT32/exFAT card, boot, confirm
  the picker lists every entry, launch one emulator and check it shows its
  version rather than a build date. Launch *Doom!* and confirm the WAD flashes
  instead of logging `AUX_ERROR`.

## Rolling back

```bash
gh release delete v0.2.1 --cleanup-tag   # removes the release and its tag
```

Re-cutting the same tag afterwards is fine — the release step creates the tag
from `github.sha`, so it will point at whatever is current.

---

## Notes and gotchas

- **The version stamp is applied at build time, never committed.** CI seds the
  tag into `CMakeLists.txt` and `pico_shared/menu.h`; `build_emulators.sh` seds
  each emulator's own `pico_shared/menu.h`. In-tree both keep their placeholders
  (`"0.1"`, `"VX.X"`), and a build from an unstamped tree shows the build date
  instead — that is how a dev build identifies itself.
- **`pico_shared` in tag mode is not the revision the tag pins.** `bld.sh` gained
  `-b` (`BUILD_FOR_BOOTLOADER`) only in `pico_shared` `f2c8be9`, which the
  emulator release tags predate — their pinned revision rejects `-b` outright.
  Tag mode builds each emulator's tagged source against `pico_shared` `main` and
  records both refs in the manifest. Once the emulator repos pin a `-b`-capable
  `pico_shared` and are re-tagged, the substitution becomes a no-op.
- **A tag pushed from CI would not trigger this workflow** — GitHub does not fire
  workflows for events created with the default `GITHUB_TOKEN`. That is why the
  release step passes `tag_name` + `target_commitish` and creates the tag itself
  rather than pushing one, and why no PAT is needed.
- **`CHANGELOG.md` is used whole** as the release body, appended below the
  generated banner and version table. There is no per-tag extraction, so prune it
  when it grows unwieldy.
- **`emu/<HW_CONFIG>/` is gitignored** — the built emulator UF2s live only in the
  archive. `emu/versions.txt` is tracked precisely so the shipped versions are
  recorded in git.
- **The self-hosted runner hardcodes** `/datalocal/pico/pico-sdk` and
  `/datalocal/Pico-PIO-USB`; a GitHub-hosted runner cannot build this.
