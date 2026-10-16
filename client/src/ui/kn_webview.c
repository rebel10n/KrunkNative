// The challenge window of kn_webview.h: WebKitGTK (webkit2gtk-4.1) on Linux when built with
// KN_WEBVIEW, a stub everywhere else. CMake builds this file into its own library (kn_webview) with
// WebKitGTK's include paths, so GTK's and FreeType's system headers never reach the rest of the client.
//
// What the window does (and does not):
//   - runs hidden first: the view lives in a GtkOffscreenWindow (no X / Wayland window at all, yet
//     mapped and visible to WebKit, so the page and Cloudflare's widget run normally). A challenge that
//     passes by itself (Turnstile's non-interactive / managed check) never shows anything. Only when
//     the player is needed is the view moved into a real window, created centred on the work area of
//     the monitor under the pointer and kept above the game (reveal()): Turnstile's
//     before-interactive-callback (interactiveBegin), a widget error / expiry / timeout (the widget's
//     own "Try again" is in the window), no token after KN_WEBVIEW_REVEAL_MS (KRUNKNATIVE_WEBVIEW_REVEAL_MS;
//     Cloudflare's interstitial and an HTTP error page are not taken as "needs the player": they may
//     pass by themselves), or the player asking for it (kn_webview_captcha_open with show_now);
//   - navigates to a REAL krunker.io URL (https only; plain http only to a loopback host, for the
//     tests' stub page) in an ephemeral web context (no cookies or storage on disk);
//   - injects one user script, which does nothing unless the top document's origin is that URL's; it
//     draws a card over the page and renders Cloudflare's widget with krunker.io's sitekey, loading
//     challenges.cloudflare.com's api.js like the live client (or using the page's own `turnstile`);
//   - the widget's callbacks hand "kind:value" strings to a second user script in an isolated script
//     world (BRIDGE_WORLD; through a DOM attribute + event on the top document), which alone can post to
//     the "knCaptcha" script-message handler: frames of other origins (and the page's own scripts'
//     window.webkit) cannot reach it. Only the first token is taken, only while the main frame is on
//     the expected origin; it goes to the caller (krunker_captcha_ui.c) once and is wiped here. It is
//     never logged;
//   - main-frame navigations away from the origin and every new window are refused;
//   - it never clicks, types, solves, rewrites requests or touches the challenge's scripts or frames.
//   - the game UI (in_ui, the default on Linux): the page is only the widget on a transparent page
//     (no card: the game's modal has the heading and Cancel). The view never leaves its offscreen host:
//     the caller takes its frames (kn_webview_captcha_frame: each paint, cairo's ARGB32 converted to RGBA)
//     and forwards its pointer and keys (GDK events to the offscreen window, GTK routes them to the view).
//     When the game UI draws no frame within INLINE_STALL_MS, the challenge's window is shown instead
//     (the fallback; the window's background is dark, the widget centred). KRUNKNATIVE_CAPTCHA_WINDOW=1
//     keeps the window path only.
#include <kn_webview.h>
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

#if defined(KN_WEBVIEW) && defined(__linux__) && !defined(__EMSCRIPTEN__)
#include <gtk/gtk.h>
#include <webkit2/webkit2.h>

#define KN_WEBVIEW_TOKEN_MAX 4096 // KRUNKER_CAPTCHA_TOKEN_MAX
#define TURNSTILE_API "https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit"
#define TURNSTILE_ORIGIN "https://challenges.cloudflare.com"
#define BRIDGE_WORLD "knCaptcha"      // the isolated script world holding the message handler
#define BRIDGE_ATTR "data-kn-captcha" // on <html>: the message, read and removed at once by the bridge
#define BRIDGE_EVENT "kn-captcha"
#define WINDOW_W 520
#define WINDOW_H 600

static struct {
    int gtk;            // 0 not tried, 1 up, -1 failed
    char gtk_err[160];
    GtkWidget *offscreen; // the hidden host of the view (GtkOffscreenWindow) until reveal(), else NULL
    GtkWidget *window;    // the real window, once revealed
    WebKitWebView *view;
    char title[200];      // the window's title, for reveal()
    gint64 opened_us;     // g_get_monotonic_time() of the open: KN_WEBVIEW_REVEAL_MS counts from it
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
    // the game UI (in_ui): the view stays in its offscreen host and the caller draws it
    int in_ui;          // opened for the game UI
    int inline_shown;   // shown in the game UI (no window)
    int inline_failed;  // the game UI could not draw it: its window instead (the stall watchdog)
    int dirty;          // the offscreen view was painted since the last frame taken
    int frames;         // frames taken while shown in the game UI
    gint64 shown_us;    // when it was shown in the game UI (the watchdog)
    unsigned buttons;   // the GDK button masks the forwarded pointer holds down
    unsigned char *px;  // the last frame: straight RGBA8, row 0 on top
    size_t px_cap;
    int px_w, px_h;
} W;

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
    if (!g_ascii_strncasecmp(url, "https://", 8)) skip = 8;
    else if (!g_ascii_strncasecmp(url, "http://", 7)) skip = 7;
    else return 0;
    size_t end = skip;
    while (url[end] && url[end] != '/' && url[end] != '?' && url[end] != '#') end++;
    if (end == skip || end >= n) return 0;
    for (size_t i = 0; i < end; i++) {
        const char c = url[i];
        if (!(g_ascii_isalnum(c) || c == '.' || c == '-' || c == ':' || c == '/' || c == '[' || c == ']')) return 0;
        out[i] = g_ascii_tolower(c);
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
static void js_string(GString *out, const char *s) {
    g_string_append_c(out, '"');
    for (const unsigned char *p = (const unsigned char *) s; p && *p; p++) {
        if (*p == '"' || *p == '\\') g_string_append_printf(out, "\\%c", *p);
        else if (*p < 0x20 || *p == '<' || *p == '>') g_string_append_printf(out, "\\u%04x", *p);
        else if (p[0] == 0xe2 && p[1] == 0x80 && (p[2] == 0xa8 || p[2] == 0xa9)) { // U+2028 / U+2029
            g_string_append_printf(out, "\\u202%c", p[2] == 0xa8 ? '8' : '9');
            p += 2;
        } else g_string_append_c(out, (char) *p);
    }
    g_string_append_c(out, '"');
}

// The user script: the widget on the krunker.io page. In a window (in_ui 0): a card over a dark page
// with a heading, a hint and the widget. In the game UI (in_ui 1): only the widget, centred on the
// transparent page (the page's own text hidden): the game's modal has the heading, the text and Cancel.
static char *user_script(const char *sitekey, const char *title, const int in_ui) {
    GString *s = g_string_new("(function () {\n'use strict';\nvar ORIGIN = ");
    js_string(s, W.origin);
    g_string_append(s, ", SITEKEY = ");
    js_string(s, sitekey);
    g_string_append(s, ", TITLE = ");
    js_string(s, title);
    g_string_append(s, ", API = ");
    js_string(s, TURNSTILE_API);
    g_string_append(
        s,
        ";\n"
        "if (window.top !== window || location.origin !== ORIGIN || window.__knCaptcha) return;\n"
        "window.__knCaptcha = 1;\n"
        "var post = function (m) { var d = document.documentElement; d.setAttribute('" BRIDGE_ATTR "', m);"
        " document.dispatchEvent(new Event('" BRIDGE_EVENT "')); d.removeAttribute('" BRIDGE_ATTR "'); };\n"
        "var el = function (tag, css, text) { var e = document.createElement(tag); e.style.cssText = css;"
        " if (text) e.textContent = text; return e; };\n"
        "var shade, holder, status, retry;\n");
    if (in_ui) {
        g_string_append(
            s,
            "shade = el('div', 'position:fixed;top:50%;left:50%;transform:translate(-50%,-50%);z-index:2147483646;width:340px;"
            "display:flex;flex-direction:column;align-items:center;margin:0;background:transparent;font:15px/1.4 sans-serif;"
            "color:#555');\n"
            "holder = el('div', 'height:65px;width:100%;display:flex;align-items:center;justify-content:center');\n"
            "status = el('div', 'color:#555;font-size:14px;min-height:18px;margin-top:6px');\n"
            "retry = el('button', 'display:none;margin:8px auto 0;padding:6px 16px;font-size:14px;cursor:pointer', 'Try again');\n"
            "shade.appendChild(holder); shade.appendChild(status); shade.appendChild(retry);\n");
    } else {
        g_string_append(
            s,
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
    g_string_append(s, "if (window._cf_chl_opt) { post('interstitial:'); return; }\n"); // Cloudflare's own check of the visit: never covered
    if (in_ui) { // Cloudflare's own page (above) stays as it is; the robots.txt text does not show
        g_string_append(s,
                        "if (document.body) { document.body.style.display = 'none'; document.body.style.background = 'transparent'; }\n"
                        "document.documentElement.style.background = 'transparent';\n"
                        "document.documentElement.appendChild(shade);\n");
    } else {
        g_string_append(s, "(document.body || document.documentElement).appendChild(shade);\n");
    }
    g_string_append(
        s,
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
    return g_string_free(s, FALSE);
}

// The bridge, in the isolated world BRIDGE_WORLD of the top frame: the only script that can post to
// the handler. It reads the main-world script's message off <html> (the DOM is shared between worlds).
static char *bridge_script(void) {
    GString *s = g_string_new("(function () {\n'use strict';\nvar ORIGIN = ");
    js_string(s, W.origin);
    g_string_append(s, ";\n"
                       "if (window.top !== window || location.origin !== ORIGIN) return;\n"
                       "var h = window.webkit && window.webkit.messageHandlers && window.webkit.messageHandlers.knCaptcha;\n"
                       "if (!h) return;\n"
                       "document.addEventListener('" BRIDGE_EVENT "', function () {\n"
                       "  var d = document.documentElement, m = d.getAttribute('" BRIDGE_ATTR "');\n"
                       "  d.removeAttribute('" BRIDGE_ATTR "');\n"
                       "  if (typeof m === 'string' && m.length <= 8192) h.postMessage(m);\n"
                       "}, true);\n"
                       "})();\n");
    return g_string_free(s, FALSE);
}

// ---------------------------------------------------------------- signals

static void on_message(WebKitUserContentManager *m, WebKitJavascriptResult *r, gpointer user) {
    (void) m;
    (void) user;
    if (!W.view) return;
    JSCValue *v = webkit_javascript_result_get_js_value(r);
    if (!v || !jsc_value_is_string(v)) return;
    char *s = jsc_value_to_string(v);
    if (!s) return;
    const size_t len = strlen(s);
    if (!same_origin(webkit_web_view_get_uri(W.view))) {
        printf("[webview] a message while the page is not on %s: ignored\n", W.origin);
    } else if (!strncmp(s, "token:", 6)) {
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
            if (!g_ascii_isalnum(*c)) *c = '_';
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
    }
    wipe(s, len);
    g_free(s);
    // (the window is closed by the next kn_webview_captcha_poll, outside this signal)
}

static int nav_allowed(const char *uri) {
    if (!uri) return 0;
    if (!strncmp(uri, "about:", 6) || !strncmp(uri, "data:", 5) || !strncmp(uri, "blob:", 5)) return 1;
    char o[sizeof(W.origin)];
    if (!origin_of(uri, o, sizeof(o))) return 0;
    return !strcmp(o, W.origin) || !strcmp(o, TURNSTILE_ORIGIN);
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
        if (!nav_allowed(uri)) {
            char o[sizeof(W.origin)];
            printf("[webview] a navigation to %s was refused\n", origin_of(uri, o, sizeof(o)) ? o : "(another scheme)");
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
    printf("[webview] %s did not load: %s\n", W.origin, error ? error->message : "?");
    char text[300];
    snprintf(text, sizeof(text), "%s did not load (%s).", W.origin, error ? error->message : "error");
    set_msg(text);
    W.ev_failed = 1;
    return FALSE;
}

// krunker.io answers some networks with an error page ("ACCESS DENIED", 403): said in the modal (the
// widget still works on it, the page being krunker.io's; the matchmaker decides).
static void on_load_changed(WebKitWebView *view, const WebKitLoadEvent ev, gpointer user) {
    (void) user;
    if (ev != WEBKIT_LOAD_FINISHED || !same_origin(webkit_web_view_get_uri(view))) return;
    WebKitWebResource *res = webkit_web_view_get_main_resource(view);
    WebKitURIResponse *resp = res ? webkit_web_resource_get_response(res) : NULL;
    const unsigned status = resp ? webkit_uri_response_get_status_code(resp) : 0;
    if (status < 400) return;
    W.http_err = status;
    printf("[webview] %s answered HTTP %u\n", W.origin, status);
    char text[300];
    snprintf(text, sizeof(text), "krunker.io answered HTTP %u: it blocks some networks"
                                 " (Settings > Client > Network > Proxy).", status);
    set_msg(text);
    W.ev_status = 1;
}

static gboolean on_tls_error(WebKitWebView *view, gchar *uri, GTlsCertificate *cert, GTlsCertificateFlags errors, gpointer user) {
    (void) view;
    (void) uri;
    (void) cert;
    (void) user;
    printf("[webview] %s: the TLS certificate was refused (flags 0x%x)\n", W.origin, (unsigned) errors);
    set_msg("krunker.io's certificate was not valid for the challenge.");
    W.ev_failed = 1;
    return FALSE; // never bypassed: WebKit shows its error
}

static void on_terminated(WebKitWebView *view, WebKitWebProcessTerminationReason reason, gpointer user) {
    (void) view;
    (void) user;
    printf("[webview] the web process ended (reason %d)\n", (int) reason);
    set_msg("The challenge's web process stopped.");
    W.ev_failed = 1;
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
        W.shown_us = g_get_monotonic_time();
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

static int env_off(void) {
    const char *e = getenv("KRUNKNATIVE_WEBVIEW");
    return e && (!strcmp(e, "0") || !g_ascii_strcasecmp(e, "off"));
}

// KRUNKNATIVE_CAPTCHA_WINDOW=1: the challenge's own window, never the game UI (a test, or a player's choice).
static int window_forced(void) {
    const char *e = getenv("KRUNKNATIVE_CAPTCHA_WINDOW");
    return e && !strcmp(e, "1");
}

int kn_webview_available(void) { return !env_off() && W.gtk >= 0; }

static int toolkit_up(void) {
    if (W.gtk) return W.gtk > 0;
    // GTK must not touch the process locale (the game parses and prints "C" numbers)
    gtk_disable_setlocale();
    // The challenge page needs no GPU. WebKit's web process (Skia GPU raster, DMA-BUF renderer)
    // segfaulted in libnvidia-eglcore while tearing its GL context down (GrDirectContext dtor),
    // so render it on the CPU. Inherited by the web processes; an explicit user value wins.
    setenv("WEBKIT_SKIA_ENABLE_CPU_RENDERING", "1", 0);
    setenv("WEBKIT_DISABLE_DMABUF_RENDERER", "1", 0);
    if (!gtk_init_check(NULL, NULL)) {
        W.gtk = -1;
        snprintf(W.gtk_err, sizeof(W.gtk_err), "GTK found no display for the challenge window");
        printf("[webview] %s\n", W.gtk_err);
        return 0;
    }
    W.gtk = 1;
    printf("[webview] WebKitGTK %u.%u.%u\n", webkit_get_major_version(), webkit_get_minor_version(), webkit_get_micro_version());
    return 1;
}

int kn_webview_captcha_open(const char *url, const char *sitekey, const char *title, const char *proxy, const int in_ui, const int show_now,
                            char *err, const size_t errn) {
    if (err && errn) err[0] = 0;
    if (env_off()) {
        if (err) snprintf(err, errn, "the webview is turned off (KRUNKNATIVE_WEBVIEW=0)");
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
    if (!url || !url[0]) {
        const char *e = getenv("KRUNKNATIVE_KRUNKER_CAPTCHA_URL");
        url = e && e[0] ? e : KN_WEBVIEW_CAPTCHA_URL;
    }
    char origin[sizeof(W.origin)];
    if (!origin_of(url, origin, sizeof(origin)) || (strncmp(origin, "https://", 8) && !is_loopback_origin(origin))) {
        if (err) snprintf(err, errn, "the challenge URL must be https:// (or http:// on this computer)");
        return 0;
    }
    for (const char *k = sitekey; k && *k; k++)
        if (!g_ascii_isalnum(*k) && *k != '_' && *k != '-') sitekey = NULL;
    if (!sitekey || !sitekey[0]) {
        if (err) snprintf(err, errn, "no Turnstile sitekey");
        return 0;
    }
    if (!toolkit_up()) {
        if (err) snprintf(err, errn, "%s", W.gtk_err);
        return 0;
    }
    snprintf(W.origin, sizeof(W.origin), "%s", origin);
    W.in_ui = in_ui && !window_forced();
    W.inline_shown = W.inline_failed = W.dirty = W.frames = 0;
    W.buttons = 0;
    W.taken = W.closing = 0;
    W.http_err = 0;
    W.ev_token = W.ev_failed = W.ev_closed = W.ev_status = 0;
    W.reveal = NULL;
    wipe(W.token, sizeof(W.token));

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
    char *js = bridge_script();
    WebKitUserScript *script = webkit_user_script_new_for_world(js, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_START,
                                                                BRIDGE_WORLD, NULL, NULL);
    g_free(js);
    webkit_user_content_manager_add_script(ucm, script);
    webkit_user_script_unref(script);
    js = user_script(sitekey, title && title[0] ? title : "Security Challenge", W.in_ui); // the main world: Turnstile's
    script = webkit_user_script_new(js, WEBKIT_USER_CONTENT_INJECT_TOP_FRAME, WEBKIT_USER_SCRIPT_INJECT_AT_DOCUMENT_END, NULL, NULL);
    g_free(js);
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
    snprintf(W.title, sizeof(W.title), "%s - krunker.io", title && title[0] ? title : "Security Challenge");
    W.offscreen = gtk_offscreen_window_new();
    g_signal_connect_after(W.offscreen, "draw", G_CALLBACK(on_draw), NULL); // a new frame for the game UI
    gtk_widget_set_size_request(GTK_WIDGET(W.view), W.in_ui ? KN_WEBVIEW_INLINE_W : WINDOW_W, W.in_ui ? KN_WEBVIEW_INLINE_H : WINDOW_H);
    gtk_container_add(GTK_CONTAINER(W.offscreen), GTK_WIDGET(W.view));
    gtk_widget_show_all(W.offscreen);
    gtk_window_set_focus(GTK_WINDOW(W.offscreen), GTK_WIDGET(W.view)); // the keys the game UI forwards go to the view
    W.opened_us = g_get_monotonic_time();
    const char *rv = getenv("KRUNKNATIVE_WEBVIEW_REVEAL_MS");
    W.reveal_ms = rv && rv[0] ? atoi(rv) : KN_WEBVIEW_REVEAL_DEFAULT_MS;
    if (show_now) W.reveal = "the player asked for it"; // shown by the next poll (it reports KN_WEBVIEW_SHOWN)

    printf("[webview] challenge running hidden: %s (%s)%s; shown only if it needs the player\n", url, W.origin,
           proxy && proxy[0] ? ", through the client's proxy" : "");
    webkit_web_view_load_uri(W.view, url);
    kn_webview_pump();
    return 1;
}

int kn_webview_captcha_is_open(void) { return W.window != NULL || W.offscreen != NULL; }

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
    W.inline_shown = W.inline_failed = W.dirty = W.frames = 0;
    W.buttons = 0;
    free(W.px);
    W.px = NULL;
    W.px_cap = 0;
    W.px_w = W.px_h = 0;
    if (!W.ev_token) wipe(W.token, sizeof(W.token));
}

int kn_webview_captcha_inline(void) { return W.inline_shown; }

int kn_webview_captcha_windowed(void) { return W.window != NULL; }

void kn_webview_pump(void) {
    if (W.gtk <= 0) return;
    for (int i = 0; i < 256 && g_main_context_iteration(NULL, FALSE); i++) {
    }
}

int kn_webview_captcha_poll(char *token, const size_t n, char *err, const size_t errn) {
    kn_webview_pump();
    if (err && errn) err[0] = 0;
    // the game UI drew nothing since it showed the challenge: the challenge's window instead
    if (W.inline_shown && kn_webview_inline_stalled(W.frames, (g_get_monotonic_time() - W.shown_us) / 1000, INLINE_STALL_MS)) {
        printf("[webview] the game UI drew no frame in %d s: the challenge window instead\n", INLINE_STALL_MS / 1000);
        W.inline_shown = 0;
        W.inline_failed = 1;
        set_msg("The challenge is in its own window: the game UI could not draw it.");
        W.reveal = "the game UI drew no frame";
    }
    int shown = 0;
    if (W.offscreen && !W.ev_token && !W.ev_failed) {
        if (!W.reveal && !W.inline_shown && W.reveal_ms >= 0 && g_get_monotonic_time() - W.opened_us >= (gint64) W.reveal_ms * 1000)
            W.reveal = "no token in time";
        if (W.reveal) {
            const int before = W.inline_shown || W.window != NULL;
            reveal(W.reveal);
            W.reveal = NULL;
            shown = !before && (W.inline_shown || W.window != NULL);
        }
    }
    if (shown) { // the player is needed now: the caller shows its prompt (the widget's line rides along)
        if (err) snprintf(err, errn, "%s", W.msg);
        return KN_WEBVIEW_SHOWN;
    }
    if (W.ev_token) {
        W.ev_token = 0;
        const size_t len = strlen(W.token);
        const int ok = token && len < n;
        if (ok) memcpy(token, W.token, len + 1);
        wipe(W.token, sizeof(W.token));
        kn_webview_captcha_close(); // done: the window goes at once
        if (ok) return KN_WEBVIEW_TOKEN;
        if (err) snprintf(err, errn, "the token did not fit");
        return KN_WEBVIEW_FAILED;
    }
    if (W.ev_failed) {
        W.ev_failed = W.ev_status = 0;
        kn_webview_captcha_close();
        if (err) snprintf(err, errn, "%s", W.msg);
        return KN_WEBVIEW_FAILED;
    }
    if (W.ev_closed) {
        W.ev_closed = W.ev_status = 0;
        return KN_WEBVIEW_CLOSED;
    }
    if (W.ev_status) {
        W.ev_status = 0;
        if (err) snprintf(err, errn, "%s", W.msg);
        return KN_WEBVIEW_STATUS;
    }
    return KN_WEBVIEW_NONE;
}

#else // no webview backend in this build

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
