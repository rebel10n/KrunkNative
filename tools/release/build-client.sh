#!/usr/bin/env bash
# Builds the macOS and Windows client package binaries (.github/workflows/client.yml, then
# tools/release/package-client.sh); Linux has build-client-linux.sh.
#
#   macos    KRUNKNATIVE_GL_BACKEND=angle (Metal) on the prebuilt ANGLE in $ANGLE_DIR
#            (tools/angle/fetch-angle.sh), macOS 11.0+ (ANGLE's minimum); <out> gets client_bin,
#            server_bin, libEGL.dylib, libGLESv2.dylib. OpenSSL: static, the pinned build of
#            tools/release/build-openssl.sh in $KN_OPENSSL_PREFIX (built there unless cached)
#   windows  MSYS2 MINGW64 (or a cross build: CMAKE=mingw64-cmake), everything static
#            (-static: no libgcc/libstdc++/winpthread DLLs); <out> gets client.exe, server.exe
#            and any MinGW DLL they still import (normally none)
# Both: FreeType without zlib/bzip2/png/harfbuzz/brotli, stripped, plus the launcher (target
# kn_launcher: krunknative / KrunkNative.exe) when the tree has it.
#
# Usage: tools/release/build-client.sh <macos|windows> <out dir>
# Env: KN_CMAKE_ARGS  extra -D flags (CI: version-meta.sh cmake_flags)
#      KN_BUILD_DIR   build tree (default <out>/../build-<os>)
#      ANGLE_DIR      macOS: the ANGLE build (lib/ + include/)
#      KN_OPENSSL_PREFIX  macOS: static OpenSSL prefix (build-openssl.sh); unset = no TLS in the client
#      CMAKE          cmake command (default cmake)
set -euo pipefail

[ $# -eq 2 ] || { sed -n '2,21p' "$0" >&2; exit 2; }
os="$1" out="$2"
case "$os" in macos|windows) ;; *) echo "unknown os $os" >&2; exit 2 ;; esac
repo="$(cd "$(dirname "$0")/../.." && pwd)"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
build="${KN_BUILD_DIR:-$(dirname "$out")/build-$os}"
cmake_cmd="${CMAKE:-cmake}"

flags=(-G Ninja -DCMAKE_BUILD_TYPE=Release -DKRUNKNATIVE_HTML_ENGINE=ON
    -DFT_DISABLE_ZLIB=TRUE -DFT_DISABLE_BZIP2=TRUE -DFT_DISABLE_PNG=TRUE -DFT_DISABLE_HARFBUZZ=TRUE -DFT_DISABLE_BROTLI=TRUE)
case "$os" in
    macos)
        [ -n "${ANGLE_DIR:-}" ] && [ -f "$ANGLE_DIR/lib/libEGL.dylib" ] || { echo "ANGLE_DIR has no lib/libEGL.dylib (tools/angle/fetch-angle.sh)" >&2; exit 1; }
        flags+=(-DKRUNKNATIVE_GL_BACKEND=angle "-DANGLE_DIR=$ANGLE_DIR" -DCMAKE_OSX_DEPLOYMENT_TARGET=11.0)
        if [ -n "${KN_OPENSSL_PREFIX:-}" ]; then
            MACOSX_DEPLOYMENT_TARGET=11.0 "$repo/tools/release/build-openssl.sh" "$KN_OPENSSL_PREFIX"
            flags+=("-DOPENSSL_ROOT_DIR=$KN_OPENSSL_PREFIX" -DOPENSSL_USE_STATIC_LIBS=TRUE -DKRUNKNATIVE_TLS_STATIC=ON)
        else
            flags+=(-DCMAKE_DISABLE_FIND_PACKAGE_OpenSSL=TRUE)
        fi
        client=client_bin server=server_bin launcher=krunknative ;;
    windows)
        flags+=(-DBUILD_SHARED_LIBS=OFF -DCMAKE_EXE_LINKER_FLAGS=-static)
        client=client.exe server=server.exe launcher=KrunkNative.exe ;;
esac
# shellcheck disable=SC2086 # KN_CMAKE_ARGS is a list of -D flags
"$cmake_cmd" -S "$repo" -B "$build" "${flags[@]}" ${KN_CMAKE_ARGS:-} > "$build.configure.log" 2>&1 || { cat "$build.configure.log" >&2; exit 1; }
grep -E 'GL backend|OpenSSL' "$build.configure.log" || true

targets=("${client%.exe}" "${server%.exe}")
[ "$os" = windows ] && targets=(client server)
if cmake --build "$build" --target help 2> /dev/null | grep '^kn_launcher:' > /dev/null; then targets+=(kn_launcher); fi
cmake --build "$build" --target "${targets[@]}"

bins=("$client" "$server")
install -m 0755 "$build/$client" "$build/$server" "$out/"
if [[ " ${targets[*]} " == *" kn_launcher "* ]]; then
    found="$(find "$build" -maxdepth 3 -type f -name "$launcher" | head -1)"
    [ -n "$found" ] || { echo "kn_launcher built no $launcher" >&2; exit 1; }
    install -m 0755 "$found" "$out/$launcher"
    bins+=("$launcher")
fi

case "$os" in
    macos)
        cp "$build/libEGL.dylib" "$build/libGLESv2.dylib" "$out/"
        # strip drops the arm64 linker signature: ad-hoc sign again (package-client.sh signs the .app)
        for f in "${bins[@]}"; do strip -x "$out/$f" && codesign --force --sign - "$out/$f"; done
        for f in "${bins[@]}"; do echo "== otool -L $f"; otool -L "$out/$f"; done
        otool -L "$out/$client" | grep -q '@rpath/libEGL.dylib'
        otool -l "$out/$client" | grep -A2 LC_RPATH | grep -q '@executable_path'
        ;;
    windows)
        strip_cmd="strip"
        command -v x86_64-w64-mingw32-strip > /dev/null && strip_cmd=x86_64-w64-mingw32-strip
        for f in "${bins[@]}"; do "$strip_cmd" "$out/$f"; done
        # whatever MinGW DLL is still imported (MSYS2's ldd prints .../mingw64/bin/<dll>), recursively
        if command -v ldd > /dev/null && [ -n "${MSYSTEM:-}" ]; then
            todo=("${bins[@]/#/$out/}")
            while [ ${#todo[@]} -gt 0 ]; do
                target="${todo[0]}"; todo=("${todo[@]:1}")
                while read -r dll; do
                    [ -n "$dll" ] && [ ! -f "$out/$(basename "$dll")" ] || continue
                    cp "$dll" "$out/"
                    todo+=("$dll")
                done < <(ldd "$target" | awk '{ print $3 }' | grep -Ei '/(mingw64|ucrt64)/bin/' || true)
            done
        fi
        ;;
esac
ls -la "$out"
# not for a cross build (Linux host, CMAKE=mingw64-cmake)
if [ "$os" = macos ] || [ -n "${MSYSTEM:-}" ]; then "$out/$client" --version; fi
