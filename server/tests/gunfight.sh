#!/usr/bin/env bash
# Real gun-fight backend test (doc/backend.md section 8): kn_brain + server_bin on the open
# arena map (server/tests/arena.json) + two headless native clients logged in as two
# accounts, shooting each other with real hitscan (KRUNKNATIVE_TEST_AIM aims every frame at
# the nearest player, the input script holds the trigger, strafes and respawns). After the
# round the script checks that kills, deaths, score, KR and the match_results rows landed in
# PostgreSQL. Never touches the main database: it creates (and drops) its own.
#
#   server/tests/gunfight.sh [BUILD_DIR]      (default cmake-build-debug; run from the repo root)
#
# Needs: a debug build with services (server_bin, client_bin, kn_brain, kn_migrate), a
# PostgreSQL the config/config.json "database" role can use, `sudo -n -u postgres` for
# createdb/dropdb (or KN_TEST_DB pointing at an existing empty database the role owns),
# libEGL for KRUNKNATIVE_HEADLESS. Env: KN_TEST_DB, KN_TEST_EGL (default mesa),
# KN_TEST_KEEP=1 keeps the database and the work dir, KN_TEST_ROUND (seconds, default 40).
set -euo pipefail

BUILD=${1:-cmake-build-debug}
DB=${KN_TEST_DB:-krunknative_gunfight}
EGL=${KN_TEST_EGL:-mesa}
ROUND=${KN_TEST_ROUND:-40}
WORK=$(mktemp -d /tmp/kn-gunfight-XXXXXX)
BRAIN_PORT=5251
GAME_PORT=21991
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
        echo "[gunfight] kept $WORK and database $DB"
    fi
}
trap cleanup EXIT

for bin in server_bin client_bin services/brain/kn_brain services/common/kn_migrate; do
    [ -x "$BUILD/$bin" ] || { echo "[gunfight] missing $BUILD/$bin"; exit 2; }
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
psql_q -c "SELECT kn_register('gunner_a', 'x', '127.0.0.1', '', 0, 0, '$TA', 3600, 'gunfight')" > /dev/null
psql_q -c "SELECT kn_register('gunner_b', 'x', '127.0.0.1', '', 0, 0, '$TB', 3600, 'gunfight')" > /dev/null

"$BUILD/services/brain/kn_brain" --set database.name="$DB" --set brain.port=$BRAIN_PORT --set brain.region=local \
    > "$WORK/brain.log" 2>&1 &
pids+=($!)
sleep 1

cat > "$WORK/gs.json" <<EOF
{"gameserver": {"node_id": "GF", "name": "gunfight", "region": "local", "listen_port": $GAME_PORT,
  "brain_url": "ws://127.0.0.1:$BRAIN_PORT/mm/internal/ws", "modes": ["ffa"], "round_seconds": $ROUND,
  "max_players": 4}}
EOF
"$BUILD/server_bin" --headless --config "$WORK/gs.json" server/tests/arena.json > "$WORK/game.log" 2>&1 < /dev/null &
pids+=($!)
for _ in $(seq 50); do grep -q welcomed "$WORK/game.log" && break; sleep 0.2; done
GAME=""
for _ in $(seq 50); do # the lobby shows once the brain processed the first snapshot
    GAME=$(curl -s "http://127.0.0.1:$BRAIN_PORT/mm/game-list" | python3 -I -c "import json,sys;print(([g[0] for g in json.load(sys.stdin)['games'] if g[0].startswith('GF:')] + [''])[0])")
    [ -n "$GAME" ] && break
    sleep 0.2
done
[ -n "$GAME" ] || { echo "[gunfight] FAIL: lobby never listed by the brain"; exit 1; }
echo "[gunfight] lobby $GAME"

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
script A $QUIT_MS | sort -s -n -k1,1 > "$WORK/b.txt"

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

for _ in $(seq $((ROUND * 5 + 300))); do grep -q "\[stats\] final" "$WORK/game.log" && break; sleep 0.2; done
sleep 2
# KN_TEST_SHOT: keep the server up until gunner_a captured its end screen
if [ -n "${KN_TEST_SHOT:-}" ]; then
    for _ in $(seq 200); do [ -f "$KN_TEST_SHOT" ] && break; sleep 0.2; done
fi

echo "[gunfight] server stats lines:"
grep "\[stats\]" "$WORK/game.log" | sed 's/^/  /'
echo "[gunfight] match_results:"
psql_q -F ' | ' -c "SELECT a.username, m.kills, m.deaths, m.score, m.won, m.games, m.kr, m.time_ms FROM match_results m JOIN accounts a ON a.id = m.account_id WHERE m.games = 1 ORDER BY a.username" | sed 's/^/  /'
echo "[gunfight] players:"
psql_q -F ' | ' -c "SELECT a.username, p.player_kills, p.player_deaths, p.player_score, p.player_wins, p.player_funds FROM players p JOIN accounts a ON a.id = p.player_id ORDER BY a.username" | sed 's/^/  /'

kills=$(psql_q -c "SELECT coalesce(sum(kills), 0) FROM match_results WHERE games = 1")
deaths=$(psql_q -c "SELECT coalesce(sum(deaths), 0) FROM match_results WHERE games = 1")
rows=$(psql_q -c "SELECT count(*) FROM match_results WHERE games = 1")
kr_ok=$(psql_q -c "SELECT bool_and(kr IN (least(30, score / 100), least(40, score / 100))) FROM match_results WHERE games = 1") # 40: premium (accounts.free_premium)
funds_ok=$(psql_q -c "SELECT bool_and(p.player_funds = (SELECT coalesce(sum(kr), 0) FROM match_results m WHERE m.account_id = p.player_id)) FROM players p")
fail=0
[ "$rows" = 2 ] || { echo "[gunfight] FAIL: $rows final match_results rows (want 2)"; fail=1; }
[ "$kills" -gt 0 ] || { echo "[gunfight] FAIL: no kills"; fail=1; }
[ "$deaths" -gt 0 ] || { echo "[gunfight] FAIL: no deaths"; fail=1; }
[ "$kr_ok" = t ] || { echo "[gunfight] FAIL: kr != min(premium ? 40 : 30, floor(score / 100))"; fail=1; }
[ "$funds_ok" = t ] || { echo "[gunfight] FAIL: player_funds != sum of rewards"; fail=1; }
[ "$fail" = 0 ] && echo "[gunfight] OK: $kills kill(s), $deaths death(s) from real hits, KR per v3.1.1 endGame"
exit $fail
