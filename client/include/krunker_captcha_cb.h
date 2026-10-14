#pragma once
// A one-shot localhost callback for the matchmaker's Turnstile token (native; krunker_captcha.h path 2b).
//
// The player's OWN userscript on https://krunker.io (doc/v10/official-captcha.md; nothing is installed
// for them) renders the widget there, where the sitekey is valid, and POSTs the token it got to
//     http://127.0.0.1:<port>/<secret>
// The listener binds 127.0.0.1 only, on a port the OS picks; <secret> is 128 random bits (hex) made per
// listener; it lives KRUNKER_CAPTCHA_CB_TTL_MS (KRUNKNATIVE_KRUNKER_CAPTCHA_TTL_MS overrides it, tests)
// and takes ONE token: later POSTs get 410, a wrong path 404, any Origin but https://krunker.io 403, a
// body that is not a token 400 (not consumed); 16 wrong paths close it. Body: the token as text/plain,
// or JSON {"token": "..."}. CORS / Private Network Access preflights from https://krunker.io are
// answered. Everything runs on the main thread (non-blocking sockets polled per frame). Never logs the
// secret or the token. Windows: not available (krunker_captcha_cb_start returns 0).
#include <stddef.h>

#define KRUNKER_CAPTCHA_CB_TTL_MS 300000 // Turnstile tokens live 300 s: the player has as long to solve

// Starts a listener (stopping any earlier one). url: http://127.0.0.1:<port>/<secret>. 1 = listening.
int krunker_captcha_cb_start(char *url, size_t n);
void krunker_captcha_cb_stop(void);
int krunker_captcha_cb_listening(void); // open and not expired
// Serves pending requests. Returns 1 once when a token arrived (copied into token, NUL-terminated;
// the listener then answers 410 until stopped), -1 once when the listener expired (it is closed),
// 0 otherwise.
int krunker_captcha_cb_poll(char *token, size_t n);
