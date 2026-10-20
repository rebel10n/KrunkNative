# Official krunker.io games: the matchmaker's captcha

How Krunker Box Office deals with the Cloudflare Turnstile check that `matchmaker.krunker.io`
asks for before it seeks a game (`client/include/krunker_captcha.h`, `client/src/ui/krunker_captcha_ui.c`, the
challenge webview `client/include/kn_webview.h` / `client/src/ui/kn_webview.c`, the join flow in
`client/include/krunker_net.h`).

**A human solves it.** This client never solves, farms, proxies or bypasses Turnstile, does not spoof origins
and uses no solver services. It shows the challenge (web; native in the game, on krunker.io itself, when a
webview is there to show it) and hands the token the player got to the matchmaker once. Where no challenge
can be completed, nothing is shown for it: the join retries its seek once without a token, and a refusal then
ends the join with the generic join failure ("Failed"). Nothing asks the player to paste or copy a token, and
no localhost callback exists.

**The challenge is the Turnstile widget and nothing else, in the game, always.** No OS window on any platform.
No box, no heading, no text of ours, no button (no Cancel): the widget is drawn centred on the screen where the
join's loading spinner is (`#instructions`, hidden while the widget is up), the way krunker.io draws its own
(game.deob.js getCaptchaToken: a fixed, centred holder, appearance `interaction-only`). No interface names the
captcha or a security check: a failed challenge is the generic join failure.

The in-game socket captcha `cpt` is a different thing: an ALTCHA proof of work that `krunker_altcha.c` computes
(that is how it is meant to be passed). Nothing is shown for it: not while it is computed, and not when it
fails (the server's own close says so). The log says which.

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

1. `CaptchaRequired` draws the Turnstile widget where the join's spinner is (`#knCaptchaHolder`, centred; the
   spinner is hidden while it is up). Nothing else is drawn: no box, no text, no button.
2. `api.js` is loaded on demand and the widget is rendered with krunker.io's sitekey and
   `appearance: "interaction-only"` (the live client's getCaptchaToken). Its token goes straight into the
   retry; it is never logged or stored.
3. **Turnstile sitekeys are bound to their domains.** Tested on 2026-10-14 from `https://krunk.io`:
   `turnstile.render` returns a widget id, then `error-callback("110200")` fires and the widget shows
   "Unable to connect to website / Troubleshoot". Nothing of ours is shown for it: the challenge ends without
   a token, the join retries its seek once without one, and a refusal ends the join with the generic
   failure. On krunk.io the challenge cannot be completed, and nothing works around the binding.
4. Any other error, the widget's own timeout and our 120 s limit end the challenge the same way.

## Native client: the challenge webview (KN_WEBVIEW)

Built with a webview (CMake `-DKN_WEBVIEW=AUTO|ON|OFF`; AUTO, the default, is on when pkg-config finds
`webkit2gtk-4.1` on Linux, always on macOS, and on Windows when the WebView2 SDK is in `KN_WEBVIEW2_DIR`),
`CaptchaRequired` runs the challenge **silently first**: a WebKitGTK view (a GTK offscreen window, never drawn
into the GL scene; WebView2 a popup parked off the desktop; on macOS a WKWebView in an invisible host window)
on a **real krunker.io page**, with nothing on screen, so a check that passes by itself (Turnstile's managed
check) shows the player nothing: the join's spinner stays. The widget comes up in the game only when the player
is needed: Turnstile's `before-interactive-callback`, no token within 20 s (`KN_WEBVIEW_REVEAL_DEFAULT_MS`,
`KRUNKNATIVE_WEBVIEW_REVEAL_MS`). Cloudflare's interstitial alone is not a reason (it may pass by itself).

- **URL: `https://krunker.io/robots.txt`** (`KN_WEBVIEW_CAPTCHA_URL`). The live client has no dedicated
  challenge page: `getCaptchaToken` (game.deob.js :115623) loads
  `https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit` into whatever krunker.io page it runs
  in and renders the widget into a holder on `<body>`; the only thing the widget needs from the page is its
  origin. `robots.txt` is the smallest document krunker.io serves (150 bytes of text/plain, no scripts, checked
  2026-10-14), so krunker.io's game does not boot in the view: no WebGL, no second seek, no second challenge of
  its own. The view really navigates there: no HTML is loaded under a borrowed base URI, no Origin / Referer is
  rewritten, and an `https://` URL is required (plain `http://` only to a loopback host:
  `KRUNKNATIVE_KRUNKER_CAPTCHA_URL`, the tests' stub page).
- **How the token is read:** two WebKit user scripts, top frame only, doing nothing unless `location.origin` is
  the URL's. One (the page's own world) renders Cloudflare's widget the live client's way (the same api.js,
  krunker.io's sitekey `KRUNKER_TURNSTILE_SITE_KEY`, `appearance: "interaction-only"`), alone, centred on a
  transparent page (the robots.txt text is hidden); a page that already has `window.turnstile` keeps its own.
  The widget's own `callback(token)` puts `token:<token>` on `<html data-kn-captcha>` and fires a `kn-captcha`
  event; the other script, in an isolated script world, is the only one with the `knCaptcha` script-message
  handler (`webkit_user_content_manager_register_script_message_handler_in_world`): it reads the attribute,
  removes it and posts it. So neither the page's own scripts nor any frame of another origin can talk to the
  client. The C side takes **one** token per challenge, only while the main frame is still on the URL's origin,
  never logs it (`a token (N chars)`), hands it to `krunker_net.c`'s retry and closes the view at once. An
  error (`error:<code>`), an expiry or a timeout ends the challenge without a token, like a challenge that
  cannot be shown (the caller's unavailable path); 1101xx / 1102xx (the key refused the domain: should not
  happen on krunker.io) do the same.
- **The view's rules:** an ephemeral web context (no cookies, cache or storage on disk); main-frame and frame
  navigations only to the URL's origin and `https://challenges.cloudflare.com` (others, and every new window
  such as Cloudflare's "Privacy" link, are refused); TLS errors are never bypassed; krunker.io's own
  interstitial ("Just a moment…", `window._cf_chl_opt`) is never covered: the widget waits for the real page.
  An HTTP error page from krunker.io is logged only. The view's traffic takes the same proxy as the matchmaker
  requests (`http_proxy_for("matchmaker.krunker.io")`: the client setting or the environment; `http://` and
  `socks5://` proxies; loopback direct), so the challenge is solved from the network the seek comes from.
- **The widget is drawn in the game, on every platform.** When the player is needed the view's frames go to a
  `<canvas>` (`kn-captcha-webview`, 340 x 170 px, `html_dom_canvas_put`) in the holder that takes the join
  spinner's place: the holder is centred on the screen (`top: 42%`, `translate(-50%, -50%)`, like `#instructions`),
  and the spinner is hidden when the first frame arrives (until then the spinner is the loading state). The
  view stays in its hidden host (GTK: cairo ARGB32, premultiplied, converted to straight RGBA8; WebView2:
  `CapturePreview` PNGs; macOS: `WKWebView` snapshots, about ten a second while shown, a frame only when the page
  changed). While the widget is shown, the pointer over the canvas and the widget's keys (Tab, Enter, Space,
  Backspace, Delete, arrows, Home, End and typed text) go to the view (`krunker_captcha_inline_pointer` / `_key` /
  `_char`, routed first in `main.c`; a press that starts on the widget keeps its release). Escape is not
  forwarded: it stays the menu's. Slow frames mean a lower frame rate, never a window.
- **No OS window, on any platform.** The window the challenge used to fall back to is gone: no stall fallback
  (the 5 s watchdog), no window on macOS, no `KRUNKNATIVE_CAPTCHA_WINDOW` (it is ignored). `kn_webview_captcha_windowed()`
  stays, always 0, so the tests can check it.
- **A challenge that cannot be completed** (no webview, a load or widget error, an expiry, a timeout, an answer
  that is not a token): the request ends unavailable, the join's seek goes once more without a token (the
  matchmaker often lets that through), and a refusal then fails the join with the generic error "Failed"
  (`join_error_text("Failed")`). A refused token (used or expired) starts a fresh run, at most twice like the
  live client. Cancel is gone: there is nothing to cancel, and the sign-in fails the same generic way.
- **macOS (WKWebView, a section of `client/src/ui/kn_webview.c`, compiled as Objective-C with ARC):** the same
  flow, scripts, messages and policies as the other backends. The pointer and keys are synthetic `NSEvent`s sent
  to the view. The data store is non-persistent; the matchmaker's proxy is set on it
  (`WKWebsiteDataStore.proxyConfigurations`, macOS 14+). Keys: WebKit takes them only in a key window, and the
  game window keeps key status, so the widget does not act on the keys in the game (the mouse does).
- **Checked against the live site once (2026-10-14, the scratch harness of this change, through the proxy):**
  the view loaded `https://krunker.io/robots.txt`, Cloudflare's real widget rendered there with krunker.io's
  sitekey, no 1102xx domain error, and Cloudflare itself answered with a token (688 characters, no interaction
  asked of that browser). It was discarded, never sent anywhere; no matchmaker request was made.

## Tests

`server/tests/krunker_official_e2e.py` (local stubs only; its matchmaker answers the first seek
`CaptchaRequired` and lets a seek with an unused `captchaToken` + an issued `validationToken` through):

- native `native` (headless, no webview: captcha_free, the default): the whole game joins with no widget shown at
  any probe; the seek is sent once with no captcha token;
- native `native-unavailable` (headless, no webview: the matchmaker asks for the captcha on every seek): nothing
  drawn at any probe, two seeks with no token and no generate-token, the log says it seeks again without one, and
  the error screen says "Failed" (the generic join failure, no wording of the check);
- native `native-token-socket-error` (`--token`): the matchmaker refuses the first seek (no token) as "Socket
  Error", a fresh token's seek is let through, joined; no captcha is asked;
- native `native-webview` (alone: `--webview`; skipped with a message, exit 77 alone, when the build has no webview
  or `xvfb-run` is missing): client_bin on a private Xvfb (GLFW headless, GTK on that display), with
  `KRUNKNATIVE_KRUNKER_CAPTCHA_URL` pointing at the test's stub page, whose stub `window.turnstile` reports how
  it was rendered (krunker.io's sitekey, `interaction-only`) and "is solved" 1.5 s later by firing the widget's
  own callback. The matchmaker refuses the first token; the challenge runs again, the second token joins and the
  game runs. Silent first: the widget stays hidden at every probe from the first seek through the game socket's
  `cpt`, no OS window is ever mapped on the display, and the page's own scripts see no `knCaptcha` handler; a
  frame of another origin is refused and its forged token never reaches the matchmaker; the view closes after
  each token; the stub page never sees a token; nothing of the run (WebKit's processes, Xvfb) is left;
- native `native-webview-interactive`: the stub widget calls `before-interactive-callback` first: only then is the
  widget shown, in the game (no OS window is mapped), the spinner is hidden while the widget is drawn, the token
  joins; the stub's game server then sends a `cpt` this client cannot answer (SHA-512): it is said in the log and
  nothing is drawn;
- web `web`: the page's request for `api.js` is answered over the DevTools protocol (in the test's own browser
  only) by a stub widget that calls back with a fake token; the widget is drawn in the game, the game runs as
  before, and the `cpt` is answered with nothing shown;
- web `web-domain`: the stub widget answers `110200`; the holder has no text of ours (the widget alone), and the
  join fails with the generic error after one retry without a token (two seeks).

Every run checks the live order (seek, CaptchaRequired, generate-token, seek with both tokens) and that no
Turnstile token reaches the log. `input_script.c`'s `0 boot` starts a script's clock before any map exists, for
these runs.

## Gaps

- The challenge is Linux (WebKitGTK) and Windows (WebView2, `client/src/ui/kn_webview.c`, built when the pinned
  SDK is in `KN_WEBVIEW2_DIR`: `make webview2`). The Windows backend loads `WebView2Loader.dll` at run time and
  makes the challenge unavailable when the Edge WebView2 Runtime is missing; the frames are `CapturePreview` PNGs
  and the input window messages, so the widget on Windows is not yet verified on a real runtime. The macOS backend
  is verified against the stub page, not the live challenge; its keyboard path in the game is the gap above.
- Windows packages must ship `WebView2Loader.dll` next to `client.exe` for the challenge to run there (not yet in
  tools/release/package-client.sh); without it the challenge is unavailable, as without the runtime.
- Every Linux client package bundles WebKitGTK (tools/release/bundle-webkit.sh: `lib/`, `libexec/`), so the captcha
  needs no system package there. The client's Linux build is the dynamic one (glibc 2.35, built on Ubuntu 22.04):
  there is no webkit2gtk-4.1 for the glibc 2.17 floor of the polyfilled build, which is the server's only. WebKit 2.52 compiles in the directory of its helper processes and has
  no setting for it, so the bundle's copy of libwebkit2gtk gets that string replaced by a relative one: the
  packaged client runs from its install root (`install_root_chdir`).
- The widget is drawn only through the HTML UI's holder: an `HTML_ENGINE=OFF` client still gives up on the challenge.
- `server.config.requireCaptcha` (the custom-game setting's label, v10's own text) is kept: it names a game option,
  not the challenge.
