# deploy/ansible

Ansible for the KrunkNative central node (`kru`) and the edge game servers.

| playbook | hosts | what |
| --- | --- | --- |
| `central.yml` | `kru` | user `kn`, OpenResty 127.0.0.1:8081 (static web build), kn_brain finder :8082 + auth/community :8083, net-bridge :8084 (`play.krunk.io`), WebRTC gateway :8085 + udp 21100 (`play.krunk.io/rtc`), `/opt/krunknative` clone |
| `edges.yml` | `edges` | host firewall drop-in (game ports), user `kn`, `server_bin` x (vcpus-1, min 1), `<edge>-<n>.krunk.io` DNS |
| `firewall.yml` | `edges` | only the host firewall (vendored `edge_firewall` role), see [Edge firewall](#edge-firewall) |
| `fail2ban.yml` | `kru`, `edges` | only fail2ban (vendored `fail2ban` role; base on kru, drop-ins on the edges), see [fail2ban](#fail2ban) |
| `site.yml` | all | both |

Tags: `user openresty repo release brain web updates bridge rtc gameserver dns firewall fail2ban`.

Client updates (`kn_updates`, doc/backend.md 9.8): `https://assets.krunk.io/updates/` serves
`/opt/krunknative-updates/public/` (OpenResty `kn.d/updates.conf`): the channel manifest CI's
`client.yml` job `updates` publishes with `kn-updates publish`, the update packages and the game
content as hardlinked objects of the web asset tree. Off until `kn_updates_enabled: true`
(`group_vars/central/main.yml`; Deploy kru runs central.yml on every push). Enable it there, apply
with `ansible-playbook central.yml --tags openresty,updates` (no full deploy needed), then set the
repository variable `KN_UPDATES_UPLOAD=1`.

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

Outbound mail (kn_brain's email verification, `doc/backend.md` 7.9): `KN_SMTP_HOST`, `KN_SMTP_PORT`,
`KN_SMTP_TLS` (`implicit` for 465, `starttls`, `auto`), `KN_SMTP_USER`, `KN_SMTP_PASSWORD`, `KN_SMTP_FROM`
(default `Krunker <noreply@caan.edu>`) in the controller's environment become
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
`kn_brain_email_require_verified` (default `[]`, v7.1.4 parity) lists the social features that
need a verified email.

Switches: `kn_coordinator_db` (`sqlite` | `none` | `postgres`; sqlite falls back to `--no-db`
while the shipped kn_brain has no SQLite store), `kn_gameserver_enabled` (edges stay down until
true), `kn_gameserver_ports_open` (escape hatch, true everywhere: edge_firewall opens the ports),
`edge_firewall_own_base` (see below).

CI: `.github/workflows/deploy-kru.yml` (manual dispatch on main) builds and runs `central.yml` with
secrets `KRU_SSH_KEY KRU_HOST KRU_PORT KRU_KNOWN_HOSTS`. That includes `kn_bridge` (the bundle is
built on the runner with its Node; the Cloudflare DNS/tunnel step is skipped without CF secrets).

`.github/workflows/deploy-edges.yml` (dispatch only, input `hosts`: `all` or e.g. `chitown,clara`,
`dry_run` = `--check --diff`) builds server_bin for bullseye/bookworm/trixie and runs
`edges.yml --tags release,gameserver`, chitown first, the others only once chitown-1 answers the
server-info query again. It restarts the selected edges' game servers. Secrets: `EDGES_SSH_KEY`
(a CI-only ed25519 key, private half in `~/.config/kn-secrets/github-ci/edges_ci_ed25519`; in each
edge's root `authorized_keys` as
`no-agent-forwarding,no-X11-forwarding,no-port-forwarding ssh-ed25519 ... krunknative-github-ci-edges`,
installed next to the other keys, the old file kept as `authorized_keys.bak.krunknative-ci.<time>`),
`EDGES_KNOWN_HOSTS` (the edges' host keys) and the `KRU_*` ones: the coordinator secrets for
`server-<n>.cfg` are read from kru's `secrets.env` at deploy time, as locally. The runner's ssh
config is `ssh_config` with kru -> `KRU_HOST:KRU_PORT` and the edges -> the CI key. Revoke: delete
the `krunknative-github-ci-edges` line on the edges and the secret.

Release builds (`.github/workflows/release.yml`) use the same `build.sh` (bookworm + bullseye,
`TARGETS=server_bin`); `KN_CMAKE_ARGS` passes the build version (`tools/release/version-meta.sh`).

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

## WebRTC gateway (play.krunk.io/rtc)

The web client's preferred transport (doc/transport.md "Web transports"): knet datagrams over
an unordered, unreliable RTCDataChannel to `net-bridge/rtc` (`kn-rtc`, Go + pion/webrtc,
pinned in its go.mod/go.sum), which relays every message 1:1 to the edge game server's UDP
port. Role `kn_rtc` (tag `rtc`; `kn_rtc_enabled: false` stops and disables the unit and leaves
the tunnel alone):

- Signaling: `POST https://play.krunk.io/rtc?target=<edge>-<n>.krunk.io:<port>` with the SDP
  offer, answered with the SDP answer. The request goes Cloudflare proxy -> tunnel ->
  `127.0.0.1:8085`. The tunnel ingress gets one path rule, `play.krunk.io` + `^/rtc(/|$)` ->
  `http://localhost:8085`, inserted just before the `play.krunk.io` -> :8084 bridge rule. Every
  other rule is kept, and kn_bridge's ingress task leaves path rules alone. The same
  `CF_API_TOKEN` / `CF_ACCOUNT_ID` are needed as for kn_bridge; without them the step is skipped.
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

## fail2ban

Every host of both repos (the four edges, amph, kru) runs fail2ban under one shared contract,
**fail2ban-contract v1**, implemented by one role. **`roles/fail2ban` is vendored** exactly like
`roles/edge_firewall`: byte-identical in abulxt and KrunkNative (`deploy/ansible/roles/fail2ban`),
re-stamped with `roles/fail2ban/check-vendored.sh --stamp` after an edit, then checked:

```sh
roles/fail2ban/check-vendored.sh ~/jails/unblocker/src/abulxt/deploy/ansible/roles/fail2ban
```

Per host exactly one repo owns the base (`fail2ban_own_base: true`): abulxt on the edges and
amph, KrunkNative on kru. Both repos set the same `fail2ban_*` variables in the same order
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
| KrunkNative | `ansible-playbook fail2ban.yml` |

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
