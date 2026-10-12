#!/bin/bash
# usage: nativecap.sh <build dir> <src root> <out dir> <map>...
# Xvfb + llvmpipe by default; EGL_DEVICE=nvidia|mesa|<n> renders headless on that EGL
# device instead (no window, no X server; client/include/headless.h).
BIN=$1/client_bin; ROOT=$2; OUT=$3; shift 3; mkdir -p $OUT
if [ -z "${EGL_DEVICE:-}" ]; then
  RUN=(xvfb-run -a -s "-screen 0 1280x720x24" env LIBGL_ALWAYS_SOFTWARE=1)
else
  RUN=(env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE="$EGL_DEVICE")
fi
for m in "$@"; do
  python3 -I -c "import json
for p in json.load(open('${POSES:-/home/user/src/KrunkNative/doc/ref/krunker-v3.1.1/sky/poses.json}'))['$m']:
    x,y,z=p['pos']; print(p['name'], f\"{x},{y},{z},{p['pitch']},{p['yaw']}\")" | while read name cam; do
    (cd $ROOT && KRUNKNATIVE_CAMERA="$cam" KRUNKNATIVE_HIDE_UI=1 KRUNKNATIVE_FOV=90 KRUNKNATIVE_CAPTURE_FRAMES=20 KRUNKNATIVE_SCREENSHOT=$OUT/$m-$name.png \
      timeout 120 "${RUN[@]}" $BIN --offline assets/maps/311_$m.json > $OUT/$m-$name.log 2>&1) || echo "FAIL $m $name"
    [ -f $OUT/$m-$name.png ] || echo "MISSING $m-$name"
  done
done
