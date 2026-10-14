// The "Security Challenge" modal of official krunker.io games (client/include/krunker_captcha.h,
// doc/v10/official-captcha.md): the matchmaker's Cloudflare Turnstile, solved by the player, and the
// game socket's ALTCHA proof of work, solved by krunker_altcha.c and only shown here.
//
// The look is the live client's socket-captcha modal (game.deob.js showCaptcha, :98251: a dim
// full-screen holder, a white 450 px box, "SECURITY CHALLENGE" 32 px uppercase, the grey 20 px line
// "Please complete the security challenge to continue."), in ui/css/krunknative.css (.kn-captcha-*).
// The markup is built here on the page's <body> (outside #uiBase, so the closed-menu pointer rule does
// not reach it), the same on both platforms; its buttons call the one page handler knCaptcha(n).
//
// Web: the real Turnstile widget (the script loads on demand, like the live getCaptchaToken, :115623,
// with KRUNKER_TURNSTILE_SITE_KEY and appearance "interaction-only"). Its sitekey only works on the
// domains Cloudflare has for it: elsewhere (krunk.io) it answers 110200 (or another 1101xx / 1102xx
// code), which the modal says plainly before offering the manual path. Native with a webview
// (kn_webview.h, KN_WEBVIEW): "Solve the challenge" opens a window on a real krunker.io page with
// Cloudflare's widget in it, the player solves it there and its token comes back here; the modal keeps
// "Open in your browser instead" and the paste box. Native without one (or when the window cannot
// start / load): the manual path at once (open krunker.io; paste a token; the localhost callback of
// krunker_captcha_cb.h).
//
// Tokens: read from the paste box / the callback / the widget / the webview, checked for shape, handed
// to the requester once, then wiped; never logged, never stored.
#include <krunker_captcha.h>
#include <krunker_captcha_cb.h>
#include <krunker_build.h>
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

#define CAPTCHA_HANDLER "knCaptcha"
#define CAPTCHA_WIDGET_TIMEOUT_MS 120000.0 // the live getCaptchaToken gives up after 120 s (0x1d4c0)
#define POW_RESULT_MS 1500.0                // the proof-of-work result stays this long

enum { // knCaptcha(n)
    ACT_OPEN = 1,   // open krunker.io in the browser (native: with the callback in the fragment)
    ACT_PASTE = 2,  // the clipboard into the token box
    ACT_USE = 3,    // use the token in the box
    ACT_CANCEL = 4,
    ACT_COPY = 5,   // the callback URL to the clipboard
    ACT_RETRY = 6,  // web: render the widget again
    ACT_SOLVE = 7,  // native: open (or raise) the webview with krunker.io's challenge
};

static struct {
    int asking;
    int attempt, rejected;
    KrunkerCaptchaDone done;
    void *user;
    unsigned gen;      // per request: late widget events of an older one are ignored
    int widget;        // web: the Turnstile widget is the path (not fallen back)
    int webview;       // native: the challenge window (kn_webview.h) is the path
    int widget_timer;  // html_timer id of the widget's 120 s limit
    int handler_set;
    int present_pending; // asked before the page was up: shown once it is (krunker_captcha_tick)
    int pow_shown, pow_timer;
    char cb_url[96];   // the callback (has the secret: shown in the box, never logged)
} C;

static void wipe(void *p, size_t n) {
    volatile unsigned char *v = p;
    while (n--) *v++ = 0;
}

// ---------------------------------------------------------------- markup

static int handle_click(void *user, const HtmlEvent *ev, const HtmlArg *args, int argc);

// The modal's markup in the page's language (kn.captcha.*: strings v10 does not have).
static char *markup(void) {
    StrBuf sb = {0};
    sb_appendf(&sb,
               "<div id=\"knCaptchaHolder\" class=\"kn-captcha-holder\" style=\"display:none\">"
               "<div id=\"knCaptchaModal\" class=\"kn-captcha-modal\">"
               "<div class=\"kn-captcha-title\">%s</div>"
               "<div id=\"knCaptchaText\" class=\"kn-captcha-text\">%s</div>"
               "<div id=\"knTurnstileHolder\" class=\"kn-captcha-widget\"></div>"
               "<div id=\"knCaptchaStatus\" class=\"kn-captcha-status\"></div>"
               "<div id=\"knCaptchaRetry\" class=\"kn-captcha-btn\" onclick=\"" CAPTCHA_HANDLER "(6)\" style=\"display:none\">%s</div>"
               "<div id=\"knCaptchaManual\" class=\"kn-captcha-manual\" style=\"display:none\">"
               "<div id=\"knCaptchaCbRow\" class=\"kn-captcha-block\" style=\"display:none\">"
               "<div class=\"kn-captcha-small\">%s</div>"
               "<div class=\"kn-captcha-row\">"
               "<input id=\"knCaptchaCallback\" class=\"kn-captcha-input\" type=\"text\" readonly autocomplete=\"off\" spellcheck=\"false\">"
               "<div id=\"knCaptchaCopy\" class=\"kn-captcha-btn kn-captcha-btn-small\" onclick=\"" CAPTCHA_HANDLER "(5)\">%s</div>"
               "</div></div>"
               "<div class=\"kn-captcha-small\">%s</div>"
               "<div class=\"kn-captcha-row\">",
               I18N("kn.captcha.title", "Security Challenge"),
               I18N("kn.captcha.text", "Please complete the security challenge to continue."),
               I18N("kn.captcha.retry", "Try again"),
               I18N("kn.captcha.userscript", "Your own userscript on krunker.io can send the token here (once, 5 min):"),
               I18N("kn.captcha.copy", "Copy"),
               I18N("kn.captcha.pasteFrom", "Or paste a token from krunker.io:"));
    sb_appendf(&sb,
               "<input id=\"knCaptchaToken\" class=\"kn-captcha-input\" type=\"text\" placeholder=\"%s\" autocomplete=\"off\""
               " spellcheck=\"false\">"
               "<div id=\"knCaptchaPaste\" class=\"kn-captcha-btn kn-captcha-btn-small\" onclick=\"" CAPTCHA_HANDLER "(2)\">%s</div>"
               "<div id=\"knCaptchaUse\" class=\"kn-captcha-btn kn-captcha-btn-small\" onclick=\"" CAPTCHA_HANDLER "(3)\">%s</div>"
               "</div>"
               "</div>"
               "<div class=\"kn-captcha-row kn-captcha-actions\">"
               "<div id=\"knCaptchaSolve\" class=\"kn-captcha-btn\" onclick=\"" CAPTCHA_HANDLER "(7)\" style=\"display:none\">%s</div>"
               "<div id=\"knCaptchaOpen\" class=\"kn-captcha-btn\" onclick=\"" CAPTCHA_HANDLER "(1)\" style=\"display:none\">%s</div>"
               "<div id=\"knCaptchaCancel\" class=\"kn-captcha-btn kn-captcha-cancel\" onclick=\"" CAPTCHA_HANDLER "(4)\">%s</div>"
               "</div>"
               "</div></div>",
               I18N("kn.captcha.tokenPlaceholder", "Turnstile token"), I18N("kn.captcha.paste", "Paste"),
               I18N("kn.captcha.use", "Use"),
               I18N("kn.captcha.solve", "Solve the challenge"), I18N("kn.captcha.openSite", "Open krunker.io"),
               I18N("generic.cancel", "Cancel"));
    return sb_finish(&sb);
}

// The holder on <body>, made on first use (the page may have been rebuilt: look it up every time).
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
    if (!C.handler_set) {
        html_ui_add_handler(CAPTCHA_HANDLER, handle_click);
        C.handler_set = 1;
    }
    return h;
}

static void show(const char *id, const int on) {
    const HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_set_display(e, on ? "" : "none");
}

static void set_text(const char *id, const char *text) {
    const HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_set_text(e, text ? text : "");
}

static void clear_token_box(void) {
    const HtmlElem e = html_dom_by_id("knCaptchaToken");
    if (e) html_dom_set_value(e, "");
}

static void hide_modal(void) {
    const HtmlElem h = html_dom_by_id("knCaptchaHolder");
    if (!h) return;
    clear_token_box();
    const HtmlElem cb = html_dom_by_id("knCaptchaCallback");
    if (cb) html_dom_set_value(cb, "");
    html_dom_set_display(h, "none");
    html_dom_set_class(h, "kn-captcha-passive", 0);
}

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
    C.webview = 0;
    kn_webview_captcha_close(); // the challenge window, natively (no-op elsewhere)
}

// ---------------------------------------------------------------- the request

static void finish(const char *token) {
    if (!C.asking) return;
    const KrunkerCaptchaDone done = C.done;
    void *user = C.user;
    C.asking = 0;
    C.done = NULL;
    C.gen++;
    widget_remove();
    krunker_captcha_cb_stop();
    wipe(C.cb_url, sizeof(C.cb_url));
    hide_modal();
    if (done) done(user, token);
}

static void start_callback(void) {
    if (krunker_captcha_cb_start(C.cb_url, sizeof(C.cb_url))) {
        const HtmlElem cb = html_dom_by_id("knCaptchaCallback");
        if (cb) html_dom_set_value(cb, C.cb_url);
        show("knCaptchaCbRow", 1);
    } else {
        C.cb_url[0] = 0;
        show("knCaptchaCbRow", 0);
    }
}

static void show_manual(const char *why) {
    C.widget = 0;
    widget_remove();
    show("knTurnstileHolder", 0);
    show("knCaptchaManual", 1);
    show("knCaptchaOpen", 1);
    show("knCaptchaSolve", 0);
    const HtmlElem m = html_dom_by_id("knCaptchaModal");
    if (m) html_dom_set_class(m, "kn-captcha-wide", 1);
    set_text("knCaptchaText", why);
#ifndef __EMSCRIPTEN__
    if (!krunker_captcha_cb_listening()) start_callback();
#endif
}

#ifndef __EMSCRIPTEN__
// The proxy the matchmaker's requests take (http_proxy.h: the client setting, else the environment) as
// a URI for the challenge window, so the player solves it from the network the seek comes from.
// "" = direct. Has the credentials: never logged, wiped by the caller.
static void webview_proxy(char *out, const size_t n) {
    out[0] = 0;
    HttpProxy px;
    if (!http_proxy_for("matchmaker.krunker.io", 1, &px)) return;
    const char *scheme = px.kind == HTTP_PROXY_HTTP ? "http" : px.kind == HTTP_PROXY_SOCKS5 || px.kind == HTTP_PROXY_SOCKS5H ? "socks5" : NULL;
    char label[200];
    http_proxy_label(&px, label, sizeof(label));
    if (!scheme) {
        printf("[krunker] captcha: the challenge window cannot use the proxy %s (https:// proxies): it goes direct\n", label);
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
        printf("[krunker] captcha: the challenge window goes through the proxy %s\n", label);
        wipe(user, sizeof(user));
        wipe(pass, sizeof(pass));
    }
    wipe(&px, sizeof(px));
}

// Native: krunker.io's challenge in its own window (kn_webview.h). 1 = the window is up (or raised); 0 =
// no webview here or it could not start (logged), the caller shows the manual path.
static int webview_start(void) {
    if (!kn_webview_available()) return 0;
    char err[200], proxy[1024];
    webview_proxy(proxy, sizeof(proxy));
    const int up = kn_webview_captcha_open(NULL, KRUNKER_TURNSTILE_SITE_KEY, I18N("kn.captcha.title", "Security Challenge"), proxy, err, sizeof(err));
    wipe(proxy, sizeof(proxy));
    if (!up) {
        printf("[krunker] captcha: no challenge window (%s): the browser / paste path\n", err);
        C.webview = 0;
        return 0;
    }
    C.webview = 1;
    printf("[krunker] captcha: krunker.io's challenge is in its own window\n");
    show("knTurnstileHolder", 0);
    // the webview is the path: the paste box and the userscript callback stay hidden (the user does not
    // want them when the window works); only "Solve the challenge" (raise the window) and Cancel show.
    // The fallbacks come up only when the window fails to start / load (show_manual, below).
    krunker_captcha_cb_stop();
    show("knCaptchaManual", 0);
    show("knCaptchaCbRow", 0);
    show("knCaptchaOpen", 0);
    show("knCaptchaSolve", 1);
    const HtmlElem m = html_dom_by_id("knCaptchaModal");
    if (m) html_dom_set_class(m, "kn-captcha-wide", 0);
    set_text("knCaptchaText", I18N("kn.captcha.inWindow", "krunker.io wants a Cloudflare check first. Solve it in the window that opened:"
                                                          " it shows krunker.io's own challenge."));
    return 1;
}

// One event of the challenge window per frame.
static void webview_tick(void) {
    char token[KRUNKER_CAPTCHA_TOKEN_MAX + 1], msg[320];
    const int rc = kn_webview_captcha_poll(token, sizeof(token), msg, sizeof(msg));
    switch (rc) {
        case KN_WEBVIEW_TOKEN:
            if (krunker_captcha_token_shape_ok(token)) {
                printf("[krunker] captcha: solved in the challenge window (%zu chars)\n", strlen(token));
                finish(token);
            } else {
                printf("[krunker] captcha: the challenge window answered something that is not a token\n");
                C.webview = 0;
                set_text("knCaptchaStatus", I18N("kn.captcha.notToken", "Cloudflare answered something that is not a token."));
            }
            break;
        case KN_WEBVIEW_FAILED: { // did not load / refused: the manual path, the window can be tried again
            printf("[krunker] captcha: the challenge window failed: %s\n", msg);
            char why[600];
            I18N_F(why, "kn.captcha.windowFailed", "{0} Solve it on krunker.io in your browser, then paste the token here.", msg);
            show_manual(why);
            show("knCaptchaSolve", 1);
            set_text("knCaptchaStatus", "");
            break;
        }
        case KN_WEBVIEW_CLOSED:
            C.webview = 0;
            set_text("knCaptchaStatus", I18N("kn.captcha.windowClosed", "The challenge window was closed: \"Solve the challenge\" opens it again."));
            break;
        case KN_WEBVIEW_STATUS:
            set_text("knCaptchaStatus", msg);
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
    kn_js_turnstile_remove();
    set_text("knCaptchaStatus", I18N("kn.captcha.timedOut", "The challenge timed out."));
    show("knCaptchaRetry", 1);
    show("knCaptchaManual", 1);
    show("knCaptchaOpen", 1);
}

static void widget_start(void) {
    C.widget = 1;
    show("knTurnstileHolder", 1);
    show("knCaptchaRetry", 0);
    set_text("knCaptchaStatus", I18N("kn.captcha.loading", "Loading Cloudflare's challenge..."));
    if (C.widget_timer) html_timer_cancel(C.widget_timer);
    C.widget_timer = html_timer_add(CAPTCHA_WIDGET_TIMEOUT_MS, widget_timeout, (void *) (uintptr_t) C.gen);
    kn_js_turnstile_render((int) C.gen, KRUNKER_TURNSTILE_SITE_KEY);
}

static void widget_event(const unsigned gen, const int kind, const char *text) {
    if (gen != C.gen || !C.asking || !C.widget) return;
    switch (kind) {
        case 1: // the token
            if (krunker_captcha_token_shape_ok(text)) {
                printf("[krunker] captcha: Turnstile solved (%zu chars)\n", strlen(text));
                finish(text);
            } else {
                set_text("knCaptchaStatus", I18N("kn.captcha.notToken", "Cloudflare answered something that is not a token."));
                show("knCaptchaRetry", 1);
            }
            break;
        case 2: { // error: 1101xx / 1102xx = the sitekey is not valid on this domain
            const char *code = text ? text : "unknown";
            printf("[krunker] captcha: Turnstile error %s\n", code);
            if (!strncmp(code, "1101", 4) || !strncmp(code, "1102", 4)) {
                char origin[160], why[900], err[17];
                html_page_origin(origin, sizeof(origin));
                snprintf(err, sizeof(err), "%.16s", code);
                I18N_F(why, "kn.captcha.wrongSite",
                       "Cloudflare won't show krunker.io's challenge on {0}: Turnstile keys only work on the sites they were made for"
                       " (error {1}). Solve it on krunker.io, then paste the token here, or play this game on krunker.io.",
                       origin, err);
                show_manual(why);
                set_text("knCaptchaStatus", "");
            } else {
                char status[320], err[17];
                snprintf(err, sizeof(err), "%.16s", code);
                I18N_F(status, "kn.captcha.failed", "The challenge failed (Cloudflare error {0}).", err);
                kn_js_turnstile_remove();
                set_text("knCaptchaStatus", status);
                show("knCaptchaRetry", 1);
                show("knCaptchaManual", 1);
                show("knCaptchaOpen", 1);
            }
            break;
        }
        case 3: // expired before we used it (it is used at once: rare): a fresh one
        case 4:
            set_text("knCaptchaStatus", kind == 3 ? I18N("kn.captcha.expired", "The challenge expired.")
                                                  : I18N("kn.captcha.timedOut", "The challenge timed out."));
            show("knCaptchaRetry", 1);
            break;
        case 5:
            set_text("knCaptchaStatus", "");
            break;
        default: // 6: api.js did not load (blocked, offline)
            printf("[krunker] captcha: the Turnstile script did not load\n");
            show_manual(I18N("kn.captcha.noScript", "Cloudflare's challenge could not be loaded here (blocked or offline). Solve it on krunker.io, then paste"
                                                    " the token here, or play this game on krunker.io."));
            set_text("knCaptchaStatus", "");
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
    if (C.pow_timer) html_timer_cancel(C.pow_timer);
    C.pow_timer = 0;
    C.pow_shown = 0;
    C.asking = 1;
    C.attempt = attempt;
    C.rejected = rejected;
    C.done = done;
    C.user = user;
    C.present_pending = 0;
    printf("[krunker] captcha: asking the player (attempt %d%s)\n", attempt, rejected ? ", the last token was refused" : "");
    if (!html_ui_available()) { // no page (HTML_ENGINE=OFF): nothing can be shown
        printf("[krunker] captcha: no HTML UI in this build to show it\n");
        finish(NULL);
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
    const int rejected = C.rejected;
    html_dom_set_class(h, "kn-captcha-passive", 0);
    const HtmlElem m = html_dom_by_id("knCaptchaModal");
    if (m) html_dom_set_class(m, "kn-captcha-wide", 0);
    html_dom_set_display(h, "");
    show("knCaptchaCancel", 1);
    show("knCaptchaRetry", 0);
    clear_token_box();
    set_text("knCaptchaStatus", rejected ? I18N("kn.captcha.refused", "krunker.io refused that token (they work once, for 5 minutes): one more, please.") : "");
#ifdef __EMSCRIPTEN__
    show("knCaptchaManual", 0);
    show("knCaptchaOpen", 0);
    set_text("knCaptchaText", I18N("kn.captcha.matchmaker", "krunker.io's matchmaker wants this check before it finds you a game."));
    widget_start();
#else
    if (webview_start()) {
        if (!rejected) set_text("knCaptchaStatus", I18N("kn.captcha.waitingWindow", "Waiting for the challenge window..."));
    } else {
        show_manual(I18N("kn.captcha.needsBrowser", "krunker.io wants a Cloudflare check first. It needs a web browser: solve it on krunker.io."));
    }
#endif
}

void krunker_captcha_close(void) {
    if (C.pow_shown) { // the proof-of-work state of a session that ended
        if (C.pow_timer) html_timer_cancel(C.pow_timer);
        C.pow_timer = C.pow_shown = 0;
        if (!C.asking) hide_modal();
    }
    if (!C.asking) return;
    C.asking = 0;
    C.done = NULL;
    C.gen++;
    widget_remove();
    krunker_captcha_cb_stop();
    wipe(C.cb_url, sizeof(C.cb_url));
    hide_modal();
}

int krunker_captcha_asking(void) { return C.asking; }

void krunker_captcha_tick(void) {
    kn_webview_pump(); // the challenge window's toolkit (a closed one finishes its teardown)
    if (!C.asking) return;
    if (C.present_pending) {
        if (holder()) present();
        return;
    }
#ifndef __EMSCRIPTEN__
    if (C.webview) webview_tick();
    if (!C.asking) return; // the window's token finished the request
#endif
    char token[KRUNKER_CAPTCHA_TOKEN_MAX + 1];
    const int rc = krunker_captcha_cb_poll(token, sizeof(token));
    if (rc > 0) {
        finish(token);
        wipe(token, sizeof(token));
    } else if (rc < 0) {
        wipe(C.cb_url, sizeof(C.cb_url));
        const HtmlElem cb = html_dom_by_id("knCaptchaCallback");
        if (cb) html_dom_set_value(cb, "");
        show("knCaptchaCbRow", 0);
        set_text("knCaptchaStatus", "The address expired: \"Open krunker.io\" makes a new one.");
    }
}

// ---------------------------------------------------------------- buttons

static void pasted(void *user, const char *text) {
    (void) user;
    if (!C.asking || !text) return;
    const HtmlElem e = html_dom_by_id("knCaptchaToken");
    if (!e) return;
    // first non-blank run only: a token has no spaces
    while (*text == ' ' || *text == '\n' || *text == '\r' || *text == '\t') text++;
    char buf[KRUNKER_CAPTCHA_TOKEN_MAX + 1];
    const size_t n = strcspn(text, " \r\n\t");
    snprintf(buf, sizeof(buf), "%.*s", (int) (n < sizeof(buf) ? n : sizeof(buf) - 1), text);
    html_dom_set_value(e, buf);
    wipe(buf, sizeof(buf));
}

static void use_pasted(void) {
    const HtmlElem e = html_dom_by_id("knCaptchaToken");
    char raw[KRUNKER_CAPTCHA_TOKEN_MAX + 64] = "";
    if (e && html_dom_get_value(e, raw, sizeof(raw)) < 0) raw[0] = 0;
    char *s = raw;
    while (*s == ' ' || *s == '\n' || *s == '\r' || *s == '\t') s++;
    size_t n = strlen(s);
    while (n && (s[n - 1] == ' ' || s[n - 1] == '\n' || s[n - 1] == '\r' || s[n - 1] == '\t')) s[--n] = 0;
    if (!krunker_captcha_token_shape_ok(s)) {
        printf("[krunker] captcha: the pasted text is not a token (%zu chars)\n", n);
        set_text("knCaptchaStatus", n ? I18N("kn.captcha.badPaste", "That is not a Turnstile token.") : I18N("kn.captcha.pasteFirst", "Paste a token first."));
        if (e) html_dom_focus(e); // back in the box, to fix it
    } else {
        printf("[krunker] captcha: a pasted token (%zu chars)\n", n);
        finish(s);
    }
    wipe(raw, sizeof(raw));
}

static void open_krunker(void) {
#ifdef __EMSCRIPTEN__
    ui_open_browser("https://krunker.io/");
#else
    if (!krunker_captcha_cb_listening()) start_callback();
    if (C.cb_url[0]) {
        // the userscript reads the callback from the fragment (never sent to krunker.io's servers):
        // #kn-captcha=<port>-<secret>
        const char *port = C.cb_url + strlen("http://127.0.0.1:");
        const char *slash = strchr(port, '/');
        char url[160];
        snprintf(url, sizeof(url), "https://krunker.io/#kn-captcha=%.*s-%s", slash ? (int) (slash - port) : 0, port, slash ? slash + 1 : "");
        ui_open_browser(url);
        wipe(url, sizeof(url));
        set_text("knCaptchaStatus", "krunker.io opened in your browser. Waiting for a token...");
    } else {
        ui_open_browser("https://krunker.io/");
    }
#endif
}

static int handle_click(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    const int act = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
    if (!C.asking) return 0;
    switch (act) {
        case ACT_OPEN: open_krunker(); break;
        case ACT_PASTE: html_clipboard_read(pasted, NULL); break;
        case ACT_USE: use_pasted(); break;
        case ACT_CANCEL:
            printf("[krunker] captcha: cancelled by the player\n");
            finish(NULL);
            break;
        case ACT_COPY:
            if (C.cb_url[0]) html_clipboard_write(C.cb_url);
            break;
        case ACT_RETRY:
#ifdef __EMSCRIPTEN__
            widget_start();
#endif
            break;
        case ACT_SOLVE:
#ifndef __EMSCRIPTEN__
            if (!webview_start()) {
                char err[200];
                snprintf(err, sizeof(err), "%s", I18N("kn.captcha.noWindow", "The challenge window could not open here."));
                show_manual(err);
            } else {
                set_text("knCaptchaStatus", "");
            }
#endif
            break;
        default: break;
    }
    return 0;
}

// ---------------------------------------------------------------- the socket's proof of work

static void pow_hide(void *user) {
    (void) user;
    C.pow_timer = 0;
    if (C.asking || !C.pow_shown) return;
    C.pow_shown = 0;
    hide_modal();
}

void krunker_captcha_pow_begin(void) {
    if (C.asking) return; // the matchmaker's modal has the screen
    const HtmlElem h = holder();
    if (!h) return;
    if (C.pow_timer) html_timer_cancel(C.pow_timer);
    C.pow_timer = 0;
    C.pow_shown = 1;
    html_dom_set_class(h, "kn-captcha-passive", 1);
    const HtmlElem m = html_dom_by_id("knCaptchaModal");
    if (m) html_dom_set_class(m, "kn-captcha-wide", 0);
    html_dom_set_display(h, "");
    show("knTurnstileHolder", 0);
    show("knCaptchaManual", 0);
    show("knCaptchaRetry", 0);
    show("knCaptchaOpen", 0);
    show("knCaptchaCancel", 0);
    set_text("knCaptchaText", I18N("kn.captcha.pow", "The game server asked for a proof of work."));
    set_text("knCaptchaStatus", I18N("kn.captcha.verifying", "Verifying\xe2\x80\xa6"));
}

void krunker_captcha_pow_end(const int solved) {
    if (C.asking) return;
    if (!C.pow_shown) krunker_captcha_pow_begin();
    if (!C.pow_shown) return;
    set_text("knCaptchaStatus", solved ? I18N("kn.captcha.verified", "Verified \xe2\x9c\x93")
                                       : I18N("kn.captcha.powFailed", "This client could not answer it: the server may close the game."));
    if (C.pow_timer) html_timer_cancel(C.pow_timer);
    C.pow_timer = html_timer_add(solved ? POW_RESULT_MS : POW_RESULT_MS * 3, pow_hide, NULL);
}
