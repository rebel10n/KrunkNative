#!/usr/bin/env bash
# Build version metadata for CI (release.yml, build.yml): the same values the CMake-generated
# kn_version.h computes from git locally, passed to builds that have no git (deploy/ansible/build.sh
# runs cmake in a Debian container) as -DKRUNKNATIVE_VERSION_* cache variables.
#
# Usage: tools/release/version-meta.sh [--server] [commit]   (default HEAD; needs the tags and the
#        history: fetch-depth 0)
#
# --server: the identity of the service binaries (server_bin, kn_brain, kn_migrate) instead of the
#   commit's: the last commit up to <commit> that changed their inputs (SERVER_PATHS below), and that
#   commit's time as the build time. A push that touches none of them (the web client, docs, deploy
#   settings) then builds byte-identical binaries, so the deploys copy nothing and restart nothing
#   (deploy-kru.yml, deploy-edges.yml), and the release metadata names the same commit, so the
#   finder's outdated check (doc/backend.md 8.12) sees the edges as current. The binaries report
#   that commit (server_bin --version, KNINFO).
# Prints GITHUB_OUTPUT lines:
#   krunker_version  Krunker's version the players see: the newest "== UPDATE x ==" of ui/windows/kn-versions.txt
#                at that commit (v10_news_version, client/src/ui/v10_news.c); "unknown" when there is none
#   describe     the commit's 12 hex digits (the kn-build key of that name; the binaries' describe): the name a build
#                goes by, never a git describe or a version number (server-info, lobby meta, release notes)
#   commit       full sha;  commit12  first 12 hex digits
#   commit_time  committer date of the commit, unix seconds
#   build_time   now (or SOURCE_DATE_EPOCH), ISO-8601 UTC
#   protocol     NET_PROTOCOL_MAJOR.NET_PROTOCOL_MINOR of shared/include/shared.h at that commit
#   cmake_flags  the -D flags for the four values above
#   kn_build     the one-line JSON kn_brain reads from release notes (<!-- kn-build ... -->)
set -euo pipefail

server=0
if [ "${1:-}" = --server ]; then
    server=1
    shift
fi
rev="${1:-HEAD}"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
git() { command git -C "$repo" "$@"; }
# Everything the service binaries and their release (deploy/ansible kn_release) are built from.
SERVER_PATHS=(shared server services cmake CMakeLists.txt CMakePresets.json .gitmodules
              assets/maps assets/skins.json deploy/ansible/build.sh tools/release/version-meta.sh)

commit="$(git rev-parse "$rev^{commit}")"
if [ "$server" = 1 ]; then
    source_commit="$(git log -1 --format=%H "$commit" -- "${SERVER_PATHS[@]}")"
    [ -n "$source_commit" ] || { echo "version-meta.sh: no commit touches the server paths (shallow clone? fetch-depth 0)" >&2; exit 1; }
    commit="$source_commit"
fi
# a build is named by its commit (12 hex), never by a git describe: no tags needed, the same value on the wire
describe="${commit:0:12}"
# the player-visible version is Krunker's (the changelog's newest update), never describe
krunker_version=unknown
kn_versions="$(git show "$commit:ui/windows/kn-versions.txt" 2> /dev/null || true)"
while IFS= read -r line; do
    if [[ "$line" =~ ^[[:space:]]*==\ UPDATE\ ([0-9][0-9.]*)\ == ]]; then krunker_version="${BASH_REMATCH[1]}"; break; fi
done <<< "$kn_versions"
commit_time="$(git log -1 --format=%ct "$commit")"
if [ "$server" = 1 ] && [ -z "${SOURCE_DATE_EPOCH:-}" ]; then
    SOURCE_DATE_EPOCH="$commit_time" # reproducible: the same inputs, the same binary
fi
if [ -n "${SOURCE_DATE_EPOCH:-}" ]; then
    build_time="$(date -u -d "@$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ 2> /dev/null || date -u -r "$SOURCE_DATE_EPOCH" +%Y-%m-%dT%H:%M:%SZ)"
else
    build_time="$(date -u +%Y-%m-%dT%H:%M:%SZ)"
fi
shared_h="$(git show "$commit:shared/include/shared.h")"
major="$(awk '$1 == "#define" && $2 == "NET_PROTOCOL_MAJOR" { print $3 }' <<< "$shared_h")"
minor="$(awk '$1 == "#define" && $2 == "NET_PROTOCOL_MINOR" { print $3 }' <<< "$shared_h")"
[[ "$major" =~ ^[0-9]+$ && "$minor" =~ ^[0-9]+$ ]] || { echo "version-meta.sh: no NET_PROTOCOL_MAJOR/MINOR in shared.h" >&2; exit 1; }
# describe is [A-Za-z0-9.-] (tags we match + hex), safe unquoted in -D and JSON
[[ "$describe" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "version-meta.sh: unexpected describe '$describe'" >&2; exit 1; }

echo "krunker_version=$krunker_version"
echo "describe=$describe"
echo "commit=$commit"
echo "commit12=${commit:0:12}"
echo "commit_time=$commit_time"
echo "build_time=$build_time"
echo "protocol=$major.$minor"
echo "cmake_flags=-DKRUNKNATIVE_VERSION_DESCRIBE=$describe -DKRUNKNATIVE_VERSION_COMMIT=${commit:0:12} -DKRUNKNATIVE_VERSION_COMMIT_TIME=$commit_time -DKRUNKNATIVE_VERSION_BUILD_TIME=$build_time"
printf 'kn_build={"describe":"%s","commit":"%s","commit_time":%s,"build_time":"%s","protocol":"%s"}\n' \
    "$describe" "$commit" "$commit_time" "$build_time" "$major.$minor"
