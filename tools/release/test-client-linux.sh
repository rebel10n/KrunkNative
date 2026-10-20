#!/usr/bin/env bash
# Runs a Linux client package in a distro container (.github/workflows/client.yml compat matrix,
# and locally): installs it the way a user would (.tar.gz/.zip unpacked; .deb/.rpm through
# apt/dnf/yum, which also checks the package's dependencies), runs client_bin/server_bin --version
# and, with a content dir, a headless offline start on Mesa's llvmpipe (EGL, no display), the
# `make smoke-native` check: the C UI logic applied its settings and a screenshot was written.
#
# Usage: tools/release/test-client-linux.sh <image> <package> [<content dir>] [--out <dir>]
#   <image>        debian:buster|bullseye|bookworm|trixie, ubuntu:*, centos:7, almalinux:8|9,
#                  rockylinux:*, fedora:*, archlinux:latest
#   <package>      krunknative-client-linux-*.{tar.gz,zip,deb,rpm}
#   <content dir>  the game content to add (an unpacked krunknative-assets bundle: <dir>/assets/...,
#                  or an assets/ tree); without it only --version runs (CI has no game content)
#   --out <dir>    where the screenshot and log go (default: a temp dir, printed)
#   --brain <url>  KRUNKNATIVE_BRAIN_URL for the headless run (https:// checks TLS + the CA bundle;
#                  the log shows "[backend] TLS to ... failed" when the handshake fails)
# Prints one line: RESULT <image> <package> install=ok version=ok render=ok|fail|skipped (<detail>)
# Needs docker (DOCKER=podman works). Exit 1 when install or --version fails, or a render fails.
set -euo pipefail

usage() { sed -n '2,17p' "$0" >&2; exit 2; }
out="" brain=""
args=()
while [ $# -gt 0 ]; do
    case "$1" in --out) out="$2"; shift 2 ;; --brain) brain="$2"; shift 2 ;; -h|--help) usage ;; *) args+=("$1"); shift ;; esac
done
[ ${#args[@]} -ge 2 ] && [ ${#args[@]} -le 3 ] || usage
image="${args[0]}" package="${args[1]}" content="${args[2]:-}"
[ -f "$package" ] || { echo "no package $package" >&2; exit 1; }
pkgdir="$(cd "$(dirname "$package")" && pwd)" pkgfile="$(basename "$package")"
[ -n "$out" ] || out="$(mktemp -d "${TMPDIR:-/tmp}/kn-compat.XXXXXX")"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
mounts=(-v "$pkgdir:/pkg:ro" -v "$out:/out")
if [ -n "$content" ]; then
    content="$(cd "$content" && pwd)"
    [ -d "$content/assets" ] && content="$content/assets"
    mounts+=(-v "$content:/content:ro")
    # assets/<dir> symlinks into another checkout (agent worktrees): mount their targets too
    for l in "$content"/*; do
        [ -L "$l" ] || continue
        t="$(readlink -f "$l")"
        mounts+=(-v "$t:$t:ro")
    done
fi

# The script run in the container. A quoted heredoc, so quotes in it are just quotes: in the
# single-quoted bash -c '...' it used to be, the '^opus=libopus ' of f75b57b ended the script at
# that grep (no RESULT line, so every CI compat run failed without saying why).
read -r -d '' inner << 'EOF' || true
set -uo pipefail
log=/out/test.log
exec 3>&1 > "$log" 2>&1
trap "chown -R $HOST_UID /out 2> /dev/null" EXIT
. /etc/os-release
echo "== $PRETTY_NAME, $(ldd --version 2>&1 | sed -n 1p)"
result() { echo "RESULT ${PRETTY_NAME// /_} $PKG $*" | tee /dev/fd/3; exit "${RC:-0}"; }
gl=""; [ -n "$HAVE_CONTENT" ] && gl=1
case "$ID" in
    debian|ubuntu)
        export DEBIAN_FRONTEND=noninteractive
        case "${VERSION_CODENAME:-}" in
            stretch|buster|bullseye)
                printf "%s\n" "deb http://archive.debian.org/debian $VERSION_CODENAME main" > /etc/apt/sources.list
                rm -f /etc/apt/sources.list.d/*
                echo "Acquire::Check-Valid-Until \"false\";" > /etc/apt/apt.conf.d/99archive ;;
        esac
        apt-get update -qq
        pkgs="ca-certificates"
        [[ "$PKG" == *.zip ]] && pkgs="$pkgs unzip"
        [[ "$PKG" == *.deb ]] || pkgs="$pkgs libegl1"
        [ -n "$gl" ] && pkgs="$pkgs libegl-mesa0 libgl1-mesa-dri"
        apt-get install -y -qq --no-install-recommends $pkgs || apt-get install -y -qq --no-install-recommends ${pkgs/libegl1/libegl1-mesa} ;;
    centos|rhel|almalinux|rocky|fedora)
        pm=dnf; command -v dnf > /dev/null || pm=yum
        if [ "$ID" = centos ] && [ "${VERSION_ID%%.*}" = 7 ]; then
            sed -i -e "s|^mirrorlist=|#mirrorlist=|" -e "s|^#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|" /etc/yum.repos.d/CentOS-*.repo
        fi
        pkgs="ca-certificates"
        [[ "$PKG" == *.zip ]] && pkgs="$pkgs unzip"
        [[ "$PKG" == *.rpm ]] || pkgs="$pkgs mesa-libEGL"
        [ -n "$gl" ] && pkgs="$pkgs mesa-libEGL mesa-dri-drivers"
        $pm install -y -q $pkgs ;;
    arch)
        pacman -Syu --noconfirm --needed libglvnd ${gl:+mesa} unzip ;; # -Syu: no partial upgrades
    *) echo "unsupported distro $ID"; RC=1; result install=fail "(distro $ID)" ;;
esac

root=/opt/krunknative
case "$PKG" in
    *.deb) apt-get install -y -qq --no-install-recommends "/pkg/$PKG" || { RC=1; result install=fail "(apt)"; } ;;
    *.rpm) $pm install -y -q "/pkg/$PKG" || { RC=1; result install=fail "($pm)"; } ;;
    *.tar.gz|*.zip)
        mkdir -p /opt/t
        case "$PKG" in *.zip) unzip -q "/pkg/$PKG" -d /opt/t ;; *) tar -xzf "/pkg/$PKG" -C /opt/t ;; esac || { RC=1; result install=fail "(unpack)"; }
        root="$(ls -d /opt/t/*)" ;;
esac
[ -x "$root/client_bin" ] || { RC=1; result install=fail "(no $root/client_bin)"; }

v="$("$root/client_bin" --version)" && "$root/server_bin" --version || { RC=1; result install=ok version=fail; }
"$root/client_bin" --voice-info | grep -q '^opus=libopus ' || { RC=1; result install=ok version=fail "(no Opus: voice chat)"; }
# the launcher (entry point when the package has one): --version needs no display or network
if [ -x "$root/krunknative" ]; then
    lv="$("$root/krunknative" --version)" || { RC=1; result install=ok version=fail "(launcher)"; }
    v="$v; $lv"
fi
echo "$v"
if [ -z "$HAVE_CONTENT" ]; then result install=ok version=ok render=skipped "($v)"; fi

# the content next to the package's own (git-tracked) assets/
for d in /content/*; do [ -e "$root/assets/$(basename "$d")" ] || ln -s "$d" "$root/assets/"; done
mkdir -p /tmp/cfg
cd / # client_bin must find its install root by itself (install_root.c); the launcher would
     # sync the game content from the updates server first, so it is not started here
[ -n "$BRAIN" ] && export KRUNKNATIVE_BRAIN_URL="$BRAIN"
env -u DISPLAY -u WAYLAND_DISPLAY XDG_CONFIG_HOME=/tmp/cfg KRUNKNATIVE_HEADLESS=1 \
    KRUNKNATIVE_SCREENSHOT=/out/menu.png KRUNKNATIVE_CAPTURE_FRAMES=60 timeout 300 "$root/client_bin" --offline > /out/client.log 2>&1
rc=$?
renderer="$(grep -m1 "GL_RENDERER" /out/client.log | sed "s/.*GL_RENDERER *//")"
tls=""
[ -n "$BRAIN" ] && tls="; TLS $(grep -q "TLS to .* failed\|no CA bundle" /out/client.log && echo FAILED || echo ok)"
if grep -q "^\[ui\] setting " /out/client.log && [ -s /out/menu.png ]; then
    result install=ok version=ok render=ok "($v; ${renderer:-?}$tls)"
fi
tail -5 /out/client.log
RC=1 result install=ok version=ok render=fail "(exit $rc; ${renderer:-no GL context}: $(grep -m1 -iE "error|failed|unsupported|cannot" /out/client.log | cut -c1-120))"
EOF

# KN_DOCKER_PLATFORM=linux/arm64: test an arm64 package under binfmt emulation (or on an arm64 host)
platform=()
[ -n "${KN_DOCKER_PLATFORM:-}" ] && platform=(--platform "$KN_DOCKER_PLATFORM")
rc=0
"${DOCKER:-docker}" run --rm "${platform[@]}" "${mounts[@]}" -e PKG="$pkgfile" -e BRAIN="$brain" -e HAVE_CONTENT="${content:+1}" -e HOST_UID="$(id -u)" \
    "$image" bash -c "$inner" || rc=$?
if ! grep -q '^RESULT ' "$out/test.log" 2> /dev/null; then
    [ "$rc" != 0 ] || rc=1
    echo "RESULT $image $pkgfile install=? version=fail (the test script ended early: exit $rc, no RESULT)"
fi
if [ "$rc" != 0 ]; then
    # why, in the CI job log too (test.log lives in the runner's temp dir and is not uploaded)
    {
        echo "== FAILED: $image $pkgfile (exit $rc), last lines of $out/test.log:"
        tail -n 40 "$out/test.log" 2> /dev/null | sed "s|^|[$image $pkgfile] |"
    } >&2
    exit "$rc"
fi
echo "log: $out" >&2
