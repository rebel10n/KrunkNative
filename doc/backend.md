# Krunker Box Office backend

Loosely based on the Krunker v3.1.1 offline reconstruction's backend
(`/home/user/jails/kru/src/Krunker_v3.1.1/{matchmaker,server,ansible}`), ported to
C11 on any database through the store drivers of 3.6 (PostgreSQL, SQLite,
MySQL/MariaDB). Each owner writes its own section; edit other sections only with
anchored, minimal edits.

<!-- section index (keep in this order) -->
1. Architecture (svc-core / svc-matchmaker)
2. Configuration (every owner documents its keys)
3. knsvc library (svc-core)
4. Brain: public HTTP routes (svc-matchmaker)
5. Brain: internal WebSocket (game servers <-> brain) (svc-matchmaker)
6. Brain: registry, federation and matchmaking (svc-matchmaker)
7. Social WebSocket, accounts, profiles (svc-accounts)
8. Game server brain client (gameserver)
9. Client integration (client-integration)
10. Deployment (svc-matchmaker)

## 1. Architecture

```
 native client ──TCP──────────────────────────────┐
 web client ──WS /native (net-bridge)──TCP────────┤
     │  HTTP /mm/* /api/*  WS /social/ws          ▼
     ▼                                     server_bin (game server, 1 lobby)
  kn_brain (region A) ◄──WS /mm/internal/ws── server_bin ... (region A)
     │  ▲                                    
     │  │ shared database (3.6): mm_* registry rows (heartbeat + TTL), accounts, ...
     ▼  │
  kn_brain (region B) ◄──WS /mm/internal/ws── server_bin ... (region B)
```

* `kn_brain` (`services/brain/`) is the v3.1.1 "brain" (matchmaker/index.ts): one
  HTTP listener serving the public matchmaker routes, the social WebSocket and the
  internal WebSocket for game servers.
* Game servers (`server_bin`) connect to the brain of their region and push their
  lobby state; they never touch the database.
* Brains are leader free: each brain owns the game servers connected to it and
  writes their rows (with heartbeats) to the shared database; every read (game list, quick
  join, game info) is global over all live rows, so any brain can answer for every
  region. A brain or game server that dies simply stops heartbeating; its rows
  expire after the TTL.

## 2. Configuration

### Loading rules and common keys (svc-core, `knsvc/config.h`)

Every service (`kn_brain`, `server_bin`, `kn_migrate`) loads the same file with
`kn_config_load()`:

* File: `--config <file>` / `--config=<file>`, else `$KN_CONFIG`, else the first
  `config/config.json` in the working directory or up to 4 parent directories. A missing
  file means "all defaults"; an unreadable or invalid one is a startup error.
  `config/config.json` is gitignored; `config/config.example.json` is the committed template.
* Per key, first hit wins: CLI `--set a.b=v` or `--a.b=v` (any argument whose name
  contains a dot) > environment `KN_<PATH>` (dots and dashes become `_`, upper-cased:
  `database.password` -> `KN_DATABASE_PASSWORD`, `gameserver.brain_url` ->
  `KN_GAMESERVER_BRAIN_URL`, array elements by index: `KN_BRAIN_REGIONS_0_ID`) > file > default.
* Override values are parsed as JSON when they look like JSON (`9001`, `true`, `[...]`,
  `{...}`, `"..."`), otherwise taken as strings. Lists also accept `a,b,c`.
* Relative paths in the config (e.g. `database.migrations_dir`) resolve against the
  repository root: the parent of the directory holding `config.json`.
* Secrets are never logged: `kn_config_dump_redacted()` masks every key containing
  `password`, `secret`, `token` or `key`.

| key | default | meaning |
| --- | --- | --- |
| `database.driver` | see below | store driver (3.6): `postgres` (aliases `postgresql`, `pg`), `sqlite` (`sqlite3`), `mysql` (`mariadb`). Absent: `postgres` when `database.host`, `.name` or `.user` is configured (and the postgres driver is built in), else `sqlite`, so a coordinator with no config at all runs on a local SQLite file |
| `database.path` | `data/coordinator.db` | sqlite: database file (relative to the config root; directories are created; `:memory:` for tests) |
| `database.host` | `127.0.0.1` | postgres / mysql server (mysql: an absolute path = Unix socket) |
| `database.port` | `5432` / `3306` | default per driver |
| `database.name` | `krunknative` | database |
| `database.user` | `krunknative` | role / user |
| `database.password` | `""` | password (prefer `KN_DATABASE_PASSWORD` in deployment) |
| `database.search_path` | (server default) | postgres: schema list set on every connection (test isolation, multi-tenant) |
| `database.pool_size` | `4` (sqlite `2`) | connections per process (worker threads for sqlite/mysql) |
| `database.connect_timeout_s` | `5` | |
| `database.statement_timeout_ms` | `0` | server-side statement timeout (postgres `statement_timeout`, MariaDB `max_statement_time`; 0 = server default) |
| `database.busy_timeout_ms` | `5000` | sqlite: wait this long for the write lock before failing with 40001 (retried) |
| `database.notify_poll_ms` | `100` | sqlite/mysql: poll interval of the pub/sub emulation (`kn_pubsub` table, 3.6) |
| `database.application_name` | `knsvc` | postgres: shows in `pg_stat_activity` |
| `database.migrations_dir` | `services/db/migrations` | migrations root; the driver's `<dialect>/` subdirectory is used when present (3.7). Unset: the config root's, else the one next to / above the executable |
| `database.auto_migrate` | on for sqlite, else off | `kn_brain` applies pending migrations at start (like `--migrate`) |
| `log.level` | `info` | `trace` `debug` `info` `warn` `error` (env `KN_LOG_LEVEL`) |
| `log.format` | `text` | `text` or `json` (one object per line; env `KN_LOG_FORMAT`) |
| `log.file` | stderr | append log lines to this file (env `KN_LOG_FILE`) |

### redis (optional hot tier, 3.11)

Off unless `redis.url` is set and kn_brain was built with hiredis (`KNSVC_REDIS`); env `KN_REDIS_<KEY>`.
With it set but Redis absent or down, every brain works exactly as without it (the database alone).

| key | default | meaning |
| --- | --- | --- |
| `redis.url` | `""` (off) | `redis://[user[:password]@]host[:port][/db]` or `unix:///path[?db=N]` (`valkey://` = `redis://`; `rediss://` is refused: no TLS client, see 3.11) |
| `redis.username` | `""` | ACL user (production: `kn_brain`); overrides the URL's |
| `redis.password` | `""` | prefer `KN_REDIS_PASSWORD` (deployment: `/etc/krunknative/secrets.env`); overrides the URL's |
| `redis.db` | the URL's, else `0` | `SELECT` on the command connection |
| `redis.prefix` | `kn:` | prefix of every key and channel (several networks or test runs can share a server) |
| `redis.connect_timeout_ms` / `redis.command_timeout_ms` | `1000` / `500` | a connection not ready, or a reply not back, in time is dropped and reconnected (250 ms .. 5 s backoff); the waiting callers fall back to the database |
| `redis.pubsub` | `true` | carry the database's pub/sub channels (`kn_db_notify`, 3.6) over Redis |
| `redis.pubsub_fallback_poll_ms` | `1000` | sqlite/mysql: `kn_pubsub` poll interval while Redis carries the channels (`database.notify_poll_ms` again when it is down) |
| `redis.session_cache_s` | `60` | session token -> account id cache lifetime (0 = off) |
| `redis.rate_limits` | `true` | the auth / finder rate limits count across every brain |
| `redis.presence` | `true` | signed-in social sockets per brain (`/mm/presence`) and social pushes to other brains |
| `redis.presence_ttl_s` | `90` | a brain refreshes its accounts every third of this; a dead brain's entries expire after it |

### brain (svc-matchmaker)

All keys are optional; env `KN_BRAIN_<KEY>` / CLI `--brain.<key>=` override.

| key | default | meaning |
| --- | --- | --- |
| `brain.host` | `127.0.0.1` | bind address (nginx fronts TLS in deployment) |
| `brain.port` | `5050` | listen port (v3.1.1 dev `matchmakerURL` is `http://127.0.0.1:5050`) |
| `brain.id` | `<hostname>-<port>` | unique brain id (registry owner column) |
| `brain.region` | `local` | default region of this brain (nodes that send none) |
| `brain.public_url` | `http://<host>:<port>` | advertised URL (logs, `/mm/brains`) |
| `brain.internal_secret` | `kn-dev-secret` | Bearer secret game servers must present on `/mm/internal/ws` (and admins on `/mm/admin/*`); must equal `gameserver.brain_secret`. Rate-limited (30 tries/min per address); the dev default (or a secret under 16 characters) only admits direct local connections, never through a proxy ([v7/command-auth.md](v7/command-auth.md)) |
| `brain.join_secret` | = `internal_secret` | HMAC key of join tickets (must equal the game servers' `gameserver.join_secret`, default their `brain_secret`) |
| `brain.join_ttl_s` | `30` | ticket lifetime / slot reservation time |
| `brain.validation_secret` | `""` (= derived from `join_secret`) | key of the seek-game validation tokens (`GET /mm/generate-token`, [v10/matchmaker-token.md](v10/matchmaker-token.md)); the same on every brain of a network. Unset: `HMAC-SHA256(join_secret, "kn-mm-validation-v1")` |
| `brain.matchmaking.validation_ttl_s` | `60` | validation token lifetime (5..3600) |
| `brain.matchmaking.require_validation_token` | `false` | refuse a seek-game without a `validationToken` (403 `InvalidClientValidationToken`); off during the rollout so older clients keep working |
| `brain.matchmaking.validation_bind_ip` | `false` | a token is only good from the address it was issued to (`InvalidClientIP`; needs `trust_proxy` behind nginx) |
| `brain.cors_origins` | `["*"]` | allowed CORS origins for the web build |
| `brain.trust_proxy` | `false` | take client IP from X-Forwarded-For (behind nginx) |
| `brain.node_ttl_s` | `30` | node/lobby row TTL (v3.1.1 `STALE_AFTER_MS`) |
| `vapid.public_key` | `""` (off) | the VAPID public key of Web Push (env `KN_VAPID_PUBLIC_KEY`): unpadded base64url of the 65-byte P-256 point (87 characters). `GET /api/push/vapid-key` answers it (404 when unset); the page subscribes with it (services/brain/push.c, push_rules.c). The private key and the subject are the send step's (not read yet) |
| `brain.heartbeat_s` | `5` | how often the owning brain refreshes its rows |
| `brain.ping_interval_s` | `10` | internal WS ping interval (v3.1.1 `PING_INTERVAL_MS`) |
| `brain.regions` | `[{"id":"local","name":"Local"}]` | known regions (`id`, `name`, optional `ping_host`); ids are the 2-letter region codes (`SV`, `LA`, `CH`, `VA`): the prefix of short game links (`?game=CH2:kz862`); the client shows their place names |
| `brain.federation` | `true` | use the shared database's rows (false or no DB = in-memory, this brain only) |
| `brain.matchmaking.cross_region` | `true` | quick join falls back to other regions when the requested one has no room |
| `brain.matchmaking.max_players` | `8` | cap when a lobby reports none |
| `brain.matchmaking.mode_caps` | `{}` | per-mode quick-join caps, by mode name or index (`{"ffa":8,"tdm":10}`) |
| `brain.matchmaking.soft_cap` | `0` | if > 0, quick join treats lobbies at/above this player count as full unless nothing else has room |
| `brain.matchmaking.require_version` | `false` | honour the client's `dataQuery.v` (lobbies with another `meta.v` are skipped) |
| `brain.host_timeout_s` | `10` | how long a host-game request waits for a game server |
| `brain.ranked.*`, `brain.turf.*` | see 6.5 | v10 ranked queue / profiles and Turf Wars (doc/v10/ranked-impl.md) |
| `brain.community.feed` | `config/community.json` | the community feed file (6.6), relative to the config root; reread when it changes |

### AI providers (`ai_provider.c`, `ai.h`)

The brain's AI consumers each use one named **provider profile**: an OpenAI-compatible endpoint
with its own key. A profile is `ai.providers.<name>`; each consumer picks one with
`ai.<consumer>.provider` (default `"default"`):

| consumer | routing key | what it does |
|---|---|---|
| editor | `ai.editor.provider` | the map editor's chat (`/ai/*`), its model list and health probe |
| banter | `ai.banter.provider` | bot chat lines for game servers (`bot-banter`) |
| moderation | `ai.moderation.provider` | avatar review (`ai_moderate_image`, once the avatar upload lands) |

| key | env form | meaning |
|---|---|---|
| `ai.providers.<name>.url` | `KN_AI_<NAME>_URL` | base ending in `/v1` (`http://` or `https://`), no trailing slash |
| `ai.providers.<name>.key` | `KN_AI_<NAME>_KEY` | Bearer key; never logged, never in an answer, masked in a config dump |
| `ai.providers.<name>.model` | | the profile's model; `""` = the consumer's choice (banter: the editor's effective default when it is the same profile, else the editor's preferred model: `ai.providers.editor.model`, or `ai.default_model`) |
| `ai.providers.<name>.rpm` | | requests per minute over every consumer of the profile (0 = none) |
| `ai.providers.<name>.daily_budget_tokens` | | prompt + completion tokens per UTC day over every consumer of the profile (0 = none) |

`<NAME>` is the profile name upper-cased (`a-z`, `0-9`, up to 15 characters, not `upstream`).
Precedence per field: env `KN_AI_<NAME>_*` > the file (`ai.providers.<name>.*`, or
`KN_AI_PROVIDERS_<NAME>_*`). A profile can exist from its env alone. `ai.<consumer>.provider`
also reads its env form (`KN_AI_EDITOR_PROVIDER`, `KN_AI_BANTER_PROVIDER`, `KN_AI_MODERATION_PROVIDER`).
A consumer routed to a missing or unconfigured profile is off (the editor's routes answer 503,
bot chat says "AI unavailable"); the log says which one.

**The legacy single key keeps working.** `ai.upstream_url` / `ai.upstream_key` (env
`KN_AI_UPSTREAM_URL` / `KN_AI_UPSTREAM_KEY`) are the `default` profile. With nothing else set, all
three consumers use it, as before. `KN_AI_DEFAULT_URL` / `KN_AI_DEFAULT_KEY` and
`ai.providers.default.*` take precedence over them.

Worked example, three keys (the keys go in the environment, `~/.config/kn-secrets/ai.env` locally,
`/etc/krunknative/ai.env` on a server; the file holds the routing and the limits):

```sh
# ~/.config/kn-secrets/ai.env
KN_AI_EDITOR_KEY=sk-...editor...
KN_AI_VISION_KEY=sk-...moderation...
KN_AI_BANTER_KEY=sk-...banter...
```

```json
{
  "ai": {
    "providers": {
      "editor": {"url": "https://chat.example.com/v1", "model": "glm-5.3-flash",
                 "rpm": 60, "daily_budget_tokens": 20000000},
      "vision": {"url": "https://vision.example.com/v1", "model": "gemini-3.8-flash-high", "rpm": 30},
      "banter": {"url": "https://chat.example.com/v1", "model": "glm-5.3-flash",
                 "rpm": 120, "daily_budget_tokens": 2000000}
    },
    "editor": {"provider": "editor"},
    "banter": {"provider": "banter"},
    "moderation": {"provider": "vision"}
  }
}
```

Each consumer now talks to its own key, and each key has its own limits; the editor and the banter
profile here are on the same host with separate keys, so one can be rotated or capped without the
other. `GET /ai/status` shows the counters per profile (`providers`: requests in the last minute,
requests and tokens today) and per consumer (`consumers`), never a key or a URL.

Single key, the same as before this change (no `providers`, no routing):

```sh
# ~/.config/kn-secrets/ai.env
KN_AI_UPSTREAM_URL=https://chat.example.com/v1
KN_AI_UPSTREAM_KEY=sk-...
```

Secrets: keep keys in `key` fields or the env, never inside a URL. The config dump
(`kn_config_dump_redacted`) masks every key whose name contains `key`, `token`, `secret` or
`password`, but it does not read a URL's user info or query string.

### gameserver / accounts

(see section 8 for `gameserver`, section 7 for `accounts`)

## 3. knsvc library (svc-core)

`services/common/` builds two static libraries; the headers in
`services/common/include/knsvc/` are the specification (every function is documented
there). C11, no C++; dependencies: cJSON (`shared/lib/cJSON`), OpenSSL libcrypto,
optionally libcrypt, and for `knsvc_db` only the client library of each store driver
that is built in (libpq, libsqlite3, libmariadb / libmysqlclient; 3.6).

| library | headers | links | used by |
| --- | --- | --- | --- |
| `knsvc` | `buf.h log.h config.h json.h loop.h http.h ws.h crypto.h msgpack.h smtp.h` (umbrella `knsvc.h`) | cJSON, libcrypto (+ libssl: wss://, SMTP TLS), libm, pthreads | `kn_brain`, `server_bin` (brain client), tools |
| `knsvc_db` | `db.h store.h migrate.h` | `knsvc` + the built-in drivers' client libraries | `kn_brain`, `kn_migrate`, `kn_dbcopy` |
| `knsvc_redis` | `redis.h` | `knsvc` + hiredis (>= 0.14) | `kn_brain` (3.11); optional (`KNSVC_REDIS`, ON when hiredis is found) |

Build: part of the normal native build when OpenSSL is found (`knsvc_db`, `kn_migrate`
and the brain additionally need at least one store driver: options
`KNSVC_STORE_POSTGRES` / `KNSVC_STORE_SQLITE` / `KNSVC_STORE_MYSQL`, each ON by default
when its library is found; `KNSVC_REDIS` likewise for the Redis hot tier, 3.11); `-DKRUNKNATIVE_SERVICES=OFF` skips `services/`
entirely, and the game targets never depend on it except `server_bin`'s optional brain
client (`if (TARGET knsvc)`). Make: `make services` (all backend targets),
`make db-migrate`, `make db-status`, `make services-test` (`ctest -L knsvc`).

### 3.1 Threading model

Everything is single-threaded on a `kn_loop` (epoll on Linux, `poll()` elsewhere or with
`KN_LOOP_BACKEND=poll`): fd watchers, a timer heap, posted tasks and self-pipe signals.
Only `kn_loop_post_threadsafe()` and `kn_loop_stop()` may be called from other threads;
that is how `server_bin` drives its brain client: a dedicated thread runs `kn_loop_run()`
and the game thread posts work into it. `kn_loop_run_once(loop, timeout)` lets another
main loop drive knsvc instead. Callbacks may freely add/remove fds and timers and close
the connection they are called for: objects are freed at the end of the outermost event
handler, never inside a user callback.

### 3.2 HTTP/1.1 server (`http.h`)

* Routes: `kn_http_route(srv, "GET", "/api/user/:name", handler, ud)`; `:param` matches
  one path segment (percent-decoded, `kn_http_param`), a trailing `*` matches the rest,
  `NULL` method = any. Registration order wins. Unknown path -> `404 {"error":"not found"}`,
  known path with another method -> `405` with `Allow`. HEAD is served by GET routes.
* Every request gets exactly one response, synchronously or later (after a DB query):
  `kn_http_respond[_json|_json_str|_text|_error]`, or a chunked stream
  (`kn_http_stream_begin/write/end`). `kn_http_req_alive()` turns 0 when the client
  disconnected; responding then only releases the request. Error bodies are always
  `{"error":"<message>"}`.
* Keep-alive (HTTP/1.1 default, `Connection: keep-alive` for 1.0), pipelining (one
  request processed at a time per connection, answers in order), chunked request bodies
  (extensions ignored, trailers skipped), `Expect: 100-continue`.
* Limits (`kn_http_config`): `max_header_bytes` 16 KiB -> 431, `max_headers` 64 -> 431,
  `max_body_bytes` 1 MiB -> 413, `request_timeout_ms` 30 s for a request to arrive
  completely -> 408, `keepalive_timeout_ms` 15 s idle, `max_connections` 4096 (extra
  accepts get 503).
* Strict parsing against request smuggling (all -> 400 and close): `Content-Length`
  together with `Transfer-Encoding`, conflicting or non-numeric `Content-Length`,
  whitespace before the header colon, obsolete line folding, control characters,
  missing `Host` in HTTP/1.1, NUL in the decoded path. Transfer codings other than
  `chunked` -> 501, versions other than HTTP/1.x -> 505. Bare-LF line endings are accepted.
* CORS: `cors_origins` (exact origins or `"*"`) adds `Access-Control-Allow-Origin` (+
  `Vary: Origin`; `Access-Control-Allow-Credentials` with `cors_allow_credentials`) to
  every response for an allowed `Origin`, and answers preflights (`OPTIONS` with
  `Access-Control-Request-Method`) on routed paths with `204`, the path's methods,
  the requested headers and `Access-Control-Max-Age: 600`.
* `trust_proxy`: `remote_addr` comes from `X-Real-IP`, else the right-most
  `X-Forwarded-For` entry (the one added by our nginx), else the socket peer.

### 3.3 WebSocket (`ws.h`)

RFC 6455 server (upgrade inside an HTTP handler: `kn_ws_accept(req, &callbacks, &opts, ud)`,
so the handler can check the query/headers/auth first) and client
(`kn_ws_connect(loop, "ws://host:port/path?q", &callbacks, &opts, ud)`, extra request
headers in `opts.headers`, e.g. `Authorization: Bearer <secret>`; `ws://` only, TLS is
nginx's job; DNS resolution is synchronous).

* Callbacks: `on_open`, `on_message(data, len, is_binary)` (complete, reassembled
  messages; text is validated UTF-8 and NUL-terminated), `on_close(code, reason)` exactly
  once (also when a client connect fails: 1006, delivered from the loop, never inside
  `kn_ws_connect`), optional `on_drain` / `on_pong`. The `kn_ws` is freed right after
  `on_close`.
* Protocol: client frames must be masked and server frames must not be (1002); RSV bits,
  unknown opcodes, fragmented or > 125-byte control frames, non-minimal lengths,
  continuation errors -> 1002; invalid UTF-8 in text or close reasons -> 1007; messages
  over `max_message_bytes` (default 1 MiB, also across fragments) -> 1009; invalid close
  codes (< 1000, 1004-1006, 1015-2999, >= 5000) -> 1002. Pings are answered
  automatically (also between fragments); `ping_interval_ms` sends keep-alive pings and
  drops the peer with 1006 when no pong arrives within `pong_timeout_ms`.
  `permessage-deflate` and other extensions are declined. Close handshake: the server
  closes TCP after echoing; `kn_ws_close` waits up to 5 s for the peer's close.
* Backpressure: `kn_ws_buffered()` is the queued outbound byte count; `on_drain` fires
  when it falls below `max_send_buffer/4` after exceeding `max_send_buffer/2`; a send that
  would exceed `max_send_buffer` (default 4 MiB) closes the connection with 1008 and
  returns -1 (slow-consumer protection).
* Server-side accessors: `kn_ws_remote_addr` (honours `trust_proxy`), `kn_ws_path`,
  `kn_ws_query`, `kn_ws_header` (upgrade request headers), `kn_ws_subprotocol`.

### 3.4 Message encoding (`msgpack.h`, `json.h`)

v3.1.1 sockets carry `msgpack-lite` payloads followed by two `0x00` bytes
(`Krunker_v3.1.1/server/io.ts` `encodePacket`/`decodePacket`), packets being arrays
`["label", arg...]`. `kn_krunker_packet_encode(cjson_array, &buf)` /
`kn_krunker_packet_decode(data, len)` implement exactly that, via a cJSON <-> msgpack
bridge whose number rule matches msgpack-lite byte for byte (integers in the int32 range
use the smallest int format, everything else float64; verified against msgpack-lite from
the reference tree). Decoding maps bin to string, ext to null and non-string map keys to
their text; nesting is limited to 64 levels and truncated input fails cleanly. A
streaming writer/reader (`kn_mp_write_*`, `kn_mp_read`) is available for hot paths.
Internal links (brain <-> game server) may use JSON text frames; each protocol section
states which.

### 3.5 Crypto (`crypto.h`, OpenSSL)

* Passwords: `kn_password_hash()` -> `$pbkdf2-sha256$i=<iterations>$<salt>$<hash>`
  (PHC-style, standard base64 without padding, 16-byte random salt, 32-byte key,
  PBKDF2-HMAC-SHA256, default 600000 iterations, `kn_password_set_iterations()`, minimum
  10000). `kn_password_verify()` returns 1/0/-1 in constant time over the derived key and
  also accepts crypt(3) hashes (`$y$` yescrypt, `$6$` sha512crypt) when libcrypt is
  present; `kn_password_needs_rehash()` flags weaker hashes for upgrade on login. Store
  the string in a `text` column (`KN_PASSWORD_HASH_MAX` = 160).
* Randomness: `kn_random_bytes/hex/token(base64url)/u64` from OpenSSL's CSPRNG (no
  fallback; failure is an error).
* `kn_ct_equal`/`kn_ct_streq` constant-time compares (use for secrets and tokens),
  `kn_sha256`, `kn_sha1`, `kn_hmac_sha256[_hex]`, `kn_hmac_sha1`, `kn_pbkdf2_sha256`, hex,
  base64 (standard/url, padded or not; strict decoding) and base32 (RFC 4648).
* TOTP (RFC 6238, SHA-1, 6 digits, 30 s): `kn_totp_code(secret, step)`,
  `kn_totp_check(secret, code, now, window, &last_step)` (constant time over the window,
  refuses steps <= `last_step`: a code is accepted once).
* Signed tokens (brain -> game server join tickets, sessions): compact JWS HS256
  (`base64url(header).base64url(claims).base64url(HMAC-SHA256)`, header
  `{"alg":"HS256","typ":"JWT"}`), `kn_signed_token_create(secret, claims, ttl_s)` adds
  `iat`/`exp`; `kn_signed_token_verify()` checks the algorithm, the signature (constant
  time) and `exp`, and returns the claims. Standard JWT libraries can read them.

### 3.6 Database store layer (`db.h`, `store.h`, `knsvc_db`)

The coordinator (kn_brain) sits in front of any database through one driver
interface. Applications use `knsvc/db.h`; drivers implement `knsvc/store.h`.

| driver (`database.driver`) | library (Fedora / Debian package) | shape | pub/sub | notes |
| --- | --- | --- | --- | --- |
| `postgres` | libpq (`libpq-devel` / `libpq-dev`); tested with PostgreSQL 18 | async on the loop | native `LISTEN/NOTIFY` | server-side SQL functions (`migrations/postgres/0103`, `0104`, `0106`, `0201`) |
| `sqlite` | libsqlite3 >= 3.38 (`sqlite-devel` / `libsqlite3-dev`); tested 3.51.2 | worker threads | polled table | zero config: one file, WAL, `BEGIN IMMEDIATE`; several coordinator processes may share it |
| `mysql` | MariaDB Connector/C (`mariadb-connector-c-devel` / `libmariadb-dev`) or libmysqlclient; tested Connector/C 3.4.8 + MariaDB 11.8.6 | worker threads | polled table | MariaDB >= 10.6; MySQL >= 8.0.13 should work (untested) |

* **Choosing**: `database.driver` (section 2). Without it a configured server means
  `postgres`, otherwise the coordinator runs on `data/coordinator.db` (`sqlite`) and
  migrates it at start (`database.auto_migrate`), so a small community needs no setup.
  Everything (accounts, inventory, stats, the federation registry) is scoped to that
  database: each coordinator network owns its economy.
* **Pool**: `kn_db_new(loop, &cfg)`. postgres: `pool_size` non-blocking libpq
  connections (`PQconnectStartParams` + `PQconnectPoll` on the loop; `search_path` and
  `statement_timeout` are connection options). sqlite/mysql: `pool_size` worker threads,
  one blocking connection each, results handed to the loop through a pipe; the loop
  never blocks. Lost connections reconnect with exponential backoff (250 ms .. 10 s);
  queued queries wait for them. Statements and procs outside a caller's transaction are
  retried up to 3 times on deadlock / serialization / busy (class `KN_DB_E_RETRY`).
* **Queries** are parameterized only: `$1..$n` placeholders, text values, NULL element =
  SQL NULL (`KN_DB_ARGS(...)`, `0, NULL` for none). Drivers rewrite placeholders (sqlite
  `?n`, binding canonical integers as INTEGER; mysql escaped literals, bare integers
  after `LIMIT`/`OFFSET`). No ordering between two pool queries; chain dependent
  statements in callbacks, use a transaction, or a kn_proc.
* **`kn_db_query(db, sql, ...)`** sends `sql` as is to whatever driver is configured.
  Code that must run everywhere uses **`kn_sql`** with one text per dialect:
  `{.pg, .sqlite, .mysql, .any}`; the driver's own text wins, else `.any`. `.pg` is sent
  verbatim (every PostgreSQL statement of the brain is unchanged); `.any` and the other
  texts expand `{macros}` (outside string literals):

  | macro | postgres | sqlite | mysql |
  | --- | --- | --- | --- |
  | `{now}` | `now()` | `strftime('%Y-%m-%d %H:%M:%f','now')` (UTC text) | `NOW(3)` (session UTC) |
  | `{ago:s}` / `{later:s}` | `now() -/+ make_interval(secs => s)` | `strftime(..., '-/+' \|\| s \|\| ' seconds')` | `NOW(3) -/+ INTERVAL s SECOND` |
  | `{epoch_ms:ts}` / `{from_ms:ms}` | `extract(epoch ...)*1000` / `to_timestamp(ms/1000)` | julianday / `unixepoch` | `UNIX_TIMESTAMP` / `FROM_UNIXTIME` |
  | `{int:x}` `{float:x}` `{text:x}` `{bool:x}` `{json:x}` | casts | `CAST(...)`, `json()` | `CAST(...)`, `JSON_EXTRACT(x,'$')` |
  | `{jagg:x}` | `jsonb_agg` | `json_group_array` | `JSON_ARRAYAGG` |
  | `{html:x}` | inline `replace()` chain = `kn_html` | same | same |
  | `{hash:x}` (session token hash) | `sha256(convert_to(x,'UTF8'))` (bytea) | `kn_sha256(x)` (hex, registered C function) | `SHA2(x,256)` (hex) |
  | `{greatest:a,b}` `{least:a,b}` | `greatest` / `least` | `max` / `min` | `GREATEST` / `LEAST` |
  | `{insert_ignore} ... {ignore_conflicts}` | `INSERT ... ON CONFLICT DO NOTHING` | `INSERT OR IGNORE ...` | `INSERT IGNORE ...` |
  | `{for_update}` | `FOR UPDATE` | (nothing: the transaction holds the write lock) | `FOR UPDATE` |
  | `{serial_pk}` `{bigserial_pk}` | identity column | `INTEGER PRIMARY KEY AUTOINCREMENT` | `AUTO_INCREMENT PRIMARY KEY` |
  | `{ilike}` `{true}` `{false}` | `ILIKE` `true` `false` | `LIKE` `1` `0` | `LIKE` `1` `0` |

  `kn_db_query_sql(db, &sql, ...)`, `kn_db_tx_query_sql`, `kn_dbconn_exec_sql`. In the
  brain `SQL(.pg = "...", .any = "...")` (`services/brain/db_procs.h`) builds one inline.
  The mysql session runs with `ANSI_QUOTES,PIPES_AS_CONCAT,NO_BACKSLASH_ESCAPES` so
  `"identifiers"`, `||` and `'...'` mean the same as in PostgreSQL and SQLite.
* **`kn_proc`** for multi-statement operations: `{name, pg, run}`. On postgres the `.pg`
  text runs (the existing plpgsql function call); everywhere else (and on postgres when
  `.pg` is NULL, e.g. `P_ENSURE_CENTRAL`) the C `run(kn_dbconn *, n, params)` executes on
  a worker connection inside its own transaction (rolled back when it returns an error)
  and returns a result shaped like the `.pg` query's: `kn_db_call` / `kn_db_tx_call`
  (inside a sqlite/mysql transaction; C procs cannot join a libpq transaction). `run`
  executes on a database thread: only its parameters and read-only data. The brain's
  procs are in `services/brain/db_accounts.c`, `db_social.c`, `db_mm.c`; the shared C
  versions of `kn_account_json`, `kn_add_funds`, `kn_add_skin`, `kn_active_ban`,
  `kn_html`, `kn_level` in `db_common.c`. User-facing refusals are SQLSTATE `P0001` with
  the message in `kn_db_user_message()` (plpgsql `RAISE`, `kn_db_result_user_error()` in
  C, sqlite trigger `RAISE(ABORT, msg)`, mysql `SIGNAL SQLSTATE '45000'`).
* **Results**: one `kn_db_result` per callback (lives until the callback returns):
  `kn_db_ok`, `kn_db_error`, `kn_db_sqlstate`, `kn_db_errclass_of` (`UNIQUE`,
  `CONSTRAINT`, `USER`, `CONNECTION`, `RETRY`, `TX_ABORTED`, `TIMEOUT`, `SYNTAX`; native
  codes are mapped: sqlite `SQLITE_CONSTRAINT_UNIQUE` / mysql 1062 -> 23505, mysql
  2006/2013 -> 08006, 1213 -> 40P01 ...), `kn_db_rows/cols/col/colname/coltype/get/
  get_by/get_int64/get_double/get_bool/isnull/affected/last_id`, `kn_db_result_to_json` /
  `kn_db_row_to_json`. Column types come from the driver (postgres OIDs; sqlite declared
  type `BOOLEAN` / `JSON TEXT` / `INTEGER` / `REAL`, else the value's storage class; mysql
  field types, `TINYINT(1)` = bool, JSON format metadata) and can be forced with an alias
  suffix: `AS name__j` json, `__b` bool, `__i` int, `__f` float, `__t` text (the suffix is
  stripped; works on postgres too). Synthetic errors: `08006` connection lost
  mid-query, `08003` pool freed, `26000` unknown prepared statement, `25P02` statement in
  a failed transaction, `40000` COMMIT of a transaction in which a statement failed.
* **Transactions**: `kn_db_begin(db, cb, ud)` pins a connection and calls back with a
  `kn_db_tx`; `kn_db_tx_query[_sql]` / `kn_db_tx_call` / `kn_db_tx_exec_prepared` run in
  order on it; `kn_db_tx_commit` / `kn_db_tx_rollback` release it. Every driver follows
  PostgreSQL's rule: after a failed statement the transaction only fails (25P02) and
  COMMIT rolls back and reports 40000. If the pinned connection dies, the remaining
  statements fail with 08006. sqlite begins with `BEGIN IMMEDIATE` (writers serialise
  across processes; `busy_timeout_ms`), mysql with `START TRANSACTION` at READ COMMITTED
  (PostgreSQL's level).
* **Prepared statements**: `kn_db_prepare(db, name, sql)` once, `kn_db_exec_prepared(db,
  name, ...)`; postgres prepares lazily per connection (again after reconnecting); sqlite
  keeps a per-connection statement cache for every statement; mysql runs the text.
* **Publish/subscribe**: `kn_db_listen(db, channel, cb, ud)`, `kn_db_notify(db, channel,
  payload)` (payload <= 7900 bytes). postgres: `LISTEN`/`pg_notify` on a dedicated
  connection (re-LISTENed after reconnects). Drivers without it (`KN_DB_CAP_NOTIFY`
  unset: sqlite, mysql) get rows in table `kn_pubsub(id, channel, payload, at_ms)`
  (created on first use) that every listener polls every `database.notify_poll_ms`
  (default 100 ms; delivered once per process, rows older than a minute are pruned).
  **Latency**: a cross-coordinator event arrives within `notify_poll_ms` plus one query
  instead of ~1 ms, so the federation's view of another coordinator's lobbies, host
  requests and `assign` heads-ups lags by up to ~0.1-0.3 s (the 1 s registry refresh
  and the 500 ms host-request poll stay as the fallback, as on postgres). With the Redis hot tier
  (3.11) `kn_db_set_pubsub()` installs Redis as the transport: notifies go over Redis while it is
  up, listeners keep the database path too (the poll slowed to `redis.pubsub_fallback_poll_ms`).
* **Synchronous API** for tools, tests and procs: `kn_dbconn_open/exec/exec_sql/call/
  exec_script/close`, `kn_dbconn_driver_name/dialect/native` (`kn_dbconn_pg`,
  `kn_db_result_pg` remain for postgres-only code).

#### Writing a new adapter

1. `services/common/src/drivers/store_<name>.c` defining
   `const kn_store_driver kn_store_driver_<name>` (`knsvc/store.h`): `name`, `dialect`
   (a new dialect name makes `kn_sql` fall back to `.any`), `caps`, `default_port`,
   `default_pool`, a `{macro}` table for `.any` texts (every macro of the table above),
   and `kn_store_blocking_ops`: `connect` / `disconnect`, `exec` (one statement with
   `$n` parameters -> `kn_db_result`; use `kn_store_sql_numbered`, `kn_store_sql_qmark` or
   `kn_store_sql_inline` for placeholders and `kn_db_result_new` / `kn_db_result_add_row`
   for rows, `kn_db_result_error` with a mapped SQLSTATE for failures), `exec_script`,
   `broken`, optional `lock` (migrations) and `interrupt`, `native`, `begin_sql`. That is
   all a blocking driver needs: the core runs it on worker threads with pooling,
   transactions, kn_procs and pub/sub emulation. An async driver additionally fills
   `kn_store_async_ops` (see `store_postgres.c`).
2. One registration line in `services/common/CMakeLists.txt`:
   `knsvc_store_driver(<OPTION> <name> src/drivers/store_<name>.c <library target>)`.
   It builds the file when `KNSVC_STORE_<OPTION>` is ON and lists the driver in the
   generated `knsvc_store_drivers.h` that `src/store_drivers.c` includes (no core edit).
   Add the `option(KNSVC_STORE_<OPTION> ...)` and its `find_package` /
   `pkg_check_modules` next to the others in `services/CMakeLists.txt`. Applications
   can also link a driver of their own and call `kn_store_driver_register()`.
3. Nothing else in knsvc: `database.driver = "<name>"` selects it.
4. `services/db/migrations/<dialect>/` with the same version numbers as the other
   dialects; `.sqlite`/`.mysql`-style texts in `kn_sql` only where the `.any` text cannot
   express a statement.
5. Run `knsvc_test_store <name>` (add it to the `foreach` in `services/common/CMakeLists.txt`)
   and the brain suites with `KN_TEST_DB_DRIVER=<name>` (teach `tests/db_scratch.py` to
   create a scratch database).

### 3.7 Migrations (`migrate.h`)

* Files: `services/db/migrations/<dialect>/NNNN_name.sql` (`postgres/`, `sqlite/`,
  `mysql/`; 4+ digits), applied in numeric order. The runner uses the connection's
  dialect subdirectory when it exists, else the directory itself (flat test dirs).
  Version ranges: `0001-0099` core, `0100-0199` accounts/social (svc-accounts),
  `0200-0299` matchmaker/registry (svc-matchmaker), `0300-0399` game servers/stats.
  A schema change gets the **same version in every dialect directory**; the sqlite and
  mysql histories start from the consolidated current schema (their 0100-0201 equal
  postgres 0100-0203; the postgres SQL functions of 0103/0104/0106/0201 are C kn_procs
  there, 3.6). Files must not contain `BEGIN`/`COMMIT`; dollar-quoted function bodies
  (postgres) and `BEGIN ... END` trigger bodies (sqlite/mysql) are fine.
* Each file runs in its own transaction together with its row in
  `kn_migrations(version primary key, name, checksum sha256 hex, applied_at)`, so a
  failing file leaves nothing behind and the error names the file (MySQL commits DDL
  implicitly: only the bookkeeping row is atomic there). Concurrent runners (several
  brains starting at once) serialise on a postgres advisory lock / MySQL `GET_LOCK` /
  sqlite `BEGIN IMMEDIATE` plus a re-check. Applied files are never re-run; an applied
  file whose checksum changed is reported as a warning. Duplicate versions are an error.
* Entry points: `kn_migrate [--config F] [--status] [--dry-run] [--dir D] [--set
  database.name=...]` (`make db-migrate`, `make db-status`), `kn_brain --migrate`
  (`kn_migrate_main()`), or `kn_migrate_run()` on a `kn_dbconn`.

### 3.8 Logging (`log.h`)

`kn_log_init("brain")` + `kn_log_configure(cfg)`; `KN_INFO("mod", fmt, ...)` and
`KN_TRACE/DEBUG/WARN/ERROR/FATAL`. Text lines
`2026-10-10T12:00:00.123Z INFO  [brain] http: listening on 127.0.0.1:5050` or JSON lines
`{"ts":...,"level":"info","svc":"brain","mod":"http","msg":"..."}`; thread-safe.

### 3.9 Tests

`ctest -L knsvc` (`make services-test`) in the build directory:

| test | covers |
| --- | --- |
| `knsvc_buf` | buffers, JSON helpers, URL parse/encode/decode |
| `knsvc_config` | file/env/CLI precedence, typed getters, lists, merged subtrees, redaction, errors |
| `knsvc_crypto` | SHA-1/256, HMAC (RFC 4231), PBKDF2 (RFC 7914), base64 (RFC 4648), WebSocket accept key, jwt.io HS256 sample, password hash/verify/rehash, yescrypt + sha512crypt, expired/tampered tokens |
| `knsvc_msgpack` | byte-exact output vs msgpack-lite, every format, framing, malformed and random input |
| `knsvc_loop` | timer order/cancel/repeat, posts, cross-thread posts and stop, fds; epoll and poll |
| `knsvc_http` | 200k mutated + 50k random parser inputs, every prefix incomplete, keep-alive + pipelining, params/query, chunked + 100-continue, 19 error statuses, 431 limits, CORS preflight, async + abandoned requests, streaming, 408 and idle timeouts |
| `knsvc_ws` | knsvc client <-> server (text/binary/ping/close codes/subprotocol/auth header, refused connect), raw-socket edge cases (handshake 400/426, fragmentation with interleaved ping, 16-bit lengths, 13 protocol violations with their close codes, byte-by-byte frames), pong timeout, on_drain, 1008 overflow |
| `knsvc_ws_interop` | python `websockets` client against the knsvc server (UTF-8, 51 KB binary, fragmented text/binary, ping, 200 ordered messages, close code echo, 1009, deflate declined) and the knsvc client against a python server; skipped without `websockets` |
| `knsvc_db` | real PostgreSQL in database `krunknative_svccore` (`KN_TEST_DATABASE`), own schema: typed results/JSON, SQL-injection-shaped params, unique violation, syntax error, 40 concurrent queries, prepared statements, commit/rollback/failed-commit, LISTEN/NOTIFY, `pg_terminate_backend` of every pool connection -> reconnect + NOTIFY again, free with queued queries |
| `knsvc_migrate` | order, idempotence, atomic failure, checksum warning, duplicates, dry run, status, and the repository's own migrations into a fresh schema twice |
| `knsvc_store_sqlite` / `_mysql` / `_postgres` | the store layer on each driver (`tests/test_store.c`): typed results + alias suffixes, injection-shaped params, unique/syntax error classes, the `{macros}`, 60 concurrent inserts, prepared statements, commit / rollback / aborted-transaction rule, kn_proc commit + rollback on a user refusal + inside a transaction, pub/sub, `kn_dbconn` + dialect-directory migrations with atomic failure, free with queued work |
| `brain_social_ws_sqlite` / `_mysql`, `brain_federation_*`, `brain_ranked_*` | the brain suites (6, 7.7) on sqlite (temp file, always runs) and MySQL/MariaDB |
| `knsvc_redis` | `knsvc/redis.h` against a throwaway `valkey-server` / `redis-server` (3.11): AUTH / SELECT / a wrong password, binary-safe strings, nil and error replies, 2000 pipelined commands in order, EVAL, pub/sub, kill -9 (pending callbacks fail, commands refused, state callback, reconnect + re-SUBSCRIBE), a SIGSTOPped server (command timeout), free with pending commands |
| `brain_social_ws_redis` / `_sqlite_redis`, `brain_ranked_*redis`, `brain_season_*redis`, `brain_federation_*redis` | the same suites with every brain on a throwaway Redis (`tests/with_redis.py`, fails when the brains never used it), on PostgreSQL and SQLite |
| `brain_redis_failover_sqlite` / `_postgres` | `tests/redis_failover_test.py`: two brains, one database, one Redis: shared session cache, join records, rate limits, presence and a chat push across brains; Redis killed -9 under load (no request fails), restarted (both reconnect, presence and cache refill); logout / password change drop cached sessions on both brains at once |
| `brain_dbcopy` | `tests/dbcopy_test.py`: `kn_dbcopy` SQLite -> PostgreSQL (3.11): checksums, follower counters, int64 premium time, sequences past SQLite's counter, refusal / `--replace` / `--verify-only`, kn_brain logging in a copied account on PostgreSQL |

DB tests skip (exit 77) when their server is unreachable. PostgreSQL: create the test
database once with `sudo -u postgres createdb -O krunknative krunknative_svccore`.
MySQL/MariaDB: `KN_TEST_MYSQL_HOST` / `_PORT` / `_USER` / `_PASSWORD` (default
`127.0.0.1:3306 root`) must be allowed to create and drop databases (`kn_test_*`).
`ctest -L sqlite` / `ctest -L mysql` run one driver; the brain scripts take
`KN_TEST_DB_DRIVER=sqlite|mysql` directly (`services/brain/tests/db_scratch.py` makes the
per-run database). The suite also runs
clean under clang `-fsanitize=address,undefined` with leak detection.

### 3.10 Outbound mail (`smtp.h`)

* `kn_smtp_send(cfg, mail, ...)` (blocking): TCP with a connect timeout, implicit TLS (port 465,
  `tls: implicit`) or STARTTLS (required when configured; bytes pipelined after the 220 are
  refused), TLS >= 1.2 with the system CA store (+ `ca_file` / `KNSVC_TLS_CA_FILE`) and host name
  verification, EHLO, AUTH PLAIN (else LOGIN; `auth` forces one; never over clear text unless
  `allow_plain_auth`), MAIL / RCPT / DATA with dot-stuffing, QUIT. Returns `KN_SMTP_OK`,
  `KN_SMTP_ETEMP` (network, TLS, timeout, 4xx) or `KN_SMTP_EPERM` (5xx, invalid message) and a
  credential-free reason (the server's reply line).
* `kn_mail_build`: RFC 5322 + MIME: `Date`, `From` (display name quoted or RFC 2047 encoded),
  `To`, `Subject` (encoded words when not ASCII), `Message-ID` (`<random@from-domain>`),
  `MIME-Version`, `Auto-Submitted: auto-generated`, multipart/alternative text + HTML (or text
  only), UTF-8 quoted-printable bodies (7-bit clean, CRLF, <= 76 columns); CR/LF in any header
  field is refused (header injection).
* `kn_mailer` (what event-loop code uses): `kn_mailer_send` copies the mail into a queue drained
  by one worker thread; the result (status, Message-ID, error) is posted back to the loop
  (`kn_loop_post_threadsafe`), so DNS, TCP, TLS and the server never block it. Temporary failures
  are retried (`retries`, `retry_delay_ms` doubling); `queue_max` bounds the backlog;
  `kn_mailer_free` stops the worker and drops undelivered callbacks.
* Test: `knsvc_smtp` (`tests/smtp_test.py` + `tests/test_smtp.c` against `tests/fake_smtp.py`
  on a free port: STARTTLS / implicit TLS with a self-signed certificate, AUTH PLAIN / LOGIN,
  5xx and 4xx paths, STARTTLS injection, the parsed MIME message, no credentials in the
  transcript, the queue with retries while a loop timer keeps ticking).

### 3.11 Redis hot tier (`redis.h`, `services/brain/hot.c`, redis-tier)

An optional Redis / Valkey in front of the brain's database. PostgreSQL (or the SQLite file of a
small coordinator) stays the source of truth; Redis only holds what can be rebuilt from it or may
be lost: caches, short-lived records, counters and a message bus.

**What it speeds up, and what it does not.** Gameplay never touches it: a match runs in memory in
`server_bin` on the edges, which talk to their brain over the internal WebSocket and never to a
database. Redis serves the brain's backend paths: session checks (`/auth/me`, token resumes of the
social socket, the game servers' `token` op, seek-game with a token), the central-auth join
records (`/auth/join` -> `/auth/hasJoined`), rate limits, presence and social pushes (chat, gifts,
`ua` after a commit) across brains, leaderboard pages, and the cross-brain pub/sub of the
federation (6.1). Logins and registrations are dominated by PBKDF2 and their writes; stats commits
and the economy stay transactions in PostgreSQL.

**Measured on kru (read only, 2026-10-13 00:40 UTC).** Nothing is hot today. kn-brain-coord: 16 MB
RSS, 0.72 s CPU in its first 10 min; finder 5 MB, 0.19 s; `/mm/status` 0.2 ms, `/servers` 0.4 ms on
loopback. coord.db 7.6 MB: 17 accounts, 17 sessions, 306 `match_results` in 28 h (peak 58 commits
per hour, 0.016/s), 19 logins/registrations in total, 38.7k `inventory` rows (staff grants). 20 game
servers (4 online_mode) linked, 0 players at that moment: the internal WebSocket carries a snapshot
per node every 5 s plus pings (~5 frames/s). `brain.federation` is false (one coordinator), so there
is no `kn_pubsub` table and no 100 ms polling at all. No request log exists (OpenResty `access_log
off`, kn_brain logs no requests), so rates come from the tables and the journal. The host itself is
the constraint: 2 vCPU with load ~13 and CPU pressure 54 % (`/proc/pressure/cpu`, from non-KrunkNative
processes), and 2.1 GB free of 32 GB (journal 2.5 GB, updates store 2.5 GB). Under heavy traffic the
first wall is PostgreSQL's commit latency on the session check (below: ~300 checks/s with the default
4 connections), long before CPU: that is the path the cache removes.

**Load test** (`services/brain/tests/load_test.py` + `kn_loadgen`; Release build, one kn_brain with
every role, 200 accounts, 10 s closed loop per endpoint, 2 x 64 keep-alive connections; a 32-thread
workstation with local PostgreSQL 18 and Valkey 9, loopback; PBKDF2 at 10000 iterations; numbers move
by ~10-20 % between runs). req/s | p99 ms:

| mode | session `/auth/me` | login | finder `/servers` | `/mm/presence` | leaderboard (WS) |
| --- | --- | --- | --- | --- | --- |
| sqlite | 14527 \| 13.3 | 2964 \| 5.5 | 74923 \| 3.0 | 80190 \| 3.2 | 4622 \| 19.4 |
| postgres (pool 4) | 298 \| 492.6 | 299 \| 43.3 | 73298 \| 3.1 | 85056 \| 2.8 | 4623 \| 17.5 |
| postgres + redis (pool 4) | 12946 \| 15.4 | 290 \| 44.5 | 74107 \| 3.0 | 65052 \| 2.8 | 4620 \| 16.1 |
| sqlite + redis | 21667 \| 7.6 | 2958 \| 5.2 | 74846 \| 3.0 | 67198 \| 2.7 | 4625 \| 16.4 |
| postgres (pool 16) | 1210 \| 132.4 | 561 \| 67.1 | 71351 \| 3.3 | 86217 \| 2.6 | 4615 \| 24.5 |
| postgres + redis (pool 16) | 23603 \| 7.0 | 580 \| 26.6 | 61024 \| 7.1 | 55017 \| 5.4 | 4599 \| 59.9 |

A session check on PostgreSQL is a write (it slides the session's expiry) and waits for the commit's
fsync; the cache answers it from Redis plus one read of the account (bans and funds stay current),
so it is 40x (pool 4) / 20x (pool 16) faster there. SQLite commits without fsync per transaction
(WAL, `synchronous=NORMAL`), hence its fast but single-writer numbers. The finder list and presence
are in memory in every mode (presence adds one Redis call for the global count); leaderboard pages
come from the per-brain 1 s cache here and are capped by the social socket's 20 frames/s per socket
(128 sockets x 18/s); with several brains the shared page saves one query per brain and TTL. Login
does not change (hashing and writes).

**Why our own Redis.** abulxt's edges already share a Redis on amph (6379, opened to the edges); it
is that project's critical store (every on-demand TLS certificate of its customers' domains) with
another owner, so KrunkNative never writes to it nor depends on it: a flush, an eviction policy, a
password rotation or an outage there must not log our players out, and our keys must not compete
with its memory. We reuse its pattern instead: a Redis per project on a central host, reachable only
over loopback / private addresses with its own password, later read replicas next to the edges.
Ours is `kn-redis.service` on kru (127.0.0.1:6380, role `kn_redis`); amph and abulxt are never
touched.

**Keys** (`redis.prefix`, default `kn:`; every multi-key operation is one Lua `EVAL`, so standalone or
replicated Redis / Valkey >= 6.2, not Redis Cluster):

| key | type | lifetime | written | rebuilt from / invalidated |
| --- | --- | --- | --- | --- |
| `sess:<sha256(token)>` | string: account id | `session_cache_s` (60 s) | a session check that went to the database | miss -> `P_SESSION_ACCOUNT` (slides the expiry). Logout, `las`, a password change and session trims delete it (and every cached session of the account through `sessidx:<id>`) after the database commit. The raw token never reaches Redis |
| `sessidx:<account>` | set of hashes | 2 x `session_cache_s` | with each cached session | revoke-all |
| `sessrev:<account>`, `sessdead:<hash>` | string: Redis time (us) | 2 x `session_cache_s` | at revocation | tombstones: a check that started before a revocation does not store its (now stale) answer |
| `join:<serverHash>` | JSON `{id, username, ip, profile}` | 30 s (`AUTH_JOIN_TTL_MS`) | `/auth/join` | hasJoined on another auth brain; adopted with its remaining lifetime and checked like a local record |
| `rl:<bucket>:<ip>` | counter | the window (60 s / 1 h) | every allowed hit (`auth_login`, `auth_register`, `auth_join`, `auth_has_joined`, `finder_submit`) | fixed window; the per-process limiter still runs first, and a key over the shared count is refused locally until its window ends (async: a burst can pass one extra hit per brain) |
| `online` | sorted set: account -> expiry ms | members expire with the refresh | social login / every `presence_ttl_s`/3 | `/mm/presence` global count (expired members pruned on read); re-registered at once when Redis comes back |
| `pres:<account>` | hash: brain id -> expiry ms | `presence_ttl_s` | same | where an account has sockets: pushes to other brains |
| `lb:<query>:<region>[:s<season>]` | JSON rows | `accounts.leaderboard_cache_s` (30 s) | read-through on a local miss | the database query; expires |

Channels: `<prefix><channel>` for the federation's `kn_mm_lobby`, `kn_mm_host`, `kn_mm_host_done`,
`kn_mm_assign` (`kn_db_set_pubsub`, 3.6: a notify is published on Redis while it is up, else through
the database; listeners always keep the database path too, the sqlite/mysql poll slowed to
`pubsub_fallback_poll_ms`, so a brain whose Redis link is down still reaches everyone), and
`<prefix>push:<brain id>` (a social push for an account that also has sockets on that brain; the
publishing brain reads `pres:<account>` in the same script). Every brain sharing a database must have
the same `redis.*` (or none), or notifications between the two groups go only through the database.

**Authoritative in PostgreSQL**: accounts, credentials, sessions (the cache only shortcuts their
check), bans, players / stats / KR / inventory / every ledger, clans, social data, the market and
trades, seasons and the Battle Pass, ranked ratings and queues, published maps, KrunkScript storage,
`mm_*` registry rows (federation). Nothing is written to Redis that is not also in the database or
deliberately ephemeral.

**Failure modes.** The hot tier never answers for the database: when a command cannot be sent the
caller takes the database path synchronously, and a sent command that fails (connection lost, no
reply within `command_timeout_ms`) completes as a miss. Redis down or restarting (persistence is off
on purpose): session checks write to the database again until the cache refills; presence restarts
from each brain's refresh (the brains re-register immediately on reconnect); rate-limit windows start
over; leaderboard pages are queried again; pub/sub messages published during the gap are lost, which
the federation already tolerates (1 s registry refresh, 500 ms host-request poll); pushes to sockets
on another brain are lost for the gap. A hung Redis costs each pending call at most
`command_timeout_ms`, then the connection is dropped and rebuilt in the background (250 ms .. 5 s).
`brain_redis_failover_*` and the cutover harness kill it under load: no request failed. Why no
persistence: a snapshot restored after a crash could resurrect a session revoked while Redis was down
(the revocation's tombstone never reached it). Residual risk: a brain cut off from Redis while Redis
stays up for the others cannot drop cached sessions it revokes; they stay valid on the other brains
for at most `session_cache_s` (60 s). Bans are not affected (the account is read from the database on
every check).

**Security.** Bound to loopback (`bind 127.0.0.1`, `protected-mode yes`); `user default off` and one
ACL user `kn_brain` limited to `~kn:*` keys, `&kn:*` channels and no `@dangerous` commands (no
FLUSHALL, CONFIG, KEYS, DEBUG...); the password is generated on the host into `secrets.env`
(`KN_REDIS_PASSWORD`, never in the repository or logs) and in `/etc/kn-redis/redis.conf` (root:redis
0640); systemd sandboxing (`ProtectSystem=strict`, no capabilities, `MemoryDenyWriteExecute`, ...).
Between regions hiredis 0.14 (Debian 11/12) has no TLS, so `rediss://` is refused: replicas in other
regions go over WireGuard / the private network or a TLS tunnel (stunnel / spiped), never over a
public port; a replica gets `replica-read-only yes`, `masterauth` and its own ACL user.

**Multi-region (later).** A read replica per region next to the edges serves `sess:` / `lb:` reads
and the pub/sub (Redis forwards PUBLISH to replicas' subscribers), writes go to kru's primary; with a
PostgreSQL primary on kru the brain of a far region then waits for neither for the hot paths.

**Phase 2 (not built).** The registry (`mm_nodes` / `mm_lobbies` / `mm_reservations`) as hashes with
`PEXPIRE node_ttl_s` instead of heartbeat rows plus every brain's 1 s reload (rebuilt by the game
servers' 5 s snapshots after a Redis restart); the finder table shared by several finders; leaderboards
as sorted sets updated write-behind by stats commits (`ZINCRBY`) and rebuilt from `players` every few
minutes; `P_SESSION_ACCOUNT` sliding the expiry at most once a minute (or `synchronous_commit = off`
for that statement) so the database path is cheap too.

**Moving a coordinator to PostgreSQL.** `kn_dbcopy --sqlite FILE` (`services/common/tools/kn_dbcopy.c`)
copies every table of a coordinator's SQLite file into the configured, already migrated PostgreSQL
database in one transaction (parents first, user triggers off so `follows_count` does not count twice,
values converted by the destination type, sequences past the copied ids and SQLite's AUTOINCREMENT
counters), then verifies per table the row count and a SHA-256 over the rows normalised by type and
sorted (exit 0 identical, 3 mismatch, 1 error; refuses a destination with accounts unless `--replace`;
`--verify-only`). On a copy of production's coord.db: 54 tables, 78130 rows copied in 1.5 s, verified
identical in 1.9 s; it found that `players.player_premium` holds 315360000000 (ten years of premium in
ms) which overflowed PostgreSQL's `integer`: migration `postgres/0110_premium_bigint.sql`. Production
runs it through `kn-db-cutover` (deploy/ansible/README.md "Database cutover").

## 4. Brain: public HTTP routes

Every route is served at `/mm/<name>` (v3.1.1 brain path) **and** `/<name>` (what
the client calls on `matchmakerURL` when nginx strips the prefix, as in the jail's
`brain.conf.j2`: `matchmaker.<domain>/<x> -> brain/mm/<x>`). Responses are JSON with
`Cache-Control: no-cache, no-store, must-revalidate` and CORS headers
(`brain.cors_origins`); `OPTIONS` preflights are answered automatically. Errors
are `{"success": false, "error": "<Name>"}` with the v3.1.1 client's names (the client maps them to
`matchmaker.*` strings): `InvalidGameId` (404), `GameFull` (409),
`NoServersMatchQuery` (503), `NoAvailableServers` (503), plus `updating` (503,
brain draining).

### Lobby tuple

`[gameId, region, players, maxPlayers, meta]` — exactly the v3.1.1
`buildLobbyTuple()` shape the Servers window destructures
(`var [id, region, players, max, meta] = tuple`).

* `gameId` = `"<nodeId>:<localLobbyId>"` (the brain namespaces the node's local id
  on ingest, as registry.ts does). It is the canonical id (JOIN `game_id`, the join
  ticket's `nid`/`lid`, reservations, `mm_lobbies`).
* Links use its short form `"<short node>:<localLobbyId>"` (`shared/src/server_names.c`):
  a node in a 2-letter region whose id stem (without the trailing instance number) is
  longer than 2 chars is the region code + instance number (`CHITOWN2` in `CH` ->
  `CH2`, so `?game=CH2:kz862`); stems of up to 4 chars stay (`NY`, `NY2`, `LOC`); longer
  ones keep their first 3 letters/digits (`KRUNKCLUB1` -> `KRU1`). `?game=`,
  `/mm/game-info?game=` and `/mm/seek-game?game=` accept both forms
  (`registry_lobby_find`: canonical first, else the short node id, case-insensitive, of
  a lobby with that local id in the lobby's or its node's region). The Servers window
  shows the server's proper name instead (`Chicago 2`: the region's city + instance
  number; other nodes their id, `NY 2`), the node id as tooltip.
* `meta` keys (sent by the game server, passed through; the brain adds `nodeId`,
  `lobbyId`, `brain`):

| key | type | meaning |
| --- | --- | --- |
| `i` | string | display label (map name, or `"<mode>_<map>"` for customs: the client takes `i.split(/_(.+)/)[1]` as the custom map name) |
| `v` | string | game version (`dataQuery.v` filter) |
| `cs` | bool | custom game (Servers window "Custom Games" group; never quick-joined) |
| `m` | string/null | custom map name |
| `pr` | bool | password protected (star icon) |
| `earnKR` | bool | KR-earning lobby (check icon) |
| `mode`, `modeName` | int, string | mode index / name |
| `map` | string | map name |
| `host`, `port` | string, int | game server native TCP endpoint |
| `webUrl` | string | optional: ws(s) URL web clients use (net-bridge `/native`) |
| `timeLeft` | int | seconds left in the round |
| `draining` | bool | the server is shutting down (not joinable) |
| `proto`, `build`, `commit`, `commitTime` | string, string, string, int | build identity of the server (`proto` "M.m", `build` = the 12-hex commit, `commit` the same, unix s): see 8.12 |
| `outdated`, `incompatible` | bool | added by `/mm/game-list` (8.12); not sent by servers |
| `mapIndex` | int | Krunker's official mapIndex (the v10.0.2 list, `shared/include/map_catalog.h` `map_catalog_map_index`; a community map sends none): seek-game `dataQuery.dsma` ([v10/matchmaker-token.md](v10/matchmaker-token.md)) |
| `flavor` | string | product flavor pool (`shared/include/flavor.h` key): `"strike"` from a Krunker Strike `server_bin` (or stamped from its node's hello); absent = the base game. See "Flavor pools" (6.2) |

### Routes

| route | v3.1.1 | response |
| --- | --- | --- |
| `GET /ping` | publicRoutes `/ping` | `{"success":"true"}` (region latency probe, also `/mm/ping`) |
| `GET /mm/ping-list[?hostname=]` | `registry.regions()` | `{"<v10 region id>": "host[:port]"}` — one ping host per listed v10 region (the id each node's KN code maps to: [v10/regions.md](v10/regions.md)); the host is `brain.regions[].ping_host` (by the v10 id, then the KN code), else the brain's public URL; the client fetches `http(s)://<value>/ping` |
| `GET /mm/regions` | (Krunker Box Office) | `[{"id","name","players","games","nodes"}]` known + live regions |
| `GET /mm/game-list[?hostname=]` | `/mm/game-list` | `{"success":true,"totalPlayerCount":N,"games":[tuple,...]}` all live lobbies of every region |
| `GET /mm/game-info?game=ID` | `/mm/game-info` | the tuple; unknown id -> best quick-join pick (v3.1.1), `InvalidGameId` if none |
| `GET /streams/top` (also `/api/streams/top`) | v10 `streams.getTopStreams` (`/streams/top?cb=2`, deob-10.0.3.js:29925-29926; consumer 162110-162195) | `{"success":true,"data":{"streams":[{id,username,title,viewerCount,thumbnailUrl,startedAt,provider,languageCode,featured}],"promo":true,"dropCampaigns":[]}}`: Twitch Helix live streams of `streams.twitch.game` (default "Krunker"), cached `streams.cache_s` (30), the last good list while Twitch is down. 10.0.3 reads `data.streams` (an array; anything else is its error state), splits `featured` from the rest sorted by `viewerCount`, shows the promo card for `promo === true` and reads `dropCampaigns || []` (`streamerNames` per campaign, 162248): `featured` comes from `streams.featured` (logins), `promo` from `streams.promo` (default true, as api.krunker.io answered on 2026-10-24), `dropCampaigns` from `streams.drop_campaigns`. No Twitch source (`streams.enabled` off, or no `streams.twitch.client_id` / `client_secret`, env `KN_STREAMS_TWITCH_CLIENT_ID` / `KN_STREAMS_TWITCH_CLIENT_SECRET`): **200** `{"success":true,"data":{"streams":[]}}`, the empty list the menu shows as no streams (not a 503: 10.0.3 throws on any status >= 500, deob-10.0.3.js:29806). A Twitch failure with no list yet, or too many waiting requests: `503 StreamsUnavailable`. `services/brain/streams.c`, `streams_norm.c`; the client never holds the credentials |
| `GET /streams/thumb?url=` (also `/api/streams/thumb`) | KrunkNative: the Live Streams cards' previews, from before the native engine loaded remote images (client/src/ui/remote_images.c). No client asks it now: a card's `<img src>` is its `thumbnailUrl`, as v10 sets it | `{"success":true,"data":{"url","image":"data:image/jpeg;base64,..."}}`. Only `https://` URLs on `streams.thumbs.hosts` (default `["static-cdn.jtvnw.net"]`) with a plain `.jpg` / `.jpeg` / `.png` path (no query, escapes or dot segments) are fetched: `400 InvalidThumbnail` otherwise. No redirects, at most `streams.thumbs.max_bytes` (256 KiB; `502 ThumbnailTooLarge`), JPEG / PNG only (`502 ThumbnailNotImage`), other failures `502 ThumbnailUnavailable`; answers cached `streams.thumbs.cache_s` (300), failures `streams.thumbs.fail_cache_s` (30), 32 URLs, 4 transfers at once. `streams.thumbs.cdn_url` (tests: a stub CDN) fetches the allowed URL's path from another base. `services/brain/stream_thumbs.c` |
| `GET /mm/generate-token` | Krunker `matchmaker/generate-token` | `text/plain` validation token for the next seek-game (rate-limited: 120/min per address, then 429 `TooManyRequests`) |
| `GET /mm/seek-game?region=R[&game=ID][&autoChangeGame=true][&dataQuery=JSON][&token=SESSION][&validationToken=T]` | `/mm/seek-game` | `{"gameId","lobbyId","host","port","clientId","changeReason","region","webUrl","map"}` |
| `GET /mm/host-game?region=R&config=JSON[&token=SESSION][&captcha=PAYLOAD]` (also `POST`, JSON body `{region, config, captcha}`) | (game socket `custom` in v3.1.1) | same as seek-game, for a freshly hosted custom lobby; the hosting captcha (7.10) |
| `GET /mm/captcha?for=login\|register\|hosting` | (Krunker Box Office; v10.0.3 gets its challenges from Cloudflare and over the game socket) | an ALTCHA challenge `{"algorithm":"SHA-256","challenge","maxnumber","salt","signature"}` (7.10, [v10/captcha.md](v10/captcha.md)); 400 `BadRequest`, 503 `Unavailable` without a key |
| `GET /mm/brains` | (Krunker Box Office) | `[{"id","region","publicUrl","nodes","lastSeen"}]` live brains (federation debug) |
| `GET /mm/status` | (Krunker Box Office) | `{"brain","region","version","nodes","lobbies","players","draining"}` |
| `GET /api/player-count`, `/social/player-count` | same | `{"playerCount":N,"socialPlayerCount":N,"editorPlayerCount":N}` players in lobbies, signed-in social sockets here, editor windows seen in the last 60 s |
| `POST /api/editor/presence` | (Krunker Box Office) | body `{"session":"..."}`: an editor window's heartbeat (every 20 s from `client/src/editor/editor.c`), counted in `editorPlayerCount` for 60 s. v10 reads `editor.krunker.io/player-count` (`deob-10.0.3.js:246286-246297`); per-brain, in memory |
| `GET /mm/presence` | (Krunker Box Office) | `{"local":N,"global":N\|null,"redis":"off"\|"up"\|"down"}` signed-in social sockets on this brain / on every brain (3.11; `null` without Redis) |
| `POST /mm/admin/drain?node=ID` | (Krunker Box Office) | Bearer `brain.internal_secret`; see 6.4 |

seek-game details:

* `clientId` is the **join ticket** (see 5.3): the game server verifies it on
  connect. `host`/`port` are the game server's native TCP endpoint; `webUrl` (if
  the node advertises one) is what web clients connect to.
* `changeReason` = `"game_changed"` when `game` was requested but another lobby was
  picked (`autoChangeGame`), else `null`. With `autoChangeGame=false` and a full or
  unknown requested game the error is `GameFull` / `InvalidGameId`.
* `players` / `maxPlayers` = the lobby's seats when the answer was made (the count the
  pick used). A client never joins a lobby whose `players` has reached `maxPlayers`: it
  seeks another one (`excludeGame`), or shows `GameFull` (client/src/ui/seek_choice.c).
* `token` (optional) = a svc-accounts session token; when valid the ticket carries
  the account id + name so the game server can attach the account (its progress is
  saved); without it the player is a guest (unsaved). Every lobby takes both, as in
  Krunker. Only a legacy `onlineMode` node (a server_bin that still refused guests,
  6.2) is for signed-in players only; a guest asking for one by id with
  `autoChangeGame=false` gets 403 `AccountRequired`.
* `FORCE_ENTER=1` cookie (or `&force=1`) bypasses the capacity check on the requested
  game (v3.1.1 `hasForceEnterCookie`).
* `validationToken` (mm-token, [v10/matchmaker-token.md](v10/matchmaker-token.md)): Krunker's
  `/generate-token` -> `?validationToken=` step. Checked and spent before a seat is reserved;
  refusals are 403 `InvalidClientValidationToken` (bad, spent, or missing while
  `require_validation_token`), `ExpiredClientValidationToken`, `InvalidClientValidationTokenData`,
  `InvalidClientIP`. Without one (older clients) the seek is served unless required.
* `dataQuery` `dsmo` / `dsma` (v10's saved `seek_modes` / `seek_maps`): quick join only takes
  lobbies with `meta.mode` / `meta.mapIndex` in the lists; lobbies with room that match neither
  answer 503 `NoMatchingServers`. A requested `game` is never filtered.

The `/api/maps`, `/api/mods`, `/api/assets`, `/api/search`, `/api/streams`,
`/api/prices` hub routes belong to svc-accounts / maps (section 7).

### 4.1 v3.1.1 parity and independence

`node tools/kref/mm-parity.js --build <dir> [--db] [--chromium]` runs the reference
matchmaker (jail `npm run matchmaker`, behind an emulation of its nginx vhost +
`cors.conf`) and kn_brain side by side. It feeds both the same lobbies from fake game
nodes over `/mm/internal/ws` and diffs status, headers and JSON bodies (key order
included) for every request the v3.1.1 client sends (ClientManager.js, index.js,
social.js). `--db` adds a second brain that only sees the lobbies through PostgreSQL.
`--chromium` drives the real v3.1.1 page (headless) against each brain and diffs its
matchmaker traffic and the resulting screens. Identical: `/ping`, `ping-list`,
`game-list`, `game-info` (every case), `seek-game` for found lobbies (the
`changeReason, gameId, lobbyId, host, port, clientId` keys come first, in order),
FORCE_ENTER, HEAD/POST/OPTIONS on the list routes, player counts, the 404
`not_found` body. Documented deviations:

* Errors use the client's names (`NoAvailableServers`, not `no_lobbies`; the client
  shows `matchmaker.none` for them, and the reference name falls to "Unhandled").
* `seek-game` keeps quick join in the requested region (falling back across regions
  per `cross_region`). A requested region no node serves (a Krunker id krunk.io has no edge
  in) is first routed to the nearest hosted region, whatever `cross_region` says: by the
  client's `?pings=` ({v10 id: ms}), else by great circle (doc/v10/regions.md "Routing"); the
  answer adds `requestedRegion`. The reference ignores `region`.
* `autoChangeGame=false` with a full or unknown `game` returns 409 `GameFull` /
  404 `InvalidGameId` (the client's `matchmaker.full2` / `.invalid`). The reference
  overflows into another lobby. With nothing that has room, kn_brain returns 503
  where the reference sends the player into the least populated (full) lobby.
* An `OPTIONS` preflight on `seek-game` / `host-game` answers 204 and issues no
  ticket.
* `seek-game` appends `region` and `webUrl` after the v3.1.1 keys. It also carries `map`: the
  lobby's map name as its game-list row names it (the loading screen's "Loading <map> ..." of a
  Quick Play join, which holds no game list). Absent when the lobby names no map.
* `/mm/ranked/*` exists (v10's ranked queue over HTTP instead of its matchmaker WebSocket; the
  old `/mm/queue/*` answers ClientOutdated, see 6.5), as do `/mm/status`, `/mm/regions`,
  `/mm/brains`, `/mm/host-game` and `/mm/admin/drain`.

Matchmaking is HTTP to the brain only. No game server answers it:

* `server_bin` serves no HTTP. With a brain (`gameserver.brain_url`) it also drops
  the hub's `NET_PACKET_SOCIAL_REQUEST` frames, which are the brain's social socket's
  job (v3.1.1 `MM_NODE_MODE=node`). The standalone server and the client's offline
  game (`local_game.c`) still answer them as the guest hub.
* `net-bridge --web` reverse-proxies `/mm/*`, `/api/*`, `/social/*`, `/ai/*`, `/auth/*`, `/streams/*`,
  `/creators/*` and `/user-assets/*` to the brain
  (`brainProxy.ts`). When no brain answers, its fallback returns 404
  `{"error":"mm_routes_disabled_on_node"}` for `/mm/*` and the player counts (never
  `/mm/status`). Only `--browser-to-native`, where the stock v3.1.1 JS client plays
  through the bridge, keeps the single-lobby matchmaker shim.
* The client never asks the game socket for lobbies. With no brain (`--offline`,
  `--connect host:port`, or a default URL whose `/mm/status` probe fails) it plays
  the local game or the direct server and shows the guest Servers capture.

## 5. Brain: internal WebSocket (game servers <-> brain)

v3.1.1 `matchmaker/internalWs.ts` + `server/brainClient.ts` + `shared/wire.ts`:
**JSON text frames**, one object per frame with a `type` discriminator. Unknown
types and unknown keys are ignored (forward compatible).

### 5.1 Connection

* URL: `ws://<brain>/mm/internal/ws` (alias `/internal/ws`, what nginx exposes as
  `wss://matchmaker.<domain>/internal/ws`).
* Header `Authorization: Bearer <secret>` (`gameserver.brain_secret` ==
  `brain.internal_secret`); wrong/missing -> HTTP 401, no upgrade.
* After the 101 the node sends `hello`; the brain answers `welcome`. A second
  connection with the same `nodeId` replaces the first (`kick` reason
  `"replaced"`, close 4003). Brain pings every `ping_interval_s`; a node that sends
  nothing (pong/snapshot/...) for `node_ttl_s` is closed 4001 `"stale"` and its
  rows removed. Reconnect backoff 1 s .. 30 s (brainClient.ts).

### 5.2 Frames

Node -> brain:

| type | fields | notes |
| --- | --- | --- |
| `hello` | `nodeId` (no `:`), `region`, `name`, `publicHost`, `publicPort`, `webUrl`?, `version` (game version, `"1.0.0"`), `build`?, `commit`?, `commitTime`?, `buildTime`?, `protocol`? (build identity, 8.12), `maxPlayers`, `custom` (bool: can host custom games), `maxLobbies`? (default 1), `onlineMode`? (0/1, default 0; legacy: 1 = the node refuses guests, only signed-in players are matched to it. Current server_bins admit guests and never send it), `password`? (bool: never quick-joined), `modes`?, `maps`? | first frame |
| `snapshot` | `lobbies: [tuple...]` | full lobby set, every 5 s (replaces the node's set) |
| `lobby-up` | `lobby: tuple` | upsert one lobby (joins/leaves, 250 ms debounce) |
| `lobby-down` | `lobbyId` | lobby removed (local or namespaced id) |
| `pong` | `ts` | answer to `ping` |
| `drain` | `reason`? | node is shutting down: its lobbies stop being offered (seek/host), still listed with `meta.draining` |
| `player-join` / `player-leave` | `lobbyId`, `clientId`? (ticket `jti`), `accountId`? | optional; join consumes the ticket's slot reservation |
| `auth` | `rid`, `ticket` or `token`, `resume`? | resolve a player: ticket = seek-game `clientId`; token = session token. `resume: true`: the node admitted this ticket before and a reconnect of that session sent it again after `exp` (8.3): signature checked, expiry not. -> `auth-reply` |
| `commit` | `rid`, `lobbyId`, `matchId`, `final`, `map`, `mode`, `players: [{accountId, name, kills, deaths, score, won, timePlayed, games, kr, xp, classIndex}]` | match result; the brain forwards each `accountId > 0` to svc-accounts op `stats` -> `commit-reply` |
| `acct` | `rid`, `op`, `args` | generic pass-through to svc-accounts (`accounts_internal_call`) -> `acct-r` |
| `host-request` | `rid`, `region`?, `config`, `hostAccountId`? | a player asked this server (game socket `custom`) to host a game; the brain finds a server with capacity -> `host-request-reply` |
| `host-reply` | `rid`, `ok`, `lobbyId`?, `err`? | answer to the brain's `host` |
| `ranked-result` | `rid`, `matchId`, `lobbyId`, `winner` (0, 1, -1 draw), `players`, `abandoned` | ranked match end (6.5) -> `ranked-result-reply` |
| `bot-banter` | `rid`, `name`, `personality`, `event` (win / lose / draw / kill / death), `other`?, `mode`?, `map`?, `kills`?, `deaths`?, `place`?, `players`? | bot chat (shared/src/bot_chat.c): one short in-character chat line for a bot player, written by the AI proxy (services/brain/ai.c) -> `bot-banter-reply` |
| `turf-kills` | `rid`, `matchId`, `mapId`, `mode`, `records` [{clan_name, player_id, map_id, kills, deaths}] | v10 Turf Wars: an eligible round's counted clan kills (6.5) -> `turf-kills-reply` {ok, recorded} |

Brain -> node:

| type | fields | notes |
| --- | --- | --- |
| `welcome` | `assignedRegion`, `brainId`, `brainVersion`, `now` (ms), `pingIntervalMs`, `ttlMs` | |
| `ping` | `ts` | answer `pong` |
| `kick` | `reason` | followed by close; on `"replaced"` do not reconnect-loop aggressively (normal backoff) |
| `assign` | `ticket`, `jti`, `lobbyId` (local id), `account` (`{id,name}` or null), `expiresIn` (s) | best effort heads-up for a ticket this brain just issued (tickets issued by another brain arrive without it: the ticket itself is authoritative) |
| `auth-reply` | `rid`, `ok`, `account`?: `{id, username, alias, clan, level, premium, dev, mod, hack, funds}`, `ticket`?: decoded claims, `err`? | |
| `commit-reply` | `rid`, `ok`, `results: [{accountId, ok, funds?, level?, err?}]` | |
| `acct-r` | `rid`, `err` (null or message), `data` | svc-accounts reply |
| `host` | `rid`, `config` (v3.1.1 custom config: `maps`, `modes`, `classes`, `passCode`, `private`, server-config `varN`s, `weps`...), `hostAccountId`?, `hostName`? | turn an idle lobby into / spawn a custom lobby; reply `host-reply` with its local `lobbyId` within `host_timeout_s`, then send its `lobby-up` (`meta.cs = true`) |
| `host-request-reply` | `rid`, `ok`, `gameId`, `host`, `port`, `webUrl`?, `clientId` (ticket for the host player), `err`? | send the host player `cust` ("Game Created", gameId) as v3.1.1 does |
| `drain` | `reason`, `deadlineMs`? | finish the round, accept no new players, then `lobby-down` + exit/idle |
| `ranked-result-reply` | `rid`, `ok`, `ratings: [{accountId, name, team, key, old, new, delta, abandoned?}]`, `err`? | |
| `bot-banter-reply` | `rid`, `ok`, `text`? (one line, <= 70 chars), `err`? ("AI unavailable", "rate limited", the upstream's failure) | the game server filters the text and falls back to a template on `ok: false` or no reply |
| `unbox` | `accountId`, `item`, `kr`, `rarity` | a store or Free KR spin's win of an account playing on this node (`game_presence`): v10.0.3 `unb` to everyone for rarity >= 2, else to the unboxer (services/brain/node_events.c, server/src/brain_events.c) |
| `raffle-win` | `accountId`, `lotteryId`, `drawId`, `prize: {type, ref, quantity}` | a lottery prize of an account playing on this node: v10.0.3 `lotWin` (the winner's uid) to everyone there |

### 5.3 Join tickets

The seek-game `clientId` is a compact JWS (`kn_signed_token`, HS256, key
`brain.join_secret`), verifiable locally by the game server without a round trip:

```
{"brn":"<brainId>","nid":"NY","lid":"ab12c","acc":12,"name":"bob",
 "jti":"<16 random bytes, hex>","iat":...,"exp":iat+join_ttl_s}
```

`acc` is 0 and `name` absent for guests. The ticket travels in the 352-byte
`NetJoinPacket.client_key`: only claims something reads are in it, and `name` is left
out when the ticket would still be longer than 351 chars (the game server's `auth`
with the ticket returns the account name anyway). A game server should accept a ticket when
the signature is valid, `exp` is in the future, `nid` is its node id and `lid` one of
its lobbies (or when `gameserver.require_ticket` is off, accept tickets-less joins
as today). Each issued ticket reserves a slot for `join_ttl_s` (reservations count
towards fill in quick join so parallel seeks do not overfill nor scatter). The seat counts
until the node's own count includes the player: an `auth` with the ticket shortens it to 10 s
(a refused player frees it), `player-join` keeps it for at most 5 s more, and the node's next
`lobby-up`/`snapshot` of that lobby (sent after the `player-join`, so it counts the player)
settles it; its `mm_reservations` row is deleted once that count is persisted. Releasing it at
`auth`/`player-join` left the lobby looking empty for the node's 250 ms lobby-up debounce, so
seekers in that window were sent to empty lobbies. Session-token auth (`auth` with `token`)
is the path for players that connect without the brain (direct connect).

A client sends its ticket in every JOIN of the session, also when its transport reconnects
mid-match (a WebRTC gateway or UDP session dropping, the WebRTC -> WebSocket fallback, a
net-bridge / gateway restart). The game server accepts a ticket it admitted before again after
`exp` while its lobby lives (8.3 "resume"); a restarted server has a new lobby id, so its players
seek a fresh ticket (9.4).

## 6. Brain: registry, federation and matchmaking

Code: `services/brain/registry.c` (pure in-memory view + pick rules, unit tested),
`federation.c` (PostgreSQL), `internal_ws.c`, `host.c`, `public_routes.c`,
`brain_util.c` (settings, tickets). Tests: `ctest -L brain` (`brain_registry`;
`brain_federation` = `services/brain/tests/federation_test.sh`: 2 brains, 4 fake
nodes in 2 regions, hosting, reservations, kill -9 / SIGSTOP / brain death
failover, no statement errors in the logs; `brain_ranked`). The DB tests run in a
per-run schema (`kn_test_mm_<pid>` / `kn_test_rq_<pid>`, created and dropped by
`tests/pg_schema.py`) of the configured database (or `$KN_MM_TEST_DATABASE`) on
ephemeral ports (`brain.port=0`, port read from the "listening on" log line), so
concurrent runs never interfere. `kn_fakenode`
(`services/brain/tools/`) is a scriptable fake game server.

### 6.1 Tables (`services/db/migrations/<dialect>/0200_matchmaker.sql`)

* `mm_brains(brain_id PK, region, public_url, version, started_at, heartbeat_at)`
* `mm_nodes(node_id PK, brain_id, region, name, public_host, public_port, web_url,
  version, max_players, max_lobbies, custom, draining, hello jsonb, connected_at, heartbeat_at)`
* `mm_lobbies(game_id PK, node_id -> mm_nodes ON DELETE CASCADE, local_id, region,
  players, max_players, custom, protected, mode, map, version, tuple jsonb, updated_at)`
* `mm_reservations(jti PK, game_id, brain_id, account_id, expires_at)`
* `mm_host_requests(id PK, from_brain, target_brain, target_node, region, config jsonb,
  host_account, host_name, status ('pending','sent','done','failed'), result jsonb, ...)`
* view `mm_live_lobbies` (operators: lobbies of nodes heartbeating in the last 30 s)

Ownership and liveness:

* A row is live while `heartbeat_at > now() - brain.node_ttl_s`; lobbies inherit
  their node's heartbeat. Readers ignore older rows; every brain's reaper deletes
  rows older than 3 x TTL (idempotent, no leader).
* The owning brain writes a node and its lobby set in one statement on `hello`
  (takeover: `INSERT .. ON CONFLICT DO UPDATE SET brain_id = me`) and on every
  change (100 ms debounce; `UPDATE .. WHERE brain_id = me`, which never resurrects a
  deleted node nor steals one another brain took over: 0 rows -> the brain drops its
  link with `kick "replaced"`). One `UPDATE` per `heartbeat_s` bumps all its nodes.
* Node disconnect (socket closed, kill -9) -> its rows are deleted at once. A hung
  node (no frame for `node_ttl_s`) is aborted by its brain's reaper -> deleted. A dead
  brain stops heartbeating -> its nodes drop out of every other brain's view after
  `node_ttl_s`; its game servers reconnect (1-30 s backoff) to the restarted brain or
  to another brain of the region and are taken over.
* Graceful brain shutdown (SIGTERM) closes the game server links with 1001 but keeps
  the rows: the nodes reconnect to the restarted brain within the TTL, so the
  lobbies never disappear from the game list during a restart.

Global view: each brain keeps its own nodes in memory (authoritative) and reloads
the other brains' live nodes, lobbies, reservation counts and brains every second
and on `NOTIFY kn_mm_lobby` (sent after each write; payload = writer brain id).

Notifications: `kn_mm_lobby`; `kn_mm_host` (`<request id>:<target brain>`) and
`kn_mm_host_done` (`<request id>:<requesting brain>`) for cross-brain hosting;
`kn_mm_assign` (`{"brain","node","frame"}`) relays `assign` heads-ups to the brain
owning the node. Host requests are also polled every 500 ms (lost-NOTIFY fallback).

### 6.2 Quick join (v3.1.1 `pickBestLobby` + region)

Candidates: live, non-draining lobbies, version-matching when
`require_version` and `dataQuery.v` are set. Taken seats = reported players +
live reservations of every brain; cap = min(maxPlayers, mode cap)
(`brain.matchmaking.mode_caps`, by `modeName` case-insensitively or mode index).

1. `game=ID` requested: if it exists and has room (or `FORCE_ENTER`/`force=1`) -> it.
   Otherwise `autoChangeGame=false` -> `GameFull` / `InvalidGameId`; with
   `autoChangeGame` (default) continue and answer `changeReason: "game_changed"`.
   Krunker Box Office: a guest (no valid `token`) asking for a lobby of a legacy
   `onlineMode` node is treated like a full one, but with `AccountRequired` (403).
2. Public pool = non-custom lobbies (custom lobbies are only reachable by id),
   minus password protected ones (hello `password` or meta `pr`) and, for guests,
   lobbies of legacy `onlineMode` nodes (hello `onlineMode`, persisted for other brains
   as meta `onlineMode: true` in the lobby tuple): only server_bin builds that refused
   guests send it ("Failed to verify username"). Current ones admit guests and accounts
   alike, like every Krunker lobby, so for them guests and accounts share one pool.
   Also skipped: v7.1.4 `meta.sk === false` (AUTO_SEEK off: by id only).
   In the requested region, the best lobby with room (first pass below `soft_cap`, if set):
   up-to-date before outdated, then the most taken seats (players + live reservations, so
   populated lobbies fill before an empty one is used: v3.1.1 `pickBestLobby`, v7.1.4
   `ServerManager.seek`), then the most round time left (`meta.timeLeft`, the end screen
   last), then the least loaded node. Neither reference has a round-end, party or
   max-player-buffer rule (the buffer is `soft_cap`).
3. No room in the region and `cross_region`: the same rule over every region.
4. Nothing with room -> `NoAvailableServers` (`NoServersMatchQuery` when lobbies
   exist but none matches the version). v3.1.1's "least populated" overflow would
   send the player into a full lobby the game server refuses, so it is reported.

Each answer issues a ticket and reserves its seat for `join_ttl_s` (in memory and
in `mm_reservations`, with a `kn_mm_lobby` NOTIFY so the other brains count it at once); it
counts until the node reports the player (5.3). Tests: `brain_registry`,
`brain_mm_fill_e2e` (`server/tests/mm_fill_e2e.py`: 3 `server_bin` nodes, sequential seekers
inside the lobby-up debounce and concurrent seekers all land in one lobby).

#### Flavor pools (Krunker Strike)

One coordinator serves both apps: Krunker Box Office and the Krunker Strike app sign in to the same
accounts and seek through the same brain, but never share a lobby. A Strike `server_bin`
(`--flavor strike`, [v10/krunker-strike.md](v10/krunker-strike.md)) says `hello.flavor = "strike"`
and `meta.flavor = "strike"` in its lobby tuples (the brain also stamps the node's flavor into the
tuple, so federation's `mm_lobbies` rows carry it); base servers say nothing, so their frames and
tuples are unchanged. Every lobby route takes `?flavor=` (`registry_flavor_match`): none / `base` =
the base game only, `strike` = Strike only, `all` / `*` = both (dashboards).

| route | per pool |
| --- | --- |
| `/mm/seek-game` | quick join over the pool only; `?game=` of another pool = not found (`InvalidGameId`, or the pool's best lobby with `autoChangeGame`) |
| `/mm/game-list`, `/mm/game-info` | the pool's lobbies (the short link `CH1:abcde` resolves inside the pool) |
| `/mm/ping-list`, `/mm/regions` | regions / nodes of the pool |
| `/mm/host-game`, a node's `host-request` | a node of the pool (the requesting node's); ranked stays base |
| `/mm/ranked/queue` | Box Office only: `?flavor=strike` is refused (`ERROR FLAVOR_UNAVAILABLE`), ranked matches are hosted on base nodes |
| finder `/servers` | the same split by the KNINFO `flavor` (`finder_table.c`) |

Every client path that picks a lobby goes through these routes: Quick Match / F4 (`excludeGame`,
`excludeRecent`), the server browser's Quick Join and rows, party follow, a friend's game, invite
links, the Join window, the restart / drop rejoin and server redirects all seek by id or quick join
in the pool. The Box Office client sends no `flavor` (unchanged requests); the Strike client adds
`flavor=strike` to each of them (`servers_ui.c` `mm_flavor_param`), lists no official krunker.io
lobbies, and joins with the coordinator's signed join ticket (5.3) like any client. The standalone
`kn_strike_director` (doc/v10/strike-director.md) is the matchmaker only where no coordinator runs.
Official Strike servers: deploy/ansible/README.md "Krunker Strike servers". Tests: `brain_registry`
(flavor block), `strike_coordinator_e2e` (`server/tests/strike_coordinator_e2e.py`: a Strike and a
base `server_bin` on one brain, seeks / lists / joins per app, both clients).

### 6.3 Hosting custom games

Admin / tooling only: the clients no longer call `/mm/host-game` (hosting = running
`server_bin`, section 8.7/8.11).

`/mm/host-game`, a node's `host-request` (game-socket `custom`), or the social WS
via `brain_host_game()` (`registry.h`): pick, in the requested region (then any
region when `cross_region`), a non-draining `custom`-capable node (not password
protected; a legacy `onlineMode` node only for a signed-in host or ranked) with spare
capacity (fewer lobbies than `maxLobbies`, or a non-custom lobby with no players and
no seat reserved), preferring this brain's own nodes, then the least loaded. Own
node -> `host` frame; another brain's node -> `mm_host_requests` row +
`NOTIFY kn_mm_host`; the owning brain claims it (`status = 'sent'`), forwards `host`,
stores the node's `host-reply` and notifies `kn_mm_host_done`. The answer is a
seek-game reply (ticket for the host) for `<nodeId>:<lobbyId>`; errors
`NoAvailableServers`, `HostTimeout` (`host_timeout_s`), or the node's `err`.

### 6.4 Admin

`POST /mm/admin/drain?node=<id>[&reason=]` with `Authorization: Bearer
<brain.internal_secret>` on the brain owning the node: sends `drain`, marks the
node draining (still listed with `meta.draining`, no longer offered). Used by
`deploy/deploy.sh drain`.


### 6.5 Ranked and Turf Wars (`ranked.c`, `turf.c`, `0205_ranked_v10.sql`, `0206_turf_wars.sql`)

Krunker v10.0.2's ranked (solo queue, TrueSkill per season and region, 14 ranks, Ranked Points,
milestones, RP shop) and Turf Wars (v10's clan war: daily per-map territory control by counted
clan kills, a KR pool split 60 / 30 / 10). The design, the v10 evidence, the route table, the
deviations and the tests are in `doc/v10/ranked-impl.md`. Summary:

- Routes: `/mm/ranked/{queue,poll,leave,connection,info,profile,leaderboard,history,claim-milestone,
  shop,purchase}` and `/api/territorial-wars/*`, `/api/clans/:id/loot-config`. The v7 party queue
  (`/mm/queue/*`) answers 410 `{"status":"ClientOutdated"}`.
- State: `ranked_profiles`, `ranked_queue`, `ranked_points(_tx)`, `ranked_season_payouts`,
  `ranked_shop(_purchases)` (0205), the 0201 match tables (+ v10 columns) and `turf_kills`,
  `turf_days`, `turf_clan_days`, `turf_payouts`, `turf_loot_config` (0206). The procs are C on
  every driver (`db_ranked.c`, `db_turf.c`); the v7 tables (`ranked_ratings`, `mm_ranked_parties`)
  stay, and 0205 seeds every v7 rating into the Beta season (0) profile of region "na".
- Game server: the `host` config and the `ranked-result` frame keep their shapes (results add
  assists / damage / objective / reward / timeMs); new `turf-kills` frame (reliable) at the end of
  each eligible round.
- Config: `brain.ranked.*` (`enabled`, `min_level` 10, `team_size` 4, `season` -1 = the calendar,
  `host_regions` {"na": "<game-server region>"}, `rating_window` 150,
  `window_growth` 10, `window_max` 1000, `cooldown_s` 1800, `poll_timeout_s` 30,
  `creating_timeout_s`, `match_timeout_s`, `start_grace_s` 60) and `brain.turf.*` (`enabled`,
  `kr_per_kill` 1, `day` -1 = the clock, `settle_check_ms` 60000).

### 6.6 Community feed (`community.c`): events, polls, news

The v10 menu's Community & Events popup and the What's New window read Krunker v10.0.2's gapi
routes, in v10's shapes (client: `client/src/ui/v10_news.c`):

| Route | Answer | v10 caller |
| --- | --- | --- |
| `GET /creators/events` | `{"data":{"events":[...],"polls":[...]}}`: event `id, player_id, player_name, title, description, banner_url, start_date, duration_hours, ended_at, links [{label, url}], tags`; poll `id, title, description, ends_in` (seconds) and `url` (KN: v10 builds `community.krunker.io/polls/<id>`) | `gapiClient.creators.getUpcomingEvents()` (Community & Events) |
| `GET /social/news` | `{"data":{"news":["<html>", ...]}}` | `gapiClient.social.getNews()` (the hub's News tab; KN's What's New window shows the items too) |
| `GET /data/events` | `{"data":{"events":[...]}}`: the feed's `gameEvents` in config.events' fields (below) | v10.0.3's bundled `config.events` (KN: the event popup, Event Challenges, the EVENT prefab) |

Sources, in this order:

- The operators' feed file `brain.community.feed` (default `config/community.json`; format and an
  example in `config/community.example.json`). It is read again whenever its modification time or
  size changes, so editing it on a coordinator needs no restart (every coordinator reads its own
  copy: deploy the same file to each). Dates are UTC ISO 8601. Events whose `ended_at` has passed,
  or whose `start_date + duration_hours` has, and polls past `ends_at` or without a `url` are not
  served; events come soonest first. A news entry is plain text or `{text, url, label}`: the brain
  writes the HTML (escaped text, one `http(s)://` or site-relative link), so the file never puts
  markup into the clients.
- With a database: the published maps and mods staff featured (`map_featured` / `mod_featured`
  > 0, the hub's Featured tab), newest first, at most 12, as events tagged `Featured Map` /
  `Featured Mod` by their creator, with `Play` (`/?play=<name>`, maps) and `Details`
  (`/social.html?p=map|mod&q=<name>`) links. `start_date` = `ended_at` = the last update, so the
  card shows that date without v10's LIVE badge. Only `http(s)` images become `banner_url`.

The same file's `gameEvents` are the game's live events, Krunker v10.0.3's `config.events` and `ongoingEvents` in
one list: `id, name, startsAt, endsAt, reveal` (ms), `imgExt, longDesc` (text, or a list of lines), `noChallenges,
rewards {skins, weights}, rewardChance, eligibleMaps, limitedTimeMode`, KrunkNative's `eventObjects` (the EVENT
prefab shows), and the kill pools' `killPools [{weight, contents [{content_type, content_ref}]}], killPoolsWeight,
killPoolsMode, killPoolsIcon` (doc/v10/events.md, doc/v10/kill-pools.md). The brain copies only well-typed values
(integer skins / maps, a bare `imgExt`). The ones running now (`startsAt` <= now < `endsAt`; no `endsAt`: open
ended) reach every game server in the internal WS `welcome` (`gameEvents`) and in a `game-events` `{events}` push
whenever the set changes (checked at each ping): a public round plays the first as its `eventIndex`, its
`limitedTimeMode` puts the event's mode first in the vote list, its reward skins and kill pools pay at the end
(`itemRewards` with the stats). `GET /data/events` serves the clients `{"data":{"events":[...]}}`: the same events
without the kill pools, plus the ones that reveal early (`startsAt - now <= reveal`): the event popup and the
Challenges window's Event Challenges. Test: `services/brain/tests/live_events_test.py` (no database).

No file and no featured creation: empty lists (the popup's "No upcoming events"). Streams have no
source yet (`/api/streams` is v3.1.1's empty placeholder): put them in the feed as events
(`tags: ["Stream"]`, a Twitch link). Test: `server/tests/v10_news_e2e.py`.

### 6.7 User-asset mirror (`user_assets.c`): community maps' Krunker user assets

`GET /api/user-assets/<id>/<file>` (alias `/user-assets/<id>/<file>`) mirrors Krunker's user-asset host
(`https://user-assets.krunker.io/<id>/<file>`) for the clients: community maps name textures, models,
sounds and resource packs there. Native clients ask Krunker's host first and come here when it refuses
them (Cloudflare answers 403 to some networks); the web client always comes here (the host sends no
CORS headers). Paths and content rules: `shared/include/user_asset_path.h` (an asset id, `md<id>` or
`m<id>`, Krunker's file names; size caps; never an HTML page). Answers from
`<accounts.assets_dir>/user-assets/`, then `user_assets.cache_dir`, else the upstream (written into the
cache); an upstream 404 is remembered `user_assets.fail_cache_s`. Config keys: `config/config.example.json`
`"user_assets"`.

`<id>/video.mp4.knv` is not on Krunker's host: it is the asset's `video.mp4` (fetched as above) as
KrunkNative's frame sidecar (`shared/include/knv.h`), made with `user_assets.ffmpeg` (role `kn_brain`
installs ffmpeg) and cached like a fetched file; the clients play it on the map object
(`client/src/map_video.c`). No ffmpeg or no video: 404.

Proxy: the upstream fetch goes through, in order, `KN_USER_ASSETS_PROXY`, `user_assets.proxy`,
`KN_FETCH_PROXY` (curl syntax; none = direct). Krunker's CDN refuses kru's network as it refuses this
dev machine's: on kru the deploy (role `kn_brain`, `tasks/user_assets_proxy.yml`) reads `PROXY` from the
krunkbox service's `/home/kru/krunkbox/.env` (`kn_brain_user_assets_proxy_env`) and writes it as
`KN_USER_ASSETS_PROXY` into `/etc/krunknative/user-assets.env` (0640 root:kn, an optional
`EnvironmentFile` of the kn-brain units), with `no_log` on every task that sees the value; only that
one key is copied (the rest of the .env is not the brain's). No file or no key: nothing changes, the
mirror fetches directly. Locally `KN_FETCH_PROXY=http://127.0.0.1:8888` (the dev machine's proxy on the
same network) makes kn_brain and the native client (krunker.io hosts only, `client/src/net/http_proxy.c`)
fetch through it; players' clients have no such variable and go direct, then to the mirror.

Tests: `services/brain/tests/user_assets_test.py` (ctest `brain_user_assets_e2e`: stub host, stub forward
proxy for both variables), `deploy/ansible/tests/user_assets_proxy_test.py` (`make deploy-test`: the role's
tasks on fake .env files, the value never in ansible's output).

## 7. Social WebSocket, accounts, profiles (svc-accounts)

Owner: svc-accounts. Code: `services/brain/accounts.c` (accounts, sessions, internal
ops, password hashing worker), `social_ws.c` (socket, sessions, dispatch, presence),
`profiles.c` (pages, profile, search, follows, reports), `leaderboards.c`,
`social_data.c` (feed, chat, gifts, clans, settings, mail, market, trades),
`creations.c` (hub HTTP catalogue, map publishing), `store.c` (store skins, prize
wheels and their item pools — `shared/include/store_spins.h`, served on
`GET /api/data/spins` like gapi, rotated by the `store_spin_data` row — Free KR
wheel, daily reward). Private header `acct_internal.h`; public
`accounts.h` (internal ops, used by `internal_ws.c` / `public_routes.c`) and
`social_ws.h`. Schema: `services/db/migrations/postgres/0100_accounts.sql`,
`0101_social.sql`, `0102_items_creations.sql`, `0103_account_functions.sql`,
`0104_social_functions.sql`, `0105_twofa.sql`, `0106_store.sql`, `0118_store_v10.sql`, `0109_account_security.sql`, `0122_email_login.sql` (applied migrations are never edited:
changes go into a new 01xx file in every dialect directory; `sqlite/` and `mysql/`
hold the same tables, the SQL functions are C kn_procs in `db_accounts.c` /
`db_social.c`, 3.6). Port of v3.1.1 `matchmaker/socialWs.ts`,
`server/accounts.ts`, `server/social.ts`, `server/schema.ts`; message labels and
shapes are what the real client (`src/social.js`, `src/index.js`) sends and reads.

### 7.1 Configuration (`"accounts"`)

| key | default | meaning |
| --- | --- | --- |
| `name_min` / `name_max` | `3` / `14` | a new username's length (v10.0.3 `maxNameLength` 14, deob-10.0.3.js:22725); letters, digits, `_` (v3.1.1 `NAME_RE`). Only registration checks it: a stored name of up to 32 characters (KrunkNative's limit was 16) still signs in, resumes and mirrors |
| `password_min` / `password_max` | `6` / `100` | password length (max bounds PBKDF2 work) |
| `password_iterations` | `600000` | PBKDF2-HMAC-SHA256 iterations (`kn_password_*`); older hashes are re-hashed on login |
| `alias_min` / `alias_max` | `3` / `14` | a new display name ("nme"): letters, digits, space, `_`, `-`; a set name is kept |
| `session_ttl_days` | `30` | sliding lifetime of session tokens |
| `max_sessions` | `20` | sessions kept per account (oldest dropped) |
| `starting_kr` | `0` | KR granted on register (ledger row `starting`) |
| `free_premium` | `true` | everyone is premium (v3.1.1 offline grants it: display names) |
| `developers` / `moderators` | `[]` | usernames flagged `player_developer` / `player_moderator` at startup (staff pages, flags, verify) |
| `login_throttle_s` | `5` | after a failed login, the same (name, IP) is refused with "Please Try Later" |
| `ip_block_failures` / `ip_block_days` | `100` / `5` | an IP with more failures within the window gets "Too many failed logins for this IP" |
| `register_per_ip_hour` | `0` | accounts per IP per hour (0 = unlimited) |
| `max_match_kr` | `500` | cap of one stats commit's / reward's KR (bounds a compromised game server) |
| `region_index` | `{}` | game region id -> `config.socialRegions` index (leaderboard region filter) |
| `rank_var` | `0.03` | `config.rankVar`: level = max(1, floor(rank_var * sqrt(score))) |
| `trade_min_level` / `trade_max_items` / `trade_max_items_premium` | `20` / `5` / `20` | v7.1.4 `config.tradeMinLvl` (both players) / `tradeMaxOut` / `tradeMaxOutP` (items per side) |
| `trade_max_open` / `trade_kr_max` / `trade_ttl_h` | `50` / `1e7` / `0` | open offers per sender, KR per side of an offer (0 = KR not tradable), offer lifetime in hours (0 = never, like v7.1.4) |
| `gift_min_level` / `gift_fee_percent` / `gift_min` / `gift_max` | `30` / `10` / `10` / `1e7` | KR gift rules (v7.1.4 `giftMinLVl`, `giftFee` 10/100 rounded up, `giftMin`, `giftMax`; the sender pays amount + fee) |
| `market_fee_percent` / `market_fee_bypass` / `market_min_level` | `10` / `0` / `15` | listing fee, floor(price * fee %) (v7.1.4 `marketFee` 10/100, `calcFee`), fee-free prices up to (`marketFeeBypass`), level to list or buy (`marketMinLVl`) |
| `sale_max` / `sale_grace_s` / `max_listings` | `1e7` / `300` / `40` | price cap (`saleMax`), a listing is buyable this long after posting (`saleGrace`), open listings ("MAX") |
| `economy_rate_<kind>` | gift `10`, list `30`, unlist `60`, buy `30`, qsell `60`, trade `10`, accept `30`, close `60` | economy actions per account and minute (`Slow down`); 0 = unlimited. Rules and integrity: [doc/v7/economy.md](v7/economy.md) |
| `economy_tick_ms` | `60000` | trade offer expiry + idempotency key cleanup timer |
| `clan_tag_max` / `clan_max_members` | `4` / `0` | clan tag length (create input maxlength) / members (0 = unlimited) |
| `post_max_len` / `comment_max_len` / `chat_max_len` | `1024` / `500` / `300` | text limits |
| `page_size` / `leaderboard_cache_s` | `100` / `30` | leaderboard rows / per (query, region) cache |
| `skins_file` / `assets_dir` | `assets/skins.json` / `assets` | store skins in `store.skins` index order (wheels, quick sell, market filters) / map catalogue JSON. The table is Krunker v10.0.2's (8069 items; 0-6447 keep v7.1.4's meaning, so inventory `skin_id`s stay valid): `make store-import` |
| `store_season` | 10 | `config.currentSeason` of the store table / prize wheels (`store.c`): a wheel without a `season` rolls its items. Kept apart from `current_season` (bumping the season does not move the wheels) |
| `store_reload_ms` | `60000` | how often `store_spin_data` (0118_store_v10: the gapi `/data/spins` JSON of a wheel rotation) is re-read; with no row the compiled-in v10 set is served |
| `current_season` | `10` | `config.currentSeason`: the Battle Pass, season stats / leaderboards and season badges of this season; raising it rolls the season over at the next start (7.8) |
| `reward_time_s` / `reward_min_level` / `daily_reward_kr` | `43200` / `5` / `50` | `config.rewardTime` between Free KR spins / daily rewards (shared `lastReward`), `config.rewardMinLvl`, KR of `clm` |

### 7.2 Schema (PostgreSQL names; same tables on sqlite / mysql)

* `accounts` (credentials, unique `lower(username)`, PHC password hash, email,
  birthday, referral, created/last-login IP, `twofa_secret`, synced `settings`);
  `players` (1:1, v3.1.1 `qs_player` column names: `player_kills`, `player_deaths`,
  `player_wins`, `player_games_played`, `player_score` (XP), `player_timeplayed` (ms),
  `player_funds` (KR, >= 0), `player_stats` jsonb (`c<class>` XP per class, `hs`,
  `wb`, `mk`, `fk`, `n`, `tmk`, `s`, `h`, ...), elo/elo2/elo4, challenge, region,
  premium, staff flags, follower counters kept by a trigger, partner/twitch).
* `sessions` (sha256 of the token, sliding expiry, IP, origin), `login_failures`,
  `login_ip_blocks`, `login_audit`, `bans` (account and/or IP, reason, expiry,
  revocation), `name_changes`, `match_results` (one row per (match, account): the
  idempotency key of stats commits and the match history), `kr_transactions` (KR
  ledger: rewards, gifts, market, clan funds, quick sell, mail).
* `follows`, `clans` (`clan_name` = tag, leader, score, funds, rank), `clan_members`
  (one clan per player, role 0 Recruit .. 3 Commander, clan XP/kills/deaths),
  `clan_requests`, `clan_messages`, `chat_messages`, `feed_posts` / `feed_comments`
  / `feed_likes`, `mail`, `reports`, `moderation_log`.
* `player_titles` (the v10.0.3 titles an account owns, one row per (account, title) with its `source`
  and store item `ref`; granted by Black Market items with a `title`): `0361_player_titles.sql`.
* `player_badges` (earned badges.js indices), `challenges` (v7 challenge definitions: type,
  DAT / REW JSON, icon, name; seeded with a starting set), `player_challenges` (PV / PT per
  account, challenge and period: UTC day / week for daily / weekly), `player_medals` (medal
  counts per account): `0107_progression.sql`, player-viewer WP-E part 2c.
* `seasons` (one row per season, `ends_at` set by the rollover), `battlepasses` (v7's
  qs_battlepass: one pass per season, `itemdata` "id,tier,col,sk", KR prices),
  `battlepass_progress` (account.BP per pass: `bpp`, `tier`, `items_claimed`, `tier_at`),
  `season_stats` (per-season totals of counted matches): `0108_seasons.sql`, 7.8.
* `accounts.email_verified_at`, `twofa_pending`, `twofa_last_step`, `twofa_failures`,
  `twofa_failed_at`; `email_tokens` (sha256 of each emailed token, address, expiry, use) and
  `mail_log` (every mail: account, kind, address, IP, status, Message-ID, the server's error):
  `0109_account_security.sql`, 7.9.
* `inventory` (skin index -> count), `skin_transactions`, `market_listings`,
  `trades` + `trade_items`, `published_maps`, `published_mods`, `published_assets`,
  `creation_votes`, `creation_favs`.
* View `player_rows` = the qs_player-shaped row the social page reads (profile,
  search, leaderboards: `player_name`, `player_stats` as a JSON *string*,
  `player_datenew` ISO date, `player_clan`, `clan_rank`, ...). SQL functions do every
  multi-statement operation atomically and raise SQLSTATE `P0001` with the message
  the client shows (`kn_register`, `kn_login_*`, `kn_commit_stats`, `kn_add_funds`,
  `kn_add_skin`, `kn_gift`, `kn_clan_*`, `kn_market_*`, `kn_trade_*`, `kn_feed`,
  `kn_post_*`, `kn_account_json`). User text is stored raw and HTML-escaped
  (`kn_html`) when served, because the v3.1.1 page inserts it with `innerHTML`.

### 7.3 Account object, levels, sessions

`kn_account_json(id)` (+ config decoration) is the account everywhere:

```
{id, username, alias, level, levelProg, score, funds, clan, clan_rank, clan_role,
 premium, dev, mod, hack, featured, elo, elo2, elo4, chal,
 banned (0/1), ban_reason, ban_expires (unix s, 0 = none/permanent),
 badges: [earned badges.js indices], data: [40 slots], token?}
```

`data` is `Account.getData()` of `src/data/account.js` in its order: 0 id, 1 kills,
2 wins, 3 games, 4 deaths, 5 funds, 6 score, 7 clan, 8 timePlayed, 9 featured,
10 skins `[{ind,cnt}]`, 11 lastReward, 12 isDev, 13 following, 14 followers,
15 followInd, 16 stats (object), 17 hack, 18 partnerName, 19 partnerApp,
20 regionInd, 21 isMod, 22 elo, 23 elo2, 24 elo4, 25 chal, 26 virus, 27 premiumT,
28 has2FA, 29 alias, 30 createDate, 31 twitch, 32 isCreationMod, 33 clanDeaths,
34 clanKills, 35 clanCon, 36 clanRegion, 37 clanRank, 38 jobRating, 39 jobRatingPos.
Level: `max(1, floor(0.03*sqrt(score)))`; `levelProg` is v7.1.4's getLevelProg, a float percent
linear in score between `(level/0.03)^2` (0 at level 1) and `((level+1)/0.03)^2` (XP = score).

Session token: 32 lowercase hex (16 random bytes, as v3.1.1), stored as sha256,
sliding `session_ttl_days`. Issued by password login and register; a token resume
keeps the client's token. Logout (`["a",-1]`) and password changes revoke tokens.
Game servers verify tokens with the internal op `token` / `session` (7.6); the
matchmaker's seek-game tickets carry the account id after the same lookup.

### 7.4 Social socket

Endpoints: `GET /api/ws` (v3.1.1 `SOCIAL_WS_PATH`; the v3.1.1 offline `hook.js` sets
`socialURL = origin + "/api"`) and `GET /social/ws` (`config.socialURL + "/ws"` with
socialURL `<brain>/social`). Frames are arrays `["label", ...args]`:

* binary: msgpack + 2 trailing bytes (the client's anti-cheat nibbles; ignored on
  receive, `0,0` on send), exactly `server/io.ts` encodePacket/decodePacket;
* text: JSON (Krunker Box Office clients). Replies use the encoding of the client's most
  recent frame; `?enc=json` makes the greeting JSON too.

On open the brain sends `["pi", null]` (client answers `["po"]`) and
`["io-init", socketId]` (16 hex). An unknown label is answered `["po", null, "Unknown request."]`
(v3.1.1 socialWs.ts dropped it; doc/social-rate-limit.md). Each
socket's frames run strictly in order: a frame waits until the database work of the
previous one finished (v3.1.1 ran them on synchronous SQLite), so e.g. `["f", ...]`
then `["r", "profile", ...]` sees the follow. Errors
without their own reply label are `["po", null, "message"]` (the page's `io_po`
error popup). Rate limit: one token bucket per message class and connection (doc/social-rate-limit.md);
a refused frame is answered (gapi 429, eb `EB_RATE_LIMITED`, `po`), never dropped. Pushes that only
the Krunker Box Office menu understands (`gmc`, `ua`) go to JSON clients only.

Auth (`socialWs.ts` handleAuth / packetHandlers io_a):

| request | reply |
| --- | --- |
| `["a", 0, [name, pass, ref, email?], captcha]` register (email: Krunker Box Office, one account per address; the v10 modal always sends it, `accounts.email.required` makes the brain insist; 7.9) | `["a", null, id, username, data40, token, info]` or `["a", "error", null, null, null, null]` |
| `["a", 1, [name, pass, null], captcha]` login (name: a username or an email, trimmed, any case: the account holding it, 0122_email_login) | same |
| `["a", 1, [name, null, token]]` token resume | same (token unchanged) |
| `["a", -1]` logout | none (session token revoked) |

`info` = `{level, levelProg, alias, premium, funds, score, clan, dev, mod, banned, badges}`
(ignored by v3.1.1). Then `["gmc", unreadMail]` (JSON clients). Error strings:
`Username must be 3–14 characters.`, `Username may only contain letters, digits, and
underscores.`, `Password must be ≥ 6 characters.`, `Password is too long.`,
`Username already taken.`, `Invalid login.`, `Invalid credentials.`, `Please Try
Later`, `Too many failed logins for this IP`, `Banned: <reason>` / `Account banned.`,
`Too many accounts created from this IP. Please Try Later`, `Invalid Email`, `Email Required`, `Email already in use`.
An unknown email answers `Invalid login.` like a wrong password (no account enumeration) and counts
in the same (name, IP) throttle and IP block.
After every login the brain pushes `["emailS", state]` (JSON clients; 7.9).

Pages: `["r", page, sub, myId, myName, s, region, item, captcha]` ->
`["0", page, sub, data, ...]` (`sub` echoed: the page's `io_0` gate):

| page | data |
| --- | --- |
| `profile` (sub = name) | `["0","profile",name,row,games[],mods[],[{f:0/1}],assets[]]`; row = `player_rows` + `player_follow` |
| `leaders` (sub = `config.leaderQueries` column, `region` = socialRegions index) | rows `{player_id, player_name, player_featured, player_hack, player_premium, player_clan, clan_rank, <column>}`; `player_clan`/`clans`: `{player_clan, clan_name, clan_rank, clan_score, clan_membercount, clan_hackcount, creatorname}` |
| `feed` | posts (followed players + own, else everyone), Je() shape `{pid, pln, pli, prm, ftr, cln, clr, flw, txt, typ, lnk, lnk2, tim (age ms), vot, cnt, dlk}` |
| `clan` (sub = tag) | `{clan_name, clan_rank, clan_score, creatorname, members:[{p, r, s, su, ve, h}]}` |
| `market` (sub `market`/`inventory`/`sales`/`stats`/`trades`/`blackm`) | listings `{i: skin, s, f: price, m: listing id, ss, p: seller, c: 1}`; inventory `{i, s, c}`; stats `{ski, cnt}`; trades `[open[], history[]]` rows `{trid, pl, player, buyer, sksell, skbuy, message, status}` |
| `itemsales` (item = skin) | `["0","itemsales","market",{ind, high, low}, owners[{nm, ct, su, ft}]]` |
| `map` / `mod` (sub = name) | published map/mod row (`map_id, map_name, map_creator, mi, vt, pl, ...`) or null |
| `maps` / `mods` | `["0", page, sub]` (lists come from `/api/maps` / `/api/mods`) |
| `clanwars` | `{}`; others (`search`, `virus`, ...): `null` |

Other labels (request -> reply):

| request | reply / effect |
| --- | --- |
| `["sp", query, captcha]` | `["spR", rows \| null]` (player search) |
| `["f", name, 1\|0]` | follow / unfollow (no reply) |
| `["gfs", name, id, n, captcha]` / `["gfg", ...]` | `["fwsr", rows, name]` followers / `["fwr", rows, name]` following; rows `{friendname, featured, subscription, date}` |
| `["pst", name, days]` | `["psd", [{d: "YYYY-MM-DD", f: KR}]]` daily market sales |
| `["guf", playerId]` | `["uf", posts]` |
| `["ps", {txt, typ, lnk, lnk2}]` | `["psr", 1]` / `["psr", null, err]` (20 posts/hour) |
| `["com", postId, {txt}]` | `["psr", 1, null, 2]` / `["psr", null, err, 2]` |
| `["gps", postId]` | `["psr", {p, c: comments[]}, null, 1]` / `["psr", null, "Post not found.", 1]` |
| `["lps", postId, 1\|0]`, `["dps", postId]` | like / unlike, delete own (staff: any); no reply |
| `["chtb"]` | `["chtb", [{fid, fna, cht: unread}]]` |
| `["ctd", peerId]` | `["chtl", [{fid, fna, txt, tim}]]` newest first; marks read |
| `["ict", peerId, text]` | `["chf", true]` / `["chf", false, err]`; recipient gets `["chtb", [{fid, fna, cht:1}]]` |
| `["kr", name, amount, msg, captcha?, {op}]` | `["gir", {funds, amount, fee}]`; receiver gets the "KR Gift" mail + `gmc`, `["econN", "gift", text]` and `ua` (economy: [doc/v7/economy.md](v7/economy.md)) |
| `["cln", tag]` (social page) | `["cln", msg, ok]` join request |
| `["cln", action, value, extra]` (menu) | `["cln", msg, data, ok, action]`: 1 create(tag) -> clanData, 2 request(tag), 3 my clan, 4 leave -> `["clnL"]`, 5 kick(id), 6 role(id, {r}) -> `{i, r}`, 7 funds(kr, {wd}) -> `{pf, cf}` |
| `["clnR", playerId, 1\|0]` | accept / decline a request (errors as `cln`) |
| `["uploadS", settings]` / `["getS"]` | `["pRes", "SUCCESS", null, msg]` / `["loadS", settings]` |
| `["nme", alias]` | `["nmeR", null, alias]` / `["nmeR", err]` |
| `["upp", old, new, repeat]` | `["upp", "Password updated.", newToken]` / `["upp", err]` (other sessions revoked) |
| `["kml", tab]` / `["rml", id]` / `["raml"]` | `["kml", tab, rows{km_id, km_datesent, km_playerfrom, km_subject, km_text, km_archive}]` / `["rml", [row]]` (credits attached KR once) / mark all read |
| `["li", skin, skinid, uid, price, {op}]` | `["lir", {cnt, funds, listingId, fee} \| {response:"MAX"}, uid]` |
| `["uli", skin, listingId, uid, {op}]` | `["ulir", "SUCCESS"\|"SOLD"\|null, uid, err]` |
| `["pi", listingId, uid, {op}]` | `["br", {f, i, sid, m, price} \| null, uid]` (client -> server `pi` is the purchase); the seller gets the "Item sold" mail, `econN` and `ua` |
| `["fil", n, text, rarities, weapons, types, season, order]` / `["srt", n, text, rarities, ...]` | `["fr", n, listings\|null]` / `["str", n, inventory]` |
| `["qsi", mode, skin, uid, amount, {op}]` (v7.1.4) / `["qsi", skin, uid, amount]` | `["qsr", {f, c, kr, j, jnk, sold, i: uid} \| null]` (KR = the item's `qsKR`, else `store.rarities[r].sell`; Junk = v10.0.3's rarity `scrap`, Contraband 5 / Unobtainable 50, 0.01 for `minQuickSell`, into the Junk wallet with reason `quicksell` (doc/v10/junk.md); mode 0 KR, 1 Junk, 2 destroy; amount 0 one, 1 all but one, 2 all; `j` the Junk balance after, `jnk` the Junk given; a noSale Black Market or opensea item is refused in every mode) |
| `["pbm", skin, {op}]` | `["bmr", {item, kr, f, title?} \| null]`: v7.1.4's Black Market, a `blackM` item for its store `funds` KR if `checkReq` holds (`accAge`, `stats`), once per account ("You already own this item"); an item with a v10.0.3 `title` grants it too (`player_titles`, `0361_player_titles`; the reply's `title`, deob-10.0.3.js:152803-152806). v10's `POST /market/black-market/purchase` answers `{item, kr, funds, title?}` (v10_market.c) |
| `["ttl"]` | `["ttl", [title id, ...]]`: the account's titles (`acc.titles`, setData slot 55; `player_titles`); the gapi profile's `player_titles` is the same list as `"87,90"` |
| `["st", skin, range, captcha]` | `["gd", [{d, f: KR total, t: sales}]]` (range: `config.dataDateRanges` index or days) |
| `["uml", playerId]` | `["ul", [{sk, skid, kr, c, ms, id}]]` (profile Listings tab) |
| `["ri", name, id, captcha]` | `["trD", name, id, [myInventory, theirInventory]]` (items `{i, c, nt?}`, `nt` = not tradable) |
| `["ctr", receiverId, give, take, msg, captcha?, {op, skr, rkr}]` / `["trA", tradeId, {op}]` / `["trC", tradeId, fast?, {op}]` | `["trR", result, tradeId\|null, msg]`; the other party gets `["econN", "trade", msg]` (give / take: `[{i, c}]`) |
| `["trCA", {op}]` | decline every open incoming offer -> `["pRes", "SUCCESS", null, "Declined All"]` |
| `["ecfg"]` | `["ecfg", {giftFee, giftMin, giftMax, giftMinLvl, marketFee, marketFeeBypass, marketMinLvl, saleMax, saleGrace, maxListings, tradeMinLvl, tradeMaxOut, tradeMaxOutP, tradeKRMax, tradeTTL}]`: the rules this server applies |
| `["krh", days]` | `["krh", [{id, amount, source, ref, cp, fee, tim}]]`: the account's own KR ledger, newest first |
| `["a2FA"]` / `["a2FAc", code]` / `["d2FA"]` / `["d2FAc", code]` | v7.1.4 2FA (authenticator app, 7.9): `["c2fa", 0, "a2FAc", otpauthUri]` -> `["c2fa", 2, null, null, true]`; `["c2fa", 0, "d2FAc"]` -> `["c2fa", 2, null, null, false]`; a wrong code: `["c2fa", 0, label, null, null, "Invalid Code"]` |
| password login of a 2FA account | `["c2fa", 0, "2fa"]` (the code popup); then `["2fa", code]` -> `["c2fa", 1]` + the `a` reply, or `["c2fa", 0, "2fa", null, null, "Invalid Code"]`. Token resumes skip 2FA; internal `login` answers `2FA required.` |
| `["cemail", email]` / `["emailv"]` / `["emailS"]` | Krunker Box Office email verification (7.9): `["cemail" \| "emailv", error, message, state]`, `["emailS", state]` |
| `["las"]` | log out every other session of the account |
| `["eb", {type, ...}]` | v10 friends event bus (doc/v10/social.md, `social_eb.c`): `friends:list` / `msg:send` / `msg:history` / `msg:read` / `presence:ping` -> `friends:list`, `msg:sent` + the peer's `msg:recv`, `msg:history:result`, `presence:pong`; pushes `unread:summary` (login), `presence:online/offline`, `friends:list` (a follow made or broke a friendship); errors `{type:"error", code: EB_*}` |
| `["gapi", rid, method, path, body]` | `["gapi", rid, status, {data} \| {error:{code, message}}]`: v10's gapi `/social/*`, `/players/*`, `/clans/*` (social_gapi.c, clan_v10.c; doc/v10/social.md) |
| `["uploadM", mapJson, thumbDataUrl, description, videoId, category]` | `["pubMR", "Map Published!" \| "Map Updated!"]` / `["upMp", err]`: publishes the map under its JSON `name` (3-30 chars; v3.1.1 catalogue names reserved; republishing your own map bumps `map_updatecounter`); a PNG data URL thumbnail (<= 200 KB) becomes `map_image` |
| `["edMp", name]` | `["edMp", mapJson \| null]` (own maps; devs / creation mods any) |
| `["ulb", mapId, type, custom, order]` | `["upMp", "Leaderboard Updated!" \| "Not allowed" \| "No leaderboard on this map" \| err]`: v10 updateMapLB (the editor's Leaderboard Display). Own maps (creator, devs, map mods): every LEADERBOARD object (`i` 72) gets `ldt` (type), `ldc` (custom text) and `ldo` (order), zero / empty values left out as v10's export does; the map is republished (`map_updatecounter` + 1). The editor shows nothing for the reply (v10: `#gameUpResp` is absent in that popup) |
| `["fvM", id, kind, 1\|0]`, `["ftC", id, 1\|0, kind]`, `["delC", id, kind]` | favourite; feature (devs); delete (creator, devs, creation mods) -> `["po", "SUCCESS", null, "Deleted"]` (kind 0 map, 1 mod, 2 asset) |
| `["uploadMo", ...]`, `["uploadA", ...]` | `["upMp", "File uploads are not supported on this server"(, 1)]` (v3.1.1 posts the files to `/upload-user-mod` / `/upload-user-asset`, not hosted) |
| `["spin", wheel, ticket, multi]` | `["spin", null, randNum(s), itemID(s), funds, ticket\|null]` / `["spin", err]`: v7.1.4 `store.wheels` (0 Starter 50 KR, 1 Elite 100, 2 Heroic 500; `multi` = 10 rolls at the price each, numbers become arrays) or a TICKET item's `spinWheel` (`wheel` null, `ticket` = its store index: consumed, no KR; Ultra (3) only spins this way, Free KR (4) through `fkr`). Each roll picks the slice its `randNum` lands on, then an item of `getSpnItems` (that rarity, `bigRar` and up for big rolls, this season unless `mSeas`, no `noSpin`/`blocked`/`limited`/`blackM`/`opensea`/`req`, matching `limType`/`limGroup`). Charged per roll and granted in one transaction (`kr_transactions` / `skin_transactions`, source `spin`, ref `<wheel>:<KR>` or `t<ticket>:0`). v7.1.4's server has no spin code: this is the contract its client implies |
| `["spinH", offset]` | `["spinH", [{ind, dte, fds}]]`: the won items newest first, 50 from `offset` (`dte` epoch ms, `fds` KR spent; v3.1.1-era rows cost their wheel's price); `null` for guests |
| `["fkr"]` | `["spinKR", lastReward, randNum, freeKRIndex, funds]` / `["spinKR", null, null, null, null, err]`: the Free KR wheel (rarities 50/39/10/1 %, `store.freeKR` 5/10/20/500 KR, level 1), once per `reward_time_s`; `sfkr` / `lfkr` / `fkrf` (v7's ad events) are accepted |
| `["clm"]` | `["clm", lastReward, funds]` / `["pRes", null, err]`: daily reward (level >= `reward_min_level`, `daily_reward_kr`, same clock as `fkr`); `["unbx"]` is accepted |
| `["rpt", name, reason, details]` / `["grp", name]` | `["rpr", ok]` / `["rps", rows]` (staff) |
| `["chg"]` | `["chgL", activeChallenges]`: `[{CI, T, DAT, REW, IC, N, PV, PT, TR?, PER}]` (v7 `account.activeChallenges`; TR = ms left of a daily / weekly one; 3 daily + 3 weekly rotate per account and UTC day / week) (WP-E part 2c, `progress.c`) |
| `["bdg", badge]` | `["bdgr", badge]` / `["po", null, "You dont own this badge"]`: `player_stats.bdg` (-1 clears; v7 io_badgeUpdate) |
| `["cbdg", badge]` | `["cbdgr", badge]` / `["po", null, err]` ("Badge not claimable", "Requirements not met", "Badge already claimed"): a claimable badge whose rule holds (Nuclear I-IV: `stats.n`) |
| `["mdl"]` | `["mdl", {medal: count}]`: the account's medal counts (Krunker Box Office) |
| `["bpdl"]` | `["bpdl", pass \| null]` then (signed in) `["bppd", {id, bpp, tier, items_claimed}]`: v7 io_dataLoaded / io_syncBpProg (7.8) |
| `["bpclm", itemId]` | `["bpclmr", null, null, skin, null, null, {id, tier, col, sk}]` + `bppd` / `["po", null, err]` ("Item not found", "Level not reached", "VIP Tier required", "Item already claimed") |
| `["xstbp", tier, code]` | `["purbp", "kr", null, null, tier, price, "KR", txid, null]` + `bppd` (+ `nbdgr`) + `ua` / `["purbp", "kr", {err}, "1"]`: the VIP tier for `tier1_price_kr` KR |
| `["xstbpl", levels]` | `["purbpl", "kr", null, null, levels, bpp, price, "KR", txid, null, badge]` + `bppd` + `ua` / `["purbpl", "kr", {err}, null, levels]`: skipped levels for `skip_price_kr` KR each |
| `["sst", name?, season?]` | `["sst", {season, name, kills, deaths, wins, games, score, timePlayed, bpp, tier, level, levels} \| null]`: the profile's season block (Krunker Box Office) |
| `["flg", name, 0\|1]` / `["ver", name, 0\|1]` | `["fgr", ok]` (mods) / `["verr", ok]` (devs) |
| `["crt", name, 0\|1]` | `["crtr", ok]` (devs): v10's creator badge (`players.creator_badge`, 0210), sent as `creator_badge` in the profile row and the account info |

Server pushes: `["ua", data40, info]` (v3.1.1 `updateAccount`) after a stats commit
of that account; `["gmc", n]` unread mail. After a commit with v7 progression fields (7.6):
`["chg", [done, CI, PV, ...]]` (v7 io_updateChallenges), `["chgC", [challenge...]]` (the
completed challenge objects: the end screen's cards and reward skins), `["chlR", skins, funds,
0, 0, kr]` (io_updateChallengeRewards) and `["nbdgr", [badge...]]` (checkBadges); with the
season (7.8) `["bppd", account.BP]` (battlePass.progress) and `["bpEnd", tmpBPData]` (the end
screen's Battle Pass section: `{bpp, pbpp, gn, cbp, bbpp, unbpp, rbpp, mx, ml, id, re}`).

### 7.5 Hub HTTP catalogue (`creations.c`, v3.1.1 `publicRoutes.ts`)

`config.apiURL` = the brain. `GET /api/maps?index=&pos=&accountId=&val=` ->
`{data, index, pos, error: null}`, 42 rows per page: the v3.1.1 map catalogue
(`shared/include/map_catalog.h`, 15 official + 21 community maps in the offline
server's tab orders: 0 Popular, 1 Featured, 2 New, 3 Favorites, 4 all) followed by
the maps players published (`published_maps`; tab 3 = accountId's favourites, tab 4
with accountId = their maps, New lists them first). Rows `{map_id, map_name,
map_date, map_initialdate, map_info, map_image, map_votes, map_pl,
map_updatecounter, map_featured, map_description, creatorname, fund, fav, mod_url,
mod_id}`. `/api/mods` (published mods), `/api/assets?index=&cat=&username=&val=`,
`/api/search?type=map|mod&val=` (`{data, index: 0, error}`), `/api/streams`,
`/api/prices` (v3.1.1 placeholders), and `GET /api/map-data?name=<map>|id=<map_id>`
(alias `/api/published-map`): the published map JSON, for clients playing and game
servers hosting community maps; 404 `{"error":"not_found"}`. `&meta=1` (game servers)
wraps it as `{"id": <map_id>, "map": <map JSON>}`: the id is v10.0.3's `customMapData.id`,
which KrunkScript `player.giveSkin` checks (deob-10.0.3.js:208303).

### 7.6 Internal ops (game servers)

`accounts_internal_call(b, op, args, cb, ud)` behind svc-matchmaker's internal WS
(`{"type":"acct","rid","op","args"}` -> `{"type":"acct-r","rid","err","data"}`);
`err` is a user-facing string to forward verbatim.

| op | args | data |
| --- | --- | --- |
| `login` / `register` | `{name, pass, ip, ref?}` | account + `token` |
| `token` / `session` | `{token, name?}` | account (token kept) |
| `get` | `{id}` | account |
| `logout` | `{token}` | `{}` |
| `alias` | `{id, alias}` | account |
| `password` | `{id, old, new}` | `{token}` |
| `stats` | `{id, match, kills, deaths, score, won, games, time_ms\|timePlayed, kr, xp?, class\|classIndex, mode, map, region, region_index?, server, stats{}, prog?, medals?, mapIndex?, mapId?, ranked?, final?}` (deltas of one match) | account + `{kr_awarded, duplicate}` |
| `rewards` | `{items: [[id, kr], ...]}` | `{funds: [...]}` |
| `give_skin` | `{id, skin, source?}` | `{skin, count}` |
| `elo` | `{id, mode: 1\|2\|4, elo}` / `{id, mode, delta}` (atomic, clamped at 0) | `{}` / `{elo}` |
| `challenge` | `{id, chal, kr}` | `{chal, funds}` |
| `ban_check` | `{ip, id?}` | `{banned, reason, expires}` |
| `storage` | `{id, name, map, type: 0-5, scope: 0\|1, data: [[key, value]...]}` | `{rows: [[key, value]...]}` (the touched keys after the operation) |
| `storage_load` | `{name, map, from?}` | `{rows: [[key, value]...]}` (`from` = another map of the same creator: its global rows) |
| `map_reward` | `{id, amount, map}` | `{rsp: "Success", funds}` or `{rsp: <refusal>}` |
| `turf_clan` | `{map}` | the clan holding the map today `{clan_id, clan_name, clan_logo, clan_banner_color}` (yesterday's settled winner) or `null` |
| `map_spin` | `{id, wheel}` | `{skin, rand, funds, rarity, wheel, price}`: one KR spin of store wheel 0..2 for a map PRIZE_WHEEL, KR and skin in one transaction (`P_SPIN_MULTI`, source `spin`); "Wheel unavailable", "Not enough KR" |
| `studio_game` | `{map}` | `{map_id, studio_id, monetized}`: whether the published map's studio is approved for monetization |
| `studio_charge` | `{id, map, amount, key}` | `{success, krBalance, charged, earned, duplicate}`: a consented PAYMENTS charge (ledger source `studio-charge`, ref = key; `studio_charges` row with the studio's 90 %); the key is idempotent; "Monetization is not enabled for this studio", "Not enough KR", "Invalid charge" |

`turf_clan` / `map_spin` / `studio_game` / `studio_charge` and the `unbox` / `raffle-win` pushes (5.2) are what a
game server needs to send Krunker v10.0.3's `warsClan`, `spinW` / `unbS`, `unb`, `lotWin` and `chreq` the way its
own server does (services/brain/node_events.h, server/src/brain_events.c, doc/v10/s2c-coverage.md; migration
`0358_studio_charges`). Tests: `ctest -R brain_node_events` (every driver), `ctest -R brain_events_e2e` (two
server_bin nodes and raw clients on SQLite).

`storage` / `storage_load` / `map_reward` are KrunkScript's `GAME.STORAGE`, the Save / Load Data
triggers and `GAME.PAYMENTS.giveKR` / Reward KR of a game server hosting a published map
(doc/v7/krunkscript.md 2.6, WP6; `db_storage.c`, migration `0300_ks_storage.sql`): rows of table
`ks_storage (account_id, map_id, skey, svalue, scope)`, the map resolved by `published_maps.map_name`
(unknown: `"Storage only supported on published maps"`), the v7 data checks again; KR rewards come out of
`published_maps.map_funds`. Tests: `ctest -R brain_storage` (ops, every driver), `ctest -R
brain_ks_storage_e2e` (server_bin + kn_brain + a hosted published map).

`stats` is idempotent per `(match, id)` (`match_results` primary key; a retry
returns `duplicate: 1` and changes nothing), adds XP to the per-class `c<class>`
stat and the clan's score, clamps KR to `max_match_kr`, writes a ledger row.
`stats{}` holds v7's incStat counters (`s, h, hs, ls, wb, mk, fk, tmk, ast, spry, crc, n, ad,
sad, cad, sl, jg, kjg, jgk, wm`), added to `player_stats`. player-viewer WP-E part 2c: when
`prog` (`[[key, value, weapon, streak, item], ...]`, the match's challenge tallies; item = the
spray / found skin of v7's `{item}` contexts, -1 none) or `medals`
(`{name: count}`) is present and the commit is not a duplicate, kn_brain then runs
`P_PROGRESS` (`db_progress.c`): v7's challenges.finalize on the account's active challenges
(`final: false` = a player who left: only `DAT.nem` ones), KR / skin rewards (`kr_transactions`
source `challenge`), the medal counts, and checkBadges (claimable badges whose rule holds), then
pushes 7.4's match-end frames. The season part of the same run (7.8): the commit's `bpp` (v7
endGame's per-match BPP before the cap, final matches only), the completed challenges' `REW.bpp`,
and `kills` / `deaths` / `won` / `games` / `score` / `timePlayed` into `season_stats`.

### 7.7 Tests

* `ctest -R brain_social_ws` (`services/brain/tests/social_ws_test.py`): starts
  `kn_brain --migrate` on a fresh schema `kn_test_accounts` of database
  `krunknative_svcacc` (override `KN_TEST_DB_NAME` / `KN_TEST_SCHEMA`), then drives
  the social socket in msgpack (own encoder, independent of knsvc) and JSON and the
  internal WS: validation strings, register/login/throttle/token resume/logout,
  profile, follows, search, feed, settings, display name, clans, chat + push,
  leaderboards, gift + mail, market list/buy/quick sell, trades, stats commit
  idempotency + `ua` push, password change revocation, map publishing + `/api/*`
  catalogue, 2FA, v10 prize wheels (the s10Pool wheels, Universal / v7 tickets, the
  served `/api/data/spins`) / Free KR / daily reward, elo deltas. One schema per run (`kn_test_accounts_<pid>`, dropped after),
  brain on an ephemeral port (read from its "listening on" log line), no sleeps
  except the login throttle window: 50/50 sequential and concurrent runs pass.
  Skips (77) without PostgreSQL or python `websockets`.
* `ctest -R brain_progress` (`services/brain/tests/progress_test.py`, every driver): the
  challenge list and its daily / weekly rotation, a commit with tallies -> the match-end pushes,
  duplicate and partial commits, badge select / claim refusals and the auto-awarded Nuclear I,
  medal counts, `badges` in the account object (WP-E part 2c).
* `ctest -R brain_account_security` (`services/brain/tests/account_security_test.py`, every
  driver, `tests/fake_smtp.py` on a free port): register with an email -> the mail (STARTTLS,
  AUTH, From, Message-ID, text + HTML) -> its link verifies (`emailS` push, gated features open),
  link reuse / expiry / a replaced address, the resend cooldown and daily cap, `mail_log` (sent
  with the Message-ID, a 550 recipient as failed, no tokens), the 2FA lock after 10 wrong codes.
  `ctest -R brain_social_ws` covers the TOTP flows (`a2FA` .. `d2FAc`, the login step, replay).
* `ctest -R brain_email_web_e2e` (`services/brain/tests/email_verify_web_e2e.py`, needs
  `KN_WEB_DIST` = a `make web` dist dir, else skipped): the real web client in headless Chromium
  registers with an email in the account window, the fake SMTP server gets the mail, its link
  turns the open Account Security block "Verified", and Enable 2FA -> v7's popup with the QR code
  -> six typed digits enable 2FA.
* `ctest -R brain_season` (`services/brain/tests/season_test.py`, every driver): the pass and
  account.BP, claims and their refusals, the match BPP and the daily cap (`bpEnd`), duplicate /
  left commits, the VIP tier and level skips in KR (`kr_transactions` rows, refusals without KR),
  the season badges, `sst` and the season leaderboard, a restart (the season row kept) and the
  rollover to season 8.
* Real v3.1.1 client: `services/brain/tests/v311_site_proxy.js <port> <brainPort>
  <v3.1.1 public dir>` (same-origin front like nginx) +
  `v311_social_client.js <port> <cdpPort> <user> <pass> <outDir>` against headless
  Chromium: the untouched v3.1.1 `social.html` logs in through its own popup,
  resumes by token, posts, and renders profile, feed, leaderboards and the 36-map
  catalogue from the brain.
* Krunker Box Office web client: client-integration's build (`?brain=<brain url>`)
  registers and logs in through the menu's account window in headless Chromium.

### 7.8 Seasons and the Battle Pass (`season.c`, `db_season.c`, `0108_seasons.sql`)

Krunker v7.1.4's `config.currentSeason` (7) and `currentBattlepass` (2), windows[5] (the Battle
Pass window), endGame's `tmpBPData` and challenges.finalize's `REW.bpp`, with the rules in
`shared/src/season.c` (client, kn_brain and tests share them):

* **Season**: `accounts.current_season` (default 7). At start kn_brain runs `P_SEASON_ENSURE`:
  it inserts the season's `seasons` row when missing (insert only: an existing row and older
  data stay as they are) and, when it inserted one, sets `ends_at` of older open seasons (the
  rollover). `season_stats` and the season leaderboards (`["r", "leaders", "season_score" |
  "season_kills" | "season_wins" | "season_games" | "season_timeplayed", ...]`, the value under
  the query's name) are per season; lifetime totals stay in `players`.
* **Pass**: the active `battlepasses` row of the season (status 1); `bpdl` sends it in v7's
  `BattlePassPayload` shape (`id, name, descr, bpp_perlevel, itemcount, tier0name/price,
  tier1name/price, itemdata, startdate, enddate`) plus `season` and `skipprice`. Seeded: the
  Season 7 pass as it shipped (id 2, 50 levels x 300 BPP, the Krunker wiki's 100 rewards: season-7 "battlepass" skins and Season 7 tickets).
* **BPP**: a finished public match's commit carries `bpp` = min(20, floor(score * rewardMlt *
  0.01)) + 30 when the player played half of the match (the game server); kn_brain awards it
  under the daily cap ((days since startdate + 1) * 500: v10.0.2's windows[5]; v7.1.4's
  `config.bppCapDay` was 300) and the pass total,
  adds the completed challenges' `REW.bpp` on top, and pushes `bppd` + `bpEnd`. Nothing after
  `enddate`.
* **Claims**: a card's skin goes to the inventory (`skin_transactions` source `battlepass`,
  ref `bp:<pass>:<item>`) when its level is reached, its track is free or the account has the
  VIP tier, and it is not in `items_claimed`.
* **KR purchases** (Krunker Box Office: v7 sold them for USD through Xsolla / Steam): the VIP tier
  (`tier1_price_kr`, default 1200) and skipped levels (`skip_price_kr` each, default 200; at most
  up to the last level) are debited through `db_add_funds` (`kr_transactions` sources
  `battlepass` / `battlepass-skip`, ref `bp:<pass>:tier1` / `bp:<pass>:skip<n>`) in the same
  transaction as the change; not enough KR refuses with "Not enough KR".
* **Season badges** (badges.js 22..28, `season_badge_rule`): Founder = the VIP tier bought
  within 7 days of the pass's startdate (v7: fixed 2022 / 2023 dates), Battle Pass Master =
  VIP + bpp >= 15000, Badge Master = the season's other badges, S6 Map Master = the 17 Season 6
  map reward skins; only in their season. Checked at every commit, VIP purchase and level skip,
  and on `cbdg`.

Migration and API summary: `0108_seasons` (postgres / sqlite / mysql) creates `seasons` (row 7),
`battlepasses` (pass 2), `battlepass_progress`, `season_stats`; procs `P_SEASON_ENSURE`,
`P_BP_DATA`, `P_BP_CLAIM`, `P_BP_BUY`, `P_BP_SKIP`, `P_SEASON_STATS` (C on every driver) and
the season part of `P_PROGRESS`; social labels `bpdl`, `bpclm`, `xstbp`, `xstbpl`, `sst` and the
pushes `bppd`, `bpEnd`, `purbp`, `purbpl`, `bpclmr`; commit fields `bpp` and the `prog` item
column; leaderboard queries `season_*`.

* **v10.0.2 additions** (`0116_v10_rewards`): `battlepasses.leveldata` ("level:bpp,...": a
  level's own cost, else `bpp_perlevel`), `krdata` / `jnkdata` ("itemId:amount,...": a card's KR
  / junk) ride in `bpdl` and `tmpBPData.lv`; a KR card pays through `db_add_funds` (source
  `battlepass`) when claimed. `["bpclmAll"]` (v10 BP_CLAIM_ALL) claims every claimable card and
  answers v10's `["bpclmr", null, funds, null, null, 0, [{id, tier, col, sk, kr, jnk}...]]`
  ("No items to claim" when none); `bpclm` keeps v7's reply. `["xstbp", tier, lang, null, null,
  channel, name]` (v10 selectBPGift) buys the VIP tier for `name` (source `battlepass-gift`,
  counterparty = the recipient, a "Battle Pass Gift" mail, `bppd` / `nbdgr` pushed to their
  sessions) and answers `purbp` with its 10th / 11th arguments `true, name` (purchaseRes gifted).

### 7.8.1 Signup rewards (`rewards.c`, `db_rewards.c`, `0116_v10_rewards.sql`)

Krunker v10.0.2's "Get Signup Rewards" popup promises "1-time signup perks: Gain 14 Cosmetics +
1 Day premium for free!"; v10's server answers a new account with POPUP_REGISTER_REWARDS
(`prr`, "Welcome to the Family!") right after the registration. kn_brain grants them right after a
social-socket registration (`"a"` action 0: the menu and the web build) through
`P_SIGNUP_REWARDS`, once per account: the `signup_rewards` row is its primary key, written first
(`{insert_ignore}`), so nothing is granted twice; a later login, a saved-token login at startup
or an account made elsewhere (internal ops, a central account) gets no popup, as in v10. Only accounts created at or after `signup_rewards_epoch.since` (the
migration's time) qualify. The cosmetics go to the inventory (source `signup`) and the premium is
`premium_ms` added to `players.player_premium` (premiumT) until `premium_until`; a sweep
(`P_SIGNUP_PREMIUM_SWEEP`, every `sweep_s`) takes it back once. On a grant the session gets
`["prr", [skin...], premium]` and `ua`.

| key (`accounts.signup_rewards.*`) | default | |
|---|---|---|
| `enabled` | `true` | grant the perks |
| `skins` | 14 common store items (`rewards.c`) | the cosmetics (store.skins indices); v10's list is server data |
| `premium_hours` | `24` | the premium (0 = none) |
| `sweep_s` | `600` | how often expired signup premium is taken back |

Tests whose fixture accounts register through the social socket and count inventories (or
whose client must not show the popup) pass `--accounts.signup_rewards.enabled=false`.
Tests: `services/brain/tests/rewards_test.py` (every driver: the grant, never twice, the epoch,
the sweep, `bpclmAll`, krdata / leveldata, gifting) and `server/tests/v10_rewards_e2e.py` (the
client on ui/: Get Signup Rewards -> register -> granted once; a challenge completed on
server_bin; a Battle Pass claim and gift).

### 7.9 Account security: 2FA, email verification, outbound mail (`account_security.c`, `db_security.c`, `0109_account_security.sql`)

What Krunker v7.1.4 has (src/index.js, i18n, packets.json): register / login by username and
password only, the password change (`upp`), "Logout of all Sessions" (`las`), and 2FA with an
authenticator app (index.js `twoFAres` / `act2FA` / `dact2FA` / `sub2FA`, `config.twoFAKeyLen` 6,
the popup's QR code of an `otpauth://` URI, "Scan QR code with Authy or Google Authenticator").
It has no email flow at all: no email field, no verification, no reset by email, no 2FA recovery
(lost passwords: "contact recovery@yendis.ch"); `Account.email` (data[40]) is never shown and
`packets.json` reserves `cemail` / `emailv` without code. Nothing is gated on an email.

* **2FA (v7.1.4 parity)**: TOTP (RFC 6238: SHA-1, 6 digits, 30 s, +-1 step) replaces v3.1.1's
  pattern grid; 0109 switches existing grid secrets off. `["a2FA"]` stores a fresh 160-bit secret
  in `twofa_pending` and answers `["c2fa", 0, "a2FAc", "otpauth://totp/<issuer>:<name>?secret=
  ..&issuer=..&algorithm=SHA1&digits=6&period=30"]`; the first valid `["a2FAc", code]` makes it
  `twofa_secret` (`["c2fa", 2, null, null, true]`, data[28] = 1). `["d2FA"]` asks for a code,
  `["d2FAc", code]` turns it off. A password login of a 2FA account gets `["c2fa", 0, "2fa"]`;
  `["2fa", code]` finishes it. Every accepted code moves `twofa_last_step` (never accepted twice);
  10 wrong codes within 15 minutes lock the code step for 15 minutes ("Too many attempts. Please
  Try Later"); a wrong code is not a login failure (no `(name, IP)` throttle: the password was
  right). The reason of a repeated prompt rides as a 6th element (`"Invalid Code"`): Krunker Box Office
  shows it in the popup (v7 shows nothing). Token resumes skip 2FA (as v7). Procs
  `P_TWOFA_CONFIRM`, `P_TWOFA_DISABLE`, `P_TWOFA_LOGIN`.
* **Email verification (Krunker Box Office)**: an email at register (`["a", 0, [name, pass, ref,
  email]]`; the clients require it, the brain with `accounts.email.required`) or later in the menu
  (`["cemail", email]`; a new address is stored unverified and voids older links; an address another
  account has is refused, `Email already in use`: one account per email, it signs in like the username) mails a link `<link_base>/auth/verify-email?token=<43 chars>` (GET, also
  `/api/verify-email`, an HTML page; tokens stored as sha256, 24 h, one use; the address must
  still be the account's). Verifying sets `email_verified_at` and pushes `["emailS", {email,
  verified, pending, wait, mail}]` to the account's sockets. `["emailv"]` resends; limits: one
  mail per `resend_cooldown_s` (60 = v7's unused `config.accValidationTime`), `max_per_account_day`,
  `max_per_address_day`, `max_per_ip_hour` (from `mail_log`). Replies `["cemail" | "emailv",
  error, message, state]`: "Verification email sent to <address>", "Please wait N seconds before
  requesting another email", "Too many emails today. Please Try Later", "Email already verified",
  "No email on this account", "Email is unavailable on this server", "Invalid Email". Procs
  `P_EMAIL_STATE`, `P_EMAIL_REQUEST`, `P_EMAIL_VERIFY`.
* **Gates**: `accounts.email.require_verified` lists social features that need a verified email
  (`chat` ict, `post` ps / com, `gift` kr, `market` li / pi, `trade` ctr / trA, `clan` create,
  `publish` uploadM) -> `["po", null, "Verify your email to use this feature"]`. Default none
  (v7.1.4 parity).
* **Password reset by email (Krunker Box Office; v7.1.4: "contact recovery@yendis.ch")**: `["rpr",
  name | email]` mails a one-time code (12 base32 characters, shown `XXXX-XXXX-XXXX`, 60 bits,
  stored as sha256 in `email_tokens` kind `reset`, `reset_ttl_min` 30, a new request voids the
  older code) to the account's **verified** email (an address shared by several accounts: one
  mail, a code per account, at most 3). The answer is always `["rpr", null, "If that account has
  a verified email, a reset code is on its way. Check your inbox."]` (no account or address
  enumeration; the resend limits above apply silently, only the per-IP cap answers "Too many
  emails. Please Try Later"). `["rpc", code]` -> `["rpc", null, username]` checks it (typed codes
  ignore case and dashes; 0 / 1 / 8 read as O / I / B); `["rps", code, new, repeat]` -> `["rps",
  null, msg, username, token]` sets the password, uses the code up, revokes every session and
  issues one (the client signs in with it); errors "Invalid or expired code.", "Passwords do not
  match.", the password rules. The mail also links `<link_base>/auth/reset-password?token=<code>`:
  GET a small form page, POST (form-urlencoded `token`, `password`, `repeat`) sets it (no cookies;
  CSP, no-store, no-referrer, frame-ancestors none). Procs `P_RESET_REQUEST`, `P_RESET_CHECK`,
  `P_RESET_APPLY` (C on every driver). 2FA recovery by email is not implemented.
* **Mail**: `knsvc/smtp.h`'s `kn_mailer` (3.10): "Verify your Krunker Box Office email" / "Reset your
  Krunker Box Office password", text + HTML, From
  `smtp.from`. Every mail is a `mail_log` row (queued -> sent with the Message-ID, or failed with
  the server's reply); log lines mask the address (`p***@example.org`); credentials and tokens are
  never logged or stored.
* **Client**: `client/src/ui/account_ui.c`: v7's 2FA popup (QR code: `client/src/ui/qr_code.c`,
  an SVG data URL; six digit inputs, `onInput2FA` / `onKeyDown2FA`, also on the hub and editor
  pages' login popups), the account window's email input ("Enter Email": required by Register, Login takes it instead of
  the username), the v10 login modal's Email / Username field (`client/src/ui/v10_login.c`), and an Email block
  in Settings > Account Security (address, Verified / Not verified, Resend, change), and
  "Forgot password?" under the login form (`knResetOpen`: name or email -> the mailed code, also
  "Got a code already?" -> the new password; Enter submits each step). Enter in the password field
  logs in; Tab / Shift+Tab move the focus like a browser (`events.c` focus_step). Login errors say
  why: Offline Mode, no account server configured, server unreachable / socket not open, no
  answer within 8 s. The HTML engine sizes `<input type=number min max>` like Blink (the six
  digit boxes in one row). Native e2e: `make native-auth-test` (`client/tests/native_auth_e2e.py`).

| key | default | meaning |
| --- | --- | --- |
| `smtp.host` | `""` | SMTP submission server; empty = no mail (accounts work, emails stay unverified) |
| `smtp.port` | 465 implicit / 587 | |
| `smtp.tls` | `auto` | `implicit` (465), `starttls`, `none` (no AUTH then), `auto` = by port |
| `smtp.user` / `smtp.password` | | AUTH PLAIN / LOGIN (`smtp.auth`: `auto`, `plain`, `login`); prefer `KN_SMTP_PASSWORD` |
| `smtp.from` | `Krunker Box Office <noreply@caan.edu>` | From header and envelope sender (`smtp.reply_to` optional) |
| `smtp.verify_peer` / `smtp.ca_file` | `true` / | certificate checks |
| `smtp.timeout_ms`, `smtp.retries`, `smtp.retry_delay_ms`, `smtp.queue_max` | 20000, 2, 5000, 256 | |
| `accounts.email.link_base` | `brain.public_url` | base of the verify link |
| `accounts.email.verify_ttl_h` | 24 | link lifetime |
| `accounts.email.reset_ttl_min` | 30 | password reset code lifetime |
| `accounts.email.resend_cooldown_s` / `max_per_account_day` / `max_per_address_day` / `max_per_ip_hour` | 60 / 5 / 5 / 10 | |
| `accounts.email.required` | false | register needs an email ("Email Required") |
| `accounts.email.require_verified` | `[]` | gated features, list or `a,b` |
| `accounts.login.require_verified` | `true` | the v10 sign-in gate: a password login of an account whose email is not verified answers `ensure_verified` (v10_auth.c); `false` = login as before |
| `accounts.login.email_code` | `true` | v10 sign-in by emailed code (POST /auth/login/email-code, rows 35-37): a six-digit code to the account's email; `false`, or no mail server, answers the not-offered 404. Accounts with 2FA get no code. Assumed from v10.0.2's consumer (the enter_code step; the 10.0.3 bundle has the same routes) |
| `accounts.login.code_ttl_min` | `10` | the emailed sign-in code's lifetime; it works once, and five wrong codes end its sign-in |
| `accounts.twofa_issuer` | `Krunker Box Office` | the otpauth issuer |

Deployment: `KN_SMTP_*` from `~/.config/kn-secrets/smtp.env` or the deploy-kru CI secrets become
`/etc/krunknative/smtp.env` (`deploy/ansible/README.md`).

Migration and API summary: `0109_account_security` (postgres / sqlite / mysql); procs above (C on
every driver); social labels `a2FA`, `a2FAc`, `d2FA`, `d2FAc`, `2fa` (code), `cemail`, `emailv`,
`emailS`, register's 4th payload element; routes `GET /auth/verify-email`, `GET /api/verify-email`.

### 7.10 Captcha (`captcha.c`, `captcha_http.c`)

Box Office's captcha is an ALTCHA proof of work in front of a password sign-in, a registration and a hosted
game (the spec, every Krunker v10.0.3 call site, and why not Turnstile: [v10/captcha.md](v10/captcha.md)).
`GET /mm/captcha?for=login|register|hosting` issues a challenge (stateless: signed with
`HMAC-SHA256(brain.join_secret, "kn-captcha-altcha-v1")`, so any brain of the network verifies it); the
client solves it (`client/src/net/kn_captcha.c`, native and web) and sends the payload in the flow's captcha
field; the brain checks it at the flow's entry, before the password is hashed: hash, signature, expiry,
purpose, and once per brain (replay).

| flow | captcha field | refusal (enforced) |
| --- | --- | --- |
| social `["a", 1, [name, pass, ...], captcha]` / `["a", 0, ...]` (login / register; not a token resume) | 4th element | `["a", "<text>"]` |
| `POST /auth/login`, `/auth/register` | `captcha` | 400 `{"ok":false,"error":"<text>"}` |
| `POST /auth/login/username`, `/auth/login/email` (v10) | `b` (v10.0.3's) | 400 `{"data":null,"error":"<text>"}` |
| `/mm/host-game` | `?captcha=` / body `captcha` | 400 `CaptchaRequired` (none) / `RecaptchaFailed` |
| `/mm/seek-game` (`for=seek`; enforced only) | `?captchaToken=` | 400 `CaptchaRequired` (any refusal): the client solves and retries, at most twice, with a fresh `validationToken` |
| a game server's player (`for=game`; enforced only) | `NET_EXT_CAPTCHA_ANSWER` | `NET_PACKET_ERROR` "CAPTCHA FAILED" (`004008`) |

`<text>` is v10.0.3's: "Please complete the security challenge to continue." (none) or "CAPTCHA FAILED". Turning
enforcement on also makes every Box Office seek and every game join solve one (a few frames each).

| key | default | meaning |
| --- | --- | --- |
| `captcha.enforce` | `false` | refuse the flows above without an accepted payload. Off: everything goes through, and what would be refused is logged at DEBUG (`login from <ip>: captcha missing (captcha.enforce off: let through)`); refusals are INFO |
| `captcha.max_number` | `100000` | the challenge's `maxnumber` (1000..1e7): a solver tries about half of it (SHA-256s). The ALTCHA widget's default is 1e6 |
| `captcha.ttl_s` | `300` | a challenge's lifetime (10..3600), in its salt's `expires` |

In-game (v10.0.3's `cpt`): the internal WS `welcome` carries `"captcha": {"enforce": bool}`; a node asks the brain
`{"type":"captcha","rid","op":"issue"}` -> `{"type":"captcha-r","rid","challenge"}` and
`{"type":"captcha","rid","op":"verify","payload","challenge"}` -> `{"type":"captcha-r","rid","ok","result"}`
(server/src/game_captcha.c asks every player connection once, closes a refused or late one).

Ansible: `kn_brain_captcha_enforce` (roles/kn_brain/defaults, `false`) is `captcha.enforce` in the brain's
config.json. The challenge route is served with enforcement on or off, and the brain logs
`captcha challenges issued, not enforced` or `captcha enforced` at start.

**Rollout** (enforcement never before the clients that solve are what players run):

1. Deploy the brain with this change and `captcha.enforce` false (the default). Every client keeps working:
   old ones send no captcha and are let through.
2. Ship the client that solves (native packages, web, launcher channel). A new client against a brain from
   before step 1 gets 404 from the route and sends no captcha, so the order of 1 and 2 does not break
   anything; the brain first means the new clients' payloads are checked (and logged) from their first day.
3. Watch the shadow log: run one brain at `--debug=2` for a while, or count the `captcha missing` lines
   against `captcha accepted`. Old web tabs reload, the launcher updates the native client (9.8); wait until
   `missing` is only scripts.
4. Enforce: `kn_brain_captcha_enforce: true` (group_vars) and redeploy. Challenges issued before the restart
   stay valid (same key). Roll back by setting it false again.
5. Every brain of a network runs the same setting and shares `brain.join_secret`. The spent set is per
   brain: a payload accepted on one brain could be accepted once more on another, each still a proof of work.

Tests: `kn_brain_captcha_test` (unit), `brain_captcha_e2e` (`services/brain/tests/captcha_test.py`),
`kn_captcha_test` (the client), `client/tests/captcha_e2e.py` (headless client_bin, enforced brain),
`brain_captcha_game_e2e` (`server/tests/captcha_game_e2e.py`: the seek's CaptchaRequired and the in-game `cpt` with a
brain-linked server_bin and client_bin).

### 7.11 Account lifecycle, Twitch linking and the raid record (`account_life.c`, `raids.c`, 0359 / 0360 / 0361)

v10.0.3's Account window behind kn_brain (deob-10.0.3.js:244591-244763):

- **Password change:** `["upp", new, repeat, sessionToken]` (v10) or `["upp", old, new, repeat]` (v7 hub). The
  v10 proof is the socket's own session token. Either changes the password once per 30 minutes
  (`accounts.password_changed_at`), revokes every other session and answers `["upp", "Password updated.", newToken]`.
- **Deletion:** `["rqd", 1 | 0]` -> `["rqd", ok, error, null, flag]`. A request sets
  `accounts.delete_requested_at` (`acc.reqDel`, "Processing Deletion") and ends every session; the client logs
  out, as v10 does. Cancelling clears it. After `accounts.deletion_grace_days` (14) with no login since the
  request, the hourly sweep (`accounts.deletion_sweep_s`; `accounts.deletion_purge: false` turns it off)
  deletes the account; references cascade.
- **Twitch:** `twitchAuth` returns the OAuth authorize URL (single-use state, 10 min), which the client opens
  in the system browser. The brain is the OAuth client (`streams.twitch.client_id` / `client_secret`;
  `redirect_uri` defaults to `<brain.public_url>/twitch/callback`). `GET /twitch/callback` exchanges the code
  on a worker thread, stores the permanent pair (`twitch_links`) and pushes `twitchVerifyResponse`.
  `removeTwitchLink` clears the account's link (the pair stays, as v10 warns), `getTwitchDrops` answers `[]`
  (no Box Office drop campaigns). With no Twitch app configured the answer is "Twitch linking is currently
  unavailable.". The account object gains `reqDel`, `twitchId`, `twitch` (the login) and `raidStats`.
- **Raid record:** a game server's stats commit may carry `raid: {map, cleared, time, aiKills, bossKills,
  deaths}` per player (public-type Raid round on an official map; `server/src/accounts.c`). Once per match,
  `raid_stats` and `raid_stats_monthly` record attempts, completions, best time and the kill counts. Served as
  `acc.raidStats` (Account window's Raids tab) and `GET /leaderboards/raids/<map>[?sort=completions]` and
  `/leaderboards/raids/<map>/monthly?month=YYYY-MM[&sort=best_time]`. A boss death (`boss` on the map AI or the
  AI.spawn option) gives every active non-bot player `bossKills` and the "bk" challenge.
- **Titles:** a Black Market item with a `title` grants it (`player_titles`, 0361) and the purchase reply names
  it; `ttl` lists the account's titles and the profile carries `player_titles`.

Tests: `brain_account_life` (`services/brain/tests/account_life_test.py`): the password shapes and clock, the
deletion request / cancel / sweep, the Twitch handlers with no app, the raid record and its leaderboards.

## 8. Game server (`server_bin`): config, brain client, accounts

Owner: gameserver. Code: `server/src/config.c` (config reader),
`server/src/brain.c` (the brain link, port of v3.1.1 `server/brainClient.ts`),
`server/src/accounts.c` (join/auth, stats, bans, billboards, hosting) and small
hooks in `server/src/net.c` / `main.c`. Client side of the new packets:
`client/src/net/player_accounts.c` (+ `prefab.c` billboards, `nametags.c`,
`hud_ui.c` scoreboard, `game_events.c` kill card).

One `server_bin` process is one lobby. **Without `gameserver.brain_url` it is the
standalone server it always was**: guests named `Guest_N`, no JOIN handling, no
PLAYER_INFO packets, no brain thread. `server_bin` links the libpq-free `knsvc`
(WebSocket client, HS256 tickets); a build without OpenSSL/knsvc ignores
`brain_url` with a warning.

### 8.1 Configuration (`config/config.json` → `"gameserver"`)

Precedence: **CLI flags > environment > config file > defaults**.

* File: `--config FILE`, else `$KN_CONFIG`, else the first `config/config.json` in the
  cwd or up to 4 parent directories (same search as `knsvc/config.h`). A missing file
  means all defaults; an invalid one is a startup error. Relative paths in the
  section (`bans_file`, `custom_map_dirs`, `billboards_file`) resolve against the
  repository root (the parent of the config directory).
* Env: `KN_GAMESERVER_<KEY>` for every key below (`KN_GAMESERVER_MAX_PLAYERS=12`,
  `KN_GAMESERVER_MODES='["tdm","ctf"]'`; values that look like JSON are parsed as
  JSON). v3.1.1 aliases (lower priority than `KN_GAMESERVER_*`): `MM_BRAIN_URL`,
  `MM_BRAIN_SECRET`, `MM_NODE_REGION`, `MM_NODE_ID`, `PUBLIC_DOMAIN`, `PUBLIC_PORT`,
  `PERMS=1`. `KN_GAMESERVER_INDEX=i` (set by `services/scripts/backend-up.sh`) runs
  server *i* of N from one config: port + i, public_port + i (when set), node id
  `<node_id><i>`, name `<name> #<i+1>`.
* CLI: `--port`, `--mode`, `--round-seconds`, `--perms`, `--headless`, positional
  `map.json` (unchanged), plus `--config FILE`, `--brain URL`, `--region ID`,
  `--node-id ID`, `--name TEXT`, `--public-host HOST`, `--public-port PORT`,
  `--max-players N`, and `--gameserver.<key>=<value>` / `--set gameserver.<key>=<value>`
  for any key. `--set <other section>.<key>=...` is accepted and ignored (backend-up
  passes the same arguments to every service).

| key | default | meaning |
| --- | --- | --- |
| `region` | `local` | brain region id (v3.1.1 `MM_NODE_REGION`): the 2-letter code (`SV`, `LA`, `CH`, `VA`), the prefix of short game links; the server browser shows its place name |
| `node_id` | `LOC` | node id (v3.1.1 `MM_NODE_ID`), prefix of the lobby id `"<node_id>:<5 chars>"`; no `:` |
| `name` | `KrunkNative` | server name (hello, lobby meta `name`) |
| `listen_host` / `listen_port` (alias `port`) | `0.0.0.0` / `21015` | TCP bind address / port |
| `public_host` / `public_port` | `127.0.0.1` / `0` (= listen port) | where clients connect (lobby meta `host`/`port`) |
| `brain_http_url` | `""` (= `http://<brain_url host:port>`) | brain HTTP base for published maps (`GET /api/map-data?name=`) |
| `web_url` | `""` | optional ws(s) URL web clients use (net-bridge `/native?...`), lobby meta `webUrl` |
| `brain_url` | `""` | `ws://<brain>/mm/internal/ws`; empty = standalone; `"auto"` = this config's brain (`ws://<brain.host>:<brain.port>/mm/internal/ws`, `0.0.0.0` dialled as `127.0.0.1`, honours `--set brain.host/port=` and `KN_BRAIN_HOST/PORT`). `KN_GAMESERVER_BRAIN_URL_FALLBACK` (backend-up sets `auto`) applies only when no `brain_url` was configured anywhere |
| `brain_secret` | `kn-dev-secret` | Bearer secret (= `brain.internal_secret`, same dev default); never logged. Set a real one in deployment: with the dev default (or a blank one) server_bin refuses to link to a coordinator that is not on this machine and runs standalone |
| `join_secret` | `""` (= `brain_secret`) | HMAC key of seek-game join tickets (= `brain.join_secret`) |
| `require_ticket` | `false` | brain-linked: refuse connections without a ticket or session token |
| `allow_guests` | `true` | brain-linked: allow players without an account (ticket `acc` 0 / no JOIN) |
| `join_timeout` | `5` | seconds a brain-linked server waits for JOIN before treating the player as a guest |
| `tick_rate` | `30` | simulation / sync rate (Hz) |
| `max_players` | `8` | lobby size (v3.1.1 `globalConfig.maxPlayers`), 1-31 |
| `mode_max_players` | `{}` | per-mode lobby size, keys = mode alias/name/id (`{"tdm":10}`) |
| `team_size` | `0` | team modes: lobby size = 2 × team_size (0 = off) |
| `mode_team_size` | `{}` | per-mode team size; `mode_max_players` wins over team sizes |
| `maps` | `[]` (Krunker rotation) | rotation: map catalogue names, keys or indices (`shared/include/map_catalog.h`: `"Burg"`, `"311_burg"` (the v3.1.1 Burg), `"152_burg"`, `"13"`; the old keys `"v3_burg"` / `"old_burg"` still resolve), and published maps as `"published:<creator>/<name>"` (or `"published:<name>"`): fetched from the brain's `/api/map-data` (`brain_http_url`, else derived from `brain_url`), checked like v10 (prepareGame + validateMap with a creator's limits), cached under `$XDG_CACHE_HOME/krunknative/published-maps` and played from that cache when the brain cannot answer; an entry that cannot be had is skipped with a log line (`server/include/published_maps.h`). The command line's map argument takes the same spec; when it fails the server plays these maps instead |
| `modes` | `["ffa"]` | rotation modes (aliases/names/ids); `"rotation"` (also as server.cfg `modes rotation` or inside a list) adds Krunker's public rotation ffa, point, ctf, kc, dom. Rounds rotate over the (map, mode) pairs a map supports (its `config.modes` plus the mode's objects, `game_map_offers_mode`); with no end-screen votes a random card wins; a multi-mode list drops modes whose minPlayers / teams do not fit the lobby (doc/v7/decisions.md "Public mode rotation") |
| `custom_map_dirs` | `[]` | extra directories of map JSONs appended to the rotation (catalogue keys are skipped) |
| `round_seconds` | `0` | round length override for every mode (0 = the mode's game time) |
| `mode_round_seconds` | `{}` | per-mode round length (`{"ffa":300}`) |
| `game` | `{}` | v3.1.1 host config (`src/config.js serverConfig` varNs, clamped the same): `gameTime`, `minPlayers`, `warmupTime`, `forceSpawn`, `lives`, `scoreLimit`, `gravMlt`, `jumpMlt`, `deltaMlt`, `slidTime`, `impulseMlt`, `wallJP`, `strafeSpd`, `healthMlt`, `fiRat`, `reSpd`, `t1Dmg`, `t2Dmg`, `canSlide`, `airStrf`, `autoJump`, `bDrop`, `hpRegen`, `killRewards`, `headshotOnly`, `noSecondary`, `noStreaks`, `disableB`, `throwMel`, `selTeam`, `allowSpect`, `thirdPerson`, `nameTags`, `nameTeam1`, `nameTeam2`, `maxPlayers` (= `max_players` when that is not set) |
| `custom_games` | `true` | accept hosted custom games from the brain (hello `custom`) |
| `motd` | `[]` | server chat line(s) sent to every player on their first spawn (`$` = their name); none by default (the map's welMsg is the menu's line); `[]` or `""` sends none |
| `join_message` / `leave_message` | `"$ joined the game"` / `"$ left the game"` | v3.1.1 `config.serverMessages[0/1]`, `""` = off |
| `billboard` / `billboards` / `billboards_file` / `billboard_image` | none | billboard ads on every map BILLBOARD (below); `""` = none |
| `idle_kick_seconds` | `60` | seconds an alive player may do nothing before the kick (v3.1.1 `SERVER_IDLE_TIMEOUT_MS`), 0 = never. Activity is v7.1.4's (controls.js idleTimer: any key, any mouse move): an input frame that turns the view or presses / releases a key, a move of `idle_move_threshold`, the respawn click (ENT), chat, votes, sprays, interacts. The clock also restarts on connect, (re)spawn, death and every map change. Never kicked: spectators, the end screen, DEV peers (ops, DEV accounts, the server's own machine), the custom lobby's host and `noKick` modes |
| `idle_menu_kick_seconds` | `300` | the same for a dead player or one in the menu (before the spawn, after a death), 0 = never. v7.1.4 kicks from the menu on the client (config.kickTimer 120 s, public lobbies only); this is the server's backstop |
| `idle_move_threshold` | `1` | units a player must move to count as active (input frames that change something count anyway) |
| `perms` | `false` | v3.1.1 `PERMS`. KrunkNative: DEV chat commands are for DEV accounts, ops and loopback whatever `perms` says, unless `perms` is off **and** `cheats` is `"all"` (v3.1.1's open server) ([v7/command-auth.md](v7/command-auth.md)) |
| `ops` | `[]` | account ids (numbers) or account usernames that are op (DEV) when they join; names only match accounts, never guests |
| `bans_file` | `""` | JSON `{"ips":[...],"accounts":[ids],"names":[usernames]}`; loaded at start, `/banip` / `/unbanip` changes saved back (atomic rename) |
| `earn_kr` | `true` | KR rewards at match end (lobby meta `earnKR`) |
| `kr_cap` / `kr_cap_premium` | `30` / `40` | per-match KR cap (v3.1.1 `min(premiumT ? 40 : 30, ...)`) |
| `kr_multiplier` | `1` | multiplies the mode's `rewardMlt` |

v3.1.1's `main/config.js` + `main/defaults.json` are the Electron client's settings
(window, keybinds, watermark) and have nothing a game server uses; everything the
v3.1.1 server made configurable through env (`PORT`, `HOST`, `PERMS`, `MM_*`,
`PUBLIC_*`, `LOBBY_COUNT` = one process per lobby here, `billboards.json`) is covered
above.

**Billboards.** v3.1.1 ships `billboards.json` (`[{txt, txtCol, bgCol}]`) and the
server rotates one entry per map cycle (`gameserver.ts pickBillboard`) into the
`init` packet's `bill` slot; the client's `game.updateBillboards` painted it on the
map's `BILLBOARD` objects with `sad` ("show ad"). v7.1.4 dropped both (its billboards
only show the six `pubs/b_<bb>` posters; `utils.js createCanvasAD` is still shipped).
Here (doc/v7/decisions.md "Billboard ads"): `"billboard": "text"` or
`{txt,txtCol,bgCol,image}`, `"billboards": [...]` (strings or objects) and/or
`"billboards_file": "<v3.1.1 billboards.json>"`, plus `"billboard_image": "<asset path>"`
(the image of every entry without one, or an image-only billboard); one entry per round,
in order. `""` / an entry with neither text nor image is no billboard.

While a billboard is set, **every** `BILLBOARD` object of every map (v7 posters, random
or fixed `bb`, v3 `sad` ones, mimicSprite ones) shows it: the image when it loads,
else the text drawn like `createCanvasAD` (2048 x 1024, `bgCol` fill, GameFont at the
largest size that fits with an 80 px margin, `txtCol` with a `rgba(0,0,0,0.5)` outline,
`client/src/billboard_ad.c`). Lighting, emissive and opacity of the object stay; its
`col` tint, frames and scrolling do not. SIGN, LEADERBOARD and textured planes / sky
domes are map content and are never replaced. The server sends `NET_PACKET_BILLBOARD`
(text) and `NET_PACKET_BILLBOARD_IMAGE` (image) before every INIT, so late joiners and
map changes get it; an INIT without them (no billboard, another server, offline) gives
the maps their own posters back. `billboard_image` is a path under `assets/`
(`textures/pubs/b_3.png`, `user-assets/<id>/texture.png`; `.png` / `.jpg`, no `..`,
no URL: the native client only fetches from the asset host). Verify with
`make billboard-e2e` and `billboard_ad_test` (`make test`).

### 8.2 Native protocol additions (`shared/include/shared.h`)

| packet | dir | struct | when |
| --- | --- | --- | --- |
| `NET_PACKET_BILLBOARD` (33) | S→C | `{char txt[64]; char txt_col[8]; char bg_col[8];}` | before every INIT, when the billboard has text |
| `NET_PACKET_BILLBOARD_IMAGE` (36) | S→C | `{char path[128];}` (asset path under `assets/`) | before every INIT, after BILLBOARD, when the billboard has an image (older clients ignore it) |
| `NET_PACKET_PLAYER_INFO` (34) | S→C | `{int32 uid, account_id, level; uint8 flags; char clan[8];}` flags: 1 premium, 2 verified/featured, 4 dev/op, 8 mod | brain-linked servers: after every SPAWN broadcast, and every player's to a joiner / spectator |
| `NET_PACKET_JOIN` (72) | C→S | `{char game_id[48], client_key[352], session[64], name[32];}` (496 bytes; the 464-byte V1 without `name` is accepted) | right after HELLO, before the first ENT |

Client use: nametag level box + clan + premium/featured badges, scoreboard badges +
`[clan]`, kill card `lvl N`. JOIN: `client_key` = the seek-game `clientId` (JWS
ticket), `session` = the social session token, `name` reserved (guests stay
`Guest_N`, as in v3.1.1).

### 8.3 Join flow (brain-linked)

1. Connect → `join_state = AWAIT`; ENTs are deferred until the identity is settled.
2. JOIN with `client_key`: verified locally (`kn_signed_token_verify`, HS256,
   `join_secret`; `exp`, `nid == node_id`, `lid == lobby id`). `acc > 0`: the ticket's
   account id and `name` are authoritative; `auth {ticket}` asks the brain for level,
   clan, flags and bans. `acc == 0`: guest (or `session` below).
3. JOIN with only `session`: `auth {token}` (svc-accounts session lookup).
4. No JOIN within `join_timeout`, or an empty one: guest, unless `require_ticket` /
   `!allow_guests` (then `NET_PACKET_ERROR` and close).
5. `auth-reply ok`: name = account alias (else username), level, clan, flags; banned
   accounts (`banned`, svc-accounts error `"Banned: ..."`, or `bans_file`) are refused
   (`ERROR` code `004003`); another connection of the same account is kicked
   ("Logged in from another location"); ids/usernames in `ops` become op.
   `auth-reply` not ok: `ERROR {err}` and close. No reply in 10 s or brain link lost:
   ticket holders keep the ticket identity, session-only joins become guests.
6. `player-join {lobbyId, clientId: jti, accountId}` releases the ticket's slot
   reservation; `player-leave` on disconnect.
7. One seat per ticket (mm-token): a second connection with the same ticket, fresh or resumed,
   replaces the first one (`004002`), so a seek's ticket never seats two players.
8. Resume (Krunker Box Office): an expired ticket whose `jti` this lobby admitted (READY) is accepted
   again until 30 min after that session's last connection ended (`accounts.c`
   `acc_resume_allowed`, at most 256 remembered): the transport reconnects of 5.3 used to end in
   `"Join key expired"` a few minutes into a match (the edges' logs: kn-rtc sessions failing,
   coordinator / gateway restarts). The new connection replaces the old one (`004002` to the
   old one); an account's `auth` carries `resume: true`. A ticket never admitted, or for another
   lobby, is refused as before.
9. Live sign-in (Krunker Box Office): signing in, switching accounts or signing out in the menu
   mid-session keeps the connection. Once its join is settled the server pushes
   `NET_EXT_KN_ACCOUNT_RIGHTS` (`shared/include/net_ext.h`: account, rights, the kinds it verifies);
   the client then sends `NET_EXT_KN_ACCOUNT_AUTH` whenever the menu's identity differs from the
   server's: the session token (`auth {token}` as in 3, brain-handed lobbies only, like
   `JOIN.session`), a fresh `"hj1:"` after `/auth/join` (hasJoined as in online_mode joins), or a
   guest. Nothing changes until it verifies; then 5 applies (a banned account is kicked with
   `004003`, another connection of the account with `004002`, `ops` follow the account), the old
   account's partial stats are committed, `player-leave` / `player-join {accountId}` move the
   presence, the new account's stats count from the next round, cheats the lost DEV / host rights
   allowed are turned off, and the answer carries the new rights (the Object Manager asks its caps
   again). A forged or expired token, a timeout (10 s) or a lost link change nothing (`REFUSED` /
   `UNAVAILABLE`); 6 verifications per connection a minute, one at a time (`BUSY`); a ranked
   match refuses switches and sign-outs; every change is logged as `[rights]`. A server without the push keeps
   the old behaviour: the client rejoins its lobby when the account changes in the menu
   (`servers_ui.c`). Test: `make live-signin-e2e`.

Error codes (`shared.h` `NET_ERROR_CODE_*`): `004001` invalid/expired ticket or session, guests
refused; `004002` logged in elsewhere; `004003` banned; `004004` server draining or stopping for
an update (`"GAME UPDATING"`); `004006` server stopped (8.5). The client rejoins through the
matchmaker on `004004` / `004006` and on a refused join key (9.4); `004004` on a live session is
"restarting for an update, reconnect after": with Settings > Client > "Auto-restart for updates"
(default on) the client restarts itself first (9.4, `client/include/game_reload.h`).

### 8.4 Lobby state, stats and rewards

* `hello` (section 5) on every (re)connect, `snapshot` every 5 s and right after
  `welcome`, `lobby-up` 250 ms after joins/leaves/map changes, `lobby-down` on
  shutdown / after a drained round. Tuple slot 0 is the local lobby id; meta adds
  `name` to the section 4 keys. Reconnect backoff 1 s → 30 s.
* Lobby size and round length are re-evaluated for every round from `max_players`,
  `mode_max_players`, `team_size`, `round_seconds`, `mode_round_seconds` (hosted
  games: the host config's `maxPlayers`).
* Stats (`commit`, reliable: kept and resent across brain reconnects until its
  `commit-reply`): at round end (`final: true`, `games: 1`, `won` from the END
  packet's didWin) for every account that played the round, and on mid-round
  disconnect / map change / shutdown (`final: false`, `games: 0`, `kr: 0`,
  `matchId` = `<round matchId>#c<connection id>` so svc-accounts' `(match, id)`
  idempotency never swallows a rejoin). Per player: `accountId, name, kills, deaths,
  score, won, timePlayed` (ms since the player's first spawn of the round), `games,
  kr, xp` (= score), `classIndex`, and (WP-E part 2c, GameHooks.progress) `stats` (v7 incStat
  counts), `prog` (challenge tallies per key, weapon and streak, plus `w` / `gp` at the round
  end), `medals` (player.medals), `mapIndex` (v7 official map index, -1 custom) and `ranked`. Like v3.1.1 `updateAccounts`, hosted (custom)
  games and `noReward` modes only add time played, and hack-flagged accounts get
  nothing. Guests are never sent.
* KR: `min(premium ? kr_cap_premium : kr_cap, floor(score × mode.rewardMlt ×
  kr_multiplier / 100))` (v3.1.1 `game.js endGame`), 0 when `earn_kr` is off, in
  hosted games or `noReward` modes. svc-accounts clamps it to `accounts.max_match_kr`.
  `commit-reply` level/funds updates are re-broadcast as PLAYER_INFO (level ups).

### 8.5 Hosted custom games, drain, shutdown

* `host {rid, config, hostAccountId, hostName}` is accepted when `custom_games` is on,
  the server is not draining, not hosting already and has no players: `config.maps`
  (catalogue indices or names), `config.modes` (hostable mode ids/aliases) and the
  serverConfig varNs replace the rotation, the lobby restarts (`cs: true`, `i:
  "<mode>_<map>"`, `earnKR: false`) and `host-reply {ok, lobbyId}` follows. A guest can host
  (`hostGuestId`). When the host leaves with two or more players left, the oldest signed-in or
  guest connection takes over (`acc_transfer_host`); with fewer ("Host ended game" chat line,
  v3.1.1 `HOST_ENDED_MESSAGE`), when the lobby empties, or when nobody joins within 60 s, it
  reverts to the public rotation. `passCode` is not enforced (the native JOIN has no password).
* **Host rights** (checked against v10.0.3, `server/src/votekick.c votekick_handle_admin`,
  `server/src/accounts.c`):
  * Kick (`NET_EXT_ADMIN_ACTION` action 0) is the custom game's host's and a moderator's (v10.0.3
    shows it for `isHost || canGlobalKick`, deob-10.0.3.js:254169); the kicked player reads "Kicked
    by host" (`game.kick`, 222586-222588); a DEV cannot be kicked that way. Custom games have no vote
    kick (v10's Votekick needs `!game.isCustom`, 250341). A DEV kicks and bans through the chat
    command layer.
  * Transfer Host (action 8, the player list's button for the host, 254169, sent with the target's id,
    254059-254071; forwarded on official games too, `krunker_link.c`): the host hands the game to a
    signed-in or guest player here (`accounts_transfer_host_to`).
  * Transfer Host on Leave (`transferHostOnLeave`, Hosting, default on, 19992-19998; `GameConfig
    .transfer_host_on_leave`): on, a host leaving with two or more players left passes the game to the
    oldest signed-in or guest player; off (the rule before the setting, v3.1.1 `HOST_ENDED_MESSAGE`),
    or with fewer players, "Host ended game" and the lobby goes back to the public rotation.
  * Every transfer says v10.0.3's lines, "You are now Host!" to the new host and "Host Transfered to
    {0}" to everyone else (256720-256727); a game's host passes at most `hostTransferLimit` 5 times
    (22620, `game.hostTransfers` 215732): past it everyone reads "This server exceeded Host Transfer
    limit", a manual transfer is refused (the host stays) and a leaving host leaves the game without
    one (inferred: the server code is not in the bundle). The host also feeds v10 `game.host`
    (`acc_sync_game_host`, `game_config_set_host`).
  * Ban (action 1) stays a DEV's, a deliberate difference: a KrunkNative ban is a persistent account
    ban across the server, while v10.0.3's host / moderator ban (`isHost || canGlobalBan`) is that
    game's only, a list KrunkNative does not keep; the host's and the moderator's Ban is refused
    rather than turned into a server ban.
  * Neither game lets an ordinary host change the settings mid-game (v10's Rehost is staff-only,
    129063, 248793-248797).
  * Tests: `server/tests/votekick_test.c` (section 9, through the test hooks
    `accounts_host_for_test` / `accounts_tick_custom_for_test`: no brain).
* **Published (community DB) maps:** `config.customMap` (alias `mapName`) names the map to
  host. A map this server has (its own list incl. custom dirs / the command-line map, then
  a catalogue entry whose name or key is exactly that) is used directly; otherwise
  server_bin fetches `GET <brain_http_url>/api/map-data?name=<customMap>&meta=1` (svc-accounts;
  worker thread, 10 s timeouts, <= 4 MiB, sanitised like v3.1.1 `sanitizeCustomMapJson`)
  and only then answers `host-reply` (404 -> `{"ok":false,"err":"map not found"}`).
  Clients that lack it get it through MAP_INFO / MAP_REQUEST / MAP_DATA as usual; the
  lobby meta is `i: "<mode>_<map>"`, `m: <map>`, `cs: true`.
* `drain` from the brain: chat notice, refuse new connections (`GAME UPDATING`),
  finish the round, then `lobby-down`.
* SIGTERM / `quit`: partial commits for everyone in the round, `drain` + `lobby-down`,
  then every player gets `ERROR` (v7.1.4 `restartForUpdate`: `["error", "GAME UPDATING"]`, the
  socket closed 100 ms later) and the sockets close 150 ms after it; up to 3 s for outstanding
  `commit-reply`s before the brain socket closes. The text follows the stop reason
  (`server_shutdown_message`): `update` -> `"GAME UPDATING"` (`004004`), `restart` ->
  `"Server restarting"`, other text -> `"Server shutting down: <text>"`, none -> `"Server
  shutting down"` (all `004006`); the drain's chat notice says the same ("Server is restarting
  for an update after this round"). The reason comes from console `quit <reason>` / `drain
  <reason>` (also over rcon), else from the first line of `gameserver.shutdown_reason_file`, read
  at the first stop request and ignored when older than 15 min: deploys write `update` there
  around the restart (section 10).

### 8.6 End-screen reward, ranked lobbies, tests

* **Reward column.** `game_end_round` sets `player.reward` through `GameHooks.reward`
  (server: `accounts_reward`, the 8.4 KR rule; NULL = 0 offline / for guests) and the END
  packet carries `int32 rewards[16]` parallel to `board.entries` (`NET_END_PACKET_V1_SIZE`
  = an END without it, still accepted). The client shows it like v3.1.1's
  `config.endForm.reward`: `<span style='color:#F8C55C'>+N</span> KR`, `NONE` for 0, in
  the end table (`hud_modes.c`, every mode's `endStats`) and as the Performance tab's
  funds; the stats commit's `kr` is the same `player.reward`.
* **Ranked** (brain `host` with `config.ranked`, section 6.5). The lobby is v7's comp lobby:
  `config.hostTypes[1]` "Comp Server Setup" (`setupGameConfig`: overDat Hardpoint, `fixMov`,
  `jumpMlt` 1.005, `slidSpd` 0.97, `warmupTime` 0.2, `selTeam`, `throwMel` off, `isComp`; defC
  `tstCmp` 1 (the comp menu), `limitClasses` 1 with `classLim<i>` 1 for config.rankedClasses)
  with `tmSize` = team size - 1, `specSlots` 0, `allowSpect` off and the queue's map, re-applied
  every round. Comp lobby rules (`shared/src/comp_lobby.c`, v7 game.js `lobbyPlayers`): every
  connection has an entry (`NET_PACKET_COMP_LOBBY` = v7 "cd" / "cda": account id, name, level,
  verified, team `t`, class `c`), `NET_PACKET_COMP_SET` = `updSyncDat("t"|"c", v)` (team: selTeam,
  tmSize + 1 per team; class: classLim per team), a player spawns only with a team and a class
  (canSpawn: "Missing Sync Data", "Please join a Team to Spawn", "Please select a Class to
  Spawn") and spawns on them; ranked fixes each roster player's `t` to its queue team (`teams[0]`
  → 1, `teams[1]` → 2) and refuses team picks ("Can't switch while in-game"). v7's server code is
  not in the reconstruction, so the rest are v3.1.1's `isFromQueue` rules: only the roster's
  accounts may join (others: `ERROR "Game in Progress"`), each gets "Joined Ranked Match.
  Abandoning a match will result in a temporary ban" once, spectating is refused ("Spectating
  Disabled in Ranked"), the lobby is `cs`/private with `meta.ranked` + `matchId`, `minPlayers` =
  roster size so the countdown waits until everyone is in (`needAll`); after `startGraceS`
  without everyone it starts with whoever is there when both teams have someone, else ends. A
  roster player who leaves before the end screen ends the round (`leaveGame` → `endGame(3)`) and
  is an abandoner (`checkLeave`), as are no-shows; the team that stayed wins, else the winner is
  the END's didWin (Hardpoint's comp mercy rule ends a match the trailing team can no longer
  win). One round (comp `endTimer` "inf"): `ranked-result {matchId, lobbyId, winner,
  players[{accountId, team, score, kills, deaths}], abandoned}` goes out once (reliable), the
  reply's rating changes are logged and sent to each player as a chat line, and the lobby
  returns to the public rotation at the end of the end screen. The normal `commit` is sent too;
  its `ranked` (checkChallenge's `T == "ranked"`) is `game.isComp`.
* **Tests** (real hits, real DB, headless clients; each creates and drops its own
  database): `server/tests/gunfight.sh [build]` — two accounts on the open
  `server/tests/arena.json`, `KRUNKNATIVE_TEST_AIM` + an input script (trigger held,
  strafes, `respawn` after every death) → checks kills/deaths/score/`kr` in
  `match_results`, `players`, and `player_funds` = sum of rewards; `KN_TEST_SHOT=file`
  captures the end screen with the Reward column. `server/tests/ranked_e2e.py BUILD_DIR` —
  8 accounts queue over `/mm/queue/*` (a party of two + solos), the brain hosts a 4v4 comp
  lobby on server_bin, 8 protocol test clients (`kn_proto`, ports from `free_ports()`) join
  with their tickets: Hardpoint, the roster teams in the comp lobby table, a stranger refused,
  the class / team / spectate rules, then one player quits: the other team gains 16,
  `ranked_ratings` / `players.player_elo`, the abandoner's cooldown.

Not ported: the game-socket account ops of v3.1.1 (`io_a` login/register over the
game socket: the native client logs in over the social WS), `host-request` from a
player inside a lobby (the native protocol has no `custom` packet yet), ELO /
challenges / class score stats (svc-accounts `elo` / `challenge` ops exist).

### 8.7 Hosting a server (dedicated server exe)

Hosting is running `server_bin`, like a CS:Source dedicated server. The client's Host Game
window (section 8.11) can also start a game on Box Office servers or krunker.io; it never runs one itself.
One process is one lobby; a network is many processes (8.10).

```sh
make server-dist            # dist/server/: server_bin + assets/maps + cfg/server.cfg + kn-query.py
cd dist/server && ./server_bin          # reads ./server.cfg, else ./cfg/server.cfg
make server-install PREFIX=/opt/krunknative   # + deploy/systemd/kn-gameserver@.service
```

**server.cfg** (`config/server.cfg.example`): one `key value` per line, `//` or `#`
comments (at line start or after whitespace, so `ws://` values survive), values
optionally `"quoted"`. Every `gameserver` key of 8.1 works, plus the classic names
`hostname`, `sv_password`, `maxplayers`, `mapcycle`/`map` (comma list), `mode`
(comma list), `public_ip`, `rcon_ip`, `coordinator_url` / `coordinator_secret` /
`coordinator_http_url` (= `brain_url` / `brain_secret` / `brain_http_url`), `sv_tags`,
`tickrate`, `logfile`, `sv_lan 1` (unlisted + offline mode), `game.<var> value` (host
config vars). Lookup: `--cfg FILE` (must exist; `none` skips), else `$KN_SERVER_CFG`,
else `./server.cfg`, else `./cfg/server.cfg`. Precedence: defaults < config.json
`"gameserver"` < server.cfg < `KN_GAMESERVER_*` env < CLI.

New keys (also in config.example.json):

| key | default | meaning |
| --- | --- | --- |
| `password` (`sv_password`) | `""` | JOIN.password must match; wrong/missing -> `ERROR 004005` |
| `rcon_password` / `rcon_port` / `rcon_host` | `""` / `0` / `127.0.0.1` | local TCP rcon (off unless both set; off loopback the password needs 12+ characters) |
| `query_port` | `0` (= game port) | UDP server-info query (8.8) |
| `online_mode` | `false` | verify signed-in players with central auth `hasJoined` (8.9); guests still join, unsaved |
| `auth_url` | `https://coordinator.krunk.io` | central auth base (`/auth/hasJoined`) |
| `finder_url` | `""` | opt-in public listing heartbeat (8.8), e.g. `https://matchmaker.krunk.io` |
| `community` | `""` | community name in lists (`""` = the coordinator's welcome `community`, else its brainId) |
| `tags` | `[]` | extra info-query tags |
| `enforce_skins` | `auto` | cosmetics must be owned (8.9); auto = on while linked to a coordinator |
| `shutdown_grace` | `10` | seconds a SIGTERM/SIGINT drain lets the round finish |
| `shutdown_reason_file` | `""` | read at the first stop request: `update` = players see `GAME UPDATING` (8.5); deploys write it |
| `lag_comp` | `true` | hits test targets where the shooter saw them (v3.1.1 storeState / fetchState, 500 ms history); console `lagcomp` |
| `lag_comp_anchor` | `1` | `1` exact: the shot input's client time (the running sum of its timeline's frame deltas, no packet change) mapped onto the server clock by a min-filtered per-connection offset, + 1 ping; a delayed packet (jitter, a lag spike) costs nothing. Falls back to `2` while a timeline calibrates (0.5 s after a spawn / seq gap / hitch) and for the rest of a life whose summed deltas run faster or slower than real time (logged `[lagcomp]`); a shot is credited at most 250 ms before its arrival and never after it. `2`: the shot input's arrival + 1 ping. `0`: v3.1.1 `ping * 2 * lagComp` from processing. `true`/`false` read as `1`/`0`. Console: `lagcomp anchor exact\|arrival\|v3` |
| `lag_comp_scale` | `1` | 0.1..1, multiplies each player's v3.1.1 `player.lagComp` (the Settings > Network "Lag Compensation" slider, ENT V7, server-clamped to 0.1..1): scales the ping part of the rewind |
| `log_file` | `""` | also append every console line (timestamped) here |

CLI additions: `--cfg FILE`, `--hostname`, `--password`, `--online-mode` /
`--offline-mode`, `--log FILE`, `--version`, and the rcon client
`server_bin --rcon [HOST:]PORT --rcon-password PW [command ...]` (no command = one per
stdin line; `KN_RCON_PASSWORD`). `server_bin --help` shows a sample server.cfg.

**Console** (stdin; replxx with history/completion on a terminal, plain lines from a pipe
or FIFO, EOF only ends the reader): `status` (hostname, map, mode, players, tick rate,
auth mode, coordinator link, listing), `players`, `kick`, `ban` (account ban into
`bans_file`, guests by IP), `map`/`changelevel`, `mode`, `say`, `round`, `op`/`deop`,
`send` (8.10), `drain [reason]`, `quit [reason]` (`update`, `restart` or any text: what the
players are told, 8.5), `help`, and every Krunker chat command without the slash.

**rcon**: the same commands, their output captured per command
(`cli_run_command`). Local TCP: line 1 = password (`OK` / `ERR bad password`, 1 s delay),
then one command per line, output lines, then a line `.`; bound to 127.0.0.1 unless
`rcon_host` says otherwise (no TLS: tunnel it; off loopback rcon refuses passwords under 12
characters). The password is compared in constant time; at most 4 connections wait for their
password at once (`ERR busy`) and 5 bad passwords within 60 s lock rcon for 60 s (`ERR locked`),
both logged. Through the coordinator: internal-WS
frame `{"type":"rcon","rid","command"}` -> `{"type":"rcon-reply","rid","ok","output"}`
(the coordinator authenticates its admin; no brain route sends it yet).

**Shutdown**: SIGINT/SIGTERM/SIGHUP/`drain` = drain: refuse new connections
(`GAME UPDATING`), `drain` to the coordinator (stops offering the lobby), chat notice,
wait until the round ends or nobody is left (at most `shutdown_grace`), then the 8.5
shutdown (partial commits, lobby-down, final finder heartbeat). A second signal or
`quit` stops at once. Ctrl+C at the interactive prompt still quits immediately.

### 8.8 Server-info query and finder heartbeat

Transport-independent, A2S-style; wire format and encode/decode in
`shared/include/server_info.h` / `shared/src/server_info.c` (pure byte buffers), socket
glue in `server/src/info_query.c` (`server_info_handle_datagram()`; the UDP game socket
demuxes the `FF FF FF FF` prefix, which no game datagram starts with).

* request: `FF FF FF FF "KNINFO" [00 <nonce <= 32 printable bytes>]`
* reply: `FF FF FF FF "KNinfo" 00 <JSON <= 1200 bytes>`:
  `{"v":1,"game":"krunknative","name","map","mode","modeId","players","max","bots",
  "tags":[...],"community","online_mode","password","region","version","port","nonce",
  "commit","commit_time","build_time","protocol"}` (`version` = the build's 12-hex commit, `unknown` without one; 8.12).
  Tags: `password`, `ranked`, `custom`, `draining`, `community` (linked to a
  coordinator), then `tags`. `online_mode` is legacy ("guests refused"): always `false`
  now, every server admits guests (older clients hide or label `true` servers). Snapshot refreshed 4x a second by the tick.
* `services/scripts/kn-query.py HOST[:PORT] ...` prints it.
* Finder (opt-in, `finder_url`): `POST <finder_url>/servers/heartbeat`
  `{"v":1,"host","port","query_port","name","community","online_mode","version","commit",
  "commit_time","build_time","protocol"[,"shutdown":true]}` (the build fields are informational:
  the listing uses the verified query reply)
  every `interval` s from the reply `{"ok":true,"interval":N}` (default 60), plus one with
  `shutdown` on exit. `host` is `""` (= the request's source IP) while `public_host` is
  loopback. The finder lists only what it verified by polling the query. HTTPS needs
  libcurl in the build (linked when found; else http:// only).

### 8.9 Accounts on a dedicated server: online mode and skins

* **Standalone, offline mode** (default): guests (`Guest_N`), as always; `password` still
  applies (a JOIN with the password, then the ENT).
* **Online mode** (`online_mode 1`, Mojang-style, contract with the auth role): after
  HELLO the server sends `NET_PACKET_AUTH_CHALLENGE` `{server_nonce (per boot),
  online_mode 1, public_key "", auth_url}`. A signed-in client posts `/auth/join {token,
  serverHash}` to central and JOINs with `client_key = "hj1:<client nonce>"`, `name` =
  username; a client without an account answers `client_key = "hj0"`
  (`AUTH_JOIN_GUEST_KEY`) and plays as a guest. A plain JOIN (no `hj1:`/`hj0`, no
  ticket or session) waits for that answer: the client's first JOIN goes out before it
  saw the challenge; a client that never answers (builds before `hj0`) is a guest at
  `join_timeout`. **Guests are welcome on every server**, as on every Krunker lobby
  (v10 `/ws?` sends `at=<token>` only when there is one, v10.js:222125; unnamed players
  are `Guest_<sid>`, game.leak players.js:1492): accounts are verified and their
  progress saved, guests play unsaved. Ranked alone needs an account (the ranked queue
  takes a session token; the ranked lobby admits its roster only, 6.5). `serverHash = hex(SHA-256("kn-join-v1" 0 server_nonce 0 public_key 0
  client_nonce))` (`shared/include/auth_join.h`). Without a coordinator server_bin calls
  `GET <auth_url>/auth/hasJoined?username=&serverHash=&ip=` itself on a worker thread
  (`server/src/central_auth.c`): 200 -> that account id/profile (stats are session-only:
  nothing is committed without a coordinator); 204 (a forged, replayed or unannounced
  join), a malformed `hj1:`, an unreachable auth service or a timeout -> a **guest**
  (`Guest_N`, never the claimed name, nothing saved), as Krunker's client connects as a
  guest when its token does not refresh (v10.js:27589). Banned accounts are refused.
* **Linked to a coordinator**: joins as 8.3 (seek-game ticket and/or session token). In
  online mode the `hj1:` JOIN goes to the coordinator as `{"type":"auth","rid",
  "hasJoined":{"username","serverHash","ip"}}`; it verifies with central, creates the
  community account on first join and answers the normal `auth-reply`. A failed
  `hasJoined` or an invalid session token is a guest too (a `Banned` reply is refused);
  an invalid join ticket is refused (`Invalid join key`, a seat reservation, not an
  account claim).
* **Skins**: the `auth-reply` account's `data[10]` (`Account.getData` skins,
  `[{ind,cnt}]` from `inventory`) is loaded into a per-connection bitset
  (`ServerAccount.skins_owned`, 4096 store indices). With `enforce_skins` on (auto: linked)
  every ENT's hat / back / waist / face / dye that the account does not own is reset to
  none before the spawn (the SPAWN all players see carries the filtered look) and the
  player gets a chat notice; guests own nothing. Skin colour / hair colour are free.
  Skins come from the coordinator's DB (`give_skin` op, store spins, market); a skin only
  has value inside its community network.
* Verified: `server/tests/dedicated_e2e.py BUILD` (own DB; coordinator + 2 servers: info
  query, ticket + session joins, `kn_add_skin` -> owned hat kept / unowned stripped /
  another account's hat stripped, password, rcon status/say/players/kick/changelevel,
  `send` redirect, SIGTERM drain + partial commit in `match_results`) and
  `server/tests/online_mode_e2e.py BUILD` (auth role, a standalone online server and one
  linked to a coordinator: challenge, `/auth/join` + `hj1:` join, the `hj0` guest answer
  and a silent old-client guest admitted as `Guest_N`, a forged `hj1:` and a forged
  session token admitted as guests, never the account; on the linked one a guest and an
  account play the same round: the account's `match_results` / KR are saved, the
  guest's are not; no `online` tag / `online_mode` in the info query).

### 8.10 A network of servers

Many small servers (Hypixel style): N `server_bin` in one or more regions, each linked to
a coordinator of its region; coordinators share one database and federate through it
(section 6), so every coordinator's game list, quick join and seek-game cover every
server. Players and their skins/stats follow them because they live in that shared DB.

```sh
make network NETWORK=services/scripts/network.example.json
services/scripts/kn-network status services/scripts/network.example.json
```

`services/scripts/kn-network` (Python, foreground) migrates the DB, starts each
`coordinators[]` entry (`kn_brain --set brain.port/region/id/community`), writes one
server.cfg per server of each `servers[]` group (`count` processes on `port`, `port+1`,
..., node ids `<node_id><i>`, `coordinator_url` of the group's coordinator; any other
group key goes into the server.cfg), waits for them to answer queries, and on Ctrl+C
drains the servers (stats commit) before stopping the coordinators. `backend-up.sh` /
`make backend-up` (one brain + `GAMESERVERS` servers via `KN_GAMESERVER_INDEX`) still work.

**Moving players between servers**: console/rcon `send <id|name|*> <target> [reason]`
sends `NET_PACKET_REDIRECT {game_id[48], host[128], port, reason[64]}`. A coordinator
lobby id (`NODE:lobby`) makes the client seek it through its matchmaker
(`/mm/seek-game?game=`: a new ticket and seat reservation, same account), a `host:port`
is a direct connect (a guest, or a signed-in player verified by `hasJoined` again). The client
(`servers_ui_redirect`, `game_events.c`) shows `reason` as a toast and switches; the
web build reloads with `?game=`. Lobby-to-lobby moves therefore always go through a
fresh seek-game ticket; there is no server-to-server state hand-off (the stats of the
old server are committed as a partial on disconnect).

### 8.11 Hosting from the client: the Host Game window

The Host Game window (`client/src/ui/host_flow_ui.c`, MENU_WINDOW_HOST = windows[8]) is v10's own
host_games window: Select, Game Settings, Advanced and Server tabs, then the last step, Start Game, which
is the choice of where the game runs. The choice has up to three paths; the first one shown depends on
the active backend (`krunker_net_active()`: the official krunker.io connection):

- **krunker.io** (while the official krunker.io connection is active): krunker.io's own hosting. The
  request is v10's createPrivateRoom object (`krunker_net_host_custom`, `client/src/net/krunker_net.c`):
  the `custom` packet over the official socket, answered by `cust` (v10.js:198129, :197942). The game
  joins on the reply. A community map is not sent here (krunker.io does not know it).
- **Box Office servers** (otherwise): the brain would host the game. The same object goes to
  `GET /mm/host-game?region=R&config=JSON` (`servers_ui_host_game`, section 8.5 of this document): the
  brain finds a server with capacity, and the client joins. A community map is its `customMap`. The client
  call is off by default (`servers_ui_host_game` returns 0 unless `KRUNKNATIVE_ALLOW_CLIENT_HOST=1`, the
  policy of sections 6.3 and 8.7), so the button reports that Box Office is unavailable and points to the
  dedicated server.
- **Host it yourself** (always): the server_bin command for the settings (`shared/src/host_command.c`), the
  Dedicated Server window (section 8.7), and Play this map offline (KrunkNative only: the embedded offline
  game on the chosen map, the catalogue map or the published one, checked like v10).

The Select tab's checkboxes are Krunker's own maps (catalogue `hostable`). A community map is chosen in the
Games window (v10's Browse Community Games banner, the Games window's Host button) and replaces them.
Raw game data, Social (share on hub), Server Upgrades, Host Presets and Rehost are not in the window: the
server cannot take the first, and the rest need accounts and features KrunkNative does not have. The menu's
Host Game button is shown; the chooser is v10's.

### 8.12 Build version and outdated servers

A build is named by its commit, never by a version number: KrunkNative has none of its own, and
players see only Krunker's version (the newest update of `ui/windows/kn-versions.txt`,
`v10_news_version`). Every binary embeds its build identity (`shared/include/build_info.h`,
generated `kn_version.h` from `cmake/kn_version.cmake` at every build): 12-hex commit, its
committer date, the build time and the protocol `NET_PROTOCOL_MAJOR.MINOR`, plus a `describe`
that is internal (local builds: git describe; CI: the commit, `tools/release/version-meta.sh`).
The wire and logs carry the commit as `kn_build_commit_id()` (`unknown` without one): the KNINFO
`version` and finder heartbeat `version` (8.8), the account meta and `hello` `build`, and the
kn-build `describe` the release notes carry. Shown by `server_bin --version` / `client_bin --version`
/ `kn_brain --version`, the server's startup log and `status`, the KNINFO reply and finder
heartbeat (8.8), the coordinator `hello` (5.2) and every lobby tuple's meta (`proto`, `build`,
`commit`, `commitTime`). The INIT packet is unchanged (fixed size; a tail would need a protocol
bump). Readers compare builds by commit: an older server that still names its build by a git
describe is matched by its commit (rule 4 and the commit rules below).

**Latest release** (`services/brain/releases.c`, rule in `release_info.c`, module `releases`,
every role). kn_brain polls `servers.releases_url` (default
`https://api.github.com/repos/rebel10n/KrunkNative/releases?per_page=30`, no token) every
`servers.latest_poll_s` (600; 0 = off) on a worker thread (libcurl), with `If-None-Match`
(a 304 keeps the cache) and backoff to `X-RateLimit-Reset` on 403/429 or a spent limit. The
latest server build is the `server-latest` / `server-v*` release (not drafts) with the newest
`commit_time` in the body line `<!-- kn-build {"describe","commit","commit_time","build_time",
"protocol"} -->` (`.github/workflows/release.yml`). Overrides: `servers.latest_version`
(describe or commit; no poll then) and `servers.download_url` (the page shown to players).

**Outdated** (`release_server_outdated`): a server is outdated when
1. its protocol major.minor is lower than this coordinator's own, or
2. the latest release is known, the server's commit is not the latest commit, and its commit
   is older than the latest commit by more than `servers.outdated_grace_h` (48 h; dirty builds
   compare by commit time like any other), or
3. the latest release is known and the server reports no build info (an older server_bin), or
4. `servers.latest_version` is set and neither the server's build (`version`/`build`) nor its commit match it.

**Incompatible**: the server announces another protocol MAJOR than the client (`?proto=M.m`;
clients without it count as the coordinator's major). Minors interoperate.

Exposed per entry as `outdated` / `incompatible` (finder `GET /servers`, `meta` of `/mm/game-list`
tuples) plus top-level `latest` `{version, commit, commit_time, build_time, protocol, tag}`,
`download_url` (release page, default `https://github.com/rebel10n/KrunkNative/releases/tag/server-latest`)
and `downloads` `{linux, linux_polyfilled, windows, macos}` (asset URLs). Matchmaking
(`/mm/seek-game`, the client sends `proto`): never a lobby of another protocol major (requested by
id: `IncompatibleVersion` 409 with `autoChangeGame=false`, else moved); up-to-date lobbies with
room win over outdated ones, which are still used when nothing else has room. The client's
Servers window shows an Outdated / Incompatible badge and a "Download server" link
(`downloadServer()`: the OS asset natively, the release page on the web; only
`https://github.com/rebel10n/KrunkNative/` URLs are opened).

## 9. Client integration (client-integration)

The native and web clients use the brain for the account, matchmaking and the social
hub; without a brain URL nothing changes (offline / singleplayer, the default server,
`--connect`).

### 9.1 Brain URL

| platform | first match wins |
| --- | --- |
| native | `--brain <url>`, env `KRUNKNATIVE_BRAIN_URL`, storage key `knBrainUrl` (`$XDG_CONFIG_HOME/krunknative/knBrainUrl.json`), else v3.1.1's dev default `http://127.0.0.1:5050` (probed) |
| web | `?brain=<url>`, `Module.krunkNativeBrainUrl`, else the page origin (probed; no brain there = the classic `/native` connection). `?brain=0` or `?offline=1` disables it |

Default URLs are probed with `GET /mm/status` (a kn_brain answers `{"brain": id, ...}`; net-bridge
answers `/ping` itself, so `/ping` cannot tell a brain from the bridge). Until the probe confirms
a brain the social socket stays closed; with none, account actions show v3.1.1's `#accResp`
error and guest play is unchanged. The client never reads `config/config.json` (it holds the
database credentials) nor touches the database: every account operation is a brain round trip.
`make run BRAIN_PORT=<p>` exports `KRUNKNATIVE_BRAIN_URL=http://127.0.0.1:<p>` for a
non-default port.

`http://` URLs only natively (TLS ends at nginx; point native clients at a plain
listener). Child windows (hub, editor) inherit the URL through the environment.
`--render-scenario` (uidiff) never uses a brain.

Startup decision (`client/src/net/backend_startup.c`): brain + not `--offline` + no
`--connect host[:port]` / `KRUNKNATIVE_SERVER_PORT` / web `?ws=` = **brain mode**: the
transport is not started until `/mm/seek-game` answers. The hub page (`--hub`,
`social.html`) with a brain opens no game connection at all.

### 9.2 Transports (`client/include/backend.h`)

* HTTP GET (`backend_http_get`): native worker thread (HTTP/1.1, `Connection: close`,
  chunked bodies), web `emscripten_async_wget2_data`. Callbacks run on the main thread
  from `backend_poll()` (called every frame in `client_tick`, also in page modes).
* Social socket (`backend_social_*`): `<brain>/social/ws?enc=json`, JSON text frames
  only (native: own RFC 6455 client thread; web: `emscripten/websocket.h`). `["pi"]` is
  answered with `["po"]`; listeners also get synthetic `["__open"]` / `["__close"]`.
  Reconnects with a 1..10 s backoff.

### 9.3 Account (`client/src/ui/account_ui.c`, index.js parity)

| UI | frames |
| --- | --- |
| startup / reconnect (`readyLoad`) | storage `krunker_username` + `krunker_token` -> `["a",1,[name,null,token]]` |
| Register / Login (window 5) | `["a",0,[name,pass,null],captcha]` / `["a",1,[name,pass,null],captcha]` -> `["a",err,id,name,data40,token,info]`; error text in `#accResp`. `captcha`: the solved `GET /mm/captcha` (7.10, `kn_captcha.c`), null when the brain has none |
| Logout | `["a",-1]`, storage keys cleared |
| header bar (`cK`) | `#signedInHeaderBar`: name (alias when premium), KR, level, level bar, `#mailCount` (`info.mail_unread`, `["gmc",n]`) |
| match result | `["ua",data40,info]` push (also `upd`); fallback: a token re-login 3 s after a round ends |
| Account window tabs | Stats / Partner / Settings (`["upp",old,new,rep]`, `["nme",alias]`) / Twitch, `["las"]` |
| Settings backup (settings tab 0) | `["uploadS",text]` -> `["pRes","SUCCESS",null,msg]`; `["getS"]` -> `["loadS",text]` |
| LOGIN REQUIRED windows | signed in: Store (tabs; no payments or wheel table), Challenges (level 30 gate, `["chg"]`), Ranked (MMR + level 15 gate; no queue yet) |

The account alias replaces `Client.menu_name`; `Client.net_join.session` carries the
session token into the next `NET_PACKET_JOIN`. Changing account while in a lobby's
menu re-seeks the same lobby so the server attaches (or drops) the account.

### 9.4 Matchmaking (`client/src/ui/servers_ui.c`, ClientManager.js parity)

1. `GET /mm/ping-list` -> region = saved `defaultRegion` / `pingRegion7` if listed,
   else the first (no per-region ping).
2. `GET /mm/generate-token`, then
   `GET /mm/seek-game?hostname=krunknative&region=R&autoChangeGame=true[&game=ID][&token=T][&dataQuery={dsmo,dsma}]&validationToken=V`
   (web: `?game=` of the page; native: `KRUNKNATIVE_GAME`; the token step and the Quickplay
   preferences: [v10/matchmaker-token.md](v10/matchmaker-token.md)). A refused token is replaced once;
   a brain without the route (404) is sought without one. Errors -> the v3.1.1 / v10
   `matchmaker.*` texts on the connection error screen; its retry link seeks again.
3. Connect: native TCP to `host:port` (names resolved per attempt); web
   `webUrl` if set, else `ws(s)://<origin>/native?target=host:port`. Right after HELLO
   both send `NET_PACKET_JOIN {game_id, client_key = clientId, session, name}`
   (`client/src/net/net_join.c`).
4. `GET /mm/game-info?game=ID` -> `#menuRegionLabel` = the server's proper name
   (`Chicago 2`; v3.1.1: `regionNames[region]`).
5. Rejoin (`servers_ui_connection_lost`): the server's `ERROR` `004004` / `004006` (it stopped,
   8.5) holds the transport and seeks the lobby again with `autoChangeGame` 2 s later (a fresh
   ticket: the same game id if it is back, else the region's best lobby), every 3 s while the
   matchmaker has none, for 2 min; a refused join key (`004001` "Join key ...", e.g. the
   transport retried a crashed server's address after its restart) seeks once at once and a
   second refusal within 30 s is shown ("Could not rejoin: ... again"). The error screen keeps the
   server's message ("GAME UPDATING" is v7's "Game Updating" header) with "Rejoining a game...".
   Direct (pinned) servers have no tickets: the transport retries them. Console `reconnect` / F5 (its reload
   ends with it) seek again too (`autoChangeGame` after a server stop), never with the old ticket.
   A session the transport lost with no server `ERROR` (not a kick or ban: a gateway or network
   drop, a crashed server) shows the transport's own reason instead of a bare "Disconnected from
   server": `Disconnected: <reason>`, ID `0x000001` and, like v7.1.4's `io_disconnect(null,
   "000001", closeEvent)`, `Code: <WebSocket close code> | Reason: <cause>` (timeout, ice,
   gateway, offline, reset, closed, ...). Examples: "no data from the server for 10 s (WebRTC)",
   "the network path to the connection gateway was lost", "the connection gateway is
   restarting", "the server closed the connection", "your network is offline". Then
   "Reconnecting..." and, for a matchmade session, a fresh ticket for the same lobby 1 s later
   (`autoChangeGame`; the transport holds so the old ticket is not resent); direct servers: the
   transport reconnects. When the rejoined sessions keep dropping within 20 s it stops after 4
   tries ("gave up after 4 quick reconnects"). The gateways tell their players before a restart (doc/transport.md
   "Goodbyes"): "GAME UPDATING" / "Server restarting" (004004 / 004006), the restart rejoin
   above.
   Auto-restart for updates (Settings > Client, default on; `client/src/game_reload.c`): `004004`
   on a session that had been joined (a drain refusing a new connection sends it too) shows the
   Game Updating screen with "Restarting..." for 1.5 s, then restarts the client instead of
   rejoining in place. Natively a launcher-started client restarts through the launcher
   (`krunknative --wait-pid PID -- <the same command line>`: its update check runs first, so a
   newer build that passed `staged_client_runs` is what comes back), else it re-executes itself
   (F4's restart; a replaced executable is the new build). The new process gets
   `KRUNKNATIVE_RELOAD_RESUME` = `rejoin <lobby id>` (or `connect host:port` for a direct server),
   skips the startup seek and runs the restart rejoin above for that lobby. The web page reloads
   (`location.reload()`: `?game=` / `?ws=` / the rest of the query kept; the lobby id goes through
   `sessionStorage` `knRejoin`). Off: the in-place rejoin. Tests: `make auto-restart-e2e`
   (`server/tests/auto_restart_e2e.py`: lobby through a stand-in launcher, new pid, same lobby;
   direct server via exec; the web reload with its query), `restart_rejoin_e2e.py` case 4 (off).

Servers window (2): `GET /mm/game-list` + `GET /api/player-count` (cached
`serverBrowserRate` 10 s), region groups, Custom Games group (`meta.cs`), search, Hide
Full, Quick Join (`quickJoinRegion`: up to 10 populated lobbies, else a region seek),
`checkedSwitchServer` (`game-info` first, FULL/ERROR). Switching: native re-seeks and
reconnects in place; web reloads with `?game=<id>` like v3.1.1. Region groups show the
place name (`Chicago`), rows the server's proper name (`Chicago 2`); the address bar,
Invite and the reload use the short id (`?game=CH2:kz862`, 4 "Lobby tuple"); Join, Connect
and console `connect` take a link or either id form. Games window (10):
`GET /api/maps?index=&pos=&accountId=` pages. Host Game: `GET /mm/host-game?region=R&config=JSON[&token=T]`,
config `{"private","maps":[name],"modes":[modeId],"map","mode","passCode"}`; the reply
is joined like a seek (needs an idle node, see 6).

### 9.5 Hub (`client/src/ui/social_ui.c`)

With a brain the hub's frames go to the social socket instead of the game connection
(`hub_send`); its `"api"` stand-in becomes `GET /api/maps|mods|search`. Rendered from
real data: login (`io_a`), profile (stats, class stats, follow state, creations),
leaders, search, feed posts (create, like, open + comments, reply), follow / followers.
Without a brain the guest stubs over the game connection are unchanged (uidiff hub-*).

### 9.5b Signed-in menu extras (account_ui.c, store_ui.c, ranked_ui.c)

| feature | v3.1.1 source | frames / routes |
| --- | --- | --- |
| Clans window (13) | windows[12] gen / sendReq / resp / kick / updReq / updRol / mangFunds / chngFunds / delReq (`client/src/ui/clan_ui.c`) | `["cln",1\|2,tag]` create / join request, `["cln",3]` my clan, `["cln",4,1]` -> `["clnL"]`, `["cln",5,id]`, `["cln",6,id,{r}]`, `["cln",7,kr,{wd}]`, `["clnR",id,1\|0]`; Clan War contracts (8) answer "Coming Soon" |
| Mail window (30) | windows[29], mailListResponse / readMailResponse / updateMailCount | `["kml",tab]` -> `["kml",tab,rows]`, `["rml",id]` -> `["rml",[row]]`, `["raml"]`, push `["gmc",n]`; `#mailCount` 99+ |
| 2FA at login | twoFAres / pick2FAkey / sub2FA (#twoFAPop grid) | password login -> `["c2fa",1]`; `["2fa",1,[cells]]` -> `["c2fa",2]` + `a` |
| 2FA enable | act2FA (no markup in the shipped page: Krunker Box Office adds a row to the Account window's Settings tab) | `["2fa"]` -> `["c2fa",null,null,pattern]` |
| Daily reward (level >= 5) | cK / dd (#claimHolder timer, "Play 1 Game" until a round ended), claimReward (ad-gated upstream) | `["clm"]` -> `["clm",lastReward,funds]` |
| Prize wheels (Store tab 0) | windows[13] tab 0, prizeWheel, spinButton/spinUI onclick, spinResponse | `["spin",i]` -> `["spin",null,rand,skin,funds,follow]`, `["unbx"]`. The canvas wheel is a white disc (no 2D canvas natively); the won item shows its preview PNG (`textures/previews/` from the asset tree, `make assets-import`; noimg fallback) |
| Ranked (27), from the menu bar's Ranked button (after Server Finder) | v7.1.4 windows[26] (tabs, justCopied, region; gen() empty in v7: the body is Krunker Box Office's in v7's window classes), joinQueue / leaveQueue / updateQueueStatus, QueueManager | `/mm/queue/rating` (the Season tab's rank card), `/mm/queue/join` (token, region, party, ready, classIndex), `/mm/queue/poll` every 1 s, update-ready / update-class / kick / promote / leave; "Join" `{gameId,host,port,webUrl,clientId}` is joined with that ticket; Host Game refuses while queued or in a ranked match (`custom.host.ranked`). Status under CLICK TO PLAY. In the match: the comp menu (`#mMenuHolComp`, `client/src/ui/comp_ui.c`) |
| Battle Pass (6) | v7.1.4 windows[5] (home / Challenges tabs, claims, purchasePop, skipLevelPop), the end screen's #endBPLayout (`client/src/ui/battlepass_ui.c`) | `["bpdl"]` -> `bpdl` + `bppd`, `["bpclm",id]` -> `bpclmr`, `["xstbp",1,code]` -> `purbp`, `["xstbpl",n]` -> `purbpl`; push `["bpEnd",tmpBPData]` after a commit (7.8). The daily (and weekly) challenges are its Challenges tab; the Challenges window (33) keeps map / weapon / nightmare |
| Season stats | Krunker Box Office: the hub profile's "Season 7" stat tab, the leaderboards' Season / S7 Kills / S7 Wins buttons (`social_ui.c`) | `["sst",name]` -> `sst`; `["r","leaders","season_score"\|"season_kills"\|"season_wins",...]` |
| playMap / `?play=` / `?game=` | playMap -> `/?play=`, autoHostMap | brain: a public lobby whose `meta.map`/`meta.m` matches, else `/mm/host-game` (`customMap` for non-catalogue maps: the game server fetches `/api/map-data`); offline: published maps are fetched (`/api/map-data?name=`) and hosted in the local game |

### 9.6 net-bridge `/native?target=`

`net-bridge/webBridge.ts`: `/native?target=host:port` bridges to that game server when
it matches `KRUNK_NATIVE_TARGETS` (comma list of `host:port`, `host:lo-hi`, `host:*`;
default `KRUNK_NATIVE_HOST:KRUNK_NATIVE_PORT..+63`); other targets are refused (1008).
No `target` = `KRUNK_NATIVE_HOST:KRUNK_NATIVE_PORT` as before.
`target=localhost:<port>` matches the `127.0.0.1` rules. A local/dev bridge also bridges
any loopback target (`localhost`, `127.x`, `::1`, any port: a developer's own `server_bin`
through the web console's `connect localhost:<port>` or Direct Connect): `make bridge` (no
`KRUNK_NATIVE_TARGETS`) or `KRUNK_NATIVE_ALLOW_LOOPBACK=1` (`deploy/local/backend.sh`).
Deployed bridges set `KRUNK_NATIVE_TARGETS` and keep the strict list.

### 9.7 Coordinator roles, central auth, server finder, client modes (coord-auth)

**Roles** (`services/brain/roles.c`): `brain.roles` = list or `"a,b"` of `auth`, `community`,
`finder` (CLI `--role auth,community` / `--brain.roles=`, env `KN_BRAIN_ROLES`); unset = all
three (one local process, as before). Modules outside a process's roles are not started, so
their routes 404. Production: `--role auth,community` on 127.0.0.1:8083
(coordinator.krunk.io) and `--role finder --no-db` on 127.0.0.1:8082 (matchmaker.krunk.io).

**Central auth** (`auth.c`, `auth_state.c`; reuses accounts.c `acct_auth` / `acct_revoke_token`,
no new tables). JSON in/out, errors `{"ok":false,"error"}` (401 bad login, 409 taken, 403
banned, 429 rate limit):

| route | body / query | reply |
| --- | --- | --- |
| `POST /auth/register`, `/auth/login` | `{username, password}` | `{ok, token, expiresAt, account}` |
| `POST /auth/refresh` | `{token}` or Bearer | same (sliding expiry, token kept) |
| `POST /auth/logout` | `{token}` or Bearer | `{ok}` (revoked) |
| `GET /auth/me` | Bearer / `?token=` | `{ok, account}` |
| `POST /auth/join` | `{token, serverHash}` | `{ok, ttl: 30}` |
| `GET /auth/hasJoined` | `?username=&serverHash=[&ip=]` | `{id, username, profile}` or 204 |
| `GET /guest/profile` | `?guest=<id>` (32 lowercase hex, the menu's guest id) | `{data: {profile: {funds, score}}}`: a signed-out guest's Unclaimed Rewards (zeros for an unknown id; creates nothing) |
| `POST /guest/claim` | `{guest: <id>}` | `{data: {profile: {funds: 0, score: 0}}}`: a login zeroes the balance (the credit rows stay) |

The guest balance (services/brain/guest.c, db_guest.c, migration 0350_guest_profiles) is credited by the
game servers' round end through the internal socket's `acct` ops `guest_credit` (`{guest, match, kr, xp}`,
one credit per match; KR and score capped at 20 and 2500, v10's guestKRCap / guestScoreCap) and zeroed by
`guest_claim` on a live sign-in (accounts.c).

`account`/`profile` = `{id, username, alias, premium, dev, mod, banned, ban_reason}`. Join
records live in memory for 30 s, keyed by serverHash. `auth.*`: `login_per_min` (10),
`register_per_hour` (5), `join_per_min` (60), `has_joined_per_min` (1200) per IP, `check_ip`
(false: also match the joining IP).

**Join handshake** (`shared/include/auth_join.h`): `serverHash = hex(SHA-256("kn-join-v1" 00
server_nonce 00 public_key 00 client_nonce))`. An online_mode `server_bin` sends
`NET_PACKET_AUTH_CHALLENGE` (73, `NetAuthChallengePacket {server_nonce[33], online_mode,
public_key[65], auth_url[128]}`) after HELLO; the client POSTs `/auth/join` to its own account
service, then sends JOIN `{name = username, client_key = "hj1:" + client_nonce}` (no session
token: tokens never reach game servers outside brain matchmaking); signed out it sends
`client_key = "hj0"` and plays as a guest (8.9: every server admits guests; a join that fails
`hasJoined` is a guest, never the claimed account). The server verifies with
`hasJoined` itself (standalone) or through its coordinator: internal WS `{"type":"auth","rid",
"hasJoined":{username, serverHash, ip}}` -> the community brain calls hasJoined (in process when
it also has the auth role, else `GET <community.auth_url>/auth/hasJoined`, libcurl for https) and
answers the normal `auth-reply` with its account row (inventory `data[10]`).
`{"type":"auth","central":{id, username}}` = the server already verified it. The community row
comes from `acct_ensure_central()` (accounts.c): upserted with the central id and username on
first join (a local account holding that name is renamed `<name>~<id>`), renamed with central.

**Coordinator rcon**: `POST /mm/admin/rcon?node=<id>` `{command}` (Bearer
`brain.internal_secret`) -> internal WS `{"type":"rcon","rid","command"}` ->
`rcon-reply {ok, output}` -> `{success, node, output}`; 404 `NodeNotLocal`, 504 timeout.

**Server finder** (`finder.c`, `finder_table.c`, in memory). `POST /servers/heartbeat` (alias
`/servers/submit`, both also under `/finder`) `{v, host, port, query_port, name, community,
online_mode, shutdown?}` -> `{ok, interval: 60, verified}`; `host` empty = the request IP;
`shutdown: true` removes the entry (same submitter only). The finder polls KNINFO over UDP with a
random nonce on submit and every `finder.poll_interval_s` (30); only entries whose last reply
echoed the nonce (and is younger than two intervals) are listed, with every field from the query
(heartbeat name/community are ignored). Dropped after `finder.expire_s` (180) without a heartbeat
or `finder.max_failures` (3) failed polls. Limits: `finder.submit_per_min` (30) per IP,
`finder.max_per_ip` (64) entries. `GET /servers?map=&mode=&tag=&space=1&online_mode=0|1&
community=&region=&q=&limit=` -> `{ok, servers:[{host, port, query_port, name, map, mode, modeId,
players, max, tags, community, online_mode, password, region, version, commit, commit_time,
build_time, protocol, ping_ms, last_seen, outdated, incompatible}], latest, download_url, downloads}`
(`?proto=M.m` = the asking client's protocol for `incompatible`; 8.12).

**Client modes** (`client/include/online_mode.h`, `client/src/net/online_mode.c`). `--mode
online|offline`, `KRUNKNATIVE_MODE`, web `?mode=`, storage `knMode` (Settings > Client > Account
Mode), default online. Online: the account service is `--brain` / `KRUNKNATIVE_BRAIN_URL` /
`knBrainUrl`, else `https://coordinator.krunk.io` (probed via `/mm/status`). Offline: no account
service at all; the header shows "Offline Mode", the loadout owns every store skin, nothing is
saved. The Servers window lists the finder (`--finder`, `KRUNKNATIVE_FINDER_URL`, web `?finder=`,
`knFinderUrl`, default `https://matchmaker.krunk.io/servers`; `off` disables) as "Community
Servers" (every server takes guests: no account-only rows, as in Krunker's list) and has a Direct Connect `host[:port]` field in
place of "+ Host Game". Native https/wss use OpenSSL libssl when CMake finds it
(`KRUNKNATIVE_HAVE_TLS`, system CA store, host name verified); web uses `fetch()`
(`backend_http_request`).

**Tests**: `brain_auth_finder` (unit: SHA-256 vectors, serverHash construction, join TTL /
user / hash / IP checks, limiter windows, finder nonce verification, filters, per-IP cap, poll
failures and heartbeat expiry); `brain_auth_e2e` = `services/brain/tests/federation_auth_e2e.sh
<build>` (auth+community, finder, a third-party community coordinator, an online_mode server linked
to it and an offline standalone one, headless clients in both modes: a guest joins the online_mode
server as a guest, rcon relay, replay check).

### 9.8 Client updates: launcher, updates server, Outdated (client-updates)

Native clients go outdated like servers (8.12) and update themselves through a **launcher**. The web
client is always the latest (served by the site) and has none of this.

**Updates server**: static files on kru behind OpenResty, `https://dl.krunk.io/updates/` (kru's
download host, host nginx + Let's Encrypt, role `kn_dl_origin`; `assets.krunk.io/updates/` serves
the same while that name is the edge origin and 302s there once it is the Pages CDN; role
`kn_updates`, root `/opt/krunknative-updates/public/`). Written only by
`tools/release/kn-updates.py`; the manifest's `assets.mirrors` come from `deploy/asset-shards.json`
(copied to `<root>/asset-shards.json`): the Pages projects of the split asset CDN, each with the
`only` / `skip` path prefixes of its part of the tree (deploy/pages/README.md "Asset CDN"):

| path | content | cache |
| --- | --- | --- |
| `client/<channel>.json` (`latest`) | channel manifest, written last | `no-cache` |
| `client/builds/<commit12>/BoxOffice-<plat>.{tar.gz,zip}` | update packages, the whole flat package (last 3 builds) | immutable |
| `assets/<version>.json` | game content list `{"format":1,"version","files":[[path,sha256,size],...]}` | immutable |
| `assets/objects/<aa>/<sha256>` | content-addressed files: hardlinks into the web asset tree `/opt/krunknative-web/assets` (no extra disk) | immutable |

Channel manifest (`format` 1; relative URLs resolve against the manifest's URL):
`{"format":1,"channel":"latest","published","build":{kn-build: describe, commit, commit_time,
build_time, protocol},"outdated_grace_h":48,"page":<release page>,"platforms":{"<plat>":{"update":
{"url","sha256","size"},"downloads":{"tar.gz"|"zip": url}}},"assets":{"version",
"manifest","sha256","size","objects","files","bytes","packaged":[paths the packages ship]}}`. Platforms: `linux-x64`,
`linux-arm64`, `windows-x64`, `macos-arm64`, `macos-x64` (the package's name is `BoxOffice-<platform>`);
the update package is the `.tar.gz` on Linux, else the `.zip` (Windows, macOS). Each package is flat:
`BoxOffice` (the launcher) and `runtime/` at its top, so the install root is the folder above `runtime/`.
Full packages (with the game content) are not hosted: the launcher downloads the content.

**Content split**. The packages ship the code-coupled, git-tracked part of `assets/` (every tracked
file but its README: shaders, maps, img, editor, `skins.json`, `previews.json`, `titles.json`, the
KN-only sounds; `client.yml` fails a package that misses one): those files belong to the executable
update, and the
content index leaves them out (`kn-updates stage` lists them in `build.json` as `package_assets`, the
manifest as `assets.packaged`, and a sync never removes them).
Everything else (models, textures, sounds, fonts: the web asset tree on kru) is the content set,
versioned by the hash of its file list. CI's kru deploy syncs the tracked files into that tree
(role `kn_web`, `tasks/tracked_assets.yml`, whenever `kn_web_sync_assets` is off; never deleting), so the web client and the Pages CDN get them with the
commit; the untracked rest only moves with a `--tags web_assets` run from a full checkout
(doc/asset-audit.md).

**Publishing** (CI, `client.yml` job `updates`, off until the repository variable
`KN_UPDATES_UPLOAD` is `1`): `kn-updates stage` picks each platform's update package from the
`client-pkg-*` artifacts, checks them against the release's `SHA256SUMS` when it is there and writes `build.json` (kn-build line, sha256 and size
per package, GitHub download links); rsync to `kru:/opt/krunknative-updates/incoming/<commit12>/`;
`ssh kru kn-updates publish <dir>` verifies the packages, moves them to `builds/`, indexes the content
(hash cache; objects hardlinked), writes the content list, then the channel manifest, and prunes builds
/ lists / objects beyond the last 3 publishes. Only moves forward (an older commit_time is skipped).
`kn-updates assets` re-indexes after the web asset tree changed (the role runs it, also with
`--tags web_assets`); `kn-updates status` prints what is live. The web asset tree must be updated by
replacing files (rsync's default temp + rename), never in place: objects share its inodes.

**Launcher** (`launcher/`, target `kn_launcher`, binary `BoxOffice` / `BoxOffice.exe` (GUI
subsystem) / the `BoxOffice.app`'s `Contents/MacOS/BoxOffice`, next to `runtime/client_bin`): what players start. A small GLFW
window (title, status, progress bar, Retry / Play / Quit after an error; FreeType with
`ui/css/fonts`, same GL backend as the client) or `--cli` / `--headless` (progress lines on stdout).
Before the game it:

1. reads `kn-install.json` (next to it, or `../Resources` in a `.app`: `{"format":1,"platform",
   "package":"tar.gz|zip|deb|rpm|app","channel","root"}`), rolls back an update a crash interrupted
   (journal in `<root>/.kn-update/`) and fetches the channel manifest (`--updates-url`,
   `KRUNKNATIVE_UPDATES_URL`, default `https://dl.krunk.io/updates/client/latest.json`, `off`
   disables; launchers before 8a010d82 have `https://assets.krunk.io/updates/...` built in, which
   302s there). A failed lookup / connection or a 5xx / 429 is tried 3 times (1 s, then 3 s
   apart; a 404 or a manifest that does not parse is not), and the reason is what the window and
   stderr show: `cannot resolve <host> (DNS: ...)`, `cannot connect to <host> port 443 (...)`,
   `TLS handshake with <host> failed: ...`, `HTTP 404 from <url>` (POSIX: `http_conn_error()`;
   Windows: the WinHTTP error named, e.g. `cannot resolve the host name (DNS) (WinHTTP error
   12007)`). 2026-10-13's "the updater fails to connect to the server" was DNS: `assets.krunk.io`
   had no record at Spaceship for a while after the move from Cloudflare. CI (`client.yml`, step
   "Launcher reaches the updates server") runs every package's launcher `--check` against the live
   manifest on its own platform (`tools/release/check-launcher.sh <package> [--old-url]`, also
   locally; a Windows zip runs through wine);
2. when the manifest's build is newer than its own (`kn_update_compare`, 8.12's rule: same commit or
   a newer commit_time = current; an older protocol, no build info or older than latest − grace =
   outdated; else update available) installs the update package: download (sha256 + size checked),
   extract (tar.gz/zip, unsafe paths refused), run the staged `client_bin --version` (a build that
   cannot run on this system is never installed), then swap every packaged file into the root with a
   journal (`ui/` as a whole; replaced files kept until the commit line, any failure rolls back). The
   running launcher is renamed aside like any file (also on Windows), so the new launcher runs from
   the next start. deb/rpm installs (and read-only roots) are left to the package manager: the window
   names the download;
   the client's own box (update_ui.c) follows the same rule, except for a local build: a dirty tree, no
   commit, or a commit origin/main does not have (`KN_VERSION_LOCAL`, cmake/kn_version.cmake). A local
   build never shows the update box, Outdated or the automatic restart, and the console says it is local
   (`KRUNKNATIVE_LOCAL_BUILD=1|0` pins the flag: the e2e tests use it);
3. syncs the game content when the manifest's content version differs from
   `<assets>/.kn-assets.json`: downloads the list (gzip on the wire), diffs it (a file the last list
   vouches for with the right size is kept; a file of the right size the list does not vouch for is
   adopted when `<assets>/.kn-hashes` knows its size + mtime, else hashed, 8 at a time), downloads
   only the missing / changed objects, deletes files the previous list had and the new one does not
   (never the user's own files), records the list and the hash cache. First install = everything
   (~1.9 GB). Downloads (`launcher/src/assets_sync.c`): each unique sha256 once (duplicate paths are
   copied), from every origin at once: the manifest's `objects` plus `assets.mirrors` (by-path copies:
   the two Pages projects of the split CDN, `{"url","layout":"path","skip"|"only":[prefixes]}`, so
   each is asked only for its own directories; without the key, a manifest from `https://dl.krunk.io/`
   uses `https://assets.krunk.io/assets/`), 16 keep-alive connections per origin
   (`--threads`, `KRUNKNATIVE_ASSET_THREADS`; `--asset-mirrors SPECS|off`, `KRUNKNATIVE_ASSET_MIRRORS`,
   specs `[only=P+P@|skip=P+P@]URL`), pulling from one queue per group of objects the same origins
   have, half from the large end, half taking batches of up to 16 small files
   HTTP/1.1-pipelined (8 in flight). DNS lookups and TLS sessions are cached per host. A failed file
   moves to another origin, then is retried with backoff (3 rounds over all origins); an origin with
   8 transport failures in a row is dropped for the run (never the last). Partial downloads stay in
   `<assets>/.kn-sync/<sha256>.part` and continue with `Range` (also in the next run); every file is
   checked (size + sha256) before its rename into place; no fsync. Measured from the dev machine in
   Oct 2026 (19,194 files, 1.9 GB): 462 s with 4 connections to `assets.krunk.io`, 41-50 s now;
4. copies the package's own `assets/` files into the content dir when that is another directory;
5. starts `client_bin` (cwd = the directory with `ui/`; `KRUNKNATIVE_ASSETS_DIR=<content dir>`,
   `KRUNKNATIVE_LAUNCHER=<launcher>`, `KRUNKNATIVE_ASSET_ORIGINS=<the sync's origins>`; POSIX `exec`,
   Windows a new process). Unknown arguments and everything after `--` go to the client.

**On-demand fetch** (`client/include/asset_fetch.h`, native only; the web build fetches every file on
demand anyway): a file of the content list (`<assets>/.kn-assets.json`) that is missing or has another
size when something loads it (the sync skipped it, a user deleted or truncated it) is downloaded in
the background: workers (2) try the origins in the sync's order (`KRUNKNATIVE_ASSET_ORIGINS`:
the mirrors first, each with its path filter (`only=P+P@<url>` / `skip=P+P@<url>`: a file goes to
its own Pages project), then `objects:<url>` = content-addressed on kru; default
`https://assets.krunk.io/assets/` then `https://dl.krunk.io/assets/`), verify size + sha256, write `<file>.kn-fetch-N.part` and rename it into place; never into
a git checkout (checked per directory, symlinks resolved). 404 / wrong content on every origin gives
the file up for the session, transport failures retry with backoff (4 rounds); one log line per file.
While it downloads, `load_texture` hands out a (0,0,0,0) texture and `load_obj_model` an empty model
(three.js before a load completes); both are filled in place when it arrives, sounds play from their
next use, page images are probed again. Off without a content list (dev trees, test fixtures) unless
`KRUNKNATIVE_ASSET_FETCH=1`; `KRUNKNATIVE_ASSET_FETCH=0` turns it off;
`KRUNKNATIVE_ASSET_FETCH_CHECKOUT=1` allows a git checkout. Tests: `asset_fetch_test`,
`make asset-fetch-test`.

Content dir: `<root>/assets` when writable (portable tar.gz/zip), else the user data dir
(`${XDG_DATA_HOME:-~/.local/share}/krunknative/assets`, `~/Library/Application Support/KrunkNative/assets`
(always on macOS: never inside the signed bundle), `%LOCALAPPDATA%\KrunkNative\assets`); always the user
data dir for deb/rpm; `--assets-dir` / `KRUNKNATIVE_ASSETS_DIR` override. `assets_path()` honours
`KRUNKNATIVE_ASSETS_DIR`; `ui/` stays beside the cwd-based default (`assets_default_path()`).
Other flags: `--no-update`, `--check` (exit 3 = newer build), `--no-launch`, `--wait-pid PID`.
HTTP: `launcher/src/http_fetch.c` (POSIX: `client/src/net/http_conn.c`, the brain client's
connection code with OpenSSL; Windows: WinHTTP with the system certificate store).

**Client** (`client/src/ui/update_ui.c`): reads the manifest 3 s after start and hourly (headless
runs only with `KRUNKNATIVE_UPDATES_URL`), and when a newer build exists shows a box in the menu's
left column above v7's notices: **Outdated** (orange) or **Update available** (yellow), the latest
version, **Update now** (only under the launcher, for self-updating installs: starts the launcher
with `--wait-pid PID -- <the client's command line>` and quits) and **Download** (the deb/rpm/archive for this
install, else the release page; only `github.com/rebel10n/KrunkNative/` and the updates host are
opened). Console: `update` (status), `update check`, `update now` (menu open only), `update download`.
With Settings > Client > **Auto-restart for updates** (default on; `client/src/game_reload.c`) a
build Update now could install restarts the client by itself: in the menu after 1.5 s, in a match
never while playing but 5 s after the match ends or when the menu next shows outside a match (Esc
or a death mid-match does not count; the box says "Restarts after this match"). A restart for a build the launcher then could not install (corrupt package, the
staged client does not run here) brings the installed client back (the launcher starts it instead
of waiting for a click) and `KRUNKNATIVE_UPDATE_TRIED` keeps it from restarting for that build
again. The web build compares its page's `<meta name="kn-build">` with the live page's (hourly): a
newer deploy is "Update available" and the same automatic restart, as `location.reload()`.

**Download window** (`client/src/ui/download_ui.c`, the header's "Download Our Client" = v7's
`showWindow(25)`; `/?download` opens it, the hub's app badges link there): every package of the
GitHub release `client-latest` (`BoxOffice-<plat>.<ext>`, `BoxOffice-android.apk`,
`BoxOffice-ios-unsigned.ipa`), the visitor's platform first and highlighted (web:
`navigator.userAgent` / `userAgentData` / the WebGL renderer for Mac CPUs; native: `kn-install.json`,
else the build's platform), every other platform listed under it. iPhone / iPad: Add to Home Screen
(the web build's PWA, `platform/web`). The importer keeps v7's header and description only
(`tools/ui-import/windows_v7.py`), so Krunker's installer links never come back.

**Tests**: `update_test` (in `make test`: manifest parsing, URL resolution, version comparison, asset
lists and diffing, the install marker, tar.gz/zip extraction incl. traversal and escaping symlinks,
the journaled swap with rollback and crash recovery, package asset mirroring);
`make launcher-e2e` (`launcher/tests/launcher_e2e.py`: a store made by `kn-updates.py` served on a
free port; executable + launcher self-update, content diff, deb deferral, corrupt package, the
client's own Outdated check and `update now`).

**kru setup**: off until `kn_updates_enabled: true` in `deploy/ansible/group_vars/central/main.yml`
(Deploy kru runs central.yml on every push; with the switch off it adds nothing to kru). Then
`ansible-playbook central.yml --tags openresty,updates` (the `kn.d/` include in nginx.conf, the store
directories, `/usr/local/bin/kn-updates`, the `/updates/` location, the first content index) and the
repository variable `KN_UPDATES_UPLOAD=1`.
The download host `dl.krunk.io` (role `kn_dl_origin`, deploy/ansible/README.md "Download host")
serves the store directly from kru; the Pages CDN never holds it (packages over 25 MiB, 37k files).

## 10. Deployment

`deploy/` (svc-matchmaker); details in `deploy/README.md`.

* Local: `make backend-up` (`deploy/local/backend.sh up`): `kn_brain --migrate-only`,
  `kn_brain` on 127.0.0.1:5050, `GAMESERVERS` (2) x `server_bin --brain ...` on
  21015.., `net-bridge --web` on 8080 when `dist/web` exists (game servers then
  advertise `webUrl = ws://127.0.0.1:8080/native?target=127.0.0.1:<port>`).
  `make backend-status`, `make backend-down`; `make backend-run` is svc-core's
  foreground variant. Knobs: `BRAIN_PORT GAME_PORT BRIDGE_PORT REGION BRIDGE
  FAKE_NODES KN_RUN_DIR BACKEND_ARGS`.
* Outbound mail: `deploy/ansible` writes `KN_SMTP_*` (7.9) to `/etc/krunknative/smtp.env`
  (0640, `no_log`), an optional `EnvironmentFile` of the kn-brain units; CI secrets `KN_SMTP_HOST`,
  `KN_SMTP_PORT`, `KN_SMTP_TLS`, `KN_SMTP_USER`, `KN_SMTP_PASSWORD`, `KN_SMTP_FROM` (deploy-kru.yml).
* systemd: `kn-brain.service` (migrations in ExecStartPre), `kn-gameserver@<NODE>`
  (`/etc/krunknative/gameserver-<NODE>.env` = `KN_GAMESERVER_*`), `kn-net-bridge`,
  `krunknative.target`. `deploy.sh push` and the ansible role `kn_gameserver` (its "Restart game
  servers" handlers) write `update` to `/run/krunknative-deploy/gameserver-stop-reason`
  (`shutdown_reason_file`) for the restart and remove it after: players see "GAME UPDATING" and
  rejoin (8.5, 9.4).
* Restarts only for a change (deploy-kru.yml / deploy-edges.yml run on every push). The service
  binaries are built reproducibly and stamped with the last commit that changed their sources
  (`tools/release/version-meta.sh --server`, also the release metadata of 8.12), the release dir
  is named by a hash of what it ships (`kn_svc_release_id`, role kn_release, content-only rsync),
  kn-rtc builds with `-buildvcs=false` and the net-bridge bundle is deterministic: a push that
  changes none of a service's inputs leaves it running. Until 2026-10-12 every push restarted
  kn-rtc (Go stamped the commit into it) and both brains and every game server (a new release dir
  per commit). A game server restart drains in parallel on each host: no new players, the round
  finishes (`kn_gameserver_drain_timeout`, 240 s = `shutdown_grace`), then "GAME UPDATING". The
  kn_rtc / kn_bridge handlers write `update` to `/run/krunknative-deploy/gateway-stop-reason`
  (`KRUNK_NATIVE_STOP_REASON_FILE`) around their restart, so the gateways send "GAME UPDATING".
* nginx (mirrors the jail's `ansible/templates/nginx/*.j2`): brain host
  `matchmaker.<domain>/<x> -> /mm/<x>` (incl. `/internal/ws` upgrade),
  `api.` -> `/api/`, `social.` -> `/social/`, `play.<host>` = web client + `/native` +
  all routes same-origin; region host = web client + `/native` + `/ping`. TLS via
  certbot, CORS snippet like the jail's `cors.conf`.
* PostgreSQL + Redis on kru (redis-tier, 3.11): roles `kn_postgres` (own cluster
  `<ver>/krunknative` on 127.0.0.1:5434, role + database `krunknative`, `KN_DATABASE_PASSWORD`
  generated into `secrets.env`), `kn_redis` (`kn-redis.service` on 127.0.0.1:6380, ACL user
  `kn_brain`, `KN_REDIS_PASSWORD`, `/etc/krunknative/redis.env`) and `kn_db_cutover` (`kn-db-cutover`:
  the coordinator's one-time move from coord.db, rolled back by itself on failure). Each kn-brain
  unit reads its database from `/etc/krunknative/<instance>-db.env`. Runbook, rollback and disk:
  `deploy/ansible/README.md` "Database cutover".
* Multi-region: `deploy/deploy.sh [-n] [--certbot] push|status|render|drain` over
  `deploy/inventory` (`<host> region= brain=1 gameservers=N first_port= node_prefix=
  bridge=1 nginx=brain|node brain_url=`). One shared PostgreSQL; one brain per region
  (game servers reach it over plain `ws://`, local or private network).
