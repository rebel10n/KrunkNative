# shellcheck shell=bash disable=SC2034  # sourced: its callers use the variables
# Sourced by deploy/pages/{deploy,github,assets-cdn}.sh: the published site's settings, one
# place for their defaults (deploy/pages/README.md). Each one can be overridden from the
# environment; CI passes the repository variables of the same name (empty = this default).

# Game content origins (README "Asset origins"). The page loads assets from KN_ASSET_ORIGIN and
# retries a failed download on KN_ASSET_FALLBACK; both are names of the origins below, written
# to kn-config.json next to index.html, so switching is a variable + a page redeploy
# (`gh workflow run pages.yml`), never a client build. An origin with an empty URL is left out.
: "${KN_ASSET_OWN_URL:=${KN_ASSET_BASE:-https://assets.krunk.io/}}"   # "own": kru's OpenResty (later abulxt's edges), the native updates host
: "${KN_ASSET_CDN_URL:=}"                           # "cdn": the Cloudflare Pages asset project, https://<project>.pages.dev/
: "${KN_ASSET_ORIGIN:=own}"                         # own | cdn
KN_ASSET_FALLBACK="${KN_ASSET_FALLBACK-}"           # own | cdn | "" (none)
KN_ASSET_ORIGINS="own=$KN_ASSET_OWN_URL,cdn=$KN_ASSET_CDN_URL"

: "${KN_BRAIN_URL:=https://coordinator.krunk.io}"
: "${KN_BRIDGE_URL:=wss://play.krunk.io/native}"  # net-bridge on kru (deploy/ansible role kn_bridge)
: "${KN_RTC_URL:=https://play.krunk.io/rtc}"     # WebRTC gateway on kru (deploy/ansible role kn_rtc, net_rtc.c); off = none
: "${KN_WT_URL:=off}"                             # no WebTransport relay deployed yet (net_wt.c)
KN_GA_ID="${KN_GA_ID-G-6EGYKRF020}"              # Google Analytics (deploy/pages/ga.py); set empty or off to disable

# The stage.py arguments every target shares.
kn_site_args() {
    printf '%s\0' --brain "$KN_BRAIN_URL" --bridge "$KN_BRIDGE_URL" --rtc "$KN_RTC_URL" --wt "$KN_WT_URL" \
        --ga-id "$KN_GA_ID" --asset-origins "$KN_ASSET_ORIGINS" --asset-origin "$KN_ASSET_ORIGIN" \
        --asset-fallback "$KN_ASSET_FALLBACK"
}

# Cloudflare credentials: CLOUDFLARE_API_TOKEN / CLOUDFLARE_ACCOUNT_ID (CI secrets), else
# CF_API_TOKEN / CF_ACCOUNT_ID from ~/.config/kn-secrets/cloudflare.env.
kn_cloudflare_credentials() {
    if [ -z "${CLOUDFLARE_API_TOKEN:-}" ] && [ -f ~/.config/kn-secrets/cloudflare.env ]; then
        set -a
        # shellcheck disable=SC1090
        . ~/.config/kn-secrets/cloudflare.env
        set +a
    fi
    export CLOUDFLARE_API_TOKEN="${CLOUDFLARE_API_TOKEN:-${CF_API_TOKEN:-}}"
    export CLOUDFLARE_ACCOUNT_ID="${CLOUDFLARE_ACCOUNT_ID:-${CF_ACCOUNT_ID:-}}"
    [ -n "$CLOUDFLARE_API_TOKEN" ] && [ -n "$CLOUDFLARE_ACCOUNT_ID" ] || { echo "${0##*/}: no Cloudflare credentials" >&2; exit 1; }
}

WRANGLER_VERSION=4.149.0                          # pinned; bump deliberately
