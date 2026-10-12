#!/usr/bin/env bash
# Web WebRTC e2e (doc/transport.md): server_bin + net-bridge/rtc (WebRTC gateway, URL published
# in rtc.json) + net-bridge --web serving a web build + headless Chromium. Passes when the page's
# session came up over the WebRTC DataChannel (knet), not a fallback, and the page got in game.
#
#   tools/kref/rtc-e2e.sh BUILD_DIR WEB_DIST_DIR [seconds]      (run from the repo root)
#   KN_RTC_DROP=15        gateway loss simulation, percent dropped in each direction
#   KN_UDP_LOSS="0 5 5"   drop/dup/reorder on the server's receive side (as udp_e2e / wt-e2e)
#   KN_TEST_MAP=map.json  another map (streamed over the reliable channel: fragmentation + resends)
set -euo pipefail
BUILD=${1:?native build dir}
DIST=${2:?web dist dir}
SECONDS_RUN=${3:-30}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kn-rtc-e2e-XXXXXX")
BASE=$((24000 + RANDOM % 1500))
GAME=$BASE ICE=$((BASE + 1)) HTTP=$((BASE + 2)) CDPP=$((BASE + 3)) SIG=$((BASE + 4))
read -r DROP DUP REORDER <<< "${KN_UDP_LOSS:-0 0 0}"
pids=()
cleanup() {
    for pid in "${pids[@]}"; do pkill -TERM -P "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -f "$DIST/rtc.json"
    [ "${KN_TEST_KEEP:-0}" = 1 ] && echo "[rtc-e2e] kept $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT

[ -x net-bridge/rtc/kn-rtc ] || (cd net-bridge/rtc && go build -trimpath -o kn-rtc .)
# the browser's ICE checks come from a LAN/host address, so the gateway listens on every
# interface (loopback included: dev, KRUNK_NATIVE_TARGETS unset)
KRUNKNATIVE_NET_DROP=$DROP KRUNKNATIVE_NET_DUP=$DUP KRUNKNATIVE_NET_REORDER=$REORDER \
    "$BUILD/server_bin" --headless --cfg none --port "$GAME" "${KN_TEST_MAP:-server/tests/arena.json}" > "$WORK/game.log" 2>&1 < /dev/null &
pids+=($!)
KRUNK_NATIVE_PORT=$GAME KRUNK_NATIVE_RTC_DROP=${KN_RTC_DROP:-0} net-bridge/rtc/kn-rtc -http "127.0.0.1:$SIG" -ice-addr ":$ICE" \
    -info-file "$DIST/rtc.json" > "$WORK/rtc.log" 2>&1 &
RTC_PID=$!
pids+=($RTC_PID)
(cd net-bridge && KRUNK_NATIVE_PORT=$GAME KRUNK_NATIVE_BRIDGE_PORT=$HTTP KRUNK_NATIVE_WEB_DIR="$DIST" exec npm run web) > "$WORK/bridge.log" 2>&1 &
pids+=($!)
chromium-browser --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
    --user-data-dir="$WORK/chrome" --remote-debugging-port=$CDPP about:blank > "$WORK/chrome.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do grep -q "listening on http" "$WORK/bridge.log" 2>/dev/null && [ -f "$DIST/rtc.json" ] && break; sleep 0.2; done

status=0
KN_E2E_REQUIRE_PLAY=1 node tools/kref/wt-e2e.js $CDPP "http://127.0.0.1:$HTTP/?mode=offline&ws=ws://127.0.0.1:$HTTP/native&transport=rtc" "$SECONDS_RUN" webrtc || status=$?
kill -TERM "$RTC_PID" 2>/dev/null || true # closes the sessions: their datagram counters
for _ in $(seq 20); do kill -0 "$RTC_PID" 2>/dev/null || break; sleep 0.1; done
echo "[rtc-e2e] gateway log:"; sed 's/^/  /' "$WORK/rtc.log" | tail -n 6
echo "[rtc-e2e] server log:"; grep -E -i "connected|bridged|Sending map|spawn|entered|joined|killed|disconnected" "$WORK/game.log" | sed 's/^/  /' | head -20
[ $status = 0 ] && echo "[rtc-e2e] PASS" || { echo "[rtc-e2e] FAIL ($status)"; exit 1; }
