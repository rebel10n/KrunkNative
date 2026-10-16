#pragma once
// A platform webview showing krunker.io's Cloudflare Turnstile challenge for the PLAYER to solve
// (krunker_captcha.h, doc/v10/official-captcha.md "Native client: the webview").
//
// The view navigates to a real krunker.io page (KN_WEBVIEW_CAPTCHA_URL; the env
// KRUNKNATIVE_KRUNKER_CAPTCHA_URL overrides it for tests), so the challenge runs on the origin its
// sitekey is bound to: nothing is spoofed (no Origin / Referer rewriting, no HTML loaded under a
// borrowed base URI). A user script, run only when the page's origin is that URL's, renders the real
// widget with krunker.io's sitekey the way the live client does (game.deob.js getCaptchaToken, :115623:
// api.js?render=explicit, turnstile.render on <body>). The human solves it; the widget's own callback
// posts its token to a script-message handler ("knCaptcha"), which takes ONE token, only while the
// main frame is still on that origin. The token is never logged or stored; the view closes at once.
//
// Where the player solves it (Linux, WebKitGTK):
//   - in the game UI (kn_webview_captcha_open in_ui = 1, the default; KRUNKNATIVE_CAPTCHA_WINDOW=1
//     forces a window): the page is only the widget on a transparent background, the view stays in its
//     hidden GTK offscreen host, and the caller draws kn_webview_captcha_frame()'s pixels in its own
//     modal, sending the pointer and keys to kn_webview_captcha_pointer / _key / _char. No OS window.
//   - in a window: the view moves into a real window (centred, above the game). This is the fallback
//     when the game UI cannot draw the frames (kn_webview_captcha_inline() is 0 after the reveal), and
//     the only mode when in_ui is 0.
// Backends: Linux WebKitGTK (webkit2gtk-4.1) when built with KN_WEBVIEW (CMake -DKN_WEBVIEW=AUTO|ON|OFF,
// AUTO = on when pkg-config finds it). Everywhere else (macOS, Windows, the web build, KN_WEBVIEW off)
// kn_webview_available() is 0 and the callers keep the system-browser / paste-box paths.
// Main thread only: kn_webview_pump() runs the toolkit's events, once per frame.
#include <stddef.h>

// The real krunker.io page the challenge is shown on: its robots.txt, the smallest document krunker.io
// serves (150 bytes of text, no scripts of its own: krunker.io's game does not boot in the view, so
// it neither seeks a game nor asks for its own challenge). Any krunker.io page is the same origin.
#define KN_WEBVIEW_CAPTCHA_URL "https://krunker.io/robots.txt"

// The size of the challenge in the game UI (the canvas the caller draws; the page lays out the widget
// in it). The window keeps its own size.
#define KN_WEBVIEW_INLINE_W 340
#define KN_WEBVIEW_INLINE_H 170

// 1 = this build has a webview backend and KRUNKNATIVE_WEBVIEW is not "0" (the toolkit may still fail
// to start: kn_webview_captcha_open says so).
int kn_webview_available(void);

// Starts the challenge on url (NULL: the env override, else KN_WEBVIEW_CAPTCHA_URL) with this sitekey,
// HIDDEN: nothing is shown while Turnstile passes by itself. The challenge is shown only when the player
// is needed: Turnstile's before-interactive-callback, a widget error / expiry / timeout (its "Try again"
// is in the challenge), no token within KRUNKNATIVE_WEBVIEW_REVEAL_MS (default KN_WEBVIEW_REVEAL_DEFAULT_MS,
// -1 never), or show_now (the player asked to see it: the modal's button). in_ui: shown in the game UI
// (see the top of this file); 0 = in a window centred on the work area of the monitor under the pointer,
// above the game. A second call while it runs hidden also shows it; when shown, a second call raises it.
// proxy: NULL / "" = none, else the proxy URI the view's traffic goes through (http:// or socks5://, may
// hold user:password@; never logged): the caller passes the one its matchmaker requests use, so the
// challenge is solved from the same network. Loopback hosts never use it. 1 = the challenge is up (or
// running hidden); 0 = no webview (err says why: no display, the toolkit failed, not built), use the
// fallback.
int kn_webview_captcha_open(const char *url, const char *sitekey, const char *title, const char *proxy, int in_ui, int show_now, char *err,
                            size_t errn);
int kn_webview_captcha_is_open(void); // running, shown or hidden
// Closes the challenge (no-op when closed). A token not yet polled is wiped.
void kn_webview_captcha_close(void);

// The state of a shown challenge: 1 = drawn in the game UI (frames + input below), 0 = hidden or in a window.
int kn_webview_captcha_inline(void);
// 1 = a real window holds the challenge (the window path, or the game UI's fallback).
int kn_webview_captcha_windowed(void);
// In the game UI: the newest frame when the page has painted since the last call. rgba is straight
// RGBA8, row 0 on top, *w x *h px, valid until the next call or close. 0 = no new frame.
int kn_webview_captcha_frame(const unsigned char **rgba, int *w, int *h);
// In the game UI: the pointer over the view, in view px (0..KN_WEBVIEW_INLINE_W/H). kind: a
// KN_WEBVIEW_POINTER_*; button: GTK's 1 left, 2 middle, 3 right (0 for a move). 1 = sent to the page.
enum { KN_WEBVIEW_POINTER_MOVE = 0, KN_WEBVIEW_POINTER_PRESS = 1, KN_WEBVIEW_POINTER_RELEASE = 2 };
int kn_webview_captcha_pointer(int kind, double x, double y, int button);
// In the game UI: a key of the widget (Tab, Enter, Space, ...) and typed text. pressed: 1 press, 0
// release, 2 repeat (not sent). 1 = sent to the page.
enum {
    KN_WEBVIEW_KEY_NONE = 0,
    KN_WEBVIEW_KEY_TAB,
    KN_WEBVIEW_KEY_ENTER,
    KN_WEBVIEW_KEY_SPACE,
    KN_WEBVIEW_KEY_BACKSPACE,
    KN_WEBVIEW_KEY_DELETE,
    KN_WEBVIEW_KEY_LEFT,
    KN_WEBVIEW_KEY_RIGHT,
    KN_WEBVIEW_KEY_UP,
    KN_WEBVIEW_KEY_DOWN,
    KN_WEBVIEW_KEY_HOME,
    KN_WEBVIEW_KEY_END,
};
int kn_webview_captcha_key(int key, int pressed);
int kn_webview_captcha_char(unsigned codepoint);

// The hidden run's backstop (kn_webview_captcha_open): no token and no signal from the widget by then
// means it is stuck, so the challenge is shown. 20 s: a passing check (WebKit start, robots.txt and api.js
// through the player's proxy, Turnstile's managed check) finishes well inside it, and a widget that
// needs the player says so at once (before-interactive, error, expiry, timeout), so the wait is paid
// only by a run that gives no signal at all.
#define KN_WEBVIEW_REVEAL_DEFAULT_MS 20000

enum {
    KN_WEBVIEW_NONE = 0,
    KN_WEBVIEW_TOKEN = 1,    // the player solved it: token copied (NUL-terminated), the challenge closed
    KN_WEBVIEW_CLOSED = -1,  // the player closed the window
    KN_WEBVIEW_FAILED = -2,  // the page did not load / the widget refused the origin (err: why, no token)
    KN_WEBVIEW_STATUS = 2,   // the widget's state changed (err: a short line; still hidden or shown)
    KN_WEBVIEW_SHOWN = 3,    // the challenge was just shown: the player is needed (err: the line for the modal);
                             // kn_webview_captcha_inline() tells the game UI from a window
};
// Main thread, once per frame: runs the toolkit's events and returns one event (KN_WEBVIEW_*).
int kn_webview_captcha_poll(char *token, size_t n, char *err, size_t errn);

// The toolkit's events without a captcha in flight (lets a closed window's teardown finish).
void kn_webview_pump(void);

// ---- decisions and conversions (pure: client/tests/kn_webview_test.c)
// Where a reveal puts the challenge: KN_WEBVIEW_VIEW_INLINE (the game UI) when in_ui and the game UI has
// not failed to draw it, else KN_WEBVIEW_VIEW_WINDOW.
enum { KN_WEBVIEW_VIEW_INLINE = 1, KN_WEBVIEW_VIEW_WINDOW = 2 };
int kn_webview_reveal_target(int in_ui, int inline_failed);
// The game UI's stall watchdog: 1 when it has drawn no frame and limit_ms have passed since it was shown.
int kn_webview_inline_stalled(int frames, long long elapsed_ms, long long limit_ms);
// Straight RGBA8 (row 0 on top, w x h px, dst = 4 * w * h bytes) from a cairo ARGB32 image (premultiplied,
// stride bytes per row, native 32-bit pixels).
void kn_webview_rgba_from_argb32(const unsigned char *src, int stride, int w, int h, unsigned char *dst);
