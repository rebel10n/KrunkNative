#!/usr/bin/env bash
# Web bridge admission e2e: server_bin + `net-bridge --web` (KRUNK_NATIVE_TARGETS = that one game
# server) serving a web build + headless Chromium; tools/kref/bridge-refusal-e2e.js has the cases
# (allowed target, localhost alias, a refused target shown with its reason and not retried).
#
#   tools/kref/bridge-refusal-e2e.sh BUILD_DIR WEB_DIST_DIR      (run from the repo root)
set -euo pipefail
BUILD=${1:?native build dir}
DIST=$(cd "${2:?web dist dir}" && pwd)
BUILD=$(cd "$BUILD" && pwd)
WORK=$(mktemp -d /tmp/kn-bridge-refusal-XXXXXX)
BASE=$((25500 + RANDOM % 1500))
GAME=$BASE HTTP=$((BASE + 1)) CDPP=$((BASE + 2))
pids=()
cleanup() {
    for pid in "${pids[@]}"; do pkill -TERM -P "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    [ "${KN_TEST_KEEP:-0}" = 1 ] && echo "[bridge-refusal] kept $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT

"$BUILD/server_bin" --headless --cfg none --port "$GAME" server/tests/arena.json > "$WORK/game.log" 2>&1 < /dev/null &
pids+=($!)
(cd net-bridge && KRUNK_NATIVE_PORT=$GAME KRUNK_NATIVE_TARGETS=127.0.0.1:$GAME KRUNK_NATIVE_BRIDGE_PORT=$HTTP \
    KRUNK_NATIVE_WEB_DIR="$DIST" exec npm run web) > "$WORK/bridge.log" 2>&1 &
pids+=($!)
chromium-browser --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader --mute-audio \
    --user-data-dir="$WORK/chrome" --remote-debugging-port=$CDPP about:blank > "$WORK/chrome.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do grep -q "listening on http" "$WORK/bridge.log" 2>/dev/null && break; sleep 0.2; done

status=0
node tools/kref/bridge-refusal-e2e.js $CDPP $HTTP $GAME || status=$?
echo "[bridge-refusal] bridge log:"; grep -E "refused|bridged|session" "$WORK/bridge.log" | sed 's/^/  /' | tail -n 8
if grep -q "upgrade refused 1008: target not allowed (/native?target=127.0.0.1:$HTTP)" "$WORK/bridge.log"; then
    echo "PASS  the bridge logs the refusal (1008, target not allowed)"
else
    echo "FAIL  the bridge logs the refusal (1008, target not allowed)"; status=1
fi
[ $status = 0 ] && echo "[bridge-refusal] PASS" || { echo "[bridge-refusal] FAIL ($status)"; exit 1; }
