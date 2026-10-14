# Official krunker.io games: the matchmaker's captcha

How Krunker Box Office deals with the Cloudflare Turnstile check that `matchmaker.krunker.io`
asks for before it seeks a game (`client/include/krunker_captcha.h`, `client/src/ui/krunker_captcha_ui.c`,
`client/src/net/krunker_captcha_cb.c`, the join flow in `client/include/krunker_net.h`).

**A human solves it.** This client never solves, farms, proxies or bypasses Turnstile, does not
spoof origins and uses no solver services. It shows the challenge (web) or sends the player to
krunker.io to solve it (native), and hands the token the player got to the matchmaker once.

The in-game socket captcha `cpt` is a different thing: an ALTCHA proof of work that
`krunker_altcha.c` computes (that is how it is meant to be passed). The same modal shows it in a
passive "Verifying…" state and then the result.

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

## Native client

There is no browser engine in the client, so the modal explains the check and offers:

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
- native `native-paste`: a bad paste refused in the modal, a token the matchmaker refuses brings the
  modal back saying so, the next one joins; the box is cleared;
- web `web`: the page's request for `api.js` is answered over the DevTools protocol (in the test's own
  browser only) by a stub widget that calls back with a fake token; the game runs as before and the
  `cpt` modal shows "Verified ✓";
- web `web-domain`: the stub widget answers `110200`; the modal explains it and the pasted token joins.

Every run checks the live order (seek, CaptchaRequired, generate-token, seek with both tokens) and
that no Turnstile token reaches the log. `input_script.c`'s `0 boot` starts a script's clock before
any map exists, for these runs.

## Gaps

- Not tried against the live matchmaker with a real token: that needs a human-solved Turnstile on
  krunker.io, and joining would open a WebSocket to a live game server.
- The web build on krunk.io cannot show the widget (110200, confirmed); it can only take a pasted
  token. A `postMessage` hand-over from a krunker.io tab the page opened is possible but not built.
- The native callback is POSIX only; Windows gets the paste box.
- The paste box has no clipboard button test (the headless GLFW null platform has no clipboard).
- `accept-language` is not sent on matchmaker requests natively.
