#!/usr/bin/env bash
# Pack the SD-card archive for a pico-bootLoader release.
#
# Source of truth is the loader repo's own emu/ tree — exactly what
# build_emulators.sh installs and what a working SD card looks like. Nothing is
# re-derived from emulator build trees, so there is no per-board filename table
# to keep in sync with pico_shared/bld.sh, and every board and program that got
# built is packed automatically.
#
# Archive layout (matches boot.txt's BASEDIR=/emu; emu/ sits at the archive root,
# so unzipping at the root of the card is all a user has to do):
#   emu/<HW_CONFIG>/<prog_name>.uf2      plus any aux (WAD) UF2s alongside
#   emu/emulators.txt
#   emu/boot.example.txt
#   emu/versions.txt                     (if present; written by build_emulators.sh -z)
#   emu/assets/themes/<0-9>/<image_key>.{444,555}   (or the .png/.jpg source,
#                                        for a key that has no cache to ship)
#   emu/assets/screensaver/<sprite>.{444,555}
#
# Usage: pack_sdcard.sh <loader_dir> <output_zip>
set -euo pipefail

if [ $# -ne 2 ]; then
    echo "Usage: $0 <loader_dir> <output_zip>" >&2
    exit 1
fi

LOADER="$1"
OUT_ZIP="$2"

[ -f "$LOADER/emu/emulators.txt" ] || { echo "emulators.txt not found at $LOADER/emu/emulators.txt" >&2; exit 1; }

# Zip with whatever is on the box. UF2s are mostly padding, so compression is
# not optional here — it is the difference between a ~10 MB and a ~90 MB
# download. `python3 -m zipfile -c` is deliberately not used: its CLI writes
# ZIP_STORED (no compression).
if command -v zip >/dev/null 2>&1; then
    ZIP_IMPL=zip
elif command -v python3 >/dev/null 2>&1; then
    ZIP_IMPL=python3
else
    echo "ERROR: need either 'zip' or 'python3' to build the archive (apt install zip)" >&2
    exit 1
fi

# make_zip <staging_dir> <out_zip>   — archives the staging dir's emu/ subtree
# with emu/ at the archive root, deflated.
make_zip() {
    local stage="$1" out="$2"
    case "$ZIP_IMPL" in
        zip)
            ( cd "$stage" && zip -r -q "$out" emu )
            ;;
        python3)
            ( cd "$stage" && python3 - "$out" <<'PY'
import os, sys, zipfile
out = sys.argv[1]
with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as zf:
    for root, dirs, files in os.walk("emu"):
        dirs.sort()
        for name in sorted(files):
            path = os.path.join(root, name)
            zf.write(path, path)
PY
            )
            ;;
    esac
}

# Absolute, so the staging-dir cd below can't break it.
case "$OUT_ZIP" in
    /*) ;;
    *) OUT_ZIP="$PWD/$OUT_ZIP" ;;
esac

warn() { echo "WARN:  $*" >&2; }
die()  { echo "ERROR: $*" >&2; exit 1; }

STAGE="$(mktemp -d -t pico-sdcard.XXXXXX)"
trap 'rm -rf "$STAGE"' EXIT
DEST="$STAGE/emu"
mkdir -p "$DEST"

shopt -s nullglob

# --- The index and the boot.txt example --------------------------------------
cp "$LOADER/emu/emulators.txt" "$DEST/emulators.txt"

# boot.txt lives at the repo root as the documented example; on the card the
# loader reads <BASEDIR>/boot.txt, so it ships as boot.example.txt for the user
# to rename. emu/boot.example.txt is kept in sync in-tree; prefer whichever
# exists, root first.
if [ -f "$LOADER/boot.txt" ]; then
    cp "$LOADER/boot.txt" "$DEST/boot.example.txt"
elif [ -f "$LOADER/emu/boot.example.txt" ]; then
    cp "$LOADER/emu/boot.example.txt" "$DEST/boot.example.txt"
else
    warn "no boot.txt / emu/boot.example.txt to pack"
fi

if [ -f "$LOADER/emu/versions.txt" ]; then
    cp "$LOADER/emu/versions.txt" "$DEST/versions.txt"
else
    warn "emu/versions.txt not found — the archive will not record emulator versions"
fi

# --- Parse the index: prog -> aux_uf2, plus the artwork keys ------------------
# Format: <program_name>;<image_key>;<display_name>[;<aux_uf2>]
PROG_NAMES=()
IMAGE_KEYS=()
declare -A AUX_OF=()
declare -A SEEN_KEY=()
while IFS=';' read -r prog img _name aux || [ -n "${prog:-}" ]; do
    [ -z "${prog:-}" ] && continue
    [[ "$prog" == \#* ]] && continue
    PROG_NAMES+=("$prog")
    AUX_OF[$prog]="$(printf '%s' "${aux:-}" | tr -d '[:space:]')"
    img="$(printf '%s' "${img:-}" | tr -d '[:space:]')"
    if [ -n "$img" ] && [ -z "${SEEN_KEY[$img]+x}" ]; then
        SEEN_KEY[$img]=1
        IMAGE_KEYS+=("$img")
    fi
done < "$LOADER/emu/emulators.txt"
[ ${#PROG_NAMES[@]} -gt 0 ] || die "no emulators listed in emu/emulators.txt"

# --- Per-board binaries ------------------------------------------------------
packed=0
missing=0
empty=0
boards=0
for hwdir in "$LOADER/emu/"[0-9] "$LOADER/emu/"[0-9][0-9]; do
    [ -d "$hwdir" ] || continue
    hw="$(basename "$hwdir")"
    have_any=0

    for prog in "${PROG_NAMES[@]}"; do
        app="$hwdir/${prog}.uf2"
        if [ ! -f "$app" ]; then
            # Not every emulator builds for every board (excluded combinations,
            # Doom's four boards); that is normal, so only note it.
            echo "  --   HW=$hw $prog: no UF2"
            missing=$((missing + 1))
            continue
        fi
        # A 0-byte UF2 is a failed build that looks like a successful one until
        # the board refuses to boot. Never ship it.
        if [ ! -s "$app" ]; then
            warn "HW=$hw $prog: ${prog}.uf2 is empty (0 bytes)"
            empty=$((empty + 1))
            continue
        fi

        mkdir -p "$DEST/$hw"
        cp "$app" "$DEST/$hw/${prog}.uf2"
        echo "  pack HW=$hw $prog.uf2"
        packed=$((packed + 1))
        have_any=1

        # Companion DATA-family UF2 (Doom's WAD): the loader derives its path
        # from column 4 of emulators.txt and expects it next to the app.
        aux="${AUX_OF[$prog]:-}"
        if [ -n "$aux" ]; then
            if [ -s "$hwdir/$aux" ]; then
                cp "$hwdir/$aux" "$DEST/$hw/$aux"
                echo "  pack HW=$hw $aux  (aux for $prog)"
                packed=$((packed + 1))
            else
                warn "HW=$hw $prog declares aux '$aux' in emulators.txt but $hwdir/$aux is missing or empty — the loader will launch without flashing it"
                missing=$((missing + 1))
            fi
        fi
    done
    (( have_any )) && boards=$((boards + 1))
done

[ "$packed" -gt 0 ] || die "nothing to pack: no non-empty UF2s under $LOADER/emu/<HW_CONFIG>/ (run build_emulators.sh first)"
if [ "$empty" -gt 0 ]; then
    die "$empty UF2(s) were empty (0 bytes) — refusing to build an archive that ships a broken build"
fi

# --- Menu artwork ------------------------------------------------------------
# Preconverted .444/.555 caches ship in preference to anything else: the .png/.jpg
# sources are ~30x larger and shipping both would roughly double the archive.
#
# But a key with no cache would otherwise have no image on the card at all, and
# the loader can only convert what is there — so for those, and only those, the
# source image ships instead and the first boot converts it.
#
# Sources are looked up by image_key from emulators.txt rather than globbed:
# theme folders also hold unused icons (2600.png, gba.png, recent.png, ...) and
# alternates (md-alt.png, sms_gg_alt.png). Globbing would ship those and, worse,
# make the loader convert them at boot — it converts every image in every theme,
# which is the expensive path.
asset_count=0
src_count=0
for theme in "$LOADER/emu/assets/themes/"[0-9]; do
    [ -d "$theme" ] || continue
    tnum="$(basename "$theme")"
    mkdir -p "$DEST/assets/themes/$tnum"
    for ext in 444 555; do
        for src in "$theme/"*."$ext"; do
            base="$(basename "$src")"
            case "$base" in
                *" copy."*) continue ;;
            esac
            cp "$src" "$DEST/assets/themes/$tnum/$base"
            asset_count=$((asset_count + 1))
        done
    done

    for key in "${IMAGE_KEYS[@]}"; do
        # Either cache is enough for the loader to render; it regenerates the
        # other on first boot.
        if [ -s "$theme/$key.444" ] || [ -s "$theme/$key.555" ]; then
            continue
        fi
        packed_src=0
        for ext in png jpg jpeg; do
            if [ -s "$theme/$key.$ext" ]; then
                cp "$theme/$key.$ext" "$DEST/assets/themes/$tnum/$key.$ext"
                echo "  pack theme $tnum $key.$ext  (no .444/.555 cache; converted on first boot)"
                src_count=$((src_count + 1))
                packed_src=1
                break
            fi
        done
        # Theme 0 is the documented fallback for every other theme, so a key it
        # cannot render at all is worth flagging. Partial non-zero themes are by
        # design (README: "Incomplete themes are fine") and stay quiet.
        if [ "$packed_src" -eq 0 ] && [ "$tnum" = 0 ]; then
            warn "theme 0 has no artwork for '$key' (no .444/.555 and no .png/.jpg) — it will show as a black tile"
        fi
    done
done
echo "  pack $asset_count theme asset(s), $src_count uncached source image(s)"

ss_count=0
if [ -d "$LOADER/emu/assets/screensaver" ]; then
    mkdir -p "$DEST/assets/screensaver"
    for ext in 444 555; do
        for src in "$LOADER/emu/assets/screensaver/"*."$ext"; do
            base="$(basename "$src")"
            case "$base" in
                *" copy."*) continue ;;
            esac
            cp "$src" "$DEST/assets/screensaver/$base"
            ss_count=$((ss_count + 1))
        done
    done
fi
echo "  pack $ss_count screensaver sprite(s)"

# --- Zip it ------------------------------------------------------------------
rm -f "$OUT_ZIP"
mkdir -p "$(dirname "$OUT_ZIP")"
make_zip "$STAGE" "$OUT_ZIP"
[ -s "$OUT_ZIP" ] || die "zip produced no output at $OUT_ZIP"

entries=$(find "$DEST" -type f | wc -l)
raw_bytes=$(du -sb "$DEST" | cut -f1)
zip_bytes=$(stat -c%s "$OUT_ZIP")
echo
echo "Packed $packed file(s) across $boards board(s); $missing expected-but-absent combination(s)."
printf "Archive: %s\n" "$OUT_ZIP"
printf "         %s entries, %s MiB raw -> %s MiB compressed (%s)\n" \
    "$entries" \
    "$(awk -v b="$raw_bytes" 'BEGIN{printf "%.1f", b/1048576}')" \
    "$(awk -v b="$zip_bytes" 'BEGIN{printf "%.1f", b/1048576}')" \
    "$ZIP_IMPL"
