#!/usr/bin/env bash
# Build test for the Krunker Strike distribution (make strike-dist, tools/release/strike-dist.sh):
#   1. `make strike-dist` builds the Strike-defaulted Release tree and produces both packages;
#   2. the dedicated server and the client in the dist report Krunker Strike on --version, headless
#      (KN_FLAVOR default strike, the compile-time default -- no --flavor / KN_FLAVOR needed);
#   3. the server advertises flavor strike, the client wordmark is "Krunker Strike";
#   4. every archive name stays BoxOffice (only the DISPLAY name is Strike);
#   5. the base build is unaffected: the default (base) server_bin still reports Krunker Box Office.
#
#   tools/release/test-strike-dist.sh [BASE_BUILD_DIR]   (run from the repository root)
#
# BASE_BUILD_DIR (default cmake-build-debug): a base (non-Strike) build for the "base unaffected"
# check; its server_bin is built if missing. Set SKIP_BUILD=1 to reuse an existing dist/strike/.
# Prints PASS/FAIL lines; exit 0 only when everything passed.
set -uo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
cd "$repo"
BASE_BUILD="${1:-cmake-build-debug}"
STRIKE_DIST="${STRIKE_DIST:-dist/strike}"
JOBS="${JOBS:-6}"
WORK="$(mktemp -d "${TMPDIR:-/tmp}/kn-strike-dist.XXXXXX")"
CFG="$WORK/cfg"
mkdir -p "$CFG"
results=()

check() { # check <name> <ok 0/1> [detail]
    results+=("$2")
    printf '%s  %s%s\n' "$([ "$2" = 0 ] && echo PASS || echo FAIL)" "$1" "${3:+  -- $3}"
}

# --version with no window on any display (brief: headless, null audio, private config).
version() { env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_AUDIO_BACKEND=null \
    XDG_CONFIG_HOME="$CFG" timeout 60 "$1" --version 2>&1; }

if [ "${SKIP_BUILD:-0}" != 1 ]; then
    echo "== make strike-dist (JOBS=$JOBS) ==" >&2
    make strike-dist JOBS="$JOBS" STRIKE_DIST="$STRIKE_DIST" >&2
    rc=$?
    check "make strike-dist succeeds" "$rc"
    [ "$rc" = 0 ] || { echo "0/${#results[@]} passed  (logs: $WORK)"; exit 1; }
fi

srv_tar="$(ls "$STRIKE_DIST"/BoxOffice-server-*.tar.gz 2> /dev/null | head -1)"
cli_tar="$(ls "$STRIKE_DIST"/BoxOffice-linux-*.tar.gz 2> /dev/null | head -1)"
check "server package produced" "$([ -f "$srv_tar" ] && echo 0 || echo 1)" "$srv_tar"
check "client package produced" "$([ -f "$cli_tar" ] && echo 0 || echo 1)" "$cli_tar"
# identifiers stay krunknative; the archive names never say strike
check "packages named BoxOffice, not strike" \
    "$([ -n "$srv_tar$cli_tar" ] && ! ls "$STRIKE_DIST"/*strike* 2> /dev/null | grep -q . && echo 0 || echo 1)"

if [ -f "$srv_tar" ]; then
    mkdir -p "$WORK/srv" && tar -xzf "$srv_tar" -C "$WORK/srv"
    sdir="$WORK/srv"
    out="$(version "$sdir/server_bin")"
    check "dist server_bin --version: Krunker Strike" \
        "$(echo "$out" | grep -q 'flavor strike (Krunker Strike)' && echo 0 || echo 1)" "$(echo "$out" | tr '\n' ' ')"
fi

if [ -f "$cli_tar" ]; then
    mkdir -p "$WORK/cli" && tar -xzf "$cli_tar" -C "$WORK/cli"
    cdir="$WORK/cli/runtime"
    out="$(version "$cdir/client_bin")"
    check "dist client_bin --version: Krunker Strike wordmark" \
        "$(echo "$out" | grep -q 'Krunker Strike (KrunkNative client)' && echo 0 || echo 1)" "$(echo "$out" | head -1)"
    # the dist server_bin shipped inside the client package is Strike too
    out="$(version "$cdir/server_bin")"
    check "client package's server_bin is Strike too" \
        "$(echo "$out" | grep -q 'Krunker Strike' && echo 0 || echo 1)"
fi

# base unaffected: the default (base) build still reports Krunker Box Office, no flavor line.
if [ ! -x "$BASE_BUILD/server_bin" ]; then
    echo "== building base server_bin for the base-unaffected check ==" >&2
    cmake --preset debug > /dev/null 2>&1 || true
    cmake --build "$BASE_BUILD" --parallel "$JOBS" --target server_bin >&2
fi
if [ -x "$BASE_BUILD/server_bin" ]; then
    out="$(version "$BASE_BUILD/server_bin")"
    check "base server_bin --version: Krunker Box Office, no flavor line" \
        "$(echo "$out" | grep -q 'Krunker Box Office' && ! echo "$out" | grep -q 'Krunker Strike' && echo 0 || echo 1)" \
        "$(echo "$out" | head -1)"
else
    check "base server_bin available" 1 "no $BASE_BUILD/server_bin"
fi

pass=0
for r in "${results[@]}"; do [ "$r" = 0 ] && pass=$((pass + 1)); done
if [ "$pass" = "${#results[@]}" ] && [ "${#results[@]}" -gt 0 ]; then
    rm -rf "$WORK"
    echo "$pass/${#results[@]} passed"
    exit 0
fi
echo "$pass/${#results[@]} passed  (logs: $WORK)"
exit 1
