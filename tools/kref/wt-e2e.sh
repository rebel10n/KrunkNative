#!/usr/bin/env bash
# Web UDP e2e (doc/transport.md): server_bin + net-bridge/wt (WebTransport relay, self-signed
# cert published in wt.json) + net-bridge --web serving a web build + headless Chromium.
# Passes when the page's session came up over WebTransport (knet), not the WebSocket fallback.
#
#   tools/kref/wt-e2e.sh BUILD_DIR WEB_DIST_DIR [seconds]      (run from the repo root)
#   KN_UDP_LOSS="10 5 5"  loss simulation on the server's receive side
set -euo pipefail
# absolute: the bridge runs from net-bridge/, where a relative dist dir served nothing (no page, no session)
BUILD=$(cd "${1:?native build dir}" && pwd)
DIST=$(cd "${2:?web dist dir}" && pwd)
SECONDS_RUN=${3:-30}
WORK=$(mktemp -d /tmp/kn-wt-e2e-XXXXXX)
BASE=$((24000 + RANDOM % 1500))
GAME=$BASE WT=$((BASE + 1)) HTTP=$((BASE + 2)) CDPP=$((BASE + 3))
read -r DROP DUP REORDER <<< "${KN_UDP_LOSS:-0 0 0}"
pids=()
cleanup() {
    for pid in "${pids[@]}"; do pkill -TERM -P "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    rm -f "$DIST/wt.json"
    [ "${KN_TEST_KEEP:-0}" = 1 ] && echo "[wt-e2e] kept $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT

[ -x net-bridge/wt/kn-wt ] || (cd net-bridge/wt && go build -trimpath -o kn-wt .)

KRUNKNATIVE_NET_DROP=$DROP KRUNKNATIVE_NET_DUP=$DUP KRUNKNATIVE_NET_REORDER=$REORDER \
    "$BUILD/server_bin" --headless --cfg none --port "$GAME" server/tests/arena.json > "$WORK/game.log" 2>&1 < /dev/null &
pids+=($!)
KRUNK_NATIVE_PORT=$GAME net-bridge/wt/kn-wt -addr "127.0.0.1:$WT" -info-file "$DIST/wt.json" > "$WORK/wt.log" 2>&1 &
pids+=($!)
(cd net-bridge && KRUNK_NATIVE_PORT=$GAME KRUNK_NATIVE_BRIDGE_PORT=$HTTP KRUNK_NATIVE_WEB_DIR="$DIST" exec npm run web) > "$WORK/bridge.log" 2>&1 &
pids+=($!)
chromium-browser --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
    --user-data-dir="$WORK/chrome" --remote-debugging-port=$CDPP about:blank > "$WORK/chrome.log" 2>&1 &
pids+=($!)
for _ in $(seq 100); do grep -q "listening on http" "$WORK/bridge.log" 2>/dev/null && [ -f "$DIST/wt.json" ] && break; sleep 0.2; done

# ?server: a direct game server for the page (no brain matchmaking), like the native --connect
status=0
node tools/kref/wt-e2e.js $CDPP "http://127.0.0.1:$HTTP/?mode=offline&ws=ws://127.0.0.1:$HTTP/native&transport=wt" "$SECONDS_RUN" || status=$?
echo "[wt-e2e] relay log:"; sed 's/^/  /' "$WORK/wt.log" | tail -n 6
echo "[wt-e2e] server log:"; grep -E "connected|bridged|Sending map|disconnected" "$WORK/game.log" | sed 's/^/  /'
[ $status = 0 ] && echo "[wt-e2e] PASS" || { echo "[wt-e2e] FAIL ($status)"; exit 1; }
