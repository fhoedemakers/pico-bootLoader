#!/usr/bin/env bash
# Clone + build the bootloader-targeted emulators for one hwconfig (or, with
# -c all, every supported hwconfig) and drop the resulting UF2s into
# emu/<hwconfig>/<prog_name>.uf2.
#
# By default every emulator repo is built from its LATEST RELEASE TAG, with that
# tag stamped into pico_shared/menu.h SWVERSION so the emulator reports a version
# instead of a build date. This is the mode the release bundle is cut from; add
# -z to also pack the SD-card zip.
#
# pico_shared is NOT left at the revision the tag pins. bld.sh only learned -b
# (BUILD_FOR_BOOTLOADER) in pico_shared f2c8be9, and the emulator release tags
# predate that: they pin a pico_shared whose bld.sh rejects -b outright, so a
# bootloader-format UF2 cannot be produced from a tag's own pin. Tag mode
# therefore builds the emulator's tagged source against pico_shared main, and
# emu/versions.txt records both refs.
#
# -B instead asks interactively for a branch to use for the emulator repos and
# for pico_shared; -m non-interactively builds each repo's default branch (main
# or master, resolved per repo from the remote HEAD). Neither stamps a version.
set -euo pipefail

JOBS=2
TAG_MODE=1
BRANCH_MODE=0
MAIN_MODE=0
PACK_ZIP=0
HWCONFIG_ARG=""
usage() {
    cat <<EOF
Usage: $0 [-c N|all] [-j N] [-B|-m] [-z] [-h]
  -c N   build for HW_CONFIG N non-interactively (skip the picker).
  -c all build for every supported HW_CONFIG, unattended. PIO-USB configs are
         skipped automatically when PICO_PIO_USB_PATH is unset/unusable.
  -j N   build up to N emulators in parallel (default: $JOBS). Each build
         is capped to nproc/N processors so total load stays near nproc.
         With -j 1, output streams live; with -j >1, per-emulator logs are
         written to a temp dir.
  -B     ask interactively for the branch to build for the emulator repos and
         for pico_shared, instead of using each repo's latest release tag.
  -m     non-interactively build each repo's default branch (main or master,
         whichever its remote HEAD points at), skipping the branch picker.
  -z     after building, write emu/versions.txt and pack the SD-card archive
         releases/pico-bootLoader_sdcard.zip. Requires -c all (a zip built from
         one hwconfig would be missing every other board) and the default tag
         mode (a release bundle must be built from tags).
  -h     this help

Default (no -B/-m): each emulator repo's latest release tag, built against
pico_shared main (the tags pin a pico_shared whose bld.sh predates -b).
EOF
}
while getopts "c:j:Btmzh" opt; do
    case "$opt" in
        c) HWCONFIG_ARG="$OPTARG" ;;
        j) JOBS="$OPTARG" ;;
        B) BRANCH_MODE=1; TAG_MODE=0 ;;
        m) MAIN_MODE=1; TAG_MODE=0 ;;
        t) TAG_MODE=1 ;;   # accepted for compatibility: tag mode is now the default
        z) PACK_ZIP=1 ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 1 ;;
    esac
done
[[ "$JOBS" =~ ^[0-9]+$ ]] && (( JOBS >= 1 )) || { echo "ERROR: -j must be a positive integer" >&2; exit 1; }
(( BRANCH_MODE && MAIN_MODE )) && { echo "ERROR: -B and -m are mutually exclusive" >&2; exit 1; }
if (( PACK_ZIP )); then
    (( TAG_MODE )) || { echo "ERROR: -z requires the default tag mode (not -B/-m): a release bundle must be built from release tags" >&2; exit 1; }
    [ "$HWCONFIG_ARG" = "all" ] || { echo "ERROR: -z requires -c all: a zip built from a single hwconfig would be missing every other board" >&2; exit 1; }
fi

cd "$(dirname "$0")"
LOADER_DIR="$(pwd)"
# Keep the clone+build tree OUTSIDE this repo so it doesn't pollute git status.
BUILD_DIR="$(dirname "$LOADER_DIR")/emu-build"
GITHUB_OWNER="fhoedemakers"

NPROC=$(nproc)
PER_PROC=$(( NPROC / JOBS ))
(( PER_PROC < 1 )) && PER_PROC=1

# prog_name -> GitHub repo name. Source of truth: workflow file.
declare -A REPO_OF=(
    [piconesPlus]=pico-infonesPlus
    [picogenesisPlus]=pico-genesisPlus
    [picopcePlus]=pico-pcePlus
    [PicoPeanutGB]=pico-peanutGB
    [picosmsPlus]=pico-smsplus
    [picoPacPlus]=pico-pacPlus
    [picosnesPlus]=pico-snesPlus
    [doom_tiny]=pico-doom
    [doom_tiny_full]=pico-doom
)

# pico-doom (Doom!) is a special case. Unlike the emulators above it targets only
# a handful of specific boards, carries no pico_shared (so there is no SWVERSION
# to stamp), and uses a per-board build script instead of bld.sh. It is built by
# _build_doom().
#
# Two variants ship from the same repo, and since the full-version branch they
# ship from the same *ref* — that branch carries the -build-forbootloader.sh and
# -build-full-forbootloader.sh families for all four boards, where main still has
# only the shareware half:
#   doom_tiny       shareware, WHX baked into flash as a companion DATA UF2
#                   (the emulators.txt aux_uf2 column)
#   doom_tiny_full  registered/Ultimate DOOM; nothing extra in flash — at boot
#                   the game copies /roms/doom/doom.whd from the SD card into
#                   PSRAM, so the board must have PSRAM
#
# Each variant still gets its own clone directory. Not to avoid branch thrashing
# any more (they share a ref now) but because with -j >1 both variants build at
# once, and one shared checkout would have two jobs fetching and checking it out
# concurrently. That is cheap now: pico-doom no longer vendors the SDK,
# pico-extras or Pico-PIO-USB as submodules — they come from the environment via
# its pico-env.sh, so a clone is small.
DOOM_REPO="pico-doom"
# The branch to build when the repo has no release tag yet. Once pico-doom is
# tagged, tag mode picks the tag up automatically (see resolve_refs) and this is
# only the fallback.
declare -A DOOM_BRANCH=(
    [doom_tiny]=full-version
    [doom_tiny_full]=full-version
)
declare -A DOOM_CLONE_OF=(
    [doom_tiny]=pico-doom
    [doom_tiny_full]=pico-doom-full
)
# The git ref each variant is actually cloned at — a tag when one exists, else
# the DOOM_BRANCH above. Filled in by resolve_refs; REF_OF keeps the display and
# manifest form, which for a branch build carries the sha too.
declare -A DOOM_CLONE_REF=()
# Per-variant build script and build tree, as printf formats over the board tag.
declare -A DOOM_SCRIPT_FMT=(
    [doom_tiny]="%s-build-forbootloader.sh"
    [doom_tiny_full]="%s-build-full-forbootloader.sh"
)
declare -A DOOM_BUILD_FMT=(
    [doom_tiny]="build_bl_%s"
    [doom_tiny_full]="build_bl_full_%s"
)
# Companion DATA-family WAD UF2 the variant must produce (emulators.txt aux_uf2
# column), or "" when it needs none. Every per-board build script that emits a
# WAD emits it under exactly this name.
declare -A DOOM_WAD_OF=(
    [doom_tiny]="doom1-whx.uf2"
    [doom_tiny_full]=""
)
# HW_CONFIG -> board "tag" used by pico-doom's per-board build scripts, for
# both variants. Each tag <T> implies, for a variant with script format <SF>,
# build format <BF> and program name <P>:
#                   build script   printf <SF> <T>
#                   build output    printf <BF> <T>/src/<P>.uf2   (app)
#                   WAD produced    printf <BF> <T>/src/${DOOM_WAD_OF[<P>]}
# The keys are exactly the configs Doom supports; any other config is skipped.
declare -A DOOM_TAG=(
    [2]=adafruitdvisd    # Adafruit DVI + MicroSD breakouts
    [8]=fruitjam         # Adafruit Fruit Jam
    [13]=murmulatorm2    # Murmulator M2
    [14]=featherrp2350   # Adafruit Feather RP2350 + TLV320DAC3100
)

# Supported RP2350-ARM hwconfigs + descriptors from pico_shared/bld.sh case
# statement. Configs 1, 2, 6, 11 are not Pico-2-only at the board level, but
# we always pass -2 to bld.sh so they build for RP2350.
HWCONFIGS=(1 2 5 6 7 8 9 13 14)
declare -A HW_DESC=(
    [1]="Pimoroni Pico DV Demo Base"
    [2]="Adafruit DVI + MicroSD breakouts / custom PCB"
    [5]="Adafruit Metro RP2350"
    [6]="WaveShare RP2350-Zero with custom PCB"
    [7]="WaveShare RP2350-PiZero (PIO USB)"
    [8]="Adafruit Fruit Jam (PIO USB)"
    [9]="WaveShare RP2350-USBA (PIO USB)"
    [13]="Murmulator M2"
    [14]="Adafruit Feather RP2350 + TLV320DAC3100 (PIO USB)"
)
# Hwconfigs that imply PIO USB (mirrors pico_shared/bld.sh).
PIOUSB_CONFIGS=(7 8 9 14)

# prog_name -> space-separated HW_CONFIGs it must NOT be built for.
declare -A EXCLUDE_HWCONFIGS=(
    [picogenesisPlus]="7"
)

die()  { echo "ERROR: $*" >&2; exit 1; }
warn() { echo "WARN:  $*" >&2; }
info() { echo "==> $*"; }
step() { echo "    -> $*"; }
hr()   { echo "=============================================================="; }

# Resolve a remote's default branch — the ref its HEAD points at (e.g. "main"
# or "master"). Prints the bare branch name, or nothing if it can't be found.
# Used by -m so repos that call their default branch "master" still build.
default_branch_of() {
    local url="$1"
    git ls-remote --symref "$url" HEAD 2>/dev/null \
        | awk '$1 == "ref:" { sub(/^refs\/heads\//, "", $2); print $2; exit }'
}

# Resolve a remote's newest release tag — the same selection the release
# workflow makes. Prints the bare tag name, or nothing when the repo has no tags
# or cannot be reached. Captures first instead of piping into head: under
# `set -o pipefail` a large tag list would give git SIGPIPE and fail the caller.
latest_tag_of() {
    local url="$1" out
    out=$(git ls-remote --tags --refs --sort=-v:refname "$url" 2>/dev/null) || return 0
    printf '%s\n' "$out" | sed -n '1s#.*refs/tags/##p'
}

# Stamp a version into the emulator's OWN pico_shared/menu.h, exactly as each
# emulator repo's release workflow does, so the built UF2 reports that version
# instead of falling back to the build date (see pico_shared/menu.cpp
# getVersionString(), which special-cases the "VX.X" placeholder).
#
# Must be called with the cwd inside the emulator's clone. Anchored on the
# #define rather than on the "VX.X" placeholder text, so it stays idempotent and
# keeps working if the placeholder is ever renamed. Repos without pico_shared
# (pico-doom) are a no-op.
#
# Deliberately NOT touching pico_set_program_version() in the emulator's
# CMakeLists.txt: the bootloader reads only the program *name* from binary_info,
# never a version, and the emulator repos don't spell that call uniformly.
stamp_swversion() {
    local ver="$1"
    [ -f pico_shared/menu.h ] || return 0
    sed -i -E "s|^#define[[:space:]]+SWVERSION[[:space:]]+\".*\"|#define SWVERSION \"${ver}\"|" pico_shared/menu.h
    step "stamped $(grep -m1 SWVERSION pico_shared/menu.h)"
}

human_size() {
    local bytes="$1"
    if (( bytes >= 1024*1024 )); then
        printf "%.1f MiB" "$(echo "$bytes 1048576" | awk '{print $1/$2}')"
    elif (( bytes >= 1024 )); then
        printf "%.1f KiB" "$(echo "$bytes 1024" | awk '{print $1/$2}')"
    else
        printf "%d B" "$bytes"
    fi
}

human_time() {
    local s="$1"
    if (( s >= 60 )); then
        printf "%dm%02ds" "$((s/60))" "$((s%60))"
    else
        printf "%ds" "$s"
    fi
}

# --- Pre-flight ---------------------------------------------------------------
info "Environment check"
command -v git >/dev/null 2>&1 || die "git not found in PATH"
step "git           : $(git --version | head -1)"
[ -n "${PICO_SDK_PATH:-}" ] || die "PICO_SDK_PATH is not set"
[ -d "$PICO_SDK_PATH" ]     || die "PICO_SDK_PATH ($PICO_SDK_PATH) is not a directory"
step "PICO_SDK_PATH : $PICO_SDK_PATH"
step "PICO_PIO_USB  : ${PICO_PIO_USB_PATH:-(unset)}"
step "PICO_EXTRAS   : ${PICO_EXTRAS_PATH:-(unset)}  (pico-doom only)"
[ -f "$LOADER_DIR/emu/emulators.txt" ] || die "uf2/emulators.txt not found (run from pico-bootLoader root)"
step "loader dir    : $LOADER_DIR"
step "build tree    : $BUILD_DIR  (kept outside the repo)"
step "host cores    : $NPROC"

# --- Load prog_names from emulators.txt --------------------------------------
PROG_NAMES=()
# The `|| [ -n "$prog" ]` keeps the final line even when emulators.txt has no
# trailing newline (otherwise read returns 1 and the body is skipped).
while IFS=';' read -r prog _rest || [ -n "${prog:-}" ]; do
    [ -z "${prog:-}" ] && continue
    [[ "$prog" == \#* ]] && continue
    if [ -z "${REPO_OF[$prog]+x}" ]; then
        warn "unknown prog_name '$prog' in emulators.txt (no repo mapping) — skipping"
        continue
    fi
    PROG_NAMES+=("$prog")
done < "$LOADER_DIR/emu/emulators.txt"

[ ${#PROG_NAMES[@]} -gt 0 ] || die "no buildable emulators found in uf2/emulators.txt"
echo
info "Emulators discovered in uf2/emulators.txt (${#PROG_NAMES[@]}):"
for prog in "${PROG_NAMES[@]}"; do
    step "$prog  ->  https://github.com/${GITHUB_OWNER}/${REPO_OF[$prog]}.git"
done

# --- Determine hwconfig(s) to build ------------------------------------------
# pico-doom sources its toolchain from the environment (its pico-env.sh) and
# needs pico-extras; the bld.sh emulators do not. Mirrors piousb_available().
doom_extras_available() { [ -n "${PICO_EXTRAS_PATH:-}" ] && [ -f "${PICO_EXTRAS_PATH}/external/pico_extras_import.cmake" ]; }

is_known_hwconfig() { local hw="$1" h; for h in "${HWCONFIGS[@]}"; do [ "$h" = "$hw" ] && return 0; done; return 1; }
hwconfig_needs_piousb() { local hw="$1" c; for c in "${PIOUSB_CONFIGS[@]}"; do [ "$c" = "$hw" ] && return 0; done; return 1; }
piousb_available() { [ -n "${PICO_PIO_USB_PATH:-}" ] && [ -r "${PICO_PIO_USB_PATH}/src/pio_usb.h" ]; }

# If -c wasn't given, ask interactively (accepting a number or 'all').
if [ -z "$HWCONFIG_ARG" ]; then
    echo
    echo "Available hardware configurations:"
    for hw in "${HWCONFIGS[@]}"; do
        printf "  HW_CONFIG=%-2s  %s\n" "$hw" "${HW_DESC[$hw]}"
    done
    echo
    read -r -p "Pick a HW_CONFIG number (e.g. ${HWCONFIGS[0]}, ${HWCONFIGS[2]}, ${HWCONFIGS[5]}), or 'all': " HWCONFIG_ARG
fi

HWCONFIGS_TO_BUILD=()
MULTI=0
if [ "$HWCONFIG_ARG" = "all" ]; then
    MULTI=1
    for hw in "${HWCONFIGS[@]}"; do
        if hwconfig_needs_piousb "$hw" && ! piousb_available; then
            warn "skipping HW_CONFIG=$hw (${HW_DESC[$hw]}): needs PIO USB but PICO_PIO_USB_PATH is unset/unusable"
            continue
        fi
        HWCONFIGS_TO_BUILD+=("$hw")
    done
    [ ${#HWCONFIGS_TO_BUILD[@]} -gt 0 ] || die "no buildable hwconfigs (every supported config needs PIO USB; set PICO_PIO_USB_PATH)"
    info "Building ALL hwconfigs: ${HWCONFIGS_TO_BUILD[*]}"
else
    is_known_hwconfig "$HWCONFIG_ARG" || die "invalid HW_CONFIG: '$HWCONFIG_ARG' (must be one of: ${HWCONFIGS[*]}, or 'all')"
    # Explicitly-targeted config: hard-fail if it needs PIO USB and it's missing.
    if hwconfig_needs_piousb "$HWCONFIG_ARG" && ! piousb_available; then
        die "HW_CONFIG=$HWCONFIG_ARG requires PIO USB; set PICO_PIO_USB_PATH to a Pico-PIO-USB checkout"
    fi
    HWCONFIGS_TO_BUILD=("$HWCONFIG_ARG")
    info "Selected HW_CONFIG=$HWCONFIG_ARG (${HW_DESC[$HWCONFIG_ARG]})"
fi

# --- Branch picker helper ----------------------------------------------------
# Usage: pick_branch <prompt-label> <repo-url> <default-branch>
# Prints chosen branch on stdout; reads from /dev/tty so it stays interactive
# even though command substitution captures stdout.
pick_branch() {
    local label="$1" url="$2" default="$3"
    local branches=() line ref
    echo >&2
    echo "Fetching branches for $label ($url) ..." >&2
    while IFS=$'\t' read -r _sha ref; do
        branches+=("${ref#refs/heads/}")
    done < <(git ls-remote --heads "$url")
    [ ${#branches[@]} -gt 0 ] || { echo "(no branches found — falling back to free-text)" >&2; }

    local default_idx=0
    if [ ${#branches[@]} -gt 0 ]; then
        echo "Available branches for $label:" >&2
        for i in "${!branches[@]}"; do
            local mark="  "
            if [ "${branches[$i]}" = "$default" ]; then
                mark="* "
                default_idx=$((i+1))
            fi
            printf "%s%2d) %s\n" "$mark" "$((i+1))" "${branches[$i]}" >&2
        done
    fi

    local prompt
    if (( default_idx > 0 )); then
        prompt="Pick branch for $label [number, or name, default=${default}]: "
    else
        prompt="Pick branch for $label [number, or name]: "
    fi

    local reply
    read -r -p "$prompt" reply </dev/tty
    if [ -z "$reply" ]; then
        if (( default_idx > 0 )); then
            echo "$default"
            return
        fi
        die "no branch chosen and no default available"
    fi
    if [[ "$reply" =~ ^[0-9]+$ ]] && (( reply >= 1 && reply <= ${#branches[@]} )); then
        echo "${branches[$((reply-1))]}"
        return
    fi
    echo "$reply"
}

# --- Ask: emulator + pico_shared branches (skipped in tag mode) --------------
if (( TAG_MODE )); then
    info "Tag mode (default): each emulator repo builds its latest release tag,"
    info "          with the tag stamped into its SWVERSION."
    EMU_BRANCH="(latest tag per repo)"
    # NOT the revision the tag pins: bld.sh gained -b (BUILD_FOR_BOOTLOADER) only
    # in pico_shared f2c8be9, and the emulator release tags predate it, so their
    # pinned pico_shared rejects -b and no bootloader UF2 can be built from it.
    SHARED_BRANCH="main"
    info "          pico_shared is switched to '$SHARED_BRANCH': the revisions the tags"
    info "          pin predate bld.sh's -b flag, so they cannot build for the loader."
elif (( MAIN_MODE )); then
    info "Main mode: building each repo's default branch (main or master),"
    info "          resolved per repo from the remote's HEAD."
    # Emulator repos are resolved individually in build_one_emulator (they may
    # not all use the same default-branch name); this is just a display label.
    EMU_BRANCH="(default branch per repo)"
    # pico_shared is a single repo — resolve its default branch now so the
    # submodule switch and the summaries use a concrete name.
    SHARED_BRANCH="$(default_branch_of "https://github.com/${GITHUB_OWNER}/pico_shared.git")"
    if [ -z "$SHARED_BRANCH" ]; then
        warn "could not resolve pico_shared default branch; falling back to 'main'"
        SHARED_BRANCH="main"
    fi
    info "pico_shared default branch: $SHARED_BRANCH"
else
    # --- Ask: emulator branch (probed against first repo) --------------------
    PROBE_REPO="${REPO_OF[${PROG_NAMES[0]}]}"
    EMU_BRANCH="$(pick_branch "emulator repos" "https://github.com/${GITHUB_OWNER}/${PROBE_REPO}.git" "bootloader")"
    info "Emulator branch: $EMU_BRANCH"

    # --- Ask: pico_shared branch ---------------------------------------------
    SHARED_BRANCH="$(pick_branch "pico_shared" "https://github.com/${GITHUB_OWNER}/pico_shared.git" "main")"
    info "pico_shared branch: $SHARED_BRANCH"
fi

# --- Resolve the ref to build for every emulator, ONCE ------------------------
# prog_name -> the ref actually built: a tag in tag mode, a branch name in
# -B/-m, or "<branch>@<shortsha>" when a repo has no tag to build yet — which is
# pico-doom's situation today, so its sha is the only version string available.
# Empty means "no ref could be resolved" — build_one_emulator turns that into a
# SKIP.
#
# Resolving up front rather than inside build_one_emulator matters for more than
# tidiness: with -c all the same repo would otherwise be probed once per
# hwconfig, so a tag pushed mid-run could put v0.43 on one board and v0.44 on
# another *within the same zip*. One resolution per repo also collapses 9x9
# ls-remote round-trips to 9, and REF_OF is exactly the manifest -z writes.
#
# Populated in the parent shell before the hwconfig loop forks its -j>1
# subshells, so children inherit it; nothing writes back.
declare -A REF_OF=()
# The pico_shared revision every emulator is built against, for the manifest.
# One value for the whole run: every emulator gets the same $SHARED_BRANCH tip.
SHARED_SHA=""
resolve_refs() {
    local prog repo url ref
    echo
    info "Resolving refs to build (${#PROG_NAMES[@]} repo(s))"
    SHARED_SHA=$(git ls-remote --heads \
        "https://github.com/${GITHUB_OWNER}/pico_shared.git" "$SHARED_BRANCH" 2>/dev/null \
        | cut -c1-7) || SHARED_SHA=""
    step "$(printf '%-16s %-20s %s' "pico_shared" "pico_shared" "${SHARED_BRANCH}${SHARED_SHA:+ @ $SHARED_SHA}")"
    for prog in "${PROG_NAMES[@]}"; do
        repo="${REPO_OF[$prog]}"
        url="https://github.com/${GITHUB_OWNER}/${repo}.git"
        ref=""
        if [ -n "${DOOM_BRANCH[$prog]+x}" ]; then
            # Doom builds from a tag when the repo has one — it uses the same
            # v*.* convention as the emulators — and otherwise from its branch,
            # where branch@shortsha is the only version string available.
            local branch="${DOOM_BRANCH[$prog]}" tag="" sha=""
            if (( TAG_MODE )); then
                tag="$(latest_tag_of "$url")"
            fi
            if [ -n "$tag" ]; then
                ref="$tag"
                DOOM_CLONE_REF[$prog]="$tag"
            else
                sha=$(git ls-remote --heads "$url" "$branch" 2>/dev/null | cut -c1-7) || sha=""
                if [ -n "$sha" ]; then
                    ref="${branch}@${sha}"
                    DOOM_CLONE_REF[$prog]="$branch"
                    (( TAG_MODE )) && warn "[$prog] $repo has no tags yet — building branch '$branch'"
                else
                    warn "[$prog] could not resolve $repo branch '$branch'"
                fi
            fi
        elif (( TAG_MODE )); then
            ref="$(latest_tag_of "$url")"
            [ -n "$ref" ] || warn "[$prog] no tags found in $repo"
        elif (( MAIN_MODE )); then
            ref="$(default_branch_of "$url" || true)"
            [ -n "$ref" ] || warn "[$prog] could not resolve default branch for $repo"
        else
            ref="$EMU_BRANCH"
        fi
        REF_OF[$prog]="$ref"
        step "$(printf '%-16s %-20s %s' "$prog" "$repo" "${ref:-(unresolved)}")"
    done
}
resolve_refs

# --- Build pico-doom (Doom!) --------------------------------------------------
# Handles both variants (doom_tiny, doom_tiny_full) — the ref, build script,
# build tree and companion WAD all come from the DOOM_* tables above. Builds for
# whichever HW_CONFIGs appear in DOOM_TAG (2, 8, 13, 14); any other config is
# skipped cleanly. Clones the resolved ref (a release tag when pico-doom has one,
# otherwise DOOM_BRANCH) and runs its per-board build script, which emits the app
# UF2 (and, for doom_tiny, a DATA-family WAD UF2) in <build tree>/src/. Installs
# the app as emu/<hw>/<prog>.uf2 and the WAD, if any, as emu/<hw>/<wad>, then
# writes the same status contract as build_one_emulator(). The clone is reused
# across configs within one run so the four boards share one checkout.
#
# pico-doom takes its toolchain from the environment (pico-env.sh): PICO_SDK_PATH,
# PICO_EXTRAS_PATH, and PICO_PIO_USB_PATH for the PIO-USB boards. PICO_EXTRAS_PATH
# is checked before we get here, in the pre-flight.
_build_doom() {
    local prog="$1" status_file="$2" t0="$3"
    local repo="$DOOM_REPO" branch="${DOOM_BRANCH[$prog]}"
    # The git ref to check out: a tag if pico-doom has one, else the branch.
    local cloneref="${DOOM_CLONE_REF[$prog]:-$branch}"
    local dest="$BUILD_DIR/${DOOM_CLONE_OF[$prog]}"
    local url="https://github.com/${GITHUB_OWNER}/${repo}.git"
    local wad="${DOOM_WAD_OF[$prog]:-}"
    local elapsed

    # Only the configs with a per-board build script get a Doom build. Note this
    # "board" is pico-doom's own board tag (fruitjam, adafruitdvisd, ...), which
    # names its script and build tree — nothing to do with a git tag.
    local board="${DOOM_TAG[$HWCONFIG]:-}"
    if [ -z "$board" ]; then
        elapsed=$(( SECONDS - t0 ))
        echo "SKIP:$prog not supported for HW_CONFIG=$HWCONFIG (Doom builds for: ${!DOOM_TAG[*]})|$elapsed" > "$status_file"
        return 0
    fi

    # pico-doom's pico-env.sh requires pico-extras; the emulators do not, so this
    # is a Doom-only skip rather than a pre-flight die. Catch it here instead of
    # letting the build script fail a minute in with its own error.
    if ! doom_extras_available; then
        elapsed=$(( SECONDS - t0 ))
        echo "SKIP:PICO_EXTRAS_PATH unset or not a pico-extras checkout (pico-doom needs it)|$elapsed" > "$status_file"
        return 0
    fi
    local build_script build_subdir
    printf -v build_script "${DOOM_SCRIPT_FMT[$prog]}" "$board"
    printf -v build_subdir "${DOOM_BUILD_FMT[$prog]}" "$board"

    hr
    echo " $prog  ($repo @ ${REF_OF[$prog]:-$cloneref})  [HW_CONFIG $HWCONFIG / $board]"
    echo " clone -> $dest"
    hr

    # Drop any artifacts from a previous run up front, so a failed rebuild can
    # never leave a stale <prog>.uf2 (or its WAD) behind in emu/$HWCONFIG/. Only
    # touch this variant's files: with -j >1 the other variant may be building
    # into the same directory right now.
    local out_dir="$LOADER_DIR/emu/$HWCONFIG"
    mkdir -p "$out_dir"
    info "[$prog] removing any previous emu/$HWCONFIG/ $prog artifacts"
    rm -f "$out_dir/${prog}.uf2"
    if [ -n "$wad" ]; then
        rm -f "$out_dir/$wad"
        # Also sweep the old per-board WAD name emulators.txt used to declare, so
        # a card refreshed from an earlier build doesn't keep an orphan around.
        shopt -s nullglob
        local stale
        for stale in "$out_dir"/doom1-whx-for-*.uf2; do rm -f "$stale"; done
        shopt -u nullglob
    fi

    # Reuse an existing checkout of the right repo across configs: clone once,
    # then move it to the wanted ref for later boards. Detaching works for both a
    # tag and a branch tip, so the same path serves either. If the update fails
    # for any reason, fall back to a clean clone.
    if [ -d "$dest/.git" ] && \
       [ "$(git -C "$dest" config --get remote.origin.url 2>/dev/null)" = "$url" ]; then
        info "[$prog] reusing existing clone at $dest; updating to ${cloneref}"
        if ! ( cd "$dest" \
                && git fetch --depth 1 origin "$cloneref" \
                && git checkout -q --detach FETCH_HEAD \
                && git submodule update --init --recursive ); then
            warn "[$prog] could not update existing clone; re-cloning from scratch"
            rm -rf "$dest"
        fi
    fi
    if [ ! -d "$dest/.git" ]; then
        info "[$prog] cleaning previous clone (if any)"
        rm -rf "$dest"
        info "[$prog] cloning ${repo}@${cloneref}"
        if ! git clone --branch "$cloneref" --recurse-submodules --depth 1 "$url" "$dest"; then
            elapsed=$(( SECONDS - t0 ))
            echo "FAIL:clone of ${repo}@${cloneref} failed|$elapsed" > "$status_file"
            return 0
        fi
    fi
    # Force a clean build tree for THIS board+variant so a prior run can't leave
    # stale objects behind (each combination has its own build tree).
    rm -rf "$dest/$build_subdir"

    # Guard against a ref that doesn't carry this variant's scripts. pico-doom's
    # main, for one, still has only the shareware -build-forbootloader.sh family;
    # both variants live on full-version.
    if [ ! -f "$dest/$build_script" ]; then
        elapsed=$(( SECONDS - t0 ))
        echo "FAIL:$build_script not found in ${repo}@${cloneref}|$elapsed" > "$status_file"
        return 0
    fi

    set +e
    (
        set -e
        cd "$dest"
        chmod +x "$build_script" 2>/dev/null || true
        info "[$prog] running ./$build_script"
        "./$build_script"
    )
    local rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        elapsed=$(( SECONDS - t0 ))
        echo "FAIL:build failed (rc=$rc)|$elapsed" > "$status_file"
        return 0
    fi

    info "[$prog] locating produced UF2s"
    local src_dir="$dest/$build_subdir/src"
    local app_uf2="$src_dir/${prog}.uf2"
    # -s, not -f: this build has been observed to leave a 0-byte doom_tiny.uf2
    # behind, which a plain -f test happily reported as BUILT and shipped.
    if [ ! -s "$app_uf2" ]; then
        elapsed=$(( SECONDS - t0 ))
        echo "MISSING|$elapsed" > "$status_file"
        [ -f "$app_uf2" ] && warn "[$prog] $(basename "$app_uf2") is empty (0 bytes) — not installing"
        return 0
    fi

    # Companion DATA-family WAD (emulators.txt aux_uf2), for the variants that
    # bake one into flash. Those variants are unplayable without it, so a missing
    # WAD is a build failure. doom_tiny_full has none: it reads the full WHD off
    # the SD card at boot.
    local aux_uf2=""
    if [ -n "$wad" ]; then
        aux_uf2="$src_dir/$wad"
        if [ ! -s "$aux_uf2" ]; then
            elapsed=$(( SECONDS - t0 ))
            echo "FAIL:WAD $wad not produced by $build_script (missing or empty)|$elapsed" > "$status_file"
            return 0
        fi
    fi

    cp "$app_uf2" "$out_dir/${prog}.uf2"
    if [ -n "$wad" ]; then
        cp "$aux_uf2" "$out_dir/$wad"
        info "[$prog] installed WAD $(basename "$aux_uf2") -> emu/$HWCONFIG/$wad"
    else
        info "[$prog] no companion WAD: reads /roms/doom/doom.whd off the SD card at boot"
    fi

    local bytes
    bytes=$(stat -c%s "$out_dir/${prog}.uf2")
    info "[$prog] installed ${prog}.uf2 -> emu/$HWCONFIG/${prog}.uf2  ($(human_size "$bytes"))"
    elapsed=$(( SECONDS - t0 ))
    echo "BUILT:${prog}.uf2|$bytes|$elapsed" > "$status_file"
}

# --- Build one emulator (used both serially and in parallel) -----------------
# Writes a one-line status to $STATUS_DIR/<prog>.status:
#   BUILT:<basename>|<bytes>|<elapsed_s>   built OK, UF2 installed
#   SKIP:<reason>|<elapsed_s>              intentionally not built (branch/config N/A)
#   FAIL:<reason>|<elapsed_s>              genuine clone/build failure
#   MISSING|<elapsed_s>                    build ran but produced no UF2
build_one_emulator() {
    local prog="$1"
    local status_file="$STATUS_DIR/$prog.status"
    local t0 elapsed
    t0=$SECONDS

    # pico-doom builds nothing like the others (a few specific boards,
    # vendored SDK, its own script, extra WAD UF2). Hand it off entirely.
    if [ -n "${DOOM_BRANCH[$prog]+x}" ]; then
        _build_doom "$prog" "$status_file" "$t0"
        return 0
    fi

    # Some emulators don't work on certain boards; skip those combinations and
    # drop any stale UF2 so an excluded config can't keep shipping an old one.
    local excluded
    for excluded in ${EXCLUDE_HWCONFIGS[$prog]:-}; do
        if [ "$excluded" = "$HWCONFIG" ]; then
            rm -f "$LOADER_DIR/emu/$HWCONFIG/${prog}.uf2"
            elapsed=$(( SECONDS - t0 ))
            echo "SKIP:excluded for HW_CONFIG=$HWCONFIG (${HW_DESC[$HWCONFIG]})|$elapsed" > "$status_file"
            return 0
        fi
    done

    local repo="${REPO_OF[$prog]}"
    local dest="$BUILD_DIR/$repo"
    local url="https://github.com/${GITHUB_OWNER}/${repo}.git"

    # The ref was resolved once for every repo by resolve_refs() before the
    # hwconfig loop started, so every board in a -c all run builds the same
    # revision even if a tag lands mid-run.
    local ref="${REF_OF[$prog]:-}" ref_desc
    if [ -z "$ref" ]; then
        elapsed=$(( SECONDS - t0 ))
        if (( TAG_MODE )); then
            echo "SKIP:no tags found in $repo|$elapsed" > "$status_file"
        else
            echo "FAIL:could not resolve a ref for $repo|$elapsed" > "$status_file"
        fi
        return 0
    fi
    if (( TAG_MODE )); then
        ref_desc="tag $ref (submodules pinned in tag)"
    elif (( MAIN_MODE )); then
        ref_desc="$ref (default branch), pico_shared @ $SHARED_BRANCH"
    else
        ref_desc="$ref, pico_shared @ $SHARED_BRANCH"
    fi

    hr
    echo " $prog  ($repo @ $ref_desc)"
    echo " clone -> $dest"
    echo " using $PER_PROC build processors"
    hr

    # Drop any artifact from a previous run up front, so a failed rebuild can
    # never leave a stale emu/$HWCONFIG/${prog}.uf2 behind.
    local dest_uf2="$LOADER_DIR/emu/$HWCONFIG/${prog}.uf2"
    if [ -f "$dest_uf2" ]; then
        info "[$prog] removing previous emu/$HWCONFIG/${prog}.uf2"
        rm -f "$dest_uf2"
    fi

    info "[$prog] cleaning previous clone (if any)"
    rm -rf "$dest"

    info "[$prog] cloning ${repo}@${ref}"
    # Tag mode: recurse submodules so pico_shared et al. land at the exact
    # revisions pinned in the tag. Branch mode: skip them here and switch
    # pico_shared to the chosen branch below.
    local recurse_flag="--no-recurse-submodules"
    (( TAG_MODE )) && recurse_flag="--recurse-submodules"
    if ! git clone --branch "$ref" "$recurse_flag" --depth 1 "$url" "$dest"; then
        elapsed=$(( SECONDS - t0 ))
        # In -m/-t we resolved $ref from the remote, so a clone failure is a real
        # error. In interactive branch mode the chosen branch may legitimately
        # not exist in every repo, so that stays a (non-fatal) skip.
        if (( MAIN_MODE || TAG_MODE )); then
            echo "FAIL:clone of '$ref' failed for $repo|$elapsed" > "$status_file"
        else
            echo "SKIP:no '$ref' branch in $repo|$elapsed" > "$status_file"
        fi
        return 0
    fi

    set +e
    (
        set -e
        cd "$dest"
        if (( TAG_MODE )); then
            # --recurse-submodules already checked them out at the tag's pins.
            # Everything except pico_shared stays there.
            info "[$prog] submodules pinned by tag"
        else
            info "[$prog] initialising submodules (pinned revisions)"
            git submodule update --init --recursive
        fi
        # pico_shared moves to $SHARED_BRANCH in every mode. In tag mode that is
        # not cosmetic: the revision the tag pins has no -b in bld.sh, so leaving
        # it there makes a bootloader build impossible (see the header comment).
        if [ -d pico_shared ]; then
            info "[$prog] switching pico_shared submodule to '${SHARED_BRANCH}'"
            (
                cd pico_shared
                if ! git fetch --depth 1 origin "$SHARED_BRANCH"; then
                    exit 73
                fi
                git checkout --detach FETCH_HEAD
                echo "    -> pico_shared now at $(git rev-parse --short HEAD)"
            ) || exit 73
        fi
        # Report the tag on the emulator's own menu screen instead of the build
        # date. Tag mode only: -B/-m have no version to stamp, and leaving the
        # placeholder is what marks those as dev builds. Must come after the
        # pico_shared checkout above, which would otherwise refuse to overwrite a
        # locally modified menu.h.
        if (( TAG_MODE )); then
            info "[$prog] stamping SWVERSION = $ref"
            stamp_swversion "$ref"
        fi
        chmod +x build*.sh bld.sh pico_shared/bld.sh 2>/dev/null || true
        info "[$prog] running ./bld.sh -c $HWCONFIG -2 -b -p $PER_PROC"
        ./bld.sh -c "$HWCONFIG" -2 -b -p "$PER_PROC"
    )
    local rc=$?
    set -e
    elapsed=$(( SECONDS - t0 ))
    if [ "$rc" -eq 73 ]; then
        echo "SKIP:pico_shared lacks branch '$SHARED_BRANCH'|$elapsed" > "$status_file"
        return 0
    elif [ "$rc" -ne 0 ]; then
        echo "FAIL:build failed (rc=$rc)|$elapsed" > "$status_file"
        return 0
    fi

    info "[$prog] locating produced UF2"
    shopt -s nullglob
    local matches=("$dest"/releases_bl/"${prog}"_*_bl.uf2)
    if [ ${#matches[@]} -eq 0 ]; then
        matches=("$dest"/releases_bl/"${prog}"_*.uf2)
    fi
    shopt -u nullglob
    if [ ${#matches[@]} -eq 0 ]; then
        elapsed=$(( SECONDS - t0 ))
        echo "MISSING|$elapsed" > "$status_file"
        return 0
    fi
    local src_uf2="${matches[0]}"
    # -s, not -f: an interrupted or failed link step can leave a 0-byte UF2
    # behind, and shipping that in the zip looks like a successful build until
    # the board refuses to boot.
    if [ ! -s "$src_uf2" ]; then
        elapsed=$(( SECONDS - t0 ))
        echo "MISSING|$elapsed" > "$status_file"
        warn "[$prog] $(basename "$src_uf2") is empty (0 bytes) — not installing"
        return 0
    fi
    cp "$src_uf2" "$dest_uf2"
    local bytes
    bytes=$(stat -c%s "$dest_uf2")
    info "[$prog] installed $(basename "$src_uf2") -> uf2/$HWCONFIG/${prog}.uf2  ($(human_size "$bytes"))"
    elapsed=$(( SECONDS - t0 ))
    echo "BUILT:$(basename "$src_uf2")|$bytes|$elapsed" > "$status_file"
}

# On a failed/missing build, surface the tail of that emulator's log so the
# error is visible without hunting for it. Only parallel mode (JOBS>1) writes a
# per-emulator log; serial mode already streamed the output live, so there is
# nothing to replay here.
show_log_tail() {
    local prog="$1" logf="${LOGS_DIR:-}/$prog.log"
    [ -n "${LOGS_DIR:-}" ] && [ -f "$logf" ] || return 0
    echo "         ---- last 25 lines of $logf ----"
    tail -n 25 "$logf" | sed 's/^/         > /'
    echo "         ---- (full log above; keep for details) ----"
}

# --- Pretty-print a finished emulator using its status file ------------------
# Reads DONE_COUNT / TOTAL / STATUS_DIR / LOGS_DIR from the calling
# build_for_hwconfig (visible via bash dynamic scope).
report_completion() {
    local prog="$1"
    DONE_COUNT=$(( DONE_COUNT + 1 ))
    local status_file="$STATUS_DIR/$prog.status"
    if [ ! -r "$status_file" ]; then
        echo "    [$DONE_COUNT/$TOTAL] ??? $prog (no status file)"
        return
    fi
    local raw="$(cat "$status_file")"
    local body="${raw%%|*}"
    local rest="${raw#*|}"
    case "$body" in
        BUILT:*)
            local fname="${body#BUILT:}"
            local bytes="${rest%%|*}"
            local elapsed="${rest#*|}"
            echo "    [$DONE_COUNT/$TOTAL] OK   $prog  ($(human_size "$bytes"), $(human_time "$elapsed"))  ${fname}"
            ;;
        SKIP:*)
            local reason="${body#SKIP:}"
            local elapsed="$rest"
            echo "    [$DONE_COUNT/$TOTAL] SKIP $prog  ($(human_time "$elapsed"))  $reason"
            ;;
        FAIL:*)
            local reason="${body#FAIL:}"
            local elapsed="$rest"
            echo "    [$DONE_COUNT/$TOTAL] FAIL $prog  ($(human_time "$elapsed"))  $reason"
            show_log_tail "$prog"
            ;;
        MISSING)
            echo "    [$DONE_COUNT/$TOTAL] MISS $prog  ($(human_time "$rest"))  build succeeded but no UF2 found"
            show_log_tail "$prog"
            ;;
        *)
            echo "    [$DONE_COUNT/$TOTAL] ??? $prog  raw='$raw'"
            ;;
    esac
}

# --- Build every emulator for ONE hwconfig -----------------------------------
# Sets globals LAST_BUILT_COUNT / LAST_FAILED_COUNT / LAST_TOTAL for the caller's
# grand summary. Returns 1 if any emulator failed (or nothing built), else 0.
build_for_hwconfig() {
    HWCONFIG="$1"
    mkdir -p "$BUILD_DIR"
    mkdir -p "$LOADER_DIR/emu/$HWCONFIG"
    STATUS_DIR=$(mktemp -d -t pico-emu-build.XXXXXX)
    LOGS_DIR="$STATUS_DIR/logs"
    mkdir -p "$LOGS_DIR"

    echo
    hr
    echo " Build plan"
    hr
    info "hwconfig         : $HWCONFIG (${HW_DESC[$HWCONFIG]})"
    info "emulator branch  : $EMU_BRANCH"
    info "pico_shared      : $SHARED_BRANCH"
    info "emulators        : ${#PROG_NAMES[@]}"
    info "parallelism      : $JOBS emulator(s) at a time"
    info "per-build cores  : $PER_PROC of $NPROC"
    info "install dir      : $LOADER_DIR/emu/$HWCONFIG/"
    info "status dir       : $STATUS_DIR"
    if (( JOBS > 1 )); then
        info "per-emulator logs: $LOGS_DIR/<prog>.log"
        info "live-tail one    : tail -f $LOGS_DIR/${PROG_NAMES[0]}.log"
    fi
    echo

    # Track outcomes for live progress reporting (fresh per hwconfig).
    declare -A PID_TO_PROG=()
    DONE_COUNT=0
    TOTAL=${#PROG_NAMES[@]}
    local RUN_T0=$SECONDS
    local idx=0 running=0 finished_pid prog

    if (( JOBS == 1 )); then
        # Serial path: stream output live, report each as it finishes.
        for prog in "${PROG_NAMES[@]}"; do
            idx=$(( idx + 1 ))
            info "[$idx/$TOTAL] starting $prog"
            build_one_emulator "$prog"
            report_completion "$prog"
            echo
        done
    else
        # Parallel path: each build's output goes to its own log file; print a
        # one-line completion notice as each finishes.
        for prog in "${PROG_NAMES[@]}"; do
            while (( running >= JOBS )); do
                finished_pid=""
                wait -n -p finished_pid || true
                if [ -n "$finished_pid" ] && [ -n "${PID_TO_PROG[$finished_pid]+x}" ]; then
                    report_completion "${PID_TO_PROG[$finished_pid]}"
                    unset 'PID_TO_PROG[$finished_pid]'
                fi
                running=$(( running - 1 ))
            done
            idx=$(( idx + 1 ))
            info "[$idx/$TOTAL] queued $prog  (log: $LOGS_DIR/$prog.log)"
            ( build_one_emulator "$prog" ) >"$LOGS_DIR/$prog.log" 2>&1 &
            PID_TO_PROG[$!]="$prog"
            running=$(( running + 1 ))
        done
        # Drain remaining background jobs.
        while (( running > 0 )); do
            finished_pid=""
            wait -n -p finished_pid || true
            if [ -n "$finished_pid" ] && [ -n "${PID_TO_PROG[$finished_pid]+x}" ]; then
                report_completion "${PID_TO_PROG[$finished_pid]}"
                unset 'PID_TO_PROG[$finished_pid]'
            fi
            running=$(( running - 1 ))
        done
    fi

    local RUN_ELAPSED=$(( SECONDS - RUN_T0 ))

    # --- Summary -------------------------------------------------------------
    local BUILT=() SKIPPED=() FAILED=() MISSING=()
    local status_file raw body rest
    for prog in "${PROG_NAMES[@]}"; do
        status_file="$STATUS_DIR/$prog.status"
        if [ ! -r "$status_file" ]; then
            FAILED+=("$prog | (no status file — crashed?)")
            continue
        fi
        raw=$(cat "$status_file")
        body="${raw%%|*}"
        rest="${raw#*|}"
        case "$body" in
            BUILT:*)
                local fname="${body#BUILT:}" bytes="${rest%%|*}" elapsed="${rest#*|}"
                BUILT+=("$prog | $(human_size "$bytes") | $(human_time "$elapsed") | ${fname}")
                ;;
            SKIP:*)
                local reason="${body#SKIP:}" elapsed="$rest"
                SKIPPED+=("$prog | $(human_time "$elapsed") | $reason")
                ;;
            FAIL:*)
                local reason="${body#FAIL:}" elapsed="$rest"
                FAILED+=("$prog | $(human_time "$elapsed") | $reason")
                ;;
            MISSING)
                MISSING+=("$prog | $(human_time "$rest") | build succeeded but no UF2 found")
                ;;
            *)
                FAILED+=("$prog | unknown status: $raw")
                ;;
        esac
    done

    echo
    hr
    echo " Summary  (HW_CONFIG=$HWCONFIG, emu=$EMU_BRANCH, pico_shared=$SHARED_BRANCH)"
    echo " Total wall-clock: $(human_time "$RUN_ELAPSED")"
    hr
    echo "Built (${#BUILT[@]}/${TOTAL}):"
    for p in "${BUILT[@]}";   do echo "  + $p"; done
    echo "Failed (${#FAILED[@]}/${TOTAL}):"
    for p in "${FAILED[@]}";  do echo "  ! $p"; done
    echo "Skipped (${#SKIPPED[@]}/${TOTAL}):"
    for p in "${SKIPPED[@]}"; do echo "  - $p"; done
    echo "Missing output (${#MISSING[@]}/${TOTAL}):"
    for p in "${MISSING[@]}"; do echo "  ? $p"; done

    echo
    info "Installed files now in $LOADER_DIR/emu/$HWCONFIG/:"
    ls -lh "$LOADER_DIR/emu/$HWCONFIG/" 2>/dev/null | tail -n +2 | sed 's/^/    /' || true

    if (( JOBS > 1 )); then
        echo
        info "Per-emulator logs preserved in: $LOGS_DIR/"
    fi

    LAST_BUILT_COUNT=${#BUILT[@]}
    LAST_FAILED_COUNT=$(( ${#FAILED[@]} + ${#MISSING[@]} ))
    LAST_TOTAL=$TOTAL
    if (( LAST_FAILED_COUNT > 0 )); then
        warn "$LAST_FAILED_COUNT emulator(s) failed for HW_CONFIG=$HWCONFIG (see FAIL/MISS above)"
        return 1
    fi
    if [ ${#BUILT[@]} -eq 0 ]; then
        warn "no emulators were built for HW_CONFIG=$HWCONFIG"
        return 1
    fi
    return 0
}

# --- Drive the selected hwconfig(s) ------------------------------------------
declare -A CONFIG_RESULT
overall_rc=0
OVERALL_T0=$SECONDS
for HW in "${HWCONFIGS_TO_BUILD[@]}"; do
    if build_for_hwconfig "$HW"; then :; else overall_rc=1; fi
    if (( ${LAST_FAILED_COUNT:-0} > 0 )); then
        CONFIG_RESULT[$HW]="${LAST_BUILT_COUNT}/${LAST_TOTAL} built, ${LAST_FAILED_COUNT} failed"
    else
        CONFIG_RESULT[$HW]="${LAST_BUILT_COUNT}/${LAST_TOTAL} built"
    fi
done

if (( MULTI )); then
    echo
    hr
    echo " Grand summary  (${#HWCONFIGS_TO_BUILD[@]} hwconfig(s), total $(human_time $(( SECONDS - OVERALL_T0 ))))"
    hr
    for HW in "${HWCONFIGS_TO_BUILD[@]}"; do
        printf "    HW_CONFIG=%-2s  %-28s  %s\n" "$HW" "${CONFIG_RESULT[$HW]}" "${HW_DESC[$HW]}"
    done
    (( overall_rc != 0 )) && warn "one or more hwconfigs had build failures or produced no UF2s"
fi

# --- Version manifest + SD-card archive (-z) ---------------------------------
# emu/versions.txt records the ref every shipped emulator was built from. It is a
# tracked file with three jobs: it ships inside the zip so users can see what
# they have, the release workflow renders it into the release notes, and its git
# diff is the record of what changed between two releases.
#
# Only emulators that actually produced a UF2 for at least one board are listed —
# the manifest describes what shipped, not what was attempted.
write_versions_manifest() {
    local out="$LOADER_DIR/emu/versions.txt"
    local prog hw listed=0
    {
        echo "# pico-bootLoader SD-card bundle — emulator versions"
        echo "# generated by build_emulators.sh; do not edit by hand"
        echo "#"
        echo "# Format: <program_name>;<repo>;<ref>;<pico_shared>"
        echo "#   ref          the release tag the emulator was built from, or"
        echo "#                <branch>@<shortsha> for a repo that has no tag yet."
        echo "#   pico_shared  the shared-framework revision it was built against."
        echo "#                Not the revision the tag pins: bld.sh gained -b only"
        echo "#                in f2c8be9, which the emulator tags predate."
        for prog in "${PROG_NAMES[@]}"; do
            [ -n "${REF_OF[$prog]:-}" ] || continue
            for hw in "${HWCONFIGS_TO_BUILD[@]}"; do
                if [ -s "$LOADER_DIR/emu/$hw/${prog}.uf2" ]; then
                    # Doom vendors no pico_shared, so that column stays empty.
                    local shared="$SHARED_SHA"
                    [ -n "${DOOM_BRANCH[$prog]+x}" ] && shared=""
                    echo "${prog};${REPO_OF[$prog]};${REF_OF[$prog]};${shared}"
                    listed=$(( listed + 1 ))
                    break
                fi
            done
        done
    } > "$out"
    info "wrote emu/versions.txt ($listed emulator(s))"
    sed 's/^/    /' "$out"
}

if (( PACK_ZIP )); then
    echo
    hr
    echo " SD-card archive"
    hr
    write_versions_manifest

    PACKER="$LOADER_DIR/.github/scripts/pack_sdcard.sh"
    [ -f "$PACKER" ] || die "packer not found: $PACKER"
    [ -x "$PACKER" ] || chmod +x "$PACKER" 2>/dev/null || true
    ZIP_OUT="$LOADER_DIR/releases/pico-bootLoader_sdcard.zip"
    mkdir -p "$LOADER_DIR/releases"
    echo
    if "$PACKER" "$LOADER_DIR" "$ZIP_OUT"; then
        info "SD-card archive: $ZIP_OUT ($(human_size "$(stat -c%s "$ZIP_OUT")"))"
    else
        warn "packing the SD-card archive failed"
        overall_rc=1
    fi
fi

exit "$overall_rc"
