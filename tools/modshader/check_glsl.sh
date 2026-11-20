#!/usr/bin/env bash
# Validates the GLSL the mod shader graph compiler (client/src/mod_shader_graph.c) emits for every node type, as
# GLSL ES 3.00 (web / ANGLE) and as GLSL 4.50 core (desktop), with glslangValidator.
#   tools/modshader/check_glsl.sh [cmake-build-debug/mod_shader_test]
set -euo pipefail
bin=${1:-cmake-build-debug/mod_shader_test}
command -v glslangValidator >/dev/null || { echo "check_glsl: glslangValidator not installed, skipped"; exit 0; }
dir=$(mktemp -d "${XDG_CACHE_HOME:-$HOME/.cache}/kn-modshader-glsl.XXXXXX")
trap 'rm -rf "$dir"' EXIT
mkdir -p "$dir/es" "$dir/450"
"$bin" --dump "$dir/es" >/dev/null
fails=0 total=0
for f in "$dir"/es/*.frag; do
    sed '1s/.*/#version 450 core/' "$f" >"$dir/450/$(basename "$f")"
    for v in "$f" "$dir/450/$(basename "$f")"; do
        total=$((total + 1))
        if ! out=$(glslangValidator -S frag "$v" 2>&1); then
            fails=$((fails + 1))
            echo "== $(basename "$(dirname "$v")")/$(basename "$v")"
            grep ERROR <<<"$out" | head -5
        fi
    done
done
echo "check_glsl: $((total - fails))/$total fragments valid"
[ "$fails" -eq 0 ]
