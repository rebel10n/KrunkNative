#!/bin/bash
# KrunkNative side of tools/kref/players-ref.js: server_bin on sandstorm plus two headless
# clients, A running tools/kref/players-scenario.json (converted to KRUNKNATIVE_INPUT_SCRIPT)
# and B watching A side-on, logging A's model per frame (KRUNKNATIVE_PLAYER_LOG) and writing
# a frame every STEP ms (B's script `shot`s).
# usage: players-native.sh <out_dir> [build_dir]   env: PORT (21841), EGL_DEVICE (nvidia),
#        AX AY AZ (A's feet, default -39 33 -210), DIST (12), START (A's scenario delay, 3000 ms)
OUT=$1; BUILD=${2:-$(dirname "$0")/../../cmake-build-debug}
SRC=$(cd "$(dirname "$0")/../.." && pwd)
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
case "$BUILD" in /*) ;; *) BUILD="$PWD/$BUILD" ;; esac
PORT=${PORT:-21841}; AX=${AX:--39}; AY=${AY:-33}; AZ=${AZ:--210}; DIST=${DIST:-12}; START=${START:-3000}; STEP=${STEP:-100}
mkdir -p "$OUT/frames" "$OUT/cfg-a" "$OUT/cfg-b/krunknative"
# B shows no first-person weapon (the ref recorder hides fpsCamera layers the same way)
echo '{"showWeapon":false,"showWeaponSec":false,"showWeaponMel":false}' > "$OUT/cfg-b/krunknative/knSettings.json"
python3 -I - "$SRC/tools/kref/players-scenario.json" "$OUT" "$AX" "$AY" "$AZ" "$DIST" "$START" "$STEP" <<'PY'
import json, sys, math
scen, out, ax, ay, az, dist, start, step = sys.argv[1], sys.argv[2], *map(float, sys.argv[3:9])
ev = json.load(open(scen))
end = max(e[0] for e in ev)
a = ['0 play', f'0 tp {ax} {ay} {az} 0 0']
for e in ev:
    t = e[0] + start
    if e[1] in ('down', 'up', 'mdown', 'mup'): a.append(f'{t:.0f} {e[1]} {e[2]}')
    elif e[1] == 'look': a.append(f'{t:.0f} look {e[2]} {e[3]}')
a.append(f'{end + start + 1500:.0f} quit')
b = ['0 play', f'0 tp {ax + dist} {ay} {az} {math.pi / 2} 0', f'1500 tp {ax + dist} {ay} {az} {math.pi / 2} 0']
t = 0.0
while t < end + start + 3000:
    b.append(f'{t:.0f} shot {out}/frames/b-{int(t):05d}.png'); t += step
b.append(f'{end + start + 3000:.0f} quit')
open(out + '/a.script', 'w').write('\n'.join(a) + '\n')
open(out + '/b.script', 'w').write('\n'.join(b) + '\n')
PY
cd "$SRC" || exit 1
RUN=(env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE="${EGL_DEVICE:-nvidia}" KRUNKNATIVE_SERVER_PORT=$PORT KRUNKNATIVE_WINDOW_SIZE=960x540)
"$BUILD/server_bin" --port "$PORT" --headless assets/maps/sandstorm.json > "$OUT/server.log" 2>&1 &
SERVER=$!
sleep 2
XDG_CONFIG_HOME=$OUT/cfg-b XDG_STATE_HOME=$OUT/cfg-b KRUNKNATIVE_HIDE_UI=1 KRUNKNATIVE_INPUT_SCRIPT=$OUT/b.script KRUNKNATIVE_PLAYER_LOG=$OUT/native-frames.jsonl \
  timeout 120 "${RUN[@]}" "$BUILD/client_bin" > "$OUT/b.log" 2>&1 &
B=$!
sleep 4
XDG_CONFIG_HOME=$OUT/cfg-a XDG_STATE_HOME=$OUT/cfg-a KRUNKNATIVE_INPUT_SCRIPT=$OUT/a.script \
  timeout 120 "${RUN[@]}" "$BUILD/client_bin" > "$OUT/a.log" 2>&1 &
A=$!
wait $A $B
kill $SERVER 2>/dev/null; wait $SERVER 2>/dev/null
echo "native: $(wc -l < "$OUT/native-frames.jsonl") logged frames, $(ls "$OUT/frames" | wc -l) shots"
