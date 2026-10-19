#pragma once
// The two captchas of official krunker.io games, shown to the player (client/src/ui/krunker_captcha_ui.c;
// doc/v10/official-captcha.md).
//
// 1. The matchmaker's Cloudflare Turnstile: seek-game answers 400 {"error":"CaptchaRequired"}; the live
//    client (game.deob.js getCaptchaToken, :115623) renders Turnstile with KRUNKER_TURNSTILE_SITE_KEY
//    and seeks again with captchaToken + a fresh validationToken. A human solves it; this client never
//    solves, farms or bypasses it:
//      web     the real widget in a "Security Challenge" modal (the live socket captcha's look, :98251).
//              Turnstile keys are bound to their domains: on any other origin than krunker.io the
//              widget refuses (error 110200 and friends), which the modal says.
//      native  with a webview (kn_webview.h, KN_WEBVIEW: WebKitGTK on Linux, WebView2 on Windows) the challenge runs on a real
//              krunker.io page, hidden first: nothing is shown while Turnstile passes by itself. It comes
//              up (in the game UI, or in its own window) only when the player is needed, and the widget's
//              token comes back once. Without a webview, or when it fails to start or load, no challenge
//              can be shown: the request ends with unavailable = 1 and the requester retries its seek
//              without a token (krunker_net.c captcha_done). Nothing is ever pasted or posted by the player.
//    Tokens are single-use, live ~300 s, are handed to krunker_net once, never logged or stored.
// 2. The game socket's "cpt": an ALTCHA proof of work that krunker_altcha.c answers itself (computing it
//    is how that captcha is passed). Nothing is shown while it works or when it passes: the player sees
//    only the loading state. A failure shows the modal's passive card (no input) a moment.
#include <stddef.h>

// token: the Turnstile response, valid during the call only (copy it). token NULL: no token. unavailable 1 =
// no challenge could be shown here (no webview, or it failed or did not load); unavailable 0 = the player
// cancelled the request. Called once per request, never after krunker_captcha_close.
typedef void (*KrunkerCaptchaDone)(void *user, const char *token, int unavailable);

// Ask the player for a matchmaker Turnstile token. attempt: 1, 2 (the live client gives up after 2);
// rejected: the previous token was refused (expired / used), said in the modal. Replaces any request.
// When no challenge can be shown, done runs before this returns (unavailable 1).
void krunker_captcha_ask(int attempt, int rejected, KrunkerCaptchaDone done, void *user);
// The request is over (left the game / switched): close the modal, done not called.
void krunker_captcha_close(void);
int krunker_captcha_asking(void);
// The modal waits for the player (Solve / Cancel): the pointer is free and the page takes the mouse and
// keys, from the menu or from inside a game (v10 frees the pointer for its modals).
int krunker_captcha_needs_player(void);
// Main thread, once per frame (krunker_net_tick): the challenge window, timeouts.
void krunker_captcha_tick(void);
// Native, the challenge drawn in the game UI (kn_webview.h; the window is its fallback): the pointer and
// the keys go to it while it is shown there, before the page or the game gets them. Each returns 1 when
// the webview took the input. kind: KN_WEBVIEW_POINTER_* (button: GTK's 1 / 2 / 3); pressed: 1 press,
// 0 release, 2 repeat (GLFW's actions); kn_key: KN_WEBVIEW_KEY_*. Web: always 0.
int krunker_captcha_inline_pointer(int kind, double x, double y, int button);
int krunker_captcha_inline_key(int kn_key, int pressed);
int krunker_captcha_inline_char(unsigned codepoint);

// The socket "cpt", solved by this client: begin / end(solved) show nothing (logged only); end(0) shows
// the modal's passive card saying it failed (a moment, then hidden). Never blocks the game's pointer.
void krunker_captcha_pow_begin(void);
void krunker_captcha_pow_end(int solved);

// The shape of a Turnstile response: 20..4096 characters of [A-Za-z0-9._~+/=-] (Cloudflare's tokens
// are dot-separated base64url-ish; the test sitekeys answer "XXXX.DUMMY.TOKEN.XXXX"). 1 = plausible.
int krunker_captcha_token_shape_ok(const char *token);
#define KRUNKER_CAPTCHA_TOKEN_MAX 4096
