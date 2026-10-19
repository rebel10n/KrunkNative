// The challenge window of kn_webview.h: WebKitGTK (webkit2gtk-4.1) on Linux, WKWebView on macOS and WebView2
// (the Edge WebView2 runtime) on Windows, when built with KN_WEBVIEW; a stub everywhere else. CMake builds
// this file into its own library (kn_webview) with the toolkit's include paths, so GTK's, WebKit's and
// WebView2's headers never reach the rest of the client.
//
// What the window does (and does not), on both backends:
//   - runs hidden first: the view lives in a hidden host (GTK: a GtkOffscreenWindow; WebView2: a popup
//     parked off the desktop that holds the view's controller). The page and Cloudflare's widget run
//     normally there. A challenge that passes by itself (Turnstile's non-interactive / managed check)
//     never shows anything. Only when the player is needed is the view moved into a real window, created
//     centred on the work area of the monitor under the pointer and kept above the game (reveal()):
//     Turnstile's before-interactive-callback (interactive:), a widget error / expiry / timeout (the
//     widget's own "Try again" is in the window), no token after KN_WEBVIEW_REVEAL_MS
//     (KRUNKNATIVE_WEBVIEW_REVEAL_MS; Cloudflare's interstitial and an HTTP error page are not taken as
//     "needs the player": they may pass by themselves), or the player asking for it (kn_webview_captcha_open
//     with show_now);
//   - navigates to a REAL krunker.io URL (https only; plain http only to a loopback host, for the
//     tests' stub page) in an ephemeral profile: GTK an ephemeral web context, WebView2 a temporary user
//     data folder removed at close. Nothing of it is kept;
//   - injects one user script, which does nothing unless the top document's origin is that URL's; it
//     draws a card over the page and renders Cloudflare's widget with krunker.io's sitekey, loading
//     challenges.cloudflare.com's api.js like the live client (or using the page's own `turnstile`);
//   - the widget's callbacks post "kind:value" strings to the host. GTK: a second script in an isolated
//     world (BRIDGE_WORLD) is the only one that can post to the "knCaptcha" handler, so frames of other
//     origins and the page's own scripts cannot reach it. WebView2 has no isolated worlds: the widget posts
//     with window.chrome.webview.postMessage, and the host takes a message only when its source is the
//     expected origin (the page has no scripts of its own: Cloudflare's are the only ones that run there).
//     Either way only the first token is taken, only while the page is on the expected origin, it goes to
//     the caller (krunker_captcha_ui.c) once and is wiped here. It is never logged;
//   - main-frame navigations away from the origin and every new window are refused;
//   - it never clicks, types, solves, rewrites requests or touches the challenge's scripts or frames.
//   - the game UI (in_ui, the default): the page is only the widget on a transparent page (no card: the
//     game's modal has the heading, the text and Cancel). The view never leaves its hidden host: the
//     caller takes its frames (kn_webview_captcha_frame) and forwards its pointer and keys. GTK: the frames
//     are the offscreen host's cairo surface, the input GDK events to the view. WebView2: the frames are
//     ICoreWebView2::CapturePreview's PNG (decoded with WIC), taken about every 60 ms while shown; the input
//     goes to the view's render window as window messages (the controller's own input API needs a
//     composition host). When the game UI draws no frame within INLINE_STALL_MS, the challenge's window is
//     shown instead (the fallback; its background is dark, the widget centred). KRUNKNATIVE_CAPTCHA_WINDOW=1
//     keeps the window path only.
// WebView2 (Windows): WebView2.h (the C interface, no C++; pinned by tools/webview2/webview2.lock) and
// WebView2Loader.dll, loaded at run time. No loader, or no runtime (GetAvailableCoreWebView2BrowserVersionString
// fails), makes the backend unavailable: the callers end the request as they do with no webview.
#include <kn_webview.h>
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------- pure (every build: client/tests/kn_webview_test.c)
// The game UI's stall watchdog: the game UI shows a frame within this, or the challenge's window.
#define INLINE_STALL_MS 5000

int kn_webview_reveal_target(const int in_ui, const int inline_failed) {
    return in_ui && !inline_failed ? KN_WEBVIEW_VIEW_INLINE : KN_WEBVIEW_VIEW_WINDOW;
}

int kn_webview_inline_stalled(const int frames, const long long elapsed_ms, const long long limit_ms) {
    return frames <= 0 && elapsed_ms >= limit_ms;
}

void kn_webview_rgba_from_argb32(const unsigned char *src, const int stride, const int w, const int h, unsigned char *dst) {
    for (int y = 0; y < h; y++) {
        const unsigned char *row = src + (size_t) y * (size_t) stride;
        unsigned char *out = dst + (size_t) y * (size_t) w * 4;
        for (int x = 0; x < w; x++) {
            uint32_t v; // cairo's ARGB32: one native 32-bit pixel, premultiplied by its alpha
            memcpy(&v, row + (size_t) x * 4, 4);
            const unsigned a = v >> 24;
            const unsigned c[3] = {(v >> 16) & 255, (v >> 8) & 255, v & 255};
            for (int k = 0; k < 3; k++) {
                unsigned straight = a ? (c[k] * 255 + a / 2) / a : 0;
                if (straight > 255) straight = 255;
                out[x * 4 + k] = (unsigned char) straight;
            }
            out[x * 4 + 3] = (unsigned char) a;
        }
    }
}

// Which backend this build has (the one place the platform is named; the rest is the same code).
#if defined(KN_WEBVIEW) && defined(__linux__) && !defined(__EMSCRIPTEN__)
#define KN_WV_GTK 1
#include <gtk/gtk.h>
#include <unistd.h>
#include <webkit2/webkit2.h>
#elif defined(KN_WEBVIEW) && defined(_WIN32) && !defined(__EMSCRIPTEN__)
#define KN_WV_WV2 1
#include <wchar.h>
#include <windows.h>
#include <objbase.h>
#include <wincodec.h>
#include <shellapi.h>
#include <WebView2.h> // (its EventToken.h: client/src/ui/kn_webview_compat, this library's include dir)
#elif defined(KN_WEBVIEW) && defined(__APPLE__) && !defined(__EMSCRIPTEN__)
// WKWebView (macOS): this file is compiled as Objective-C on Apple (CMake: LANGUAGE OBJC, -fobjc-arc).
#if !defined(__OBJC__) || !__has_feature(objc_arc)
#error "kn_webview.c on macOS is compiled as Objective-C with ARC (CMake sets LANGUAGE OBJC and -fobjc-arc)"
#endif
#define KN_WV_WK 1
#include <time.h>
#import <Cocoa/Cocoa.h>
#import <Network/Network.h>
#import <WebKit/WebKit.h>
@class KNCaptchaWebDelegate;
// The navigation, UI and script-message delegate of the view (one per challenge: W.session tells a late callback).
@interface KNCaptchaWebDelegate : NSObject <WKNavigationDelegate, WKUIDelegate, WKScriptMessageHandler, NSWindowDelegate> {
@public
    int session; // W.session when this delegate was made
}
@end
#endif

#if defined(KN_WV_GTK) || defined(KN_WV_WV2) || defined(KN_WV_WK)
// ---------------------------------------------------------------- shared by the backends
#define KN_WEBVIEW_TOKEN_MAX 4096 // KRUNKER_CAPTCHA_TOKEN_MAX
#define TURNSTILE_API "https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit"
#define TURNSTILE_ORIGIN "https://challenges.cloudflare.com"
#define BRIDGE_WORLD "knCaptcha"      // GTK: the isolated script world holding the message handler
#define BRIDGE_ATTR "data-kn-captcha" // GTK: on <html>: the message, read and removed at once by the bridge
#define BRIDGE_EVENT "kn-captcha"
#define WINDOW_W 520
#define WINDOW_H 600

// A growing string: the scripts are built here (no GLib: the same code serves both backends).
typedef struct {
    char *s;
    size_t n, cap;
    int bad; // an allocation failed: sb_take() gives NULL
} Sb;

static void sb_add(Sb *b, const char *s, const size_t n) {
    if (b->bad) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->n + n + 1) cap *= 2;
        char *p = realloc(b->s, cap);
        if (!p) {
            b->bad = 1;
            return;
        }
        b->s = p;
        b->cap = cap;
    }
    memcpy(b->s + b->n, s, n);
    b->n += n;
    b->s[b->n] = 0;
}

static void sb_put(Sb *b, const char *s) { sb_add(b, s, strlen(s)); }

static void sb_putc(Sb *b, const char c) { sb_add(b, &c, 1); }

static void sb_printf(Sb *b, const char *fmt, ...) {
    char tmp[64];
    va_list ap;
    va_start(ap, fmt);
    const int n = vsnprintf(tmp, sizeof(tmp), fmt, ap);
    va_end(ap);
    if (n < 0) {
        b->bad = 1;
        return;
    }
    if ((size_t) n < sizeof(tmp)) {
        sb_add(b, tmp, (size_t) n);
        return;
    }
    char *big = malloc((size_t) n + 1);
    if (!big) {
        b->bad = 1;
        return;
    }
    va_start(ap, fmt);
    vsnprintf(big, (size_t) n + 1, fmt, ap);
    va_end(ap);
    sb_add(b, big, (size_t) n);
    free(big);
}

// The text (the caller frees it), NULL when an allocation failed.
static char *sb_take(Sb *b) {
    if (b->bad) {
        free(b->s);
        return NULL;
    }
    return b->s ? b->s : strdup("");
}

static int ascii_alnum(const char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }

static char ascii_lower(const char c) { return c >= 'A' && c <= 'Z' ? (char) (c + 32) : c; }

// s starts with prefix (a lower-case literal), ignoring the case of s.
static int ascii_prefix_ci(const char *s, const char *prefix) {
    for (; *prefix; prefix++, s++) {
        if (!*s || ascii_lower(*s) != *prefix) return 0;
    }
    return 1;
}

static int ascii_equal_ci(const char *a, const char *b) {
    for (; *a && *b; a++, b++) {
        if (ascii_lower(*a) != ascii_lower(*b)) return 0;
    }
    return *a == *b;
}

static struct {
    int toolkit;          // GTK: 0 not tried, 1 up, -1 failed (WebView2 keeps its own state: wv2_loader)
    char toolkit_err[160];
#if defined(KN_WV_GTK)
    GtkWidget *offscreen; // the hidden host of the view (GtkOffscreenWindow) until reveal(), else NULL
    GtkWidget *window;    // the real window, once revealed
    WebKitWebView *view;
#elif defined(KN_WV_WV2)
    HWND host;            // the hidden host of the controller until reveal(), else NULL
    HWND window;          // the real window, once revealed
    HWND target;          // the view's render window (the input goes there), found on first use
    ICoreWebView2Environment *env;
    ICoreWebView2Controller *controller;
    ICoreWebView2 *view;
    wchar_t *script;      // the widget's script (wide), added once the view exists
    wchar_t *navigate;    // the challenge's URL (wide), navigated to once the view exists
    int capturing;        // a CapturePreview is in flight
    wchar_t session_dir[MAX_PATH]; // the temporary user data folder of this session
#elif defined(KN_WV_WK)
    WKWebView *view;      // the challenge's view, nil when no challenge is up
    WKUserContentController *ucc;
    WKContentWorld *world;
    KNCaptchaWebDelegate *delegate;
    NSWindow *host;       // the hidden host of the view until reveal(), else nil
    NSWindow *window;     // the real window, once revealed
    int session;          // bumped at every teardown: a late callback of an older challenge is ignored
    unsigned main_status; // the main frame's last HTTP status (its navigation response)
    int snap_inflight;    // a snapshot of the view is in flight
    unsigned snap_gen;    // the latest snapshot's number (an older one's completion is dropped)
    long long snap_started_ms, snap_last_ms;
#endif
    char title[200];      // the window's title, for reveal()
    long long opened_ms;  // now_ms() of the open: KN_WEBVIEW_REVEAL_MS counts from it
    long long shown_ms;   // now_ms() when it was shown in the game UI (the watchdog)
    long long capture_ms; // now_ms() of the last frame request (WebView2)
    int reveal_ms;
    int closing;        // we are destroying the window (not the player)
    int taken;          // this window gave its token
    unsigned http_err;  // the page's HTTP status when >= 400 (its note stays in the modal)
    char origin[200];   // scheme://host[:port] of the page
    // pending events, polled in this order
    int ev_token, ev_failed, ev_closed, ev_status;
    const char *reveal; // a reason to show the window (static text), done by the next poll
    char token[KN_WEBVIEW_TOKEN_MAX + 1];
    char msg[300];
    // the game UI (in_ui): the view stays in its hidden host and the caller draws it
    int in_ui;          // opened for the game UI
    int inline_shown;   // shown in the game UI (no window)
    int inline_failed;  // the game UI could not draw it: its window instead (the stall watchdog)
    int dirty;          // a new frame is in px since the last one taken
    int frames;         // frames taken while shown in the game UI
    unsigned buttons;   // the pointer buttons the forwarded pointer holds down (GTK: GDK masks; WebView2 and WKWebView: 1 left, 2 middle, 4 right)
    unsigned char *px;  // the last frame: straight RGBA8, row 0 on top
    size_t px_cap;
    int px_w, px_h;
} W;

// The platform's clock (ms), the hidden host and the reveal are the backend's.
static long long now_ms(void);
static int hidden_host(void);
static void reveal(const char *why);
static void backend_tick(void);

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

static void set_msg(const char *text) { snprintf(W.msg, sizeof(W.msg), "%s", text ? text : ""); }

// Where the player solves it, for the lines the modal shows: in the game UI, or the challenge's window.
static const char *place(void) { return W.in_ui && !W.inline_failed ? "below" : "in the challenge window"; }

// scheme://host[:port] of an absolute http(s) URL, lower-cased; 0 when it is not one.
static int origin_of(const char *url, char *out, const size_t n) {
    if (!url) return 0;
    size_t skip;
    if (ascii_prefix_ci(url, "https://")) skip = 8;
    else if (ascii_prefix_ci(url, "http://")) skip = 7;
    else return 0;
    size_t end = skip;
    while (url[end] && url[end] != '/' && url[end] != '?' && url[end] != '#') end++;
    if (end == skip || end >= n) return 0;
    for (size_t i = 0; i < end; i++) {
        const char c = url[i];
        if (!(ascii_alnum(c) || c == '.' || c == '-' || c == ':' || c == '/' || c == '[' || c == ']')) return 0;
        out[i] = ascii_lower(c);
    }
    out[end] = 0;
    return !strchr(out + skip, '@'); // (the character test above already refuses userinfo)
}

static int is_loopback_origin(const char *origin) {
    const char *h = strstr(origin, "://");
    if (!h) return 0;
    h += 3;
    return !strncmp(h, "127.0.0.1", 9) || !strncmp(h, "localhost", 9) || !strncmp(h, "[::1]", 5);
}

static int same_origin(const char *url) {
    char o[sizeof(W.origin)];
    return W.origin[0] && origin_of(url, o, sizeof(o)) && !strcmp(o, W.origin);
}

// A JS string literal (double quotes) of s.
static void js_string(Sb *out, const char *s) {
    sb_putc(out, '"');
    for (const unsigned char *p = (const unsigned char *) s; p && *p; p++) {
        if (*p == '"' || *p == '\\') sb_printf(out, "\\%c", *p);
        else if (*p < 0x20 || *p == '<' || *p == '>') sb_printf(out, "\\u%04x", *p);
        else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0xa8 || p[2] == 0xa9)) { // U+2028 / U+2029
            sb_printf(out, "\\u202%c", p[2] == 0xa8 ? '8' : '9');
            p += 2;
        } else sb_putc(out, (char) *p);
    }
    sb_putc(out, '"');
}

#if defined(KN_WV_GTK) || defined(KN_WV_WK)
// The bridge, in the isolated world BRIDGE_WORLD of the top frame: the only script that can post to
// the handler. It reads the main-world script's message off <html> (the DOM is shared between worlds).
// The caller frees the text (NULL when out of memory).
static char *bridge_script(void) {
    Sb s = {0};
    sb_put(&s, "(function () {\n'use strict';\nvar ORIGIN = ");
    js_string(&s, W.origin);
    sb_put(&s, ";\n"
               "if (window.top !== window || location.origin !== ORIGIN) return;\n"
               "var h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.knCaptcha;\n"
               "if (!h) return;\n"
               "document.addEventListener('" BRIDGE_EVENT "', function () {\n"
               "  var d = document.documentElement, m = d.getAttribute('" BRIDGE_ATTR "');\n"
               "  d.removeAttribute('" BRIDGE_ATTR "');\n"
               "  if (typeof m === 'string' && m.length <= 8192) h.postMessage(m);\n"
               "}, true);\n"
               "})();\n");
    return sb_take(&s);
}
#endif

// How the widget reports to the host: GTK's bridge (an isolated world, see the top of this file) or
// WebView2's postMessage (the host checks the source's origin there).
#if defined(KN_WV_WV2)
#define POST_JS "var post = function (m) { window.chrome.webview.postMessage(m); };\n"
// The HTTP status of the page: WebView2 does not report it to the host, so the script does, once the page
// is parsed ("http:" + status; the host shows the note for 400 and up, as GTK does).
#define HTTP_JS                                                                                                              \
    "if (window.top === window && location.origin === ORIGIN) { try { var nav = performance.getEntriesByType('navigation')[0];" \
    " if (nav && nav.responseStatus >= 400) post('http:' + nav.responseStatus); } catch (e) {} }\n"
#else
#define POST_JS                                                                                                              \
    "var post = function (m) { var d = document.documentElement; d.setAttribute('" BRIDGE_ATTR "', m);"                      \
    " document.dispatchEvent(new Event('" BRIDGE_EVENT "')); d.removeAttribute('" BRIDGE_ATTR "'); };\n"
#define HTTP_JS ""
#endif

// The user script: the widget on the krunker.io page. In a window (in_ui 0): a card over a dark page
// with a heading, a hint and the widget. In the game UI (in_ui 1): only the widget, centred on the
// transparent page (the page's own text hidden): the game's modal has the heading, the text and Cancel.
// The caller frees the text (NULL when out of memory).
static char *user_script(const char *sitekey, const char *title, const int in_ui) {
    Sb s = {0};
    sb_put(&s, "(function () {\n'use strict';\nvar ORIGIN = ");
    js_string(&s, W.origin);
    sb_put(&s, ", SITEKEY = ");
    js_string(&s, sitekey);
    sb_put(&s, ", TITLE = ");
    js_string(&s, title);
    sb_put(&s, ", API = ");
    js_string(&s, TURNSTILE_API);
    sb_put(&s,
           ";\n"
           "if (window.top !== window || location.origin !== ORIGIN || window.__knCaptcha) return;\n"
           "window.__knCaptcha = 1;\n" POST_JS HTTP_JS
           "var el = function (tag, css, text) { var e = document.createElement(tag); e.style.cssText = css;"
           " if (text) e.textContent = text; return e; };\n"
           "var shade, holder, status, retry;\n");
    if (in_ui) {
        sb_put(&s,
               "shade = el('div', 'position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);z-index:2147483646;width:340px;"
               "display:flex;flex-direction:column;align-items:center;margin:0;background:transparent;font:15px/1.4 sans-serif;"
               "color:#555');\n"
               "holder = el('div', 'height:65px;width:100%;display:flex;align-items:center;justify-content:center');\n"
               "status = el('div', 'color:#555;font-size:14px;min-height:18px;margin-top:6px');\n"
               "retry = el('button', 'display:none;margin:8px auto 0;padding:6px 16px;font-size:14px;cursor:pointer', 'Try again');\n"
               "shade.appendChild(holder); shade.appendChild(status); shade.appendChild(retry);\n");
    } else {
        sb_put(&s,
               "shade = el('div', 'position:fixed;top:0;left:0;right:0;bottom:0;z-index:2147483646;background:#1b1b1b;"
               "display:flex;align-items:center;justify-content:center;font:15px/1.4 sans-serif;margin:0');\n"
               "var card = el('div', 'background:#fff;color:#222;padding:22px 24px;border-radius:6px;width:400px;max-width:90vw;"
               "text-align:center;box-shadow:0 4px 24px rgba(0,0,0,.5)');\n"
               "var head = el('div', 'font-size:22px;font-weight:bold;text-transform:uppercase;margin-bottom:6px', TITLE);\n"
               "var hint = el('div', 'color:#555;font-size:14px', 'Solve Cloudflare\\u2019s check for krunker.io below. Krunker Box Office"
               " takes the result once and closes this window.');\n"
               "holder = el('div', 'min-height:70px;display:flex;align-items:center;justify-content:center;margin:14px 0 6px');\n"
               "status = el('div', 'color:#555;font-size:13px;min-height:18px');\n"
               "retry = el('button', 'display:none;margin:10px auto 0;padding:6px 16px;font-size:14px;cursor:pointer', 'Try again');\n"
               "var where = el('div', 'color:#999;font-size:12px;margin-top:10px', location.origin);\n"
               "card.appendChild(head); card.appendChild(hint); card.appendChild(holder); card.appendChild(status);"
               " card.appendChild(retry); card.appendChild(where); shade.appendChild(card);\n");
    }
    sb_put(&s, "if (window._cf_chl_opt) { post('interstitial:'); return; }\n"); // Cloudflare's own check of the visit: never covered
    if (in_ui) { // Cloudflare's own page (above) stays as it is; the robots.txt text does not show
        sb_put(&s,
               "if (document.body) { document.body.style.display = 'none'; document.body.style.background = 'transparent'; }\n"
               "document.documentElement.style.background = 'transparent';\n"
               "document.documentElement.appendChild(shade);\n");
    } else {
        sb_put(&s, "(document.body || document.documentElement).appendChild(shade);\n");
    }
    sb_put(&s,
           "var say = function (t) { status.textContent = t || ''; };\n"
           "var widget = null, loading = null;\n"
           "var load = function () {\n"
           "  if (window.turnstile) return Promise.resolve();\n"
           "  if (!loading) loading = new Promise(function (ok, fail) {\n"
           "    var sc = document.createElement('script'); sc.src = API; sc.async = true;\n"
           "    sc.onload = function () { ok(); }; sc.onerror = function () { loading = null; fail(new Error('load')); };\n"
           "    (document.head || document.documentElement).appendChild(sc);\n"
           "  });\n"
           "  return loading;\n"
           "};\n"
           "var failed = function (t, m) { say(t); retry.style.display = ''; post(m); };\n"
           "var render = function () {\n"
           "  retry.style.display = 'none';\n"
           "  say('Loading Cloudflare\\u2019s challenge\\u2026');\n"
           "  load().then(function () {\n"
           "    var ts = window.turnstile;\n"
           "    if (!ts) { failed('Cloudflare\\u2019s challenge did not load.', 'error:noscript'); return; }\n"
           "    try { if (widget != null) ts.remove(widget); } catch (e) {}\n"
           "    widget = null;\n"
           "    try {\n"
           "      widget = ts.render(holder, {\n"
           "        sitekey: SITEKEY, appearance: 'always',\n"
           "        callback: function (t) { say('Done.'); post('token:' + t); },\n"
           "        'error-callback': function (c) { c = c == null ? 'unknown' : String(c);"
           " failed('Cloudflare error ' + c + '.', 'error:' + c); return true; },\n"
           "        'expired-callback': function () { failed('The challenge expired.', 'expired:'); },\n"
           "        'timeout-callback': function () { failed('The challenge timed out.', 'timeout:'); },\n"
           "        'before-interactive-callback': function () { say(''); post('interactive:'); }\n"
           "      });\n"
           "      if (widget == null) failed('Cloudflare\\u2019s challenge could not be shown.', 'error:render');\n"
           "      else { say(''); post('shown:'); }\n"
           "    } catch (e) { failed('Cloudflare\\u2019s challenge could not be shown.', 'error:render'); }\n"
           "  }, function () { failed('Cloudflare\\u2019s challenge did not load (blocked or offline).', 'error:noscript'); });\n"
           "};\n"
           "retry.onclick = render;\n"
           "render();\n"
           "})();\n");
    return sb_take(&s);
}

static int nav_allowed(const char *uri) {
    if (!uri) return 0;
    if (!strncmp(uri, "about:", 6) || !strncmp(uri, "data:", 5) || !strncmp(uri, "blob:", 5)) return 1;
    char o[sizeof(W.origin)];
    if (!origin_of(uri, o, sizeof(o))) return 0;
    return !strcmp(o, W.origin) || !strcmp(o, TURNSTILE_ORIGIN);
}

// A navigation the page asks for: 1 = allowed (else it is refused and said here).
static int navigation_ok(const char *uri) {
    if (nav_allowed(uri)) return 1;
    char o[sizeof(W.origin)];
    printf("[webview] a navigation to %s was refused\n", origin_of(uri, o, sizeof(o)) ? o : "(another scheme)");
    return 0;
}

// krunker.io answers some networks with an error page ("ACCESS DENIED", 403): said in the modal (the
// widget still works on it, the page being krunker.io's; the matchmaker decides).
static void http_status(const unsigned status) {
    if (status < 400) return;
    W.http_err = status;
    printf("[webview] %s answered HTTP %u\n", W.origin, status);
    char text[300];
    snprintf(text, sizeof(text), "krunker.io answered HTTP %u: it blocks some networks"
                                 " (Settings > Client > Network > Proxy).",
             status);
    set_msg(text);
    W.ev_status = 1;
}

// The widget's messages ("kind:value", from the user script). on_origin: the message came from the
// expected origin's page (else it is ignored). One token at most; the states the modal shows.
static void widget_message(const char *s, const int on_origin) {
    if (!on_origin) {
        printf("[webview] a message while the page is not on %s: ignored\n", W.origin);
        return;
    }
    const size_t len = strlen(s);
    if (!strncmp(s, "token:", 6)) {
        const size_t n = len - 6;
        if (W.taken) {
            printf("[webview] a second token: ignored (one per window)\n");
        } else if (n == 0 || n > KN_WEBVIEW_TOKEN_MAX) {
            printf("[webview] the widget answered something that is not a token (%zu chars)\n", n);
            W.ev_status = 1;
            set_msg("Cloudflare answered something that is not a token.");
        } else {
            memcpy(W.token, s + 6, n + 1);
            W.taken = 1;
            W.ev_token = 1;
            printf("[webview] the challenge was solved: a token (%zu chars)\n", n);
        }
    } else if (!strncmp(s, "error:", 6)) {
        char code[17];
        snprintf(code, sizeof(code), "%s", s + 6);
        for (char *c = code; *c; c++)
            if (!ascii_alnum(*c)) *c = '_';
        printf("[webview] the widget reported error %s\n", code);
        char text[200];
        if (!strncmp(code, "1101", 4) || !strncmp(code, "1102", 4)) {
            snprintf(text, sizeof(text), "Cloudflare refused krunker.io's challenge on %s (error %s).", W.origin, code);
            W.ev_failed = 1;
        } else if (!strcmp(code, "noscript")) {
            snprintf(text, sizeof(text), "Cloudflare's challenge did not load (blocked or offline).");
        } else {
            snprintf(text, sizeof(text), "The challenge failed (Cloudflare error %s): \"Try again\" %s.", code, place());
        }
        set_msg(text);
        if (!W.ev_failed) { // a failure the player can retry: its "Try again" is in the window
            W.ev_status = 1;
            W.reveal = "a failure the player can retry";
        }
    } else if (!strncmp(s, "expired:", 8) || !strncmp(s, "timeout:", 8)) {
        printf("[webview] the challenge %s\n", s[0] == 'e' ? "expired" : "timed out");
        W.ev_status = 1;
        W.reveal = s[0] == 'e' ? "the challenge expired" : "the challenge timed out";
        char m[200];
        if (s[0] == 'e') snprintf(m, sizeof(m), "The challenge expired: \"Try again\" %s.", place());
        else snprintf(m, sizeof(m), "The challenge timed out: \"Try again\" %s.", place());
        set_msg(m);
    } else if (!strncmp(s, "shown:", 6)) {
        printf("[webview] Cloudflare's widget is up%s\n", W.window ? "" : " (hidden: shown only if it needs the player)");
        if (!W.http_err) {
            W.ev_status = 1;
            set_msg(W.window ? "Cloudflare's widget is up in the challenge window." : "Cloudflare is checking this computer\xe2\x80\xa6");
        }
    } else if (!strncmp(s, "interstitial:", 13)) {
        // Cloudflare's own page check: it may pass by itself, so it is not a reason to show the window
        // (the no-token backstop still is). Its line is kept for the window's reveal.
        printf("[webview] Cloudflare checks the visit to %s first (hidden: waiting for it)\n", W.origin);
        char m[200];
        snprintf(m, sizeof(m), "Cloudflare checks your visit to krunker.io first: follow it %s.", place());
        set_msg(m);
    } else if (!strncmp(s, "interactive:", 12)) {
        printf("[webview] Turnstile needs the player\n");
        W.reveal = "Turnstile needs the player";
        W.ev_status = 1;
        char m[200];
        snprintf(m, sizeof(m), "Cloudflare wants you to tick the box %s.", place());
        set_msg(m);
    } else if (!strncmp(s, "http:", 5)) {
        http_status((unsigned) strtoul(s + 5, NULL, 10));
    }
}

// The page did not load (its own origin): the modal says so; the request ends.
static void load_failed(const char *why) {
    printf("[webview] %s did not load: %s\n", W.origin, why);
    char text[300];
    snprintf(text, sizeof(text), "%s did not load (%s).", W.origin, why);
    set_msg(text);
    W.ev_failed = 1;
}

// A certificate of krunker.io was refused: never bypassed (the toolkit's error stays, the request ends).
static void tls_refused(const char *detail) {
    printf("[webview] %s: the TLS certificate was refused (%s)\n", W.origin, detail);
    set_msg("krunker.io's certificate was not valid for the challenge.");
    W.ev_failed = 1;
}

// The web process of the view ended: the request ends.
static void page_process_stopped(const char *detail) {
    printf("[webview] the web process ended (%s)\n", detail);
    set_msg("The challenge's web process stopped.");
    W.ev_failed = 1;
}

static int env_off(void) {
    const char *e = getenv("KRUNKNATIVE_WEBVIEW");
    return e && (!strcmp(e, "0") || ascii_equal_ci(e, "off"));
}

// KRUNKNATIVE_CAPTCHA_WINDOW=1: the challenge's own window, never the game UI (a test, or a player's choice).
static int window_forced(void) {
    const char *e = getenv("KRUNKNATIVE_CAPTCHA_WINDOW");
    return e && !strcmp(e, "1");
}

// The challenge's URL (NULL: the env override, else KN_WEBVIEW_CAPTCHA_URL) and sitekey are checked here;
// origin receives the URL's origin, *resolved the URL. 0 = refused (err says why).
static int captcha_check(const char *url, const char *sitekey, char *origin, const size_t n, const char **resolved, char *err,
                         const size_t errn) {
    if (!url || !url[0]) {
        const char *e = getenv("KRUNKNATIVE_KRUNKER_CAPTCHA_URL");
        url = e && e[0] ? e : KN_WEBVIEW_CAPTCHA_URL;
    }
    if (!origin_of(url, origin, n) || (strncmp(origin, "https://", 8) && !is_loopback_origin(origin))) {
        if (err && errn) snprintf(err, errn, "the challenge URL must be https:// (or http:// on this computer)");
        return 0;
    }
    for (const char *k = sitekey; k && *k; k++)
        if (!ascii_alnum(*k) && *k != '_' && *k != '-') sitekey = NULL;
    if (!sitekey || !sitekey[0]) {
        if (err && errn) snprintf(err, errn, "no Turnstile sitekey");
        return 0;
    }
    *resolved = url;
    return 1;
}

// A new challenge session: the state the open starts from (the backend's objects are its own).
static void session_reset(const int in_ui, const char *origin, const char *title) {
    snprintf(W.origin, sizeof(W.origin), "%s", origin);
    snprintf(W.title, sizeof(W.title), "%s - krunker.io", title && title[0] ? title : "Security Challenge");
    W.in_ui = in_ui && !window_forced();
    W.inline_shown = W.inline_failed = W.dirty = W.frames = 0;
    W.buttons = 0;
    W.taken = W.closing = 0;
    W.http_err = 0;
    W.ev_token = W.ev_failed = W.ev_closed = W.ev_status = 0;
    W.reveal = NULL;
    wipe(W.token, sizeof(W.token));
    const char *rv = getenv("KRUNKNATIVE_WEBVIEW_REVEAL_MS");
    W.reveal_ms = rv && rv[0] ? atoi(rv) : KN_WEBVIEW_REVEAL_DEFAULT_MS;
    W.opened_ms = now_ms();
}

// The common end of a close: the game UI's frames and the token wiped (the backend's objects are gone).
static void session_end(void) {
    W.inline_shown = W.inline_failed = W.dirty = W.frames = 0;
    W.buttons = 0;
    free(W.px);
    W.px = NULL;
    W.px_cap = 0;
    W.px_w = W.px_h = 0;
    if (!W.ev_token) wipe(W.token, sizeof(W.token));
}

// The poll's reveal decisions, the same on both backends: the game UI's stall watchdog, then the reasons
// to show the challenge (the widget's, the player's, the backstop). 1 = the challenge was shown by this poll.
static int poll_reveal(void) {
    if (W.inline_shown && kn_webview_inline_stalled(W.frames, now_ms() - W.shown_ms, INLINE_STALL_MS)) {
        printf("[webview] the game UI drew no frame in %d s: the challenge window instead\n", INLINE_STALL_MS / 1000);
        W.inline_shown = 0;
        W.inline_failed = 1;
        set_msg("The challenge is in its own window: the game UI could not draw it.");
        W.reveal = "the game UI drew no frame";
    }
    if (!hidden_host() || W.ev_token || W.ev_failed) return 0;
    if (!W.reveal && !W.inline_shown && W.reveal_ms >= 0 && now_ms() - W.opened_ms >= W.reveal_ms) W.reveal = "no token in time";
    if (!W.reveal) return 0;
    const int before = W.inline_shown || W.window != NULL;
    const char *why = W.reveal;
    W.reveal = NULL; // (a reveal that cannot happen yet sets it again: reveal() keeps the reason)
    reveal(why);
    return !before && (W.inline_shown || W.window != NULL);
}

// The events a poll reports, one per call, in order: the token (the challenge closes), the failure, the
// window's closing, the widget's status line.
static int poll_events(char *token, const size_t n, char *err, const size_t errn) {
    if (W.ev_token) {
        W.ev_token = 0;
        const size_t len = strlen(W.token);
        const int ok = token && len < n;
        if (ok) memcpy(token, W.token, len + 1);
        wipe(W.token, sizeof(W.token));
        kn_webview_captcha_close(); // done: the window goes at once
        if (ok) return KN_WEBVIEW_TOKEN;
        if (err && errn) snprintf(err, errn, "the token did not fit");
        return KN_WEBVIEW_FAILED;
    }
    if (W.ev_failed) {
        W.ev_failed = W.ev_status = 0;
        kn_webview_captcha_close();
        if (err && errn) snprintf(err, errn, "%s", W.msg);
        return KN_WEBVIEW_FAILED;
    }
    if (W.ev_closed) {
        W.ev_closed = W.ev_status = 0;
        return KN_WEBVIEW_CLOSED;
    }
    if (W.ev_status) {
        W.ev_status = 0;
        if (err && errn) snprintf(err, errn, "%s", W.msg);
        return KN_WEBVIEW_STATUS;
    }
    return KN_WEBVIEW_NONE;
}

int kn_webview_captcha_poll(char *token, const size_t n, char *err, const size_t errn) {
    kn_webview_pump();
    if (err && errn) err[0] = 0;
    backend_tick();
    if (poll_reveal()) { // the player is needed now: the caller shows its prompt (the widget's line rides along)
        if (err && errn) snprintf(err, errn, "%s", W.msg);
        return KN_WEBVIEW_SHOWN;
    }
    return poll_events(token, n, err, errn);
}

int kn_webview_captcha_is_open(void) { return W.window != NULL || hidden_host(); }

int kn_webview_captcha_inline(void) { return W.inline_shown; }

int kn_webview_captcha_windowed(void) { return W.window != NULL; }

#endif // KN_WV_GTK || KN_WV_WV2 || KN_WV_WK

// ---------------------------------------------------------------- GTK (Linux)
#if defined(KN_WV_GTK)

static long long now_ms(void) { return (long long) (g_get_monotonic_time() / 1000); }

static int hidden_host(void) { return W.offscreen != NULL; }

static void backend_tick(void) {}

// ---------------------------------------------------------------- signals

static void on_message(WebKitUserContentManager *m, WebKitJavascriptResult *r, gpointer user) {
    (void) m;
    (void) user;
    if (!W.view) return;
    JSCValue *v = webkit_javascript_result_get_js_value(r);
    if (!v || !jsc_value_is_string(v)) return;
    char *s = jsc_value_to_string(v);
    if (!s) return;
    widget_message(s, same_origin(webkit_web_view_get_uri(W.view)));
    wipe(s, strlen(s));
    g_free(s);
    // (the window is closed by the next kn_webview_captcha_poll, outside this signal)
}

static gboolean on_policy(WebKitWebView *view, WebKitPolicyDecision *d, const WebKitPolicyDecisionType type, gpointer user) {
    (void) view;
    (void) user;
    if (type == WEBKIT_POLICY_DECISION_TYPE_NEW_WINDOW_ACTION) { // Cloudflare's "Privacy" / "Help" links
        printf("[webview] a new window was refused\n");
        webkit_policy_decision_ignore(d);
        return TRUE;
    }
    if (type == WEBKIT_POLICY_DECISION_TYPE_NAVIGATION_ACTION) {
        WebKitNavigationAction *a = webkit_navigation_policy_decision_get_navigation_action(WEBKIT_NAVIGATION_POLICY_DECISION(d));
        WebKitURIRequest *req = a ? webkit_navigation_action_get_request(a) : NULL;
        const char *uri = req ? webkit_uri_request_get_uri(req) : NULL;
        if (!navigation_ok(uri)) {
            webkit_policy_decision_ignore(d);
            return TRUE;
        }
    }
    return FALSE; // WebKit's default
}

static gboolean on_load_failed(WebKitWebView *view, WebKitLoadEvent ev, gchar *uri, GError *error, gpointer user) {
    (void) view;
    (void) ev;
    (void) user;
    if (error && ((error->domain == WEBKIT_NETWORK_ERROR && error->code == WEBKIT_NETWORK_ERROR_CANCELLED) ||
                  error->domain == WEBKIT_POLICY_ERROR))
        return FALSE; // a navigation we refused / replaced
    if (!same_origin(uri)) return FALSE;
    load_failed(error ? error->message : "error");
    return FALSE;
}

static void on_load_changed(WebKitWebView *view, const WebKitLoadEvent ev, gpointer user) {
    (void) user;
    if (ev != WEBKIT_LOAD_FINISHED || !same_origin(webkit_web_view_get_uri(view))) return;
    WebKitWebResource *res = webkit_web_view_get_main_resource(view);
    WebKitURIResponse *resp = res ? webkit_web_resource_get_response(res) : NULL;
    http_status(resp ? webkit_uri_response_get_status_code(resp) : 0);
}

static gboolean on_tls_error(WebKitWebView *view, gchar *uri, GTlsCertificate *cert, GTlsCertificateFlags errors, gpointer user) {
    (void) view;
    (void) uri;
    (void) cert;
    (void) user;
    char detail[32];
    snprintf(detail, sizeof(detail), "flags 0x%x", (unsigned) errors);
    tls_refused(detail);
    return FALSE; // never bypassed: WebKit shows its error
}

static void on_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer user) {
    (void) view;
    (void) user;
    char detail[32];
    snprintf(detail, sizeof(detail), "reason %d", (int) reason);
    page_process_stopped(detail);
}

// the real window (after reveal()); the hidden host is only ever destroyed by us
static void on_destroy(GtkWidget *w, gpointer user) {
    (void) w;
    (void) user;
    W.window = NULL;
    W.view = NULL;
    if (!W.closing) {
        printf("[webview] the player closed the challenge window\n");
        W.ev_closed = 1;
    }
}

// The offscreen host was painted: the game UI takes a new frame (kn_webview_captcha_frame).
static gboolean on_draw(GtkWidget *w, cairo_t *cr, gpointer user) {
    (void) w;
    (void) cr;
    (void) user;
    W.dirty = 1;
    return FALSE;
}

// The GDK device of a synthetic event: the display's default seat's pointer or keyboard (GTK warns about an
// event without one, and WebKit reads it).
static GdkDevice *seat_device(const int keyboard) {
    GdkDisplay *d = gdk_display_get_default();
    GdkSeat *seat = d ? gdk_display_get_default_seat(d) : NULL;
    if (!seat) return NULL;
    return keyboard ? gdk_seat_get_keyboard(seat) : gdk_seat_get_pointer(seat);
}

static guint32 event_time(void) { return (guint32) (g_get_monotonic_time() / 1000); }

// A synthetic event straight to the view's handlers (WebKit's button, motion and key handlers), the way
// GTK delivers one to a widget: the view's own window and coordinates. The event owns its window reference.
static void dispatch(GdkEvent *ev) {
    gtk_widget_event(GTK_WIDGET(W.view), ev);
    gdk_event_free(ev);
}

static guint key_keyval(const int key) {
    switch (key) {
        case KN_WEBVIEW_KEY_TAB: return GDK_KEY_Tab;
        case KN_WEBVIEW_KEY_ENTER: return GDK_KEY_Return;
        case KN_WEBVIEW_KEY_SPACE: return GDK_KEY_space;
        case KN_WEBVIEW_KEY_BACKSPACE: return GDK_KEY_BackSpace;
        case KN_WEBVIEW_KEY_DELETE: return GDK_KEY_Delete;
        case KN_WEBVIEW_KEY_LEFT: return GDK_KEY_Left;
        case KN_WEBVIEW_KEY_RIGHT: return GDK_KEY_Right;
        case KN_WEBVIEW_KEY_UP: return GDK_KEY_Up;
        case KN_WEBVIEW_KEY_DOWN: return GDK_KEY_Down;
        case KN_WEBVIEW_KEY_HOME: return GDK_KEY_Home;
        case KN_WEBVIEW_KEY_END: return GDK_KEY_End;
        default: return 0;
    }
}

// A key press or release on the view (text: the characters it types, press only).
static void send_key(const guint keyval, char *text, const int pressed) {
    GdkWindow *gw = W.offscreen ? gtk_widget_get_window(W.offscreen) : NULL;
    if (!gw) return;
    GdkEvent *ev = gdk_event_new(pressed ? GDK_KEY_PRESS : GDK_KEY_RELEASE);
    ev->key.window = g_object_ref(gw);
    ev->key.send_event = TRUE;
    ev->key.time = event_time();
    ev->key.keyval = keyval;
    ev->key.state = 0;
    ev->key.group = 0;
    GdkDevice *kbd = seat_device(1);
    if (kbd) gdk_event_set_device(ev, kbd);
    GdkKeymap *km = gdk_keymap_get_for_display(gdk_display_get_default());
    GdkKeymapKey *keys = NULL;
    gint n = 0;
    if (km && gdk_keymap_get_entries_for_keyval(km, keyval, &keys, &n) && n > 0) {
        ev->key.hardware_keycode = (guint16) keys[0].keycode;
        ev->key.group = (guint8) keys[0].group;
    }
    g_free(keys);
    if (text) {
        ev->key.string = text; // the caller's buffer: cleared again before the event is freed
        ev->key.length = (gint) strlen(text);
    }
    gtk_widget_event(GTK_WIDGET(W.view), ev);
    ev->key.string = NULL;
    ev->key.length = 0;
    gdk_event_free(ev);
}

// The game UI's view: the newest frame of the offscreen host when it was painted since the last one.
// The surface is mapped to an image (free for an image surface), read, and converted to straight RGBA.
int kn_webview_captcha_frame(const unsigned char **rgba, int *w, int *h) {
    if (!W.inline_shown || !W.dirty || !W.offscreen) return 0;
    cairo_surface_t *s = gtk_offscreen_window_get_surface(GTK_OFFSCREEN_WINDOW(W.offscreen));
    if (!s || cairo_surface_status(s) != CAIRO_STATUS_SUCCESS) return 0;
    cairo_surface_t *img = cairo_surface_map_to_image(s, NULL);
    if (!img || cairo_surface_status(img) != CAIRO_STATUS_SUCCESS) return 0;
    int ok = 0;
    if (cairo_image_surface_get_format(img) == CAIRO_FORMAT_ARGB32) {
        cairo_surface_flush(img);
        const int iw = cairo_image_surface_get_width(img), ih = cairo_image_surface_get_height(img);
        const unsigned char *data = cairo_image_surface_get_data(img);
        const int stride = cairo_image_surface_get_stride(img);
        const size_t need = (size_t) iw * (size_t) ih * 4;
        int room = 1;
        if (need > W.px_cap) {
            unsigned char *p = realloc(W.px, need);
            room = p != NULL;
            if (p) {
                W.px = p;
                W.px_cap = need;
            }
        }
        if (data && iw > 0 && ih > 0 && room) {
            kn_webview_rgba_from_argb32(data, stride, iw, ih, W.px);
            W.px_w = iw;
            W.px_h = ih;
            W.dirty = 0;
            W.frames++;
            *rgba = W.px;
            *w = iw;
            *h = ih;
            ok = 1;
        }
    }
    cairo_surface_unmap_image(s, img);
    return ok;
}

int kn_webview_captcha_pointer(const int kind, const double x, const double y, const int button) {
    if (!W.inline_shown || !W.offscreen) return 0;
    GdkWindow *gw = gtk_widget_get_window(W.offscreen);
    if (!gw) return 0;
    const unsigned mask = button == 1 ? GDK_BUTTON1_MASK : button == 2 ? GDK_BUTTON2_MASK : button == 3 ? GDK_BUTTON3_MASK : 0;
    GdkEvent *ev;
    if (kind == KN_WEBVIEW_POINTER_MOVE) {
        ev = gdk_event_new(GDK_MOTION_NOTIFY);
        ev->motion.state = W.buttons;
        ev->motion.x = ev->motion.x_root = x;
        ev->motion.y = ev->motion.y_root = y;
        ev->motion.time = event_time();
    } else {
        const int press = kind == KN_WEBVIEW_POINTER_PRESS;
        ev = gdk_event_new(press ? GDK_BUTTON_PRESS : GDK_BUTTON_RELEASE);
        ev->button.button = (guint) button;
        ev->button.state = W.buttons;
        ev->button.x = ev->button.x_root = x;
        ev->button.y = ev->button.y_root = y;
        ev->button.time = event_time();
        if (press) W.buttons |= mask;
        else W.buttons &= ~mask;
    }
    GdkWindow *target = gtk_widget_get_window(GTK_WIDGET(W.view)); // the view's own window (its offscreen host's when it has none)
    ev->any.window = g_object_ref(target ? target : gw);
    ev->any.send_event = TRUE;
    GdkDevice *dev = seat_device(0);
    if (dev) gdk_event_set_device(ev, dev);
    dispatch(ev);
    return 1;
}

int kn_webview_captcha_key(const int key, const int pressed) {
    if (!W.inline_shown || !W.offscreen) return 0;
    if (pressed == 2) return 1; // a repeat: taken, not sent (the widget takes the press)
    const guint kv = key_keyval(key);
    if (!kv) return 0;
    send_key(kv, NULL, pressed == 1);
    return 1;
}

int kn_webview_captcha_char(const unsigned codepoint) {
    if (!W.inline_shown || !W.offscreen || codepoint <= 0x20 || codepoint == 0x7f) return 0; // (space is a key, see key_keyval)
    char utf8[8] = {0};
    const int n = g_unichar_to_utf8((gunichar) codepoint, utf8);
    if (n <= 0 || n >= (int) sizeof(utf8)) return 0;
    utf8[n] = 0;
    const guint kv = gdk_unicode_to_keyval((guint32) codepoint);
    send_key(kv, utf8, 1); // the press types the text; the release carries none
    send_key(kv, NULL, 0);
    return 1;
}

// The work area (screen coordinates) of the monitor under the pointer (where the player looks: the
// game window), else the primary one, else the first.
static int work_area(GdkRectangle *wa) {
    GdkDisplay *display = gdk_display_get_default();
    if (!display) return 0;
    GdkMonitor *monitor = NULL;
    GdkSeat *seat = gdk_display_get_default_seat(display);
    GdkDevice *pointer = seat ? gdk_seat_get_pointer(seat) : NULL;
    if (pointer) {
        int px = 0, py = 0;
        gdk_device_get_position(pointer, NULL, &px, &py);
        monitor = gdk_display_get_monitor_at_point(display, px, py);
    }
    if (!monitor) monitor = gdk_display_get_primary_monitor(display);
    if (!monitor && gdk_display_get_n_monitors(display) > 0) monitor = gdk_display_get_monitor(display, 0);
    if (!monitor) return 0;
    gdk_monitor_get_workarea(monitor, wa);
    return wa->width > 0 && wa->height > 0;
}

// Shows the challenge: the view leaves its hidden host for a real window, created at its final
// place (centred on the work area, on top of the game) before it is mapped, so it never jumps.
// The game UI (in_ui, not failed) takes it with no window: the caller draws its frames (inline_shown).
// Otherwise the window, as before; with in_ui its page is the widget only, so the window's background
// is set dark (the widget centred on it, the game's modal is not there).
static void reveal(const char *why) {
    if (W.inline_shown || W.window || !W.view || !W.offscreen) return;
    if (kn_webview_reveal_target(W.in_ui, W.inline_failed) == KN_WEBVIEW_VIEW_INLINE) {
        W.inline_shown = 1;
        W.shown_ms = now_ms();
        W.frames = 0;
        W.dirty = 1; // (the first frame is taken at once)
        printf("[webview] challenge shown in the game UI (%s): no window\n", why);
        return;
    }
    GtkWidget *view = GTK_WIDGET(W.view);
    W.window = gtk_window_new(GTK_WINDOW_TOPLEVEL);
    gtk_window_set_title(GTK_WINDOW(W.window), W.title);
    int w = WINDOW_W, h = WINDOW_H, x = 0, y = 0;
    GdkRectangle wa = {0};
    const int placed = work_area(&wa);
    if (placed) {
        if (w > wa.width) w = wa.width;
        if (h > wa.height) h = wa.height;
        x = wa.x + (wa.width - w) / 2;
        y = wa.y + (wa.height - h) / 2;
    }
    gtk_window_set_default_size(GTK_WINDOW(W.window), w, h);
    if (placed) gtk_window_move(GTK_WINDOW(W.window), x, y); // before the map: the first position is the final one
    else gtk_window_set_position(GTK_WINDOW(W.window), GTK_WIN_POS_CENTER);
    gtk_window_set_keep_above(GTK_WINDOW(W.window), TRUE); // over a fullscreen game window
    g_signal_connect(W.window, "destroy", G_CALLBACK(on_destroy), NULL);
    g_object_ref(view);
    gtk_container_remove(GTK_CONTAINER(W.offscreen), view);
    gtk_widget_set_size_request(view, -1, -1);
    if (W.in_ui) { // the widget alone on the dark page (its transparent background shows this)
        GdkRGBA dark = {0.106, 0.106, 0.106, 1.0};
        webkit_web_view_set_background_color(W.view, &dark);
    }
    gtk_container_add(GTK_CONTAINER(W.window), view);
    g_object_unref(view);
    gtk_widget_destroy(W.offscreen);
    W.offscreen = NULL;
    gtk_widget_show_all(W.window);
    gtk_window_present(GTK_WINDOW(W.window));
    if (placed)
        printf("[webview] challenge window shown (%s) at %d,%d %dx%d, centred on the work area %d,%d %dx%d\n", why, x, y, w, h, wa.x, wa.y,
               wa.width, wa.height);
    else
        printf("[webview] challenge window shown (%s), centred by GTK\n", why);
}

// ---------------------------------------------------------------- API

int kn_webview_available(void) { return !env_off() && W.toolkit >= 0; }

// The packaged client's bundled TLS module (tools/release/bundle-webkit.sh: lib/gio/modules next to
// client_bin). WebKit's network process needs glib-networking's TLS backend for https (krunker.io's
// page). GIO_EXTRA_MODULES adds the bundle's directory to the host's own modules (its proxy
// modules stay); nothing is set when the directory is not there (a development build).
static void add_bundled_gio_modules(void) {
    char exe[4096];
    const ssize_t n = readlink("/proc/self/exe", exe, sizeof(exe) - 1);
    if (n <= 0) return;
    exe[n] = 0;
    char *slash = strrchr(exe, '/');
    if (!slash) return;
    *slash = 0; // the install root
    char dir[4200];
    snprintf(dir, sizeof(dir), "%s/lib/gio/modules", exe);
    if (!g_file_test(dir, G_FILE_TEST_IS_DIR)) return;
    const char *old = getenv("GIO_EXTRA_MODULES");
    char joined[8400];
    if (old && *old) snprintf(joined, sizeof(joined), "%s:%s", old, dir);
    else snprintf(joined, sizeof(joined), "%s", dir);
    setenv("GIO_EXTRA_MODULES", joined, 1);
}

static int toolkit_up(void) {
    if (W.toolkit) return W.toolkit > 0;
    // GTK must not touch the process locale (the game parses and prints "C" numbers)
    gtk_disable_setlocale();
    // The challenge page needs no GPU. WebKit's web process (Skia GPU raster, DMA-BUF renderer)
    // segfaulted in libnvidia-eglcore while tearing its GL context down (GrDirectContext dtor),
    // so render it on the CPU. Inherited by the web processes; an explicit user value wins.
    setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", 0);
    setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
    add_bundled_gio_modules();
    if (!gtk_init_check(NULL, NULL)) {
        W.toolkit = -1;
        snprintf(W.toolkit_err, sizeof(W.toolkit_err), "GTK found no display for the challenge window");
        printf("[webview] %s\n", W.toolkit_err);
        return 0;
    }
    W.toolkit = 1;
    printf("[webview] WebKitGTK %u.%u.%u\n", webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    return 1;
}

int kn_webview_captcha_open(const char *url, const char *sitekey, const char *title, const char *proxy, const int in_ui, const int show_now,
                            char *err, const size_t errn) {
    if (err && errn) err[0] = 0;
    if (env_off()) {
        if (err && errn) snprintf(err, errn, "the webview is turned off (KRUNKNATIVE_WEBVIEW=0)");
        return 0;
    }
    if (W.window) { // already up: bring it forward
        gtk_window_present(GTK_WINDOW(W.window));
        return 1;
    }
    if (W.inline_shown) return 1; // shown in the game UI already
    if (W.offscreen) { // running hidden: the player asks to see it (the modal's button)
        W.reveal = "the player asked for it";
        return 1;
    }
    char origin[sizeof(W.origin)];
    const char *resolved = NULL;
    if (!captcha_check(url, sitekey, origin, sizeof(origin), &resolved, err, errn)) return 0;
    if (!toolkit_up()) {
        if (err && errn) snprintf(err, errn, "%s", W.toolkit_err);
        return 0;
    }
    session_reset(in_ui, origin, title);
    // the scripts first: nothing is created when they cannot be built
    const char *shown_title = title && title[0] ? title : "Security Challenge";
    char *bridge = bridge_script();
    char *js = bridge ? user_script(sitekey, shown_title, W.in_ui) : NULL;
    if (!bridge || !js) {
        free(bridge);
        free(js);
        if (err && errn) snprintf(err, errn, "out of memory");
        return 0;
    }

    WebKitWebContext *ctx = webkit_web_context_new_ephemeral(); // nothing of it reaches the disk
    if (proxy && proxy[0]) { // the matchmaker's proxy (the player's setting / environment), loopback excluded
        static const char *const direct[] = {"localhost", "127.0.0.0/8", "::1", NULL};
        WebKitNetworkProxySettings *ps = webkit_network_proxy_settings_new(proxy, direct);
        webkit_website_data_manager_set_network_proxy_settings(webkit_web_context_get_website_data_manager(ctx), WEBKIT_NETWORK_PROXY_MODE_CUSTOM, ps);
        webkit_network_proxy_settings_free(ps);
    }
    WebKitUserContentManager *ucm = webkit_user_content_manager_new();
    g_signal_connect(ucm, "script-message-received::knCaptcha", G_CALLBACK(on_message), NULL);
    webkit_user_content_manager_register_script_message_handler_in_world(ucm, "knCaptcha", BRIDGE_WORLD);
    WebKitUserScript *script = webkit_user_script_new_for_world(bridge, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                                BRIDGE_WORLD, NULL, NULL);
    free(bridge);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
    script = webkit_user_script_new(js, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL); // the main world: Turnstile's
    free(js);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);

    W.view = WEBKIT_WEB_VIEW(g_object_new(WEBKIT_TYPE_WEB_VIEW, "web-context", ctx, "user-content-manager", ucm, NULL));
    g_object_unref(ucm);
    g_object_unref(ctx);
    WebKitSettings *settings = webkit_web_view_get_settings(W.view);
    webkit_settings_set_javascript_can_open_windows_automatically(settings, FALSE);
    webkit_settings_set_enable_developer_extras(settings, FALSE);
    webkit_settings_set_hardware_acceleration_policy(settings, WEBKIT_HARDWARE_ACCELERATION_POLICY_NEVER); // see toolkit_up
    if (W.in_ui) { // the page is drawn over the game's modal: nothing behind the widget but the transparent page
        GdkRGBA clear = {0.0, 0.0, 0.0, 0.0};
        webkit_web_view_set_background_color(W.view, &clear);
    }
    g_signal_connect(W.view, "decide-policy", G_CALLBACK(on_policy), NULL);
    g_signal_connect(W.view, "load-failed", G_CALLBACK(on_load_failed), NULL);
    g_signal_connect(W.view, "load-changed", G_CALLBACK(on_load_changed), NULL);
    g_signal_connect(W.view, "load-failed-with-tls-errors", G_CALLBACK(on_tls_error), NULL);
    g_signal_connect(W.view, "web-process-terminated", G_CALLBACK(on_terminated), NULL);

    // hidden first (see the top of this file): an offscreen toplevel, laid out at the window's size
    W.offscreen = gtk_offscreen_window_new();
    g_signal_connect_after(W.offscreen, "draw", G_CALLBACK(on_draw), NULL); // a new frame for the game UI
    gtk_widget_set_size_request(GTK_WIDGET(W.view), W.in_ui ? KN_WEBVIEW_INLINE_W : WINDOW_W, W.in_ui ? KN_WEBVIEW_INLINE_H : WINDOW_H);
    gtk_container_add(GTK_CONTAINER(W.offscreen), GTK_WIDGET(W.view));
    gtk_widget_show_all(W.offscreen);
    gtk_window_set_focus(GTK_WINDOW(W.offscreen), GTK_WIDGET(W.view)); // the keys the game UI forwards go to the view
    if (show_now) W.reveal = "the player asked for it"; // shown by the next poll (it reports KN_WEBVIEW_SHOWN)

    printf("[webview] challenge running hidden: %s (%s)%s; shown only if it needs the player\n", resolved, W.origin,
           proxy && proxy[0] ? ", through the client's proxy" : "");
    webkit_web_view_load_uri(W.view, resolved);
    kn_webview_pump();
    return 1;
}

void kn_webview_captcha_close(void) {
    if (W.window || W.offscreen) {
        W.closing = 1;
        GtkWidget *w = W.window ? W.window : W.offscreen;
        const int shown = W.window != NULL;
        if (W.view) webkit_web_view_stop_loading(W.view);
        gtk_widget_destroy(w); // the view, its context and web processes go with it
        W.window = W.offscreen = NULL;
        W.view = NULL;
        W.closing = 0;
        W.reveal = NULL;
        printf("[webview] challenge window closed%s\n", shown ? "" : " (it was never shown)");
    }
    if (W.inline_shown) printf("[webview] challenge closed (shown in the game UI)\n");
    session_end();
}

void kn_webview_pump(void) {
    if (W.toolkit <= 0) return;
    for (int i = 0; i < 256 && g_main_context_iteration(NULL, FALSE); i++) {
    }
}

#endif // KN_WV_GTK

// ---------------------------------------------------------------- WebView2 (Windows)
#if defined(KN_WV_WV2)

#define WV2_CLASS L"KrunkNativeWebView2" // the hidden host and the window share one window class
#define WV2_RUNTIME_MISSING "The Microsoft Edge WebView2 Runtime is not installed, so krunker.io's challenge cannot be shown."
#define WV2_OFF_DESKTOP (-32000)          // the hidden host sits here: nothing covers it, the player never sees it
#define WV2_CAPTURE_MS 60                 // the game UI's frames: a PNG about this often while the widget is shown

typedef HRESULT(STDAPICALLTYPE *PFN_CreateEnv)(PCWSTR, PCWSTR, ICoreWebView2EnvironmentOptions *,
                                              ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *);
typedef HRESULT(STDAPICALLTYPE *PFN_RuntimeVersion)(PCWSTR, LPWSTR *);

// Process state: the loader, the runtime check, the window class, the WIC factory, the proxy variable.
static struct {
    int state;       // 0 not tried, 1 ready, -1 unavailable (err says why)
    char err[200];
    HMODULE lib;
    PFN_CreateEnv create_env;
    PFN_RuntimeVersion runtime_version;
    ATOM atom;
    IWICImagingFactory *wic;
    int proxy_env;   // WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS holds the session's proxy (cleared once created)
} L;

// A session counter: callbacks of a session that ended are ignored (their objects are released by WebView2).
static int wv_gen;

static long long now_ms(void) { return (long long) GetTickCount64(); }

static int hidden_host(void) { return W.host != NULL; }

// A WebView2 completion or event handler: a COM object of our own with the one Invoke each of them takes
// (the arguments are pointers or HRESULTs: passed as intptr_t, which both are on x64).
typedef struct WvHandler WvHandler;
typedef HRESULT (*WvFn)(WvHandler *h, intptr_t a, intptr_t b);
typedef struct {
    HRESULT(STDMETHODCALLTYPE *QueryInterface)(WvHandler *h, REFIID riid, void **out);
    ULONG(STDMETHODCALLTYPE *AddRef)(WvHandler *h);
    ULONG(STDMETHODCALLTYPE *Release)(WvHandler *h);
    HRESULT(STDMETHODCALLTYPE *Invoke)(WvHandler *h, intptr_t a, intptr_t b);
} WvVtbl;
struct WvHandler {
    const WvVtbl *lpVtbl;
    LONG refs;
    const GUID *iid; // the handler's interface (QueryInterface accepts it, as IUnknown)
    WvFn fn;
    void *ctx;       // the callback's own state (a capture's stream)
    int gen;         // wv_gen when it was made
};

static ULONG STDMETHODCALLTYPE wv_addref(WvHandler *h) { return (ULONG) InterlockedIncrement(&h->refs); }

static ULONG STDMETHODCALLTYPE wv_rel(WvHandler *h) {
    const LONG n = InterlockedDecrement(&h->refs);
    if (n == 0) free(h);
    return (ULONG) n;
}

static HRESULT STDMETHODCALLTYPE wv_qi(WvHandler *h, REFIID riid, void **out) {
    if (IsEqualIID(riid, &IID_IUnknown) || (h->iid && IsEqualIID(riid, h->iid))) {
        *out = h;
        wv_addref(h);
        return S_OK;
    }
    *out = NULL;
    return E_NOINTERFACE;
}

static HRESULT STDMETHODCALLTYPE wv_invoke(WvHandler *h, intptr_t a, intptr_t b) { return h->fn(h, a, b); }

static const WvVtbl wv_vtbl = {wv_qi, wv_addref, wv_rel, wv_invoke};

static WvHandler *wv_handler(const GUID *iid, WvFn fn) {
    WvHandler *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    h->lpVtbl = &wv_vtbl;
    h->refs = 1;
    h->iid = iid;
    h->fn = fn;
    h->gen = wv_gen;
    return h;
}

// Our reference is given up after the call that took the handler (WebView2 keeps its own).
static void wv_unref(WvHandler *h) {
    if (h) wv_rel(h);
}

static HRESULT on_ignored(WvHandler *h, intptr_t a, intptr_t b) {
    (void) h;
    (void) a;
    (void) b;
    return S_OK;
}

// UTF-8 to a new wide string (the caller frees it), and back (CoTaskMemFree is the caller's for the latter).
static wchar_t *wide_of(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t) n * sizeof(wchar_t));
    if (w && !MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n)) {
        free(w);
        return NULL;
    }
    return w;
}

static char *utf8_of(const wchar_t *w) {
    if (!w) return NULL;
    const int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, NULL, 0, NULL, NULL);
    if (n <= 0) return NULL;
    char *s = malloc((size_t) n);
    if (s && !WideCharToMultiByte(CP_UTF8, 0, w, -1, s, n, NULL, NULL)) {
        free(s);
        return NULL;
    }
    return s;
}

// The loader and the runtime, once: no loader or no runtime makes the backend unavailable (err says why).
static int wv2_unavailable(const char *why) {
    snprintf(L.err, sizeof(L.err), "%s", why);
    printf("[webview] %s\n", why);
    return 0;
}

static int wv2_load(void) {
    // the loader next to the client, else the default search
    wchar_t path[MAX_PATH];
    const DWORD n = GetModuleFileNameW(NULL, path, MAX_PATH);
    const wchar_t *slash = n > 0 && n < MAX_PATH ? wcsrchr(path, L'\\') : NULL;
    if (slash) {
        wchar_t dll[MAX_PATH];
        _snwprintf(dll, MAX_PATH, L"%.*s\\WebView2Loader.dll", (int) (slash - path), path);
        dll[MAX_PATH - 1] = 0;
        L.lib = LoadLibraryW(dll);
    }
    if (!L.lib) L.lib = LoadLibraryW(L"WebView2Loader.dll");
    if (!L.lib) return wv2_unavailable("The WebView2 loader (WebView2Loader.dll) could not be loaded, so krunker.io's challenge cannot be shown.");
    L.create_env = (PFN_CreateEnv) (void *) GetProcAddress(L.lib, "CreateCoreWebView2EnvironmentWithOptions");
    L.runtime_version = (PFN_RuntimeVersion) (void *) GetProcAddress(L.lib, "GetAvailableCoreWebView2BrowserVersionString");
    if (!L.create_env || !L.runtime_version)
        return wv2_unavailable("The WebView2 loader is too old, so krunker.io's challenge cannot be shown.");
    // the widget's objects live on the main thread: a single-threaded apartment
    const HRESULT com = CoInitializeEx(NULL, COINIT_APARTMENTTHREADED);
    if (FAILED(com)) return wv2_unavailable("This thread cannot host the WebView2 runtime, so krunker.io's challenge cannot be shown.");
    // the runtime: Edge's WebView2 runtime is what the view is, and GetAvailable... fails without it
    LPWSTR version = NULL;
    const HRESULT hr = L.runtime_version(NULL, &version);
    if (FAILED(hr) || !version) {
        printf("[webview] no WebView2 runtime (0x%08lx)\n", (unsigned long) hr);
        return wv2_unavailable(WV2_RUNTIME_MISSING);
    }
    CoTaskMemFree(version);
    return 1;
}

static int wv2_loader(char *err, const size_t errn) {
    if (!L.state) L.state = wv2_load() ? 1 : -1;
    if (L.state < 0 && err && errn) snprintf(err, errn, "%s", L.err);
    return L.state > 0;
}

// The windows' messages: the player closing the challenge's window ends it (the poll reports it).
static LRESULT CALLBACK wv2_wndproc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    if (msg == WM_DESTROY) {
        if (hwnd == W.window) {
            W.window = NULL;
            W.target = NULL;
            if (!W.closing) {
                printf("[webview] the player closed the challenge window\n");
                W.ev_closed = 1;
            }
        } else if (hwnd == W.host) {
            W.host = NULL;
            W.target = NULL;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

static int wv2_class(void) {
    if (L.atom) return 1;
    WNDCLASSEXW wc;
    memset(&wc, 0, sizeof(wc));
    wc.cbSize = sizeof(wc);
    wc.lpfnWndProc = wv2_wndproc;
    wc.hInstance = GetModuleHandleW(NULL);
    wc.lpszClassName = WV2_CLASS;
    wc.hCursor = LoadCursorW(NULL, (LPCWSTR) IDC_ARROW);
    L.atom = RegisterClassExW(&wc);
    return L.atom != 0;
}

// The hidden host: a popup parked off the desktop (nothing covers it, the player never sees it). WebView2
// renders into it all the same, and the controller is created on it.
static HWND wv2_host(const int w, const int h) {
    return CreateWindowExW(WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE, WV2_CLASS, L"", WS_POPUP, WV2_OFF_DESKTOP, WV2_OFF_DESKTOP, w, h, NULL,
                           NULL, GetModuleHandleW(NULL), NULL);
}

// The session's proxy (the matchmaker's, the same as the game's requests): WebView2 takes the browser's
// arguments from WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS when its environment is created. The address only:
// Chromium's proxy switch takes no user name, so a proxy with one is refused (the request ends).
static int wv2_proxy_env(const char *proxy, char *err, const size_t errn) {
    if (!proxy || !proxy[0]) return 1;
    if (strchr(proxy, '@') || strlen(proxy) > 300) {
        if (err && errn) snprintf(err, errn, "the Windows webview cannot use this proxy (it takes an address, without a user name)");
        return 0;
    }
    char uri[320];
    if (strstr(proxy, "://")) snprintf(uri, sizeof(uri), "%s", proxy);
    else snprintf(uri, sizeof(uri), "http://%s", proxy);
    char args[400];
    snprintf(args, sizeof(args), "--proxy-server=%s --proxy-bypass-list=localhost;127.0.0.1;[::1]", uri);
    wchar_t *w = wide_of(args);
    const BOOL ok = w && SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", w);
    free(w);
    if (!ok) {
        if (err && errn) snprintf(err, errn, "the Windows webview could not take the proxy");
        return 0;
    }
    L.proxy_env = 1;
    return 1;
}

static void wv2_clear_proxy_env(void) {
    if (!L.proxy_env) return;
    SetEnvironmentVariableW(L"WEBVIEW2_ADDITIONAL_BROWSER_ARGUMENTS", NULL);
    L.proxy_env = 0;
}

// The session's temporary user data folder: the view keeps nothing (removed at close, best effort).
static int wv2_session_dir(void) {
    wchar_t tmp[MAX_PATH];
    if (!GetTempPathW(MAX_PATH, tmp)) return 0;
    _snwprintf(W.session_dir, MAX_PATH, L"%sKrunkNative-webview2-%lu-%d", tmp, (unsigned long) GetCurrentProcessId(), wv_gen);
    W.session_dir[MAX_PATH - 1] = 0;
    return CreateDirectoryW(W.session_dir, NULL) ? 1 : 0;
}

static void wv2_remove_session_dir(void) {
    if (!W.session_dir[0]) return;
    wchar_t from[MAX_PATH + 1];
    memset(from, 0, sizeof(from)); // (the two NULs at the end close the list)
    wcsncpy(from, W.session_dir, MAX_PATH);
    SHFILEOPSTRUCTW op;
    memset(&op, 0, sizeof(op));
    op.wFunc = FO_DELETE;
    op.pFrom = from;
    op.fFlags = FOF_NO_UI | FOF_NOCONFIRMATION | FOF_SILENT | FOF_NOERRORUI;
    SHFileOperationW(&op);
    W.session_dir[0] = 0;
}

// The widget's script, wrapped to run once the document has parsed (WebView2 injects it before the page's
// own scripts; the widget needs the body). js is freed.
static char *wv2_wrap(char *js) {
    Sb b = {0};
    sb_put(&b, "(function () {\nvar go = function () {\n");
    if (js) sb_put(&b, js);
    sb_put(&b, "};\nif (document.readyState === 'loading') document.addEventListener('DOMContentLoaded', go); else go();\n})();\n");
    free(js);
    return sb_take(&b);
}

// A PNG of the view (CapturePreview) to the straight RGBA8 frame the game UI draws (W.px, row 0 on top).
static void decode_frame(IStream *s) {
    if (!L.wic && FAILED(CoCreateInstance(&CLSID_WICImagingFactory, NULL, CLSCTX_INPROC_SERVER, &IID_IWICImagingFactory, (void **) &L.wic)))
        return;
    LARGE_INTEGER zero;
    zero.QuadPart = 0;
    s->lpVtbl->Seek(s, zero, STREAM_SEEK_SET, NULL);
    IWICBitmapDecoder *dec = NULL;
    IWICBitmapFrameDecode *frame = NULL;
    IWICFormatConverter *conv = NULL;
    UINT w = 0, h = 0;
    HRESULT hr = L.wic->lpVtbl->CreateDecoderFromStream(L.wic, s, NULL, WICDecodeMetadataCacheOnDemand, &dec);
    if (SUCCEEDED(hr)) hr = dec->lpVtbl->GetFrame(dec, 0, &frame);
    if (SUCCEEDED(hr)) hr = frame->lpVtbl->GetSize(frame, &w, &h);
    if (SUCCEEDED(hr) && (w == 0 || h == 0)) hr = E_FAIL;
    if (SUCCEEDED(hr)) hr = L.wic->lpVtbl->CreateFormatConverter(L.wic, &conv);
    if (SUCCEEDED(hr))
        hr = conv->lpVtbl->Initialize(conv, (IWICBitmapSource *) frame, &GUID_WICPixelFormat32bppRGBA, WICBitmapDitherTypeNone, NULL, 0.0,
                                      WICBitmapPaletteTypeCustom);
    const size_t need = (size_t) w * (size_t) h * 4;
    if (SUCCEEDED(hr) && need > W.px_cap) {
        unsigned char *p = realloc(W.px, need);
        if (p) {
            W.px = p;
            W.px_cap = need;
        } else {
            hr = E_OUTOFMEMORY;
        }
    }
    if (SUCCEEDED(hr)) hr = conv->lpVtbl->CopyPixels(conv, NULL, w * 4, (UINT) need, W.px);
    if (SUCCEEDED(hr)) {
        W.px_w = (int) w;
        W.px_h = (int) h;
        W.dirty = 1;
    }
    if (conv) conv->lpVtbl->Release(conv);
    if (frame) frame->lpVtbl->Release(frame);
    if (dec) dec->lpVtbl->Release(dec);
}

// The capture's completion: the PNG is decoded into the frame (for this session only); the stream goes.
static HRESULT on_capture_done(WvHandler *h, intptr_t hr, intptr_t unused) {
    (void) unused;
    IStream *s = (IStream *) h->ctx;
    h->ctx = NULL;
    if (h->gen == wv_gen) {
        W.capturing = 0;
        if (SUCCEEDED((HRESULT) hr) && s) decode_frame(s);
    }
    if (s) s->lpVtbl->Release(s);
    return S_OK;
}

// One frame request while the game UI shows the widget (at most one in flight).
static void wv2_request_capture(void) {
    if (!W.view || W.capturing) return;
    IStream *s = NULL;
    if (FAILED(CreateStreamOnHGlobal(NULL, TRUE, &s)) || !s) return;
    WvHandler *h = wv_handler(&IID_ICoreWebView2CapturePreviewCompletedHandler, on_capture_done);
    if (!h) {
        s->lpVtbl->Release(s);
        return;
    }
    h->ctx = s;
    W.capturing = 1;
    W.capture_ms = now_ms();
    const HRESULT hr = W.view->lpVtbl->CapturePreview(W.view, COREWEBVIEW2_CAPTURE_PREVIEW_IMAGE_FORMAT_PNG, s,
                                                     (ICoreWebView2CapturePreviewCompletedHandler *) h);
    if (FAILED(hr)) { // no completion comes: the stream is still ours
        W.capturing = 0;
        h->ctx = NULL;
        s->lpVtbl->Release(s);
    }
    wv_unref(h);
}

static void backend_tick(void) {
    if (W.inline_shown && !W.capturing && W.view && now_ms() - W.capture_ms >= WV2_CAPTURE_MS) wv2_request_capture();
}

// The challenge did not start or broke (the runtime, the window): the request ends (the poll reports it).
static void wv2_fail(const char *text) {
    printf("[webview] %s\n", text);
    set_msg(text);
    W.ev_failed = 1;
}

static HRESULT on_nav_starting(WvHandler *h, intptr_t sender, intptr_t args_) {
    (void) sender;
    if (h->gen != wv_gen) return S_OK;
    ICoreWebView2NavigationStartingEventArgs *args = (ICoreWebView2NavigationStartingEventArgs *) args_;
    LPWSTR w = NULL;
    char *uri = NULL;
    if (SUCCEEDED(args->lpVtbl->get_Uri(args, &w))) uri = utf8_of(w);
    CoTaskMemFree(w);
    if (!navigation_ok(uri)) args->lpVtbl->put_Cancel(args, TRUE);
    free(uri);
    return S_OK;
}

static int wv2_is_tls(const COREWEBVIEW2_WEB_ERROR_STATUS st) {
    return st == COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_COMMON_NAME_IS_INCORRECT || st == COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_EXPIRED ||
           st == COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_REVOKED || st == COREWEBVIEW2_WEB_ERROR_STATUS_CERTIFICATE_IS_INVALID;
}

// A navigation that ended: a failure of krunker.io's own page is said (a certificate is never bypassed).
static HRESULT on_nav_completed(WvHandler *h, intptr_t sender, intptr_t args_) {
    (void) sender;
    if (h->gen != wv_gen || !W.view) return S_OK;
    ICoreWebView2NavigationCompletedEventArgs *args = (ICoreWebView2NavigationCompletedEventArgs *) args_;
    BOOL ok = FALSE;
    COREWEBVIEW2_WEB_ERROR_STATUS st = COREWEBVIEW2_WEB_ERROR_STATUS_UNKNOWN;
    args->lpVtbl->get_IsSuccess(args, &ok);
    args->lpVtbl->get_WebErrorStatus(args, &st);
    if (ok || st == COREWEBVIEW2_WEB_ERROR_STATUS_OPERATION_CANCELED) return S_OK;
    LPWSTR w = NULL;
    char *uri = NULL;
    if (SUCCEEDED(W.view->lpVtbl->get_Source(W.view, &w))) uri = utf8_of(w);
    CoTaskMemFree(w);
    if (same_origin(uri)) {
        char detail[48];
        snprintf(detail, sizeof(detail), "error %d", (int) st);
        if (wv2_is_tls(st)) tls_refused(detail);
        else load_failed(detail);
    }
    free(uri);
    return S_OK;
}

static HRESULT on_new_window(WvHandler *h, intptr_t sender, intptr_t args_) {
    (void) sender;
    if (h->gen != wv_gen) return S_OK;
    ICoreWebView2NewWindowRequestedEventArgs *args = (ICoreWebView2NewWindowRequestedEventArgs *) args_;
    printf("[webview] a new window was refused\n");
    args->lpVtbl->put_Handled(args, TRUE);
    return S_OK;
}

// The widget's messages: the source's origin is checked here (WebView2 has no isolated world to post from).
static HRESULT on_web_message(WvHandler *h, intptr_t sender, intptr_t args_) {
    (void) sender;
    if (h->gen != wv_gen) return S_OK;
    ICoreWebView2WebMessageReceivedEventArgs *args = (ICoreWebView2WebMessageReceivedEventArgs *) args_;
    LPWSTR src = NULL, msg = NULL;
    args->lpVtbl->get_Source(args, &src);
    args->lpVtbl->TryGetWebMessageAsString(args, &msg);
    char *from = utf8_of(src);
    char *text = utf8_of(msg);
    char o[sizeof(W.origin)];
    const int on_origin = from && origin_of(from, o, sizeof(o)) && !strcmp(o, W.origin);
    if (text) {
        widget_message(text, on_origin);
        wipe(text, strlen(text));
    }
    free(text);
    free(from);
    CoTaskMemFree(src);
    CoTaskMemFree(msg);
    return S_OK;
}

static HRESULT on_process_failed(WvHandler *h, intptr_t sender, intptr_t args_) {
    (void) sender;
    if (h->gen != wv_gen) return S_OK;
    ICoreWebView2ProcessFailedEventArgs *args = (ICoreWebView2ProcessFailedEventArgs *) args_;
    COREWEBVIEW2_PROCESS_FAILED_KIND kind = COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED;
    args->lpVtbl->get_ProcessFailedKind(args, &kind);
    if (kind == COREWEBVIEW2_PROCESS_FAILED_KIND_BROWSER_PROCESS_EXITED || kind == COREWEBVIEW2_PROCESS_FAILED_KIND_RENDER_PROCESS_EXITED) {
        char detail[32];
        snprintf(detail, sizeof(detail), "kind %d", (int) kind);
        page_process_stopped(detail);
    }
    return S_OK;
}

// The controller's background: alpha 0 is transparent (the game UI's page), else the colour.
static void set_background(ICoreWebView2Controller *ctrl, const BYTE a, const BYTE r, const BYTE g, const BYTE b) {
    ICoreWebView2Controller2 *c2 = NULL;
    if (SUCCEEDED(ctrl->lpVtbl->QueryInterface(ctrl, &IID_ICoreWebView2Controller2, (void **) &c2)) && c2) {
        COREWEBVIEW2_COLOR col;
        col.A = a;
        col.R = r;
        col.G = g;
        col.B = b;
        c2->lpVtbl->put_DefaultBackgroundColor(c2, col);
        c2->lpVtbl->Release(c2);
    }
}

// The view exists: its settings, its size, the events the session watches, the widget's script, then the
// challenge's page (in that order: the script runs in the page's first document).
static HRESULT on_controller_created(WvHandler *h, intptr_t hr_, intptr_t ctrl_) {
    if (h->gen != wv_gen) return S_OK;
    const HRESULT hr = (HRESULT) hr_;
    ICoreWebView2Controller *ctrl = (ICoreWebView2Controller *) ctrl_;
    if (FAILED(hr) || !ctrl || !W.host) {
        char text[160];
        snprintf(text, sizeof(text), "The Microsoft Edge WebView2 Runtime could not attach the challenge (0x%08lx).", (unsigned long) hr);
        wv2_fail(text);
        return S_OK;
    }
    ctrl->lpVtbl->AddRef(ctrl);
    W.controller = ctrl;
    ICoreWebView2 *view = NULL;
    if (FAILED(ctrl->lpVtbl->get_CoreWebView2(ctrl, &view)) || !view) {
        wv2_fail("The Microsoft Edge WebView2 Runtime gave no view for the challenge.");
        return S_OK;
    }
    W.view = view;
    ICoreWebView2Settings *settings = NULL;
    if (SUCCEEDED(view->lpVtbl->get_Settings(view, &settings)) && settings) {
        settings->lpVtbl->put_AreDefaultContextMenusEnabled(settings, FALSE);
        settings->lpVtbl->put_AreDevToolsEnabled(settings, FALSE);
        settings->lpVtbl->put_IsStatusBarEnabled(settings, FALSE);
        settings->lpVtbl->Release(settings);
    }
    RECT rc;
    memset(&rc, 0, sizeof(rc));
    GetClientRect(W.host, &rc);
    ctrl->lpVtbl->put_Bounds(ctrl, rc);
    ctrl->lpVtbl->put_IsVisible(ctrl, TRUE);
    if (W.in_ui) set_background(ctrl, 0, 0, 0, 0); // transparent: the page is the widget only

    EventRegistrationToken token;
    WvHandler *e;
    e = wv_handler(&IID_ICoreWebView2NavigationStartingEventHandler, on_nav_starting);
    if (e) {
        view->lpVtbl->add_NavigationStarting(view, (ICoreWebView2NavigationStartingEventHandler *) e, &token);
        wv_unref(e);
    }
    e = wv_handler(&IID_ICoreWebView2NavigationCompletedEventHandler, on_nav_completed);
    if (e) {
        view->lpVtbl->add_NavigationCompleted(view, (ICoreWebView2NavigationCompletedEventHandler *) e, &token);
        wv_unref(e);
    }
    e = wv_handler(&IID_ICoreWebView2NewWindowRequestedEventHandler, on_new_window);
    if (e) {
        view->lpVtbl->add_NewWindowRequested(view, (ICoreWebView2NewWindowRequestedEventHandler *) e, &token);
        wv_unref(e);
    }
    e = wv_handler(&IID_ICoreWebView2WebMessageReceivedEventHandler, on_web_message);
    if (e) {
        view->lpVtbl->add_WebMessageReceived(view, (ICoreWebView2WebMessageReceivedEventHandler *) e, &token);
        wv_unref(e);
    }
    e = wv_handler(&IID_ICoreWebView2ProcessFailedEventHandler, on_process_failed);
    if (e) {
        view->lpVtbl->add_ProcessFailed(view, (ICoreWebView2ProcessFailedEventHandler *) e, &token);
        wv_unref(e);
    }
    if (W.script) {
        WvHandler *a = wv_handler(&IID_ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler, on_ignored);
        if (a) {
            view->lpVtbl->AddScriptToExecuteOnDocumentCreated(view, W.script, (ICoreWebView2AddScriptToExecuteOnDocumentCreatedCompletedHandler *) a);
            wv_unref(a);
        }
        free(W.script);
        W.script = NULL;
    }
    if (W.navigate) {
        view->lpVtbl->Navigate(view, W.navigate);
        free(W.navigate);
        W.navigate = NULL;
    }
    ctrl->lpVtbl->MoveFocus(ctrl, COREWEBVIEW2_MOVE_FOCUS_REASON_PROGRAMMATIC); // the keys the game UI forwards go to the view
    return S_OK;
}

// The environment exists: its controller on the hidden host (the browser's arguments were read by now).
static HRESULT on_env_created(WvHandler *h, intptr_t hr_, intptr_t env_) {
    if (h->gen != wv_gen) return S_OK;
    wv2_clear_proxy_env();
    const HRESULT hr = (HRESULT) hr_;
    ICoreWebView2Environment *env = (ICoreWebView2Environment *) env_;
    if (FAILED(hr) || !env || !W.host) {
        char text[160];
        snprintf(text, sizeof(text), "The Microsoft Edge WebView2 Runtime did not start (0x%08lx).", (unsigned long) hr);
        wv2_fail(text);
        return S_OK;
    }
    env->lpVtbl->AddRef(env);
    W.env = env;
    WvHandler *c = wv_handler(&IID_ICoreWebView2CreateCoreWebView2ControllerCompletedHandler, on_controller_created);
    const HRESULT r = c ? env->lpVtbl->CreateCoreWebView2Controller(env, W.host, (ICoreWebView2CreateCoreWebView2ControllerCompletedHandler *) c)
                        : E_OUTOFMEMORY;
    wv_unref(c);
    if (FAILED(r)) {
        char text[160];
        snprintf(text, sizeof(text), "The Microsoft Edge WebView2 Runtime could not show the challenge (0x%08lx).", (unsigned long) r);
        wv2_fail(text);
    }
    return S_OK;
}

// The view's render window: the one that takes the forwarded input (found on first use, again after a reveal).
static BOOL CALLBACK wv2_find_render(HWND h, LPARAM p) {
    wchar_t cls[64];
    memset(cls, 0, sizeof(cls));
    GetClassNameW(h, cls, 64);
    if (!wcscmp(cls, L"Chrome_RenderWidgetHostHWND")) {
        *(HWND *) p = h;
        return FALSE;
    }
    EnumChildWindows(h, wv2_find_render, p);
    return *(HWND *) p == NULL;
}

static HWND wv2_target(void) {
    if (W.target && IsWindow(W.target)) return W.target;
    HWND root = W.window ? W.window : W.host;
    if (!root) return NULL;
    HWND found = NULL;
    EnumChildWindows(root, wv2_find_render, (LPARAM) &found);
    W.target = found ? found : root;
    return W.target;
}

static void wv2_release_objects(void) {
    if (W.view) {
        W.view->lpVtbl->Release(W.view);
        W.view = NULL;
    }
    if (W.controller) {
        W.controller->lpVtbl->Release(W.controller);
        W.controller = NULL;
    }
    if (W.env) {
        W.env->lpVtbl->Release(W.env);
        W.env = NULL;
    }
    free(W.script);
    W.script = NULL;
    free(W.navigate);
    W.navigate = NULL;
}

// A session that did not start: everything it made goes.
static void wv2_abort(void) {
    HWND host = W.host;
    W.host = NULL;
    if (host) DestroyWindow(host);
    wv2_release_objects();
    wv2_clear_proxy_env();
    wv2_remove_session_dir();
    W.target = NULL;
    wv_gen++;
    session_end();
}

// Shows the challenge: the view leaves its hidden host for a real window, created at its final place
// (centred on the work area, on top of the game) before it is shown, so it never jumps. The game UI
// (in_ui, not failed) takes it with no window: the caller draws its frames (inline_shown). Otherwise the
// window; with in_ui its page is the widget only, so the window's background is set dark.
static void reveal(const char *why) {
    if (W.inline_shown || W.window || !W.host) return;
    if (!W.controller) { // the view is not ready yet (the environment is still starting): the reason waits for it
        W.reveal = why;
        return;
    }
    if (kn_webview_reveal_target(W.in_ui, W.inline_failed) == KN_WEBVIEW_VIEW_INLINE) {
        W.inline_shown = 1;
        W.shown_ms = now_ms();
        W.frames = 0;
        W.dirty = 1; // (the first frame is taken at once)
        printf("[webview] challenge shown in the game UI (%s): no window\n", why);
        return;
    }
    RECT want = {0, 0, WINDOW_W, WINDOW_H};
    AdjustWindowRect(&want, WS_OVERLAPPEDWINDOW, FALSE);
    int w = want.right - want.left, h = want.bottom - want.top, x = 0, y = 0;
    POINT pt = {0, 0};
    GetCursorPos(&pt);
    MONITORINFO mi;
    memset(&mi, 0, sizeof(mi));
    mi.cbSize = sizeof(mi);
    const int placed = GetMonitorInfoW(MonitorFromPoint(pt, MONITOR_DEFAULTTOPRIMARY), &mi);
    if (placed) { // centred on the work area of the monitor under the pointer (the game's)
        const int aw = mi.rcWork.right - mi.rcWork.left, ah = mi.rcWork.bottom - mi.rcWork.top;
        if (w > aw) w = aw;
        if (h > ah) h = ah;
        x = mi.rcWork.left + (aw - w) / 2;
        y = mi.rcWork.top + (ah - h) / 2;
    }
    wchar_t *title = wide_of(W.title);
    HWND win = CreateWindowExW(WS_EX_TOPMOST, WV2_CLASS, title ? title : L"", WS_OVERLAPPEDWINDOW, placed ? x : CW_USEDEFAULT,
                               placed ? y : CW_USEDEFAULT, w, h, NULL, NULL, GetModuleHandleW(NULL), NULL);
    free(title);
    if (!win) {
        wv2_fail("The challenge's window could not be created.");
        return;
    }
    W.window = win;
    if (W.in_ui) set_background(W.controller, 255, 27, 27, 27); // the widget alone on the dark page
    W.controller->lpVtbl->put_ParentWindow(W.controller, win);
    RECT rc;
    memset(&rc, 0, sizeof(rc));
    GetClientRect(win, &rc);
    W.controller->lpVtbl->put_Bounds(W.controller, rc);
    HWND host = W.host;
    W.host = NULL;
    W.target = NULL;
    if (host) DestroyWindow(host);
    ShowWindow(win, SW_SHOW);
    SetForegroundWindow(win);
    printf("[webview] challenge window shown (%s) at %d,%d %dx%d%s\n", why, x, y, w, h, placed ? ", centred on the work area" : "");
}

// ---------------------------------------------------------------- API (WebView2)

int kn_webview_available(void) { return !env_off() && wv2_loader(NULL, 0); }

int kn_webview_captcha_open(const char *url, const char *sitekey, const char *title, const char *proxy, const int in_ui, const int show_now,
                            char *err, const size_t errn) {
    if (err && errn) err[0] = 0;
    if (env_off()) {
        if (err && errn) snprintf(err, errn, "the webview is turned off (KRUNKNATIVE_WEBVIEW=0)");
        return 0;
    }
    if (W.window) { // already up: bring it forward
        ShowWindow(W.window, SW_SHOW);
        SetForegroundWindow(W.window);
        return 1;
    }
    if (W.inline_shown) return 1; // shown in the game UI already
    if (W.host) { // running hidden: the player asks to see it (the modal's button)
        W.reveal = "the player asked for it";
        return 1;
    }
    char origin[sizeof(W.origin)];
    const char *resolved = NULL;
    if (!captcha_check(url, sitekey, origin, sizeof(origin), &resolved, err, errn)) return 0;
    if (!wv2_loader(err, errn)) return 0; // no loader or no runtime: unavailable, as no webview
    if (!wv2_class()) {
        if (err && errn) snprintf(err, errn, "the challenge's window class could not be registered");
        return 0;
    }
    if (!wv2_proxy_env(proxy, err, errn)) return 0;
    session_reset(in_ui, origin, title);
    wv_gen++;
    char *js = user_script(sitekey, title && title[0] ? title : "Security Challenge", W.in_ui);
    char *wrapped = js ? wv2_wrap(js) : NULL;
    W.script = wrapped ? wide_of(wrapped) : NULL;
    free(wrapped);
    W.navigate = wide_of(resolved);
    if (!W.script || !W.navigate || !wv2_session_dir()) {
        wv2_abort();
        if (err && errn) snprintf(err, errn, "the challenge could not be prepared (out of memory or no temporary folder)");
        return 0;
    }
    W.host = wv2_host(W.in_ui ? KN_WEBVIEW_INLINE_W : WINDOW_W, W.in_ui ? KN_WEBVIEW_INLINE_H : WINDOW_H);
    if (!W.host) {
        wv2_abort();
        if (err && errn) snprintf(err, errn, "the challenge's window could not be created");
        return 0;
    }
    ShowWindow(W.host, SW_SHOWNOACTIVATE); // (off the desktop: WebView2 renders the view into it)
    WvHandler *h = wv_handler(&IID_ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler, on_env_created);
    const HRESULT hr = h ? L.create_env(NULL, W.session_dir, NULL, (ICoreWebView2CreateCoreWebView2EnvironmentCompletedHandler *) h) : E_OUTOFMEMORY;
    wv_unref(h);
    if (FAILED(hr)) {
        wv2_abort();
        if (err && errn) snprintf(err, errn, "The Microsoft Edge WebView2 Runtime did not start (0x%08lx).", (unsigned long) hr);
        return 0;
    }
    if (show_now) W.reveal = "the player asked for it"; // shown by the next poll (it reports KN_WEBVIEW_SHOWN)
    printf("[webview] challenge running hidden: %s (%s)%s; shown only if it needs the player\n", resolved, W.origin,
           proxy && proxy[0] ? ", through the client's proxy" : "");
    kn_webview_pump();
    return 1;
}

void kn_webview_captcha_close(void) {
    if (W.window || W.host || W.controller || W.env) {
        W.closing = 1;
        const int shown = W.window != NULL;
        if (W.view) W.view->lpVtbl->Stop(W.view);
        if (W.controller) W.controller->lpVtbl->Close(W.controller);
        wv2_release_objects();
        HWND win = W.window, host = W.host;
        W.window = NULL;
        W.host = NULL;
        W.target = NULL;
        W.capturing = 0;
        if (win) DestroyWindow(win);
        if (host) DestroyWindow(host);
        wv2_clear_proxy_env();
        wv2_remove_session_dir();
        wv_gen++;
        W.closing = 0;
        printf("[webview] challenge window closed%s\n", shown ? "" : " (it was never shown)");
    }
    if (W.inline_shown) printf("[webview] challenge closed (shown in the game UI)\n");
    session_end();
}

// The window messages of this thread (the view's callbacks come through them), a few at a time.
void kn_webview_pump(void) {
    if (!W.host && !W.window && !W.env) return;
    MSG m;
    for (int i = 0; i < 256 && PeekMessageW(&m, NULL, 0, 0, PM_REMOVE); i++) {
        if (m.message == WM_QUIT) {
            PostQuitMessage((int) m.wParam);
            break;
        }
        TranslateMessage(&m);
        DispatchMessageW(&m);
    }
}

// The game UI's view: the newest frame when the view has painted since the last one taken.
int kn_webview_captcha_frame(const unsigned char **rgba, int *w, int *h) {
    if (!W.inline_shown || !W.dirty || !W.px) return 0;
    W.dirty = 0;
    W.frames++;
    *rgba = W.px;
    *w = W.px_w;
    *h = W.px_h;
    return 1;
}

// The pointer over the view, in view px: the render window gets the mouse messages (its own coordinates).
int kn_webview_captcha_pointer(const int kind, const double x, const double y, const int button) {
    if (!W.inline_shown || !W.host) return 0;
    HWND t = wv2_target();
    if (!t) return 0;
    if (kind == KN_WEBVIEW_POINTER_MOVE) {
        // (no button change)
    } else {
        const unsigned bit = button == 1 ? 1u : button == 2 ? 2u : button == 3 ? 4u : 0u;
        if (!bit) return 0;
        if (kind == KN_WEBVIEW_POINTER_PRESS) W.buttons |= bit;
        else W.buttons &= ~bit;
    }
    const WPARAM held = (W.buttons & 1 ? MK_LBUTTON : 0) | (W.buttons & 2 ? MK_MBUTTON : 0) | (W.buttons & 4 ? MK_RBUTTON : 0);
    UINT msg = WM_MOUSEMOVE;
    if (kind != KN_WEBVIEW_POINTER_MOVE) {
        const int press = kind == KN_WEBVIEW_POINTER_PRESS;
        if (button == 1) msg = press ? WM_LBUTTONDOWN : WM_LBUTTONUP;
        else if (button == 2) msg = press ? WM_MBUTTONDOWN : WM_MBUTTONUP;
        else msg = press ? WM_RBUTTONDOWN : WM_RBUTTONUP;
    }
    PostMessageW(t, msg, held, MAKELPARAM((int) x, (int) y));
    return 1;
}

static UINT vk_of(const int key) {
    switch (key) {
        case KN_WEBVIEW_KEY_TAB: return VK_TAB;
        case KN_WEBVIEW_KEY_ENTER: return VK_RETURN;
        case KN_WEBVIEW_KEY_SPACE: return VK_SPACE;
        case KN_WEBVIEW_KEY_BACKSPACE: return VK_BACK;
        case KN_WEBVIEW_KEY_DELETE: return VK_DELETE;
        case KN_WEBVIEW_KEY_LEFT: return VK_LEFT;
        case KN_WEBVIEW_KEY_RIGHT: return VK_RIGHT;
        case KN_WEBVIEW_KEY_UP: return VK_UP;
        case KN_WEBVIEW_KEY_DOWN: return VK_DOWN;
        case KN_WEBVIEW_KEY_HOME: return VK_HOME;
        case KN_WEBVIEW_KEY_END: return VK_END;
        default: return 0;
    }
}

int kn_webview_captcha_key(const int key, const int pressed) {
    if (!W.inline_shown || !W.host) return 0;
    if (pressed == 2) return 1; // a repeat: taken, not sent (the widget takes the press)
    const UINT vk = vk_of(key);
    if (!vk) return 0;
    HWND t = wv2_target();
    if (!t) return 0;
    const UINT sc = MapVirtualKeyW(vk, MAPVK_VK_TO_VSC);
    LPARAM lp = 1 | ((LPARAM) sc << 16); // repeat count 1, the scan code
    if (!pressed) lp |= ((LPARAM) 1 << 30) | ((LPARAM) 1 << 31); // released: it was down, and it is up now
    PostMessageW(t, pressed ? WM_KEYDOWN : WM_KEYUP, vk, lp);
    return 1;
}

// The typed text: one WM_CHAR per UTF-16 unit (a code point above U+FFFF is a surrogate pair).
int kn_webview_captcha_char(const unsigned codepoint) {
    if (!W.inline_shown || !W.host || codepoint <= 0x20 || codepoint == 0x7f || codepoint > 0x10ffff) return 0; // (space is a key)
    HWND t = wv2_target();
    if (!t) return 0;
    if (codepoint < 0x10000) {
        PostMessageW(t, WM_CHAR, (WPARAM) codepoint, 1);
    } else {
        const unsigned v = codepoint - 0x10000;
        PostMessageW(t, WM_CHAR, (WPARAM) (0xd800 + (v >> 10)), 1);
        PostMessageW(t, WM_CHAR, (WPARAM) (0xdc00 + (v & 0x3ff)), 1);
    }
    return 1;
}

#endif // KN_WV_WV2

// ---------------------------------------------------------------- WKWebView (macOS)
// The same contract as the GTK and WebView2 backends, on WKWebView (ARC; this file is compiled as Objective-C
// on Apple: CMake). The view lives in an invisible host window (alpha 0, never key, ignores the mouse) until
// reveal(). The game UI's frames are WKWebView snapshots (takeSnapshotWithConfiguration:), about ten a second
// while the challenge is shown in the game UI, each converted to the KN_WEBVIEW_INLINE_W x H RGBA frame and
// handed over only when the page has changed. The pointer and keys are NSEvents sent straight to the view.
// The window is an NSWindow above the game, centred on the screen under the pointer. The matchmaker's proxy
// is the data store's (macOS 14+, loopback excluded); the data store is non-persistent.
#if defined(KN_WV_WK)

#define SNAPSHOT_EVERY_MS 100  // the game UI's frame rate while it shows the challenge (about ten a second)
#define SNAPSHOT_STALE_MS 2000 // a snapshot not back by then is given up (the next one replaces it)
#define BRIDGE_HANDLER "knCaptcha" // the script message handler (in the bridge's world)

static long long now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (long long) ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int hidden_host(void) { return W.host != nil; }

static void backend_tick(void) {}

static void wk_activate(void) {
    if (@available(macOS 14.0, *)) [NSApp activate];
    else [NSApp activateIgnoringOtherApps:YES];
}

static NSScreen *wk_screen_under_pointer(void) {
    const NSPoint m = [NSEvent mouseLocation];
    for (NSScreen *s in [NSScreen screens])
        if (NSPointInRect(m, s.frame)) return s;
    return [NSScreen mainScreen];
}

// A snapshot (any size) to the game UI's frame: ARGB32 premultiplied (cairo's layout, little endian) at
// KN_WEBVIEW_INLINE_W x H, then straight RGBA8. The frame is new only when the page changed.
static void wk_frame_from_image(NSImage *image) {
    CGImageRef cg = [image CGImageForProposedRect:NULL context:nil hints:nil];
    if (!cg) return;
    const size_t need = (size_t) KN_WEBVIEW_INLINE_W * KN_WEBVIEW_INLINE_H * 4;
    unsigned char *argb = malloc(need), *next = malloc(need);
    if (!argb || !next) {
        free(argb);
        free(next);
        return;
    }
    CGColorSpaceRef cs = CGColorSpaceCreateDeviceRGB();
    CGContextRef ctx = CGBitmapContextCreate(argb, KN_WEBVIEW_INLINE_W, KN_WEBVIEW_INLINE_H, 8, KN_WEBVIEW_INLINE_W * 4, cs,
                                             kCGBitmapByteOrder32Little | kCGImageAlphaPremultipliedFirst);
    CGColorSpaceRelease(cs);
    if (ctx) {
        const CGRect r = CGRectMake(0, 0, KN_WEBVIEW_INLINE_W, KN_WEBVIEW_INLINE_H);
        CGContextClearRect(ctx, r);
        CGContextSetInterpolationQuality(ctx, kCGInterpolationHigh);
        CGContextDrawImage(ctx, r, cg); // (the image's top row goes to row 0 of the bitmap)
        CGContextRelease(ctx);
        kn_webview_rgba_from_argb32(argb, KN_WEBVIEW_INLINE_W * 4, KN_WEBVIEW_INLINE_W, KN_WEBVIEW_INLINE_H, next);
        if (!W.px || W.px_cap != need || memcmp(next, W.px, need) != 0) {
            free(W.px);
            W.px = next;
            next = NULL;
            W.px_cap = need;
            W.px_w = KN_WEBVIEW_INLINE_W;
            W.px_h = KN_WEBVIEW_INLINE_H;
            W.dirty = 1;
        }
    }
    free(argb);
    free(next);
}

static void wk_snapshot_now(void) {
    if (!W.view) return;
    const long long t = now_ms();
    W.snap_last_ms = t;
    W.snap_started_ms = t;
    W.snap_inflight = 1;
    const unsigned gen = ++W.snap_gen;
    const int session = W.session;
    WKSnapshotConfiguration *c = [[WKSnapshotConfiguration alloc] init];
    c.snapshotWidth = @(KN_WEBVIEW_INLINE_W); // points: the frame is downscaled to the contract size
    [W.view takeSnapshotWithConfiguration:c
                        completionHandler:^(NSImage *image, NSError *error) {
                            if (session != W.session || gen != W.snap_gen) return; // an older one: dropped
                            W.snap_inflight = 0;
                            if (image) wk_frame_from_image(image);
                            else if (error) printf("[webview] a frame could not be taken: %s\n", error.localizedDescription.UTF8String);
                        }];
}

// The view and its handlers go; the host and the window are the caller's to drop (see below).
static void wk_drop_view(void) {
    W.session++; // late callbacks of this challenge are ignored from now on
    WKWebView *v = W.view;
    WKUserContentController *ucc = W.ucc;
    W.view = nil;
    W.ucc = nil;
    W.delegate = nil;
    if (v) {
        [v stopLoading];
        v.navigationDelegate = nil;
        v.UIDelegate = nil;
        [v removeFromSuperview];
    }
    if (ucc) {
        [ucc removeScriptMessageHandlerForName:[NSString stringWithUTF8String:BRIDGE_HANDLER] contentWorld:W.world];
        [ucc removeAllUserScripts];
    }
    W.world = nil;
    W.snap_inflight = 0;
}

static void wk_drop_host(void) {
    NSWindow *h = W.host;
    W.host = nil;
    if (h) {
        h.contentView = nil;
        [h orderOut:nil];
        [h close];
    }
}

static void wk_drop_window(void) {
    NSWindow *w = W.window;
    W.window = nil;
    if (w) {
        w.delegate = nil;
        [w orderOut:nil];
        [w close];
    }
}

// A load that failed: the navigation we refused is not one, and only the krunker.io origin's failures count.
static void wk_failed(WKWebView *v, NSError *error) {
    if (!error) return;
    if ([error.domain isEqualToString:NSURLErrorDomain] && error.code == NSURLErrorCancelled) return;
    if ([error.domain isEqualToString:@"WebKitErrorDomain"] && error.code == 102) return; // refused by policy
    NSURL *failing = error.userInfo[NSURLErrorFailingURLErrorKey];
    if (!same_origin((failing ? failing.absoluteString : v.URL.absoluteString).UTF8String)) return;
    if ([error.domain isEqualToString:NSURLErrorDomain] && error.code <= -1200 && error.code >= -1204) { // the certificate
        char detail[48];
        snprintf(detail, sizeof(detail), "NSURLError %ld", (long) error.code);
        tls_refused(detail);
        return;
    }
    load_failed(error.localizedDescription.UTF8String ? error.localizedDescription.UTF8String : "error");
}

static void wk_set_proxy(WKWebsiteDataStore *ds, const char *proxy) {
    if (@available(macOS 14.0, *)) {
        NSURLComponents *c = [NSURLComponents componentsWithString:[NSString stringWithUTF8String:proxy]];
        const BOOL http = [c.scheme isEqualToString:@"http"], socks = [c.scheme isEqualToString:@"socks5"];
        if (!c || !c.host || !c.port || (!http && !socks)) {
            printf("[webview] the proxy is not an http or socks5 proxy with a port: the challenge goes direct\n");
            return;
        }
        NSString *port = [NSString stringWithFormat:@"%@", c.port];
        nw_endpoint_t endpoint = nw_endpoint_create_host(c.host.UTF8String, port.UTF8String);
        nw_proxy_config_t config = http ? nw_proxy_config_create_http_connect(endpoint, NULL) : nw_proxy_config_create_socksv5(endpoint);
        if (c.user) nw_proxy_config_set_username_and_password(config, c.user.UTF8String, c.password ? c.password.UTF8String : NULL);
        nw_proxy_config_add_excluded_domain(config, "localhost");
        nw_proxy_config_add_excluded_domain(config, "127.0.0.1");
        ds.proxyConfigurations = @[ config ];
    } else {
        printf("[webview] the challenge's proxy needs macOS 14: the challenge goes direct\n");
    }
}

@implementation KNCaptchaWebDelegate

- (void)userContentController:(WKUserContentController *)controller didReceiveScriptMessage:(WKScriptMessage *)message {
    (void) controller;
    if (self->session != W.session || !W.view || ![message.body isKindOfClass:[NSString class]]) return;
    widget_message(((NSString *) message.body).UTF8String, same_origin(W.view.URL.absoluteString.UTF8String));
}

- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationAction:(WKNavigationAction *)action
                    decisionHandler:(void (^)(WKNavigationActionPolicy))handler {
    (void) webView;
    const int ok = self->session == W.session && navigation_ok(action.request.URL.absoluteString.UTF8String);
    handler(ok ? WKNavigationActionPolicyAllow : WKNavigationActionPolicyCancel);
}

- (void)webView:(WKWebView *)webView
    decidePolicyForNavigationResponse:(WKNavigationResponse *)response
                      decisionHandler:(void (^)(WKNavigationResponsePolicy))handler {
    (void) webView;
    if (self->session == W.session && response.isForMainFrame && [response.response isKindOfClass:[NSHTTPURLResponse class]])
        W.main_status = (unsigned) ((NSHTTPURLResponse *) response.response).statusCode;
    handler(WKNavigationResponsePolicyAllow);
}

- (void)webView:(WKWebView *)webView didFinishNavigation:(WKNavigation *)navigation {
    (void) navigation;
    if (self->session == W.session && same_origin(webView.URL.absoluteString.UTF8String)) http_status(W.main_status);
}

- (void)webView:(WKWebView *)webView didFailNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void) navigation;
    if (self->session == W.session) wk_failed(webView, error);
}

- (void)webView:(WKWebView *)webView didFailProvisionalNavigation:(WKNavigation *)navigation withError:(NSError *)error {
    (void) navigation;
    if (self->session == W.session) wk_failed(webView, error);
}

- (void)webViewWebContentProcessDidTerminate:(WKWebView *)webView {
    (void) webView;
    if (self->session == W.session) page_process_stopped("the web content process");
}

// Cloudflare's "Privacy" / "Help" links open windows: refused.
- (WKWebView *)webView:(WKWebView *)webView
    createWebViewWithConfiguration:(WKWebViewConfiguration *)configuration
               forNavigationAction:(WKNavigationAction *)navigationAction
                    windowFeatures:(WKWindowFeatures *)windowFeatures {
    (void) webView;
    (void) configuration;
    (void) navigationAction;
    (void) windowFeatures;
    printf("[webview] a new window was refused\n");
    return nil;
}

// The window closed by the player (not by us): the poll reports it, the view goes with it.
- (void)windowWillClose:(NSNotification *)note {
    if (self->session != W.session || W.window == nil || note.object != W.window) return;
    W.window.delegate = nil;
    W.window = nil;
    if (!W.closing) {
        printf("[webview] the player closed the challenge window\n");
        W.ev_closed = 1;
    }
    wk_drop_view();
}

@end

// The challenge's view in the game UI (in_ui) or its window: the same reveal decision as the other backends.
static void reveal(const char *why) {
    if (W.inline_shown || W.window || !W.view || !W.host) return;
    if (kn_webview_reveal_target(W.in_ui, W.inline_failed) == KN_WEBVIEW_VIEW_INLINE) {
        W.inline_shown = 1;
        W.shown_ms = now_ms();
        W.frames = 0;
        W.dirty = 0;
        printf("[webview] challenge shown in the game UI (%s): no window\n", why);
        wk_snapshot_now(); // the first frame at once
        return;
    }
    NSRect wa = [wk_screen_under_pointer() visibleFrame];
    const CGFloat w = WINDOW_W < wa.size.width ? WINDOW_W : wa.size.width;
    const CGFloat h = WINDOW_H < wa.size.height ? WINDOW_H : wa.size.height;
    const NSRect r = NSMakeRect(wa.origin.x + (wa.size.width - w) / 2, wa.origin.y + (wa.size.height - h) / 2, w, h);
    NSWindow *win = [[NSWindow alloc] initWithContentRect:r
                                                styleMask:NSWindowStyleMaskTitled | NSWindowStyleMaskClosable | NSWindowStyleMaskMiniaturizable
                                                  backing:NSBackingStoreBuffered
                                                    defer:NO];
    win.title = [NSString stringWithUTF8String:W.title];
    win.releasedWhenClosed = NO;
    win.delegate = W.delegate;
    win.level = NSFloatingWindowLevel; // above the game window
    if (W.in_ui) // the widget alone on the dark page (its transparent background shows this)
        win.backgroundColor = [NSColor colorWithSRGBRed:0.106 green:0.106 blue:0.106 alpha:1.0];
    [W.view removeFromSuperview];
    W.view.frame = win.contentView.bounds;
    W.view.autoresizingMask = NSViewWidthSizable | NSViewHeightSizable;
    [win.contentView addSubview:W.view];
    wk_drop_host();
    W.window = win;
    wk_activate();
    [win makeKeyAndOrderFront:nil];
    [win makeFirstResponder:W.view];
    printf("[webview] challenge window shown (%s) at %d,%d %dx%d\n", why, (int) r.origin.x, (int) r.origin.y, (int) w, (int) h);
}

int kn_webview_available(void) { return !env_off(); }

int kn_webview_captcha_open(const char *url, const char *sitekey, const char *title, const char *proxy, const int in_ui, const int show_now,
                            char *err, const size_t errn) {
    if (err && errn) err[0] = 0;
    if (env_off()) {
        if (err && errn) snprintf(err, errn, "the webview is turned off (KRUNKNATIVE_WEBVIEW=0)");
        return 0;
    }
    if (W.window) { // already up: bring it forward
        wk_activate();
        [W.window makeKeyAndOrderFront:nil];
        return 1;
    }
    if (W.inline_shown) return 1; // shown in the game UI already
    if (W.host) { // running hidden: the player asks to see it (the modal's button)
        W.reveal = "the player asked for it";
        return 1;
    }
    char origin[sizeof(W.origin)];
    const char *resolved = NULL;
    if (!captcha_check(url, sitekey, origin, sizeof(origin), &resolved, err, errn)) return 0;
    NSURL *nsurl = [NSURL URLWithString:[NSString stringWithUTF8String:resolved]];
    if (!nsurl) {
        if (err && errn) snprintf(err, errn, "the challenge URL is not valid");
        return 0;
    }
    [NSApplication sharedApplication]; // (GLFW has made it for the game; a test may not have)
    session_reset(in_ui, origin, title);
    // the scripts first: nothing is created when they cannot be built
    const char *shown_title = title && title[0] ? title : "Security Challenge";
    char *bridge = bridge_script();
    char *js = bridge ? user_script(sitekey, shown_title, W.in_ui) : NULL;
    if (!bridge || !js) {
        free(bridge);
        free(js);
        if (err && errn) snprintf(err, errn, "out of memory");
        return 0;
    }

    wk_drop_view();
    wk_drop_host();
    wk_drop_window();
    WKWebsiteDataStore *ds = [WKWebsiteDataStore nonPersistentDataStore]; // nothing of it reaches the disk
    if (proxy && proxy[0]) wk_set_proxy(ds, proxy);
    WKWebViewConfiguration *cfg = [[WKWebViewConfiguration alloc] init];
    cfg.websiteDataStore = ds;
    cfg.preferences.javaScriptCanOpenWindowsAutomatically = NO;
    W.ucc = [[WKUserContentController alloc] init];
    cfg.userContentController = W.ucc;
    W.world = [WKContentWorld worldWithName:[NSString stringWithUTF8String:BRIDGE_WORLD]];
    W.delegate = [[KNCaptchaWebDelegate alloc] init];
    W.delegate->session = W.session;
    [W.ucc addScriptMessageHandler:W.delegate contentWorld:W.world name:[NSString stringWithUTF8String:BRIDGE_HANDLER]];
    WKUserScript *bridge_js = [[WKUserScript alloc] initWithSource:[NSString stringWithUTF8String:bridge]
                                                     injectionTime:WKUserScriptInjectionTimeAtDocumentStart
                                                  forMainFrameOnly:YES
                                                    inContentWorld:W.world];
    free(bridge);
    [W.ucc addUserScript:bridge_js];
    WKUserScript *page_js = [[WKUserScript alloc] initWithSource:[NSString stringWithUTF8String:js]
                                                   injectionTime:WKUserScriptInjectionTimeAtDocumentEnd
                                                forMainFrameOnly:YES
                                                  inContentWorld:[WKContentWorld pageWorld]]; // the main world: Turnstile's
    free(js);
    [W.ucc addUserScript:page_js];

    W.view = [[WKWebView alloc] initWithFrame:NSMakeRect(0, 0, KN_WEBVIEW_INLINE_W, KN_WEBVIEW_INLINE_H) configuration:cfg];
    W.view.navigationDelegate = W.delegate;
    W.view.UIDelegate = W.delegate;
    if (W.in_ui) { // the page is drawn over the game's modal: nothing behind the widget but the transparent page
        W.view.underPageBackgroundColor = [NSColor clearColor];
        @try {
            [W.view setValue:@NO forKey:@"drawsBackground"];
        } @catch (NSException *e) {
            (void) e;
        }
    }
    // hidden first (see the top of this file): an invisible host window, never key, that ignores the mouse
    W.host = [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, KN_WEBVIEW_INLINE_W, KN_WEBVIEW_INLINE_H)
                                         styleMask:NSWindowStyleMaskBorderless
                                           backing:NSBackingStoreBuffered
                                             defer:NO];
    W.host.releasedWhenClosed = NO;
    W.host.backgroundColor = [NSColor clearColor];
    W.host.opaque = NO;
    W.host.hasShadow = NO;
    W.host.alphaValue = 0.0;
    W.host.ignoresMouseEvents = YES;
    W.host.level = NSNormalWindowLevel - 1;
    [W.host setContentView:W.view];
    [W.host orderBack:nil];
    [W.host makeFirstResponder:W.view];
    if (show_now) W.reveal = "the player asked for it"; // shown by the next poll (it reports KN_WEBVIEW_SHOWN)

    printf("[webview] challenge running hidden: %s (%s)%s; shown only if it needs the player\n", resolved, W.origin,
           proxy && proxy[0] ? ", through the client's proxy" : "");
    [W.view loadRequest:[NSURLRequest requestWithURL:nsurl]];
    kn_webview_pump();
    return 1;
}

void kn_webview_captcha_close(void) {
    if (W.window || W.host || W.view) {
        const int shown = W.window != nil;
        W.closing = 1;
        wk_drop_view();
        wk_drop_host();
        wk_drop_window();
        W.closing = 0;
        W.reveal = NULL;
        printf("[webview] challenge window closed%s\n", shown ? "" : " (it was never shown)");
    }
    if (W.inline_shown) printf("[webview] challenge closed (shown in the game UI)\n");
    session_end();
}

// The toolkit's events (the snapshots' completions and WebKit's callbacks) and, while the challenge is shown in
// the game UI, the next snapshot once the last one came back (or was given up).
void kn_webview_pump(void) {
    if (!W.view) return;
    for (int i = 0; i < 256; i++)
        if (CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0, true) != kCFRunLoopRunHandledSource) break;
    if (!W.view || !W.inline_shown) return;
    const long long t = now_ms();
    if (W.snap_inflight && t - W.snap_started_ms >= SNAPSHOT_STALE_MS) W.snap_inflight = 0;
    if (!W.snap_inflight && t - W.snap_last_ms >= SNAPSHOT_EVERY_MS) wk_snapshot_now();
}

int kn_webview_captcha_frame(const unsigned char **rgba, int *w, int *h) {
    if (!W.inline_shown || !W.dirty || !W.px) return 0;
    W.dirty = 0;
    W.frames++;
    *rgba = W.px;
    *w = W.px_w;
    *h = W.px_h;
    return 1;
}

// A mouse event straight to the view (its window is the host's: the event's window number). kind: the NSEvent type.
static void wk_send_mouse(const NSEventType type, const NSPoint at) {
    const int down = type == NSEventTypeLeftMouseDown || type == NSEventTypeRightMouseDown || type == NSEventTypeOtherMouseDown;
    NSEvent *ev = [NSEvent mouseEventWithType:type
                                     location:at
                                modifierFlags:0
                                    timestamp:[[NSProcessInfo processInfo] systemUptime]
                                 windowNumber:W.host.windowNumber
                                      context:nil
                                  eventNumber:0
                                   clickCount:1
                                     pressure:down ? 1.0 : 0.0];
    switch (type) {
        case NSEventTypeLeftMouseDown: [W.view mouseDown:ev]; break;
        case NSEventTypeLeftMouseUp: [W.view mouseUp:ev]; break;
        case NSEventTypeLeftMouseDragged: [W.view mouseDragged:ev]; break;
        case NSEventTypeRightMouseDown: [W.view rightMouseDown:ev]; break;
        case NSEventTypeRightMouseUp: [W.view rightMouseUp:ev]; break;
        case NSEventTypeRightMouseDragged: [W.view rightMouseDragged:ev]; break;
        case NSEventTypeOtherMouseDown: [W.view otherMouseDown:ev]; break;
        case NSEventTypeOtherMouseUp: [W.view otherMouseUp:ev]; break;
        case NSEventTypeOtherMouseDragged: [W.view otherMouseDragged:ev]; break;
        default: [W.view mouseMoved:ev]; break;
    }
}

// A key press or release straight to the view (chars: the characters it types, press only).
static void wk_send_key(NSString *chars, const unsigned short code, const int pressed) {
    NSEvent *ev = [NSEvent keyEventWithType:pressed ? NSEventTypeKeyDown : NSEventTypeKeyUp
                                   location:NSZeroPoint
                              modifierFlags:0
                                  timestamp:[[NSProcessInfo processInfo] systemUptime]
                               windowNumber:W.host.windowNumber
                                    context:nil
                                 characters:chars
                charactersIgnoringModifiers:chars
                                  isARepeat:NO
                                    keyCode:code];
    if (!ev) return;
    if (pressed) [W.view keyDown:ev];
    else [W.view keyUp:ev];
}

// The view's own coordinates (flipped: top-left), as the caller gives them. The buttons are W.buttons' masks.
int kn_webview_captcha_pointer(const int kind, const double x, const double y, const int button) {
    if (!W.inline_shown || !W.view || !W.host) return 0;
    const int bit = button >= 1 && button <= 3 ? 1 << (button - 1) : 0;
    const NSPoint at = [W.view convertPoint:NSMakePoint((CGFloat) x, (CGFloat) y) toView:nil];
    if (kind == KN_WEBVIEW_POINTER_MOVE) {
        NSEventType t = NSEventTypeMouseMoved;
        if (W.buttons & 1) t = NSEventTypeLeftMouseDragged;
        else if (W.buttons & 4) t = NSEventTypeRightMouseDragged;
        else if (W.buttons & 2) t = NSEventTypeOtherMouseDragged;
        wk_send_mouse(t, at);
        return 1;
    }
    const int press = kind == KN_WEBVIEW_POINTER_PRESS;
    NSEventType t;
    if (button == 3) t = press ? NSEventTypeRightMouseDown : NSEventTypeRightMouseUp;
    else if (button == 2) t = press ? NSEventTypeOtherMouseDown : NSEventTypeOtherMouseUp;
    else t = press ? NSEventTypeLeftMouseDown : NSEventTypeLeftMouseUp;
    if (press) W.buttons |= bit;
    else W.buttons &= ~bit;
    wk_send_mouse(t, at);
    return 1;
}

int kn_webview_captcha_key(const int key, const int pressed) {
    if (!W.inline_shown || !W.view || !W.host) return 0;
    if (pressed == 2) return 1; // a repeat: taken, not sent (the widget takes the press)
    NSString *chars = nil;
    unsigned short code = 0;
    switch (key) {
        case KN_WEBVIEW_KEY_TAB: chars = @"\t"; code = 48; break;
        case KN_WEBVIEW_KEY_ENTER: chars = @"\r"; code = 36; break;
        case KN_WEBVIEW_KEY_SPACE: chars = @" "; code = 49; break;
        case KN_WEBVIEW_KEY_BACKSPACE: chars = @"\x7f"; code = 51; break;
        case KN_WEBVIEW_KEY_DELETE: chars = [NSString stringWithFormat:@"%C", (unichar) NSDeleteFunctionKey]; code = 117; break;
        case KN_WEBVIEW_KEY_LEFT: chars = [NSString stringWithFormat:@"%C", (unichar) NSLeftArrowFunctionKey]; code = 123; break;
        case KN_WEBVIEW_KEY_RIGHT: chars = [NSString stringWithFormat:@"%C", (unichar) NSRightArrowFunctionKey]; code = 124; break;
        case KN_WEBVIEW_KEY_UP: chars = [NSString stringWithFormat:@"%C", (unichar) NSUpArrowFunctionKey]; code = 126; break;
        case KN_WEBVIEW_KEY_DOWN: chars = [NSString stringWithFormat:@"%C", (unichar) NSDownArrowFunctionKey]; code = 125; break;
        case KN_WEBVIEW_KEY_HOME: chars = [NSString stringWithFormat:@"%C", (unichar) NSHomeFunctionKey]; code = 115; break;
        case KN_WEBVIEW_KEY_END: chars = [NSString stringWithFormat:@"%C", (unichar) NSEndFunctionKey]; code = 119; break;
        default: return 0;
    }
    wk_send_key(chars, code, pressed == 1);
    return 1;
}

int kn_webview_captcha_char(const unsigned codepoint) {
    if (!W.inline_shown || !W.view || !W.host || codepoint <= 0x20 || codepoint == 0x7f) return 0; // (space is a key)
    const uint32_t cp = codepoint;
    NSString *s = [[NSString alloc] initWithBytes:&cp length:4 encoding:NSUTF32LittleEndianStringEncoding];
    if (!s || s.length == 0) return 0;
    wk_send_key(s, 0, 1); // the press types the text; the release carries none
    wk_send_key(@"", 0, 0);
    return 1;
}

#endif // KN_WV_WK

// ---------------------------------------------------------------- no backend in this build
#if !defined(KN_WV_GTK) && !defined(KN_WV_WV2) && !defined(KN_WV_WK)

int kn_webview_available(void) { return 0; }

int kn_webview_captcha_open(const char *url, const char *sitekey, const char *title, const char *proxy, const int in_ui, const int show_now,
                            char *err, const size_t errn) {
    (void) url;
    (void) proxy;
    (void) sitekey;
    (void) title;
    (void) in_ui;
    (void) show_now;
    if (err && errn) snprintf(err, errn, "this build has no webview");
    return 0;
}

int kn_webview_captcha_is_open(void) { return 0; }
void kn_webview_captcha_close(void) {}
int kn_webview_captcha_inline(void) { return 0; }
int kn_webview_captcha_windowed(void) { return 0; }
int kn_webview_captcha_frame(const unsigned char **rgba, int *w, int *h) {
    (void) rgba;
    (void) w;
    (void) h;
    return 0;
}
int kn_webview_captcha_pointer(const int kind, const double x, const double y, const int button) {
    (void) kind;
    (void) x;
    (void) y;
    (void) button;
    return 0;
}
int kn_webview_captcha_key(const int key, const int pressed) {
    (void) key;
    (void) pressed;
    return 0;
}
int kn_webview_captcha_char(const unsigned codepoint) {
    (void) codepoint;
    return 0;
}
void kn_webview_pump(void) {}

int kn_webview_captcha_poll(char *token, const size_t n, char *err, const size_t errn) {
    (void) token;
    (void) n;
    if (err && errn) err[0] = 0;
    return 0;
}

#endif
