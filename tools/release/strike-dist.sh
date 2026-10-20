#!/usr/bin/env bash
# Assembles the Krunker Strike distribution from a Strike-flavored build tree (make strike-dist:
# a Release tree configured -DKRUNKNATIVE_FLAVOR=strike, so the binaries default to the Strike
# flavor -- flavor_display_name() == "Krunker Strike", server_bin/client_bin --version and the
# server-info `flavor` field say strike -- while every identifier stays krunknative).
#
# It reuses the base release packagers unchanged (tools/release/package-server.sh and
# package-client.sh): only the binaries' compile-time default flavor differs, so the archives
# keep the BoxOffice names and the "Krunker Box Office" README / license text. Nothing here
# touches prod or the live update feed.
#
# Usage: tools/release/strike-dist.sh <build dir> <out dir>
#   <build dir>   a Strike-flavored build (make strike-dist / the strike-release preset) with
#                 server_bin and client_bin
#   <out dir>     where the packages go (make strike-dist: dist/strike/)
# Out (linux-<arch>):
#   <out>/BoxOffice-server-linux-<arch>.tar.gz               the dedicated server, default Strike
#   <out>/BoxOffice-linux-<arch>.tar.gz                      the client, default Strike
set -euo pipefail

[ $# -eq 2 ] || { sed -n '2,19p' "$0" >&2; exit 2; }
build="$1" out="$2"
here="$(cd "$(dirname "$0")" && pwd)"
repo="$(cd "$here/../.." && pwd)"

for f in server_bin client_bin; do
    [ -f "$build/$f" ] || { echo "strike-dist: no $f in $build (run: make strike-dist)" >&2; exit 1; }
done

# package-server.sh accepts any linux-<arch>; package-client.sh wants linux-x64 / linux-arm64.
case "$(uname -s)" in Linux) os=linux ;; Darwin) os=macos ;; *) echo "strike-dist: unsupported OS $(uname -s)" >&2; exit 1 ;; esac
case "$(uname -m)" in
    x86_64|amd64)  arch=x64 ;;
    aarch64|arm64) arch=arm64 ;;
    *) echo "strike-dist: unsupported arch $(uname -m)" >&2; exit 1 ;;
esac

# Branding metadata (version, commit, protocol) for the package READMEs, as CI does; the flavor
# comes from the compiled-in default, not from here. Best-effort: a tagless tree leaves them unset.
if command -v git > /dev/null && git -C "$repo" rev-parse --git-dir > /dev/null 2>&1; then
    while IFS='=' read -r k v; do
        case "$k" in
            describe)   export KN_DESCRIBE="$v" ;;
            commit)     export KN_COMMIT="$v" ;;
            commit_time) export KN_COMMIT_TIME="$v" ;;
            build_time) export KN_BUILD_TIME="$v" ;;
            protocol)   export KN_PROTOCOL="$v" ;;
        esac
    done < <("$here/version-meta.sh" 2> /dev/null || true)
fi

mkdir -p "$out"
out="$(cd "$out" && pwd)"

echo "== Krunker Strike dedicated-server package ($os-$arch) ==" >&2
"$here/package-server.sh" "$os-$arch" "$build/server_bin" "$out"

echo "== Krunker Strike client package ($os-$arch) ==" >&2
"$here/package-client.sh" "$os-$arch" "$build" "$out"

echo "Krunker Strike dist ready in $out:" >&2
(cd "$out" && ls -1 ./*.tar.gz) | sed 's|.*/|  |' >&2
