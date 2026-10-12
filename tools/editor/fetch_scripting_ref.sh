#!/usr/bin/env bash
# Krunker's KrunkScript window (scripting.html), which the v7.1.4 reference tree lacks
# (doc/v7/editor.md 1.8), recovered for the reference capture (tools/editor/v7ref.js scripting*):
#   - scripting.html and css/scripting.css from the Wayback Machine's 2025-02-03 krunker.io
#     snapshot (the v7.1.4 editor page loads monokai like krunker.io's from 2024 on; every
#     snapshot 2021-10 .. 2025-02 has the same markup, mode, hint code and CSS apart from build
#     ids, the GameFont file and an unused iconC rule),
#   - the CodeMirror 5.61.1 files it loads from cdnjs.
# Into DIR (default ${XDG_CACHE_HOME:-~/.cache}/krunknative/scripting-ref), never into the repo:
# the page's script is Krunker's. The cdnjs URLs are rewritten to /kn-cm/..., which v7ref.js
# serves next to the reference tree. Every file is checked against its pinned SHA-256.
#
#   tools/editor/fetch_scripting_ref.sh [DIR]
set -euo pipefail

DIR=${1:-${XDG_CACHE_HOME:-$HOME/.cache}/krunknative/scripting-ref}
WB=https://web.archive.org/web
CM=https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.61.1

HERE=$(cd "$(dirname "$0")" && pwd)
# path in DIR | URL (the hashes of the decoded files: scripting_ref.sha256)
FILES="
scripting.orig.html|$WB/20250203172046id_/https://krunker.io/scripting.html
css/scripting.css|$WB/20250203172047id_/https://krunker.io/css/scripting.css?build=bDnK6
"
CM_FILES="codemirror.min.css theme/material-palenight.min.css addon/dialog/dialog.min.css addon/hint/show-hint.css
addon/fold/foldgutter.css codemirror.min.js addon/search/search.min.js addon/search/searchcursor.min.js
addon/search/jump-to-line.min.js addon/dialog/dialog.min.js addon/selection/active-line.min.js
addon/edit/matchbrackets.min.js addon/edit/closebrackets.min.js addon/mode/simple.js addon/hint/show-hint.js
addon/comment/comment.js addon/selection/mark-selection.js addon/fold/foldcode.js addon/fold/foldgutter.js
addon/fold/brace-fold.js"

mkdir -p "$DIR"
fetch() { # fetch REL URL: decoded (gzip / brotli transfer the archive keeps) into DIR/REL
    local rel=$1 url=$2 tmp
    mkdir -p "$(dirname "$DIR/$rel")"
    tmp=$(mktemp "$DIR/.dl.XXXXXX")
    curl -fsSL --retry 3 -m 120 -o "$tmp" "$url"
    if gzip -t "$tmp" 2>/dev/null; then gzip -dc < "$tmp" > "$DIR/$rel"; rm -f "$tmp"
    else mv "$tmp" "$DIR/$rel"; fi
}

while IFS='|' read -r rel url; do
    [ -n "$rel" ] || continue
    [ -s "$DIR/$rel" ] || fetch "$rel" "$url"
done <<< "$FILES"
for f in $CM_FILES; do
    [ -s "$DIR/kn-cm/$f" ] || fetch "kn-cm/$f" "$CM/$f"
done

# the page served by v7ref.js: cdnjs -> /kn-cm, build query strings dropped
sed -e 's#https://cdnjs.cloudflare.com/ajax/libs/codemirror/5.61.1/#/kn-cm/#g' -e 's#?build=[A-Za-z0-9]*##g' \
    "$DIR/scripting.orig.html" > "$DIR/scripting.html"

(cd "$DIR" && sha256sum --quiet -c "$HERE/scripting_ref.sha256")
echo "scripting reference in $DIR"
