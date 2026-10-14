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
// code), which the modal says plainly before offering the manual path. Native: the manual path at once
// (open krunker.io; paste a token; the localhost callback of krunker_captcha_cb.h).
//
// Tokens: read from the paste box / the callback / the widget, checked for shape, handed to the
// requester once, then wiped; never logged, never stored.
#include <krunker_captcha.h>
#include <krunker_captcha_cb.h>
#include <krunker_build.h>
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
};

static struct {
    int asking;
    int attempt, rejected;
    KrunkerCaptchaDone done;
    void *user;
    unsigned gen;      // per request: late widget events of an older one are ignored
    int widget;        // web: the Turnstile widget is the path (not fallen back)
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

static const char k_markup[] =
    "<div id=\"knCaptchaHolder\" class=\"kn-captcha-holder\" style=\"display:none\">"
    "<div id=\"knCaptchaModal\" class=\"kn-captcha-modal\">"
    "<div class=\"kn-captcha-title\">Security Challenge</div>"
    "<div id=\"knCaptchaText\" class=\"kn-captcha-text\">Please complete the security challenge to continue.</div>"
    "<div id=\"knTurnstileHolder\" class=\"kn-captcha-widget\"></div>"
    "<div id=\"knCaptchaStatus\" class=\"kn-captcha-status\"></div>"
    "<div id=\"knCaptchaRetry\" class=\"kn-captcha-btn\" onclick=\"" CAPTCHA_HANDLER "(6)\" style=\"display:none\">Try again</div>"
    "<div id=\"knCaptchaManual\" class=\"kn-captcha-manual\" style=\"display:none\">"
    "<div id=\"knCaptchaCbRow\" class=\"kn-captcha-block\" style=\"display:none\">"
    "<div class=\"kn-captcha-small\">Your own userscript on krunker.io can send the token here (once, 5 min):</div>"
    "<div class=\"kn-captcha-row\">"
    "<input id=\"knCaptchaCallback\" class=\"kn-captcha-input\" type=\"text\" readonly autocomplete=\"off\" spellcheck=\"false\">"
    "<div id=\"knCaptchaCopy\" class=\"kn-captcha-btn kn-captcha-btn-small\" onclick=\"" CAPTCHA_HANDLER "(5)\">Copy</div>"
    "</div></div>"
    "<div class=\"kn-captcha-small\">Or paste a token from krunker.io:</div>"
    "<div class=\"kn-captcha-row\">"
    "<input id=\"knCaptchaToken\" class=\"kn-captcha-input\" type=\"text\" placeholder=\"Turnstile token\" autocomplete=\"off\""
    " spellcheck=\"false\">"
    "<div id=\"knCaptchaPaste\" class=\"kn-captcha-btn kn-captcha-btn-small\" onclick=\"" CAPTCHA_HANDLER "(2)\">Paste</div>"
    "<div id=\"knCaptchaUse\" class=\"kn-captcha-btn kn-captcha-btn-small\" onclick=\"" CAPTCHA_HANDLER "(3)\">Use</div>"
    "</div>"
    "</div>"
    "<div class=\"kn-captcha-row kn-captcha-actions\">"
    "<div id=\"knCaptchaOpen\" class=\"kn-captcha-btn\" onclick=\"" CAPTCHA_HANDLER "(1)\" style=\"display:none\">Open krunker.io</div>"
    "<div id=\"knCaptchaCancel\" class=\"kn-captcha-btn kn-captcha-cancel\" onclick=\"" CAPTCHA_HANDLER "(4)\">Cancel</div>"
    "</div>"
    "</div></div>";

// The holder on <body>, made on first use (the page may have been rebuilt: look it up every time).
static HtmlElem holder(void) {
    if (!html_ui_available() || !html_ui_is_ready()) return 0;
    HtmlElem h = html_dom_by_id("knCaptchaHolder");
    if (!h) {
        const HtmlElem body = html_dom_body();
        if (!body) return 0;
        html_dom_insert_html(body, HTML_INSERT_BEFORE_END, k_markup);
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
    const HtmlElem m = html_dom_by_id("knCaptchaModal");
    if (m) html_dom_set_class(m, "kn-captcha-wide", 1);
    set_text("knCaptchaText", why);
#ifndef __EMSCRIPTEN__
    if (!krunker_captcha_cb_listening()) start_callback();
#endif
}

#ifdef __EMSCRIPTEN__
static void widget_timeout(void *user) {
    if ((unsigned) (uintptr_t) user != C.gen || !C.asking || !C.widget) return;
    C.widget_timer = 0;
    kn_js_turnstile_remove();
    set_text("knCaptchaStatus", "The challenge timed out.");
    show("knCaptchaRetry", 1);
    show("knCaptchaManual", 1);
    show("knCaptchaOpen", 1);
}

static void widget_start(void) {
    C.widget = 1;
    show("knTurnstileHolder", 1);
    show("knCaptchaRetry", 0);
    set_text("knCaptchaStatus", "Loading Cloudflare's challenge...");
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
                set_text("knCaptchaStatus", "Cloudflare answered something that is not a token.");
                show("knCaptchaRetry", 1);
            }
            break;
        case 2: { // error: 1101xx / 1102xx = the sitekey is not valid on this domain
            const char *code = text ? text : "unknown";
            printf("[krunker] captcha: Turnstile error %s\n", code);
            if (!strncmp(code, "1101", 4) || !strncmp(code, "1102", 4)) {
                char origin[160], why[600];
                html_page_origin(origin, sizeof(origin));
                snprintf(why, sizeof(why),
                         "Cloudflare won't show krunker.io's challenge on %s: Turnstile keys only work on the sites they were made for"
                         " (error %.16s). Solve it on krunker.io, then paste the token here, or play this game on krunker.io.",
                         origin, code);
                show_manual(why);
                set_text("knCaptchaStatus", "");
            } else {
                char status[160];
                snprintf(status, sizeof(status), "The challenge failed (Cloudflare error %.16s).", code);
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
            set_text("knCaptchaStatus", kind == 3 ? "The challenge expired." : "The challenge timed out.");
            show("knCaptchaRetry", 1);
            break;
        case 5:
            set_text("knCaptchaStatus", "");
            break;
        default: // 6: api.js did not load (blocked, offline)
            printf("[krunker] captcha: the Turnstile script did not load\n");
            show_manual("Cloudflare's challenge could not be loaded here (blocked or offline). Solve it on krunker.io, then paste"
                        " the token here, or play this game on krunker.io.");
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
    set_text("knCaptchaStatus", rejected ? "krunker.io refused that token (they work once, for 5 minutes): one more, please." : "");
#ifdef __EMSCRIPTEN__
    show("knCaptchaManual", 0);
    show("knCaptchaOpen", 0);
    set_text("knCaptchaText", "krunker.io's matchmaker wants this check before it finds you a game.");
    widget_start();
#else
    show_manual("krunker.io wants a Cloudflare check first. It needs a web browser: solve it on krunker.io.");
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
    if (!C.asking) return;
    if (C.present_pending) {
        if (holder()) present();
        return;
    }
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
        set_text("knCaptchaStatus", n ? "That is not a Turnstile token." : "Paste a token first.");
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
    set_text("knCaptchaText", "The game server asked for a proof of work.");
    set_text("knCaptchaStatus", "Verifying\xe2\x80\xa6");
}

void krunker_captcha_pow_end(const int solved) {
    if (C.asking) return;
    if (!C.pow_shown) krunker_captcha_pow_begin();
    if (!C.pow_shown) return;
    set_text("knCaptchaStatus", solved ? "Verified \xe2\x9c\x93" : "This client could not answer it: the server may close the game.");
    if (C.pow_timer) html_timer_cancel(C.pow_timer);
    C.pow_timer = html_timer_add(solved ? POW_RESULT_MS : POW_RESULT_MS * 3, pow_hide, NULL);
}
