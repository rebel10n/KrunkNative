# Krunker Box Office deployment

Everything needed to run the backend locally and across regions/nodes. Protocols
and config keys: `doc/backend.md`. The layout mirrors the v3.1.1 jail's `ansible/`
(brain + nodes, nginx with TLS and WebSocket upgrade, CORS snippet), as a shell
script instead of ansible.

| path | purpose |
| --- | --- |
| `local/backend.sh` | `make backend-up` / `backend-down` / `backend-status`: migrate + kn_brain + N game servers + net-bridge on this machine (background, pidfiles) |
| `deploy.sh` | multi-host deploy: `build`, `push [host..]`, `status`, `render <host> <dir>`, `drain <host> <instance>` |
| `inventory.example` | hosts with `region=`, `brain=1`, `gameservers=N`, `bridge=1`, `nginx=brain\|node` (copy to `deploy/inventory`, gitignored) |
| `systemd/kn-brain.service` | the brain (`--migrate-only` in ExecStartPre, then serve) |
| `systemd/kn-gameserver@.service` | one `server_bin` lobby per instance; optional `/etc/krunknative/server-<instance>.cfg` (server.cfg syntax, doc/backend.md 8.7) over the JSON config, optional `gameserver-<instance>.env`; SIGTERM drains (TimeoutStopSec 30) |
| `systemd/kn-net-bridge.service` | `net-bridge --web`: web client + `/native` WebSocket -> game server TCP |
| `systemd/krunknative.target` | groups the units of a host |
| `nginx/brain.conf.template` | `matchmaker.<domain>` -> `/mm/`, `api.` -> `/api/`, `social.` -> `/social/`, `play.` = web client + all routes on one origin |
| `nginx/node.conf.template` | region host: web client + `/native` + `/ping` |
| `nginx/cors.conf`, `nginx/krunknative-proxy.conf` | snippets (CORS like the jail, WS upgrade + forwarded headers) |

## Local (one machine)

```sh
cp config/config.example.json config/config.json   # set database.password
make backend-up          # migrate, kn_brain :5050, 2 x server_bin :21015/:21016, net-bridge :8080 (if dist/web exists)
make backend-status      # processes + /mm/status + /mm/game-list
make backend-down
```

Knobs: `GAMESERVERS=3`, `BACKEND_ARGS="--set brain.matchmaking.soft_cap=6"`, and the
env variables at the top of `local/backend.sh` (`BRAIN_PORT`, `GAME_PORT`, `REGION`,
`BRIDGE`, `FAKE_NODES`). Logs and pidfiles: `$XDG_RUNTIME_DIR/krunknative-backend/`.
The native client finds the brain with `KRUNKNATIVE_BRAIN_URL=http://127.0.0.1:5050`;
the web client served by net-bridge uses the same origin.

A second "region" on the same machine (federation test):

```sh
build=cmake-build-debug
$build/services/brain/kn_brain --brain.port=5051 --brain.region=de-fra --brain.id=fra &
$build/server_bin --headless --port 21030 --node-id FRA1 --region de-fra \
    --public-host 127.0.0.1 --brain ws://127.0.0.1:5051/mm/internal/ws &
curl -s localhost:5050/mm/game-list    # lists both regions' lobbies
```

## Multi-region

Topology (leader free, doc/backend.md section 6):

* one PostgreSQL reachable from every brain host (private network);
* one `kn_brain` per region (or per host), bound to 127.0.0.1 behind nginx; game
  servers connect to the brain of their region over plain `ws://` (local or a private
  address: knsvc's WebSocket client does not speak TLS);
* each brain writes its own game servers' rows and heartbeats; every brain answers
  every public route for all regions, so `matchmaker.<domain>` can point at any or
  all of them (DNS round robin / geo DNS);
* game servers listen on public TCP ports (`first_port`..), web clients reach them
  through `wss://<region host>/native?target=127.0.0.1:<port>` (net-bridge).

```sh
cp deploy/inventory.example deploy/inventory      # edit hosts/regions
cat > deploy/deploy.env <<'ENV'                    # gitignored
DEPLOY_SECRET=<long random string>                 # brain.internal_secret == gameserver.brain_secret
DEPLOY_DOMAIN=example.com
DEPLOY_CONFIG=/path/to/prod-config.json            # shared DB credentials, lobby sizes, billboards...
ENV
deploy/deploy.sh build                 # release server_bin + kn_brain
deploy/deploy.sh -n push               # dry run: print every remote command
deploy/deploy.sh --certbot push        # first time (obtains TLS certs), later just `push`
deploy/deploy.sh push fra.example.com  # one host
deploy/deploy.sh status
deploy/deploy.sh drain nj.example.com NJ2   # finish the round, then the instance leaves the list
```

Per-host files written by `push`: `/etc/krunknative/config.json` (from
`DEPLOY_CONFIG`), `brain.env` (`KN_BRAIN_ID/REGION/PUBLIC_URL/INTERNAL_SECRET`,
`TRUST_PROXY=true`), `gameserver-<NODE>.env` (`KN_GAMESERVER_NODE_ID/PORT/REGION/
PUBLIC_HOST/BRAIN_URL/BRAIN_SECRET/WEB_URL`), `net-bridge.env` (`KRUNK_NATIVE_TARGETS`
allowlist), `/etc/nginx/conf.d/krunknative.conf`. Hosts need nginx, rsync,
node/npm (bridge hosts), certbot (with `--certbot`), and libpq + OpenSSL runtime
libraries.

Adding a region: add a host line (`region=`, `brain=1`, `gameservers=`), push it.
The other brains list its lobbies within a second of its first heartbeat; a host
that dies disappears from every game list after `brain.node_ttl_s` (30 s).
