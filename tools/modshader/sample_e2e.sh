#!/usr/bin/env bash
# Mod shader passes end to end (client/include/mod_shader.h): the sample mods in client/tests/data/mod_shader/
# (greyscale with a param, invert) zipped as shaders/<name>.json, loaded through a copy of Sandstorm whose
# modURL points at them, rendered headless from a pinned camera world-only, and compared with the same map
# without a mod: the grey frame must be grey and keep the plain frame's luminance, the invert frame its complement.
#
#   tools/modshader/sample_e2e.sh [CLIENT_BIN] [OUT_DIR]   (cmake-build-debug/client_bin, ~/.cache/krunknative/modshader)
# Env: KRUNKNATIVE_EGL_DEVICE (default mesa).
set -uo pipefail
cd "$(dirname "$0")/../.."
BIN=${1:-cmake-build-debug/client_bin}
OUT=${2:-$HOME/.cache/krunknative/modshader}
POSE="0,22,40,-0.15,0.6" # rt0_identity.sh's Sandstorm pose
PACKS=assets/user-assets/kn_modshader_e2e # (assets/user-assets is not tracked) removed on exit
mkdir -p "$OUT" "$PACKS"
trap 'rm -rf "$PACKS"' EXIT
python3 -I - "$PACKS" "$OUT" <<'PY'
import json, sys, zipfile
packs, out = sys.argv[1], sys.argv[2]
for name in ("grey", "invert"):
    with zipfile.ZipFile(f"{packs}/{name}.zip", "w") as z:
        z.writestr("shaders/", b"")  # a directory entry is skipped
        z.write(f"client/tests/data/mod_shader/{name}.json", f"shaders/{name}.json")
    m = json.load(open("assets/maps/311_sandstorm.json"))
    m["modURL"] = f"https://user-assets.krunker.io/kn_modshader_e2e/{name}.zip"
    json.dump(m, open(f"{out}/sandstorm-{name}.json", "w"))
m = json.load(open("assets/maps/311_sandstorm.json"))
json.dump(m, open(f"{out}/sandstorm-plain.json", "w"))
PY
fail=0
for tag in plain grey invert; do
    cfg="$OUT/cfg-$tag"; rm -rf "$cfg"; mkdir -p "$cfg"
    env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE="${KRUNKNATIVE_EGL_DEVICE:-mesa}" \
        KRUNKNATIVE_AUDIO_BACKEND=null XDG_CONFIG_HOME="$cfg" KRUNKNATIVE_FIXED_DELTA=16.667 KRUNKNATIVE_CAPTURE_FRAMES=60 \
        KRUNKNATIVE_CAMERA="$POSE" KRUNKNATIVE_HIDE_UI=1 KRUNKNATIVE_SCREENSHOT="$OUT/$tag.png" \
        timeout 180 "$BIN" --offline "$OUT/sandstorm-$tag.json" > "$OUT/$tag.log" 2>&1
    [ -s "$OUT/$tag.png" ] || { echo "FAIL  $tag: no screenshot (see $OUT/$tag.log)"; fail=1; }
done
grep -q 'shader pass 1 added' "$OUT/grey.log" || { echo "FAIL  grey: the pass was not added (see $OUT/grey.log)"; fail=1; }
grep -q 'shader pass 1 added' "$OUT/invert.log" || { echo "FAIL  invert: the pass was not added (see $OUT/invert.log)"; fail=1; }
[ $fail = 0 ] || exit 1
python3 -I - "$OUT" <<'PY' || fail=1
import sys, numpy as np
from PIL import Image
out = sys.argv[1]
load = lambda t: np.asarray(Image.open(f"{out}/{t}.png").convert("RGB"), dtype=np.float64)
plain, grey, inv = load("plain"), load("grey"), load("invert")
ok = True
spread = lambda img: img.max(axis=2) - img.min(axis=2)
colourful = (spread(plain) > 8).mean()
greyish = (spread(grey) <= 2).mean()
lum = plain @ np.array([0.2126, 0.7152, 0.0722])
lum_err = np.abs(grey.mean(axis=2) - lum).mean()
inv_err = np.abs(inv - (255.0 - plain)).mean()
print(f"plain: {100 * colourful:.1f}% coloured pixels")
for label, good, text in (
        ("grey", colourful > 0.2 and greyish > 0.99, f"{100 * greyish:.2f}% of pixels have R=G=B (+-2)"),
        ("grey luminance", lum_err < 2.0, f"mean |grey - Rec.709 luminance of plain| = {lum_err:.2f}"),
        ("invert", inv_err < 2.0, f"mean |invert - (255 - plain)| = {inv_err:.2f}")):
    print(("PASS  " if good else "FAIL  ") + f"{label}: {text}")
    ok &= good
sys.exit(0 if ok else 1)
PY
exit $fail
