#!/usr/bin/env bash
# Build version metadata for CI (release.yml, build.yml): the same values the CMake-generated
# kn_version.h computes from git locally, passed to builds that have no git (deploy/ansible/build.sh
# runs cmake in a Debian container) as -DKRUNKNATIVE_VERSION_* cache variables.
#
# Usage: tools/release/version-meta.sh [commit]   (default HEAD; needs the tags: fetch-depth 0)
# Prints GITHUB_OUTPUT lines:
#   describe     git describe (server-v* / vN tags, never v3.1.1-parity), e.g. v1.0.0-51-g6f0cea9
#   commit       full sha;  commit12  first 12 hex digits
#   commit_time  committer date of the commit, unix seconds
#   build_time   now (or SOURCE_DATE_EPOCH), ISO-8601 UTC
#   protocol     NET_PROTOCOL_MAJOR.NET_PROTOCOL_MINOR of shared/include/shared.h at that commit
#   cmake_flags  the -D flags for the four values above
#   kn_build     the one-line JSON kn_brain reads from release notes (<!-- kn-build ... -->)
set -euo pipefail

rev="${1:-HEAD}"
repo="$(cd "$(dirname "$0")/../.." && pwd)"
git() { command git -C "$repo" "$@"; }

commit="$(git rev-parse "$rev^{commit}")"
describe="$(git describe --tags --match 'server-v*' --match 'v[0-9]*' --exclude '*-parity*' --always "$commit")"
commit_time="$(git log -1 --format=%ct "$commit")"
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

echo "describe=$describe"
echo "commit=$commit"
echo "commit12=${commit:0:12}"
echo "commit_time=$commit_time"
echo "build_time=$build_time"
echo "protocol=$major.$minor"
echo "cmake_flags=-DKRUNKNATIVE_VERSION_DESCRIBE=$describe -DKRUNKNATIVE_VERSION_COMMIT=${commit:0:12} -DKRUNKNATIVE_VERSION_COMMIT_TIME=$commit_time -DKRUNKNATIVE_VERSION_BUILD_TIME=$build_time"
printf 'kn_build={"describe":"%s","commit":"%s","commit_time":%s,"build_time":"%s","protocol":"%s"}\n' \
    "$describe" "$commit" "$commit_time" "$build_time" "$major.$minor"
