#!/usr/bin/env bash
# Builds client_bin + server_bin for the Linux client packages (.github/workflows/client.yml,
# then tools/release/package-client.sh). Run it on Ubuntu 22.04 (glibc 2.35), natively (CI runners
# ubuntu-22.04 / ubuntu-22.04-arm) or in an ubuntu:22.04 container (--docker), amd64 or aarch64:
#
#   dynamic     the distro build: links the system OpenSSL 3 (libssl3: security updates come from
#               the distro) and needs glibc >= 2.35. Bundles WebKitGTK for the captcha webview
#               (tools/release/bundle-webkit.sh: lib/, libexec/). Packages: linux-<arch>.
#   polyfilled  for old and new distros alike (fastfetch's "-polyfilled"): OpenSSL static too
#               (libssl.a), then polyfill-glibc --target-glibc=2.17 rewrites the newer glibc symbol
#               versions to 2.17 ones or links in polyfills. Packages: linux-<arch>-polyfilled.
#
# Both variants: FreeType without zlib/bzip2/png/harfbuzz/brotli (the vendored tree, as the web
# build), GLFW/lexbor/glad/cJSON compiled in, libgcc/libstdc++/libcrypt static, stripped, server_bin
# without libcurl (http:// finder/auth only: public servers use the server packages). What stays
# dynamic is the OS's: libc, libm, libdl, libpthread, libEGL.so.1 (libglvnd/Mesa; headless mode) and the libraries loaded at run time
# with dlopen (libGL/libEGL/libGLESv2, X11/Wayland/xkbcommon by GLFW, ALSA/PulseAudio/JACK by
# miniaudio; libopus is compiled in: client/lib/opus). The NEEDED list is checked at the end.
#
# Usage: tools/release/build-client-linux.sh [--deps] [--docker <image>] <dynamic|polyfilled> <out dir>
#   --deps            apt-get install the build dependencies first (sudo when not root)
#   --docker <image>  run this script inside <image> (e.g. ubuntu:22.04), repo mounted read-only
# Out: <out>/client_bin, <out>/server_bin (+ <out>/krunknative, the launcher, when the tree has
#      target kn_launcher), <out>/build-info.txt (NEEDED + max GLIBC version)
# Env: KN_CMAKE_ARGS   extra -D flags (CI: version-meta.sh cmake_flags; the container has no git)
#      KN_BUILD_DIR    build tree (default <out>/../build-<variant>)
#      POLYFILL_GLIBC  polyfill-glibc binary to use (default: built from the pinned source, see below)
set -euo pipefail

usage() { sed -n '2,27p' "$0" >&2; exit 2; }
deps=0 image=""
while [ $# -gt 0 ]; do
    case "$1" in
        --deps) deps=1; shift ;;
        --docker) image="$2"; shift 2 ;;
        -h|--help) usage ;;
        -*) echo "unknown option $1" >&2; usage ;;
        *) break ;;
    esac
done
[ $# -eq 2 ] || usage
variant="$1" out="$2"
case "$variant" in dynamic|polyfilled) ;; *) echo "unknown variant $variant" >&2; usage ;; esac

repo="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$out"
out="$(cd "$out" && pwd)"

# polyfill-glibc (https://github.com/corsix/polyfill-glibc), built from source: upstream at
# POLYFILL_COMMIT (it has the x86_64 fix for constructors called with a misaligned stack: the
# CarterLi/polyfill-glibc v0.0.1 binaries fastfetch uses crash server_bin's C++ static
# initialisers on Arch) plus POLYFILL_SINGLE_THREADED, CarterLi's __libc_single_threaded
# polyfill (not upstream), which the static libstdc++ needs.
POLYFILL_COMMIT=e8107d6b05aab7dfe1f52ff3d362c15909cb03ff
POLYFILL_SINGLE_THREADED=f2efab1470a5e2d81dad9d47aed73a58eede0ced

if [ -n "$image" ]; then
    build="${KN_BUILD_DIR:-$(dirname "$out")/build-$variant}"
    mkdir -p "$build"
    build="$(cd "$build" && pwd)"
    extra=()
    if [ "$variant" = polyfilled ] && [ -n "${POLYFILL_GLIBC:-}" ]; then
        extra=(-v "$(cd "$(dirname "$POLYFILL_GLIBC")" && pwd)/$(basename "$POLYFILL_GLIBC"):/polyfill-glibc:ro" -e POLYFILL_GLIBC=/polyfill-glibc)
    fi
    exec "${DOCKER:-docker}" run --rm -v "$repo:/src:ro" -v "$build:/build" -v "$out:/out" \
        -e KN_CMAKE_ARGS="${KN_CMAKE_ARGS:-}" -e KN_BUILD_DIR=/build -e HOST_UID="$(id -u)" -e HOST_GID="$(id -g)" \
        "${extra[@]}" "$image" bash -c '
            set -e
            trap "chown -R \"$HOST_UID:$HOST_GID\" /out /build" EXIT
            bash /src/tools/release/build-client-linux.sh --deps "$0" /out' "$variant"
fi

case "$(uname -m)" in x86_64) arch=amd64 ;; aarch64|arm64) arch=aarch64 ;; *) echo "unsupported arch $(uname -m)" >&2; exit 1 ;; esac

if [ "$deps" = 1 ]; then
    sudo=""
    [ "$(id -u)" = 0 ] || sudo=sudo
    export DEBIAN_FRONTEND=noninteractive
    # build.yml's Linux list (GLFW X11 + Wayland headers, EGL), plus libssl-dev (libssl.a for the
    # polyfilled build), libcrypt-dev (libcrypt.a), git (polyfill-glibc source) and file
    pkgs=(build-essential cmake ninja-build pkg-config ca-certificates file
        libwayland-dev wayland-protocols libx11-dev libxcursor-dev libxinerama-dev libxrandr-dev
        libxi-dev libgl1-mesa-dev libxkbcommon-dev libxcb-present-dev libegl-dev libssl-dev libcrypt-dev git)
    # the dynamic build bundles WebKitGTK (tools/release/bundle-webkit.sh): its -dev package, the TLS
    # module of glib-networking (https in WebKit's network process) and patchelf (RUNPATH of the bundle)
    if [ "$variant" = dynamic ]; then pkgs+=(libwebkit2gtk-4.1-dev glib-networking patchelf); fi
    $sudo apt-get update -qq > /dev/null
    $sudo apt-get install -y -qq --no-install-recommends "${pkgs[@]}" > /dev/null
fi

build="${KN_BUILD_DIR:-$(dirname "$out")/build-$variant}"
mkdir -p "$build"

flags=(-DCMAKE_BUILD_TYPE=Release -DKRUNKNATIVE_HTML_ENGINE=ON
    -DFT_DISABLE_ZLIB=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_PNG=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -DFT_DISABLE_BROTLI=TRUE)
# server_bin here is for LAN/sandbox hosting: no libcurl (Debian's libcurl.so.4 carries symbol
# versions Fedora's lacks; public servers use the server packages), libgcc/libstdc++ (replxx) and
# crypt(3) static (libxcrypt's libcrypt.a: libcrypt.so.1 is a compat package on Fedora/Arch)
flags+=(-DCMAKE_DISABLE_FIND_PACKAGE_CURL=TRUE "-DCMAKE_EXE_LINKER_FLAGS=-static-libgcc -static-libstdc++")
# the captcha webview (client/include/kn_webview.h, WebKitGTK) is bundled with the dynamic packages
# (bundle-webkit.sh, below). The polyfilled ones keep it out: a webkit2gtk-4.1 for their glibc 2.17
# floor does not exist, and a load-time link would make it a hard dependency of the portable client
if [ "$variant" = dynamic ]; then flags+=(-DKN_WEBVIEW=ON); else flags+=(-DKN_WEBVIEW=OFF); fi
libcrypt_a="$(gcc -print-file-name=libcrypt.a)"
if [ -f "$libcrypt_a" ]; then flags+=("-DKN_LIBCRYPT=$libcrypt_a"); else flags+=(-DKN_LIBCRYPT=KN_LIBCRYPT-NOTFOUND); fi
if [ "$variant" = polyfilled ]; then
    # KRUNKNATIVE_TLS_STATIC: backend_native.c skips the foreign distro's openssl.cnf and finds
    # the CA bundle itself (a static libssl only knows the build host's OPENSSLDIR)
    flags+=(-DOPENSSL_USE_STATIC_LIBS=TRUE -DKRUNKNATIVE_TLS_STATIC=ON)
fi
# shellcheck disable=SC2086 # KN_CMAKE_ARGS is a list of -D flags
cmake -S "$repo" -B "$build" -G Ninja "${flags[@]}" ${KN_CMAKE_ARGS:-} > "$build/configure.log" || { cat "$build/configure.log" >&2; exit 1; }
grep -E 'GL backend|OpenSSL|libssl|Headless|Webview' "$build/configure.log" || true
# the launcher (target kn_launcher, output krunknative; agent client-updates) when the tree has it
targets=(client_bin server_bin)
bins=(client_bin server_bin)
if cmake --build "$build" --target help 2> /dev/null | grep '^kn_launcher:' > /dev/null; then targets+=(kn_launcher); fi
cmake --build "$build" --target "${targets[@]}"

install -m 0755 "$build/client_bin" "$build/server_bin" "$out/"
if [[ " ${targets[*]} " == *" kn_launcher "* ]]; then
    launcher="$(find "$build" -maxdepth 3 -type f -name krunknative -perm -u+x | head -1)"
    [ -n "$launcher" ] || { echo "kn_launcher built no krunknative binary" >&2; exit 1; }
    install -m 0755 "$launcher" "$out/krunknative"
    bins+=(krunknative)
fi
for f in "${bins[@]}"; do strip --strip-unneeded "$out/$f"; done
if [ "$variant" = dynamic ]; then
    # WebKitGTK for the captcha webview, beside the binaries: lib/, libexec/, webkit-bundle.txt. It
    # links client_bin to the bundle (RUNPATH) and checks that it does
    "$repo/tools/release/bundle-webkit.sh" "$out"
fi

if [ "$variant" = polyfilled ]; then
    tool="${POLYFILL_GLIBC:-}"
    if [ -z "$tool" ]; then
        src="$build/polyfill-glibc-${POLYFILL_COMMIT:0:12}-${POLYFILL_SINGLE_THREADED:0:12}"
        tool="$src/polyfill-glibc"
        if [ ! -x "$tool" ]; then
            rm -rf "$src"
            git init -q "$src"
            git -C "$src" fetch -q --depth 1 https://github.com/corsix/polyfill-glibc.git "$POLYFILL_COMMIT"
            git -C "$src" checkout -q FETCH_HEAD
            git -C "$src" fetch -q --depth 2 https://github.com/CarterLi/polyfill-glibc.git "$POLYFILL_SINGLE_THREADED"
            git -C "$src" cherry-pick -n "$POLYFILL_SINGLE_THREADED"
            ninja -C "$src" polyfill-glibc > /dev/null
        fi
    fi
    for f in "${bins[@]}"; do "$tool" --target-glibc=2.17 "$out/$f"; done
fi

# Only OS libraries may be NEEDED (polyfilled: glibc's and libEGL only; dynamic: also the bundle's
# lib/, which bundle-webkit.sh filled from the same host); report the newest glibc symbol version
# each binary asks for.
allowed='^(libc|libm|libdl|libpthread|librt|libEGL|ld-linux-x86-64|ld-linux-aarch64)\.so'
[ "$variant" = dynamic ] && allowed='^(libc|libm|libdl|libpthread|librt|libEGL|ld-linux-x86-64|ld-linux-aarch64|libssl|libcrypto)\.so'
{
    echo "variant $variant, arch $arch, built on $(. /etc/os-release && echo "$PRETTY_NAME"), $(ldd --version | sed -n 1p)"
    for f in "${bins[@]}"; do
        needed="$(readelf -d "$out/$f" | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p' | tr '\n' ' ')"
        glibc="$(readelf -V "$out/$f" | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1)"
        echo "$f: NEEDED $needed; max $glibc"
        for lib in $needed; do
            [[ "$lib" =~ $allowed || -e "$out/lib/$lib" ]] || { echo "$f links $lib, which is neither an OS library nor in lib/" >&2; exit 1; }
        done
    done
    if [ -d "$out/lib" ]; then
        glibc="$(find "$out/lib" "$out/libexec" -type f -print0 | xargs -0 readelf -V 2>/dev/null | grep -o 'GLIBC_[0-9.]*' | sort -uV | tail -1 || true)"
        echo "bundled lib/ and libexec/: max $glibc"
    fi
} | tee "$out/build-info.txt"
"$out/client_bin" --version
"$out/client_bin" --voice-info | grep -q '^opus=libopus ' # voice chat's codec is linked in
