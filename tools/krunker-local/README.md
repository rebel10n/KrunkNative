# Krunker v10.0.3 local harness

Runs the official Krunker v10.0.3 client from a local copy and sends its matchmaker
hosts to our brain (`coordinator.krunk.io`). Nothing from Krunker is committed: the
copy lives in `~/.cache/kn-krunker-local/www` and is made from the read-only tree
`/home/user/jails/kru/src/Krunker_v10.0.3`. The client is not patched. Chromium maps the
official hostnames to a local HTTPS listener, and that listener forwards `/mm/*` to the brain.

## Run

```sh
node tools/krunker-local/krunker-local.mjs prepare   # copy the client, fill assets, make a local cert
node tools/krunker-local/krunker-local.mjs serve &   # http://krunker-local.test:8094/ + https :8443
node tools/krunker-local/krunker-local.mjs check     # headless Chromium: JSON report, menu.png
```

`check` prints a JSON report: 404s, failed requests, matchmaker calls, console errors,
exceptions and the menu state. The screenshot goes to `~/.cache/kn-krunker-local/run/shots/menu.png`.
Environment: `KN_KRUNKER_CHROME` (Chromium binary; default is the newest
`~/.cache/ms-playwright/chromium-*`), `KN_KRUNKER_LOAD_MS` (wait before the report).

Optional overrides go in `tools/krunker-local/config.json` (git-ignored); the defaults are in
`config.example.json`: the source tree, the KrunkNative roots used to fill missing assets,
ports, the brain URL and the official hosts to route.

## Playability check

`make playable-check` (or `tools/krunker-local/playable.sh`) starts a stack on free loopback ports (its own
brain, server, bridge and harness page, with pids in `$KN_PLAYABLE_HOME/run`, default
`~/.cache/kn-playable-check`), joins the official client headless, plays one round, and asserts each of these
from the report that `krunker-local.mjs playable` writes:

| | assertion (PASS or FAIL) |
|---|---|
| a | init completes: `initInProgress` false, the mode set |
| b | the HUD renders in the round (DOM: `#bottomLeftHealth`, `#ammoVal` laid out, a canvas visible) |
| c | `window.__kn_game` readable during the round |
| d | input reaches the server and makes state: W held (the player moves), the mouse moved (the player turns), a click fires (the game's own shot for the local player); the bridge's `<- q` frames and `-> l` samples advance |
| e | the frame counter (the page's animation frames) advances in every 5 s of a 60 s window |
| f | the round ends: `#endUI` (the end screen) is shown |
| g | the next round starts: a new `init` on the socket, the local player active, the end screen gone |
| h | page exceptions at or under `KN_PLAYABLE_MAX_EXC` (default 0); the first is listed with its location |
| i | the tracer trail: the local Sniper Rifle shot's `physObj` gets trail 1 (the page's `physObj` for the local shooter; every local shot gets the rule's value, `weapon.trail ? 1 : 0`, 2 for `weapon.trailChrg` on a full charge), and the second player's assault-rifle tracers reach the first page with trail 0 (its socket's tracer `9`, index 6) |

With `KN_PLAYABLE_PLAYERS=2` (the default) a second official client joins the same round, in its own headless
Chromium (its own profile, so its own anonymous account) and its own bridge session (its own WebSocket and TCP
connection to the server). It presses CLICK TO PLAY like the first player. In the first player's frame window it
holds W, turns, shoots twice and leaves (navigates away; its Chromium is stopped). The rows read the first page's
socket (decoded in `peer_frames.mjs`) and the first page itself:

| | second player assertion (PASS or FAIL) |
|---|---|
| 2a | its add arrives (label `0`, sid = its uid as a string), the first page lists two players in the round, and its positions reach the first page (label `k`, keyed by its uid; they moved) |
| 2b | its shot reaches the first page as tracer `9` with its uid as the shooter (its own page counts the shots) |
| 2c | its leave reaches the first page as label `2` with its uid, and the first page has no exception in that window |
| 2d | its join gives no exception on either page (threshold `KN_PLAYABLE_MAX_EXC`) |

Other players' positions come as `k` (UPDATE_PLAYERS): `l` is the local player's own state (RECON) and `aai` is the
bot add, so neither carries another player's position. `KN_PLAYABLE_PLAYERS=1` runs the single-player check alone.

User scripts (run in the page before its own scripts, `KRUNKER_LOCAL_USER_SCRIPTS` or `--user-script FILE`) are in
`doc/v10/userscripts.md`; samples in `examples/`, tests in `user_scripts_test.mjs`.

`make playable-node` (or `node tools/krunker-local/playable-node.mjs`) runs the same check from Node. It starts the
brain, server, bridge and harness as its own children, each with a pid file in `$KN_PLAYABLE_HOME/run` (default
`~/.cache/kn-playable-node`, so it never stops a `playable.sh` stack), runs `playable()` from `krunker-local.mjs`
(the same CDP driving of the official client), writes the report, and runs `playable-check.mjs` on it unchanged.
Its exit status is the check's.

It exits 0 only when every assertion passes. `playable-check.mjs <report.json> [max]` runs the
assertions again on a saved report. Env: `KN_PLAYABLE_ROUND_S` (round length, default 90),
`KN_PLAYABLE_MAX_EXC`, `KN_PLAYABLE_HOME`, `KN_PLAYABLE_PLAYERS`. The report is `$KN_PLAYABLE_HOME/run/playable-report.json`;
screenshots are in `shots/playable-*.png`. The local shot is counted from the page's `physObj` call with the local player as
the shooter, and its trail argument is assertion (i): the Krunker v10 rule on the page's weapon (1 for `weapon.trail`, 2 for
`weapon.trailChrg` on a full charge, else 0). A round needs
the game server's round length, so the run takes about 5 minutes.

The run plays the Hunter (class 1, Sniper Rifle primary). A guest cannot pick it: the official client locks the Hunter's card
("Register to unlock", no onclick). The class control writes the pick to localStorage `classindex` (`selectClass(i)`), and the
page restores it at start, so `krunker-local.mjs` (`classSeed`) sets that pick before the page starts. The report's
`classPick` gives the saved value and the menu's label; the first enter carries class 1, and the server spawns the Hunter's
loadout. A living player is not respawned by a later enter, so the pick has to be in place before the first spawn.

The page draws a shot's tracer (the physObj call that carries the trail) only when the shot's first ray hits something at least
20 units away. The input window therefore fires the Sniper from the spawn, turning the view between shots, until one shot is
logged with trail 1.

The report's `bridgeCounts` are what the bridge sent (its trace's `-> label` lines, this run's file). They are
not rounds: `rounds` (bridge_counts.mjs `pageRounds`) gives, per INIT and END the bridge sent, the samples in
which the page had the local player active, was in the round, showed the HUD, or showed the end screen.
`playable-check.mjs` prints both under the assertions; the counting rule is tested by `bridge_counts.test.mjs`.

## The v10 bridge (the join path)

The official client's WebSocket (msgpack, v10 labels) reaches a KrunkerBox game server through the
bridge, a C program in `server/bridge` (`v10_bridge`, the port of `net-bridge/v10Proxy.ts`). Start it
next to the game server:

```sh
make v10-bridge V10_LISTEN=8081 V10_NATIVE=127.0.0.1:21015   # ws://127.0.0.1:8081/ws -> the game server
```

`V10_BIND` (default 127.0.0.1) is the listen address, `ARGS=` adds flags (`--lobby=KN:local`; the
`KRUNK_NATIVE_HOST`, `KRUNK_NATIVE_PORT` and `KRUNK_NATIVE_LOBBY_ID` variables work too). The
client's URL is `ws://<bind>:<port>/ws?gameId=...`. The bridge answers the connect packets
(`io-init`, `load`, `inst-id`), HELLO, the map and the INIT, and the joins' packets it knows; the
first native packet it cannot translate stops that session and is named in its log.

## How it works

- The page is served from `krunker-local.test`. The client picks its matchmaker from the
  hostname (localhost and 127.0.0.1 are its dev mode), so the test name keeps it in
  production mode. Chromium resolves that name to 127.0.0.1 via `--host-resolver-rules`.
- `matchmaker.krunker.io` and `matchmaker.beta.krunker.io` resolve to the local HTTPS
  listener. It forwards `/seek-game`, `/game-list`, `/generate-token` to `brainUrl/mm/...`.
- `gapi.svc.krunker.io` and `gapi.beta.krunker.io` forward to the brain root, where v10_api.c
  answers `/maps` (the list the client fills its catalogue from), `/maps/<index>` (the map the
  client loads by mapIndex), `/titles`, `/data/skins` and `/data/spins`.
- Missing files are symlinks into KrunkNative `assets/` or `ui/` (first hit wins). Files no
  root has stay 404 and are listed in the report.
- Static assets (textures, models, images, sounds, fonts, `manifest.json`) are not in any local root.
  `routes.mjs` (the route table; `routes_test.mjs` pins it, `make test`) fetches each one once through the local
  HTTP proxy (`staticProxy`, default `http://127.0.0.1:8888`; `KN_FETCH_PROXY` overrides it), from `assets.krunker.io`
  and `user-assets.krunker.io` (`staticHosts`) or, for page-relative files, from the page's site (`staticPageOrigin`). It caches them under
  `staticCache` (outside git) and serves them from the local origin. A file the proxy reports missing stays a 404.
  **The proxy is for static assets only**: account, matchmaking, API, script and data requests never go to it.
  Third-party hosts (analytics, consent, captcha, web3 CDN) are not static and do not resolve.
- The game object is reachable from the page (harness-only, not in the bundle file or the shipped client).
  The bundle keeps its Game in its closure; `loader.js` (`captureGame`) adds `window.__kn_game` and
  `window.RENDER` to the bundle's text in memory, right after the bundle stores its Game (`<state>['Game']=<game>`).
  The report's `globals.kn` says whether the capture happened; `inGame` is read from the game object.

## Status (v10.0.3, 2026-10-20)

Works: the page, CSS, libs, loader and all ten `core.dat` splits load; the consent
dialog renders and accepts; `generate-token` and `game-list` reach the brain through the
official hostnames (`seek-game` answers too, with `angeles-8.krunk.io:21022`).

The round works end to end (`make playable-check`): the page joins its lobby by itself at startup (no Quick Play click),
the harness presses CLICK TO PLAY for real, and the round, the next round and the input are checked from the report.
The bridge answers an ENT from a living player with that player's spawn and START (the client's loading waits for it).
Static files: no 404s since the static route (`routes.mjs`) fetches them through the proxy (the
page's `img/*`, `manifest.json`, `css/fonts/*`, and the `assets` and `user-assets` CDN files). Blocked by design:
`analytics.google.com`, the other third-party hosts, and the FRVR and Cloudflare hosts.

A join works against a local game (checked 2026-10-21, `join`: init completes, the map loads
through `GET gapi.../maps/<index>`, the client reaches "CLICK TO PLAY"). Two settings are needed,
or the client never seeks: the game's region must be an id the production client pings
(`server_bin --region us-nj`, see doc/v10/regions.md; `local` is not pinged on this host name), and
the brain's `--brain.public_url=https://matchmaker.beta.krunker.io`, so `/mm/ping-list` names a
host the harness routes to `/mm/ping`. A `"Lost Connection"` box on the menu with no socket
means one of them is missing. Packets after the init that the bridge cannot translate stop the
session (named in the bridge log); the client's errors after the init are in the report.

The localhost gate the handoff describes is avoided by the `.test` hostname, so the copy is
not patched. The matchmaker calls above prove the gate does not block the client.
