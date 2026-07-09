#!/usr/bin/env bash
# Interactive helper: clone + build the bootloader-targeted emulators for one
# hwconfig (or, with -c all, every supported hwconfig) and drop the resulting
# UF2s into emu/<hwconfig>/<prog_name>.uf2.
#
# Mirrors the "Clone and build the emulators" step of
# .github/workflows/BuildAndRelease.yml, but interactive and scoped to a single
# hwconfig + a user-chosen branch for emulator repos and pico_shared. With -t it
# instead builds each repo's latest release tag (submodules pinned in the tag),
# matching the release workflow. With -m it non-interactively builds each repo's
# default branch (main or master, resolved per repo from the remote HEAD).
set -euo pipefail

JOBS=2
TAG_MODE=0
MAIN_MODE=0
HWCONFIG_ARG=""
usage() {
    cat <<EOF
Usage: $0 [-c N|all] [-j N] [-t] [-m] [-h]
  -c N   build for HW_CONFIG N non-interactively (skip the picker).
  -c all build for every supported HW_CONFIG, unattended. PIO-USB configs are
         skipped automatically when PICO_PIO_USB_PATH is unset/unusable.
         Combine with -t or -m for a fully hands-off build of all configs.
  -j N   build up to N emulators in parallel (default: $JOBS). Each build
         is capped to nproc/N processors so total load stays near nproc.
         With -j 1, output streams live; with -j >1, per-emulator logs are
         written to a temp dir.
  -t     build each emulator repo's latest release tag (like the release
         workflow / pack_release path) instead of asking for branches.
         Submodules — including pico_shared — are left at the revisions
         pinned in that tag.
  -m     non-interactively build each repo's default branch (main or master,
         whichever its remote HEAD points at), skipping the branch picker.
         Mutually exclusive with -t.
  -h     this help
EOF
}
while getopts "c:j:tmh" opt; do
    case "$opt" in
        c) HWCONFIG_ARG="$OPTARG" ;;
        j) JOBS="$OPTARG" ;;
        t) TAG_MODE=1 ;;
        m) MAIN_MODE=1 ;;
        h) usage; exit 0 ;;
        *) usage >&2; exit 1 ;;
    esac
done
[[ "$JOBS" =~ ^[0-9]+$ ]] && (( JOBS >= 1 )) || { echo "ERROR: -j must be a positive integer" >&2; exit 1; }
(( TAG_MODE && MAIN_MODE )) && { echo "ERROR: -t and -m are mutually exclusive" >&2; exit 1; }

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
    [doom_tiny]=fruitjam-doom
)

# fruitjam-doom (Doom!) is a special case. Unlike the emulators above it targets
# a single board (Adafruit Fruit Jam / HW_CONFIG 8), has no release tags (built
# from a branch), vendors its own pico-sdk/pico-extras as submodules, and uses a
# bespoke build script that additionally emits a DATA-family WAD UF2. It is
# built by _build_doom() instead of the generic bld.sh path.
DOOM_PROG="doom_tiny"
DOOM_REPO="fruitjam-doom"
DOOM_BRANCH="adafruit-fruitjam"
DOOM_HWCONFIG=8
DOOM_BUILD_SCRIPT="fruitjam-build-forbootloader.sh"
DOOM_BUILD_SUBDIR="build_bl_fruitjam/src"

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
    info "Tag mode: each emulator repo builds its latest release tag; submodules"
    info "          (including pico_shared) stay at the revisions pinned in the tag."
    EMU_BRANCH="(latest tag per repo)"
    SHARED_BRANCH="(pinned in tag)"
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

# --- Build fruitjam-doom (Doom!) ---------------------------------------------
# Fruit Jam only. Clones the branch with its vendored SDK/extras submodules and
# runs fruitjam-build-forbootloader.sh, which emits the app UF2 plus a DATA
# family WAD UF2 in build_bl_fruitjam/src/. Installs both into emu/<hw>/ and
# writes the same status contract as build_one_emulator().
_build_doom() {
    local prog="$1" status_file="$2" t0="$3"
    local repo="$DOOM_REPO" branch="$DOOM_BRANCH"
    local dest="$BUILD_DIR/$repo"
    local url="https://github.com/${GITHUB_OWNER}/${repo}.git"
    local elapsed

    # Only meaningful for the Fruit Jam config; skip cleanly for any other.
    if [ "$HWCONFIG" != "$DOOM_HWCONFIG" ]; then
        elapsed=$(( SECONDS - t0 ))
        echo "SKIP:doom_tiny only builds for HW_CONFIG=$DOOM_HWCONFIG (Fruit Jam)|$elapsed" > "$status_file"
        return 0
    fi

    hr
    echo " $prog  ($repo @ branch $branch)  [Fruit Jam / HW_CONFIG $DOOM_HWCONFIG only]"
    echo " clone -> $dest"
    hr

    # Drop any artifacts from a previous run up front, so a failed rebuild can
    # never leave a stale doom_tiny.uf2 (or its WAD) behind in emu/$HWCONFIG/.
    local out_dir="$LOADER_DIR/emu/$HWCONFIG"
    mkdir -p "$out_dir"
    info "[$prog] removing any previous emu/$HWCONFIG/ Doom artifacts"
    rm -f "$out_dir/${prog}.uf2"
    shopt -s nullglob
    local stale
    for stale in "$out_dir"/*-for-fruitjam.uf2; do rm -f "$stale"; done
    shopt -u nullglob

    info "[$prog] cleaning previous clone (if any)"
    rm -rf "$dest"

    info "[$prog] cloning ${repo}@${branch} (recursive: vendored pico-sdk/pico-extras)"
    if ! git clone --branch "$branch" --recurse-submodules --depth 1 "$url" "$dest"; then
        elapsed=$(( SECONDS - t0 ))
        echo "FAIL:clone of ${repo}@${branch} failed|$elapsed" > "$status_file"
        return 0
    fi

    set +e
    (
        set -e
        cd "$dest"
        chmod +x "$DOOM_BUILD_SCRIPT" 2>/dev/null || true
        info "[$prog] running ./$DOOM_BUILD_SCRIPT"
        "./$DOOM_BUILD_SCRIPT"
    )
    local rc=$?
    set -e
    if [ "$rc" -ne 0 ]; then
        elapsed=$(( SECONDS - t0 ))
        echo "FAIL:build failed (rc=$rc)|$elapsed" > "$status_file"
        return 0
    fi

    info "[$prog] locating produced UF2s"
    local src_dir="$dest/$DOOM_BUILD_SUBDIR"
    local app_uf2="$src_dir/doom_tiny.uf2"
    if [ ! -f "$app_uf2" ]; then
        elapsed=$(( SECONDS - t0 ))
        echo "MISSING|$elapsed" > "$status_file"
        return 0
    fi

    cp "$app_uf2" "$out_dir/${prog}.uf2"
    # Companion DATA-family WAD (emulators.txt aux_uf2); the build tags it
    # *-for-fruitjam.uf2. Install alongside the app so the loader can flash it.
    shopt -s nullglob
    local aux
    for aux in "$src_dir"/*-for-fruitjam.uf2; do
        cp "$aux" "$out_dir/$(basename "$aux")"
        info "[$prog] installed aux $(basename "$aux") -> emu/$HWCONFIG/"
    done
    shopt -u nullglob

    local bytes
    bytes=$(stat -c%s "$out_dir/${prog}.uf2")
    info "[$prog] installed doom_tiny.uf2 -> emu/$HWCONFIG/${prog}.uf2  ($(human_size "$bytes"))"
    elapsed=$(( SECONDS - t0 ))
    echo "BUILT:doom_tiny.uf2|$bytes|$elapsed" > "$status_file"
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

    # fruitjam-doom builds nothing like the others (single board, vendored SDK,
    # its own script, extra WAD UF2). Hand it off entirely.
    if [ "$prog" = "$DOOM_PROG" ]; then
        _build_doom "$prog" "$status_file" "$t0"
        return 0
    fi

    local repo="${REPO_OF[$prog]}"
    local dest="$BUILD_DIR/$repo"
    local url="https://github.com/${GITHUB_OWNER}/${repo}.git"

    # Resolve the ref to build: the chosen branch, or (in tag mode) this repo's
    # latest release tag — the same selection the release workflow makes with
    # `git ls-remote --tags --refs --sort=-v:refname | head -n1`.
    local ref ref_desc
    if (( TAG_MODE )); then
        info "[$prog] resolving latest tag for $repo"
        ref=$(git ls-remote --tags --refs --sort=-v:refname "$url" \
            | head -n1 | sed 's#.*refs/tags/##')
        if [ -z "$ref" ]; then
            elapsed=$(( SECONDS - t0 ))
            echo "SKIP:no tags found in $repo|$elapsed" > "$status_file"
            return 0
        fi
        ref_desc="tag $ref (submodules pinned in tag)"
    elif (( MAIN_MODE )); then
        info "[$prog] resolving default branch for $repo"
        ref=$(default_branch_of "$url")
        if [ -z "$ref" ]; then
            elapsed=$(( SECONDS - t0 ))
            echo "FAIL:could not resolve default branch for $repo|$elapsed" > "$status_file"
            return 0
        fi
        ref_desc="$ref (default branch), pico_shared @ $SHARED_BRANCH"
    else
        ref="$EMU_BRANCH"
        ref_desc="$EMU_BRANCH, pico_shared @ $SHARED_BRANCH"
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
            info "[$prog] submodules pinned by tag (pico_shared left as-is)"
            if [ -d pico_shared ]; then
                local shared_sha
                shared_sha=$(git -C pico_shared rev-parse --short HEAD)
                echo "    -> pico_shared at $shared_sha"
            fi
        else
            info "[$prog] initialising submodules (pinned revisions)"
            git submodule update --init --recursive
            if [ -d pico_shared ]; then
                info "[$prog] switching pico_shared submodule to '${SHARED_BRANCH}'"
                (
                    cd pico_shared
                    if ! git fetch --depth 1 origin "$SHARED_BRANCH"; then
                        exit 73
                    fi
                    git checkout --detach FETCH_HEAD
                    local shared_sha
                    shared_sha=$(git rev-parse --short HEAD)
                    echo "    -> pico_shared now at $shared_sha"
                ) || exit 73
            fi
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
    local matches=("$dest"/releases/"${prog}"_*_bl.uf2)
    if [ ${#matches[@]} -eq 0 ]; then
        matches=("$dest"/releases/"${prog}"_*.uf2)
    fi
    shopt -u nullglob
    if [ ${#matches[@]} -eq 0 ]; then
        elapsed=$(( SECONDS - t0 ))
        echo "MISSING|$elapsed" > "$status_file"
        return 0
    fi
    local src_uf2="${matches[0]}"
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

exit "$overall_rc"
