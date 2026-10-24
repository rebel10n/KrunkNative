# Captchas: what Krunker v10.0.3 asks for, and Box Office's

What the v10.0.3 client does for captchas (every call site in `deob-10.0.3.js`, the decoded bundle), and how Box Office
covers them: one ALTCHA proof of work that the brain issues and verifies and that the C client solves on native and web.
Code: `services/brain/captcha.h` / `captcha.c` / `captcha_http.c` (brain), `client/include/kn_captcha.h` /
`client/src/net/kn_captcha.c` (client), `client/src/net/krunker_altcha.c` (the search). Official krunker.io games keep
their own path: [official-captcha.md](official-captcha.md). Rollout: [doc/backend.md 7.10](../backend.md#710-captcha-captchac-captcha_httpc).

## v10.0.3

Two providers. v10.0.2 has the same code (its bundle, `deob/v10.0.2/game.deob.js` :111389, :128618, :129074, :164633).

| Where | Provider | When | Token flow | Field | Source |
| --- | --- | --- | --- | --- | --- |
| Sign-in by username | Cloudflare Turnstile | before every username + password login | `getTurnstileToken()` then `auth.loginUsername(username, password, a, token ?? undefined)` | `b` in the JSON body of `POST /auth/login/username` (not `captchaToken`) | :130762-130763, :30111-30122 |
| Matchmaker seek | Cloudflare Turnstile | only when `/seek-game` answers `{error: "CaptchaRequired"}`, at most twice | `getCaptchaToken()`, a fresh `fetchValidationToken()`, the same seek again | `captchaToken` query parameter | :164680-164699, :164897-164899, :229402-229404 |
| Game socket | ALTCHA (`<altcha-widget>`) | when the game server sends `cpt` [challenge] | the "Security Challenge" modal solves it; `cptR` [value] goes back, the one packet without an ahNum step | `cptR`'s argument | :111424-111437, :111513, :111558-111625 |
| Hosting a custom game | ALTCHA | before every host (`createPrivateRoom`, the Host window's Start Game, :229780; `autoHostMap`, :255718) | `rpc("bCpt", "hosting")` returns the challenge; the widget's value goes with `custom` | `custom`'s argument after the config | :133912-133919, :229992-229998, :255893-255897 |

- **Turnstile** (`getTurnstileToken`, :130320-130369): `api.js?render=explicit` loaded on demand (:130304-130319) and
  preloaded at startup (:229405); a fixed, centred `#turnstileHolder`; `turnstile.render` with
  `turnstileSiteKey` (:22959; :19304: `0x4AAAAAAD19NTFH3Itq7QOn` in production, Cloudflare's test key
  `1x00000000000000000000BB` otherwise) and `appearance: "interaction-only"`, so nothing shows unless Cloudflare wants
  an interaction. It resolves the token, or `null` on a widget error or after 120 s. The sign-in sends the login
  without `b` then; what the server does with a missing or bad token is not in the client: a non-200 answer is thrown
  as `error || "Login failed before challenges"` (:130772-130773) and shown.
- **Not gated** in v10.0.3: `loginEmail` (email + password, :130765), `loginEmailCode` (:130770), `registerEmailCode`
  (sign-up, :130712, :30190), recovery, the 2FA / code / verification steps.
- **ALTCHA** is the open-source web component, shipped next to the bundle as `libs/altcha.1.0.6.min.js` (not in
  `game.js`). The modal (:111558-111619) is "Security Challenge" / "Please complete the security challenge to
  continue." with `<altcha-widget challengejson="JSON(challenge)">`, polled every 200 ms for its hidden
  `input[name='altcha']`. The server's `cptR` hides it (:111436), so does `error` (:111454); a failed widget ends the
  socket with "CAPTCHA FAILED" (:111428-111431). A failed hosting captcha shows `custom.tryagain` (:229994). When a
  server sends `cpt` is its own decision: nothing in the client says.
- **The ALTCHA format**, checked in `altcha.1.0.6.min.js` (its worker's `b()` search, `kn()` / `kt()`, and `B()`, the
  value): challenge `{algorithm, challenge, salt, signature, maxnumber?}`; the number `n` in `[0, maxnumber]` (default
  `1e6`) with `hex(SHA-256(salt + String(n))) == challenge` (also SHA-384 / SHA-512); the value
  `btoa(JSON.stringify({algorithm, challenge, number, salt, signature, test?, took}))`. The widget reads
  `?expires=` from the salt (altcha-lib's salt params). Server-side (altcha-lib `verifySolution`): the hash, then
  `signature == HMAC(key, challenge)`, then the expiry.

## Box Office

**Every captcha is ALTCHA**, issued and verified by kn_brain, solved by the client in C (`krunker_altcha.c`'s search,
the one that answers official games' `cpt`), on native and web alike. Deliberate difference from v10.0.3: the sign-in's
captcha is a proof of work, not Turnstile.

Why not Turnstile: a Turnstile token is verified with Cloudflare's secret for a sitekey bound to the page's domain.
Box Office would need a Turnstile site of its own, the web build could render it on krunk.io, but the native client is
no web page: it could only show Turnstile through a platform webview on a page of that domain (what
[official-captcha.md](official-captcha.md) does for krunker.io's matchmaker, Linux / Windows / macOS only, never on an
`HTML_ENGINE=OFF` build) and every brain would have to call Cloudflare to verify. ALTCHA needs no third party, no
browser and no domain, and the brain checks it with a hash and an HMAC. What it is not: a proof of a human. It makes each
automated attempt cost CPU (about `max_number / 2` SHA-256s) and rate-limits scripted sign-ups and password guessing; it
does not stop a solver that pays that cost. The sign-in's per-IP limits stay in front of it.

### Flows

| Flow | Purpose (`for=`) | The client sends it in | The brain checks it in |
| --- | --- | --- | --- |
| Sign-in with a password (login card, hub login) | `login` | the social frame `["a", 1, [name, pass, null], <payload>]` (sendAccountRequest's captcha slot) | `social_ws.c` `sh_auth` |
| Registration | `register` | `["a", 0, [name, pass, ref, email], <payload>]` | `social_ws.c` `sh_auth` |
| Hosting a game on Box Office servers | `hosting` | `GET /mm/host-game?...&captcha=<payload>` | `public_routes.c` `h_host_game` (also a POST body's `captcha`) |
| Seeking a game (only after `CaptchaRequired`) | `seek` | `GET /mm/seek-game?...&validationToken=<fresh>&captchaToken=<payload>`, at most twice | `public_routes.c` `h_seek_game` |
| In the game (the server's `cpt`) | `game` | `NET_EXT_CAPTCHA_ANSWER` [payload] to the game server, which asks the brain | `captcha_node_op` (internal WS), bound to that connection's challenge |
| HTTP sign-in (tools, the coordinator API) | `login` / `register` | `POST /auth/login`, `/auth/register` `{captcha}` | `auth.c` `password_auth` |
| v10 HTTP sign-in | `login` | `POST /auth/login/username` and `/auth/login/email` `{b}` (v10.0.3's field) | `v10_auth.c` `login_route` |

A payload is good for its own flow only (a login challenge cannot host a game) and once. Not gated: a saved session's
resume (`["a", 1, [name, null, token]]`), `/auth/refresh`, the 2FA / code / verification steps that follow a gated
first step, the emailed sign-in code, recovery. Deviation: v10.0.3 asks only before a username login; Box Office's one
login field takes a username or an email, and its registration takes a password, so both are gated.

**The seek** (v10.0.3's retry, :164680-164699): with `captcha.enforce` on, `/mm/seek-game` without an accepted
`captchaToken` answers `400 {"error":"CaptchaRequired"}`, before the validation token is spent; every refusal is
`CaptchaRequired`, as Krunker's matchmaker answers a refused token. The client (`servers_ui.c` `on_seek`) then solves
`for=seek` (`kn_captcha_request`), fetches a fresh validation token and seeks again with `captchaToken`, at most twice;
no captcha to send ends the seek with its error (the generic "Failed"), as v10.0.3 throws it. With enforcement off
the brain never asks.

**In the game** (v10.0.3's `cpt` / `cptR`, :111424-111437): with `captcha.enforce` on, the brain's welcome tells its
game servers (`{"captcha": {"enforce": true}}`) and server_bin (`server/src/game_captcha.c`) asks every player
connection once its identity has settled: `{"type":"captcha","op":"issue"}` to the brain, `NET_EXT_CAPTCHA`
[challenge] to the player, its `NET_EXT_CAPTCHA_ANSWER` [payload] back to the brain (`op: verify`, bound to that
connection's challenge, so another connection's answer is refused). Accepted: an empty `NET_EXT_CAPTCHA_ANSWER` (the
server's `cptR`). Refused, no answer within 45 s, or a peer without EXT packets: `NET_PACKET_ERROR` "CAPTCHA FAILED"
(code `004008`), the text v10.0.3 closes with. The game goes on while it is solved (v10.0.3's modal does not stop it
either). The client (`client/src/net/kn_captcha_game.c`) solves it with `kn_captcha_solve`, nothing shown; one it
cannot solve closes the connection with "CAPTCHA FAILED" on the error screen, v10.0.3's failure branch
(:111428-111431). Without a brain link (offline, direct servers) nothing is asked. When v10.0.3's servers send `cpt` is
their own rule, not in the client: Box Office asks each connection once.

### Official krunker.io servers (the official link)

| 10.0.3 flow | What this client does |
| --- | --- |
| `cpt` / `cptR` (ALTCHA) | solved natively in C: on the socket thread natively, in slices of the frame loop on the web (`krunker_link.c` `on_captcha`, `krunker_net.c`); `cptR` goes without the ahNum step (`KRUNKER_AHNUM_EXEMPT`). One it cannot solve (not SHA-256, `maxnumber` over 1e7): the "CAPTCHA FAILED" error and the socket closed (`krunker_link_captcha_failed`), v10.0.3's failure branch. Checked by `krunker_link_test` and `krunker_official_e2e.py` |
| `/seek-game` `CaptchaRequired` (Turnstile) | retried at most twice with `captchaToken` and a fresh validation token (`krunker_net.c` `on_seek` / `captcha_done`). The token is Cloudflare's: the native client shows krunker.io's own Turnstile widget in a platform webview on a krunker.io page (WebKitGTK, WebView2, WKWebView; a build with `KN_WEBVIEW`), drawn in the game, and the player solves it when Cloudflare asks. Without a webview (or on an `HTML_ENGINE=OFF` build) no token can be had: one seek without it, then the generic failure. The web build renders the widget too, but the sitekey is bound to krunker.io, so on krunk.io it fails (Cloudflare 110200). Details: [official-captcha.md](official-captcha.md) |
| `loginUsername` `b` (Turnstile) | the krunker.io account's sign-in asks the same widget first (`krunker_account.c` `start_sign_in`), the same limits |
| `bCpt` before hosting (ALTCHA) | not implemented: the official link has no RPC, so a guest cannot host a krunker.io custom game (`krunker_net_host_block`: `KRUNKER_HOST_GUEST`); a signed-in krunker.io account hosts without it, as v10.0.3 does (`if (account)` at :229989) |

What a native client cannot do: produce a Turnstile token without a browser engine on krunker.io's origin (the
sitekey is bound to the domain and Cloudflare decides when a human must interact), or verify one (that takes
krunker.io's secret). It never solves, farms or proxies Turnstile; the player does, in the webview.

### The brain (`services/brain/captcha.h`)

- `GET /mm/captcha?for=login|register|hosting|seek` (`captcha_http.c`, AUTH and COMMUNITY roles) answers an ALTCHA
  challenge, the JSON an `<altcha-widget challengeurl>` fetches: `{"algorithm":"SHA-256","challenge","maxnumber","salt",
  "signature"}` (`for=game` only for a game server, over the internal WS: `captcha_node_op`). `salt` = 24 random hex
  digits + `?expires=<unix s>&for=<purpose>`, `challenge` =
  `hex(SHA-256(salt + number))` for a random number in `[0, captcha.max_number]`, `signature` =
  `hex(HMAC-SHA256(key, challenge))`. Another purpose: 400 `BadRequest`; no key: 503 `Unavailable`. No-store, CORS.
  v10.0.3 has no such route (its challenges come from Cloudflare and over the game socket): it is Box Office's, and it is
  served whether or not enforcement is on. It sits under `/mm/` so that every proxy in front of the brain hands it to a
  web page (net-bridge `--web`'s `PROXIED_PREFIXES`, the deploy's OpenResty `^/(auth|mm|api|social|ai)/`).
- **Key**: `HMAC-SHA256(brain.join_secret, "kn-captcha-altcha-v1")`, so any brain of a network verifies what another
  issued, and a captcha signature is never a validation token's or a join ticket's. Issuing stores nothing.
- **Verify** (`captcha_verify`): SHA-256 only; the hash and the signature (constant time); then, the payload being ours,
  the salt's `expires` (`CAPTCHA_EXPIRED`) and `for` (`CAPTCHA_PURPOSE`); then **replay**: an accepted challenge is
  spent, remembered per brain until its expiry (16384 slots; a payload is accepted at most once per brain).
- **Gate** (`captcha_gate`): at each flow's entry, before the password is hashed. `captcha.enforce` off (the default):
  everything goes through, and what enforcement would refuse is logged at DEBUG (`login from <ip>: captcha missing
  (captcha.enforce off: let through)`). On: refused, logged at INFO, answered with v10.0.3's own strings: "Please
  complete the security challenge to continue." (missing, :111601) or "CAPTCHA FAILED" (anything else, :111430) on the
  sign-in routes and the social socket; `CaptchaRequired` / `RecaptchaFailed` (v10.0.3's matchmaker error names,
  :164975-164976) on `/mm/host-game`, which the client shows as the generic "Failed"; always `CaptchaRequired` on
  `/mm/seek-game` (the client retries); "CAPTCHA FAILED" in the game.
- Config: `captcha.enforce` (false), `captcha.max_number` (100000; the widget's own default is 1e6; clamped to
  1000..1e7, the client's cap), `captcha.ttl_s` (300, clamped to 10..3600).

### The client (`client/include/kn_captcha.h`)

- `kn_captcha_request(purpose, done, user)`: `GET <brain>/mm/captcha?for=<purpose>` (`backend_http_get`, so the
  same on native and web), then the search in slices of `KN_CAPTCHA_SLICE_MS` (6 ms) per frame from `backend_frame`
  (`kn_captcha_tick`), round robin over the captchas in flight (at most 4), never in one go. With the default
  `max_number` that is about 50 000 hashes: a few frames.
- `done(user, payload)` once: the payload, or `NULL` when there is none to send (a brain from before the route answers
  404, an error, no answer, a challenge it refuses: not SHA-256 or over the cap, no solution, 30 s,
  `KN_CAPTCHA_DEADLINE_MS`). The request then goes without one, as v10.0.3 sends `loginUsername` without `b` when
  Turnstile gave none. Old brains keep working with new clients; the brain decides.
- Nothing is shown: the sign-in's "Please wait..." and the host's "Creating Game..." stay up, as v10.0.3's
  interaction-only Turnstile shows nothing. The sign-in's answer timeout (8 s, `account_ui.c`) starts when the frame is
  sent, not while the proof of work runs. A second Login click replaces the first; a logout drops one in flight.
- `kn_captcha_send_frame(purpose, frame, slot, send)`: the social frame helper (`account_ui.c`'s
  `send_account_request_email`, `social_ui.c`'s hub login): the payload in `frame[slot]` (null without one), sent, then
  the frame's strings (the password) wiped.
- Hosting: `servers_ui.c` `servers_ui_host_game` asks for the `hosting` captcha, then sends `/mm/host-game` with
  `&captcha=` (`host_captcha_done`); a request that cannot go ends like an unanswered one (`custom.tryagain`).

### Tests

- `kn_brain_captcha_test` (`services/brain/tests/test_captcha.c`, CTest `brain_captcha`): the challenge's shape, solved
  and accepted once, replay, expiry (the expired check spends nothing), another flow's payload, forgeries (a wrong
  number, an edited salt, another key, SHA-512, garbage), a network's key, the gate on and off, clamping, no key, and a
  known answer computed with Python's hashlib / hmac.
- `kn_captcha_test` (`client/tests/kn_captcha_test.c`): the solver on that same known answer (the payload is the 1.0.6
  widget's value, field order included), a challenge solved over frames, 404 / no answer / 503 / refused challenges sent
  without one, no slot, cancel, and the sign-in frame (its slot, null without a captcha, never sent once cancelled).
- `brain_captcha_e2e` (`services/brain/tests/captcha_test.py`): kn_brain on sqlite with `captcha.enforce` on, then off:
  the route, every gated flow over HTTP and the social socket (missing, solved, replayed, another flow's, forged), a
  saved session's resume, host-game, and the shadow log.
- `client/tests/captcha_e2e.py`: headless client_bin against a brain with `captcha.enforce` on: register and sign in
  through v10's login popup, host a game, and a sign-in without a captcha refused.
- `server/tests/captcha_game_e2e.py` (CTest `brain_captcha_game_e2e`): kn_brain with `captcha.enforce` on and a
  brain-linked server_bin: seek-game's `CaptchaRequired` and its solved retry, the in-game `cpt` answered and accepted,
  a wrong number / another connection's answer / a peer without EXT packets closed with "CAPTCHA FAILED", a headless
  client_bin seeking at launch (CaptchaRequired, the seek's captcha, the join, the `cpt` answered); enforcement off
  asks nothing.
- `krunker_link_test`: an official `cpt` solved (`cptR`, no ahNum step) and the unsolvable ones ending with "CAPTCHA
  FAILED".
- `tools/kref/web-session/captcha-e2e.js`: the web build on the web stack (`stack.sh` with
  `KN_WEB_BRAIN_ARGS="--captcha.enforce=true --debug=2"`): through net-bridge's `/mm` proxy the page solves the seek's
  captcha after `CaptchaRequired`, answers the node's in-game `cpt`, and signs in after the login captcha.
