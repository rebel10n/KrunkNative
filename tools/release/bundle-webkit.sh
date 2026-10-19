#!/usr/bin/env bash
# shellcheck disable=SC2016 # the loader's $ORIGIN is single-quoted on purpose (RUNPATH)
# Bundles WebKitGTK (webkit2gtk-4.1) into a dynamic Linux client build, for the captcha window
# (client/src/ui/kn_webview.c, KN_WEBVIEW=ON): a packaged client then shows krunker.io's security
# check with no system package installed. Run by build-client-linux.sh (dynamic) after client_bin is
# linked, on the same host, so the bundled libraries are the ones client_bin was linked against.
#
# Bundle layout, under <bin dir> (package-client.sh puts it in the install root, /opt/krunknative):
#   lib/                             the libraries client_bin loads: the closure of its ldd minus EXCLUDE
#                                    (what every desktop provides). client_bin: RUNPATH $ORIGIN/lib; the
#                                    libraries: RUNPATH $ORIGIN
#   lib/gio/modules/libgiognutls.so  glib-networking's TLS module (https in WebKit's network process);
#                                    kn_webview.c adds this directory to GIO_EXTRA_MODULES
#   libexec/webkit2gtk-4.1/          WebKitWebProcess, WebKitNetworkProcess, WebKitGPUProcess
#   host-libs.txt                    the sonames the bundle leaves to the player's system (EXCLUDE, needed directly
#                                    by client_bin or a bundled file): the rpm's Requires (package-client.sh)
#   webkit-bundle.txt                WebKitGTK's version and the bundled libraries (for the licence notice)
#
# WebKitGTK starts its helper processes from a directory compiled into libwebkit2gtk (/usr/libexec/
# webkit2gtk-4.1 on Fedora, /usr/lib/x86_64-linux-gnu/webkit2gtk-4.1 on Ubuntu) and has no setting
# for it (WebKit 2.52 reads no WEBKIT_EXEC_PATH). So the bundled copy of libwebkit2gtk, and only that
# copy, gets the compiled-in string replaced by "libexec/webkit2gtk-4.1" (NUL-padded to the same
# length, so nothing else moves). It is relative to the working directory, and client_bin changes to
# its install root at startup (install_root_chdir, client/src/install_root.c), where libexec/ is.
#
# Usage: tools/release/bundle-webkit.sh <bin dir>
# Needs ldd, readlink, pkg-config, python3 and patchelf, and webkit2gtk-4.1 + glib-networking on the host.
set -euo pipefail

[ $# -eq 1 ] || { sed -n '2,22p' "$0" >&2; exit 2; }
bindir="$(cd "$1" && pwd)"
bin="$bindir/client_bin"
[ -f "$bin" ] || { echo "bundle-webkit: no client_bin in $bindir" >&2; exit 1; }
for tool in ldd readelf readlink pkg-config python3 patchelf; do
    command -v "$tool" > /dev/null || { echo "bundle-webkit: $tool is needed" >&2; exit 1; }
done
if readelf -d "$bin" | grep RUNPATH | grep -q '\$ORIGIN/lib\]'; then
    echo "bundle-webkit: $bin is bundled already (its RUNPATH is \$ORIGIN/lib): bundle a fresh build" >&2
    exit 1
fi

# Never bundled: the player's system provides them. The C library and loader, the C++ runtime, zlib and
# OpenSSL (the dynamic package's Depends), the GPU stack (GL, EGL, GBM, DRM, Vulkan: they must match the
# player's driver), the display and input stack (X11, XCB, Wayland, xkbcommon), audio, D-Bus, udev and
# systemd. A bundled copy would shadow the player's own in every library that loads it.
EXCLUDE='^(ld-linux|linux-vdso|libc\.so|libm\.so|libdl\.so|libpthread\.so|librt\.so|libutil\.so|libresolv\.so|libmvec\.so|libanl\.so|libnss_|libcrypt\.so|libgcc_s\.so|libstdc\+\+\.so|libz\.so|libssl\.so|libcrypto\.so|libEGL|libGL|libOpenGL|libGLES|libgbm\.so|libdrm|libvulkan|libglapi|libLLVM|libnvidia|libX|libxcb|libxkbcommon|libwayland|libxshmfence|libasound|libpulse|libdbus-1|libudev|libsystemd)'

declare -A libs=() # the name the loader asks for -> the file it resolves to on this host
add_deps() { # add_deps <ELF file>: its ldd closure, minus EXCLUDE
    local out name path
    out="$(env -u LD_LIBRARY_PATH ldd "$1")" || { echo "bundle-webkit: ldd failed on $1" >&2; exit 1; }
    if grep -q 'not found' <<< "$out"; then
        echo "bundle-webkit: $1 needs a library this host does not have:" >&2
        grep 'not found' <<< "$out" >&2
        exit 1
    fi
    while read -r name path; do
        echo "$name" | grep -Eq "$EXCLUDE" && continue
        libs["$name"]="$path"
    done < <(sed -n 's|^[[:space:]]*\([^[:space:]]*\) => \(/[^[:space:]]*\) (0x[0-9a-f]*)$|\1 \2|p' <<< "$out")
}

# the host's libwebkit2gtk-4.1 and its helper processes
webkit="$(env -u LD_LIBRARY_PATH ldd "$bin" | sed -n 's|^[[:space:]]*libwebkit2gtk-4\.1\.so\.0 => \(/[^[:space:]]*\).*|\1|p')"
[ -n "$webkit" ] || { echo "bundle-webkit: client_bin does not link webkit2gtk-4.1 (build it with KN_WEBVIEW=ON)" >&2; exit 1; }
webkit_real="$(readlink -f "$webkit")"
wdir="$(dirname "$webkit_real")"
helpers=""
for d in "$wdir/webkit2gtk-4.1" "$wdir/../libexec/webkit2gtk-4.1"; do
    if [ -f "$d/WebKitWebProcess" ]; then helpers="$(readlink -f "$d")"; break; fi
done
[ -n "$helpers" ] || { echo "bundle-webkit: no WebKitWebProcess next to $webkit_real" >&2; exit 1; }

tls="$(find "$(pkg-config --variable=giomoduledir gio-2.0)" -maxdepth 1 -name libgiognutls.so 2>/dev/null | head -1)"
[ -n "$tls" ] || { echo "bundle-webkit: glib-networking's libgiognutls.so is not installed (https in WebKit)" >&2; exit 1; }

add_deps "$bin"
add_deps "$helpers/WebKitWebProcess"
add_deps "$helpers/WebKitNetworkProcess"
[ -f "$helpers/WebKitGPUProcess" ] && add_deps "$helpers/WebKitGPUProcess"
add_deps "$tls"

rm -rf "${bindir:?}/lib" "${bindir:?}/libexec"
mkdir -p "$bindir/lib/gio/modules" "$bindir/libexec/webkit2gtk-4.1"
for name in "${!libs[@]}"; do
    real="$(readlink -f "${libs[$name]}")"
    base="$(basename "$real")"
    if [ ! -f "$bindir/lib/$base" ]; then
        cp -L "$real" "$bindir/lib/$base"
        chmod u+w "$bindir/lib/$base"
    fi
    [ "$name" = "$base" ] || ln -sfn "$base" "$bindir/lib/$name"
done

# the helper-path string of the bundled libwebkit2gtk (see the header)
python3 -I - "$bindir/lib/$(basename "$webkit_real")" "$helpers" << 'PYEOF'
import sys

path, old_dir = sys.argv[1], sys.argv[2]
new = b"libexec/webkit2gtk-4.1"
old = old_dir.encode()
if len(new) > len(old):
    sys.exit("bundle-webkit: the relative helper path is longer than the compiled-in one")
data = bytearray(open(path, "rb").read())
needle = b"\x00" + old + b"\x00"
count = data.count(needle)
if count != 1:
    sys.exit(f"bundle-webkit: {old_dir} occurs {count} times in {path} (want 1)")
at = data.find(needle) + 1
data[at:at + len(old) + 1] = new + b"\x00" * (len(old) + 1 - len(new))
open(path, "wb").write(bytes(data))
PYEOF

for helper in WebKitWebProcess WebKitNetworkProcess WebKitGPUProcess; do
    if [ -f "$helpers/$helper" ]; then cp -L "$helpers/$helper" "$bindir/libexec/webkit2gtk-4.1/$helper"; fi
done
[ -f "$bindir/libexec/webkit2gtk-4.1/WebKitWebProcess" ] && [ -f "$bindir/libexec/webkit2gtk-4.1/WebKitNetworkProcess" ] \
    || { echo "bundle-webkit: the web and network processes were not copied" >&2; exit 1; }
cp -L "$tls" "$bindir/lib/gio/modules/libgiognutls.so"
# the system's share of the bundle: the sonames that EXCLUDE leaves to the player's system and that a file of
# the bundle (or client_bin) needs directly. A library's own host dependencies are not listed: they are
# its business, not the package's
{
    for f in "$bin" "$bindir"/lib/*.so* "$bindir"/lib/gio/modules/*.so "$bindir"/libexec/webkit2gtk-4.1/*; do
        if [ -f "$f" ] && [ ! -L "$f" ]; then readelf -d "$f" 2> /dev/null | sed -n 's/.*(NEEDED).*\[\(.*\)\]/\1/p'; fi
    done
} | { grep -E "$EXCLUDE" || true; } | LC_ALL=C sort -u > "$bindir/host-libs.txt"

# RUNPATH: client_bin finds lib/, the libraries find each other, the helpers and the TLS module find lib/
patchelf --set-rpath '$ORIGIN/lib' "$bin"
for f in "$bindir"/lib/*.so*; do
    if [ -f "$f" ] && [ ! -L "$f" ]; then patchelf --set-rpath '$ORIGIN' "$f"; fi
done
patchelf --set-rpath '$ORIGIN/../..' "$bindir/lib/gio/modules/libgiognutls.so"
for f in "$bindir"/libexec/webkit2gtk-4.1/*; do patchelf --set-rpath '$ORIGIN/../../lib' "$f"; done

# self-check: client_bin resolves every library inside the bundle (or the host's own)
resolved="$(env -u LD_LIBRARY_PATH ldd "$bin")"
if grep -q 'not found' <<< "$resolved"; then
    echo "bundle-webkit: client_bin does not resolve:" >&2
    grep 'not found' <<< "$resolved" >&2
    exit 1
fi
webkit_now="$(sed -n 's|^[[:space:]]*libwebkit2gtk-4\.1\.so\.0 => \(/[^[:space:]]*\).*|\1|p' <<< "$resolved")"
case "$webkit_now" in
    "$bindir/lib/"*) ;;
    *) echo "bundle-webkit: client_bin would load webkit2gtk from $webkit_now, not the bundle" >&2; exit 1 ;;
esac

version="$(pkg-config --modversion webkit2gtk-4.1)"
{
    echo "WebKitGTK $version (webkit2gtk-4.1) and the libraries it needs, from the build host's packages."
    echo "The bundled libwebkit2gtk has one changed string: the helper process directory (see"
    echo "tools/release/bundle-webkit.sh). Every library is the host's copy, unmodified, except that."
    echo
    echo "lib/ (libraries, RUNPATH \$ORIGIN):"
    (cd "$bindir/lib" && find . -maxdepth 1 \( -type f -o -type l \) -name '*.so*' | sed 's|^\./|  |' | LC_ALL=C sort)
    echo "lib/gio/modules/ (glib-networking TLS module):"
    echo "  libgiognutls.so"
    echo "libexec/webkit2gtk-4.1/ (helper processes):"
    (cd "$bindir/libexec/webkit2gtk-4.1" && find . -maxdepth 1 -type f | sed 's|^\./|  |' | LC_ALL=C sort)
} > "$bindir/webkit-bundle.txt"

echo "bundle-webkit: WebKitGTK $version bundled in $bindir (lib/ $(du -sh "$bindir/lib" | cut -f1), $(find "$bindir/lib" -maxdepth 1 -name '*.so*' | wc -l) libraries)" >&2
