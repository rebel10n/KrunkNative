#!/usr/bin/env bash
# Web pointer-lock cycles (tools/kref/pointer-lock-e2e.js): a web build on a static server, offline
# (the in-page sandbox server, no game server), in headless Chromium.
#
#   tools/kref/pointer-lock-e2e.sh WEB_DIST_DIR [cycles]      (run from the repo root)
set -euo pipefail
DIST=$(cd "${1:?web dist dir}" && pwd)
CYCLES=${2:-3}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kn-pointer-lock-XXXXXX")
BASE=$((29500 + RANDOM % 400))
HTTP=$BASE CDPP=$((BASE + 1))
pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    [ "${KN_TEST_KEEP:-0}" = 1 ] && echo "[pointer-lock] kept $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT

(cd "$DIST" && exec python3 -m http.server "$HTTP" --bind 127.0.0.1) > "$WORK/static.log" 2>&1 &
pids+=($!)
chromium-browser --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader --mute-audio \
    --window-size=960,540 --user-data-dir="$WORK/chrome" --remote-debugging-port=$CDPP about:blank > "$WORK/chrome.log" 2>&1 &
pids+=($!)

status=0
node tools/kref/pointer-lock-e2e.js $CDPP "http://127.0.0.1:$HTTP/?offline=1&map=kanji" "$CYCLES" || status=$?
[ $status = 0 ] && echo "[pointer-lock] PASS" || { echo "[pointer-lock] FAIL ($status)"; exit 1; }
