#!/usr/bin/env bash
# Web offline e2e (client/src/net/offline.c): a web build served over HTTP + headless Chromium
# in real time (no virtual-time budget). Passes when the page plays offline on the in-process
# sandbox server: joined, in game, dev console commands answered by the sandbox (bot_add, god).
#
#   tools/kref/offline-e2e.sh WEB_DIST_DIR [query] [seconds]     (run from the repo root)
#   query defaults to "offline=1"; e.g. "sandbox", "offline=1&map=custom/ESP_Parkour"
set -euo pipefail
DIST=${1:?web dist dir}
QUERY=${2:-offline=1}
SECONDS_RUN=${3:-45}
WORK=$(mktemp -d /tmp/kn-offline-e2e-XXXXXX)
BASE=$((25500 + RANDOM % 1500))
HTTP=$BASE CDPP=$((BASE + 1))
pids=()
cleanup() {
    for pid in "${pids[@]}"; do pkill -TERM -P "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    [ "${KN_TEST_KEEP:-0}" = 1 ] && echo "[offline-e2e] kept $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT

chrome=$(command -v chromium-browser || command -v chromium || command -v google-chrome)
(cd "$DIST" && exec python3 -m http.server "$HTTP" --bind 127.0.0.1) > "$WORK/http.log" 2>&1 &
pids+=($!)
"$chrome" --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
    --window-size=1280,720 --user-data-dir="$WORK/chrome" --remote-debugging-port=$CDPP about:blank > "$WORK/chrome.log" 2>&1 &
pids+=($!)
sleep 1

status=0
node tools/kref/offline-e2e.js $CDPP "http://127.0.0.1:$HTTP/?$QUERY" "$SECONDS_RUN" "${KN_E2E_SHOT:-/tmp/kn-offline-e2e.png}" || status=$?
[ $status = 0 ] && echo "[offline-e2e] PASS" || { echo "[offline-e2e] FAIL ($status)"; exit 1; }
