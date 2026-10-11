#!/usr/bin/env bash
# UDP transport end-to-end (doc/transport.md): server_bin + two headless native clients
# shooting each other on the arena map over UDP (optionally with simulated loss / dup /
# reorder on both sides), then checks from the logs that both sessions were UDP, both
# clients spawned, kills happened, pings were measured and nobody fell back to TCP.
#
#   server/tests/udp_e2e.sh [BUILD_DIR]       (default cmake-build-debug; run from the repo root)
#   KN_UDP_LOSS="10 5 5"   drop / dup / reorder percent on every receiver (default: none)
#   KN_UDP_TRANSPORT=tcp   run the same scenario over TCP (comparison)
#   KN_TEST_MAP=map.json   another map (a custom one is streamed: the reliable channel's fragmentation)
#   KN_TEST_EGL=mesa|nvidia, KN_TEST_ROUND=seconds (default 25), KN_TEST_KEEP=1 keeps the logs
set -euo pipefail

BUILD=${1:-cmake-build-debug}
EGL=${KN_TEST_EGL:-mesa}
ROUND=${KN_TEST_ROUND:-25}
TRANSPORT=${KN_UDP_TRANSPORT:-udp}
read -r DROP DUP REORDER <<< "${KN_UDP_LOSS:-0 0 0}"
WORK=$(mktemp -d /tmp/kn-udp-e2e-XXXXXX)
PORT=$((22000 + RANDOM % 2000))
pids=()

cleanup() {
    for pid in "${pids[@]}"; do pkill -TERM -P "$pid" 2>/dev/null || true; kill "$pid" 2>/dev/null || true; done
    wait 2>/dev/null || true
    if [ "${KN_TEST_KEEP:-0}" != 1 ]; then rm -rf "$WORK"; else echo "[udp-e2e] kept $WORK"; fi
}
trap cleanup EXIT

for bin in server_bin client_bin; do
    [ -x "$BUILD/$bin" ] || { echo "[udp-e2e] missing $BUILD/$bin"; exit 2; }
done

export KRUNKNATIVE_NET_DROP=$DROP KRUNKNATIVE_NET_DUP=$DUP KRUNKNATIVE_NET_REORDER=$REORDER

"$BUILD/server_bin" --headless --cfg none --port "$PORT" --round-seconds "$ROUND" "${KN_TEST_MAP:-server/tests/arena.json}" \
    > "$WORK/game.log" 2>&1 < /dev/null &
pids+=($!)
for _ in $(seq 50); do grep -q "Listening on UDP" "$WORK/game.log" && break; sleep 0.1; done
grep -q "Listening on UDP" "$WORK/game.log" || { echo "[udp-e2e] FAIL: no UDP listener"; cat "$WORK/game.log"; exit 1; }

script() { # $1 strafe key, $2 quit ms (same pattern as gunfight.sh)
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
QUIT_MS=$(( (ROUND - 3) * 1000 ))
script D $QUIT_MS | sort -s -n -k1,1 > "$WORK/a.txt"
script A $QUIT_MS | sort -s -n -k1,1 > "$WORK/b.txt"

client() { # $1 name, $2 script
    local cfg="$WORK/cfg-$1"
    mkdir -p "$cfg"
    env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE="$EGL" \
        XDG_CONFIG_HOME="$cfg" XDG_STATE_HOME="$cfg" KRUNKNATIVE_NET_TRANSPORT="$TRANSPORT" \
        KRUNKNATIVE_TEST_AIM=1 KRUNKNATIVE_INPUT_SCRIPT="$2" KRUNKNATIVE_FRAME_CAP=60 KRUNKNATIVE_PACKET_LOG=1 \
        timeout $((ROUND + 30)) "$BUILD/client_bin" --mode offline --connect "127.0.0.1:$PORT" > "$WORK/client-$1.log" 2>&1
}
client a "$WORK/a.txt" &
pids+=($!)
client b "$WORK/b.txt" &
pids+=($!)
wait "${pids[1]}" "${pids[2]}" || true

fail=0
check() { # $1 description, $2 command
    if eval "$2"; then echo "  ok   $1"; else echo "  FAIL $1"; fail=1; fi
}
echo "[udp-e2e] transport=$TRANSPORT loss drop=$DROP% dup=$DUP% reorder=$REORDER% port=$PORT"
for c in a b; do
    log="$WORK/client-$c.log"
    if [ "$TRANSPORT" = udp ]; then
        check "client $c: UDP session" "grep -q 'udp session established' '$log'"
        check "client $c: no TCP fallback" "! grep -q 'No UDP answer' '$log'"
    fi
    check "client $c: connected" "grep -q 'connected to server' '$log'"
    grep -E "udp session ended" "$log" | sed 's/^/       /' || true
done
count() { grep -c "<= .*\"type\":\"$2\"" "$WORK/client-$1.log" || true; }
for c in a b; do
    spawns=$(count $c spawn); states=$(count $c state); kills=$(count $c kill); hits=$(count $c hit); pings=$(count $c ping_result)
    echo "       client $c received: spawn $spawns state $states kill $kills hit $hits ping_result $pings"
    check "client $c: spawns of both players" "[ $spawns -ge 2 ]"
    check "client $c: STATE stream" "[ $states -ge $(( (ROUND - 8) * 10 )) ]"
    [ -z "${KN_TEST_MAP:-}" ] && check "client $c: kills (arena)" "[ $kills -ge 1 ]"
    [ -n "${KN_TEST_MAP:-}" ] && check "client $c: map streamed" "grep -q '^downloaded map' '$WORK/client-$c.log'"
    check "client $c: ping measured" "[ $pings -ge 1 ]"
    grep "<= .*ping_result" "$WORK/client-$c.log" | tail -n 1 | sed 's/^.*{/       last {/' || true
done
echo "[udp-e2e] server log tail:"
tail -n 15 "$WORK/game.log" | sed 's/^/  /'
[ "$fail" = 0 ] && echo "[udp-e2e] PASS" || { echo "[udp-e2e] FAIL"; KN_TEST_KEEP=1; exit 1; }
