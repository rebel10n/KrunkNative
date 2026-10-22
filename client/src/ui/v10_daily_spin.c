// Krunker v10.0.2's Daily Spin (v10.js 117982-118000 the $dailySpinState store, 144980 and
// 146815-146990 the sidebar item): a signed-in player earns score in matches; at maxScore (5000)
// the item turns into FREE SPIN, a click claims it (rpc DAILY_SPIN_CLAIM "dsl" -> "dslr"
// {ticketId}) and the ticket's wheel opens (account.addSkin(ticketId); prizeWheel(0, ticketId)).
// The tier is (stage + 1) % 7 == 0 Heroic, % 3 == 0 Elite, else Starter; its ticket is that
// wheel's ticketIndex (store_ui.c / kn_brain store.c data, so v10's 8067 / 8068 / 7993 follow
// the item table). KrunkNative serves the progress over the social socket ("dsp"), not
// gapi /daily-spin/progress (the client never talks to anything but kn_brain).
// The item sits in #menuItemContainer after the Battle Pass item (.bpItem:not(.guideItem)), with
// the sidebar divider v10 draws after it.
#include <ui_logic.h>
#include <client.h> // glfwGetTime
#include <html_ui.h> // html_ui_is_menu_open
#include <backend_ui.h>
#include <i18n.h>
#include <krunker_account.h> // KRUNKER_SVC_DATA: the krunker.io account's progress read (official servers)
#include <cJSON.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define DS_REFRESH_MS 60000.0 // the progress is asked again at most once a minute (and on login)
#define DS_CLAIM_TIMEOUT_MS 15000.0 // a claim with no answer by then is over: the item says so, it does not spin on

static struct {
    int known;               // a "dsp" reply arrived for this account
    int failed;              // the last progress ask was refused or had no answer: the item says Unavailable
    int score, max_score, stage, claimed, claiming;
    double time_remaining;   // ms to the daily reset
    int account_id;          // whose state this is
    double asked_at;         // html timer clock of the last "dsp"
    double claim_at;         // html timer clock of the last "dsl" (its answer clears claiming)
    int tip;                 // hover: the tooltip shows
} g_ds = {.max_score = 5000};

static double now_ms(void) { return glfwGetTime() * 1000.0; }

// ({stage + 1} % 7 == 0 ? Heroic : % 3 == 0 ? Elite : Starter)
static void tier(const int stage, const char **name, const char **color) {
    const int n = stage + 1;
    if (n % 7 == 0) *name = "Heroic", *color = "#fc9803";
    else if (n % 3 == 0) *name = "Elite", *color = "#2196F3";
    else *name = "Starter", *color = "#b2f252";
}

static int claimable(void) { return active_account() && g_ds.known && !g_ds.claimed && g_ds.score >= g_ds.max_score; }

#define DS_TOOLTIP "<div class=\"dsTooltip svelte-159x57l\"><p class=\"svelte-159x57l\">Earn free spins by playing games and scoring points!</p> <div class=\"dsTipTiers svelte-159x57l\"><div class=\"dsTipTier svelte-159x57l\"><div class=\"dsTipName svelte-159x57l\" style=\"color:#b2f252;\">Starter</div> <div class=\"dsTipDesc svelte-159x57l\">Default spin (most days)</div></div> <div class=\"dsTipTier svelte-159x57l\"><div class=\"dsTipName dsTipGlowElite svelte-159x57l\" style=\"color:#2196F3;\">Elite</div> <div class=\"dsTipDesc svelte-159x57l\">Every 3rd day (better rewards)</div></div> <div class=\"dsTipTier svelte-159x57l\"><div class=\"dsTipName dsTipGlowHeroic svelte-159x57l\" style=\"color:#fc9803;\">Heroic</div> <div class=\"dsTipDesc svelte-159x57l\">Every 7th day (best rewards)</div></div></div> <ul class=\"svelte-159x57l\"><li class=\"svelte-159x57l\">Play matches to earn score points</li> <li class=\"svelte-159x57l\">Reach enough points to unlock a free spin</li> <li class=\"svelte-159x57l\">Resets daily — come back tomorrow for more!</li></ul></div>"

static char g_item_html[2048]; // what #dailySpinDiv shows besides the tooltip (written only when it changes)

// _52122's Daily Spin block (v10:146955-147020). Svelte updates only what changed: the item is
// rewritten only when its content does (so .dsClaim's dsGlow keeps its phase), and hovering only
// adds / removes the tooltip ({#if hover}, its dsTipFade runs on each show).
static void render_item(HtmlElem item) {
    const char *name, *color;
    tier(g_ds.stage, &name, &color);
    const int can = claimable(), done = active_account() && g_ds.claimed;
    StrBuf sb = {0};
    sb_append(&sb, "<img class=\"dsLogo svelte-159x57l\" src=\"./img/menu/free_spin.png\" alt=\"Daily Spin\"> <div class=\"dsInfo svelte-159x57l\"><div class=\"menuItemTitle svelte-159x57l\" style=\"color:#fff;\">Daily Spin - ");
    sb_appendf(&sb, "<span class=\"svelte-159x57l\" style=\"color:%s\">%s</span></div> <div class=\"dsMeta svelte-159x57l\">", color, name);
    if (can) {
        sb_appendf(&sb, "<span class=\"dsClaim svelte-159x57l\">%s</span>", g_ds.claiming ? "CLAIMING..." : "FREE SPIN");
    } else if (done) {
        sb_append(&sb, "<span class=\"dsDone svelte-159x57l\">CLAIMED</span>");
    } else if (g_ds.failed && !g_ds.known) {
        sb_appendf(&sb, "<span style=\"color:#fff;\" class=\"svelte-159x57l\">%s</span>", I18N("kn.dailySpin.unavailable", "Unavailable"));
    } else {
        const double pct = g_ds.max_score > 0 ? (g_ds.score >= g_ds.max_score ? 100.0 : 100.0 * g_ds.score / g_ds.max_score) : 0.0;
        sb_appendf(&sb, "<span style=\"color:#fff;\" class=\"svelte-159x57l\">%d/%d</span> <div class=\"dsBar svelte-159x57l\"><div class=\"dsBarFill svelte-159x57l\" style=\"width:%g%%\"></div></div>",
                   g_ds.score, g_ds.max_score, pct);
    }
    sb_append(&sb, "</div></div> ");
    char *html = sb_finish(&sb);
    HtmlElem tip = html_dom_query(item, ".dsTooltip");
    if (strcmp(html, g_item_html) != 0 || (!tip && !html_dom_query(item, ".dsLogo"))) {
        snprintf(g_item_html, sizeof g_item_html, "%s", html);
        html_dom_set_html(item, html);
        tip = 0;
    }
    free(html);
    if (g_ds.tip && !tip) html_dom_insert_html(item, HTML_INSERT_BEFORE_END, DS_TOOLTIP);
    else if (!g_ds.tip && tip) html_dom_remove(tip);
    html_dom_set_class(item, "dsClaimable", can);
    html_dom_set_class(item, "dsClaimed", done);
}

// the item (and v10's divider after it) exists only for an account, in #menuItemContainer
static void sync_item(void) {
    HtmlElem item = html_dom_by_id("dailySpinDiv");
    const int want = active_account() != NULL;
    if (!want) {
        if (item) {
            HtmlElem div = html_dom_by_id("knDailySpinDivider");
            html_dom_remove(item);
            if (div) html_dom_remove(div);
        }
        return;
    }
    if (!item) {
        HtmlElem box = html_dom_by_id("menuItemContainer");
        if (!box) return;
        HtmlElem items[8], bp = 0;
        const int n = html_dom_query_all(box, ".bpItem", items, 8); // the Battle Pass item, not the guests' Guide
        for (int i = 0; i < n && !bp; i++) if (!html_dom_has_class(items[i], "guideItem")) bp = items[i];
        static const char MARKUP[] = "<div class=\"menuItem dsItem svelte-159x57l\" id=\"dailySpinDiv\" onclick=\"knDailySpin()\" onmouseenter=\"playTick();knDailySpinTip(1)\" onmouseleave=\"knDailySpinTip(0)\"></div> <div class=\"sidebarDivider svelte-159x57l\" id=\"knDailySpinDivider\"></div>";
        html_dom_insert_html(bp ? bp : box, bp ? HTML_INSERT_AFTER_END : HTML_INSERT_AFTER_BEGIN, MARKUP);
        item = html_dom_by_id("dailySpinDiv");
        if (!item) return;
        g_item_html[0] = 0;
    }
    render_item(item);
}

// The krunker.io account's progress (official servers): v10's dailySpin.getProgress, GET api.krunker.io
// /daily-spin/progress with the account's token, answered {data: {score, maxScore, timeRemaining, claimed,
// stage}}. The claim is v10's game-socket rpc, which the krunker.io account does not send (krunker_net.h),
// so the item says so instead of sending nothing.
static void kio_progress_done(void *user, const int status, const cJSON *res) {
    const AccountInfo *acc = active_account();
    if (!acc || acc->id != (int) (uintptr_t) user) return; // another account's answer
    const cJSON *data = cJSON_IsObject(res) ? cJSON_GetObjectItem(res, "data") : NULL;
    if (status != 200 || !cJSON_IsObject(data)) { // refused, unreachable or no answer: Unavailable, asked again later
        g_ds.failed = 1;
        sync_item();
        return;
    }
    cJSON *frame = cJSON_CreateArray();
    if (!frame) return;
    cJSON_AddItemToArray(frame, cJSON_CreateString("dsp"));
    cJSON_AddItemToArray(frame, cJSON_Duplicate(data, 1));
    v10_daily_spin_frame("dsp", frame);
    cJSON_Delete(frame);
}

static void ask(void) {
    g_ds.asked_at = now_ms();
    if (account_backend() == ACCOUNT_BACKEND_KRUNKER) {
        const AccountInfo *acc = active_account();
        if (!acc || !krunker_account_api("GET", KRUNKER_SVC_DATA, "/daily-spin/progress", NULL, kio_progress_done,
                                         (void *) (uintptr_t) acc->id)) {
            g_ds.failed = 1; // refused at once (every request slot busy): the item says so
            sync_item();
        }
        return;
    }
    account_send_frame("dsp");
}

void v10_daily_spin_tick(void) {
    const AccountInfo *acc = active_account();
    const int id = acc ? acc->id : 0;
    const double t = now_ms();
    if (id != g_ds.account_id) { // a login / logout: the old state is someone else's
        memset(&g_ds, 0, sizeof(g_ds));
        g_ds.max_score = 5000;
        g_ds.account_id = id;
        if (id) ask();
        sync_item();
        return;
    }
    if (g_ds.claiming && t - g_ds.claim_at > DS_CLAIM_TIMEOUT_MS) { // no "dslr" came: say so, stop CLAIMING...
        g_ds.claiming = 0;
        account_popup(0, I18N("kn.dailySpin.noAnswer", "No answer from Box Office. Try again."), NULL);
        sync_item();
    }
    if (id && html_ui_is_menu_open() && t - g_ds.asked_at > DS_REFRESH_MS) ask();
    if ((id != 0) != (html_dom_by_id("dailySpinDiv") != 0)) sync_item(); // the page was rebuilt
}

void v10_daily_spin_frame(const char *label, const cJSON *frame) {
    if (!strcmp(label, "dsp")) { // ["dsp", {score, maxScore, timeRemaining, claimed, stage}] | ["dsp", null, err]
        const cJSON *d = cJSON_GetArrayItem(frame, 1);
        if (!cJSON_IsObject(d)) { // Box Office could not answer: Unavailable
            g_ds.failed = 1;
            sync_item();
            return;
        }
        g_ds.known = 1;
        g_ds.failed = 0;
        g_ds.score = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(d, "score"));
        const int max = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(d, "maxScore"));
        g_ds.max_score = max > 0 ? max : 5000;
        g_ds.time_remaining = cJSON_GetNumberValue(cJSON_GetObjectItem(d, "timeRemaining"));
        g_ds.claimed = cJSON_IsTrue(cJSON_GetObjectItem(d, "claimed"));
        g_ds.stage = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(d, "stage"));
        if (g_ds.score < 0) g_ds.score = 0;
        sync_item();
        return;
    }
    // ["dslr", {ticketId}] | ["dslr", null, err]: the ticket is the account's; its wheel opens
    g_ds.claiming = 0;
    const cJSON *d = cJSON_GetArrayItem(frame, 1);
    const cJSON *ticket = cJSON_IsObject(d) ? cJSON_GetObjectItem(d, "ticketId") : NULL;
    if (!cJSON_IsNumber(ticket)) {
        const char *err = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 2));
        if (err) account_popup(0, err, NULL);
        sync_item();
        return;
    }
    g_ds.claimed = 1;
    account_add_skin(ticket->valueint);
    sync_item();
    const HtmlArg args[2] = {{.kind = HTML_ARG_NUM, .num = 0}, {.kind = HTML_ARG_NUM, .num = ticket->valuedouble}};
    html_ui_dispatch("prizeWheel", NULL, args, 2); // window.prizeWheel(0, ticketId)
}

int v10_daily_spin_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    if (!strcmp(name, "knDailySpin")) { // the item's click: claim it (Box Office answers the claim, "dslr": a ticket or why not)
        if (g_ds.claiming) return 1;
        if (account_backend() == ACCOUNT_BACKEND_KRUNKER) { // the claim is a game-socket rpc the krunker.io account does not send
            account_popup(0, claimable() ? I18N("kn.dailySpin.officialClaim", "The Daily Spin can't be claimed on krunker.io servers yet. Claim it on a Box Office server.")
                                         : I18N("kn.dailySpin.playMore", "Play more to unlock the daily spin"), NULL);
            return 1;
        }
        g_ds.claiming = 1;
        g_ds.claim_at = now_ms();
        account_send_frame("dsl");
        sync_item();
        return 1;
    }
    if (!strcmp(name, "knDailySpinTip")) {
        g_ds.tip = argc > 0 && args[0].num != 0;
        sync_item();
        return 1;
    }
    return -1;
}
