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
