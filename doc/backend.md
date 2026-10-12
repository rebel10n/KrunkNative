# KrunkNative backend

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

### brain (svc-matchmaker)

All keys are optional; env `KN_BRAIN_<KEY>` / CLI `--brain.<key>=` override.

| key | default | meaning |
| --- | --- | --- |
| `brain.host` | `127.0.0.1` | bind address (nginx fronts TLS in deployment) |
| `brain.port` | `5050` | listen port (v3.1.1 dev `matchmakerURL` is `http://127.0.0.1:5050`) |
| `brain.id` | `<hostname>-<port>` | unique brain id (registry owner column) |
| `brain.region` | `local` | default region of this brain (nodes that send none) |
| `brain.public_url` | `http://<host>:<port>` | advertised URL (logs, `/mm/brains`) |
| `brain.internal_secret` | `kn-dev-secret` | Bearer secret game servers must present on `/mm/internal/ws`; must equal `gameserver.brain_secret` |
| `brain.join_secret` | = `internal_secret` | HMAC key of join tickets (must equal the game servers' `gameserver.join_secret`, default their `brain_secret`) |
| `brain.join_ttl_s` | `30` | ticket lifetime / slot reservation time |
| `brain.cors_origins` | `["*"]` | allowed CORS origins for the web build |
| `brain.trust_proxy` | `false` | take client IP from X-Forwarded-For (behind nginx) |
| `brain.node_ttl_s` | `30` | node/lobby row TTL (v3.1.1 `STALE_AFTER_MS`) |
| `brain.heartbeat_s` | `5` | how often the owning brain refreshes its rows |
| `brain.ping_interval_s` | `10` | internal WS ping interval (v3.1.1 `PING_INTERVAL_MS`) |
| `brain.regions` | `[{"id":"local","name":"Local"}]` | known regions (`id`, `name`, optional `ping_host`); ids are the 2-letter region codes the client shows (`SV`, `LA`, `CH`, `VA`) |
| `brain.federation` | `true` | use the shared database's rows (false or no DB = in-memory, this brain only) |
| `brain.matchmaking.cross_region` | `true` | quick join falls back to other regions when the requested one has no room |
| `brain.matchmaking.max_players` | `8` | cap when a lobby reports none |
| `brain.matchmaking.mode_caps` | `{}` | per-mode quick-join caps, by mode name or index (`{"ffa":8,"tdm":10}`) |
| `brain.matchmaking.soft_cap` | `0` | if > 0, quick join treats lobbies at/above this player count as full unless nothing else has room |
| `brain.matchmaking.require_version` | `false` | honour the client's `dataQuery.v` (lobbies with another `meta.v` are skipped) |
| `brain.host_timeout_s` | `10` | how long a host-game request waits for a game server |
| `brain.ranked.*` | see 6.5 | ranked queue (min level, mode, maps, rating window, K, abandon penalty/cooldown, timeouts) |

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
| `knsvc` | `buf.h log.h config.h json.h loop.h http.h ws.h crypto.h msgpack.h` (umbrella `knsvc.h`) | cJSON, libcrypto, libm, pthreads | `kn_brain`, `server_bin` (brain client), tools |
| `knsvc_db` | `db.h store.h migrate.h` | `knsvc` + the built-in drivers' client libraries | `kn_brain`, `kn_migrate` |

Build: part of the normal native build when OpenSSL is found (`knsvc_db`, `kn_migrate`
and the brain additionally need at least one store driver: options
`KNSVC_STORE_POSTGRES` / `KNSVC_STORE_SQLITE` / `KNSVC_STORE_MYSQL`, each ON by default
when its library is found); `-DKRUNKNATIVE_SERVICES=OFF` skips `services/`
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
  `kn_sha256`, `kn_sha1`, `kn_hmac_sha256[_hex]`, `kn_pbkdf2_sha256`, hex and base64
  (standard/url, padded or not; strict decoding).
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
  and the 500 ms host-request poll stay as the fallback, as on postgres).
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

DB tests skip (exit 77) when their server is unreachable. PostgreSQL: create the test
database once with `sudo -u postgres createdb -O krunknative krunknative_svccore`.
MySQL/MariaDB: `KN_TEST_MYSQL_HOST` / `_PORT` / `_USER` / `_PASSWORD` (default
`127.0.0.1:3306 root`) must be allowed to create and drop databases (`kn_test_*`).
`ctest -L sqlite` / `ctest -L mysql` run one driver; the brain scripts take
`KN_TEST_DB_DRIVER=sqlite|mysql` directly (`services/brain/tests/db_scratch.py` makes the
per-run database). The suite also runs
clean under clang `-fsanitize=address,undefined` with leak detection.

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
  on ingest, as registry.ts does; the Servers window shows `gameId.split(":")[0]`
  as the region label of custom games, so short node ids like `NY` read well).
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
| `proto`, `build`, `commit`, `commitTime` | string, string, string, int | build identity of the server (`proto` "M.m", git describe, 12-hex commit, unix s): see 8.12 |
| `outdated`, `incompatible` | bool | added by `/mm/game-list` (8.12); not sent by servers |

### Routes

| route | v3.1.1 | response |
| --- | --- | --- |
| `GET /ping` | publicRoutes `/ping` | `{"success":"true"}` (region latency probe, also `/mm/ping`) |
| `GET /mm/ping-list[?hostname=]` | `registry.regions()` | `{"<region>": "host[:port]"}` — one ping host per live region (`brain.regions[].ping_host`, else a live node's public host); the client fetches `http(s)://<value>/ping` |
| `GET /mm/regions` | (KrunkNative) | `[{"id","name","players","games","nodes"}]` known + live regions |
| `GET /mm/game-list[?hostname=]` | `/mm/game-list` | `{"success":true,"totalPlayerCount":N,"games":[tuple,...]}` all live lobbies of every region |
| `GET /mm/game-info?game=ID` | `/mm/game-info` | the tuple; unknown id -> best quick-join pick (v3.1.1), `InvalidGameId` if none |
| `GET /mm/seek-game?region=R[&game=ID][&autoChangeGame=true][&dataQuery=JSON][&token=SESSION]` | `/mm/seek-game` | `{"gameId","lobbyId","host","port","clientId","changeReason","region","webUrl"}` |
| `GET /mm/host-game?region=R&config=JSON[&token=SESSION]` (also `POST`, JSON body `{region, config}`) | (game socket `custom` in v3.1.1) | same as seek-game, for a freshly hosted custom lobby |
| `GET /mm/brains` | (KrunkNative) | `[{"id","region","publicUrl","nodes","lastSeen"}]` live brains (federation debug) |
| `GET /mm/status` | (KrunkNative) | `{"brain","region","version","nodes","lobbies","players","draining"}` |
| `GET /api/player-count`, `/social/player-count` | same | `{"playerCount":N}` |
| `POST /mm/admin/drain?node=ID` | (KrunkNative) | Bearer `brain.internal_secret`; see 6.4 |

seek-game details:

* `clientId` is the **join ticket** (see 5.3): the game server verifies it on
  connect. `host`/`port` are the game server's native TCP endpoint; `webUrl` (if
  the node advertises one) is what web clients connect to.
* `changeReason` = `"game_changed"` when `game` was requested but another lobby was
  picked (`autoChangeGame`), else `null`. With `autoChangeGame=false` and a full or
  unknown requested game the error is `GameFull` / `InvalidGameId`.
* `token` (optional) = a svc-accounts session token; when valid the ticket carries
  the account id + name so the game server can attach the account. Only such
  signed-in players are matched to `onlineMode` nodes (6.2); a guest asking for one
  by id with `autoChangeGame=false` gets 403 `AccountRequired`.
* `FORCE_ENTER=1` cookie (or `&force=1`) bypasses the capacity check on the requested
  game (v3.1.1 `hasForceEnterCookie`).

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
  per `cross_region`). The reference ignores `region`.
* `autoChangeGame=false` with a full or unknown `game` returns 409 `GameFull` /
  404 `InvalidGameId` (the client's `matchmaker.full2` / `.invalid`). The reference
  overflows into another lobby. With nothing that has room, kn_brain returns 503
  where the reference sends the player into the least populated (full) lobby.
* An `OPTIONS` preflight on `seek-game` / `host-game` answers 204 and issues no
  ticket.
* `seek-game` appends `region` and `webUrl` after the v3.1.1 keys.
* `/mm/queue/*` exists (the v3.1.1 client's ClientManager calls it; the
  reconstruction has no such routes, see 6.5), as do `/mm/status`, `/mm/regions`,
  `/mm/brains`, `/mm/host-game` and `/mm/admin/drain`.

Matchmaking is HTTP to the brain only. No game server answers it:

* `server_bin` serves no HTTP. With a brain (`gameserver.brain_url`) it also drops
  the hub's `NET_PACKET_SOCIAL_REQUEST` frames, which are the brain's social socket's
  job (v3.1.1 `MM_NODE_MODE=node`). The standalone server and the client's offline
  game (`local_game.c`) still answer them as the guest hub.
* `net-bridge --web` reverse-proxies `/mm/*`, `/api/*` and `/social/*` to the brain
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
| `hello` | `nodeId` (no `:`), `region`, `name`, `publicHost`, `publicPort`, `webUrl`?, `version` (game version, `"1.0.0"`), `build`?, `commit`?, `commitTime`?, `buildTime`?, `protocol`? (build identity, 8.12), `maxPlayers`, `custom` (bool: can host custom games), `maxLobbies`? (default 1), `onlineMode`? (0/1, default 0: only signed-in players are matched to it), `password`? (bool: never quick-joined), `modes`?, `maps`? | first frame |
| `snapshot` | `lobbies: [tuple...]` | full lobby set, every 5 s (replaces the node's set) |
| `lobby-up` | `lobby: tuple` | upsert one lobby (joins/leaves, 250 ms debounce) |
| `lobby-down` | `lobbyId` | lobby removed (local or namespaced id) |
| `pong` | `ts` | answer to `ping` |
| `drain` | `reason`? | node is shutting down: its lobbies stop being offered (seek/host), still listed with `meta.draining` |
| `player-join` / `player-leave` | `lobbyId`, `clientId`? (ticket `jti`), `accountId`? | optional; join consumes the ticket's slot reservation |
| `auth` | `rid`, `ticket` or `token` | resolve a player: ticket = seek-game `clientId`; token = session token. -> `auth-reply` |
| `commit` | `rid`, `lobbyId`, `matchId`, `final`, `map`, `mode`, `players: [{accountId, name, kills, deaths, score, won, timePlayed, games, kr, xp, classIndex}]` | match result; the brain forwards each `accountId > 0` to svc-accounts op `stats` -> `commit-reply` |
| `acct` | `rid`, `op`, `args` | generic pass-through to svc-accounts (`accounts_internal_call`) -> `acct-r` |
| `host-request` | `rid`, `region`?, `config`, `hostAccountId`? | a player asked this server (game socket `custom`) to host a game; the brain finds a server with capacity -> `host-request-reply` |
| `host-reply` | `rid`, `ok`, `lobbyId`?, `err`? | answer to the brain's `host` |
| `ranked-result` | `rid`, `matchId`, `lobbyId`, `winner` (0, 1, -1 draw), `players`, `abandoned` | ranked match end (6.5) -> `ranked-result-reply` |

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
towards fill in quick join so parallel seeks do not overfill); `player-join` with
the `jti` releases the reservation early. Session-token auth (`auth` with `token`)
is the path for players that connect without the brain (direct connect).

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
   KrunkNative: a guest (no valid `token`) asking for an `onlineMode` lobby is
   treated like a full one, but with `AccountRequired` (403).
2. Public pool = non-custom lobbies (custom lobbies are only reachable by id),
   minus password protected ones (hello `password` or meta `pr`) and, for guests,
   lobbies of `onlineMode` nodes (hello `onlineMode`, persisted for other brains as
   meta `onlineMode: true` in the lobby tuple): the server would refuse them
   ("Failed to verify username").
   In the requested region, the most populated lobby with room (first pass below
   `soft_cap`, if set).
3. No room in the region and `cross_region`: the same rule over every region.
4. Nothing with room -> `NoAvailableServers` (`NoServersMatchQuery` when lobbies
   exist but none matches the version). v3.1.1's "least populated" overflow would
   send the player into a full lobby the game server refuses, so it is reported.

Each answer issues a ticket and reserves its seat for `join_ttl_s` (in memory and
in `mm_reservations`); `player-join` (or an `auth` with the ticket) releases it.

### 6.3 Hosting custom games

Admin / tooling only: the clients no longer call `/mm/host-game` (hosting = running
`server_bin`, section 8.7/8.11).

`/mm/host-game`, a node's `host-request` (game-socket `custom`), or the social WS
via `brain_host_game()` (`registry.h`): pick, in the requested region (then any
region when `cross_region`), a non-draining `custom`-capable node (not password
protected; `onlineMode` only for a signed-in host or ranked) with spare
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


### 6.5 Ranked queue (`ranked.c`, `0201_ranked.sql`)

v3.1.1's ranked matchmaking (index.js `joinQueue` / `leaveQueue`,
ClientManager's `QueueManager`, `config.queues`, `minRankedLevel`, `eloK`,
`rankedDeduction`, `rankedCooldown`). The reconstruction leaves it unimplemented
(`UPDATE_ELO` is a no-op), so the routes and shapes follow the v3.1.1 client.

State is in the shared database (`mm_ranked_parties`, `mm_ranked_members`,
`mm_ranked_matches`, `mm_ranked_match_players`, `mm_ranked_bans`; logic in the
plpgsql functions `kn_rq_*` on PostgreSQL, the `P_RQ_*` kn_procs of `db_mm.c`
elsewhere; the matcher lock is `pg_try_advisory_xact_lock`, the sqlite write lock
or `SELECT ... FOR UPDATE NOWAIT` on `mm_ranked_lock`), so any brain serves any client. Ratings are
svc-accounts' `players.player_elo/elo2/elo4` (read from the session's account,
written with the accounts op `"elo"` `{id, mode: 1|2|4, delta}`).

Routes (each also without the `/mm` prefix; all GET):

| route | answer |
| --- | --- |
| `/mm/queue/join?token=SESSION&region=R&queues=["r2v2"]&party=ID\|&ready=0\|1&classIndex=N` | `{"clientId","clientKey","status":null}`, or `{"clientId":null,"clientKey":null,"status":ERR[, "cooldownMs"]}` (v3.1.1 `"iq"` values; ERR = `AccountAlreadyPlaying`, `LoginRequired`, `LevelTooLow`, `PartyNotFound`, `PartyFull`, `RankedCooldown`) |
| `/mm/queue/poll?clientKey=` | poll state (below); members that stop polling for `poll_timeout_s` are dropped |
| `/mm/queue/update-ready?clientKey&isReady=true\|false` | state |
| `/mm/queue/update-queues?clientKey&queues=JSON` | state (leader only; unknown ids dropped) |
| `/mm/queue/update-class?clientKey&classIndex=N` | state |
| `/mm/queue/kick-party-member?clientKey&kickClientId=` | state (leader only) |
| `/mm/queue/promote-party-member?clientKey&promoteClientId=` | state (leader only) |
| `/mm/queue/leave?clientKey=` | `{"left":true}` (v3.1.1 "unqueue") |

Unknown key: 404 `{"error":"InvalidClientKey"}` (the client then drops the queue).
State (v3.1.1 poll response):
`{"status":{"t":"Queued"|"CreatingGame"|"Join","c":{...}},"partyId","partyLeaderId",
"partyMembers":[{"clientId","accountId":<username>,"accountData":"{\"c\":clan,\"cR\":rank,\"e\":rating}",
"classIndex","isReady"}],"queueIds":["r2v2"],"region"}`; `c` is `{queueSize}` while
Queued and `{gameId, host, port, webUrl, matchId, clientId: <this player's ticket>}`
on Join. `HostGame` is not used (the brain creates the lobby). Join tickets reserve
no seat: either connect with `clientId` or seek `?game=gameId&token=`.

Matching (every second, by whichever brain gets `pg_try_advisory_xact_lock`):
parties whose members are all ready, per region and queue (r4v4, r2v2, r1v1 =
2 teams of 4/2/1; a party fits a queue when its size <= the team size). The
longest-waiting party anchors; parties within `rating_window + window_growth x
wait` (max `window_max`) of its average rating fill team 0, then team 1. The match
is hosted with `host` (config `{"ranked":true, "matchId", "queue", "teamSize",
"maxPlayers", "mode": brain.ranked.mode (38, MODE_ID_RANKED), "modes", "maps"/"map" (random from
brain.ranked.maps, if set), "private":true, "startGraceS", "teams":[[{accountId,
name}],[...]]}`); hosting failure puts the parties back (10 s back-off).

Results: the game server sends `{"type":"ranked-result","rid","matchId","winner":0|1|-1,
"players":[...],"abandoned":[accountIds]}`; the brain closes the match once
(idempotent), applies Elo per player on team averages (K = `brain.ranked.k`, 32;
ratings floor at 0), `-abandon_penalty` (30) and a `cooldown_s` (2 h) queue ban for
abandoners, and replies `{"type":"ranked-result-reply","rid","ok":true,"ratings":
[{accountId,name,team,key,old,new,delta,abandoned?}]}`. Matches without a result
are aborted after `match_timeout_s`.

Config `brain.ranked.*`: `enabled`, `min_level` (15), `mode` (38: KN Ranked on the v7.1.4 mode table), `maps`,
`rating_window`, `window_growth`, `window_max`, `k`, `abandon_penalty`,
`cooldown_s`, `poll_timeout_s`, `creating_timeout_s`, `match_timeout_s`,
`start_grace_s`. Test: `ctest -R brain_ranked` (`tests/ranked_test.py`). The matcher's
advisory lock key includes the schema (`0202`), so schemas sharing a database match independently.

## 7. Social WebSocket, accounts, profiles (svc-accounts)

Owner: svc-accounts. Code: `services/brain/accounts.c` (accounts, sessions, internal
ops, password hashing worker), `social_ws.c` (socket, sessions, dispatch, presence),
`profiles.c` (pages, profile, search, follows, reports), `leaderboards.c`,
`social_data.c` (feed, chat, gifts, clans, settings, mail, market, trades),
`creations.c` (hub HTTP catalogue, map publishing), `store.c` (store skins, prize
wheels, Free KR wheel, daily reward). Private header `acct_internal.h`; public
`accounts.h` (internal ops, used by `internal_ws.c` / `public_routes.c`) and
`social_ws.h`. Schema: `services/db/migrations/postgres/0100_accounts.sql`,
`0101_social.sql`, `0102_items_creations.sql`, `0103_account_functions.sql`,
`0104_social_functions.sql`, `0105_twofa.sql`, `0106_store.sql` (applied migrations are never edited:
changes go into a new 01xx file in every dialect directory; `sqlite/` and `mysql/`
hold the same tables, the SQL functions are C kn_procs in `db_accounts.c` /
`db_social.c`, 3.6). Port of v3.1.1 `matchmaker/socialWs.ts`,
`server/accounts.ts`, `server/social.ts`, `server/schema.ts`; message labels and
shapes are what the real client (`src/social.js`, `src/index.js`) sends and reads.

### 7.1 Configuration (`"accounts"`)

| key | default | meaning |
| --- | --- | --- |
| `name_min` / `name_max` | `3` / `16` | username length; letters, digits, `_` (v3.1.1 `NAME_RE`) |
| `password_min` / `password_max` | `6` / `100` | password length (max bounds PBKDF2 work) |
| `password_iterations` | `600000` | PBKDF2-HMAC-SHA256 iterations (`kn_password_*`); older hashes are re-hashed on login |
| `alias_min` / `alias_max` | `3` / `16` | display name ("nme"): letters, digits, space, `_`, `-` |
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
| `trade_min_level` / `trade_max_items` | `20` / `10` | `config.tradeMinLvl` / `tradeMaxOutP` |
| `gift_min_level` / `gift_fee_percent` | `30` / `10` | KR gift rules (v3.1.1 `giftMinLVl`, `giftFee` 0.1; the sender pays amount + fee) |
| `market_fee_percent` / `market_fee_bypass` / `sale_max` / `max_listings` | `5` / `0` / `1e7` / `50` | market listing fee (social.js calcFee), fee-free prices up to (`marketFeeBypass`), price cap (`saleMax`), open listings ("MAX") |
| `clan_tag_max` / `clan_max_members` | `4` / `0` | clan tag length (create input maxlength) / members (0 = unlimited) |
| `post_max_len` / `comment_max_len` / `chat_max_len` | `1024` / `500` / `300` | text limits |
| `page_size` / `leaderboard_cache_s` | `100` / `30` | leaderboard rows / per (query, region) cache |
| `skins_file` / `assets_dir` | `assets/skins.json` / `assets` | store skins in `store.skins` index order (wheels, quick sell, market filters) / map catalogue JSON |
| `current_season` | `3` | `config.currentSeason`: wheels only roll skins of this season |
| `reward_time_s` / `reward_min_level` / `daily_reward_kr` | `21600` / `5` / `50` | `config.rewardTime` between Free KR spins / daily rewards (shared `lastReward`), `config.rewardMinLvl`, KR of `clm` |
| `free_wheel_cooldown_s` | `21600` | the "Free" wheel (index 4) once per this |

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
* `player_badges` (earned badges.js indices), `challenges` (v7 challenge definitions: type,
  DAT / REW JSON, icon, name; seeded with a starting set), `player_challenges` (PV / PT per
  account, challenge and period: UTC day / week for daily / weekly), `player_medals` (medal
  counts per account): `0107_progression.sql`, player-viewer WP-E part 2c.
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
* text: JSON (KrunkNative clients). Replies use the encoding of the client's most
  recent frame; `?enc=json` makes the greeting JSON too.

On open the brain sends `["pi", null]` (client answers `["po"]`) and
`["io-init", socketId]` (16 hex). Unknown labels are dropped (socialWs.ts). Each
socket's frames run strictly in order: a frame waits until the database work of the
previous one finished (v3.1.1 ran them on synchronous SQLite), so e.g. `["f", ...]`
then `["r", "profile", ...]` sees the follow. Errors
without their own reply label are `["po", null, "message"]` (the page's `io_po`
error popup). Rate limit: 20 frames/s, burst 60 (excess dropped). Pushes that only
the KrunkNative menu understands (`gmc`, `ua`) go to JSON clients only.

Auth (`socialWs.ts` handleAuth / packetHandlers io_a):

| request | reply |
| --- | --- |
| `["a", 0, [name, pass, ref], captcha]` register | `["a", null, id, username, data40, token, info]` or `["a", "error", null, null, null, null]` |
| `["a", 1, [name, pass, null], captcha]` login | same |
| `["a", 1, [name, null, token]]` token resume | same (token unchanged) |
| `["a", -1]` logout | none (session token revoked) |

`info` = `{level, levelProg, alias, premium, funds, score, clan, dev, mod, banned, badges}`
(ignored by v3.1.1). Then `["gmc", unreadMail]` (JSON clients). Error strings:
`Username must be 3–16 characters.`, `Username may only contain letters, digits, and
underscores.`, `Password must be ≥ 6 characters.`, `Password is too long.`,
`Username already taken.`, `Invalid login.`, `Invalid credentials.`, `Please Try
Later`, `Too many failed logins for this IP`, `Banned: <reason>` / `Account banned.`,
`Too many accounts created from this IP. Please Try Later`.

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
| `["kr", name, amount, msg, captcha]` | `["gir", {funds}]`; receiver gets a mail + `gmc` |
| `["cln", tag]` (social page) | `["cln", msg, ok]` join request |
| `["cln", action, value, extra]` (menu) | `["cln", msg, data, ok, action]`: 1 create(tag) -> clanData, 2 request(tag), 3 my clan, 4 leave -> `["clnL"]`, 5 kick(id), 6 role(id, {r}) -> `{i, r}`, 7 funds(kr, {wd}) -> `{pf, cf}` |
| `["clnR", playerId, 1\|0]` | accept / decline a request (errors as `cln`) |
| `["uploadS", settings]` / `["getS"]` | `["pRes", "SUCCESS", null, msg]` / `["loadS", settings]` |
| `["nme", alias]` | `["nmeR", null, alias]` / `["nmeR", err]` |
| `["upp", old, new, repeat]` | `["upp", "Password updated.", newToken]` / `["upp", err]` (other sessions revoked) |
| `["kml", tab]` / `["rml", id]` / `["raml"]` | `["kml", tab, rows{km_id, km_datesent, km_playerfrom, km_subject, km_text, km_archive}]` / `["rml", [row]]` (credits attached KR once) / mark all read |
| `["li", skin, skinid, uid, price]` | `["lir", {cnt, funds} \| {response:"MAX"}, uid]` |
| `["uli", skin, listingId, uid]` | `["ulir", "SUCCESS"\|"SOLD"\|null, uid, err]` |
| `["pi", listingId, uid]` | `["br", {f, i, m} \| null, uid]` (client -> server `pi` is the purchase) |
| `["fil", n, text, rarities, ...]` / `["srt", n, text, rarities, ...]` | `["fr", n, listings\|null]` / `["str", n, inventory]` |
| `["qsi", skin, uid, count]` | `["qsr", {f, c, i: uid} \| null]` (KR = `store.rarities[r].sell`) |
| `["st", skin, days, captcha]` | `["gd", [{d, f: avg price, t: sales}]]` |
| `["ri", name, id, captcha]` | `["trD", name, id, [myInventory, theirInventory]]` |
| `["ctr", receiverId, giveIds, takeIds, msg, captcha]` / `["trA", tradeId]` / `["trC", tradeId]` | `["trR", result, tradeId\|null, msg]`; the other party gets `["pRes", "SUCCESS", null, msg]` |
| `["trCA"]` | decline every open incoming offer |
| `["2fa"]` (logged in) | `["c2fa", null, null, pattern]` / `["c2fa", null, err]`: enables v3.1.1's 2FA (5 random cells of the 5x5 grid, letters of `config.alphabet`; stored as a password hash) |
| password login of a 2FA account | `["c2fa", 1]` (show the grid); then `["2fa", 1, [cells]]` -> `["c2fa", 2]` + the `a` reply, or `["a", "Invalid 2FA pattern.", ...]` + `["c2fa", 1]`. Token resumes skip 2FA; internal `login` answers `2FA required.` |
| `["las"]` | log out every other session of the account |
| `["uploadM", mapJson, thumbDataUrl, description, videoId, category]` | `["pubMR", "Map Published!" \| "Map Updated!"]` / `["upMp", err]`: publishes the map under its JSON `name` (3-30 chars; v3.1.1 catalogue names reserved; republishing your own map bumps `map_updatecounter`); a PNG data URL thumbnail (<= 200 KB) becomes `map_image` |
| `["edMp", name]` | `["edMp", mapJson \| null]` (own maps; devs / creation mods any) |
| `["fvM", id, kind, 1\|0]`, `["ftC", id, 1\|0, kind]`, `["delC", id, kind]` | favourite; feature (devs); delete (creator, devs, creation mods) -> `["po", "SUCCESS", null, "Deleted"]` (kind 0 map, 1 mod, 2 asset) |
| `["uploadMo", ...]`, `["uploadA", ...]` | `["upMp", "File uploads are not supported on this server"(, 1)]` (v3.1.1 posts the files to `/upload-user-mod` / `/upload-user-asset`, not hosted) |
| `["spin", wheel(, twitch)]` | `["spin", null, randNum, skinIndex, funds, 0]` / `["spin", err]`: v3.1.1 `store.wheels` (0 Starter 50 KR, 1 Elite 100, 2 Heroic 500, 3 Valour locked, 4 Free (lvl 5, cooldown), 5 Free KR -> `fkr`, 6 Twitch refused); rarity roll and `getSpnItems` like `io_spin`; the skin goes to the inventory |
| `["fkr", captcha]` | `["spinKR", lastReward, randNum, rewardIndex, funds]` / `["spinKR", null, null, null, null, err]`: `store.freeKR[i].kr[level >= 20]`, once per `reward_time_s` |
| `["clm"]` | `["clm", lastReward, funds]` / `["pRes", null, err]`: daily reward (level >= `reward_min_level`, `daily_reward_kr`, same clock as `fkr`); `["unbx"]` is accepted |
| `["rpt", name, reason, details]` / `["grp", name]` | `["rpr", ok]` / `["rps", rows]` (staff) |
| `["chg"]` | `["chgL", activeChallenges]`: `[{CI, T, DAT, REW, IC, N, PV, PT, TR?, PER}]` (v7 `account.activeChallenges`; TR = ms left of a daily / weekly one; 3 daily + 3 weekly rotate per account and UTC day / week) (WP-E part 2c, `progress.c`) |
| `["bdg", badge]` | `["bdgr", badge]` / `["po", null, "You dont own this badge"]`: `player_stats.bdg` (-1 clears; v7 io_badgeUpdate) |
| `["cbdg", badge]` | `["cbdgr", badge]` / `["po", null, err]` ("Badge not claimable", "Requirements not met", "Badge already claimed"): a claimable badge whose rule holds (Nuclear I-IV: `stats.n`) |
| `["mdl"]` | `["mdl", {medal: count}]`: the account's medal counts (KrunkNative) |
| `["flg", name, 0\|1]` / `["ver", name, 0\|1]` | `["fgr", ok]` (mods) / `["verr", ok]` (devs) |

Server pushes: `["ua", data40, info]` (v3.1.1 `updateAccount`) after a stats commit
of that account; `["gmc", n]` unread mail. After a commit with v7 progression fields (7.6):
`["chg", [done, CI, PV, ...]]` (v7 io_updateChallenges), `["chgC", [challenge...]]` (the
completed challenge objects: the end screen's cards and reward skins), `["chlR", skins, funds,
0, 0, kr]` (io_updateChallengeRewards) and `["nbdgr", [badge...]]` (checkBadges).

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
servers hosting community maps; 404 `{"error":"not_found"}`.

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
`prog` (`[[key, value, weapon, streak], ...]`, the match's challenge tallies) or `medals`
(`{name: count}`) is present and the commit is not a duplicate, kn_brain then runs
`P_PROGRESS` (`db_progress.c`): v7's challenges.finalize on the account's active challenges
(`final: false` = a player who left: only `DAT.nem` ones), KR / skin rewards (`kr_transactions`
source `challenge`), the medal counts, and checkBadges (claimable badges whose rule holds), then
pushes 7.4's match-end frames.

### 7.7 Tests

* `ctest -R brain_social_ws` (`services/brain/tests/social_ws_test.py`): starts
  `kn_brain --migrate` on a fresh schema `kn_test_accounts` of database
  `krunknative_svcacc` (override `KN_TEST_DB_NAME` / `KN_TEST_SCHEMA`), then drives
  the social socket in msgpack (own encoder, independent of knsvc) and JSON and the
  internal WS: validation strings, register/login/throttle/token resume/logout,
  profile, follows, search, feed, settings, display name, clans, chat + push,
  leaderboards, gift + mail, market list/buy/quick sell, trades, stats commit
  idempotency + `ua` push, password change revocation, map publishing + `/api/*`
  catalogue, 2FA, prize wheels / Free KR / daily reward, elo deltas. One schema per run (`kn_test_accounts_<pid>`, dropped after),
  brain on an ephemeral port (read from its "listening on" log line), no sleeps
  except the login throttle window: 50/50 sequential and concurrent runs pass.
  Skips (77) without PostgreSQL or python `websockets`.
* `ctest -R brain_progress` (`services/brain/tests/progress_test.py`, every driver): the
  challenge list and its daily / weekly rotation, a commit with tallies -> the match-end pushes,
  duplicate and partial commits, badge select / claim refusals and the auto-awarded Nuclear I,
  medal counts, `badges` in the account object (WP-E part 2c).
* Real v3.1.1 client: `services/brain/tests/v311_site_proxy.js <port> <brainPort>
  <v3.1.1 public dir>` (same-origin front like nginx) +
  `v311_social_client.js <port> <cdpPort> <user> <pass> <outDir>` against headless
  Chromium: the untouched v3.1.1 `social.html` logs in through its own popup,
  resumes by token, posts, and renders profile, feed, leaderboards and the 36-map
  catalogue from the brain.
* KrunkNative web client: client-integration's build (`?brain=<brain url>`)
  registers and logs in through the menu's account window in headless Chromium.

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
| `region` | `local` | brain region id (v3.1.1 `MM_NODE_REGION`): the 2-letter code shown in the server browser (`SV`, `LA`, `CH`, `VA`) |
| `node_id` | `LOC` | node id (v3.1.1 `MM_NODE_ID`), prefix of the lobby id `"<node_id>:<5 chars>"`; no `:` |
| `name` | `KrunkNative` | server name (hello, lobby meta `name`) |
| `listen_host` / `listen_port` (alias `port`) | `0.0.0.0` / `21015` | TCP bind address / port |
| `public_host` / `public_port` | `127.0.0.1` / `0` (= listen port) | where clients connect (lobby meta `host`/`port`) |
| `brain_http_url` | `""` (= `http://<brain_url host:port>`) | brain HTTP base for published maps (`GET /api/map-data?name=`) |
| `web_url` | `""` | optional ws(s) URL web clients use (net-bridge `/native?...`), lobby meta `webUrl` |
| `brain_url` | `""` | `ws://<brain>/mm/internal/ws`; empty = standalone; `"auto"` = this config's brain (`ws://<brain.host>:<brain.port>/mm/internal/ws`, `0.0.0.0` dialled as `127.0.0.1`, honours `--set brain.host/port=` and `KN_BRAIN_HOST/PORT`). `KN_GAMESERVER_BRAIN_URL_FALLBACK` (backend-up sets `auto`) applies only when no `brain_url` was configured anywhere |
| `brain_secret` | `kn-dev-secret` | Bearer secret (= `brain.internal_secret`, same dev default); never logged. Set a real one in deployment |
| `join_secret` | `""` (= `brain_secret`) | HMAC key of seek-game join tickets (= `brain.join_secret`) |
| `require_ticket` | `false` | brain-linked: refuse connections without a ticket or session token |
| `allow_guests` | `true` | brain-linked: allow players without an account (ticket `acc` 0 / no JOIN) |
| `join_timeout` | `5` | seconds a brain-linked server waits for JOIN before treating the player as a guest |
| `tick_rate` | `30` | simulation / sync rate (Hz) |
| `max_players` | `8` | lobby size (v3.1.1 `globalConfig.maxPlayers`), 1-31 |
| `mode_max_players` | `{}` | per-mode lobby size, keys = mode alias/name/id (`{"tdm":10}`) |
| `team_size` | `0` | team modes: lobby size = 2 × team_size (0 = off) |
| `mode_team_size` | `{}` | per-mode team size; `mode_max_players` wins over team sizes |
| `maps` | `[]` (Krunker rotation) | rotation: map catalogue names, keys or indices (`shared/include/map_catalog.h`: `"Burg"`, `"311_burg"` (the v3.1.1 Burg), `"152_burg"`, `"13"`; the old keys `"v3_burg"` / `"old_burg"` still resolve) |
| `modes` | `["ffa"]` | rotation modes (aliases/names/ids) or `"rotation"` (Krunker's public mode rotation) |
| `custom_map_dirs` | `[]` | extra directories of map JSONs appended to the rotation (catalogue keys are skipped) |
| `round_seconds` | `0` | round length override for every mode (0 = the mode's game time) |
| `mode_round_seconds` | `{}` | per-mode round length (`{"ffa":300}`) |
| `game` | `{}` | v3.1.1 host config (`src/config.js serverConfig` varNs, clamped the same): `gameTime`, `minPlayers`, `warmupTime`, `forceSpawn`, `lives`, `scoreLimit`, `gravMlt`, `jumpMlt`, `deltaMlt`, `slidTime`, `impulseMlt`, `wallJP`, `strafeSpd`, `healthMlt`, `fiRat`, `reSpd`, `t1Dmg`, `t2Dmg`, `canSlide`, `airStrf`, `autoJump`, `bDrop`, `hpRegen`, `killRewards`, `headshotOnly`, `noSecondary`, `noStreaks`, `disableB`, `throwMel`, `selTeam`, `allowSpect`, `thirdPerson`, `nameTags`, `nameTeam1`, `nameTeam2`, `maxPlayers` (= `max_players` when that is not set) |
| `custom_games` | `true` | accept hosted custom games from the brain (hello `custom`) |
| `motd` | `[]` | server chat line(s) sent to every player on their first spawn (`$` = their name) |
| `join_message` / `leave_message` | `"$ joined the game"` / `"$ left the game"` | v3.1.1 `config.serverMessages[0/1]`, `""` = off |
| `billboard` / `billboards` / `billboards_file` | none | billboard ads (below) |
| `idle_kick_seconds` | `60` | v3.1.1 `SERVER_IDLE_TIMEOUT_MS`, 0 = never |
| `idle_move_threshold` | `1` | units a player must move to count as active |
| `perms` | `false` | v3.1.1 `PERMS`: DEV chat commands only for ops and loopback |
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
`init` packet's `bill` slot; the client's `game.updateBillboards` paints it on every
map object `BILLBOARD` with `sad` ("show ad"), defaults `"Your Ad Here"`, `#e3e3e3`,
`#000000`. Here: `"billboard": "text"` or `{txt,txtCol,bgCol}`, `"billboards": [...]`
(strings or objects) and/or `"billboards_file": "<v3.1.1 billboards.json>"`; one entry
per round, in order. The server sends `NET_PACKET_BILLBOARD` before every INIT; the
client (`prefab.c prefab_ad_texture`) draws the text at the largest fitting size on
the background colour. No billboard configured = no packet = the defaults.

### 8.2 Native protocol additions (`shared/include/shared.h`)

| packet | dir | struct | when |
| --- | --- | --- | --- |
| `NET_PACKET_BILLBOARD` (33) | S→C | `{char txt[64]; char txt_col[8]; char bg_col[8];}` | before every INIT, when configured |
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

Error codes: `004001` invalid/expired ticket or session, guests refused; `004002`
logged in elsewhere; `004003` banned; `004004` server draining (`"GAME UPDATING"`).

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
  "<mode>_<map>"`, `earnKR: false`) and `host-reply {ok, lobbyId}` follows. When the
  host leaves after players joined ("Host ended game" chat line, v3.1.1
  `HOST_ENDED_MESSAGE`), the lobby empties, or nobody joins within 60 s, it reverts to
  the public rotation. `passCode` is not enforced (the native JOIN has no password).
* **Published (community DB) maps:** `config.customMap` (alias `mapName`) names the map to
  host. A map this server has (its own list incl. custom dirs / the command-line map, then
  a catalogue entry whose name or key is exactly that) is used directly; otherwise
  server_bin fetches `GET <brain_http_url>/api/map-data?name=<customMap>` (svc-accounts;
  worker thread, 10 s timeouts, <= 4 MiB, sanitised like v3.1.1 `sanitizeCustomMapJson`)
  and only then answers `host-reply` (404 -> `{"ok":false,"err":"map not found"}`).
  Clients that lack it get it through MAP_INFO / MAP_REQUEST / MAP_DATA as usual; the
  lobby meta is `i: "<mode>_<map>"`, `m: <map>`, `cs: true`.
* `drain` from the brain: chat notice, refuse new connections (`GAME UPDATING`),
  finish the round, then `lobby-down`.
* SIGTERM / `quit`: partial commits for everyone in the round, `drain` + `lobby-down`,
  then up to 3 s for outstanding `commit-reply`s before the socket closes.

### 8.6 End-screen reward, ranked lobbies, tests

* **Reward column.** `game_end_round` sets `player.reward` through `GameHooks.reward`
  (server: `accounts_reward`, the 8.4 KR rule; NULL = 0 offline / for guests) and the END
  packet carries `int32 rewards[16]` parallel to `board.entries` (`NET_END_PACKET_V1_SIZE`
  = an END without it, still accepted). The client shows it like v3.1.1's
  `config.endForm.reward`: `<span style='color:#F8C55C'>+N</span> KR`, `NONE` for 0, in
  the end table (`hud_modes.c`, every mode's `endStats`) and as the Performance tab's
  funds; the stats commit's `kr` is the same `player.reward`.
* **Ranked** (brain `host` with `config.ranked`, section 6.5). v3.1.1 `isFromQueue` rules:
  only the roster's accounts may join (ticket `acc` or session; others get `ERROR "This
  is a ranked match"`), each spawns on its queue team (`teams[0]` → team 1, `teams[1]` →
  team 2, `players.setTeam`), the lobby is `cs`/private with `meta.ranked` + `matchId`,
  `minPlayers` = roster size so the mode's countdown waits until everyone is in
  (`needAll`); after `startGraceS` without everyone it starts with whoever is there when
  both teams have someone, else ends. A roster player who leaves before the end screen
  ends the round (`leaveGame` → `endGame(3)`) and is an abandoner (`checkLeave`), as are
  no-shows; the team that stayed wins, else the winner is the END's didWin. One round
  (`infEndTimer`): `ranked-result {matchId, lobbyId, winner, players[{accountId, team,
  score, kills, deaths}], abandoned}` goes out once (reliable), the reply's rating
  changes are logged and sent to each player as a chat line, and the lobby returns to
  the public rotation at the end of the end screen. The normal `commit` is sent too
  (ranked modes are `noReward`: time played only, like `updateAccounts`).
* **Tests** (real hits, real DB, headless clients; each creates and drops its own
  database): `server/tests/gunfight.sh [build]` — two accounts on the open
  `server/tests/arena.json`, `KRUNKNATIVE_TEST_AIM` + an input script (trigger held,
  strafes, `respawn` after every death) → checks kills/deaths/score/`kr` in
  `match_results`, `players`, and `player_funds` = sum of rewards; `KN_TEST_SHOT=file`
  captures the end screen with the Reward column. `server/tests/ranked1v1.sh [build]` —
  both accounts queue `r1v1` over `/mm/queue/*`, the brain hosts the match, the clients
  play to 10 kills, checks `mm_ranked_matches` / `players.player_elo`;
  `KN_TEST_ABANDON=1` makes one player quit mid-match (abandoner, other team wins,
  cooldown row).

Not ported: the game-socket account ops of v3.1.1 (`io_a` login/register over the
game socket: the native client logs in over the social WS), `host-request` from a
player inside a lobby (the native protocol has no `custom` packet yet), ELO /
challenges / class score stats (svc-accounts `elo` / `challenge` ops exist).

### 8.7 Hosting a server (dedicated server exe)

Hosting is running `server_bin`, like a CS:Source dedicated server. The client has no
Host Game flow any more (section 8.11). One process is one lobby; a network is many
processes (8.10).

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
| `rcon_password` / `rcon_port` / `rcon_host` | `""` / `0` / `127.0.0.1` | local TCP rcon (off unless both set) |
| `query_port` | `0` (= game port) | UDP server-info query (8.8) |
| `online_mode` | `false` | verify every player with central auth (8.9); guests refused |
| `auth_url` | `https://coordinator.krunk.io` | central auth base (`/auth/hasJoined`) |
| `finder_url` | `""` | opt-in public listing heartbeat (8.8), e.g. `https://matchmaker.krunk.io` |
| `community` | `""` | community name in lists (`""` = the coordinator's welcome `community`, else its brainId) |
| `tags` | `[]` | extra info-query tags |
| `enforce_skins` | `auto` | cosmetics must be owned (8.9); auto = on while linked to a coordinator |
| `shutdown_grace` | `10` | seconds a SIGTERM/SIGINT drain lets the round finish |
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
`send` (8.10), `drain`, `quit`, `help`, and every Krunker chat command without the slash.

**rcon**: the same commands, their output captured per command
(`cli_run_command`). Local TCP: line 1 = password (`OK` / `ERR bad password`, 1 s delay),
then one command per line, output lines, then a line `.`; bound to 127.0.0.1 unless
`rcon_host` says otherwise (no TLS: tunnel it). Through the coordinator: internal-WS
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
  "commit","commit_time","build_time","protocol"}` (`version` = the build's git describe; 8.12).
  Tags: `online`/`offline`, `password`, `ranked`, `custom`, `draining`, `community`
  (linked to a coordinator), then `tags`. Snapshot refreshed 4x a second by the tick.
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
  online_mode 1, public_key "", auth_url}`. The client posts `/auth/join {token,
  serverHash}` to central and JOINs with `client_key = "hj1:<client nonce>"`, `name` =
  username (a JOIN without `hj1:` is ignored: the client's first JOIN goes out before it
  saw the challenge). `serverHash = hex(SHA-256("kn-join-v1" 0 server_nonce 0 public_key 0
  client_nonce))` (`shared/include/auth_join.h`). Without a coordinator server_bin calls
  `GET <auth_url>/auth/hasJoined?username=&serverHash=&ip=` itself on a worker thread
  (`server/src/central_auth.c`): 200 -> that account id/profile (stats are session-only:
  nothing is committed without a coordinator); 204 -> `ERROR "Failed to verify username"`;
  unreachable -> `"Authentication servers are down"`. Guests are refused.
* **Linked to a coordinator**: joins as 8.3 (seek-game ticket and/or session token). In
  online mode the `hj1:` JOIN goes to the coordinator as `{"type":"auth","rid",
  "hasJoined":{"username","serverHash","ip"}}`; it verifies with central, creates the
  community account on first join and answers the normal `auth-reply`.
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
  `server/tests/online_mode_e2e.py BUILD` (auth role + standalone online server:
  challenge, `/auth/join` + `hj1:` join, unannounced hash and guests refused).

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
is a direct connect (guest, or online-mode join again). The client
(`servers_ui_redirect`, `game_events.c`) shows `reason` as a toast and switches; the
web build reloads with `?game=`. Lobby-to-lobby moves therefore always go through a
fresh seek-game ticket; there is no server-to-server state hand-off (the stats of the
old server are committed as a partial on disconnect).

### 8.11 No hosting from the client

Removed: the menu's Host Game button (`#menuBtnHost` hidden with `display:none`, kept in
the DOM for v3.1.1 parity; `--render-scenario` keeps it visible for uidiff), the
`openHostWindow` handler (now an alert "run the dedicated server: server_bin"),
`createPrivateRoom` -> `/mm/host-game`, `playMap` -> `/mm/host-game` (a map no server
runs is played offline), and `servers_ui_host_game()` returns 0 (debug only:
`KRUNKNATIVE_ALLOW_CLIENT_HOST=1`). The Servers window's "+ Host Game" link is replaced by
direct connect / community servers (coord-auth). Kept: offline / practice play
(`local_game.c`, "play offline" on the error screen, editor Test Map), the server
browser and join. `/mm/host-game` and node `host-request` stay on the coordinator for
admin tooling (6.3); clients never call them.

### 8.12 Build version and outdated servers

Every binary embeds its build identity (`shared/include/build_info.h`, generated
`kn_version.h` from `cmake/kn_version.cmake` at every build): git describe (`--match
'server-v*' --match 'v[0-9]*' --exclude '*-parity*' --always --dirty`), 12-hex commit, its
committer date, the build time and the protocol `NET_PROTOCOL_MAJOR.MINOR`. CI builds without
git pass `-DKRUNKNATIVE_VERSION_{DESCRIBE,COMMIT,COMMIT_TIME,BUILD_TIME}`. Shown by
`server_bin --version` / `client_bin --version` / `kn_brain --version`, the server's startup log
and `status`, the KNINFO reply and finder heartbeat (8.8), the coordinator `hello` (5.2) and every
lobby tuple's meta (`proto`, `build`, `commit`, `commitTime`). The INIT packet is unchanged
(fixed size; a tail would need a protocol bump).

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
4. `servers.latest_version` is set and neither the server's describe nor its commit match it.

**Incompatible**: the server announces another protocol MAJOR than the client (`?proto=M.m`;
clients without it count as the coordinator's major). Minors interoperate.

Exposed per entry as `outdated` / `incompatible` (finder `GET /servers`, `meta` of `/mm/game-list`
tuples) plus top-level `latest` `{version, commit, commit_time, build_time, protocol, tag}`,
`download_url` (release page, default `https://github.com/rebel10n/KrunkNative/releases/tag/server-latest`)
and `downloads` `{linux, linux_bullseye, windows, macos}` (asset URLs). Matchmaking
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
| Register / Login (window 5) | `["a",0,[name,pass,null],null]` / `["a",1,[name,pass,null],null]` -> `["a",err,id,name,data40,token,info]`; error text in `#accResp` |
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
2. `GET /mm/seek-game?hostname=krunknative&region=R&autoChangeGame=true[&game=ID][&token=T]`
   (web: `?game=` of the page; native: `KRUNKNATIVE_GAME`). Errors -> the v3.1.1
   `matchmaker.*` texts on the connection error screen; its retry link seeks again.
3. Connect: native TCP to `host:port` (names resolved per attempt); web
   `webUrl` if set, else `ws(s)://<origin>/native?target=host:port`. Right after HELLO
   both send `NET_PACKET_JOIN {game_id, client_key = clientId, session, name}`
   (`client/src/net/net_join.c`).
4. `GET /mm/game-info?game=ID` -> `#menuRegionLabel` = `regionNames[region]`.

Servers window (2): `GET /mm/game-list` + `GET /api/player-count` (cached
`serverBrowserRate` 10 s), region groups, Custom Games group (`meta.cs`), search, Hide
Full, Quick Join (`quickJoinRegion`: up to 10 populated lobbies, else a region seek),
`checkedSwitchServer` (`game-info` first, FULL/ERROR). Switching: native re-seeks and
reconnects in place; web reloads with `?game=<id>` like v3.1.1. Games window (10):
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
| 2FA enable | act2FA (no markup in the shipped page: KrunkNative adds a row to the Account window's Settings tab) | `["2fa"]` -> `["c2fa",null,null,pattern]` |
| Daily reward (level >= 5) | cK / dd (#claimHolder timer, "Play 1 Game" until a round ended), claimReward (ad-gated upstream) | `["clm"]` -> `["clm",lastReward,funds]` |
| Prize wheels (Store tab 0) | windows[13] tab 0, prizeWheel, spinButton/spinUI onclick, spinResponse | `["spin",i]` -> `["spin",null,rand,skin,funds,follow]`, `["unbx"]`. The canvas wheel is a white disc (no 2D canvas natively); the won item shows its preview PNG (`textures/previews/` from the asset tree, `make assets-import`; noimg fallback) |
| Ranked (27) | windows[26], joinQueue / leaveQueue / updateQueueStatus, QueueManager | `/mm/queue/join` (token, region, queues, party, ready, classIndex), `/mm/queue/poll` every 1 s, update-ready / update-queues / update-class / kick / promote / leave; "Join" `{gameId,host,port,webUrl,clientId}` is joined with that ticket. Status under CLICK TO PLAY |
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

`account`/`profile` = `{id, username, alias, premium, dev, mod, banned, ban_reason}`. Join
records live in memory for 30 s, keyed by serverHash. `auth.*`: `login_per_min` (10),
`register_per_hour` (5), `join_per_min` (60), `has_joined_per_min` (1200) per IP, `check_ip`
(false: also match the joining IP).

**Join handshake** (`shared/include/auth_join.h`): `serverHash = hex(SHA-256("kn-join-v1" 00
server_nonce 00 public_key 00 client_nonce))`. An online_mode `server_bin` sends
`NET_PACKET_AUTH_CHALLENGE` (73, `NetAuthChallengePacket {server_nonce[33], online_mode,
public_key[65], auth_url[128]}`) after HELLO; the client POSTs `/auth/join` to its own account
service, then sends JOIN `{name = username, client_key = "hj1:" + client_nonce}` (no session
token: tokens never reach game servers outside brain matchmaking). The server verifies with
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
Servers" (offline mode hides online_mode servers) and has a Direct Connect `host[:port]` field in
place of "+ Host Game". Native https/wss use OpenSSL libssl when CMake finds it
(`KRUNKNATIVE_HAVE_TLS`, system CA store, host name verified); web uses `fetch()`
(`backend_http_request`).

**Tests**: `brain_auth_finder` (unit: SHA-256 vectors, serverHash construction, join TTL /
user / hash / IP checks, limiter windows, finder nonce verification, filters, per-IP cap, poll
failures and heartbeat expiry); `brain_auth_e2e` = `services/brain/tests/federation_auth_e2e.sh
<build>` (auth+community, finder, a third-party community coordinator, an online_mode server linked
to it and an offline standalone one, headless clients in both modes, rcon relay, replay check).

## 10. Deployment

`deploy/` (svc-matchmaker); details in `deploy/README.md`.

* Local: `make backend-up` (`deploy/local/backend.sh up`): `kn_brain --migrate-only`,
  `kn_brain` on 127.0.0.1:5050, `GAMESERVERS` (2) x `server_bin --brain ...` on
  21015.., `net-bridge --web` on 8080 when `dist/web` exists (game servers then
  advertise `webUrl = ws://127.0.0.1:8080/native?target=127.0.0.1:<port>`).
  `make backend-status`, `make backend-down`; `make backend-run` is svc-core's
  foreground variant. Knobs: `BRAIN_PORT GAME_PORT BRIDGE_PORT REGION BRIDGE
  FAKE_NODES KN_RUN_DIR BACKEND_ARGS`.
* systemd: `kn-brain.service` (migrations in ExecStartPre), `kn-gameserver@<NODE>`
  (`/etc/krunknative/gameserver-<NODE>.env` = `KN_GAMESERVER_*`), `kn-net-bridge`,
  `krunknative.target`.
* nginx (mirrors the jail's `ansible/templates/nginx/*.j2`): brain host
  `matchmaker.<domain>/<x> -> /mm/<x>` (incl. `/internal/ws` upgrade),
  `api.` -> `/api/`, `social.` -> `/social/`, `play.<host>` = web client + `/native` +
  all routes same-origin; region host = web client + `/native` + `/ping`. TLS via
  certbot, CORS snippet like the jail's `cors.conf`.
* Multi-region: `deploy/deploy.sh [-n] [--certbot] push|status|render|drain` over
  `deploy/inventory` (`<host> region= brain=1 gameservers=N first_port= node_prefix=
  bridge=1 nginx=brain|node brain_url=`). One shared PostgreSQL; one brain per region
  (game servers reach it over plain `ws://`, local or private network).
