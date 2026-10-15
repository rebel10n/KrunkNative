#!/usr/bin/env bash
# Web GL-error e2e: a web build served over HTTP + headless Chromium plays offline for a minute
# (tools/kref/gl-errors-e2e.js) and fails when the console shows any GL_INVALID_* error.
#
#   tools/kref/gl-errors-e2e.sh WEB_DIST_DIR [query] [seconds]     (run from the repo root)
#   query defaults to "offline=1&map=kanji", seconds (of play) to 60
set -euo pipefail
DIST=${1:?web dist dir}
QUERY=${2:-offline=1&map=kanji}
SECONDS_RUN=${3:-60}
WORK=$(mktemp -d /tmp/kn-gl-errors-e2e-XXXXXX)
BASE=$((27100 + RANDOM % 1500))
HTTP=$BASE CDPP=$((BASE + 1))
pids=()
cleanup() {
    for pid in "${pids[@]}"; do pkill -TERM -P "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    [ "${KN_TEST_KEEP:-0}" = 1 ] && echo "[gl-errors-e2e] kept $WORK" || rm -rf "$WORK"
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
node tools/kref/gl-errors-e2e.js $CDPP "http://127.0.0.1:$HTTP/?$QUERY" "$SECONDS_RUN" "${KN_E2E_SHOT:-/tmp/kn-gl-errors-e2e.png}" || status=$?
[ $status = 0 ] && echo "[gl-errors-e2e] PASS" || { echo "[gl-errors-e2e] FAIL ($status)"; exit 1; }
