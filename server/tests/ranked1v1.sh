#!/usr/bin/env bash
# Ranked 1v1 end to end (doc/backend.md 6.5 + 8.6): kn_brain's ranked queue (HTTP, like the
# client's Ranked window) matches two accounts, the brain hosts the match on server_bin
# (ranked lobby: roster only, teams, needAll), two headless native clients join it with
# their sessions and fight for real (KRUNKNATIVE_TEST_AIM + input script) until the mode's
# 10 kills, server_bin sends ranked-result and the brain writes the Elo change. Checks the
# DB: mm_ranked_matches closed, players.player_elo moved by +-delta, winner gained.
#
#   server/tests/ranked1v1.sh [BUILD_DIR]     (default cmake-build-debug; run from the repo root)
#
# Same needs and env as server/tests/gunfight.sh (KN_TEST_DB, KN_TEST_EGL, KN_TEST_KEEP).
set -euo pipefail

BUILD=${1:-cmake-build-debug}
DB=${KN_TEST_DB:-krunknative_ranked}
EGL=${KN_TEST_EGL:-mesa}
ROUND=${KN_TEST_ROUND:-150} # the ranked round ends at 10 kills; this is the cap
WORK=$(mktemp -d /tmp/kn-ranked-XXXXXX)
BRAIN_PORT=5252
GAME_PORT=21992
created_db=0
pids=()

cleanup() {
    # children first: client() subshells run `timeout client_bin` (timeout forwards TERM)
    for pid in "${pids[@]}"; do pkill -TERM -P "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    if [ "${KN_TEST_KEEP:-0}" != 1 ]; then
        [ "$created_db" = 1 ] && sudo -n -u postgres dropdb "$DB" 2>/dev/null || true
        rm -rf "$WORK"
    else
        echo "[ranked] kept $WORK and database $DB"
    fi
}
trap cleanup EXIT

for bin in server_bin client_bin services/brain/kn_brain services/common/kn_migrate; do
    [ -x "$BUILD/$bin" ] || { echo "[ranked] missing $BUILD/$bin"; exit 2; }
done

PGPASSWORD=$(python3 -I -c "import json;print(json.load(open('config/config.json'))['database'].get('password',''))")
export PGPASSWORD
PGHOST=$(python3 -I -c "import json;print(json.load(open('config/config.json'))['database'].get('host','127.0.0.1'))")
PGUSER=$(python3 -I -c "import json;print(json.load(open('config/config.json'))['database'].get('user','krunknative'))")
psql_q() { psql -h "$PGHOST" -U "$PGUSER" -d "$DB" -v ON_ERROR_STOP=1 -qAt "$@"; }

if [ -z "${KN_TEST_DB:-}" ]; then
    sudo -n -u postgres dropdb --if-exists "$DB"
    sudo -n -u postgres createdb -O "$PGUSER" "$DB"
    created_db=1
fi
"$BUILD/services/common/kn_migrate" --set database.name="$DB" > "$WORK/migrate.log" 2>&1

# two accounts with sessions (svc-accounts' kn_register: what a social-WS register does)
TA=$(openssl rand -hex 16)
TB=$(openssl rand -hex 16)
psql_q -c "SELECT kn_register('gunner_a', 'x', '127.0.0.1', '', 0, 0, '$TA', 3600, 'ranked')" > /dev/null
psql_q -c "SELECT kn_register('gunner_b', 'x', '127.0.0.1', '', 0, 0, '$TB', 3600, 'ranked')" > /dev/null

"$BUILD/services/brain/kn_brain" --set database.name="$DB" --set brain.port=$BRAIN_PORT --set brain.region=local \
    --set brain.ranked.min_level=0 --set 'brain.ranked.maps=["GS Arena"]' --set brain.ranked.start_grace_s=90 \
    > "$WORK/brain.log" 2>&1 &
pids+=($!)
sleep 1

cat > "$WORK/gs.json" <<EOF
{"gameserver": {"node_id": "RK", "name": "ranked", "region": "local", "listen_port": $GAME_PORT,
  "brain_url": "ws://127.0.0.1:$BRAIN_PORT/mm/internal/ws", "modes": ["ffa"], "round_seconds": $ROUND, "custom_games": true,
  "max_players": 4}}
EOF
"$BUILD/server_bin" --headless --config "$WORK/gs.json" server/tests/arena.json > "$WORK/game.log" 2>&1 < /dev/null &
pids+=($!)
for _ in $(seq 50); do grep -q welcomed "$WORK/game.log" && break; sleep 0.2; done
for _ in $(seq 50); do curl -s "http://127.0.0.1:$BRAIN_PORT/mm/game-list" | grep -q '"RK:' && break; sleep 0.2; done

# queue both accounts for r1v1 (index.js joinQueue / the poll loop), keep polling meanwhile
B="http://127.0.0.1:$BRAIN_PORT"
queue() { # $1 token -> clientKey
    curl -s -G "$B/mm/queue/join" --data-urlencode "token=$1" --data-urlencode "region=local" \
        --data-urlencode 'queues=["r1v1"]' --data-urlencode "ready=1" | python3 -I -c "import json,sys;d=json.load(sys.stdin);print(d.get('clientKey') or 'ERR:'+str(d.get('status')))"
}
KA=$(queue "$TA"); KB=$(queue "$TB")
case "$KA$KB" in *ERR*) echo "[ranked] FAIL: queue join $KA / $KB"; exit 1;; esac
poller() { while :; do curl -s -G "$B/mm/queue/poll" --data-urlencode "clientKey=$1" > "$WORK/poll-$2.json" 2>/dev/null || true; sleep 1; done; }
poller "$KA" a & pids+=($!)
poller "$KB" b & pids+=($!)
GAME=""
for _ in $(seq 60); do
    GAME=$(python3 -I -c "import json,sys
try:
    s=json.load(open(sys.argv[1]))['status']
    print(s['c']['gameId'] if s and s.get('t')=='Join' else '')
except Exception: print('')" "$WORK/poll-a.json" 2>/dev/null)
    [ -n "$GAME" ] && break
    sleep 1
done
[ -n "$GAME" ] || { echo "[ranked] FAIL: no match (poll: $(cat "$WORK/poll-a.json" 2>/dev/null))"; exit 1; }
echo "[ranked] match lobby $GAME"
for _ in $(seq 50); do grep -q "\[ranked\] match" "$WORK/game.log" && break; sleep 0.2; done

# input script: hold the trigger, strafe every 3 s (players sharing a spawn separate), Play
# again after every death; quit after the round's end screen started
script() { # $1 strafe key, $2 quit ms
    echo "0 play"
    echo "300 mdown left"
    for ((t = 1000; t < $2 - 1000; t += 3000)); do
        echo "$t down $1"
        echo "$((t + 350)) up $1"
    done
    for ((t = 3500; t < $2 - 1000; t += 1000)); do
        echo "$t respawn"
        echo "$((t + 150)) mdown left"
    done
    echo "$2 quit"
}
QUIT_MS=$(( (ROUND + 14) * 1000 ))
# events run in file order: sort by time (play stays first, t = 0)
script D $QUIT_MS | sort -s -n -k1,1 > "$WORK/a.txt"
# KN_TEST_ABANDON=1: gunner_b quits 25 s into the match (v3.1.1 leaveGame: needAll ends the
# round, checkLeave makes it an abandoner: the other team wins, rankedDeduction + cooldown)
script A $([ "${KN_TEST_ABANDON:-0}" = 1 ] && echo 25000 || echo $QUIT_MS) | sort -s -n -k1,1 > "$WORK/b.txt"

client() { # $1 name, $2 token, $3 script
    local cfg="$WORK/cfg-$1"
    mkdir -p "$cfg/krunknative"
    printf %s "$2" > "$cfg/krunknative/krunker_token.json"
    printf %s "$1" > "$cfg/krunknative/krunker_username.json"
    env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE="$EGL" \
        XDG_CONFIG_HOME="$cfg" XDG_STATE_HOME="$cfg" KRUNKNATIVE_BRAIN_URL="http://127.0.0.1:$BRAIN_PORT" \
        KRUNKNATIVE_GAME="$GAME" KRUNKNATIVE_TEST_AIM=1 KRUNKNATIVE_INPUT_SCRIPT="$3" KRUNKNATIVE_FRAME_CAP=60 \
        ${4:+KRUNKNATIVE_SCREENSHOT=$4 KRUNKNATIVE_CAPTURE_FRAMES=$5} \
        timeout $((ROUND + 60)) "$BUILD/client_bin" > "$WORK/client-$1.log" 2>&1
}
client gunner_a "$TA" "$WORK/a.txt" "${KN_TEST_SHOT:-}" "$(( (ROUND + 7) * 60 ))" &
pids+=($!)
client gunner_b "$TB" "$WORK/b.txt" &
pids+=($!)

for _ in $(seq $((ROUND * 5 + 300))); do grep -q "\[ranked\] account .* Rating" "$WORK/game.log" && break; sleep 0.2; done
sleep 2
if [ -n "${KN_TEST_SHOT:-}" ]; then
    for _ in $(seq 200); do [ -f "$KN_TEST_SHOT" ] && break; sleep 0.2; done
fi

echo "[ranked] server:"
grep -E "\[ranked\]|\[stats\]" "$WORK/game.log" | sed 's/^/  /'
echo "[ranked] players (elo):"
psql_q -F ' | ' -c "SELECT a.username, p.player_elo, p.player_kills, p.player_deaths FROM players p JOIN accounts a ON a.id = p.player_id ORDER BY a.username" | sed 's/^/  /'
echo "[ranked] mm_ranked_matches:"
psql_q -F ' | ' -c "SELECT * FROM mm_ranked_matches" | sed 's/^/  /'
echo "[ranked] mm_ranked_match_players:"
psql_q -F ' | ' -c "SELECT * FROM mm_ranked_match_players" | sed 's/^/  /'

fail=0
winner=$(sed -n 's/.*winner team \(-\?[0-9]\).*/\1/p' "$WORK/game.log" | head -1)
elo_a=$(psql_q -c "SELECT p.player_elo FROM players p JOIN accounts a ON a.id = p.player_id WHERE a.username = 'gunner_a'")
elo_b=$(psql_q -c "SELECT p.player_elo FROM players p JOIN accounts a ON a.id = p.player_id WHERE a.username = 'gunner_b'")
kills=$(sed -n 's/.*\[stats\] final .* K \([0-9]*\) .*/\1/p' "$WORK/game.log" | sort -n | tail -1)
[ -n "$winner" ] && [ "$winner" != -1 ] || { echo "[ranked] FAIL: no winner ($winner)"; fail=1; }
if [ "${KN_TEST_ABANDON:-0}" = 1 ]; then
    grep -q "1 abandoned" "$WORK/game.log" || { echo "[ranked] FAIL: gunner_b not reported as abandoned"; fail=1; }
    [ "$winner" = 0 ] || { echo "[ranked] FAIL: the team that stayed did not win ($winner)"; fail=1; }
    banned=$(psql_q -c "SELECT count(*) FROM mm_ranked_bans" 2>/dev/null || echo 0)
    [ "${banned:-0}" -ge 1 ] || { echo "[ranked] FAIL: no ranked cooldown for the abandoner"; fail=1; }
else
    [ "${kills:-0}" -ge 10 ] || { echo "[ranked] FAIL: nobody reached 10 kills ($kills)"; fail=1; }
fi
[ "$elo_a" != "$elo_b" ] || { echo "[ranked] FAIL: ratings did not move ($elo_a / $elo_b)"; fail=1; }
[ "$fail" = 0 ] && echo "[ranked] OK: winner team $winner, elo gunner_a=$elo_a gunner_b=$elo_b"
exit $fail
