#!/usr/bin/env bash
# Web hot loader e2e + timings (doc/web-hot-loader.md): kn_brain (scratch SQLite) + a brain-linked
# server_bin + net-bridge/rtc (WebRTC gateway, rtc.json) + net-bridge --web serving a copy of the web
# build (its /mm/* proxied to the brain) + headless Chromium. Each run loads the game page, clicks to
# play once the menu offers it and prints the time from navigation to each step.
#
#   tools/kref/hot-e2e.sh BUILD_DIR WEB_DIST_DIR [runs]      (run from the repo root)
#
# For each of the hot loader on and off (?nohot), a cold load (a fresh browser profile) and then
# [runs] warm loads (the same profile: the HTTP cache). Passes when every hot run took the page's
# seek and its WebRTC channel and got in game, and every run got in game.
set -euo pipefail
BUILD=$(cd "${1:?native build dir}" && pwd)
DIST_SRC=$(cd "${2:?web dist dir}" && pwd)
RUNS=${3:-2}
ROOT=$(pwd)
WORK=$(mktemp -d "$HOME/.cache/kn-hot-e2e-XXXXXX")
BASE=$((26000 + RANDOM % 1500))
GAME=$BASE ICE=$((BASE + 1)) HTTP=$((BASE + 2)) CDPP=$((BASE + 3)) SIG=$((BASE + 4))
SECRET="kn-hot-e2e-$$"
pids=()
cleanup() {
    for pid in "${pids[@]}"; do pkill -TERM -P "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    python3 -I services/brain/tests/db_scratch.py "$ROOT" drop sqlite "kn_hot_e2e_$$" >/dev/null 2>&1 || true
    [ "${KN_TEST_KEEP:-0}" = 1 ] && echo "[hot-e2e] kept $WORK" || rm -rf "$WORK"
}
trap cleanup EXIT

# the page tree is copied (rtc.json is written next to it), the content stays linked
mkdir -p "$WORK/web"
cp -a "$DIST_SRC/." "$WORK/web/"
DIST="$WORK/web"

db=$(python3 -I services/brain/tests/db_scratch.py "$ROOT" create sqlite "kn_hot_e2e_$$")
eval "$(python3 -I -c 'import json,sys; [print("export KN_DATABASE_%s=%s" % (k.upper(), json.dumps(str(v)))) for k, v in json.loads(sys.argv[1]).items()]' "$db")"
export KN_BRAIN_INTERNAL_SECRET=$SECRET
"$BUILD/services/brain/kn_brain" --migrate-only > "$WORK/migrate.log" 2>&1
"$BUILD/services/brain/kn_brain" --brain.host=127.0.0.1 --brain.port=0 --brain.region=local --brain.id="hot-e2e-$$" \
    > "$WORK/brain.log" 2>&1 < /dev/null &
pids+=($!)
for _ in $(seq 100); do grep -qE 'listening on http://127\.0\.0\.1:[0-9]+ \(ws' "$WORK/brain.log" && break; sleep 0.1; done
BRAIN=$(grep -oE 'listening on http://127\.0\.0\.1:[0-9]+' "$WORK/brain.log" | head -1 | grep -oE '[0-9]+$')
[ -n "$BRAIN" ] || { echo "[hot-e2e] kn_brain did not start"; tail -5 "$WORK/brain.log"; exit 1; }

cat > "$WORK/node.json" <<JSON
{"gameserver": {"node_id": "HOT", "name": "hot e2e", "region": "local", "listen_port": $GAME, "public_host": "127.0.0.1",
 "brain_url": "ws://127.0.0.1:$BRAIN/mm/internal/ws", "brain_secret": "$SECRET", "modes": ["ffa"], "maps": ["Burg"],
 "max_players": 8, "idle_kick_seconds": 0, "idle_menu_kick_seconds": 0}}
JSON
"$BUILD/server_bin" --headless --config "$WORK/node.json" --cfg none > "$WORK/game.log" 2>&1 < /dev/null &
pids+=($!)
[ -x net-bridge/rtc/kn-rtc ] || (cd net-bridge/rtc && go build -trimpath -o kn-rtc .)
KRUNK_NATIVE_PORT=$GAME net-bridge/rtc/kn-rtc -http "127.0.0.1:$SIG" -ice-addr ":$ICE" -info-file "$DIST/rtc.json" > "$WORK/rtc.log" 2>&1 &
pids+=($!)
(cd net-bridge && KRUNK_NATIVE_PORT=$GAME KRUNK_NATIVE_BRIDGE_PORT=$HTTP KRUNK_NATIVE_WEB_DIR="$DIST" KN_BRAIN_URL="http://127.0.0.1:$BRAIN" \
    exec npm run web) > "$WORK/bridge.log" 2>&1 &
pids+=($!)
for _ in $(seq 150); do
    grep -q "listening on http" "$WORK/bridge.log" 2>/dev/null && [ -f "$DIST/rtc.json" ] && grep -q welcomed "$WORK/game.log" && break
    sleep 0.2
done
grep -q welcomed "$WORK/game.log" || { echo "[hot-e2e] the node did not link to the brain"; tail -5 "$WORK/game.log"; exit 1; }

status=0
results="$WORK/results.jsonl"
browse() { # <label> <query> <warm runs>: a fresh profile, one cold load, then warm ones
    local label=$1 query=$2 warm=$3
    rm -rf "$WORK/chrome"
    chromium-browser --headless=new --no-sandbox --use-gl=angle --use-angle=swiftshader --enable-unsafe-swiftshader \
        --autoplay-policy=no-user-gesture-required --user-data-dir="$WORK/chrome" --remote-debugging-port=$CDPP about:blank \
        > "$WORK/chrome-$label.log" 2>&1 &
    local chrome=$!
    for _ in $(seq 50); do curl -s "http://127.0.0.1:$CDPP/json/version" >/dev/null 2>&1 && break; sleep 0.1; done
    local url="http://127.0.0.1:$HTTP/$query"
    KN_E2E_VERBOSE=${KN_E2E_VERBOSE:-} node tools/kref/hot-e2e.js $CDPP "$url" 90 "$label-cold" | tee -a "$WORK/runs.log" || status=1
    for i in $(seq "$warm"); do node tools/kref/hot-e2e.js $CDPP "$url" 90 "$label-warm$i" | tee -a "$WORK/runs.log" || status=1; done
    kill "$chrome" 2>/dev/null || true
    wait "$chrome" 2>/dev/null || true
}
browse hot "" "$RUNS"
browse nohot "?nohot=1" "$RUNS"
grep -E '^\{' "$WORK/runs.log" > "$results" || true

python3 -I - "$results" <<'PY' || status=1
import json, sys
runs = [json.loads(l) for l in open(sys.argv[1])]
ok = True
print("%-12s %8s %8s %8s %8s %8s" % ("run", "answer", "taken", "connect", "click", "ingame"))
for r in runs:
    ms, hot = r["ms"], r.get("hot") or {}
    t = hot.get("times", {})
    print("%-12s %8s %8s %8s %8s %8s" % (r["label"], t.get("answer", "-"), ms.get("hotTaken") or "-", ms.get("connected") or "-",
                                         ms.get("clickToPlay", "-"), ms.get("inGame", "-")))
    if r["label"].startswith("hot"):
        rtc = hot.get("rtc") or {}
        if not (hot.get("used") and ms.get("hotTaken") and rtc.get("adopted") and ms.get("rtcAdopted")):
            print("  FAIL: %s did not take the hot seek and its channel: %s" % (r["label"], hot)); ok = False
    elif hot:
        print("  FAIL: %s ran the hot loader with ?nohot" % r["label"]); ok = False
    if ms.get("inGame") is None:
        print("  FAIL: %s never got in game" % r["label"]); ok = False
sys.exit(0 if ok else 1)
PY
echo "[hot-e2e] brain log:"; { grep -E 'hot-seek|seek' "$WORK/brain.log" || true; } | tail -n 4 | sed 's/^/  /'
[ $status = 0 ] && echo "[hot-e2e] PASS" || { echo "[hot-e2e] FAIL"; KN_TEST_KEEP=1; exit 1; }
