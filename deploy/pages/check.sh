#!/bin/bash
# Check a published front end and its asset origins over HTTP (deploy/pages/README.md "Cutover"):
#   deploy/pages/check.sh <site URL> [curl args...]
#     e.g. deploy/pages/check.sh https://rebel10n.github.io/KrunkNative/
#          deploy/pages/check.sh https://krunk.io/ --resolve krunk.io:443:185.199.108.153 -k
#   deploy/pages/check.sh --assets <asset origin URL>     e.g. https://krunknative-assets-a.pages.dev/
# Site: the page, its kn-config.json, the hashed client files (wasm MIME), 404s; then every asset
# origin kn-config.json names. Asset origin: version.txt (no-store), CORS for a page origin, a
# byte range (206), the caching of a ?v= URL. Exits non-zero on the first hard failure.
set -uo pipefail
fail=0
ok() { printf '  ok    %s\n' "$*"; }
bad() { printf '  FAIL  %s\n' "$*"; fail=1; }
warn() { printf '  warn  %s\n' "$*"; }

assets() {
    local base=$1; shift
    echo "asset origin $base"
    local v
    v=$(curl -fsS "$@" "${base}assets/version.txt" | tr -d '[:space:]') || { bad "${base}assets/version.txt"; return; }
    [[ $v =~ ^[0-9A-Za-z._-]{1,64}$ ]] && ok "version.txt $v" || bad "version.txt: '$v'"
    local h
    h=$(curl -fsS -o /dev/null -D - "$@" "${base}assets/version.txt") || true
    grep -qi '^cache-control:.*no-store' <<< "$h" && ok "version.txt no-store" || warn "version.txt Cache-Control: $(grep -i '^cache-control' <<< "$h" | tr -d '\r')"
    h=$(curl -fsS -o /dev/null -D - "$@" -H 'Origin: https://krunk.io' -H 'Range: bytes=0-15' "${base}assets/img/icon.png?v=$v") \
        || { bad "assets/img/icon.png"; return; }
    grep -qiE '^access-control-allow-origin: (\*|https://krunk\.io)' <<< "$h" && ok "CORS" || bad "no Access-Control-Allow-Origin for https://krunk.io"
    grep -qE '^HTTP/[0-9.]+ 206' <<< "$h" && ok "byte range: 206 $(grep -i '^content-range' <<< "$h" | tr -d '\r' | cut -d' ' -f2-)" \
        || warn "byte range: $(head -1 <<< "$h" | tr -d '\r') (the client never sends one; fine)"
    ok "?v= caching: $(grep -i '^cache-control' <<< "$h" | tr -d '\r' | cut -d' ' -f2-)"
    local code
    code=$(curl -s -o /dev/null -w '%{http_code}' "$@" "${base}assets/does/not/exist.png")
    [ "$code" = 404 ] && ok "missing file: 404" || bad "missing file: $code"
}

if [ "${1:-}" = --assets ]; then
    shift
    base=${1:?asset origin URL}; shift
    assets "${base%/}/" "$@"
    exit $fail
fi

site=${1:?site URL}; shift
site="${site%/}/"
echo "site $site"
page=$(curl -fsS "$@" "$site") || { bad "$site"; exit 1; }
ok "index.html $(printf '%s' "$page" | wc -c) bytes"
build=$(grep -o '<meta name="kn-build" content="[0-9a-f]*"' <<< "$page" | sed 's/.*content="//; s/"$//')
[ -n "$build" ] && ok "kn-build $build" || bad "no kn-build meta"
for ext in js wasm data; do
    h=$(curl -fsS -o /dev/null -D - "$@" "${site}client.$build.$ext") || { bad "client.$build.$ext"; continue; }
    ok "client.$build.$ext $(grep -i '^content-type' <<< "$h" | tr -d '\r' | cut -d' ' -f2-), $(grep -i '^cache-control' <<< "$h" | tr -d '\r' | cut -d' ' -f2-)"
    [ $ext = wasm ] && { grep -qi '^content-type: application/wasm' <<< "$h" || bad "wasm is not application/wasm (no streaming compile)"; }
done
for f in js/loader.js manifest.webmanifest img/pwa/icon-192.png; do
    code=$(curl -s -o /dev/null -w '%{http_code}' "$@" "$site$f")
    [ "$code" = 200 ] && ok "$f" || bad "$f: $code"
done
code=$(curl -s -o /dev/null -w '%{http_code}' "$@" "${site}no-such-page-$$")
[ "$code" = 404 ] && ok "unknown path: 404" || bad "unknown path: $code"
cfg=$(curl -fsS "$@" "${site}kn-config.json") || { bad "kn-config.json"; exit 1; }
python3 -c 'import json,sys; c=json.loads(sys.argv[1]); print("  ok    kn-config.json: origin %s, fallback %s" % (c["asset_origin"], c["asset_fallback"] or "none"))' "$cfg" \
    || bad "kn-config.json is not the expected JSON"
for base in $(python3 -c 'import json,sys; print(" ".join(json.loads(sys.argv[1])["asset_origins"].values()))' "$cfg" 2>/dev/null); do
    assets "$base"
done
exit $fail
