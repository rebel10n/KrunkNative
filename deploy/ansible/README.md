# deploy/ansible

Ansible for the KrunkNative central node (`kru`) and the edge game servers.

| playbook | hosts | what |
| --- | --- | --- |
| `central.yml` | `kru` | user `kn`, OpenResty 127.0.0.1:8081 (static web build), kn_brain finder :8082 + auth/community :8083, net-bridge :8084 (`play.krunk.io`), `/opt/krunknative` clone |
| `edges.yml` | `edges` | host firewall drop-in (game ports), user `kn`, `server_bin` x (vcpus-1, min 1), `<edge>-<n>.krunk.io` DNS |
| `firewall.yml` | `edges` | only the host firewall (vendored `edge_firewall` role), see [Edge firewall](#edge-firewall) |
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
true), `kn_gameserver_ports_open` (escape hatch, true everywhere: edge_firewall opens the ports),
`edge_firewall_own_base` (see below).

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

The edges run abulxt's OpenResty relays and KrunkNative's game servers, so their host
firewall is a shared contract, **edge-firewall-contract v1**, implemented by one role.

**`roles/edge_firewall` is vendored**: the same role ships byte-identical in abulxt and KrunkNative
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
  two sets. abulxt: `50-abulxt.nft` (tcp 80/443/8080, udp 443). KrunkNative:
  `60-krunknative.nft` (game ports 21015..21015+count-1, tcp and udp). Port lists must not
  overlap between repos (nft 0.9.8 on santaclara/clara rejects duplicate interval elements);
  an empty list renders no line.
- `edge_firewall_manage` (role off when false), `edge_firewall_ssh_port`,
  `edge_firewall_rollback_seconds`.

This repo: the drop-in's range is `kn_gameserver_port_range`, derived from
`kn_gameserver_first_port` / `kn_gameserver_count` in the same file, which `kn_gameserver`
also uses, so the open range always matches the instances (today clara/chitown/ashburn
21015-21017, angeles 21015-21025). Verify with `nc -zv <ip> 21015` / `nc -zuv <ip> 21015`.

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
| KrunkNative | `ansible-playbook firewall.yml` |

### Handover (who owns the base)

Exactly one repo has `edge_firewall_own_base: true` (now abulxt). To move the base, change
both repos in the same change window (abulxt `inventory/group_vars/edge.yml`, KrunkNative
`group_vars/edges/main.yml`):

1. New owner `edge_firewall_own_base: true`, old owner `edge_firewall_own_base: false`.
2. New owner's firewall command with `--check --diff`: only the base's `# Owner:` line
   changes. Then run it (same rules, so the reload changes no traffic).
3. Old owner's firewall command with `--check --diff` must show no change, then run it (it
   now manages only its drop-in).

Never let both repos own the base at the same time (they would fight over the `# Owner:`
line).
