#!/usr/bin/env bash
# KrunkNative multi-node deploy (shell replacement for the v3.1.1 jail's ansible/
# deploy.yml + nginx.yml). Pushes a release build to every host of the inventory,
# renders per-host config/env files, installs the systemd units and nginx vhosts,
# and restarts the services. Idempotent; re-run after every change.
#
#   deploy/deploy.sh [options] build             release build (server_bin, kn_brain, web optional)
#   deploy/deploy.sh [options] push [host...]     sync + configure + restart (default: all hosts)
#   deploy/deploy.sh [options] status [host...]   systemctl status + brain /mm/status
#   deploy/deploy.sh [options] render <host> DIR  only render that host's files into DIR (review)
#   deploy/deploy.sh [options] drain <host> <instance>   ask the brain to drain one game server
# options: -i INVENTORY (deploy/inventory)  -n (dry run: print remote commands)
#          --certbot (obtain missing TLS certs with certbot --nginx)
#
# Settings (env or deploy/deploy.env):
#   DEPLOY_CONFIG   config.json to install as /etc/krunknative/config.json (database
#                   credentials of the SHARED PostgreSQL, brain/gameserver/accounts
#                   sections). Default config/config.json. Never committed.
#   DEPLOY_SECRET   brain.internal_secret == gameserver.brain_secret (required for push)
#   DEPLOY_DOMAIN   base domain for the brain vhosts (matchmaker./api./social./play.)
#   DEPLOY_BUILD    build dir (cmake-build-release)
#   DEPLOY_PREFIX   install dir on the hosts (/opt/krunknative)
#   DEPLOY_USER     ssh user (root)
#   BRAIN_PORT=5050 BRIDGE_PORT=8080
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
[ -f "$ROOT/deploy/deploy.env" ] && . "$ROOT/deploy/deploy.env"
DEPLOY_CONFIG=${DEPLOY_CONFIG:-$ROOT/config/config.json}
DEPLOY_SECRET=${DEPLOY_SECRET:-}
DEPLOY_DOMAIN=${DEPLOY_DOMAIN:-example.com}
DEPLOY_BUILD=${DEPLOY_BUILD:-$ROOT/cmake-build-release}
DEPLOY_PREFIX=${DEPLOY_PREFIX:-/opt/krunknative}
DEPLOY_USER=${DEPLOY_USER:-root}
BRAIN_PORT=${BRAIN_PORT:-5050}
BRIDGE_PORT=${BRIDGE_PORT:-8080}
# read by server_bin on SIGTERM (gameserver.shutdown_reason_file): "update" while push restarts
STOP_REASON_FILE=/run/krunknative-deploy/gameserver-stop-reason
INVENTORY=$ROOT/deploy/inventory
DRY=0
CERTBOT=0

while [ $# -gt 0 ]; do
    case "$1" in
        -i) INVENTORY=$2; shift 2 ;;
        -n) DRY=1; shift ;;
        --certbot) CERTBOT=1; shift ;;
        *) break ;;
    esac
done
CMD=${1:-}; shift || true

say() { echo "[deploy] $*"; }
die() { echo "[deploy] error: $*" >&2; exit 1; }

# ---------------------------------------------------------------- inventory
declare -a HOSTS=()
declare -A VARS=()

load_inventory() {
    [ -f "$INVENTORY" ] || die "no inventory at $INVENTORY (copy deploy/inventory.example)"
    while read -r line; do
        line=${line%%#*}
        [ -z "${line// }" ] && continue
        read -r -a f <<< "$line"
        local h=${f[0]}
        HOSTS+=("$h")
        for kv in "${f[@]:1}"; do VARS["$h.${kv%%=*}"]=${kv#*=}; done
    done < "$INVENTORY"
}

var() { # host key default
    local k="$1.$2"
    if [ -n "${VARS[$k]+x}" ]; then echo "${VARS[$k]}"; else echo "${3:-}"; fi
}

# ---------------------------------------------------------------- remote helpers
remote() { # host cmd...
    local h=$1; shift
    if [ "$DRY" = 1 ]; then echo "  ssh $DEPLOY_USER@$h -- $*"; return 0; fi
    ssh -o BatchMode=yes "$DEPLOY_USER@$h" -- "$@"
}

sync_to() { # host src... dest
    local h=$1; shift
    local dest=${*: -1}
    local srcs=("${@:1:$#-1}")
    if [ "$DRY" = 1 ]; then echo "  rsync ${srcs[*]} -> $h:$dest"; return 0; fi
    rsync -az --delete-after "${srcs[@]}" "$DEPLOY_USER@$h:$dest"
}

subst() { # template -> stdout, ${VAR} from the environment
    local t
    t=$(cat "$1")
    for v in BRAIN_PORT BRIDGE_PORT MM_DOMAIN API_DOMAIN SOCIAL_DOMAIN PLAY_DOMAIN NODE_DOMAIN; do
        t=${t//\$\{$v\}/${!v:-}}
    done
    printf '%s\n' "$t"
}

# ---------------------------------------------------------------- render one host
render_host() { # host dir
    local h=$1 out=$2
    mkdir -p "$out/etc" "$out/nginx"
    local region; region=$(var "$h" region local)
    local pub; pub=$(var "$h" public_host "$h")
    local n; n=$(var "$h" gameservers 0)
    local first; first=$(var "$h" first_port 21015)
    local prefix; prefix=$(var "$h" node_prefix "$(echo "${h%%.*}" | tr '[:lower:]' '[:upper:]')")
    local brain_url; brain_url=$(var "$h" brain_url "ws://127.0.0.1:$BRAIN_PORT/mm/internal/ws")
    local bridge; bridge=$(var "$h" bridge 0)

    if [ "$(var "$h" brain 0)" = 1 ]; then
        cat > "$out/etc/brain.env" <<ENV
KN_BRAIN_ID=$h
KN_BRAIN_REGION=$region
KN_BRAIN_HOST=127.0.0.1
KN_BRAIN_PORT=$BRAIN_PORT
KN_BRAIN_PUBLIC_URL=https://$pub
KN_BRAIN_TRUST_PROXY=true
KN_BRAIN_INTERNAL_SECRET=$DEPLOY_SECRET
ENV
    fi
    local targets=""
    for ((i = 1; i <= n; i++)); do
        local port=$((first + i - 1)) node="$prefix$i"
        targets+="${targets:+,}127.0.0.1:$port"
        {
            echo "KN_GAMESERVER_NODE_ID=$node"
            echo "KN_GAMESERVER_NAME=Krunker Box Office $node"
            echo "KN_GAMESERVER_REGION=$region"
            echo "KN_GAMESERVER_PORT=$port"
            echo "KN_GAMESERVER_PUBLIC_HOST=$pub"
            echo "KN_GAMESERVER_PUBLIC_PORT=$port"
            echo "KN_GAMESERVER_BRAIN_URL=$brain_url"
            echo "KN_GAMESERVER_BRAIN_SECRET=$DEPLOY_SECRET"
            echo "KN_GAMESERVER_SHUTDOWN_REASON_FILE=$STOP_REASON_FILE"
            [ "$bridge" = 1 ] && echo "KN_GAMESERVER_WEB_URL=wss://$pub/native?target=127.0.0.1:$port"
        } > "$out/etc/gameserver-$node.env"
    done
    if [ "$bridge" = 1 ]; then
        cat > "$out/etc/net-bridge.env" <<ENV
KRUNK_NATIVE_BRIDGE_HOST=127.0.0.1
KRUNK_NATIVE_BRIDGE_PORT=$BRIDGE_PORT
KRUNK_NATIVE_HOST=127.0.0.1
KRUNK_NATIVE_PORT=$first
KRUNK_NATIVE_TARGETS=$targets
KRUNK_NATIVE_WEB_DIR=$DEPLOY_PREFIX/dist/web
ENV
    fi
    case "$(var "$h" nginx none)" in
        brain)
            MM_DOMAIN=matchmaker.$DEPLOY_DOMAIN API_DOMAIN=api.$DEPLOY_DOMAIN SOCIAL_DOMAIN=social.$DEPLOY_DOMAIN \
            PLAY_DOMAIN=$pub subst "$ROOT/deploy/nginx/brain.conf.template" > "$out/nginx/krunknative.conf" ;;
        node)
            NODE_DOMAIN=$pub subst "$ROOT/deploy/nginx/node.conf.template" > "$out/nginx/krunknative.conf" ;;
    esac
    echo "$n" > "$out/gameservers"
    echo "$prefix" > "$out/node_prefix"
}

# ---------------------------------------------------------------- commands
cmd_build() {
    say "release build -> $DEPLOY_BUILD"
    cmake --preset release -B "$DEPLOY_BUILD" -DKRUNKNATIVE_HTML_ENGINE=OFF >/dev/null
    cmake --build "$DEPLOY_BUILD" --target server_bin services_all --parallel
}

push_host() {
    local h=$1
    local tmp; tmp=$(mktemp -d)
    render_host "$h" "$tmp"
    local n; n=$(cat "$tmp/gameservers")
    local prefix; prefix=$(cat "$tmp/node_prefix")
    say "== $h ($(var "$h" region local), $n game servers)"
    remote "$h" "id -u krunknative >/dev/null 2>&1 || useradd --system --home $DEPLOY_PREFIX --shell /usr/sbin/nologin krunknative; mkdir -p $DEPLOY_PREFIX/bin /etc/krunknative"
    sync_to "$h" "$DEPLOY_BUILD/server_bin" "$DEPLOY_BUILD/services/brain/kn_brain" "$DEPLOY_PREFIX/bin/"
    sync_to "$h" "$ROOT/assets" "$ROOT/services" "$DEPLOY_PREFIX/"
    if [ "$(var "$h" bridge 0)" = 1 ]; then
        sync_to "$h" "$ROOT/net-bridge" "$DEPLOY_PREFIX/"
        [ -d "$ROOT/dist/web" ] && sync_to "$h" "$ROOT/dist" "$DEPLOY_PREFIX/"
        remote "$h" "cd $DEPLOY_PREFIX/net-bridge && npm ci --omit=dev --silent"
    fi
    sync_to "$h" "$DEPLOY_CONFIG" "/etc/krunknative/config.json"
    sync_to "$h" "$tmp/etc/" "/etc/krunknative/"
    sync_to "$h" "$ROOT/deploy/systemd/" "/etc/systemd/system/"
    remote "$h" "chown -R root:krunknative /etc/krunknative && chmod 750 /etc/krunknative && chmod 640 /etc/krunknative/* && systemctl daemon-reload"
    if [ -f "$tmp/nginx/krunknative.conf" ]; then
        sync_to "$h" "$ROOT/deploy/nginx/cors.conf" "/etc/nginx/snippets/cors.conf"
        sync_to "$h" "$ROOT/deploy/nginx/krunknative-proxy.conf" "/etc/nginx/snippets/krunknative-proxy.conf"
        sync_to "$h" "$tmp/nginx/krunknative.conf" "/etc/nginx/conf.d/krunknative.conf"
        if [ "$CERTBOT" = 1 ]; then
            local names; names=$(grep -ho 'server_name [^;]*' "$tmp/nginx/krunknative.conf" | sed 's/server_name //' | tr ' ' '\n' | sort -u | tr '\n' ' ')
            for d in $names; do
                remote "$h" "test -f /etc/letsencrypt/live/$d/fullchain.pem || certbot certonly --nginx -d $d --non-interactive --agree-tos -m admin@$DEPLOY_DOMAIN"
            done
        fi
        remote "$h" "nginx -t && systemctl reload nginx"
    fi
    local units="krunknative.target"
    [ "$(var "$h" brain 0)" = 1 ] && units+=" kn-brain.service"
    [ "$(var "$h" bridge 0)" = 1 ] && units+=" kn-net-bridge.service"
    for ((i = 1; i <= n; i++)); do units+=" kn-gameserver@$prefix$i.service"; done
    # brain first (migrations run in its ExecStartPre), then the rest. The stop reason file tells
    # the game servers' players "GAME UPDATING" (server_bin shutdown_reason_file) during the restart.
    remote "$h" "install -d -m 0755 ${STOP_REASON_FILE%/*} && echo update > $STOP_REASON_FILE && chmod 0644 $STOP_REASON_FILE; systemctl enable $units >/dev/null && systemctl restart $units; rc=\$?; rm -f $STOP_REASON_FILE; exit \$rc"
    # stop instances that are no longer in the inventory
    remote "$h" "for u in \$(systemctl list-units --plain --no-legend 'kn-gameserver@*' | awk '{print \$1}'); do case \" $units \" in *\" \$u \"*) ;; *) systemctl disable --now \$u ;; esac; done"
    rm -rf "$tmp"
}

cmd_push() {
    [ -n "$DEPLOY_SECRET" ] || die "DEPLOY_SECRET is required (shared brain/game server secret)"
    [ -f "$DEPLOY_CONFIG" ] || die "DEPLOY_CONFIG $DEPLOY_CONFIG not found"
    [ "$DRY" = 1 ] || [ -x "$DEPLOY_BUILD/server_bin" ] || die "no release build in $DEPLOY_BUILD (deploy.sh build)"
    local targets=("$@")
    [ ${#targets[@]} -gt 0 ] || targets=("${HOSTS[@]}")
    # brains before game servers so nodes find their brain at once
    for h in "${targets[@]}"; do [ "$(var "$h" brain 0)" = 1 ] && push_host "$h"; done
    for h in "${targets[@]}"; do [ "$(var "$h" brain 0)" = 1 ] || push_host "$h"; done
}

cmd_status() {
    local targets=("$@")
    [ ${#targets[@]} -gt 0 ] || targets=("${HOSTS[@]}")
    for h in "${targets[@]}"; do
        say "== $h"
        remote "$h" "systemctl --no-pager --plain list-units 'kn-*' 'krunknative.target'; curl -fs http://127.0.0.1:$BRAIN_PORT/mm/status || true; echo"
    done
}

cmd_drain() {
    local h=$1 inst=$2
    remote "$h" "curl -fs -X POST -H 'Authorization: Bearer $DEPLOY_SECRET' 'http://127.0.0.1:$BRAIN_PORT/mm/admin/drain?node=$inst'"
}

load_inventory
case "$CMD" in
    build) cmd_build ;;
    push) cmd_push "$@" ;;
    status) cmd_status "$@" ;;
    render) [ $# -eq 2 ] || die "render <host> <dir>"; render_host "$1" "$2"; say "rendered into $2" ;;
    drain) [ $# -eq 2 ] || die "drain <host> <instance>"; cmd_drain "$1" "$2" ;;
    *) sed -n '2,25p' "$0"; exit 2 ;;
esac
