#!/bin/bash
# One command for a v10 join through v10_bridge with the official client (headless Chromium, the harness):
#   tools/krunker-local/join.sh [JOIN_PROFILE]      (default profile: join)
#
# Starts what the join needs, if it is not already listening: the scratch kn_brain (sqlite, loopback :5150), the
# KrunkNative server registered with it (:21115, one lobby), v10_bridge (:8097) and the harness page server
# (:8194, TLS :8543). Then it runs the join and prints the outcome. Build first: `make` (or cmake --build cmake-build-debug
# --target server_bin v10_bridge kn_brain).
#
# Everything per-machine lives under $KN_JOIN_HOME (default ~/.cache/kn-v10-join): the brain database, the brain secret
# (a random value, mode 600, never printed or committed), the logs, the pid files, the krunker-local run and www dirs, and
# the Chromium profile. The generated tools/krunker-local/config.json is git-ignored.
# User scripts: KRUNKER_LOCAL_USER_SCRIPTS=/abs/a.js:/abs/b.js tools/krunker-local/join.sh (doc/v10/userscripts.md).
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
HOME_DIR="${KN_JOIN_HOME:-$HOME/.cache/kn-v10-join}"
BUILD="$REPO/cmake-build-debug"
PROFILE="${1:-join}"
# the ports (defaults are this join's); a run next to another join on the same host sets its own: a busy port is
# reused as "already listening", so a stale process from another build would answer the join
BRAIN_PORT="${KN_JOIN_BRAIN_PORT:-5150}"
SERVER_PORT="${KN_JOIN_SERVER_PORT:-21115}"
BRIDGE_PORT="${KN_JOIN_BRIDGE_PORT:-8097}"
HARNESS_PORT="${KN_JOIN_HARNESS_PORT:-8194}"
HARNESS_TLS_PORT="${KN_JOIN_HARNESS_TLS_PORT:-8543}"
# user scripts (doc/v10/userscripts.md): KRUNKER_LOCAL_USER_SCRIPTS is a colon-separated list of files, run in the page in
# the order given. Each must exist and be readable; this is checked before any service starts, and the paths are made
# absolute here because the node steps run from the repo.
if [ -n "${KRUNKER_LOCAL_USER_SCRIPTS:-}" ]; then
  USER_SCRIPTS=""
  IFS=: read -ra UPATHS <<< "$KRUNKER_LOCAL_USER_SCRIPTS"
  for f in "${UPATHS[@]}"; do
    [ -n "$f" ] || continue
    if [ ! -f "$f" ] || [ ! -r "$f" ]; then echo "join.sh: user script not found or not readable: $f" >&2; exit 1; fi
    USER_SCRIPTS="${USER_SCRIPTS:+$USER_SCRIPTS:}$(cd "$(dirname "$f")" && pwd -P)/$(basename "$f")"
  done
  export KRUNKER_LOCAL_USER_SCRIPTS="$USER_SCRIPTS"
fi
mkdir -p "$HOME_DIR/run" && umask 077

# the secret for the brain and the server (generated once)
[ -s "$HOME_DIR/run/brain.secret" ] || openssl rand -hex 16 > "$HOME_DIR/run/brain.secret"
SECRET="$(cat "$HOME_DIR/run/brain.secret")"

# the harness config: the example with this machine's dirs and the ports the join uses (git-ignored)
node -e '
const fs = require("fs"); const home = process.argv[1]; const [hp, tp, bp] = [process.argv[3], process.argv[4], process.argv[5]];
const c = JSON.parse(fs.readFileSync(process.argv[2] + "/tools/krunker-local/config.example.json", "utf8"));
c.wwwDir = home + "/krunker/www"; c.runDir = home + "/krunker/run";
c.port = +hp; c.tlsPort = +tp; c.localUrl = "http://krunker-local.test:" + hp + "/";
c.bridge.log = home + "/run/bridge.out"; c.brainUrl = "http://127.0.0.1:" + bp;
c.bridge.port = +process.argv[6];
c.loadWaitMs = 20000; c.afterAcceptMs = 6000; c.joinWaitMs = 60000;
fs.writeFileSync(process.argv[2] + "/tools/krunker-local/config.json", JSON.stringify(c, null, 2) + "\n");' "$HOME_DIR" "$REPO" \
  "$HARNESS_PORT" "$HARNESS_TLS_PORT" "$BRAIN_PORT" "$BRIDGE_PORT"

up() { ss -ltn | grep -q "$1" ; }
start() { # start NAME PIDFILE PORT CMD...
  local name=$1 pidf=$2 port=$3; shift 3
  if up ":$port "; then echo "$name: already listening on $port"; return; fi
  (cd "$REPO" && setsid nohup "$@" > "$HOME_DIR/run/$name.out" 2>&1 & echo $! > "$pidf")
  for _ in $(seq 1 60); do up ":$port " && break; sleep 0.5; done
  up ":$port " && echo "$name: up on $port" || { echo "$name: did not listen on $port (see $HOME_DIR/run/$name.out)"; exit 1; }
}

export KN_BRAIN_INTERNAL_SECRET="$SECRET"
[ -s "$HOME_DIR/run/empty.json" ] || echo '{}' > "$HOME_DIR/run/empty.json" # the server's --config (a fresh home has none)
start brain "$HOME_DIR/run/brain.pid" "$BRAIN_PORT" "$BUILD/services/brain/kn_brain" --brain.host=127.0.0.1 --brain.port="$BRAIN_PORT" \
  --database.driver=sqlite --database.path="$HOME_DIR/run/brain.db" --migrate --brain.region=local --brain.id=v10join \
  --accounts.signup_rewards.enabled=false --finder.poll_interval_s=1
start server "$HOME_DIR/run/server.pid" "$SERVER_PORT" env KN_GAMESERVER_BRAIN_SECRET="$SECRET" "$BUILD/server_bin" --cfg none \
  --config "$HOME_DIR/run/empty.json" --headless --no-cli --port "$SERVER_PORT" --mode ffa --round-seconds "${KN_ROUND_SECONDS:-30}" --region local --node-id V10J \
  --name "Krunker Box Office v10join" --public-host 127.0.0.1 --public-port "$SERVER_PORT" \
  --brain "ws://127.0.0.1:$BRAIN_PORT/mm/internal/ws" --log "$HOME_DIR/run/server.log" "$REPO/assets/maps/official/00_burg.json"
start bridge "$HOME_DIR/run/bridge.pid" "$BRIDGE_PORT" env KRUNK_NATIVE_V10_TRACE=1 "$BUILD/v10_bridge" --v10-listen="$BRIDGE_PORT" \
  --bind=127.0.0.1 --native="127.0.0.1:$SERVER_PORT"

cd "$REPO" && node tools/krunker-local/krunker-local.mjs prepare > /dev/null || { echo "prepare failed"; exit 1; }
start harness "$HOME_DIR/run/serve.pid" "$HARNESS_PORT" node tools/krunker-local/krunker-local.mjs serve

# the report of a join is tens of MB and the default V8 heap can end the process with no report: a larger heap unless set
export NODE_OPTIONS="${NODE_OPTIONS:---max-old-space-size=12288}"
KN_JOIN_PROFILE="$PROFILE" node tools/krunker-local/krunker-local.mjs join > "$HOME_DIR/run/join-$PROFILE.json" 2> "$HOME_DIR/run/join-$PROFILE.err"
node -e '
const r = require(process.argv[1]);
console.log("framesIn " + r.framesIn + " framesOut " + r.framesOut + " seek " + JSON.stringify(r.seek.map((x) => x.slice(0, 4))) + " sockets " + r.sockets.length + " exceptions " + r.exceptions.length);
console.log("game " + JSON.stringify(r.game && r.game.exposed) + " text " + JSON.stringify(r.state && r.state.text).slice(0, 120));
console.log("round end " + r.roundEnd + " seek before/after " + r.seekBeforeEnd + "/" + r.seekAfterEnd + " next " + JSON.stringify(r.nextRoundText).slice(0, 90));
console.log("screenshots " + (r.shots || []).join(" "));
if (r.userScripts) console.log("user scripts " + JSON.stringify(r.userScripts));' "$HOME_DIR/run/join-$PROFILE.json"
