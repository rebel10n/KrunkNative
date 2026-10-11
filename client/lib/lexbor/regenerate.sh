#!/usr/bin/env bash
# Regenerates lexbor.h / lexbor.c / LICENSE from the pinned upstream commit.
#
#   client/lib/lexbor/regenerate.sh [<existing lexbor checkout>]
#
# Without an argument the pinned commit is cloned into a fresh temporary
# directory. Needs git and perl. See README.md for the pin and the layout.
set -euo pipefail

LEXBOR_REPO="https://github.com/lexbor/lexbor"
LEXBOR_COMMIT="f4cbbcd91359a0ec9499e3ce7e263de629482d61"   # 2026-09-28, version 3.1.0
MODULES="html css selectors"

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
src="${1:-}"
tmp=""
if [ -z "$src" ]; then
    tmp="$(mktemp -d)"
    trap 'rm -rf "$tmp"' EXIT
    src="$tmp/lexbor"
    git clone -q "$LEXBOR_REPO" "$src"
fi
git -C "$src" checkout -q "$LEXBOR_COMMIT"
[ "$(git -C "$src" rev-parse HEAD)" = "$LEXBOR_COMMIT" ] || { echo "wrong commit"; exit 1; }

single="$(mktemp)"
# shellcheck disable=SC2086
perl "$src/single.pl" --port=posix $MODULES > "$single"

# The amalgamation is one file laid out as:
#   banner + license | Defines | Includes | Headers | Resources | Source Code
# lexbor.h gets banner + Includes + Headers (declarations, inline helpers and the
# static const callback tables upstream also keeps in headers); lexbor.c gets the
# feature-test Defines (they must come before any system header and must not leak
# into consumers), <dirent.h> (only the fs port uses it) and the Resources +
# Source Code sections.
awk -v out_h="$here/lexbor.h" -v out_c="$here/lexbor.c" '
function section(name) {
    # a section starts with a 3-line comment: "/*", " * Name:", " */"
    return (prev == "/*" && $0 == " * " name ":")
}
BEGIN { mode = "banner" }
{
    if (section("Defines"))     { mode = "defines";   buf_h = buf_h "\n" }
    if (section("Includes"))    { mode = "includes" }
    if (section("Headers"))     { mode = "headers" }
    if (section("Resources"))   { mode = "resources" }
    if (section("Source Code")) { mode = "sources" }

    if (mode == "banner")    { banner = banner $0 "\n" }
    if (mode == "defines")   { defines = defines $0 "\n" }
    if (mode == "includes")  { if ($0 != "#include <dirent.h>") includes = includes $0 "\n" }
    if (mode == "headers")   { headers = headers $0 "\n" }
    if (mode == "resources" || mode == "sources") { sources = sources $0 "\n" }
    prev = $0
}
END {
    # each buffer ends with the "/*" that opened the next section comment
    sub(/\/\*\n$/, "", banner)
    sub(/\/\*\n$/, "", defines)
    sub(/\/\*\n$/, "", includes)
    sub(/\/\*\n$/, "", headers)
    sub(/^\/\*\n/, "", defines)
    sub(/^\/\*\n/, "", includes)
    sub(/^\/\*\n/, "", headers)
    sub(/^\/\*\n/, "", sources)

    printf "%s", banner > out_h
    printf "/* KrunkNative: split into lexbor.h (declarations) + lexbor.c (implementation)\n" > out_h
    printf " * by client/lib/lexbor/regenerate.sh. Consumers #include <lexbor.h> only. */\n\n" > out_h
    printf "#ifndef KRUNKNATIVE_LEXBOR_H\n#define KRUNKNATIVE_LEXBOR_H\n\n" > out_h
    printf "#ifndef LEXBOR_STATIC\n#define LEXBOR_STATIC\n#endif\n\n" > out_h
    printf "/*\n%s\n/*\n%s\n#endif /* KRUNKNATIVE_LEXBOR_H */\n", includes, headers > out_h

    printf "%s", banner > out_c
    printf "/* KrunkNative: implementation half of the Lexbor amalgamation, see lexbor.h. */\n\n" > out_c
    printf "/*\n%s\n#include <dirent.h>\n#include \"lexbor.h\"\n\n/*\n%s", defines, sources > out_c
}' "$single"
rm -f "$single"

{ cat "$src/LICENSE"; printf "\n\n---- NOTICE ----\n\n"; cat "$src/NOTICE"; } > "$here/LICENSE"

echo "generated $(wc -c < "$here/lexbor.h") bytes lexbor.h, $(wc -c < "$here/lexbor.c") bytes lexbor.c from $LEXBOR_COMMIT"
