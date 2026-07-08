#!/usr/bin/env bash
# Assemble an SD-card-ready tree for the pico-bootLoader release.
#
# Layout produced:
#   <output_dir>/emu/uf2/<HW_CONFIG>/<prog_name>.uf2
#   <output_dir>/emu/uf2/emulators.txt
#   <output_dir>/emu/assets/<image_key>.{444,555}
#
# Usage: pack_release.sh <output_dir> <emu_build_dir> <loader_dir>
set -euo pipefail

if [ $# -ne 3 ]; then
    echo "Usage: $0 <output_dir> <emu_build_dir> <loader_dir>" >&2
    exit 1
fi

OUTDIR="$1"
EMU_BUILD="$2"
LOADER="$3"

[ -f "$LOADER/uf2/emulators.txt" ] || { echo "emulators.txt not found at $LOADER/uf2/emulators.txt" >&2; exit 1; }
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
SKIP_PROGS=("picoPacPlus")

PROG_NAMES=()
while IFS=';' read -r prog _img _name; do
    [ -z "${prog:-}" ] && continue
    [[ "$prog" == \#* ]] && continue
    skip=0
    for s in "${SKIP_PROGS[@]}"; do
        if [ "$prog" = "$s" ]; then skip=1; break; fi
    done
    [ $skip -eq 1 ] && continue
    PROG_NAMES+=("$prog")
done < "$LOADER/uf2/emulators.txt"

if [ ${#PROG_NAMES[@]} -eq 0 ]; then
    echo "No emulators selected from emulators.txt" >&2
    exit 1
fi

mkdir -p "$OUTDIR/emu/uf2" "$OUTDIR/emu/assets"
cp "$LOADER/uf2/emulators.txt" "$OUTDIR/emu/uf2/emulators.txt"

shopt -s nullglob
asset_count=0
for ext in 444 555; do
    for src in "$LOADER/assets/"*."$ext"; do
        base="$(basename "$src")"
        case "$base" in
            *" copy."*) continue ;;
        esac
        cp "$src" "$OUTDIR/emu/assets/$base"
        asset_count=$((asset_count + 1))
    done
done
echo "Copied $asset_count asset file(s) to $OUTDIR/emu/assets/"

missing=0
packed=0
for hw in "${!DESCRIPTOR[@]}"; do
    desc="${DESCRIPTOR[$hw]}"
    mkdir -p "$OUTDIR/emu/uf2/$hw"
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
        cp "$chosen" "$OUTDIR/emu/uf2/$hw/${prog}.uf2"
        echo "Packed: HW=$hw $prog <- $(basename "$chosen")"
        packed=$((packed + 1))
    done
done

echo
echo "Packed $packed binary(ies); $missing combination(s) had no matching build."
echo "Tree:"
find "$OUTDIR/emu" -maxdepth 3 | sort
