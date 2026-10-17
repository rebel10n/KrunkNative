// Krunker Strike's Double XP (client/include/strike_double_xp.h). FRVR 1.37.2's lean end screen has a
// "Double XP" button in its reward row (.lean-end-screen__additional-reward-buttons-container >
// .lean-end-screen__double-reward: the blue frvr-button--size-sm / --width-150 with the ad icon on its left,
// pulsing while it can be taken, "Collected" once spent). Here it is the per-match offer of double_xp_offer.c:
// shown while the account's offer is claimable, the claim sends "rqdxp" at once (no ad), and it stays as
// "Collected" for the match once granted. The badge "2x XP" (FRVR's booster layout, .booster-time-left,
// top right) follows the mode's doubleXP flag, as Box Office's does. Only the Strike page calls these.
#include <strike_double_xp.h>

#include <double_xp_offer.h>
#include <html_ui.h>
#include <i18n.h>
#include <strike_ui.h>
#include <ui_logic.h> // menu_ui(): the client that the badge reads the mode of

#include <stdio.h>
#include <string.h>

// the badge (FRVR's booster layout) and the reward row's button (FRVR's Double XP label,
// window.endgame.player.doubleGainedXP; the ad icon on its left)
#define DXP_BADGE_HTML_FMT "<div id=\"dxpBadge\" class=\"booster-time-left dxp-badge\" style=\"display:none\">" \
    "<div class=\"booster-time-left__bg dxp-badge__bg\"></div>" \
    "<div class=\"booster-time-left__text-holder\"><div class=\"booster-time-left__text\">%s</div></div></div>"

#define DXP_BUTTON_HTML_FMT "<div class=\"lean-end-screen__additional-reward-buttons-container\" style=\"display:none\">" \
    "<div class=\"lean-end-screen__double-reward\">" \
    "<div id=\"dxpStrikeButton\" class=\"frvr-button frvr-button--blue frvr-button--size-sm frvr-button--width-150 frvr-animation-pulse\" " \
    "onclick=\"strikeAction('double-xp')\"><img src=\"./img/frvr-ui/ad-icon.svg\" class=\"\">" \
    "<span class=\"frvr-button-text\">%s</span></div></div></div>"

static int g_busy; // a claim is in flight (the button takes no second click)

// the reward row's container: shown while claimable or after the claim (Collected), pulsing only while
// claimable; only what changed is written
static void sync_button(void) {
    const HtmlElem button = html_dom_by_id("dxpStrikeButton");
    if (!button) return;
    const HtmlElem row = html_dom_closest(button, ".lean-end-screen__additional-reward-buttons-container");
    const int open = double_xp_offer_open(), taken = double_xp_taken();
    if (row) {
        const int shown = open || taken;
        if (html_dom_is_shown(row) != shown) html_dom_set_display(row, shown ? "" : "none");
    }
    if (html_dom_has_class(button, "is-disabled") == open) html_dom_set_class(button, "is-disabled", !open);
    if (html_dom_has_class(button, "frvr-animation-pulse") != open) html_dom_set_class(button, "frvr-animation-pulse", open);
    const HtmlElem label = html_dom_query(button, ".frvr-button-text");
    if (!label) return;
    const char *text = taken ? I18N("kn.dxp.collected", "Collected") : I18N("window.endgame.player.doubleGainedXP", "Double XP");
    char cur[32] = "";
    html_dom_get_text(label, cur, sizeof(cur));
    if (strcmp(cur, text)) html_dom_set_text(label, text);
}

void strike_double_xp_tick(void) {
    sync_button();
    const HtmlElem holder = html_dom_by_id("frvrMenuMobileHolder");
    if (!holder) return;
    if (!html_dom_by_id("dxpBadge")) {
        char html[640];
        snprintf(html, sizeof(html), DXP_BADGE_HTML_FMT, I18N("kn.dxp.badge", "2x XP"));
        html_dom_insert_html(holder, HTML_INSERT_BEFORE_END, html);
    }
    const HtmlElem badge = html_dom_by_id("dxpBadge");
    if (!badge) return;
    const MenuUi *menu = menu_ui();
    const Client *client = menu ? menu->client : NULL;
    const int xp = client && client->game.map && client->game.mode && client->game.mode->config.double_xp;
    html_dom_set_display(badge, xp ? "" : "none");
}

void strike_double_xp_end_fill(const HtmlElem end_root) {
    if (!end_root || html_dom_query(end_root, ".lean-end-screen__double-reward")) return; // the end timer repeats its fill
    const HtmlElem wrapper = html_dom_query(end_root, ".lean-end-screen__rewards-wrapper");
    if (!wrapper) return;
    char html[768];
    snprintf(html, sizeof(html), DXP_BUTTON_HTML_FMT, I18N("window.endgame.player.doubleGainedXP", "Double XP"));
    html_dom_insert_html(wrapper, HTML_INSERT_BEFORE_END, html);
}

// the claim's answer: rrqdxp's doubled score pops "XP Doubled!" (FRVR's reward popup of origin doubleXp, a
// Nice button that closes it through strikeAction('back'), the POPUP screen's own close)
static void popup(const char *title, const char *text) {
    strike_screen_push(STRIKE_SCREEN_POPUP);
    const HtmlElem root = strike_screen_root(STRIKE_SCREEN_POPUP);
    const HtmlElem holder = root ? html_dom_query(root, ".popup__holder") : 0;
    if (!holder) return;
    char html[1024];
    snprintf(html, sizeof(html),
             "<div class=\"dxp-popup\"><div class=\"frvr-window-title\">%s</div><div class=\"dxp-popup__text\">%s</div>"
             "<div class=\"frvr-button frvr-button--blue frvr-button--size-md frvr-button--width-150\" "
             "onclick=\"strikeAction('back')\"><span class=\"frvr-button-text\">%s</span></div></div>",
             title, text, I18N("generic.nice", "Nice!"));
    html_dom_set_html(holder, html);
}

static void on_claim(void *user, const DoubleXpResult result) {
    (void) user;
    g_busy = 0;
    if (result == DOUBLE_XP_GRANTED)
        popup(I18N("window.endgame.player.xpDoubled", "XP Doubled!"), I18N("window.endgame.player.xpDoubleSubTitle", "The more XP the better, right? Let's go!"));
    sync_button();
}

void strike_double_xp_claim(void) {
    if (g_busy) return;
    if (!html_dom_by_id("dxpStrikeButton")) return;
    g_busy = 1;
    double_xp_claim(on_claim, NULL);
}
