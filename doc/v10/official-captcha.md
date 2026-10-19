# Official krunker.io games: the matchmaker's captcha

How Krunker Box Office deals with the Cloudflare Turnstile check that `matchmaker.krunker.io`
asks for before it seeks a game (`client/include/krunker_captcha.h`, `client/src/ui/krunker_captcha_ui.c`, the challenge window `client/include/kn_webview.h` /
`client/src/ui/kn_webview.c`, the join flow in `client/include/krunker_net.h`).

**A human solves it.** This client never solves, farms, proxies or bypasses Turnstile, does not
spoof origins and uses no solver services. It shows the challenge (web; native in a window on
krunker.io itself, when a webview is there to show it) and hands the token the player got to the
matchmaker once. Where no challenge can be shown, nothing is shown at all: the join retries its seek
once without a token, and otherwise ends with a sentence on the error screen. Nothing asks the player to
paste or copy a token, and no localhost callback exists.

The in-game socket captcha `cpt` is a different thing: an ALTCHA proof of work that
`krunker_altcha.c` computes (that is how it is meant to be passed). Nothing is shown while it is
computed or when it passes (the player sees only the loading state); only a failure shows the same
modal's passive card, saying so, for a moment.

## What the live client does (game.deob.js)

- `iïîìîíì` (:222266) calls `seek({autoChangeGame: false, dataQuery: {v: buildVersion}, validationToken: undefined})`
  for a guest: **the first `seek-game` has no `validationToken`**.
- `seek` (:147410) loops: `matchmakerSeek(...)`; on `400 {"error":"CaptchaRequired"}`, at most twice,
  `getCaptchaToken()` (Turnstile, :115623: `api.js?render=explicit`, sitekey
  `KRUNKER_TURNSTILE_SITE_KEY`, `appearance: "interaction-only"`, 120 s timeout), then a fresh
  `fetchValidationToken()` (`GET /generate-token`) and the same seek with `validationToken` and
  `captchaToken`.
- `matchmakerSeek` (:147610) builds the query in this order, dropping null ones: `hostname, region,
  autoChangeGame, validationToken, game, excludeGame, dataQuery, captchaToken, accessToken`, and sends
  `accept-language: en-US,en;q=0.6;` (this client does not; the browser adds its own on the web).

`krunker_net.c` follows that order exactly. Checked on 2026-10-14 with two GETs through the proxy:
the first seek in the live order (no `validationToken`) still answers `400 {"error":"CaptchaRequired"}`,
with or without the `accept-language` header. Skipping the early token does not avoid the captcha.

The validation token's lifetime in KN (`client/src/net/krunker_net.c`, `fetch_token` / `seek` / `on_seek`):
a token is fetched only for the seek after a captcha (after the player's Turnstile, or after a refused
token) and the seek that carries it spends it (`seek()` wipes it), so no later seek of the join sends it:
Find New Game's re-seeks, the build's retry and a reload's fallback send none and get `CaptchaRequired`
again, as the live client's seek does. A refused token (`InvalidClientValidationToken`,
`ExpiredClientValidationToken`) gets one fresh token and one more seek per join, without the captcha (it
went with the refused seek); a second refusal is the error, "Socket Error". v10 shows the error at the
first refusal and has no such retry: that is KN's addition. The matchmaker's own spend rule and token
lifetime are not visible from the client; the rule above is what the client does, and what the e2e pins.

## Web build

1. `CaptchaRequired` opens the "Security Challenge" modal (the live socket-captcha modal's look:
   `ui/css/krunknative.css` `.kn-captcha-*`).
2. `api.js` is loaded on demand and the widget is rendered in the modal with krunker.io's sitekey.
   Its token goes straight into the retry; it is never logged or stored.
3. **Turnstile sitekeys are bound to their domains.** Tested on 2026-10-14 from `https://krunk.io`:
   `turnstile.render` returns a widget id, then `error-callback("110200")` fires and the widget shows
   "Unable to connect to website / Troubleshoot". The modal detects 1101xx / 1102xx codes and says so
   plainly ("Cloudflare won't show krunker.io's challenge on https://krunk.io: Turnstile keys only work
   on the sites they were made for (error 110200)…"), with Cancel as the only way out (no paste box, no
   button to open krunker.io). So on krunk.io the challenge cannot be shown, and nothing works around the
   binding.
4. Other errors, the widget's own timeout and our 120 s limit show "Try again" (and Cancel).

## Native client: the challenge window (KN_WEBVIEW)

Built with a webview (CMake `-DKN_WEBVIEW=AUTO|ON|OFF`; AUTO, the default, is on when pkg-config finds
`webkit2gtk-4.1` on Linux, always on macOS, and on Windows when the WebView2 SDK is in `KN_WEBVIEW2_DIR`),
`CaptchaRequired` runs the challenge **silently first**: a WebKitGTK view (a GTK offscreen window, never drawn
into the GL scene; on macOS a WKWebView in an invisible host window, see below) on a **real krunker.io page**,
with no window and no modal on screen, so a check that passes by itself (Turnstile's managed check)
only ever shows the player the loading state. The view moves into a real OS window and the modal
comes up only when the player is needed: Turnstile's `before-interactive-callback`, a widget error /
expiry / timeout (its "Try again" is in the window), no token within 20 s
(`KN_WEBVIEW_REVEAL_DEFAULT_MS`, `KRUNKNATIVE_WEBVIEW_REVEAL_MS`), or the player's "Solve the
challenge". Cloudflare's interstitial alone is not a reason (it may pass by itself):

- **URL: `https://krunker.io/robots.txt`** (`KN_WEBVIEW_CAPTCHA_URL`). The live client has no dedicated
  challenge page: `getCaptchaToken` (game.deob.js :115623) loads
  `https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit` into whatever krunker.io page
  it runs in and renders the widget into a `#turnstileHolder` on `<body>`; the only thing the widget
  needs from the page is its origin. `robots.txt` is the smallest document krunker.io serves (150 bytes
  of text/plain, no scripts, checked 2026-10-14), so krunker.io's game does not boot in the window: no
  WebGL, no second seek, no second challenge of its own. `/` would work the same, at the cost of the
  whole game starting in the window. The window really navigates there: no HTML is loaded under a
  borrowed base URI, no Origin / Referer is rewritten, and an `https://` URL is required (plain
  `http://` only to a loopback host: `KRUNKNATIVE_KRUNKER_CAPTCHA_URL`, the tests' stub page).
- **How the token is read:** two WebKit user scripts, top frame only, doing nothing unless
  `location.origin` is the URL's. One (the page's own world) draws a card over the page and renders
  Cloudflare's widget the live client's way (the same api.js, krunker.io's sitekey
  `KRUNKER_TURNSTILE_SITE_KEY`, `appearance: "always"` so the player sees it in the window); a page that
  already has `window.turnstile` keeps its own. The widget's own `callback(token)` puts
  `token:<token>` on `<html data-kn-captcha>` and fires a `kn-captcha` event; the other script, in an
  isolated script world, is the only one with the `knCaptcha` script-message handler
  (`webkit_user_content_manager_register_script_message_handler_in_world`): it reads the attribute,
  removes it and posts it. So neither the page's own scripts nor any frame of another origin can talk
  to the client. The C side takes **one** token per window, only while the main frame is still on the
  URL's origin, never logs it (`a token (N chars)`), hands it to `krunker_net.c`'s retry and closes
  the window at once. Errors (`error:<code>`, `expired`, `timeout`) are shown in the window (with a
  "Try again" button the player clicks) and in the modal; 1101xx / 1102xx (the key refused the
  domain: should not happen on krunker.io) closes the window and ends the request (no challenge is shown).
- **The window's rules:** an ephemeral web context (no cookies, cache or storage on disk); main-frame
  and frame navigations only to the URL's origin and `https://challenges.cloudflare.com` (others, and
  every new window such as Cloudflare's "Privacy" link, are refused); TLS errors are never bypassed;
  krunker.io's own interstitial ("Just a moment…", `window._cf_chl_opt`) is never covered: the card
  waits for the real page. An HTTP error page from krunker.io ("ACCESS DENIED" 403 to blocked
  networks) is said in the modal (Settings > Client > Network > Proxy). The window's traffic takes
  the same proxy as the matchmaker requests (`http_proxy_for("matchmaker.krunker.io")`: the client
  setting or the environment; `http://` and `socks5://` proxies; loopback direct), so the challenge is
  solved from the network the seek comes from.
- **The challenge is drawn in the game UI (Linux, the default).** When the player is needed, the
  modal shows the challenge where the live client shows its Turnstile holder: v10's black loading
  backdrop (the join spinner's place, which is hidden meanwhile), "Security Challenge", the text and Cancel,
  with the widget centred in it. No OS window
  opens. The view stays in its hidden GTK offscreen host; the page is only the widget on a transparent
  background (the robots.txt text is hidden, no card). Each paint of the host is converted
  (cairo ARGB32, premultiplied, to straight RGBA8) and put into a `<canvas>` of the modal
  (`kn-captcha-webview`, 340 x 170 px, `html_dom_canvas_put`). While it is shown, the pointer over the
  canvas and the widget's keys (Tab, Enter, Space, Backspace, Delete, arrows, Home, End and typed text)
  go to the view (`krunker_captcha_inline_pointer` / `_key` / `_char`, routed first in `main.c`; a
  press that starts on the widget keeps its release). Escape is not forwarded: it stays the menu's.
  The view's own window is the fallback: when the game UI draws no frame within 5 s of showing it
  (`INLINE_STALL_MS`), the challenge moves into a window (dark background, the widget centred), and
  `KRUNKNATIVE_CAPTCHA_WINDOW=1` keeps the window path only.
- **macOS (WKWebView, a section of `client/src/ui/kn_webview.c`, compiled as Objective-C with ARC):** the same
  flow, scripts, messages and policies as the other backends. The game UI's frames are `WKWebView` snapshots
  (`takeSnapshotWithConfiguration:`, about ten a second while shown, a frame only when the page changed)
  converted to the 340 x 170 RGBA frame; the pointer and keys are synthetic `NSEvent`s sent to the view. The
  window path is an `NSWindow` (titled, above the game, centred on the screen under the pointer). The data
  store is non-persistent; the matchmaker's proxy is set on it (`WKWebsiteDataStore.proxyConfigurations`,
  macOS 14+). Keys: WebKit takes them only in a key window, and the game window keeps key status, so the
  widget does not act on the keys in the game UI (the mouse does); the window path has the key window.
- **The window path, for the fallback and the env override:** the view moves into a real window,
  centred on the work area of the monitor under the pointer and kept above the game. There the modal has
  only "Solve the challenge" (raises the window) **and Cancel**; closing the window says so ("Solve the
  challenge" opens it again).
- **In both,** Cancel ends the join (its sentence says the challenge was closed). A refused token (used or
  expired) starts a fresh silent run, at most twice like the live client.
- **Checked against the live site once (2026-10-14, the scratch harness of this change, through the
  proxy):** the window loaded `https://krunker.io/robots.txt`, Cloudflare's real widget rendered there
  with krunker.io's sitekey, no 1102xx domain error, and Cloudflare itself answered with a token
  (688 characters, no interaction asked of that browser). It was discarded, never sent anywhere; no
  matchmaker request was made.

## Tests

`server/tests/krunker_official_e2e.py` (local stubs only; its matchmaker answers the first seek
`CaptchaRequired` and lets a seek with an unused `captchaToken` + an issued `validationToken` through):

- native `native` (headless, no webview: captcha_free, the default): the whole game joins with no modal shown
  at any probe; the seek is sent once with no captcha token;
- native `native-unavailable` (headless, no webview: the matchmaker asks for the captcha on every seek): no modal
  at any probe, two seeks with no token and no generate-token, the log says it seeks again without one, and the
  error screen says the security check needs WebKitGTK;
- native `native-token-socket-error` (`--token`): the matchmaker refuses the first seek (no token) as "Socket
  Error", a fresh token's seek is let through, joined; no captcha is asked;
- native `native-webview` (alone: `--webview`; skipped with a message, exit 77 alone, when the build
  has no webview or `xvfb-run` is missing): client_bin under its own `xvfb-run` (GLFW headless, GTK on
  the private Xvfb), `KRUNKNATIVE_KRUNKER_CAPTCHA_URL` pointing at the test's stub page, which defines a
  stub `window.turnstile` that reports how it was rendered and "is solved" 1.5 s later by firing the
  widget's own callback. The matchmaker refuses the first token; the challenge runs again, the second
  token joins and the game runs. Silent first: no window is ever mapped, and the modal stays hidden at
  every probe from the first seek through the game socket's `cpt` (answered with no card), so the
  player sees only the loading state; the page's own scripts see no `knCaptcha` handler;
  a frame of another origin is refused and its forged token never reaches the matchmaker; the window
  closes after each token; the stub page never sees a token; nothing of the run (WebKit's processes,
  Xvfb) is left. The headless runs (`native`, `native-unavailable`) have no display, so in a webview build
  they also cover GTK failing to start and the no-challenge path;
- native `native-webview-interactive`: the stub widget calls `before-interactive-callback` first: only
  then is the window mapped (centred, never moved) and the modal comes up with it; the stub's game
  server then sends a `cpt` this client cannot answer (SHA-512), and only that failure shows the
  passive card;
- web `web`: the page's request for `api.js` is answered over the DevTools protocol (in the test's own
  browser only) by a stub widget that calls back with a fake token; the game runs as before and the
  `cpt` is answered with no card on screen;
- web `web-domain`: the stub widget answers `110200`; the modal explains it, offers Cancel and no paste box; Cancel
  ends the join with one seek.

Every run checks the live order (seek, CaptchaRequired, generate-token, seek with both tokens) and
that no Turnstile token reaches the log. `input_script.c`'s `0 boot` starts a script's clock before
any map exists, for these runs.

## Gaps

- The challenge is Linux (WebKitGTK) and Windows (WebView2, `client/src/ui/kn_webview.c`, built when the
  pinned SDK is in `KN_WEBVIEW2_DIR`: `make webview2`). The Windows backend loads `WebView2Loader.dll` at
  run time and makes the challenge unavailable when the Edge WebView2 Runtime is missing (the join says
  so); the frames are `CapturePreview` PNGs and the input window messages, so the widget on Windows is
  not yet verified on a real runtime. The macOS backend is verified against the stub page, not the live
  challenge; its keyboard path in the game UI is the gap above.
- Windows packages must ship `WebView2Loader.dll` next to `client.exe` for the challenge to run there (not
  yet in tools/release/package-client.sh); without it the challenge is unavailable, as without the runtime.
- The dynamic Linux packages bundle WebKitGTK (tools/release/bundle-webkit.sh: `lib/`, `libexec/`), so
  the captcha needs no system package there. The polyfilled packages are built with `KN_WEBVIEW=OFF`:
  there is no webkit2gtk-4.1 for their glibc 2.17 floor. WebKit 2.52 compiles in the directory of its
  helper processes and has no setting for it, so the bundle's copy of libwebkit2gtk gets that string
  replaced by a relative one: the packaged client runs from its install root (`install_root_chdir`).
- The window's own texts (card, statuses from the widget) are English; the modal's are localised.
- No window manager under the tests' Xvfb: the window's placement, `keep_above` over a fullscreen
  game and focus were not checked on a real desktop (Wayland / X11, NVIDIA). WebKitGTK on some NVIDIA
  setups needs `WEBKIT_DISABLE_DMABUF_RENDERER=1` (a WebKitGTK issue, not set here).
- The window is used only through the HTML UI's modal: an `HTML_ENGINE=OFF` client still gives up on
  the captcha.

- Not tried against the live matchmaker with a real token: that needs a human-solved Turnstile on
  krunker.io, and joining would open a WebSocket to a live game server.
- The web build on krunk.io cannot show the widget (110200, confirmed): the join ends with Cancel. Nothing
  else is offered; a `postMessage` hand-over from a krunker.io tab was never built and is not planned.
- `accept-language` is not sent on matchmaker requests natively.
