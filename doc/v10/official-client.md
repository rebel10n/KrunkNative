# The official Krunker v10.0.3 client: where it lives and how to run it

## Where the official files live

- The official browser client is a read-only reference folder outside the repo:
  `~/jails/kru/src/Krunker_v10.0.3/`. Its README.md and AGENTS.md say what is in it.
- Nothing from it is committed to KrunkNative. The harness (`tools/krunker-local/`) copies it into
  `~/.cache/kn-krunker-local/www` and never writes to the source. Config: `krunkerSrc` in
  `tools/krunker-local/config.example.json`.
- The repo's tracked files derived from the official client (captures under `doc/ref/`, UI import data
  under `tools/ui-import/`) are fine. No official `game.js` is tracked in KrunkNative.
- The official client is a second-class reference. Box Office bugs go to the native client.

## Usable parts of the folder

- `game.js`: the official bundle, the source for v10.0.3 offsets.
- `index.html`, `css/`, `img/`, `libs/`, `static/`, `data/`: the page the harness drives.
- `pkg/`: the official WASM loader, kept because the harness overwrites it with its own loader route.

Removed on 2026-10-22: the old JS server (`server.mjs`), the old handoff notes (`HANDOFF.md`), the
duplicate `js/` folder, the root `frvr*.js` files, the Tampermonkey userscript, and `local.html`. The
harness never read them.

## How to run the official client against the native server

Build first, then use the one-command join:

    cd ~/src/KrunkNative
    make                                  # builds server_bin, v10_bridge, kn_brain, the client
    tools/krunker-local/join.sh           # brain :5150, server :21115, bridge :8097, harness :8194 (TLS :8543)

`join.sh` starts what the join needs, runs headless Chromium through the harness, and prints the
outcome. Its state lives in `~/.cache/kn-v10-join`. Pass a profile name as the first argument to use a
different run (default `join`). Round length: `KN_ROUND_SECONDS` (default 30).

To check the whole path (init, HUD, input, 60 s of frames, the round end, the next round, two players):

    make playable-check                   # exit 1 on any failed assertion

To run your own scripts in the page (before its scripts, read-only paths), see `doc/v10/userscripts.md`:
`KRUNKER_LOCAL_USER_SCRIPTS=/abs/a.js:/abs/b.js tools/krunker-local/join.sh`.

Start pieces by hand, if you need them:

    cmake-build-debug/v10_bridge --v10-listen=8097 --native=127.0.0.1:21115   # the bridge, or: make v10-bridge
    cmake-build-debug/server_bin ...                                        # the native server, with a brain

## Known gaps

- `make playable-node` (driving the same check from Node) does not get into a round yet. Use `join.sh` or
  `make playable-check`.
- The stability target (`make playable-check-stable`) does not pass on current main: the bullet-trail
  assertion fails in about half the runs.
- The Hunter class is selected by seeding the saved class setting, not by clicking the locked card.

## Rules

- Never edit the official folder. Read byte offsets from `game.js`, or the decoded copy if present.
- Never run the official client against production hosts. The harness blocks them; keep it that way.
