#!/usr/bin/env bash
# Build the bootLoader for every supported RP2350 hardware
# configuration. Outputs land in releases/.
set -e
cd "$(dirname "$0")"

[ -d releases ] && rm -rf releases
mkdir releases

if ! command -v picotool >/dev/null 2>&1; then
    echo "picotool not found in PATH" >&2
    exit 1
fi

HWCONFIGS="5 6 7 8 9 11 13 14"
for HWCONFIG in $HWCONFIGS; do
    ./bld.sh -c "$HWCONFIG" -2
done

if [ -z "$(ls -A releases)" ]; then
    echo "No UF2 files produced in releases/" >&2
    exit 1
fi

echo
echo "Built loader binaries:"
for UF2 in releases/*.uf2; do
    ls -l "$UF2"
done
