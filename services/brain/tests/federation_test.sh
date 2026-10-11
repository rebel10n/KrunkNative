#!/usr/bin/env bash
# Multi-brain integration test (doc/backend.md section 6): 2 brains on
# ephemeral ports sharing one PostgreSQL database, fake game servers across 2
# regions. Checks the global game list, region quick join + cross-region
# fallback, reservations, cross-brain hosting, failover on game server death
# (socket close and TTL expiry) and brain death (TTL expiry), and that the brain
# logs carry no statement errors.
#
#   services/brain/tests/federation_test.sh <build dir> [database]
# Database: argument, else $KN_MM_TEST_DATABASE, else config/config.json's
# database.name. Every run migrates into its own schema (kn_test_mm_<pid>,
# dropped afterwards), so concurrent runs never share tables. Exit 77 (CTest
# skip) when PostgreSQL is unreachable. KN_TEST_DB_DRIVER=sqlite|mysql runs the
# brains on that store driver instead (db_scratch.py; sqlite = one shared temp
# file, federation pub/sub then polls the kn_pubsub table).
set -uo pipefail
BUILD=${1:?build dir}
cd "$(dirname "$0")/../../.." || exit 1
ROOT=$PWD
DB=${2:-${KN_MM_TEST_DATABASE:-}}
if [ -z "$DB" ]; then
    DB=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1])).get("database",{}).get("name",""))' \
         config/config.json 2>/dev/null)
fi
DRIVER=${KN_TEST_DB_DRIVER:-postgres}
[ -n "$DB" ] || [ "$DRIVER" != postgres ] || { echo "federation test: no database configured; skipped"; exit 77; }
BRAIN=$BUILD/services/brain/kn_brain
FAKE=$BUILD/services/brain/kn_fakenode
SCHEMA=kn_test_mm_$$
TTL=6
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kn-fedtest.XXXXXX")
if [ "$DRIVER" = postgres ]; then
    PG=(python3 -I "$ROOT/services/brain/tests/pg_schema.py" "$ROOT" "$DB")
    "${PG[@]}" create "$SCHEMA" || { rc=$?; rm -rf "$WORK"; echo "federation test: skipped"; exit "$rc"; }
    export KN_DATABASE_NAME=$DB KN_DATABASE_SEARCH_PATH=$SCHEMA KN_LOG_LEVEL=${KN_LOG_LEVEL:-info}
else
    SCRATCH=(python3 -I "$ROOT/services/brain/tests/db_scratch.py" "$ROOT")
    DBENV=$("${SCRATCH[@]}" create "$DRIVER" "$SCHEMA" --shell) || { rc=$?; rm -rf "$WORK"; echo "federation test: $DRIVER unavailable; skipped"; exit "$rc"; }
    eval "$DBENV"
    export KN_LOG_LEVEL=${KN_LOG_LEVEL:-info}
fi
export KN_BRAIN_INTERNAL_SECRET=fedtest-$$
pids=()
fails=0

cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    sleep 0.3
    for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null; done
    wait 2>/dev/null
    if [ "$DRIVER" = postgres ]; then "${PG[@]}" drop "$SCHEMA" >/dev/null 2>&1
    else "${SCRATCH[@]}" drop "$DRIVER" "$SCHEMA" >/dev/null 2>&1; fi
    if [ "$fails" = 0 ]; then rm -rf "$WORK"; else echo "logs kept in $WORK"; fi
}
trap cleanup EXIT

ok() { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fails=$((fails + 1)); }
check() { # description, command (exit 0 = pass)
    local d=$1; shift
    if "$@"; then ok "$d"; else bad "$d"; fi
}
get() { curl -fs "http://127.0.0.1:$1$2"; }
json() { python3 -I -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
games() { get "$1" /mm/game-list | json "' '.join(sorted(g[0] for g in d['games']))"; }
wait_for() { # seconds, command
    local n=$(( $1 * 10 )); shift
    for _ in $(seq 1 "$n"); do "$@" && return 0; sleep 0.1; done
    return 1
}
has_games() { [ "$(games "$1" 2>/dev/null)" = "$2" ]; }

start() { # name, cmd...
    local name=$1; shift
    "$@" > "$WORK/$name.log" 2>&1 &
    pids+=($!)
    eval "PID_$name=$!"
}

# brain.port=0: the kernel picks a free port and the brain logs it
bound_port() { # log file
    local p=""
    for _ in $(seq 1 100); do
        p=$(sed -n 's/.*listening on http:\/\/127\.0\.0\.1:\([0-9]*\) (ws.*/\1/p' "$1" | head -n1)
        [ -n "$p" ] && { echo "$p"; return 0; }
        sleep 0.1
    done
    return 1
}

"$BRAIN" --migrate-only > "$WORK/migrate.log" 2>&1 || { cat "$WORK/migrate.log"; exit 1; }
# rows left by an earlier run are older than the TTL: readers ignore them and the
# brains reap them; reused node ids are simply taken over on hello.

common=(--brain.host=127.0.0.1 --brain.port=0 --brain.node_ttl_s=$TTL --brain.heartbeat_s=1 --brain.ping_interval_s=2
        --brain.join_ttl_s=20)
start A "$BRAIN" --brain.region=us-nj --brain.id=test-a-$$ "${common[@]}"
start B "$BRAIN" --brain.region=de-fra --brain.id=test-b-$$ "${common[@]}"
PA=$(bound_port "$WORK/A.log") && PB=$(bound_port "$WORK/B.log") || { bad "brains up"; exit 1; }

start NJ1 "$FAKE" --secret "$KN_BRAIN_INTERNAL_SECRET" --brain ws://127.0.0.1:$PA/mm/internal/ws --node TNJ1 --region us-nj --port 22001 --players 3 --quiet
start NJ2 "$FAKE" --secret "$KN_BRAIN_INTERNAL_SECRET" --brain ws://127.0.0.1:$PA/mm/internal/ws --node TNJ2 --region us-nj --port 22002 --players 5 --custom --quiet
start FRA "$FAKE" --secret "$KN_BRAIN_INTERNAL_SECRET" --brain ws://127.0.0.1:$PB/mm/internal/ws --node TFRA1 --region de-fra --port 22003 --players 1 --custom --quiet
start FRA2 "$FAKE" --secret "$KN_BRAIN_INTERNAL_SECRET" --brain ws://127.0.0.1:$PB/mm/internal/ws --node TFRA2 --region de-fra --port 22004 --players 0 --custom --quiet
sleep 2.5

echo "== global view"
la=$(games $PA); lb=$(games $PB)
check "brain A lists 4 lobbies ($la)" test "$(echo "$la" | wc -w)" = 4
check "brain B lists the same lobbies" test "$la" = "$lb"
check "ping-list has both regions" sh -c "get() { curl -fs http://127.0.0.1:$PA\$1; }; get /mm/ping-list | grep -q us-nj && get /mm/ping-list | grep -q de-fra"
check "/mm/brains lists 2 brains" test "$(get $PA /mm/brains | json 'len(d)')" = 2
check "player count is global (9)" test "$(get $PB /api/player-count | json "d['playerCount']")" = 9

echo "== hosting"
h=$(curl -s "http://127.0.0.1:$PA/mm/host-game?region=de-fra&config=%7B%22maps%22%3A%5B0%5D%7D")
hg=$(echo "$h" | json "d.get('gameId','') or d.get('error')")
check "host-game on A for de-fra is routed to B's idle node TFRA2 ($hg)" test "${hg%%:*}" = TFRA2
# without native pub/sub (sqlite, mysql) brain A learns B's new lobby within
# database.notify_poll_ms + one refresh: wait for it before asking again
lists_game() { games "$1" 2>/dev/null | grep -qw "$2"; }
[ "$DRIVER" = postgres ] || wait_for 5 lists_game "$PA" "$hg"
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PA/mm/host-game?region=de-fra")
check "second host-game: no idle custom-capable node left -> 503 ($code)" test "$code" = 503
wait_for 4 sh -c "curl -fs http://127.0.0.1:$PA/mm/game-list | grep -q '\"$hg\".*\"cs\":true'"
check "hosted custom lobby is listed with cs=true" test $? = 0

echo "== quick join"
gid=$(get $PA "/mm/seek-game?region=us-nj" | json "d['gameId']")
check "us-nj quick join on A -> most populated (TNJ2: $gid)" test "${gid%%:*}" = TNJ2
gid=$(get $PA "/mm/seek-game?region=de-fra" | json "d['gameId']")
check "de-fra quick join on A -> B's node ($gid)" test "${gid%%:*}" = TFRA1
port=$(get $PB "/mm/seek-game?region=us-nj" | json "d['port']")
check "us-nj quick join on B -> A's nodes (port $port)" sh -c "[ $port = 22001 ] || [ $port = 22002 ]"
# fill TFRA1 (1 player + 1 reservation above) with 6 more reservations through brain B -> 8/8
for _ in 1 2 3 4 5 6; do get $PB "/mm/seek-game?region=de-fra" > /dev/null; done
sleep 1.5
gid=$(get $PA "/mm/seek-game?region=de-fra" | json "d['gameId']")
check "reservations made on B count on A: full de-fra falls back cross-region ($gid)" test "${gid%%:*}" != TFRA1
code=$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PA/mm/seek-game?region=us-nj&game=TFRA1:nope&autoChangeGame=false")
check "unknown game without autoChangeGame -> 404 InvalidGameId" test "$code" = 404
info=$(get $PB "/mm/game-info?game=$(get $PA /mm/game-list | json "d['games'][0][0]")" | json 'd[0]')
check "game-info on B for an A lobby ($info)" test -n "$info"

echo "== failover: game server dies (socket close)"
kill -9 "$PID_NJ1"
wait_for 4 sh -c "! curl -fs http://127.0.0.1:$PB/mm/game-list | grep -q TNJ1:"
check "TNJ1 vanished from B's list after kill -9" test $? = 0

echo "== failover: game server hangs (TTL ${TTL}s)"
kill -STOP "$PID_NJ2"
t0=$(date +%s)
wait_for $((TTL * 3)) sh -c "! curl -fs http://127.0.0.1:$PB/mm/game-list | grep -q TNJ2:"
r=$?; t1=$(date +%s)
check "TNJ2 reaped after $((t1 - t0))s (TTL $TTL)" test $r = 0
kill -CONT "$PID_NJ2"
wait_for 20 sh -c "curl -fs http://127.0.0.1:$PB/mm/game-list | grep -q TNJ2:"
check "TNJ2 re-registered after resuming (reconnect)" test $? = 0

echo "== failover: brain B dies (TTL)"
kill -9 "$PID_B"
t0=$(date +%s)
wait_for $((TTL * 3)) sh -c "! curl -fs http://127.0.0.1:$PA/mm/game-list | grep -q TFRA1:"
r=$?; t1=$(date +%s)
check "B's node TFRA1 expired from A after $((t1 - t0))s" test $r = 0
r=$(curl -s "http://127.0.0.1:$PA/mm/seek-game?region=de-fra" | json "d.get('gameId') or d.get('error')")
check "de-fra quick join no longer offers the dead brain's lobbies ($r)" sh -c "case '$r' in TFRA1:*|'') exit 1;; esac"

echo "== node moves to the surviving brain"
kill "$PID_FRA" 2>/dev/null
start FRA3 "$FAKE" --secret "$KN_BRAIN_INTERNAL_SECRET" --brain ws://127.0.0.1:$PA/mm/internal/ws --node TFRA1 --region de-fra --port 22003 --quiet
wait_for 5 sh -c "curl -fs http://127.0.0.1:$PA/mm/game-list | grep -q TFRA1:"
check "TFRA1 registered with brain A" test $? = 0

echo "== brain logs"
# every DB statement of the registry must succeed (e.g. an untyped/unused
# parameter fails with "could not determine data type of parameter $1")
errs=$(grep -hE "federation: .*failed|ERROR|FATAL" "$WORK/A.log" "$WORK/B.log" | sort | uniq -c | sort -rn | head -5)
check "no database/statement errors in the brain logs${errs:+: $errs}" test -z "$errs"

echo
if [ "$fails" = 0 ]; then echo "federation test: OK"; else echo "federation test: $fails failure(s)"; fi
exit "$fails"
