// The Security Challenge of official krunker.io games (client/include/krunker_captcha.h,
// doc/v10/official-captcha.md): the matchmaker's Cloudflare Turnstile, solved by the player, and the game
// socket's ALTCHA proof of work, solved by krunker_altcha.c (nothing is shown for it).
//
// What is shown is the Turnstile widget and nothing else: no box, no text, no button, the way krunker.io does
// it (game.deob.js getCaptchaToken: appearance "interaction-only", rendered into a holder centred on the screen).
// It takes the place of the join's loading spinner (#instructions, hidden while the widget is up, back when the
// request ends). Web: the widget is Cloudflare's own (the script loads on demand). Native with a webview
// (kn_webview.h, KN_WEBVIEW): the challenge runs hidden on a real krunker.io page and the spinner stays while it
// passes by itself; when the player is needed the view's frames are drawn in the same spot (a canvas, the pointer
// and keys forwarded to it). The challenge is never in an OS window.
// A challenge that cannot be completed (no webview, a load, widget or token error, expiry, a timeout) ends the
// request unavailable: the join goes on once without a token, as the live client does, and a refusal then fails
// the join with the generic error. Nothing is shown for a failure, and no text of this file reaches the page.
//
// Tokens: read from the widget / the webview, checked for shape, handed to the requester once, then
// wiped; never logged, never stored.
#include <krunker_captcha.h>
#include "klog.h"
#include <krunker_build_live.h> // the current build's Turnstile site key
#include <kn_webview.h>
#ifndef __EMSCRIPTEN__
#include <http_proxy.h>
#endif
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#endif

#define CAPTCHA_WIDGET_TIMEOUT_MS 120000.0 // the live getCaptchaToken gives up after 120 s (0x1d4c0)
// The holder's content: the Turnstile widget (web), the webview's canvas (native: its frames, see webview_frame)
#ifdef __EMSCRIPTEN__
#define CAPTCHA_CONTENT "<div id=\"knTurnstileHolder\" class=\"kn-captcha-widget\"></div>"
#else
#define CAPTCHA_CONTENT "<canvas id=\"knCaptchaWebview\" class=\"kn-captcha-webview\" style=\"display:none\"></canvas>"
#endif

static struct {
    int asking;
    int attempt, rejected;
    KrunkerCaptchaDone done;
    void *user;
    unsigned gen;        // per request: late widget events of an older one are ignored
    int widget;          // web: the Turnstile widget is the path
    int webview;         // native: the challenge webview (kn_webview.h) is the path
    int frame_seen;      // native: the first frame is drawn (the spinner stays up until then: the loading state)
    int spinner_hidden;  // the join's spinner is hidden by this file, and comes back with it
    int widget_timer;    // html_timer id of the widget's 120 s limit
    int present_pending; // asked before the page was up: shown once it is (krunker_captcha_tick)
    int needs_player;    // the widget is up and waits for the player (the menu's clicks and typing go to it)
    int inline_press;    // native: a press of the pointer went to the webview (its release does too)
    int pow_failed;      // the failed "cpt" is logged once (the socket's outcome and its notice both end here)
} C;

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

// ---------------------------------------------------------------- the holder

// The holder on <body>, made on first use (the page may have been rebuilt: look it up every time). It is the
// challenge's one element: centred on the screen where the join's spinner is, hidden until the widget is shown.
static char *markup(void) {
    StrBuf sb = {0};
    sb_appendf(&sb, "<div id=\"knCaptchaHolder\" class=\"kn-captcha-holder\" style=\"display:none\">" CAPTCHA_CONTENT "</div>");
    return sb_finish(&sb);
}

static HtmlElem holder(void) {
    if (!html_ui_available() || !html_ui_is_ready()) return 0;
    HtmlElem h = html_dom_by_id("knCaptchaHolder");
    if (!h) {
        const HtmlElem body = html_dom_body();
        if (!body) return 0;
        char *html = markup();
        html_dom_insert_html(body, HTML_INSERT_BEFORE_END, html);
        free(html);
        h = html_dom_by_id("knCaptchaHolder");
    }
    return h;
}

static void show(const char *id, const int on) {
    const HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_set_display(e, on ? "" : "none");
}

// The holder (the widget or the webview's canvas) is shown, or hidden.
static void holder_show(const int on) {
    const HtmlElem h = html_dom_by_id("knCaptchaHolder");
    if (h) html_dom_set_display(h, on ? "" : "none");
}

// The join's loading spinner (#instructions: the ring while the game is not yet playable) is the widget's place:
// hidden while the widget is up, back when the request ends (only when this file hid it).
static void loading_ring(const int on) {
    if (!on && !C.spinner_hidden) {
        C.spinner_hidden = 1;
        show("instructions", 0);
    } else if (on && C.spinner_hidden) {
        C.spinner_hidden = 0;
        show("instructions", 1);
    }
}

// The challenge is over on the page: the widget (or the canvas) goes and the spinner comes back.
static void hide_modal(void) {
    holder_show(0);
    show("knCaptchaWebview", 0);
    loading_ring(1);
    C.needs_player = 0;
    C.inline_press = 0;
}

// The widget is up and waits for the player (krunker_captcha_needs_player): the menu's clicks and typing are left
// to it (v10 frees the pointer for its modals, v10.js:219744; menu_ui.c and main.c ask this).
int krunker_captcha_needs_player(void) { return C.asking && C.needs_player; }

// ---------------------------------------------------------------- web: the Turnstile widget

#ifdef __EMSCRIPTEN__
static void widget_event(unsigned gen, int kind, const char *text);

// kinds: 1 token, 2 error (code), 3 expired, 4 interaction timed out, 5 needs interaction, 6 no script
EMSCRIPTEN_KEEPALIVE void krunker_captcha_web_event(const int gen, const int kind, char *text) {
    widget_event((unsigned) gen, kind, text);
    if (text) {
        wipe(text, strlen(text));
        free(text);
    }
}

// Load api.js once (explicit rendering, like the live client) and render into #knTurnstileHolder.
EM_JS(void, kn_js_turnstile_render, (int gen, const char *sitekey_c), {
    var sitekey = UTF8ToString(sitekey_c);
    var post = function(kind, text) { _krunker_captcha_web_event(gen, kind, text == null ? 0 : stringToNewUTF8(String(text))); };
    if (!Module.knTurnstileLoad) {
        Module.knTurnstileLoad = new Promise(function(ok, fail) {
            if (window.turnstile) { ok(); return; }
            var s = document.createElement('script');
            s.src = 'https://challenges.cloudflare.com/turnstile/v0/api.js?render=explicit';
            s.async = true;
            s.onload = function() { ok(); };
            s.onerror = function() { Module.knTurnstileLoad = null; fail(new Error('load')); };
            document.head.appendChild(s);
        });
    }
    Module.knTurnstileLoad.then(function() {
        var holder = document.getElementById('knTurnstileHolder');
        if (!holder || !window.turnstile) { post(6, null); return; }
        try { if (Module.knTurnstileWidget != null) window.turnstile.remove(Module.knTurnstileWidget); } catch (e) {}
        Module.knTurnstileWidget = null;
        try {
            Module.knTurnstileWidget = window.turnstile.render(holder, {
                sitekey: sitekey, appearance: 'interaction-only',
                callback: function(t) { post(1, t); },
                'error-callback': function(code) { post(2, code == null ? 'unknown' : code); return true; },
                'expired-callback': function() { post(3, null); },
                'timeout-callback': function() { post(4, null); },
                'before-interactive-callback': function() { post(5, null); }
            });
            if (Module.knTurnstileWidget == null) post(2, 'render');
        } catch (e) { post(2, 'render'); }
    }, function() { post(6, null); });
});

EM_JS(void, kn_js_turnstile_remove, (void), {
    try { if (Module.knTurnstileWidget != null && window.turnstile) window.turnstile.remove(Module.knTurnstileWidget); } catch (e) {}
    Module.knTurnstileWidget = null;
});
#endif

static void widget_remove(void) {
    if (C.widget_timer) html_timer_cancel(C.widget_timer);
    C.widget_timer = 0;
#ifdef __EMSCRIPTEN__
    kn_js_turnstile_remove();
#endif
    C.widget = 0;
    C.webview = 0;
    kn_webview_captcha_close(); // the challenge webview, natively (no-op elsewhere)
}

// ---------------------------------------------------------------- the request

// The request ends: the widget goes and done runs once. token: the Turnstile response (NULL: none);
// unavailable: no challenge could be completed here (finish_with(NULL, 1)), else a token.
static void finish_with(const char *token, const int unavailable) {
    if (!C.asking) return;
    const KrunkerCaptchaDone done = C.done;
    void *user = C.user;
    C.asking = 0;
    C.done = NULL;
    C.gen++;
    widget_remove();
    hide_modal();
    if (done) done(user, token, unavailable);
}

static void finish(const char *token) { finish_with(token, 0); }

#ifndef __EMSCRIPTEN__
// The proxy the matchmaker's requests take (http_proxy.h: the client setting, else the environment) as
// a URI for the challenge webview, so the player solves it from the network the seek comes from.
// "" = direct. Has the credentials: never logged, wiped by the caller.
static void webview_proxy(char *out, const size_t n) {
    out[0] = 0;
    HttpProxy px;
    if (!http_proxy_for("matchmaker.krunker.io", 1, &px)) return;
    const char *scheme = px.kind == HTTP_PROXY_HTTP ? "http" : px.kind == HTTP_PROXY_SOCKS5 || px.kind == HTTP_PROXY_SOCKS5H ? "socks5" : NULL;
    char label[200];
    http_proxy_label(&px, label, sizeof(label));
    if (!scheme) {
        printf("[krunker] captcha: the challenge cannot use the proxy %s (https:// proxies): it goes direct\n", label);
    } else {
        char user[3 * sizeof(px.user)] = "", pass[3 * sizeof(px.pass)] = "";
        for (int k = 0; k < 2; k++) { // percent-encode the credentials
            const unsigned char *in = (const unsigned char *) (k ? px.pass : px.user);
            char *o = k ? pass : user;
            for (; *in; in++) {
                if ((*in >= 'A' && *in <= 'Z') || (*in >= 'a' && *in <= 'z') || (*in >= '0' && *in <= '9') || strchr("-._~", *in) != NULL) *o++ = (char) *in;
                else o += sprintf(o, "%%%02X", *in);
            }
            *o = 0;
        }
        const int v6 = strchr(px.host, ':') != NULL; // an IPv6 literal goes in brackets
        snprintf(out, n, "%s://%s%s%s%s%s%s%s:%s", scheme, user, pass[0] ? ":" : "", pass, user[0] ? "@" : "", v6 ? "[" : "", px.host,
                 v6 ? "]" : "", px.port);
        KN_LOG_VERBOSE2("[krunker] captcha: the challenge goes through the proxy %s\n", label);
        wipe(user, sizeof(user));
        wipe(pass, sizeof(pass));
    }
    wipe(&px, sizeof(px));
}

// Native: krunker.io's challenge in the webview (kn_webview.h), run hidden unless show_now. 1 = the challenge
// runs; 0 = no webview here or it could not start (logged): the caller ends the request. Shows nothing itself:
// the widget comes up when KN_WEBVIEW_SHOWN arrives.
static int webview_start(const int show_now) {
    if (!kn_webview_available()) {
        printf("[krunker] captcha: this build has no challenge webview: no challenge can be shown here\n");
        return 0;
    }
    char err[200], proxy[1024];
    webview_proxy(proxy, sizeof(proxy));
    const int up = kn_webview_captcha_open(NULL, krunker_build()->turnstile_site_key, NULL, proxy, 1 /* in_ui: the game, the only place */,
                                           show_now, err, sizeof(err));
    wipe(proxy, sizeof(proxy));
    if (!up) {
        printf("[krunker] captcha: no challenge webview (%s): no challenge can be shown here\n", err);
        C.webview = 0;
        return 0;
    }
    C.webview = 1;
    KN_LOG_VERBOSE2("[krunker] captcha: krunker.io's challenge runs in the webview (%s)\n",
           show_now ? "shown at once" : "hidden: shown only if it needs the player");
    return 1;
}

// The webview's newest frame goes to the canvas, where the spinner was. The first frame takes the spinner's place
// (until then the spinner is the loading state).
static void webview_frame(void) {
    const unsigned char *px = NULL;
    int w = 0, h = 0;
    if (!kn_webview_captcha_frame(&px, &w, &h)) return;
    const HtmlElem c = html_dom_by_id("knCaptchaWebview");
    if (c) html_dom_canvas_put(c, px, w, h);
    if (!C.frame_seen) {
        C.frame_seen = 1;
        show("knCaptchaWebview", 1);
        holder_show(1);
        loading_ring(0);
    }
}

// One event of the challenge webview per frame.
static void webview_tick(void) {
    char token[KRUNKER_CAPTCHA_TOKEN_MAX + 1], msg[320];
    const int rc = kn_webview_captcha_poll(token, sizeof(token), msg, sizeof(msg));
    switch (rc) {
        case KN_WEBVIEW_TOKEN:
            if (krunker_captcha_token_shape_ok(token)) {
                KN_LOG_DEBUG1("[krunker] captcha: solved in the webview (%zu chars)\n", strlen(token));
                finish(token);
            } else {
                printf("[krunker] captcha: the webview answered something that is not a token\n");
                C.webview = 0;
                finish_with(NULL, 1);
            }
            break;
        case KN_WEBVIEW_FAILED: // did not load / refused / failed: the challenge cannot be completed here
            printf("[krunker] captcha: the webview failed: %s\n", msg);
            finish_with(NULL, 1);
            break;
        case KN_WEBVIEW_SHOWN: // the player is needed: the widget comes up in the game (its frames once drawn)
            KN_LOG_VERBOSE2("[krunker] captcha: the player is needed: the widget is shown in the game\n");
            C.needs_player = 1;
            break;
        default: break;
    }
    wipe(token, sizeof(token));
}
#endif

#ifdef __EMSCRIPTEN__
static void widget_timeout(void *user) {
    if ((unsigned) (uintptr_t) user != C.gen || !C.asking || !C.widget) return;
    C.widget_timer = 0;
    printf("[krunker] captcha: the widget timed out\n");
    finish_with(NULL, 1);
}

static void widget_start(void) {
    C.widget = 1;
    holder_show(1);
    loading_ring(0);
    if (C.widget_timer) html_timer_cancel(C.widget_timer);
    C.widget_timer = html_timer_add(CAPTCHA_WIDGET_TIMEOUT_MS, widget_timeout, (void *) (uintptr_t) C.gen);
    kn_js_turnstile_render((int) C.gen, krunker_build()->turnstile_site_key);
}

// The widget's events: a token ends the request; an error, an expiry or a timeout ends it without one (the
// request's unavailable path: the join goes on once without a token, as the live client's null does).
static void widget_event(const unsigned gen, const int kind, const char *text) {
    if (gen != C.gen || !C.asking || !C.widget) return;
    switch (kind) {
        case 1: // the token
            if (krunker_captcha_token_shape_ok(text)) {
                KN_LOG_VERBOSE2("[krunker] captcha: Turnstile solved (%zu chars)\n", strlen(text));
                finish(text);
            } else {
                printf("[krunker] captcha: Turnstile answered something that is not a token\n");
                finish_with(NULL, 1);
            }
            break;
        case 2: // error: 1101xx / 1102xx = the sitekey is not valid on this domain
            printf("[krunker] captcha: Turnstile error %s\n", text ? text : "unknown");
            finish_with(NULL, 1);
            break;
        case 3: // expired before we used it (it is used at once: rare)
        case 4:
            KN_LOG_VERBOSE2("[krunker] captcha: Turnstile %s\n", kind == 3 ? "expired" : "timed out");
            finish_with(NULL, 1);
            break;
        case 5: // needs the player: the widget is up and shows what to do
            break;
        default: // 6: api.js did not load (blocked, offline)
            printf("[krunker] captcha: the Turnstile script did not load\n");
            finish_with(NULL, 1);
            break;
    }
}
#endif

static void present(void);

void krunker_captcha_ask(const int attempt, const int rejected, const KrunkerCaptchaDone done, void *user) {
    if (C.asking) {
        C.asking = 0; // replaced: the older request ends without a callback
        widget_remove();
    }
    C.gen++;
    C.asking = 1;
    C.attempt = attempt;
    C.rejected = rejected;
    C.done = done;
    C.user = user;
    C.present_pending = 0;
    KN_LOG_VERBOSE2("[krunker] captcha: asking the player (attempt %d%s)\n", attempt, rejected ? ", the last token was refused" : "");
    if (!html_ui_available()) { // no page (HTML_ENGINE=OFF): nothing can be shown
        printf("[krunker] captcha: no HTML UI in this build to show it\n");
        finish_with(NULL, 1);
        return;
    }
    if (!holder()) { // the page is still loading (a --krunker join at startup)
        C.present_pending = 1;
        return;
    }
    present();
}

static void present(void) {
    const HtmlElem h = holder();
    if (!h) return;
    C.present_pending = 0;
    if (C.rejected) printf("[krunker] captcha: krunker.io refused the last token: one more\n");
#ifdef __EMSCRIPTEN__
    C.needs_player = 1;
    widget_start();
#else
    // Silent first: the widget stays hidden while the challenge runs hidden (Turnstile usually passes by itself)
    // and the spinner stays up. The widget comes up (KN_WEBVIEW_SHOWN) only when the player is needed. With no
    // webview to try, the request ends unavailable at once (finish_with: nothing is shown).
    C.frame_seen = 0;
    C.needs_player = 0;
    holder_show(0);
    show("knCaptchaWebview", 0);
    loading_ring(1);
    C.inline_press = 0;
    if (!webview_start(0)) finish_with(NULL, 1);
#endif
}

void krunker_captcha_close(void) {
    if (!C.asking) return;
    C.asking = 0;
    C.done = NULL;
    C.gen++;
    widget_remove();
    hide_modal();
}

int krunker_captcha_asking(void) { return C.asking; }

void krunker_captcha_tick(void) {
    kn_webview_pump(); // the challenge webview's toolkit (a closed one finishes its teardown)
    if (!C.asking) return;
    if (C.present_pending) {
        if (holder()) present();
        return;
    }
#ifndef __EMSCRIPTEN__
    if (C.webview) webview_tick();
    if (!C.asking) return; // the webview's token finished the request
    if (C.webview) webview_frame(); // the widget's frames (none unless the challenge is shown in the game)
#endif
}

// ---------------------------------------------------------------- the game UI's input (native webview)
// While the challenge is drawn in the game UI (kn_webview_captcha_inline), the pointer over its canvas and
// the widget's keys go to the webview; the rest stays with the game. main.c asks first.
// The canvas's box (CSS px, the pointer's space) maps onto the view's px.
#ifndef __EMSCRIPTEN__
static int inline_point(const double x, const double y, double *vx, double *vy, int *inside) {
    const HtmlElem c = html_dom_by_id("knCaptchaWebview");
    HtmlRect r;
    if (!c || !html_dom_get_rect(c, &r) || r.w <= 0 || r.h <= 0) return 0;
    *vx = (x - r.x) * KN_WEBVIEW_INLINE_W / r.w;
    *vy = (y - r.y) * KN_WEBVIEW_INLINE_H / r.h;
    *inside = x >= r.x && x < r.x + r.w && y >= r.y && y < r.y + r.h;
    return 1;
}
#endif

// kind: a KN_WEBVIEW_POINTER_*; button: GTK's 1 / 2 / 3. 1 = the webview took it (main.c stops there).
int krunker_captcha_inline_pointer(const int kind, const double x, const double y, const int button) {
#ifdef __EMSCRIPTEN__
    (void) kind;
    (void) x;
    (void) y;
    (void) button;
    return 0;
#else
    if (!C.asking || !C.webview || !kn_webview_captcha_inline()) return 0;
    double vx = 0, vy = 0;
    int inside = 0;
    const int placed = inline_point(x, y, &vx, &vy, &inside);
    if (kind == KN_WEBVIEW_POINTER_RELEASE) { // the release of a press the webview took, wherever it is now
        if (!C.inline_press) return 0;
        C.inline_press = 0;
        return kn_webview_captcha_pointer(kind, vx, vy, button);
    }
    if (!placed) return 0;
    if (kind == KN_WEBVIEW_POINTER_PRESS) {
        if (!inside) return 0; // outside the widget: the page's own controls
        C.inline_press = 1;
        return kn_webview_captcha_pointer(kind, vx, vy, button);
    }
    if (!inside && !C.inline_press) return 0; // a move over the rest of the page: the page's hover
    return kn_webview_captcha_pointer(KN_WEBVIEW_POINTER_MOVE, vx, vy, 0);
#endif
}

// kn_key: a KN_WEBVIEW_KEY_* (main.c maps the keys); pressed: 1 press, 0 release, 2 repeat (GLFW's actions).
int krunker_captcha_inline_key(const int kn_key, const int pressed) {
#ifdef __EMSCRIPTEN__
    (void) kn_key;
    (void) pressed;
    return 0;
#else
    if (!C.asking || !C.webview || !kn_webview_captcha_inline()) return 0;
    return kn_webview_captcha_key(kn_key, pressed);
#endif
}

// Typed text while the challenge is drawn in the game UI.
int krunker_captcha_inline_char(const unsigned codepoint) {
#ifdef __EMSCRIPTEN__
    (void) codepoint;
    return 0;
#else
    if (!C.asking || !C.webview || !kn_webview_captcha_inline()) return 0;
    return kn_webview_captcha_char(codepoint);
#endif
}

// ---------------------------------------------------------------- the socket's proof of work
// The client answers the game socket's "cpt" itself (krunker_altcha.c): nothing is shown, neither while it is
// computed nor when it fails (the server's own close says so). The log says what happened.

void krunker_captcha_pow_begin(void) {
    if (C.asking) return; // the matchmaker's challenge has the screen
    C.pow_failed = 0;
    KN_LOG_VERBOSE2("[krunker] captcha: the game server's proof of work: solving it (nothing shown)\n");
}

void krunker_captcha_pow_end(const int solved) {
    if (C.asking) return;
    if (solved) {
        KN_LOG_VERBOSE2("[krunker] captcha: the game server's proof of work passed (nothing shown)\n");
        return;
    }
    if (C.pow_failed) return;
    C.pow_failed = 1;
    printf("[krunker] captcha: the game server's proof of work failed (nothing shown)\n");
}

// The shape of a Turnstile response (krunker_captcha.h): 20..4096 characters of [A-Za-z0-9._~+/=-].
int krunker_captcha_token_shape_ok(const char *token) {
    if (!token) return 0;
    const size_t n = strlen(token);
    if (n < 20 || n > KRUNKER_CAPTCHA_TOKEN_MAX) return 0;
    for (const char *p = token; *p; p++) {
        const char c = *p;
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-' || c == '~' ||
              c == '+' || c == '/' || c == '='))
            return 0;
    }
    return 1;
}
