#!/usr/bin/env bash
# Stand-in renderer for uidiff self-tests: writes the reference image as the
# "render", optionally shifted by UIDIFF_STUB_SHIFT="+dx+dy" (ImageMagick
# -roll syntax). Invoked by uidiff.sh as `stub-render.sh <scenario.json> <out.png>`
# with UIDIFF_REF/UIDIFF_W/UIDIFF_H/UIDIFF_SCENARIO exported.
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: $0 <scenario.json> <out.png>" >&2; exit 2; }
out=$2
shift_by=${UIDIFF_STUB_SHIFT:-}
if command -v magick >/dev/null 2>&1; then conv=(magick); else conv=(convert); fi
if [ -n "$shift_by" ]; then
    "${conv[@]}" "$UIDIFF_REF" -roll "$shift_by" "$out"
else
    cp "$UIDIFF_REF" "$out"
fi
