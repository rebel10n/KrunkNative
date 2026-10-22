// Krunker Strike's PROFILE-DETAILS and message popups (strike_ui.h): FRVR 1.37.2's Mithril views, transcribed from
// assets/index-Cz2uYXJQ.js (read as text; no sandbox capture exists, so nothing here is pixel-verified).
//   jC PROFILE-DETAILS  the Profile's Account Details (KU's #accountdetails): FT tabs [Account Overview,
//                       Account Settings]. Que (overview): the face, the name, "Level N" with the XP into it,
//                       "Your game stats" (Kills, Deaths, KDR, Games) and Logout (QU). Vue's Profile section: the
//                       name change (Nue), which goes once it has been used this session (VU).
//   Oue (XA / p0 / UI, QU) the message frame: an icon, the message and one or two buttons. Success (XA), error
//                       (p0), the name's warning (UI) and the Logout confirm (QU) share it (POPUP_MESSAGE).
// Differences (documented): the country flag (Uue), the password (Hue, FRVR platform only) and Delete your
// account (zue, FRVR login only) are not shown: KrunkNative's brain has no route for them. The name is checked
// by kn_brain (its length and characters): its refusal is the error message, where FRVR checks it in the form.
// The icons are Material glyphs (FRVR's info-*.svg / icon-logout.svg are not in the 1.37.2 tree). Answers to the
// page's requests (nmeR, bpclmr) arrive as popupResponse: strike_popup_message takes them while expected.
#include <strike_ui.h>
#include <player_names.h> // Hide player names (player_names.h)
#include <strike_econ.h>
#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <i18n.h>
#include <ui_logic.h>

#include <cJSON.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef enum {
    MSG_SUCCESS, // XA: the checkmark, Close
    MSG_ERROR,   // p0: the cross, Close
    MSG_WARNING, // UI: the name's confirm (Pretty sure / Go back)
    MSG_LOGOUT,  // QU: the Logout confirm (Cancel / Logout)
} MsgKind;

typedef enum {
    PEND_NONE,   // the buttons only close the popup
    PEND_NAME,   // Pretty sure: send the name (nme)
    PEND_LOGOUT, // Logout: sign out and go back to the menu
    PEND_RETRY,  // Retry: the failed action runs again (strike_popup_retry); Close only closes
} MsgPending;

static struct {
    int tab;             // PROFILE-DETAILS' tab: 0 Account Overview, 1 Account Settings
    int name_changed;    // VU: the name changed this session (the name form goes)
    int expect;          // strike_popup_expect: the next popupResponse (kind 0 / 1) is this page's answer
    int expect_name;     // the expected answer is the name change's
    char expect_text[96];
    // the message popup (Oue)
    MsgKind kind;
    char message[512];   // the message's HTML (escaped by the caller)
    char primary[32], secondary[32];
    int inverse;        // inverseCtaOrder: the secondary button first
    MsgPending pending;
    void (*retry)(void); // PEND_RETRY: the action Retry runs
    char name[64];       // the name the confirm asks for
} P;

// ---------------------------------------------------------------- helpers

static char *part(const char *file, const char *name, const StrikeVar *vars, const int count) {
    char *html = strike_template_part(file, name, vars, count);
    return html ? html : calloc(1, 1);
}

// "1234.5678" -> "1,234.5678" (N.commaFloat): commas in the whole part only
static void commas(const char *num, char *out, const size_t n) {
    const char *dot = strchr(num, '.');
    const size_t whole = dot ? (size_t) (dot - num) : strlen(num);
    const size_t sign = num[0] == '-' ? 1 : 0;
    size_t o = 0;
    if (sign) out[o++] = '-';
    for (size_t i = sign; i < whole && o + 2 < n; i++) {
        if (i > sign && (whole - i) % 3 == 0) out[o++] = ',';
        out[o++] = num[i];
    }
    for (const char *p = dot; p && *p && o + 1 < n; p++) out[o++] = *p;
    out[o] = 0;
}

static char *escaped(const char *text) {
    StrBuf sb = {0};
    sb_append_esc(&sb, text ? text : "");
    return sb_finish(&sb);
}

static const char *account_name(void) {
    static char name[64];
    const AccountInfo *acc = active_account();
    snprintf(name, sizeof name, "%s", !acc ? "Guest" : player_names_account(acc->alias[0] ? acc->alias : acc->name));
    return name;
}

// ---------------------------------------------------------------- PROFILE-DETAILS (jC)

// Que: the overview (its Logout only for an account: FRVR's isLogoutSupported, the guest has nothing to leave)
static char *overview_html(void) {
    const AccountInfo *acc = active_account();
    const StrikeAccount *st = strike_econ_account();
    const int level = st->level.level > 0 ? st->level.level : 1;
    const double kills = (double) st->kills, deaths = (double) st->deaths;
    char avatar[96], lvl[16], xp[32], next[32], num[32], kills_text[48], deaths_text[48], kdr_text[32], kdr[48], games_text[48];
    snprintf(avatar, sizeof avatar, "./cdn/textures/classes/icon_%d.png", strike_menu_class_id());
    snprintf(lvl, sizeof lvl, "%d", level);
    snprintf(xp, sizeof xp, "%lld", (long long) st->level.xp_in_level);
    snprintf(next, sizeof next, "%lld", (long long) st->level.xp_to_next);
    snprintf(num, sizeof num, "%lld", (long long) st->kills);
    commas(num, kills_text, sizeof kills_text);
    snprintf(num, sizeof num, "%lld", (long long) st->deaths);
    commas(num, deaths_text, sizeof deaths_text);
    snprintf(kdr_text, sizeof kdr_text, "%.4f", kills / (deaths > 1.0 ? deaths : 1.0)); // (kills / max(deaths, 1)).toFixed(4)
    commas(kdr_text, kdr, sizeof kdr);
    snprintf(num, sizeof num, "%lld", (long long) st->games);
    commas(num, games_text, sizeof games_text);
    char *logout = acc ? part("PROFILE-DETAILS", "logout", NULL, 0) : NULL;
    const StrikeVar v[] = {{"avatar", avatar}, {"name", account_name()}, {"level", lvl}, {"xp", xp}, {"nextXp", next},
                           {"kills", kills_text}, {"deaths", deaths_text}, {"kdr", kdr}, {"games", games_text},
                           {"logout", logout ? logout : ""}};
    char *html = strike_template_part("PROFILE-DETAILS", "overview", v, 10);
    free(logout);
    return html;
}

// Vue's Profile section with Nue (the name change): shown to an account that has not changed its name
static char *settings_html(void) {
    if (!active_account() || P.name_changed) return calloc(1, 1);
    return part("PROFILE-DETAILS", "settings", NULL, 0);
}

static void profile_tab(const HtmlElem root, const int tab) {
    P.tab = tab ? 1 : 0;
    HtmlElem items[2];
    const int n = html_dom_query_all(root, ".tabSelectorItem", items, 2);
    for (int i = 0; i < n; i++) html_dom_set_class(items[i], "selected", i == P.tab);
    const HtmlElem content = html_dom_query(root, ".tabsContent");
    if (!content) return;
    char *html = P.tab == 0 ? overview_html() : settings_html();
    html_dom_set_html(content, html ? html : "");
    free(html);
}

// ---------------------------------------------------------------- the message popup (Oue)

static const char *msg_icon(const MsgKind k) {
    switch (k) {
    case MSG_SUCCESS: return "done";
    case MSG_ERROR: return "clear";
    case MSG_WARNING: return "warning";
    case MSG_LOGOUT: return "logout";
    }
    return "info";
}

static void message_set(const MsgKind kind, const char *message_html, const char *primary, const char *secondary,
                        const int inverse, const MsgPending pending) {
    P.kind = kind;
    snprintf(P.message, sizeof P.message, "%s", message_html);
    snprintf(P.primary, sizeof P.primary, "%s", primary);
    snprintf(P.secondary, sizeof P.secondary, "%s", secondary ? secondary : "");
    P.inverse = inverse;
    P.pending = pending;
}

// the popup frame: the screen is pushed once, or redrawn with the new message
static void message_show(void) {
    if (strike_screen_shown(STRIKE_SCREEN_POPUP_MESSAGE)) strike_screen_rerender(STRIKE_SCREEN_POPUP_MESSAGE);
    else strike_screen_push(STRIKE_SCREEN_POPUP_MESSAGE);
}

static void message_fill(const HtmlElem root) {
    const HtmlElem content = html_dom_query(root, ".alert-content");
    if (!content) return;
    const int two = P.secondary[0] != 0;
    char primary[320] = "", secondary[320] = "";
    snprintf(primary, sizeof primary,
             "<div id=\"alertScreenBtn\" class=\"frvr-button %s\" onclick=\"strikeAction('msg-primary')\"><span class=\"frvr-button-text\">%s</span></div>",
             two ? "frvr-button--width-120" : "frvr-button--width-150", P.primary);
    if (two) {
        snprintf(secondary, sizeof secondary,
                 "<div id=\"alertScreenBtnSec\" class=\"frvr-button frvr-button--gray frvr-button--width-120\" onclick=\"strikeAction('msg-secondary')\"><span class=\"frvr-button-text\">%s</span></div>",
                 P.secondary);
    }
    StrBuf sb = {0};
    // .alert-img is an img's width (65 x the base scale): as a glyph it is a centred block of that size
    sb_appendf(&sb, "<i class=\"alert-img material-icons\" style=\"display:block;margin:0 auto;line-height:1;font-size:calc(42 * var(--base-scale));\">%s</i>",
               msg_icon(P.kind));
    sb_appendf(&sb, "<div class=\"alert-text\"><span>%s</span></div>", P.message);
    sb_append(&sb, P.inverse ? secondary : primary);
    sb_append(&sb, P.inverse ? primary : secondary);
    char *html = sb_finish(&sb);
    html_dom_set_html(content, html ? html : "");
    free(html);
}

// ---------------------------------------------------------------- actions

static void logout(void) {
    account_ui_logout(); // logoutAcc(): the vault keeps the profile; battlepass_ui_reset and the account box follow
    strike_screen_replace(STRIKE_SCREEN_MAIN_MENU); // the menu again (as Home does: the stack becomes the menu)
}

// nme: the name change (kn_brain's acct_set_alias); the answer (nmeR) is the expected popup
static void name_send(void) {
    if (!active_account() || !P.name[0]) return;
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("nme"));
    cJSON_AddItemToArray(frame, cJSON_CreateString(P.name));
    char *text = cJSON_PrintUnformatted(frame);
    cJSON_Delete(frame);
    strike_popup_expect("Name updated successfully!");
    P.expect_name = 1;
    if (text) backend_social_send_text(text);
    free(text);
}

// Nue's Submit: the name is trimmed; an empty one is marked, else the confirm asks for it
static void name_submit(const HtmlElem root) {
    (void) root;
    const HtmlElem in = html_dom_by_id("newName");
    char name[64] = "";
    if (in) html_dom_get_value(in, name, sizeof name);
    size_t a = 0;
    while (name[a] == ' ') a++;
    char trimmed[64];
    snprintf(trimmed, sizeof trimmed, "%s", name + a);
    for (size_t n = strlen(trimmed); n > 0 && trimmed[n - 1] == ' '; n--) trimmed[n - 1] = 0;
    if (in) html_dom_set_class(in, "is-error", !trimmed[0]);
    if (!trimmed[0]) return;
    snprintf(P.name, sizeof P.name, "%s", trimmed);
    char *title = escaped(trimmed);
    char html[640];
    snprintf(html, sizeof html,
             "<p class=\"title-text\">%s</p><div><span>Are you sure this name is cool enough?</span><div class=\"strong\">You won't be able to change your username again.</div></div>",
             title ? title : "");
    free(title);
    message_set(MSG_WARNING, html, "Pretty sure", "Go back", 1, PEND_NAME);
    message_show();
}

int strike_profile_render(const StrikeScreen s, const HtmlElem root) {
    if (s == STRIKE_SCREEN_PROFILE_DETAILS) {
        profile_tab(root, P.tab);
        return 1;
    }
    if (s == STRIKE_SCREEN_POPUP_MESSAGE) {
        message_fill(root);
        return 1;
    }
    return 0;
}

int strike_profile_action(const char *a) {
    if (!strcmp(a, "profile-details")) { // Account Details (KU's #accountdetails): Pt(jC)
        P.tab = 0;
        strike_screen_push(STRIKE_SCREEN_PROFILE_DETAILS);
        return 1;
    }
    if (!strncmp(a, "profile-tab:", 12)) {
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_PROFILE_DETAILS);
        if (root) profile_tab(root, atoi(a + 12));
        return 1;
    }
    if (!strcmp(a, "logout")) { // QU: kr(QU) is the confirm
        message_set(MSG_LOGOUT, "Logout from this account?", "Cancel", "Logout", 0, PEND_LOGOUT);
        message_show();
        return 1;
    }
    if (!strcmp(a, "name-submit")) {
        const HtmlElem root = strike_screen_root(STRIKE_SCREEN_PROFILE_DETAILS);
        if (root) name_submit(root);
        return 1;
    }
    if (!strcmp(a, "msg-primary") || !strcmp(a, "msg-secondary")) {
        const int primary = !strcmp(a, "msg-primary");
        const MsgPending pending = P.pending;
        P.pending = PEND_NONE;
        if (pending == PEND_RETRY) {
            void (*retry)(void) = P.retry;
            P.retry = NULL;
            if (strike_screen_top() == STRIKE_SCREEN_POPUP_MESSAGE) strike_screen_pop();
            if (primary && retry) retry();
        } else if (pending == PEND_LOGOUT && !primary) logout();
        else if (pending == PEND_NAME && primary) {
            strike_screen_pop(); // the confirm closes (kt), then the name goes
            name_send();
        } else if (strike_screen_top() == STRIKE_SCREEN_POPUP_MESSAGE) strike_screen_pop();
        return 1;
    }
    return 0;
}

// ---------------------------------------------------------------- the account's answers

// The next popupResponse of kind 0 (error) / 1 (success) is this page's: the success text when it has none.
void strike_popup_expect(const char *success) {
    P.expect = 1;
    P.expect_name = 0;
    snprintf(P.expect_text, sizeof P.expect_text, "%s", success ? success : "");
}

// A failed action's error (p0): its text, Retry (runs retry again) and Close. The text is plain (escaped here).
void strike_popup_retry(const char *text, void (*retry)(void)) {
    char *esc = escaped(text ? text : "");
    char html[640];
    snprintf(html, sizeof html, "<div><span>%s</span></div>", esc ? esc : "");
    free(esc);
    message_set(MSG_ERROR, html, I18N("kn.retry", "Retry"), I18N("generic.close", "Close"), 0, PEND_RETRY);
    P.retry = retry;
    message_show();
}

// account_ui's popup_response: 1 when the Strike page shows the answer (the v10 popup is not on this page)
int strike_popup_message(const int kind, const char *error, const char *text) {
    if (!P.expect || kind == 2) return 0; // a loading popup stays with the request
    P.expect = 0;
    const int name = P.expect_name;
    P.expect_name = 0;
    char *body;
    if (kind == 1) {
        const char *msg = text && text[0] ? text : P.expect_text[0] ? P.expect_text : "Success!";
        body = escaped(msg);
        if (name) P.name_changed = 1;
        message_set(MSG_SUCCESS, body ? body : "", "Close", NULL, 0, PEND_NONE);
    } else {
        body = escaped(error && error[0] ? error : "Failed. Try Again");
        message_set(MSG_ERROR, body ? body : "", "Close", NULL, 0, PEND_NONE);
    }
    free(body);
    message_show();
    if (name) strike_screen_rerender(STRIKE_SCREEN_PROFILE_DETAILS); // Vue drops the name form (VU)
    return 1;
}
