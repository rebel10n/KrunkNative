#!/usr/bin/env bash
# Packaging test for the Windows zip's captcha loader (tools/release/package-client.sh webview2_loader;
# doc/v10/official-captcha.md): a client.exe with the WebView2 backend needs WebView2Loader.dll next to it.
#   1. a client.exe without the backend: the zip has no WebView2Loader.dll;
#   2. a client.exe with it and the SDK staged in KN_WEBVIEW2_DIR: runtime/WebView2Loader.dll is in the zip,
#      byte for byte the staged one, in SHA256SUMS, and LICENSE-NOTICE.txt and README.txt name it;
#   3. the DLL in <bin dir> (the CI's win-bin) is taken first;
#   4. with the backend and no DLL anywhere the packaging fails;
#   5. a DLL whose sha256 is not the pinned one (tools/webview2/webview2.lock) is refused.
# The binaries are stand-ins (the backend is known by the CreateCoreWebView2EnvironmentWithOptions string the real
# client looks up); the stand-in DLL is pinned through KN_WEBVIEW2_LOCK. No compiler, no download, no wine.
#
#   tools/release/test-package-windows.sh        (from anywhere; needs zip and unzip)
# Prints PASS/FAIL lines; exit 0 only when everything passed.
set -uo pipefail

repo="$(cd "$(dirname "$0")/../.." && pwd)"
pkg="$repo/tools/release/package-client.sh"
work="$(mktemp -d "${TMPDIR:-/tmp}/kn-pkg-win.XXXXXX")"
trap 'rm -rf "$work"' EXIT
fails=0

check() { # check <name> <ok 0/1> [detail]
    [ "$2" = 0 ] || fails=$((fails + 1))
    if [ "$2" = 0 ]; then printf 'PASS  %s\n' "$1"; else printf 'FAIL  %s%s\n' "$1" "${3:+  -- $3}"; fi
}
for t in zip unzip; do command -v "$t" > /dev/null || { echo "no $t: skipped"; exit 77; }; done

sha() { sha256sum "$1" | awk '{ print $1 }'; }

# bindir <dir> <with backend 0/1>: stand-in client.exe, server.exe and BoxOffice.exe
bindir() {
    mkdir -p "$1"
    { printf 'MZ stand-in client\n'; [ "$2" = 1 ] && printf 'CreateCoreWebView2EnvironmentWithOptions\0'; } > "$1/client.exe"
    printf 'MZ stand-in server\n' > "$1/server.exe"
    printf 'MZ stand-in launcher\n' > "$1/BoxOffice.exe"
}

sdk="$work/sdk"
mkdir -p "$sdk/x64"
printf 'stand-in WebView2Loader %s\n' "$$" > "$sdk/x64/WebView2Loader.dll"
lock="$work/webview2.lock"
awk -F '\t' -v OFS='\t' -v d="$(sha "$sdk/x64/WebView2Loader.dll")" \
    '$1 == "x64/WebView2Loader.dll" { $3 = d } { print }' "$repo/tools/webview2/webview2.lock" > "$lock"

# package <name> <bin dir> [env ...]: the zip unpacked in $work/<name>/t; status in $work/<name>/rc
package() {
    local name="$1" bin="$2"
    shift 2
    mkdir -p "$work/$name"
    env "$@" "$pkg" windows-x64 "$bin" "$work/$name/out" > "$work/$name/log" 2>&1
    echo $? > "$work/$name/rc"
    if [ -f "$work/$name/out/BoxOffice-windows-x64.zip" ]; then
        mkdir -p "$work/$name/t"
        unzip -q "$work/$name/out/BoxOffice-windows-x64.zip" -d "$work/$name/t"
        unzip -Z1 "$work/$name/out/BoxOffice-windows-x64.zip" > "$work/$name/list"
    fi
}

# 1. no backend: no loader
bindir "$work/plain" 0
package plain "$work/plain" KN_WEBVIEW2_DIR="$sdk" KN_WEBVIEW2_LOCK="$lock"
check "a client without the WebView2 backend packages" "$(cat "$work/plain/rc")" "$(tail -3 "$work/plain/log")"
grep -qx 'runtime/WebView2Loader.dll' "$work/plain/list" 2> /dev/null
check "a client without the backend gets no WebView2Loader.dll" "$([ $? = 1 ] && echo 0 || echo 1)"

# 2. the backend, the SDK staged: the loader next to client.exe
bindir "$work/wv2" 1
package wv2 "$work/wv2" KN_WEBVIEW2_DIR="$sdk" KN_WEBVIEW2_LOCK="$lock"
check "a client with the WebView2 backend packages" "$(cat "$work/wv2/rc")" "$(tail -3 "$work/wv2/log")"
grep -qx 'runtime/WebView2Loader.dll' "$work/wv2/list" 2> /dev/null && grep -qx 'runtime/client.exe' "$work/wv2/list"
check "the zip has runtime/WebView2Loader.dll next to runtime/client.exe" "$?"
cmp -s "$sdk/x64/WebView2Loader.dll" "$work/wv2/t/runtime/WebView2Loader.dll"
check "the packaged loader is the staged SDK's file" "$?"
grep -q "  runtime/WebView2Loader.dll\$" "$work/wv2/t/SHA256SUMS" 2> /dev/null && (cd "$work/wv2/t" && sha256sum --quiet -c SHA256SUMS > /dev/null)
check "SHA256SUMS lists the loader and every sum matches" "$?"
grep -q 'Microsoft.Web.WebView2 1\.0\.' "$work/wv2/t/LICENSE-NOTICE.txt" 2> /dev/null && grep -q 'WebView2Loader.dll' "$work/wv2/t/README.txt"
check "LICENSE-NOTICE.txt names the WebView2 package and README.txt the file" "$?"

# 3. the bin dir's own copy first (a stale SDK in KN_WEBVIEW2_DIR is not read)
bindir "$work/ownbin" 1
cp "$sdk/x64/WebView2Loader.dll" "$work/ownbin/"
package ownbin "$work/ownbin" KN_WEBVIEW2_DIR="$work/nowhere" KN_WEBVIEW2_LOCK="$lock"
cmp -s "$work/ownbin/WebView2Loader.dll" "$work/ownbin/t/runtime/WebView2Loader.dll" 2> /dev/null
check "the bin dir's WebView2Loader.dll is shipped" "$?" "$(tail -3 "$work/ownbin/log")"

# 4. the backend and no loader: refused
bindir "$work/missing" 1
package missing "$work/missing" KN_WEBVIEW2_DIR="$work/nowhere" KN_WEBVIEW2_LOCK="$lock"
[ "$(cat "$work/missing/rc")" != 0 ] && grep -q 'no WebView2Loader.dll' "$work/missing/log"
check "no loader for a WebView2 client fails the packaging" "$?" "$(tail -3 "$work/missing/log")"

# 5. a loader that is not the pinned one: refused (the real lock pins Microsoft's 1.0.4258.31 x64 loader)
bindir "$work/foreign" 1
package foreign "$work/foreign" KN_WEBVIEW2_DIR="$sdk"
[ "$(cat "$work/foreign/rc")" != 0 ] && grep -q 'is not the pinned' "$work/foreign/log"
check "a loader with another sha256 than webview2.lock's is refused" "$?" "$(tail -3 "$work/foreign/log")"

echo "test-package-windows: $([ "$fails" = 0 ] && echo OK || echo "$fails failure(s)")"
[ "$fails" = 0 ]
