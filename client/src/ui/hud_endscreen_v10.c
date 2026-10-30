// Krunker v10.0.2's end-of-match components on the end screen card (hud_endscreen_v10.h): the rank
// change, Other Earnings, the raid promo, the Collect Rewards button and the rewards overlay.
//
// Source: ~/.cache/kn-official/game.deob.js (`g:N`): GUI.endScreen.show() g:203320-203388 mounts
// the Svelte components onto the card; the components are g:195637 (rank change, svelte-19e82s0),
// g:195699 (other earnings), g:195990 (raid promo), g:195975 (Collect button, svelte-3svau8),
// g:195767 (rewards overlay, svelte-1sac6ik), g:15652 (mount(): a fixed pointer-events:none wrapper
// in <body>), flying item g:195648 (svelte-5yw9uo) and flying icon g:195881 (svelte-ory46c).
//
// The markup carries v10's own classes (ui/css/svelte.css is v10's copy), so what the engine can
// not paint yet lights up when it can: the card flip (3D transforms, ui-e2-paint) and the
// bezier flight of the items (@property --progress in calc(), ui-e1-anim).
//
// The overlay's chained setTimeouts (g:195771-195973) are one absolute-time schedule here, run from
// the per-frame tick: a clock jump (the --render-scenario capture clock) fires every step that fell
// into it in order, each derived from its scheduled time, never from the jump.
#include <hud_endscreen_v10.h>
#include <hud_v10.h>
#include <ranked.h>
#include <html_dom.h>
#include <html_ui.h>
#include <i18n.h>
#include <ui_logic.h>
#include <backend_ui.h>
#include <client.h>
#include <double_xp_offer.h>
#include <kn_ad.h> // the Double XP button's rewarded ad (the placeholder)
#include <v10_ui.h>
#include <audio.h>
#include "../viewer/viewer_store.h"
#include <game_config.h> // game_config_is_custom (showRaidPromo's !isCustom)
#include <raid_drops.h>  // raid_double_drops (isDoubleRaidDrops)
#include <ks_game.h>     // ks_wall_ms: the UTC clock of the weekend rule
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

#define END_ITEMS_MAX 48
#define END_EVENTS_MAX 512
#define AUTO_COLLECT_MS 10000.0 // autoCollectTime = now + 0x2710
#define RAID_MODE_INDEX 23      // showRaidPromo: 0x17 !== modeIndex
// #doubleXPButton (v10.js 203200, every match but a comp one, signed in or not): the ad icon and the label
// (window.endgame.player.doubleGainedXP, %s). The click claims at games % 3 (double_xp_end_claim, double_xp_offer.c).
// v10's whitespace kept: the newline + tabs between the inline icon and #returnText lay out as one space.
#define DOUBLE_XP_HTML "<div id=\"doubleXPButton\" class=\"doubleXPButton\" onmouseenter=\"playTick(0.1);\" onclick=\"knDoubleXP()\">\n\t\t\t\t\t\t" \
    "<img style=\"vertical-align: middle;display:inline\" height=\"60px\" src=\"./img/frvr-ui/ad-icon.svg\">\n\t\t\t\t\t\t" \
    "<div id=\"returnText\">%s</div>\n\t\t\t\t\t</div>"
// #returnButton (v10.js 203200, a comp match, isComp): "Return to Krunker" (%s) leaves the match for the menu:
// v10's backToOrigin(), here h_return_to_krunker (see there for the disconnect notice that differs).
#define RETURN_HTML "<div id=\"returnButton\" onclick=\"knReturnToKrunker()\">\n\t\t\t\t\t\t" \
    "<div id=\"returnText\">%s</div>\n\t\t\t\t\t</div>"

typedef struct {
    int skin;
    char icon[32]; // "./img/<icon>.webp"
} EndItem;

enum { EV_PLAN, EV_FLIP, EV_SOUND, EV_BACKPACK, EV_FLY, EV_FLY_ANIM, EV_FLY_DONE, EV_FINISH, EV_ICON_END };
typedef struct {
    double at;
    int type;
    int idx;
} EndEvent;

static struct {
    // the stores
    int rank, mmr, rp;
    // otherData
    EndItem items[END_ITEMS_MAX];
    int item_count;
    int show_raid, double_raid;
    double auto_at_ms; // autoCollectTime
    int collected;     // hasCollectedRewards
    int results;       // endScreen.init ran (the results show): autoCollectTime is set
    // the Collect button component
    int btn_mounted, secs;
    double next_tick;
    // the rewards overlay
    int ov_active, ov_n, ov_cols, ov_rows, ov_backpack, ov_fly_active, ov_fly_count;
    double ov_t0;
    unsigned char flipped[END_ITEMS_MAX], flying[END_ITEMS_MAX];
    int order[END_ITEMS_MAX];
    EndEvent ev[END_EVENTS_MAX];
    int ev_count;
    double now;
    int css_added, handler_added;
    // --render-scenario: start the overlay / auto-collect when the card first shows
    int scn_collect; // 0 none, 1 overlay, 2 auto
    int scn_claim;   // state.endscreen.doubleXP "claim": the Double XP button clicked when the card shows
    int force_raid;     // state.raidPromo (0 / 1) for a console `end` in a scenario, -1: roll
    double scn_elapsed; // state.endscreen.autoCollectElapsed: ms from endScreen.init to the button's mount
} E = {.force_raid = -1};

/* ------------------------------------------------------------ helpers */

static void add_event(const double at, const int type, const int idx) {
    if (E.ev_count >= END_EVENTS_MAX) return;
    int i = E.ev_count++;
    while (i > 0 && E.ev[i - 1].at > at) { // stable: after the events of the same time
        E.ev[i] = E.ev[i - 1];
        i--;
    }
    E.ev[i].at = at;
    E.ev[i].type = type;
    E.ev[i].idx = idx;
}

static void sound_unwrap(void) { audio_play("unwrap_0", 1.0f, 0, 1.0f); } // playSound('unwrap_0', undefined, false, -1)

static HtmlElem by_id(const char *id) { return html_dom_by_id(id); }

static void remove_id(const char *id) {
    const HtmlElem e = by_id(id);
    if (e) html_dom_remove(e);
}

static void ensure_css(void) {
    if (E.css_added) return;
    E.css_added = 1;
    html_ui_add_stylesheet("css/kn-v10-endscreen.css");
}

static int card_present(void) { return html_dom_query(0, "#endTabbedView .endCard") != 0; }

static const char *rarity_color(const ViewerSkin *skin) {
    if (!skin || skin->rarity < 0 || skin->rarity >= g_viewer_rarity_count) return "#B0B0B0";
    return g_viewer_rarities[skin->rarity].color;
}

// The item's face: a spray (type 4) is a sprite on a background, everything else an img.
static void item_face(StrBuf *sb, const int skin_index, const char *cls) {
    const ViewerSkin *skin = viewer_skin(skin_index);
    char url[256] = "";
    store_skin_preview(skin_index, url, sizeof(url));
    if (skin && skin->type == 4) {
        sb_appendf(sb, "<div class=\"item-img spray-img %s\" style=\"background-image: url(&quot;", cls);
        sb_append_esc(sb, url);
        sb_append(sb, "&quot;);\"></div>");
    } else {
        sb_appendf(sb, "<img draggable=\"false\" class=\"item-img %s%s\" src=\"", cls, skin && (skin->flags & VS_RGB) ? " rgbHue" : "");
        sb_append_esc(sb, url);
        sb_append(sb, "\" alt=\"");
        sb_append_esc(sb, skin && skin->name ? skin->name : "");
        sb_append(sb, "\">");
    }
}

/* ------------------------------------------------------------ rank change, earnings, raid promo */

// svelte-19e82s0 (g:195637), shown when $latestRank > 0; the Rating stat (svelte-m5jxz9) when the change != 0.
static void mount_rank(void) {
    const HtmlElem holder = by_id("endLayoutHolder");
    if (!holder) return;
    const HtmlElem old = html_dom_query(holder, ".rank-display");
    if (old) html_dom_remove(old);
    if (E.rank <= 0) return;
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"rank-display svelte-19e82s0\"><div class=\"rank-content svelte-19e82s0\"><img class=\"rank-icon svelte-19e82s0\" src=\"./");
    sb_append_esc(&sb, ranked_rank_icon(E.rank));
    sb_append(&sb, "\" alt=\"");
    sb_append_esc(&sb, ranked_rank_name(E.rank));
    sb_appendf(&sb, "\"> <div class=\"rank-info svelte-19e82s0\"><div class=\"rank-value svelte-19e82s0\" style=\"color: %s\">", ranked_rank_color(E.rank));
    sb_append_esc(&sb, ranked_rank_name(E.rank));
    sb_append(&sb, "</div></div>");
    if (E.mmr != 0)
        sb_appendf(&sb, " <div class=\"crStat\"><div class=\"crLabel\">Rating</div> <div class=\"crVal svelte-m5jxz9 %s\">%s%d</div></div>",
                   E.mmr > 0 ? "positive" : "negative", E.mmr > 0 ? "+" : "", E.mmr);
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    html_dom_insert_html(holder, HTML_INSERT_BEFORE_END, html);
    free(html);
}

// .otherEarnings (g:195699): "+N RP" in the RP colour while $rankedPointsChange != 0
static void mount_other_earnings(void) {
    const HtmlElem holder = html_dom_query(0, ".otherEarnings");
    if (!holder) return;
    char html[320];
    html[0] = 0;
    if (E.rp != 0)
        snprintf(html, sizeof(html),
                 "<div class=\"crStat\" style=\"display: flex; align-items: center; justify-content: center; padding: 10px 15px;\">"
                 "<div class=\"crVal\"><span style=\"color: #4caf50\">+%d</span> RP</div></div>", E.rp);
    html_dom_set_html(holder, html);
}

// #raidPromoContainer (g:195990). The card opens the Raid Server Setup: openHostWindow(false, 4)
// (deob-10.0.5.js:200914-200916), the Host Game window's Raid type (host_flow_ui.c).
static void mount_raid(void) {
    const HtmlElem holder = by_id("raidPromoContainer");
    if (!holder) return;
    char html[640];
    html[0] = 0;
    if (E.show_raid)
        snprintf(html, sizeof(html),
                 "<div class=\"crStat raidPromo\" style=\"display: flex; align-items: center; justify-content: center; padding: 10px 15px; cursor: pointer;\" "
                 "onclick=\"openHostWindow(false, 4)\"><div class=\"crVal\" style=\"color: %s; font-size: 13px;\">%s "
                 "<span style=\"font-size: 11px; opacity: 0.8; margin-left: 5px;color: gold;\">Play Now &rarr;</span></div></div>",
                 E.double_raid ? "#FFD700" : "#d95487", E.double_raid ? "2x Raid Drops This Weekend!" : "Battle Bosses in Raids!");
    html_dom_set_html(holder, html);
}

/* ------------------------------------------------------------ the Collect Rewards button */

static void add_rewards(void) {
    for (int i = 0; i < E.item_count; i++) hud_ui_add_reward_skin(E.items[i].skin); // iïÏïìïí: tabs.rewards.list.skins
}

// The tab the card shows now (GUI.endScreen.currentTab), from the selected header.
static const char *current_tab(void) {
    const HtmlElem sel = html_dom_query(0, "#endTabbedView .tabHeader.selected");
    char text[32] = "";
    if (sel) html_dom_get_text(sel, text, sizeof(text));
    if (!strcmp(text, "Performance")) return "performance";
    if (!strcmp(text, "Medals")) return "medals";
    if (!strcmp(text, "Rewards")) return "rewards";
    return "leaderboard";
}

static int tab_header_center(const char *label, float *x, float *y) {
    HtmlElem heads[8];
    const int n = html_dom_query_all(0, "#endTabbedView .tabHeader", heads, 8);
    for (int i = 0; i < n; i++) {
        char text[32] = "";
        html_dom_get_text(heads[i], text, sizeof(text));
        HtmlRect r;
        if (!strcmp(text, label) && html_dom_get_rect(heads[i], &r)) {
            *x = r.x + r.w / 2.0f;
            *y = r.y + r.h / 2.0f;
            return 1;
        }
    }
    return 0;
}

static void start_overlay(double now);

// onCollect (g:195886 iíÏiíìï): auto = the 10 s ran out (the icon flies to the Rewards tab),
// else the click (the overlay opens, then the Rewards tab)
static void collect(const int auto_collect, const double now) {
    if (E.collected || E.item_count == 0) return;
    if (!html_ui_is_menu_open()) sound_unwrap(); // $hasMenuUI is false on the end screen
    float fx = 0.0f, fy = 0.0f;
    int have_from = 0;
    const HtmlElem icon = html_dom_query(0, "#collectRewardsButtonContainer .button-icon");
    HtmlRect r;
    if (icon && html_dom_get_rect(icon, &r)) {
        fx = r.x + r.w / 2.0f;
        fy = r.y + r.h / 2.0f;
        have_from = 1;
    }
    char icon_name[32];
    snprintf(icon_name, sizeof(icon_name), "%s", E.items[0].icon);
    E.collected = 1; // GUI.endScreen.hasCollectedRewards
    E.btn_mounted = 0;
    if (auto_collect) {
        add_rewards();
        hud_ui_end_screen_tab(current_tab()); // show(): the button is gone, the Rewards tab lists the skins
        float tx, ty;
        if (have_from && tab_header_center("Rewards", &tx, &ty)) {
            // flying-icon (g:195881): 800 ms, from the button icon to the Rewards tab header
            StrBuf sb = {0};
            sb_appendf(&sb,
                       "<div id=\"knFlyIconW\" style=\"position: fixed; top: 0px; left: 0px; pointer-events: none; z-index: 9999; width: 100%%; height: 100%%;\">"
                       "<div class=\"flying-icon svelte-ory46c animated\" style=\"--duration: 800ms; --start-x: %.2fpx; --start-y: %.2fpx; --mid-x: %.2fpx; --mid-y: %.2fpx; --end-x: %.2fpx; --end-y: %.2fpx;\">"
                       "<img alt=\"Icon\" class=\"svelte-ory46c\" src=\"./img/%s.webp\"></div></div>",
                       fx, fy, fx + 0.5f * (tx - fx), fminf(fy, ty) - 50.0f, tx, ty, icon_name);
            char *html = sb_finish(&sb);
            remove_id("knFlyIconW");
            html_dom_insert_html(html_dom_body(), HTML_INSERT_BEFORE_END, html);
            free(html);
            add_event(now + 800.0, EV_ICON_END, 0);
        }
        return;
    }
    const HtmlElem holder = by_id("collectRewardsButtonContainer"); // unmount(collectRewardsButtonComponent)
    if (holder) html_dom_set_html(holder, "");
    start_overlay(now);
}

static int h_collect(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    collect(0, E.now);
    return 1;
}

// v10's "XP Doubled!" pop (genericPopup, dxp-pop: window.endgame.player.xpDoubled / xpDoubleSubTitle)
static void xp_doubled_pop(void) {
    ui_loadout_clear_pops(); // clearPops() before the popup, as genericPopup does
    char html[1024];
    snprintf(html, sizeof(html),
             "<div class=\"dxp-img-holder\">\n\t\t\t<div class=\"dxp-img-anim\"><div></div></div>\n\t\t\t<div class=\"dxp-img\"></div>\n\t\t</div>\n"
             "\t\t<div class=\"dxp-text\">\n\t\t\t<div class=\"dxp-header\">%s</div>\n"
             "\t\t\t<div class=\"dxp-info\">%s</div>\n\t\t</div>\n"
             "\t\t<div class=\"dxp-btn-holder\">\n\t\t\t<div class='button dxp-btn' onmouseenter=\"playTick(0.1);\" "
             "onclick='playSelect(0.1);clearPops();'>%s</div>\n\t\t</div>",
             I18N("window.endgame.player.xpDoubled", "XP Doubled!"),
             I18N("window.endgame.player.xpDoubleSubTitle", "The more XP the better, right? Let's go!"),
             I18N("generic.nice", "Nice!"));
    v10_popup_show("dxp-pop no-box-shadow", html, NULL);
}

// showDoubleXPRewardedAd()'s answer (double_xp_offer.c): a claimed offer pops "XP Doubled!" (v10's rrqdxp
// answer, _73930). Nothing else shows (v10 shows nothing for a failed claim either).
static void double_xp_done(void *user, const DoubleXpResult result) {
    (void) user;
    if (result == DOUBLE_XP_GRANTED) xp_doubled_pop();
}

// The card's button (v10.js 203200): #returnButton ("Return to Krunker") on a comp match, #doubleXPButton (Double XP)
// on every other match, signed in or not (a signed-out player sees it too, above the signup banner, the v10 sandbox
// capture ingame-endscreen-late). The card is built with the match's comp flag (Game.isComp = game.config.comp):
// no per-frame state decides which one shows. Called by the card builder (hud_ui.c).
const char *hud_endscreen_v10_end_button_html(const Client *client) {
    static char html[512]; // the builder copies it at once
    if (client && client->game.config.comp) {
        snprintf(html, sizeof(html), RETURN_HTML, I18N("kn.endgame.returnToKrunker", "Return to Krunker"));
    } else {
        snprintf(html, sizeof(html), DOUBLE_XP_HTML, I18N("window.endgame.player.doubleGainedXP", "Double XP"));
    }
    return html;
}

// showRewardedAd's callback (onExit, watched or not): claimDoubleXpReward (games % 3, with an account)
static void double_xp_after_ad(void *user) {
    (void) user;
    double_xp_end_claim(double_xp_done, NULL);
}

// The Double XP click: v10's showDoubleXPRewardedAd (deob-10.0.5.js:227983-227991) -> a rewarded ad (KrunkNative's
// placeholder, kn_ad.h) -> claimDoubleXpReward (:227961-227963) once it is closed
static int h_double_xp(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    kn_ad_show_rewarded(double_xp_after_ad, NULL);
    return 1;
}

// The Return click. v10's backToOrigin() reloads the page at the origin (the menu, no seek). The nearest KrunkNative
// action is the console's disconnect (servers_ui_disconnect: the page to "/", no auto-seek), which reports
// "Disconnected" on the menu where v10's reload shows nothing; a silent leave needs ui_console.c's stop_net.
static int h_return_to_krunker(void *user, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) user; (void) ev; (void) args; (void) argc;
    servers_ui_disconnect();
    return 1;
}

static void ensure_handler(void) {
    if (E.handler_added) return;
    E.handler_added = 1;
    html_ui_add_handler("knCollectRewards", h_collect);
    html_ui_add_handler("knDoubleXP", h_double_xp);
    html_ui_add_handler("knReturnToKrunker", h_return_to_krunker);
}

static void update_label(void) {
    const HtmlElem label = html_dom_query(0, "#collectRewardsButtonContainer .auto-collect-label");
    char text[48];
    snprintf(text, sizeof(text), "%ds to auto collect", E.secs);
    if (label) html_dom_set_text(label, text);
}

// The button (g:195974): the first collectable item's icon, COLLECT, the countdown label
static void mount_collect(void) {
    E.btn_mounted = 0;
    if (E.collected || E.item_count == 0 || !E.results) return;
    HtmlElem holder = by_id("collectRewardsButtonContainer");
    if (!holder) {
        const HtmlElem card = html_dom_query(0, "#endTabbedView .endCard");
        if (!card) return;
        if (!by_id("endButtonsHolder")) html_dom_insert_html(card, HTML_INSERT_BEFORE_END, "<div id=\"endButtonsHolder\"></div>");
        const HtmlElem buttons = by_id("endButtonsHolder");
        if (!buttons) return;
        html_dom_insert_html(buttons, HTML_INSERT_BEFORE_END, "<div id=\"collectRewardsButtonContainer\" style=\"position: relative;\"></div>");
        holder = by_id("collectRewardsButtonContainer");
        if (!holder) return;
    }
    ensure_handler();
    E.secs = (int) fmax(0.0, floor((E.auto_at_ms - E.now) / 1000.0)); // iïÎìïìï
    E.next_tick = E.now + 1000.0;                                      // setInterval(.., 1000)
    char html[640];
    snprintf(html, sizeof(html),
             "<button class=\"collect-rewards-button svelte-3svau8\" title=\"Collect Rewards\" onclick=\"knCollectRewards()\" onmouseenter=\"playTick()\">"
             "<img alt=\"Icon\" class=\"button-icon svelte-3svau8\" src=\"./img/%s.webp\"> <span class=\"collect-text svelte-3svau8\">COLLECT</span> "
             "<span class=\"auto-collect-label svelte-3svau8\">%ds to auto collect</span></button>", E.items[0].icon, E.secs);
    html_dom_set_html(holder, html);
    E.btn_mounted = 1;
}

/* ------------------------------------------------------------ the rewards overlay */

// svelte-1sac6ik's column count (g:195781): the grid closest to 3:1, few empty cells
static int overlay_cols(const int n) {
    if (n == 0) return 1;
    int best = 1;
    double best_score = INFINITY;
    for (int c = 1; c <= 12; c++) {
        const int rows = (n + c - 1) / c;
        const int empty = c * rows - n;
        const double ratio = (double) c / rows;
        const double score = 10.0 * fabs(ratio - 3.0) + (double) empty / n * 5.0 + (rows > 5 ? 0.5 * (rows - 5) : 0.0);
        if (score < best_score) {
            best = c;
            best_score = score;
        }
    }
    return best;
}

static double overlay_stagger(const int n) {
    const double step = n > 1 ? fmax(0.0, 1600.0 / (n - 1)) : 0.0; // 0x640 / (n - 1)
    return fmin(step, 2000.0 / n);                                    // 0x7d0 / n
}

static void card_class(const int i, const char *cls, const int on) {
    char id[24];
    snprintf(id, sizeof(id), "knRwCard%d", i);
    const HtmlElem card = by_id(id);
    if (card) html_dom_set_class(card, cls, on);
}

static void start_overlay(const double now) {
    ensure_css();
    E.ov_active = 1;
    E.ov_t0 = now;
    E.ov_n = E.item_count;
    E.ov_cols = overlay_cols(E.ov_n);
    E.ov_rows = E.ov_n == 0 ? 1 : (E.ov_n + E.ov_cols - 1) / E.ov_cols;
    E.ov_backpack = 0;
    E.ov_fly_active = 0;
    E.ov_fly_count = 0;
    memset(E.flipped, 0, sizeof(E.flipped));
    memset(E.flying, 0, sizeof(E.flying));
    StrBuf sb = {0};
    sb_append(&sb, "<div id=\"knRewardsOverlay\" style=\"position: fixed; top: 0px; left: 0px; pointer-events: none; z-index: 9999; width: 100%; height: 100%;\">"
                   "<div class=\"rewards-overlay svelte-1sac6ik\"><div class=\"rewards-container svelte-1sac6ik\">");
    sb_appendf(&sb, " <div class=\"rewards-grid svelte-1sac6ik\" id=\"knRewardsGrid\" style=\"--cols: %d; --rows: %d;\">", E.ov_cols, E.ov_rows);
    for (int i = 0; i < E.ov_n; i++) {
        const ViewerSkin *skin = viewer_skin(E.items[i].skin);
        sb_appendf(&sb, "<div class=\"reward-card svelte-1sac6ik\" id=\"knRwCard%d\" style=\"--rarity-color: %s;\">"
                        "<div class=\"card-front svelte-1sac6ik\"><div class=\"card-back-content svelte-1sac6ik\">"
                        "<img alt=\"Reward\" class=\"svelte-1sac6ik\" src=\"./img/%s.webp\"> <div class=\"reward-count svelte-1sac6ik\">%d</div></div></div> "
                        "<div class=\"card-back svelte-1sac6ik\">",
                   i, rarity_color(skin), E.items[i].icon, i + 1);
        item_face(&sb, E.items[i].skin, "svelte-1sac6ik");
        sb_append(&sb, "</div></div>");
    }
    sb_append(&sb, "</div></div></div></div>");
    char *html = sb_finish(&sb);
    remove_id("knRewardsOverlay");
    html_dom_insert_html(html_dom_body(), HTML_INSERT_BEFORE_END, html);
    free(html);
    add_event(now + 500.0, EV_PLAN, 0); // onMount: setTimeout(.., 500)
}

static void end_overlay(void) {
    E.ov_active = 0;
    remove_id("knRewardsOverlay");
    for (int i = 0; i < E.ev_count;) { // an interrupted overlay leaves nothing behind
        if (E.ev[i].type != EV_ICON_END) {
            E.ev[i] = E.ev[--E.ev_count];
            continue;
        }
        i++;
    }
    for (int i = 0; i < END_ITEMS_MAX; i++) {
        char id[24];
        snprintf(id, sizeof(id), "knFlyW%d", i);
        remove_id(id);
    }
}

// g:195800: the flip order (rows snake: even rows left to right, odd right to left), the flips,
// the backpack, then 600 ms after the last flip the flights
static void plan_overlay(const double at) {
    const int n = E.ov_n;
    const double stagger = overlay_stagger(n);
    int count = 0;
    for (int row = 0; row < E.ov_rows; row++) {
        const int first = row * E.ov_cols;
        const int last = first + E.ov_cols < n ? first + E.ov_cols : n;
        if (row % 2 == 0) {
            for (int i = first; i < last; i++) E.order[count++] = i;
        } else {
            for (int i = last - 1; i >= first; i--) E.order[count++] = i;
        }
    }
    const double backpack_delay = (count - 1) * stagger + 400.0 - 1000.0;
    add_event(backpack_delay > 0.0 ? at + backpack_delay : at, EV_BACKPACK, 0);
    for (int k = 0; k < count; k++) {
        const double t = at + k * stagger;
        add_event(t, EV_FLIP, E.order[k]);
        add_event(t + 200.0, EV_SOUND, 0);
        if (k == count - 1) {
            for (int i = 0; i < n; i++) add_event(t + 600.0 + i * stagger, EV_FLY, i); // setTimeout(.., 0x258) then i * stagger
        }
    }
}

static void fly_item(const int i, const double at) {
    const HtmlElem grid = by_id("knRewardsGrid");
    if (!grid) return;
    HtmlRect g;
    if (!html_dom_get_rect(grid, &g)) return;
    float tx = 50.0f, ty = 50.0f;
    const HtmlElem pack = html_dom_query(0, "#knRewardsOverlay .backpack-target");
    HtmlRect b;
    if (pack && html_dom_get_rect(pack, &b)) {
        tx = b.x + b.w / 2.0f;
        ty = b.y + b.h / 2.0f;
    }
    const int col = i % E.ov_cols, row = i / E.ov_cols;
    const float fx = g.x + 90.0f * col + 40.0f, fy = g.y + 90.0f * row + 40.0f;
    E.flying[i] = 1;
    card_class(i, "flying", 1);
    const ViewerSkin *skin = viewer_skin(E.items[i].skin);
    StrBuf sb = {0};
    sb_appendf(&sb,
               "<div id=\"knFlyW%d\" style=\"position: fixed; top: 0px; left: 0px; pointer-events: none; z-index: 9999; width: 100%%; height: 100%%;\">"
               "<div id=\"knFly%d\" class=\"flying-item svelte-5yw9uo\" style=\"--duration: 400ms; --item-color: %s; --start-x: %.2fpx; --start-y: %.2fpx; "
               "--mid-x: %.2fpx; --mid-y: %.2fpx; --end-x: %.2fpx; --end-y: %.2fpx;\"><div class=\"item-content svelte-5yw9uo\">",
               i, i, rarity_color(skin), fx, fy, fx + 0.5f * (tx - fx), fminf(fy, ty) - 50.0f, tx, ty);
    item_face(&sb, E.items[i].skin, "svelte-5yw9uo");
    sb_append(&sb, "</div></div></div>");
    char *html = sb_finish(&sb);
    html_dom_insert_html(html_dom_body(), HTML_INSERT_BEFORE_END, html);
    free(html);
    E.ov_fly_active++;
    add_event(at + 1000.0 / 60.0, EV_FLY_ANIM, i); // setTimeout(.., delay 0) -> .animated
    add_event(at + 400.0, EV_FLY_DONE, i);
}

static void run_event(const EndEvent *ev) {
    switch (ev->type) {
        case EV_PLAN:
            if (E.ov_active) plan_overlay(ev->at);
            break;
        case EV_FLIP:
            if (!E.ov_active) break;
            E.flipped[ev->idx] = 1;
            card_class(ev->idx, "flipped", 1);
            break;
        case EV_SOUND:
            if (E.ov_active) sound_unwrap();
            break;
        case EV_BACKPACK: {
            if (!E.ov_active || E.ov_backpack) break;
            E.ov_backpack = 1;
            const HtmlElem container = html_dom_query(0, "#knRewardsOverlay .rewards-container");
            if (container)
                html_dom_insert_html(container, HTML_INSERT_AFTER_BEGIN,
                                     "<div class=\"backpack-target svelte-1sac6ik\"><i class=\"material-icons svelte-1sac6ik\" style=\"color:#fff;font-size:50px;\">backpack</i></div>");
            break;
        }
        case EV_FLY:
            if (E.ov_active) fly_item(ev->idx, ev->at);
            break;
        case EV_FLY_ANIM: {
            char id[24];
            snprintf(id, sizeof(id), "knFly%d", ev->idx);
            const HtmlElem item = by_id(id);
            if (item) html_dom_set_class(item, "animated", 1);
            break;
        }
        case EV_FLY_DONE: {
            char id[24];
            snprintf(id, sizeof(id), "knFlyW%d", ev->idx);
            remove_id(id);
            if (!E.ov_active) break;
            E.ov_fly_active--;
            E.ov_fly_count++;
            sound_unwrap();
            if (E.ov_fly_active == 0 && E.ov_fly_count >= E.ov_n) add_event(ev->at + 200.0, EV_FINISH, 0);
            break;
        }
        case EV_FINISH:
            if (!E.ov_active) break;
            end_overlay();
            add_rewards();                   // onCompleteManualCollect: iïÏïìïí() then changeTab('rewards')
            hud_ui_end_screen_tab("rewards");
            break;
        case EV_ICON_END:
            remove_id("knFlyIconW");
            break;
        default:
            break;
    }
}

/* ------------------------------------------------------------ public */

void hud_endscreen_v10_reset(void) {
    if (E.ov_active) end_overlay();
    remove_id("knFlyIconW");
    E.rank = E.mmr = E.rp = 0;
    E.item_count = 0;
    E.show_raid = E.double_raid = 0;
    E.collected = 0;
    E.results = 0;
    E.auto_at_ms = 0.0;
    E.btn_mounted = 0;
    E.ev_count = 0;
    E.scn_collect = 0;
    E.scn_elapsed = 0.0;
}

void hud_endscreen_v10_begin(const Client *client, const double now_ms) {
    E.now = now_ms;
    E.collected = 0;
    E.results = 0; // the card renders at the END too: the button waits for the results (autoCollectTime)
    E.auto_at_ms = 0.0;
    const Game *game = client ? &client->game : NULL;
    // END_GAME's otherData (deob-10.0.4.js:194894-194895): showRaidPromo = 0x17 !== modeIndex && !isCustom && isPubType()
    // && Math.random() < 0.5, isDoubleRaidDrops = the round's flag (updateEventFlags); the promo's isDouble (:207036-207043)
    E.show_raid = game && game_mode_id(game) != RAID_MODE_INDEX && !game_config_is_custom(game) && game_is_pub_type(game) && (rand() & 1);
    if (E.force_raid >= 0) E.show_raid = E.force_raid; // --render-scenario state.raidPromo: the capture's roll
    // The server rolls both and sends them; KN's END has no field for them, so the client takes the round's flag where it
    // is the authority (offline / sandbox: game_update_event_flags set it) and else applies the same rule
    // (raid_double_drops: a public round on a Saturday or Sunday, UTC), which differs only when the clocks disagree
    E.double_raid = game && (game_is_authority(game) ? game->double_raid_drops
                                                     : raid_double_drops(game_is_pub_type(game), (int64_t) ks_wall_ms()));
}

void hud_endscreen_v10_results(const double now_ms) {
    E.now = now_ms;
    // endScreen.init: autoCollectTime = now + 0x2710 (g:203310); a scenario mounts the button some time after it
    E.auto_at_ms = now_ms + AUTO_COLLECT_MS - E.scn_elapsed;
    E.results = 1;
}

void hud_endscreen_v10_ranked(const int mmr_change, const int rank, const int rp_change) {
    E.mmr = mmr_change;
    E.rank = rank;
    E.rp = rp_change;
    if (card_present()) {
        mount_rank();
        mount_other_earnings();
    }
}

static int offer(const int skin, const char *icon) {
    if (E.collected || E.item_count >= END_ITEMS_MAX) return 0;
    E.items[E.item_count].skin = skin;
    snprintf(E.items[E.item_count].icon, sizeof(E.items[E.item_count].icon), "%s", icon);
    E.item_count++;
    if (E.results && card_present() && !E.btn_mounted) mount_collect(); // it arrived after the results showed
    return 1;
}

int hud_endscreen_v10_offer_skin(const int skin) { return offer(skin, "gift"); }

int hud_endscreen_v10_offer_item(const int skin, const char *icon) {
    // `items.some(i => i.r !== undefined)` / filter(t === "skin" && r !== undefined) (deob-10.0.3.js:236769-236771,
    // :227422): an item without an icon is never offered (the account has it all the same). The icon names
    // ./img/<icon>.webp in the markup: a plain name only.
    if (!icon || !icon[0] || skin < 0) return 0;
    for (const char *c = icon; *c; c++)
        if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-')) return 0;
    return offer(skin, icon);
}

void hud_endscreen_v10_shown(void) {
    // the holder and its buttons come with the card (hud_ui.c); this mounts the components and the Collect button
    ensure_css();
    mount_rank();
    mount_other_earnings();
    mount_raid();
    ensure_handler();
    mount_collect();
    if (E.scn_collect) {
        const int mode = E.scn_collect;
        E.scn_collect = 0;
        collect(mode == 2, E.now);
    }
    if (E.scn_claim) { // the scenario's click on the Double XP button (its handler, as a click runs it)
        E.scn_claim = 0;
        h_double_xp(NULL, NULL, NULL, 0);
    }
}

void hud_endscreen_v10_tick(const double now_ms) {
    E.now = now_ms;
    if (E.btn_mounted && !E.collected && by_id("collectRewardsButtonContainer")) {
        while (E.btn_mounted && now_ms >= E.next_tick) {
            E.next_tick += 1000.0;
            E.secs--;
            update_label();
            if (E.secs <= 0) {
                collect(1, E.next_tick - 1000.0);
                break;
            }
        }
    }
    while (E.ev_count > 0 && E.ev[0].at <= now_ms) {
        const EndEvent ev = E.ev[0];
        memmove(&E.ev[0], &E.ev[1], sizeof(EndEvent) * (size_t) --E.ev_count);
        run_event(&ev);
    }
}

void hud_endscreen_v10_scenario(const cJSON *end) {
    if (!cJSON_IsObject(end)) return; // after hud_ui_show_end_screen (begin): the scenario overrides its values
    E.rank = E.mmr = E.rp = 0;
    E.item_count = 0;
    E.show_raid = E.double_raid = 0;
    E.scn_collect = 0;
    const cJSON *rank = cJSON_GetObjectItemCaseSensitive(end, "rank"), *item;
    if (cJSON_IsObject(rank)) {
        const cJSON *r = cJSON_GetObjectItemCaseSensitive(rank, "rank"), *m = cJSON_GetObjectItemCaseSensitive(rank, "mmr");
        E.rank = cJSON_IsNumber(r) ? r->valueint : 0;
        E.mmr = cJSON_IsNumber(m) ? m->valueint : 0;
    }
    if (cJSON_IsNumber(item = cJSON_GetObjectItemCaseSensitive(end, "rp"))) E.rp = item->valueint;
    cJSON_ArrayForEach(item, cJSON_GetObjectItemCaseSensitive(end, "items")) {
        const cJSON *skin = cJSON_GetObjectItemCaseSensitive(item, "skin"), *icon = cJSON_GetObjectItemCaseSensitive(item, "r");
        if (E.item_count >= END_ITEMS_MAX || !cJSON_IsNumber(skin)) continue;
        E.items[E.item_count].skin = skin->valueint;
        snprintf(E.items[E.item_count].icon, sizeof(E.items[E.item_count].icon), "%s", cJSON_IsString(icon) ? icon->valuestring : "gift");
        E.item_count++;
    }
    const cJSON *raid = cJSON_GetObjectItemCaseSensitive(end, "raid");
    E.show_raid = cJSON_IsTrue(raid) || (cJSON_IsNumber(raid) && raid->valueint);
    const cJSON *dbl = cJSON_GetObjectItemCaseSensitive(end, "doubleRaid");
    E.double_raid = cJSON_IsTrue(dbl) || (cJSON_IsNumber(dbl) && dbl->valueint);
    const cJSON *collect_mode = cJSON_GetObjectItemCaseSensitive(end, "collect");
    // the time v10's button mounted after endScreen.init in the capture (its first label: floor((10 s - this) / 1 s))
    const cJSON *elapsed = cJSON_GetObjectItemCaseSensitive(end, "autoCollectElapsed");
    E.scn_elapsed = cJSON_IsNumber(elapsed) ? elapsed->valuedouble : 0.0;
    if (cJSON_IsString(collect_mode)) E.scn_collect = !strcmp(collect_mode->valuestring, "auto") ? 2 : !strcmp(collect_mode->valuestring, "overlay") ? 1 : 0;
    const cJSON *dxp_click = cJSON_GetObjectItemCaseSensitive(end, "doubleXP");
    E.scn_claim = cJSON_IsString(dxp_click) && !strcmp(dxp_click->valuestring, "claim");
}

int hud_endscreen_v10_overlay_active(void) { return E.ov_active; }

void hud_endscreen_v10_scenario_raid(const int raid) { E.force_raid = raid; }
