#!/bin/bash
# Stage the web build (dist/web) for GitHub Pages into KN_GH_PAGES_STAGE (default
# dist/gh-pages): what CI uploads with actions/upload-pages-artifact and publishes with
# actions/deploy-pages (deploy-kru.yml job `pages`, pages.yml). Nothing is deployed from here;
# `make pages-github` runs it on a local `make web` for a look (deploy/pages/README.md).
# Settings: site.sh, plus KN_PAGES_CNAME (the custom domain, CNAME) and KN_PAGES_LIVE_URL
# (the live site whose client files the new deploy keeps for cached pages; empty = none).
set -euo pipefail
cd "$(dirname "$0")/../.."
. deploy/pages/site.sh

: "${KN_GH_PAGES_STAGE:=dist/gh-pages}"
: "${KN_PAGES_CNAME:=krunk.io}"
KN_PAGES_LIVE_URL="${KN_PAGES_LIVE_URL-https://krunk.io/}"
DIST="${1:-dist/web}"

mapfile -d '' site < <(kn_site_args)
python3 deploy/pages/stage.py "$DIST" "$KN_GH_PAGES_STAGE" --target github \
    --cname "$KN_PAGES_CNAME" --keep-from "$KN_PAGES_LIVE_URL" "${site[@]}"
