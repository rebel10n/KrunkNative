#pragma once
// The two captchas of official krunker.io games, shown to the player (client/src/ui/krunker_captcha_ui.c,
// the localhost callback client/src/net/krunker_captcha_cb.c; doc/v10/official-captcha.md).
//
// 1. The matchmaker's Cloudflare Turnstile: seek-game answers 400 {"error":"CaptchaRequired"}; the live
//    client (game.deob.js getCaptchaToken, :115623) renders Turnstile with KRUNKER_TURNSTILE_SITE_KEY
//    and seeks again with captchaToken + a fresh validationToken. A human solves it; this client never
//    solves, farms or bypasses it:
//      web     the real widget in a "Security Challenge" modal (the live socket captcha's look, :98251).
//              Turnstile keys are bound to their domains: on any other origin than krunker.io the
//              widget refuses (error 110200 and friends), which the modal says, falling back to:
//      native  with a webview (kn_webview.h, KN_WEBVIEW: WebKitGTK on Linux) the modal opens a window
//              on a real krunker.io page with Cloudflare's widget, the player solves it there and the
//              widget's token comes back once. The window runs hidden first: the modal stays hidden
//              while Turnstile passes by itself, and comes up (with the window shown) only when the
//              player is needed. The paths below stay as "Open in your browser instead" and the paste
//              box, and take over when the window cannot start or load.
//              Without one the modal explains, opens krunker.io in the system browser, and
//              takes the token either pasted by the player (2a) or POSTed by the player's own
//              userscript on krunker.io to a one-shot 127.0.0.1 callback (2b, krunker_captcha_cb.h).
//    Tokens are single-use, live ~300 s, are handed to krunker_net once, never logged or stored.
// 2. The game socket's "cpt": an ALTCHA proof of work that krunker_altcha.c answers itself (computing it
//    is how that captcha is passed). Nothing is shown while it works or when it passes: the player sees
//    only the loading state. A failure shows the modal's passive card (no input) a moment.
#include <stddef.h>

// token: the Turnstile response, valid during the call only (copy it); NULL = the player cancelled /
// the challenge failed for good.
typedef void (*KrunkerCaptchaDone)(void *user, const char *token);

// Ask the player for a matchmaker Turnstile token. attempt: 1, 2 (the live client gives up after 2);
// rejected: the previous token was refused (expired / used), said in the modal. Replaces any request.
void krunker_captcha_ask(int attempt, int rejected, KrunkerCaptchaDone done, void *user);
// The request is over (left the game / switched): close the modal and the callback, done not called.
void krunker_captcha_close(void);
int krunker_captcha_asking(void);
// The modal waits for the player (a paste box, Solve / Cancel): the pointer is free and the page takes
// the mouse and keys, from the menu or from inside a game (v10 frees the pointer for its modals).
int krunker_captcha_needs_player(void);
// Main thread, once per frame (krunker_net_tick): the callback listener, the challenge window, timeouts.
void krunker_captcha_tick(void);

// The socket "cpt", solved by this client: begin / end(solved) show nothing (logged only); end(0) shows
// the modal's passive card saying it failed (a moment, then hidden). Never blocks the game's pointer.
void krunker_captcha_pow_begin(void);
void krunker_captcha_pow_end(int solved);

// The shape of a Turnstile response: 20..4096 characters of [A-Za-z0-9._~+/=-] (Cloudflare's tokens
// are dot-separated base64url-ish; the test sitekeys answer "XXXX.DUMMY.TOKEN.XXXX"). 1 = plausible.
int krunker_captcha_token_shape_ok(const char *token);
#define KRUNKER_CAPTCHA_TOKEN_MAX 4096
