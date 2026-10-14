#!/bin/bash
# Publish the game content to the Cloudflare Pages asset CDN (deploy/pages/README.md "Asset
# CDN"): mirror the own origin's tree (assets_cdn.py) and `wrangler pages deploy` it to the
# project KN_ASSET_CDN_PROJECT on its *.pages.dev URL (no custom domain). wrangler hashes
# every file and uploads only the ones Cloudflare does not have yet, so a deploy after a
# content change sends just the new files. CI: .github/workflows/assets-cdn.yml.
#
# Settings (site.sh): KN_ASSET_OWN_URL (the source), KN_ASSET_CDN_URL (the CDN, to skip an
# unchanged version), plus KN_ASSET_CDN_PROJECT (default krunknative-assets),
# KN_ASSET_CDN_BRANCH (its production branch, default main), KN_ASSET_CDN_STAGE (dist/assets-cdn),
# KN_ASSET_CDN_OBJECTS (object cache, ~/.cache/kn-assets-objects), KN_ASSET_CDN_EXCLUDE_FILE
# (the prefixes left out, default deploy/pages/assets-cdn-exclude.txt; "none" = none),
# KN_ASSET_CDN_EXCLUDE (space-separated prefixes left out on top of it), KN_ASSET_CDN_FORCE=1
# (deploy an unchanged version).
#
# --dry-run (or KN_ASSET_CDN_DRY_RUN=1): only read the origin's content list, print the file
# counts (excluded per prefix, left, to spare, the biggest directories) and check the Pages limits;
# no download, no staging, no Cloudflare credentials, no upload.
set -euo pipefail
cd "$(dirname "$0")/../.."
. deploy/pages/site.sh

: "${KN_ASSET_CDN_PROJECT:=krunknative-assets}"
: "${KN_ASSET_CDN_BRANCH:=main}"
: "${KN_ASSET_CDN_STAGE:=dist/assets-cdn}"
: "${KN_ASSET_CDN_OBJECTS:=$HOME/.cache/kn-assets-objects}"
: "${KN_ASSET_CDN_EXCLUDE_FILE:=deploy/pages/assets-cdn-exclude.txt}"
: "${KN_ASSET_CDN_EXCLUDE:=}"
: "${KN_ASSET_CDN_FORCE:=0}"
: "${KN_ASSET_CDN_DRY_RUN:=0}"
[ "${1:-}" = --dry-run ] && KN_ASSET_CDN_DRY_RUN=1

excludes=()
[ "$KN_ASSET_CDN_EXCLUDE_FILE" = none ] || excludes+=(--exclude-from "$KN_ASSET_CDN_EXCLUDE_FILE")
for p in $KN_ASSET_CDN_EXCLUDE; do excludes+=(--exclude "$p"); done

if [ "$KN_ASSET_CDN_DRY_RUN" = 1 ]; then
    exec python3 deploy/pages/assets_cdn.py "$KN_ASSET_CDN_STAGE" --origin "$KN_ASSET_OWN_URL" --dry-run "${excludes[@]}"
fi

kn_cloudflare_credentials

if [ "$KN_ASSET_CDN_FORCE" != 1 ] && [ -n "$KN_ASSET_CDN_URL" ] &&
   [ "$(python3 deploy/pages/assets_cdn.py "$KN_ASSET_CDN_STAGE" --origin "$KN_ASSET_OWN_URL" --current "$KN_ASSET_CDN_URL")" = same ]; then
    echo "assets-cdn.sh: $KN_ASSET_CDN_URL already has the own origin's version, nothing to deploy"
    exit 0
fi

python3 deploy/pages/assets_cdn.py "$KN_ASSET_CDN_STAGE" --origin "$KN_ASSET_OWN_URL" \
    --objects "$KN_ASSET_CDN_OBJECTS" "${excludes[@]}"

version=$(cat "$KN_ASSET_CDN_STAGE/assets/version.txt")
npx --yes "wrangler@$WRANGLER_VERSION" pages deploy "$KN_ASSET_CDN_STAGE" \
    --project-name "$KN_ASSET_CDN_PROJECT" --branch "$KN_ASSET_CDN_BRANCH" \
    --commit-hash "$(git rev-parse HEAD 2>/dev/null || echo 0)" --commit-message "assets $version" --commit-dirty=true
echo "assets-cdn.sh: deployed assets $version to $KN_ASSET_CDN_PROJECT"
