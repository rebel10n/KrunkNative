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

## Node driver (launch.mjs)

`make krunker-node-smoke` (or `node tools/krunker-local/launch.mjs --smoke`) starts the local stack on free loopback
ports (kn_brain on a sqlite scratch database, one server_bin, v10_bridge) and the harness page server with a config
written for the run (`KN_KRUNKER_LOCAL_CONFIG`). It opens headless Chromium, accepts the consent dialog, clicks Quick
Match, and records every WebSocket frame (CDP Network domain) to `~/.cache/kn-krunker-node/run/<pid>/frames.jsonl`
(one JSON object per line; binary payloads base64). The result, with `window.Game` and `window.RENDER`, goes to `result.json`.
Each child is recorded in `<run>/pids/` and stopped by its handle. The script refuses to start unless every host it binds
or dials is 127.0.0.1 or `*.krunker-local.test`. It never reads `config/config.json`.

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
- `gapi.svc.krunker.io` and `gapi.beta.krunker.io` forward to the brain root (the brain
  answers them with 404 today).
- Missing files are symlinks into KrunkNative `assets/` or `ui/` (first hit wins). Files no
  root has stay 404 and are listed in the report.
- The game object is reachable from the page (harness-only, not in the bundle file or the shipped client).
  The bundle keeps its Game in its closure; `loader.js` (`captureGame`) adds `window.__kn_game` and
  `window.RENDER` to the bundle's text in memory, right after the bundle stores its Game (`<state>['Game']=<game>`).
  The report's `globals.kn` says whether the capture happened; `inGame` is read from the game object.

## Status (v10.0.3, 2026-10-20)

Works: the page, CSS, libs, loader and all ten `core.dat` splits load; the consent
dialog renders and accepts; `generate-token` and `game-list` reach the brain through the
official hostnames (`seek-game` answers too, with `angeles-8.krunk.io:21022`).

Does not work yet: the client stays on its loading spinner after the token, even after 120 s.
It never calls game-list from the menu, so Quick Play cannot be reached from the harness.
Known 404s: `manifest.json`, `img/pwa/icon32x32.png` (not in KrunkNative). `gapi.*/data/skins`
answers 404 from the brain. Blocked by design: `analytics.google.com`.

A join to a krunk.io game server from the official client is not verified, and none is
possible yet. `angeles-8.krunk.io:21022` answers a WebSocket upgrade with a raw binary frame,
not HTTP 101, so it speaks KN's framing (`doc/transport.md`), not the official
msgpack-over-WebSocket protocol. `play.krunk.io/native` (101) is KN's raw passthrough, and
`play.krunk.io/ws` returns 502. The path to our servers is net-bridge's msgpack/ahNum codec
(`net-bridge/utils.ts`, `krunkerProxy.ts`) adapted to v10.0.3's packets (Option B in the
handoff). That translator is not written; `krunkerProxy.ts` still targets the v7 ENT layout.

The localhost gate the handoff describes is avoided by the `.test` hostname, so the copy is
not patched. The matchmaker calls above prove the gate does not block the client.
