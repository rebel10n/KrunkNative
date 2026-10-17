#!/usr/bin/env bash
# Web start-up without the first-run popup: a web build served over HTTP + headless Chromium, on one fresh
# browser profile (the menu is up, no popup), then the same profile again (no popup), then a second fresh
# profile (no popup either). The page's origin has no bridge: /healthz is a 404, the brain is not there,
# and the menu stays up on the default connection.
#
#   tools/kref/startup-no-popup-web-e2e.sh WEB_DIST_DIR     (run from the repo root)
set -euo pipefail
DIST=${1:?web dist dir}
WORK=$(mktemp -d /tmp/kn-startup-no-popup-web-XXXXXX)
BASE=$((25500 + RANDOM % 1500))
HTTP=$BASE CDPP=$((BASE + 1))
pids=()
cleanup() {
    for pid in "${pids[@]}"; do pkill -TERM -P "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    [ "${KN_TEST_KEEP:-0}" = 1 ] && echo "[startup-no-popup-web-e2e] kept $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT

chrome=$(command -v chromium-browser || command -v chromium || command -v google-chrome)
(cd "$DIST" && exec python3 -m http.server "$HTTP" --bind 127.0.0.1) > "$WORK/http.log" 2>&1 &
pids+=($!)
sleep 1

run_chrome() { # profile dir, then the mode
    local profile=$1 mode=$2 status=0
    "$chrome" --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
        --window-size=1280,720 --user-data-dir="$profile" --remote-debugging-port=$CDPP about:blank > "$profile.log" 2>&1 &
    local chrome_pid=$!
    pids+=($chrome_pid)
    sleep 2
    node tools/kref/startup-no-popup-web-e2e.js $CDPP "http://127.0.0.1:$HTTP/" "$mode" || status=$?
    wait $chrome_pid 2>/dev/null || true # the node check closed the browser (a clean shutdown: the profile is saved)
    return $status
}

status=0
run_chrome "$WORK/profile-a" fresh || status=$?
run_chrome "$WORK/profile-a" again || status=$?
run_chrome "$WORK/profile-b" fresh || status=$?
[ $status = 0 ] && echo "[startup-no-popup-web-e2e] PASS" || { echo "[startup-no-popup-web-e2e] FAIL ($status)"; exit 1; }
