// KrunkNative's placeholder ad (client/include/kn_ad.h): where v10 plays an FRVR ad, a card that says so, counts down and
// then grants what the watched ad grants. No ad network, no script: the flow is kn_ad_flow.c, the card #genericPop
// (v10's page) or, on a page without one (Krunker Strike's FRVR page, ui/strike/), #knAdStrikePop under <body>.
#include <kn_ad.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <i18n.h>
#include "klog.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static KnAdFlow g_flow;
static KnAdRequest g_req;
static int g_timer;
static int g_shown; // the card was shown: the ad's show() resolved, so onShow runs as it ends

static int ad_seconds(void) {
    const char *e = getenv("KRUNKNATIVE_AD_SECONDS"); // tests and screenshots: 0 = ready at once
    if (e && *e) {
        const int v = atoi(e);
        return v < 0 ? 0 : v > 60 ? 60 : v;
    }
    return KN_AD_DEFAULT_SECONDS;
}

static void ad_render(void) {
    const HtmlElem count = html_dom_by_id("knAdCount");
    if (count) {
        char t[64];
        if (g_flow.phase == KN_AD_COUNTING)
            snprintf(t, sizeof t, I18N("kn.ad.countdown", "Ad ends in %d"), g_flow.seconds_left);
        else
            snprintf(t, sizeof t, "%s", I18N("kn.ad.ready", "Ad finished"));
        html_dom_set_text(count, t);
    }
    const HtmlElem cont = html_dom_query(0, html_dom_by_id("genericPop") ? "#genericPop .knAdContinue" : "#knAdStrikePop .knAdContinue");
    if (cont) html_dom_set_class(cont, "knAdWait", g_flow.phase != KN_AD_READY);
}

// The card's own holder on a page without v10's #popupHolder / #genericPop (Strike: FRVR's page has neither):
// a full-screen layer under <body>, styled by ui/strike/css/kn-strike-overrides.css. 0 = no body either.
static HtmlElem strike_pop(void) {
    HtmlElem pop = html_dom_by_id("knAdStrikePop");
    if (pop) return pop;
    const HtmlElem body = html_dom_body();
    if (!body) return 0;
    html_dom_insert_html(body, HTML_INSERT_BEFORE_END, "<div id='knAdStrikePop' class='knAdStrikePop'></div>");
    return html_dom_by_id("knAdStrikePop");
}

// 1 = the card shows (with its Continue / close), 0 = the page has nowhere to show it
static int ad_popup(void) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class='knAdCard' style='width:100%;text-align:center'>"
                   "<div class='knAdClose material-icons' onclick='knAdClose()' style='float:right;cursor:pointer;color:rgba(255,255,255,0.5)'>close</div>"
                   "<div class='knAdLabel' style='font-size:14px;color:rgba(255,255,255,0.5);text-align:left'>");
    sb_append_esc(&sb, I18N("kn.ad.label", "Advertisement"));
    sb_append(&sb, "</div><div class='knAdBody' style='margin:24px 0;padding:56px 12px;border:2px dashed rgba(255,255,255,0.25);"
                   "font-size:30px;color:#fff'>");
    sb_append_esc(&sb, I18N("kn.ad.placeholder", "[insert FRVR ad here]"));
    sb_append(&sb, "</div><div id='knAdCount' style='font-size:18px;color:rgba(255,255,255,0.6)'></div>"
                   "<div id='confirmBtn' class='knAdContinue' style='width:calc(100% - 20px);margin-top:15px' onmouseover='playTick(0.1)' "
                   "onclick='knAdContinue()'>");
    sb_append_esc(&sb, I18N("kn.ad.continue", "Continue"));
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    HtmlElem pop = html_dom_by_id("genericPop");
    if (pop) {
        ui_loadout_clear_pops();
        const HtmlElem holder = html_dom_by_id("popupHolder");
        if (holder) html_dom_set_display(holder, "block");
        html_dom_set_attr(pop, "class", "loadingPop knAdPop");
        html_dom_set_attr(pop, "style", "");
    } else {
        pop = strike_pop();
    }
    if (pop) {
        html_dom_set_html(pop, html);
        html_dom_set_display(pop, "block");
    }
    free(html);
    if (!pop) return 0;
    ad_render();
    return 1;
}

static void ad_tick(void *user) {
    (void) user;
    g_timer = 0;
    if (g_flow.phase != KN_AD_COUNTING) return;
    kn_ad_flow_tick(&g_flow);
    ad_render();
    if (g_flow.phase == KN_AD_COUNTING) g_timer = html_timer_add(1000.0, ad_tick, NULL);
}

// the card goes, then onComplete / onError, then onExit (the finally): the feature may open its own window
static void ad_end(const int completed) {
    if (!kn_ad_flow_running(&g_flow)) return;
    if (g_timer) html_timer_cancel(g_timer);
    g_timer = 0;
    const KnAdRequest req = g_req;
    const int shown = g_shown;
    g_shown = 0;
    const int result = kn_ad_flow_end(&g_flow, completed);
    memset(&g_req, 0, sizeof g_req);
    const HtmlElem pop = html_dom_by_id("genericPop");
    char cls[48] = "";
    if (pop && html_dom_get_attr(pop, "class", cls, sizeof cls) >= 0 && strstr(cls, "knAdPop")) ui_loadout_clear_pops();
    const HtmlElem strike = html_dom_by_id("knAdStrikePop");
    if (strike) html_dom_remove(strike);
    KN_LOG_DEBUG1("[ad] placeholder %s\n", result == KN_AD_COMPLETED ? "completed" : "skipped");
    // FRVR.ads.show(type).then(result => { onShow && onShow(); ... }) (deob-10.0.7.js:229900-229912): onShow runs once
    // the ad is over (completed or delivered), right before onComplete or the "skipped" onError; an ad that could not
    // be shown rejects before it (onError, onExit only)
    if (shown && req.on_show) req.on_show(req.user);
    if (result == KN_AD_COMPLETED) {
        if (req.on_complete) req.on_complete(req.user);
    } else if (req.on_error) {
        req.on_error(req.user, "skipped");
    }
    if (req.on_exit) req.on_exit(req.user);
}

int kn_ad_show(const KnAdRequest *req) {
    if (!req || !kn_ad_flow_start(&g_flow, ad_seconds(), req->allow_skip)) return 0;
    g_req = *req;
    if (!ad_popup()) { // nowhere to show the card: the ad fails at once (onError, onExit) instead of staying "running"
        KN_LOG_DEBUG1("[ad] placeholder: the page has no holder for the card\n");
        ad_end(0);
        return 1;
    }
    g_shown = 1;
    if (g_flow.phase == KN_AD_COUNTING) g_timer = html_timer_add(1000.0, ad_tick, NULL);
    return 1;
}

int kn_ad_running(void) { return kn_ad_flow_running(&g_flow); }

// showRewardedAd(cb): showAd({onExit: cb})
static void (*g_rewarded_cb)(void *user);
static void rewarded_exit(void *user) {
    void (*cb)(void *) = g_rewarded_cb;
    g_rewarded_cb = NULL;
    if (cb) cb(user);
}

int kn_ad_show_rewarded(void (*cb)(void *user), void *user) {
    if (kn_ad_running()) return 0;
    g_rewarded_cb = cb;
    const KnAdRequest req = {.on_exit = rewarded_exit, .user = user};
    if (kn_ad_show(&req)) return 1;
    g_rewarded_cb = NULL;
    return 0;
}

// forceVideoAd: "vad" 1 asked, 2 shown, 3 completed (setTimeout 200 ms), -1 failed
static KnAdVideoStateFn g_video_send;
static double g_video_last;

static void video_send(const int state) {
    if (g_video_send) g_video_send(state);
}
static void video_shown(void *user) {
    (void) user;
    video_send(2);
}
static void video_completed_later(void *user) {
    (void) user;
    video_send(3);
}
static void video_complete(void *user) {
    (void) user;
    html_timer_add(200.0, video_completed_later, NULL);
}
static void video_error(void *user, const char *error) {
    (void) user;
    (void) error;
    video_send(-1);
}

int kn_ad_force_video(const KnAdVideoStateFn send_state) {
    if (kn_ad_running() || !kn_ad_flow_video_gate(&g_video_last, (double) time(NULL) * 1000.0)) return 0; // Date.now()
    g_video_send = send_state;
    video_send(1);
    const KnAdRequest req = {.on_show = video_shown, .on_complete = video_complete, .on_error = video_error};
    if (kn_ad_show(&req)) return 1;
    video_send(-1);
    return 0;
}

int kn_ad_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    (void) args;
    (void) argc;
    if (!name) return -1;
    if (!strcmp(name, "knAdContinue")) {
        if (g_flow.phase == KN_AD_READY) ad_end(1);
        return 1;
    }
    if (!strcmp(name, "knAdClose")) {
        ad_end(0);
        return 1;
    }
    return -1;
}
