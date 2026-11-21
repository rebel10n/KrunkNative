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

### Production baseline (before the hot loader)

Measured on 2026-11-20 against https://krunk.io/ with the same headless Chromium (`hot-e2e.js`), a
guest. Production still runs the old startup chain (no `/mm/hot-seek` deployed yet). All numbers are
ms from navigation:

| krunk.io (no hot loader) | WebRTC starts | connect | click to play | in game |
|---|---|---|---|---|
| cold | 4385 | 4649 | 9333 | 13356 |
| warm 1 | 1226 | 1513 | 4539 | 5781 |
| warm 2 | 1031 | 1269 | 5206 | 6511 |

On a cold load, 4.3 MB of wasm is transferred, compressed, by about 2.3 s. The matchmaker's
requests (`mm`) end just before the connect. The hot loader moves the seek and the channel ahead of
the download. These numbers were taken on another day under other network conditions. Compare the
"after" numbers below with their own A/B arms, not with this table.

### After (production, 2026-11-21)

**Setup.** Measured on 2026-11-21 from 02:16 to 02:58 UTC, after krunk.io moved to the owned host
(deploy/pages/OWNED_HOST.md). Same dev box, network and headless Chromium as the baseline, a
guest, and `hot-e2e.js` unchanged. Chrome ran with `--mute-audio`. All numbers are ms from
navigation.

**The three arms.** Each was interleaved round by round, with a fresh profile per round: one cold
load, then two warm loads in the same profile.

- **hot**: `https://krunk.io/`, the owned host, build `00b9da7e4e577ebd` with the hot loader.
- **nohot**: `https://krunk.io/?nohot=1`, the same host and build, with the hot loader skipped. This
  arm isolates the hot loader.
- **gh**: `https://krunk.io/` with `--host-resolver-rules` pinning krunk.io to GitHub Pages
  (185.199.108.153). That serves the old build `daeb2112581dc60c` (no hot loader) on the same
  origin, brain, gateway and asset hosts. `rebel10n.github.io/KrunkNative/` itself only 301s to
  krunk.io now.

The driver is a loop around `hot-e2e.js`: `~/.cache/kn-hot-after/run.sh` on the dev box, not in
the repo. The table shows the median, with the range in brackets.

**Clean window.** The table is rounds 5 to 7 (02:43 to 02:58). Every run in it connected and got in
game, all arms in the same stretch.

| rounds 5-7 | n | hot answer | channel open | connect | click to play | in game |
|---|---|---|---|---|---|---|
| hot, cold | 3 | 101 [90-117] | 453 [363-455] | 1998 [1770-2372] | 4964 [3521-5708] | 7065 [6407-7819] |
| nohot, cold | 3 | – | – | 2530 [2493-3063] | 6356 [4379-7489] | 10568 [8447-15795] |
| gh, cold | 3 | – | – | 2784 [2781-3225] | 4600 [4555-5080] | 8171 [7552-82237] |
| hot, warm | 6 | 60 [28-97] | 303 [246-484] | 1056 [1005-1432] | 3128 [2946-3644] | 4706 [4082-7294] |
| nohot, warm | 6 | – | – | 1285 [1213-1385] | 3196 [3046-3403] | 4179 [3831-4808] |
| gh, warm | 6 | – | – | 1666 [1242-5007] | 3823 [3251-11191] | 6599 [4690-66170] |

**All seven rounds (59 loads).** Medians with ranges:

| | hot | nohot | gh |
|---|---|---|---|
| cold connect | 3560 [1770-46083] | 2796 [2435-81244] | 3846 [2781-101905] |
| warm connect | 1276 [1005-27120] | 1385 [1213-5010] | 1880 [1238-8517] |

In rounds 1 to 4, loads stalled for tens of seconds in every arm. Cold wasm downloads took 41 to
96 s, and one warm client took 26 s to start. Four loads never connected within 90 s, all in round
4: the hot cold load and all three nohot loads. Three gh loads connected but never got in game. The
box was heavily loaded
during the run (load average about 135 on 32 cores, other sessions' work). Those stalls are not
the hot loader's doing, and the all-rounds medians mostly measure them.

**What the hot loader does in production.**

- **The page side works.** The `/mm/hot-seek` answer is back at about 100 ms cold and 60 ms warm.
  The WebRTC channel is open at about 300 to 450 ms.
- **The client takes both as soon as its wasm runs:** at 1.5 to 2.1 s cold and 0.75 to 0.93 s
  warm, the "taken" step.
- **The connect is earlier, but by less than on the local stack.** It is about 0.5 s earlier cold
  (1998 against 2530) and about 0.23 s earlier warm (1056 against 1285), not the 1.0 to 1.4 s
  measured at 60 ms on the local stack. Production's own seek chain is short from here. In nohot,
  the WebRTC setup alone (`[rtc] connecting` to `connected`) is 0.4 to 0.5 s. With the hot loader,
  about 0.25 s is left after adoption, for the knet handshake and HELLO.
- **No measurable gain to time-to-game.** Click to play is unchanged warm (3128 against 3196 ms).
  Cold it is 1.4 s earlier, but on three runs with a 3.1 s spread. In game is noisier still:
  - The harness clicks every 700 ms.
  - Some runs needed many clicks. gh cold reached 82 s, and outliers of 15 to 66 s show up in
    every arm.
  - Hot warm (4706) came out slower than nohot warm (4179), which is within that noise.

  As on loopback, the map's load and build after the connect (about 2 to 3 s, on SwiftShader on
  a loaded CPU) dominates.

**The owned host does not compress the wasm or the data.** `client.<hash>.wasm` goes out with
`Content-Length: 12682042` and no `Content-Encoding`. The cold loads transfer 12.4 MB of wasm and
1.2 MB of data, where GitHub Pages sent 4.3 MB and 230 KB gzipped (`client.js` is gzipped on both
hosts).

- On this link the wasm still finished by 0.5 to 0.8 s in the clean rounds, so the cost was small
  here.
- For a player on a slower link it is about 9 MB more per cold load. That is more than the hot
  loader saves.
- Against the old GitHub Pages build, the owned host plus hot loader connects about 0.8 s sooner
  cold (1998 against 2784) and 0.6 s sooner warm. It does not reach click to play sooner cold
  (4964 against 4600).
- Gzip or brotli for `.wasm` and `.data` on the origin or edge would be the larger win. It was not
  changed here.
- The hosts also cache differently. Pages sends `max-age=600`; the owned host sends
  `max-age=31536000, immutable` on the hashed client files. Warm loads more than 10 minutes apart
  would revalidate on Pages but not here. This run's warm loads were seconds apart, so it does not
  show.

**Failure modes seen.**

- **A slow cold load loses the early channel.** In round 2's hot cold load the wasm arrived at
  about 42 s. By then the answer was older than `HOT_TICKET_MS`, and the client logged "the hot
  seek is not used: too old (37 s)". The page's channel had already closed at 30.6 s, before the
  client could take it. The client then ran its own seek and connected at 46 s, so the fallback
  works.
- Round 4's hot cold load closed its channel the same way, at 31.5 s, and never connected, but
  neither did any nohot load in that round.
- In round 4's warm 1 the answer was dropped as too old (26 s), yet the still-open channel was
  adopted (`[rtc] taking the hot loader's channel`) and connected.
- So an early channel lives about 30 s, not the page's 90 s. Something on the gateway or server
  path closes it first.

## Ideas not done

- **Prefetch the map's assets while the wasm downloads.** The hot answer names the map, but
  deriving its asset list in JS would be game logic in the page.
- **Skip the session-restore wait for a signed-in web player.** The session is in the encrypted
  vault, so the page cannot send it.
