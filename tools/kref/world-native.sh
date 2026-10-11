#!/bin/bash
# KrunkNative world capture at a pinned camera pose (client/src/debug_capture.c).
# usage: world-native.sh <map name | map.json path> "<x,y,z,pitch,yaw>" <out.png> [build dir]
# Env: SRC (repo root to run from, default: this checkout), CFG (XDG_CONFIG_HOME so the
# user's stored settings do not leak in; default an empty /tmp dir; put a
# krunknative/knSettings.json there for setting runs, e.g. {"lighting":2}), FOV (default 90),
# EGL_DEVICE=nvidia|mesa|<n> renders headless instead of under Xvfb (no window and no X
# server, client/include/headless.h): nvidia for NVIDIA-only driver bugs. Headless frames
# come faster than Xvfb ones, so wall-clock animation (grass sway) can differ by a few px.
MAP=$1; POSE=$2; OUT=$3; BUILD=${4:-$(dirname "$0")/../../cmake-build-debug}
SRC=${SRC:-$(cd "$(dirname "$0")/../.." && pwd)}
CFG=${CFG:-/tmp/kn-world-cfg}; mkdir -p "$CFG"
case "$OUT" in /*) ;; *) OUT="$PWD/$OUT" ;; esac
case "$BUILD" in /*) ;; *) BUILD="$PWD/$BUILD" ;; esac
case "$MAP" in *.json) MAPFILE=$(cd "$(dirname "$MAP")" && pwd)/$(basename "$MAP") ;; *) MAPFILE="assets/maps/$MAP.json" ;; esac
cd "$SRC" || exit 1
if [ -z "${EGL_DEVICE:-}" ]; then
  RUN=(xvfb-run -a -s "-screen 0 1280x720x24" env LIBGL_ALWAYS_SOFTWARE=1)
else
  RUN=(env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE="$EGL_DEVICE")
fi
XDG_CONFIG_HOME=$CFG KRUNKNATIVE_RESOLUTION=1 KRUNKNATIVE_CAMERA="$POSE" KRUNKNATIVE_HIDE_UI=1 \
KRUNKNATIVE_SCREENSHOT="$OUT" KRUNKNATIVE_FOV=${FOV:-90} \
  timeout 120 "${RUN[@]}" "$BUILD/client_bin" --offline "$MAPFILE" 2>&1 | grep -E "debug capture|GL_RENDERER"
