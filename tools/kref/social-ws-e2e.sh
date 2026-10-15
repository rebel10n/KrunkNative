#!/usr/bin/env bash
# The social WebSocket on local web hosting (tools/kref/social-ws-e2e.js has the cases): the web
# build served by a plain static server (`make web-serve`), by `net-bridge --web` with no brain, and
# by `net-bridge --web --brain` in front of kn_brain --no-db; one headless Chromium page each.
#
#   tools/kref/social-ws-e2e.sh BUILD_DIR WEB_DIST_DIR      (run from the repo root; BUILD_DIR has
#                                                           services/brain/kn_brain)
set -euo pipefail
BUILD=$(cd "${1:?native build dir}" && pwd)
DIST=$(cd "${2:?web dist dir}" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kn-social-ws-XXXXXX")
BASE=$((28500 + RANDOM % 1000))
STATIC=$BASE BRIDGE1=$((BASE + 1)) BRIDGE2=$((BASE + 2)) BRAIN=$((BASE + 3)) CDPP=$((BASE + 4)) GAME=$((BASE + 5))
pids=()
cleanup() {
    for pid in "${pids[@]}"; do pkill -TERM -P "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    [ "${KN_TEST_KEEP:-0}" = 1 ] && echo "[social-ws] kept $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT
wait_for() { for _ in $(seq 150); do grep -q "$2" "$1" 2>/dev/null && return 0; sleep 0.2; done; echo "timed out waiting for '$2' in $1"; return 1; }

(cd "$DIST" && exec python3 -m http.server "$STATIC" --bind 127.0.0.1) > "$WORK/static.log" 2>&1 &
pids+=($!)
(cd net-bridge && KRUNK_NATIVE_PORT=$GAME KRUNK_NATIVE_BRIDGE_PORT=$BRIDGE1 KRUNK_NATIVE_WEB_DIR="$DIST" \
    exec npm run web -- --brain "http://127.0.0.1:$((BASE + 7))") > "$WORK/bridge-nobrain.log" 2>&1 &
pids+=($!)
"$BUILD/services/brain/kn_brain" --no-db "--brain.port=$BRAIN" --brain.id=socialws$$ > "$WORK/brain.log" 2>&1 &
pids+=($!)
(cd net-bridge && KRUNK_NATIVE_PORT=$GAME KRUNK_NATIVE_BRIDGE_PORT=$BRIDGE2 KRUNK_NATIVE_WEB_DIR="$DIST" \
    exec npm run web -- --brain "http://127.0.0.1:$BRAIN") > "$WORK/bridge-brain.log" 2>&1 &
pids+=($!)
chromium-browser --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader --mute-audio \
    --user-data-dir="$WORK/chrome" --remote-debugging-port=$CDPP about:blank > "$WORK/chrome.log" 2>&1 &
pids+=($!)
wait_for "$WORK/bridge-nobrain.log" "listening on http"
wait_for "$WORK/bridge-brain.log" "listening on http"
wait_for "$WORK/brain.log" "listening on http"

status=0
node tools/kref/social-ws-e2e.js $CDPP static "http://127.0.0.1:$STATIC/" || status=$?
node tools/kref/social-ws-e2e.js $CDPP nobrain "http://127.0.0.1:$BRIDGE1/" || status=$?
node tools/kref/social-ws-e2e.js $CDPP brain "http://127.0.0.1:$BRIDGE2/" || status=$?
[ $status = 0 ] && echo "[social-ws] PASS" || { echo "[social-ws] FAIL ($status)"; exit 1; }
