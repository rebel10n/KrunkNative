#pragma once
// A platform webview showing krunker.io's Cloudflare Turnstile challenge for the PLAYER to solve
// (krunker_captcha.h, doc/v10/official-captcha.md "Native client: the webview").
//
// The view navigates to a real krunker.io page (KN_WEBVIEW_CAPTCHA_URL; the env
// KRUNKNATIVE_KRUNKER_CAPTCHA_URL overrides it for tests), so the challenge runs on the origin its
// sitekey is bound to: nothing is spoofed (no Origin / Referer rewriting, no HTML loaded under a
// borrowed base URI). A user script, run only when the page's origin is that URL's, renders the real
// widget with krunker.io's sitekey the way the live client does (game.deob.js getCaptchaToken, :115623:
// api.js?render=explicit, turnstile.render, appearance "interaction-only"). The human solves it; the widget's
// own callback posts its token to a script-message handler ("knCaptcha"), which takes ONE token, only while the
// main frame is still on that origin. The token is never logged or stored; the view closes at once.
//
// Where the player solves it: in the game, always. The page is only the widget on a transparent background,
// and the view stays in its hidden host (GTK offscreen, WebView2 off-desktop popup, macOS invisible window).
// The caller draws kn_webview_captcha_frame()'s pixels where the join's spinner is and sends the pointer and
// keys to kn_webview_captcha_pointer / _key / _char. No backend opens an OS window for the challenge.
// Backends: Linux WebKitGTK (webkit2gtk-4.1), macOS WKWebView (its section of kn_webview.c, Objective-C with ARC)
// and Windows WebView2 (the Edge runtime, through its C interface) when built with KN_WEBVIEW (CMake
// -DKN_WEBVIEW=AUTO|ON|OFF; AUTO = on when pkg-config finds webkit2gtk-4.1, on macOS, or the WebView2 SDK is in
// KN_WEBVIEW2_DIR). Everywhere else (the web build, KN_WEBVIEW off, no WebView2 runtime) kn_webview_available()
// is 0 and the callers end the request (no challenge is shown). On macOS the frames are WKWebView snapshots and
// the input is sent as NSEvents.
// Main thread only: kn_webview_pump() runs the toolkit's events, once per frame.
#include <stddef.h>

// The real krunker.io page the challenge is shown on: its robots.txt, the smallest document krunker.io
// serves (150 bytes of text, no scripts of its own: krunker.io's game does not boot in the view, so
// it neither seeks a game nor asks for its own challenge). Any krunker.io page is the same origin.
#define KN_WEBVIEW_CAPTCHA_URL "https://krunker.io/robots.txt"

// The size of the widget's canvas in the game (the page lays the widget out in it). The caller draws it
// centred where the join's spinner is.
#define KN_WEBVIEW_INLINE_W 340
#define KN_WEBVIEW_INLINE_H 170

// 1 = this build has a webview backend and KRUNKNATIVE_WEBVIEW is not "0" (the toolkit may still fail
// to start: kn_webview_captcha_open says so).
int kn_webview_available(void);
// Windows: the main thread takes the single-threaded apartment WebView2 needs, before the audio starts (miniaudio's
// multithreaded request is the one refused). Every other platform: nothing. main() calls it first.
void kn_webview_apartment_init(void);

// Starts the challenge on url (NULL: the env override, else KN_WEBVIEW_CAPTCHA_URL) with this sitekey,
// HIDDEN: nothing is shown while Turnstile passes by itself. The widget is shown only when the player
// is needed: Turnstile's before-interactive-callback, a widget error / expiry / timeout (the challenge then
// ends with KN_WEBVIEW_FAILED), no token within KRUNKNATIVE_WEBVIEW_REVEAL_MS (default KN_WEBVIEW_REVEAL_DEFAULT_MS,
// -1 never), or show_now. title and in_ui are not used: the challenge is always the widget in the game (they
// stay in the signature so its callers do not change). proxy: NULL / "" = none, else the proxy URI the view's
// traffic goes through (http:// or socks5://, may hold user:password@; never logged): the caller passes the one
// its matchmaker requests use, so the challenge is solved from the same network. Loopback hosts never use it.
// 1 = the challenge is up (or running hidden); 0 = no webview (err says why: no display, the toolkit failed, not
// built), use the fallback.
int kn_webview_captcha_open(const char *url, const char *sitekey, const char *title, const char *proxy, int in_ui, int show_now, char *err,
                            size_t errn);
int kn_webview_captcha_is_open(void); // running, shown or hidden
// Closes the challenge (no-op when closed). A token not yet polled is wiped.
void kn_webview_captcha_close(void);

// The state of a shown challenge: 1 = the widget is shown (the caller draws its frames and sends its input below).
int kn_webview_captcha_inline(void);
// Always 0: no backend opens a window for the challenge. Kept so the callers and the tests can check it.
int kn_webview_captcha_windowed(void);
// The newest frame when the page has painted since the last call. rgba is straight RGBA8, row 0 on top,
// *w x *h px, valid until the next call or close. 0 = no new frame.
int kn_webview_captcha_frame(const unsigned char **rgba, int *w, int *h);
// The pointer over the view, in view px (0..KN_WEBVIEW_INLINE_W/H). kind: a KN_WEBVIEW_POINTER_*; button:
// GTK's 1 left, 2 middle, 3 right (0 for a move). 1 = sent to the page.
enum { KN_WEBVIEW_POINTER_MOVE = 0, KN_WEBVIEW_POINTER_PRESS = 1, KN_WEBVIEW_POINTER_RELEASE = 2 };
int kn_webview_captcha_pointer(int kind, double x, double y, int button);
// A key of the widget (Tab, Enter, Space, ...) and typed text. pressed: 1 press, 0 release, 2 repeat (not sent).
// 1 = sent to the page.
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
// means it is stuck, so the widget is shown. 20 s: a passing check (WebKit start, robots.txt and api.js
// through the player's proxy, Turnstile's managed check) finishes well inside it, and a widget that
// needs the player says so at once (before-interactive), so the wait is paid only by a run that gives no
// signal at all.
#define KN_WEBVIEW_REVEAL_DEFAULT_MS 20000

enum {
    KN_WEBVIEW_NONE = 0,
    KN_WEBVIEW_TOKEN = 1,   // the challenge gave a token (Turnstile passed or the player solved it): copied (NUL-terminated), closed
    KN_WEBVIEW_FAILED = -2, // the challenge could not be completed (err: why, no token): closed
    KN_WEBVIEW_SHOWN = 3,   // the widget was just shown: the player is needed (err: the reason, for the log)
};
// Main thread, once per frame: runs the toolkit's events and returns one event (KN_WEBVIEW_*).
int kn_webview_captcha_poll(char *token, size_t n, char *err, size_t errn);

// The toolkit's events without a captcha in flight (lets a closed view's teardown finish).
void kn_webview_pump(void);

// ---- conversions (pure: client/tests/kn_webview_test.c)
// Straight RGBA8 (row 0 on top, w x h px, dst = 4 * w * h bytes) from a cairo ARGB32 image (premultiplied,
// stride bytes per row, native 32-bit pixels).
void kn_webview_rgba_from_argb32(const unsigned char *src, int stride, int w, int h, unsigned char *dst);
