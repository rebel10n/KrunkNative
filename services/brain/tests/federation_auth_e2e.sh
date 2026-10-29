#!/usr/bin/env bash
# End-to-end test of the federation layers (doc/backend.md "Central auth" / "Server finder"):
#
#   kn_brain --role auth,community     coordinator.krunk.io: accounts, join / hasJoined, social
#   kn_brain --role finder --no-db     matchmaker.krunk.io: the server list
#   kn_brain --role community          a third-party community coordinator (verifies hasJoined
#                                      against central over HTTP; same database here)
#   server_bin A  online_mode (accounts verified, guests welcome), linked to the community
#                 coordinator, listed in the finder
#   server_bin B  offline mode, standalone, listed in the finder
#   client_bin (headless) ONLINE: signs in, joins A through the AUTH_CHALLENGE ->
#              /auth/join -> hasJoined (via the coordinator) handshake, A loads its inventory
#   client_bin (headless) OFFLINE: guest on B, every skin unlocked, nothing saved
#
#   services/brain/tests/federation_auth_e2e.sh <build dir> [database]
# Exit 77 (CTest skip) without PostgreSQL. Ports are ephemeral; nothing touches a display.
set -uo pipefail
BUILD=${1:?build dir}
cd "$(dirname "$0")/../../.." || exit 1
ROOT=$PWD
DB=${2:-${KN_MM_TEST_DATABASE:-}}
if [ -z "$DB" ]; then
    DB=$(python3 -I -c 'import json,sys; print(json.load(open(sys.argv[1])).get("database",{}).get("name",""))' \
         config/config.json 2>/dev/null)
fi
[ -n "$DB" ] || { echo "auth e2e: no database configured; skipped"; exit 77; }
BRAIN=$BUILD/services/brain/kn_brain
SERVER=$BUILD/server_bin
CLIENT=$BUILD/client_bin
for b in "$BRAIN" "$SERVER" "$CLIENT"; do [ -x "$b" ] || { echo "missing $b"; exit 1; }; done
SCHEMA=kn_test_fa_$$
WORK=$(mktemp -d "${TMPDIR:-/tmp}/kn-authe2e.XXXXXX")
echo '{}' >"$WORK/empty.json" # server_bin: no config/config.json gameserver section (server.cfg only)
PG=(python3 -I "$ROOT/services/brain/tests/pg_schema.py" "$ROOT" "$DB")
"${PG[@]}" create "$SCHEMA" || { rc=$?; rm -rf "$WORK"; echo "auth e2e: skipped"; exit "$rc"; }
export KN_DATABASE_NAME=$DB KN_DATABASE_SEARCH_PATH=$SCHEMA KN_LOG_LEVEL=info
export KN_BRAIN_INTERNAL_SECRET=authe2e-$$ KN_ACCOUNTS_PASSWORD_ITERATIONS=10000
pids=()
fails=0

cleanup() {
    for p in "${pids[@]}"; do kill "$p" 2>/dev/null; done
    sleep 0.3
    for p in "${pids[@]}"; do kill -9 "$p" 2>/dev/null; done
    wait 2>/dev/null
    "${PG[@]}" drop "$SCHEMA" >/dev/null 2>&1
    if [ "$fails" = 0 ]; then rm -rf "$WORK"; else echo "logs kept in $WORK"; fi
}
trap cleanup EXIT

ok() { echo "  ok   $*"; }
bad() { echo "  FAIL $*"; fails=$((fails + 1)); }
check() { local d=$1; shift; if "$@"; then ok "$d"; else bad "$d"; fi; }
wait_log() { # file regex timeout_s
    local i
    for ((i = 0; i < $3 * 10; i++)); do grep -qE "$2" "$1" 2>/dev/null && return 0; sleep 0.1; done
    return 1
}
brain_port() { sed -nE 's/.*listening on http:\/\/[^:]+:([0-9]+) \(ws.*/\1/p' "$1" | head -1; }
free_port() { python3 -I -c 'import socket;s=socket.socket();s.bind(("127.0.0.1",0));print(s.getsockname()[1])'; }

# ---- central (auth + finder) and the community coordinator
"$BRAIN" --migrate-only >"$WORK/migrate.log" 2>&1 || { bad "migrations"; cat "$WORK/migrate.log"; exit 1; }
"$BRAIN" --role auth,community --brain.port=0 --brain.id=central --brain.federation=false >"$WORK/central.log" 2>&1 &
pids+=($!)
"$BRAIN" --role finder --no-db --brain.port=0 --brain.id=finder --finder.poll_interval_s=2 >"$WORK/finder.log" 2>&1 &
pids+=($!)
wait_log "$WORK/central.log" "listening on" 10 && wait_log "$WORK/finder.log" "listening on" 10 || { bad "brains start"; exit 1; }
CENTRAL=http://127.0.0.1:$(brain_port "$WORK/central.log")
FINDER=http://127.0.0.1:$(brain_port "$WORK/finder.log")
# the community verifies hasJoined against central over HTTP (not in-process)
"$BRAIN" --role community --brain.port=0 --brain.id=community --brain.federation=false \
    --community.auth_url="$CENTRAL" >"$WORK/community.log" 2>&1 &
pids+=($!)
wait_log "$WORK/community.log" "listening on" 10 || { bad "community start"; exit 1; }
MP=$(brain_port "$WORK/community.log")
echo "central $CENTRAL, finder $FINDER, community http://127.0.0.1:$MP"

check "community has no /auth routes" test "$(curl -s -o /dev/null -w '%{http_code}' "http://127.0.0.1:$MP/auth/me")" = 404
check "finder has no /auth routes" test "$(curl -s -o /dev/null -w '%{http_code}' "$FINDER/auth/me")" = 404
check "central has no finder routes" test "$(curl -s -o /dev/null -w '%{http_code}' "$CENTRAL/servers")" = 404

# ---- account at central
REG=$(curl -s -X POST "$CENTRAL/auth/register" -H 'Content-Type: application/json' -d '{"username":"e2e_alice","password":"secret12"}')
TOKEN=$(python3 -I -c 'import json,sys; print(json.loads(sys.argv[1]).get("token",""))' "$REG")
AID=$(python3 -I -c 'import json,sys; print(json.loads(sys.argv[1])["account"]["id"])' "$REG" 2>/dev/null)
check "register -> token" test -n "$TOKEN"
LOGIN=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$CENTRAL/auth/login" -d '{"username":"e2e_alice","password":"nope"}')
check "wrong password -> 401" test "$LOGIN" = 401
# inventory in the community: one skin (shared database: the community row is the account id)
python3 -I - "$MP" "$KN_BRAIN_INTERNAL_SECRET" "$AID" >"$WORK/give.log" 2>&1 <<'EOF'
import asyncio, json, sys
import websockets
async def main():
    port, secret, aid = sys.argv[1], sys.argv[2], int(sys.argv[3])
    async with websockets.connect("ws://127.0.0.1:%s/mm/internal/ws" % port,
                                  additional_headers={"Authorization": "Bearer " + secret}) as ws:
        await ws.send(json.dumps({"type": "hello", "nodeId": "GIVER", "region": "local", "name": "giver",
                                  "publicHost": "127.0.0.1", "publicPort": 1, "version": "1", "maxPlayers": 1}))
        await ws.send(json.dumps({"type": "acct", "rid": 1, "op": "give_skin", "args": {"id": aid, "skin": 5, "source": "e2e"}}))
        while True:
            f = json.loads(await ws.recv())
            if f.get("type") == "acct-r":
                print(f); return 0 if not f.get("err") else 1
sys.exit(asyncio.run(main()))
EOF
check "community inventory seeded (give_skin)" test $? = 0

# ---- game servers
PA=$(free_port); PB=$(free_port)
cat >"$WORK/a.cfg" <<EOF
hostname "E2E Online"
port $PA
online_mode 1
auth_url "$CENTRAL"
coordinator_url "ws://127.0.0.1:$MP/mm/internal/ws"
coordinator_secret "$KN_BRAIN_INTERNAL_SECRET"
finder_url "$FINDER"
node_id E2EA
EOF
cat >"$WORK/b.cfg" <<EOF
hostname "E2E Offline"
port $PB
online_mode 0
finder_url "$FINDER"
node_id E2EB
EOF
(cd "$ROOT" && exec "$SERVER" --headless --cfg "$WORK/a.cfg" --config "$WORK/empty.json") >"$WORK/a.log" 2>&1 &
pids+=($!)
(cd "$ROOT" && exec "$SERVER" --headless --cfg "$WORK/b.cfg" --config "$WORK/empty.json") >"$WORK/b.log" 2>&1 &
pids+=($!)

# ---- finder lists both, with data from the query
listed() { curl -s "$FINDER/servers$1" | python3 -I -c 'import json,sys; print(len(json.load(sys.stdin)["servers"]))'; }
for _ in $(seq 1 100); do [ "$(listed '')" = 2 ] && break; sleep 0.2; done
check "finder verified both servers" test "$(listed '')" = 2
# online_mode on the wire is legacy "guests refused": no current server is account-only
check "finder: no account-only server (online_mode=1 lists none)" test "$(listed '?online_mode=1')" = 0
check "finder filter online_mode=0 lists both" test "$(listed '?online_mode=0')" = 2
check "finder filter q=offline" test "$(listed '?q=offline')" = 1

# ---- online client joins A
run_client() { # name mode port token
    local cfg="$WORK/cfg-$1"
    mkdir -p "$cfg/krunknative"
    if [ -n "$4" ]; then
        printf '%s' e2e_alice >"$cfg/krunknative/krunker_username.json"
        printf '%s' "$4" >"$cfg/krunknative/krunker_token.json"
    fi
    printf '0 play\n6000 quit\n' >"$WORK/$1.script"
    # KRUNKNATIVE_DEBUG=2: the lines checked below ([account] logged in as, [auth] joining as, [mode] offline) are
    # debug-level since 5f55eef1 (klog.h KN_LOG_DEBUG1 / KN_LOG_VERBOSE2)
    (cd "$ROOT" && exec env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE=mesa KRUNKNATIVE_DEBUG=2 \
        KRUNKNATIVE_AUDIO=0 XDG_CONFIG_HOME="$cfg" XDG_STATE_HOME="$cfg" KRUNKNATIVE_INPUT_SCRIPT="$WORK/$1.script" \
        KRUNKNATIVE_FIXED_DELTA=33 KRUNKNATIVE_WINDOW_SIZE=640x360 \
        timeout 60 stdbuf -oL "$CLIENT" --mode "$2" --brain "$CENTRAL" --finder "$FINDER/servers" --connect "127.0.0.1:$3") \
        >"$WORK/client-$1.log" 2>&1
}
run_client online online "$PA" "$TOKEN"
check "client: online mode + signed in" grep -q "logged in as e2e_alice" "$WORK/client-online.log"
check "client: /auth/join -> JOIN hj1" grep -q "\[auth\] joining as e2e_alice" "$WORK/client-online.log"
check "server A: verified through the coordinator" grep -qE "e2e_alice|account $AID" "$WORK/a.log"
check "server A: inventory loaded (1 skin)" grep -q "owns 1 skin" "$WORK/a.log"
check "server A: no failed verification" bash -c "! grep -q 'Failed to verify username' '$WORK/a.log'"

# ---- offline client joins B
run_client offline offline "$PB" ""
check "client: offline mode" grep -q "\[mode\] offline" "$WORK/client-offline.log"
check "client: no account service in offline mode" bash -c "! grep -q 'logged in as' '$WORK/client-offline.log'"
check "server B: guest joined" grep -qiE "joined|guest" "$WORK/b.log"
check "offline client: nothing saved (no token)" bash -c "! test -s '$WORK/cfg-offline/krunknative/krunker_token.json'"

# ---- an offline-mode client joins A as a guest (every Krunker lobby admits guests)
run_client offline2 offline "$PA" ""
check "client: answers A's challenge as a guest" grep -q "not signed in: joining as a guest" "$WORK/client-offline2.log"
check "server A admits the guest" grep -q "joins as a guest (no account)" "$WORK/a.log"
check "server A refused nobody" bash -c "! grep -q 'Rejecting' '$WORK/a.log'"

# ---- admin rcon relayed by the coordinator to server A (internal WS "rcon")
RC=$(curl -s -X POST "http://127.0.0.1:$MP/mm/admin/rcon?node=E2EA" -H "Authorization: Bearer $KN_BRAIN_INTERNAL_SECRET" \
     -d '{"command":"status"}')
check "coordinator rcon relay (status)" python3 -I -c 'import json,sys; r=json.loads(sys.argv[1]); sys.exit(0 if r.get("success") and "E2E Online" in r.get("output","") else 1)' "$RC"
check "coordinator rcon needs the secret" test "$(curl -s -o /dev/null -w '%{http_code}' -X POST "http://127.0.0.1:$MP/mm/admin/rcon?node=E2EA" -d '{"command":"status"}')" = 401

# ---- hasJoined replay protection: the recorded hash does not verify another server/user
H=$(python3 -I -c 'import hashlib;print(hashlib.sha256(b"kn-join-v1\0x\0\0y").hexdigest())')
curl -s -X POST "$CENTRAL/auth/join" -d "{\"token\":\"$TOKEN\",\"serverHash\":\"$H\"}" >/dev/null
check "hasJoined ok for the joined hash" test "$(curl -s -o /dev/null -w '%{http_code}' "$CENTRAL/auth/hasJoined?username=e2e_alice&serverHash=$H")" = 200
H2=$(python3 -I -c 'import hashlib;print(hashlib.sha256(b"kn-join-v1\0x2\0\0y").hexdigest())')
check "hasJoined 204 for another server's hash" test "$(curl -s -o /dev/null -w '%{http_code}' "$CENTRAL/auth/hasJoined?username=e2e_alice&serverHash=$H2")" = 204

for l in central finder community a b; do
    if grep -qE "ERROR|FATAL|AddressSanitizer" "$WORK/$l.log"; then bad "$l log has errors"; grep -E "ERROR|FATAL" "$WORK/$l.log" | head -5; fi
done
[ "$fails" = 0 ] && echo "federation auth e2e: all checks passed" || echo "federation auth e2e: $fails failure(s)"
[ "$fails" = 0 ]
