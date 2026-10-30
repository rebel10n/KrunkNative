// Krunker v10.0.3 main menu shell on the v10 page (ui/, doc/v10/ui.md 2; the shell is the same in 10.0.2, doc/v10/ui-1003-diff.md).
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
//     Social and Exit (the desktop client's only, v10 showExit). The Guide card, What's New and
//     Community & Events are v10_news.c's, Turf Wars / Leaderboards / Ranked v10_competitive.c's
//   - the header bar (_52526 / _52519, v10:145938-146560): guest "Login or Register" / "Get
//     Signup Rewards" (KrunkNative's "Account Manager" between them, showWindow(ALT_MANAGER_WINDOW)),
//     signed in avatar + name + level bar (KrunkNative's Account Manager after it), KR, Junk, RP,
//     Wallet; Notifications, Settings (showWindow(1)),
//     More Krunker (showMoreKrunker(): the moreKrunkerPopup, v10:196675)
//   - the class preview container shown at boot (menuClassContainer.style.display = '', v10:222888)
//   - the Live Streams overlay (.streams-overlay in #tlInfHold, v10:145187 / 146035): /streams/top
//     (Twitch's live Krunker streams) on every Server Type and account: the brain's (streams.c) on Box Office,
//     api.krunker.io's (v10's own, menu_sources.h) otherwise; the small view's three cards, a click
//     opens twitch.tv/<name> in the browser; no answer hides it, no live streams shows v10's text;
//     each card's picture is the answer's thumbnailUrl, its <img src> as v10's (remote_images.c loads it)
//   - the menu's idle state (v10_menu_tick): Popular Now (_52548 v10:147200, svelte-6ewa5r: the top 3
//     custom maps of Box Office's game list by players, on the Box Office Server Type only (menu_sources.h),
//     asked again every 60 s), the match-info bar's
//     #activeBuffs (svelte-1we2ubc: 2x XP / 2x Class XP / 2x Raid Drops, set at every INIT as
//     v10:219599-219601 does, #buffSepStart shown with any of them, v10:147300), the Store item's
//     shop badge (limited spins on now, rotating every 4 s) and KR DISCOUNT countdown (KR sale
//     weekends, v10:146842-146893), and the CurrencyDisplay's flying "+N" (svelte-1gpc1nd, v10:25667:
//     v10_flying_number). v10's header bar (_52526) writes plain text into #menuKRCount /
//     #menuJNKCount / #menuRPCount: no flying number and no count animation there; v10 shows the
//     flying number only on the Ranked shop's RP balance (CurrencyDisplay showFlyingNumbers,
//     v10:113910).
//   - the header, nav and Spectate follow the ACTIVE account (backend_ui.h active_account()): the
//     krunker.io account in an official game, the krunk.io one elsewhere, each shown exactly as v10
//     shows its own (signed-in bar, or the guest's Login / Register). Joining or leaving an official
//     game switches them at once (v10_menu_account_tick).
// The login / register modal (loginOrRegister) is v10_login.c.
#include <v10_menu.h>
#include <player_names.h> // Hide player names (player_names.h)
#include <backend.h>
#include <backend_ui.h>
#include <account_split.h> // the krunker.io identity a streams read may carry (account_split.h)
#include <web_push.h> // the Notifications panel's Web Push stores (web_push.c)
#include <i18n.h>
#include <krunker_account.h>
#include <krunker_net.h>
#include <krunker_kio_routes.h> // the streams' krunker.io URL (krunker_kio_routes.c)
#include <krunker_ranked.h>     // the streams' Bearer on a signed-in krunker.io account (v10's API client interceptor)
#include <menu_sources.h>    // Popular Now's Server Type gate and the streams' host
#include <seek_prefs.h>      // seek_prefs_servers: the Server Type
#include <v10_wallet.h>       // the header's Wallet: the Wallet and Inventory modal (v10_wallet.c)
#include <v10_social.h>       // the Social hub's gapi reads (v10_gapi.c)
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
#include <strings.h>
#include <time.h>

int  v10_login_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // v10_login.c
void v10_login_account_changed(void);
void v10_login_tick(void); // the krunker.io sign-in's progress in the login modal

static struct {
    int wired;            // the page's markup has its handlers (v10_menu_dom_init)
    int signed_in;        // the header / nav state last applied (-1 = none yet)
    int level;
    int notif_open;       // the Notifications toggle (webpush-container) is expanded
    unsigned active_gen;  // account_active_generation() the header last followed
} V = {.signed_in = -1};

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

int v10_menu_kr_sale(void) { return kr_sale(menu_now_ms()); }

int v10_menu_page(void) { return html_ui_available() && by_id("menuBtnQuickMatch") != 0; }

static void set_display(HtmlElem el, const char *value) {
    if (el) html_dom_set_display(el, value);
}

// Svelte's __click + mouseenter playTick on a component root (_50466: playSelect, then the action).
// The click sounds are v10.0.3's: the side-nav items, the action cards, Invite and Join call
// playSelect() with no volume, 0.15 (deob-10.0.3.js:158419-158436, 158443-158458, 158554-158566,
// 254013); the header bar's Settings / More Krunker / Inbox play 0.1 (162512-162525); the
// player header's items (login, signup rewards, profile, KR, Junk, RP, wallet) play none
// (163104-163112, 163665-163730).
// Only where nothing set one yet: the other v10 areas (news, rewards, market, social, ...) wire
// their entry points with the same v10 handler names, so whichever runs first is the same call.
static void notifications_redraw(void);

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

// KrunkNative's Account Manager (ALT_MANAGER_WINDOW, alt_manager_ui.c): a separator and the item,
// with its neighbours' markup (v10 has no such button). The guest bar has it after Login or
// Register, the signed-in bar after the profile item. Icon only (no text label): the header has
// to fit the centred logo at UI scale 1.0 (the Wallet and RP items sit under it otherwise); the
// title keeps the button's name for hover and assistive tech.
static void account_manager_markup(char *out, const size_t n, const char *id) {
    char action[64];
    snprintf(action, sizeof(action), "playSelect(0.1),showWindow(%d)", ALT_MANAGER_WINDOW);
    snprintf(out, n,
        "<div class=\"verticalSeparator svelte-da3zva\" style=\"height:35px;\"></div> "
        "<div class=\"ph-item svelte-da3zva\" id=\"%s\" title=\"Account Manager\" onclick=\"%s\" onmouseenter=\"playTick()\">"
        "<span class=\"material-icons ph-icon svelte-da3zva\" style=\"color:#fff\">switch_account</span></div>", id, action);
}

// #signedInHeaderBar (_52526) for an account, #signedOutHeaderBar (_52519) for a guest. Both live
// in #playerHeaderEl; v10 swaps the component, here the guest bar stays in the page and the
// signed-in one is built next to it once.
// acc: the active account (backend_ui.h), NULL = a guest.
static void header_update(const AccountInfo *acc) {
    HtmlElem out = by_id("signedOutHeaderBar");
    HtmlElem in = by_id("signedInHeaderBar");
    if (acc && !in && out) {
        html_dom_insert_html(out, HTML_INSERT_AFTER_END,
            "<div id=\"signedInHeaderBar\" class=\"svelte-da3zva\">"
            "<div class=\"ph-item svelte-da3zva\" id=\"knV10Profile\" onclick=\"showWindow(5)\" onmouseenter=\"playTick()\">"
            "<img class=\"ph-avatar svelte-da3zva\" id=\"knV10Avatar\"> <div class=\"ph-info svelte-da3zva\">"
            "<div class=\"ph-name-row svelte-da3zva\"><span class=\"ph-name svelte-da3zva\" id=\"knV10Name\"> </span></div> "
            "<div class=\"ph-level-row svelte-da3zva\"><span class=\"ph-level-badge svelte-da3zva\" id=\"knV10Level\"> </span> "
            "<div class=\"ph-xp-bar svelte-da3zva\"><div class=\"ph-xp-fill svelte-da3zva\" id=\"knV10XpFill\"></div></div></div></div></div> "
            "<div class=\"verticalSeparator svelte-da3zva\" style=\"height:35px;\"></div> "
            "<div class=\"ph-item svelte-da3zva\" onclick=\"windows[13].purchase(1)\" onmouseenter=\"playTick()\">"
            "<span class=\"ph-currency-icon svelte-da3zva\" style=\"color:#fbc02d\">KR</span> <span class=\"ph-value svelte-da3zva\" id=\"menuKRCount\"> </span></div> "
            "<div class=\"verticalSeparator svelte-da3zva\" style=\"height:35px;\"></div> "
            "<div class=\"ph-item svelte-da3zva\" onclick=\"windows[13].craftMenu()\" onmouseenter=\"playTick()\">"
            "<span class=\"material-icons ph-icon svelte-da3zva\" style=\"color:#cc51f8\">plumbing</span> <span class=\"ph-value svelte-da3zva\" id=\"menuJNKCount\"> </span></div> "
            "<div class=\"verticalSeparator svelte-da3zva\" style=\"height:35px;\"></div> "
            "<div class=\"ph-item svelte-da3zva\" onclick=\"openRankedMenu()\" onmouseenter=\"playTick()\">"
            "<span class=\"material-icons ph-icon svelte-da3zva\" style=\"color:#4caf50;margin-top:-4px\">emoji_events</span> <span class=\"ph-value svelte-da3zva\" id=\"menuRPCount\"> </span></div> "
            "<div class=\"verticalSeparator svelte-da3zva\" style=\"height:35px;\"></div> "
            // Wallet (_52516: the Wallet and Inventory modal, v10_wallet.c)
            "<div class=\"ph-item svelte-da3zva\" id=\"knV10WalletBtn\" title=\"Wallet\" onclick=\"knV10Wallet()\" onmouseenter=\"playTick()\">"
            "<span class=\"material-icons ph-icon svelte-da3zva\">backpack</span></div></div>");
        in = by_id("signedInHeaderBar");
        HtmlElem profile = by_id("knV10Profile");
        if (profile && !by_id("knV10AccountsSignedIn")) {
            char markup[512];
            account_manager_markup(markup, sizeof(markup), "knV10AccountsSignedIn");
            html_dom_insert_html(profile, HTML_INSERT_AFTER_END, markup);
        }
    }
    set_display(out, acc ? "none" : "");
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
    if (!acc) return;

    HtmlElem el;
    // premiumT && settings.tmp.premiumBadge ? alias : name
    if ((el = by_id("knV10Name"))) html_dom_set_text(el, player_names_account(acc->premium && !ui_setting_is_false("premiumBadge") && acc->alias[0] ? acc->alias : acc->name));
    // the picture (v10:209536 _70104, run at sign-in): the menu class's icon, textures/classes/icon_N.png
    // (defaultSrc). KN has no profile.png upload, so a premium account's picture is the class icon too.
    // Classes past Krunker's icons (KN's own from KN_CLASS_BASE: the Nettspend) keep the stock picture.
    if ((el = by_id("knV10Avatar"))) {
        char src[64];
        const int cls = menu_ui()->client->menu_class_index;
        if (cls >= 0 && cls < KN_CLASS_BASE) snprintf(src, sizeof(src), "textures/classes/icon_%d.png", cls);
        else snprintf(src, sizeof(src), "./img/player_pfp.png");
        html_dom_set_attr(el, "src", src);
    }
    char text[48];
    snprintf(text, sizeof(text), "Lv %d", acc->level > 0 ? acc->level : 1);
    if ((el = by_id("knV10Level"))) html_dom_set_text(el, text);
    snprintf(text, sizeof(text), "width:%.6g%%", acc->level_prog);
    if ((el = by_id("knV10XpFill"))) html_dom_set_attr(el, "style", text);
    comma(text, sizeof(text), acc->funds);
    if ((el = by_id("menuKRCount"))) html_dom_set_text(el, text);
    snprintf(text, sizeof(text), "%.2f", acc->junk); // _8988(junk.toFixed(2)): Box Office accounts have no junk (0.00)
    if ((el = by_id("menuJNKCount"))) html_dom_set_text(el, text);
    comma(text, sizeof(text), acc->ranked_points); // _8986(rankedPoints): the krunker.io account's; Box Office's in the Ranked window
    if ((el = by_id("menuRPCount"))) html_dom_set_text(el, text);
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

// v10 runs fetchIsPlayerInRankedMatch() once per sign-in, when the home loads (v10:209847); this
// is set by a sign-in and sent by v10_menu_tick once the brain can answer (ranked_ui_home_ready)
static int g_rejoin_check;

// --render-scenario state.officialSession: the menu as in a krunker.io server without one (its
// account is the active one then: krunker.io's, backend_ui.h)
void v10_menu_scenario_official(const int on) {
    account_ui_scenario_official(on);
    v10_menu_account_changed();
}
int v10_menu_official_session(void) { return account_backend() == ACCOUNT_BACKEND_KRUNKER; }

void v10_menu_account_changed(void) {
    if (!v10_menu_page()) return;
    v10_market_account_changed(); // the open exchange's tab follows the account (a no-op when the account is the same)
    const AccountInfo *acc = active_account(); // krunker.io's in an official game, krunk.io's elsewhere
    if (acc && V.signed_in != 1) g_rejoin_check = 1;
    header_update(acc);
    nav_update(acc);
    // hideSpectateIfGuestUser (v10:208579-208581, after a login v10:209811 and a logout v10:209874):
    // the menu's Spectate switch is for accounts only (main.css hides #spectButtonHolder)
    set_display(by_id("spectButtonHolder"), acc ? "block" : "none");
    V.signed_in = acc != NULL;
    V.level = acc ? acc->level : 0;
    ranked_ui_account_changed(); // v10's $account effect on the ranked popup (v10:111598-111600)
    V.active_gen = account_active_generation();
    v10_login_account_changed(); // a login closes the modal (v10: $account set -> the modal leaves the stack)
    v10_wallet_account_changed(); // the Wallet and Inventory modal leaves the stack with its account
}

// ---------------------------------------------------------------- the idle state

// Popular Now (_52548): fetchMatches -> the entries with players > 0, sorted by players
// (descending, stable), the first 3; rendered as the first child of #subLogoButtons while there is
// one (none: no rail, what v10 shows for an empty fetchMatches). onMount fetches once, then every 60 s
// unless the rail is hidden (offsetParent === null).
// KrunkNative: fetchMatches counts Box Office's custom games (servers_ui_fetch_matches), on the Box Office Server
// Type only (menu_sources.h): on krunker.io and Both there is no rail. The first fetch waits for the brain probe
// (backend_available) instead of answering {} while it runs, which would leave the rail empty for the first minute.
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

// krunker.io is the active backend: an official krunker.io game is the session (the brain is gated)
static int krunker_active(void) { return account_backend() == ACCOUNT_BACKEND_KRUNKER; }

// Popular Now is Box Office's only (menu_sources.h): its game list is the brain's, so krunker.io's lobby list is
// never asked for it.
static void popular_interval(void *user) {
    (void) user;
    P.timer = html_timer_add(POPULAR_REFRESH_MS, popular_interval, NULL);
    HtmlElem rail = popular_rail();
    if (rail && !html_dom_is_shown(rail)) return; // offsetParent === null
    popular_fetch();
}

// ---------------------------------------------------------------- Live Streams (.streams-overlay)

// The Live Streams overlay (_52196 / _52549, v10:145187, 146035): the top right panel's card list.
// fetchTopStreams (_52138, v10:145037) asks the data client's getTopStreams (v10:25991: API_URI,
// https://api.krunker.io/streams/top?cb=2) once the menu mounts, then every 30 s unless the view is closed
// (v10:146178), whoever is signed in: the overlay's visibility reads no account, and it shows on every Server Type.
// KrunkNative's host (menu_sources.h menu_streams_source): the brain's /streams/top on Box Office while it answers,
// from Twitch's live streams of the Krunker category (streams.c) in v10's {streams, promo, dropCampaigns} shape;
// api.krunker.io's (krunker_kio_routes.h, v10's own) on a krunker.io Server Type, in an official game (the brain is
// gated), with a signed-in krunker.io account, or on Both without a brain. The small view (v10's default, _52546)
// shows the first three cards: a red live dot with the viewer count (formatViewerCount: "1.3K" for 1250), the title, the
// streamer's name, and a click that opens twitch.tv/<name> (openStream, v10:145133). An answer with no live
// streams and no promo hides the overlay (v10's visibility rule). No answer (Box Office with no brain: nobody to
// ask; the host unreachable) hides the overlay, as v10's error state does.
//
// The Live Streams panel: a port of v10.0.3's streams-overlay component (the Svelte component that reads
// /streams/top's {streams, promo, dropCampaigns}). Each rule below cites the component's code in the v10.0.3 bundle
// (/home/user/jails/kru/src/Krunker_v10.0.3/game.js), read through its own string table: the store, the derived
// lists, the template conditions and the handlers.
#define STREAMS_SHOWN 3           // the small view: three cards, or two and the promo (slice(0, 3) / slice(0, min(2, n)))
#define STREAMS_EXPANDED_MAX 12   // the expanded view: three rows of four (slice(0, 4 * 3 - promo))
#define STREAMS_COLS 4            // the expanded view's columns (800 px: four 180 px cards)
#define STREAMS_FULL_ROWS 3       // expanded rows from which the view is full: the rest folds away
#define STREAMS_KEEP 256          // answers kept (the brain sends at most 100: streams.c STREAMS_MAX_PER_PAGE)
#define STREAMS_REFRESH_MS 30000.0
#define STREAMS_NAME_MAX 25
#define STREAMS_TITLE_MAX 160
#define STREAMS_THUMB_MAX 320
#define STREAMS_LANG_MAX 16
#define STREAMS_FADE_MS 200       // v10's fade transitions: duration 0xc8, linear easing
#define STREAMS_STAGGER_MS 50     // v10's delay per card: 0x32 * index
#define STREAMS_REST_STAGGER_MAX 8 // the rest grid staggers its first eight cards only (index < 0x8)
#define STREAMS_VIEW_KEY "streams-overlay-view-mode"
#define STREAMS_PROMO_URL "https://twitch.tv/directory/category/krunker"
enum { STREAMS_SMALL, STREAMS_EXPANDED, STREAMS_CLOSED };
// v10 keys its each-blocks by index and its if-blocks by presence: an element that was already on screen does not fade
// in again. These are the blocks that fade: the featured grid's cards and promo, the plain grid's, the rest grid's, the
// overlay itself, and the promo of each grid.
enum { SB_FEAT, SB_PLAIN, SB_REST, SB_ROOT, SB_COUNT };
enum { PB_FEAT, PB_PLAIN, PB_ROW, PB_COUNT };

struct stream_entry {
    char user[64];
    char title[STREAMS_TITLE_MAX];
    char thumb[STREAMS_THUMB_MAX];
    char lang[STREAMS_LANG_MAX];  // languageCode as sent (the flag's class)
    int viewers;
    int featured;                 // featured (truthy in JS), kept first
    int dropped;                  // a drop campaign names this streamer (hasDropsAvailable)
    int has_tags;                 // eventTags?.length
    char *tags;                   // the event tag chips' markup (NULL without tags)
};
static struct {
    int listed;                   // 1 = the last answer was a list (the overlay may show), 0 = hidden
    int promo;                    // showPromo: the answer's promo === true
    int drops;                    // dropCampaigns.length > 0 (the header tag, has-drops)
    int nfeat;                    // streams the answer marks featured: they lead e[]
    int n;                        // streams kept: e[0..nfeat) featured, then the rest, by viewers
    struct stream_entry e[STREAMS_KEEP];
    int mode;                     // STREAMS_SMALL / STREAMS_EXPANDED / STREAMS_CLOSED
    int mode_loaded;              // the saved view mode is read once
    int prev_count[SB_COUNT];     // cards each block showed at the last render (0: the block was not there)
    int prev_promo[PB_COUNT];     // each grid's promo at the last render
    int timer;                    // the refresh interval (html timer id)
    int override;                 // tests: knV10Streams gave the list, the brain is not asked
} L;

// The card's picture: v10 sets its <img src> to the answer's thumbnailUrl as it is (v10.js:146254). The engine loads an
// http(s) image itself (remote_images.c: the media lane, a disk cache, drawn once it arrives), so the card asks nobody
// for it. Any other value, or one too long for the card's buffer, shows no picture: the card's grey box, as v10's
// while an image loads.
static int streams_picture_ok(const char *url) {
    if (strlen(url) >= sizeof L.e[0].thumb) return 0;
    return !strncasecmp(url, "https://", 8) || !strncasecmp(url, "http://", 7);
}

// Twitch's CDN refuses the native media lane on some networks, so a Twitch-hosted thumbnail goes
// through the brain's /streams/thumb (its allowlist is streams_thumb_url_ok's hosts). Any other host
// is loaded directly, and with no brain the original URL is kept, as v10's <img src>.
static void streams_thumb_url(const char *url, char *out, const size_t n) {
    out[0] = 0;
    if (!streams_picture_ok(url)) return;
    const char *scheme = strstr(url, "://");
    const char *host = scheme ? scheme + 3 : url;
    const char *slash = strchr(host, '/');
    const size_t hn = slash ? (size_t) (slash - host) : strlen(host);
    const int twitch = (hn >= 9 && !strncasecmp(host + hn - 9, "jtvnw.net", 9)) ||
                       (hn >= 10 && !strncasecmp(host + hn - 10, "twitch.tv", 10));
    if (!twitch || !backend_url()[0]) {
        snprintf(out, n, "%s", url);
        return;
    }
    char enc[3 * STREAMS_THUMB_MAX];
    backend_url_encode(enc, sizeof(enc), url);
    if (snprintf(out, n, "%s/streams/thumb?url=%s", backend_url(), enc) >= (int) n) out[0] = 0;
}

// a Twitch login: letters, digits and '_' (the card's link is twitch.tv/<name>)
static int streams_login_ok(const char *s) {
    const size_t n = strlen(s);
    if (!n || n > STREAMS_NAME_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        if (!isalnum((unsigned char) s[i]) && s[i] != '_') return 0;
    }
    return 1;
}

// JavaScript truthiness of an answer field (the bundle tests featured with !x and x[...] directly)
static int streams_truthy(const cJSON *v) {
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0 && v->valuedouble == v->valuedouble;
    if (cJSON_IsString(v)) return v->valuestring && v->valuestring[0];
    return 1;
}

// formatViewerCount (v10.0.3, streams-overlay): from 1000 on, (v / 1000).toFixed(1) + 'K'; below, the number. toFixed
// rounds the double's exact value to the nearest tenth and a tie up, so the digit after the tenth decides: 5 or more
// rounds up (printf would round exact ties to even, 1250 -> "1.2K" where the page shows "1.3K").
static void streams_viewers(char *out, const size_t n, const int v) {
    if (v < 1000) {
        snprintf(out, n, "%d", v);
        return;
    }
    char exact[128];
    snprintf(exact, sizeof exact, "%.80f", v / 1000.0);
    const char *p = exact;
    long long whole = 0;
    for (; *p && *p != '.'; p++) whole = whole * 10 + (*p - '0');
    int tenth = 0, hundredth = 0;
    if (*p == '.') {
        tenth = p[1] ? p[1] - '0' : 0;
        hundredth = (p[1] && p[2]) ? p[2] - '0' : 0;
    }
    long long tenths = whole * 10 + tenth + (hundredth >= 5);
    snprintf(out, n, "%lld.%lldK", tenths / 10, tenths % 10);
}

// hasDropsAvailable (the bundle's component method): some campaign lists the streamer's login in its streamerNames
// (case-insensitive; the logins are ASCII, see streams_login_ok)
static int streams_drop_for(const cJSON *campaigns, const char *user) {
    const cJSON *c = NULL;
    cJSON_ArrayForEach(c, campaigns) {
        const cJSON *names = cJSON_GetObjectItemCaseSensitive(c, "streamerNames");
        const cJSON *n = NULL;
        cJSON_ArrayForEach(n, names) {
            if (cJSON_IsString(n) && !strcasecmp(n->valuestring, user)) return 1;
        }
    }
    return 0;
}

// the event tags' chips: one per tag, in the answer's order (the bundle's each over eventTags)
static char *streams_tags_html(const cJSON *tags) {
    StrBuf sb = {0};
    const cJSON *t = NULL;
    cJSON_ArrayForEach(t, tags) {
        if (!cJSON_IsString(t)) continue;
        sb_append(&sb, "<span class=\"event-tag-chip svelte-60shhe\">");
        sb_append_esc(&sb, t->valuestring);
        sb_append(&sb, "</span>");
    }
    return sb_finish(&sb);
}

static void streams_clear(void) {
    for (int i = 0; i < L.n; i++) {
        free(L.e[i].tags);
        L.e[i].tags = NULL;
    }
    L.n = L.nfeat = 0;
}

// The view mode (the store, streams-overlay-view-mode): read once, saved on every change as v10 does (expanded is
// saved as small, so the expanded view is not kept across launches).
static void streams_mode_load(void) {
    if (L.mode_loaded) return;
    L.mode_loaded = 1;
    char buf[32] = "";
    L.mode = STREAMS_SMALL;
    if (html_storage_get(STREAMS_VIEW_KEY, buf, sizeof buf) >= 0) {
        if (!strcmp(buf, "closed")) L.mode = STREAMS_CLOSED;
        else if (!strcmp(buf, "expanded")) L.mode = STREAMS_EXPANDED;
    }
}

static void streams_mode_set(const int mode) {
    L.mode = mode;
    html_storage_set(STREAMS_VIEW_KEY, mode == STREAMS_CLOSED ? "closed" : "small");
}

// The layout: every rule is v10.0.3's derived store or template condition (streams-overlay, decoded). D = the displayed
// streams (the featured ones when there are any, else the rest), R = the rest behind the featured ones.
struct streams_layout {
    int nd, rest, promo, prom_grid, prom_row, shown, cols, cols_width, rows, full, body, feat, rest_on, max_h;
};
static struct streams_layout streams_layout(void) {
    struct streams_layout y = {0};
    const int exp = L.mode == STREAMS_EXPANDED, small = L.mode == STREAMS_SMALL, closed = L.mode == STREAMS_CLOSED;
    const int nd = L.nfeat ? L.nfeat : L.n;
    const int P = L.listed && L.promo;
    // promo in the grid: small always, expanded where it fills the row (three cards, or more not a multiple of four)
    const int prom_vis = P && (small || (exp && (nd == 3 || (nd > 3 && nd % STREAMS_COLS != 0))));
    const int count = nd + prom_vis;
    const int rows = exp ? (count + STREAMS_COLS - 1) / STREAMS_COLS : 1;
    const int full = exp && rows >= STREAMS_FULL_ROWS;
    y.nd = nd;
    y.rest = L.n - L.nfeat;
    y.promo = prom_vis;
    y.shown = exp ? (nd < STREAMS_EXPANDED_MAX - prom_vis ? nd : STREAMS_EXPANDED_MAX - prom_vis)
                  : (small && P ? (nd < 2 ? nd : 2) : (nd < STREAMS_SHOWN ? nd : STREAMS_SHOWN));
    y.prom_grid = prom_vis && (!full || (exp && nd < STREAMS_EXPANDED_MAX));
    const int grid_cards = y.shown + prom_vis;
    y.cols = exp ? STREAMS_COLS : (grid_cards < STREAMS_SHOWN ? grid_cards : STREAMS_SHOWN);
    // the overlay's inline width (small only): the width formula reads the promo only below three streams
    const int width_cards = nd + (P && nd < STREAMS_SHOWN ? 1 : 0);
    const int width_cols = exp ? STREAMS_COLS : (width_cards < STREAMS_SHOWN ? width_cards : STREAMS_SHOWN);
    y.cols_width = width_cols;
    y.rows = rows;
    y.full = full;
    y.body = !closed && (nd > 0 || P);
    y.feat = !closed && L.nfeat > 0;
    y.rest_on = exp && !full && L.nfeat > 0 && y.rest > 0;
    y.prom_row = exp && P && nd > 3 && nd % STREAMS_COLS == 0; // only inside the rest section (v10's row promo)
    y.max_h = exp && rows >= 2 && !full ? 204 : 408;
    return y;
}

// one stream card (v10's .stream-card: the thumbnail with the viewer count, the drop badge, the event tags, the language
// flag; then the title and the streamer's name). anim = fade in with a delay (ms), else it is already on screen.
static void streams_card(StrBuf *sb, const int i, const int anim, const int delay) {
    const struct stream_entry *s = &L.e[i];
    char count[24];
    streams_viewers(count, sizeof count, s->viewers);
    sb_appendf(sb, "<div class=\"stream-card%s svelte-60shhe\" style=\"", s->has_tags ? " has-event" : "");
    if (anim) sb_appendf(sb, "animation:kn-stream-fade %dms linear %dms both;", STREAMS_FADE_MS, delay);
    sb_append(sb, "pointer-events:all;cursor:pointer;\" onclick=\"knV10Stream(");
    sb_appendf(sb, "%d)\"><div class=\"thumbnail svelte-60shhe\">", i);
    sb_appendf(sb, "<img loading=\"lazy\" decoding=\"async\" class=\"svelte-60shhe\" id=\"knStreamThumb%d\" src=\"", i);
    sb_append_esc(sb, s->thumb); // streams_thumb_url: the answer's thumbnailUrl, or the brain's thumb proxy for Twitch
    sb_append(sb, "\" alt=\"");
    sb_append_esc(sb, s->title);
    sb_append(sb, "\"> <div class=\"viewer-count svelte-60shhe\"><span class=\"dot svelte-60shhe\"></span> <span class=\"svelte-60shhe\">");
    sb_append_esc(sb, count);
    sb_append(sb, " viewers</span></div>");
    if (s->dropped) {
        sb_append(sb, "<div class=\"drop-badge svelte-60shhe\"><span class=\"drop-icon svelte-60shhe\" role=\"img\" aria-label=\"🎁\" style=\"display:inline-block;width:17.44px;height:14px;background:url(img/gift-emoji.png) no-repeat center / contain;\"></span> <span class=\"drop-text svelte-60shhe\">DROPS</span></div>");
    }
    if (s->has_tags) {
        // under a drop badge the chips sit 36 px down (v10's .thumbnail:has(.drop-badge) .event-tags-badge)
        sb_append(sb, s->dropped ? "<div class=\"event-tags-badge svelte-60shhe\" style=\"top:36px;\">"
                                 : "<div class=\"event-tags-badge svelte-60shhe\">");
        if (s->tags) sb_append(sb, s->tags);
        sb_append(sb, "</div>");
    }
    // the flag shows for any language but English (v10's languageCode !== 'en' test)
    if (s->lang[0] && strcasecmp(s->lang, "en") != 0) {
        sb_append(sb, "<div class=\"language-flag svelte-60shhe\"><div class=\"flag-icon flag-icon-");
        sb_append_esc(sb, s->lang);
        sb_append(sb, " svelte-60shhe\"></div></div>");
    }
    sb_append(sb, "</div><div class=\"stream-info svelte-60shhe\"><h3 class=\"stream-title svelte-60shhe\">");
    sb_append_esc(sb, s->title);
    sb_append(sb, "</h3><p class=\"stream-username svelte-60shhe\">");
    sb_append_esc(sb, s->user);
    sb_append(sb, "</p></div></div>");
}

// v10's promo card (.promo-card): the grey Krunker map, the live icon, the call to stream; a click opens Krunker's
// Twitch category (openTwitch). The map is the page's img/maps/map_35.png (byte-identical to v10's).
static void streams_promo(StrBuf *sb, const int anim, const int delay) {
    sb_append(sb, "<div class=\"stream-card promo-card svelte-60shhe\" style=\"");
    if (anim) sb_appendf(sb, "animation:kn-stream-fade %dms linear %dms both;", STREAMS_FADE_MS, delay);
    sb_append(sb, "pointer-events:all;cursor:pointer;\" onclick=\"knV10StreamsPromo()\"><div class=\"thumbnail svelte-60shhe\">"
                  "<img src=\"img/maps/map_35.png\" alt=\"Stream Krunker\" class=\"grayscale svelte-60shhe\" loading=\"lazy\" decoding=\"async\">"
                  " <div class=\"live-icon-overlay svelte-60shhe\"><img src=\"img/live-icon.png\" alt=\"Live Icon\" loading=\"eager\" "
                  "decoding=\"async\" class=\"svelte-60shhe\"></div></div>"
                  "<div class=\"stream-info svelte-60shhe\"><h3 class=\"stream-title promo-title svelte-60shhe\">Stream Krunker and get featured!</h3></div></div>");
}

// the header's right side (v10's button-group): Show while closed; else the More / Less toggle (when expanded, or more
// than three streams, or rest streams in the small view) and Hide
static void streams_buttons(StrBuf *sb, const struct streams_layout *y) {
    sb_append(sb, "<div class=\"button-group svelte-60shhe\" id=\"knStreamsButtons\">");
    if (L.mode == STREAMS_CLOSED) {
        sb_append(sb, "<button class=\"toggle-button svelte-60shhe\" id=\"knStreamsShow\" onclick=\"knV10StreamsToggle()\">Show</button>");
    } else {
        const int more = y->nd > STREAMS_SHOWN || (y->rest > 0 && L.mode == STREAMS_SMALL);
        if (L.mode == STREAMS_EXPANDED || more) {
            sb_appendf(sb, "<button class=\"toggle-button svelte-60shhe\" id=\"knStreamsMode\" onclick=\"knV10StreamsMode()\">%s</button>",
                       L.mode == STREAMS_EXPANDED ? "Less" : "More");
        }
        sb_append(sb, "<button class=\"hide-button svelte-60shhe\" id=\"knStreamsHide\" onclick=\"knV10StreamsToggle()\">Hide</button>");
    }
    sb_append(sb, "</div>");
}

// Replaces the element `id` with `html` inserted at the end of `parent` (a fresh copy each render).
static void streams_replace(const char *parent, const char *id, const char *html) {
    HtmlElem old = by_id(id);
    if (old) html_dom_remove(old);
    HtmlElem p = by_id(parent);
    if (p && html) html_dom_insert_html(p, HTML_INSERT_BEFORE_END, html);
}

// The overlay is the page's own (ui/index.html #knStreams: v10's header with the Hide button). Its body and header parts
// are rebuilt at every answer and every toggle; the overlay is shown while there is a stream or the promo, else hidden.
static void streams_render(void) {
    streams_mode_load();
    HtmlElem ov = by_id("knStreams");
    if (!ov) return;
    const struct streams_layout y = streams_layout();
    const int visible = L.listed && (y.nd > 0 || (L.promo));
    int prev_count[SB_COUNT], prev_promo[PB_COUNT];
    memcpy(prev_count, L.prev_count, sizeof prev_count);
    memcpy(prev_promo, L.prev_promo, sizeof prev_promo);
    const int root_fade = visible && !L.prev_count[SB_ROOT];
    memset(L.prev_count, 0, sizeof L.prev_count);
    memset(L.prev_promo, 0, sizeof L.prev_promo);
    if (!visible) {
        set_display(ov, "none");
        return;
    }
    set_display(ov, "");
    L.prev_count[SB_ROOT] = 1;
    html_dom_set_class(ov, "closed", L.mode == STREAMS_CLOSED);
    html_dom_set_class(ov, "small", L.mode == STREAMS_SMALL);
    html_dom_set_class(ov, "expanded", L.mode == STREAMS_EXPANDED);
    html_dom_set_class(ov, "has-drops", L.drops);
    if (L.mode == STREAMS_SMALL) {
        char px[32];
        snprintf(px, sizeof px, "%dpx", y.body ? 180 * y.cols_width + 16 * (y.cols_width - 1) + 32 : 0);
        html_dom_set_style(ov, "width", px);
    } else {
        html_dom_set_style(ov, "width", "");
    }
    html_dom_set_style(ov, "animation", root_fade ? "kn-stream-fade 200ms linear 0ms both" : "");

    StrBuf sb = {0};
    if (y.body) {
        sb_append(&sb, "<div class=\"streams-body svelte-60shhe\" id=\"knStreamsBody\">");
        if (y.feat) {
            sb_append(&sb, "<div class=\"featured-section svelte-60shhe\"><div class=\"featured-label svelte-60shhe\">Featured</div>");
            sb_appendf(&sb, "<div class=\"streams-grid featured-grid svelte-60shhe\" style=\"grid-template-columns: repeat(%d, 180px);\">", y.cols);
            for (int i = 0; i < y.shown; i++) {
                const int anim = i >= prev_count[SB_FEAT];
                streams_card(&sb, i, anim, STREAMS_STAGGER_MS * i);
                L.prev_count[SB_FEAT] = i + 1;
            }
            if (y.prom_grid) {
                const int anim = !prev_promo[PB_FEAT];
                streams_promo(&sb, anim, STREAMS_STAGGER_MS * y.shown);
                L.prev_promo[PB_FEAT] = 1;
            }
            sb_append(&sb, "</div></div>");
        } else {
            sb_appendf(&sb, "<div class=\"streams-grid regular-grid svelte-60shhe\" style=\"grid-template-columns: repeat(%d, 180px);\">", y.cols);
            for (int i = 0; i < y.shown; i++) {
                const int anim = i >= prev_count[SB_PLAIN];
                streams_card(&sb, i, anim, STREAMS_STAGGER_MS * i);
                L.prev_count[SB_PLAIN] = i + 1;
            }
            if (y.prom_grid) {
                const int anim = !prev_promo[PB_PLAIN];
                streams_promo(&sb, anim, STREAMS_STAGGER_MS * y.shown);
                L.prev_promo[PB_PLAIN] = 1;
            }
            sb_append(&sb, "</div>");
        }
        if (y.rest_on) {
            sb_appendf(&sb, "<div class=\"streams-grid regular-grid svelte-60shhe\" style=\"grid-template-columns: repeat(%d, 180px); max-height: %dpx;\">",
                       STREAMS_COLS, y.max_h);
            for (int j = 0; j < y.rest; j++) {
                const int anim = j >= prev_count[SB_REST];
                const int delay = j < STREAMS_REST_STAGGER_MAX ? STREAMS_STAGGER_MS * j : 0;
                streams_card(&sb, L.nfeat + j, anim, delay);
                L.prev_count[SB_REST] = j + 1;
            }
            sb_append(&sb, "</div>");
            if (y.prom_row) {
                const int anim = !prev_promo[PB_ROW];
                streams_promo(&sb, anim, 0);
                L.prev_promo[PB_ROW] = 1;
            }
        }
        sb_append(&sb, "</div>");
    }
    char *html = sb_finish(&sb);
    HtmlElem old = by_id("knStreamsBody");
    if (old) html_dom_remove(old);
    if (html) html_dom_insert_html(ov, HTML_INSERT_BEFORE_END, html);
    free(html);

    // the header: the drops tag beside the title (dropCampaigns.length > 0), the buttons at the right
    StrBuf hb = {0};
    if (L.drops) {
        sb_append(&hb, "<div class=\"drops-available-tag svelte-60shhe\" id=\"knStreamsDrops\"><span class=\"drops-icon svelte-60shhe\" role=\"img\" aria-label=\"🎁\" style=\"display:inline-block;width:14.95px;height:12px;background:url(img/gift-emoji-12.png) no-repeat center / contain;\"></span> "
                       "<span class=\"drops-text svelte-60shhe\">DROPS AVAILABLE</span></div>");
    }
    char *drops = sb_finish(&hb);
    streams_replace("knStreamsTitle", "knStreamsDrops", drops);
    free(drops);
    StrBuf bb = {0};
    streams_buttons(&bb, &y);
    char *buttons = sb_finish(&bb);
    streams_replace("knStreamsHeader", "knStreamsButtons", buttons);
    free(buttons);
}

// an answer: the whole /streams/top data object (NULL = no answer: hidden). The streams keep the featured ones first, in
// the answer's order; the rest follow, by viewers, highest first (the bundle's sort: b.viewerCount - a.viewerCount).
static void streams_apply(const cJSON *data) {
    streams_mode_load();
    streams_clear();
    const cJSON *streams = cJSON_GetObjectItemCaseSensitive(data, "streams");
    const cJSON *campaigns = cJSON_GetObjectItemCaseSensitive(data, "dropCampaigns");
    L.listed = cJSON_IsArray(streams);
    L.promo = L.listed && cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(data, "promo"));
    L.drops = L.listed && cJSON_IsArray(campaigns) && cJSON_GetArraySize(campaigns) > 0;
    const cJSON *s = NULL;
    for (int pass = 1; L.listed && pass >= 0; pass--) {
        cJSON_ArrayForEach(s, streams) {
            if (streams_truthy(cJSON_GetObjectItemCaseSensitive(s, "featured")) != pass) continue;
            const cJSON *u = cJSON_GetObjectItemCaseSensitive(s, "username");
            if (!cJSON_IsString(u) || !streams_login_ok(u->valuestring)) continue;
            if (L.n >= STREAMS_KEEP) continue;
            struct stream_entry *e = &L.e[L.n];
            memset(e, 0, sizeof *e);
            const cJSON *t = cJSON_GetObjectItemCaseSensitive(s, "title");
            const cJSON *v = cJSON_GetObjectItemCaseSensitive(s, "viewerCount");
            const cJSON *th = cJSON_GetObjectItemCaseSensitive(s, "thumbnailUrl");
            const cJSON *lg = cJSON_GetObjectItemCaseSensitive(s, "languageCode");
            const cJSON *tags = cJSON_GetObjectItemCaseSensitive(s, "eventTags");
            snprintf(e->user, sizeof e->user, "%s", u->valuestring);
            snprintf(e->title, sizeof e->title, "%s", cJSON_IsString(t) ? t->valuestring : "");
            streams_thumb_url(cJSON_IsString(th) ? th->valuestring : "", e->thumb, sizeof e->thumb);
            if (cJSON_IsString(lg)) snprintf(e->lang, sizeof e->lang, "%s", lg->valuestring);
            e->viewers = cJSON_IsNumber(v) && v->valuedouble > 0 ? (int) v->valuedouble : 0;
            e->featured = pass;
            e->dropped = streams_drop_for(campaigns, e->user);
            e->has_tags = cJSON_IsArray(tags) && cJSON_GetArraySize(tags) > 0;
            e->tags = e->has_tags ? streams_tags_html(tags) : NULL;
            L.n++;
            if (pass) L.nfeat++;
        }
    }
    // the regular streams by viewers, highest first, stable (insertion: the list is short and keeps the answer's order)
    for (int i = L.nfeat + 1; i < L.n; i++) {
        struct stream_entry tmp = L.e[i];
        int j = i - 1;
        while (j >= L.nfeat && L.e[j].viewers < tmp.viewers) {
            L.e[j + 1] = L.e[j];
            j--;
        }
        L.e[j + 1] = tmp;
    }
    if (v10_menu_page()) streams_render();
}

static void streams_body(void *user, const int status, const char *body, size_t length) {
    (void) user;
    (void) length;
    cJSON *res = status == 200 && body ? cJSON_Parse(body) : NULL;
    streams_apply(cJSON_GetObjectItemCaseSensitive(res, "data"));
    cJSON_Delete(res);
}

// krunker.io's answer ({data: {streams}} parsed by krunker_ranked_request: the data client's envelope)
static void streams_ranked_body(void *user, const int status, const cJSON *body) {
    (void) user;
    streams_apply(status == 200 ? cJSON_GetObjectItemCaseSensitive(body, "data") : NULL);
}

static void streams_fetch(void) {
    if (L.override) return;
    int sent = 0;
    // A krunker.io account signed in on a Box Office server is nobody's here (account_split.h): it chooses no host and
    // supplies no Bearer. Only an official krunker.io game makes its account the one a krunker.io request carries.
    const int kio_identity = account_split_krunker_identity();
    const unsigned source = menu_streams_source(seek_prefs_servers(), krunker_active(), kio_identity, backend_available());
    if (source == MENU_SRC_KIO) {
        // krunker.io: v10 getTopStreams (v10:25991) is the data client's, api.krunker.io (not gapi: 404 there). Its
        // API client's interceptor sends the signed-in account's Bearer with every request (v10:27595 / krunker_account.c);
        // a guest's goes without (a public read): krunker_ranked_request carries the token, the guest's read is plain
        sent = kio_identity && krunker_ranked_request(KRUNKER_RANKED_API, "GET", "/streams/top?cb=2", NULL, streams_ranked_body, NULL);
        char base[256], url[320];
        krunker_host_url(KRUNKER_HOST_API, "", base, sizeof(base));
        if (!sent) sent = krunker_kio_streams_url(url, sizeof(url), base) != 0 && backend_http_get_remote(url, KRUNKER_KIO_READ_TIMEOUT_MS, streams_body, NULL);
    } else if (source == MENU_SRC_KN) {
        sent = backend_http_get("/streams/top?cb=2", streams_body, NULL); // Box Office: the brain's (streams.c)
    }
    if (!sent) streams_apply(NULL);
}

static void streams_interval(void *user) {
    (void) user;
    L.timer = html_timer_add(STREAMS_REFRESH_MS, streams_interval, NULL);
    streams_mode_load();
    if (L.mode != STREAMS_CLOSED) streams_fetch(); // v10: setInterval(... "closed" !== mode && fetchTopStreams(), 30 s)
}

// The Server Type changed (Settings > Quickplay, ui_settings.c store_setting): Popular Now and the Live Streams
// follow it at once (menu_sources.h) rather than at their next interval: the rail shows on Box Office only, and the
// overlay asks the new Server Type's host.
void v10_menu_sources_changed(void) {
    if (!P.first_done || !v10_menu_page()) return;
    popular_fetch();
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
    // #doubleXPAd: the mode's doubleXP flag (v10:219357; the account's offer is not a buff)
    const int xp = game && game->map && game->mode && game->mode->config.double_xp;
    const int class_xp = game && game->map && double_class_xp(now), raid = game && game->map && double_raid_drops(now);
    set_display(by_id("doubleXPAd"), xp ? "block" : "none");
    set_display(by_id("doubleClassXPAd"), class_xp ? "block" : "none");
    set_display(by_id("doubleRaidDropsAd"), raid ? "block" : "none");
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
    if (g_rejoin_check && ranked_ui_home_ready()) g_rejoin_check = 0; // v10:209847: the rejoin check (ranked_ui.c)
    if (!P.first_done) { // onMount (see popular_render)
        if (backend_available() == -1 && !scenario_on()) return;
        P.first_done = 1;
        popular_fetch();
        P.timer = html_timer_add(POPULAR_REFRESH_MS, popular_interval, NULL);
        streams_fetch(); // fetchTopStreams on mount (_52138, v10:146178), then every 30 s
        L.timer = html_timer_add(STREAMS_REFRESH_MS, streams_interval, NULL);
        store_item_interval(NULL);
    }
    buffs_update(menu_ui()->client, 0);
}

// Per frame (backend_frame): the active account changed (an official game joined or left, either
// backend's sign-in / out): the header follows at once.
void v10_menu_account_tick(void) {
    if (!V.wired || !v10_menu_page()) return;
    v10_login_tick();
    v10_wallet_tick(); // the Wallet and Inventory modal follows its account (funds, skins)
    if (account_active_generation() == V.active_gen) return;
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
    wire(by_id("inviteButton"), "playSelect(),copyInviteLink()");
    wire(by_id("menuBtnJoin"), "playSelect(),openJoinWindow()");
}

static void nav_setup(void) {
    // the Guide card, What's New, Community & Events: v10_news.c; Turf Wars / Leaderboards:
    // v10_competitive.c (the signed-in Guide entry below calls the same openGuide())
    HtmlElem guide_card = html_dom_query(0, "#menuItemContainer .guideItem");
    // the signed-in Guide entry (a plain menuItem, v10:146966), placed where Svelte renders it
    if (guide_card && !by_id("knV10GuideItem"))
        html_dom_insert_html(guide_card, HTML_INSERT_AFTER_END,
            "<div class=\"menuItem svelte-fgmdj8\" id=\"knV10GuideItem\" style=\"display:none\" onclick=\"playSelect(),openGuide()\" "
            "onmouseenter=\"playTick()\"><span class=\"material-icons-outlined menuItemIcon svelte-fgmdj8\">school</span> "
            "<div class=\"menuItemTitle svelte-fgmdj8\">Guide</div></div>");
    wire_nav("menuBtnMarket", "playSelect(),openExchange()");
    wire_nav("menuBtnShop", "playSelect(),showWindow(14)");
    wire_nav("menuBtnChall", "playSelect(),windows[32].openHome()");
    wire_nav("menuBtnSocial", "playSelect(),openSocial()");
    // Exit: the web build has none (Krunker has none on the web). Natively its display is the Client settings'
    // exitButton (ui_client_settings.c apply_exit_button); v10's showExit gate is deob-10.0.3.js:262620
#ifndef __EMSCRIPTEN__
    HtmlElem holder = by_id("menuItemContainer");
    if (holder && !by_id("clientExit"))
        html_dom_insert_html(holder, HTML_INSERT_BEFORE_END,
            "<div class=\"menuItem svelte-fgmdj8\" id=\"clientExit\" onclick=\"playSelect(),clientExitPopup()\" onmouseenter=\"playTick()\">"
            "<span class=\"material-icons-outlined menuItemIcon svelte-fgmdj8\">exit_to_app</span> "
            "<div class=\"menuItemTitle svelte-fgmdj8\" id=\"menuBtnExit\">Exit</div></div>");
#endif
}

static void header_setup(void) {
    HtmlElem login = html_dom_query(0, "#signedOutHeaderBar .ph-login-wrap");
    if (login) html_dom_set_attr(login, "id", "knV10LoginBtn");
    wire(login, "loginOrRegister()");
    // KrunkNative's Account Manager (alt_manager_ui.c) sits between the guest's Login or Register and
    // Get Signup Rewards, with its neighbours' markup (v10 has no such button): a separator, then the item
    if (login && !by_id("knV10AccountsBtn")) {
        char markup[512];
        account_manager_markup(markup, sizeof(markup), "knV10AccountsBtn");
        html_dom_insert_html(login, HTML_INSERT_AFTER_END, markup);
    }
    wire(by_id("signupRewardsButton"), "showSignupRewards()");
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
    wire(html_dom_query(0, "#playerHeaderEl .webpush-container"), "knV10Notifications(0)");
    web_push_set_listener(notifications_redraw);
    notifications_redraw(); // the header as v10 mounts it (the bell only for a subscribed browser)
}

// v10's INIT (g:219154-219156): inviteButton's buttonD (greyed, no pointer events) in a ranked match. menuBtnHost gets
// buttonD from `ranked || official` (g:219155) and then from `official` alone (g:219156), so the final state is
// official only; a ranked match leaves the Host card as it is (KN follows the final state).
void v10_menu_match_gate(const int ranked, const int official) {
    HtmlElem invite = by_id("inviteButton"), host = by_id("menuBtnHost");
    if (invite) html_dom_set_class(invite, "buttonD", ranked);
    if (host) html_dom_set_class(host, "buttonD", official);
}

void v10_menu_dom_init(void) {
    if (!v10_menu_page()) return;
    // the action cards (Ranked's click: v10_competitive.c). Ranked's card carries its "2x KR"
    // ribbon (the component's children slot, _50464 v10:147331)
    HtmlElem ranked = by_id("menuBtnRanked");
    if (ranked && !html_dom_query(ranked, ".menuItemRankedLabel"))
        html_dom_insert_html(ranked, HTML_INSERT_BEFORE_END, " <div class=\"menuItemRankedLabel\">2x KR</div>");
    wire(by_id("menuBtnQuickMatch"), "playSelect(),backToOrigin()");
    wire(by_id("menuBtnHost"), "playSelect(),showWindow(42)");
    wire(by_id("menuBtnBrowser"), "playSelect(),showWindow(48)");
    wire(by_id("menuBtnCustomGames"), "playSelect(),showGames()");
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
// window (showWindow(25), download_ui.c). The Account Manager (KrunkNative's saved accounts) is
// the guest header's button, not an item here.
static void more_krunker(void) {
    const struct { const char *title, *symbol, *img, *desc, *action; } k_options[] = {
        {"Mods", "color_lens", NULL, "Customize your experience", "clearPops(),showWindow(4)"},
        {"Sandbox", "castle", NULL, "Try every skin", "clearPops(),knHostOffline()"},
        {"Editor", "construction", NULL, "Create your own maps and game modes", "clearPops(),openURL('/editor.html')"},
        {"KPD", NULL, "./img/police.png", "Stop criminals", "clearPops(),shoPolicePop()"},
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

// The Notifications panel: v10's webpush component (svelte-5kyah8, deob-10.0.5.js:146795-146806, its logic
// :148622-148860) in #playerHeaderEl's .webpush-container, over web_push.c's stores. The header shows the title
// and the details toggle (\u25b6 / \u25bc) unless the player is subscribed and the panel is collapsed (sub-not-expanded:
// the bell only). Expanded, .webpush-content shows, in v10's order: not supported (native builds: no Push API);
// else the controls: blocked (permission denied), Enable Notifications ("Subscribing..." while loading) with
// v10's description, or Notifications enabled. with Disable ("Unsubscribing..."); then the error, if any.
// A click on the header toggles; a click elsewhere on the collapsed container expands it (both stop there).
static void notifications_header(const HtmlElem box) {
    const int sub_collapsed = web_push_subscribed() && !V.notif_open; // isSubscribed && !expanded
    html_dom_set_class(box, "not-expanded", !V.notif_open);
    html_dom_set_class(box, "sub-not-expanded", sub_collapsed);
    HtmlElem header = html_dom_query(box, ".webpush-header");
    if (!header) return;
    StrBuf sb = {0};
    sb_append(&sb, "<span class=\"webpush-title svelte-5kyah8\"><span class=\"material-icons bell-icon svelte-5kyah8\">notifications</span> ");
    if (!sub_collapsed) sb_append(&sb, "Notifications");
    sb_append(&sb, "</span> ");
    if (!sub_collapsed)
        sb_appendf(&sb, "<button class=\"webpush-details-toggle svelte-5kyah8\">%s</button>", V.notif_open ? "\xE2\x96\xBC" : "\xE2\x96\xB6");
    char *html = sb_finish(&sb);
    html_dom_set_html(header, html);
    free(html);
    html_dom_set_attr(header, "onclick", "knV10Notifications(1)");
}

static void notifications_render(const HtmlElem content) {
    StrBuf sb = {0};
    if (!web_push_supported()) {
        sb_append(&sb, "<div class=\"webpush-message unsupported svelte-5kyah8\"><p class=\"svelte-5kyah8\">"
                       "Push notifications are not supported in your browser.</p></div>");
    } else {
        const int loading = web_push_loading();
        sb_append(&sb, "<div class=\"webpush-controls svelte-5kyah8\">");
        if (web_push_denied()) {
            sb_append(&sb, "<div class=\"webpush-message blocked svelte-5kyah8\"><p class=\"svelte-5kyah8\">"
                           "To enable notifications, go to browser settings.</p></div>");
        } else if (!web_push_subscribed()) { // supported && !subscribed && permission !== "denied"
            sb_appendf(&sb, "<button class=\"webpush-btn subscribe svelte-5kyah8\" onclick=\"knV10WebPush(1)\"%s>%s</button> "
                            "<p class=\"webpush-description svelte-5kyah8\">Get notified about game updates, events, and promotions!</p>",
                       loading ? " disabled=\"\"" : "", loading ? "Subscribing..." : "Enable Notifications");
        } else {
            sb_appendf(&sb, "<div class=\"webpush-subscribed svelte-5kyah8\"><p class=\"webpush-status svelte-5kyah8\">"
                            "<svg xmlns=\"http://www.w3.org/2000/svg\" class=\"checkmark-icon svelte-5kyah8\" viewBox=\"0 0 24 24\" fill=\"currentColor\">"
                            "<path d=\"M9 16.17L4.83 12l-1.42 1.41L9 19 21 7l-1.41-1.41z\" class=\"svelte-5kyah8\"></path></svg> Notifications enabled.</p> "
                            "<button class=\"webpush-btn unsubscribe svelte-5kyah8\" onclick=\"knV10WebPush(0)\"%s>%s</button></div>",
                       loading ? " disabled=\"\"" : "", loading ? "Unsubscribing..." : "Disable");
        }
        sb_append(&sb, "</div>");
    }
    const char *error = web_push_error();
    if (error) {
        sb_append(&sb, " <div class=\"webpush-error svelte-5kyah8\"><p class=\"svelte-5kyah8\">");
        sb_append_esc(&sb, error);
        sb_append(&sb, "</p></div>");
    }
    char *html = sb_finish(&sb);
    html_dom_set_html(content, html);
    free(html);
}

static HtmlElem notifications_content(const HtmlElem box) {
    HtmlElem content = html_dom_query(box, ".webpush-content");
    if (!content) {
        html_dom_insert_html(box, HTML_INSERT_BEFORE_END, "<div class=\"webpush-content svelte-5kyah8\"></div>");
        content = html_dom_query(box, ".webpush-content");
    }
    return content;
}

// the header, and the content while expanded (a store changed or the panel opened / closed)
static void notifications_redraw(void) {
    HtmlElem box = html_dom_query(0, "#playerHeaderEl .webpush-container");
    if (!box) return;
    notifications_header(box);
    HtmlElem content = notifications_content(box);
    if (V.notif_open) notifications_render(content);
    set_display(content, V.notif_open ? "" : "none");
}

// knV10Notifications(1): the header's click (expanded = !expanded); knV10Notifications(0): the container's
// (expands a collapsed panel only)
static void notifications_click(const int header) {
    html_ui_stop_propagation();
    if (!header && V.notif_open) return;
    V.notif_open = !V.notif_open;
    notifications_redraw();
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
        notifications_click(argc > 0 && args[0].kind == HTML_ARG_NUM && args[0].num != 0.0);
        return 1;
    }
    if (!strcmp(name, "knV10WebPush")) { // Enable Notifications (1) / Disable (0)
        html_ui_stop_propagation();
        if (argc > 0 && args[0].kind == HTML_ARG_NUM && args[0].num != 0.0) web_push_subscribe();
        else web_push_unsubscribe();
        return 1;
    }
    if (!strcmp(name, "knV10StreamsMode")) { // More / Less: v10's toggleViewMode (small <-> expanded)
        streams_mode_load();
        streams_mode_set(L.mode == STREAMS_SMALL ? STREAMS_EXPANDED : STREAMS_SMALL);
        streams_render();
        return 1;
    }
    if (!strcmp(name, "knV10StreamsToggle")) { // Hide / Show: v10's setViewMode('closed') / setViewMode('small')
        streams_mode_load();
        streams_mode_set(L.mode == STREAMS_CLOSED ? STREAMS_SMALL : STREAMS_CLOSED);
        streams_render();
        return 1;
    }
    if (!strcmp(name, "knV10StreamsPromo")) { // the promo card: openTwitch = window.open("https://twitch.tv/directory/category/krunker")
        ui_open_external(STREAMS_PROMO_URL);
        return 1;
    }
    if (!strcmp(name, "knV10Stream")) { // a Live Streams card: openStream(username) = window.open("https://twitch.tv/" + username)
        const int i = argc > 0 ? (int) args[0].num : -1;
        if (i < 0 || i >= L.n) return 1;
        char url[128];
        snprintf(url, sizeof url, "https://twitch.tv/%s", L.e[i].user); // openStream(username)
        ui_open_external(url);
        return 1;
    }
    if (!strcmp(name, "knV10Streams")) { // tests: the Live Streams list from a JSON {"streams":[...]} ("" = back to the brain's answer)
        const char *json = argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL;
        L.override = json && *json;
        if (L.override) {
            cJSON *res = cJSON_Parse(json);
            streams_apply(res);
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
    if (!strcmp(name, "knV10OfficialSession")) { // tests: the krunker.io account is the session (1) or Box Office's again (0): the Games tabs' source
        v10_menu_scenario_official(argc > 0 && args[0].num != 0.0);
        return 1;
    }
    return -1;
}
