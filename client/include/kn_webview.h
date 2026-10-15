#pragma once
// A platform webview in its own OS window, showing krunker.io's Cloudflare Turnstile challenge for
// the PLAYER to solve (krunker_captcha.h, doc/v10/official-captcha.md "Native client: the webview").
//
// The window navigates to a real krunker.io page (KN_WEBVIEW_CAPTCHA_URL; the env
// KRUNKNATIVE_KRUNKER_CAPTCHA_URL overrides it for tests), so the challenge runs on the origin its
// sitekey is bound to: nothing is spoofed (no Origin / Referer rewriting, no HTML loaded under a
// borrowed base URI). A user script, run only when the page's origin is that URL's, renders the real
// widget with krunker.io's sitekey the way the live client does (game.deob.js getCaptchaToken, :115623:
// api.js?render=explicit, turnstile.render on <body>). The human solves it; the widget's own callback
// posts its token to a script-message handler ("knCaptcha"), which takes ONE token, only while the
// main frame is still on that origin. The token is never logged or stored; the window closes at once.
//
// Backends: Linux WebKitGTK (webkit2gtk-4.1) when built with KN_WEBVIEW (CMake -DKN_WEBVIEW=AUTO|ON|OFF,
// AUTO = on when pkg-config finds it). Everywhere else (macOS, Windows, the web build, KN_WEBVIEW off)
// kn_webview_available() is 0 and the callers keep the system-browser / paste-box paths.
// Main thread only: kn_webview_pump() runs the toolkit's events, once per frame.
#include <stddef.h>

// The real krunker.io page the challenge is shown on: its robots.txt, the smallest document krunker.io
// serves (150 bytes of text, no scripts of its own: krunker.io's game does not boot in the window, so
// it neither seeks a game nor asks for its own challenge). Any krunker.io page is the same origin.
#define KN_WEBVIEW_CAPTCHA_URL "https://krunker.io/robots.txt"

// 1 = this build has a webview backend and KRUNKNATIVE_WEBVIEW is not "0" (the toolkit may still fail
// to start: kn_webview_captcha_open says so).
int kn_webview_available(void);

// Starts the challenge on url (NULL: the env override, else KN_WEBVIEW_CAPTCHA_URL) with this sitekey,
// HIDDEN: no window exists while Turnstile passes by itself. The window is created (centred on the
// work area of the monitor under the pointer, above the game) only when the player is needed:
// Turnstile's before-interactive-callback, a widget error / expiry / timeout (its "Try again" is in
// the window), no token within KRUNKNATIVE_WEBVIEW_REVEAL_MS (default KN_WEBVIEW_REVEAL_DEFAULT_MS,
// -1 never), or show_now (the player asked to see it: the modal's button). A second call while it runs
// hidden also shows it; when shown, a second call raises it. proxy: NULL / "" = none, else the proxy URI the window's
// traffic goes through (http:// or socks5://, may hold user:password@; never logged): the caller passes
// the one its matchmaker requests use, so the challenge is solved from the same network. Loopback hosts
// never use it. 1 = the window is up (or running hidden); 0 = no webview (err says why: no display, the toolkit failed, not
// built), use the fallback.
int kn_webview_captcha_open(const char *url, const char *sitekey, const char *title, const char *proxy, int show_now, char *err, size_t errn);
int kn_webview_captcha_is_open(void); // running, shown or hidden
// Closes the window (no-op when closed). A token not yet polled is wiped.
void kn_webview_captcha_close(void);

// The hidden run's backstop (kn_webview_captcha_open): no token and no signal from the widget by then
// means it is stuck, so the window is shown. 20 s: a passing check (WebKit start, robots.txt and api.js
// through the player's proxy, Turnstile's managed check) finishes well inside it, and a widget that
// needs the player says so at once (before-interactive, error, expiry, timeout), so the wait is paid
// only by a run that gives no signal at all.
#define KN_WEBVIEW_REVEAL_DEFAULT_MS 20000

enum {
    KN_WEBVIEW_NONE = 0,
    KN_WEBVIEW_TOKEN = 1,    // the player solved it: token copied (NUL-terminated), the window closed
    KN_WEBVIEW_CLOSED = -1,  // the player closed the window
    KN_WEBVIEW_FAILED = -2,  // the page did not load / the widget refused the origin (err: why, no token)
    KN_WEBVIEW_STATUS = 2,   // the widget's state changed (err: a short line; window still hidden or up)
    KN_WEBVIEW_SHOWN = 3,    // the window was just shown: the player is needed (err: the line for the modal)
};
// Main thread, once per frame: runs the toolkit's events and returns one event (KN_WEBVIEW_*).
int kn_webview_captcha_poll(char *token, size_t n, char *err, size_t errn);

// The toolkit's events without a captcha in flight (lets a closed window's teardown finish).
void kn_webview_pump(void);
