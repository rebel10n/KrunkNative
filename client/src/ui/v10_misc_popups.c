// Krunker v10.0.2's Svelte popups the port never built (v10 bundle: game.deob.js, svelte.css), 1:1 from the
// templates (the _4461 strings), with the Svelte events as inline handlers (knMisc*):
//
//   survey          svelte-1u0qxrf (v10:116796-116895), pushed as a notification item with an expiration bar
//                   (the GAME_SURVEY handler, v10:210359, its notification store v10:118762), in #notificationCenter.
//                   Backend: none. kn_brain has no survey route and the servers never send GAME_SURVEY, so it
//                   shows only when v10_misc_survey_show() is called (e2e, scenarios); answering would be the
//                   SURVEY_ANSWER rpc, which nothing answers, so the buttons stay inert.
//   mission briefing  svelte-1849bws (v10:118656-118790), the guest "Operation: Ghost Recruit" modal in the view
//                   stack: 30 s timeout, KRUNKIES / XP earned (capacity warnings at 20 KR / 2500 XP), ABORT,
//                   DEPLOY NOW (-> loginOrRegister(true)). v10.0.2's manager (v10:118812) has config.enabled
//                   = false and nothing ever enables it, so v10 never shows it: here knMiscOpen("hook") only.
//   k-ID dialog     svelte-lbi007 (v10:118640, mounted v10:223353): the GDC 2025 intro, shown when the display
//                   mode is "gdc-kid" (page ?displayMode=gdc-kid, else the saved krunker-display-mode) and the
//                   player is a guest; fly y:20 over 500 ms. Register -> loginOrRegister(). Inert otherwise (no
//                   k-ID backend; v10's own wipe of localStorage + reload on first use is not ported).
//   lottery         svelte-1rj22es (v10:127123-127780): window.openLottery(id = 3) -> genericPopup "defaultModal
//                   lotteryModal"; themes christmas (light bulbs 1fenhdo) / easter (eggs gt9grt) / neutral
//                   (gifts wjqbtw), the quantity steppers (jsgxg6), winners (14vp3ai). Data: GET /api/lottery/<id>
//                   and POST /api/lottery/<id>/participate on the brain. The brain has no lottery yet: the request
//                   fails and the popup shows v10's loading box with the error, as v10 does when its API fails.
//   premium shop    svelte-190e6fj (v10:129616+, cancel confirm 18wpngg): the Store Premium tab's subscription
//                   branch, which v10.0.2 hides behind `() => !0x0` (the four legacy cards show instead), so v10
//                   never shows it either: knMiscOpen("premium") only. Products / subscriptions: none (no shop).
//   usernames       svelte-27udiy (v10:114613-115040): manageAccNames(), the Account window's Manage button. The
//                   list is the account's own username (state active, never expires); the buy flow ($9.99,
//                   renew $4.99) asks for the confirmation and then answers that purchases are unavailable.
//
// Everything is mounted through the shared popup API (client/include/v10_ui.h) or, for the k-ID dialog (top
// layer) and the survey (the toast stack, .notification-list), as elements of its own. Scenario state:
// state.community.popups (v10_misc_popups_scenario).
#include <v10_misc_popups.h>
#include <v10_ui.h>
#include <desktop_notify.h>
#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <i18n.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define timegm _mkgmtime // MinGW has no timegm (same as account_ui.c, editor_net.c)
#endif

#include "../viewer/viewer_store.h"

static double now_s(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static double mono_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec * 1e3 + (double) ts.tv_nsec / 1e6;
}

static struct {
    int scenario;  // --render-scenario: countdowns stand still
    int css;
} G;

static int arg_int(const HtmlArg *args, const int argc, const int i, const int def) {
    return i < argc && args[i].kind == HTML_ARG_NUM ? (int) args[i].num : def;
}

static const char *arg_str(const HtmlArg *args, const int argc, const int i) {
    return i < argc && args[i].kind == HTML_ARG_STR ? args[i].str : "";
}

// a name as an inline handler's string argument: the characters a username carries (the brain's names are
// letters, digits, space, _ . -); anything else is dropped, not escaped
static void js_name(char *out, const size_t n, const char *name) {
    size_t k = 0;
    for (const char *p = name; *p && k + 1 < n; p++) {
        if (isalnum((unsigned char) *p) || *p == '_' || *p == '-' || *p == '.' || *p == ' ') out[k++] = *p;
    }
    out[k] = 0;
}

static const char *tr(const char *key, const char *english) { return I18N(key, english); }

// ================================================================== survey

#define SURVEY_MAX_ANSWERS 6

static struct {
    int active, expired, answered;
    char question[256];
    char answers[SURVEY_MAX_ANSWERS][96];
    int n_answers;
    int reward;
    double expires_s, duration_s, frozen_left;
    double last_ms;
} SV;

static int survey_left(void) {
    const double left = SV.frozen_left >= 0 ? SV.frozen_left : SV.expires_s - now_s();
    return left <= 0 ? 0 : (int) floor(left);
}

static void survey_time_text(char *out, const size_t n) {
    char arg[32];
    const int left = survey_left();
    if (left <= 0) {
        snprintf(out, n, "%s", tr("generic.survey.expired", "Survey expired"));
        return;
    }
    snprintf(arg, sizeof(arg), "%d:%02d", left / 60, left % 60);
    char text[96];
    I18N_F(text, "generic.survey.timeRemaining", "Time remaining: {0}", arg);
    snprintf(out, n, "%s", text);
}

static double survey_pct(void) {
    if (SV.duration_s <= 0) return 0;
    const double left = SV.frozen_left >= 0 ? SV.frozen_left : SV.expires_s - now_s();
    return left <= 0 ? 0 : fmin(100.0, left / SV.duration_s * 100.0);
}

// The survey is a notification item of v10's notification center (the store behind GAME_SURVEY's
// showNotification). v10's #notificationCenter sits in the chat holder (index.html: #chatUIHolder > #chatUI >
// #chatHolder > #notificationCenter), so the survey shows where the chat does (a signed-in account, in game),
// never for a guest on the menu. The .notification-center carries `hidden` until a notification arrives.
static HtmlElem survey_center(void) {
    return html_dom_query(html_dom_by_id("notificationCenter"), ".notification-center");
}

static HtmlElem survey_holder(void) {
    HtmlElem h = html_dom_by_id("knSurveyHolder");
    if (h) return h;
    HtmlElem center = survey_center();
    if (!center) return 0;
    html_dom_insert_html(center, HTML_INSERT_BEFORE_END, "<div id=\"knSurveyHolder\"></div>");
    return html_dom_by_id("knSurveyHolder");
}

static void survey_render(void) {
    HtmlElem h = survey_holder();
    if (!h) return;
    HtmlElem center = survey_center();
    if (!SV.active) {
        html_dom_set_html(h, "");
        if (center) html_dom_set_class(center, "hidden", 1);
        return;
    }
    if (center) html_dom_set_class(center, "hidden", 0);
    char time[96], amount[16];
    survey_time_text(time, sizeof(time));
    snprintf(amount, sizeof(amount), "%d", SV.reward);
    StrBuf sb = {0};
    // the notification item (v10 _4461 notification-item): the component and its expiration bar
    sb_append(&sb, "<div class=\"notification-item svelte-nrigjp\"><div class=\"survey-notification svelte-1u0qxrf\">"
                   "<div class=\"notification-header svelte-1u0qxrf\"><div class=\"header-left svelte-1u0qxrf\">"
                   "<div class=\"notification-icon material-icons svelte-1u0qxrf\">quiz</div> "
                   "<div class=\"header-text svelte-1u0qxrf\"><div class=\"notification-title svelte-1u0qxrf\">");
    sb_append_esc(&sb, tr("generic.survey.title", "Survey"));
    sb_append(&sb, "</div> <div class=\"time-remaining svelte-1u0qxrf\" id=\"knSurveyTime\">");
    sb_append_esc(&sb, time);
    sb_append(&sb, "</div></div></div> <button class=\"dismiss-btn svelte-1u0qxrf\" aria-label=\"Close notification\" "
                   "onclick=\"knMiscSurveyDismiss()\">\xe2\x9c\x95</button></div> <div class=\"notification-content svelte-1u0qxrf\">"
                   "<div class=\"question-section svelte-1u0qxrf\"><p class=\"question-text svelte-1u0qxrf\">");
    sb_append_esc(&sb, SV.question);
    sb_append(&sb, "</p></div> <div class=\"answers-section svelte-1u0qxrf\">");
    for (int i = 0; i < SV.n_answers; i++) {
        sb_appendf(&sb, "<button class=\"answer-btn svelte-1u0qxrf%s\"%s onmouseenter=\"playTick(0.1)\" onclick=\"knMiscSurveyAnswer(%d)\"> ",
                   SV.answered ? " answered" : "", SV.answered ? " disabled" : "", i);
        sb_append_esc(&sb, SV.answers[i]);
        sb_append(&sb, "</button>");
    }
    sb_append(&sb, "</div> ");
    if (SV.answered) {
        char msg[160];
        I18N_F(msg, "generic.survey.answerSubmitted", "Answer submitted! {0} KR reward will be added to your account.", amount);
        sb_append(&sb, "<div class=\"success-message svelte-1u0qxrf\"><div class=\"success-icon material-icons svelte-1u0qxrf\">check_circle</div> "
                       "<div class=\"success-text svelte-1u0qxrf\">");
        sb_append_esc(&sb, msg);
        sb_append(&sb, "</div></div>");
    } else {
        sb_append(&sb, "<!---->");
    }
    sb_appendf(&sb, "</div></div> <div class=\"expiration-bar svelte-16a8kon%s\" id=\"knSurveyBarWrap\"><div class=\"expiration-bar-track svelte-16a8kon\">"
                    "<div class=\"expiration-bar-fill svelte-16a8kon\" id=\"knSurveyBar\" style=\"width: %.17g%%\"></div></div></div></div>",
               SV.expired ? " expired" : "", survey_pct());
    char *html = sb_finish(&sb);
    html_dom_set_html(h, html);
    free(html);
}

void v10_misc_survey_clear(void) {
    if (!SV.active) return;
    memset(&SV, 0, sizeof(SV));
    SV.frozen_left = -1;
    survey_render();
}

void v10_misc_survey_show(const cJSON *s) {
    if (!cJSON_IsObject(s)) return;
    const char *id = v10_str(s, "surveyId");
    if (!strcmp(id, "CLEAR_SURVEY")) { // v10:210359
        v10_misc_survey_clear();
        return;
    }
    memset(&SV, 0, sizeof(SV));
    SV.frozen_left = -1;
    SV.active = 1;
    snprintf(SV.question, sizeof(SV.question), "%s", v10_str(s, "question"));
    const cJSON *answers = cJSON_GetObjectItem(s, "answers"), *a;
    cJSON_ArrayForEach(a, answers) {
        if (SV.n_answers < SURVEY_MAX_ANSWERS && cJSON_IsString(a)) snprintf(SV.answers[SV.n_answers++], sizeof(SV.answers[0]), "%s", a->valuestring);
    }
    SV.reward = (int) v10_num(s, "rewardAmount", 10); // v10:210376: rewardAmount || 10
    if (SV.reward <= 0) SV.reward = 10;
    SV.expires_s = v10_num(s, "expiresAt", now_s() + 120);
    SV.duration_s = SV.expires_s - floor(now_s()); // the toast's duration: 1000 * (expiresAt - now)
    if (G.scenario && cJSON_IsNumber(cJSON_GetObjectItem(s, "secondsLeft"))) {
        SV.frozen_left = v10_num(s, "secondsLeft", 120);
        SV.duration_s = v10_num(s, "durationSeconds", SV.frozen_left);
    }
    SV.last_ms = mono_ms();
    survey_render();
    desktop_notify_item(tr("generic.survey.title", "Survey"), SV.question); // once per survey, as the centre's item
}

static void survey_tick(void) {
    if (!SV.active || SV.frozen_left >= 0) return;
    const double now = mono_ms();
    if (now - SV.last_ms < 100) return;
    SV.last_ms = now;
    if (SV.duration_s > 0 && SV.expires_s - now_s() <= -0.5) { // the toast's duration ran out: removed
        v10_misc_survey_clear();
        return;
    }
    char time[96];
    survey_time_text(time, sizeof(time));
    HtmlElem t = html_dom_by_id("knSurveyTime"), bar = html_dom_by_id("knSurveyBar"), wrap = html_dom_by_id("knSurveyBarWrap");
    if (t) html_dom_set_text(t, time);
    if (bar) {
        char w[32];
        snprintf(w, sizeof(w), "%.17g%%", survey_pct());
        html_dom_set_style(bar, "width", w);
    }
    if (wrap && !SV.expired && survey_left() <= 0) {
        SV.expired = 1;
        html_dom_set_class(wrap, "expired", 1);
    }
}

// ================================================================== mission briefing

static struct {
    int open, secs;
    double kr, score;
    double last_ms;
    int frozen;
} HK;

static void hook_html(StrBuf *sb) {
    char kr[32], xp[32];
    v10_fmt_int(kr, sizeof(kr), HK.kr);
    v10_fmt_int(xp, sizeof(xp), HK.score);
    sb_append(sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 1100px;\">"
                  "<div class=\"mission-briefing svelte-1849bws\"><div class=\"mission-header svelte-1849bws\">"
                  "<div class=\"classified-banner svelte-1849bws\"><span class=\"material-icons svelte-1849bws\">security</span> "
                  "<span class=\"svelte-1849bws\">CLASSIFIED INTEL</span> <span class=\"material-icons svelte-1849bws\">security</span></div> "
                  "<div class=\"header-content svelte-1849bws\"><div class=\"mission-title svelte-1849bws\"><h1 class=\"svelte-1849bws\">OPERATION: GHOST RECRUIT</h1> "
                  "<div class=\"mission-subtitle svelte-1849bws\">Your deployment window is closing</div></div> ");
    sb_appendf(sb, "<div class=\"mission-timer svelte-1849bws%s\" id=\"knHookTimerBox\"><div class=\"timer-label svelte-1849bws\">MISSION TIMEOUT</div> "
                   "<div class=\"timer-value svelte-1849bws\" id=\"knHookTimer\">%02d:%02d</div> "
                   "<div class=\"timer-warning svelte-1849bws\"><span class=\"material-icons svelte-1849bws\">warning</span> Data will be lost after timeout</div></div></div></div> ",
               HK.secs <= 10 ? " critical" : "", HK.secs / 60, HK.secs % 60);
    sb_append(sb, "<div class=\"mission-content svelte-1849bws\"><div class=\"intel-section svelte-1849bws\"><div class=\"intel-header svelte-1849bws\">"
                  "<span class=\"material-icons svelte-1849bws\">assessment</span> <h3 class=\"svelte-1849bws\">FIELD REPORT</h3></div> "
                  "<div class=\"intel-stats svelte-1849bws\">");
    if (HK.kr > 0) {
        const int full = HK.kr >= 20; // v10:16544
        sb_appendf(sb, "<div class=\"stat-item svelte-1849bws%s\"><div class=\"stat-icon svelte-1849bws\"><img src=\"/img/kr_icon_angled.svg\" alt=\"KR\" class=\"svelte-1849bws\"></div> "
                       "<div class=\"stat-data svelte-1849bws\"><div class=\"stat-value svelte-1849bws\">%s</div> <div class=\"stat-label svelte-1849bws\">KRUNKIES EARNED</div> %s</div></div>",
                   full ? " critical" : "", kr, full ? "<div class=\"stat-warning svelte-1849bws\">CAPACITY EXCEEDED</div>" : "<!---->");
    } else {
        sb_append(sb, "<!---->");
    }
    sb_append(sb, " ");
    if (HK.score > 0) {
        const int full = HK.score >= 2500;
        sb_appendf(sb, "<div class=\"stat-item svelte-1849bws%s\"><div class=\"stat-icon xp svelte-1849bws\"><span class=\"svelte-1849bws\">XP</span></div> "
                       "<div class=\"stat-data svelte-1849bws\"><div class=\"stat-value svelte-1849bws\">%s</div> <div class=\"stat-label svelte-1849bws\">EXPERIENCE POINTS</div> %s</div></div>",
                   full ? " critical" : "", xp, full ? "<div class=\"stat-warning svelte-1849bws\">CAPACITY EXCEEDED</div>" : "<!---->");
    } else {
        sb_append(sb, "<!---->");
    }
    sb_append(sb, "</div> <div class=\"intel-warning svelte-1849bws\"><span class=\"material-icons svelte-1849bws\">error</span> <div class=\"svelte-1849bws\">"
                  "<div class=\"warning-title svelte-1849bws\">CRITICAL ALERT</div> <div class=\"warning-text svelte-1849bws\">All progress will be PERMANENTLY LOST without deployment</div></div></div></div> "
                  "<div class=\"deployment-section svelte-1849bws\"><div class=\"deployment-header svelte-1849bws\"><span class=\"material-icons svelte-1849bws\">military_tech</span> "
                  "<h3 class=\"svelte-1849bws\">DEPLOYMENT BENEFITS</h3></div> <div class=\"benefits-grid svelte-1849bws\">");
    static const char *const benefits[4][3] = {
        {"shield", "PERMANENT STORAGE", "Secure all earned rewards forever"},
        {"workspace_premium", "EXCLUSIVE COSMETICS", "14 premium skins + special items"},
        {"star", "PREMIUM ACCESS", "24-hour elite status activation"},
        {"leaderboard", "RANKED PROGRESSION", "Access competitive matchmaking"},
    };
    for (int i = 0; i < 4; i++)
        sb_appendf(sb, "%s<div class=\"benefit-item svelte-1849bws\"><div class=\"benefit-icon svelte-1849bws\"><span class=\"material-icons svelte-1849bws\">%s</span></div> "
                       "<div class=\"benefit-text svelte-1849bws\"><div class=\"benefit-title svelte-1849bws\">%s</div> <div class=\"benefit-desc svelte-1849bws\">%s</div></div></div>",
                   i ? " " : "", benefits[i][0], benefits[i][1], benefits[i][2]);
    sb_append(sb, "</div></div></div> <div class=\"mission-actions svelte-1849bws\">"
                  "<button class=\"abort-btn svelte-1849bws\" onclick=\"knMiscHookAbort()\"><span class=\"material-icons svelte-1849bws\">close</span> ABORT MISSION</button> "
                  "<button class=\"deploy-btn svelte-1849bws\" onclick=\"knMiscHookDeploy()\"><span class=\"material-icons svelte-1849bws\">rocket_launch</span> DEPLOY NOW "
                  "<div class=\"deploy-subtitle svelte-1849bws\">Secure your rewards</div></button></div> "
                  "<div class=\"scan-line svelte-1849bws\"></div> <div class=\"grid-overlay svelte-1849bws\"></div></div></div></div>");
}

static void hook_closed(const int accepted, void *user) {
    (void) accepted;
    (void) user;
    HK.open = 0; // popped (ABORT, the overlay, the timeout)
}

static void hook_open(const int secs) {
    HK.open = 1;
    HK.secs = secs > 0 ? secs : 30; // v10: let secs = 30
    HK.last_ms = mono_ms();
    StrBuf sb = {0};
    hook_html(&sb);
    v10_view_push(sb_finish(&sb), NULL, hook_closed, NULL);
}

void v10_misc_guest_progress(const double kr, const double score) {
    HK.kr = kr;
    HK.score = score;
}

static void hook_tick(void) {
    if (!HK.open || HK.frozen) return;
    const double now = mono_ms();
    if (now - HK.last_ms < 1000) return;
    HK.last_ms += 1000;
    HK.secs--;
    if (HK.secs <= 0) {
        v10_view_pop(0);
        return;
    }
    char t[16];
    snprintf(t, sizeof(t), "%02d:%02d", HK.secs / 60, HK.secs % 60);
    HtmlElem el = html_dom_by_id("knHookTimer"), box = html_dom_by_id("knHookTimerBox");
    if (el) html_dom_set_text(el, t);
    if (box) html_dom_set_class(box, "critical", HK.secs <= 10);
}

// ================================================================== k-ID dialog

static struct {
    int open;
} KID;

static void kid_close(void) {
    HtmlElem layer = html_dom_by_id("knKidLayer");
    if (layer) html_dom_remove(layer);
    KID.open = 0;
}

static void kid_open(void) {
    if (KID.open) return;
    HtmlElem body = html_dom_body();
    if (!body) return;
    StrBuf sb = {0};
    // <dialog class="kid-display-dialog"> in the top layer: a fixed layer with its ::backdrop
    // (rgba(0,0,0,0.8) + blur(5px)) and the dialog centred in it (css/kn-v10-popups.css)
    sb_append(&sb, "<div class=\"kn-kid-layer\" id=\"knKidLayer\"><div class=\"kid-display-dialog svelte-lbi007 kn-kid-fly\" open=\"\">"
                   "<div class=\"kid-display-container svelte-lbi007\"><div class=\"kid-display-header svelte-lbi007\">"
                   "<div class=\"logo-container svelte-lbi007\"><div class=\"krunker-logo svelte-lbi007\">KRUNKER</div> <div class=\"partnership-text svelte-lbi007\">with</div> "
                   "<div class=\"kid-logo svelte-lbi007\">k-ID</div></div> <h2 class=\"svelte-lbi007\">");
    sb_append_esc(&sb, tr("gdc.kid.subtitle", "Experience the Future of Gaming at GDC 2025"));
    sb_append(&sb, "</h2></div> <div class=\"kid-display-content svelte-lbi007\"><div class=\"kid-display-left svelte-lbi007\">"
                   "<div class=\"kid-display-description svelte-lbi007\"><p class=\"svelte-lbi007\">");
    sb_append_esc(&sb, tr("gdc.kid.description", "Join thousands of players in the ultimate FPS experience. Create your Krunker account with k-ID integration to enjoy a safe, compliant gaming experience!"));
    sb_append(&sb, "</p></div> <div class=\"kid-display-benefits svelte-lbi007\"><h3 class=\"svelte-lbi007\">");
    sb_append_esc(&sb, tr("gdc.kid.benefits.title", "Benefits of k-ID Integration"));
    sb_append(&sb, "</h3> <ul class=\"svelte-lbi007\">");
    // v10's icons are the text emoji U+1F6E1 U+FE0F, U+2705, U+1F512 and U+1F46A (v10:117081), which Chrome draws
    // with Noto Color Emoji; the engine has no colour emoji font, so they are that font's artwork as images
    // (ui/img/kn/emoji/, the same rasters ranked_ui.c's emoji() draws; ui/README.md)
    static const struct { const char *emoji, *key, *english; } benefits[4] = {
        {"1f6e1-fe0f", "gdc.kid.benefits.safety", "Safe, age-appropriate gaming environment"},
        {"2705", "gdc.kid.benefits.compliance", "Global regulatory compliance built-in"},
        {"1f512", "gdc.kid.benefits.privacy", "Privacy protection for younger players"},
        {"1f46a", "gdc.kid.benefits.parental", "Simplified parental controls and oversight"},
    };
    for (int i = 0; i < 4; i++) {
        sb_appendf(&sb, "%s<li class=\"svelte-lbi007\"><div class=\"benefit-icon svelte-lbi007\"><img class=\"kn-emoji kid-emoji\" src=\"./img/kn/emoji/%s.png\" alt=\"\"></div> <div class=\"benefit-text svelte-lbi007\">", i ? " " : "", benefits[i].emoji);
        sb_append_esc(&sb, tr(benefits[i].key, benefits[i].english));
        sb_append(&sb, "</div></li>");
    }
    sb_append(&sb, "</ul></div> <div class=\"partnership-info svelte-lbi007\"><div class=\"partnership-icon svelte-lbi007\">"
                   // v10's inline <svg> (24 x 24, the info circle): the engine draws no inline SVG, so it is
                   // the same document as an image (the pattern of v10_news.c's Twitch icon)
                   "<img class=\"svelte-lbi007\" width=\"24\" height=\"24\" alt=\"\" src=\"data:image/svg+xml,%3Csvg width='24' height='24' viewBox='0 0 24 24' fill='none' "
                   "xmlns='http://www.w3.org/2000/svg'%3E%3Cpath d='M12 2C6.48 2 2 6.48 2 12C2 17.52 6.48 22 12 22C17.52 22 22 17.52 22 12C22 6.48 17.52 2 12 2ZM12 20C7.59 20 "
                   "4 16.41 4 12C4 7.59 7.59 4 12 4C16.41 4 20 7.59 20 12C20 16.41 16.41 20 12 20ZM11 7H13V9H11V7ZM11 11H13V17H11V11Z' fill='white'/%3E%3C/svg%3E\"></div> <p class=\"svelte-lbi007\">");
    sb_append_esc(&sb, tr("gdc.kid.partnership.text", "k-ID provides age verification and compliance technology that keeps Krunker safe for players of all ages"));
    sb_append(&sb, "</p></div></div> <div class=\"kid-display-right svelte-lbi007\"><div class=\"kid-display-character svelte-lbi007\"><div class=\"guide-gif svelte-lbi007\"></div></div> "
                   "<div class=\"kid-display-buttons svelte-lbi007\"><button class=\"kid-button kid-button-register svelte-lbi007\" onclick=\"knMiscKidRegister()\">");
    sb_append_esc(&sb, tr("gdc.kid.register", "Create Account And Play"));
    sb_append(&sb, "</button></div></div></div> <div class=\"kid-display-footer svelte-lbi007\"><p class=\"svelte-lbi007\">");
    sb_append_esc(&sb, tr("gdc.kid.footer.text", "Krunker with k-ID provides safe, compliant gaming for all ages"));
    sb_append(&sb, "</p> <div class=\"gdc-badge svelte-lbi007\">GDC 2025</div></div></div></div></div>");
    char *html = sb_finish(&sb);
    html_dom_insert_html(body, HTML_INSERT_BEFORE_END, html);
    free(html);
    KID.open = 1;
}

// v10:118600-118630: displayMode = ?displayMode= (when it is a known mode), else the saved krunker-display-mode
static int display_mode_kid(void) {
    char q[512] = "", saved[32] = "";
    if (html_page_query(q, sizeof(q)) >= 0) {
        const char *p = strstr(q, "displayMode=");
        if (p) return !strncmp(p + 12, "gdc-kid", 7) && (p[19] == 0 || p[19] == '&');
    }
    return html_storage_get("krunker-display-mode", saved, sizeof(saved)) >= 0 && !strcmp(saved, "gdc-kid");
}

// ================================================================== lottery

#define LOT_MAX_COSTS 6

static struct {
    int id;
    int qty[LOT_MAX_COSTS];
    int submitting;
    char error[200];
    int frozen;               // --render-scenario: the draw's time left stands still (popups.lotterySecondsLeft)
    double frozen_left;
    int timer;                // the view's deadline (html_timer id, 0 none): no answer by then fails the window
    int timed_out;
} LT;

// the lottery view has a deadline: no answer by then (the brain is gone, or it stalls) shows the failure with Retry
#define LOT_WAIT_MS 15000

// the lottery's brain route, with the account's session token in the URL (v10's ?token=, as its API client sends it;
// a guest's is empty and the brain answers the guest's view)
static char *lot_path(char *out, const size_t n, const char *suffix) {
    char t[256];
    v10_url_token(t, sizeof t);
    snprintf(out, n, "/api/lottery/%d%s%s%s", LT.id, suffix, t[0] ? "?token=" : "", t);
    return out;
}

static const char *cur_name(const char *ref) { return !strcmp(ref, "1") ? "KR" : !strcmp(ref, "2") ? "JNK" : !strcmp(ref, "3") ? "RP" : "KR"; }

// the account's available quantity of a cost (the skin count in data[10], or the KR balance)
static int lot_available(const cJSON *cost) {
    const AccountInfo *acc = active_account();
    if (!acc) return 0;
    const char *type = v10_str(cost, "cost_type");
    const int ref = (int) v10_num(cost, "cost_ref", 0);
    if (!strcmp(type, "skin")) {
        const cJSON *inventory = acc->data ? cJSON_GetArrayItem(acc->data, 10) : NULL, *e;
        cJSON_ArrayForEach(e, inventory) {
            if ((int) v10_num(e, "ind", -1) == ref) return (int) v10_num(e, "cnt", 1);
        }
        return 0;
    }
    if (!strcmp(type, "currency") && ref == 1) return (int) acc->funds;
    return 0;
}

// a prize / cost item as v10 shows it: the item card, or the currency display
static void lot_item(StrBuf *sb, const char *type, const char *ref, const double quantity, const int card, const char *children, const char *fallback) {
    if (!strcmp(type, "skin")) {
        const int index = atoi(ref);
        if (viewer_skin(index)) {
            v10_market_card(sb, index, card, card, "top-center", NULL, children);
            return;
        }
    } else if (!strcmp(type, "currency")) {
        v10_currency(sb, quantity, cur_name(ref), "large", 0);
        return;
    }
    sb_append(sb, "<div>");
    sb_append_esc(sb, fallback);
    sb_append(sb, "</div>");
}

static const char *skin_name(const char *ref) {
    const ViewerSkin *skin = viewer_skin(atoi(ref));
    return skin && skin->name && skin->name[0] ? skin->name : "Item";
}

static void lot_decoration(StrBuf *sb, const char *kind, const int salt) {
    // deterministic stand-ins for Math.random() (v10 picks colours / tilt / bob delay per instance)
    static const char *const eggs[] = {"#FFB3BA", "#BAFFC9", "#BAE1FF", "#FFFFBA", "#E8BAFF", "#FFD9BA", "#BAFFEE", "#FFC9DE", "#C9BAFF"};
    static const char *const bands[] = {"#FF6B6B", "#4ECDC4", "#FFE66D", "#A8E6CF", "#DDA0DD", "#FF8C42"};
    static const char *const boxes[] = {"#C0392B", "#2980B9", "#27AE60", "#8E44AD", "#D35400", "#16A085", "#C0A062"};
    static const char *const ribbons[] = {"#F1C40F", "#ECF0F1", "#E74C3C", "#F39C12"};
    static const char *const bulbs[] = {"#ff0000", "#00ff00", "#ffff00", "#ff6b6b", "#4ecdc4", "#ffe66d", "#ff8c42", "#a8e6cf", "#ffd93d", "#ff6b9d"};
    const unsigned h = (unsigned) salt * 2654435761u;
    const double tilt = (double) ((h >> 8) % 1000) / 1000.0, bob = (double) ((h >> 4) % 1000) / 1000.0 * 3.0;
    if (!strcmp(kind, "lightbulbs")) {
        sb_appendf(sb, "<div class=\"light-bulb svelte-1fenhdo\" style=\"--bulb-color: %s; --opacity: 1; --tilt-angle: %.3fdeg;\"><div class=\"connection svelte-1fenhdo\"></div> <div class=\"bulb svelte-1fenhdo\"></div></div>",
                   bulbs[(h >> 12) % 10], 60.0 * tilt - 30.0);
    } else if (!strcmp(kind, "eggs")) {
        sb_appendf(sb, "<div class=\"easter-egg svelte-gt9grt\" style=\"--egg-color: %s; --band-color: %s; --tilt: %.3fdeg; --bob-delay: %.3fs;\"><div class=\"egg svelte-gt9grt\"><div class=\"band svelte-gt9grt\"></div></div></div>",
                   eggs[(h >> 12) % 9], bands[(h >> 16) % 6], 40.0 * tilt - 20.0, bob);
    } else {
        sb_appendf(sb, "<div class=\"gift svelte-wjqbtw\" style=\"--box-color: %s; --ribbon-color: %s; --tilt: %.3fdeg; --bob-delay: %.3fs;\"><div class=\"box svelte-wjqbtw\"><div class=\"ribbon-h svelte-wjqbtw\"></div> "
                       "<div class=\"ribbon-v svelte-wjqbtw\"></div></div> <div class=\"bow svelte-wjqbtw\"><div class=\"bow-loop bow-left svelte-wjqbtw\"></div> <div class=\"bow-loop bow-right svelte-wjqbtw\"></div> "
                       "<div class=\"bow-knot svelte-wjqbtw\"></div></div></div>",
                   boxes[(h >> 12) % 7], ribbons[(h >> 16) % 4], 30.0 * tilt - 15.0, bob);
    }
}

static const cJSON *lot_data(const V10Req **req_out) {
    char path[512];
    const V10Req *r = v10_get(lot_path(path, sizeof(path), ""), 30000);
    if (req_out) *req_out = r;
    if (!r || r->status != 200 || !r->json) return NULL;
    const cJSON *data = cJSON_GetObjectItem(r->json, "data");
    return cJSON_IsObject(data) ? data : r->json; // v10 stores answer.data
}

static void lot_stepper(StrBuf *sb, const int i, const int value, const int max, const int step, const int disabled) {
    sb_appendf(sb, "<div class=\"quantity-selector svelte-jsgxg6\"><button class=\"quantity-button svelte-jsgxg6\"%s onmouseenter=\"playTick(0.1)\" onclick=\"knMiscLotQty(%d,-1)\">-</button> "
                   "<div class=\"quantity-display svelte-jsgxg6\"><div class=\"quantity-number svelte-jsgxg6\">%d</div> <!----></div> "
                   "<button class=\"quantity-button svelte-jsgxg6\"%s onmouseenter=\"playTick(0.1)\" onclick=\"knMiscLotQty(%d,1)\">+</button></div>",
               disabled || value <= 0 ? " disabled" : "", i, value, disabled || value >= max ? " disabled" : "", i);
    (void) step;
}

static char *lot_html(void) {
    const V10Req *req = NULL;
    const cJSON *lottery = lot_data(&req);
    StrBuf sb = {0};
    if (!lottery) {
        // v10's loading box (the window's !lottery branch): "Loading..." while the view is asked for, else the failure
        // with Retry: the brain refused it (an error status, or an answer with no lottery in it), it could not be
        // reached, or no answer came by LOT_WAIT_MS (LT.timed_out)
        const int failed = LT.timed_out || LT.error[0] || (req && req->status != 0);
        sb_append(&sb, "<div class=\"lottery-window-loading svelte-1rj22es\"><div class=\"window-container svelte-623zut\"><div>");
        if (failed) {
            sb_append(&sb, "<div>");
            sb_append_esc(&sb, LT.error[0] ? LT.error : I18N("kn.lottery.loadFailed", "Couldn't load the lottery."));
            sb_appendf(&sb, "</div> <button id=\"knLotRetry\" class=\"enter-lottery-button svelte-1rj22es\" onmouseenter=\"playTick(0.1)\" onclick=\"knMiscLotRetry()\">%s</button>",
                       I18N("kn.retry", "Retry"));
        } else {
            sb_append(&sb, "Loading...");
        }
        sb_append(&sb, "</div></div></div>");
        return sb_finish(&sb);
    }
    LT.timed_out = 0;
    const cJSON *info = cJSON_GetObjectItem(lottery, "lottery"), *draw = cJSON_GetObjectItem(lottery, "current_draw");
    const cJSON *costs = cJSON_GetObjectItem(lottery, "costs"), *prev = cJSON_GetObjectItem(lottery, "previous_draws");
    if (!info) info = lottery; // answers shaped {id, title, theme, current_draw, costs, ...}
    if (!draw) draw = cJSON_GetObjectItem(info, "current_draw");
    if (!costs) costs = cJSON_GetObjectItem(info, "costs");
    if (!prev) prev = cJSON_GetObjectItem(info, "previous_draws");
    const char *theme = v10_str(info, "theme"), *title = v10_str(info, "title");
    if (strcmp(theme, "christmas") && strcmp(theme, "easter")) theme = "neutral";
    const char *dec = !strcmp(theme, "christmas") ? "lightbulbs" : !strcmp(theme, "easter") ? "eggs" : "gifts";
    const int n_border = !strcmp(theme, "christmas") ? 30 : 24, n_sep = !strcmp(theme, "christmas") ? 12 : 10;
    sb_appendf(&sb, "<div class=\"lottery-window svelte-1rj22es theme-%s\">", theme);
    for (int i = 0; i < n_border; i++) { // the border: a rectangle walked clockwise
        const double p = (double) i / n_border * 400.0;
        const double x = p <= 100 ? p : p <= 200 ? 100 : p <= 300 ? 100 - (p - 200) : 0, y = p <= 100 ? 0 : p <= 200 ? p - 100 : p <= 300 ? 100 : 100 - (p - 300);
        sb_appendf(&sb, "<div class=\"decoration-container svelte-1rj22es\" style=\"--dec-x: %.17g%%; --dec-y: %.17g%%;\">", x, y);
        lot_decoration(&sb, dec, i + 1);
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, " <div class=\"window-container svelte-623zut\"><div class=\"lottery-title svelte-1rj22es\">");
    sb_append_esc(&sb, title[0] ? title : "Raffle");
    sb_append(&sb, "</div> <div class=\"title-separator svelte-1rj22es\">");
    for (int i = 0; i < n_sep; i++) {
        sb_appendf(&sb, "<div class=\"separator-decoration-container svelte-1rj22es\" style=\"--separator-dec-x: %.17g%%;\">", (double) i / (n_sep - 1) * 100.0);
        lot_decoration(&sb, dec, 100 + i);
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "</div> ");
    if (draw) {
        const char *ends_at = v10_str(draw, "ends_at");
        int y, mo, d, h = 0, mi = 0, s = 0;
        double ends_s = 0;
        if (sscanf(ends_at, "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) >= 3) {
            struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = s};
            ends_s = (double) timegm(&tm);
        }
        const double left = LT.frozen ? LT.frozen_left : ends_s - now_s();
        const int open = left > 0, drawing = !open && !cJSON_IsTrue(cJSON_GetObjectItem(draw, "drawn_at")) && !v10_str(draw, "drawn_at")[0];
        const int frequency = (int) v10_num(info, "frequency", 0);
        const int n_prizes = cJSON_GetArraySize(cJSON_GetObjectItem(draw, "prizes"));
        const int extra = n_prizes > 1 ? n_prizes - 1 : 0;
        sb_append(&sb, "<div class=\"lottery-container svelte-1rj22es\"><div class=\"left-column svelte-1rj22es\"><div class=\"description svelte-1rj22es\">Earn materials by eliminating players in-game. Enter the ");
        sb_append_esc(&sb, title[0] ? title : "Raffle");
        sb_append(&sb, " for a chance at prizes!");
        if (frequency > 0) {
            sb_appendf(&sb, "  <span class=\"description-muted svelte-1rj22es\"> A new draw happens every %d %s, with 1 grand prize winner and up to %d %s receiving additional prizes.</span>",
                       frequency, frequency == 1 ? "hour" : "hours", extra, extra == 1 ? "extra winner" : "extra winners");
        }
        sb_append(&sb, "</div> ");
        if (drawing) {
            sb_append(&sb, "<div class=\"draw-timer drawing-status svelte-1rj22es\"><span class=\"timer-label svelte-1rj22es\">Status:</span> <span class=\"timer-value svelte-1rj22es\">Drawing in progress</span></div>");
        } else if (open) {
            const long total = (long) left;
            sb_appendf(&sb, "<div class=\"draw-timer svelte-1rj22es\"><span class=\"timer-label svelte-1rj22es\">Next Draw:</span> <span class=\"timer-value svelte-1rj22es\" id=\"knLotTimer\">%ldh %ldm %lds</span></div>",
                       total / 3600, total % 3600 / 60, total % 60);
        } else {
            sb_append(&sb, "<!---->");
        }
        sb_append(&sb, " <div class=\"costs-row svelte-1rj22es\">");
        double submitting = 0;
        const cJSON *cost;
        int i = 0;
        cJSON_ArrayForEach(cost, costs) {
            if (i >= LOT_MAX_COSTS) break;
            const char *type = v10_str(cost, "cost_type"), *ref = v10_str(cost, "cost_ref");
            char refbuf[16];
            if (!ref[0]) {
                snprintf(refbuf, sizeof(refbuf), "%d", (int) v10_num(cost, "cost_ref", 0));
                ref = refbuf;
            }
            const int step = (int) v10_num(cost, "cost_quantity", 1) > 0 ? (int) v10_num(cost, "cost_quantity", 1) : 1;
            const double per = v10_num(cost, "num_entries", 1) / step;
            const int avail = lot_available(cost);
            const int max = avail / step * step;
            if (LT.qty[i] > max) LT.qty[i] = max;
            const double entries = LT.qty[i] > 0 ? LT.qty[i] * per : 0;
            submitting += entries;
            const int disabled = LT.submitting || !open;
            const int is_skin = !strcmp(type, "skin");
            const char *name = is_skin ? skin_name(ref) : cur_name(ref);
            const char *color = "#ffffff";
            sb_appendf(&sb, "<div class=\"cost-column themed-box svelte-1rj22es\"><div class=\"cost-header svelte-1rj22es\"><div class=\"cost-name svelte-1rj22es\" style=\"color: %s\">", color);
            sb_append_esc(&sb, name);
            sb_appendf(&sb, "</div> <div class=\"cost-entries svelte-1rj22es\">(%.17g %s)</div></div> <div class=\"cost-item-display svelte-1rj22es\">", per, per == 1 ? "entry" : "entries");
            char kids[160] = "";
            if (entries > 0) snprintf(kids, sizeof(kids), "<div class=\"entries-from-this svelte-1rj22es\">(%.17g entries)</div>", entries);
            lot_item(&sb, type, ref, v10_num(cost, "cost_quantity", 1), 160, kids, name);
            sb_appendf(&sb, "</div> <div class=\"available-quantity-display svelte-1rj22es\">Available: %d</div> ", avail);
            lot_stepper(&sb, i, LT.qty[i], max, step, disabled);
            sb_appendf(&sb, " <button class=\"add-all-button svelte-1rj22es\"%s onmouseenter=\"playTick(0.1)\" onclick=\"knMiscLotAll(%d)\">Add All</button></div>",
                       disabled || avail == 0 || max == LT.qty[i] ? " disabled" : "", i);
            i++;
        }
        sb_appendf(&sb, "</div> <button class=\"enter-lottery-button svelte-1rj22es\"%s onmouseenter=\"playTick(0.1)\" onclick=\"knMiscLotEnter()\">%s</button> <div class=\"entry-info svelte-1rj22es\">",
                   LT.submitting || submitting <= 0 || !open ? " disabled" : "", LT.submitting ? "ENTERING..." : open ? "ENTER RAFFLE!" : "SUBMISSION CLOSED");
        if (submitting > 0) sb_appendf(&sb, "<div class=\"submitting-entries svelte-1rj22es\">Submitting: %.17g %s</div>", submitting, submitting == 1 ? "entry" : "entries");
        else sb_append(&sb, "<!---->");
        sb_appendf(&sb, " <div class=\"your-total-entries svelte-1rj22es\">Your Total Entries: %d</div> <div class=\"draw-total-entries svelte-1rj22es\">Total Draw Entries: %d</div></div></div> ",
                   (int) v10_num(draw, "player_entries", 0), (int) v10_num(draw, "total_entries", 0));
        // the right column: grand prize, additional prizes, last draw's winners
        sb_append(&sb, "<div class=\"right-column svelte-1rj22es\">");
        const cJSON *first = cJSON_GetArrayItem(cJSON_GetObjectItem(draw, "prizes"), 0);
        if (first) {
            char ref[16];
            snprintf(ref, sizeof(ref), "%d", (int) v10_num(first, "prize_ref", 0));
            sb_append(&sb, "<div class=\"grand-prize-container themed-box svelte-1rj22es\"><div class=\"grand-prize-title svelte-1rj22es\">Grand Prize</div> <div class=\"grand-prize-display svelte-1rj22es\">");
            lot_item(&sb, v10_str(first, "prize_type"), ref, v10_num(first, "prize_quantity", 1), 200, NULL, "Prize");
            sb_append(&sb, "</div></div> ");
        } else {
            sb_append(&sb, "<!----> ");
        }
        sb_appendf(&sb, "<div class=\"additional-prizes-box themed-box svelte-1rj22es\"><div class=\"additional-prizes-title svelte-1rj22es\">Additional prizes</div> "
                        "<div class=\"additional-prizes-count svelte-1rj22es\">%d %s will also receive a prize</div></div> ",
                   extra, extra == 1 ? "other entry" : "other entries");
        const cJSON *last = cJSON_GetArrayItem(prev, 0);
        if (cJSON_GetArraySize(prev) > 0) {
            sb_append(&sb, "<div class=\"last-draw-winners themed-box svelte-1rj22es\"><div class=\"last-draw-winners-title svelte-1rj22es\">Last Draw Winners</div> <div class=\"winners-scroll svelte-14vp3ai\">");
            const cJSON *p;
            cJSON_ArrayForEach(p, cJSON_GetObjectItem(last, "prizes")) {
                const char *who = v10_str(p, "won_by_name");
                char ref[16];
                snprintf(ref, sizeof(ref), "%d", (int) v10_num(p, "prize_ref", 0));
                const char *type = v10_str(p, "prize_type");
                sb_append(&sb, "<div class=\"winner-entry svelte-14vp3ai\">");
                if (who[0]) {
                    char js[64];
                    js_name(js, sizeof(js), who);
                    sb_appendf(&sb, "<button type=\"button\" class=\"winner-link svelte-14vp3ai\" onclick=\"openPlayerProfile('%s')\">", js);
                    sb_append_esc(&sb, who);
                    sb_append(&sb, "</button>  -  ");
                }
                if (!strcmp(type, "skin")) sb_appendf(&sb, "%s x%d", skin_name(ref), (int) v10_num(p, "prize_quantity", 1));
                else if (!strcmp(type, "currency")) sb_appendf(&sb, "%d %s", (int) v10_num(p, "prize_quantity", 1), cur_name(ref));
                else sb_appendf(&sb, "%s %s x%d", type, ref, (int) v10_num(p, "prize_quantity", 1));
                sb_append(&sb, "</div>");
            }
            sb_append(&sb, "</div></div>");
        } else {
            sb_append(&sb, "<!---->");
        }
        sb_append(&sb, "</div></div>");
    } else if (cJSON_IsTrue(cJSON_GetObjectItem(info, "event_ended"))) {
        sb_append(&sb, "<div class=\"no-drawing-message svelte-1rj22es\">The raffle event has ended. Thank you for participating!</div>");
    } else {
        sb_append(&sb, "<div class=\"no-drawing-message svelte-1rj22es\">A new drawing will be created soon. Please check back in a few moments!</div>");
    }
    sb_append(&sb, "</div></div>");
    return sb_finish(&sb);
}

static void lot_render(void) {
    char *html = lot_html();
    v10_popup_set("defaultModal lotteryModal", html);
    free(html);
}

// no answer for the view by LOT_WAIT_MS: the window fails with Retry (a late answer still shows the lottery)
static void lot_expired(void *user) {
    (void) user;
    LT.timer = 0;
    if (lot_data(NULL)) return;
    LT.timed_out = 1;
    if (v10_popup_open("defaultModal lotteryModal")) lot_render();
}

static void lot_arm_deadline(void) {
    html_timer_cancel(LT.timer);
    LT.timer = G.scenario ? 0 : html_timer_add(LOT_WAIT_MS, lot_expired, NULL);
    LT.timed_out = 0;
}

static void lot_open(const int id) {
    LT.id = id > 0 ? id : 3; // openLottery(): lotteryId = arguments[0] ?? 3
    memset(LT.qty, 0, sizeof(LT.qty));
    LT.submitting = 0;
    if (!G.scenario) LT.error[0] = 0; // a scenario's error (its answer) is the state it stands in for
    lot_arm_deadline();
    char *html = lot_html();
    v10_popup_show("defaultModal lotteryModal", html, lot_render);
    free(html);
}

static void lot_done(void *user, const int status, const cJSON *json) {
    (void) user;
    LT.submitting = 0;
    if (status == 200) {
        memset(LT.qty, 0, sizeof(LT.qty));
        LT.error[0] = 0;
        char path[512];
        v10_forget(lot_path(path, sizeof(path), ""));
    } else {
        const char *e = json ? v10_str(json, "error") : "";
        snprintf(LT.error, sizeof(LT.error), "%s", e[0] ? e : "Failed to participate");
    }
    if (v10_popup_open("defaultModal lotteryModal")) lot_render();
}

static void lot_enter(void) {
    const cJSON *lottery = lot_data(NULL);
    const cJSON *info = lottery ? cJSON_GetObjectItem(lottery, "lottery") : NULL;
    const cJSON *costs = lottery ? cJSON_GetObjectItem(lottery, "costs") : NULL;
    if (!costs && info) costs = cJSON_GetObjectItem(info, "costs");
    if (!costs) {
        snprintf(LT.error, sizeof(LT.error), "No costs available");
        return;
    }
    int i = 0;
    const cJSON *cost;
    cJSON_ArrayForEach(cost, costs) {
        if (i < LOT_MAX_COSTS && LT.qty[i] > 0) {
            char path[512], body[160], ref[16];
            const char *r = v10_str(cost, "cost_ref");
            if (!r[0]) {
                snprintf(ref, sizeof(ref), "%d", (int) v10_num(cost, "cost_ref", 0));
                r = ref;
            }
            snprintf(body, sizeof(body), "{\"cost_type\":\"%s\",\"cost_ref\":\"%s\",\"cost_quantity\":%d}", v10_str(cost, "cost_type"), r, LT.qty[i]);
            LT.submitting = 1;
            LT.error[0] = 0;
            v10_send("POST", lot_path(path, sizeof(path), "/participate"), body, lot_done, NULL);
            return;
        }
        i++;
    }
    snprintf(LT.error, sizeof(LT.error), "Please select at least one item to participate");
}

static void lot_tick(void) {
    HtmlElem t = html_dom_by_id("knLotTimer");
    static double last;
    const double now = mono_ms();
    if (!t || G.scenario || now - last < 1000) return;
    last = now;
    const cJSON *lottery = lot_data(NULL), *draw = lottery ? cJSON_GetObjectItem(lottery, "current_draw") : NULL;
    if (!draw && lottery) draw = cJSON_GetObjectItem(cJSON_GetObjectItem(lottery, "lottery"), "current_draw");
    int y, mo, d, h = 0, mi = 0, s = 0;
    if (!draw || sscanf(v10_str(draw, "ends_at"), "%4d-%2d-%2dT%2d:%2d:%2d", &y, &mo, &d, &h, &mi, &s) < 3) return;
    struct tm tm = {.tm_year = y - 1900, .tm_mon = mo - 1, .tm_mday = d, .tm_hour = h, .tm_min = mi, .tm_sec = s};
    const long left = (long) ((double) timegm(&tm) - now_s());
    if (left <= 0) {
        if (v10_popup_open("defaultModal lotteryModal")) lot_render(); // "Drawing in progress"
        return;
    }
    char text[48];
    snprintf(text, sizeof(text), "%ldh %ldm %lds", left / 3600, left % 3600 / 60, left % 60);
    html_dom_set_text(t, text);
}

// ================================================================== premium subscription shop

static struct {
    cJSON *shop;     // {children: [{id, product_id, data}], products: {id: {...}}}
    cJSON *subs;     // account.subscriptions: [{id, product_id, name, type, renewing}]
    char error[160];
} PR;

static const cJSON *prem_sub(const char *product_id) {
    const cJSON *s;
    cJSON_ArrayForEach(s, PR.subs) {
        if (!strcmp(v10_str(s, "product_id"), product_id)) return s;
    }
    return NULL;
}

static void prem_card(StrBuf *sb, const cJSON *child, const cJSON *product) {
    const char *pid = v10_str(product, "id");
    const cJSON *price = NULL, *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(product, "prices")) {
        if (cJSON_IsTrue(cJSON_GetObjectItem(p, "available")) && !strcmp(v10_str(p, "type"), "real")) {
            price = p;
            break;
        }
    }
    if (!price) return;
    const cJSON *sub = prem_sub(pid);
    const int subscribed = sub != NULL, any = cJSON_GetArraySize(PR.subs) > 0, disabled = any && !subscribed;
    const int renewing = sub && cJSON_IsTrue(cJSON_GetObjectItem(sub, "renewing"));
    const int popular = !strcmp(v10_str(cJSON_GetObjectItem(child, "data"), "tag"), "popular");
    const cJSON *rec = cJSON_GetObjectItem(price, "recurring");
    const cJSON *real = cJSON_GetObjectItem(price, "real");
    char amount[24], period[48] = "", billing[64] = "";
    snprintf(amount, sizeof(amount), "%.2f", v10_num(real, "price_usd", 0));
    if (rec) {
        const int n = (int) v10_num(rec, "amount", 1);
        const char *per = v10_str(rec, "periodicity");
        if (n == 1) snprintf(period, sizeof(period), " / %s", per);
        else snprintf(period, sizeof(period), " / %d %ss", n, per);
        snprintf(billing, sizeof(billing), "Billed every %d %s", n, n == 1 ? per : "months");
        if (n != 1) snprintf(billing, sizeof(billing), "Billed every %d %ss", n, per);
    }
    const char *img = v10_str(product, "image_url");
    sb_appendf(sb, "<div class=\"premium-card svelte-190e6fj%s%s%s\" style=\"--accent-color: %s;\" onmouseenter=\"%s\">"
                   "<div class=\"card-background svelte-190e6fj\" style=\"background-image: url(%s)\"></div> <div class=\"card-overlay svelte-190e6fj\"></div> ",
               popular ? " popular-card" : "", subscribed ? " subscribed-card" : "", disabled ? " disabled-card" : "",
               v10_str(product, "headCol")[0] ? v10_str(product, "headCol") : "#ffc147", disabled ? "" : "playTick()", img[0] ? img : "/img/prem_crd.png");
    if (subscribed)
        sb_append(sb, "<div class=\"subscription-status-overlay svelte-190e6fj\"><div class=\"status-badge svelte-190e6fj\"><i class=\"material-icons svelte-190e6fj\">check_circle</i> <span class=\"svelte-190e6fj\">Active Plan</span></div></div> ");
    else sb_append(sb, "<!----> ");
    sb_append(sb, "<div class=\"card-content svelte-190e6fj\"><div class=\"card-header svelte-190e6fj\"><h2 class=\"product-name svelte-190e6fj\">");
    sb_append_esc(sb, v10_str(product, "name")[0] ? v10_str(product, "name") : "Premium");
    sb_append(sb, "</h2> ");
    if (v10_str(product, "description")[0]) {
        sb_append(sb, "<p class=\"product-description svelte-190e6fj\">");
        sb_append_esc(sb, v10_str(product, "description"));
        sb_append(sb, "</p>");
    } else {
        sb_append(sb, "<!---->");
    }
    sb_appendf(sb, "</div> <div class=\"card-price svelte-190e6fj\"><div class=\"price-background-overlay svelte-190e6fj\"></div> <div class=\"price-main svelte-190e6fj\">"
                   "<span class=\"currency svelte-190e6fj\">$</span> <span class=\"amount svelte-190e6fj\">%s</span> <span class=\"period svelte-190e6fj\">%s</span></div> ", amount, period);
    if (billing[0]) sb_appendf(sb, "<div class=\"billing-info svelte-190e6fj\">%s</div>", billing);
    else sb_append(sb, "<!---->");
    sb_append(sb, "</div> ");
    if (!subscribed) {
        sb_appendf(sb, "<div class=\"card-action svelte-190e6fj\"><button class=\"subscribe-btn svelte-190e6fj\"%s>%s</button></div>", disabled ? " disabled" : "", disabled ? "Plan Unavailable" : "Subscribe Now");
    } else {
        sb_appendf(sb, "<div class=\"card-action svelte-190e6fj\"><button class=\"subscribed-btn svelte-190e6fj\"><i class=\"material-icons svelte-190e6fj\">check_circle</i> <span class=\"svelte-190e6fj\">Current Plan</span></button></div>");
        sb_appendf(sb, " <div class=\"renewal-info svelte-190e6fj\"><div class=\"renewal-icon svelte-190e6fj\"><i class=\"material-icons svelte-190e6fj\">%s</i></div> "
                       "<div class=\"renewal-text svelte-190e6fj\">%s<span class=\"renewal-date svelte-190e6fj\">%s</span></div></div>",
                   renewing ? "autorenew" : "pause_circle", renewing ? "<span class=\"renewal-label svelte-190e6fj\">Auto-renewal</span> " : "<!----> ",
                   renewing ? "Next billing" : "Expires");
    }
    sb_append(sb, "</div> <div class=\"premium-badge svelte-190e6fj\"><i class=\"material-icons svelte-190e6fj\">beenhere</i></div> ");
    if (popular) sb_append(sb, "<div class=\"popular-tag svelte-190e6fj\"><span class=\"svelte-190e6fj\">Best Value!</span></div> ");
    else sb_append(sb, "<!----> ");
    if (disabled) sb_append(sb, "<div class=\"disabled-indicator svelte-190e6fj\"><i class=\"material-icons svelte-190e6fj\">lock</i> <span class=\"svelte-190e6fj\">Other Plan Active</span></div>");
    else sb_append(sb, "<!---->");
    sb_append(sb, "</div>");
}

static char *prem_html(void) {
    // no shop on this server (no products route): the request fails at once, as v10's shop shows a failed one
    if (!PR.shop && !PR.error[0]) snprintf(PR.error, sizeof(PR.error), "%s", "Premium shop is not available on this server.");
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"premium-container svelte-190e6fj\">");
    if (PR.error[0]) {
        sb_append(&sb, "<div class=\"error-container svelte-190e6fj\"><div class=\"error-text svelte-190e6fj\">Error: ");
        sb_append_esc(&sb, PR.error);
        sb_append(&sb, "</div> <button class=\"retry-button svelte-190e6fj\" onclick=\"knMiscPremRetry()\">Retry</button></div>");
    } else {
        sb_append(&sb, "<div class=\"premium-cards-grid svelte-190e6fj\">");
        const cJSON *child, *products = cJSON_GetObjectItem(PR.shop, "products");
        cJSON_ArrayForEach(child, cJSON_GetObjectItem(PR.shop, "children")) {
            const cJSON *product = cJSON_GetObjectItem(products, v10_str(child, "product_id"));
            if (product && cJSON_IsTrue(cJSON_GetObjectItem(product, "available"))) prem_card(&sb, child, product);
        }
        sb_append(&sb, " <div class=\"benefits-card svelte-190e6fj\"><div class=\"card-content svelte-190e6fj\"><div class=\"card-header svelte-190e6fj\"><h2 class=\"product-name svelte-190e6fj\">Benefits</h2></div> "
                       "<div class=\"benefits-list svelte-190e6fj\">");
        static const char *const benefits[] = {"Golden Badge", "Custom Display Name", "40 KR Per Round", "Premium Commands", "16 Player Lobbies", "Enhanced Trading", "And more"};
        for (int i = 0; i < 7; i++)
            sb_appendf(&sb, "%s<div class=\"benefit-row svelte-190e6fj\"><span class=\"benefit-icon svelte-190e6fj\">\xe2\x98\x85</span> <span class=\"benefit-text svelte-190e6fj\">%s</span></div>", i ? " " : "", benefits[i]);
        sb_append(&sb, "</div></div></div></div>");
    }
    sb_append(&sb, "</div> ");
    const cJSON *s;
    int renewing = 0;
    cJSON_ArrayForEach(s, PR.subs) renewing |= !strcmp(v10_str(s, "type"), "premium") && cJSON_IsTrue(cJSON_GetObjectItem(s, "renewing"));
    if (cJSON_GetArraySize(PR.subs) > 0 && renewing)
        sb_append(&sb, "<div class=\"cancel-subscription-section svelte-190e6fj\"><div class=\"cancel-container svelte-190e6fj\"><div class=\"cancel-info svelte-190e6fj\">"
                       "<i class=\"material-icons svelte-190e6fj\">warning</i> <div class=\"cancel-text svelte-190e6fj\"><h3 class=\"svelte-190e6fj\">Cancel Subscription</h3> "
                       "<p class=\"svelte-190e6fj\">Cancel your current subscription. You'll keep your benefits until the end of the current billing period.</p></div></div> "
                       "<button class=\"cancel-btn svelte-190e6fj\" onclick=\"knMiscPremCancel()\"><i class=\"material-icons svelte-190e6fj\">cancel</i> <span class=\"svelte-190e6fj\">Cancel</span></button></div></div>");
    else sb_append(&sb, "<!---->");
    return sb_finish(&sb);
}

static void prem_render(void) {
    char *html = prem_html();
    v10_popup_set("defaultModal premiumShopModal", html);
    free(html);
}

static void prem_open(void) {
    char *html = prem_html();
    v10_popup_show("defaultModal premiumShopModal", html, prem_render);
    free(html);
}

static void prem_cancel_closed(const int accepted, void *user) {
    (void) user;
    if (!accepted) return;
    // v10: shop.cancelSubscription(id); no shop backend, so the request is never answered: the error popup
    v10_view_push(v10_io_error("Error", "An unexpected error occurred while canceling your subscription. Please try again."), NULL, NULL, NULL);
}

static void prem_cancel(void) {
    const cJSON *s, *sub = NULL;
    cJSON_ArrayForEach(s, PR.subs) {
        if (!strcmp(v10_str(s, "type"), "premium")) sub = s;
    }
    if (!sub) return;
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 700px;\">"
                   "<div class=\"confirm-cancel-container io-popup-container\"><div class=\"confirm-cancel-header io-popup-header io-popup-header--slim svelte-18wpngg\">"
                   "<svg xmlns=\"http://www.w3.org/2000/svg\" class=\"cancel-icon svelte-18wpngg\" viewBox=\"0 0 24 24\" fill=\"currentColor\"><path d=\"M12 2C6.48 2 2 6.48 2 12s4.48 10 10 10 10-4.48 10-10S17.52 2 12 2zm-2 15l-5-5 1.41-1.41L10 14.17l7.59-7.59L19 8l-9 9z\" class=\"svelte-18wpngg\"></path></svg> Cancel subscription?</div> "
                   "<div class=\"confirm-cancel-content io-popup-content svelte-18wpngg\"><div class=\"cancel-preview svelte-18wpngg\"><div class=\"warning-icon-container svelte-18wpngg\"><i class=\"material-icons svelte-18wpngg\">warning</i></div> "
                   "<div class=\"cancel-info svelte-18wpngg\"><div class=\"cancel-question svelte-18wpngg\">Are you sure you want to cancel your <strong class=\"svelte-18wpngg\">");
    sb_append_esc(&sb, v10_str(sub, "name")[0] ? v10_str(sub, "name") : "Premium Subscription");
    sb_append(&sb, "</strong> ?</div> <div class=\"consequences svelte-18wpngg\"><h4 class=\"svelte-18wpngg\">What happens when you cancel:</h4> <ul class=\"svelte-18wpngg\">"
                   "<li class=\"svelte-18wpngg\">You'll keep your Premium benefits until the end of your current billing period</li> <li class=\"svelte-18wpngg\">Your subscription will not renew automatically</li> "
                   "<li class=\"svelte-18wpngg\">You can resubscribe at any time</li></ul></div></div></div> <div class=\"io-button-group svelte-18wpngg\">"
                   "<button type=\"button\" class=\"io-button\" onclick=\"knViewPop(0)\">Keep Subscription</button> "
                   "<button type=\"button\" class=\"io-button io-button--danger\" onclick=\"knViewPop(1)\">Cancel Subscription</button></div></div></div></div>");
    v10_view_push(sb_finish(&sb), NULL, prem_cancel_closed, NULL);
}

// ================================================================== usernames

static struct {
    char buy[24];
    int confirm_buy;               // the inline "Buy X for $9.99?" confirmation
    char feedback[200], message[200];
    int busy;
    cJSON *names;                  // scenario: [{name, state, expires_secs, renew_secs, release_secs, original}]
    double cooldown_s;
} UN;

static void un_render(void);

static void un_close(const int accepted, void *user) {
    (void) accepted;
    (void) user;
    cJSON_Delete(UN.names);
    UN.names = NULL;
}

// a time left as v10's readable form for the usernames list: "3d 4h" (secs < 0: none given)
static void un_time(char *out, const size_t n, const double secs) {
    const long s = secs > 0 ? (long) secs : 0;
    snprintf(out, n, "%ldd %ldh", s / 86400, s % 86400 / 3600);
}

static void un_row(StrBuf *sb, const char *name, const char *state, const double left_secs) {
    char buf[200], arg[48], js[64];
    js_name(js, sizeof(js), name);
    const int active = !strcmp(state, "active"), grace = !strcmp(state, "grace");
    sb_appendf(sb, "<div class=\"username-row svelte-27udiy%s%s\"><div class=\"row-main svelte-27udiy\"><span class=\"username-name svelte-27udiy\">", active ? " active" : "", grace ? " grace" : "");
    sb_append_esc(sb, name);
    sb_append(sb, "</span> ");
    if (active) {
        sb_append(sb, "<span class=\"username-active svelte-27udiy\">");
        sb_append_esc(sb, tr("account.usernames.active", "Active"));
        sb_append(sb, "</span>");
    } else {
        sb_append(sb, "<div class=\"row-buttons svelte-27udiy\"><button type=\"button\" class=\"io-button io-button--slim svelte-27udiy\" onclick=\"knMiscUnRenew('");
        sb_append(sb, js);
        sb_append(sb, "')\">");
        sb_append_esc(sb, tr("account.usernames.renew", "Renew"));
        sb_append(sb, "</button> ");
        if (!grace) {
            sb_append(sb, "<button type=\"button\" class=\"io-button io-button--slim io-button--accept-secondary svelte-27udiy\" onclick=\"knMiscUnSwap('");
            sb_append(sb, js);
            sb_append(sb, "')\">");
            sb_append_esc(sb, tr("account.usernames.use", "Use"));
            sb_append(sb, "</button>");
        } else {
            sb_append(sb, "<!---->");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div> ");
    if (active) {
        sb_append(sb, "<span class=\"username-meta svelte-27udiy\">");
        sb_append_esc(sb, tr("account.usernames.active.never", "Never expires while active"));
        sb_append(sb, "</span>");
    } else if (grace) {
        un_time(arg, sizeof(arg), left_secs);
        I18N_F(buf, "account.usernames.grace.left", "Released in {0} unless renewed", arg);
        sb_append(sb, "<span class=\"username-meta grace-meta svelte-27udiy\"><i class=\"material-icons svelte-27udiy\">warning</i> <span class=\"username-grace svelte-27udiy\">");
        sb_append_esc(sb, tr("account.usernames.grace", "Pending release"));
        sb_append(sb, "</span> ");
        sb_append_esc(sb, buf);
        sb_append(sb, "</span>");
    } else {
        un_time(arg, sizeof(arg), left_secs);
        I18N_F(buf, "account.usernames.expires", "Expires {0}", arg);
        sb_append(sb, "<span class=\"username-meta svelte-27udiy\">");
        sb_append_esc(sb, buf);
        sb_append(sb, "</span>");
    }
    sb_append(sb, "</div>");
}

static char *un_html(void) {
    char buf[320], a0[16], a1[16];
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 900px;\"><div class=\"io-popup-container\"><div class=\"io-popup-header io-popup-header--slim\">");
    sb_append_esc(&sb, tr("account.usernames.title", "Usernames"));
    sb_append(&sb, "</div> <div class=\"usernames-content svelte-27udiy\"><div class=\"usernames-top svelte-27udiy\"><p class=\"usernames-desc svelte-27udiy\">");
    snprintf(a0, sizeof(a0), "7");
    snprintf(a1, sizeof(a1), "365");
    I18N_F(buf, "account.usernames.desc", "Switch between the usernames you own once every {0} days. A username you're not using is kept for {1} days, then released for anyone to take unless you renew it.", a0, a1);
    sb_append_esc(&sb, buf);
    sb_append(&sb, "</p> <div class=\"usernames-login-note svelte-27udiy\"><i class=\"material-icons svelte-27udiy\">info</i> <span>");
    sb_append_esc(&sb, tr("account.usernames.login", "Your active username is also your login username. After switching, sign in with the new one."));
    sb_append(&sb, "</span></div></div> ");
    if (UN.names) {
    sb_append(&sb, "<div class=\"usernames-columns svelte-27udiy\"><section class=\"usernames-col svelte-27udiy\"><div class=\"col-header svelte-27udiy\">");
    sb_append_esc(&sb, tr("account.usernames.owned.header", "Your Usernames"));
    sb_append(&sb, "</div> <div class=\"usernames-list svelte-27udiy\">");
    {
        const cJSON *n;
        cJSON_ArrayForEach(n, UN.names) un_row(&sb, v10_str(n, "name"), v10_str(n, "state"), v10_num(n, "release_secs", v10_num(n, "expires_secs", -1)));
    }
    sb_append(&sb, "</div> ");
    sb_append(&sb, "<!----> <!----> <!----></section> <section class=\"usernames-col buy-col svelte-27udiy\"><div class=\"col-header svelte-27udiy\">");
    sb_append_esc(&sb, tr("account.usernames.buy.header", "Buy a Username"));
    sb_append(&sb, "</div> <p class=\"buy-desc svelte-27udiy\">");
    snprintf(a0, sizeof(a0), "15");
    I18N_F(buf, "account.usernames.buy.desc", "Pick any free username. It's valid for {1} days and nobody else can use it. It's held for you for {0} minutes while you pay.", a0, a1);
    sb_append_esc(&sb, buf);
    sb_append(&sb, "</p> <input class=\"io-input-field buy-input svelte-27udiy\" type=\"text\" maxlength=\"14\" id=\"knUnBuy\" placeholder=\"");
    sb_append_esc(&sb, tr("account.usernames.buy.placeholder", "Enter a username"));
    sb_appendf(&sb, "\" value=\"%s\" oninput=\"knMiscUnInput()\"> ", UN.buy);
    if (UN.confirm_buy) {
        char name[32];
        snprintf(name, sizeof(name), "%s", UN.buy);
        // {3} is the renewal price (v10.0.3: usernameRenewPriceUSD, 4.99; 10.0.2 put $9.99 in {1} here)
        I18N_F(buf, "account.usernames.buy.confirm", "Buy {0} for {1}? It's valid for {2} days; renew it for {3} to keep it longer.", name, "$9.99", a1, "$4.99");
        sb_append(&sb, "<div class=\"inline-confirm svelte-27udiy\"><span>");
        sb_append_esc(&sb, buf);
        sb_appendf(&sb, "</span> <!----> <div class=\"confirm-buttons svelte-27udiy\"><button type=\"button\" class=\"io-button io-button--slim\" onclick=\"knMiscUnBuyCancel()\">%s</button> "
                        "<button type=\"button\" class=\"io-button io-button--slim io-button--accept\"%s onclick=\"knMiscUnBuy()\">%s</button></div></div>",
                   tr("generic.cancel", "Cancel"), UN.busy ? " disabled" : "", tr("account.usernames.buy.button", "Buy"));
    } else {
        sb_appendf(&sb, "<button type=\"button\" class=\"io-button io-button--accept buy-button svelte-27udiy\"%s onclick=\"knMiscUnBuy()\">%s $9.99</button>",
                   UN.busy || !UN.buy[0] ? " disabled" : "", tr("account.usernames.buy.button", "Buy"));
    }
    sb_append(&sb, " <!----></section></div> ");
    }
    if (UN.feedback[0]) {
        sb_append(&sb, "<div class=\"io-error-message usernames-feedback svelte-27udiy\">");
        sb_append_esc(&sb, UN.feedback);
        sb_append(&sb, "</div>");
    } else if (UN.message[0]) {
        sb_append(&sb, "<div class=\"io-message usernames-feedback usernames-message svelte-27udiy\">");
        sb_append_esc(&sb, UN.message);
        sb_append(&sb, "</div>");
    } else {
        sb_append(&sb, "<!---->");
    }
    sb_appendf(&sb, " <div class=\"usernames-footer svelte-27udiy\"><button type=\"button\" class=\"io-button\" onclick=\"knViewPop(0)\">%s</button></div></div></div></div></div>", tr("generic.close", "Close"));
    return sb_finish(&sb);
}

static void un_render(void) { v10_view_set(un_html()); }

static void un_open(void) {
    UN.buy[0] = UN.feedback[0] = UN.message[0] = 0;
    UN.confirm_buy = UN.busy = 0;
    v10_view_push(un_html(), un_render, un_close, NULL);
}

// ================================================================== opening / closing

static void close_all(void) {
    v10_misc_survey_clear();
    if (HK.open || UN.names || PR.shop) v10_view_clear();
    if (KID.open) kid_close();
    if (v10_popup_open("defaultModal lotteryModal") || v10_popup_open("defaultModal premiumShopModal")) v10_popup_close();
}

static void open_by_name(const char *what) {
    if (!strcmp(what, "survey")) {
        cJSON *s = cJSON_CreateObject();
        cJSON_AddStringToObject(s, "surveyId", "kn-demo");
        cJSON_AddStringToObject(s, "question", "How is your Krunker Box Office experience so far?");
        cJSON *a = cJSON_AddArrayToObject(s, "answers");
        cJSON_AddItemToArray(a, cJSON_CreateString("Great"));
        cJSON_AddItemToArray(a, cJSON_CreateString("Good"));
        cJSON_AddItemToArray(a, cJSON_CreateString("Okay"));
        cJSON_AddItemToArray(a, cJSON_CreateString("Bad"));
        cJSON_AddNumberToObject(s, "expiresAt", now_s() + 120);
        cJSON_AddNumberToObject(s, "rewardAmount", 10);
        v10_misc_survey_show(s);
        cJSON_Delete(s);
    } else if (!strcmp(what, "hook")) {
        hook_open(30);
    } else if (!strcmp(what, "kid")) {
        kid_open();
    } else if (!strcmp(what, "lottery")) {
        lot_open(3);
    } else if (!strcmp(what, "premium")) {
        prem_open();
    } else if (!strcmp(what, "usernames")) {
        un_open();
    }
}

void v10_misc_popups_setup(void) {
    if (!html_dom_by_id("notificationCenter")) return; // not the v10 page
    if (!G.css) {
        html_ui_add_stylesheet("css/kn-v10-popups.css");
        G.css = 1;
    }
    // GDC 2025 display mode: the intro for a guest (v10:222205 showingDisplayIntro, :223353 mount)
    if (!G.scenario && !active_account() && display_mode_kid()) kid_open();
}

void v10_misc_popups_tick(void) {
    survey_tick();
    hook_tick();
    lot_tick();
}

int v10_misc_popups_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name || (strncmp(name, "knMisc", 6) && strcmp(name, "openLottery") && strcmp(name, "window.openLottery") &&
                  strcmp(name, "manageAccNames") && strcmp(name, "window.manageAccNames")))
        return -1;
    if (!strcmp(name, "openLottery") || !strcmp(name, "window.openLottery")) {
        lot_open(arg_int(args, argc, 0, 0));
        return 1;
    }
    if (!strcmp(name, "manageAccNames") || !strcmp(name, "window.manageAccNames")) {
        un_open();
        return 1;
    }
    if (!strcmp(name, "knMiscOpen")) {
        open_by_name(arg_str(args, argc, 0));
        return 1;
    }
    if (!strcmp(name, "knMiscClose")) {
        close_all();
        return 1;
    }
    // survey
    if (!strcmp(name, "knMiscSurveyDismiss")) {
        v10_misc_survey_clear();
        return 1;
    }
    if (!strcmp(name, "knMiscSurveyAnswer")) {
        // the SURVEY_ANSWER rpc: nothing answers it here (no brain route), so no button changes state
        return 1;
    }
    // mission briefing
    if (!strcmp(name, "knMiscHookAbort")) { // guest_hook_abort: close
        v10_view_pop(0);
        return 1;
    }
    if (!strcmp(name, "knMiscHookDeploy")) { // guest_hook_deploy (v10:118684): close, then the register modal (defaultToRegister)
        v10_view_pop(0);
        HtmlArg a = {HTML_ARG_BOOL, 1, NULL};
        html_ui_dispatch("loginOrRegister", ev, &a, 1);
        return 1;
    }
    // k-ID
    if (!strcmp(name, "knMiscKidRegister")) { // showingDisplayIntro = false; dialog.close(); loginOrRegister()
        kid_close();
        HtmlArg a = {HTML_ARG_BOOL, 0, NULL};
        html_ui_dispatch("loginOrRegister", ev, &a, 1);
        return 1;
    }
    // lottery
    if (!strcmp(name, "knMiscLotQty") || !strcmp(name, "knMiscLotAll")) {
        const int i = arg_int(args, argc, 0, -1);
        const cJSON *lottery = lot_data(NULL);
        const cJSON *info = lottery ? cJSON_GetObjectItem(lottery, "lottery") : NULL;
        const cJSON *costs = lottery ? cJSON_GetObjectItem(lottery, "costs") : NULL;
        if (!costs && info) costs = cJSON_GetObjectItem(info, "costs");
        const cJSON *cost = i >= 0 && i < LOT_MAX_COSTS ? cJSON_GetArrayItem(costs, i) : NULL;
        if (!cost || LT.submitting) return 1;
        const int step = (int) v10_num(cost, "cost_quantity", 1) > 0 ? (int) v10_num(cost, "cost_quantity", 1) : 1;
        const int max = lot_available(cost) / step * step;
        if (!strcmp(name, "knMiscLotAll")) LT.qty[i] = max;
        else LT.qty[i] = (int) fmax(0, fmin(max, LT.qty[i] + arg_int(args, argc, 1, 0) * step));
        lot_render();
        return 1;
    }
    if (!strcmp(name, "knMiscLotRetry")) { // the failed view is asked for again (a fresh request, a fresh deadline)
        char path[512];
        v10_forget(lot_path(path, sizeof(path), ""));
        LT.error[0] = 0;
        lot_arm_deadline();
        lot_render();
        return 1;
    }
    if (!strcmp(name, "knMiscLotEnter")) {
        lot_enter();
        lot_render();
        return 1;
    }
    // premium
    if (!strcmp(name, "knMiscPremRetry")) {
        PR.error[0] = 0;
        prem_render();
        return 1;
    }
    if (!strcmp(name, "knMiscPremCancel")) {
        prem_cancel();
        return 1;
    }
    // usernames
    if (!strcmp(name, "knMiscUnInput")) {
        HtmlElem in = html_dom_by_id("knUnBuy");
        if (in) html_dom_get_value(in, UN.buy, sizeof(UN.buy));
        UN.confirm_buy = 0;
        UN.feedback[0] = UN.message[0] = 0;
        HtmlElem btn = html_dom_query(0, ".buy-button.svelte-27udiy");
        if (btn) html_dom_set_attr(btn, UN.buy[0] ? "data-kn-ready" : "disabled", "");
        un_render();
        HtmlElem again = html_dom_by_id("knUnBuy");
        if (again) html_dom_focus(again);
        return 1;
    }
    if (!strcmp(name, "knMiscUnBuy")) {
        if (!UN.buy[0]) return 1;
        if (!UN.confirm_buy) {
            UN.confirm_buy = 1; // the inline confirmation first
        } else {
            // account.reserveUsername(name): the brain has no username shop, so the answer is an error
            UN.confirm_buy = 0;
            snprintf(UN.feedback, sizeof(UN.feedback), "Username purchases are not available on this server.");
        }
        un_render();
        return 1;
    }
    if (!strcmp(name, "knMiscUnBuyCancel")) {
        UN.confirm_buy = 0;
        un_render();
        return 1;
    }
    if (!strcmp(name, "knMiscUnSwap") || !strcmp(name, "knMiscUnRenew")) {
        snprintf(UN.feedback, sizeof(UN.feedback), "Username purchases are not available on this server.");
        un_render();
        return 1;
    }
    return -1;
}

// state.community.popups: {open: "survey"|"hook"|"kid"|"lottery"|"premium"|"usernames", lottery: <the brain's
// answer>, survey: {...}, hook: {kr, score, secs}, premium: {shop, subscriptions, error}, usernames: {names}}
void v10_misc_popups_scenario(const cJSON *p) {
    if (!cJSON_IsObject(p)) return;
    G.scenario = 1;
    v10_misc_popups_setup();
    const char *open = v10_str(p, "open");
    const cJSON *lot = cJSON_GetObjectItem(p, "lottery");
    const char *lot_error = v10_str(p, "lotteryError"); // the sandbox's answer: a non-JSON body (v10:25907)
    if (lot_error[0]) snprintf(LT.error, sizeof(LT.error), "%s", lot_error);
    if (lot) {
        LT.frozen = cJSON_IsNumber(cJSON_GetObjectItem(p, "lotterySecondsLeft"));
        LT.frozen_left = v10_num(p, "lotterySecondsLeft", 0);
        const int id = (int) v10_num(lot, "id", 3);
        char path[512];
        snprintf(path, sizeof(path), "/api/lottery/%d", id);
        v10_scenario_answer(path, lot);
    }
    const cJSON *hook = cJSON_GetObjectItem(p, "hook");
    if (hook) {
        v10_misc_guest_progress(v10_num(hook, "kr", 0), v10_num(hook, "score", 0));
        HK.frozen = 1;
    }
    const cJSON *prem = cJSON_GetObjectItem(p, "premium");
    if (prem) {
        cJSON_Delete(PR.shop);
        cJSON_Delete(PR.subs);
        PR.shop = cJSON_Duplicate(cJSON_GetObjectItem(prem, "shop"), 1);
        PR.subs = cJSON_Duplicate(cJSON_GetObjectItem(prem, "subscriptions"), 1);
        if (!PR.subs) PR.subs = cJSON_CreateArray();
        snprintf(PR.error, sizeof(PR.error), "%s", v10_str(prem, "error"));
    }
    const cJSON *un = cJSON_GetObjectItem(p, "usernames");
    if (un) {
        cJSON_Delete(UN.names);
        UN.names = cJSON_Duplicate(cJSON_GetObjectItem(un, "names"), 1);
        snprintf(UN.buy, sizeof(UN.buy), "%s", v10_str(un, "buy"));
        UN.confirm_buy = cJSON_IsTrue(cJSON_GetObjectItem(un, "confirmBuy"));
        snprintf(UN.feedback, sizeof(UN.feedback), "%s", v10_str(un, "feedback"));
    }
    if (!open[0]) return;
    if (!strcmp(open, "survey")) {
        v10_misc_survey_show(cJSON_GetObjectItem(p, "survey"));
    } else if (!strcmp(open, "hook")) {
        hook_open((int) v10_num(hook, "secs", 30));
    } else if (!strcmp(open, "usernames")) {
        v10_view_push(un_html(), un_render, un_close, NULL);
    } else if (!strcmp(open, "lottery")) {
        lot_open(lot ? (int) v10_num(lot, "id", 3) : 3);
    } else {
        open_by_name(open);
    }
}
