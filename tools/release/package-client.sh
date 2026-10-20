#!/usr/bin/env bash
# Packages a native Krunker Box Office client build as one release asset per platform
# (.github/workflows/client.yml, publish.sh):
#   <out>/BoxOffice-linux-x64.tar.gz      <out>/BoxOffice-linux-arm64.tar.gz
#   <out>/BoxOffice-windows-x64.zip
#   <out>/BoxOffice-macos-arm64.zip       <out>/BoxOffice-macos-x64.zip
# The package is flat: the launcher and runtime/ at the top, no folder around them:
#   BoxOffice (BoxOffice.exe; macOS: BoxOffice.app)  the launcher, the one file a player starts
#   runtime/client_bin (client.exe)                  the game
#   runtime/server_bin (server.exe)                  the dedicated server, for hosting on this machine
#   runtime/ui/, runtime/assets/                     the HTML menu and the git-tracked part of assets/
#                                                    (shaders, maps, ...); the launcher downloads the rest
#   runtime/licenses/, runtime/kn-install.json       the third-party licence texts; the updater's marker
#   runtime/*.dll (Windows), runtime/libEGL.dylib + libGLESv2.dylib (macOS, ANGLE), runtime/lib/ and
#   runtime/libexec/ (Linux: WebKitGTK for the captcha webview, in every Linux client)
#   README.txt, LICENSE-NOTICE.txt, SHA256SUMS      (the checksums of every other file in the package)
# The launcher is the entry point on every platform. Linux: tar.gz; Windows and macOS: zip. There is
# no .deb or .rpm: the archive is the install, and each platform has exactly one archive.
#
# Usage: tools/release/package-client.sh <platform> <bin dir> <out dir> [options]
#   <platform>         linux-x64 linux-arm64 windows-x64 macos-arm64 macos-x64 (also the platform id
#                      the launcher and the updates manifest use)
#   <bin dir>          client_bin, server_bin and the launcher BoxOffice (windows: client.exe, server.exe,
#                      BoxOffice.exe and *.dll; macos: + libEGL.dylib libGLESv2.dylib), e.g. the output
#                      of build-client-linux.sh or build-client.sh
#   --launcher <file>  the launcher (default <bin dir>/BoxOffice or BoxOffice.exe)
#   --assets <dir>     full package: the whole game content as assets/ too (the name gets -full)
#   --channel <name>   kn-install.json channel (default latest; client-v* tags: the tag)
# Env: KN_DESCRIBE KN_COMMIT KN_COMMIT_TIME KN_BUILD_TIME KN_PROTOCOL (tools/release/version-meta.sh)
set -euo pipefail

usage() { sed -n '2,36p' "$0" >&2; exit 2; }
[ $# -ge 3 ] || usage
platform="$1" bindir="$2" out="$3"
shift 3
launcher="" assets="" channel="latest"
while [ $# -gt 0 ]; do
    case "$1" in
        --launcher) launcher="$2"; shift 2 ;;
        --assets) assets="$2"; shift 2 ;;
        --channel) channel="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; usage ;;
    esac
done

case "$platform" in
    linux-x64|linux-arm64) os=linux; ext=tar.gz ;;
    windows-x64) os=windows; ext=zip ;;
    macos-arm64|macos-x64) os=macos; ext=zip ;;
    *) echo "unknown platform $platform" >&2; usage ;;
esac
if [ -n "$assets" ]; then
    [ -d "$assets" ] || { echo "no assets directory $assets" >&2; exit 1; }
fi
[[ "$channel" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "bad channel $channel" >&2; exit 2; }

repo="$(cd "$(dirname "$0")/../.." && pwd)"
client="client_bin" server="server_bin" launcher_name="BoxOffice"
[ "$os" = windows ] && client="client.exe" server="server.exe" launcher_name="BoxOffice.exe"
[ -n "$launcher" ] || launcher="$bindir/$launcher_name"
for f in "$bindir/$client" "$bindir/$server"; do [ -f "$f" ] || { echo "no $(basename "$f") in $bindir" >&2; exit 1; }; done
[ -f "$launcher" ] || { echo "no launcher at $launcher (the package's entry point)" >&2; exit 1; }
name="BoxOffice-$platform${assets:+-full}"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

sha256() { if command -v sha256sum > /dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }
sha256_cmd=(sha256sum)
command -v sha256sum > /dev/null || sha256_cmd=(shasum -a 256)
# GNU tar for sorted, owner-less archives (macOS: gtar when Homebrew has it)
tar_bin=tar
command -v gtar > /dev/null && tar_bin=gtar
tar_flags=()
if "$tar_bin" --version 2> /dev/null | grep -q 'GNU tar'; then
    tar_flags=(--sort=name --owner=0 --group=0 --numeric-owner)
    [ -n "${KN_COMMIT_TIME:-}" ] && tar_flags+=("--mtime=@$KN_COMMIT_TIME")
fi

describe="${KN_DESCRIBE:-unknown}"
stage="$work/package"   # the archive's top level
rt="$stage/runtime"     # everything but the launcher
mkdir -p "$rt"

# the glibc floor of the package: the newest GLIBC_ symbol version the binaries ask for (readme)
glibc_min=""
if [ "$os" = linux ] && command -v readelf > /dev/null; then
    glibc_min="$({ readelf -V "$bindir/$client" "$bindir/$server" "$launcher"
        find "$bindir/lib" "$bindir/libexec" -type f -print0 2> /dev/null | xargs -0 -r readelf -V; } \
        | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1 || true)"
fi

# ---------------------------------------------------------------- runtime/
# tracked_assets: the git-tracked files under assets/ (paths relative to it), without its README;
# outside a git checkout (a source tarball) the code-coupled directories
tracked_assets() {
    if git -C "$repo" rev-parse --is-inside-work-tree > /dev/null 2>&1; then
        git -C "$repo" ls-files -- assets | sed 's|^assets/||' | grep -vx 'README.md'
    else
        (cd "$repo/assets" && find shaders maps img editor -type f && ls skins.json previews.json titles.json 2> /dev/null)
    fi
}

# fill_runtime: ui/, the code-coupled assets, the content (--assets), licences, the updater's marker
fill_runtime() {
    mkdir -p "$rt/assets" "$rt/licenses"
    cp -R "$repo/ui" "$rt/ui"
    # git-tracked, code-coupled content (assets/README.md: only these are in git): every tracked
    # file, not a list of directories, so a new one (titles.json, the Nettspend sounds/kn_*.mp3)
    # ships with the build that uses it; the launcher's content index leaves these out
    tracked_assets | while IFS= read -r f; do
        mkdir -p "$rt/assets/$(dirname "$f")"
        cp "$repo/assets/$f" "$rt/assets/$f"
    done
    if [ -n "$assets" ]; then
        # the whole content tree (package-assets.sh --list: the same filter as the assets bundle)
        "$repo/tools/release/package-assets.sh" --list "$assets" | (cd "$assets" && while IFS= read -r f; do
            mkdir -p "$rt/assets/$(dirname "$f")"
            cp -L "$f" "$rt/assets/$f"
        done)
    fi
    # the updater's marker: the launcher finds the install root ("..", above runtime/) from it
    printf '{"format":1,"platform":"%s","package":"%s","channel":"%s","root":".."}\n' \
        "$platform" "$ext" "$channel" > "$rt/kn-install.json"
    licenses
}

# licences/: the third-party texts of the compiled-in libraries (runtime/licenses/); LICENSE-NOTICE.txt
# at the top of the package names them
licenses() {
    local root="$rt"
    cp "$repo/client/lib/glfw/LICENSE.md" "$root/licenses/glfw.md"
    cp "$repo/client/lib/freetype/LICENSE.TXT" "$root/licenses/freetype.txt"
    cp "$repo/client/lib/lexbor/LICENSE" "$root/licenses/lexbor.txt"
    cp "$repo/client/lib/miniaudio/LICENSE" "$root/licenses/miniaudio.txt"
    cp "$repo/client/lib/opus/COPYING" "$root/licenses/opus.txt"
    cp "$repo/client/lib/nanosvg/LICENSE.txt" "$root/licenses/nanosvg.txt"
    cp "$repo/client/lib/cgltf/LICENSE" "$root/licenses/cgltf.txt"
    sed -n '/^\/\*/,/\*\//p' "$repo/shared/lib/cJSON/cJSON.h" | sed -n '1,/\*\//p' > "$root/licenses/cJSON.txt"
    cp "$repo/shared/lib/pcg_basic/LICENSE.txt" "$root/licenses/pcg_basic.txt"
    cp "$repo/server/lib/replxx/LICENSE.md" "$root/licenses/replxx.md"
    cat > "$stage/LICENSE-NOTICE.txt" << 'NOTICE'
Krunker Box Office client: license notice
=========================================

Krunker Box Office (formerly KrunkNative, https://github.com/rebel10n/KrunkNative) is a
native C port of Krunker. Its repository does not currently carry a license file: no license
to the Krunker Box Office source or binaries is granted by this package beyond what its
authors state in the repository.

Krunker, its maps, menu artwork, models, textures, sounds and other game content are the
property of their owners (Yendis Entertainment / FRVR). They are included only so the game
runs; they are not covered by any Krunker Box Office license.

Third-party components compiled into the binaries, with their licenses in runtime/licenses/:
  GLFW        zlib/libpng License         runtime/licenses/glfw.md
  FreeType    FreeType License (FTL)      runtime/licenses/freetype.txt
  Lexbor      Apache License 2.0          runtime/licenses/lexbor.txt
  miniaudio   public domain / MIT-0       runtime/licenses/miniaudio.txt
  Opus        BSD 3-Clause (libopus)      runtime/licenses/opus.txt
  nanosvg     zlib License                runtime/licenses/nanosvg.txt
  cgltf       MIT                         runtime/licenses/cgltf.txt
  cJSON       MIT                         runtime/licenses/cJSON.txt
  pcg_basic   Apache License 2.0          runtime/licenses/pcg_basic.txt
  replxx      BSD 3-Clause (server_bin)   runtime/licenses/replxx.md
  glad        generated loader, public domain / CC0 / MIT

Windows packages contain the MinGW-w64 runtime (statically linked: ZPL / MIT / GPL with
the GCC runtime library exception). macOS packages bundle ANGLE (libEGL, libGLESv2:
BSD 3-Clause, https://chromium.googlesource.com/angle/angle/+/HEAD/LICENSE) and may bundle
Homebrew's OpenSSL (Apache License 2.0). The Linux packages link OpenSSL 3 statically
(Apache License 2.0, https://www.openssl.org/source/license.html) and are patched by
polyfill-glibc (https://github.com/corsix/polyfill-glibc, MIT, its polyfill code is license-waived).
NOTICE
    if [ -d "$bindir/lib" ]; then
        # the Linux client's WebKitGTK (lib/, libexec/; tools/release/bundle-webkit.sh)
        cp "$bindir/webkit-bundle.txt" "$root/licenses/webkitgtk-bundle.txt"
        cat >> "$stage/LICENSE-NOTICE.txt" << 'NOTICE'

WebKitGTK (webkit2gtk-4.1, the captcha webview) and the libraries bundled with it (runtime/lib/,
runtime/libexec/; the list is runtime/licenses/webkitgtk-bundle.txt) are free software under their own
licences: WebKitGTK under the GNU Lesser General Public License, version 2.1 or later (WebKit also
BSD-2-Clause in parts), GTK, GLib and libsoup under the LGPL too, the others mostly LGPL, MIT, BSD or
zlib (their licence texts come with their source). The source of WebKitGTK and the bundled libraries:
https://webkitgtk.org/ and the source packages of the distribution this bundle was built on.
The libraries are dynamically linked: the files in runtime/lib/ can be replaced by another build of
the same libraries. The bundled libwebkit2gtk-4.1 differs from the distribution's in one string only,
its helper process directory (tools/release/bundle-webkit.sh, which writes that change).
NOTICE
    fi
}

readme() { # readme <file>: README.txt at the top of the package
    local file="$1" run runtime_line
    case "$platform" in
        linux-*) runtime_line="Linux with glibc ${glibc_min:-2.35}+, OpenSSL 3 (libssl3) and OpenGL 4.5 drivers (Mesa 20.3+, NVIDIA, AMD)" ;;
        windows-*) runtime_line="Windows 10+ x64 with OpenGL 4.5 drivers" ;;
        macos-*) runtime_line="macOS 11+ ($platform), rendering through Metal (ANGLE)" ;;
    esac
    case "$os" in
        linux) run="Start BoxOffice: in a terminal, from this folder, run  ./BoxOffice  (a file manager may ask to run it).
Unpack the archive into a folder of its own: it puts BoxOffice and runtime/ in the folder you choose." ;;
        windows) run="Start BoxOffice.exe (double-click it). Unpack the zip into a folder of its own." ;;
        macos) run="Open BoxOffice.app. It is not notarized: on the first start right-click it and pick Open, or run
  xattr -dr com.apple.quarantine BoxOffice.app
Unpack the zip into a folder of its own." ;;
    esac
    {
        echo "Krunker Box Office client ($platform)"
        echo "========================================="
        echo
        echo "Version:   $describe"
        echo "Commit:    ${KN_COMMIT:-unknown}"
        echo "Built:     ${KN_BUILD_TIME:-unknown}"
        echo "Protocol:  ${KN_PROTOCOL:-unknown} (NET_PROTOCOL major.minor)"
        echo "Runs on:   $runtime_line"
        if [ -d "$bindir/lib" ]; then echo "Bundled:   WebKitGTK for the captcha webview (runtime/lib/, runtime/libexec/; see LICENSE-NOTICE.txt)"; fi
        echo
        echo "$run"
        echo
        if [ -n "$assets" ]; then
            echo "This is the full package: the game content (runtime/assets/) is included."
        else
            echo "The game content (runtime/assets/: models, textures, sounds, ~1.7 GB) is not in this package."
            echo "The launcher downloads it on the first start and keeps the client up to date. It goes into"
            echo "runtime/assets/, or into your user data folder when runtime/ is read-only."
        fi
        echo
        echo "Settings and logins are stored in ~/.config/krunknative (Windows: %USERPROFILE%\\.config\\krunknative)."
        echo
        echo "Files"
        echo "-----"
        if [ "$os" = macos ]; then echo "  BoxOffice.app        the launcher: start this"; else echo "  $launcher_name            the launcher: start this"; fi
        echo "  runtime/             the game, the dedicated server, the menu (ui/), assets/ and licences"
        echo "  runtime/$client      the game itself (runtime/$server is the dedicated server: --help lists its flags)"
        echo "  runtime/host-libs.txt the system libraries it loads (a desktop Linux install has them: GL, X11, OpenSSL 3, zlib)"
        echo "  LICENSE-NOTICE.txt   third-party licences; SHA256SUMS: checksums of every file in this package"
        echo
        echo "To host a game, use the server packages instead of this one:"
        echo "  https://github.com/rebel10n/KrunkNative/releases/tag/server-latest"
        echo "Releases: https://github.com/rebel10n/KrunkNative/releases"
    } > "$file"
}

# sums <dir>: SHA256SUMS of every file under <dir> (paths relative to <dir>), written into <dir>
sums() {
    (cd "$1" && find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | xargs -0 "${sha256_cmd[@]}" | sed 's|  \./|  |') > "$work/SHA256SUMS"
    mv "$work/SHA256SUMS" "$1/SHA256SUMS"
}

# ---------------------------------------------------------------- binaries
# macOS: copy every dylib outside /usr/lib, /System and @rpath (Homebrew OpenSSL, ...) next to the
# binary and point the load commands at @executable_path (package-server.sh does the same).
bundle_dylibs() {
    local dir="$1" target="$2" dep base
    while read -r dep; do
        base="$(basename "$dep")"
        if [ ! -f "$dir/$base" ]; then
            cp "$dep" "$dir/$base"
            chmod 0644 "$dir/$base"
            install_name_tool -id "@executable_path/$base" "$dir/$base"
            bundle_dylibs "$dir" "$dir/$base"
        fi
        install_name_tool -change "$dep" "@executable_path/$base" "$target"
    done < <(otool -L "$target" | awk 'NR > 1 { print $1 }' | grep -Ev '^(/usr/lib/|/System/|@)' || true)
}

# runtime/: the game, the server and the bundled libraries
put_runtime_bins() {
    install -m 0755 "$bindir/$client" "$bindir/$server" "$rt/"
    case "$os" in
        windows)
            find "$bindir" -maxdepth 1 -type f -iname '*.dll' -exec cp {} "$rt/" \;
            ;;
        linux)
            # a dynamic build's WebKitGTK for the captcha webview (tools/release/bundle-webkit.sh)
            for d in lib libexec; do
                if [ -d "$bindir/$d" ]; then cp -R "$bindir/$d" "$rt/"; fi
            done
            # the system libraries the bundle leaves to the distribution (desktop graphics, X11, OpenSSL 3, zlib)
            if [ -f "$bindir/host-libs.txt" ]; then cp "$bindir/host-libs.txt" "$rt/host-libs.txt"; fi
            ;;
        macos)
            for lib in libEGL.dylib libGLESv2.dylib; do
                [ -f "$bindir/$lib" ] || { echo "no $lib in $bindir (ANGLE build?)" >&2; exit 1; }
                install -m 0644 "$bindir/$lib" "$rt/"
            done
            ;;
    esac
    return 0
}

# BoxOffice: the launcher at the top (macOS: Contents/MacOS/BoxOffice in BoxOffice.app). It has no
# bundled dylibs of its own: ANGLE (macOS) is loaded from runtime/ through an rpath.
put_launcher() {
    case "$os" in
        windows|linux)
            install -m 0755 "$launcher" "$stage/$launcher_name"
            ;;
        macos)
            local app="$stage/BoxOffice.app" exe
            exe="$app/Contents/MacOS/BoxOffice"
            mkdir -p "$app/Contents/MacOS" "$app/Contents/Resources"
            install -m 0755 "$launcher" "$exe"
            if otool -L "$exe" | awk 'NR > 1 { print $1 }' \
                | grep -Ev '^(/usr/lib/|/System/|@rpath/lib(EGL|GLESv2)\.dylib)' > /dev/null; then
                echo "the launcher links a dylib that is neither system nor ANGLE:" >&2
                otool -L "$exe" >&2
                exit 1
            fi
            install_name_tool -add_rpath "@executable_path/../../../runtime" "$exe" 2> /dev/null || true
            otool -l "$exe" | grep -q '@executable_path/../../../runtime' || { echo "the launcher has no rpath to runtime/" >&2; exit 1; }
            ;;
    esac
}

# ---------------------------------------------------------------- assemble
put_launcher
put_runtime_bins
fill_runtime
if [ "$os" = macos ]; then
    app="$stage/BoxOffice.app"
    for f in "$rt/$client" "$rt/$server"; do bundle_dylibs "$rt" "$f"; done
    if command -v sips > /dev/null && command -v iconutil > /dev/null; then
        iconset="$work/BoxOffice.iconset"
        mkdir -p "$iconset"
        # krunker.io's 1024 px app icon (tools/branding/fetch_branding.py; the iOS app's too)
        icon_src="$repo/platform/ios/KrunkNative/Assets.xcassets/AppIcon.appiconset/icon-1024.png"
        for s in 16 32 128 256 512; do
            sips -z "$s" "$s" "$icon_src" --out "$iconset/icon_${s}x${s}.png" > /dev/null
            d=$((s * 2)); sips -z "$d" "$d" "$icon_src" --out "$iconset/icon_${s}x${s}@2x.png" > /dev/null
        done
        iconutil -c icns "$iconset" -o "$app/Contents/Resources/BoxOffice.icns"
    fi
    minos="$(otool -l "$rt/$client" | awk '$1 == "minos" { print $2; exit }')"
    short="$(echo "$describe" | sed -n 's/^[a-z-]*v\([0-9][0-9.]*\).*/\1/p')"
    cat > "$app/Contents/Info.plist" << PLIST
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Krunker Box Office</string>
  <key>CFBundleDisplayName</key><string>Krunker Box Office</string>
  <key>CFBundleIdentifier</key><string>io.krunk.krunknative</string>
  <key>CFBundleExecutable</key><string>BoxOffice</string>
  <key>CFBundleIconFile</key><string>BoxOffice</string>
  <key>CFBundlePackageType</key><string>APPL</string>
  <key>CFBundleShortVersionString</key><string>${short:-0.0.0}</string>
  <key>CFBundleVersion</key><string>${KN_COMMIT_TIME:-0}</string>
  <key>CFBundleGetInfoString</key><string>Krunker Box Office $describe</string>
  <key>LSMinimumSystemVersion</key><string>${minos:-11.0}</string>
  <key>LSApplicationCategoryType</key><string>public.app-category.action-games</string>
  <key>NSHighResolutionCapable</key><true/>
  <key>NSMicrophoneUsageDescription</key><string>Krunker Box Office uses the microphone for in-game voice chat.</string>
</dict>
</plist>
PLIST
    # ad-hoc signatures (not notarized): the nested code first (dylibs, the executables), then the
    # bundle, which signs its CFBundleExecutable. Before the checksums: signing changes the binaries.
    if command -v codesign > /dev/null; then
        for f in "$rt"/*.dylib "$rt/$client" "$rt/$server"; do [ -f "$f" ] && codesign --force --sign - "$f"; done
        codesign --force --sign - "$app/Contents/MacOS/BoxOffice"
        codesign --force --sign - "$app"
        codesign --verify --deep --strict "$app"
    fi
    if command -v otool > /dev/null; then echo "== otool -L $client" >&2; otool -L "$rt/$client" >&2; fi
fi

readme "$stage/README.txt"
sums "$stage"

# ---------------------------------------------------------------- the archive
# the package's top level, by name (no "./" entries, no wrapping folder)
if [ "$os" = macos ]; then entries=(BoxOffice.app); else entries=("$launcher_name"); fi
entries+=(runtime README.txt LICENSE-NOTICE.txt SHA256SUMS)
file="$out/$name.$ext"
rm -f "$file"
case "$ext" in
    tar.gz) (cd "$stage" && "$tar_bin" ${tar_flags[@]+"${tar_flags[@]}"} -cf - "${entries[@]}" | gzip -n -9 > "$file") ;;
    zip) (cd "$stage" && zip -qr9 -y "$file" "${entries[@]}") ;;
esac
echo "packaged $file ($(du -h "$file" | awk '{ print $1 }'))" >&2
