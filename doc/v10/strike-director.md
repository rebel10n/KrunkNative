# Krunker Strike director (`kn_strike_director`)

Krunker Strike's own matchmaker: the same shape as FRVR's Strike "director"
(`https://director.<region>-svc.krunker.frvr.com`, read from the Strike 1.37.2 bundle,
`/home/user/src/KrunkerStrike_v1.37.2/public/assets/index-Cz2uYXJQ.deob.js` around line 103680:
`getLatency`, `allocateServer`, `allocateCustomPrivateServer`). One director per region, separate
from the main backend: a standalone C binary (`services/strike/director.c`), knsvc only, **no
database and no kn_brain**. It is fed by Strike game servers' finder heartbeats and hands clients a
game server plus a signed ticket that the game server checks.

## Pieces

| Piece | File | Role |
|---|---|---|
| director | `services/strike/director.c` | HTTP routes below; the brain finder's server table (`services/brain/finder_table.c`) + KNINFO UDP polls; seat reservations, private lobbies, room pins (memory only) |
| ticket | `services/strike/director_ticket.{c,h}` | HS256 JWS (knsvc `kn_signed_token`) under `HMAC-SHA256(secret, "kn-strike-director-v1")` hex, the mm-token construction (`services/brain/mm_token.c`); linked by the director **and** server_bin |
| server gate | `server/src/director_gate.c`, hooks in `server/src/accounts.c` | verifies the JOIN's `client_key`; private-lobby codes; the `private` server-info tag |
| client | `client/src/net/strike_director.c`, `client/src/ui/servers_ui.c` | a Strike client seeks through the director, falls back to the finder's Strike pool |

Only servers whose **verified** KNINFO reply says `"flavor":"strike"` are handed out. A base server
may heartbeat to a director; it is polled and verified, then never given a player.

## HTTP contract

Every allocate route takes the game version from the `x-game-version` header (FRVR's) or, for
clients whose HTTP layer sets no custom headers (ours), `gameVersion` in the JSON body. Dotted
numeric compare against `strike_director.min_game_version` (default: the director build's network
protocol, e.g. `1.4`); missing or older = **426**. FRVR's `x-min-game-version` and `Authorization:
Bearer` are accepted and ignored. CORS: `*` unless `strike_director.cors_origins` is set.

### `GET /live`

FRVR's client only times it (`fetch(director + "/live").text()`, twice per region, keeps the
fastest). Ours answers:

```json
{"ok":true,"service":"kn-strike-director","region":"local","servers":2,"open":6,"lobbies":0,
 "minVersion":"1.4","version":"v1.0.0-336-g61e5d8f"}
```

### `POST /v1/director/allocate`

Request (every field optional):

```json
{"region":"eu","mode":"ffa","roomId":"r-123","openSlots":1,"gameVersion":"1.4"}
```

- `region`: preferred region (the server's `gameserver.region`); falls back to any region.
- `mode`: finder mode filter (name or id).
- `roomId`: a room's members land in the same public lobby while it has seats (pinned
  `room_pin_s`, default 120 s). S6 builds on it.
- `openSlots` (FRVR): seats needed together (a party), default 1, all reserved.
- FRVR's beginner `fleet` is accepted and ignored.

Pick: the fullest Strike server with enough free seats (fill first), skipping private lobbies,
password / `draining` / `ranked` servers. Free seats = `max - players - tickets of the last
join_window_s` (default 15 s, so loading clients are never missed).

200:

```json
{"address":"10.0.0.5:7777","host":"10.0.0.5","port":7777,"gameId":"eu:3f2a9c01",
 "ticket":"eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9.eyJ0eXAiOiJzZHQiLC...","region":"eu","mode":"Free for All",
 "map":"Sandstorm","expiresIn":60,"gameServer":{"host":"10.0.0.5","port":7777},"roomId":"r-123"}
```

`address` is what FRVR's client reads; `gameServer` is FRVR's older field; `host`/`port`/`ticket`
are what our client uses (`ticket` = `NetJoinPacket.client_key`). `roomId` only when given.
`gameId` = `<region>:<first 8 hex of sha256("host:port")>`, stable per server.

Errors (`{"error": ...}`): 400 `BadRequest`, 426 `{"error":"GameVersionTooOld","minVersion":"1.4"}`,
429 `TooManyRequests` (`allocate_per_min` per IP, default 60), 503 `NoAvailableServers`, 500
`TicketFailed`.

### `POST /v1/director/allocate/private`

Request, one of:

```json
{"customMatchId":"K7QX2M"}                                            join the lobby with that code
{"roomId":"r-123"}                                                    join the room's lobby, or open one
{"capacity":12,"region":"us-central","customId":"usc1a::<frvr id>"}   FRVR's body: customId = roomId
{}                                                                    open a new lobby
```

plus optional `region`, `mode`, `gameVersion`. FRVR's `customId` is the host's regioned FRVR id
(`makeRegionedCustomId`): the part after `::` is the room key, so every friend who sends it lands in
the same lobby (create-or-join). `capacity` (FRVR: 12) caps the lobby's seats. Codes are 6 chars of
`ABCDEFGHJKLMNPQRSTUVWXYZ23456789`, matched case-insensitively.

Opening takes an **empty** Strike server (no players, no recent tickets, not private), the asked
region first. Joining checks seats as `capacity|max - players - tickets issued since the last poll`
(a friend is never refused by a seat counted twice; server_bin's max_players is the hard cap).

200 = the public answer plus:

```json
{"code":"K7QX2M","customMatchId":"K7QX2M","roomId":"r-123","created":true}
```

Errors: 404 `LobbyNotFound` (unknown code, or its server is gone), 409 `GameFull`, 503
`NoAvailableServers` (no empty server to open one), and the version / rate errors above.

A private lobby ends when its server leaves the table, or after `private_idle_s` (default 120 s)
without allocations once a poll shows it empty.

### `GET /v1/director/servers`

The Strike servers as the finder lists them (`/servers` entry shape) plus `gameId`, `pending`,
`private` (bool; the code is never listed). For dashboards and tests.

### `POST /servers/heartbeat` (and `/servers/submit`)

The finder contract (`server/src/info_query.c` heartbeat; `services/brain/finder.c`): a game server
with `gameserver.finder_url = <director URL>` registers here, the director polls KNINFO over UDP and
lists it once verified. `strike_director.trusted_submitters` (IP list) restricts who may register.

## The ticket on the game server

Claims `{"typ":"sdt","gid":<gameId>,"port":<game port>,"jti":<32 hex>,"prv":<code, private only>,
"iat","exp"}` (TTL `ticket_ttl_s`, default 60 s; at most 351 chars). With
`gameserver.director_secret` set (= the director's `strike_director.ticket_secret`), server_bin:

- refuses a ticket that does not verify (`Invalid join key` / `Join key expired`), never downgrading
  it to a guest (unless a brain links the server: then it may be a brain ticket and that path decides);
- refuses a ticket for another port (`Join key is for another server`);
- private lobbies: the first valid ticket into an empty lobby sets its code (`""` = public); while
  anyone plays only that code gets in (`This lobby is private` / `Private lobby closed`), a ticketless
  guest too; the server-info query then carries the `private` tag, so even a restarted director never
  hands it out publicly;
- `gameserver.director_require_ticket true`: a JOIN without a ticket (or no JOIN) is refused
  (`Join through matchmaking`).

A ticket is an admission, not a single seat (FRVR hands out a bare address); seat accounting is the
director's. Admitted players are guests (Strike accounts are S5's).

## Client

A Strike client (`flavor_get() == FLAVOR_STRIKE`) with a director URL seeks through it: native
`KRUNKNATIVE_STRIKE_DIRECTOR`, web `?director=`, else the `knStrikeDirector` storage key. Without a
brain this is the startup matchmaking too (`backend_startup.c`: the transport starts once the
director answers). On failure other than `GameVersionTooOld` / `LobbyNotFound` / `GameFull`, it falls
back to the finder's Strike pool (`GET <finder>?flavor=strike&space=1`, the fullest Strike server
with room, joined without a ticket). The Join window takes a private code (4-12 letters/digits) ->
`allocate/private {customMatchId}`. The server browser of a Strike client lists `?flavor=strike`.

Not mapped (yet): FRVR connects `wss://<address>/ws`; our native client speaks TCP to `host:port`,
the web build goes through net-bridge as for any server.

## Config (`config/config.json` section `strike_director`, or `--strike_director.<key>=`, or `KN_STRIKE_DIRECTOR_<KEY>`)

| key | default | |
|---|---|---|
| `ticket_secret` | (required) | = every Strike server's `gameserver.director_secret` |
| `host` / `port` | `0.0.0.0` / `8095` | `port 0` = ephemeral |
| `region` | `local` | the region this director reports / defaults game ids to |
| `min_game_version` | build protocol | 426 below it |
| `ticket_ttl_s` | 60 | |
| `join_window_s` | 15 | public seats held after a ticket |
| `private_idle_s` | 120 | |
| `room_pin_s` | 120 | |
| `allocate_per_min` | 60 | per IP |
| `poll_interval_s` / `poll_timeout_s` / `expire_s` / `max_failures` / `max_per_ip` / `heartbeat_interval_s` | 10 / 3 / 180 / 3 / 64 / 30 | the finder table |
| `trusted_submitters` | `[]` (any) | IPs allowed to heartbeat |
| `cors_origins` | `["*"]` | |
| `trust_proxy` | false | X-Real-IP / X-Forwarded-For |

Game server (`server.cfg` or `gameserver.*`): `flavor strike`, `finder_url http://<director>`,
`director_secret <secret>`, optionally `director_require_ticket true`, `region <id>`.

## Deploy note (not deployed; no ansible / prod changes)

Krunker Strike is off prod until the coordinator says otherwise. When it goes on:

1. Build `kn_strike_director` (`make` builds it with the services; OpenSSL only, no libpq).
2. One director per region behind TLS (e.g. `strike-director.<region>.krunk.io` -> `127.0.0.1:8095`),
   `trust_proxy true`, a long random `ticket_secret` kept with the other secrets (never in git), a
   systemd unit like kn_brain's but with no database dependency.
3. Strike `server_bin` instances: `finder_url` = their region's director, the same secret as
   `director_secret`, `director_require_ticket true` once every Strike client has a director URL;
   the UDP query port reachable from the director; `trusted_submitters` = the game hosts.
4. Strike client packages (`make strike-dist`): bake the director URL in via the
   `knStrikeDirector` default or the launcher's environment.
5. Health: `GET /live` (`servers` > 0, `open` > 0).

## Tests

- `services/strike/tests/test_director_ticket.c` (`ctest -R strike_director_ticket`): sign, verify,
  forged / tampered / expired / foreign-typ / raw-secret tickets.
- `services/strike/tests/director_e2e.py BUILD_DIR` (`ctest -R strike_director_e2e`): 2 Strike
  servers + 1 base server against the director: version gate, FRVR's private body (customId,
  capacity, `address`), join by ticket and by code, room = same lobby, public fill with `openSlots`
  then 503, the base server never handed out, forged / tampered / foreign / public-into-private /
  ticketless joins refused by the server, and client_bin through the director and through the finder
  fallback (needs `assets/css/fonts`; `KRUNKNATIVE_ASSETS_DIR` otherwise).
