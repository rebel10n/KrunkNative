# Command and route authorisation audit (2026-10-13)

The question was: are commands properly authenticated? Specifically, is every privileged or
player-targeting command authorised on the server against the sender's real identity, and can
it be spoofed, replayed or escalated?

**Scope:** the chat/console commands, the server CLI and rcon, the client console, the
join/auth handshake and kn_brain.

**Method:**
- Read the code and wrote tests; no live service was probed.
- Line numbers refer to the tree after the fixes listed here.
- Test: `make command-auth-e2e` (`server/tests/command_auth_e2e.py`).

Verdicts:
- **OK**: sound as written.
- **FIXED**: was WEAK or BROKEN; fixed in this change, and the row says how.
- **WEAK** / **BROKEN**: still open; see the end of this file for why.

## Where identity comes from (game server)

A chat command runs for `ChatPeer`, the server's own record of the connection id that sent the
packet (`net_chat_peer_at`, server/src/net.c:907). No packet field can set any of its identity:

| Field | Derived from | Evidence |
|---|---|---|
| `dev` | `client->op` (CLI `op`, config `ops`), `client->local` (TCP peer 127/8, or a `NET_PACKET_FORWARDED` claim only a loopback peer may make before anything else), `NET_PLAYER_INFO_DEV` (the brain's account row for a verified ticket / session) | net.c:856, net.c:1006, net.c:1308, accounts.c:1318, accounts.c:1729 |
| `premium_host` | the connection's verified `account_id` == the brain's `host_account_id` for this custom lobby | accounts.c:955, accounts.c:2071 |
| `account_id` | an HMAC join ticket (constant-time check, expiry, `nid` and `lid` bound to this server and lobby), a session token the brain verifies, or online-mode `hasJoined`; 0 = guest | accounts.c:1643-1662, services/common/src/crypto.c:409 |
| `ops` by name | only for an account (`account_id > 0`): a guest can't take an op's nickname | config.c:1066 |

The client's own state never takes part: no role or host byte, no name, no lobby meta. Ranked
seats come from the brain's match config.

A live sign-in (`NET_EXT_KN_ACCOUNT_AUTH`, doc/backend.md 8.3 item 9) changes `account_id` (and
with it `dev`, `ops` by name and `premium_host`) only after the same verification as a JOIN's
(session token through the brain, or hasJoined); the packet carries a credential, never a claim.
Forged / expired: `REFUSED`, nothing changes; 6 a minute per connection; banned: kicked; logged
as `[rights]`.

## Chat / console commands (`NET_PACKET_CHAT_SEND` 8, `NET_PACKET_CHAT_V7` 245)

| Command(s) | Gate | Before | After | Evidence |
|---|---|---|---|---|
| kick, ban, unban, bans, ip, banip, unbanip, ipbans, end, endless, borders, forcecol, mode, modes, map, maps, cycle, game (`CHAT_PERM_DEV`) | `allowed()` | **BROKEN**: with `perms` off (the server_bin default, and what prod runs, see `server.cfg.j2`) every guest could kick, ban (persisted), IP-ban, read every player's IP, change map or end the round | **FIXED**: DEV peers only unless perms off **and** cheats `"all"` (v3.1.1's open server; the offline sandbox). This matches v7.1.4, which has no perms-off free-for-all (`requireHostOrDev` / `requireAuth`) | shared/src/chat_commands.c:129 |
| god, ungod, smite, noclip, notarget, kill (`CHAT_PERM_CHEAT`); forcehide, unlimitedammo, score, summon (`CHAT_PERM_DEV_CHEAT`) | `allowed()` | OK (already gated, cheats `"auth"` by default) | OK | chat_commands.c:129 |
| bot*, tp, bring (`CHAT_PERM_HOST_DEV`) | `allowed()` | OK: DEV or custom host, perms on or off | OK | chat_commands.c:129 |
| Any command (the table permission) | the dispatcher | **WEAK**: each `cmd_*` checked its own gate; one missing check would have opened the command | **FIXED**: `chat_command_handle` checks `info.perm` before parsing arguments or resolving any target | chat_commands.c:1978 |
| Selectors (`kick *`, `god @bots`, `bring all`, `kill *`) | gate first, then `target_select` | BROKEN with perms off (`kick *` by a guest kicked everyone) | **FIXED** (above). A guest's `kick *` is refused before resolution: no count, no "no match" (test A1) | chat_commands.c:414 |
| give `<weapon>` `<other / selector>` | Object Manager rules; giving to others requires `perms.dev` / `perms.host` | OK (the selector resolves first, but nothing changes before the gate) | OK | chat_commands.c:1664 |
| ping `<target>` | `is_dev` for a target | OK | OK | chat_commands.c:627 |
| thirdperson | `third_person_allowed` (sandbox / DEV / custom host or knTPCam / an open server) | OK (the server grants it; the camera is client-side by nature) | OK | chat_commands.c:1818 |
| c, count, server, team, teams, players, ais, drop (`CHAT_PERM_ANY`) | none needed (read-only, or about yourself) | OK | OK | chat_commands.c table |
| Denial | notify + log | the log line had no account | **FIXED**: `perms denied: <cmd> from <id> (<name>, account <id>)` | chat_commands.c:136 |
| Chat / commands before the JOIN settles (gated servers) | `net_client_admitted` | **WEAK**: an unauthenticated connection could chat and run commands as a guest until `join_timeout` | **FIXED**: refused and logged (`join not verified`, throttled) | net.c:1147, net.c:1171 |

## Dev packets (sent by the client for the mod's keys)

| Packet | Gate | Verdict | Evidence |
|---|---|---|---|
| `NET_PACKET_TELEPORT` (tp) | `CHAT_PERM_DEV_CHEAT` | OK (now also requires an admitted JOIN) | net.c:1335 |
| `NET_PACKET_CYCLE` | `CHAT_PERM_DEV` | was BROKEN with perms off (above); **FIXED** | net.c:1651 |
| `NET_PACKET_PREMIUM_ACTION` (prmA) | `CHAT_PERM_CHEAT` | OK (now also requires an admitted JOIN) | net.c:1328 |
| `NET_PACKET_CLEAR_LOG` | none (as in v3.1.1) | **FIXED**: any guest could write terminal clear escapes to the operator's console and the journal; now `CHAT_PERM_DEV` | net.c:1657 |
| `NET_PACKET_FORWARDED` | loopback peer, first packet only | OK. A late or remote claim (`127.0.0.1` from a guest) was ignored silently; it is now logged (test A3) | net.c:1308 |
| `NET_PACKET_SPAWN_REQ` (Object Manager, incl. the Properties tool's field edits) | `object_manager_request` / `object_manager_edit(_fields)` with `net_om_perms` (server-derived); the map's own objects need `OM_CAP_MAP` (DEV or the custom host, never in ranked) | OK | net.c:861, shared/src/object_manager.c:351 |
| `NET_PACKET_COMP_SET` | changes only the sender's own entry (`comp_lobby_update(uid = client->id)`) | OK | shared/src/comp_lobby.c:104 |
| `NET_PACKET_SOCIAL_REQUEST` | dropped behind a brain; the standalone stub has no accounts | OK | net.c:1224 |

## Server CLI and rcon

| Path | Authorises | Verdict | Evidence |
|---|---|---|---|
| CLI (stdin) | the operator's terminal; runs as `CHAT_CMD_CONSOLE` (every permission) | OK | server/src/cli.c:808 |
| `op` / `deop` / `ban` / `kick` / `send` | CLI and rcon only (no chat path reaches `cli_execute`) | OK | cli.c:694 |
| rcon password compare | | **FIXED** (WEAK): the old `rcon_streq` was close to constant time but ad hoc. Now `kn_secret_equal`: SHA-256 both sides, then a constant-time digest compare | rcon.c:108, shared/src/auth_join.c:135 |
| rcon rate limit | | **FIXED** (WEAK): there was one thread per connection with no cap, and only a 1 s delay per connection, so guessing in parallel was unbounded. Now at most 4 connections may wait for a password (`ERR busy`), and 5 bad passwords within 60 s lock rcon for 60 s (`ERR locked`). Both are logged | rcon.c:47 |
| rcon bind | `rcon_host` defaults to 127.0.0.1 | OK. **FIXED**: off loopback, a password under 12 characters is refused (rcon does not listen) | rcon.c:173 |
| rcon blank password | `rcon_start` returns when `rcon_password` is empty; `kn_secret_equal` never matches a blank secret | OK | rcon.c:159 |
| Coordinator rcon frame | the authenticated coordinator link (wss, certificate verified) | OK (as strong as the brain's internal secret, below) | rcon.c:310 |
| Server password (`gameserver.password`) | | **FIXED** (WEAK): it was compared with `strcmp` and could be guessed without limit. Now constant time, and an address is locked out for 60 s after 5 wrong passwords | accounts.c:1563-1630 |
| Join ticket verify | HMAC-SHA256, `kn_ct_equal`, `exp`, `nid` / `lid` binding | OK | accounts.c:1643-1662 |
| Ticket resume | an expired ticket only if this lobby admitted its `jti` (30 min after its last connection) | OK | accounts.c:1648 |
| Default secret | `brain_secret` defaults to the public `kn-dev-secret` | **FIXED** (WEAK): server_bin now refuses to link to a coordinator that is not on loopback with the dev default or a blank secret (it runs standalone, guests only), so nobody can forge a ticket for an account. Prod already uses a random secret of 32+ characters (asserted in `kn_gameserver/tasks/main.yml:47`) | server/src/brain.c:315 |

## Client console (`register_local` vs `register_server`)

Every `register_local` command (client/src/ui/ui_console.c:1305-1340) changes client state only:
help, clear, echo, bind, alias, exec, connect, disconnect, quit, reconnect, offline, sandbox,
version, update, say, status, pos, rot, ping, netinfo, show_navmeshes, tool_snap,
editor (an unsaved local copy of the map), firstperson, and thirdperson (it asks the server,
which grants or refuses it). `toggle` and `say` send chat lines that the server gates. Every
command that affects the world or other players is `register_server` (ui_console.c:1342+): it
is sent as chat and runs only through the gate above. **OK.** A modified client can of course do
anything locally (rendering, camera); none of that is authority.

## kn_brain

A review of every route and WebSocket handler. Sessions and per-account data are sound:
- Tokens have 128 random bits, are stored as SHA-256, and have an expiry.
- Every social, economy and market handler acts on the session's `s->account_id`, never on a
  client-supplied id, clan tag or name.
- SQL procedures check officer/leader, seller, receiver and ownership.
- Every secret comparison uses `kn_ct_equal` / `kn_ct_streq`.

| Route / message | Authorises | Identity from | Verdict | Evidence |
|---|---|---|---|---|
| `/auth/register`, `/auth/login` | per-IP limits (5/h, 10/min) plus a DB throttle per (name, IP) | credentials | OK; see WEAK (b) | services/brain/auth.c:184 |
| `/auth/me`, `/auth/refresh`, `/auth/logout` | session | session | OK | auth.c:212-241 |
| `/auth/join`, `/auth/hasJoined` | session / serverHash | session | WEAK (c) | auth.c:367, auth.c:404 |
| Password reset, email verify | emailed code (60 bits, single use, TTL) | code | OK | account_security.c:830 |
| Social WS (`kr`, clans, market, trades, posts, maps, 2FA, ...) | session; SQL ownership checks | `s->account_id` | OK; see WEAK (a) | social_ws.c:386, social_data.c |
| Staff actions (`grp`, `flg`, `ver`, `ftC`, `edMp`, `delC`) | DB columns `player_moderator` / `player_developer` / `player_mapmod` of the session's account | session | OK; see WEAK (d) | profiles.c:344, creations.c:401 |
| `/mm/ranked/*` (queue, profile, purchase, ...) | session | session | OK | ranked.c:260-700 |
| `PUT /api/clans/:id/loot-config` | session; DB requires owner or rank ≥ 2 | session | OK | turf.c:447 |
| `POST /ai/chat` | session; banned accounts refused | session | OK (cost limits are per process: an operational note) | ai.c:1140 |
| `/mm/internal/ws` upgrade | Bearer `brain.internal_secret` | node | **FIXED** (WEAK): now `mm_internal_authorized`: <ul><li>30 tries a minute per address (local plus Redis)</li><li>blank secret refused</li><li>the public `kn-dev-secret`, or a secret under 16 characters, only admits direct local connections (403 through a proxy)</li><li>every refusal logged</li></ul> | brain_util.c:204, internal_ws.c:661 |
| `/mm/admin/drain`, `/mm/admin/rcon` | the same secret | n/a | **FIXED** (WEAK): the same gate; before, wrong guesses were unlimited through the public `/mm/` proxy | public_routes.c:484, public_routes.c:533 |
| Internal `auth` with `ticket` | HMAC ticket | ticket claims | **FIXED** (WEAK): <ul><li>the ticket's `nid` must be the connected node (`ticket is for another server`), so a node can't redeem another server's ticket</li><li>`resume` accepts tickets that expired at most 24 h ago; before, it passed `now = 1`, so any ticket ever issued was accepted</li></ul> | internal_ws.c:312-327 |
| Internal `acct` / `db-call`, `commit`, `central`, `turf-kills`, `ranked-result`, `host-request` | the shared internal secret | node-supplied account ids | **WEAK** (e): a node is fully trusted for every account | internal_ws.c:585-606 |

## Still open (not fixed here, with the reason)

**(a) WEAK: revoking a session does not log out open social sockets.**
- Revoking means "log out other sessions", a password change or reset, or a ban.
- These delete the session rows, but `s->account_id` stays set on sockets that are already
  open (social_ws.c:386), so they keep working until they reconnect.
- Fixing it needs a revoke broadcast (Redis channel) to every brain's sockets. That is a
  cross-process feature, not a one-line check.

**(b) WEAK: no lockout per account for password guessing.**
- Login throttles only per (name, IP) and per IP
  (services/db/migrations/postgres/0103_account_functions.sql:93).
- A lockout per account lets anyone lock a victim out of login, so it needs a product
  decision first (e.g. a CAPTCHA, or backoff instead of a lockout).

**(c) WEAK: an online-mode join can be relayed or reused.**
- The serverHash binds the server nonce, but not a server key (`public_key` is empty), and
  `auth.check_ip` defaults to off. A malicious online-mode server could relay a player's join
  to another server within the 30 s record.
- The fix is a server key in the hash, plus deleting the record when it is used. That is a
  protocol change (client and server).
- Turning on `check_ip` would break players behind the web bridge.

**(d) WEAK: dev/mod status is granted from `accounts.developers` / `moderators` usernames at
start-up (brain accounts.c:1274).**
- A listed name that is not registered yet, or is later renamed, becomes staff.
- The fix is listing staff by account id. That changes the config format and the deployment
  vault, so it is left for the operators.

**(e) WEAK (design): one shared secret for every game server.**
- Holding `brain.internal_secret` lets a node act on any account: stats, KR, skins, Elo, ranked
  results, turf kills.
- Prod uses a random secret of 32+ characters, and attempts are now rate-limited.
- The real fix is a credential per node, with the brain only accepting account operations for
  accounts that node admitted. That is a new trust model, not a small change.
- Related: `/mm/internal/` and `/mm/admin/` are reachable through the public `/mm/` proxy
  (`deploy/ansible/roles/openresty/templates/nginx.conf.j2:46`). The game servers need
  `/internal` over the edges. `/mm/admin/` could be restricted at nginx once the operators say
  where admin calls come from.

**(f) Availability, not privilege:**
- The web bridge runs on the central node and reaches the edge game servers by public IP, so it
  sends no `FORWARDED` there. Every bridged web player then has the bridge's address on the
  game server, and `/banip` or a CLI `ban` of a web guest bans every web player.
- The fix is a signed forwarded-address claim from the bridge (protocol and bridge change).
- Also: anyone can fill lobbies through `/mm/seek-game`, finder entries can be hijacked, and the
  AI chat's cost limits are per process. These are abuse limits, outside command authorisation;
  they were recorded during the brain review.

**Custom-host moderation (parity note):** v7.1.4 lets the custom game's host `/kick` / `/ban` /
`/ip` in their own lobby (`actorIsHost`). KrunkNative keeps those commands for DEV peers only.
Allowing the host would widen access, which is outside this fix.

## Tests (`make command-auth-e2e`)

`server/tests/command_auth_e2e.py`: three standalone server_bin processes and a kn_brain on a
scratch SQLite database. All ports come from `kn_proto.free_ports()`, and every process is
stopped at the end.

- **A.** The prod default config:
  - A guest forges `/kick`, `/kick *`, `/kick @humans` (over CHAT_V7), `/ban`, `/banip`, `/ip`,
    `/map`, `/end`, `/god *`, `/kill *`, `/bring all`, `/bot_add`, `/unban` and `/give` to
    another player.
  - It also forges prmA, tp, cycle and clearLog packets, and sends a late `FORWARDED 127.0.0.1`.
  - Every one is refused and logged, nothing leaks and nobody leaves.
  - Positive cases: the host's `/ip`, `/god` and `/kick` work, and rcon with its password works.
  - rcon: a wrong password and a blank one are refused and logged; after 5 bad passwords the
    lockout refuses even the right one.
- **B.** Tickets:
  - A ticket signed with the wrong secret, claiming the op account: refused.
  - A ticket for another node: refused.
  - Chat before the JOIN: refused.
  - A valid non-op account's `/kick`: refused, and the log names the account.
  - Positive: the op account's `/kick` works.
- **C.** Server-side configuration checks:
  - A remote coordinator with the dev secret: the server does not link.
  - rcon on `0.0.0.0` with a short password: rcon does not listen.
  - Positive: perms off plus cheats `"all"` still gives a guest `/god`.
- **D.** kn_brain:
  - A wrong secret on `/mm/admin/drain`: 401.
  - The dev secret through a proxy: 403.
  - The dev secret on a direct local connection: passes the gate.
  - 30 or more tries a minute: 429.
  - All of these are logged.
