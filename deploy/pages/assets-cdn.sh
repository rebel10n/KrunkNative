#!/bin/bash
# Publish the game content to the Cloudflare Pages asset CDN (deploy/pages/README.md "Asset
# CDN"): mirror the kru download host's tree (assets_cdn.py), split by directory over the Pages
# projects of deploy/asset-shards.json, and `wrangler pages deploy` each part to its project
# (krunknative-assets-a: everything else + the _redirects old clients need, krunknative-assets-b:
# its directories). wrangler hashes every file and uploads only the ones Cloudflare does not have
# yet, so a deploy after a content change sends just the new files. The other shards go first and
# the default shard last: its assets/version.txt is what the next run's "already deployed" check
# and loader.js read. CI: .github/workflows/assets-cdn.yml.
#
# Settings (site.sh): KN_ASSET_DL_URL (the source, default the file's "download": https://dl.krunk.io/),
# KN_ASSET_SHARDS_FILE (default deploy/asset-shards.json), KN_ASSET_CDN_BRANCH (the projects'
# production branch, default main), KN_ASSET_CDN_STAGE (dist/assets-cdn), KN_ASSET_CDN_OBJECTS
# (object cache, ~/.cache/kn-assets-objects), KN_ASSET_CDN_EXCLUDE (space-separated prefixes on no
# project, on top of the file's "exclude"), KN_ASSET_CDN_FORCE=1 (deploy an unchanged version).
#
# --dry-run (or KN_ASSET_CDN_DRY_RUN=1): only read the origin's content list, print the file
# counts per shard and directory and check the Pages limits; no download, no staging, no
# Cloudflare credentials, no upload.
set -euo pipefail
cd "$(dirname "$0")/../.."
. deploy/pages/site.sh

: "${KN_ASSET_SHARDS_FILE:=deploy/asset-shards.json}"
: "${KN_ASSET_CDN_BRANCH:=main}"
: "${KN_ASSET_CDN_STAGE:=dist/assets-cdn}"
: "${KN_ASSET_CDN_OBJECTS:=$HOME/.cache/kn-assets-objects}"
: "${KN_ASSET_CDN_EXCLUDE:=}"
: "${KN_ASSET_CDN_FORCE:=0}"
: "${KN_ASSET_CDN_DRY_RUN:=0}"
[ "${1:-}" = --dry-run ] && KN_ASSET_CDN_DRY_RUN=1

# KN_SHARDS, KN_SHARD_DEFAULT, KN_SHARD_<name>_{PROJECT,URL,PAGES_DEV}
eval "$(python3 -I deploy/pages/asset_shards.py --config "$KN_ASSET_SHARDS_FILE" env)"
src=${KN_ASSET_DL_URL:-$KN_ASSET_DL_URL_DEFAULT}
args=("$KN_ASSET_CDN_STAGE" --config "$KN_ASSET_SHARDS_FILE" --origin "$src")
for p in $KN_ASSET_CDN_EXCLUDE; do args+=(--exclude "$p"); done

if [ "$KN_ASSET_CDN_DRY_RUN" = 1 ]; then
    exec python3 -I deploy/pages/assets_cdn.py "${args[@]}" --dry-run
fi

kn_cloudflare_credentials

if [ "$KN_ASSET_CDN_FORCE" != 1 ] && [ "$(python3 -I deploy/pages/assets_cdn.py "${args[@]}" --current)" = same ]; then
    echo "assets-cdn.sh: every shard already has the version of $src, nothing to deploy"
    exit 0
fi

python3 -I deploy/pages/assets_cdn.py "${args[@]}" --objects "$KN_ASSET_CDN_OBJECTS"

version=$(cat "$KN_ASSET_CDN_STAGE/$KN_SHARD_DEFAULT/assets/version.txt")
order=()
for s in $KN_SHARDS; do [ "$s" = "$KN_SHARD_DEFAULT" ] || order+=("$s"); done
order+=("$KN_SHARD_DEFAULT")
for s in "${order[@]}"; do
    project_var="KN_SHARD_${s}_PROJECT"
    project=${!project_var}
    npx --yes "wrangler@$WRANGLER_VERSION" pages deploy "$KN_ASSET_CDN_STAGE/$s" \
        --project-name "$project" --branch "$KN_ASSET_CDN_BRANCH" \
        --commit-hash "$(git rev-parse HEAD 2>/dev/null || echo 0)" --commit-message "assets $version ($s)" --commit-dirty=true
    echo "assets-cdn.sh: deployed shard $s (assets $version) to $project"
done
