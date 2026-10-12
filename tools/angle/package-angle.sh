#!/usr/bin/env bash
# Packs an ANGLE_DIR staged by build-angle.sh into the prebuilt release asset that CI downloads
# (tools/angle/fetch-angle.sh) and prints its angle.lock line:
#
#   <outdir>/angle-<commit12>-<platform>.tar.gz   ANGLE_BUILD, lib/libEGL.*, lib/libGLESv2.*, include/
#
# Usage: tools/angle/package-angle.sh <angle-dir> <platform> [<outdir>]
#
#   <platform>  macos-arm64 | macos-x86_64 (the asset suffix and the angle.lock key)
#
# Publish (the `angle` workflow does all of this, or by hand from a Mac):
#   gh release create angle-<commit12> --title "ANGLE <commit12> (prebuilt)" --notes "..." || true
#   gh release upload angle-<commit12> <outdir>/angle-<commit12>-<platform>.tar.gz --clobber
# then put the printed `sha256 <platform> <hash>` line into tools/angle/angle.lock.
set -euo pipefail

[ $# -ge 2 ] || { sed -n '2,15p' "$0" >&2; exit 2; }
DIR="$1" PLATFORM="$2" OUT="${3:-.}"
HERE="$(cd "$(dirname "$0")" && pwd)"
# shellcheck disable=SC2016 # a literal ${ in the sed pattern
PIN="$(sed -n 's/^ANGLE_COMMIT="\${ANGLE_COMMIT:-\([0-9a-f]*\)}"$/\1/p' "$HERE/build-angle.sh")"
[ -n "$PIN" ] || { echo "package-angle.sh: no ANGLE_COMMIT pin in build-angle.sh" >&2; exit 1; }

case "$PLATFORM" in
    macos-arm64) CPU=arm64 SO=dylib ;;
    macos-x86_64) CPU=x64 SO=dylib ;;
    *) echo "package-angle.sh: unknown platform $PLATFORM" >&2; exit 2 ;;
esac
[ -f "$DIR/ANGLE_BUILD" ] && [ -f "$DIR/lib/libEGL.$SO" ] && [ -f "$DIR/lib/libGLESv2.$SO" ] \
    || { echo "package-angle.sh: $DIR is not an ANGLE build (run build-angle.sh)" >&2; exit 1; }
STAMP="$(cat "$DIR/ANGLE_BUILD")"
case "$STAMP" in "angle=$PIN "*) ;; *) echo "package-angle.sh: $DIR is not ANGLE $PIN: $STAMP" >&2; exit 1 ;; esac
case "$STAMP" in *"target_cpu=\"$CPU\""*) ;; *) echo "package-angle.sh: $DIR is not target_cpu=$CPU" >&2; exit 1 ;; esac
case "$STAMP" in *angle_enable_swiftshader=true*) echo "package-angle.sh: $DIR is a --swiftshader build; package the default one" >&2; exit 1 ;; esac
if command -v lipo > /dev/null; then
    want="$([ "$CPU" = x64 ] && echo x86_64 || echo arm64)"
    for lib in libEGL libGLESv2; do
        lipo -archs "$DIR/lib/$lib.$SO" | grep -qw "$want" || { echo "package-angle.sh: $lib.$SO is not $want" >&2; exit 1; }
    done
fi

ASSET="angle-${PIN:0:12}-$PLATFORM.tar.gz"
mkdir -p "$OUT"
# COPYFILE_DISABLE: no macOS ._* AppleDouble entries in the archive
COPYFILE_DISABLE=1 tar -czf "$OUT/$ASSET" -C "$DIR" ANGLE_BUILD "lib/libEGL.$SO" "lib/libGLESv2.$SO" include
if command -v sha256sum > /dev/null; then SUM="$(sha256sum "$OUT/$ASSET" | cut -d' ' -f1)"
else SUM="$(shasum -a 256 "$OUT/$ASSET" | cut -d' ' -f1)"; fi
echo "$OUT/$ASSET" >&2
echo "sha256 $PLATFORM $SUM"
