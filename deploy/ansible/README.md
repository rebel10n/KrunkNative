# deploy/ansible

Ansible for the Krunker Box Office central node (`kru`) and the edge game servers.

| playbook | hosts | what |
| --- | --- | --- |
| `central.yml` | `kru` | user `kn`, OpenResty 127.0.0.1:8081 (static web build), kn_brain finder :8082 + auth/community :8083, PostgreSQL 127.0.0.1:5434 + Redis 127.0.0.1:6380 for kn_brain (see [Database cutover](#database-cutover-postgresql--redis-on-kru)), net-bridge :8084 (`play.krunk.io`), WebRTC gateway :8085 + udp 21100 (`play.krunk.io/rtc`), edge origin (host nginx :443 vhost + OpenResty 127.0.0.1:8086, off until `kn_edge_origin_enabled`), download host `dl.krunk.io` (host nginx :80/:443 + Let's Encrypt + OpenResty 127.0.0.1:8087, off until `kn_dl_origin_enabled`), `/opt/krunknative` clone |
| `edges.yml` | `edges` | host firewall drop-in (game ports), user `kn`, `server_bin` x (vcpus-1, min 1), Krunker Strike `server_bin --flavor strike` x (1, 2 with 8+ vcpus; see [Krunker Strike servers](#krunker-strike-servers)), `<edge>-<n>.krunk.io` DNS |
| `firewall.yml` | `edges` | only the host firewall (vendored `edge_firewall` role), see [Edge firewall](#edge-firewall) |
| `fail2ban.yml` | `kru`, `edges` | only fail2ban (vendored `fail2ban` role; base on kru, drop-ins on the edges), see [fail2ban](#fail2ban) |
| `site.yml` | all | both |

Tags: `user disk openresty repo release postgres redis brain cutover web updates bridge rtc edge_origin dl_origin gameserver dns firewall fail2ban`.

Client updates (`kn_updates`, doc/backend.md 9.8): `https://dl.krunk.io/updates/` (and
`assets.krunk.io/updates/` while that name is the edge origin) serves
`/opt/krunknative-updates/public/` (OpenResty `kn.d/updates.conf`): the channel manifest CI's
`client.yml` job `updates` publishes with `kn-updates publish`, the update packages and the game
content as hardlinked objects of the web asset tree. Off until `kn_updates_enabled: true`
(`group_vars/central/main.yml`; Deploy kru runs central.yml on every push). Enable it there, apply
with `ansible-playbook central.yml --tags openresty,updates` (no full deploy needed), then set the
repository variable `KN_UPDATES_UPLOAD=1`.

Routing on kru (the abulxt edges -> kru's OpenResty, [Edge origin](#edge-origin-abulxts-edges-instead-of-cloudflare)): `assets.krunk.io -> :8081`,
`matchmaker.krunk.io -> :8082`, `coordinator.krunk.io -> :8083`, `play.krunk.io -> :8084` (the
last one is kept by the `kn_bridge` role, see [net-bridge](#net-bridge-playkrunkio)). The page itself (`krunk.io`,
`www.`) is on GitHub Pages (deploy-kru.yml job `pages`), see `deploy/pages/README.md` (also caching and
the krunker.party redirect: the site 301s, `coordinator.`/`matchmaker.krunker.party` keep
working for old clients and are not redirected). With `kn_edge_origin_enabled` the same four
krunk.io names are also reachable through abulxt's edges, see [Edge origin](#edge-origin-abulxts-edges-instead-of-cloudflare). Not managed here:
the host's nginx (:80/:443), cloudflared, the pm2 app krunkbox (:2025, kru.eli.gift), user `kru`.

## Run

```sh
cd deploy/ansible
ansible-galaxy collection install -r requirements.yml
TARGETS="kn_brain kn_migrate" ./build.sh bookworm   # dist/deploy/bookworm/bin (docker)
make -C ../.. web                                   # dist/web
ansible-playbook central.yml --check --diff
ansible-playbook central.yml                        # -e kn_web_sync_assets=false skips the 1 GB asset sync
set -a; . ~/.config/kn-secrets/cloudflare.env; set +a
ansible-playbook edges.yml --tags dns
```

Layout on hosts: `/opt/krunknative-svc/{releases/<id>,current}` (binaries, maps, migrations,
release config), `/opt/krunknative-web/{releases/<id>,current,assets}`, state
`/var/lib/krunknative`, secrets `/etc/krunknative/secrets.env` (generated once, root 0600; also holds the generated
`KN_DATABASE_PASSWORD` / `KN_REDIS_PASSWORD`;
set `kn_brain_internal_secret` from an ansible-vault file to pin it). The last 3 releases are kept.

Outbound mail (kn_brain's email verification, `doc/backend.md` 7.9): `KN_SMTP_HOST`, `KN_SMTP_PORT`,
`KN_SMTP_TLS` (`implicit` for 465, `starttls`, `auto`), `KN_SMTP_USER`, `KN_SMTP_PASSWORD`, `KN_SMTP_FROM`
(default `Krunker Box Office <noreply@caan.edu>`) in the controller's environment become
`/etc/krunknative/smtp.env` (root:kn 0640, written with `no_log`, an optional `EnvironmentFile` of
the kn-brain units). Locally they live in `~/.config/kn-secrets/smtp.env` (mode 600):

```sh
set -a; . ~/.config/kn-secrets/smtp.env; set +a
ansible-playbook central.yml
```

In CI they are repository secrets of the same names (deploy-kru.yml); set them from that file
without printing anything:

```sh
set -a; . ~/.config/kn-secrets/smtp.env; set +a
for k in KN_SMTP_HOST KN_SMTP_PORT KN_SMTP_TLS KN_SMTP_USER KN_SMTP_PASSWORD KN_SMTP_FROM; do
  printf '%s' "${!k}" | gh secret set "$k" --repo <owner>/<repo>
done
```

Unset (no host or password): the file is left as it is and kn_brain sends no mail.

The map editor's AI (`services/brain/ai.h`): `KN_AI_UPSTREAM_URL` (the OpenAI-compatible base, `.../v1`)
and `KN_AI_UPSTREAM_KEY` become `/etc/krunknative/ai.env` the same way (root:kn 0640, `no_log`, an optional
`EnvironmentFile` of the kn-brain units). The key never reaches a client: kn_brain adds it to its own
upstream requests. Locally: `~/.config/kn-secrets/ai.env`; in CI the repository secrets of the same names:

```sh
set -a; . ~/.config/kn-secrets/ai.env; set +a
for k in KN_AI_UPSTREAM_URL KN_AI_UPSTREAM_KEY; do printf '%s' "${!k}" | gh secret set "$k" --repo <owner>/<repo>; done
```

Unset: the file is left as it is; without one the `/ai/*` routes answer 503 and the editor shows "AI
unavailable". After a deploy `curl -s https://coordinator.krunk.io/ai/status` shows the upstream's health
(the brain's known-prompt probe per model: `healthy`, or `degraded` when the provider drops request content;
`health.models` lists each probed model's status, and the default moves to a model that answers).

Web search for the editor AI (role `kn_ai_search`, `services/brain/ai_search.c`): the pinned
`duckduckgo-mcp-server` (`kn_ai_search_package_version`), installed with a pinned `uv`
(`kn_ai_search_uv_version`, pip-installed into its own venv) as a uv tool under `/opt/krunknative-ai`
(root-owned). kn_brain runs it itself as a stdio child (`ai.search.command` in the release config): no
port, no unit of its own, the kn-brain unit's sandbox, and an environment with none of the brain's
secrets. `kn_ai_search_enabled: false` turns it off (the editor then offers no Web chip).
`kn_brain_email_require_verified` (default `[]`, v7.1.4 parity) lists the social features that
need a verified email.

Switches: `kn_coordinator_db` (`sqlite` | `none` | `postgres`; sqlite falls back to `--no-db`
while the shipped kn_brain has no SQLite store; postgres = [Database cutover](#database-cutover-postgresql--redis-on-kru)),
`kn_postgres_enabled` / `kn_redis_enabled` (roles default off; on for kru in `group_vars/central`), `kn_gameserver_enabled` (edges stay down until
true), `kn_gameserver_ports_open` (escape hatch, true everywhere: edge_firewall opens the ports),
`edge_firewall_own_base` (see below).

CI: `.github/workflows/deploy-kru.yml` (manual dispatch on main) builds and runs `central.yml` with
secrets `KRU_SSH_KEY KRU_HOST KRU_PORT KRU_KNOWN_HOSTS`. That includes `kn_bridge` (the bundle is
built on the runner with its Node; the Cloudflare DNS/tunnel step is skipped without CF secrets).

`.github/workflows/deploy-edges.yml` (dispatch only, input `hosts`: `all` or e.g. `chitown,clara`,
`dry_run` = `--check --diff`) builds server_bin for bullseye/bookworm/trixie and runs
`edges.yml --tags release,gameserver`, chitown first, the others only once chitown-1 answers the
server-info query again. It restarts the game servers only when server_bin, its data or its
config changed (below, "Restarts"), draining them. Secrets: `EDGES_SSH_KEY`
(a CI-only ed25519 key, private half in `~/.config/kn-secrets/github-ci/edges_ci_ed25519`; in each
edge's root `authorized_keys` as
`no-agent-forwarding,no-X11-forwarding,no-port-forwarding ssh-ed25519 ... krunknative-github-ci-edges`,
installed next to the other keys, the old file kept as `authorized_keys.bak.krunknative-ci.<time>`),
`EDGES_KNOWN_HOSTS` (the edges' host keys) and the `KRU_*` ones: the coordinator secrets for
`server-<n>.cfg` are read from kru's `secrets.env` at deploy time, as locally. The runner's ssh
config is `ssh_config` with kru -> `KRU_HOST:KRU_PORT` and the edges -> the CI key. Revoke: delete
the `krunknative-github-ci-edges` line on the edges and the secret.

Release builds (`.github/workflows/release.yml`) use the same `build.sh` (bookworm + bullseye,
`TARGETS=server_bin`); `KN_CMAKE_ARGS` passes the build version (`tools/release/version-meta.sh
--server`: the last commit that changed the service sources, with its time as the build time).

### Restarts

A restart drops players, and both deploys run on every push, so a service restarts only when
what it runs changed:

- kn_brain, kn_migrate and server_bin are reproducible container builds stamped with their own
  source commit (`version-meta.sh --server`; two builds of the same tree are byte-identical).
  `kn_svc_release_id` (role kn_release) is `<codename>-<sha256 of the shipped binaries, data and
  migrations>`, and the syncs compare content only (`times: false`). An unchanged build finds its
  release in place: nothing is copied, `current` stays, and no handler fires. `-e
  kn_svc_release_id=` overrides it. `kn_release_id` (the commit) still names the web releases.
- kn-rtc builds with `-buildvcs=false`. Go's default VCS stamping changed the binary on every
  commit, so every push restarted the gateway. net-bridge's esbuild bundle is deterministic.
- Templates (units, env files, `server.cfg`, `config.json`) restart their service when their text
  changes, e.g. `kn_brain_developers`.
- When a game server must restart, all instances of a host drain in parallel (one `systemctl
  restart` call). Each refuses new players, finishes its round (`kn_gameserver_drain_timeout`,
  240 s, as `shutdown_grace`; the unit's `TimeoutStopSec` is 30 s more), and its players get
  "GAME UPDATING" and rejoin. An empty instance restarts at once.
- kn-rtc / kn-bridge restarts write `update` to `kn_gateway_stop_reason_file` first. The
  gateways then tell every player "GAME UPDATING" before they close, and the clients rejoin
  (doc/transport.md "Goodbyes").

Checked with a local harness: two systemd Debian containers (central: kn_user, kn_release,
kn_brain, kn_bridge, kn_rtc; edge: kn_user, kn_release, kn_gameserver), production URLs
overridden. One apply converges. A second apply, then `--check --diff` twice, and an apply after
touching every shipped file (new mtimes, as in a fresh CI checkout) report `changed=0` with no
handler. A changed drain timeout drains a connected player for the old `shutdown_grace`, then
sends GAME UPDATING, while the empty instance restarts at once.

## Edge origin (abulxt's edges instead of Cloudflare)

`assets.`, `coordinator.`, `matchmaker.` and `play.krunk.io` can be served by abulxt's OpenResty
edges instead of the Cloudflare proxy + tunnel: each name is a DNS-only CNAME to
`byod.abstracteducation.loan` (the edges' round-robin, kept healthy by abulxt's watchdog). The
front end (`krunk.io`, `www.`) and the game servers (`<edge>-<n>.krunk.io`) are not part of it.

```
browser -> edge :443 (Let's Encrypt on demand, shared Redis on amph; abulxt edge_relay
           conf.d/20-krunkio.conf, assets. cached there)
        -> https://5.188.124.56:443, SNI kn-origin.krunk.io (the host nginx; meal's firewall
           passes 22/80/443 to kru, so no new port): pinned self-signed certificate, edge /24s
           only, X-KN-Edge-Token, else 403
        -> OpenResty 127.0.0.1:8086 (role openresty): Host -> the static site / :8082 / :8083 /
           :8084 (/rtc -> :8085), client address from X-Real-IP (also passed on as
           CF-Connecting-IP, which kn-bridge and kn-rtc read)
```

Role `kn_edge_origin` (tag `edge_origin`) renders `/etc/nginx/sites-available/krunknative-edge-origin.conf`
(linked as `sites-enabled/zz-krunknative-edge-origin.conf`, after the existing sites, so the
default `:443` server does not change), runs `nginx -t` and puts the previous file back if it
fails. `kn_edge_origin_enabled: false` (`group_vars/central/main.yml`) removes the vhost and the
8086 listener; nothing else changes, the tunnel keeps working. The certificate, its key and the
token live in `/etc/krunknative/edge-origin/` (root 0700). They are generated once on the
controller and copied from `~/.config/kn-secrets/edge-origin/` when that directory exists (CI
keeps what is on kru):

```sh
d=~/.config/kn-secrets/edge-origin; install -d -m 700 $d; cd $d
openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:prime256v1 -nodes -days 3650 \
  -subj /CN=kn-origin.krunk.io -addext subjectAltName=DNS:kn-origin.krunk.io -keyout origin.key -out origin.crt
(umask 077; openssl rand -hex 32 > token)
```

`origin.crt` is abulxt's `roles/edge_relay/files/krunkio-origin.crt`, `token` its vaulted
`krunkio_origin_token`: rotate both repos together. Logs: `/var/log/nginx/krunknative-edge-origin.log`
(browser address, edge, host). The rtc media path does not change: ICE/DTLS goes straight to
`5.188.124.56:21100/udp`, only the `/rtc` signaling POST goes through the edges.

DNS: the krunk.io zone is on Spaceship (`launch1`/`launch2.spaceship.net`, since 2026-10-13) and
every record is kept by hand in Spaceship's DNS panel; nothing in this repository writes DNS or
uses the Cloudflare API for it any more (the cf_dns role and kn_bridge / kn_rtc's tunnel CNAME and
ingress tasks are gone). `edges.yml --tags dns` (role `kn_dns_check`) only checks that every
`<edge>-<n>.krunk.io` resolves to its edge at `launch1.spaceship.net` and prints the records to add
there (a new edge, a raised `kn_gameserver_count`).

## Download host (`dl.krunk.io`)

kru's own public name for the whole game content tree and the updates store, with nothing in
front of it: the source the Pages asset CDN is mirrored from (deploy/pages/assets-cdn.sh), the
clients' fallback (kn-config.json `dl`, the launcher's objects) and where the CDN's default
project (`assets.krunk.io`) 302s `/updates/*` and excluded prefixes (deploy/asset-shards.json
`download`). The launcher's default updates URL is `https://dl.krunk.io/updates/client/latest.json`.

```
client -> dl.krunk.io (A record at Spaceship: 5.188.124.56, kru's public IP; meal passes 80/443)
       -> host nginx :443, Let's Encrypt certificate (certbot --webroot on :80, renewed by
          certbot's timer, deploy hook reloads nginx); GET/HEAD/OPTIONS only; client in X-Real-IP
       -> OpenResty 127.0.0.1:8087 (role openresty, the static_site with CORS): /assets/, /updates/
```

Role `kn_dl_origin` (tag `dl_origin`) renders `/etc/nginx/sites-available/krunknative-dl.conf`
(linked as `sites-enabled/zz-krunknative-dl.conf`, after the existing sites, so the default
servers do not change): first the :80 server (the ACME challenge from
`/var/www/krunknative-acme`, everything else 301 to https), then `certbot certonly --webroot` once
(`creates:` the certificate), then the same file with the :443 server; `nginx -t` before every
reload, the previous file back if it fails. Log: `/var/log/nginx/krunknative-dl.log`.
`kn_dl_origin_enabled: false` (`group_vars/central/main.yml`) removes the vhost and the 8087
listener. Turn it on only after the A record resolves (the first run asks Let's Encrypt; a failure
stops the play):

```sh
dig +short dl.krunk.io @launch1.spaceship.net   # 5.188.124.56
ansible-playbook central.yml --tags openresty,dl_origin -e kn_dl_origin_enabled=true
curl -fsSI https://dl.krunk.io/assets/version.txt; curl -fsSI https://dl.krunk.io/updates/client/latest.json
```

then set `kn_dl_origin_enabled: true` in group_vars (Deploy kru runs central.yml on every push).

## Database cutover (PostgreSQL + Redis on kru)

`doc/backend.md` 3.11 has the design (what Redis holds, failure modes, security, the measurements
and the load test). `group_vars/central/main.yml` sets `kn_postgres_enabled: true`,
`kn_redis_enabled: true` and `kn_coordinator_db: postgres` (the roles' own defaults stay off and
`sqlite`, so other inventories get nothing). The first `central.yml` run with that (CI's Deploy kru
after the push) does, unattended:

| step | role / tool | on failure |
| --- | --- | --- |
| PostgreSQL: our own cluster `<newest installed>/krunknative` on 127.0.0.1:5434 (`pg_createcluster`; kru's stopped 17/main + 18/main are left alone), scram on loopback only, sandboxed unit, role + database `krunknative`, `KN_DATABASE_PASSWORD` generated into `secrets.env` | `kn_postgres` | the play stops; kn_brain still on SQLite |
| Redis: `kn-redis.service` on 127.0.0.1:6380 (the package's 6379 instance masked), ACL user `kn_brain` on `kn:*` only, no persistence, `KN_REDIS_PASSWORD` generated, `/etc/krunknative/redis.env` | `kn_redis` | same |
| the kn-brain units read `/etc/krunknative/<instance>-db.env` (created as the SQLite file they used before) and `redis.env`; restarted, still on SQLite, now with the hot tier | `kn_brain` | same |
| **backup -> stop writers -> migrate -> verify -> switch -> start -> verify** with `kn-db-cutover run` (below) | `kn_db_cutover` | rolls back to SQLite by itself, sets a hold, fails the job |

`kn-db-cutover run` (`roles/kn_db_cutover/files/kn-db-cutover`, log `/var/lib/krunknative/cutover-coord.log`):

1. preflight: `kn_dbcopy` / `kn_migrate` in the release, PostgreSQL login with the generated
   password, the unit reads `coord-db.env`, free disk >= max(1024 MB, 3 x coord.db + 512 MB)
   (`kn_db_cutover_min_free_mb`); otherwise it refuses, sets the hold and fails the job, changing nothing;
2. `kn_migrate` the PostgreSQL schema (kn-brain-coord still serving);
3. stop kn-brain-coord (the only writer: the finder runs `--no-db`, game servers never touch the
   database; they reconnect within their 1-30 s backoff);
4. `sqlite3 coord.db ".backup coord.db.bak-cutover-<time>"`;
5. `kn_dbcopy --sqlite <backup> --replace`: copy in one transaction + verify row counts and checksums
   of every table (report `<backup>.verify.txt`);
6. `coord-db.env` -> postgres (`KN_DATABASE_*`, pool 8), marker `/var/lib/krunknative/.cutover-postgres-coord`;
7. start kn-brain-coord; healthy = `/mm/status` 200 and `/auth/me` with a bogus token 401 (the
   accounts path reached PostgreSQL) within 60 s;
8. any failure in 4-7 restores `coord-db.env` = SQLite, restarts, checks health, writes the hold
   `/var/lib/krunknative/.cutover-hold-coord` (later deploys skip the cutover instead of retrying on
   every push) and exits 1. Measured on the production-sized copy in the container harness:
   **3.6-3.8 s** from stop to healthy (copy + verify 2.6 s of it).

Every later run is a no-op (`CHANGED=0`; a second apply of the whole play reports `changed=0`).

Go / no-go before pushing it: coord.db 7.6 MB and 2.1 GB free on kru (need 1 GB): go, but cap the
journal first if possible ([Disk on kru](#disk-on-kru)); expect ~5 s of coordinator downtime
(logins, joins, social sockets reconnect; matches in progress are not affected); watch the job's
"Cutover log" output.

On kru (root):

```sh
kn-db-cutover status                 # driver in coord-db.env, marker, hold
kn-db-cutover check                  # preflight only
kn-db-cutover rollback --yes         # back to coord.db; sets the hold. Writes made on PostgreSQL since
                                     # the cutover stay in PostgreSQL (not copied back)
rm /var/lib/krunknative/.cutover-hold-coord   # allow the next deploy to try again, or:
kn-db-cutover run --force            # try again now
journalctl -u kn-brain-coord | grep -E 'db: driver|hot:'   # what it runs on
```

Redis can go away at any time: the brains log `hot: redis ... down: serving from the database` and
keep serving (`systemctl stop kn-redis` is safe). To turn the hot tier off for good: set
`kn_redis_enabled: false`, then on kru `rm /etc/krunknative/redis.env && systemctl restart
kn-brain-coord kn-brain-finder`. To leave PostgreSQL: `kn-db-cutover rollback --yes` and
`kn_coordinator_db: sqlite`. Postgres data: `/var/lib/postgresql/<ver>/krunknative`; connect with
`runuser -u postgres -- psql -p 5434 krunknative`.

## Disk on kru

2026-10-13: 2.1 GB free of 32 GB (94 %). Ours: the journal 2.5 GB (back to 2025-07; journald's
default cap is 10 % = 3.2 GB, so it still grows ~0.7 GB), the updates store 2.5 GB (1.9 GB of content
objects are hardlinks of the web asset tree, not extra space; 3 x 221 MB client builds), apt's cache
0.47 GB, `/var/log/btmp.1` 163 MB, releases ~10 MB each (3 kept). Not ours: `/home/kru` 12 GB,
`/home/glorp` 4.2 GB. Proposals, in order of safety:

* `apt-get clean` (0.47 GB, nothing lost): `kn_disk_apt_clean: true` (role `kn_disk`);
* cap the journal: `kn_journald_max_use: 1G` (role `kn_disk`: a journald drop-in + one
  `journalctl --vacuum-size`) frees ~1.5 GB, deleting entries older than a few months;
* keep two client builds in the updates store instead of three (~0.22 GB; `kn-updates` prune setting);
* `/home/kru` (12 GB) and `/home/glorp` belong to others: ask, never clean from here.

The cutover needs about 1 GB (backup + PostgreSQL cluster and WAL, `max_wal_size` 256 MB) and
refuses below that.

## net-bridge (play.krunk.io)

Browsers cannot open TCP, so the web client (Pages, `kn-bridge` meta, deploy/pages) reaches the
edge game servers through net-bridge on kru: `wss://play.krunk.io/native?target=<edge>-<n>.krunk.io:<port>`
(the host/port the matchmaker returned) -> Cloudflare proxy -> tunnel -> `127.0.0.1:8084` ->
TCP to the edge. Role `kn_bridge` (tag `bridge`):

- `net-bridge/index.ts --bridge-only`: only the `/native` WebSocket passthrough and `GET /healthz`;
  no static files, no brain routes, no `/ws`. Shipped as one esbuild bundle (`npm run bundle` in
  net-bridge, run on the controller by the role; esbuild pinned in package-lock) to
  `/opt/krunknative-bridge/net-bridge.js`.
- Node: a private pinned LTS, binary only, in `/opt/krunknative-node/node-v<version>-linux-x64`
  (`kn_bridge_node_version` + `kn_bridge_node_sha256`; installed only when missing). The host's
  `/usr/local/bin/node` (20, EOL, used by the pm2 app) is not used or touched.
- `kn-bridge.service`: user `kn`, kn_brain-style hardening (minus MemoryDenyWriteExecute, V8
  needs W+X), settings in `/etc/krunknative/bridge.env` (root:kn 0640, no secrets).
  The optional internal proxy for krunker.io (`KN_BRIDGE_KRUNKER_PROXY`, net-bridge/krunkerRelay.ts)
  goes in `/etc/krunknative/bridge-proxy.env` (root:kn 0640, not templated, so ansible never writes or
  reads the credentials): one line `KN_BRIDGE_KRUNKER_PROXY=socks5h://user:password@PROXY_HOST:PORT`,
  then `systemctl restart kn-bridge`. Empty or missing = off.
- Allowlist (`KRUNK_NATIVE_TARGETS`), generated from the `edges` inventory: `<edge>-<n>.krunk.io`
  for n = 1..11 and each edge's `kn_public_ip`, ports 21015-21025 (`kn_bridge_target_port_lo/hi`).
  Anything else, including a missing `target`, loopback and other kru ports, is refused with 403
  before the WebSocket handshake. Adding an edge to the inventory and re-running `--tags bridge`
  updates it.
- Limits: 8 sessions per client IP (`CF-Connecting-IP`; cloudflared connects from loopback),
  1024 in total, 90 s idle close, WebSocket ping every 30 s (Cloudflare drops idle sockets after
  100 s), 10 s TCP connect timeout, 128 KiB max message. Refusals are 403/429/503.
- Routing: `play.krunk.io` is a DNS-only CNAME to the abulxt edges (Spaceship DNS, by hand), which
  relay to kru's OpenResty and on to `127.0.0.1:8084` ([Edge origin](#edge-origin-abulxts-edges-instead-of-cloudflare)).

```sh
ansible-playbook central.yml --tags bridge --check --diff
ansible-playbook central.yml --tags bridge
ssh kru journalctl -u kn-bridge -f          # one line per session: client IP -> edge, close reason
curl https://play.krunk.io/healthz           # {"ok":true,"sessions":..,"nativeConnected":..}
```

The edges see every bridged player as kru's address (5.188.124.56): their server_bin only honours
the bridge's FORWARDED frame from loopback, so the bridge sends it only to loopback targets. An
IP ban on an edge would therefore hit all web players; ban by account instead.

## WebRTC gateway (play.krunk.io/rtc)

The web client's preferred transport (doc/transport.md "Web transports"): knet datagrams over
an unordered, unreliable RTCDataChannel to `net-bridge/rtc` (`kn-rtc`, Go + pion/webrtc,
pinned in its go.mod/go.sum), which relays every message 1:1 to the edge game server's UDP
port. Role `kn_rtc` (tag `rtc`; `kn_rtc_enabled: false` stops and disables the unit):

- Signaling: `POST https://play.krunk.io/rtc?target=<edge>-<n>.krunk.io:<port>` with the SDP
  offer, answered with the SDP answer. The request goes abulxt edge -> kru's OpenResty
  (`/rtc` on `play.krunk.io`) -> `127.0.0.1:8085` ([Edge origin](#edge-origin-abulxts-edges-instead-of-cloudflare)).
- Media: ICE-lite plus DTLS/SCTP straight to `5.188.124.56:21100/udp` (`kn_rtc_public_ip`,
  `kn_rtc_ice_port`). This is one UDP port for every session (ICE UDP mux), bound to that
  address only. The role asserts that the address is on the host. No certificate is needed and
  no STUN/TURN: the gateway has a public address, and browsers behind any NAT reach it.
- `kn-rtc.service`: user `kn`, kn_brain hardening, plus `AF_NETLINK` (pion lists the
  interfaces). The static binary is built on the controller (`go build`, CGO off, reproducible)
  into `/opt/krunknative-rtc/kn-rtc`. Settings are in `/etc/krunknative/rtc.env` (root:kn 0640,
  no secrets).
- Allowlist and limits mirror kn_bridge: the edges by name and IP, ports 21015-21025, no
  loopback and no default target. Also 8 sessions per `CF-Connecting-IP`, 1024 in total, 10 s
  for ICE + DTLS + the channel, and 30 s without a datagram from the edge. CORS and Origin are
  limited to `kn_web_origins`.
- Host firewall: kru's nftables is Debian's default accept-all ruleset (not managed here), so
  no rule is needed on the host.
- Upstream firewall: kru is CT 120 on the Proxmox host `meal`, whose PVE firewall rejects
  inbound by default. `/etc/pve/firewall/120.fw` carries `IN ACCEPT -p udp -dport 21100`
  (added 2026-10-11, not managed here). Moving the port means changing that rule too. To
  check from outside, send UDP to another port and watch `nstat -az UdpNoPorts` on kru.

```sh
set -a; . ~/.config/kn-secrets/cloudflare.env; set +a
ansible-playbook central.yml --tags rtc --check --diff
ansible-playbook central.yml --tags rtc
ssh kru journalctl -u kn-rtc -f     # one line per session: client -> edge, close reason, datagram counters
curl https://play.krunk.io/rtc -X OPTIONS -H 'Origin: https://krunk.io' -i   # 204 + CORS headers
```

The edges see WebRTC players as kru's address, as with the bridge (forwarded addresses are
trusted from loopback only). Phase 2, a gateway on each edge, removes both that and the extra
hop (doc/transport.md).

## Krunker Strike servers

Every edge also runs official Krunker Strike lobbies (role `kn_gameserver`, `tasks/strike.yml`),
next to the Box Office instances, whose units, `server-<n>.cfg` and ports stay exactly as they
were (`deploy/ansible/tests/render_test.py` renders both). Each is `server_bin --headless --no-cli
--flavor strike --cfg /etc/krunknative/strike-<n>.cfg` (unit `kn-strike@<n>`), playing the Strike
rotation over FRVR's pool maps (`assets/maps/strike/`, shipped with `assets/maps`), linked to the
same coordinator (`wss://coordinator.krunk.io/mm/internal/ws`, the same secrets) and heartbeating to
the same finder. The coordinator keeps it in the Strike flavor pool (doc/backend.md "Flavor pools"):
the Strike app seeks, lists and joins only these, Box Office never sees them.

| | Box Office (unchanged) | Krunker Strike |
| --- | --- | --- |
| count | vcpus - 1 (min 1) | `kn_strike_count`: 2 with 8+ vcpus, else 1 (group_vars/edges), fewer when memory is short (below) |
| port (tcp game + udp knet / query) | 21015 + n - 1 | `kn_strike_first_port` 21115 + n - 1 (group_vars/all, span `kn_strike_port_span` 4) |
| node id / name | `CHITOWN2`, `Krunker Box Office Chitown #2` | `CHITOWNSTRIKE1`, `Krunker Strike Chitown #1` |
| public host | `<edge>-<n>.krunk.io` | `<edge>-1.krunk.io` (the existing A record: no DNS change) |
| lobby | 8, Krunker's rotation, bots to 8 | 10 (Strike's cap), the Strike rotation, bots to 10 |
| unit limits | CPUWeight 80 | CPUWeight 50, MemoryHigh 192M, MemoryMax 256M, no swap |
| state / stop reason | `gs-<n>`, `gameserver-stop-reason` | `strike-<n>`, `strike-stop-reason` |
| restart handler | `Restart game servers` | `Restart Strike servers` (same drain, `kn_gameserver_drain_timeout`) |

Memory budget per host (`kn_strike_memory_*`, `kn_gameserver_memory_estimate_mb`): a Strike
instance starts only while `1024 (OS + abulxt's relays) + Box Office count x 160 + Strike count x
256 <= MemTotal`; the play prints `Strike instance plan` with the numbers, and a host that cannot
hold its planned count starts fewer (none: the firewall still opens 21115, nothing listens). An idle
server_bin is ~100 MB RSS on one map; the 256 MB cap is a hard limit (systemd kills and restarts
that instance), so a Strike lobby can never take memory from the Box Office servers. Off switch:
`kn_strike_enabled: false` (group_vars/edges; the units are stopped and their configs removed, like
stale Box Office instances). net-bridge allows `<edge>-1.krunk.io` and the edge IP on 21115-21118
for the web Strike app (`kn_bridge_strike`). Deploy edges checks `<edge>-1.krunk.io:21115` after
the Box Office health check and only warns when it does not answer as a Strike server.

## Edge firewall

The edges run abulxt's OpenResty relays and Krunker Box Office's game servers, so their host
firewall is a shared contract, **edge-firewall-contract v1**, implemented by one role.

**`roles/edge_firewall` is vendored**: the same role ships byte-identical in abulxt and Krunker Box Office
(both at `deploy/ansible/roles/edge_firewall`). Never edit one copy alone: change both repos
together, re-stamp with `roles/edge_firewall/check-vendored.sh --stamp` (rewrites `VERSION`:
contract version + sha256 of the role), then check both copies:

```sh
roles/edge_firewall/check-vendored.sh ~/jails/unblocker/src/abulxt/deploy/ansible/roles/edge_firewall
```

Both repos call the role the same way: directly from their playbooks (`firewall.yml` alone,
and their full edge play with tag `firewall`), with all inputs in group_vars, the same
`edge_firewall_*` variables in the same order (this repo: `group_vars/edges/main.yml`):

- `edge_firewall_own_base`: render the base `/etc/nftables.conf`. Exactly one repo sets true
  (now abulxt). Base: `flush ruleset`; `table inet filter` with interval sets `tcp_accept` /
  `udp_accept`; input policy drop (lo, established/related, icmp + ipv6-icmp, tcp
  `edge_firewall_ssh_port` 22, `@tcp_accept`, `@udp_accept`); forward drop; output accept;
  then `include "/etc/nftables.d/*.nft"`. `edge_firewall_owner` fills its `# Owner:` line.
- `edge_firewall_dropins`: this repo's drop-in, `{name, priority, source, comment, tcp,
  udp}`, rendered to `/etc/nftables.d/<priority>-<name>.nft`; it only adds elements to the
  two sets. abulxt: `50-abulxt.nft` (tcp 80/443/8080, udp 443). Krunker Box Office:
  `60-krunknative.nft` (game ports 21015..21015+count-1, tcp and udp). Port lists must not
  overlap between repos (nft 0.9.8 on santaclara/clara rejects duplicate interval elements);
  an empty list renders no line.
- `edge_firewall_manage` (role off when false), `edge_firewall_ssh_port`,
  `edge_firewall_rollback_seconds`.

This repo: the drop-in's ranges are `kn_gameserver_port_range`, derived from
`kn_gameserver_first_port` / `kn_gameserver_count` in the same file, which `kn_gameserver`
also uses, so the open range always matches the instances (today clara/chitown/ashburn
21015-21017, angeles 21015-21025), then `kn_strike_port_range` for the Krunker Strike
instances (21115, angeles 21115-21116). Verify with `nc -zv <ip> 21015` / `nc -zuv <ip> 21015`
(and 21115).

Safe apply (`roles/edge_firewall/tasks/apply.yml`), only when a rendered file changed: the files are
tarred to `/etc/nftables.rollback.tgz` before rendering; `nft -c -f /etc/nftables.conf` (on
failure the files are restored and the play fails); the live ruleset is saved to
`/etc/nftables.rollback.nft`; a transient `edge-firewall-rollback.timer` is armed (120 s)
that reloads it and restores the files; nftables is enabled and reloaded; the SSH
ControlMaster is dropped and Ansible logs in again on a fresh connection; the timer is
disarmed. If the fresh login fails the play fails and the timer rolls back. Each base render
also leaves a timestamped `/etc/nftables.conf.<pid>.<date>@<time>~`.

One firewall command per repo (from `deploy/ansible`; add `--check --diff` first, and
`--limit <host>` to go one host at a time):

| repo | command |
| --- | --- |
| abulxt | `ansible-playbook playbooks/firewall.yml --vault-password-file <file>` |
| Krunker Box Office | `ansible-playbook firewall.yml` |

### Handover (who owns the base)

Exactly one repo has `edge_firewall_own_base: true` (now abulxt). To move the base, change
both repos in the same change window (abulxt `inventory/group_vars/edge.yml`, Krunker Box Office
`group_vars/edges/main.yml`):

1. New owner `edge_firewall_own_base: true`, old owner `edge_firewall_own_base: false`.
2. New owner's firewall command with `--check --diff`: only the base's `# Owner:` line
   changes. Then run it (same rules, so the reload changes no traffic).
3. Old owner's firewall command with `--check --diff` must show no change, then run it (it
   now manages only its drop-in).

Never let both repos own the base at the same time (they would fight over the `# Owner:`
line).

## fail2ban

Every host of both repos (the four edges, amph, kru) runs fail2ban under one shared contract,
**fail2ban-contract v1**, implemented by one role. **`roles/fail2ban` is vendored** exactly like
`roles/edge_firewall`: byte-identical in abulxt and Krunker Box Office (`deploy/ansible/roles/fail2ban`),
re-stamped with `roles/fail2ban/check-vendored.sh --stamp` after an edit, then checked:

```sh
roles/fail2ban/check-vendored.sh ~/jails/unblocker/src/abulxt/deploy/ansible/roles/fail2ban
```

Per host exactly one repo owns the base (`fail2ban_own_base: true`): abulxt on the edges and
amph, Krunker Box Office on kru. Both repos set the same `fail2ban_*` variables in the same order
(this repo: `group_vars/all.yml`; kru's ownership, approval gate and sshd settings in `group_vars/central/main.yml`), and keep `fail2ban_ignoreip` identical.

- Base (owner only): `/etc/fail2ban/jail.local` with `[DEFAULT]` ignoreip (127.0.0.1/8, ::1,
  `fail2ban_ignoreip`, the host's own addresses, optionally GitHub's Actions ranges), backend
  systemd, ban actions `nftables[type=multiport]` / `nftables[type=allports]`,
  `bantime.increment` (each repeat ban doubles, capped at `fail2ban_bantime_maxtime` 1w);
  `[sshd]` (mode normal, port 22, journalmatch for ssh.service / sshd / sshd-session, 5
  failures in 10m = 1h ban); `[recidive]` (5 bans in 1d = every TCP port for 1w).
  `fail2ban.d/50-fail2ban-contract.local` (`dbpurgeage` 8d, longer than any ban, so a restart
  keeps them), the nftables.service hook below, the optional sshd drop-in, the packages
  (fail2ban, python3-systemd, nftables for the `nft` binary: Debian never enables
  nftables.service on install and the role never does) and `fail2ban_retire_files`.
- Drop-ins (every repo): `fail2ban_dropins` entries `{name, priority, source, comment,
  filters, jails}` render `/etc/fail2ban/jail.d/<priority>-<name>.local` (+
  `filter.d/<name>-<filter>.conf`). Jails inherit ignoreip, ban action and times from the
  base and never redefine sshd or recidive. Today neither repo has one: no HTTP jails, because
  edge clients share school and carrier NAT addresses, and amph's HTTP arrives via Cloudflare
  and the edges. A non-owner run on a host without fail2ban only prints a note.
- `fail2ban_sshd_settings` (owner only, `{}` = none): `/etc/ssh/sshd_config.d/10-fail2ban-contract.conf`.
  sshd keeps the first value it reads, so it beats `sshd_config` and `11-*`..`99-*`, not `00-*`.
  Edges and kru: `MaxStartups 50:30:200` (scanners filled 10:30:100 and sshd dropped CI's
  connections) and `LoginGraceTime 30`. amph: none.

**Firewall coexistence.** Bans live in fail2ban's own nftables table `inet f2b-table` (hook
input, priority -1), which runs before edge_firewall's `inet filter` and iptables-nft (both
priority 0), whatever those accept. The edge_firewall base starts with `flush ruleset`, which
drops that table and its bans; the base's `/etc/systemd/system/nftables.service.d/50-fail2ban-contract.conf`
(`ExecReload=` / `ExecStartPost=` `systemctl --no-block try-restart fail2ban`) restarts fail2ban
after every load, and fail2ban restores the bans from its database. The edge_firewall rollback
(`nft -f /etc/nftables.rollback.nft`) puts the table back as snapshotted (it is part of
`nft list ruleset`; `nft -c` accepts it on nft 0.9.8 and 1.1.3). After any other flush,
fail2ban's next ban fails `actioncheck`, recreates the table and re-bans. edge_firewall itself
is unchanged.

**Lockout.** ignoreip covers the admin workstation's egress, amph (the byod watchdog SSHes
into every edge from 5.188.124.55), kru, meal, the edges, the private ranges and each host's own
addresses. CI logs in with keys: in mode normal, key logins, pre-auth closes and MaxStartups
drops never count as failures, so GitHub's runners are not listed.
`fail2ban_ignoreip_github_actions: true` adds api.github.com/meta "actions" (about 7000
CIDRs, fetched on the controller with its `GITHUB_TOKEN` if set, since anonymous requests are
limited to 60/hour; fail2ban restarts whenever GitHub edits the list); turn it on before
`fail2ban_sshd_mode: aggressive`, which also counts pre-auth closes. recidive blocks every
TCP port of an address, the web and game ports included. Unban: `fail2ban-client unban <ip>`.

Safe apply, only when a rendered file changed: `/etc/fail2ban` is tarred to
`/etc/fail2ban.rollback.tgz` before rendering; `fail2ban-client -t` (on failure the files are
restored, a running fail2ban keeps its config, the play fails); then the owner restarts
fail2ban (a non-owner reloads it) and `fail2ban-client status sshd` must answer. The sshd
drop-in: `sshd -t`, a transient `fail2ban-sshd-rollback.timer` (120 s) that restores the
previous file, `systemctl reload ssh` (open sessions stay), a fresh SSH login, disarm.

One fail2ban command per repo (from `deploy/ansible`; add `--check --diff` first, and
`--limit <host>` to go one host at a time):

| repo | command |
| --- | --- |
| abulxt | `ansible-playbook playbooks/fail2ban.yml --vault-password-file <file>` |
| Krunker Box Office | `ansible-playbook fail2ban.yml` |

Handover of a host's base works like the firewall's (above): flip `fail2ban_own_base` in both
repos together, run the new owner first (only `# Owner:` lines change), then the old owner
(no change).

This repo: on the edges the role only adds drop-ins (none today), so `edges.yml` and the
Deploy edges workflow (tag `fail2ban`) change nothing there. On kru it owns the base but stays
off (`fail2ban_manage: false` in `group_vars/central/main.yml`) until the user approves it,
because Deploy kru runs `central.yml` on every push. First apply by hand, then set it true:

```sh
ansible-playbook fail2ban.yml --limit kru -e fail2ban_manage=true --check --diff
ansible-playbook fail2ban.yml --limit kru -e fail2ban_manage=true
```
