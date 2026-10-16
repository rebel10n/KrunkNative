#!/usr/bin/env bash
# Web e2e stack: kn_brain (sqlite, random port) + server_bin (one offline-capable node that lists
# itself with the brain) + net-bridge --web (serves the web dist, proxies /mm /api /social /ai /auth
# /streams /creators /user-assets to the brain, /native to the node) + headless Chromium with a
# remote-debugging port. Writes its ports to $RUN/env (BRAIN GAME BRIDGE CDPP RUN) and runs until
# killed or until its parent script exits.
#
#   KN_WEB_BUILD=<native build dir> KN_WEB_DIST=<web dist dir> tools/kref/web-session/stack.sh [RUN_DIR]
#
# KN_WEB_BUILD must hold services/brain/kn_brain and server_bin (make, or cmake --build ... --target
# client_bin server_bin kn_brain); KN_WEB_DIST is `make web`'s dist/web (or its build's dist dir).
# net-bridge/node_modules must exist (make bridge-install). Run from anywhere.
set -euo pipefail
ROOT=$(cd "$(dirname "$0")/../../.." && pwd)
B=$(realpath "${KN_WEB_BUILD:-$ROOT/cmake-build-debug}" 2>/dev/null || echo "${KN_WEB_BUILD:-}")
DIST=$(realpath "${KN_WEB_DIST:-$ROOT/dist/web}" 2>/dev/null || echo "${KN_WEB_DIST:-}")
RUN=$(realpath -m "${1:-${KN_WEB_RUN:-$(mktemp -d /tmp/kn-web-session-XXXXXX)}}")
CHROME=${KN_WEB_CHROME:-$(command -v chromium-browser || command -v chromium || command -v google-chrome || true)}
[ -x "$B/server_bin" ] && [ -x "$B/services/brain/kn_brain" ] || { echo "stack: no server_bin/kn_brain in $B (build them first)" >&2; exit 2; }
[ -f "$DIST/client.wasm" ] || { echo "stack: no web build in $DIST (make web)" >&2; exit 2; }
[ -d "$ROOT/net-bridge/node_modules" ] || { echo "stack: run make bridge-install first" >&2; exit 2; }
[ -n "$CHROME" ] || { echo "stack: needs chromium or google-chrome" >&2; exit 2; }
mkdir -p "$RUN"; rm -f "$RUN/env"

read -r BRAIN GAME BRIDGE CDPP < <(python3 -I -c 'import socket
s=[socket.socket() for _ in range(4)]
[x.bind(("127.0.0.1",0)) for x in s]
print(*[x.getsockname()[1] for x in s])')
SECRET="webses-$$-$RANDOM"
pids=()
cleanup() {
    for p in "${pids[@]}"; do pkill -TERM -P "$p" 2>/dev/null || true; kill "$p" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
trap cleanup EXIT

KN_BRAIN_INTERNAL_SECRET="$SECRET" "$B/services/brain/kn_brain" --brain.host=127.0.0.1 --brain.port="$BRAIN" --database.driver=sqlite \
    --database.path="$RUN/brain.db" --migrate --brain.region=local --brain.id=websession \
    --accounts.signup_rewards.enabled=false --finder.poll_interval_s=1 > "$RUN/brain.log" 2>&1 &
pids+=($!)
for _ in $(seq 1 40); do curl -sf "http://127.0.0.1:$BRAIN/mm/status" >/dev/null 2>&1 && break; sleep 0.25; done

cat > "$RUN/node.json" <<J
{"gameserver": {"node_id": "WEB1", "name": "web session node", "region": "local", "listen_port": $GAME,
 "public_host": "127.0.0.1", "brain_url": "ws://127.0.0.1:$BRAIN/mm/internal/ws", "brain_secret": "$SECRET",
 "modes": ["ffa"], "max_players": 8, "shutdown_grace": 0, "idle_kick_seconds": 0, "ruleset": "v10",
 "finder_url": "http://127.0.0.1:$BRAIN"}}
J
"$B/server_bin" --headless --config "$RUN/node.json" --cfg none > "$RUN/node.log" 2>&1 &
pids+=($!)

(cd "$ROOT/net-bridge" && KRUNK_NATIVE_PORT=$GAME KRUNK_NATIVE_BRIDGE_PORT=$BRIDGE KRUNK_NATIVE_WEB_DIR="$DIST" \
    exec npm run --silent web -- --brain "http://127.0.0.1:$BRAIN") > "$RUN/bridge.log" 2>&1 &
pids+=($!)
for _ in $(seq 1 80); do curl -sf "http://127.0.0.1:$BRIDGE/healthz" >/dev/null 2>&1 && break; sleep 0.25; done

"$CHROME" --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
    --autoplay-policy=no-user-gesture-required --window-size=1280,720 \
    --user-data-dir="$RUN/chrome" --remote-debugging-port="$CDPP" about:blank > "$RUN/chrome.log" 2>&1 &
pids+=($!)

printf 'BRAIN=%s GAME=%s BRIDGE=%s CDPP=%s RUN=%s\n' "$BRAIN" "$GAME" "$BRIDGE" "$CDPP" "$RUN" > "$RUN/env"
echo "stack up: $(cat "$RUN/env")"
wait
