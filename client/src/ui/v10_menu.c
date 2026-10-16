// Krunker v10.0.2 main menu shell on the v10 page (ui/, doc/v10/ui.md 2).
//
// v10 mounts its menu as Svelte components (_52533 into #mainMenuUIMount, v10:222882): the page
// (ui/index.html) keeps the markup those components render for a guest, without their
// listeners. This file gives that markup v10's behaviour through the page's own inline-handler
// contract (onclick names C answers, doc/html-ui-engine.md), so the native engine and web_dom.c
// behave the same:
//   - the action cards (#subLogoButtons, _50410 v10:147318): Quick Match = backToOrigin(false,
//     {exclude: <this lobby>}) (_50443), Host Game = showWindow(42), Find Game = showWindow(48),
//     Custom Games = showGames(); Ranked's click belongs to v10_competitive.c
//   - the match-info bar (_50455, v10:147245): #mapInfoHld / #menuRegionLabel / .menuDebugInfo
//     / #mapVote move into its segments; Invite = copyInviteLink(), Join = openJoinWindow()
//   - the side nav (_50477, v10:146960-147175): order, v10's visibility rules (guest: the big
//     Guide card, no Turf Wars / Market / Leaderboards; signed in: the Battle Pass / Daily Spin
//     cards (v10_rewards / v10_customize), a small Guide item while level <= 20, Turf Wars,
//     Market & Trading, Leaderboards) and the actions of Market & Trading, Store, Challenges,
//     Social and Exit (the desktop client's, v10 showExit). The Guide card, What's New and
//     Community & Events are v10_news.c's, Turf Wars / Leaderboards / Ranked v10_competitive.c's
//   - the header bar (_52526 / _52519, v10:145938-146560): guest "Login or Register" / "Get
//     Signup Rewards", signed in avatar + name + level bar, KR, Junk, RP, Wallet; Notifications,
//     Settings (showWindow(1)), More Krunker (showMoreKrunker(): the moreKrunkerPopup, v10:196675)
//   - the class preview container shown at boot (menuClassContainer.style.display = '', v10:222888)
//   - the Live Streams overlay (.streams-overlay in #tlInfHold, v10:145187 / 146035): the brain's
//     /streams/top (Twitch's live Krunker streams, streams.c), the small view's three cards, a click
//     opens twitch.tv/<name> in the browser; no answer hides it, no live streams shows v10's text;
//     the cards' Twitch previews come through the brain's /streams/thumb (stream_thumbs.c)
//   - the menu's idle state (v10_menu_tick): Popular Now (_52548 v10:147200, svelte-6ewa5r: the top 3
//     custom maps of the game list by players, asked again every 60 s), the match-info bar's
//     #activeBuffs (svelte-1we2ubc: 2x XP / 2x Class XP / 2x Raid Drops, set at every INIT as
//     v10:219599-219601 does, #buffSepStart shown with any of them, v10:147300), the Store item's
//     shop badge (limited spins on now, rotating every 4 s) and KR DISCOUNT countdown (KR sale
//     weekends, v10:146842-146893), and the CurrencyDisplay's flying "+N" (svelte-1gpc1nd, v10:25667:
//     v10_flying_number). v10's header bar (_52526) writes plain text into #menuKRCount /
//     #menuJNKCount / #menuRPCount: no flying number and no count animation there; v10 shows the
//     flying number only on the Ranked shop's RP balance (CurrencyDisplay showFlyingNumbers,
//     v10:113910).
//   - while the client is in a krunker.io server (krunker_net_active), the header shows the krunker.io
//     account alone (krunker_account.h): its name and level, or Login; the KrunkNative account's bars,
//     Inbox and Wallet are hidden, nav and Spectate follow a guest. Leaving the server restores them.
// The login / register modal (loginOrRegister) is v10_login.c.
#include <v10_menu.h>
#include <backend.h>
#include <backend_ui.h>
#include <client_settings.h>
#include <i18n.h>
#include <krunker_account.h>
#include <krunker_net.h>
#include <krunker_lobbies.h> // Popular Now: krunker.io's custom games
#include <html_ui.h>
#include <ui_logic.h>
#include <store_spins.h>
#include <ks_game.h> // ks_wall_ms: the portable wall clock
#include <ctype.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

int  v10_login_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // v10_login.c
void v10_login_account_changed(void);

static struct {
    int wired;            // the page's markup has its handlers (v10_menu_dom_init)
    int signed_in;        // the header / nav state last applied (-1 = none yet)
    int level;
    int notif_open;       // the Notifications toggle (webpush-container) is expanded
    int official;         // the header last followed: 1 = the client is in a krunker.io server (-1 = none yet)
    unsigned kio_gen;     // krunker_account_generation() the header last followed
} V = {.signed_in = -1, .official = -1};

static HtmlElem by_id(const char *id) { return html_dom_by_id(id); }

// ---------------------------------------------------------------- the menu's clock

// The scenario renders (native only: ui_scenario.c; the web build has none)
static int scenario_on(void) {
#ifdef UI_SCENARIO_SUPPORTED
    return ui_scenario_active();
#else
    return 0;
#endif
}


// The date the weekend badges read (new Date() in v10). knV10MenuClock(ms) pins it (tests); a
// --render-scenario render pins it to a weekday (the v10 reference captures show no weekend badge).
#define SCENARIO_CLOCK_MS 1791979200000.0 // 2026-10-14T12:00:00Z, a Wednesday
static double g_clock_ms; // 0 = real time

static double menu_now_ms(void) {
    if (g_clock_ms > 0.0) return g_clock_ms;
    if (scenario_on()) return SCENARIO_CLOCK_MS;
    return ks_wall_ms(); // portable: MinGW's msvcrt has no timespec_get / TIME_UTC
}

static long long days_from_civil(long long y, const int m, const int d) { // proleptic Gregorian, 1970-01-01 = 0
    y -= m <= 2;
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const long long yoe = y - era * 400;
    const long long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const long long doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

static long long civil_year(const long long z0) { // the year of day z0
    const long long z = z0 + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097;
    const long long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const long long mp = (5 * doy + 2) / 153;
    return yoe + era * 400 + (mp >= 10);
}

static int utc_day(const double ms) { // getUTCDay(): 0 Sunday .. 6 Saturday
    const long long day = (long long) floor(ms / 86400000.0);
    return (int) (((day + 4) % 7 + 7) % 7);
}

// Date.prototype.getWeek (v10:24788): the ISO week of the UTC date
static int iso_week(const double ms) {
    const long long day = (long long) floor(ms / 86400000.0);
    const long long thursday = day + 3 - (utc_day(ms) + 6) % 7;
    const long long jan4 = days_from_civil(civil_year(thursday), 1, 4);
    const int jan4_wday = (int) (((jan4 + 4) % 7 + 7) % 7);
    return 1 + (int) floor(((double) (thursday - jan4) - 3.0 + (jan4_wday + 6) % 7) / 7.0 + 0.5);
}

// v10:24796 isWeekend(days, oddWeek, anyWeek): a Saturday / Sunday (UTC) of an odd week (oddWeek),
// an even one (!oddWeek) or any (anyWeek)
static int weekend(const double ms, const int odd_week, const int any_week) {
    const int wday = utc_day(ms);
    if (wday != 6 && wday != 0) return 0;
    const int odd = iso_week(ms) & 1;
    return any_week || (odd_week ? odd : !odd);
}

static int kr_sale(const double ms) { return weekend(ms, 1, 0); }         // v10:24803 (window.isSteamBuild: never here)
static int double_class_xp(const double ms) { return weekend(ms, 0, 0); } // v10:24806
static int double_raid_drops(const double ms) { return weekend(ms, 0, 1); } // v10:24809

int v10_menu_page(void) { return html_ui_available() && by_id("menuBtnQuickMatch") != 0; }

static void set_display(HtmlElem el, const char *value) {
    if (el) html_dom_set_display(el, value);
}

// Svelte's __click + mouseenter playTick on a component root (_50466: playSelect, then the action).
// Only where nothing set one yet: the other v10 areas (news, rewards, market, social, ...) wire
// their entry points with the same v10 handler names, so whichever runs first is the same call.
static void wire(HtmlElem el, const char *onclick) {
    char have[8] = "";
    if (!el || html_dom_get_attr(el, "onclick", have, sizeof(have)) > 0) return;
    html_dom_set_attr(el, "onclick", onclick);
    html_dom_set_attr(el, "onmouseenter", "playTick()");
}

// A side-nav item: the .menuItem around its #menuBtn* title
static HtmlElem nav_item(const char *title_id) {
    HtmlElem t = by_id(title_id);
    return t ? html_dom_closest(t, ".menuItem") : 0;
}

static void wire_nav(const char *title_id, const char *onclick) {
    wire(nav_item(title_id), onclick);
}

// ---------------------------------------------------------------- header bar

static void comma(char *out, const size_t n, const double value) { // _8986: 1234567 -> "1,234,567"
    char digits[32];
    snprintf(digits, sizeof(digits), "%.0f", value < 0 ? 0 : value);
    const size_t len = strlen(digits);
    size_t o = 0;
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = digits[i];
    }
    out[o] = 0;
}

// #menuRPCount (v10 updateMenuAccountData: rankedPoints || 0, v10:97365; the header's _8986 comma, v10:146528).
// A krunker.io account signed in is the active one beside a KrunkNative account (ranked_route.h: krunker.io
// first): its balance, or "--" while no krunker.io game has sent it. Otherwise KrunkNative's value: 0.
static void header_rp(char *out, const size_t n) {
    double points = 0;
    if (!krunker_account_signed_in()) {
        snprintf(out, n, "0");
        return;
    }
    if (krunker_account_ranked_points(&points)) comma(out, n, points);
    else snprintf(out, n, "--");
}

// The krunker.io account's bar (#knKioHeaderBar), shown in a krunker.io server only. In there the
// krunker.io account is the one v10 would show, so the bar is v10's own: signed in, the profile item
// of #signedInHeaderBar (avatar, name, "Lv N" badge; no XP or currencies: krunker.io does not send
// them), as a guest the "Login or Register" item of #signedOutHeaderBar. The only cue that it is the
// krunker.io account is v10's hover tooltip (.ph-tooltip); a click opens the krunker.io account modal
// (kioAccount: Login / Logout there). #knKioHeaderText holds what the bar says (tests).
static void kio_bar_update(const int shown) {
    HtmlElem bar = by_id("knKioHeaderBar");
    if (shown && !bar) {
        HtmlElem out = by_id("signedOutHeaderBar");
        if (out) {
            html_dom_insert_html(out, HTML_INSERT_AFTER_END,
                "<div id=\"knKioHeaderBar\" class=\"svelte-da3zva\" style=\"display:none\">"
                "<div class=\"ph-item svelte-da3zva\" id=\"knKioHeaderItem\" onclick=\"playSelect(0.1),kioAccount()\" onmouseenter=\"playTick()\"></div></div>");
            bar = by_id("knKioHeaderBar");
        }
    }
    if (!bar) return;
    HtmlElem item = by_id("knKioHeaderItem");
    if (shown && item) {
        StrBuf sb = {0};
        if (krunker_account_signed_in()) {
            const int level = krunker_account_level();
            char lv[24];
            snprintf(lv, sizeof(lv), "Lv %d", level > 0 ? level : 1);
            html_dom_set_attr(item, "class", "ph-item svelte-da3zva");
            sb_append(&sb, "<img class=\"ph-avatar svelte-da3zva\" src=\"./img/player_pfp.png\"> <div class=\"ph-info svelte-da3zva\" id=\"knKioHeaderText\">"
                           "<div class=\"ph-name-row svelte-da3zva\"><span class=\"ph-name svelte-da3zva\">");
            sb_append_esc(&sb, krunker_account_name()[0] ? krunker_account_name() : I18N("kn.accounts.signedIn", "signed in"));
            sb_append(&sb, "</span></div> <div class=\"ph-level-row svelte-da3zva\"><span class=\"ph-level-badge svelte-da3zva\">");
            sb_append_esc(&sb, lv);
            sb_append(&sb, "</span></div></div> <div class=\"ph-tooltip svelte-da3zva\">");
            sb_append_esc(&sb, I18N("kn.accounts.kioHeaderTip", "Your krunker.io account: krunker.io servers see it"));
            sb_append(&sb, "</div>");
        } else {
            html_dom_set_attr(item, "class", "ph-item ph-login-wrap svelte-da3zva");
            sb_append(&sb, "<span class=\"material-icons ph-icon svelte-da3zva\" style=\"color:#fff\">person</span> <span class=\"ph-label svelte-da3zva\" id=\"knKioHeaderText\">");
            sb_append_esc(&sb, I18N("kn.accounts.kioHeaderGuest", "Login or Register"));
            sb_append(&sb, "</span> <div class=\"ph-tooltip svelte-da3zva\">");
            sb_append_esc(&sb, I18N("kn.accounts.kioHeaderGuestTip", "Log in with your krunker.io account: krunker.io servers see it"));
            sb_append(&sb, "</div>");
        }
        char *html = sb_finish(&sb);
        if (html) html_dom_set_html(item, html);
        free(html);
    }
    set_display(bar, shown ? "" : "none");
}

// #signedInHeaderBar (_52526) for an account, #signedOutHeaderBar (_52519) for a guest. Both live
// in #playerHeaderEl; v10 swaps the component, here the guest bar stays in the page and the
// signed-in one is built next to it once.
// In a krunker.io server the header's account is the krunker.io one (kio_bar_update), the KrunkNative
// bars are hidden. acc: the KrunkNative account, NULL while in a krunker.io server.
static void header_update(const AccountInfo *acc, const int official) {
    HtmlElem out = by_id("signedOutHeaderBar");
    HtmlElem in = by_id("signedInHeaderBar");
    if (acc && !in && out) {
        html_dom_insert_html(out, HTML_INSERT_AFTER_END,
            "<div id=\"signedInHeaderBar\" class=\"svelte-da3zva\">"
            "<div class=\"ph-item svelte-da3zva\" id=\"knV10Profile\" onclick=\"playSelect(0.1),showWindow(5)\" onmouseenter=\"playTick()\">"
            "<img class=\"ph-avatar svelte-da3zva\" id=\"knV10Avatar\" src=\"./img/player_pfp.png\"> <div class=\"ph-info svelte-da3zva\">"
            "<div class=\"ph-name-row svelte-da3zva\"><span class=\"ph-name svelte-da3zva\" id=\"knV10Name\"> </span></div> "
            "<div class=\"ph-level-row svelte-da3zva\"><span class=\"ph-level-badge svelte-da3zva\" id=\"knV10Level\"> </span> "
            "<div class=\"ph-xp-bar svelte-da3zva\"><div class=\"ph-xp-fill svelte-da3zva\" id=\"knV10XpFill\"></div></div></div></div></div> "
            "<div class=\"verticalSeparator svelte-da3zva\" style=\"height:35px;\"></div> "
            "<div class=\"ph-item svelte-da3zva\" onclick=\"playSelect(0.1),windows[13].purchase(1)\" onmouseenter=\"playTick()\">"
            "<span class=\"ph-currency-icon svelte-da3zva\" style=\"color:#fbc02d\">KR</span> <span class=\"ph-value svelte-da3zva\" id=\"menuKRCount\"> </span></div> "
            "<div class=\"verticalSeparator svelte-da3zva\" style=\"height:35px;\"></div> "
            "<div class=\"ph-item svelte-da3zva\" onclick=\"playSelect(0.1),windows[13].craftMenu()\" onmouseenter=\"playTick()\">"
            "<span class=\"material-icons ph-icon svelte-da3zva\" style=\"color:#cc51f8\">plumbing</span> <span class=\"ph-value svelte-da3zva\" id=\"menuJNKCount\"> </span></div> "
            "<div class=\"verticalSeparator svelte-da3zva\" style=\"height:35px;\"></div> "
            "<div class=\"ph-item svelte-da3zva\" onclick=\"playSelect(0.1),openRankedMenu()\" onmouseenter=\"playTick()\">"
            "<span class=\"material-icons ph-icon svelte-da3zva\" style=\"color:#4caf50;margin-top:-4px\">emoji_events</span> <span class=\"ph-value svelte-da3zva\" id=\"menuRPCount\"> </span></div> "
            "<div class=\"verticalSeparator svelte-da3zva\" style=\"height:35px;\"></div> "
            // Wallet (_52516: the inventory / currencies view): the Account window's inventory
            "<div class=\"ph-item svelte-da3zva\" onclick=\"playSelect(0.1),showWindow(5)\" onmouseenter=\"playTick()\">"
            "<span class=\"material-icons ph-icon svelte-da3zva\">backpack</span> <span class=\"ph-label svelte-da3zva\">Wallet</span></div></div>");
        in = by_id("signedInHeaderBar");
    }
    set_display(out, acc || official ? "none" : "");
    set_display(in, acc ? "" : "none");
    // the Inbox entry (_52295) of .headerBarRight: signed in only
    HtmlElem inbox = by_id("knV10Inbox");
    if (acc && !inbox) {
        HtmlElem right = html_dom_query(0, "#playerHeaderEl .headerBarRight");
        if (right) html_dom_insert_html(right, HTML_INSERT_AFTER_BEGIN,
            "<div id=\"knV10Inbox\" style=\"display:contents\"><div class=\"nav-item svelte-11p80bh\" role=\"button\" tabindex=\"0\" "
            "onclick=\"playSelect(0.1),showWindow(30)\" onmouseenter=\"playTick()\"><div class=\"nav-icon-wrap svelte-11p80bh\">"
            "<span class=\"material-icons nav-mat-icon svelte-11p80bh\">email</span></div> <span class=\"nav-label svelte-11p80bh\">Inbox</span> "
            "<div id=\"mailCount\" class=\"nav-badge svelte-11p80bh\"></div></div> <div class=\"verticalSeparator\" style=\"height:35px;\"></div></div>");
        inbox = by_id("knV10Inbox");
    }
    set_display(inbox, acc ? "contents" : "none");
    kio_bar_update(official);
    if (!acc) return;

    HtmlElem el;
    // premiumT && settings.tmp.premiumBadge ? alias : name
    if ((el = by_id("knV10Name"))) html_dom_set_text(el, acc->premium && !ui_setting_is_false("premiumBadge") && acc->alias[0] ? acc->alias : acc->name);
    char text[48];
    snprintf(text, sizeof(text), "Lv %d", acc->level > 0 ? acc->level : 1);
    if ((el = by_id("knV10Level"))) html_dom_set_text(el, text);
    snprintf(text, sizeof(text), "width:%.6g%%", acc->level_prog);
    if ((el = by_id("knV10XpFill"))) html_dom_set_attr(el, "style", text);
    comma(text, sizeof(text), acc->funds);
    if ((el = by_id("menuKRCount"))) html_dom_set_text(el, text);
    if ((el = by_id("menuJNKCount"))) html_dom_set_text(el, "0.00"); // _8988(junk.toFixed(2)): KrunkNative accounts have no junk
    if ((el = by_id("menuRPCount"))) {
        char rp[32];
        header_rp(rp, sizeof(rp));
        html_dom_set_text(el, rp);
    }
    if ((el = by_id("mailCount"))) {
        char count[16] = "";
        if (acc->mail_unread > 99) snprintf(count, sizeof(count), "99+");
        else if (acc->mail_unread > 0) snprintf(count, sizeof(count), "%d", acc->mail_unread);
        html_dom_set_text(el, count);
        html_dom_set_display(el, acc->mail_unread > 0 ? "block" : "none");
    }
}

// ---------------------------------------------------------------- side nav

// v10's visibility (_52878 = !account, _52869 = (account.level || 0) <= 20)
static void nav_update(const AccountInfo *acc) {
    const int guest = !acc;
    const int new_player = (acc ? acc->level : 0) <= 20;
    HtmlElem guide_card = html_dom_query(0, "#menuItemContainer .guideItem");
    set_display(guide_card, guest ? "" : "none");
    set_display(by_id("knV10GuideItem"), !guest && new_player ? "" : "none");
    // the divider after What's New: {#if _52869}
    HtmlElem dividers[4];
    const int nd = html_dom_query_all(0, "#menuItemContainer > .sidebarDivider", dividers, 4);
    if (nd > 0) set_display(dividers[0], new_player ? "" : "none");
    set_display(nav_item("menuBtnTurfWars"), guest ? "none" : "");
    set_display(nav_item("menuBtnMarket"), guest ? "none" : "");
    set_display(nav_item("menuBtnLeaderboards"), guest ? "none" : "");
}

// --render-scenario state.officialSession: the menu as in a krunker.io server (its header) without one
static int g_scenario_official;
void v10_menu_scenario_official(const int on) {
    g_scenario_official = on;
    v10_menu_account_changed();
}
int v10_menu_official_session(void) { return krunker_net_active() || g_scenario_official; }
static int official_session(void) { return v10_menu_official_session(); }

void v10_menu_account_changed(void) {
    if (!v10_menu_page()) return;
    // a krunker.io server: the KrunkNative account is not what the menu shows (its nav, Spectate and
    // header bars follow a guest; the krunker.io bar shows the krunker.io account)
    const int official = official_session();
    const AccountInfo *acc = official ? NULL : account_get();
    header_update(acc, official);
    nav_update(acc);
    // hideSpectateIfGuestUser (v10:208579-208581, after a login v10:209811 and a logout v10:209874):
    // the menu's Spectate switch is for accounts only (main.css hides #spectButtonHolder)
    set_display(by_id("spectButtonHolder"), acc ? "block" : "none");
    V.signed_in = acc != NULL;
    V.level = acc ? acc->level : 0;
    V.official = official;
    V.kio_gen = krunker_account_generation();
    ranked_ui_account_changed(); // v10's $account effect on the ranked popup (v10:111598-111600)
    v10_login_account_changed(); // a login closes the modal (v10: $account set -> the modal leaves the stack)
}

// ---------------------------------------------------------------- the idle state

// Popular Now (_52548): fetchMatches -> the entries with players > 0, sorted by players
// (descending, stable), the first 3; rendered as the first child of #subLogoButtons while there is
// one. onMount fetches once, then every 60 s unless the rail is hidden (offsetParent === null).
// KrunkNative: the first fetch waits for the brain probe (backend_available) instead of answering
// {} while it runs, which would leave the rail empty for the first minute.
#define POPULAR_MAX 3
#define POPULAR_POOL 64
#define POPULAR_REFRESH_MS 60000.0
static struct {
    int count;
    char name[POPULAR_MAX][128];
    int players[POPULAR_MAX];
    char key[POPULAR_MAX * 140];
    int first_done;  // onMount's fetch went out
    int timer;       // the 60 s interval (html timer id)
} P;

static HtmlElem popular_rail(void) { return html_dom_query(0, "#subLogoButtons > .popRail"); }

static void popular_render(void) {
    HtmlElem rail = popular_rail();
    if (!P.count) {
        if (rail) html_dom_remove(rail);
        return;
    }
    char key[sizeof P.key] = "";
    for (int i = 0; i < P.count; i++) {
        char part[140];
        snprintf(part, sizeof part, "%s\x1f%d\x1e", P.name[i], P.players[i]);
        strncat(key, part, sizeof key - strlen(key) - 1);
    }
    if (rail && !strcmp(key, P.key)) return;
    snprintf(P.key, sizeof P.key, "%s", key);
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popRail svelte-6ewa5r\"><div class=\"popRailHeader svelte-6ewa5r\">");
    sb_append_esc(&sb, I18N("menu.popular.header", "Popular Now"));
    sb_append(&sb, "</div> <div class=\"popRailCards svelte-6ewa5r\">");
    for (int i = 0; i < P.count; i++) {
        char count[24], playing[64];
        snprintf(count, sizeof count, "%d", P.players[i]);
        I18N_F(playing, "menu.popular.players", "{0} playing", count);
        sb_append(&sb, "<div class=\"popRailCard svelte-6ewa5r\" title=\"");
        sb_append_esc(&sb, P.name[i]);
        sb_appendf(&sb, "\" onclick=\"knV10PopularPlay(%d)\" onmouseenter=\"playTick()\"><div class=\"popRailInfo svelte-6ewa5r\"><div class=\"popRailName svelte-6ewa5r\">", i);
        sb_append_esc(&sb, P.name[i]);
        sb_append(&sb, "</div> <div class=\"popRailCount svelte-6ewa5r\">");
        sb_append_esc(&sb, playing);
        sb_append(&sb, "</div></div> <span class=\"material-icons popRailPlay svelte-6ewa5r\">play_arrow</span></div>");
    }
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    if (rail) html_dom_remove(rail);
    HtmlElem holder = by_id("subLogoButtons");
    if (holder) html_dom_insert_html(holder, HTML_INSERT_AFTER_BEGIN, html);
    free(html);
}

static void popular_matches(void *user, const cJSON *matches) {
    (void) user;
    struct { const char *name; int players; } pool[POPULAR_POOL];
    int n = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, matches) {
        const int players = cJSON_IsNumber(m) ? (int) m->valuedouble : 0;
        if (players <= 0 || !m->string || n >= POPULAR_POOL) continue;
        pool[n].name = m->string;
        pool[n++].players = players;
    }
    for (int i = 1; i < n; i++) { // Array.sort((a, b) => b.players - a.players): stable
        const int p = pool[i].players;
        const char *name = pool[i].name;
        int j = i - 1;
        while (j >= 0 && pool[j].players < p) {
            pool[j + 1] = pool[j];
            j--;
        }
        pool[j + 1].players = p;
        pool[j + 1].name = name;
    }
    P.count = n < POPULAR_MAX ? n : POPULAR_MAX;
    for (int i = 0; i < P.count; i++) {
        snprintf(P.name[i], sizeof P.name[i], "%s", pool[i].name);
        P.players[i] = pool[i].players;
    }
    if (v10_menu_page()) popular_render();
}

static void popular_fetch(void) { servers_ui_fetch_matches(popular_matches, NULL); }

// krunker.io's lobby list for Popular Now (its custom games, v10 fetchMatches): asked a few seconds after the
// menu's first draw rather than with it, so a proxy the player sets right after the start (Settings >
// Client) carries it; then with every 60 s refresh. Its answer asks Popular Now again
// (v10_menu_popular_changed); cached, never more often than krunker_lobbies.h allows.
#define POPULAR_KIO_DELAY_MS 3000.0
static void popular_kio_refresh(void *user) {
    (void) user;
    krunker_lobbies_refresh(0);
}

void v10_menu_popular_changed(void) {
    if (P.first_done && v10_menu_page()) popular_fetch();
}

static void popular_interval(void *user) {
    (void) user;
    P.timer = html_timer_add(POPULAR_REFRESH_MS, popular_interval, NULL);
    HtmlElem rail = popular_rail();
    if (rail && !html_dom_is_shown(rail)) return; // offsetParent === null
    krunker_lobbies_refresh(0);
    popular_fetch();
}

// ---------------------------------------------------------------- Live Streams (.streams-overlay)

// The Live Streams overlay (_52196 / _52549, v10:145187, 146035): the top right panel's card list.
// fetchTopStreams (_52138, v10:145037) asks /streams/top, then every 30 s (v10:146178). KrunkNative's
// brain answers it from Twitch's live streams of the Krunker category (streams.c), in v10's
// {streams, promo, dropCampaigns} shape. The small view (v10's default, _52546) shows the first three
// cards: a red live dot with the viewer count (formatViewerCount: "1.2K"), the title, the streamer's
// name, and a click that opens twitch.tv/<name> (openStream, v10:145133). An answer with no live
// streams shows v10's text (streamers.none, "No active streams!"). No answer (no brain, the feature
// off) hides the overlay.
#define STREAMS_SHOWN 3
#define STREAMS_REFRESH_MS 30000.0
#define STREAMS_NAME_MAX 25
static struct {
    int listed;               // 1 = the last answer was a list (the overlay shows), 0 = hidden
    int count;                // live streams in the answer
    int n;                    // cards kept (at most STREAMS_SHOWN)
    char user[STREAMS_SHOWN][64];
    char title[STREAMS_SHOWN][160];
    char thumb[STREAMS_SHOWN][320];
    int viewers[STREAMS_SHOWN];
    int closed;               // the Hide button (v10's closed view): the cards fold away, the header stays
    int timer;                // the 30 s interval (html timer id)
    int override;             // tests: knV10Streams gave the list, the brain is not asked
} L;

// a Twitch login: letters, digits and '_' (the card's link is twitch.tv/<name>)
// The cards' previews. The engine loads page and asset images only, so a thumbnailUrl
// (https://static-cdn.jtvnw.net/...) is asked of the brain's /streams/thumb (stream_thumbs.c:
// allow-listed Twitch CDN hosts, size-capped, cached), which answers it as a data: URL; the card
// shows it once it arrives (blank until then, or when the brain cannot fetch it). Kept for
// STREAMS_THUMB_TTL_MS per URL; a failed URL is asked again after STREAMS_THUMB_RETRY_MS.
#define STREAMS_THUMB_SLOTS 8
#define STREAMS_THUMB_TTL_MS 300000.0
#define STREAMS_THUMB_RETRY_MS 60000.0
static struct {
    char url[320];
    char *image;    // the data: URL (owned), NULL = none (yet)
    double at;      // menu_now_ms() of the request
    double used;
    unsigned gen;   // bumped at every reuse: a late answer for the old URL is dropped
    int pending;
} T[STREAMS_THUMB_SLOTS];

static const char *streams_thumb_image(const char *url) {
    for (int i = 0; url[0] && i < STREAMS_THUMB_SLOTS; i++)
        if (T[i].image && !strcmp(T[i].url, url)) return T[i].image;
    return "";
}

static void streams_render(void);

static void streams_thumb_body(void *user, const int status, const char *body, size_t length) {
    (void) length;
    const uintptr_t tag = (uintptr_t) user;
    const int i = (int) (tag & 0xff);
    if (i < 0 || i >= STREAMS_THUMB_SLOTS || T[i].gen != (unsigned) (tag >> 8)) return;
    T[i].pending = 0;
    cJSON *res = status == 200 && body ? cJSON_Parse(body) : NULL;
    const char *img = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(res, "data"), "image"));
    if (img && (!strncmp(img, "data:image/jpeg;base64,", 23) || !strncmp(img, "data:image/png;base64,", 22))) {
        free(T[i].image);
        T[i].image = strdup(img);
        if (v10_menu_page()) streams_render();
    }
    cJSON_Delete(res);
}

// asks the brain for url's preview unless it is cached, fresh or on its way
static void streams_thumb_want(const char *url) {
    if (!url[0] || backend_available() != 1) return;
    const double now = menu_now_ms();
    int slot = -1;
    for (int i = 0; i < STREAMS_THUMB_SLOTS; i++)
        if (T[i].url[0] && !strcmp(T[i].url, url)) slot = i;
    if (slot >= 0) {
        T[slot].used = now;
        const double age = now - T[slot].at;
        if (T[slot].pending || age < (T[slot].image ? STREAMS_THUMB_TTL_MS : STREAMS_THUMB_RETRY_MS)) return;
    } else {
        slot = 0; // a free slot, else the least recently used one
        for (int i = 0; i < STREAMS_THUMB_SLOTS; i++) {
            if (!T[i].url[0]) {
                slot = i;
                break;
            }
            if (T[i].used < T[slot].used) slot = i;
        }
        free(T[slot].image);
        T[slot].image = NULL;
        T[slot].gen++;
        T[slot].pending = 0;
        snprintf(T[slot].url, sizeof T[slot].url, "%s", url);
        T[slot].used = now;
    }
    char enc[960], path[1024];
    backend_url_encode(enc, sizeof enc, url);
    snprintf(path, sizeof path, "/streams/thumb?url=%s", enc);
    T[slot].at = now;
    T[slot].pending = backend_http_get(path, streams_thumb_body, (void *) (uintptr_t) ((uintptr_t) slot | ((uintptr_t) T[slot].gen << 8)));
}

static int streams_login_ok(const char *s) {
    const size_t n = strlen(s);
    if (!n || n > STREAMS_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char) s[i]) && s[i] != '_') return 0;
    }
    return 1;
}

// formatViewerCount (v10:146000): from 1000 on, one decimal and K
static void streams_viewers(char *out, const size_t n, const int v) {
    if (v >= 1000) snprintf(out, n, "%.1fK", v / 1000.0);
    else snprintf(out, n, "%d", v);
}

// The overlay is the page's own (ui/index.html #knStreams: v10's header with the Hide button); its
// body (the cards, or the empty text) is rebuilt at every answer. No answer: the overlay is hidden.
static void streams_render(void) {
    HtmlElem ov = by_id("knStreams");
    if (!ov) return;
    if (!L.listed) {
        set_display(ov, "none");
        return;
    }
    set_display(ov, "");
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"streams-body svelte-60shhe\" id=\"knStreamsBody\">");
    if (!L.count) {
        sb_append(&sb, "<div class=\"streams-empty svelte-60shhe\" style=\"padding:8px 4px;font-size:14px;\">");
        sb_append_esc(&sb, I18N("streamers.none", "No active streams!"));
        sb_append(&sb, "</div>");
    } else {
        sb_appendf(&sb, "<div class=\"streams-grid regular-grid svelte-60shhe\" style=\"grid-template-columns: repeat(%d, 180px);\">", L.n);
        for (int i = 0; i < L.n; i++) {
            char count[24], views[64];
            streams_viewers(count, sizeof count, L.viewers[i]);
            I18N_F(views, "streamers.views", "{0} viewers", count);
            sb_appendf(&sb, "<div class=\"stream-card svelte-60shhe\" style=\"pointer-events:all;cursor:pointer;\" title=\"");
            sb_append_esc(&sb, L.title[i]);
            sb_appendf(&sb, "\" onclick=\"knV10Stream(%d)\"><div class=\"thumbnail svelte-60shhe\">"
                            "<img loading=\"lazy\" decoding=\"async\" class=\"svelte-60shhe\" id=\"knStreamThumb%d\" src=\"", i, i);
            sb_append_esc(&sb, streams_thumb_image(L.thumb[i])); // the brain's data: URL of thumbnailUrl
            sb_append(&sb, "\" alt=\"");
            sb_append_esc(&sb, L.title[i]);
            sb_append(&sb, "\"> <div class=\"viewer-count svelte-60shhe\"><span class=\"dot svelte-60shhe\"></span> <span class=\"svelte-60shhe\">");
            sb_append_esc(&sb, views);
            sb_append(&sb, "</span></div></div><div class=\"stream-info svelte-60shhe\"><h3 class=\"stream-title svelte-60shhe\">");
            sb_append_esc(&sb, L.title[i]);
            sb_append(&sb, "</h3><p class=\"stream-username svelte-60shhe\">");
            sb_append_esc(&sb, L.user[i]);
            sb_append(&sb, "</p></div></div>");
        }
        sb_append(&sb, "</div>");
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    HtmlElem old = by_id("knStreamsBody");
    if (old) html_dom_remove(old);
    if (html) html_dom_insert_html(ov, HTML_INSERT_BEFORE_END, html);
    free(html);
    set_display(by_id("knStreamsBody"), L.closed ? "none" : "");
    HtmlElem hide = by_id("knStreamsHide");
    if (hide) html_dom_set_text(hide, L.closed ? "Show" : "Hide");
}

// an answer: the streams array of /streams/top's data (NULL = no answer: hidden)
static void streams_apply(const cJSON *streams) {
    L.listed = cJSON_IsArray(streams);
    L.count = L.n = 0;
    const cJSON *s = NULL;
    if (L.listed) cJSON_ArrayForEach(s, streams) {
        const cJSON *u = cJSON_GetObjectItemCaseSensitive(s, "username");
        if (!cJSON_IsString(u) || !streams_login_ok(u->valuestring)) continue;
        L.count++;
        if (L.n >= STREAMS_SHOWN) continue;
        const cJSON *t = cJSON_GetObjectItemCaseSensitive(s, "title");
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(s, "viewerCount");
        const cJSON *th = cJSON_GetObjectItemCaseSensitive(s, "thumbnailUrl");
        snprintf(L.user[L.n], sizeof L.user[0], "%s", u->valuestring);
        snprintf(L.title[L.n], sizeof L.title[0], "%s", cJSON_IsString(t) ? t->valuestring : "");
        const int https = cJSON_IsString(th) && !strncmp(th->valuestring, "https://", 8);
        snprintf(L.thumb[L.n], sizeof L.thumb[0], "%s", https ? th->valuestring : "");
        L.viewers[L.n] = cJSON_IsNumber(v) && v->valuedouble > 0 ? (int) v->valuedouble : 0;
        streams_thumb_want(L.thumb[L.n]);
        L.n++;
    }
    if (v10_menu_page()) streams_render();
}

static void streams_body(void *user, const int status, const char *body, size_t length) {
    (void) user;
    (void) length;
    cJSON *res = status == 200 && body ? cJSON_Parse(body) : NULL;
    streams_apply(cJSON_GetObjectItemCaseSensitive(cJSON_GetObjectItemCaseSensitive(res, "data"), "streams"));
    cJSON_Delete(res);
}

static void streams_fetch(void) {
    if (L.override) return;
    if (backend_available() != 1 || !backend_http_get("/streams/top?cb=2", streams_body, NULL)) streams_apply(NULL);
}

static void streams_interval(void *user) {
    (void) user;
    L.timer = html_timer_add(STREAMS_REFRESH_MS, streams_interval, NULL);
    streams_fetch();
}

// #activeBuffs (_52546): the badges' display at every INIT (v10:219599-219601) and #buffSepStart
// after them (its MutationObserver, v10:147302)
static char g_buff_init[48]; // the INIT (map, mode) the badges were set for

static void buffs_update(const Client *client, const int force) {
    const Game *game = client ? &client->game : NULL;
    char init[sizeof g_buff_init];
    snprintf(init, sizeof init, "%p|%p", game ? (const void *) game->map : NULL, game ? (const void *) game->mode : NULL);
    if (!force && !strcmp(init, g_buff_init)) return; // v10 keeps what the last INIT set
    snprintf(g_buff_init, sizeof g_buff_init, "%s", init);
    const double now = menu_now_ms();
    const int xp = game && game->map && game->mode && game->mode->config.double_xp;
    const int class_xp = game && game->map && double_class_xp(now), raid = game && game->map && double_raid_drops(now);
    set_display(by_id("doubleClassXPAd"), class_xp ? "block" : "none");
    set_display(by_id("doubleRaidDropsAd"), raid ? "block" : "none");
    set_display(by_id("doubleXPAd"), xp ? "block" : "none");
    set_display(by_id("buffSepStart"), class_xp || raid || xp ? "" : "none");
}

// The Store item's children (v10:146879-146893, 147097-147111): the shop badge with the current
// message ("<name> spin available" per limited wheel on now, "Bundle sale on now" for a discounted
// bundle: KrunkNative sells none) and, on KR sale weekends, KR DISCOUNT with the time to Monday 00:00
// UTC. Only their texts change once shown, so heartbeat / shine / krSaleGlow keep running.
#define SHOP_BADGE_ROTATE_TICKS 4 // setInterval 4000 ms, counted on the 1 s one
static struct {
    int ticks, index; // the 1 s interval's count; the message shown
    int timer;        // the 1 s interval (html timer id)
} S;

// v10:24816 (a wheel with a startDate / endDate): now in (startDate, endDate]
static int wheel_live(const StoreWheel *w, const double now) {
    if (!w->start_ms && !w->end_ms) return 1;
    const double start = w->start_ms ? (double) w->start_ms : now - 1000.0, end = w->end_ms ? (double) w->end_ms : now + 1000.0;
    return now > start && !(now > end);
}

static void kr_sale_timer(const double now, char *out, const size_t n) { // v10:146845
    out[0] = 0;
    const int wday = utc_day(now);
    const int add = wday == 6 ? 2 : wday == 0 ? 1 : 0;
    if (!add) return;
    const double end = (floor(now / 86400000.0) + add) * 86400000.0;
    const double left = end - now;
    if (left <= 0) return;
    const long long h = (long long) floor(left / 3600000.0), m = (long long) floor(fmod(left, 3600000.0) / 60000.0),
                    sec = (long long) floor(fmod(left, 60000.0) / 1000.0);
    snprintf(out, n, "%02lld:%02lld:%02lld", h, m, sec);
}

static void store_item_update(void) {
    HtmlElem title = by_id("menuBtnShop");
    if (!title) return;
    const double now = menu_now_ms();
    char messages[8][64];
    int count = 0;
    const StoreSpins *spins = store_spins_builtin();
    for (int i = 0; spins && i < spins->wheel_count && count < 8; i++) {
        const StoreWheel *w = &spins->wheels[i];
        if ((w->start_ms || w->end_ms) && !w->hide && !w->locked && wheel_live(w, now))
            snprintf(messages[count++], sizeof messages[0], "%s spin available", w->name);
    }
    if (count > 1 && S.ticks > 0 && S.ticks % SHOP_BADGE_ROTATE_TICKS == 0) S.index = (S.index + 1) % count;
    HtmlElem badge = html_dom_query(title, ".shop-badge");
    if (count) {
        char text[80];
        snprintf(text, sizeof text, " %s", messages[S.index % count]);
        if (!badge) {
            HtmlElem kr = html_dom_query(title, ".kr-sale-info");
            if (kr) html_dom_remove(kr); // the badge comes first (re-added below)
            html_dom_insert_html(title, HTML_INSERT_BEFORE_END, "<span class=\"shop-badge svelte-159x57l\"><span class=\"shop-badge-star svelte-159x57l\">&#9733;</span><span class=\"knShopBadgeText\"></span></span>");
            badge = html_dom_query(title, ".shop-badge");
        }
        HtmlElem t = badge ? html_dom_query(badge, ".knShopBadgeText") : 0;
        char have[80] = "";
        if (t) html_dom_get_text(t, have, sizeof have);
        if (t && strcmp(have, text) != 0) html_dom_set_text(t, text);
    } else if (badge) {
        html_dom_remove(badge);
    }
    HtmlElem kr = html_dom_query(title, ".kr-sale-info");
    if (kr_sale(now)) {
        char timer[32];
        kr_sale_timer(now, timer, sizeof timer);
        if (!kr) {
            html_dom_insert_html(title, HTML_INSERT_BEFORE_END, " <span class=\"kr-sale-info svelte-159x57l\"><span class=\"kr-sale-title svelte-159x57l\">KR DISCOUNT</span> "
                                                                "<span class=\"kr-sale-timer svelte-159x57l\"></span></span>");
            kr = html_dom_query(title, ".kr-sale-info");
        }
        HtmlElem t = kr ? html_dom_query(kr, ".kr-sale-timer") : 0;
        char have[32] = "";
        if (t) html_dom_get_text(t, have, sizeof have);
        if (t && strcmp(have, timer) != 0) html_dom_set_text(t, timer);
    } else if (kr) {
        html_dom_remove(kr);
    }
}

// The CurrencyDisplay's flying number (_8800 v10:25667, mounted by _8787 into a fixed full-window
// layer on <body>): value > 0 "+N" green, < 0 "-N" red, centred over the display (left = its
// centre, top = its top - 10), flyUp 1.5 s, removed after 1.5 s; a new one replaces the last.
static struct {
    int seq, timer;
    char id[24];
} F;

static void flying_remove(void *user) {
    (void) user;
    F.timer = 0;
    HtmlElem el = F.id[0] ? by_id(F.id) : 0;
    if (el) html_dom_remove(el);
    F.id[0] = 0;
}

void v10_flying_number(const HtmlElem anchor, const double value) {
    HtmlRect r;
    HtmlElem body = html_dom_body();
    if (!anchor || !body || !html_dom_get_rect(anchor, &r)) return;
    if (F.timer) html_timer_cancel(F.timer);
    flying_remove(NULL);
    char digits[40], text[48];
    comma(digits, sizeof digits, fabs(value)); // _8986 (toString + thousands commas)
    snprintf(text, sizeof text, "%s%s", value > 0 ? "+" : value < 0 ? "-" : "", digits);
    snprintf(F.id, sizeof F.id, "knV10Fly%d", ++F.seq);
    char html[512];
    snprintf(html, sizeof html,
             "<div id=\"%s\" style=\"position: fixed; top: 0px; left: 0px; pointer-events: none; z-index: 9999; width: 100%%; height: 100%%;\">"
             "<div class=\"flying-number svelte-1gpc1nd%s\" style=\"left: %.6gpx; top: %.6gpx;\">%s</div></div>",
             F.id, value > 0 ? " positive" : value < 0 ? " negative" : "", r.x + r.w / 2.0f, r.y - 10.0f, text);
    html_dom_insert_html(body, HTML_INSERT_BEFORE_END, html);
    F.timer = html_timer_add(1500.0, flying_remove, NULL); // onComplete after 0x5dc ms
}

static void store_item_interval(void *user) { // the KR sale timer's 1 s interval
    (void) user;
    S.timer = html_timer_add(1000.0, store_item_interval, NULL);
    store_item_update();
    S.ticks++;
}

void v10_menu_tick(void) {
    if (!V.wired || !v10_menu_page()) return;
    if (!P.first_done) { // onMount (see popular_render)
        if (backend_available() == -1 && !scenario_on()) return;
        P.first_done = 1;
        popular_fetch();
        P.timer = html_timer_add(POPULAR_REFRESH_MS, popular_interval, NULL);
        html_timer_add(POPULAR_KIO_DELAY_MS, popular_kio_refresh, NULL);
        streams_fetch(); // fetchTopStreams on mount (_52138, v10:146178), then every 30 s
        L.timer = html_timer_add(STREAMS_REFRESH_MS, streams_interval, NULL);
        store_item_interval(NULL);
    }
    buffs_update(menu_ui()->client, 0);
}

// Per frame (backend_frame): joining or leaving a krunker.io server, or the krunker.io account's
// sign-in / out, changes the header at once (no need for the menu's own account change).
void v10_menu_account_tick(void) {
    if (!V.wired || !v10_menu_page()) return;
    if (official_session() == V.official && krunker_account_generation() == V.kio_gen) return;
    v10_menu_account_changed();
}

// ---------------------------------------------------------------- set-up

// The match-info bar's segments take the menu's info elements, as its $effect does (v10:147258).
static void match_info_setup(void) {
    HtmlElem segs[4];
    const int n = html_dom_query_all(0, "#matchInfoHolder > .match-seg", segs, 4);
    if (n < 4) return;
    HtmlElem el;
    if ((el = by_id("mapInfoHld"))) {
        html_dom_set_display(el, "");
        html_dom_append(segs[0], el);
    }
    if ((el = by_id("menuServerInfoContainer"))) html_dom_set_display(el, "");
    if ((el = by_id("menuRegionLabel"))) {
        html_dom_append(segs[1], el);
        char text[64] = ""; // the moved label's text again: the native engine lays a moved text run out at 0 x 0
        html_dom_get_text(el, text, sizeof(text));
        html_dom_set_text(el, text);
    }
    if ((el = html_dom_query(0, "#menuServerInfoContainer .menuDebugInfo"))) html_dom_append(segs[3], el);
    if ((el = by_id("mapVote"))) { // shown even while empty: the segment's gap before it is part of the bar's width
        html_dom_set_display(el, "");
        html_dom_append(segs[3], el);
    }
    wire(by_id("inviteButton"), "playSelect(0.1),copyInviteLink()");
    wire(by_id("menuBtnJoin"), "playSelect(0.1),openJoinWindow()");
}

static void nav_setup(void) {
    // the Guide card, What's New, Community & Events: v10_news.c; Turf Wars / Leaderboards:
    // v10_competitive.c (the signed-in Guide entry below calls the same openGuide())
    HtmlElem guide_card = html_dom_query(0, "#menuItemContainer .guideItem");
    // the signed-in Guide entry (a plain menuItem, v10:146966), placed where Svelte renders it
    if (guide_card && !by_id("knV10GuideItem"))
        html_dom_insert_html(guide_card, HTML_INSERT_AFTER_END,
            "<div class=\"menuItem svelte-fgmdj8\" id=\"knV10GuideItem\" style=\"display:none\" onclick=\"playSelect(0.1),openGuide()\" "
            "onmouseenter=\"playTick()\"><span class=\"material-icons-outlined menuItemIcon svelte-fgmdj8\">school</span> "
            "<div class=\"menuItemTitle svelte-fgmdj8\">Guide</div></div>");
    wire_nav("menuBtnMarket", "playSelect(0.1),openExchange()");
    wire_nav("menuBtnShop", "playSelect(0.1),showWindow(14)");
    wire_nav("menuBtnChall", "playSelect(0.1),windows[32].openHome()");
    wire_nav("menuBtnSocial", "playSelect(0.1),openSocial()");
    // Exit (showExit = typeof window.closeClient == "function": the desktop client); its display
    // is the Client settings' exitButton (ui_client_settings.c apply_exit_button)
    HtmlElem holder = by_id("menuItemContainer");
    if (holder && !by_id("clientExit"))
        html_dom_insert_html(holder, HTML_INSERT_BEFORE_END,
            "<div class=\"menuItem svelte-fgmdj8\" id=\"clientExit\" onclick=\"playSelect(0.1),clientExitPopup()\" onmouseenter=\"playTick()\">"
            "<span class=\"material-icons-outlined menuItemIcon svelte-fgmdj8\">exit_to_app</span> "
            "<div class=\"menuItemTitle svelte-fgmdj8\" id=\"menuBtnExit\">Exit</div></div>");
}

static void header_setup(void) {
    HtmlElem login = html_dom_query(0, "#signedOutHeaderBar .ph-login-wrap");
    if (login) html_dom_set_attr(login, "id", "knV10LoginBtn");
    wire(login, "playSelect(0.1),loginOrRegister()");
    wire(by_id("signupRewardsButton"), "playSelect(0.1),showSignupRewards()");
    HtmlElem items[4];
    const int n = html_dom_query_all(0, "#playerHeaderEl .headerBarRight > .nav-item", items, 4);
    for (int i = 0; i < n; i++) {
        char label[32] = "";
        HtmlElem l = html_dom_query(items[i], ".nav-label");
        if (l) html_dom_get_text(l, label, sizeof(label));
        if (!strcmp(label, "Settings")) {
            html_dom_set_attr(items[i], "id", "knV10Settings");
            wire(items[i], "playSelect(0.1),showWindow(1)");
        } else if (!strcmp(label, "More Krunker")) {
            html_dom_set_attr(items[i], "id", "knV10MoreKrunker");
            wire(items[i], "playSelect(0.1),showMoreKrunker()");
        }
    }
    wire(html_dom_query(0, "#playerHeaderEl .webpush-container"), "knV10Notifications()");
}

void v10_menu_dom_init(void) {
    if (!v10_menu_page()) return;
    // the action cards (Ranked's click: v10_competitive.c). Ranked's card carries its "2x KR"
    // ribbon (the component's children slot, _50464 v10:147331)
    HtmlElem ranked = by_id("menuBtnRanked");
    if (ranked && !html_dom_query(ranked, ".menuItemRankedLabel"))
        html_dom_insert_html(ranked, HTML_INSERT_BEFORE_END, " <div class=\"menuItemRankedLabel\">2x KR</div>");
    wire(by_id("menuBtnQuickMatch"), "playSelect(0.1),backToOrigin()");
    wire(by_id("menuBtnHost"), "playSelect(0.1),showWindow(42)");
    wire(by_id("menuBtnBrowser"), "playSelect(0.1),showWindow(48)");
    wire(by_id("menuBtnCustomGames"), "playSelect(0.1),showGames()");
    match_info_setup();
    nav_setup();
    header_setup();
    // menuClassContainer.style.display = '' once the menu is mounted (v10:222888)
    set_display(by_id("menuClassContainer"), "");
    // the idle state starts over with the page (v10_menu_tick: Popular Now's onMount, the intervals)
    html_timer_cancel(P.timer);
    html_timer_cancel(S.timer);
    html_timer_cancel(L.timer);
    P.timer = S.timer = L.timer = 0;
    P.first_done = 0;
    streams_render(); // the last answer stays (the overlay is hidden until the first one: its markup starts hidden)
    P.key[0] = 0;
    g_buff_init[0] = 0;
    V.wired = 1;
    V.signed_in = -1;
    v10_menu_account_changed();
}

// ---------------------------------------------------------------- handlers

// genericPopup(html, cls): #genericPop over #popupBack
static void generic_popup(const char *html, const char *cls) {
    ui_loadout_clear_pops();
    HtmlElem holder = by_id("popupHolder");
    set_display(holder, "block");
    HtmlElem pop = by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", cls);
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

// showMoreKrunker(): the moreKrunkerPopup (_66797). Mods: window.canShowMods (the Mod Manager
// exists natively); Sandbox: openURL("/?sandbox") = KrunkNative's embedded offline game; Editor:
// openURL("/editor.html") (app_mode); KPD: showPolicePop(); "Download our client": the Client
// window (showWindow(25), download_ui.c). Accounts: KrunkNative's saved accounts (alt_manager_ui.c),
// not a Krunker option.
static void more_krunker(void) {
    char accounts_action[48];
    snprintf(accounts_action, sizeof(accounts_action), "clearPops(),showWindow(%d)", ALT_MANAGER_WINDOW);
    const struct { const char *title, *symbol, *img, *desc, *action; } k_options[] = {
        {"Mods", "color_lens", NULL, "Customize your experience", "clearPops(),showWindow(4)"},
        {"Sandbox", "castle", NULL, "Try every skin", "clearPops(),knHostOffline()"},
        {"Editor", "construction", NULL, "Create your own maps and game modes", "clearPops(),openURL('/editor.html')"},
        {"KPD", NULL, "./img/police.png", "Stop criminals", "clearPops(),shoPolicePop()"},
        {"Accounts", "switch_account", NULL, "Switch between your own accounts", accounts_action},
    };
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"moreKrunkerHolder svelte-19783yx\"><div class=\"moreKrunkerTitle svelte-19783yx\"><span class=\"svelte-19783yx\">"
                   "Find more ways to enjoy Krunker</span></div> <div class=\"moreKrunkerContent svelte-19783yx\"><div class=\"moreKrunkerOptions svelte-19783yx\">");
    for (size_t i = 0; i < sizeof(k_options) / sizeof(k_options[0]); i++) {
        sb_appendf(&sb, "<div class=\"moreKrunkerOption svelte-19783yx\" onclick=\"%s\" onmouseenter=\"playTick()\"><div class=\"moreKrunkerOptionTitle svelte-19783yx\">%s</div> ",
                   k_options[i].action, k_options[i].title);
        if (k_options[i].img) sb_appendf(&sb, "<img style=\"width: 76px; height: 74px;\" src=\"%s\">", k_options[i].img);
        else sb_appendf(&sb, "<span class=\"material-icons-outlined\" style=\"font-size: 76px; color: #fff; vertical-align: middle;\">%s</span>", k_options[i].symbol);
        sb_appendf(&sb, " <div class=\"moreKrunkerOptionDescription svelte-19783yx\">%s</div></div>", k_options[i].desc);
    }
    sb_append(&sb, "</div> <div class=\"moreKrunkerClient svelte-19783yx\"><img src=\"./img/client-promo.png\" class=\"svelte-19783yx\"> "
                   "<div class=\"moreKrunkerClientBtn svelte-19783yx\" onclick=\"clearPops(),showWindow(25)\" onmouseenter=\"playTick()\"><span class=\"svelte-19783yx\">"
                   "Download our client</span></div> <div class=\"moreKrunkerClientDesc svelte-19783yx\"><span class=\"svelte-19783yx\">"
                   "Play Krunker on your PC to get increased performance</span></div></div></div></div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "moreKrunkerPopup");
    free(html);
}

// The Notifications toggle (webpush-container, _52289): expanded it says what v10 says where the
// Push API is missing ("Push notifications are not supported in your browser.").
static void notifications_toggle(void) {
    HtmlElem box = html_dom_query(0, "#playerHeaderEl .webpush-container");
    if (!box) return;
    V.notif_open = !V.notif_open;
    html_dom_set_class(box, "not-expanded", !V.notif_open);
    HtmlElem toggle = html_dom_query(box, ".webpush-details-toggle");
    if (toggle) html_dom_set_text(toggle, V.notif_open ? "\xE2\x96\xBC" : "\xE2\x96\xB6"); // ▼ / ▶
    HtmlElem content = html_dom_query(box, ".webpush-content");
    if (!content) {
        html_dom_insert_html(box, HTML_INSERT_BEFORE_END,
            "<div class=\"webpush-content svelte-5kyah8\"><div class=\"webpush-message unsupported svelte-5kyah8\"><p class=\"svelte-5kyah8\">"
            "Push notifications are not supported in your browser.</p></div></div>");
        content = html_dom_query(box, ".webpush-content");
    }
    set_display(content, V.notif_open ? "" : "none");
}

// backToOrigin(false, {exclude: ?game}): Quick Match. A new matchmaker seek that leaves this lobby
// (servers_ui_quick_match); offline (no matchmaker) the only game is the embedded one: play it.
static void quick_match(void) {
    if (servers_ui_quick_match()) {
        menu_close_window();
        return;
    }
    menu_log("Quick Match: no matchmaker (offline), playing the local game");
    menu_play();
}

int v10_menu_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    const int login = v10_login_handle(name, ev, args, argc);
    if (login >= 0) return login;
    if (!strcmp(name, "backToOrigin")) {
        quick_match();
        return 1;
    }
    if (!strcmp(name, "showMoreKrunker")) {
        more_krunker();
        return 1;
    }
    if (!strcmp(name, "knV10Notifications")) {
        notifications_toggle();
        return 1;
    }
    if (!strcmp(name, "knV10StreamsToggle")) { // the overlay's Hide / Show (v10's closed view: the header stays)
        L.closed = !L.closed;
        streams_render();
        return 1;
    }
    if (!strcmp(name, "knV10Stream")) { // a Live Streams card: openStream(username) = window.open("https://twitch.tv/" + username)
        const int i = argc > 0 ? (int) args[0].num : -1;
        if (i < 0 || i >= L.n) return 1;
        char url[128];
        snprintf(url, sizeof url, "https://twitch.tv/%s", L.user[i]);
        ui_open_external(url);
        return 1;
    }
    if (!strcmp(name, "knV10Streams")) { // tests: the Live Streams list from a JSON {"streams":[...]} ("" = back to the brain's answer)
        const char *json = argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL;
        L.override = json && *json;
        if (L.override) {
            cJSON *res = cJSON_Parse(json);
            streams_apply(cJSON_GetObjectItemCaseSensitive(res, "streams"));
            cJSON_Delete(res);
        } else {
            streams_fetch();
        }
        return 1;
    }
    if (!strcmp(name, "knV10PopularPlay")) { // a Popular Now card: playSelect(); openURL("/?play=" + name, 1)
        const int i = argc > 0 ? (int) args[0].num : -1;
        if (i < 0 || i >= P.count) return 1;
        const HtmlArg map = {.kind = HTML_ARG_STR, .str = P.name[i]};
        html_ui_dispatch("playSelect", ev, NULL, 0);
        // a map only krunker.io's custom games run: the busiest of them (krunker.io's own /?play=)
        if (servers_ui_popular_official(P.name[i])) return 1;
        html_ui_dispatch("playMap", ev, &map, 1); // KrunkNative: ?play= in place (servers_ui.c)
        return 1;
    }
    if (!strcmp(name, "knV10MenuClock")) { // tests: the date the weekend badges read (ms since 1970, 0 = now); an INIT re-reads it
        g_clock_ms = argc > 0 ? args[0].num : 0.0;
        buffs_update(menu_ui()->client, 1);
        store_item_update();
        return 1;
    }
    if (!strcmp(name, "knV10FlyingNumber")) { // tests: v10_flying_number(querySelector(sel), value)
        const char *sel = argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL;
        v10_flying_number(sel ? html_dom_query(0, sel) : 0, argc > 1 ? args[1].num : 0.0);
        return 1;
    }
    if (!strcmp(name, "knV10Popular")) { // tests: fetch again now (the 60 s interval's body); a JSON game list first replaces the brain's
        if (argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str) {
            cJSON *list = cJSON_Parse(args[0].str);
            if (list) servers_ui_set_games(list);
            cJSON_Delete(list);
        }
        popular_fetch();
        return 1;
    }
    return -1;
}
