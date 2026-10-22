#!/bin/bash
# The playability check of the official Krunker v10.0.3 client on the local stack, headless:
#   tools/krunker-local/playable.sh        (make playable-check builds the debug binaries first)
# Starts the brain, the game server, the v10 bridge and the harness page on free loopback ports, joins the official client
# in headless Chromium, plays one round with input, holds the frame counter for 60 s, waits for the end screen and the next
# round, then asserts (a)-(h) from the report (playable-check.mjs). Exit 0 only when every assertion passes.
# Env: KN_PLAYABLE_HOME (absolute; default ~/.cache/kn-playable-check: run dir, brain database, logs, pid files, Chromium
# profile), KN_PLAYABLE_ROUND_S (round length, default 90), KN_PLAYABLE_MAX_EXC (exception threshold, default 0),
# KN_BUILD (the CMake debug build dir, default <repo>/cmake-build-debug). Loopback only; nothing leaves the machine.
# Only the processes this script started are stopped: each has a pid file ($RUN/<name>.pid, "pid command-name"), and a pid
# is signalled only while /proc/<pid>/cmdline still names that command. Other stacks on this host are not touched.
set -u
REPO="$(cd "$(dirname "$0")/../.." && pwd)"
HOME_DIR="${KN_PLAYABLE_HOME:-$HOME/.cache/kn-playable-check}"
case "$HOME_DIR" in /*) ;; *) echo "KN_PLAYABLE_HOME must be an absolute path" >&2; exit 1 ;; esac
RUN="$HOME_DIR/run"
BUILD="${KN_BUILD:-$REPO/cmake-build-debug}"
ROUND_S="${KN_PLAYABLE_ROUND_S:-90}"
MAX_EXC="${KN_PLAYABLE_MAX_EXC:-0}"
cd "$REPO" || exit 1
mkdir -p "$RUN" && umask 077

# the ports this run uses: free on the host when picked, never reused from another stack
declare -A USED=()
busy() { ss -ltn | awk '{print $4}' | grep -q ":$1\$"; }
pick() { local p=$1; while busy "$p" || [ -n "${USED[$p]:-}" ]; do p=$((p + 1)); done; USED[$p]=1; PICK=$p; }

# stop what this script started (leftovers of an earlier run included): signal only pids of its own pid files whose
# command line still names the command
stop_ours() {
  local f pid bin
  for f in "$RUN"/*.pid; do
    [ -f "$f" ] || continue
    read -r pid bin < "$f"
    if [ -n "${pid:-}" ] && tr '\0' ' ' < "/proc/$pid/cmdline" 2>/dev/null | grep -q -- "$bin"; then
      kill "$pid" 2>/dev/null
      for _ in 1 2 3 4 5 6 7 8 9 10; do kill -0 "$pid" 2>/dev/null || break; sleep 0.5; done
      kill -0 "$pid" 2>/dev/null && kill -KILL "$pid" 2>/dev/null
    fi
    rm -f "$f"
  done
}
trap stop_ours EXIT
stop_ours

# start NAME COMMAND-NAME PORT CMD...: the command runs in the background with its output in $RUN/NAME.out; it is up when
# PORT listens
start() {
  local name=$1 bin=$2 port=$3 pid i
  shift 3
  "$@" > "$RUN/$name.out" 2>&1 &
  pid=$!
  echo "$pid $bin" > "$RUN/$name.pid"
  for i in $(seq 1 120); do
    kill -0 "$pid" 2>/dev/null || { echo "playable: $name exited (see $RUN/$name.out)" >&2; return 1; }
    busy "$port" && { echo "playable: $name up on $port"; return 0; }
    sleep 0.5
  done
  echo "playable: $name did not listen on $port (see $RUN/$name.out)" >&2
  return 1
}

for b in "$BUILD/services/brain/kn_brain" "$BUILD/server_bin" "$BUILD/v10_bridge"; do
  [ -x "$b" ] || { echo "playable: missing $b (run make build first)" >&2; exit 1; }
done

BASE=$((20000 + RANDOM % 25000))
pick "$BASE"; BRAIN=$PICK
pick $((BRAIN + 1)); SERVER=$PICK
pick $((SERVER + 1)); BRIDGE=$PICK
pick $((BRIDGE + 1)); HTTP=$PICK
pick $((HTTP + 1)); TLS=$PICK

# a fresh run: a new brain database and an empty server config; the brain secret is generated once, never printed
rm -f "$RUN/brain.db" "$RUN/brain.db-"* "$RUN/brain.db-journal"
echo '{}' > "$RUN/empty.json"
[ -s "$RUN/brain.secret" ] || openssl rand -hex 16 > "$RUN/brain.secret"
SECRET="$(cat "$RUN/brain.secret")"

# the harness config for this run (git-ignored): this checkout's client, this run's dirs and ports
KN_CFG_HOME="$HOME_DIR" KN_CFG_HTTP=$HTTP KN_CFG_TLS=$TLS KN_CFG_BRAIN=$BRAIN KN_CFG_BRIDGE=$BRIDGE node -e '
const fs = require("fs"); const e = process.env; const dir = process.argv[1];
const c = JSON.parse(fs.readFileSync(dir + "/tools/krunker-local/config.example.json", "utf8"));
c.wwwDir = e.KN_CFG_HOME + "/krunker/www"; c.runDir = e.KN_CFG_HOME + "/krunker/run";
c.port = +e.KN_CFG_HTTP; c.tlsPort = +e.KN_CFG_TLS; c.localUrl = "http://krunker-local.test:" + e.KN_CFG_HTTP + "/";
c.brainUrl = "http://127.0.0.1:" + e.KN_CFG_BRAIN;
c.bridge.port = +e.KN_CFG_BRIDGE; c.bridge.log = e.KN_CFG_HOME + "/run/bridge.out";
c.loadWaitMs = 20000; c.afterAcceptMs = 6000; c.joinWaitMs = 150000;
fs.writeFileSync(dir + "/tools/krunker-local/config.json", JSON.stringify(c, null, 2) + "\n");' "$REPO" || exit 1

export KN_BRAIN_INTERNAL_SECRET="$SECRET"
start brain kn_brain "$BRAIN" "$BUILD/services/brain/kn_brain" --brain.host=127.0.0.1 --brain.port="$BRAIN" \
  --database.driver=sqlite --database.path="$RUN/brain.db" --migrate --brain.region=local --brain.id=playable \
  --brain.public_url=https://matchmaker.beta.krunker.io \
  --accounts.signup_rewards.enabled=false --finder.poll_interval_s=1 || exit 1
start server server_bin "$SERVER" env KN_GAMESERVER_BRAIN_SECRET="$SECRET" "$BUILD/server_bin" --cfg none \
  --config "$RUN/empty.json" --headless --no-cli --port "$SERVER" --mode ffa --round-seconds "$ROUND_S" \
  --region us-nj --node-id PLAYABLE --name "Krunker Box Office playable" \
  --public-host 127.0.0.1 --public-port "$SERVER" \
  --brain "ws://127.0.0.1:$BRAIN/mm/internal/ws" --log "$RUN/server.log" "$REPO/assets/maps/official/00_burg.json" || exit 1
start bridge v10_bridge "$BRIDGE" env KRUNK_NATIVE_V10_TRACE=1 "$BUILD/v10_bridge" --v10-listen="$BRIDGE" \
  --bind=127.0.0.1 --native="127.0.0.1:$SERVER" || exit 1
node tools/krunker-local/krunker-local.mjs prepare > "$RUN/prepare.log" 2>&1 || { echo "playable: prepare failed ($RUN/prepare.log)" >&2; exit 1; }
start harness krunker-local.mjs "$HTTP" node tools/krunker-local/krunker-local.mjs serve || exit 1

# the join, the round and the report: one JSON document on stdout, progress on stderr
export NODE_OPTIONS="${NODE_OPTIONS:---max-old-space-size=12288}"
KN_ROUND_SECONDS="$ROUND_S" \
  timeout 1500 node tools/krunker-local/krunker-local.mjs playable > "$RUN/playable-report.json" 2> "$RUN/playable.log"
rc=$?
if [ $rc -ne 0 ]; then
  echo "playable: the harness failed (exit $rc, see $RUN/playable.log)" >&2
  exit 1
fi
node tools/krunker-local/playable-check.mjs "$RUN/playable-report.json" "$MAX_EXC"
