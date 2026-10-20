// The in-game HUD on the Krunker v10.0.2 page (ui/, client/include/hud_v10.h).
//
// v10's in-game HUD is still the legacy DOM of v7.1.4 (#inGameUI and its ids, doc/v10/ui.md
// section 5), which hud_ui.c drives; this file holds what v10 does differently, read
// from the v10.0.2 client (~/.cache/kn-v10/v10.js, `v10:N` = its line) and checked against the
// offline v10 sandbox captures (doc/ref/krunker-v10/ingame/, tools/uidiff/capture-v10.js):
//   - #chatUIHolder (new in v10, markup display:none) wraps the chat: toggleMenuHider(on) runs
//     hideChatIfGuestUser(on) (v10:161017, 208576): on the menu a guest has no chat, an account
//     has it. KrunkNative keeps the chat up on the menu for a guest too (it went away on Esc), and
//     hides it only while the game loads (hud_ui.c hud_chat_wanted: the boot curtain, a join's
//     spinner, a map download);
//   - the Old Scoreboard holder #leaderboardHolder is always shown (switchLeaderboard writes
//     style.display = true, v10:207925, which CSS ignores; hud_ui.c shows it);
//   - the end screen's Performance tab is Combat Earnings + Combat Record (v10:203117) and a
//     guest gets the signup incentive under the results (v10:219663, origin match_end).
#include <hud_v10.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <backend_ui.h> // active_account
#include <v10_menu.h>   // v10_menu_page
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned g_page_serial = ~0u;
static int g_is_v10;
static char g_chat_shown[8]; // last #chatUIHolder display written ("" = none yet)

int hud_v10_page(void) {
    if (!html_ui_available() || !html_ui_is_ready()) return 0;
    const unsigned serial = html_ui_page_serial();
    if (serial != g_page_serial) {
        g_page_serial = serial;
        g_is_v10 = v10_menu_page(); // the same test as the v10 menu (v10_menu.c), cached per page load
        g_chat_shown[0] = 0;
    }
    return g_is_v10;
}

// The 2x XP badge under the timer (#dxpHudBadge, ui/index.html) while the match's mode has the doubleXP flag
// (v10's #doubleXPAd rule, v10:219357): checked each frame, changed only when the mode changes. A KrunkNative
// addition to v10's HUD, which has no such badge.
static int g_dxp_shown = -1;

static void double_xp_hud(void) {
    const MenuUi *menu = menu_ui();
    const Client *client = menu ? menu->client : NULL;
    const int on = client && client->game.map && client->game.mode && client->game.mode->config.double_xp;
    if (on == g_dxp_shown) return;
    g_dxp_shown = on;
    const HtmlElem badge = html_dom_by_id("dxpHudBadge");
    if (badge) html_dom_set_display(badge, on ? "block" : "none");
}

void hud_v10_tick(const int chat_wanted) {
    if (!hud_v10_page()) return;
    double_xp_hud();
    // UI.toggleMenuHider(on) -> hideChatIfGuestUser(on): on ? (account ? block : none) : block.
    // KrunkNative: block whenever the chat is wanted (not loading), on the menu for a guest too.
    const char *want = chat_wanted ? "block" : "none";
    if (!strcmp(want, g_chat_shown)) return;
    const HtmlElem el = html_dom_by_id("chatUIHolder");
    if (!el) return;
    html_dom_set_display(el, want);
    snprintf(g_chat_shown, sizeof(g_chat_shown), "%s", want);
}

static void ap(char *out, const size_t size, size_t *len, const char *fmt, ...) {
    if (*len >= size) return;
    va_list ap_;
    va_start(ap_, fmt);
    const int n = vsnprintf(out + *len, size - *len, fmt, ap_);
    va_end(ap_);
    if (n > 0) *len = *len + (size_t) n < size ? *len + (size_t) n : size - 1;
}

// tabs.performance.gen() (v10:203117), the part after the Battle Pass / Account Level sections:
// funds ("NONE" -> "+0 KR"), the gained score as EXP, Score / Kills / Deaths / Rank / KDR.
void hud_v10_end_performance_html(char *out, const size_t size, size_t *len, const char *kr_html, const int score,
                                  const int kills, const int deaths, const int rank) {
    const char *cell = "display: flex; align-items: center; justify-content: center; padding: 10px 15px;";
    const AccountInfo *acc = active_account();
    if (acc) {
        // Account Level (v10:203104): the level icons (img/levels/<roundToNearest(2) - 1>), the
        // bar at levelProg (the old part, then this match's score in #fff), "<xp> / <need>"
        const double k = 0.03; // _7340
        const int lvl = acc->level > 0 ? acc->level : 1;
        const double base = lvl > 1 ? (lvl / k) * (lvl / k) : 0.0;
        const double next = ((lvl + 1) / k) * ((lvl + 1) / k);
        const double prog = acc->level_prog;
        const double gained = next > base ? score / (next - base) * 100.0 : 0.0;
        const int over = prog > 100.0;
        int icon = (int) (lvl % 2 ? lvl + 1 : lvl) - 1; // Math.max(Math.min(101, roundToNearest(2) - 1), 0)
        icon = icon < 0 ? 0 : icon > 101 ? 101 : icon;
        double w0 = over ? (double) ((int) prog % 100) : prog - gained, w1 = over ? 0.0 : gained;
        if (w0 < 0) w0 = 0;
        // The row's wrapper is not #endBPLayout: that id is the Battle Pass grid (80% | 20% columns, the
        // battlepass_ui.c markup and v10's main.css), which would leave this row 80% wide on the card.
        ap(out, size, len, "<div class=\"endSection\" style=\"margin-top:10px\"><div class='endTitle'>Account Level</div>"
                           "<div id='endLvlLayout'><div id='lvlLayout'><img src='./img/levels/%d.png' class='xpRnk'><div class='xpLvl'>%d</div>"
                           "<div class='xpEndBar'><div class='xpEndBarB' style='width: %.2f%%;%s'></div>"
                           "<div class='xpEndBarB' style='width: %.2f%%;background-color: #fff;margin-left: -7px;'></div>"
                           "<span class='xpEndBarV blackShad'>%d / %d</span></div>"
                           "<img src='./img/levels/%d.png' class='xpRnk' style='margin-left: auto'><div class='xpLvl'>%d</div></div></div></div>",
           icon, lvl, w0, over ? "background-color:#fff" : "", w1, (int) (acc->score - base), (int) (next - base), icon, lvl + 1);
    }
    ap(out, size, len, "<div id='endLayout'><div class='endSection'><div class='endTitle'>Combat Earnings</div>"
                       "<div id='combatRecordLayout' style=\"grid-template-columns: auto;\">");
    ap(out, size, len, "<div class='crStat' style=\"%s\"><div class='crVal'>", cell);
    if (!kr_html || !kr_html[0] || !strcmp(kr_html, "NONE")) ap(out, size, len, "<span style=\"color:#F8C55C\">+0</span> KR");
    else ap(out, size, len, "%s", kr_html);
    ap(out, size, len, "</div></div><div class='crStat' style=\"%s\"><div class='crVal'><span style=\"color:#F8C55C\">+%d</span> EXP</div></div>",
       cell, score);
    ap(out, size, len, "<div class=\"otherEarnings\"></div><div id=\"raidPromoContainer\"></div></div></div>");
    ap(out, size, len, "<div class='endSection'><div class='endTitle'>Combat Record</div><div id='combatRecordLayout'>");
    ap(out, size, len, "<div class='crStat'><div class='crLabel'>Score</div><div class='crVal'>%d</div></div>", score);
    ap(out, size, len, "<div id='kdLayout'><div class='crStat'><div class='crLabel'>Kills</div><div class='crVal'>%d</div></div>", kills);
    ap(out, size, len, "<div class='crStat'><div class='crLabel'>Deaths</div><div class='crVal'>%d</div></div></div>", deaths);
    ap(out, size, len, "<div class='crStat'><div class='crLabel'>Rank</div><div class='crVal'>#%d</div></div>", rank);
    ap(out, size, len, "<div class='crStat'><div class='crLabel'>KDR</div><div class='crVal'>%.2f</div></div>",
       (double) kills / (double) (deaths ? deaths : 1));
    ap(out, size, len, "</div></div></div>");
}

// The signup rewards component (v10:115204, _39408) with origin "match_end" (v10_rewards.c: its
// markup, a guest's Unclaimed Rewards box, REGISTER -> knSignupRegister).
// KrunkNative: the guest banner flows under the results instead of v10's fixed `top: 850px`
// (#matchEndSignupIncentives, main.css), so it starts below the last table row. v10 centres the
// component on that point (.signup-rewards-container: absolute, top / left 50%, translate(-50%,
// -50%)); here the container is in flow and centred by `margin: 0 auto`. It moves into #endMidHolder
// after the results (the card renders rewrite only #endTabbedView).
void hud_v10_end_signup(const int show) {
    if (!hud_v10_page()) return;
    const HtmlElem el = html_dom_by_id("matchEndSignupIncentives");
    if (!el) return;
    const int on = show && !active_account(); // the component renders nothing for an $account
    if (on) v10_rewards_signup_mount(el);
    else html_dom_set_html(el, "");
    html_dom_set_display(el, on ? "block" : "none");
    if (!on) return;
    html_dom_set_style(el, "position", "relative");
    html_dom_set_style(el, "top", "auto");
    html_dom_set_style(el, "left", "auto");
    html_dom_set_style(el, "z-index", "auto");
    html_dom_set_style(el, "margin-top", "10px");
    const HtmlElem box = html_dom_query(el, ".signup-rewards-container");
    if (box) {
        html_dom_set_style(box, "position", "relative");
        html_dom_set_style(box, "top", "auto");
        html_dom_set_style(box, "left", "auto");
        html_dom_set_style(box, "transform", "none");
        html_dom_set_style(box, "margin", "0 auto");
    }
    const HtmlElem mid = html_dom_by_id("endMidHolder");
    if (mid) html_dom_append(mid, el);
}
