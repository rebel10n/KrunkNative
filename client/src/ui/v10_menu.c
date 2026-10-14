// Krunker v10.0.2 main menu shell on the v10 page (ui/v10, doc/v10/ui.md 2).
//
// v10 mounts its menu as Svelte components (_52533 into #mainMenuUIMount, v10:222882): the page
// (ui/v10/index.html) keeps the markup those components render for a guest, without their
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
//   - no live-streams overlay (v10 hides it until Twitch answers with streams; KrunkNative has none)
// The login / register modal (loginOrRegister) is v10_login.c.
#include <v10_menu.h>
#include <backend.h>
#include <backend_ui.h>
#include <client_settings.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int  v10_login_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, int argc); // v10_login.c
void v10_login_account_changed(void);

static struct {
    int wired;            // the page's markup has its handlers (v10_menu_dom_init)
    int signed_in;        // the header / nav state last applied (-1 = none yet)
    int level;
    int notif_open;       // the Notifications toggle (webpush-container) is expanded
} V = {.signed_in = -1};

static HtmlElem by_id(const char *id) { return html_dom_by_id(id); }

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

// #signedInHeaderBar (_52526) for an account, #signedOutHeaderBar (_52519) for a guest. Both live
// in #playerHeaderEl; v10 swaps the component, here the guest bar stays in the page and the
// signed-in one is built next to it once.
static void header_update(const AccountInfo *acc) {
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
    if ((el = by_id("knV10Name"))) html_dom_set_text(el, acc->premium && !ui_setting_is_false("premiumBadge") && acc->alias[0] ? acc->alias : acc->name);
    char text[48];
    snprintf(text, sizeof(text), "Lv %d", acc->level > 0 ? acc->level : 1);
    if ((el = by_id("knV10Level"))) html_dom_set_text(el, text);
    snprintf(text, sizeof(text), "width:%.6g%%", acc->level_prog);
    if ((el = by_id("knV10XpFill"))) html_dom_set_attr(el, "style", text);
    comma(text, sizeof(text), acc->funds);
    if ((el = by_id("menuKRCount"))) html_dom_set_text(el, text);
    if ((el = by_id("menuJNKCount"))) html_dom_set_text(el, "0.00"); // _8988(junk.toFixed(2)): KrunkNative accounts have no junk
    if ((el = by_id("menuRPCount"))) html_dom_set_text(el, "0");     // _8986(rankedPoints): the ranked window shows the balance
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

void v10_menu_account_changed(void) {
    if (!v10_menu_page()) return;
    const AccountInfo *acc = account_get();
    header_update(acc);
    nav_update(acc);
    V.signed_in = acc != NULL;
    V.level = acc ? acc->level : 0;
    v10_login_account_changed(); // a login closes the modal (v10: $account set -> the modal leaves the stack)
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
    if ((el = by_id("mapVote"))) html_dom_append(segs[3], el);
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
    // .streams-overlay: only with streams to show (_52549); KrunkNative lists none
    set_display(html_dom_query(0, "#tlInfHold .streams-overlay"), "none");
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
// window (showWindow(25), download_ui.c).
static void more_krunker(void) {
    static const struct { const char *title, *symbol, *img, *desc, *action; } k_options[] = {
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
    return -1;
}
