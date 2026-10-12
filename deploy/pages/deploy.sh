#!/bin/bash
# Deploy the web build (dist/web) to the Cloudflare Pages project behind krunk.io
# (deploy/pages/README.md). Needs `make web` first and CLOUDFLARE_API_TOKEN +
# CLOUDFLARE_ACCOUNT_ID (or CF_API_TOKEN / CF_ACCOUNT_ID from ~/.config/kn-secrets/cloudflare.env).
set -euo pipefail
cd "$(dirname "$0")/../.."

: "${KN_PAGES_PROJECT:=krunknative}"
: "${KN_PAGES_BRANCH:=slop}"                      # the project's production branch
: "${KN_ASSET_BASE:=https://assets.krunk.io/}"
: "${KN_BRAIN_URL:=https://coordinator.krunk.io}"
: "${KN_BRIDGE_URL:=wss://play.krunk.io/native}"  # net-bridge on kru (deploy/ansible role kn_bridge)
: "${KN_WT_URL:=off}"                             # no WebTransport relay deployed yet (net_wt.c)
: "${KN_PAGES_STAGE:=dist/pages}"
KN_GA_ID="${KN_GA_ID-G-6EGYKRF020}"              # Google Analytics (deploy/pages/ga.py); set empty or off to disable
WRANGLER_VERSION=4.149.0                          # pinned; bump deliberately
: "${KN_PURGE_ZONE:=krunk.io}"                      # zone whose edge cache is purged after the deploy
: "${KN_PURGE_HOSTS:=krunk.io www.krunk.io}"        # hosts purged (assets.krunk.io is versioned, kept); KN_PURGE_HOSTS= skips

if [ -z "${CLOUDFLARE_API_TOKEN:-}" ] && [ -f ~/.config/kn-secrets/cloudflare.env ]; then
    set -a; . ~/.config/kn-secrets/cloudflare.env; set +a
fi
export CLOUDFLARE_API_TOKEN="${CLOUDFLARE_API_TOKEN:-${CF_API_TOKEN:-}}"
export CLOUDFLARE_ACCOUNT_ID="${CLOUDFLARE_ACCOUNT_ID:-${CF_ACCOUNT_ID:-}}"
[ -n "$CLOUDFLARE_API_TOKEN" ] && [ -n "$CLOUDFLARE_ACCOUNT_ID" ] || { echo "deploy.sh: no Cloudflare credentials" >&2; exit 1; }

python3 deploy/pages/stage.py dist/web "$KN_PAGES_STAGE" --asset-base "$KN_ASSET_BASE" --brain "$KN_BRAIN_URL" --bridge "$KN_BRIDGE_URL" --wt "$KN_WT_URL" --ga-id "$KN_GA_ID"
sha=$(git rev-parse --short=12 HEAD 2>/dev/null || echo local)
npx --yes "wrangler@$WRANGLER_VERSION" pages deploy "$KN_PAGES_STAGE" \
    --project-name "$KN_PAGES_PROJECT" --branch "$KN_PAGES_BRANCH" \
    --commit-hash "$(git rev-parse HEAD 2>/dev/null || echo 0)" --commit-message "KrunkNative $sha" --commit-dirty=true

# Purge Cloudflare's edge cache for the page hosts, so every client gets the new build on its
# next load (the hashed client files and ?v= assets never need it). The token needs Zone Read
# + Cache Purge on the zone (the CI token has both).
if [ -n "$KN_PURGE_HOSTS" ]; then
    cf() { curl -fsS -H "Authorization: Bearer $CLOUDFLARE_API_TOKEN" -H 'Content-Type: application/json' "$@"; }
    zone=$(cf "https://api.cloudflare.com/client/v4/zones?name=$KN_PURGE_ZONE" | python3 -c 'import json,sys; r=json.load(sys.stdin)["result"]; print(r[0]["id"] if r else "")')
    [ -n "$zone" ] || { echo "deploy.sh: zone $KN_PURGE_ZONE not found for the purge" >&2; exit 1; }
    hosts=$(python3 -c 'import json,sys; print(json.dumps({"hosts": sys.argv[1:]}))' $KN_PURGE_HOSTS)
    cf -X POST -d "$hosts" "https://api.cloudflare.com/client/v4/zones/$zone/purge_cache" \
        | python3 -c 'import json,sys; r=json.load(sys.stdin); sys.exit(0 if r.get("success") else 1)' \
        || { echo "deploy.sh: cache purge failed" >&2; exit 1; }
    echo "deploy.sh: purged the Cloudflare cache for $KN_PURGE_HOSTS"
fi
