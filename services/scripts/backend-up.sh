#!/usr/bin/env bash
# Local backend: migrate the database, start one brain and $GAMESERVERS game servers,
# stop everything on ctrl-c. Run from the repository root (make backend-up).
# Every service reads config/config.json (+ KN_* env, + --set a.b=c); extra arguments
# are passed to every service. Logs: $LOG_DIR (default /tmp/krunknative-backend).
set -euo pipefail

BUILD_DIR=${BUILD_DIR:-cmake-build-debug}
GAMESERVERS=${GAMESERVERS:-2}
LOG_DIR=${LOG_DIR:-/tmp/krunknative-backend}
mkdir -p "$LOG_DIR"

pids=()
cleanup() {
    trap - INT TERM EXIT
    for pid in "${pids[@]}"; do kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
}
trap cleanup INT TERM EXIT

echo "[backend-up] migrating"
"$BUILD_DIR/services/common/kn_migrate" "$@"

brain="$BUILD_DIR/services/brain/kn_brain"
if [ -x "$brain" ]; then
    echo "[backend-up] brain -> $LOG_DIR/brain.log"
    "$brain" "$@" > "$LOG_DIR/brain.log" 2>&1 &
    pids+=($!)
    sleep 1
else
    echo "[backend-up] kn_brain not built yet (services/brain); skipping"
fi

# Game servers: one per index, ports from gameserver.port + i (owner: gameserver agent
# wires the brain client and the exact flags; see doc/backend.md).
server="$BUILD_DIR/server_bin"
for ((i = 0; i < GAMESERVERS; i++)); do
    [ -x "$server" ] || { echo "[backend-up] $server missing"; break; }
    echo "[backend-up] game server $i -> $LOG_DIR/game$i.log"
    # KN_GAMESERVER_INDEX: port + i, node id <node_id><i> (doc/backend.md section 8.1);
    # BRAIN_URL_FALLBACK=auto links to this config's brain unless gameserver.brain_url is set
    KN_GAMESERVER_INDEX=$i KN_GAMESERVER_BRAIN_URL_FALLBACK=${KN_GAMESERVER_BRAIN_URL_FALLBACK:-auto} \
        "$server" --headless "$@" > "$LOG_DIR/game$i.log" 2>&1 < /dev/null &
    pids+=($!)
done

echo "[backend-up] running (${#pids[@]} processes); ctrl-c to stop"
wait -n "${pids[@]}" 2>/dev/null || true
echo "[backend-up] a service exited; stopping the rest"
