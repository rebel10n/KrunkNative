// v10.0.7's Security Challenge card and its ALTCHA widget (client/include/altcha_modal.h).
//
// showCaptcha (deob-10.0.7.js:98914-98968), as the bundle builds it on <body>:
//   #captcha-holder   rgba(0,0,0,0.5), z-index 0x7fffffff, fixed over the whole screen, pointer-events all
//     card            white, fixed at 50% / 50% translated back, padding 20px, radius 6px, box-shadow
//                     0 9px 0 0 #a6a6a6, 450px wide, a centred flex column
//       title         32px, uppercase, margin-bottom 10px: "Security Challenge"
//       text          rgba(0,0,0,0.6), 20px, margin-bottom 10px: "Please complete the security challenge to continue."
//       div > <altcha-widget challengejson="JSON">
//   setInterval(200 ms): once the widget has input[name=altcha], cb(value, null), and the card goes when hideOnSolve.
// The bundle's strings are English literals (no i18n keys), so are these.
//
// The widget is libs/altcha.1.0.6.min.js's (no `auto` attribute: the player clicks): its DOM per state (Tn, Ln, Nn,
// zn, st, ct, at in the library) and its strings (label "I'm not a robot", verifying "Verifying...", verified
// "Verified", error "Verification failed. Try again later.", footer "Protected by ALTCHA"); its stylesheet
// (svelte-ddsc3z) is in ui/css/krunknative.css. A click in the unverified or error state starts the search.
// KRUNKNATIVE_ALTCHA_CLICK=1 (tests and screenshots: the official e2e's joins) clicks the checkbox as the card shows.
#include <altcha_modal.h>

#include <kn_captcha.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include "klog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define ALTCHA_POLL_MS 200.0 // the bundle's setInterval(..., 0xc8)
#define ALTCHA_ALIVE_MS 100.0

static struct {
    int open;
    int state;
    int hide_on_solve;
    char *challenge;   // the challenge JSON (the widget's challengejson)
    char *payload;     // the verified value (the hidden input's)
    unsigned solve_id; // kn_captcha_solve in flight
    int poll_timer, alive_timer;
    int handlers;
    unsigned page;     // html_ui_page_serial of the page the handlers were added on
    AltchaModalDone done;
    AltchaModalAlive alive;
    void *user;
} M;

static void wipe_free(char **p) {
    if (!*p) return;
    memset(*p, 0, strlen(*p));
    free(*p);
    *p = NULL;
}

// ---------------------------------------------------------------- the widget's markup

// The ALTCHA logo (st(): the link to altcha.org around a 22x22 svg)
#define ALTCHA_LOGO                                                                                                                  \
    "<div><a href=\"https://altcha.org/\" target=\"_blank\" class=\"altcha-logo svelte-ddsc3z\" aria-label=\"Visit Altcha.org\">"     \
    "<svg width=\"22\" height=\"22\" viewBox=\"0 0 20 20\" fill=\"none\" xmlns=\"http://www.w3.org/2000/svg\">"                       \
    "<path d=\"M2.33955 16.4279C5.88954 20.6586 12.1971 21.2105 16.4279 17.6604C18.4699 15.947 19.6548 13.5911 19.9352 "             \
    "11.1365L17.9886 10.4279C17.8738 12.5624 16.909 14.6459 15.1423 16.1284C11.7577 18.9684 6.71167 18.5269 3.87164 "                 \
    "15.1423C1.03163 11.7577 1.4731 6.71166 4.8577 3.87164C8.24231 1.03162 13.2883 1.4731 16.1284 4.8577C16.9767 5.86872 "            \
    "17.5322 7.02798 17.804 8.2324L19.9522 9.01429C19.7622 7.07737 19.0059 5.17558 17.6604 3.57212C14.1104 -0.658624 7.80283 "         \
    "-1.21043 3.57212 2.33956C-0.658625 5.88958 -1.21046 12.1971 2.33955 16.4279Z\" fill=\"currentColor\"></path>"                    \
    "<path d=\"M3.57212 2.33956C1.65755 3.94607 0.496389 6.11731 0.12782 8.40523L2.04639 9.13961C2.26047 7.15832 3.21057 5.25375 "    \
    "4.8577 3.87164C8.24231 1.03162 13.2883 1.4731 16.1284 4.8577L13.8302 6.78606L19.9633 9.13364C19.7929 7.15555 19.0335 "            \
    "5.20847 17.6604 3.57212C14.1104 -0.658624 7.80283 -1.21043 3.57212 2.33956Z\" fill=\"currentColor\"></path>"                     \
    "<path d=\"M7 10H5C5 12.7614 7.23858 15 10 15C12.7614 15 15 12.7614 15 10H13C13 11.6569 11.6569 13 10 13C8.3431 13 7 11.6569 "     \
    "7 10Z\" fill=\"currentColor\"></path></svg></a></div>"
// lt(): the spinner while verifying
#define ALTCHA_SPINNER                                                                                                               \
    "<svg width=\"24\" height=\"24\" viewBox=\"0 0 24 24\" xmlns=\"http://www.w3.org/2000/svg\">"                                    \
    "<path d=\"M12,1A11,11,0,1,0,23,12,11,11,0,0,0,12,1Zm0,19a8,8,0,1,1,8-8A8,8,0,0,1,12,20Z\" fill=\"currentColor\" "              \
    "opacity=\".25\"></path><path d=\"M12,4a8,8,0,0,1,7.89,6.7A1.53,1.53,0,0,0,21.38,12h0a1.5,1.5,0,0,0,1.48-1.75,11,11,0,0,0-21.72,"  \
    "0A1.5,1.5,0,0,0,2.62,12h0a1.53,1.53,0,0,0,1.49-1.3A8,8,0,0,1,12,4Z\" fill=\"currentColor\" class=\"altcha-spinner "             \
    "svelte-ddsc3z\"></path></svg>"
// ct() with Sn(): the error row
#define ALTCHA_ERROR_ROW                                                                                                             \
    "<div class=\"altcha-error svelte-ddsc3z\"><svg width=\"14\" height=\"14\" xmlns=\"http://www.w3.org/2000/svg\" fill=\"none\" "  \
    "viewBox=\"0 0 24 24\" stroke-width=\"1.5\" stroke=\"currentColor\"><path stroke-linecap=\"round\" stroke-linejoin=\"round\" "    \
    "d=\"M6 18L18 6M6 6l12 12\"></path></svg><div title=\"Verification failed. Try again later.\">Verification failed. Try again "      \
    "later.</div></div>"
// at(): the footer
#define ALTCHA_FOOTER                                                                                                                \
    "<div class=\"altcha-footer svelte-ddsc3z\"><div class=\"svelte-ddsc3z\">Protected by <a href=\"https://altcha.org/\" "           \
    "target=\"_blank\" aria-label=\"Visit Altcha.org\">ALTCHA</a></div></div>"

static const char *const STATE_NAME[] = {"unverified", "verifying", "verified", "error"};

// the widget's content (what the custom element renders inside <altcha-widget>)
static void widget_inner(StrBuf *out, const int state, const char *payload) {
    StrBuf sb = {0};
    const int s = state >= ALTCHA_UNVERIFIED && state <= ALTCHA_ERROR ? state : ALTCHA_UNVERIFIED;
    sb_appendf(&sb, "<div class=\"altcha svelte-ddsc3z\" data-state=\"%s\"><div class=\"altcha-main svelte-ddsc3z\">", STATE_NAME[s]);
    if (s == ALTCHA_VERIFYING) sb_append(&sb, ALTCHA_SPINNER);
    // the checkbox (hidden while verifying, checked once verified); its change runs the widget's verify
    sb_appendf(&sb, "<div class=\"altcha-checkbox svelte-ddsc3z%s\"><input type=\"checkbox\" id=\"altcha_checkbox\" required "
                    "class=\"svelte-ddsc3z\" onclick=\"knAltchaCheck()\"%s></div>",
               s == ALTCHA_VERIFYING ? " altcha-hidden" : "", s == ALTCHA_VERIFIED ? " checked" : "");
    sb_append(&sb, "<div class=\"altcha-label svelte-ddsc3z\">");
    if (s == ALTCHA_VERIFIED) {
        sb_append(&sb, "<span>Verified</span> <input type=\"hidden\" name=\"altcha\" value=\"");
        sb_append_esc(&sb, payload ? payload : "");
        sb_append(&sb, "\">");
    } else if (s == ALTCHA_VERIFYING) {
        sb_append(&sb, "<span>Verifying...</span>");
    } else {
        sb_append(&sb, "<label for=\"altcha_checkbox\" class=\"svelte-ddsc3z\" onclick=\"knAltchaCheck()\">I'm not a robot</label>");
    }
    sb_append(&sb, "</div>" ALTCHA_LOGO "</div>");
    if (s == ALTCHA_ERROR) sb_append(&sb, ALTCHA_ERROR_ROW);
    sb_append(&sb, ALTCHA_FOOTER "</div>");
    char *html = sb_finish(&sb);
    sb_append(out, html);
    free(html);
}

char *altcha_modal_widget_html(const int state, const char *challenge_json, const char *payload) {
    StrBuf sb = {0};
    sb_append(&sb, "<altcha-widget challengejson=\"");
    sb_append_esc(&sb, challenge_json ? challenge_json : "");
    sb_append(&sb, "\">");
    widget_inner(&sb, state, payload);
    sb_append(&sb, "</altcha-widget>");
    return sb_finish(&sb);
}

// ---------------------------------------------------------------- the card

static void render_widget(void) {
    const HtmlElem holder = html_dom_by_id("captcha-holder");
    const HtmlElem widget = holder ? html_dom_query(holder, "altcha-widget") : 0;
    if (!widget) return;
    StrBuf sb = {0};
    widget_inner(&sb, M.state, M.payload);
    char *html = sb_finish(&sb);
    html_dom_set_html(widget, html);
    free(html);
}

static void timers_stop(void) {
    if (M.poll_timer) html_timer_cancel(M.poll_timer);
    if (M.alive_timer) html_timer_cancel(M.alive_timer);
    M.poll_timer = M.alive_timer = 0;
}

void altcha_modal_hide(void) {
    if (!M.open) return;
    timers_stop();
    kn_captcha_cancel(M.solve_id);
    M.solve_id = 0;
    const HtmlElem holder = html_dom_by_id("captcha-holder");
    if (holder) html_dom_remove(holder);
    wipe_free(&M.challenge);
    wipe_free(&M.payload);
    M.open = 0;
    M.done = NULL;
    M.alive = NULL;
    M.user = NULL;
    KN_LOG_DEBUG1("[captcha] the security challenge card is hidden\n");
}

int altcha_modal_open(void) { return M.open; }

// The interval's look at the widget: the hidden input is there, so cb(value, null), and the card goes when hideOnSolve
static void poll_fire(void *user) {
    (void) user;
    M.poll_timer = 0;
    if (!M.open || M.state != ALTCHA_VERIFIED || !M.payload) return;
    const AltchaModalDone done = M.done;
    void *u = M.user;
    char *payload = M.payload;
    M.payload = NULL;
    M.done = NULL; // the interval is cleared: called once
    if (M.hide_on_solve) altcha_modal_hide();
    if (done) done(u, payload);
    wipe_free(&payload);
}

static void alive_fire(void *user) {
    (void) user;
    M.alive_timer = 0;
    if (!M.open) return;
    if (M.alive && !M.alive(M.user)) {
        altcha_modal_hide();
        return;
    }
    M.alive_timer = html_timer_add(ALTCHA_ALIVE_MS, alive_fire, NULL);
}

static void on_solved(void *user, const char *payload) {
    (void) user;
    M.solve_id = 0;
    if (!M.open) return;
    if (!payload) { // no number up to maxnumber, past the deadline: the widget's error (and nothing is sent)
        M.state = ALTCHA_ERROR;
        render_widget();
        KN_LOG_DEBUG1("[captcha] security challenge: verification failed (the widget's error; a click tries again)\n");
        return;
    }
    M.state = ALTCHA_VERIFIED;
    M.payload = strdup(payload);
    render_widget();
    KN_LOG_DEBUG1("[captcha] security challenge: verified\n");
    if (!M.poll_timer) M.poll_timer = html_timer_add(ALTCHA_POLL_MS, poll_fire, NULL);
}

// The checkbox's change: verify() from the unverified, error or expired state
static void verify(void) {
    if (!M.open || (M.state != ALTCHA_UNVERIFIED && M.state != ALTCHA_ERROR)) return;
    M.state = ALTCHA_VERIFYING;
    render_widget();
    M.solve_id = kn_captcha_solve("altcha", M.challenge, on_solved, NULL);
    if (!M.solve_id) { // not a challenge the widget can verify (algorithm, its fields, over the cap)
        M.state = ALTCHA_ERROR;
        render_widget();
    }
    KN_LOG_DEBUG1("[captcha] security challenge: the player clicked \"I'm not a robot\"%s\n", M.solve_id ? "" : " (cannot be verified)");
}

// KRUNKNATIVE_ALTCHA_CLICK: the player's click, made for them
static void auto_check(void *user) {
    (void) user;
    KN_LOG_DEBUG1("[captcha] security challenge: KRUNKNATIVE_ALTCHA_CLICK clicks the checkbox\n");
    verify();
}

static int auto_click(void) {
    const char *e = getenv("KRUNKNATIVE_ALTCHA_CLICK");
    return e && *e && strcmp(e, "0") != 0;
}

static int h_check(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user;
    (void) ev;
    (void) args;
    (void) argc;
    verify();
    return 1;
}

int altcha_modal_available(void) {
    if (!html_ui_available() || !html_ui_is_ready() || !html_dom_body()) return 0;
    const char *page = html_ui_current_page();
    return page && !strcmp(page, "index.html");
}

int altcha_modal_show(const char *challenge_json, const int hide_on_solve, const AltchaModalDone done, const AltchaModalAlive alive, void *user) {
    if (!altcha_modal_available()) return 0;
    if (M.open) {
        if (html_dom_by_id("captcha-holder")) return 1; // null == captchaHolder only: the card up stays, this call is dropped
        altcha_modal_hide(); // the page was rebuilt under it
    }
    if (!M.handlers || M.page != html_ui_page_serial()) {
        html_ui_add_handler("knAltchaCheck", h_check);
        M.handlers = 1;
        M.page = html_ui_page_serial();
    }
    M.open = 1;
    M.state = ALTCHA_UNVERIFIED;
    M.hide_on_solve = hide_on_solve != 0;
    M.challenge = strdup(challenge_json ? challenge_json : "{}");
    M.done = done;
    M.alive = alive;
    M.user = user;
    char *widget = altcha_modal_widget_html(M.state, M.challenge, NULL);
    StrBuf sb = {0};
    sb_append(&sb, "<div id=\"captcha-holder\" style=\"background-color: rgba(0, 0, 0, 0.5); z-index: 2147483647; position: fixed; "
                   "top: 0px; left: 0px; bottom: 0px; right: 0px; pointer-events: all;\">"
                   "<div style=\"background-color: white; z-index: 999999; position: fixed; top: 50%; left: 50%; "
                   "transform: translate(-50%, -50%); padding: 20px; border-radius: 6px; box-shadow: rgb(166, 166, 166) 0px 9px 0px 0px; "
                   "width: 450px; display: flex; text-align: center; flex-direction: column; justify-content: center; "
                   "align-items: center; pointer-events: all;\">"
                   "<div style=\"font-size: 32px; text-transform: uppercase; margin-bottom: 10px;\">Security Challenge</div>"
                   "<div style=\"color: rgba(0, 0, 0, 0.6); font-size: 20px; margin-bottom: 10px;\">Please complete the security challenge to continue.</div>"
                   "<div>");
    sb_append(&sb, widget);
    sb_append(&sb, "</div></div></div>");
    free(widget);
    char *html = sb_finish(&sb);
    html_dom_insert_html(html_dom_body(), HTML_INSERT_BEFORE_END, html);
    free(html);
    if (alive) M.alive_timer = html_timer_add(ALTCHA_ALIVE_MS, alive_fire, NULL);
    KN_LOG_DEBUG1("[captcha] security challenge card shown%s\n", hide_on_solve ? " (hidden once solved)" : "");
    if (auto_click()) html_timer_add(0.0, auto_check, NULL);
    return 1;
}
