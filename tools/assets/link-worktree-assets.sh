#!/usr/bin/env bash
# Give a linked git worktree the game content: the gitignored assets/*/* (fonts, models,
# textures, sounds, videos, ...) that only the main checkout has (assets/README.md). Links what the
# worktree lacks one level inside each content directory (assets/<dir>/<entry> -> the main
# checkout's assets/, or KN_ASSETS_FROM=<dir>). Nothing is copied or committed: every link falls
# under .gitignore's assets/*/*. The tracked dirs (maps/ img/ shaders/ editor/) are left
# alone, so the worktree's own maps and shaders stay its own.
# Usage: tools/assets/link-worktree-assets.sh [--check]   (--check: exit 1 when content is missing)
set -euo pipefail

repo="$(git rev-parse --show-toplevel)"
dst="$repo/assets"
probe="css/fonts/font2.ttf" # the first content file the client loads (canvas2d.h)

if [ "${1:-}" = "--check" ]; then
    [ -e "$dst/$probe" ] && exit 0
    exit 1
fi

main="$(cd "$(git rev-parse --path-format=absolute --git-common-dir)/.." && pwd)"
src="${KN_ASSETS_FROM:-$main/assets}"
if [ "$(cd "$src" 2>/dev/null && pwd -P)" = "$(cd "$dst" && pwd -P)" ]; then
    [ -e "$dst/$probe" ] && exit 0
    echo "assets: no game content in $dst (gitignored): import it (make assets-import-v7) or set KN_ASSETS_FROM=<assets dir>" >&2
    exit 1
fi
if [ ! -e "$src/$probe" ]; then
    echo "assets: $src has no game content ($probe); set KN_ASSETS_FROM=<assets dir>" >&2
    exit 1
fi

tracked=" maps img shaders editor "
linked=0
for dir in "$src"/*/; do
    name="$(basename "$dir")"
    case "$tracked" in *" $name "*) continue ;; esac
    # a real directory with links one level down, so .gitignore's assets/*/* covers every link
    mkdir -p "$dst/$name"
    for f in "$src/$name"/* "$src/$name"/.[!.]*; do
        [ -e "$f" ] || continue
        t="$dst/$name/$(basename "$f")"
        [ -e "$t" ] || [ -L "$t" ] || { ln -s "$f" "$t"; linked=$((linked + 1)); }
    done
done
echo "assets: linked $linked entries from $src into $dst"
