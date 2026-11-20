#!/usr/bin/env bash
# AGENTS.md "Ray tracing": with rtMode 0 (the default) the frame must stay pixel-identical. Pinned-camera
# captures of the offline game on Sandstorm and Kanji from two client_bin builds (a baseline commit's and
# this tree's), with the page and world-only (KRUNKNATIVE_HIDE_UI=1), compared pixel for pixel.
#
# Some pixels move from run to run of the same binary (wall-clock animation: Kanji's billboard video, the
# menu's class preview idle and the click-to-play pulse). Each build is captured RUNS times; the pixels that
# differ between runs of the same build are that capture's noise, and the two builds must agree on every
# other pixel (and the noise must be the same small set: it is reported).
#
#   tools/replay/rt0_identity.sh BASE_CLIENT_BIN NEW_CLIENT_BIN [OUT_DIR]
# Env: KRUNKNATIVE_EGL_DEVICE (default mesa), RUNS (default 6), MAPS ("sandstorm kanji"). Exit 0 only when every pair agrees.
set -uo pipefail
cd "$(dirname "$0")/../.."
BASE=$1; NEW=$2; OUT=${3:-$HOME/.cache/krunknative/rt0}
RUNS=${RUNS:-6}
mkdir -p "$OUT"
POSES=("sandstorm|0,22,40,-0.15,0.6" "kanji|0,30,0,-0.2,2.3")
[ -n "${MAPS:-}" ] && POSES=($(for e in "${POSES[@]}"; do case " $MAPS " in *" ${e%%|*} "*) echo "$e";; esac; done))
fail=0
cap() { # bin tag map pose hide run
    local cfg="$OUT/cfg-$2-$3-$5-$6"; rm -rf "$cfg"; mkdir -p "$cfg"
    env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE="${KRUNKNATIVE_EGL_DEVICE:-mesa}" \
        KRUNKNATIVE_AUDIO_BACKEND=null XDG_CONFIG_HOME="$cfg" KRUNKNATIVE_FIXED_DELTA=16.667 KRUNKNATIVE_CAPTURE_FRAMES=60 \
        KRUNKNATIVE_CAMERA="$4" KRUNKNATIVE_HIDE_UI="$5" KRUNKNATIVE_SCREENSHOT="$OUT/$2-$3-ui$5-$6.png" \
        timeout 180 "$1" --offline --map "$3" > "$OUT/$2-$3-ui$5-$6.log" 2>&1
}
for entry in "${POSES[@]}"; do
    map=${entry%%|*}; pose=${entry#*|}
    for hide in 0 1; do
        for r in $(seq 1 "$RUNS"); do
            cap "$BASE" base "$map" "$pose" "$hide" "$r"
            cap "$NEW" new "$map" "$pose" "$hide" "$r"
        done
        label="$map $( [ "$hide" = 1 ] && echo world-only || echo with-UI )"
        if ! python3 -I - "$OUT" "$map" "$hide" "$RUNS" "$label" <<'PY'
import sys, numpy as np
from PIL import Image
out, m, hide, runs, label = sys.argv[1], sys.argv[2], sys.argv[3], int(sys.argv[4]), sys.argv[5]
load = lambda tag, r: np.asarray(Image.open(f"{out}/{tag}-{m}-ui{hide}-{r}.png").convert("RGBA"), dtype=np.int16)
base = [load("base", r) for r in range(1, runs + 1)]
new = [load("new", r) for r in range(1, runs + 1)]
noise = np.zeros(base[0].shape[:2], bool)
for imgs in (base, new):
    for i in imgs[1:]:
        noise |= np.any(i != imgs[0], axis=2)
diff = np.any(base[0] != new[0], axis=2) & ~noise
total = noise.size
print(("PASS" if not diff.any() else "FAIL") + f"  {label}: {int(diff.sum())} pixels differ outside the run-to-run noise "
      f"({int(noise.sum())} noisy pixels, {100.0 * noise.sum() / total:.2f}%)")
if diff.any():
    Image.fromarray((diff * 255).astype(np.uint8)).save(f"{out}/diff-{m}-ui{hide}.png")
sys.exit(1 if diff.any() else 0)
PY
        then fail=1; fi
    done
done
exit $fail
