# Official krunker.io games: the matchmaker's captcha

How Krunker Box Office deals with the Cloudflare Turnstile check that `matchmaker.krunker.io`
asks for before it seeks a game (`client/include/krunker_captcha.h`, `client/src/ui/krunker_captcha_ui.c`,
`client/src/net/krunker_captcha_cb.c`, the challenge window `client/include/kn_webview.h` /
`client/src/ui/kn_webview.c`, the join flow in `client/include/krunker_net.h`).

**A human solves it.** This client never solves, farms, proxies or bypasses Turnstile, does not
spoof origins and uses no solver services. It shows the challenge (web; native in a window on
krunker.io itself) or sends the player to krunker.io to solve it, and hands the token the player got
to the matchmaker once.

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
   on the sites they were made for (error 110200)…"), then offers the manual path: open krunker.io,
   paste a token, or play the game on krunker.io. So on krunk.io, in practice, the paste box is the
   path. Nothing works around the binding.
4. Other errors, the widget's own timeout and our 120 s limit show "Try again" plus the manual path.

## Native client: the challenge window (KN_WEBVIEW)

Built with a webview (CMake `-DKN_WEBVIEW=AUTO|ON|OFF`; AUTO, the default, is on when pkg-config finds
`webkit2gtk-4.1`; Linux only for now), `CaptchaRequired` runs the challenge **silently first**: a
WebKitGTK view (a GTK offscreen window, never drawn into the GL scene) on a **real krunker.io page**,
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
  domain: should not happen on krunker.io) closes the window and falls back to the manual path.
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
  modal shows the challenge where the live client shows its Turnstile holder: the dim backdrop, the
  white box, "Security Challenge", the text and Cancel, with the widget centred in it. No OS window
  opens. The view stays in its hidden GTK offscreen host; the page is only the widget on a transparent
  background (the robots.txt text is hidden, no card). Each paint of the host is converted
  (cairo ARGB32, premultiplied, to straight RGBA8) and put into a `<canvas>` of the modal
  (`kn-captcha-webview`, 340 x 170 px, `html_dom_canvas_put`). While it is shown, the pointer over the
  canvas and the widget's keys (Tab, Enter, Space, Backspace, Delete, arrows, Home, End and typed text)
  go to the view (`krunker_captcha_inline_pointer` / `_key` / `_char`, routed first in `main.c`; a
  press that starts on the widget keeps its release). Escape is not forwarded: it stays the menu's.
  The view's own window is the fallback: when the game UI draws no frame within 5 s of showing it
  (`INLINE_STALL_MS`), the challenge moves into a window (dark background, the widget centred), and
  `KRUNKNATIVE_CAPTCHA_WINDOW=1` keeps the window path only. macOS and Windows have no webview backend
  (their paths are unchanged).
- **The window path, for the fallback and the env override:** the view moves into a real window,
  centred on the work area of the monitor under the pointer and kept above the game. There the modal has
  only "Solve the challenge" (raises the window) **and Cancel**; closing the window says so ("Solve the
  challenge" opens it again).
- **In both,** the paste box, the userscript callback and the "open krunker.io in the browser" path
  stay hidden: the webview is the one path the player sees (the user asked not to be shown the
  paste / userscript route when the webview works). Cancel ends the join. A refused token (used or
  expired) starts a fresh silent run, at most twice like the live client.
- **Fallbacks only when the window can't be the path:** no webview in the build (macOS, Windows,
  `KN_WEBVIEW=OFF`, the release packages), `KRUNKNATIVE_WEBVIEW=0`, no display for GTK, or the window
  failing to start / load / returning a non-token — then the modal shows the manual path below (open
  krunker.io, the paste box, the userscript callback), with "Solve the challenge" still there to try
  the window again.
  GTK is started on first use with `gtk_disable_setlocale()` (the game's "C" number formatting stays)
  and pumped from `krunker_captcha_tick` once per frame on the main thread.
- **Checked against the live site once (2026-10-14, the scratch harness of this change, through the
  proxy):** the window loaded `https://krunker.io/robots.txt`, Cloudflare's real widget rendered there
  with krunker.io's sitekey, no 1102xx domain error, and Cloudflare itself answered with a token
  (688 characters, no interaction asked of that browser). It was discarded, never sent anywhere; no
  matchmaker request was made.

## Native client: the manual path

Without the challenge window (or when it cannot start), the modal explains the check and offers:

- **Open krunker.io**: the system browser (`ui_open_browser`). With the callback below running, the
  link is `https://krunker.io/#kn-captcha=<port>-<secret>` (a fragment never reaches krunker.io's
  servers; the client log shows `#(not logged)`).
- **Paste a token** (2a): the box takes a token the player got on krunker.io with their own tools.
  It is shape-checked (20 to 4096 characters of `A-Z a-z 0-9 . _ - ~ + / =`), used once, then the
  box is cleared.
- **The localhost callback** (2b): `http://127.0.0.1:<port>/<secret>`, shown in the modal with a
  Copy button, for the player's own userscript:
  - binds 127.0.0.1 only, on a port the OS picks; the secret is 128 random bits per listener;
  - lives 5 minutes (as long as a Turnstile token); "Open krunker.io" starts a new one after that;
  - takes **one** token: `POST` the token as `text/plain` (or JSON `{"token": "..."}`) → `200`; later
    POSTs `410` until the listener closes (right after the token is handed over: a late POST is
    refused at the TCP level); a wrong path `404` (16 of them close it); an `Origin` other than
    `https://krunker.io` `403`; a body that is not a token `400` (not consumed);
  - answers the CORS / Private Network Access preflight from `https://krunker.io`;
  - polled on the main thread, logs neither the secret nor the token.

Whatever the path, the token goes to `krunker_net.c`, which fetches a fresh validation token and seeks
again. If the matchmaker still answers `CaptchaRequired` (the token was used or expired: Turnstile
tokens work once, for 300 s), the modal comes back saying so; after the second refusal the join fails
like the live client's (`IÏìiííi >= 2`). Cancel stops the join.

### The companion userscript (install it yourself)

Nothing is installed for you. This is a template for a userscript manager (Tampermonkey,
Violentmonkey). It only does something on a krunker.io page opened by the client's
"Open krunker.io" button (the `#kn-captcha=` fragment): it renders Turnstile there, where krunker.io's
sitekey is valid, you solve it, and it sends the token to the client's one-time address. The krunker.io
page may also show its own challenge for its own matchmaking; that one is the page's.

```js
// ==UserScript==
// @name         Krunker Box Office: Turnstile token to the native client
// @namespace    krunknative
// @version      1
// @match        https://krunker.io/*
// @run-at       document-start
// @grant        GM_xmlhttpRequest
// @connect      127.0.0.1
// ==/UserScript==
(function () {
  'use strict';
  const m = /(?:^#|&)kn-captcha=(\d{1,5})-([0-9a-f]{32})(?:&|$)/.exec(location.hash);
  if (!m) return;
  const target = 'http://127.0.0.1:' + m[1] + '/' + m[2];
  history.replaceState(history.state, '', location.pathname + location.search); // drop the fragment
  const SITEKEY = '0x4AAAAAAD19NTFH3Itq7QOn'; // KRUNKER_TURNSTILE_SITE_KEY (shared/include/krunker_build.h)

  function send(token, say) {
    const done = (ok, why) => say(ok ? 'Sent. Back to Krunker Box Office.' : 'Not sent (' + why + ').');
    if (typeof GM_xmlhttpRequest === 'function') {
      GM_xmlhttpRequest({method: 'POST', url: target, data: token, headers: {'Content-Type': 'text/plain'},
        onload: r => done(r.status === 200, 'HTTP ' + r.status), onerror: () => done(false, 'no answer')});
    } else {
      fetch(target, {method: 'POST', body: token, headers: {'Content-Type': 'text/plain'}})
        .then(r => done(r.ok, 'HTTP ' + r.status), () => done(false, 'no answer'));
    }
  }

  function loadTurnstile() {
    if (window.turnstile) return Promise.resolve();
    return new Promise((ok, fail) => {
      const s = document.createElement('script');
      s.src = 'https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit';
      s.onload = ok;
      s.onerror = fail;
      document.head.appendChild(s);
    });
  }

  function start() {
    const shade = document.createElement('div');
    shade.style.cssText = 'position:fixed;inset:0;z-index:2147483647;background:rgba(0,0,0,.6);' +
      'display:flex;align-items:center;justify-content:center;font:16px sans-serif';
    const card = document.createElement('div');
    card.style.cssText = 'background:#fff;color:#000;padding:20px;border-radius:6px;width:360px;text-align:center';
    const title = document.createElement('div');
    title.textContent = 'Krunker Box Office: verification';
    title.style.cssText = 'font-size:20px;margin-bottom:10px';
    const holder = document.createElement('div');
    const status = document.createElement('div');
    status.style.cssText = 'margin-top:10px;color:#555';
    const close = document.createElement('button');
    close.textContent = 'Close';
    close.style.marginTop = '10px';
    close.onclick = () => shade.remove();
    card.append(title, holder, status, close);
    shade.append(card);
    document.body.append(shade);
    const say = text => { status.textContent = text; };
    loadTurnstile().then(() => {
      window.turnstile.render(holder, {
        sitekey: SITEKEY,
        callback: token => send(token, say),
        'error-callback': code => { say('Cloudflare error ' + code); return true; },
      });
    }, () => say('Turnstile did not load.'));
  }

  if (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', start);
  else start();
})();
```

This script was not run against the live krunker.io (that needs a real browser session solving a real
challenge); the client side of the exchange is covered by the tests below.

## Tests

`server/tests/krunker_official_e2e.py` (local stubs only; its matchmaker answers the first seek
`CaptchaRequired` and lets a seek with an unused `captchaToken` + an issued `validationToken` through):

- native `native`: the callback (wrong secret 404, foreign Origin 403, a non-token 400, the preflight,
  refused after its TTL, "Open krunker.io" re-arms a new secret, one token taken, a second refused), then
  the whole game as before;
- native `native-token-refused` / `native-token-refused-twice` / `native-token-reuse` (`--token`): the
  matchmaker refuses the captcha's token as stale (one retry: a fresh token, no captcha, the second
  captcha, joined; a second refusal is "Socket Error"), and the build's retry after a captcha carries no
  validation token (the spent one is never sent again: that was "Socket Error" before);
- native `native-paste`: a bad paste refused in the modal, a token the matchmaker refuses brings the
  modal back saying so, the next one joins; the box is cleared;
- native `native-webview` (alone: `--webview`; skipped with a message, exit 77 alone, when the build
  has no webview or `xvfb-run` is missing): client_bin under its own `xvfb-run` (GLFW headless, GTK on
  the private Xvfb), `KRUNKNATIVE_KRUNKER_CAPTCHA_URL` pointing at the test's stub page, which defines a
  stub `window.turnstile` that reports how it was rendered and "is solved" 1.5 s later by firing the
  widget's own callback. The matchmaker refuses the first token; the challenge runs again, the second
  token joins and the game runs. Silent first: no window is ever mapped, and the modal stays hidden at
  every probe from the first seek through the game socket's `cpt` (answered with no card), so the
  player sees only the loading state; no callback is armed; the page's own scripts see no `knCaptcha` handler;
  a frame of another origin is refused and its forged token never reaches the matchmaker; the window
  closes after each token; the stub page never sees a token; nothing of the run (WebKit's processes,
  Xvfb) is left. The `native` / `native-paste` runs have no display, so in a webview build they also
  cover GTK failing to start and the fallback;
- native `native-webview-interactive`: the stub widget calls `before-interactive-callback` first: only
  then is the window mapped (centred, never moved) and the modal comes up with it; the stub's game
  server then sends a `cpt` this client cannot answer (SHA-512), and only that failure shows the
  passive card;
- web `web`: the page's request for `api.js` is answered over the DevTools protocol (in the test's own
  browser only) by a stub widget that calls back with a fake token; the game runs as before and the
  `cpt` is answered with no card on screen;
- web `web-domain`: the stub widget answers `110200`; the modal explains it and the pasted token joins.

Every run checks the live order (seek, CaptchaRequired, generate-token, seek with both tokens) and
that no Turnstile token reaches the log. `input_script.c`'s `0 boot` starts a script's clock before
any map exists, for these runs.

## Gaps

- The challenge window is Linux (WebKitGTK) only. macOS (WKWebView) and Windows (WebView2) are not
  wired: they keep the browser / paste-box path. `kn_webview.h` is the seam for them.
- The release packages are built with `KN_WEBVIEW=OFF` (tools/release/build-client-linux.sh): linking
  WebKitGTK would make it a hard dependency of the portable client. Loading it at run time (dlopen)
  would lift that.
- The window's own texts (card, statuses from the widget) are English; the modal's are localised.
- No window manager under the tests' Xvfb: the window's placement, `keep_above` over a fullscreen
  game and focus were not checked on a real desktop (Wayland / X11, NVIDIA). WebKitGTK on some NVIDIA
  setups needs `WEBKIT_DISABLE_DMABUF_RENDERER=1` (a WebKitGTK issue, not set here).
- The window is used only through the HTML UI's modal: an `HTML_ENGINE=OFF` client still gives up on
  the captcha.

- Not tried against the live matchmaker with a real token: that needs a human-solved Turnstile on
  krunker.io, and joining would open a WebSocket to a live game server.
- The web build on krunk.io cannot show the widget (110200, confirmed); it can only take a pasted
  token. A `postMessage` hand-over from a krunker.io tab the page opened is possible but not built.
- The native callback is POSIX only; Windows gets the paste box.
- The paste box has no clipboard button test (the headless GLFW null platform has no clipboard).
- `accept-language` is not sent on matchmaker requests natively.
