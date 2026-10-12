#!/bin/bash
# WP-F viewer render scenarios: client_bin --viewer "<query>" headless (frozen clock, no panel)
# against the Krunker v7.1.4 viewer captures in doc/ref/krunker-v7.1.4/viewer/ (RMSE, ImageMagick).
# usage: tools/kref/viewer-diff.sh [build_dir=cmake-build-debug] [out_dir=~/.cache/kn-viewer-diff]
#   env EGL_DEVICE (nvidia|mesa|<index>), ONLY=<name regex>
#   REFS=1: (re)capture the references instead (needs the v7 public/ served on 127.0.0.1:6790
#   and headless Chromium, see tools/kref/viewer-ref.js)
set -u
SRC=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$SRC/cmake-build-debug}
OUT=${2:-$HOME/.cache/kn-viewer-diff}
case "$BUILD" in /*) ;; *) BUILD="$PWD/$BUILD" ;; esac
REF=$SRC/doc/ref/krunker-v7.1.4/viewer
mkdir -p "$OUT/cfg"
pass=0 fail=0
while IFS='|' read -r name query max actions; do
    case "$name" in ''|'#'*) continue ;; esac
    [ -n "${ONLY:-}" ] && ! [[ "$name" =~ $ONLY ]] && continue
    if [ -n "${REFS:-}" ]; then
        GUI_ACTIONS="${actions:-}" node "$SRC/tools/kref/viewer-ref.js" "$query" "$REF/$name.png" 800 600 3500 >/dev/null && echo "ref $name" || echo "ref $name FAILED"
        continue
    fi
    (cd "$SRC" && env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_EGL_DEVICE="${EGL_DEVICE:-nvidia}" \
        XDG_CONFIG_HOME="$OUT/cfg" KRUNKNATIVE_WINDOW_SIZE=800x600 KRUNKNATIVE_VIEWER_FREEZE=1 KRUNKNATIVE_HIDE_UI=1 KRUNKNATIVE_VIEWER_GUI="${actions:-}" \
        KRUNKNATIVE_CAPTURE_FRAMES=60 KRUNKNATIVE_SCREENSHOT="$OUT/$name.png" \
        timeout 90 "$BUILD/client_bin" --offline --viewer "$query" > "$OUT/$name.log" 2>&1)
    rmse=$(magick compare -metric RMSE "$REF/$name.png" "$OUT/$name.png" null: 2>&1 | sed -n 's/.*(\([0-9.e-]*\)).*/\1/p')
    if [ -n "$rmse" ] && awk "BEGIN{exit !($rmse <= $max)}"; then
        echo "PASS  $name  rmse $rmse (max $max)"; pass=$((pass + 1))
    else
        echo "FAIL  $name  rmse ${rmse:-?} (max $max)  $OUT/$name.png"; fail=$((fail + 1))
    fi
done < "$SRC/tools/kref/viewer-scenarios.txt"
[ -n "${REFS:-}" ] && exit 0
echo "viewer-diff: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
