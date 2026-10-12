#!/usr/bin/env bash
# Publishes release assets for CI (release.yml: server-latest / server-v*, client.yml: client-latest / client-v*).
#
# Usage: tools/release/publish.sh <tag> <title> <notes.md> <asset>...
# Env:   GH_TOKEN, GH_REPO (owner/name), COMMIT (full sha), COMMIT_TIME (unix seconds)
#
#   *-latest tags  rolling prerelease: assets replaced in place (--clobber), then the notes, then
#                  the tag is moved to COMMIT. Only moves forward: when the published notes'
#                  <!-- kn-build ... --> line has a newer commit_time than COMMIT_TIME (an older
#                  commit), nothing is published (exit 0, a ::notice).
#   other tags     the tag must exist (pushed by hand); the release is created once, later runs
#                  replace its assets and notes.
# Never marks a release "Latest" (v1.0.0 stays the repo's Latest release).
set -euo pipefail

[ $# -ge 4 ] || { sed -n '2,14p' "$0" >&2; exit 2; }
tag="$1" title="$2" notes="$3"
shift 3
: "${GH_REPO:?}" "${COMMIT:?}" "${COMMIT_TIME:?}"
[[ "$COMMIT" =~ ^[0-9a-f]{40}$ && "$COMMIT_TIME" =~ ^[0-9]+$ ]] || { echo "publish.sh: bad COMMIT/COMMIT_TIME" >&2; exit 2; }
rolling=0
[[ "$tag" == *-latest ]] && rolling=1

exists=0
published=0
if body="$(gh release view "$tag" --json body --jq .body 2> /dev/null)"; then
    exists=1
    line="$(printf '%s\n' "$body" | tr -d '\r' | sed -n 's/^<!-- kn-build \(.*\) -->$/\1/p' | head -1)"
    if [ -n "$line" ]; then published="$(jq -r '.commit_time // 0' <<< "$line" 2> /dev/null || echo 0)"; fi
fi
[[ "$published" =~ ^[0-9]+$ ]] || published=0
echo "publish.sh: $tag exists=$exists published commit_time=$published, this build $COMMIT_TIME (${COMMIT:0:12})"
if [ "$rolling" = 1 ] && [ "$published" -gt "$COMMIT_TIME" ]; then
    echo "::notice::$tag already has a newer build than ${COMMIT:0:12}: not publishing"
    exit 0
fi

if [ "$exists" = 0 ]; then
    if [ "$rolling" = 1 ]; then
        gh release create "$tag" --target "$COMMIT" --prerelease --latest=false --title "$title" --notes-file "$notes" "$@"
    else
        gh release create "$tag" --verify-tag --latest=false --title "$title" --notes-file "$notes" "$@"
    fi
else
    gh release upload "$tag" "$@" --clobber
    gh release edit "$tag" --title "$title" --notes-file "$notes"
    if [ "$rolling" = 1 ]; then
        # GITHUB_TOKEN may not move a ref onto a commit whose .github/workflows differ ("Resource
        # not accessible by integration"); the assets and the kn-build notes are already updated,
        # so a tag left behind is only cosmetic.
        gh api -X PATCH "repos/$GH_REPO/git/refs/tags/$tag" -f sha="$COMMIT" -F force=true > /dev/null \
            || echo "::warning::could not move tag $tag to ${COMMIT:0:12} (workflow files changed); assets and notes are updated"
        # drop assets this build no longer produces (renamed / removed platforms)
        keep=" "
        for f in "$@"; do keep+="$(basename "$f") "; done
        gh release view "$tag" --json assets --jq '.assets[].name' | while read -r old; do
            [[ "$keep" == *" $old "* ]] || gh release delete-asset "$tag" "$old" --yes
        done
    fi
fi
gh release view "$tag" --json url,assets --jq '.url, (.assets[] | "  " + .name)'
