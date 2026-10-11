# deploy/ansible

Ansible for the KrunkNative central node (`kru`) and the edge game servers.

| playbook | hosts | what |
| --- | --- | --- |
| `central.yml` | `kru` | user `kn`, OpenResty 127.0.0.1:8081 (static web build), kn_brain finder :8082 + auth/community :8083, net-bridge :8084 (`play.krunk.io`), `/opt/krunknative` clone |
| `edges.yml` | `edges` | host firewall drop-in (game ports), user `kn`, `server_bin` x (vcpus-1, min 1), `<edge>-<n>.krunk.io` DNS |
| `firewall.yml` | `edges` | only the host firewall (`kn_firewall` -> vendored `edge_firewall`), see [Edge firewall](#edge-firewall) |
| `site.yml` | all | both |

Tags: `user openresty repo release brain web bridge gameserver dns firewall`.

Routing on kru (Cloudflare tunnel ingress, managed in Cloudflare): `assets.krunk.io -> :8081`,
`matchmaker.krunk.io -> :8082`, `coordinator.krunk.io -> :8083`, `play.krunk.io -> :8084` (the
last one is kept by the `kn_bridge` role, see [net-bridge](#net-bridge-playkrunkio)). The page itself (`krunk.io`,
`www.`) is on Cloudflare Pages: `make pages`, see `deploy/pages/README.md` (also caching and
the krunker.party redirect: the site 301s, `coordinator.`/`matchmaker.krunker.party` keep
working for old clients and are not redirected). Not managed here:
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
`/var/lib/krunknative`, secrets `/etc/krunknative/secrets.env` (generated once, root:kn 0640;
set `kn_brain_internal_secret` from an ansible-vault file to pin it). The last 3 releases are kept.

Switches: `kn_coordinator_db` (`sqlite` | `none` | `postgres`; sqlite falls back to `--no-db`
while the shipped kn_brain has no SQLite store), `kn_gameserver_enabled` (edges stay down until
true), `kn_gameserver_ports_open` (escape hatch, true everywhere: kn_firewall opens the ports),
`kn_firewall_own_base` (see below).

CI: `.github/workflows/deploy-kru.yml` (push to slop) builds and runs `central.yml` with
secrets `KRU_SSH_KEY KRU_HOST KRU_PORT KRU_KNOWN_HOSTS`. That includes `kn_bridge` (the bundle is
built on the runner with its Node; the Cloudflare DNS/tunnel step is skipped without CF secrets).

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
- Allowlist (`KRUNK_NATIVE_TARGETS`), generated from the `edges` inventory: `<edge>-<n>.krunk.io`
  for n = 1..11 and each edge's `kn_public_ip`, ports 21015-21025 (`kn_bridge_target_port_lo/hi`).
  Anything else, including a missing `target`, loopback and other kru ports, is refused with 403
  before the WebSocket handshake. Adding an edge to the inventory and re-running `--tags bridge`
  updates it.
- Limits: 8 sessions per client IP (`CF-Connecting-IP`; cloudflared connects from loopback),
  1024 in total, 90 s idle close, WebSocket ping every 30 s (Cloudflare drops idle sockets after
  100 s), 10 s TCP connect timeout, 128 KiB max message. Refusals are 403/429/503.
- Cloudflare (controller side, needs `CF_API_TOKEN` + `CF_ACCOUNT_ID` from
  `~/.config/kn-secrets/cloudflare.env`; skipped with a note without them, e.g. in CI): DNS
  `play.krunk.io` CNAME `<tunnel id>.cfargotunnel.com` (proxied), and the tunnel ingress rule
  `play.krunk.io -> http://localhost:8084`. The tunnel is remote-managed (cloudflared runs with
  a token), so the role reads the configuration over the API and PUTs it back with the rule
  replaced in place or inserted just before the catch-all; every other rule is kept, and nothing
  is written when it is already there.

```sh
set -a; . ~/.config/kn-secrets/cloudflare.env; set +a
ansible-playbook central.yml --tags bridge --check --diff
ansible-playbook central.yml --tags bridge
ssh kru journalctl -u kn-bridge -f          # one line per session: client IP -> edge, close reason
curl https://play.krunk.io/healthz           # {"ok":true,"sessions":..,"nativeConnected":..}
```

The edges see every bridged player as kru's address (5.188.124.56): their server_bin only honours
the bridge's FORWARDED frame from loopback, so the bridge sends it only to loopback targets. An
IP ban on an edge would therefore hit all web players; ban by account instead.

## Edge firewall

The edges are shared with abulxt (OpenResty relays on 80/443/8080, wispurr on loopback
:6001). Their host firewall follows **edge-firewall-contract v1**.

**`roles/edge_firewall` is vendored**: the same role ships byte-identical in abulxt
(`deploy/ansible/roles/edge_firewall`). Never edit one copy alone: change both repos together,
re-stamp with `roles/edge_firewall/check-vendored.sh --stamp` (rewrites
`roles/edge_firewall/VERSION`: contract version + sha256 of the role), then check both copies:

```sh
roles/edge_firewall/check-vendored.sh ~/jails/unblocker/src/abulxt/deploy/ansible/roles/edge_firewall
```

The role owns the whole contract (base template, drop-in rendering, safe apply); each repo only
feeds it `edge_firewall_*` inputs. Here `roles/kn_firewall` is a thin wrapper that imports it
with `kn_firewall_*` values:

- `/etc/nftables.conf`, the base, has exactly ONE owner (`edge_firewall_own_base`, here from
  `kn_firewall_own_base`): `flush ruleset`; `table inet filter` with interval sets
  `tcp_accept` / `udp_accept` (auto-merge); input policy drop (lo, established/related, icmp +
  ipv6-icmp, tcp 22, `tcp dport @tcp_accept`, `udp dport @udp_accept`); forward drop; output
  accept; then `include "/etc/nftables.d/*.nft"`.
- `edge_firewall_dropins` (`{name, priority, tcp, udp}` plus optional `source`/`comment`
  header text) render `/etc/nftables.d/<priority>-<name>.nft`, which only add set elements:
  abulxt `50-abulxt.nft` (tcp 80/443/8080, udp 443), here `60-krunknative.nft`:
  `kn_gameserver_first_port`..+`kn_gameserver_count`-1 (`kn_firewall_game_ports`), tcp AND
  udp (game TCP, knet UDP, server-info UDP). Both vars live in `group_vars/edges/main.yml` and
  are shared with `kn_gameserver`, so the open range always matches the instances. Today:
  clara/chitown/ashburn 21015-21017, angeles 21015-21025.
- Port lists must not overlap between drop-ins (nft 0.9.8 on clara rejects duplicate interval
  elements); an empty list renders no line (`{ }` is a syntax error).
- `kn_firewall_own_base: false` (now): abulxt renders the base; this repo only writes
  `60-krunknative.nft` and reloads. If the base is not a v1 base yet, the drop-in is written
  but not loaded (the base owner's play loads it).

Safe apply (`roles/edge_firewall/tasks/apply.yml`), only when a rendered file changed: the
files are tarred to `/etc/nftables.rollback.tgz` before rendering; `nft -c -f
/etc/nftables.conf` (on failure the files are restored and the play fails); the live ruleset
is saved to `/etc/nftables.rollback.nft`; a transient `edge-firewall-rollback.timer` is armed
(120 s, `kn_firewall_rollback_seconds`) that reloads it and restores the files; nftables is
enabled and reloaded; the SSH ControlMaster is dropped and Ansible logs in again on a fresh
connection; the timer is disarmed. If the fresh login fails the play fails and the timer rolls
back.

```sh
ansible-playbook firewall.yml --check --diff
ansible-playbook firewall.yml --limit com.chitown     # one host first, then the rest
ssh com.chitown nft list ruleset
nc -zv <ip> 21015; nc -zuv <ip> 21015                 # refused (not a timeout) = open, no server
```

### Handover (KrunkNative owns the base)

Flip both repos in the same change window and run KrunkNative first:

1. here: `kn_firewall_own_base: true` in `group_vars/edges/main.yml`;
   abulxt: `edge_firewall_own_base: false` in `inventory/group_vars/edge.yml`.
2. `ansible-playbook firewall.yml --check --diff`: only the base's `# Owner:` line changes.
3. `ansible-playbook firewall.yml` (same rules, so the reload changes no traffic).
4. abulxt: `ansible-playbook playbooks/edges.yml --tags firewall --check --diff` must show no
   change to `/etc/nftables.conf`, then run it for real (it now manages only `50-abulxt.nft`).

Handing it back is the reverse (abulxt `edge_firewall_own_base: true`, `kn_firewall_own_base: false`, run
abulxt first). Never let both repos own the base at the same time. Adding a KrunkNative port
outside the game range: add it to the drop-in in `roles/kn_firewall/tasks/main.yml`, keeping it
disjoint from abulxt's.
