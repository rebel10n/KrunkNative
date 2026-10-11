#!/bin/sh
# Copies the assembled web client (default: ../../dist/web from `make web`) into
# platform/ios/web/ for the app bundle, without the game assets symlink; the tracked
# small assets (maps, img, shaders, skins.json) are bundled as web/assets/.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
src=${1:-$repo/dist/web}
[ -f "$src/client.wasm" ] && [ -f "$src/index.html" ] || { echo "no web client in $src: run 'make web' first" >&2; exit 1; }
rm -rf "$here/web"
mkdir -p "$here/web/assets"
(cd "$src" && tar -cf - --exclude=./assets --exclude=assets .) | (cd "$here/web" && tar -xf -)
for item in maps img shaders skins.json; do
  [ -e "$repo/assets/$item" ] && cp -R "$repo/assets/$item" "$here/web/assets/"
done
echo "bundled $(du -sh "$here/web" | cut -f1) web client into $here/web"
