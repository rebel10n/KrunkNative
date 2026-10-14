// The in-game HUD on the Krunker v10.0.2 page (ui/, client/include/hud_v10.h).
//
// v10's in-game HUD is still the legacy DOM of v7.1.4 (#inGameUI and its ids, doc/v10/ui.md
// section 5), which hud_ui.c drives; this file holds what v10 does differently, read
// from the v10.0.2 client (~/.cache/kn-v10/v10.js, `v10:N` = its line) and checked against the
// offline v10 sandbox captures (doc/ref/krunker-v10/ingame/, tools/uidiff/capture-v10.js):
//   - #chatUIHolder (new in v10, markup display:none) wraps the chat: toggleMenuHider(on) runs
//     hideChatIfGuestUser(on) (v10:161017, 208576): on the menu a guest has no chat, an account
//     has it; out of the menu (the game, the death and end screens) it is always up;
//   - the Old Scoreboard holder #leaderboardHolder is always shown (switchLeaderboard writes
//     style.display = true, v10:207925, which CSS ignores; hud_ui.c shows it);
//   - the end screen's Performance tab is Combat Earnings + Combat Record (v10:203117) and a
//     guest gets the signup incentive under the results (v10:219663, origin match_end).
#include <hud_v10.h>
#include <html_dom.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <backend_ui.h> // account_get
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

void hud_v10_tick(void) {
    if (!hud_v10_page()) return;
    // UI.toggleMenuHider(on) -> hideChatIfGuestUser(on): on ? (account ? block : none) : block
    const MenuUi *menu = menu_ui();
    const char *want = menu && menu->menu_open && !account_get() ? "none" : "block";
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
    const AccountInfo *acc = account_get();
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
        ap(out, size, len, "<div class=\"endSection\" style=\"margin-top:10px\"><div class='endTitle'>Account Level</div>"
                           "<div id='endBPLayout'><div id='lvlLayout'><img src='./img/levels/%d.png' class='xpRnk'><div class='xpLvl'>%d</div>"
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

// The signup rewards component (v10:115204, _39408) with origin "match_end": its markup with the
// svelte-1v6b0qm classes (ui/css/svelte.css), REGISTER -> loginOrRegister (knEndRegister).
static const char SIGNUP_HTML[] =
    "<div class=\"signup-rewards-container signup-rewards-container--match-end svelte-1v6b0qm\">"
    "<div class=\"signup-rewards-header svelte-1v6b0qm\">Join Krunker today!</div> "
    "<div class=\"signup-rewards-content svelte-1v6b0qm\"><div class=\"signup-rewards-list signup-rewards-list--match-end svelte-1v6b0qm\">"
    "<div class=\"signup-reward svelte-1v6b0qm\"><i class=\"material-icons\" style=\"color:#3be354; font-size: 29px; margin-right: 18px;\">done</i> Complete challenges</div> "
    "<div class=\"signup-reward svelte-1v6b0qm\"><i class=\"material-icons\" style=\"color:#3be354; font-size: 29px; margin-right: 18px;\">done</i> Spend KR on cosmetics</div> "
    "<div class=\"signup-reward svelte-1v6b0qm\"><i class=\"material-icons\" style=\"color:#3be354; font-size: 29px; margin-right: 18px;\">done</i> Customize your avatar</div> "
    "<div class=\"signup-reward svelte-1v6b0qm\" style=\"margin-left: 88px;\">And much more!</div></div> "
    "<div class=\"rewards-side-panel svelte-1v6b0qm\"><div class=\"rewards-side-panel-title svelte-1v6b0qm\"><p class=\"svelte-1v6b0qm\">1-time signup perks:</p></div> "
    "<div class=\"rewards-side-panel-description svelte-1v6b0qm\"><p class=\"svelte-1v6b0qm\">Gain 14 Cosmetics +</p> "
    "<p style=\"margin-top: 0px;\" class=\"svelte-1v6b0qm\">1 Day premium for free!</p></div></div> "
    "<div class=\"signup-rewards-guest-earned-collect svelte-1v6b0qm\"></div> "
    "<div class=\"button bp-generic-btn signup-rewards-register-button svelte-1v6b0qm\" onmouseenter=\"playTick()\" "
    "onclick=\"knEndRegister()\">REGISTER</div></div></div>";

void hud_v10_end_signup(const int show) {
    if (!hud_v10_page()) return;
    const HtmlElem el = html_dom_by_id("matchEndSignupIncentives");
    if (!el) return;
    const int on = show && !account_get(); // the component renders nothing for an $account
    html_dom_set_html(el, on ? SIGNUP_HTML : "");
    html_dom_set_display(el, on ? "block" : "none");
}

int hud_v10_handle(const char *name) {
    if (!name) return -1;
    if (!strcmp(name, "knEndRegister")) {
        // _39403: clearPops(); loginOrRegister(true): the v10 login modal on its Register side
        // (v10_login.c, backed by kn_brain)
        static const HtmlArg reg = {HTML_ARG_BOOL, 1, NULL};
        html_ui_dispatch("clearPops", NULL, NULL, 0);
        html_ui_dispatch("loginOrRegister", NULL, &reg, 1);
        return 1;
    }
    return -1;
}
