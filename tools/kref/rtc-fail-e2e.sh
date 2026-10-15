#!/usr/bin/env bash
# Failed WebRTC sessions in the dev console: server_bin + two net-bridge/rtc gateways (one refusing
# the target, one with a 3 s idle timeout) + net-bridge --web serving a web build + headless
# Chromium; tools/kref/rtc-fail-e2e.js has the cases.
#
#   tools/kref/rtc-fail-e2e.sh BUILD_DIR WEB_DIST_DIR      (run from the repo root)
set -euo pipefail
BUILD=$(cd "${1:?native build dir}" && pwd)
DIST=$(cd "${2:?web dist dir}" && pwd)
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kn-rtc-fail-XXXXXX")
BASE=$((27000 + RANDOM % 1500))
GAME=$BASE HTTP=$((BASE + 1)) CDPP=$((BASE + 2)) SIG1=$((BASE + 3)) ICE1=$((BASE + 4)) SIG2=$((BASE + 5)) ICE2=$((BASE + 6))
pids=()
cleanup() {
    for pid in "${pids[@]}"; do kill -CONT "$pid" 2>/dev/null || true; pkill -TERM -P "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    [ "${KN_TEST_KEEP:-0}" = 1 ] && echo "[rtc-fail] kept $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT

(cd net-bridge/rtc && go build -trimpath -o kn-rtc .)
"$BUILD/server_bin" --headless --cfg none --port "$GAME" server/tests/arena.json > "$WORK/game.log" 2>&1 < /dev/null &
SERVER_PID=$!
pids+=($SERVER_PID)
# gateway 1 allows only another port: every offer for the game server is refused
KRUNK_NATIVE_PORT=$GAME KRUNK_NATIVE_TARGETS=127.0.0.1:$((GAME + 50)) net-bridge/rtc/kn-rtc -http "127.0.0.1:$SIG1" -ice-addr ":$ICE1" \
    > "$WORK/rtc-refuse.log" 2>&1 &
pids+=($!)
KRUNK_NATIVE_PORT=$GAME KRUNK_NATIVE_RTC_IDLE_TIMEOUT_MS=3000 net-bridge/rtc/kn-rtc -http "127.0.0.1:$SIG2" -ice-addr ":$ICE2" \
    > "$WORK/rtc-idle.log" 2>&1 &
pids+=($!)
(cd net-bridge && KRUNK_NATIVE_PORT=$GAME KRUNK_NATIVE_BRIDGE_PORT=$HTTP KRUNK_NATIVE_WEB_DIR="$DIST" exec npm run web) > "$WORK/bridge.log" 2>&1 &
pids+=($!)
chromium-browser --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader --mute-audio \
    --user-data-dir="$WORK/chrome" --remote-debugging-port=$CDPP about:blank > "$WORK/chrome.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do grep -q "listening on http" "$WORK/bridge.log" 2>/dev/null && break; sleep 0.2; done

status=0
node tools/kref/rtc-fail-e2e.js $CDPP "http://127.0.0.1:$HTTP/?mode=offline&ws=ws://127.0.0.1:$HTTP/native&transport=rtc" \
    "http://127.0.0.1:$SIG1/rtc" "http://127.0.0.1:$SIG2/rtc" $SERVER_PID || status=$?
echo "[rtc-fail] gateway logs:"; grep -h "refused\|closed:" "$WORK/rtc-refuse.log" "$WORK/rtc-idle.log" | sed 's/^/  /' | tail -n 4
[ $status = 0 ] && echo "[rtc-fail] PASS" || { echo "[rtc-fail] FAIL ($status)"; exit 1; }
