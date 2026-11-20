# The hot loader: seek and connect before the client has loaded

The old startup ran its seek only once the client was up. On the web that meant after `client.js`,
the 12.6 MB `.wasm` and the `.data` had downloaded and compiled. Natively it meant after the window,
GL, the fonts and the menu page were set up. The seek was then a chain of round trips:
`/mm/status` → `/mm/ping-list` → a ping race → `/mm/generate-token` → `/mm/seek-game`. The game
transport opened only after all of that: on the web, the WebRTC DataChannel (signaling, ICE, DTLS,
SCTP), then the WebTransport and WebSocket fallbacks.

The hot loader runs that work in parallel with loading:

- **One brain request.** `GET /mm/hot-seek` does the whole find in kn_brain.
- **Sent as early as possible.**
  - Web: the first script in `<head>`.
  - Native: right after the command line is parsed.
- **The web also opens the game transport.** It opens the WebRTC channel to the answer's server
  while the client is still downloading.

## Pieces

| Part | Where |
|---|---|
| The endpoint | `services/brain/public_routes.c` `h_hot_seek` (route `hot-seek`: `/mm/hot-seek` and `/hot-seek`) |
| The page's script | `platform/web/hot-loader.js`, inlined into `dist/web/index.html` by `platform/web/hot_loader.py` (CMake `web_dist`) |
| The client side | `client/include/net_hot.h`, `client/src/net/net_hot.c` |
| The startup seek adopting it | `client/src/ui/servers_ui.c` `hot_adopt`, `servers_ui_hot_start` |
| The channel adoption | `client/src/net/net_rtc.c` `kn_rtc_open` |
| Native send | `client/src/net/backend_startup.c` `backend_hot_start`, called from `main.c` |
| Tests | `server/tests/hot_seek_e2e.py` (ctest `brain_hot_seek_e2e`), `tools/kref/hot-e2e.sh` + `hot-e2e.js` |

## /mm/hot-seek

`/mm/hot-seek` is `/mm/seek-game`'s answer, with the client's three earlier requests done by the
brain:

- **Validation token.** The brain issues one for the caller's IP and spends it itself
  (`mm_token_issue` + `mm_token_check`), so a brain with
  `brain.matchmaking.require_validation_token` serves the endpoint too.
  - The endpoint has its own per-IP limit: 120 per minute.
  - With `captcha.enforce` on, it answers 400 `CaptchaRequired` and the client seeks itself.
- **Preferences, resolved by the brain.** v10 reads `seek_modes` and `seek_maps` into dataQuery
  `dsmo` and `dsma` (deob-10.0.7.js:229233-229237).
  - The page sends the saved switches as comma lists: `seek_modes=1,3&seek_maps=5`.
  - Each entry is checked as the client's `seek_filter_parse` checks it: a mode of this flavor, a
    map index from 0 to 255. Repeated entries count once.
  - An empty list, or one with only invalid entries, means the public rotation:
    `flavor_rotation_modes` and `g_rotation_maps`. The client does the same.
- **Region.**
  - The saved Default Region, or the region an earlier ping race confirmed (`pingRegion7` when it
    equals `knPingRegionPinged`).
  - Otherwise none. The brain then routes the seek to the nearest region that has a node (the
    regions doc's "Routing").
  - No ping list and no ping race: with one server, geo-balancing belongs to DNS.
- **Everything else is `seek-game`'s:** `game` (with `autoChangeGame=false`: a lobby that is gone
  is refused, never swapped for another), `proto`, `ruleset`, `flavor`, `token`.
- **The answer** is `seek-game`'s JSON plus `"hot": true`. The old routes are unchanged.

## Web

`hot-loader.js` runs before anything else on the page:

1. **Hints.** `preload` for `client.js` (as script) and the `.wasm` and `.data` (as fetch), and
   `preconnect` to a cross-origin brain or WebRTC gateway. File names follow the `kn-build` and
   `kn-root` metas, the way `loader.js` names them.
2. **Seek.** `fetch(brain + '/mm/hot-seek?…')` with the saved knSettings.
3. **Channel.** On a 200, it opens the `knet` DataChannel to `host:port`, exactly as `net_rtc.c`
   opens it: unordered, no retransmits, one WHIP-style POST to the gateway (`kn-rtc` meta or
   `rtc.json`).
   - The game server sees nothing until the client's knet handshake, so the server's pre-HELLO
     `join_timeout` (5 s from accept) never runs while the wasm downloads.
   - The game WebSocket is never opened early: its TCP accept would start that 5 s clock.
4. **Hand-over.** `window.__knHot` holds the answer and the channel:
   - When the client runs, `servers_ui.c` takes the answer (`net_hot_seek_take`).
   - `kn_rtc_open` takes the channel when its target matches. Any message that arrived first is
     replayed in order.
   - A channel nobody takes is closed after 90 s.

**The page skips the hot loader** for any of these query parameters:

- `nohot` (measurements)
- `offline`, `sandbox`, `test`, `map`, `testMap`, `play`
- `ws`, `transport`, `host`
- `demo`, `editor`, `viewer`

It also skips `mode=offline` and Krunker Strike pages (`kn-flavor` meta), which seek through their
director. The client then does what it always did.

## Native

`backend_hot_start(offline, scenario)` runs right after `parse_startup_args`. It sends the hot seek
only for a plain online launch into the game page that will matchmake through a brain. It skips:

- `--offline`, `--sandbox`, a demo, `--krunker`
- `--connect`, `KRUNKNATIVE_SERVER_PORT`
- `--hub`, `--editor`, `--viewer`, or a page query
- an F5 relaunch, which rejoins its own lobby
- Server Type krunker.io or both, and Strike's director
- `KRUNKNATIVE_NO_HOT=1` (measurements)

When it does send, it runs `online_mode_init` and `backend_init` early; they read only the command
line, the environment and stored keys, and `backend_startup` does not run them again. It also loads
knSettings early, and `main.c` loads them again with the menu. `KRUNKNATIVE_GAME` names the lobby,
as it does for the startup seek.

The answer is delivered by `backend_poll` once the main loop runs. By then the window, GL and the
menu page are up, so the answer is waiting for the startup seek.

## The client adopting it (both platforms)

The startup seek (`servers_ui_tick`, `!g_started`) waits for an answer that is still out, for up to
`HOT_WAIT_MS` (8 s) after the hot seek started. `hot_adopt` then drops the answer, and the client
seeks exactly as before, in any of these cases:

- it is not a 200;
- the client wants another lobby, or the answer is for a lobby other than the one asked for;
- the client is pinned to a server;
- it is older than `HOT_TICKET_MS` (20 s), so the ticket may be past `brain.join_ttl_s` (30 s).

Otherwise the startup seek takes the answer's region, and `seek()` takes the answer as its own
`seek-game` reply. There is no ping list, no ping race, no token and no seek of its own.

**A signed-in player.** The hot seek is a guest's: on the web the page cannot read the session
token, which is in the encrypted alt vault. So:

- With an account active, only the answer's region is used, and the client seeks with the account.
- On the web, the page's WebRTC channel is still adopted when that seek lands on the same server.
  It does with one server.

## Timings

Measured on 2026-11-20 on the dev box, with headless Chromium (SwiftShader) for the web and headless
mesa llvmpipe for native. A local stack: kn_brain on scratch SQLite, one brain-linked `server_bin`
(ffa, Burg), `kn-rtc`, and `net-bridge --web`. All numbers are ms from navigation, or from process
start for native.

Web: `tools/kref/hot-e2e.sh cmake-build-debug dist/web 2`. "connect" is the first `connected`; "in
game" is after the click on the instructions line, recorded once it is offered.

| loopback | connect | click to play | in game |
|---|---|---|---|
| hot, cold | 3194 | 5809 | 7535 |
| hot, warm | 1003 | 3806 | 5324 |
| no hot, cold | 3034 | 5237 | 6308 |
| no hot, warm | 1291 | 3919 | 4954 |

On loopback the seek chain costs next to nothing, so the remaining time is the client's own load.
With `KN_E2E_LATENCY=60 KN_E2E_MBPS=100` (60 ms of added latency and 100 Mbit/s, CDP network
emulation), which is closer to a real player:

| 60 ms, 100 Mbit/s | seek taken | connect | click to play | in game |
|---|---|---|---|---|
| hot, cold | 4677 | 4974 | 8918 | 11042 |
| hot, warm 1 | 2720 | 2977 | 6035 | 7216 |
| hot, warm 2 | 2349 | 2616 | 5696 | 7246 |
| no hot, cold | – | 6023 | 8802 | 10846 |
| no hot, warm 1 | – | 4010 | 7152 | 8331 |
| no hot, warm 2 | – | 3773 | 6527 | 7959 |

**The web connects 1.0 to 1.4 s earlier.** The answer is back about 17 to 75 ms after navigation,
and the WebRTC channel is open at about 150 to 270 ms. The client takes both as soon as its wasm
runs.

After the connect, the time to the click is about 3 s in both modes, even with warm cached assets.
That is the map's load and build (CPU and GL on SwiftShader), so the hot loader cannot win it. The
cold load's remaining cost is the client itself: 12.6 MB of wasm, 1.2 MB of data, and about 188
asset requests (8 MB). The in-game times are noisier than the connect times because the
click-to-play poll runs every 700 ms.

Native (`client_bin`, headless): connected to the game server, in ms from process start. Four runs
each, brain behind a proxy adding 30 ms each way (60 ms RTT); the game server is on loopback in both
modes.

| native, 60 ms RTT to the brain | run 1 | run 2 | run 3 | run 4 |
|---|---|---|---|---|
| hot | 475 | 480 | 491 | 446 |
| no hot (`KRUNKNATIVE_NO_HOT=1`) | 1422 | 1556 | 1486 | 1434 |

**Native connects about 1 s earlier.** The hot seek is sent at 30 to 90 ms, and its answer is
waiting when the main loop starts (about 400 ms). The old chain from `backend_startup` to
`connected` was about 1 s at this RTT: `/mm/status`, ping list, ping race, token and seek.

On plain loopback the native difference (50 to 150 ms) is below the dev box's run-to-run noise:
GL setup alone varies from 100 to 600 ms there.

## Ideas not done

- **Prefetch the map's assets while the wasm downloads.** The hot answer names the map, but
  deriving its asset list in JS would be game logic in the page.
- **Skip the session-restore wait for a signed-in web player.** The session is in the encrypted
  vault, so the page cannot send it.
