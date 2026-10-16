# Matchmaker token, seek preferences and official krunker.io lobbies

How Krunker's HTTP matchmaker token flow works (static reading, nothing executed), how
Krunker Box Office implements the same flow on kn_brain + server_bin, the v10 seek preferences
(Settings > General > Quickplay), and the native client's list of official krunker.io lobbies.

Citations: `V` = `~/.cache/kn-v10/v10.js` (v10.0.2 client, identifier-renamed, original line
layout; it labels itself `10.0.1r1`, V:16405-16406), `S` = `~/jails/kru/src/sketch/` (a
reconstruction of Krunker's WASM loader).

## 1. Krunker's flow

```
 loader                     client (v10)                          matchmaker.krunker.io        game server
   | GET /generate-token  ------------------------------------------------>|
   |<---------------------- text (opaque, ~55 chars) ----------------------|
   | game(token) -> WP_MMToken                                             |
   |                          GET /seek-game?hostname&region&autoChangeGame=false
   |                              &validationToken=<token>&dataQuery={v,dsmo,dsma}... ->|
   |                          <- {host, port, gameId, clientId, data} ------|
   |                          wss://host/ws?gameId=<id>&clientKey=<clientId>&... --------------------> |
```

### 1.1 The token

* Who fetches it: the WASM loader, before the game code runs. Sketch's port fetches
  `https://matchmaker.krunker.io/generate-token` with no credentials and returns the trimmed text
  (S/src/loader/token.ts:1-8), and calls the game body with it as `arguments[0]`
  (S/src/loader/execute.ts:37-38, 63-65). The game prologue does `var WP_MMToken = arguments[0]`
  (S/src/index.ts:88-90). The seek fails with "socket error" when the token is missing
  (S/AGENTS.md:71-76, S/README.md:178-180).
* v10 uses `WP_MMToken` unchanged as `validationToken` (V:197520, V:222033-222034). v7.1.4 shifted
  each character by -5 first (`Krunker_v7.1.4/src/index.js:11-14`).
* Endpoint: `GET {matchmakerURL}/generate-token`, a plain `fetch` with no headers. The text is
  returned when the response is OK, else `null` (V:147557-147565). `matchmakerURL` is
  `https://matchmaker.krunker.io` (beta `matchmaker.beta.krunker.io`, local `http://127.0.0.1:5050`)
  (V:16439).
* Refresh: the client fetches a fresh token only for the captcha retry. On `CaptchaRequired` it
  gets a Turnstile token, then a new `/generate-token`, and seeks again, at most twice
  (V:147430-147448).
* Format and lifetime: opaque text. Sketch logged one 55-character token (S/console.log:13). The
  client never decodes, signs or checks the token. The matchmaker's error names show what it
  checks (V:147673-147693):
  * `InvalidClientValidationToken` ("Socket Error")
  * `ExpiredClientValidationToken` ("Validation Error"): a lifetime
  * `InvalidClientIP` ("Socket Mismatch"): bound to the client's address
  * `InvalidClientValidationTokenData` ("Data Error")

  The server side is not public, so the exact lifetime and signature are unknown. Sketch's
  LOADER.md:14, 46-48 claims a `gapi.svc.krunker.io/generate-token` and HMAC-SHA384, but its own
  code (S/src/loader/token.ts:1) and v10 (V:147560) use the matchmaker host. The HMAC/SHA384
  markers belong to the core.dat cipher (S/src/loader/signature.ts:47-53). Treat that claim as
  unverified.

### 1.2 seek-game

`GET /seek-game` through `matchmakerRequest` (V:147626-147653). Null parameters are dropped. The
only header is `accept-language: en-US,en;q=0.6;`. A non-OK answer throws
`{message, response: {data}}`. Parameters (V:147601-147611):

* `hostname`: `__HOSTNAME__`, else `location.hostname` (V:147387-147390)
* `region`: `__REGION__`, else the caller's `regionId`, else `getRegion()` (V:147469-147507): the
  cached `PING_REGION_CACHE_KEY` (`pingRegion7`) when still available, else GET `/ping-list`
  (`{region: "host[:port]"}`) and every available, non-dedicated-only region's `/ping` fetched at the
  same time (`pingFetch`, 5 s abort); the FIRST to answer is the region and is cached
  (`setDefaultRegion`). No answer at all rejects the seek ("All pings failed."). KrunkNative
  (`region_ping.h`): the same race for krunker.io (cached as `knKrunkerPingRegion`; all pings failing
  seeks in `de-fra`, the list's first, for a minute) and for the brain's `/mm/ping-list` (cached in
  `pingRegion7`, marked by `knPingRegionPinged`; all failing = the list's first)
* `autoChangeGame`: the callers pass `false`
* `validationToken`
* `game`: when a game id is known (`?game=`)
* `excludeGame`: `?exclude=` (Quick Match, V:222057-222058)
* `dataQuery`: JSON (below)
* `captchaToken`
* `accessToken`: the FRVR token (V:147399-147424)

`dataQuery` is `{v: "<build key>"}` (V:222035-222036, constant V:20115), plus:

* `i`: the `?play=` map (V:222041)
* `dsma`: the saved `seek_maps`
* `dsmo`: the saved `seek_modes`

`seek_maps` and `seek_modes` are comma lists of map and mode indices read through `getSavedVal`
(V:222011-222016, 222051-222055). v10 has no settings UI for them. When none match, the matchmaker
answers `NoMatchingServers`, whose English text is "No Games Found / Try changing Quickplay
options" (i18n `matchmaker.NoMatchingServers`).

The answer is `{host, port, gameId, clientId, data}`. The client connects with
`clientKey = clientId` (V:222068-222074) over
`wss://<host>/ws?gameId=&clientKey=&clientUID=&bpt=&tc=&at=&dm=` (V:222104-222126). The
validation token never reaches the game server. The client keeps no record of how the game
server checks `clientKey` (nothing in V or S).

### 1.3 game-list (the server browser)

`GET /game-list?hostname=<host>` (V:147575-147581; direct fetches at V:107131, 215633) answers
`{games: [...], totalPlayerCount}`. Each game is
`[gameId, region, players, maxPlayers, data, secondsLeft]` (V:211417-211436). The `data` keys:

* `i`: map name
* `h`: host or custom game name
* `g`: mode index
* `c`: custom
* `cm`: custom map id
* `oc`: official custom
* `ds`: 24/7
* `v`: build key
* `pr`: priority
* `kr`: KR badge
* `pw`: password

Region names come from `_7517` (V:18672-18701, e.g. `us-nj` = New York). Clicking a row calls
`checkedSwitchServer(id)`, which asks `/game-info` and then navigates to `/?game=<id>`
(V:197742-197766, 147545-147548). Game ids are `<PREFIX>:<code>`, e.g. `NY:2zqse`.

## 2. Krunker Box Office

The same three steps, against kn_brain and server_bin:

| step | Krunker | here |
| --- | --- | --- |
| token | `GET matchmaker/generate-token`, text | `GET /mm/generate-token` (alias `/generate-token`), `text/plain`, CORS + no-cache like every brain route. Rate limit: 120 a minute per address (`hot_limit "mm_token"`), then 429 `TooManyRequests` |
| seek | `/seek-game?...&validationToken=` | `/mm/seek-game?...&validationToken=` (doc/backend.md 4), checked and spent before a seat is reserved |
| join | `clientKey` = `clientId` | `NetJoinPacket.client_key` = `clientId`: the HS256 join ticket (doc/backend.md 5.3), verified locally by server_bin with `gameserver.join_secret` |

### 2.1 The validation token (`services/brain/mm_token.c`)

* It is a compact JWS (knsvc `kn_signed_token`, HS256) with the claims
  `{"typ":"mmv","jti":<12 random bytes, hex>,"ip":<first 12 hex of sha256(client address)>,"iat","exp"}`,
  about 215 characters.
* Key: `HMAC-SHA256(brain.join_secret, "kn-mm-validation-v1")` as hex, or the same over
  `brain.validation_secret` when that is set. Every brain and game server of a network already
  shares `join_secret`, so any brain accepts a token another brain issued, and the rollout needs
  no new secret. The derived key differs from the join ticket key, so a join ticket is never a
  validation token, and the reverse.
* Lifetime: `brain.matchmaking.validation_ttl_s`, default 60 s, clamped to 5..3600.
* Refusals, all 403 with Krunker's names, before any seat or ticket is handed out:

  | case | error |
  | --- | --- |
  | bad signature, malformed, or already spent | `InvalidClientValidationToken` |
  | `exp` passed | `ExpiredClientValidationToken` |
  | signed with our key but not a validation token | `InvalidClientValidationTokenData` |
  | issued to another address (with `validation_bind_ip`) | `InvalidClientIP` |
  | none, while `require_validation_token` is on | `InvalidClientValidationToken` |

* Replay: a token is spent by the seek it validates. Each brain keeps an in-memory table of spent
  `jti`s (8192 slots, kept until their `exp`). A token used at brain A could still be used once at
  brain B within its lifetime. That buys B one ticket for a seat, nothing more.
* IP binding: `brain.matchmaking.validation_bind_ip`, off by default. Behind nginx it needs
  `brain.trust_proxy`, and dual-stack clients can change address between the two requests, so it
  is an opt-in hardening. `::ffff:a.b.c.d` counts as `a.b.c.d`.

### 2.2 The join ticket on the game server (unchanged format, one new rule)

* server_bin verifies the ticket's signature, `exp`, `nid` (its node) and `lid` (its lobby): a
  forged or foreign ticket is `Invalid join key` (004001), an expired one `Join key expired`.
* Unchanged: the resume of an expired ticket the lobby already admitted (doc/backend.md 8.3).
* New, replay: a join ticket is one seat. A second connection with the same ticket (a fresh one
  too, not only a resume) replaces the first (004002 "Logged in from another location"), so one
  seek can never seat several players.

### 2.3 Client (`client/src/ui/servers_ui.c`)

1. Every seek first sends `GET /mm/generate-token`, then
   `/mm/seek-game?...&validationToken=<token>` (`seek`, `on_token`).
2. If the token is refused on the way (403 with one of the names above), the client fetches a
   fresh token and seeks once more. A second refusal is shown with v10's texts ("Socket Error",
   "Validation Error", "Socket Mismatch", "Data Error").
3. Degrading: a brain from before the token answers 404 to `/mm/generate-token`; one without a key
   answers 503; a rate limit answers 429. In all three cases the client seeks without a token,
   which such a brain accepts.
4. If the brain is down, the token request fails the same way the seek used to:
   "Could not reach the matchmaker", and restart rejoins keep retrying.
5. Offline and sandbox play never call the matchmaker, so nothing changes there.
6. The ranked queue's tickets (`/mm/ranked/*`) and the admin-only `/mm/host-game` are not part of
   this flow.

### 2.4 Rollout

1. This release: brains issue and check tokens, but `brain.matchmaking.require_validation_token`
   stays `false`.
   * New clients send tokens. Old clients (no token) are still served.
   * A new client facing an old brain gets 404 on `/generate-token` and seeks without a token.
   * server_bin needs nothing new: the join ticket is unchanged.
2. When the old clients are gone (the launcher updates them), set
   `KN_BRAIN_MATCHMAKING_REQUIRE_VALIDATION_TOKEN=true` on kru (and any other brain).
   * Seeks without a token then get 403 `InvalidClientValidationToken`.
   * The flag can be turned back off at any time.
3. Optional: `brain.validation_secret` (`KN_BRAIN_VALIDATION_SECRET`, 32+ random characters, the
   same on every brain) separates the validation key from `join_secret`. Without it the key is
   derived from `join_secret`.

Tests:

* `kn_brain_mm_token_test` (ctest `brain_mm_token`)
* `kn_brain_registry_test`
* `server/tests/mm_token_e2e.py`:
  * protocol P1-P9: the token, single use, forged, expired, optional, required on a second brain,
    `dsmo`/`dsma`, `NoMatchingServers`, a forged / expired / reused join ticket
  * clients C1-C5: below

## 3. Seek preferences (Settings > General > Quickplay)

v10.0.2 reads `seek_modes` / `seek_maps` (getSavedVal, a "1,3" string or an array, V:222011-222016)
and has no page that writes them. The rows here are KrunkNative's, a Quickplay folder at the end of
the General tab in both views (the basic view also gets Server Type / krunker.io Region, last in
v10's own "Server Browser & Matchmaking" folder; `--render-scenario` leaves the basic view's rows out,
so uidiff still compares v10's tab):

* **Folded, with presets** (KrunkNative): the Quickplay header starts folded (chevron right; `collapseFolder`
  shows the body). The state is the page's, not saved, so each opening of the menu starts folded. Above the
  body, always shown, a row of presets (`knSeekPreset(n)`, `SeekPreset` in `seek_filter.h`), each saving the
  lists its switches would save (the same text, `save_list`) and setting the switches to match; a switch
  can still be changed after it:
  * Krunker rotation: both lists empty, the default (every switch off).
  * Team modes: Team Deathmatch, Hardpoint, Capture the Flag, Kill Confirmed, Domination, Squad Deathmatch
    (modes 1, 2, 3, 19, 25, 26) on the rotation maps.
  * Free for All: Free for All, Classic FFA, Gun Game, Last Man Standing, Kranked FFA, Bighead FFA (modes
    0, 13, 10, 8, 27, 34) on the rotation maps.
  * Competitive: Hardpoint, the mode of Ranked, on the Ranked map pool (`g_ranked_map_pool`: maps 2, 4, 11,
    14, 17, 39, 42).
  * Every mode and map: all 37 modes and all 46 official maps.
  Unit test `seek_filter_test` (the table; a preset's text equals the switches' text for the same choice);
  e2e `v10_settings_e2e.py` run 2d (folded at start, the presets, the header).
* **One switch per mode and per map**: Seek Modes (a folder, open at first) and Seek Maps (closed), each
  row a `label.switch` like Settings > General's own switches, under a `setHed` folder (collapseFolder).
  A switch click saves the switches that are on, in order, as v10 reads them (`"0,2,3,18,19,25"`;
  `knSeekSwitch(key, index, checked)`, `seek_filter_toggle`). Mode ids are v10's `gameModes` indices, map
  ids Krunker's `mapIndex`. v10 has no switch list for `seek_modes`, so uidiff `kn-settings-quickplay`
  compares the folder with KrunkNative's own render in `doc/ref/krunknative/`.
* **On at first = Krunker's public rotation**, every other switch off: the modes of v10's public rotation
  (`rotationModes` [0, 2, 3, 19, 25], V:167202: FFA, Hardpoint, CTF, Kill Confirmed, Domination;
  `g_rotation_modes` / `flavor_rotation_modes`, Krunker Strike its own) and the maps of v10's
  `rotationMaps` (34 maps, V:167210; `seek_filter_rotation_maps`). A saved list replaces that default
  switch by switch. With every switch off the saved list is empty, and the seek uses the rotation again:
  an empty choice never sends an empty pool. Quick Play, the launch seek and Find New Game never land in
  Trade, Parkour, TDM, or a map outside the rotation unless the player switched it on. Lobby picks also
  never take a custom game or a password lobby, and an official custom (`oc`) or dedicated (`ds`) lobby
  only for a mode switched on; krunker.io sends those flags as `1` (or `true`): both count
  (`seek_filter.c`, unit test `seek_filter_test`).
* **Server Type**, native only: Box Office servers (the default), krunker.io servers or Both, stored as
  `knSeekServers`. It only picks what Quick Match / F4 seek into: the server browser lists both.
  The web build has no row and always seeks our servers.

Seeks send `dataQuery` `dsma` / `dsmo` arrays in v10's order, except when the seek names a game: always
both lists, the switched-on ones or the rotation (never empty), to krunker.io and to our brain. v10 sends
none when nothing is picked, and its matchmaker's public pool is the same rotation; the brain has no
pool of its own.
Lobbies announce `meta.mode` and `meta.mapIndex` (server_bin). The brain's quick join only takes
lobbies matching both; requested games are never filtered. When lobbies have room but none matches,
it answers 503 `NoMatchingServers`. The server browser's rows are filtered only once a switch was changed (every row before that).

Find New Game (F4) and Quick Match (`servers_ui_find_new_game`):

* **KrunkNative**: a new seek here with `excludeGame` = this lobby and the preferences.
* **Official**: an official krunker.io lobby that matches (below), opened in the browser.
* **Both**: our seek first. If it answers `NoMatchingServers` / `NoAvailableServers`, an official
  lobby instead.

A find that comes back empty shows v10's error screen, in the game too (v10.js:222076-222090: the menu
with its message, the game left behind it); a find whose only lobby is the one the player is in lands
back in it without a word, as v10's matchmaker does. The startup seek also sends the preferences (v10
parity) but never opens krunker.io.

The server browser (Servers, window 2) shows ours and (native) the official krunker.io lobbies
whatever the server type, filtered to the seek mode and map, with a "Showing <mode> on <map>" line
when a filter is set.

Refresh (v10 windows[1], v10.js:211155-211240, 211452-211460): the list is asked again only when the
window opens (or switches tab) and the last request is older than 10 s (v10's `serverBrowserRate`); the
re-renders inside that time (a region click, a tab switch, an answer of another list) ask nothing. A
hosted game forces the next opening to ask. Each list load also measures every region's `/ping` (v10
`getRegionPing`): the rows' ping icons are those round trips (`pingData`: under 100 ms low, 100-299 ms
medium, 300 ms and more high; no answer = low). Krunker.io's regions come from its own `/ping-list`.
Find Game (window 48, `openServerWindow`): Public and Customs open the list; Raids (2), Zombies (3) and
Trade Plaza (4) join the official krunker.io custom game of that mode with room (the krunker.io Region
first, then the busiest), or say "Couldnt find any". v10's "Searching..." waiting popup shows while the list
is fetched; a join that has not taken the player into the game 2 s later shows "Took to long to find" (v10's
timer; the page would have navigated away on the site). "Couldnt find any" is v10's responsePop popup.

`?play=<map>` (hub links, v10 playMap, v10.js:222040-222051) for one of v10's maps: a matchmaker seek in any
region (an empty `region`), with v10's dataQuery `{"i": "<map>", "dsma": [<index>]}` (`i` is v10's; `dsma` is the
brain's map index, the lobby's mapIndex), autoChangeGame false and no lobby named. When no lobby plays the map
(`NoMatchingServers`, the brain's answer; v10 says NoServersMatchQuery) the plain seek runs in place (v10's
retry), with the Quickplay lists. Any other failure is v10's error screen. Community maps keep the list search
(a lobby named after the map) and the offline practice of the published map.

Server list (windows[1] loadData): a failed game list keeps the window on LOADING until the next opening
(v10's Promise.all rejects, loadedServers stays false, nothing re-renders).

## 4. Official krunker.io lobbies (native only, `client/src/ui/krunker_lobbies.c`)

* Request: `GET https://matchmaker.krunker.io/game-list?hostname=krunker.io`, the same request
  krunker.io's own page makes (V:107131).
  * GET only. No login, no token, no cookies, no Krunker credentials.
  * It runs on its own HTTP worker (`backend_http_get_external`), so it never delays a brain
    request.
  * Timeout: 6 s.
  * Caching: an answer is reused for 10 s (v10's `serverBrowserRate`, the same as the brain's list), a
    forced refresh (a seek) never comes sooner than 15 s, and a failure is retried after 30 s.
* Compiled under `#ifndef __EMSCRIPTEN__`. The web build has stubs and never lists or requests
  them.
* `KRUNKNATIVE_KRUNKER_MATCHMAKER` overrides the URL (tests: a local stub; development: a mirror);
  `off` disables it. `KRUNKNATIVE_KRUNKER_URL` overrides the site the join link opens. Headless
  runs and `--render-scenario` never ask krunker.io unless the URL is given explicitly.
* Blocked networks: krunker.io answers some networks (this machine's and our servers' IPs) with a
  403 "ACCESS DENIED" HTML page. The answer is parsed defensively (JSON with a `games` array or
  nothing), and the KRUNKER.IO section says "krunker.io refused this network (ACCESS DENIED). Set a
  proxy in Settings > Client." instead of an empty list (also: loading, unreachable, the proxy not
  reaching krunker.io, refused through the proxy too).
* Proxy: Settings > Client > Network > Proxy (`client/include/http_proxy.h`): `http://`,
  `https://`, `socks5://` or `socks5h://host:port` (optional `user:password@`), and Proxy Scope
  "krunker.io only" (the default: `*.krunker.io`) or "All client traffic" (also the account
  service, the server finder, news and the asset downloads). Empty: `HTTPS_PROXY` / `HTTP_PROXY` /
  `ALL_PROXY` minus `NO_PROXY` apply, to every host. Loopback is never proxied. It applies without a
  restart: the next connection takes it, krunker.io is asked again at once, and with the scope
  "all" the social socket reconnects.
  * Linux / macOS (`http_conn.c`): every proxied connection, plain or TLS, is tunnelled with HTTP
    CONNECT (Basic credentials) or SOCKS5 (RFC 1928/1929); TLS then runs end to end, verified for
    the target host. An `https://` proxy gets its own TLS with the target's TLS inside it.
  * Windows (WinHTTP): `http://` proxies are set per request; WinHTTP has no SOCKS, so other kinds
    are skipped with a warning and the system proxy stays.
  * Never proxied: the game connection (TCP / UDP / WebTransport to a game server), the server
    info queries, voice. The launcher process honours the environment variables only (it does not
    read the client's settings). The web build has no row: the browser is its network stack.
  * Tests: `client/tests/http_proxy_test.c` (parsing, scope, environment, NO_PROXY) and
    `server/tests/client_proxy_e2e.py` (`make client-proxy-e2e`: a stub CONNECT / SOCKS5 proxy, the
    block page, persistence, a live change, `HTTP_PROXY`).
* Display: each lobby is a server-browser row drawn like v10's own (no tag), with the same columns
  as ours:
  * name (`h`, else "<mode> - <map>" with Krunker's map name `i` as sent)
  * players/max
  * the region cell as v10 has it: only on custom / official-custom rows and in the mode-grouped
    browser, reading the id's prefix ("NY")
  * ping icon
  * "Ends in" (the tuple's seconds left)

  krunker.io's official customs and custom games sit in their own groups after ours, "krunker.io -
  Official Customs" and "krunker.io - Custom Games" (their `oc` and `c` rows), never in Box Office's
  Official Customs or Custom Games. Its other lobbies sit in one group per Krunker region after
  those, with v10's header: the place name (`_7517`, "New York"). Each krunker.io group has Quick
  Join and "N online". In the mode-grouped browser the customs take the same two groups and the
  other lobbies join the mode's own group, as in v10. One line before the official groups is the
  only cue that they are krunker.io's (`#kioStatus`, `.kioSep`): "krunker.io servers", why there
  are none when there are none (loading, refused, unreachable, nothing matching) and, on the right,
  the krunker.io account they see with Login / Logout. The footer's "In Game" adds krunker.io's
  `totalPlayerCount`.
* Region: Settings > Quickplay > krunker.io Region (`knKrunkerRegion`, the matchmaker's regions by
  Krunker's place names) is the region official seeks ask for (after `KRUNKNATIVE_KRUNKER_REGION`)
  and the one Quick Match / F4 prefer in the list; Default is the ping race's region (1.2
  `region`), which the row then shows, like v10's Region row.
* Joining: a click (`krunkerJoin(id)`) joins the lobby in this client through Krunker's own
  protocol (`krunker_net.h`, KN_KRUNKER_OFFICIAL): the Servers window closes, seek-game asks for
  that game (its region from the list), the Security Challenge modal shows when the matchmaker
  wants a captcha, and a join that fails says why (captcha closed or refused, matchmaker
  unreachable, ACCESS DENIED with the proxy hint, a map outside our catalogue).
  * Opt-out: `KRUNKNATIVE_KRUNKER_JOIN=browser` (`?krunkerJoin=browser` on the web) opens
    `https://krunker.io/?game=<id>` in the system browser instead, with a toast saying so (also the
    Windows default, which has no native WebSocket client for it yet). It goes through
    `ui_open_browser`, not `ui_open_external`, whose in-app routing would take a krunker.io link for
    this client's own page.
  * Ids are checked against `[A-Za-z0-9:_-]{1,64}`.
* A seek for an official lobby picks a public one (not custom, no password), not full, in the
  wanted mode and map, with the most players, in the seek's region first (the krunker.io Region, else
  the ping race's): never the busiest region the world over while the near one has a lobby with room.
* Loading: a join from the list or a link raises v10's loading screen (the ring, "Loading <map>
  ..." for a listed lobby) until the game's INIT or the error screen.

## 5. Keys

* **F4** = Find New Game (Krunker's client): `servers_ui_find_new_game`, no restart. Before, it
  relaunched the process (`location.href = "/"`).
* **F5** (the Reload Game bind) relaunches the client the way F4 used to (`app_reload`: the same
  command line, the window kept), and hands the session over (`KRUNKNATIVE_RELOAD_RESUME`):
  * a lobby goes to the new process's startup seek (as the web's `?game=` reload does)
  * the offline game restarts on its map and mode
  * a direct server is reconnected

  If the exec fails, the in-place reload runs instead.
* The console's `reload` is still the in-place reload. The web build keeps F5 = page reload.
* Tests can hand the relaunched process another input script with
  `KRUNKNATIVE_INPUT_SCRIPT_RELAUNCH` (app_mode.c).

## 6. Not ported / differences

* Krunker's token step is client attestation: only Krunker's obfuscated loader can fetch and
  forward it in the expected way. An open-source client cannot hide anything, so here the token
  only proves a fresh round trip to our matchmaker. It is single use, short-lived, rate-limited,
  optionally bound to the address, and checked before a seat is reserved. It does not prove which
  client sent it.
* Captcha (Turnstile `captchaToken`) and FRVR `accessToken` are not ported.
* The official list could not be checked live while writing this: the dev proxy at
  127.0.0.1:8888 was down and no captured `/game-list` answer exists in the mirror. The field
  layout comes from how v10 reads it (section 1.3), and the e2e uses a stub of that shape.
