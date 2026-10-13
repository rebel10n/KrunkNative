#!/usr/bin/env bash
# KrunkNative local backend: PostgreSQL migrations + kn_brain + N game servers
# (+ net-bridge --web when dist/web exists), all in the background.
#
#   deploy/local/backend.sh up      (make backend-up)
#   deploy/local/backend.sh down    (make backend-down)
#   deploy/local/backend.sh status  (make backend-status)
#   deploy/local/backend.sh logs
#
# Run from the repository root. Every service reads config/config.json (database
# credentials, "brain" and "gameserver" sections) plus KN_* env overrides.
#
# Environment:
#   BUILD_DIR=cmake-build-debug   where kn_brain / server_bin / kn_fakenode are
#   GAMESERVERS=2                 number of game servers
#   BRAIN_PORT=5050               brain listen port (127.0.0.1)
#   GAME_PORT=21015               first game server port (then +1, +2, ...)
#   REGION=local                  region of the brain and its game servers
#   BRIDGE=auto|0|1               net-bridge --web on BRIDGE_PORT (auto: if dist/web exists)
#   BRIDGE_PORT=8080
#   FAKE_NODES=auto|0|1           use kn_fakenode instead of server_bin (auto: when
#                                 server_bin has no brain client yet)
#   KN_RUN_DIR                    pid/log dir (default $XDG_RUNTIME_DIR/krunknative-backend)
#   BACKEND_ARGS                  extra args for kn_brain and server_bin (e.g. --set x=y)
set -euo pipefail

BUILD_DIR=${BUILD_DIR:-cmake-build-debug}
GAMESERVERS=${GAMESERVERS:-2}
BRAIN_PORT=${BRAIN_PORT:-5050}
GAME_PORT=${GAME_PORT:-21015}
REGION=${REGION:-local}
BRIDGE=${BRIDGE:-auto}
BRIDGE_PORT=${BRIDGE_PORT:-8080}
FAKE_NODES=${FAKE_NODES:-auto}
RUN_DIR=${KN_RUN_DIR:-${XDG_RUNTIME_DIR:-/tmp}/krunknative-backend}
read -r -a EXTRA <<< "${BACKEND_ARGS:-}"

BRAIN_BIN="$BUILD_DIR/services/brain/kn_brain"
FAKE_BIN="$BUILD_DIR/services/brain/kn_fakenode"
SERVER_BIN="$BUILD_DIR/server_bin"
BRAIN_URL="ws://127.0.0.1:$BRAIN_PORT/mm/internal/ws"

say() { echo "[backend] $*"; }

pid_alive() { [ -f "$1" ] && kill -0 "$(cat "$1")" 2>/dev/null; }

start() { # name, command...
    local name=$1; shift
    if pid_alive "$RUN_DIR/$name.pid"; then
        say "$name already running (pid $(cat "$RUN_DIR/$name.pid"))"
        return
    fi
    "$@" > "$RUN_DIR/$name.log" 2>&1 < /dev/null &
    echo $! > "$RUN_DIR/$name.pid"
    say "$name started (pid $!, log $RUN_DIR/$name.log)"
}

stop() { # pidfile
    local f=$1 name pid
    name=$(basename "$f" .pid)
    pid=$(cat "$f" 2>/dev/null || true)
    if [ -n "$pid" ] && kill -0 "$pid" 2>/dev/null; then
        kill -TERM "$pid" 2>/dev/null || true
        for _ in $(seq 1 30); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
        kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null || true
        say "$name stopped"
    fi
    rm -f "$f"
}

secret() {
    # for kn_fakenode stand-ins: kn_brain and server_bin read brain.internal_secret /
    # gameserver.brain_secret from config.json (both default to "kn-dev-secret")
    echo "${KN_BRAIN_INTERNAL_SECRET:-${KN_GAMESERVER_BRAIN_SECRET:-kn-dev-secret}}"
}

server_has_brain_client() {
    [ -x "$SERVER_BIN" ] && "$SERVER_BIN" --help 2>&1 | grep -q -- "--brain"
}

wait_http() { # url, seconds
    for _ in $(seq 1 $(( $2 * 10 ))); do
        curl -fs -o /dev/null "$1" 2>/dev/null && return 0
        sleep 0.1
    done
    return 1
}

cmd_up() {
    mkdir -p "$RUN_DIR"
    [ -x "$BRAIN_BIN" ] || { say "missing $BRAIN_BIN (make services)"; exit 1; }
    say "migrating database"
    "$BRAIN_BIN" --migrate-only "${EXTRA[@]}"

    start brain "$BRAIN_BIN" --brain.host=127.0.0.1 --brain.port="$BRAIN_PORT" --brain.region="$REGION" \
        --brain.public_url="http://127.0.0.1:$BRAIN_PORT" "${EXTRA[@]}"
    wait_http "http://127.0.0.1:$BRAIN_PORT/ping" 10 || { say "brain did not come up:"; tail -20 "$RUN_DIR/brain.log"; exit 1; }

    local fake=$FAKE_NODES
    if [ "$fake" = auto ]; then
        if server_has_brain_client; then fake=0; else fake=1; fi
        [ "$fake" = 1 ] && say "server_bin has no brain client (yet): using kn_fakenode stand-ins"
    fi
    local use_bridge=$BRIDGE
    [ "$use_bridge" = auto ] && { [ -f dist/web/index.html ] && [ -d net-bridge/node_modules ] && use_bridge=1 || use_bridge=0; }

    local targets=""
    for ((i = 0; i < GAMESERVERS; i++)); do
        local port=$((GAME_PORT + i)) node="local-$((i + 1))"
        local web=""
        [ "$use_bridge" = 1 ] && web="ws://127.0.0.1:$BRIDGE_PORT/native?target=127.0.0.1:$port"
        targets+="${targets:+,}127.0.0.1:$port"
        if [ "$fake" = 1 ]; then
            start "game$i" "$FAKE_BIN" --brain "$BRAIN_URL" --secret "$(secret)" --node "$node" --region "$REGION" \
                --host 127.0.0.1 --port "$port" --custom ${web:+--web-url "$web"} --quiet
        else
            start "game$i" "$SERVER_BIN" --headless --port "$port" --node-id "$node" --region "$REGION" \
                --name "Krunker Box Office $node" --public-host 127.0.0.1 --public-port "$port" --brain "$BRAIN_URL" \
                ${web:+--gameserver.web_url="$web"} "${EXTRA[@]}"
        fi
    done

    if [ "$use_bridge" = 1 ]; then
        start bridge env KRUNK_NATIVE_BRIDGE_HOST=127.0.0.1 KRUNK_NATIVE_BRIDGE_PORT="$BRIDGE_PORT" \
            KRUNK_NATIVE_HOST=127.0.0.1 KRUNK_NATIVE_PORT="$GAME_PORT" KRUNK_NATIVE_TARGETS="$targets" KRUNK_NATIVE_ALLOW_LOOPBACK=1 \
            KRUNKNATIVE_BRAIN_URL="http://127.0.0.1:$BRAIN_PORT" KN_BRAIN_URL="http://127.0.0.1:$BRAIN_PORT" \
            bash -c 'cd net-bridge && exec node -r ts-node/register/transpile-only index.ts --web'
    else
        say "net-bridge skipped (needs dist/web from 'make web' and net-bridge/node_modules; BRIDGE=1 forces)"
    fi
    sleep 1.5
    cmd_status
}

cmd_down() {
    [ -d "$RUN_DIR" ] || { say "nothing running"; return; }
    # game servers and bridge first, the brain last (nodes send lobby-down/drain)
    for f in "$RUN_DIR"/bridge.pid "$RUN_DIR"/game*.pid "$RUN_DIR"/brain.pid; do
        [ -f "$f" ] && stop "$f"
    done
    say "down"
}

cmd_status() {
    for f in "$RUN_DIR"/*.pid; do
        [ -f "$f" ] || continue
        local name; name=$(basename "$f" .pid)
        if pid_alive "$f"; then echo "  $name: running (pid $(cat "$f"))"; else echo "  $name: DEAD (see $RUN_DIR/$name.log)"; fi
    done
    if curl -fs "http://127.0.0.1:$BRAIN_PORT/mm/status" 2>/dev/null; then
        echo
        curl -fs "http://127.0.0.1:$BRAIN_PORT/mm/game-list" 2>/dev/null | head -c 2000; echo
    else
        echo "  brain not answering on http://127.0.0.1:$BRAIN_PORT"
    fi
}

cmd_logs() { tail -n 30 "$RUN_DIR"/*.log; }

case "${1:-up}" in
    up) cmd_up ;;
    down) cmd_down ;;
    status) cmd_status ;;
    logs) cmd_logs ;;
    *) echo "usage: $0 up|down|status|logs" >&2; exit 2 ;;
esac
