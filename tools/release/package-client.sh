#!/usr/bin/env bash
# Packages a native Krunker Box Office client build as release assets (.github/workflows/client.yml),
# named like fastfetch's: krunknative-client-<platform>.<ext> + <file>.sha256 (sha256sum format).
# Players see "Krunker Box Office" (formerly KrunkNative: window titles, the desktop entry, the
# .app's CFBundleName / CFBundleDisplayName); the file names keep the old name on purpose: the
# updates manifest, the Download window, CI and installed launchers' self-update locate the
# packages, the launcher (krunknative / KrunkNative.exe) and KrunkNative.app by these names.
#
#   linux-amd64 linux-aarch64                       .tar.gz .zip .deb .rpm  (distro OpenSSL 3, glibc 2.34+)
#   linux-amd64-polyfilled linux-aarch64-polyfilled .tar.gz .zip .deb .rpm  (static deps, glibc 2.17+)
#   windows-amd64                                   .zip
#   macos-aarch64 macos-amd64                       .tar.gz .zip            (KrunkNative.app, ANGLE/Metal)
#
# Layout: krunknative-client-<platform>/ holding client_bin (client.exe), server_bin (server.exe),
# the bundled libraries (Windows DLLs; macOS ANGLE + non-system dylibs), ui/, the code-coupled
# part of assets/ that is in git (shaders, maps, img, editor, skins.json, previews.json; the
# rest of the game content comes separately, see --assets and package-assets.sh), kn-install.json
# (the updater's install marker), README.txt, LICENSE-NOTICE.txt + licenses/, SHA256SUMS.
# macOS archives hold KrunkNative.app (Contents/MacOS: binaries + dylibs, Contents/Resources: the
# rest, kn-install.json root "../.."). deb/rpm install the tree to /opt/krunknative, with
# /usr/bin/krunknative, a desktop entry and an icon.
#
# Usage: tools/release/package-client.sh <platform> <bin dir> <out dir> [options]
#   <bin dir>          client_bin + server_bin (windows: client.exe + server.exe + *.dll; macos:
#                      + libEGL.dylib libGLESv2.dylib), e.g. build-client-linux.sh's output
#   --launcher <file>  the launcher (target kn_launcher; default <bin dir>/krunknative or
#                      KrunkNative.exe when present): shipped as krunknative / KrunkNative.exe /
#                      Contents/MacOS/KrunkNative and made the entry point (/usr/bin/krunknative,
#                      desktop entry, CFBundleExecutable); without one the entry point is client_bin
#   --assets <dir>     full package: add the whole game content (package-assets.sh's filter) as
#                      assets/, file names get "-full"; only .tar.gz (.zip on windows)
#   --channel <name>   kn-install.json channel (default latest; client-v* tags: the tag)
#   --formats "<ext>"  subset of the platform's formats, e.g. "tar.gz deb"
# Env: KN_DESCRIBE KN_COMMIT KN_COMMIT_TIME KN_BUILD_TIME KN_PROTOCOL (tools/release/version-meta.sh)
set -euo pipefail

usage() { sed -n '2,32p' "$0" >&2; exit 2; }
[ $# -ge 3 ] || usage
platform="$1" bindir="$2" out="$3"
shift 3
launcher="" assets="" channel="latest" formats=""
while [ $# -gt 0 ]; do
    case "$1" in
        --launcher) launcher="$2"; shift 2 ;;
        --assets) assets="$2"; shift 2 ;;
        --channel) channel="$2"; shift 2 ;;
        --formats) formats="$2"; shift 2 ;;
        *) echo "unknown option $1" >&2; usage ;;
    esac
done

case "$platform" in
    linux-amd64|linux-aarch64|linux-amd64-polyfilled|linux-aarch64-polyfilled) os=linux; all="tar.gz zip deb rpm" ;;
    windows-amd64) os=windows; all="zip" ;;
    macos-aarch64|macos-amd64) os=macos; all="tar.gz zip" ;;
    *) echo "unknown platform $platform" >&2; usage ;;
esac
arch="${platform#*-}"
arch="${arch%-polyfilled}"
if [ -n "$assets" ]; then
    [ -d "$assets" ] || { echo "no assets directory $assets" >&2; exit 1; }
    all="tar.gz"
    [ "$os" = windows ] && all="zip"
fi
[ -n "$formats" ] || formats="$all"
for f in $formats; do [[ " $all " == *" $f "* ]] || { echo "$platform has no $f package${assets:+ (full)}" >&2; exit 2; }; done
[ -z "$launcher" ] || [ -f "$launcher" ] || { echo "no launcher at $launcher" >&2; exit 1; }
[[ "$channel" =~ ^[A-Za-z0-9._-]+$ ]] || { echo "bad channel $channel" >&2; exit 2; }

repo="$(cd "$(dirname "$0")/../.." && pwd)"
suffix=""
[ "$os" = windows ] && suffix=".exe"
client="client_bin" server="server_bin"
[ "$os" = windows ] && client="client.exe" server="server.exe"
for f in "$client" "$server"; do [ -f "$bindir/$f" ] || { echo "no $f in $bindir" >&2; exit 1; }; done
name="krunknative-client-$platform${assets:+-full}"
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
# The launcher (CMake target kn_launcher, agent client-updates): krunknative / KrunkNative.exe,
# taken from <bin dir> when --launcher is not given. Contents/MacOS/KrunkNative in the .app.
launcher_name=krunknative
[ "$os" = windows ] && launcher_name=KrunkNative.exe
[ "$os" = macos ] && launcher_name=KrunkNative
if [ -z "$launcher" ]; then
    for f in "$bindir/krunknative" "$bindir/KrunkNative.exe"; do [ -f "$f" ] && launcher="$f"; done
fi
entry="$client"
[ -n "$launcher" ] && entry="$launcher_name"

# ---------------------------------------------------------------- the install tree
# fill_root <dir> <package>: everything that lives at the install root (macOS: Contents/Resources)
fill_root() {
    local root="$1" package="$2" rel="."
    [ "$package" = app ] && rel="../.."
    mkdir -p "$root/assets" "$root/licenses"
    cp -R "$repo/ui" "$root/ui"
    # git-tracked, code-coupled content (assets/README.md: only these are in git)
    for d in shaders maps img editor; do cp -R "$repo/assets/$d" "$root/assets/"; done
    for f in skins.json previews.json; do cp "$repo/assets/$f" "$root/assets/"; done
    if [ -n "$assets" ]; then
        # the whole content tree (package-assets.sh --list: the same filter as the assets bundle)
        "$repo/tools/release/package-assets.sh" --list "$assets" | (cd "$assets" && while IFS= read -r f; do
            mkdir -p "$root/assets/$(dirname "$f")"
            cp -L "$f" "$root/assets/$f"
        done)
    fi
    printf '{"format":1,"platform":"%s","package":"%s","channel":"%s","root":"%s"}\n' \
        "$platform" "$package" "$channel" "$rel" > "$root/kn-install.json"
    licenses "$root"
    readme "$root/README.txt" "$package"
}

licenses() {
    local root="$1"
    cp "$repo/client/lib/glfw/LICENSE.md" "$root/licenses/glfw.md"
    cp "$repo/client/lib/freetype/LICENSE.TXT" "$root/licenses/freetype.txt"
    cp "$repo/client/lib/lexbor/LICENSE" "$root/licenses/lexbor.txt"
    cp "$repo/client/lib/miniaudio/LICENSE" "$root/licenses/miniaudio.txt"
    cp "$repo/client/lib/nanosvg/LICENSE.txt" "$root/licenses/nanosvg.txt"
    cp "$repo/client/lib/cgltf/LICENSE" "$root/licenses/cgltf.txt"
    sed -n '/^\/\*/,/\*\//p' "$repo/shared/lib/cJSON/cJSON.h" | sed -n '1,/\*\//p' > "$root/licenses/cJSON.txt"
    cp "$repo/shared/lib/pcg_basic/LICENSE.txt" "$root/licenses/pcg_basic.txt"
    cp "$repo/server/lib/replxx/LICENSE.md" "$root/licenses/replxx.md"
    cat > "$root/LICENSE-NOTICE.txt" << 'EOF'
Krunker Box Office client: license notice
=========================================

Krunker Box Office (formerly KrunkNative, https://github.com/rebel10n/KrunkNative) is a
native C port of Krunker. Its repository does not currently carry a license file: no license
to the Krunker Box Office source or binaries is granted by this package beyond what its
authors state in the repository.

Krunker, its maps, menu artwork, models, textures, sounds and other game content are the
property of their owners (Yendis Entertainment / FRVR). They are included only so the game
runs; they are not covered by any Krunker Box Office license.

Third-party components compiled into the binaries, with their licenses in licenses/:
  GLFW        zlib/libpng License         licenses/glfw.md
  FreeType    FreeType License (FTL)      licenses/freetype.txt
  Lexbor      Apache License 2.0          licenses/lexbor.txt
  miniaudio   public domain / MIT-0       licenses/miniaudio.txt
  nanosvg     zlib License                licenses/nanosvg.txt
  cgltf       MIT                         licenses/cgltf.txt
  cJSON       MIT                         licenses/cJSON.txt
  pcg_basic   Apache License 2.0          licenses/pcg_basic.txt
  replxx      BSD 3-Clause (server_bin)   licenses/replxx.md
  glad        generated loader, public domain / CC0 / MIT

Linux "-polyfilled" packages also link OpenSSL 3 statically (Apache License 2.0,
https://www.openssl.org/source/license.html) and are patched by polyfill-glibc
(https://github.com/corsix/polyfill-glibc, MIT, its polyfill code is license-waived).
Windows packages contain the MinGW-w64 runtime (statically linked: ZPL / MIT / GPL with
the GCC runtime library exception). macOS packages bundle ANGLE (libEGL, libGLESv2:
BSD 3-Clause, https://chromium.googlesource.com/angle/angle/+/HEAD/LICENSE) and may bundle
Homebrew's OpenSSL (Apache License 2.0).
EOF
}

readme() {
    local file="$1" package="$2" runtime run
    case "$platform" in
        linux-*-polyfilled) runtime="any x86_64/aarch64 Linux with glibc 2.17+ (CentOS 7, Debian 9, Ubuntu 18.04 and newer) and OpenGL 4.5 drivers (Mesa 20.3+, NVIDIA, AMD)" ;;
        linux-*) runtime="Linux with glibc 2.34+ and OpenSSL 3 (Debian 12+, Ubuntu 22.04+, Fedora 36+, Arch) and OpenGL 4.5 drivers. Older distros: the -polyfilled package" ;;
        windows-*) runtime="Windows 10+ x64 with OpenGL 4.5 drivers" ;;
        macos-*) runtime="macOS 11+ ($arch), rendering through Metal (ANGLE)" ;;
    esac
    case "$package" in
        deb|rpm) run="Start \"Krunker Box Office\" from the applications menu, or run: krunknative" ;;
        app) run="Open KrunkNative.app. It is not notarized: on the first start right-click it and pick Open, or run
  xattr -dr com.apple.quarantine KrunkNative.app" ;;
        *) run="Run ./$entry from this directory (Windows: double-click $entry)." ;;
    esac
    {
        echo "Krunker Box Office client ($platform)"
        echo "========================================="
        echo
        echo "Version:   $describe"
        echo "Commit:    ${KN_COMMIT:-unknown}"
        echo "Built:     ${KN_BUILD_TIME:-unknown}"
        echo "Protocol:  ${KN_PROTOCOL:-unknown} (NET_PROTOCOL major.minor)"
        echo "Runs on:   $runtime"
        echo
        echo "$run"
        echo
        if [ -n "$assets" ]; then
            echo "This is the full package: the game content (assets/) is included."
        elif [ -n "$launcher" ]; then
            echo "The game content (assets/: models, textures, sounds, ~1.7 GB) is not in this package:"
            echo "the launcher downloads it on the first start and keeps the client up to date."
        else
            echo "The game content (assets/: models, textures, sounds, ~1.7 GB) is not in this package,"
            echo "only the part that is in git (shaders, maps, menu images). Unpack the assets bundle"
            echo "(krunknative-assets-<version>.tar.gz, linked from the release notes) over assets/ here."
        fi
        echo
        echo "Settings and logins are stored in ~/.config/krunknative (Windows: %USERPROFILE%\\.config\\krunknative)."
        echo "server_bin$suffix is the dedicated server (./server_bin$suffix --help); public servers"
        echo "should use the server packages: https://github.com/rebel10n/KrunkNative/releases/tag/server-latest"
        echo
        echo "Command line: $client --help. Releases: https://github.com/rebel10n/KrunkNative/releases"
        echo "kn-install.json marks this directory as a Krunker Box Office install for the updater."
    } > "$file"
}

sums() {
    (cd "$1" && find . -type f ! -name SHA256SUMS -print0 | LC_ALL=C sort -z | xargs -0 "${sha256_cmd[@]}" | sed 's|  \./|  |') > "$work/SHA256SUMS"
    mv "$work/SHA256SUMS" "$1/SHA256SUMS"
}

# ---------------------------------------------------------------- binaries
# put_bins <dir>: client, server, launcher and bundled libraries
put_bins() {
    local dir="$1"
    mkdir -p "$dir"
    install -m 0755 "$bindir/$client" "$bindir/$server" "$dir/"
    [ -n "$launcher" ] && install -m 0755 "$launcher" "$dir/$launcher_name"
    case "$os" in
        windows)
            find "$bindir" -maxdepth 1 -type f -iname '*.dll' -exec cp {} "$dir/" \;
            ;;
        macos)
            for lib in libEGL.dylib libGLESv2.dylib; do
                [ -f "$bindir/$lib" ] || { echo "no $lib in $bindir (ANGLE build?)" >&2; exit 1; }
                install -m 0644 "$bindir/$lib" "$dir/"
            done
            ;;
    esac
    return 0
}

# macOS: copy every dylib outside /usr/lib, /System and @rpath (Homebrew OpenSSL, ...) next to
# the binaries and point the load commands at @executable_path (package-server.sh does the same).
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

archive() { # archive <parent dir> <top entry> <ext>
    local parent="$1" top="$2" ext="$3" file="$out/$name.$3"
    rm -f "$file"
    case "$ext" in
        tar.gz) (cd "$parent" && "$tar_bin" ${tar_flags[@]+"${tar_flags[@]}"} -cf - "$top" | gzip -n -9 > "$file") ;;
        zip) (cd "$parent" && zip -qr9 -y "$file" "$top") ;;
    esac
    (cd "$out" && sha256 "$(basename "$file")" > "$(basename "$file").sha256")
    echo "packaged $file ($(du -h "$file" | awk '{ print $1 }'))" >&2
}

# ---------------------------------------------------------------- linux / windows archives
tree_formats=""
for f in $formats; do case "$f" in tar.gz|zip) tree_formats+="$f " ;; esac; done

if [ "$os" != macos ] && [ -n "$tree_formats" ]; then
    stage="$work/tree/$name"
    put_bins "$stage"
    marker=tar.gz
    [ "$os" = windows ] && marker=zip
    fill_root "$stage" "$marker"
    sums "$stage"
    for f in $tree_formats; do archive "$work/tree" "$name" "$f"; done
fi

# ---------------------------------------------------------------- macOS .app
if [ "$os" = macos ]; then
    app="$work/app/KrunkNative.app"
    put_bins "$app/Contents/MacOS"
    for f in "$app/Contents/MacOS/$client" "$app/Contents/MacOS/$server" ${launcher:+"$app/Contents/MacOS/$launcher_name"}; do
        bundle_dylibs "$app/Contents/MacOS" "$f"
    done
    fill_root "$app/Contents/Resources" app
    if command -v sips > /dev/null && command -v iconutil > /dev/null; then
        iconset="$work/krunknative.iconset"
        mkdir -p "$iconset"
        # krunker.io's 1024 px app icon (tools/branding/fetch_branding.py; the iOS app's too)
        icon_src="$repo/platform/ios/KrunkNative/Assets.xcassets/AppIcon.appiconset/icon-1024.png"
        for s in 16 32 128 256 512; do
            sips -z "$s" "$s" "$icon_src" --out "$iconset/icon_${s}x${s}.png" > /dev/null
            d=$((s * 2)); sips -z "$d" "$d" "$icon_src" --out "$iconset/icon_${s}x${s}@2x.png" > /dev/null
        done
        iconutil -c icns "$iconset" -o "$app/Contents/Resources/krunknative.icns"
    fi
    minos="$(otool -l "$app/Contents/MacOS/$client" | awk '$1 == "minos" { print $2; exit }')"
    short="$(echo "$describe" | sed -n 's/^[a-z-]*v\([0-9][0-9.]*\).*/\1/p')"
    cat > "$app/Contents/Info.plist" << EOF
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0">
<dict>
  <key>CFBundleName</key><string>Krunker Box Office</string>
  <key>CFBundleDisplayName</key><string>Krunker Box Office</string>
  <key>CFBundleIdentifier</key><string>io.krunk.krunknative</string>
  <key>CFBundleExecutable</key><string>$entry</string>
  <key>CFBundleIconFile</key><string>krunknative</string>
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
EOF
    sums "$app/Contents/Resources"
    # ad-hoc signatures (not notarized): the nested code first (dylibs, the other executables),
    # then the bundle, which signs its CFBundleExecutable
    if command -v codesign > /dev/null; then
        for f in "$app"/Contents/MacOS/*; do [ "$(basename "$f")" = "$entry" ] || codesign --force --sign - "$f"; done
        codesign --force --sign - "$app"
        codesign --verify --deep --strict "$app"
    fi
    for f in $formats; do archive "$work/app" KrunkNative.app "$f"; done
    if command -v otool > /dev/null; then echo "== otool -L $client" >&2; otool -L "$app/Contents/MacOS/$client" >&2; fi
fi

# ---------------------------------------------------------------- deb / rpm (/opt/krunknative)
pkg_root() { # pkg_root <dir> <deb|rpm>
    local root="$1" package="$2"
    put_bins "$root/opt/krunknative"
    fill_root "$root/opt/krunknative" "$package"
    sums "$root/opt/krunknative"
    mkdir -p "$root/usr/bin" "$root/usr/share/applications" "$root/usr/share/icons/hicolor/256x256/apps"
    ln -s "/opt/krunknative/$entry" "$root/usr/bin/krunknative"
    cp "$repo/assets/img/icon.png" "$root/usr/share/icons/hicolor/256x256/apps/krunknative.png"
    cat > "$root/usr/share/applications/krunknative.desktop" << EOF
[Desktop Entry]
Type=Application
Name=Krunker Box Office
GenericName=First-person shooter
Comment=Krunker, native (formerly KrunkNative)
Exec=$( [ -n "$launcher" ] && echo "krunknative %U" || echo "/opt/krunknative/$entry" )
Icon=krunknative
Terminal=false
Categories=Game;ActionGame;
Keywords=krunker;fps;shooter;krunknative;box office;
EOF
}

# version strings: describe v1.0.0-134-g1b5b5ef -> base 1.0.0, rest 134.g1b5b5ef
pkg_base="$(echo "$describe" | sed -n 's/^[a-z-]*v\([0-9][0-9.]*\).*/\1/p')"
pkg_rest="$(echo "$describe" | sed -n 's/^[a-z-]*v[0-9][0-9.]*-\([0-9]*\)-\(g[0-9a-f]*\)$/\1.\2/p')"
[ -n "$pkg_base" ] || { pkg_base=0; pkg_rest="$(echo "$describe" | tr -c 'A-Za-z0-9.\n' .)"; }
# deb Depends: libc6 >= the newest GLIBC_ symbol version the binaries ask for
glibc_min=""
if [ "$os" = linux ] && command -v readelf > /dev/null; then
    glibc_min="$(readelf -V "$bindir/$client" "$bindir/$server" ${launcher:+"$launcher"} | grep -o 'GLIBC_[0-9.]*' | sed 's/GLIBC_//' | sort -uV | tail -1)"
fi
summary="Native C port of the Krunker first-person shooter"
description="Krunker Box Office client ($describe, protocol ${KN_PROTOCOL:-?}): the game, its HTML menu and the dedicated server. The game content (models, textures, sounds, about 1.7 GB) is not in this package: the first start downloads it."

for f in $formats; do
    case "$f" in
        deb)
            root="$work/deb"
            pkg_root "$root" deb
            darch="$arch"; [ "$arch" = aarch64 ] && darch=arm64
            depends="libc6 (>= ${glibc_min:-2.17}), libegl1 | libegl1-mesa"
            case "$platform" in *-polyfilled) ;; *) depends+=", libssl3 | libssl3t64" ;; esac
            mkdir -p "$root/DEBIAN"
            cat > "$root/DEBIAN/control" << EOF
Package: krunknative
Version: $pkg_base${pkg_rest:++$pkg_rest}
Architecture: $darch
Maintainer: Krunker Box Office <noreply@krunk.io>
Installed-Size: $(du -sk "$root/opt" | cut -f1)
Depends: $depends
Recommends: libgl1, libopus0, libasound2 | libasound2t64 | libpulse0
Section: games
Priority: optional
Homepage: https://github.com/rebel10n/KrunkNative
Description: $summary
 $description
EOF
            dpkg-deb --root-owner-group -Zxz --build "$root" "$out/$name.deb" > /dev/null
            (cd "$out" && sha256 "$name.deb" > "$name.deb.sha256")
            echo "packaged $out/$name.deb ($(du -h "$out/$name.deb" | awk '{ print $1 }'))" >&2
            ;;
        rpm)
            root="$work/rpm-root"
            pkg_root "$root" rpm
            top="$work/rpmbuild"
            mkdir -p "$top"/{BUILD,RPMS,SOURCES,SPECS,SRPMS}
            # prebuilt files: no strip / debuginfo / build-id links; gzip payload so rpm 4.11
            # (CentOS 7) reads the polyfilled package. Requires come from rpm's ELF scan.
            cat > "$top/SPECS/krunknative.spec" << EOF
%global debug_package %{nil}
%global __os_install_post %{nil}
%define _build_id_links none
%define _binary_payload w9.gzdio
Name: krunknative
Version: $pkg_base
Release: ${pkg_rest:-1}
Summary: $summary
License: Proprietary
URL: https://github.com/rebel10n/KrunkNative
Recommends: opus
%description
$description
%install
cp -a "$root/." %{buildroot}/
%files
/opt/krunknative
/usr/bin/krunknative
/usr/share/applications/krunknative.desktop
/usr/share/icons/hicolor/256x256/apps/krunknative.png
EOF
            rarch="$arch"; [ "$arch" = amd64 ] && rarch=x86_64
            rpmbuild --quiet -bb --target "$rarch-linux" --define "_topdir $top" --define "_rpmfilename $name.rpm" \
                --buildroot "$work/rpm-buildroot" "$top/SPECS/krunknative.spec" > "$work/rpmbuild.log" 2>&1 \
                || { cat "$work/rpmbuild.log" >&2; exit 1; }
            mv "$top/RPMS/$name.rpm" "$out/$name.rpm"
            (cd "$out" && sha256 "$name.rpm" > "$name.rpm.sha256")
            echo "packaged $out/$name.rpm ($(du -h "$out/$name.rpm" | awk '{ print $1 }'))" >&2
            ;;
    esac
done
