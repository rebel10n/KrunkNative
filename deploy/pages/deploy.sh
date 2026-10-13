#!/bin/bash
# Deploy the web build (dist/web) to the Cloudflare Pages project that served krunk.io before
# GitHub Pages (deploy/pages/README.md "GitHub Pages"). Kept while the DNS still points at it and
# as the way back; CI runs it unless the repository variable KN_CF_FRONTEND is 0.
# Needs `make web` first and CLOUDFLARE_API_TOKEN + CLOUDFLARE_ACCOUNT_ID (or CF_API_TOKEN /
# CF_ACCOUNT_ID from ~/.config/kn-secrets/cloudflare.env). Settings: site.sh.
set -euo pipefail
cd "$(dirname "$0")/../.."
. deploy/pages/site.sh

: "${KN_PAGES_PROJECT:=krunknative}"
: "${KN_PAGES_BRANCH:=slop}"                      # the project's production branch
: "${KN_PAGES_STAGE:=dist/pages}"
: "${KN_PURGE_ZONE:=krunk.io}"                      # zone whose edge cache is purged after the deploy
: "${KN_PURGE_HOSTS:=krunk.io www.krunk.io}"        # hosts purged (assets.krunk.io is versioned, kept); KN_PURGE_HOSTS= skips

kn_cloudflare_credentials

mapfile -d '' site < <(kn_site_args)
python3 deploy/pages/stage.py dist/web "$KN_PAGES_STAGE" --target cloudflare "${site[@]}"
sha=$(git rev-parse --short=12 HEAD 2>/dev/null || echo local)
npx --yes "wrangler@$WRANGLER_VERSION" pages deploy "$KN_PAGES_STAGE" \
    --project-name "$KN_PAGES_PROJECT" --branch "$KN_PAGES_BRANCH" \
    --commit-hash "$(git rev-parse HEAD 2>/dev/null || echo 0)" --commit-message "KrunkNative $sha" --commit-dirty=true

# Purge Cloudflare's edge cache for the page hosts, so every client gets the new build on its
# next load (the hashed client files and ?v= assets never need it). The token needs Zone Read
# + Cache Purge on the zone (the CI token has both). Harmless once the hosts are DNS only
# (GitHub Pages): nothing of them is in Cloudflare's cache then.
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
