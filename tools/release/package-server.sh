#!/usr/bin/env bash
# Packages a dedicated-server build as a release asset (.github/workflows/release.yml):
#   <out>/krunknative-server-<platform>.tar.gz   (windows-*: .zip)
#   <out>/krunknative-server-<platform>.tar.gz.sha256   (sha256sum format)
# Layout (the `make server-dist` bundle): server_bin, assets/maps + assets/skins.json,
# cfg/server.cfg (from the example) + server.cfg.example, kn-query.py, README.txt,
# LICENSE-NOTICE.txt + licenses/, SHA256SUMS, and runtime libraries that are not part of the OS:
#   windows-*  the MinGW DLLs passed with --libs <dir>
#   macos-*    non-system dylibs (Homebrew libcrypto) copied next to server_bin and relinked
#              to @executable_path, then ad-hoc re-signed
#
# Usage: tools/release/package-server.sh <platform> <server binary> <out dir> [--libs <dir>]
# Env (README text, from tools/release/version-meta.sh): KN_DESCRIBE KN_COMMIT KN_BUILD_TIME KN_PROTOCOL
set -euo pipefail

[ $# -ge 3 ] || { sed -n '2,14p' "$0" >&2; exit 2; }
platform="$1" bin="$2" out="$3"
shift 3
libs=""
while [ $# -gt 0 ]; do
    case "$1" in --libs) libs="$2"; shift 2 ;; *) echo "unknown option $1" >&2; exit 2 ;; esac
done
case "$platform" in linux-*|windows-*|macos-*) ;; *) echo "unknown platform $platform" >&2; exit 2 ;; esac
[ -f "$bin" ] || { echo "no server binary at $bin" >&2; exit 1; }

repo="$(cd "$(dirname "$0")/../.." && pwd)"
name="krunknative-server-$platform"
mkdir -p "$out"
out="$(cd "$out" && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT
stage="$work/$name"
mkdir -p "$stage/assets" "$stage/cfg" "$stage/licenses"

exe="server_bin"
case "$platform" in windows-*) exe="server_bin.exe" ;; esac
cp "$bin" "$stage/$exe"
chmod 0755 "$stage/$exe"
cp -R "$repo/assets/maps" "$stage/assets/"
[ -f "$repo/assets/skins.json" ] && cp "$repo/assets/skins.json" "$stage/assets/"
cp "$repo/config/server.cfg.example" "$stage/server.cfg.example"
cp "$repo/config/server.cfg.example" "$stage/cfg/server.cfg"
cp "$repo/services/scripts/kn-query.py" "$stage/"

if [ -n "$libs" ]; then
    find "$libs" -maxdepth 1 -type f -iname '*.dll' -exec cp {} "$stage/" \;
fi

sha256() { if command -v sha256sum > /dev/null; then sha256sum "$@"; else shasum -a 256 "$@"; fi; }

# macOS: bundle every dylib outside /usr/lib and /System (recursively) and point the
# load commands at @executable_path, so the server runs without Homebrew.
if [[ "$platform" == macos-* ]]; then
    bundle_dylibs() {
        local target="$1" dep base
        while read -r dep; do
            base="$(basename "$dep")"
            if [ ! -f "$stage/$base" ]; then
                cp "$dep" "$stage/$base"
                chmod 0644 "$stage/$base"
                install_name_tool -id "@executable_path/$base" "$stage/$base"
                bundle_dylibs "$stage/$base"
            fi
            install_name_tool -change "$dep" "@executable_path/$base" "$target"
        done < <(otool -L "$target" | awk 'NR > 1 { print $1 }' | grep -Ev '^(/usr/lib/|/System/|@)' || true)
    }
    bundle_dylibs "$stage/$exe"
    for f in "$stage"/*.dylib "$stage/$exe"; do
        [ -f "$f" ] && codesign --force --sign - "$f"
    done
    echo "== otool -L $exe" >&2
    otool -L "$stage/$exe" >&2
fi

# Third-party notices for what is compiled in (shared/lib, server/lib) and bundled.
cp "$repo/server/lib/replxx/LICENSE.md" "$stage/licenses/replxx.md"
cp "$repo/shared/lib/pcg_basic/LICENSE.txt" "$stage/licenses/pcg_basic.txt"
sed -n '/^\/\*/,/\*\//p' "$repo/shared/lib/cJSON/cJSON.h" | sed -n '1,/\*\//p' > "$stage/licenses/cJSON.txt"

cat > "$stage/LICENSE-NOTICE.txt" << 'EOF'
Krunker Box Office dedicated server: license notice
====================================================

Krunker Box Office (formerly KrunkNative, https://github.com/rebel10n/KrunkNative) is a
native C port of Krunker. Its repository does not currently carry a license file: no license
to the Krunker Box Office source or binaries is granted by this package beyond what its
authors state in the repository.

Krunker, its maps and other game content are the property of their owners
(Yendis Entertainment / FRVR). The map files under assets/maps are included only so
this server can run the game; they are not covered by any Krunker Box Office license.

Third-party components compiled into server_bin, with their licenses in licenses/:
  cJSON      MIT                     licenses/cJSON.txt
  replxx     BSD 3-Clause            licenses/replxx.md
  pcg_basic  Apache License 2.0      licenses/pcg_basic.txt

Libraries loaded at run time are the operating system's (Linux: libcurl, OpenSSL,
libcrypt from the distribution). Windows packages bundle the MinGW-w64 runtime and
MSYS2 DLLs (libcurl, OpenSSL, zlib, ... each under its own license, see
https://packages.msys2.org); macOS packages may bundle Homebrew's OpenSSL libcrypto
(Apache License 2.0).
EOF

case "$platform" in
    linux-x86_64-bullseye) runtime="Debian 11 (glibc 2.31, OpenSSL 1.1): apt install libcurl4 libssl1.1 libcrypt1" ;;
    linux-*) runtime="Debian 12+/Ubuntu 22.04+ (glibc 2.36, OpenSSL 3): apt install libcurl4 libssl3 libcrypt1 (Debian 13: libcurl4t64 libssl3t64)" ;;
    windows-*) runtime="Windows 10+ x64: the DLLs next to server_bin.exe are all it needs" ;;
    macos-*)
        minos="$(otool -l "$stage/$exe" | awk '$1 == "minos" { print $2; exit }')"
        runtime="macOS ${minos:-?}+ on Apple silicon; the bundled dylibs are next to server_bin. First run: xattr -d com.apple.quarantine server_bin *.dylib" ;;
esac

cat > "$stage/README.txt" << EOF
Krunker Box Office dedicated server ($platform)
===============================================

Version:   ${KN_DESCRIBE:-unknown}
Commit:    ${KN_COMMIT:-unknown}
Built:     ${KN_BUILD_TIME:-unknown}
Protocol:  ${KN_PROTOCOL:-unknown} (clients and coordinators speak NET_PROTOCOL major.minor)
Runtime:   $runtime

Run
---
  1. Edit cfg/server.cfg (a copy of server.cfg.example): hostname, region, port,
     public_ip, mapcycle, mode. Every key is documented in server.cfg.example.
  2. Start it from this directory:  ./$exe        (./$exe --help, ./$exe --version)
  3. Open the game port (default 21015) for TCP and UDP. The server-info query
     answers on UDP of the same port:  python3 kn-query.py <host> 21015

To be listed in the Krunker Box Office server browser set finder_url in cfg/server.cfg
(https://matchmaker.krunk.io). Servers built from an old commit or speaking an older
protocol are shown as "Outdated": download the current build again from
https://github.com/rebel10n/KrunkNative/releases/tag/server-latest

Files
-----
  $exe                 the server
  cfg/server.cfg       your config (kept by upgrades: copy only the binary over)
  server.cfg.example   the documented defaults
  assets/maps          map catalogue the server loads
  kn-query.py          server-info query client (python3)
  SHA256SUMS           checksums of every file in this package
  LICENSE-NOTICE.txt   license notice; licenses/ third-party license texts

Docs: https://github.com/rebel10n/KrunkNative/blob/main/doc/backend.md
EOF

(cd "$stage" && find . -type f | LC_ALL=C sort | sed 's|^\./||' | while read -r f; do sha256 "$f"; done) > "$work/SHA256SUMS"
mv "$work/SHA256SUMS" "$stage/SHA256SUMS"

cd "$work"
case "$platform" in
    windows-*) archive="$name.zip"; rm -f "$out/$archive"; zip -qr9 "$out/$archive" "$name" ;;
    *) archive="$name.tar.gz"; tar -czf "$out/$archive" "$name" ;;
esac
(cd "$out" && sha256 "$archive" > "$archive.sha256")
echo "packaged $out/$archive" >&2
cat "$out/$archive.sha256" >&2
