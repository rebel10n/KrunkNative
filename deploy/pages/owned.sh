#!/usr/bin/env bash
# Stage the Pages front end on the owned kru host without changing public ingress.
# No live DNS writes, no production vhost: 127.0.0.1:18080 only.
# Usage: deploy/pages/owned.sh stage|publish-stage|verify-stage|snapshot-dns
set -euo pipefail
cd "$(dirname "$0")/../.."

. deploy/pages/site.sh
: "${KN_OWNED_STAGE_HOST:=kru}"
: "${KN_OWNED_STAGE_DIR:=dist/owned-pages}"
KN_OWNED_STAGE_PREVIOUS="${KN_OWNED_STAGE_PREVIOUS-https://krunk.io/}"
: "${KN_ASSET_FALLBACK:=dl}"
ssh_args=(-F deploy/ansible/ssh_config -o BatchMode=yes)

case "$KN_OWNED_STAGE_HOST" in
    *[!a-zA-Z0-9-]*|'') echo "owned.sh: invalid SSH host alias" >&2; exit 2 ;;
esac

stage() {
    local site=()
    mapfile -d '' site < <(kn_site_args)
    python3 deploy/pages/stage.py dist/web "$KN_OWNED_STAGE_DIR" --target nginx \
        --keep-from "$KN_OWNED_STAGE_PREVIOUS" "${site[@]}"
}

# Snapshot the resolved public records before any proposed future cutover. This
# intentionally never reads Spaceship credentials or prints the records to chat.
snapshot_dns() {
    command -v dig >/dev/null || { echo 'owned.sh: dig is required for DNS rollback capture' >&2; return 1; }
    local savedir="$HOME/.config/kn-pages" file
    umask 077
    mkdir -p "$savedir"
    file=$(mktemp "$savedir/rollback-dns-$(date -u +%Y%m%dT%H%M%SZ)-XXXXXX.txt")
    {
        printf '# Observed DNS answers (UTC) %s; compare with Spaceship before changing records\n' "$(date -u +%FT%TZ)"
        for record in 'krunk.io A' 'krunk.io AAAA' 'www.krunk.io CNAME' 'www.krunk.io A' 'www.krunk.io AAAA'; do
            read -r host kind <<< "$record"
            printf '# %s %s\n' "$host" "$kind"
            dig +noall +answer "$host" "$kind"
        done
    } > "$file"
    if ! grep -q '^krunk\.io\..*[[:space:]]A[[:space:]]' "$file" || \
       ! grep -q '^www\.krunk\.io\..*[[:space:]]CNAME[[:space:]]' "$file"; then
        rm -f "$file"
        echo 'owned.sh: DNS answers incomplete; rollback snapshot not saved' >&2
        return 1
    fi
    printf 'Private DNS rollback snapshot: %s\n' "$file"
}

publish_stage() {
    command -v rsync >/dev/null || { echo 'owned.sh: rsync is required' >&2; return 1; }
    stage
    snapshot_dns

    # Include the page config in the release id; the same Wasm build may be
    # republished with different asset origins or URLs.
    local release
    release=$(cd "$KN_OWNED_STAGE_DIR" && find . -type f -print0 | LC_ALL=C sort -z | \
        xargs -0 sha256sum | sha256sum | cut -c1-16)
    [[ $release =~ ^[a-f0-9]{16}$ ]] || { echo 'owned.sh: invalid release hash' >&2; return 1; }

    ssh "${ssh_args[@]}" "$KN_OWNED_STAGE_HOST" \
        "install -d -m 0755 /opt/krunknative-pages/releases/$release"
    rsync -rt --checksum --delete --chmod=D755,F644 \
        -e "ssh -F deploy/ansible/ssh_config -o BatchMode=yes" \
        "$KN_OWNED_STAGE_DIR/" "$KN_OWNED_STAGE_HOST:/opt/krunknative-pages/releases/$release/"

    # The remote config lives only on loopback. Never overwrite a different
    # operator-owned file of the same name, or leave a broken nginx vhost enabled.
    ssh "${ssh_args[@]}" "$KN_OWNED_STAGE_HOST" \
        'cat > /opt/krunknative-pages/.stage-nginx-candidate.conf' < deploy/pages/owned-stage.nginx.conf
    ssh "${ssh_args[@]}" "$KN_OWNED_STAGE_HOST" "bash -se -- $release" <<'REMOTE'
release=$1
base=/opt/krunknative-pages
conf=/etc/nginx/sites-available/krunknative-pages-stage.conf
link=/etc/nginx/sites-enabled/zz-krunknative-pages-stage.conf
tmp=/opt/krunknative-pages/.stage-nginx-candidate.conf
created_conf=0
created_link=0
if [ -e "$conf" ] && ! cmp -s "$tmp" "$conf"; then
    rm -f "$tmp"
    echo 'owned.sh: staged nginx config differs from existing host config; review it first' >&2
    exit 1
fi
if [ ! -e "$conf" ]; then
    install -m 0644 "$tmp" "$conf"
    created_conf=1
fi
rm -f "$tmp"
if [ ! -L "$link" ]; then
    if [ -e "$link" ]; then
        echo 'owned.sh: nginx site link path is occupied' >&2
        exit 1
    fi
    ln -s "$conf" "$link"
    created_link=1
fi
if ! nginx -t; then
    if [ "$created_link" = 1 ]; then rm -f "$link"; fi
    if [ "$created_conf" = 1 ]; then rm -f "$conf"; fi
    exit 1
fi
systemctl reload nginx
ln -s "releases/$release" "$base/.current.next.$$"
mv -Tf "$base/.current.next.$$" "$base/current"
printf 'Staged on kru loopback 127.0.0.1:18080, release %s\n' "$release"
REMOTE
}

verify_stage() {
    # SSH forwarding checks the real kru nginx listener, but leaves all public DNS
    # untouched. curl --resolve makes Host krunk.io while connecting to loopback.
    local tunnel_pid='' verify_port
    # Use a fresh local port. A fixed port can accidentally hit another tunnel,
    # giving a false success if the requested SSH forward failed to start.
    verify_port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')
    ssh "${ssh_args[@]}" -o ExitOnForwardFailure=yes -N \
        -L "127.0.0.1:$verify_port:127.0.0.1:18080" "$KN_OWNED_STAGE_HOST" &
    tunnel_pid=$!
    # Capture the numeric PID now; function locals may be gone at shell EXIT.
    # shellcheck disable=SC2064
    trap "kill $tunnel_pid 2>/dev/null || true" EXIT
    local ready=0
    for _attempt in {1..20}; do
        if ! kill -0 "$tunnel_pid" 2>/dev/null; then break; fi
        if curl --noproxy '*' --silent --fail \
            --resolve "krunk.io:$verify_port:127.0.0.1" \
            "http://krunk.io:$verify_port/index.html" -o /dev/null; then
            ready=1
            break
        fi
        if ! kill -0 "$tunnel_pid" 2>/dev/null; then break; fi
        sleep 0.25
    done
    [ "$ready" = 1 ] || { echo 'owned.sh: SSH tunnel/staged nginx did not respond' >&2; return 1; }
    local result=0
    if ! curl --noproxy '*' --fail --silent --show-error \
        --resolve "krunk.io:$verify_port:127.0.0.1" \
        "http://krunk.io:$verify_port/index.html" | grep -F '<!-- kn-hot-loader -->' >/dev/null; then
        echo 'owned.sh: staged index.html is missing the early hot loader' >&2
        result=1
    fi
    deploy/pages/check.sh "http://krunk.io:$verify_port/" \
        --noproxy '*' --resolve "krunk.io:$verify_port:127.0.0.1" || result=$?
    kill "$tunnel_pid" 2>/dev/null || true
    wait "$tunnel_pid" 2>/dev/null || true
    trap - EXIT
    return "$result"
}

case "${1:-}" in
    stage) stage ;;
    publish-stage) publish_stage ;;
    verify-stage) verify_stage ;;
    snapshot-dns) snapshot_dns ;;
    *) echo 'usage: deploy/pages/owned.sh stage|publish-stage|verify-stage|snapshot-dns' >&2; exit 2 ;;
esac
