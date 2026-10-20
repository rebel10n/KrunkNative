#!/usr/bin/env bash
# Runs a Linux client package in a distro container (.github/workflows/client.yml compat matrix,
# and locally): installs it the way a user would (BoxOffice-linux-<arch>.tar.gz unpacked into an empty
# folder), runs runtime/client_bin and runtime/server_bin --version and BoxOffice --version and, with a
# content dir, a headless offline start on Mesa's llvmpipe (EGL, no display), the `make smoke-native`
# check: the C UI logic applied its settings and a screenshot was written.
#
# Usage: tools/release/test-client-linux.sh <image> <package> [<content dir>] [--out <dir>]
#   <image>        debian:buster|bullseye|bookworm|trixie, ubuntu:*, centos:7, almalinux:8|9,
#                  rockylinux:*, fedora:*, archlinux:latest
#   <package>      BoxOffice-linux-<arch>.tar.gz
#   <content dir>  the game content to add (an unpacked BoxOffice-assets bundle: <dir>/assets/...,
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
        # the desktop libraries of runtime/host-libs.txt (a desktop install has them; a container does not)
        pkgs="ca-certificates libegl1 libgl1 libglx0 libopengl0 libgbm1 libdrm2 libx11-6 libx11-xcb1 libxcomposite1 libxcursor1 libxdamage1 libxext6 libxfixes3 libxi6 libxinerama1 libxrandr2 libxrender1 libxcb1 libxcb-render0 libxcb-shm0 libxkbcommon0 libwayland-client0 libwayland-cursor0 libwayland-egl1 libwayland-server0 libpulse0 libdbus-1-3 libudev1 libsystemd0 zlib1g libstdc++6 libgcc-s1"
        if apt-cache show libssl3t64 > /dev/null 2>&1; then pkgs="$pkgs libssl3t64"; else pkgs="$pkgs libssl3"; fi
        [[ "$PKG" == *.zip ]] && pkgs="$pkgs unzip"
        [ -n "$gl" ] && pkgs="$pkgs libegl-mesa0 libgl1-mesa-dri"
        apt-get install -y -qq --no-install-recommends $pkgs || apt-get install -y -qq --no-install-recommends ${pkgs/libegl1/libegl1-mesa} ;;
    centos|rhel|almalinux|rocky|fedora)
        pm=dnf; command -v dnf > /dev/null || pm=yum
        if [ "$ID" = centos ] && [ "${VERSION_ID%%.*}" = 7 ]; then
            sed -i -e "s|^mirrorlist=|#mirrorlist=|" -e "s|^#baseurl=http://mirror.centos.org|baseurl=http://vault.centos.org|" /etc/yum.repos.d/CentOS-*.repo
        fi
        pkgs="ca-certificates mesa-libEGL libglvnd-opengl libglvnd-glx mesa-libgbm libdrm libX11 libX11-xcb libXcomposite libXcursor libXdamage libXext libXfixes libXi libXinerama libXrandr libXrender libxcb libxkbcommon libwayland-client libwayland-cursor libwayland-egl libwayland-server pulseaudio-libs dbus-libs systemd-libs openssl-libs zlib libstdc++ libgcc"
        [[ "$PKG" == *.zip ]] && pkgs="$pkgs unzip"
        [ -n "$gl" ] && pkgs="$pkgs mesa-dri-drivers"
        $pm install -y -q $pkgs ;;
    arch)
        pacman -Syu --noconfirm --needed libglvnd openssl ${gl:+mesa} unzip ;; # -Syu: no partial upgrades
    *) echo "unsupported distro $ID"; RC=1; result install=fail "(distro $ID)" ;;
esac

# the package is flat: BoxOffice and runtime/ go into an empty folder of their own
root=/opt/t
mkdir -p "$root"
case "$PKG" in
    *.zip) unzip -q "/pkg/$PKG" -d "$root" || { RC=1; result install=fail "(unpack)"; } ;;
    *.tar.gz) tar -xzf "/pkg/$PKG" -C "$root" || { RC=1; result install=fail "(unpack)"; } ;;
    *) RC=1; result install=fail "(not a BoxOffice package)" ;;
esac
[ -x "$root/runtime/client_bin" ] || { RC=1; result install=fail "(no $root/runtime/client_bin)"; }

v="$("$root/runtime/client_bin" --version)" && "$root/runtime/server_bin" --version || { RC=1; result install=ok version=fail; }
"$root/runtime/client_bin" --voice-info | grep -q '^opus=libopus ' || { RC=1; result install=ok version=fail "(no Opus: voice chat)"; }
# the launcher (the entry point): --version needs no display or network
lv="$("$root/BoxOffice" --version)" || { RC=1; result install=ok version=fail "(launcher)"; }
v="$v; $lv"
echo "$v"
if [ -z "$HAVE_CONTENT" ]; then result install=ok version=ok render=skipped "($v)"; fi

# the content next to the package's own (git-tracked) assets/
for d in /content/*; do [ -e "$root/runtime/assets/$(basename "$d")" ] || ln -s "$d" "$root/runtime/assets/"; done
mkdir -p /tmp/cfg
cd / # client_bin must find its install root by itself (install_root.c); the launcher would
     # sync the game content from the updates server first, so it is not started here
[ -n "$BRAIN" ] && export KRUNKNATIVE_BRAIN_URL="$BRAIN"
env -u DISPLAY -u WAYLAND_DISPLAY XDG_CONFIG_HOME=/tmp/cfg KRUNKNATIVE_HEADLESS=1 \
    KRUNKNATIVE_SCREENSHOT=/out/menu.png KRUNKNATIVE_CAPTURE_FRAMES=60 timeout 300 "$root/runtime/client_bin" --offline > /out/client.log 2>&1
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

rc=0
"${DOCKER:-docker}" run --rm "${mounts[@]}" -e PKG="$pkgfile" -e BRAIN="$brain" -e HAVE_CONTENT="${content:+1}" -e HOST_UID="$(id -u)" \
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
