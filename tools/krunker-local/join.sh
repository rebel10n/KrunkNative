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
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
HOME_DIR="${KN_JOIN_HOME:-$HOME/.cache/kn-v10-join}"
BUILD="$REPO/cmake-build-debug"
PROFILE="${1:-join}"
mkdir -p "$HOME_DIR/run" && umask 077

# the secret for the brain and the server (generated once)
[ -s "$HOME_DIR/run/brain.secret" ] || openssl rand -hex 16 > "$HOME_DIR/run/brain.secret"
SECRET="$(cat "$HOME_DIR/run/brain.secret")"

# the harness config: the example with this machine's dirs and the ports the join uses (git-ignored)
node -e '
const fs = require("fs"); const home = process.argv[1];
const c = JSON.parse(fs.readFileSync(process.argv[2] + "/tools/krunker-local/config.example.json", "utf8"));
c.wwwDir = home + "/krunker/www"; c.runDir = home + "/krunker/run";
c.port = 8194; c.tlsPort = 8543; c.localUrl = "http://krunker-local.test:8194/";
c.bridge.log = home + "/run/bridge.log"; c.brainUrl = "http://127.0.0.1:5150";
c.loadWaitMs = 20000; c.afterAcceptMs = 6000; c.joinWaitMs = 60000;
fs.writeFileSync(process.argv[2] + "/tools/krunker-local/config.json", JSON.stringify(c, null, 2) + "\n");' "$HOME_DIR" "$REPO"

up() { ss -ltn | grep -q "$1" ; }
start() { # start NAME PIDFILE PORT CMD...
  local name=$1 pidf=$2 port=$3; shift 3
  if up ":$port "; then echo "$name: already listening on $port"; return; fi
  (cd "$REPO" && setsid nohup "$@" > "$HOME_DIR/run/$name.out" 2>&1 & echo $! > "$pidf")
  for _ in $(seq 1 60); do up ":$port " && break; sleep 0.5; done
  up ":$port " && echo "$name: up on $port" || { echo "$name: did not listen on $port (see $HOME_DIR/run/$name.out)"; exit 1; }
}

export KN_BRAIN_INTERNAL_SECRET="$SECRET"
start brain "$HOME_DIR/run/brain.pid" 5150 "$BUILD/services/brain/kn_brain" --brain.host=127.0.0.1 --brain.port=5150 \
  --database.driver=sqlite --database.path="$HOME_DIR/run/brain.db" --migrate --brain.region=local --brain.id=v10join \
  --accounts.signup_rewards.enabled=false --finder.poll_interval_s=1
start server "$HOME_DIR/run/server.pid" 21115 env KN_GAMESERVER_BRAIN_SECRET="$SECRET" "$BUILD/server_bin" --cfg none \
  --config "$HOME_DIR/run/empty.json" --headless --no-cli --port 21115 --mode ffa --region local --node-id V10J \
  --name "Krunker Box Office v10join" --public-host 127.0.0.1 --public-port 21115 \
  --brain ws://127.0.0.1:5150/mm/internal/ws --log "$HOME_DIR/run/server.log" "$REPO/assets/maps/official/00_burg.json"
[ -s "$HOME_DIR/run/empty.json" ] || echo '{}' > "$HOME_DIR/run/empty.json"
start bridge "$HOME_DIR/run/bridge.pid" 8097 env KRUNK_NATIVE_V10_TRACE=1 "$BUILD/v10_bridge" --v10-listen=8097 \
  --bind=127.0.0.1 --native=127.0.0.1:21115

cd "$REPO" && node tools/krunker-local/krunker-local.mjs prepare > /dev/null || { echo "prepare failed"; exit 1; }
start harness "$HOME_DIR/run/serve.pid" 8194 node tools/krunker-local/krunker-local.mjs serve

KN_JOIN_PROFILE="$PROFILE" node tools/krunker-local/krunker-local.mjs join > "$HOME_DIR/run/join-$PROFILE.json" 2> "$HOME_DIR/run/join-$PROFILE.err"
node -e '
const r = require(process.argv[1]);
console.log("framesIn " + r.framesIn + " framesOut " + r.framesOut + " seek " + JSON.stringify(r.seek.map((x) => x.slice(0, 4))) + " sockets " + r.sockets.length + " exceptions " + r.exceptions.length);
console.log("game " + JSON.stringify(r.game && r.game.exposed) + " text " + JSON.stringify(r.state && r.state.text).slice(0, 120));
console.log("screenshots " + (r.shots || []).join(" "));' "$HOME_DIR/run/join-$PROFILE.json"
