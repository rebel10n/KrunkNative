# User scripts for the official client (local harness)

A user script is a JavaScript file that the harness runs inside the official Krunker v10.0.3 page. It runs in
every new document of the page, after the harness's own profile seed and before the page's own scripts, so it
can set globals the page reads. It is for trying things on the local stack, not for shipping.

## Paths

- The official folder `/home/user/jails/kru/src/Krunker_v10.0.3/` is read-only. Never put a script in it. The
  harness copies the client into `~/.cache/kn-krunker-local/www` and injects scripts at runtime only.
- Keep your scripts anywhere outside the repo and the official folder, for example `~/scripts/`. Pass absolute paths.
- Samples: `tools/krunker-local/examples/user_a.js` and `user_b.js`.

## Running with one or more scripts

With `join.sh` (the join against the local Box Office stack):

```sh
KRUNKER_LOCAL_USER_SCRIPTS=/home/you/scripts/a.js:/home/you/scripts/b.js tools/krunker-local/join.sh
```

`KRUNKER_LOCAL_USER_SCRIPTS` is a colon-separated list, run in the order given. Each file must exist and be
readable; otherwise `join.sh` exits with `user script not found or not readable: <path>` before any service starts.

The same works with the node driver, `playable.sh`, and `make playable-check`, and the CLI takes one flag per file:

```sh
node tools/krunker-local/krunker-local.mjs join --user-script /home/you/scripts/a.js --user-script /home/you/scripts/b.js
```

The env list runs first, then the `--user-script` flags, each in the order given.

The harness log names the loaded scripts on one line (name and byte count). A script that throws does not stop the
others: its error is logged as `[user-script] <name> threw: ...` and shows in the report's console errors.

To see what your scripts left on the page, set `KRUNKER_LOCAL_USER_PROBE` to a JavaScript expression. It is
evaluated on the page after load, and its value goes to the report as `userScripts.probe`, and `join.sh` prints it:

```sh
KRUNKER_LOCAL_USER_PROBE='({ a: window.__userA, b: window.__userB })' \
KRUNKER_LOCAL_USER_SCRIPTS=tools/krunker-local/examples/user_a.js:tools/krunker-local/examples/user_b.js \
  tools/krunker-local/join.sh
```

## Writing one

- Use `window.name = value` for globals. Your file runs inside a function, so a bare `var` stays local to it.
- Do not rely on the page's own code having run. Your script runs first; read page globals from a later script or
  from `setTimeout`/`requestAnimationFrame` callbacks.
- Every new document gets your scripts again, including the second player's page in `playable.sh`.

## What the page is

- The page is the official krunker.io client, unpatched. Its globals are the official ones (the check reads
  `window.Game` and `window.RENDER`). The harness adds `window.__kn_game` for its own reads; that is harness-only.
- The game server behind the page is Box Office's native server. The bridge (`v10_bridge`) translates the official
  v10 WebSocket protocol to it. The page does not know it is not talking to krunker.io.

## What the local stack does not do

- It does not reach real krunker.io game servers, matchmaking, accounts or the API. Those hosts are routed to the
  local brain and bridge; other hosts do not resolve in the harness's Chromium.
- The static files (textures, models, sounds) come from the harness's own cache, fetched once through the local
  asset proxy. Account, matchmaking and API requests never go to it.
- Nothing from a run is committed; the reports and screenshots stay under `~/.cache/`.

## Known gaps

- `playable-check` is not stable: the stability target (`make playable-check-stable`) fails the bullet-trail
  assertion in about half the runs. A green run does not prove the round is reliable.
- The Hunter class is selected by seeding the saved class setting (`localStorage`), not by clicking its locked card.

See `doc/v10/official-client.md` and `tools/krunker-local/README.md`.
