#!/usr/bin/env bash
# Pipeline self-test (no ui_test binary needed):
#   1. render = reference            -> every region must PASS (exit 0)
#   2. render = reference rolled 3px -> text/flat regions must FAIL (exit 1)
set -euo pipefail
here=$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)
out=${UIDIFF_OUT:-/tmp/uidiff-selftest}
scenarios=("$@")
[ ${#scenarios[@]} -gt 0 ] || scenarios=(menu-1280 ingame-plain)

echo "== self-test 1: render == reference (expect PASS)"
UIDIFF_OUT="$out/pass" UIDIFF_RENDER_CMD=cp "$here/uidiff.sh" /nonexistent "${scenarios[@]}"

echo
echo "== self-test 2: render shifted by 3 px (expect FAIL)"
if UIDIFF_OUT="$out/fail" UIDIFF_RENDER_CMD="$here/stub-render.sh" UIDIFF_STUB_SHIFT=+3+0 \
    "$here/uidiff.sh" /nonexistent "${scenarios[@]}"; then
    echo "selftest: shifted render unexpectedly passed" >&2
    exit 1
fi
echo
echo "selftest: OK (identical render passes, shifted render fails)"
