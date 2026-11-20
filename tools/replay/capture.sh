#!/usr/bin/env bash
# K.P.D replay playback capture (doc/v10/replays.md 3): records a demo on each map with server_bin
# (server/tests/kpd_demo_e2e.py), plays it headless with client_bin --demo and screenshots the replay
# a fixed number of frames in. Checks the replay started, reached its world and drew the viewer bar.
#
#   tools/replay/capture.sh [BUILD_DIR] [OUT_DIR] [MAPS...]     (defaults: cmake-build-debug,
#                                                                ~/.cache/krunknative/replay, Sandstorm Kanji)
# Env: KRUNKNATIVE_EGL_DEVICE (default mesa), FRAMES (default 150 frames of 33.333 ms).
set -euo pipefail
cd "$(dirname "$0")/../.."
BUILD=${1:-cmake-build-debug}
OUT=${2:-$HOME/.cache/krunknative/replay}
shift $(( $# > 2 ? 2 : $# )) || true
MAPS=("$@")
[ ${#MAPS[@]} -eq 0 ] && MAPS=(Sandstorm Kanji)
FRAMES=${FRAMES:-150}
mkdir -p "$OUT/demos" "$OUT/cfg" "$OUT/tmp"
fail=0
for map in "${MAPS[@]}"; do
    key=$(echo "$map" | tr 'A-Z' 'a-z')
    rm -f "$OUT/demos/$key-"*.kndemo
    if ! TMPDIR="$OUT/tmp" python3 -I server/tests/kpd_demo_e2e.py "$BUILD" "$map" "$OUT/demos" > "$OUT/$key-record.log" 2>&1; then
        echo "FAIL  $map: recording (see $OUT/$key-record.log)"; fail=1; continue
    fi
    demo=$(ls "$OUT/demos/$key-"*.kndemo | head -1)
    png="$OUT/$key.png"
    rm -f "$png"
    env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE="${KRUNKNATIVE_EGL_DEVICE:-mesa}" \
        XDG_CONFIG_HOME="$OUT/cfg" KRUNKNATIVE_FIXED_DELTA=33.333 KRUNKNATIVE_CAPTURE_FRAMES="$FRAMES" \
        KRUNKNATIVE_REPLAY_LOG=1 KRUNKNATIVE_SCREENSHOT="$png" \
        timeout 180 "$BUILD/client_bin" --demo "$demo" > "$OUT/$key-play.log" 2>&1 || true
    if ! grep -q '^\[replay\] demo ' "$OUT/$key-play.log"; then echo "FAIL  $map: the replay did not start"; fail=1; continue; fi
    if ! grep -qE '^\[replay\] [0-9]+ / ' "$OUT/$key-play.log"; then echo "FAIL  $map: the playhead never moved"; fail=1; continue; fi
    if [ ! -s "$png" ]; then echo "FAIL  $map: no screenshot"; fail=1; continue; fi
    echo "PASS  $map: $(basename "$demo") -> $png ($(grep -cE '^\[replay\] [0-9]+ / ' "$OUT/$key-play.log") s played)"
done
exit $fail
