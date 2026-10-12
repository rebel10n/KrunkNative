#!/bin/bash
# Renders every tools/canvas2d scene with canvas2d_test (headless GL) and with headless
# Chromium's Canvas 2D (ref.js: the v7 GAME.OVERLAY code), then pixel-diffs them.
#   tools/canvas2d/run.sh [build dir] [scene names...]
# env: OUT (default /tmp/canvas2d), CHROME (default google-chrome), KRUNKNATIVE_EGL_DEVICE.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD=${1:-$ROOT/cmake-build-debug}
shift || true
OUT=${OUT:-/tmp/canvas2d}
CHROME=${CHROME:-google-chrome}
mkdir -p "$OUT"
SCENES=("$@")
[ ${#SCENES[@]} -eq 0 ] && SCENES=($(cd "$ROOT/tools/canvas2d/scenes" && ls *.txt | sed 's/\.txt$//'))
status=0
cd "$ROOT"
for s in "${SCENES[@]}"; do
    scene=tools/canvas2d/scenes/$s.txt
    read -r W H < <(awk '/^@size/ {print $2, $3; f=1} END {if (!f) print 800, 600}' "$scene")
    DPR=$(awk '/^@dpr/ {print $2; f=1} END {if (!f) print 1}' "$scene")
    env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 XDG_CONFIG_HOME="$OUT/cfg" \
        "$BUILD/canvas2d_test" "$scene" "$OUT/$s.native.png" > "$OUT/$s.native.txt" 2> "$OUT/$s.native.log"
    rc=$?
    [ $rc -ne 0 ] && { echo "$s: canvas2d_test exit $rc"; status=1; }
    node tools/canvas2d/ref.js "$scene" "$OUT/$s.html"
    "$CHROME" --headless=new --disable-gpu --hide-scrollbars --no-first-run --no-default-browser-check \
        --user-data-dir="$OUT/chrome-profile" --force-device-scale-factor="$DPR" --window-size="$W,$H" \
        --virtual-time-budget=5000 --screenshot="$OUT/$s.chrome.png" "file://$OUT/$s.html" > /dev/null 2>&1
    "$CHROME" --headless=new --disable-gpu --no-first-run --user-data-dir="$OUT/chrome-profile" \
        --virtual-time-budget=5000 --dump-dom "file://$OUT/$s.html" 2> /dev/null \
        | python3 -I -c 'import sys, re, html; m = re.search(r"<pre id=.out.>(.*?)</pre>", sys.stdin.read(), re.S); print(html.unescape(m.group(1)) if m else "")' \
        > "$OUT/$s.chrome.txt"
    printf '%-10s ' "$s"
    python3 -I tools/canvas2d/diff.py "$OUT/$s.native.png" "$OUT/$s.chrome.png" "$OUT/$s.diff.png" || status=1
    if ! diff -q <(grep -- '->' "$OUT/$s.native.txt") <(grep -- '->' "$OUT/$s.chrome.txt") > /dev/null; then
        echo "  return values differ:"; diff <(grep -- '->' "$OUT/$s.native.txt") <(grep -- '->' "$OUT/$s.chrome.txt") | sed 's/^/    /'
    fi
done
exit $status
