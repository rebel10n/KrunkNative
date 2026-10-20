#!/usr/bin/env bash
# The game content bundle for the native client packages (tools/release/package-client.sh):
#   <out>/BoxOffice-assets-<version>.tar.gz (+ .sha256)     members assets/..., unpack into an install root
#   <out>/BoxOffice-assets-<version>.manifest              sha256sum list of every file (also inside, as
#                                                          assets/kn-assets.sha256)
# <version> = the first 16 hex digits of the manifest's sha256: it changes exactly when the content does.
# The archive is reproducible: the same files give the same bytes (GNU tar: sorted, owner 0, fixed
# mtime 2026-01-01, symlinks followed; gzip -n), so a bundle can be rebuilt and checked anywhere.
#
# What goes in (the filter kn_web uses for the web host, deploy/ansible/roles/kn_web): every file of
# assets/ except README.md, css/{textures,models,sound}/ (copies of the top-level dirs), *.blend,
# *.psd and version.txt.
#   --part content  (default) only what git does not track: the exe packages already carry the
#                   code-coupled, git-tracked part (shaders/ maps/ img/ editor/ skins.json previews.json)
#   --part all      everything (an offline install from one archive)
#
# Usage: tools/release/package-assets.sh [--part content|all] <assets dir> <out dir>
#        tools/release/package-assets.sh --list [--part content|all] <assets dir>     file list (stdout)
#        tools/release/package-assets.sh --version [--part content|all] <assets dir>  <version> (stdout)
# Needs GNU tar and gzip (Linux; macOS: gtar).
set -euo pipefail

usage() { sed -n '2,23p' "$0" >&2; exit 2; }
mode=pack part=content
while [ $# -gt 0 ]; do
    case "$1" in
        --list) mode=list; shift ;;
        --version) mode=version; shift ;;
        --part) part="$2"; shift 2 ;;
        -h|--help) usage ;;
        -*) echo "unknown option $1" >&2; usage ;;
        *) break ;;
    esac
done
case "$part" in content|all) ;; *) echo "unknown part $part" >&2; usage ;; esac
if [ "$mode" = pack ]; then [ $# -eq 2 ] || usage; else [ $# -eq 1 ] || usage; fi
src="$1"
[ -d "$src" ] || { echo "no assets directory $src" >&2; exit 1; }

# sorted relative paths of the files that go in (-L: assets/<dir> may be symlinks)
list() {
    (cd "$src" && find -L . -type f -print0 | LC_ALL=C sort -z | tr '\0' '\n' | sed 's|^\./||' | awk -v part="$part" '
        $0 == "README.md" || $0 == "version.txt" || $0 ~ /^kn-assets\./ { next }
        $0 ~ /^css\/(textures|models|sound)\// { next }
        $0 ~ /\.(blend|psd)$/ { next }
        part == "content" && ($0 ~ /^(shaders|maps|img|editor)\// || $0 == "skins.json" || $0 == "previews.json") { next }
        { print }')
}

sha256_cmd=(sha256sum)
command -v sha256sum > /dev/null || sha256_cmd=(shasum -a 256)
manifest() { list | (cd "$src" && tr '\n' '\0' | xargs -0 "${sha256_cmd[@]}"); }
version_of() { "${sha256_cmd[@]}" < "$1" | cut -c1-16; }

case "$mode" in
    list) list; exit 0 ;;
    version)
        tmp="$(mktemp)"; trap 'rm -f "$tmp"' EXIT
        manifest > "$tmp"
        version_of "$tmp"
        exit 0 ;;
esac

out="$2"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
tar_bin=tar
command -v gtar > /dev/null && tar_bin=gtar
"$tar_bin" --version 2> /dev/null | grep -q 'GNU tar' || { echo "package-assets.sh needs GNU tar" >&2; exit 1; }

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
manifest > "$work/manifest"
[ -s "$work/manifest" ] || { echo "no files in $src" >&2; exit 1; }
version="$(version_of "$work/manifest")"
name="BoxOffice-assets-$version"
files="$(wc -l < "$work/manifest")"
stat_size=(stat -L -c %s)
stat -L -c %s / > /dev/null 2>&1 || stat_size=(stat -L -f %z) # BSD stat
bytes="$(list | (cd "$src" && tr '\n' '\0' | xargs -0 "${stat_size[@]}") | awk '{ s += $1 } END { printf "%.0f\n", s }')"

# kn-assets.json / kn-assets.sha256 sit inside assets/ so an install knows its content version
mkdir -p "$work/meta"
cp "$work/manifest" "$work/meta/kn-assets.sha256"
printf '{"format":1,"version":"%s","part":"%s","files":%s,"bytes":%s}\n' "$version" "$part" "$files" "$bytes" > "$work/meta/kn-assets.json"

# shellcheck disable=SC2054 # --mode takes a comma list
tar_flags=(--sort=name --owner=0 --group=0 --numeric-owner --mtime=@1767225600 --mode=u+rw,go+r,go-w,a-s
    --format=gnu --dereference --no-recursion)
# files only (tar creates the directories), in list() order, all under assets/
list | "$tar_bin" -C "$src" "${tar_flags[@]}" --transform 's,^,assets/,' -T - -cf "$work/content.tar"
"$tar_bin" -C "$work/meta" "${tar_flags[@]}" --transform 's,^,assets/,' -cf "$work/meta.tar" kn-assets.json kn-assets.sha256
"$tar_bin" --concatenate -f "$work/content.tar" "$work/meta.tar"
gzip -n -6 < "$work/content.tar" > "$out/$name.tar.gz"
cp "$work/manifest" "$out/$name.manifest"
(cd "$out" && "${sha256_cmd[@]}" "$name.tar.gz" > "$name.tar.gz.sha256")
echo "packaged $out/$name.tar.gz: $files files, $bytes bytes ($(du -h "$out/$name.tar.gz" | cut -f1) compressed), part $part" >&2
cat "$out/$name.tar.gz.sha256" >&2
echo "version=$version"
echo "sha256=$(cut -d' ' -f1 "$out/$name.tar.gz.sha256")"
