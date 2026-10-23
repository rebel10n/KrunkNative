#!/usr/bin/env bash
# Packs a Linux client package (BoxOffice-linux-<arch>.tar.gz, tools/release/package-client.sh) into an
# AppImage: one file that runs on any x86_64 / aarch64 distro with glibc 2.35+ and no install step.
# The AppDir is the package unchanged under usr/bin (the launcher finds runtime/ beside it), plus
# AppRun, the desktop entry and the icon. Its kn-install.json says package "AppImage": the launcher
# never updates a read-only image itself and points at the release page instead (launcher/src).
#
# Usage: tools/release/package-appimage.sh <BoxOffice-linux-<arch>.tar.gz> <out dir>
#   -> <out dir>/BoxOffice-linux-<arch>.AppImage (+ the appimagetool it was built with, cached)
# Env: APPIMAGETOOL  a local appimagetool to use instead of the pinned download below
# appimagetool runs with APPIMAGE_EXTRACT_AND_RUN=1, so no FUSE is needed (CI containers, WSL).
set -euo pipefail

usage() { sed -n '2,15p' "$0" >&2; exit 2; }
[ $# -eq 2 ] || usage
pkg="$1" out="$2"
[ -f "$pkg" ] || { echo "no package $pkg" >&2; exit 1; }

# package name -> AppImage architecture (the appimagetool asset suffix)
base="$(basename "$pkg")"
case "$base" in
    BoxOffice-linux-x64.tar.gz) arch=x86_64 ;;
    BoxOffice-linux-arm64.tar.gz) arch=aarch64 ;;
    *) echo "not a Linux client package: $base (BoxOffice-linux-<x64|arm64>.tar.gz)" >&2; exit 2 ;;
esac

# appimagetool 1.9.1 (AppImage/appimagetool releases; the sha256 are GitHub's asset digests)
TOOL_VERSION=1.9.1
case "$arch" in
    x86_64) tool_sha=ed4ce84f0d9caff66f50bcca6ff6f35aae54ce8135408b3fa33abfc3cb384eb0 ;;
    aarch64) tool_sha=f0837e7448a0c1e4e650a93bb3e85802546e60654ef287576f46c71c126a9158 ;;
esac

tool="${APPIMAGETOOL:-}"
if [ -z "$tool" ]; then
    cache="${XDG_CACHE_HOME:-$HOME/.cache}/krunknative/appimagetool-$TOOL_VERSION-$arch.AppImage"
    if [ ! -f "$cache" ] || ! echo "$tool_sha  $cache" | sha256sum -c --status; then
        mkdir -p "$(dirname "$cache")"
        curl -fsSL -o "$cache.part" "https://github.com/AppImage/appimagetool/releases/download/$TOOL_VERSION/appimagetool-$arch.AppImage"
        echo "$tool_sha  $cache.part" | sha256sum -c --status || { echo "appimagetool $arch: checksum mismatch" >&2; exit 1; }
        chmod +x "$cache.part"
        mv "$cache.part" "$cache"
    fi
    tool="$cache"
fi

work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
appdir="$work/BoxOffice.AppDir"
mkdir -p "$appdir/usr/bin"

# the package as shipped: BoxOffice and runtime/ at the top of usr/bin
tar -xzf "$pkg" -C "$appdir/usr/bin"
[ -x "$appdir/usr/bin/BoxOffice" ] || { echo "no BoxOffice launcher in $pkg" >&2; exit 1; }
[ -f "$appdir/usr/bin/runtime/client_bin" ] || { echo "no runtime/client_bin in $pkg" >&2; exit 1; }

# the updater's marker: an AppImage is replaced by downloading it (launcher kn_install_self_updatable)
marker="$appdir/usr/bin/runtime/kn-install.json"
grep -q '"package":"tar.gz"' "$marker" || { echo "unexpected $marker" >&2; exit 1; }
sed -i 's/"package":"tar.gz"/"package":"AppImage"/' "$marker"

cat > "$appdir/AppRun" <<'EOF'
#!/bin/sh
# AppImage entry point: the launcher in usr/bin, with its runtime/ beside it (tools/release/package-appimage.sh)
here="$(dirname "$(readlink -f "$0")")"
exec "$here/usr/bin/BoxOffice" "$@"
EOF
chmod 0755 "$appdir/AppRun"

cat > "$appdir/BoxOffice.desktop" <<'EOF'
[Desktop Entry]
Type=Application
Name=Krunker Box Office
Comment=Krunker, with Box Office servers and lobbies
Exec=BoxOffice %U
Icon=BoxOffice
Categories=Game;
Terminal=false
EOF

# the launcher's icon (the same file the package's launcher window shows)
icon="$appdir/usr/bin/runtime/assets/img/icon.png"
[ -f "$icon" ] || { echo "no icon at $icon" >&2; exit 1; }
cp "$icon" "$appdir/BoxOffice.png"
ln -s BoxOffice.png "$appdir/.DirIcon"

mkdir -p "$out"
result="$out/${base%.tar.gz}.AppImage"
rm -f "$result"
ARCH="$arch" APPIMAGE_EXTRACT_AND_RUN=1 "$tool" --no-appstream "$appdir" "$result"
chmod 0755 "$result"
echo "$(sha256sum "$result")"
