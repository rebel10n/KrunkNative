#!/bin/bash
# tools/uidiff/paint.sh <ui_test> [out-dir]
#
# The paint pages (tools/uidiff/paint/*.html: a mid-flip reward card with 3D transforms, masked
# icons, background-clip: text, color-mix, outline-offset) rendered by `ui_test render-page` and
# pixel-diffed against Chrome 152 captures of the same pages (doc/ref/krunknative/paint/, headless,
# 1280x720). The page is the whole ui/ tree with its index.html swapped in: a private
# KRUNKNATIVE_ROOT whose ui/ entries are symlinks into the repository.
#
# Exit status 0 when every page is within its threshold, 1 otherwise, 2 on usage errors.
set -u
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BIN=${1:-}
if [ -z "$BIN" ] || [ ! -x "$BIN" ]; then
    echo "usage: tools/uidiff/paint.sh <ui_test> [out-dir]" >&2
    exit 2
fi
OUT=${2:-$ROOT/cmake-build-debug/uidiff/paint}
mkdir -p "$OUT/root/ui" "$OUT/cfg"
ln -sfn "$ROOT/assets" "$OUT/root/assets"
for f in "$ROOT"/ui/* "$ROOT"/ui/.[!.]*; do
    b=$(basename "$f")
    [ "$b" = index.html ] || ln -sfn "$f" "$OUT/root/ui/$b"
done

# max % of pixels that may differ (fuzz 6%): the flip is geometry only; the mask page's
# KRUNKER gradient text is laid out with the fallback font, so it gets a looser bound.
threshold() {
    case "$1" in
        flip-mid) echo 0.5 ;;
        mask-icons) echo 2.0 ;;
        backdrop-tex) echo 1.5 ;;
        *) echo 1.0 ;;
    esac
}

rc=0
for page in "$ROOT"/tools/uidiff/paint/*.html; do
    n=$(basename "$page" .html)
    ref="$ROOT/doc/ref/krunknative/paint/$n.png"
    cp "$page" "$OUT/root/ui/index.html"
    if ! env -u DISPLAY -u WAYLAND_DISPLAY KRUNKNATIVE_HEADLESS=1 KRUNKNATIVE_AUDIO_BACKEND=null \
        XDG_CONFIG_HOME="$OUT/cfg" KRUNKNATIVE_ROOT="$OUT/root" \
        "$BIN" render-page 1280 720 '#000000' "$OUT/$n.png" --frames 3 > "$OUT/$n.log" 2>&1; then
        echo "FAIL $n: render (see $OUT/$n.log)"
        rc=1
        continue
    fi
    ae=$(magick compare -metric AE -fuzz 6% "$OUT/$n.png" "$ref" null: 2>&1 | awk '{print $1}')
    pct=$(awk -v a="$ae" 'BEGIN { printf "%.3f", 100 * a / (1280 * 720) }')
    max=$(threshold "$n")
    if awk -v p="$pct" -v m="$max" 'BEGIN { exit !(p + 0 <= m + 0) }'; then
        echo "PASS $n: $pct% differ (max $max%)"
    else
        echo "FAIL $n: $pct% differ (max $max%), diff in $OUT/$n.png"
        rc=1
    fi
done
exit $rc
