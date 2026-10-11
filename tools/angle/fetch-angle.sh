#!/usr/bin/env bash
# Installs the prebuilt ANGLE recorded in tools/angle/angle.lock into an ANGLE_DIR, instead of
# building it (build-angle.sh, ~10-65 min). Used by CI (.github/workflows/build.yml) and works
# locally too.
#
# Usage: tools/angle/fetch-angle.sh <platform> <angle-dir>
#
#   <platform>  macos-arm64 | macos-x86_64
#
# Prints `available=true|false` on stdout (a $GITHUB_OUTPUT line); everything else goes to
# stderr. available=false (exit 0) when angle.lock has no asset for <platform> or was written
# for another ANGLE pin than build-angle.sh's: publish one with the `angle` workflow. A listed
# asset that cannot be downloaded or fails its sha256 is an error (exit 1).
set -euo pipefail

[ $# -eq 2 ] || { sed -n '2,13p' "$0" >&2; exit 2; }
PLATFORM="$1" DIR="$2"
HERE="$(cd "$(dirname "$0")" && pwd)"
LOCK="$HERE/angle.lock"
REPO="${KN_ANGLE_REPO:-rebel10n/KrunkNative}"

# shellcheck disable=SC2016 # a literal ${ in the sed pattern
PIN="$(sed -n 's/^ANGLE_COMMIT="\${ANGLE_COMMIT:-\([0-9a-f]*\)}"$/\1/p' "$HERE/build-angle.sh")"
LOCK_PIN="$(awk '$1 == "commit" { print $2 }' "$LOCK")"
SUM="$(awk -v p="$PLATFORM" '$1 == "sha256" && $2 == p { print $3 }' "$LOCK")"
unavailable() { echo "::warning::fetch-angle.sh: $1" >&2; echo "available=false"; exit 0; }
[ -n "$PIN" ] || { echo "fetch-angle.sh: no ANGLE_COMMIT pin in build-angle.sh" >&2; exit 1; }
[ "$LOCK_PIN" = "$PIN" ] || unavailable "angle.lock is for ANGLE ${LOCK_PIN:-?}, build-angle.sh pins $PIN: no prebuilt ANGLE"
[ -n "$SUM" ] || unavailable "no prebuilt ANGLE for $PLATFORM in angle.lock (run the angle workflow)"

ASSET="angle-${PIN:0:12}-$PLATFORM.tar.gz"
URL="https://github.com/$REPO/releases/download/angle-${PIN:0:12}/$ASSET"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
echo "fetch-angle.sh: $URL" >&2
curl -fsSL --retry 3 --retry-delay 5 -o "$TMP/$ASSET" "$URL"
if command -v sha256sum > /dev/null; then GOT="$(sha256sum "$TMP/$ASSET" | cut -d' ' -f1)"
else GOT="$(shasum -a 256 "$TMP/$ASSET" | cut -d' ' -f1)"; fi
[ "$GOT" = "$SUM" ] || { echo "fetch-angle.sh: $ASSET sha256 $GOT, angle.lock says $SUM" >&2; exit 1; }

rm -rf "${DIR:?}/lib" "${DIR:?}/include" "$DIR/ANGLE_BUILD"
mkdir -p "$DIR"
tar -xzf "$TMP/$ASSET" -C "$DIR"
echo "fetch-angle.sh: ANGLE ${PIN:0:12} ($PLATFORM) in $DIR" >&2
echo "available=true"
