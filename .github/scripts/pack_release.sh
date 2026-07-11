#!/usr/bin/env bash
# Assemble an SD-card-ready tree for the pico-bootLoader release.
#
# Layout produced (matches boot.txt's BASEDIR=/emu; the workflow zips the emu/
# subtree, so everything must live under <output_dir>/emu/):
#   <output_dir>/emu/<HW_CONFIG>/<prog_name>.uf2
#   <output_dir>/emu/emulators.txt
#   <output_dir>/emu/boot.example.txt
#   <output_dir>/emu/assets/<image_key>.{444,555}
#   <output_dir>/emu/assets/screensaver/<sprite>.{444,555}   (if present)
#
# Usage: pack_release.sh <output_dir> <emu_build_dir> <loader_dir>
# The loader dir is the pico-bootLoader repo root; its SD tree lives under emu/.
set -euo pipefail

if [ $# -ne 3 ]; then
    echo "Usage: $0 <output_dir> <emu_build_dir> <loader_dir>" >&2
    exit 1
fi

OUTDIR="$1"
EMU_BUILD="$2"
LOADER="$3"

[ -f "$LOADER/emu/emulators.txt" ] || { echo "emulators.txt not found at $LOADER/emu/emulators.txt" >&2; exit 1; }
[ -d "$EMU_BUILD" ] || { echo "emulator build dir not found: $EMU_BUILD" >&2; exit 1; }

# HW_CONFIG -> board descriptor used in bld.sh-produced filenames.
# Derived from pico_shared/bld.sh's case statement; RP2350-ARM configs only.
# Configs 1 and 2 leave USESIMPLEFILENAMES=0, so their filenames carry an extra
# _<PICO_BOARD> segment (pico2) before _arm; fold it into the descriptor so the
# ${prog}_${desc}_arm* glob below still matches (the _w variants are filtered out).
declare -A DESCRIPTOR=(
    [1]="PimoroniDVI_pico2"
    [2]="AdafruitDVISD_pico2"
    [5]="AdafruitMetroRP2350"
    [6]="WaveShareRP2350ZeroWithPCB"
    [7]="WaveShareRP2350PiZero"
    [8]="AdafruitFruitJam"
    [9]="WaveShare2350USBA"
    [13]="MurmulatorM2"
    [14]="AdafruitFeatherRP2350_TLV320DAC3100"
)

# Emulators in emulators.txt with no bootloader branch yet.
SKIP_PROGS=()

# fruitjam-doom (Doom!) is packed specially: it targets only the Fruit Jam board
# (HW_CONFIG 8), is built outside the generic */releases/ layout, and ships a
# companion DATA-family WAD UF2 (the emulators.txt aux_uf2). See the dedicated
# block after the generic loop. DOOM_AUX is captured from column 4 below.
DOOM_PROG="doom_tiny"
DOOM_HW=8
DOOM_SRC="$EMU_BUILD/fruitjam-doom/build_bl_fruitjam/src"
DOOM_AUX=""

PROG_NAMES=()
while IFS=';' read -r prog _img _name aux; do
    [ -z "${prog:-}" ] && continue
    [[ "$prog" == \#* ]] && continue
    if [ "$prog" = "$DOOM_PROG" ]; then
        # Remember the aux (WAD) filename; doom is packed separately below.
        DOOM_AUX="$(printf '%s' "${aux:-}" | tr -d '[:space:]')"
        continue
    fi
    skip=0
    for s in "${SKIP_PROGS[@]}"; do
        if [ "$prog" = "$s" ]; then skip=1; break; fi
    done
    [ $skip -eq 1 ] && continue
    PROG_NAMES+=("$prog")
done < "$LOADER/emu/emulators.txt"

if [ ${#PROG_NAMES[@]} -eq 0 ]; then
    echo "No emulators selected from emulators.txt" >&2
    exit 1
fi

mkdir -p "$OUTDIR/emu" "$OUTDIR/emu/assets"
cp "$LOADER/emu/emulators.txt" "$OUTDIR/emu/emulators.txt"
cp "$LOADER/boot.txt" "$OUTDIR/emu/boot.example.txt"
shopt -s nullglob
asset_count=0
for ext in 444 555; do
    for src in "$LOADER/emu/assets/"*."$ext"; do
        base="$(basename "$src")"
        case "$base" in
            *" copy."*) continue ;;
        esac
        cp "$src" "$OUTDIR/emu/assets/$base"
        asset_count=$((asset_count + 1))
    done
done
echo "Copied $asset_count asset file(s) to $OUTDIR/emu/assets/"

# Screensaver sprites (optional): <BASEDIR>/assets/screensaver/<name>.444|.555
if [ -d "$LOADER/emu/assets/screensaver" ]; then
    mkdir -p "$OUTDIR/emu/assets/screensaver"
    ss_count=0
    for ext in 444 555; do
        for src in "$LOADER/emu/assets/screensaver/"*."$ext"; do
            base="$(basename "$src")"
            case "$base" in
                *" copy."*) continue ;;
            esac
            cp "$src" "$OUTDIR/emu/assets/screensaver/$base"
            ss_count=$((ss_count + 1))
        done
    done
    echo "Copied $ss_count screensaver sprite(s) to $OUTDIR/emu/assets/screensaver/"
fi

missing=0
packed=0
for hw in "${!DESCRIPTOR[@]}"; do
    desc="${DESCRIPTOR[$hw]}"
    mkdir -p "$OUTDIR/emu/$hw"
    for prog in "${PROG_NAMES[@]}"; do
        matches=()
        for f in "$EMU_BUILD"/*/releases/"${prog}_${desc}_arm"*.uf2; do
            [ -f "$f" ] || continue
            base="$(basename "$f")"
            case "$base" in
                *_riscv*|*_w_*|*_w.uf2) continue ;;
            esac
            matches+=("$f")
        done
        if [ ${#matches[@]} -eq 0 ]; then
            echo "WARN: no $prog binary for HW_CONFIG=$hw ($desc)"
            missing=$((missing + 1))
            continue
        fi
        chosen=""
        for m in "${matches[@]}"; do
            if [[ "$(basename "$m")" == *"_bl.uf2" ]]; then
                chosen="$m"
                break
            fi
        done
        [ -z "$chosen" ] && chosen="${matches[0]}"
        cp "$chosen" "$OUTDIR/emu/$hw/${prog}.uf2"
        echo "Packed: HW=$hw $prog <- $(basename "$chosen")"
        packed=$((packed + 1))
    done
done

# --- fruitjam-doom (Doom!) : Fruit Jam only, bespoke build layout ------------
# The app and its WAD live in build_bl_fruitjam/src/ (not */releases/) and go
# only into the HW_CONFIG 8 dir, next to each other so the loader finds the aux.
mkdir -p "$OUTDIR/emu/$DOOM_HW"
doom_app="$DOOM_SRC/doom_tiny.uf2"
if [ -f "$doom_app" ]; then
    cp "$doom_app" "$OUTDIR/emu/$DOOM_HW/${DOOM_PROG}.uf2"
    echo "Packed: HW=$DOOM_HW $DOOM_PROG <- $(basename "$doom_app")"
    packed=$((packed + 1))
    if [ -n "$DOOM_AUX" ]; then
        if [ -f "$DOOM_SRC/$DOOM_AUX" ]; then
            cp "$DOOM_SRC/$DOOM_AUX" "$OUTDIR/emu/$DOOM_HW/$DOOM_AUX"
            echo "Packed: HW=$DOOM_HW aux <- $DOOM_AUX"
            packed=$((packed + 1))
        else
            echo "WARN: doom aux $DOOM_AUX not found in $DOOM_SRC"
            missing=$((missing + 1))
        fi
    fi
else
    echo "WARN: no $DOOM_PROG binary (looked in $DOOM_SRC)"
    missing=$((missing + 1))
fi

echo
echo "Packed $packed binary(ies); $missing combination(s) had no matching build."
echo "Tree:"
find "$OUTDIR/emu" -maxdepth 3 | sort
