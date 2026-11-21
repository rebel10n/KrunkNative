#!/usr/bin/env bash
# Hand deploy of one commit without GitHub Actions (deploy/ci-runner/README.md "Local CI"): what
# .github/workflows/deploy-kru.yml, deploy-edges.yml and assets-cdn.yml do, from this machine, over divide
# (deploy/ansible/ssh_config; the operator's own ssh keys). Refuses unless tools/ci/local-ci.sh passed for
# the commit (~/.cache/kn-ci/<commit12>/PASS).
#
#   kru    make web, kn_brain + kn_migrate for bookworm (deploy/ansible/build.sh), the Go pin check, then
#          ansible-playbook central.yml (-e kn_web_sync_assets=false -e kn_release_id=<commit12>), with the
#          secrets of ~/.config/kn-secrets/{smtp,ai,discord}.env (a missing one keeps kru's file)
#   edges  server_bin + kn_rentd for bullseye / bookworm / trixie, edges.yml (tags firewall,fail2ban,user,
#          release,gameserver) on the canary kn-ams, its health, then kn-mnz, then every edge's health
#   cdn    deploy/pages/assets-cdn.sh when the CDN lacks dl.krunk.io's assets/version.txt (CLOUDFLARE_* from
#          ~/.config/kn-secrets/github-ci/)
# Not here: the krunk.io front end (since 2026-11-21 kru's /opt/krunknative-pages, published with
# deploy/pages/owned.sh stage / publish-stage / verify-stage, deploy/pages/OWNED_HOST.md; GitHub Pages is only
# the rollback), and the client-latest / server-latest releases (client.yml / release.yml publish, they need
# GitHub: every platform or none, see the README). kru's own web release (central.yml, role kn_web) does go out.
#
# Usage: tools/ci/local-deploy.sh [--only kru,edges,cdn] [--dry-run] <rev>
#   --dry-run  builds, then ansible --check --diff (changes nothing); no CDN upload
set -euo pipefail

only="" dry=0
while [ $# -gt 1 ]; do
    case "$1" in
        --only) only=",$2,"; shift 2 ;;
        --dry-run) dry=1; shift ;;
        *) break ;;
    esac
done
[ $# -eq 1 ] || { sed -n '2,20p' "$0" >&2; exit 2; }

repo="$(cd "$(dirname "$0")/../.." && pwd)"
sha="$(git -C "$repo" rev-parse --verify "$1^{commit}")"
s12="${sha:0:12}"
ci="${KN_CI_OUT:-$HOME/.cache/kn-ci}/$s12"
secrets="$HOME/.config/kn-secrets"
want() { [ -z "$only" ] || [[ "$only" == *",$1,"* ]]; }
log() { printf '[local-deploy %s] %s\n' "$(date +%H:%M:%S)" "$*" >&2; }

[ "$(cat "$ci/PASS" 2> /dev/null)" = "$sha" ] || { echo "local-deploy: no passing local CI for $sha ($ci/PASS): run tools/ci/local-ci.sh $s12" >&2; exit 1; }

out="$ci/deploy"
src="$out/src"
mkdir -p "$out"
log "commit $sha: exporting to $src"
rm -rf "$src" && mkdir -p "$src"
git -C "$repo" archive --format=tar "$sha" | tar -x -C "$src"
# the service sources' own commit (version-meta.sh --server): unchanged sources build byte-identical binaries
"$repo/tools/release/version-meta.sh" --server "$sha" > "$out/meta-server.txt"
server_flags="$(sed -n 's/^cmake_flags=//p' "$out/meta-server.txt")"
server12="$(sed -n 's/^commit12=//p' "$out/meta-server.txt")"
export ANSIBLE_COLLECTIONS_PATH="$repo/deploy/ansible/.ansible/collections:$HOME/.ansible/collections:/usr/share/ansible/collections"
check=(); [ $dry = 1 ] && check=(--check --diff)

# ---- kru (deploy-kru.yml) --------------------------------------------------------------------------
if want kru; then
    log "kru: make web"
    (cd "$src" && EMSDK="${EMSDK:-$HOME/emsdk}" make web) > "$out/web.log" 2>&1 || { tail -30 "$out/web.log" >&2; exit 1; }
    log "kru: kn_brain + kn_migrate (bookworm)"
    (cd "$src" && TARGETS="kn_brain kn_migrate" KN_CMAKE_ARGS="$server_flags" deploy/ansible/build.sh bookworm) \
        > "$out/build-services.log" 2>&1 || { tail -30 "$out/build-services.log" >&2; exit 1; }
    # deploy-kru.yml's Go pin: net-bridge/*/go.mod's go line; GOTOOLCHAIN=local, never a download
    go_pin="$(sed -n 's/^  GO_VERSION: //p' "$src/.github/workflows/deploy-kru.yml")"
    for mod in "$src"/net-bridge/*/go.mod; do
        [ "$(awk '$1 == "go" { print $2 }' "$mod")" = "$go_pin" ] || { echo "$mod: go line is not deploy-kru.yml's $go_pin" >&2; exit 1; }
    done
    export GOTOOLCHAIN=local
    log "kru: ansible-playbook central.yml ($(go version | cut -d' ' -f3))"
    (
        set -a
        # shellcheck disable=SC1090 # the operator's secret env files
        for f in smtp ai discord; do [ -f "$secrets/$f.env" ] && . "$secrets/$f.env"; done
        set +a
        cd "$src/deploy/ansible"
        ansible-playbook central.yml -e kn_web_sync_assets=false -e "kn_release_id=$s12" "${check[@]}"
    ) > "$out/central.log" 2>&1 || { tail -40 "$out/central.log" >&2; exit 1; }
    grep -E '^(PLAY RECAP|kru )' -A2 "$out/central.log" | tail -3 >&2
fi

# ---- edges (deploy-edges.yml) ----------------------------------------------------------------------
edge_health() { # edge_health <edge>...: instance 1 answers and runs this build's server commit
    local edge target info commit
    for edge in "$@"; do
        target="$edge-1.krunk.io:21015"
        for _ in $(seq 1 18); do
            if info="$(python3 -I "$src/services/scripts/kn-query.py" --json --timeout 3 "$target" 2> /dev/null)"; then
                commit="$(jq -r '.commit // empty' <<< "$info")"
                echo "$target: $(jq -c '{name, map, players, max, version, commit, protocol}' <<< "$info")" >&2
                if [ $dry = 0 ] && [ -n "$commit" ] && [ "${commit:0:12}" != "${server12:0:${#commit}}" ]; then
                    echo "$target runs $commit, expected $server12" >&2; return 1
                fi
                continue 2
            fi
            sleep 5
        done
        echo "$target does not answer the server-info query" >&2; return 1
    done
    for edge in "$@"; do # Krunker Strike instance 1: reported, never fatal
        info="$(python3 -I "$src/services/scripts/kn-query.py" --json --timeout 3 "$edge-1.krunk.io:21115" 2> /dev/null)" \
            && echo "$edge-1.krunk.io:21115: $(jq -c '{name, flavor, map, players, commit}' <<< "$info")" >&2 \
            || echo "warning: $edge-1.krunk.io:21115 (Krunker Strike) does not answer" >&2
    done
}
if want edges; then
    for c in bullseye bookworm trixie; do
        log "edges: server_bin + kn_rentd ($c)"
        (cd "$src" && TARGETS="server_bin kn_rentd" KN_CMAKE_ARGS="$server_flags" deploy/ansible/build.sh "$c") \
            > "$out/build-edges-$c.log" 2>&1 || { tail -30 "$out/build-edges-$c.log" >&2; exit 1; }
    done
    tags=firewall,fail2ban,user,release,gameserver
    cd "$src/deploy/ansible"
    ansible -m ansible.builtin.ping "kru,kn-ams,kn-mnz" > "$out/edges-ping.log" 2>&1 || { cat "$out/edges-ping.log" >&2; exit 1; }
    log "edges: canary kn-ams"
    ansible-playbook edges.yml --tags "$tags" --limit kn-ams "${check[@]}" > "$out/edges-ams.log" 2>&1 \
        || { tail -40 "$out/edges-ams.log" >&2; exit 1; }
    edge_health ams
    log "edges: kn-mnz (ashburn)"
    ansible-playbook edges.yml --tags "$tags" --limit kn-mnz "${check[@]}" > "$out/edges-mnz.log" 2>&1 \
        || { tail -40 "$out/edges-mnz.log" >&2; exit 1; }
    edge_health ams ashburn
    cd "$repo"
fi

# ---- asset CDN (assets-cdn.yml) --------------------------------------------------------------------
if want cdn && [ $dry = 0 ]; then
    cd "$src"
    eval "$(python3 -I deploy/pages/asset_shards.py env)"
    dl="${KN_ASSET_DL_URL:-$KN_ASSET_DL_URL_DEFAULT}"
    state="$(python3 -I deploy/pages/assets_cdn.py dist/assets-cdn --origin "$dl" --current)"
    log "cdn: source $dl $(curl -fsS "${dl}assets/version.txt" | tr -d '[:space:]'): CDN $state"
    if [ "$state" = changed ]; then
        CLOUDFLARE_API_TOKEN="$(cat "$secrets/github-ci/CLOUDFLARE_API_TOKEN")" \
        CLOUDFLARE_ACCOUNT_ID="$(cat "$secrets/github-ci/CLOUDFLARE_ACCOUNT_ID")" \
        KN_ASSET_CDN_OBJECTS="$HOME/.cache/kn-ci/assets-cdn-objects" KN_ASSET_CDN_FORCE=1 \
            deploy/pages/assets-cdn.sh > "$out/cdn.log" 2>&1 || { tail -30 "$out/cdn.log" >&2; exit 1; }
    fi
    cd "$repo"
fi

log "done: $sha (server sources $server12); logs in $out"
