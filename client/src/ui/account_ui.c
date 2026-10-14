// Krunker v3.1.1 account in the main menu, over the brain's social socket
// (client/include/backend.h). Readable source: public/js/src/index.js
// (sendAccountRequest, accountResponse, updateAccount, cK, registerAcc, loginAcc,
// logoutAcc, readyLoad's token restore, windows[4] = the Account window) and
// src/data/account.js (Account.setData / updateLevel).
//
// v3.1.1 sends "a" on the game socket and the game server proxies it; KrunkNative's
// menu can sit on an offline game or a direct --connect server, so it authenticates
// on the brain's social socket directly (same frames: ["a", action, payload] ->
// ["a", error, id, username, data, token]). The game server learns the account from
// the session token / seek ticket in NET_PACKET_JOIN (client/src/net/net.c).
//
// Storage keys are v3.1.1's (krunker_id, krunker_username, krunker_token); the
// social hub (social_ui.c) reads the same ones.
#include <v10_menu.h> // the v10 page's header bar (v10_menu.c)
#include <backend.h>
#include <backend_ui.h>
#include <app_mode.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#define timegm _mkgmtime // MinGW has no timegm (same as editor_net.c)
#endif
#include "account_flags.h"
#include <progression.h> // WP-E part 2c: v7 level progress, challenge names
#include <badges.h>      // WP-E part 2c: badges.js
#include <map_catalog.h> // WP-E part 2c: the Map Challenges cards
#include <weapons.h>
#include "../viewer/viewer_store.h" // WP-E part 2c: challenge reward skins (store.skins)
#include "qr_code.h"                // v7.1.4 2FA: the otpauth:// QR code (QrCode.toDataURL)
#include <ruleset.h>
#include <v10_social.h> // the window's Friends tab (v10_social.c)
#include <online_mode.h>  // auth_unreachable: offline mode

#define RANK_VAR 0.03            // config.rankVar
#define ACC_STR_WAIT "Please wait..." // generic.wait
#define REWARD_MIN_LVL 1             // the Free KR wheel's minLvl (v3.1.1's daily reward: config.rewardMinLvl 5)
#define REWARD_TIME_MS 43200000.0    // config.rewardTime (12 h in v7.1.4)

static Client *g_client;
static AccountInfo g_acc;
static int g_logged_in;
static int g_header_dirty = 1;   // cK() pending (page not up yet / offsetWidth not laid out)
static int g_header_tries;
static int g_restore_sent;       // a token restore is in flight
static int g_tab_index;          // windows[4].tabIndex
static int g_logout_sessions;    // d1: "Logout of all Sessions" clicked
static char g_token[192];
static char g_guest_name[32];    // client->menu_name before the account took it over
static int g_reward_played;      // dc: a match ended this session (canClaimReward)
static char g_reward_text[16];   // d8: last #claimTimer text
// windows[29] (Krunker Mail)
static int g_mail_tab;           // the kml request / answer: 0 unread, 1 archive
static int g_mail_view;          // windows[29].tabIndex (v7: Followers, Clan, Hub, Inbox, Archive)
static cJSON *g_mail_rows;       // mailData.d (owned), NULL = not loaded
static int g_mail_loaded;        // mailData != null
// v7.1.4 2FA (authenticator app): v10808, the label sub2FA sends the code with ("2fa" = the
// login step, "a2FAc" / "d2FAc" = confirm enabling / disabling)
#define TWOFA_KEY_LEN 6 // config.twoFAKeyLen
static char g_2fa_label[8];
// KrunkNative email verification (kn_brain "emailS" / "cemail" / "emailv"; v7.1.4 has none)
static struct {
    int known, verified, pending, wait, mail;
    char addr[256];
} g_email;
static double g_auth_sent_at;    // a login/register is waiting for its "a" reply (glfwGetTime), 0 none
// player-viewer WP-E part 2c: v7 challenges (windows[32]) and the account's medal counts
static cJSON *g_chal;            // account.activeChallenges from kn_brain's "chgL" (owned), NULL = not loaded
static int g_chal_requested;     // a "chg" request is out
static char g_chal_typ[12];      // windows[32].openChallenge.typ ("" = home)
static int g_chal_ind;           // windows[32].openChallenge.ind
static cJSON *g_medal_counts;    // "mdl" {medal: count} (owned), NULL = not loaded
static int g_scenario;           // account_ui_scenario: a stand-in account (no kn_brain)
static cJSON *g_scenario_c2fa;   // account.c2fa: a "c2fa" frame replayed after the window (the 2FA popup)
#define AUTH_TIMEOUT_SECONDS 8.0

// ------------------------------------------------------------------ helpers

static void storage_get(const char *key, char *out, const size_t n) {
    out[0] = 0;
    if (html_storage_get(key, out, n) < 0) out[0] = 0;
}

static void set_display(const char *id, const char *value) {
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_set_display(el, value);
}

static void copy_str(char *dst, const size_t n, const char *src) {
    snprintf(dst, n, "%s", src ? src : "");
}

static double num_at(const cJSON *arr, const int i) {
    const cJSON *v = cJSON_GetArrayItem(arr, i);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : 0.0;
}

static const char *str_at(const cJSON *arr, const int i) {
    const cJSON *v = cJSON_GetArrayItem(arr, i);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

// UTILS.commaNumber
static void comma_number(char *out, const size_t n, const double value) {
    char digits[32];
    snprintf(digits, sizeof(digits), "%.0f", value);
    const char *d = digits;
    size_t o = 0;
    if (*d == '-') {
        if (o + 1 < n) out[o++] = '-';
        d++;
    }
    const size_t len = strlen(d);
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

// UTILS.getReadableTime(ms)
static void readable_time(char *out, const size_t n, const double ms) {
    long long s = (long long) floor(ms / 1000.0);
    long long m = s / 60;
    long long h = m / 60;
    m %= 60;
    const long long d = h / 24;
    h %= 24;
    char dp[24] = "", hp[24] = "";
    if (d) snprintf(dp, sizeof(dp), "%lldd ", d);
    if (h) snprintf(hp, sizeof(hp), "%lldh ", h);
    snprintf(out, n, "%s%s%lldm ", dp, hp, m);
}

void account_readable_time(char *out, const size_t n, const double ms) { readable_time(out, n, ms); }

// ------------------------------------------------------------------ state

const AccountInfo *account_get(void) { return g_logged_in ? &g_acc : NULL; }

// One no-argument social frame (v10 "ttl": the account's titles; v10_customize.c). The frame
// goes through the same queue as the mail frames (backend_social_send_text).
void account_send_frame(const char *label) {
    char frame[64];
    snprintf(frame, sizeof(frame), "[\"%s\"]", label);
    backend_social_send_text(frame);
}

static void mark_header(void);
static void rerender_windows(void);

static void reward_tick(int force);

void account_remove_skin(const int skin_index) {
    if (!g_logged_in || !g_acc.data) return;
    cJSON *skins = cJSON_GetArrayItem(g_acc.data, 10);
    cJSON *entry;
    int i = 0;
    cJSON_ArrayForEach(entry, skins) {
        cJSON *ind = cJSON_GetObjectItem(entry, "ind"), *cnt = cJSON_GetObjectItem(entry, "cnt");
        if (cJSON_IsNumber(ind) && ind->valueint == skin_index) {
            const int n = (cJSON_IsNumber(cnt) ? cnt->valueint : 1) - 1;
            if (n > 0) cJSON_ReplaceItemInObject(entry, "cnt", cJSON_CreateNumber(n));
            else cJSON_DeleteItemFromArray(skins, i);
            return;
        }
        i++;
    }
}

void account_set_last_reward(const double ms) {
    if (!g_logged_in || !g_acc.data) return;
    cJSON_ReplaceItemInArray(g_acc.data, 11, cJSON_CreateNumber(ms));
    reward_tick(1);
}

void account_set_funds(const double funds) {
    if (!g_logged_in) return;
    g_acc.funds = funds;
    if (g_acc.data) cJSON_ReplaceItemInArray(g_acc.data, 5, cJSON_CreateNumber(funds));
    mark_header();
}

int account_add_skin(const int skin_index) {
    if (!g_logged_in || !g_acc.data) return 0;
    cJSON *skins = cJSON_GetArrayItem(g_acc.data, 10);
    if (!cJSON_IsArray(skins)) {
        skins = cJSON_CreateArray();
        cJSON_ReplaceItemInArray(g_acc.data, 10, skins);
    }
    cJSON *entry;
    cJSON_ArrayForEach(entry, skins) {
        cJSON *ind = cJSON_GetObjectItem(entry, "ind");
        if (cJSON_IsNumber(ind) && ind->valueint == skin_index) {
            cJSON *cnt = cJSON_GetObjectItem(entry, "cnt");
            const int n = (cJSON_IsNumber(cnt) ? cnt->valueint : 0) + 1;
            cJSON_ReplaceItemInObject(entry, "cnt", cJSON_CreateNumber(n));
            return n;
        }
    }
    entry = cJSON_CreateObject();
    cJSON_AddNumberToObject(entry, "ind", skin_index);
    cJSON_AddNumberToObject(entry, "cnt", 1);
    cJSON_AddItemToArray(skins, entry);
    return 1;
}
const char *account_session_token(void) { return g_token; }

// windows[12]: acc.clan = clanData.clan_name / null (data slot 7 follows)
void account_set_clan(const char *tag) {
    if (!g_logged_in) return;
    copy_str(g_acc.clan, sizeof(g_acc.clan), tag ? tag : "");
    if (g_acc.data) cJSON_ReplaceItemInArray(g_acc.data, 7, tag && tag[0] ? cJSON_CreateString(tag) : cJSON_CreateNull());
    mark_header();
}

// Account.updateLevel(): levelProg is v7.1.4's getLevelProg (linear in score, WP-E part 2c)
static void update_level(void) {
    g_acc.level = progression_level(g_acc.score, RANK_VAR);
    g_acc.level_prog = progression_level_prog(g_acc.score, RANK_VAR);
}

// Account.setData(h)
static void set_data(const cJSON *h) {
    if (!cJSON_IsArray(h)) return;
    g_acc.kills = num_at(h, 1);
    g_acc.wins = num_at(h, 2);
    g_acc.games = num_at(h, 3);
    g_acc.deaths = num_at(h, 4);
    g_acc.funds = num_at(h, 5);
    g_acc.score = num_at(h, 6);
    update_level();
    copy_str(g_acc.clan, sizeof(g_acc.clan), str_at(h, 7));
    g_acc.time_played = num_at(h, 8);
    g_acc.featured = (int) num_at(h, 9);
    g_acc.is_dev = (int) num_at(h, 12);
    g_acc.following = (int) num_at(h, 13);
    g_acc.followers = (int) num_at(h, 14);
    cJSON_Delete(g_acc.stats);
    const cJSON *stats = cJSON_GetArrayItem(h, 16);
    g_acc.stats = cJSON_IsObject(stats) ? cJSON_Duplicate(stats, 1) : cJSON_CreateObject();
    g_acc.hack = (int) num_at(h, 17);
    g_acc.is_mod = (int) num_at(h, 21);
    g_acc.elo = num_at(h, 22); // player_elo: this season's ranked rating (kn_brain ranked.c)
    g_acc.premium = num_at(h, 27) > 0;
    g_acc.has_2fa = (int) num_at(h, 28);
    const char *alias = str_at(h, 29);
    copy_str(g_acc.alias, sizeof(g_acc.alias), g_acc.premium && alias && alias[0] ? alias : g_acc.name);
    cJSON_Delete(g_acc.data);
    g_acc.data = cJSON_Duplicate(h, 1);
}

static double stat_num(const char *key) {
    const cJSON *v = g_acc.stats ? cJSON_GetObjectItem(g_acc.stats, key) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : 0.0;
}

// The account name is the name the menu and HUD show (v3.1.1: acc.alias / "Guest").
static void apply_menu_name(void) {
    if (!g_client) return;
    if (g_logged_in) {
        if (!g_guest_name[0]) copy_str(g_guest_name, sizeof(g_guest_name), g_client->menu_name);
        copy_str(g_client->menu_name, sizeof(g_client->menu_name), g_acc.alias);
    } else if (g_guest_name[0]) {
        copy_str(g_client->menu_name, sizeof(g_client->menu_name), g_guest_name);
        g_guest_name[0] = 0;
    }
    // the game server reads the session token from the JOIN packet of the next connection
    pthread_mutex_lock(&g_client->net_lock);
    copy_str(g_client->net_join.session, sizeof(g_client->net_join.session), g_logged_in ? g_token : "");
    pthread_mutex_unlock(&g_client->net_lock);
}

// ------------------------------------------------------------------ cK(): header bar

// updateMenuAccountData() (v7.1.4): #signedInHeaderBar (name, KR, junk, level bar and
// box) for an account, #signedOutHeaderBar ("Login or Register") for a guest.
static void update_header(void) {
    g_header_dirty = 0;
    if (v10_menu_page()) { // Krunker v10's header bar (v10_menu.c)
        v10_menu_account_changed();
        return;
    }
    if (!html_ui_available() || !html_dom_by_id("signedInHeaderBar")) {
        g_header_dirty = 1;
        return;
    }
    if (g_logged_in) {
        set_display("signedOutHeaderBar", "none");
        set_display("signedInHeaderBar", "");
        HtmlElem el;
        // premiumT && SETTINGS.tmp.premiumBadge ? alias : name (v7 privacy setting, WP-E part 2c)
        if ((el = html_dom_by_id("menuAccountUsername"))) html_dom_set_text(el, g_acc.premium && !ui_setting_is_false("premiumBadge") ? g_acc.alias : g_acc.name);
        if ((el = html_dom_by_id("menuKRCount"))) {
            char funds[32], html[128];
            comma_number(funds, sizeof(funds), g_acc.funds);
            snprintf(html, sizeof(html), "%s <span style='color:#fbc02d'>KR</span>", funds);
            html_dom_set_html(el, html);
        }
        // junk (crafting material): KrunkNative's accounts have none (the store economy is out of scope)
        if ((el = html_dom_by_id("menuJNKCount"))) html_dom_set_html(el, "0.00<span class='material-icons' style='color:#cc51f8;font-size:27px;margin-left:5px;vertical-align:top;margin-right:-5px;'>plumbing</span>");
        if ((el = html_dom_by_id("menuLevelBar"))) {
            char w[16];
            snprintf(w, sizeof(w), "%.6g%%", g_acc.level_prog); // menuLevelBar.style.width = levelProg + "%"
            html_dom_set_style(el, "width", w);
        }
        if ((el = html_dom_by_id("mLevelContV"))) {
            char lvl[16];
            snprintf(lvl, sizeof(lvl), "%d", g_acc.level ? g_acc.level : 1);
            html_dom_set_text(el, lvl);
        }
        if ((el = html_dom_by_id("mailCount"))) { // windows[29].mailCount(count)
            char count[16] = "";
            if (g_acc.mail_unread > 99) snprintf(count, sizeof(count), "99+");
            else if (g_acc.mail_unread > 0) snprintf(count, sizeof(count), "%d", g_acc.mail_unread);
            html_dom_set_html(el, count[0] ? count : "0");
            html_dom_set_display(el, g_acc.mail_unread > 0 ? "block" : "none");
        }
        HtmlElem bar = html_dom_by_id("signedInHeaderBar");
        if (bar && html_dom_offset_width(bar) > 0.0f) html_dom_set_style(bar, "border-radius", "0px");
        else if (g_header_tries++ < 30) g_header_dirty = 1; // laid out on a later frame
    } else {
        set_display("signedOutHeaderBar", "");
        set_display("signedInHeaderBar", "none");
    }
}

static void mark_header(void) {
    g_header_dirty = 1;
    g_header_tries = 0;
}

// showAccResp(html)
static void show_acc_resp(const char *html) {
    HtmlElem el = html_dom_by_id("accResp");
    if (!el) return;
    html_dom_set_html(el, html ? html : "");
    html_dom_set_display(el, html && html[0] ? "block" : "none");
}

// popupResponse(kind, error, text) / dP(text) over the menu's #popupHolder > #popupContent
static void popup_response(const int kind, const char *error, const char *text) {
    static const char *const pops[] = {"popupContent", "confPremiumPop", "gameCodePop", "tradePop", "twitchPop", "pfpPop", "dropsPop", "confPop"};
    set_display("popupHolder", "block");
    for (size_t i = 0; i < sizeof(pops) / sizeof(pops[0]); i++) set_display(pops[i], "none");
    HtmlElem content = html_dom_by_id("popupContent");
    if (!content) return;
    html_dom_set_style(content, "width", "300px");
    html_dom_set_display(content, "block");
    StrBuf sb = {0};
    if (kind == 2) {
        sb_append(&sb, "<div style='width:100%;text-align:center'><div class='lds-ring sm'><div></div><div></div><div></div><div></div></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
        sb_append_esc(&sb, text ? text : "");
    } else if (kind == 1) {
        sb_append(&sb, "<div style='width:100%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#3be354'>done</i></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
        sb_append_esc(&sb, text && text[0] ? text : "Success!");
    } else {
        sb_append(&sb, "<div style='width:100%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#e33b3b'>clear</i></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
        sb_append_esc(&sb, error && error[0] ? error : "Failed. Try Again");
    }
    sb_append(&sb, "</div></div>");
    char *html = sb_finish(&sb);
    html_dom_set_html(content, html);
    free(html);
}

void account_popup(const int kind, const char *error, const char *text) { popup_response(kind, error, text); }

static void rerender_windows(void) {
    menu_rerender_if_open(5);
    menu_rerender_if_open(6); // v7.1.4 Battle Pass (battlepass_ui.c)
    menu_rerender_if_open(14);
    menu_rerender_if_open(27);
    menu_rerender_if_open(33);
    menu_rerender_if_open(13);
}

// ------------------------------------------------------------------ requests

// sendAccountRequest(action, payload); email: KrunkNative's optional 4th payload element (register)
static void send_account_request_email(const int action, const char *name, const char *pass, const char *third,
                                       const char *email) {
    if (action >= 0) show_acc_resp(ACC_STR_WAIT);
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("a"));
    cJSON_AddItemToArray(frame, cJSON_CreateNumber(action));
    if (action >= 0) {
        cJSON *payload = cJSON_CreateArray();
        cJSON_AddItemToArray(payload, cJSON_CreateString(name ? name : ""));
        cJSON_AddItemToArray(payload, pass ? cJSON_CreateString(pass) : cJSON_CreateNull());
        cJSON_AddItemToArray(payload, third ? cJSON_CreateString(third) : cJSON_CreateNull());
        if (email && email[0]) cJSON_AddItemToArray(payload, cJSON_CreateString(email));
        cJSON_AddItemToArray(frame, payload);
        cJSON_AddItemToArray(frame, cJSON_CreateNull()); // captcha key (no captcha in KrunkNative)
    }
    backend_social_send(frame);
    cJSON_Delete(frame);
}

static void send_account_request(const int action, const char *name, const char *pass, const char *third) {
    send_account_request_email(action, name, pass, third, NULL);
}

// readyLoad(): krunker_token + krunker_username -> sendAccountRequest(1, [name, null, token])
static void try_restore(void) {
    if (g_restore_sent || !backend_enabled()) return;
    char name[64], token[sizeof(g_token)];
    storage_get("krunker_username", name, sizeof(name));
    storage_get("krunker_token", token, sizeof(token));
    if (!name[0] || !token[0]) return;
    g_restore_sent = 1;
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("a"));
    cJSON_AddItemToArray(frame, cJSON_CreateNumber(1));
    cJSON *payload = cJSON_CreateArray();
    cJSON_AddItemToArray(payload, cJSON_CreateString(name));
    cJSON_AddItemToArray(payload, cJSON_CreateNull());
    cJSON_AddItemToArray(payload, cJSON_CreateString(token));
    cJSON_AddItemToArray(frame, payload);
    backend_social_send(frame);
    cJSON_Delete(frame);
}

static void clear_account(void) {
    g_logged_in = 0;
    store_ui_account_changed();
    clan_ui_reset();
    cJSON_Delete(g_acc.stats);
    cJSON_Delete(g_acc.data);
    cJSON_Delete(g_acc.badges);
    memset(&g_acc, 0, sizeof(g_acc));
    cJSON_Delete(g_chal);
    g_chal = NULL;
    g_chal_requested = 0;
    g_chal_typ[0] = 0;
    cJSON_Delete(g_medal_counts);
    g_medal_counts = NULL;
    battlepass_ui_reset(); // windows[5].data / account.BP
    memset(&g_email, 0, sizeof(g_email));
}

// WP-E part 2c: acc.badges from kn_brain's account info ("badges": badges.js indices)
static void set_badges(const cJSON *info) {
    const cJSON *badges = cJSON_IsObject(info) ? cJSON_GetObjectItem(info, "badges") : NULL;
    if (!cJSON_IsArray(badges)) return;
    cJSON_Delete(g_acc.badges);
    g_acc.badges = cJSON_Duplicate(badges, 1);
}

int account_owns_badge(const int badge) {
    const cJSON *b;
    cJSON_ArrayForEach(b, (g_logged_in ? g_acc.badges : NULL)) {
        if (cJSON_IsNumber(b) && b->valueint == badge) return 1;
    }
    return 0;
}

static int stat_bdg(void) {
    const cJSON *v = g_acc.stats ? cJSON_GetObjectItem(g_acc.stats, "bdg") : NULL;
    return cJSON_IsNumber(v) ? v->valueint : -1;
}

// updateClassPreview: account.stats.bdg >= 0 && Game.badges[bdg] && customBadge && owned
int account_custom_badge(void) {
    if (!g_logged_in || ui_setting_is_false("customBadge")) return -1;
    const int bdg = stat_bdg();
    return badge_get(bdg) && account_owns_badge(bdg) ? bdg : -1;
}

static void add_badge(const int badge) {
    if (account_owns_badge(badge)) return;
    if (!g_acc.badges) g_acc.badges = cJSON_CreateArray();
    cJSON_AddItemToArray(g_acc.badges, cJSON_CreateNumber(badge));
}

// acc.stats[key] = value in both copies (g_acc.stats and data[16])
static void set_stat(const char *key, const double value) {
    if (!g_acc.stats) g_acc.stats = cJSON_CreateObject();
    cJSON_DeleteItemFromObject(g_acc.stats, key);
    cJSON_AddNumberToObject(g_acc.stats, key, value);
    cJSON *stats = g_acc.data ? cJSON_GetArrayItem(g_acc.data, 16) : NULL;
    if (cJSON_IsObject(stats)) {
        cJSON_DeleteItemFromObject(stats, key);
        cJSON_AddNumberToObject(stats, key, value);
    }
}

// accountResponse(html, id, username, data, token)
static void account_response(const cJSON *frame) {
    const cJSON *err = cJSON_GetArrayItem(frame, 1);
    const int was_restore = g_restore_sent;
    g_restore_sent = 0;
    g_auth_sent_at = 0;
    if (cJSON_IsString(err) && err->valuestring[0]) {
        show_acc_resp(err->valuestring);
        if (was_restore && !g_logged_in) {
            // a stale saved token: v3.1.1 keeps the values, but nothing would ever log in
            // with them again; drop the token so the next start does not retry it
            html_storage_set("krunker_token", "");
            g_token[0] = 0;
        }
        return;
    }
    const cJSON *id = cJSON_GetArrayItem(frame, 2);
    const char *username = str_at(frame, 3);
    if (!cJSON_IsNumber(id) || !username) return;

    const int had = g_logged_in;
    clear_account();
    g_logged_in = 1;
    g_acc.id = id->valueint;
    copy_str(g_acc.name, sizeof(g_acc.name), username);
    copy_str(g_acc.alias, sizeof(g_acc.alias), username);
    set_data(cJSON_GetArrayItem(frame, 4));
    const cJSON *info = cJSON_GetArrayItem(frame, 6); // KrunkNative extra: {level, levelProg, alias, mail_unread, ...}
    if (cJSON_IsObject(info)) {
        const cJSON *mail = cJSON_GetObjectItem(info, "mail_unread");
        if (cJSON_IsNumber(mail)) g_acc.mail_unread = mail->valueint;
        set_badges(info);
    }

    char idtext[24];
    snprintf(idtext, sizeof(idtext), "%d", g_acc.id);
    html_storage_set("krunker_id", idtext);
    html_storage_set("krunker_username", username);
    const char *token = str_at(frame, 5);
    if (token && token[0]) {
        copy_str(g_token, sizeof(g_token), token);
        html_storage_set("krunker_token", token);
    }
    apply_menu_name();
    mark_header();
    show_acc_resp(NULL);
    if (!had) printf("[account] logged in as %s (id %d, level %d, %.0f KR)\n", g_acc.name, g_acc.id, g_acc.level, g_acc.funds);
    // updateAccount -> updateWindow(5); updateWindow(null, true): the open window re-renders
    rerender_windows();
    menu_update_class_box(); // dJ()
}

// logoutAcc()
static void logout(void) {
    send_account_request(-1, NULL, NULL, NULL);
    show_acc_resp(NULL);
    html_storage_set("krunker_id", "");
    html_storage_set("krunker_username", "");
    html_storage_set("krunker_token", "");
    g_token[0] = 0;
    g_tab_index = 0;
    g_logout_sessions = 0;
    clear_account();
    apply_menu_name();
    mark_header();
    printf("[account] logged out\n");
    menu_show_window(5, 1); // showWindow(5)
    menu_update_class_box();
}

static void reward_tick(int force);
static void auth_unreachable(void);
static int progress_frame(const char *label, const cJSON *frame);
static int progress_handle(const char *name, const HtmlArg *args, int argc);
static void medal_tab_html(StrBuf *sb);
static void badge_url(int index, char *out, size_t n);
static void mail_count(int count);
static void mail_read_data(const cJSON *rows);
static void twofa_res(const cJSON *frame);
static void email_frame(const char *label, const cJSON *frame);
static int reset_frame(const char *label, const cJSON *frame);

static void on_social(void *user, const cJSON *frame) {
    (void) user;
    const char *label = str_at(frame, 0);
    if (!label) return;
    if (!strcmp(label, "__open")) {
        if (g_logged_in && g_token[0]) {
            // the socket dropped and came back: authenticate this socket again
            g_restore_sent = 0;
            try_restore();
        } else {
            try_restore();
        }
    } else if (!strcmp(label, "a")) {
        account_response(frame);
    } else if ((!strcmp(label, "ua") || !strcmp(label, "upd")) && g_logged_in) { // ua: updateAccount(data) after a match
        set_data(cJSON_GetArrayItem(frame, 1));
        set_badges(cJSON_GetArrayItem(frame, 2)); // WP-E part 2c: the info's "badges"
        mark_header();
        rerender_windows();
    } else if (!strcmp(label, "spin") || !strcmp(label, "spinKR") || !strcmp(label, "spinH")) {
        store_ui_frame(frame);
    } else if (!strcmp(label, "ttl")) { // v10: the account's titles (v10_customize.c)
        v10_customize_frame(frame);
    } else if (!strcmp(label, "dsp") || !strcmp(label, "dslr")) { // v10 Daily Spin progress / claim (v10_daily_spin.c)
        v10_daily_spin_frame(label, frame);
    } else if (battlepass_ui_frame(label, frame)) { // v7.1.4 Battle Pass: bpdl, bppd, bpclmr, purbp, purbpl, bpEnd
    } else if (!strcmp(label, "cln") || !strcmp(label, "clnL")) { // io_cln / io_clnL (windows[12])
        clan_ui_frame(frame);
    } else if (!strcmp(label, "pRes")) { // uploadS reply: popupResponse(kind, error, text)
        const char *kind = str_at(frame, 1);
        popup_response(kind && !strcmp(kind, "SUCCESS") ? 1 : 0, str_at(frame, 2), str_at(frame, 3));
    } else if (!strcmp(label, "loadS")) { // loadSettings(text)
        const char *text = str_at(frame, 1);
        if (text && text[0] && ui_settings_import_text(text)) popup_response(1, NULL, "Settings Loaded");
        else popup_response(0, "No settings found", NULL);
        menu_rerender_if_open(1);
    } else if (!strcmp(label, "econN")) { // KrunkNative economy note (kn_brain economy.c): a toast outside the hub
        const char *text = str_at(frame, 2);
        if (text && app_mode() != APP_MODE_HUB) {
            StrBuf sb = {0};
            sb_append_esc(&sb, text);
            char *html = sb_finish(&sb);
            ui_notify(html);
            free(html);
        }
    } else if (!strcmp(label, "gmc")) { // updateMailCount(count)
        mail_count((int) num_at(frame, 1));
    } else if (!strcmp(label, "kml")) { // mailListResponse(tab, rows)
        g_mail_tab = (int) num_at(frame, 1);
        cJSON_Delete(g_mail_rows);
        const cJSON *rows = cJSON_GetArrayItem(frame, 2);
        g_mail_rows = cJSON_IsArray(rows) ? cJSON_Duplicate(rows, 1) : NULL;
        g_mail_loaded = g_mail_rows != NULL;
        menu_rerender_if_open(30);
    } else if (!strcmp(label, "rml")) { // readMailResponse(rows)
        mail_read_data(cJSON_GetArrayItem(frame, 1));
    } else if (!strcmp(label, "c2fa")) {
        twofa_res(frame);
    } else if (!strcmp(label, "emailS") || !strcmp(label, "cemail") || !strcmp(label, "emailv")) {
        email_frame(label, frame);
    } else if (reset_frame(label, frame)) { // KrunkNative forgot password: rpr / rpc / rps
    } else if (!strcmp(label, "clm")) { // claimRewardResponse(lastReward, funds)
        if (g_logged_in && g_acc.data) {
            cJSON_ReplaceItemInArray(g_acc.data, 11, cJSON_CreateNumber(num_at(frame, 1)));
            g_acc.funds = num_at(frame, 2);
            mark_header();
            reward_tick(1);
        }
    } else if (!strcmp(label, "nmeR")) { // windows[4].aResp(err, alias)
        const char *err = str_at(frame, 1);
        const char *alias = str_at(frame, 2);
        if (err && err[0]) {
            popup_response(0, err, NULL); // io_pRes(null, err)
        } else {
            popup_response(1, NULL, NULL); // io_pRes("SUCCESS")
            if (alias && g_logged_in) copy_str(g_acc.alias, sizeof(g_acc.alias), alias);
            apply_menu_name();
            mark_header();             // cK()
            menu_update_class_box();   // dJ()
            rerender_windows();        // updateWindow(null, true)
        }
    } else if (!strcmp(label, "flgr")) { // io_flgr(flag): acc.stats.flg; updateWindow(5)
        const cJSON *flag = cJSON_GetArrayItem(frame, 1);
        if (g_logged_in && cJSON_IsNumber(flag)) {
            if (!g_acc.stats) g_acc.stats = cJSON_CreateObject();
            cJSON_DeleteItemFromObject(g_acc.stats, "flg");
            cJSON_AddNumberToObject(g_acc.stats, "flg", flag->valueint);
            if (g_acc.data) {
                cJSON *stats = cJSON_GetArrayItem(g_acc.data, 16);
                if (cJSON_IsObject(stats)) {
                    cJSON_DeleteItemFromObject(stats, "flg");
                    cJSON_AddNumberToObject(stats, "flg", flag->valueint);
                }
            }
        }
        menu_rerender_if_open(5);
    } else if (progress_frame(label, frame)) { // WP-E part 2c: badges, challenges, medals
    } else if (!strcmp(label, "upp")) { // changePasswordResponse(msg, newToken)
        const char *token = str_at(frame, 2);
        if (token && token[0]) {
            copy_str(g_token, sizeof(g_token), token);
            html_storage_set("krunker_token", token);
            apply_menu_name();
        }
        HtmlElem res = html_dom_by_id("newPRes");
        if (res) html_dom_set_html(res, str_at(frame, 1) ? str_at(frame, 1) : "");
    } else if (!strcmp(label, "__close")) {
        g_restore_sent = 0;
        if (g_auth_sent_at > 0) auth_unreachable();
    }
}

void account_ui_init(Client *client) {
    g_client = client;
    storage_get("krunker_token", g_token, sizeof(g_token));
    if (!backend_enabled()) return;
    backend_social_listen(on_social, NULL);
    backend_social_open(); // "__open" restores the saved session
}

// The daily free KR (config.rewardTime after acc.lastReward): v7.1.4 shows it only as the
// Store's "Free KR Spin" card (store_ui.c), a countdown until it can be claimed. KrunkNative
// also wants one finished match this session (no ads to watch). ms left, 0 = claimable, -1 = none.
double account_reward_ms(void) {
    if (!g_logged_in || g_acc.level < REWARD_MIN_LVL) return -1.0;
    const double last = g_acc.data ? num_at(g_acc.data, 11) : 0.0;
    const double db = REWARD_TIME_MS - ((double) time(NULL) * 1000.0 - last);
    return db > 0 ? db : 0.0;
}

static void reward_tick(const int force) {
    if (!g_logged_in || account_reward_ms() < 0) return;
    const double ms = account_reward_ms();
    char t[16];
    snprintf(t, sizeof(t), "%d", ms > 0 ? (int) (ms / 1000) : g_reward_played ? -1 : -2);
    if (!force && !strcmp(t, g_reward_text)) return;
    snprintf(g_reward_text, sizeof(g_reward_text), "%s", t);
    menu_rerender_if_open(14); // the Free KR card's countdown
}

// ---- windows[29]: Krunker Mail

static void mail_count(const int count) {
    g_acc.mail_unread = count < 0 ? 0 : count;
    mark_header();
}

// UTILS.getReadableTime2(ms ago)
static void readable_time2(char *out, const size_t n, const double ms) {
    if (ms < 0) {
        snprintf(out, n, "just now");
        return;
    }
    const long long m = (long long) floor(ms / 60000.0), h = m / 60, d = h / 24;
    if (d) snprintf(out, n, "%lldd ago", d);
    else if (h % 24) snprintf(out, n, "%lldh ago", h % 24);
    else if (m % 60) snprintf(out, n, "%lldm ago", m % 60);
    else snprintf(out, n, "just now");
}

// new Date(text): "YYYY-MM-DD[ T]hh:mm:ss[.frac][Z|+hh[:mm]|-hh[:mm]]" (PostgreSQL / ISO);
// without a zone the time is local, like the browser's parser.
static double parse_date_ms(const char *s) {
    int y, mo, d, h = 0, mi = 0, se = 0, used = 0;
    if (!s || sscanf(s, "%d-%d-%d%*c%d:%d:%d%n", &y, &mo, &d, &h, &mi, &se, &used) < 6) return 0;
    const char *p = s + used;
    if (*p == '.') while (*++p >= '0' && *p <= '9') {}
    struct tm tmv = {0};
    tmv.tm_year = y - 1900;
    tmv.tm_mon = mo - 1;
    tmv.tm_mday = d;
    tmv.tm_hour = h;
    tmv.tm_min = mi;
    tmv.tm_sec = se;
    if (*p == 'Z') return (double) timegm(&tmv) * 1000.0;
    if (*p == '+' || *p == '-') {
        const int sign = *p == '-' ? -1 : 1;
        int oh = 0, om = 0;
        if (sscanf(p + 1, "%2d:%2d", &oh, &om) < 2) sscanf(p + 1, "%2d%2d", &oh, &om);
        return ((double) timegm(&tmv) - sign * (oh * 3600.0 + om * 60.0)) * 1000.0;
    }
    tmv.tm_isdst = -1;
    return (double) mktime(&tmv) * 1000.0;
}

static char *gen_mail(void) {
    static const char *const tabs[5] = {"Followers", "Clan", "Hub", "Inbox", "Archive"}; // config.mailTabs
    StrBuf sb = {0};
    sb_append(&sb, "<div>Your Notifications<div class='button buttonR readAll' onclick='windows[29].readAll()'>Read All</div></div><div style='height:30px;'></div><div class='bigMenTabs' style='table-layout: fixed;'>");
    for (int i = 0; i < 5; i++) sb_appendf(&sb, "<div class='bigMenTab%s' onclick='windows[29].switchTab(%d)' style='height:30px;'>%s</div>", g_mail_view == i ? " bigMenTabA" : "", i, tabs[i]);
    sb_append(&sb, "</div><div id='mailList'>");
    if (!g_mail_loaded) {
        char frame[32];
        snprintf(frame, sizeof(frame), "[\"kml\",%d]", g_mail_view == 4 ? 1 : 0);
        backend_social_send_text(frame);
        sb_append(&sb, "Loading...");
    } else if (!g_mail_rows) {
        sb_append(&sb, "Loading...");
    } else {
        int shown = 0;
        const cJSON *r;
        const double now = (double) time(NULL) * 1000.0;
        cJSON_ArrayForEach(r, g_mail_rows) {
            const cJSON *archive = cJSON_GetObjectItem(r, "km_archive");
            const int archived = cJSON_IsTrue(archive) || (cJSON_IsNumber(archive) && archive->valuedouble != 0);
            if (archived != (g_mail_view == 4)) continue;
            // km_notificationtype: 0 follow, 1 clan, 2 hub, 3 inbox (4 / 5 count as 3); none = inbox
            const cJSON *type_v = cJSON_GetObjectItem(r, "km_notificationtype");
            int type = cJSON_IsNumber(type_v) ? type_v->valueint : 3;
            if (type == 4 || type == 5) type = 3;
            if (g_mail_view != 4 && type != g_mail_view) continue;
            shown++;
            const int id = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(r, "km_id"));
            char ago[32];
            readable_time2(ago, sizeof(ago), now - parse_date_ms(cJSON_GetStringValue(cJSON_GetObjectItem(r, "km_datesent"))));
            sb_appendf(&sb, "<div class='mailObj' id='mail_%d' onclick='windows[29].readMail(this, %d, %d)'><div style='position:relative;'><div class='mailDate'>%s</div></div><div class='mailFrom' style='margin-bottom:15px'>",
                       id, id, type, ago);
            const char *from = cJSON_GetStringValue(cJSON_GetObjectItem(r, "km_playerfrom"));
            const char *subject = cJSON_GetStringValue(cJSON_GetObjectItem(r, "km_subject"));
            sb_append_esc(&sb, from ? from : "");
            sb_append(&sb, "</div><span class='mailSubj'>");
            sb_append_esc(&sb, subject ? subject : "");
            sb_append(&sb, "</span><div class='mailText'></div></div>");
        }
        if (!shown) sb_append(&sb, "No messages");
        cJSON_Delete(g_mail_rows); // this.mailData = null: the next render asks again
        g_mail_rows = NULL;
        g_mail_loaded = 0;
    }
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

// onReadData([row]): the message text into the opened row
static void mail_read_data(const cJSON *rows) {
    const cJSON *row = cJSON_GetArrayItem(rows, 0);
    if (!row) return;
    char id[32];
    snprintf(id, sizeof(id), "mail_%d", (int) cJSON_GetNumberValue(cJSON_GetObjectItem(row, "km_id")));
    HtmlElem el = html_dom_by_id(id);
    if (el) {
        html_dom_set_style(el, "border", "");
        html_dom_set_class(el, "mailUnread", 0);
        const HtmlElem text = html_dom_query(el, ".mailText");
        const char *km_text = cJSON_GetStringValue(cJSON_GetObjectItem(row, "km_text"));
        StrBuf sb = {0};
        for (const char *p = km_text ? km_text : ""; *p; p++) { // replace(/<|>/g,"").replace("&","") + ASCII only
            if (*p == '<' || *p == '>' || (unsigned char) *p >= 0x80) continue;
            sb_append_n(&sb, p, 1);
        }
        char *clean = sb_finish(&sb);
        if (text) html_dom_set_text(text, clean[0] ? clean : "No message provided");
        free(clean);
    }
    mail_count(g_acc.mail_unread - 1);
}

// ---- 2FA: v7.1.4 index.js twoFAres / f1789 / onInput2FA / onKeyDown2FA / sub2FA / act2FA / dact2FA

// f1789(config.twoFAKeyLen): one digit input per code character
static char *twofa_inputs(void) {
    StrBuf sb = {0};
    for (int i = 0; i < TWOFA_KEY_LEN; i++)
        sb_appendf(&sb, "<input class='twoFAIn' type='number' min='0' max='9' maxlength='1' placeholder=' ' oninput='onInput2FA(this);' onkeydown='onKeyDown2FA(this);' %s>",
                   i == 0 ? "autofocus" : "");
    return sb_finish(&sb);
}

// QrCode.toDataURL(otpUri): an SVG data URL (the HTML engine rasterises SVG with nanosvg)
static char *qr_data_url(const char *text) {
    static QrCode qr;
    if (qr_encode((const unsigned char *) text, strlen(text), QR_ECC_M, -1, &qr) != 0) return NULL;
    static char svg[48 * 1024];
    const int n = qr_svg(&qr, svg, sizeof(svg));
    if (n <= 0) return NULL;
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t len = (size_t) n, cap = 32 + (len + 2) / 3 * 4;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = (size_t) snprintf(out, cap, "data:image/svg+xml;base64,");
    for (size_t i = 0; i < len; i += 3) {
        const unsigned v = (unsigned char) svg[i] << 16 | (i + 1 < len ? (unsigned char) svg[i + 1] << 8 : 0) |
                           (i + 2 < len ? (unsigned char) svg[i + 2] : 0);
        out[o++] = b64[v >> 18 & 63];
        out[o++] = b64[v >> 12 & 63];
        out[o++] = i + 1 < len ? b64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? b64[v & 63] : '=';
    }
    out[o] = 0;
    return out;
}

// The popup's "Enter Verification Code" line (the second #twoFAmsg of the page)
static HtmlElem twofa_msg_el(void) {
    HtmlElem all[4];
    const int n = html_dom_query_all(0, "#twoFAmsg", all, 4);
    return n > 0 ? all[n - 1] : 0;
}

// twoFAres(status, nextLabel, otpUri, twoFAVal) + KrunkNative's message (6th element)
static void twofa_res(const cJSON *frame) {
    const int status = (int) num_at(frame, 1);
    const char *label = str_at(frame, 2);
    const char *uri = str_at(frame, 3);
    const char *msg = str_at(frame, 5);
    copy_str(g_2fa_label, sizeof(g_2fa_label), label);
    ui_loadout_clear_pops(); // clearPops()
    if (status) {
        if (status == 1) {
            set_display("twoFAPop", "none");
        } else if (status == 2 && g_logged_in) {
            const cJSON *val = cJSON_GetArrayItem(frame, 4);
            g_acc.has_2fa = cJSON_IsTrue(val) || (cJSON_IsNumber(val) && val->valuedouble != 0);
            set_display("twoFAPop", "none"); // KrunkNative: the confirm popup closes too
            menu_rerender_if_open(5);         // updateWindow(5)
        }
        return;
    }
    set_display("twoFAPop", "block");
    HtmlElem btn = html_dom_by_id("twoFAbtn");
    if (btn) {
        html_dom_set_html(btn, "Submit");
        html_dom_set_attr(btn, "class", "twoFAbtn");
    }
    HtmlElem form = html_dom_by_id("twoFAForm");
    char *inputs = twofa_inputs();
    if (form) html_dom_set_html(form, inputs);
    free(inputs);
    HtmlElem first = form ? html_dom_query(form, ".twoFAIn") : 0;
    if (first) html_dom_focus(first);
    HtmlElem qr = html_dom_by_id("twoFAqr");
    if (uri && qr) {
        char *url = qr_data_url(uri);
        HtmlElem img = html_dom_query(qr, "img");
        if (url && img) {
            html_dom_set_attr(img, "src", url);
            html_dom_set_display(qr, "block");
        }
        free(url);
    } else if (qr && (!label || strcmp(label, "a2FAc") != 0)) {
        html_dom_set_display(qr, "none"); // v7 leaves the setup QR up for later prompts; KrunkNative hides it
    }
    HtmlElem m = twofa_msg_el();
    if (m) {
        html_dom_set_text(m, msg && msg[0] ? msg : "Enter Verification Code");
        html_dom_set_style(m, "color", msg && msg[0] ? "#e33b3b" : "");
    }
}

// The code typed so far (only single digits count, like sub2FA)
static int twofa_code(char out[TWOFA_KEY_LEN + 1]) {
    HtmlElem ins[TWOFA_KEY_LEN];
    const int n = html_dom_query_all(0, ".twoFAIn", ins, TWOFA_KEY_LEN);
    int len = 0;
    for (int i = 0; i < n && i < TWOFA_KEY_LEN; i++) {
        char v[16] = "";
        html_dom_get_value(ins[i], v, sizeof(v));
        if (strlen(v) == 1 && isdigit((unsigned char) v[0])) out[len++] = v[0];
    }
    out[len] = 0;
    return len;
}

// sub2FA(): io.send(v10808 || "2fa", code) once every digit is there
static void twofa_submit(void) {
    char code[TWOFA_KEY_LEN + 1];
    if (twofa_code(code) != TWOFA_KEY_LEN) return;
    HtmlElem btn = html_dom_by_id("twoFAbtn");
    if (btn) {
        html_dom_set_html(btn, "...");
        html_dom_set_attr(btn, "class", "twoFAbtnN");
    }
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString(g_2fa_label[0] ? g_2fa_label : "2fa"));
    cJSON_AddItemToArray(frame, cJSON_CreateString(code));
    backend_social_send(frame);
    cJSON_Delete(frame);
}

// previousElementSibling / nextElementSibling among the popup's inputs
static HtmlElem twofa_sibling(const HtmlElem el, const int dir) {
    HtmlElem ins[TWOFA_KEY_LEN];
    const int n = html_dom_query_all(0, ".twoFAIn", ins, TWOFA_KEY_LEN);
    for (int i = 0; i < n; i++)
        if (ins[i] == el) return i + dir >= 0 && i + dir < n ? ins[i + dir] : 0;
    return 0;
}

// onInput2FA(this): one digit per box, the caret moves on; the last box submits
static void twofa_on_input(const HtmlElem el) {
    if (!el) return;
    char v[32] = "";
    html_dom_get_value(el, v, sizeof(v));
    const size_t len = strlen(v);
    if (len == 0) {
        const HtmlElem prev = twofa_sibling(el, -1);
        if (prev) html_dom_focus(prev);
        return;
    }
    if (len > 1) {
        const char last[2] = {v[len - 1], 0};
        html_dom_set_value(el, last);
    }
    const HtmlElem next = twofa_sibling(el, 1);
    if (next) html_dom_focus(next);
    else twofa_submit(); // twoFAbtn.click()
}

// onKeyDown2FA(this): Enter submits a complete code; Backspace clears and steps back
static int twofa_on_keydown(const HtmlElem el, const int key) {
    if (html_key_is_enter(key)) {
        char code[TWOFA_KEY_LEN + 1];
        if (twofa_code(code) >= TWOFA_KEY_LEN) twofa_submit();
        return 0;
    }
    if (key == 259 || key == 8) { // GLFW_KEY_BACKSPACE natively, keyCode 8 on the web
        char v[16] = "";
        if (el) html_dom_get_value(el, v, sizeof(v));
        const HtmlElem prev = el ? twofa_sibling(el, -1) : 0;
        if (!v[0] && prev) {
            html_dom_focus(prev);
            html_dom_set_value(prev, "");
        } else if (el) {
            html_dom_set_value(el, "");
        }
        return 0;
    }
    return 1;
}

// ---- KrunkNative email verification (Account Security)

static void email_res(const char *text, const int error) {
    HtmlElem el = html_dom_by_id("emailRes");
    if (!el) return;
    html_dom_set_text(el, text ? text : "");
    html_dom_set_style(el, "color", error ? "#e33b3b" : "rgba(255,255,255,0.5)");
}

static void email_state(const cJSON *st) {
    if (!cJSON_IsObject(st)) return;
    const char *addr = cJSON_GetStringValue(cJSON_GetObjectItem(st, "email"));
    copy_str(g_email.addr, sizeof(g_email.addr), addr);
    g_email.verified = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(st, "verified"));
    g_email.pending = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(st, "pending"));
    g_email.wait = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(st, "wait"));
    g_email.mail = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(st, "mail"));
    g_email.known = 1;
}

// ["emailS", state] / ["cemail" | "emailv", error, message, state]
static void email_frame(const char *label, const cJSON *frame) {
    if (!g_logged_in) return;
    if (!strcmp(label, "emailS")) {
        email_state(cJSON_GetArrayItem(frame, 1));
        if (g_tab_index == 3) menu_rerender_if_open(5);
        return;
    }
    const char *err = str_at(frame, 1), *msg = str_at(frame, 2);
    email_state(cJSON_GetArrayItem(frame, 3));
    if (g_tab_index == 3) menu_rerender_if_open(5);
    email_res(err ? err : msg, err != NULL);
}

// The Account Security block: the address, its state, change / resend
static void email_html(StrBuf *sb) {
    sb_append(sb, "<div class='settName' style='margin-bottom:20px;'></div><div style='font-size:21px;color:rgba(255,255,255,0.4);margin-top:5px;margin-bottom: 10px;'>");
    if (g_email.addr[0]) {
        sb_append(sb, "Email: <span style='color:#fff'>");
        sb_append_esc(sb, g_email.addr);
        sb_appendf(sb, "</span> <span style='color:%s'>%s</span>", g_email.verified ? "#3be354" : "#e33b3b",
                   g_email.verified ? "Verified" : "Not verified");
        if (!g_email.verified && g_email.mail)
            sb_append(sb, "<div class='subPassChg' onclick='resendEmail()' style='float: right;margin-top: -8px;'>Resend</div>");
    } else {
        sb_append(sb, "Add an <span style='color:#fff'>Email</span> to verify your account");
    }
    sb_append(sb, "</div><input type='email' id='accEmailNew' class='smlInput' style='width:82%;margin-bottom:8px' placeholder='");
    sb_append(sb, g_email.addr[0] ? "New Email" : "Email");
    sb_append(sb, "' /><div class='subPassChg' onclick='changeEmail()'>Submit</div><div id='emailRes' style='color:rgba(255,255,255,0.5)'>");
    if (!g_email.mail && g_email.known) sb_append(sb, "Verification emails are unavailable on this server");
    else if (!g_email.verified && g_email.pending) sb_append(sb, "Check your inbox for the verification link");
    sb_append(sb, "</div>");
}

void account_ui_tick(Client *client) {
    store_ui_tick(); // the wheel screen; also without a brain (a --render-scenario's state.spin)
    if (!backend_enabled()) return;
    // a match ended (Krunker "end": canClaimReward) -> dc
    static int reward_end = -1;
    const int end_now = client->net_end_seconds >= 0;
    if (end_now && reward_end == 0 && g_logged_in) {
        g_reward_played = 1;
        reward_tick(1);
    }
    reward_end = end_now;
    reward_tick(0);
    if (g_auth_sent_at > 0 && glfwGetTime() - g_auth_sent_at > AUTH_TIMEOUT_SECONDS) auth_unreachable();
    if (g_header_dirty) update_header();
    // A round ended: the game server reported the match to the brain. Refresh the
    // account (KR, score, level) with a token login a moment later, like v3.1.1's
    // updateAccount after the end screen (unless the brain pushed "upd" already).
    static int last_end = -1;
    static double refresh_at;
    const int end = client->net_end_seconds >= 0;
    if (end && !last_end && g_logged_in) refresh_at = glfwGetTime() + 3.0;
    last_end = end;
    if (refresh_at > 0 && glfwGetTime() >= refresh_at) {
        refresh_at = 0;
        if (g_logged_in && g_token[0] && backend_social_is_open()) {
            g_restore_sent = 0;
            try_restore();
        }
    }
}

// ------------------------------------------------------------------ handlers

static void read_input(const char *id, char *out, const size_t n) {
    out[0] = 0;
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_get_value(el, out, n);
}

// No account server answering. v3.1.1 only says "Please try again." in #accResp; KrunkNative
// names the reason, so a player can tell offline mode, a down server and no connection apart
// (the details also go to the log).
static void auth_unreachable(void) {
    const char *msg;
    if (!online_mode_online()) {
        msg = "Accounts are off in Offline Mode. Switch Account Mode to Online in Settings &gt; Client and restart.";
        printf("[account] offline mode: no account service\n");
    } else if (!backend_enabled()) {
#ifdef _WIN32
        msg = "Accounts are not available in this build yet. Please try again after the next update.";
#else
        msg = "No account server is set. Check Settings &gt; Client &gt; Matchmaker URL.";
#endif
        printf("[account] no account server configured (--brain / KRUNKNATIVE_BRAIN_URL / knBrainUrl)\n");
    } else if (backend_available() == 0) {
        msg = "Can't reach the account server. Check your internet connection and try again.";
        printf("[account] no account server at %s (GET /mm/status found no kn_brain)\n", backend_url());
    } else if (!backend_social_is_open()) {
        msg = "Can't reach the account server. Check your internet connection and try again.";
        printf("[account] the social socket to %s did not open\n", backend_url());
    } else {
        msg = "The account server did not answer. Please try again.";
        printf("[account] no answer from %s within %.0f s\n", backend_url(), AUTH_TIMEOUT_SECONDS);
    }
    show_acc_resp(msg);
    g_auth_sent_at = 0;
}

// ---- KrunkNative password reset ("Forgot password?" in the guest Account window)
//
// v7.1.4's login form only says "For lost Passwords/Accounts contact recovery@yendis.ch".
// kn_brain mails a one-time code to the account's verified email (services/brain/
// account_security.c): ["rpr", name | email] -> ["rpr", err | null, msg]; ["rpc", code] ->
// ["rpc", err | null, username]; ["rps", code, new, repeat] -> ["rps", err | null, msg,
// username, token]. Step 1 asks for the name or email, step 2 for the code (pasted or typed;
// the mail's link opens the same form on the brain's site instead), step 3 for the new
// password; a success signs in with the returned session, like a login.

static struct {
    int step;          // 0 = the login form, 1 = name / email, 2 = code, 3 = new password
    char code[32];     // the checked code (step 3)
    char user[64];     // its account
    char note[320];    // the line under the fields (escaped HTML)
    int note_error;
} g_reset;

static void reset_note(const char *text, const int error) {
    HtmlElem el = html_dom_by_id("knResetRes");
    copy_str(g_reset.note, sizeof(g_reset.note), text);
    g_reset.note_error = error;
    if (!el) return;
    html_dom_set_text(el, text ? text : "");
    html_dom_set_style(el, "color", error ? "#e33b3b" : "rgba(255,255,255,0.5)");
}

// The reset steps over the Account window's guest form (window 5's #menuWindow content)
static void reset_render(void) {
    HtmlElem win = html_dom_by_id("menuWindow");
    if (!win || g_reset.step <= 0) return;
    StrBuf sb = {0};
    sb_append(&sb, "<div style='position:relative;z-index:9'><div id='referralHeader'>Reset Password</div><div style='height:20px;'></div>");
    if (g_reset.step == 1) {
        sb_append(&sb, "<div style='font-size:18px;color:rgba(255,255,255,0.5);margin-bottom:12px'>Enter your username or the verified "
                       "email of your account. We'll email you a reset code.</div>"
                       "<input id='knResetWho' type='text' placeholder='Username or Email' class='accountInput' style='margin-top:0'>");
    } else if (g_reset.step == 2) {
        sb_append(&sb, "<div style='font-size:18px;color:rgba(255,255,255,0.5);margin-bottom:12px'>Enter the code from the email "
                       "(XXXX-XXXX-XXXX). It works once and expires after 30 minutes.</div>"
                       "<input id='knResetCode' type='text' placeholder='Reset Code' class='accountInput' style='margin-top:0' "
                       "autocomplete='one-time-code'>");
    } else {
        sb_append(&sb, "<div style='font-size:18px;color:rgba(255,255,255,0.5);margin-bottom:12px'>Choose a new password for <span style='color:#fff'>");
        sb_append_esc(&sb, g_reset.user);
        sb_append(&sb, "</span>.</div><input id='knResetPass' type='password' placeholder='New Password' class='accountInput' style='margin-top:0'>"
                       "<input id='knResetPass2' type='password' placeholder='Repeat Password' class='accountInput'>");
    }
    sb_appendf(&sb, "<div class='setBodH' style='margin-left:0px;width:calc(100%% - 40px)'><div id='knResetRes' style='margin-top:20px;"
                    "margin-bottom:20px;font-size:18px;color:%s;text-align:center'>",
               g_reset.note_error ? "#e33b3b" : "rgba(255,255,255,0.5)");
    sb_append_esc(&sb, g_reset.note);
    sb_append(&sb, "</div></div><div style='width:100%;text-align:center;margin-top:10px;background-color:rgba(0,0,0,0.3);padding-top:10px;padding-bottom:20px;'>"
                   "<div class='accBtn button buttonG' onclick='knResetBack()'>Back</div>");
    sb_appendf(&sb, "<div class='accBtn button buttonP' id='knResetGo' onclick='knResetNext()'>%s</div>",
               g_reset.step == 1 ? "Send Code" : g_reset.step == 2 ? "Continue" : "Set Password");
    if (g_reset.step == 1) sb_append(&sb, "<div style='margin-top:14px;font-size:16px;color:rgba(255,255,255,0.5)'>Got a code already? "
                                         "<span id='knResetHave' onclick='knResetHave()' style='color:rgba(255,255,255,0.8);cursor:pointer;"
                                         "text-decoration:underline'>Enter it</span></div>");
    if (g_reset.step == 2) sb_append(&sb, "<div style='margin-top:14px;font-size:16px;color:rgba(255,255,255,0.5)'>No email? "
                                         "<span id='knResetResend' onclick='knResetResend()' style='color:rgba(255,255,255,0.8);cursor:pointer;text-decoration:underline'>Send another code</span></div>");
    sb_append(&sb, "</div></div>");
    html_dom_set_html(win, sb.data ? sb.data : "");
    free(sb.data);
    const char *focus = g_reset.step == 1 ? "knResetWho" : g_reset.step == 2 ? "knResetCode" : "knResetPass";
    HtmlElem f = html_dom_by_id(focus);
    if (f) html_dom_focus(f);
}

static void reset_open(const int step, const char *note, const int error) {
    g_reset.step = step;
    copy_str(g_reset.note, sizeof(g_reset.note), note);
    g_reset.note_error = error;
    if (step <= 0) {
        memset(&g_reset, 0, sizeof(g_reset));
        menu_rerender_if_open(5); // the login form again
        return;
    }
    reset_render();
}

static void reset_send(const char *label, const char *a, const char *b, const char *c) {
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString(label));
    cJSON_AddItemToArray(frame, cJSON_CreateString(a ? a : ""));
    if (b) cJSON_AddItemToArray(frame, cJSON_CreateString(b));
    if (c) cJSON_AddItemToArray(frame, cJSON_CreateString(c));
    backend_social_send(frame);
    cJSON_Delete(frame);
}

static char g_reset_who[256]; // step 1's input, for "Send another code"

static void reset_next(void) {
    if (!backend_enabled() || backend_available() == 0) {
        auth_unreachable();
        HtmlElem res = html_dom_by_id("accResp");
        char text[320] = "";
        if (res) html_dom_get_text(res, text, sizeof(text));
        reset_note(text[0] ? text : "Please try again.", 1);
        return;
    }
    if (g_reset.step == 1) {
        read_input("knResetWho", g_reset_who, sizeof(g_reset_who));
        if (!g_reset_who[0]) {
            reset_note("Enter your username or email", 1);
            return;
        }
        reset_note(ACC_STR_WAIT, 0);
        reset_send("rpr", g_reset_who, NULL, NULL);
    } else if (g_reset.step == 2) {
        char code[64];
        read_input("knResetCode", code, sizeof(code));
        if (!code[0]) {
            reset_note("Enter the code from the email", 1);
            return;
        }
        copy_str(g_reset.code, sizeof(g_reset.code), code);
        reset_note(ACC_STR_WAIT, 0);
        reset_send("rpc", code, NULL, NULL);
    } else if (g_reset.step == 3) {
        char p1[128], p2[128];
        read_input("knResetPass", p1, sizeof(p1));
        read_input("knResetPass2", p2, sizeof(p2));
        if (!p1[0] || strcmp(p1, p2) != 0) {
            reset_note(p1[0] ? "Passwords do not match." : "Enter a new password", 1);
            return;
        }
        reset_note(ACC_STR_WAIT, 0);
        reset_send("rps", g_reset.code, p1, p2);
        memset(p1, 0, sizeof(p1));
        memset(p2, 0, sizeof(p2));
    }
}

// ["rpr" | "rpc" | "rps", error | null, message, ...]
static int reset_frame(const char *label, const cJSON *frame) {
    const int rpr = !strcmp(label, "rpr"), rpc = !strcmp(label, "rpc"), rps = !strcmp(label, "rps");
    if (!rpr && !rpc && !rps) return 0;
    if (g_reset.step <= 0) return 1;
    const char *err = str_at(frame, 1), *msg = str_at(frame, 2);
    if (err) {
        reset_note(err, 1);
        return 1;
    }
    if (rpr) {
        reset_open(2, msg ? msg : "Check your inbox.", 0);
    } else if (rpc) {
        copy_str(g_reset.user, sizeof(g_reset.user), msg);
        reset_open(3, "", 0);
    } else {
        // signed in with the new session: the same restore as a saved login
        const char *user = str_at(frame, 3), *token = str_at(frame, 4);
        memset(&g_reset, 0, sizeof(g_reset));
        if (user && user[0] && token && token[0]) {
            html_storage_set("krunker_username", user);
            html_storage_set("krunker_token", token);
            copy_str(g_token, sizeof(g_token), token);
            g_restore_sent = 0;
            try_restore();
        }
        menu_rerender_if_open(5);
        show_acc_resp(msg ? msg : "Password updated.");
        ui_notify(msg ? "Password updated. You are now logged in." : "Password updated.");
    }
    return 1;
}

int account_ui_reset_enter(const HtmlElem target) {
    if (g_reset.step <= 0 || !target) return 0;
    static const char *const ids[] = {"knResetWho", "knResetCode", "knResetPass", "knResetPass2"};
    for (size_t i = 0; i < sizeof(ids) / sizeof(ids[0]); i++) {
        if (target == html_dom_by_id(ids[i])) {
            if (target == html_dom_by_id("knResetPass")) {
                HtmlElem next = html_dom_by_id("knResetPass2");
                if (next) html_dom_focus(next);
            } else {
                reset_next();
            }
            return 1;
        }
    }
    return 0;
}

// knResetOpen / knResetNext / knResetBack / knResetHave / knResetResend (onclick names C writes)
static int reset_handle(const char *name) {
    if (!strcmp(name, "knResetOpen")) {
        char who[64];
        read_input("accName", who, sizeof(who));
        reset_open(1, "", 0);
        HtmlElem in = html_dom_by_id("knResetWho");
        if (in && who[0]) html_dom_set_value(in, who);
        return 1;
    }
    if (!strcmp(name, "knResetNext")) {
        reset_next();
        return 1;
    }
    if (!strcmp(name, "knResetBack")) {
        reset_open(g_reset.step >= 2 ? g_reset.step - 1 : 0, "", 0);
        return 1;
    }
    if (!strcmp(name, "knResetHave")) { // a code from an earlier request (the game was restarted meanwhile)
        reset_open(2, "", 0);
        return 1;
    }
    if (!strcmp(name, "knResetResend")) {
        if (!g_reset_who[0]) {
            reset_open(1, "", 0);
            return 1;
        }
        reset_note(ACC_STR_WAIT, 0);
        reset_send("rpr", g_reset_who, NULL, NULL);
        return 1;
    }
    return 0;
}

int account_ui_twofa_handler(const char *name) {
    return name && (!strcmp(name, "sub2FA") || !strcmp(name, "onInput2FA") || !strcmp(name, "onKeyDown2FA"));
}

int account_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    // No account service, or the probe found none: say why at once. A socket that is still
    // connecting (the first seconds after start, a slow TLS handshake) is not a failure: the
    // request is queued until it opens (backend_social_send) and AUTH_TIMEOUT_SECONDS reports
    // a socket that never does.
    if (name && (!strcmp(name, "registerAcc") || !strcmp(name, "loginAcc")) && (!backend_enabled() || backend_available() == 0)) {
        if (backend_enabled()) backend_probe_start(); // maybe the backend came up since: ask again
        auth_unreachable();
        return 1;
    }
    if (name && !g_logged_in && reset_handle(name)) return 1; // KrunkNative forgot password (works without a brain: says why)
    if (!name || (!backend_enabled() && !g_scenario)) return -1; // g_scenario: --render-scenario's stand-in account
    if (battlepass_ui_handle(name, ev, args, argc) >= 0) return 1; // before the Store: creatorCodeAction(.., 3)
    if (store_ui_handle(name, ev, args, argc) >= 0) return 1;
    if (clan_ui_handle(name, ev, args, argc) >= 0) return 1;
    if (ranked_ui_handle(name, ev, args, argc) >= 0) return 1;
    if (turf_ui_handle(name, ev, args, argc) >= 0) return 1;
    if (!strcmp(name, "registerAcc") || !strcmp(name, "loginAcc")) {
        char user[64], pass[128];
        read_input("accName", user, sizeof(user));
        read_input("accPass", pass, sizeof(pass));
        char mail[256] = "";
        read_input("accEmail", mail, sizeof(mail)); // KrunkNative: optional, register only
        if (!strcmp(name, "registerAcc")) send_account_request_email(0, user, pass, NULL, mail); // [name, pass, referralCode, email]
        else send_account_request(1, user, pass, NULL);
        g_auth_sent_at = glfwGetTime();
        return 1;
    }
    if (!strcmp(name, "logoutAcc")) {
        if (g_logged_in) logout();
        return 1;
    }
    if (!strcmp(name, "logoutSessions")) { // io.send("las")
        g_logout_sessions = 1;
        if (ev && ev->target) html_dom_set_display(ev->target, "none");
        backend_social_send_text("[\"las\"]");
        return 1;
    }
    if (progress_handle(name, args, argc) >= 0) return 1; // WP-E part 2c: badges, windows[32]
    if (!strcmp(name, "windows.4.switchStat")) { // statTabB<n> shown, statTab<n> active
        const int t = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        for (int i = 0; i < 5; i++) { // + KrunkNative's Medals tab (WP-E part 2c)
            char sel[24], id[16];
            snprintf(sel, sizeof(sel), ".statTabB%d", i);
            snprintf(id, sizeof(id), "statTab%d", i);
            HtmlElem b = html_dom_query(0, sel), tab = html_dom_by_id(id);
            if (b) html_dom_set_class(b, "hide", i != t);
            if (tab) html_dom_set_class(tab, "bigMenTabA", i == t);
        }
        return 1;
    }
    if (!strcmp(name, "windows.4.switchTab")) {
        g_tab_index = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        menu_show_window(5, 1); // updateWindow(5)
        return 1;
    }
    if (!strcmp(name, "claimReward") && g_logged_in) {
        // v7.1.4 plays an ad, then opens the Free KR wheel (prizeWheel(4)) whose spin asks the
        // brain (["fkr"]); KrunkNative has no ads: the wheel opens once the timer ran out and a
        // match was played this session
        const double last = g_acc.data ? num_at(g_acc.data, 11) : 0.0;
        if (g_reward_played && REWARD_TIME_MS - ((double) time(NULL) * 1000.0 - last) <= 0) {
            ui_loadout_clear_pops();
            store_ui_free_kr_wheel();
        }
        return 1;
    }
    if (!strcmp(name, "windows.29.switchTab")) {
        const int tab = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        const int force = argc > 1;
        if (g_mail_view != tab || force) {
            g_mail_view = tab >= 0 && tab < 5 ? tab : 3;
            g_mail_tab = g_mail_view == 4;
            cJSON_Delete(g_mail_rows);
            g_mail_rows = NULL;
            g_mail_loaded = 0;
            menu_show_window(30, 1);
        }
        return 1;
    }
    if (!strcmp(name, "windows.29.readMail")) { // toggle the text; "Loading..." + io.send("rml", id)
        HtmlElem obj = ev ? ev->target : 0;
        const HtmlElem text = obj ? html_dom_query(obj, ".mailText") : 0;
        if (!text) return 1;
        char cur[16] = "";
        html_dom_get_text(text, cur, sizeof(cur));
        if (cur[0]) {
            html_dom_set_html(text, "");
        } else {
            html_dom_set_html(text, "Loading...");
            char frame[48];
            snprintf(frame, sizeof(frame), "[\"rml\",%d]", argc > 1 && args[1].kind == HTML_ARG_NUM ? (int) args[1].num : 0);
            backend_social_send_text(frame);
        }
        return 1;
    }
    if (!strcmp(name, "windows.29.readAll")) {
        backend_social_send_text("[\"raml\"]");
        g_mail_tab = 1;
        g_mail_view = 4;
        cJSON_Delete(g_mail_rows);
        g_mail_rows = NULL;
        g_mail_loaded = 0;
        mail_count(0);
        menu_show_window(30, 1);
        return 1;
    }
    if (!strcmp(name, "onInput2FA")) {
        twofa_on_input(ev ? ev->target : 0);
        return 1;
    }
    if (!strcmp(name, "onKeyDown2FA")) return twofa_on_keydown(ev ? ev->target : 0, ev ? ev->key : 0);
    if (!strcmp(name, "sub2FA")) {
        twofa_submit();
        return 1;
    }
    if (!strcmp(name, "act2FA") && g_logged_in) { // io.send("a2FA")
        backend_social_send_text("[\"a2FA\"]");
        return 1;
    }
    if (!strcmp(name, "dact2FA") && g_logged_in) { // io.send("d2FA")
        backend_social_send_text("[\"d2FA\"]");
        return 1;
    }
    if (!strcmp(name, "changeEmail") && g_logged_in) { // KrunkNative: ["cemail", email]
        char addr[256];
        read_input("accEmailNew", addr, sizeof(addr));
        if (!addr[0]) {
            email_res("Enter an Email", 1);
            return 1;
        }
        email_res(ACC_STR_WAIT, 0);
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("cemail"));
        cJSON_AddItemToArray(frame, cJSON_CreateString(addr));
        backend_social_send(frame);
        cJSON_Delete(frame);
        return 1;
    }
    if (!strcmp(name, "resendEmail") && g_logged_in) { // KrunkNative: ["emailv"]
        email_res(ACC_STR_WAIT, 0);
        backend_social_send_text("[\"emailv\"]");
        return 1;
    }
    if (!strcmp(name, "uploadSettings") || !strcmp(name, "fetchSettings")) {
        if (!g_logged_in) {
            popup_response(0, "Must be logged in", NULL);
            return 1;
        }
        if (!strcmp(name, "fetchSettings")) {
            backend_social_send_text("[\"getS\"]");
            return 1;
        }
        popup_response(2, NULL, "Loading..."); // dP("Loading...")
        char *text = ui_settings_export_text();
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("uploadS"));
        cJSON_AddItemToArray(frame, cJSON_CreateString(text ? text : "{}"));
        backend_social_send(frame);
        cJSON_Delete(frame);
        free(text);
        return 1;
    }
    if (!strcmp(name, "redeemVoucher")) { // io.send("vr", code)
        char code[64];
        read_input("redVouch", code, sizeof(code));
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("vr"));
        cJSON_AddItemToArray(frame, cJSON_CreateString(code));
        backend_social_send(frame);
        cJSON_Delete(frame);
        return 1;
    }
    if (!strcmp(name, "selectFlag")) { // if (acc) io.send("flg", parseInt(value))
        const char *v = argc > 0 && args[0].kind == HTML_ARG_STR ? args[0].str : NULL;
        const int flag = v ? atoi(v) : argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
        if (g_logged_in) {
            char frame[48];
            snprintf(frame, sizeof(frame), "[\"flg\",%d]", flag);
            backend_social_send_text(frame);
        }
        return 1;
    }
    if (!strcmp(name, "changePass")) { // io.send("upp", old, new, repeat)
        char old_pass[128], new_pass[128], rep[128];
        HtmlElem res = html_dom_by_id("newPRes");
        if (res) html_dom_set_html(res, "Please wait...");
        read_input("oldPass", old_pass, sizeof(old_pass));
        read_input("newPass", new_pass, sizeof(new_pass));
        read_input("repNPass", rep, sizeof(rep));
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("upp"));
        cJSON_AddItemToArray(frame, cJSON_CreateString(old_pass));
        cJSON_AddItemToArray(frame, cJSON_CreateString(new_pass));
        cJSON_AddItemToArray(frame, cJSON_CreateString(rep));
        backend_social_send(frame);
        cJSON_Delete(frame);
        return 1;
    }
    if (!strcmp(name, "changeName")) { // dP("Please Wait..."); io.send("nme", displayN.value)
        char alias[64];
        popup_response(2, NULL, "Please Wait...");
        read_input("displayN", alias, sizeof(alias));
        if (!g_logged_in || !g_acc.premium || !alias[0]) {
            popup_response(0, "Invalid name", NULL);
            return 1;
        }
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("nme"));
        cJSON_AddItemToArray(frame, cJSON_CreateString(alias));
        backend_social_send(frame);
        cJSON_Delete(frame);
        return 1;
    }
    return -1;
}

// ------------------------------------------------------------------ windows

static void stat_row(StrBuf *sb, const char *label, const char *value) {
    sb_append(sb, "<div class='settName'>");
    sb_append(sb, label);
    sb_append(sb, "<span class='floatR'>");
    sb_append(sb, value);
    sb_append(sb, "</span></div>");
}

static void stat_row_num(StrBuf *sb, const char *label, const double v) {
    char text[32];
    snprintf(text, sizeof(text), "%.0f", v);
    stat_row(sb, label, text);
}

static void stat_row_fixed(StrBuf *sb, const char *label, const double v) {
    char text[32];
    snprintf(text, sizeof(text), "%.2f", v);
    stat_row(sb, label, text);
}

// UTILS.xpBarText(acc)
static void xp_bar_text(char *out, const size_t n) {
    const double m = g_acc.level > 1 ? pow(g_acc.level / RANK_VAR, 2) : 0.0;
    snprintf(out, n, "%d / %d", (int) (g_acc.score - m), (int) (pow((g_acc.level + 1) / RANK_VAR, 2) - m));
}

// UTILS.commaFloat(v.toFixed(decimals)): thousands separators on the integer part, the
// toFixed fraction kept as is (v7 passes the toFixed string: SPK "12,500,000.00", KDR "0.0000")
static void comma_float(char *out, const size_t n, const double v, const int decimals) {
    char fixed[64], whole[48];
    snprintf(fixed, sizeof(fixed), "%.*f", decimals > 0 ? decimals : 0, v);
    char *dot = strchr(fixed, '.');
    if (dot) *dot = 0;
    comma_number(whole, sizeof(whole), strtod(fixed, NULL));
    if (fixed[0] == '-' && whole[0] != '-') { // -0.5 -> "-0"
        memmove(whole + 1, whole, strlen(whole) + 1);
        whole[0] = '-';
    }
    snprintf(out, n, "%s%s%s", whole, dot ? "." : "", dot ? dot + 1 : "");
}

static void pst(StrBuf *sb, const char *label, const char *value) {
    sb_appendf(sb, "<div class='pSt'>%s<strong>%s</strong></div>", label, value);
}

static void pst_num(StrBuf *sb, const char *label, const double v, const int decimals) {
    char t[48];
    comma_float(t, sizeof(t), v, decimals);
    pst(sb, label, t);
}

// windows[4].gen() for a signed-in account (the guest form is ui/windows/5.html): the
// profile tabs of v7.1.4 (home = Your Account + the four stat tabs, Friends, Partner,
// Settings, Connections, promo codes) and the logout links.
static char *gen_account(void) {
    static const char *const tabs[] = {"<span class='material-icons' style='color:#fbc02d;font-size:42px;vertical-align:bottom'>home</span>", "Friends", "Partner",
                                       "Settings", "Connections", "<span class='material-icons' style='color:#96ff50;font-size:42px;vertical-align:bottom'>vpn_key</span>"}; // config.profileTabs
    // config.statTabs + KrunkNative's Medals (the account's medal counts, kn_brain "mdl", WP-E part 2c)
    static const char *const stat_tabs[] = {"General Stats", "Killstreak Stats", "Other Stats", "Class Stats", "Medals"};
    StrBuf sb = {0};
    char a[64];
    sb_append(&sb, "<div style='height:15px;'></div><div class='bigMenTabs'>");
    for (int i = 0; i < 6; i++) {
        if (i == 4) continue; // window.canConnectExternalAccounts (Twitch / Metamask): not in KrunkNative
        sb_appendf(&sb, "<div class='bigMenTab%s' style='font-size:16px' onmouseenter='playTick(0.1);' onclick='playSelect();windows[4].switchTab(%d)'>%s</div>", g_tab_index == i ? " bigMenTabA" : "", i, tabs[i]);
    }
    sb_append(&sb, "</div>");
    if (g_tab_index == 0) {
        sb_append(&sb, "<div class='setHed' style='margin-top:10px;margin-bottom:20px;'>Your Account</div>");
        sb_append(&sb, "<div id='profilePicM' class='blurred'><img style='width: inherit;height: inherit;border-radius: inherit;' loading='lazy' src='textures/classes/icon_0.png'><a class='pfpAction' onmouseenter='playTick(0.1)' onclick='pfpPopup();'>Change Picture</a></div>");
        sb_append(&sb, "<div class='setBodH' style='display:inline-block;padding-top:10px;vertical-align:top;width:1004px;margin-left:15px;margin-top:0px;'><div class='settName' style='width:100%;margin-top:0px'>Name<a href='/social.html?p=profile&q=");
        sb_append_esc(&sb, g_acc.name);
        sb_append(&sb, "' class='floatR' target='_blank'>");
        sb_append_esc(&sb, g_acc.name);
        sb_append(&sb, "</a></div><div class='settName' style='width:100%'>Clan<a style='float:right'  onclick='showWindow(13)'>");
        if (g_acc.clan[0]) {
            sb_append(&sb, "[");
            sb_append_esc(&sb, g_acc.clan);
            sb_append(&sb, "]");
        } else {
            sb_append(&sb, "None");
        }
        sb_appendf(&sb, "</a></div><div class='settName'>Level<span class='floatR' style='color:rgba(255,255,255,0.8)'>%d</span></div></div>", g_acc.level);
        sb_append(&sb, "<div class='setHed' style='margin-top:10px;margin-bottom:15px;'>Your Stats</div>");
        xp_bar_text(a, sizeof(a));
        sb_appendf(&sb, "<div class='xpBar' style='height:31px;line-height:34px;font-size:28px;'><div class='xpBarB' style='width:%.6g%%'></div><span class='xpBarV'>%s</span></div>", g_acc.level_prog, a);
        sb_append(&sb, "<div id='statHolder'><div style='height:15px;'></div><div class='bigMenTabs' style='margin-bottom: 5px;table-layout: fixed;'>");
        for (int i = 0; i < 5; i++) {
            sb_appendf(&sb, "<div id='statTab%d' class='bigMenTab%s' onclick='playSelect(0.1);windows[4].switchStat(%d)' style='padding-left: 5px; padding-right: 5px; padding-top: 10px; padding-bottom: 5px; line-height: 35px;font-size: 16px;'onmouseenter='playTick();'>%s</div>",
                       i, i == 0 ? " bigMenTabA" : "", i, stat_tabs[i]);
        }
        sb_append(&sb, "</div><div class='statTabB0'>");
        const double losses = g_acc.games - g_acc.wins > 0 ? g_acc.games - g_acc.wins : 0;
        const double shots = stat_num("s"), hits = fmin(stat_num("s"), stat_num("h"));
        snprintf(a, sizeof(a), "%d", g_acc.level);
        pst(&sb, "LVL", a);
        pst_num(&sb, "KR", g_acc.funds, 0);
        pst_num(&sb, "Junk", 0, 0);
        pst(&sb, "Inventory", " ~0");
        pst_num(&sb, "Score", g_acc.score, 0);
        pst_num(&sb, "SPK", g_acc.score / fmax(g_acc.kills, 1), 2);
        pst_num(&sb, "Kills", g_acc.kills, 0);
        pst_num(&sb, "Deaths", g_acc.deaths, 0);
        pst_num(&sb, "KDR", g_acc.kills / fmax(g_acc.deaths, 1), 4);
        pst_num(&sb, "KPG", g_acc.kills / fmax(g_acc.games, 1), 4);
        pst_num(&sb, "Games", g_acc.games, 0);
        snprintf(a, sizeof(a), "%g", round(g_acc.wins / fmax(losses, 1) * 1000) / 1000);
        pst(&sb, "W/L", a);
        pst_num(&sb, "Wins", g_acc.wins, 0);
        pst_num(&sb, "Losses", losses, 0);
        pst_num(&sb, "Assists", stat_num("ast"), 0);
        pst_num(&sb, "Melee", stat_num("mk"), 0);
        pst_num(&sb, "Beatdowns", stat_num("fk"), 0);
        pst_num(&sb, "Bullseyes", stat_num("tmk"), 0);
        pst_num(&sb, "Headshots", stat_num("hs"), 0);
        pst_num(&sb, "Legshots", stat_num("ls"), 0);
        pst_num(&sb, "Wallbangs", stat_num("wb"), 0);
        pst_num(&sb, "Shots", shots, 0);
        pst_num(&sb, "Hits", hits, 0);
        pst_num(&sb, "Misses", shots - hits, 0);
        snprintf(a, sizeof(a), "%g%%", shots > 0 ? round(hits / shots * 100 * 100) / 100 : 0.0);
        pst(&sb, "Accuracy", a);
        readable_time(a, sizeof(a), g_acc.time_played);
        pst(&sb, "Time Played", a);
        sb_append(&sb, "</div><div class='statTabB1 hide'>");
        pst_num(&sb, "Nukes", stat_num("n"), 0);
        pst_num(&sb, "Airdrops", stat_num("ad"), 0);
        pst_num(&sb, "Airdrops Stolen", stat_num("sad"), 0);
        pst_num(&sb, "Slimers", stat_num("sl"), 0);
        pst_num(&sb, "Juggernauts", stat_num("jg"), 0);
        sb_appendf(&sb, "<div class='pSt' style='font-size: 12px;padding-left: 0px;padding-right: 0px;width: 174px;'>Juggernauts Killed<strong>%.0f</strong></div>", stat_num("kjg"));
        pst_num(&sb, "War Machines", stat_num("wm"), 0);
        sb_append(&sb, "</div><div class='statTabB2 hide'>");
        pst_num(&sb, "Crouches", stat_num("crc"), 0);
        pst_num(&sb, "Sprays Placed", stat_num("spry"), 0);
        pst(&sb, "Helpful Reports", "0/0");
        sb_append(&sb, "</div><div class='statTabB3 hide'>");
        const MenuUi *ui = menu_ui();
        for (int i = 0; i < ui->class_count; i++) {
            const MenuClass *c = &ui->classes[i];
            char key[16];
            snprintf(key, sizeof(key), "c%d", c->index);
            const double xp = stat_num(key), lv = RANK_VAR * sqrt(xp);
            int level = (int) floor(lv);
            const int pct = (int) lround((lv - level) * 100.0);
            const double lo = level > 1 ? pow(level / RANK_VAR, 2) : 0.0, hi = pow((level + 1) / RANK_VAR, 2);
            level = level + 1 > 1 ? level + 1 : 1;
            sb_appendf(&sb, "<div class='clsStatCard'><img class='clsStatImg' src='%s'/><div class='clsStatName'>", c->icon);
            sb_append_esc(&sb, c->name ? c->name : "");
            sb_append(&sb, "</div><div class='clsStatWeap'>");
            sb_append_esc(&sb, c->weapon ? c->weapon : "");
            sb_appendf(&sb, "</div><div class='clsStatXPBar'><div class='clsStatXPBarC' style='width:%d%%'></div><div class='clsStatXPBarV'>%d / %d</div></div><div class='clsStatLvl'>Lvl %d</div></div>",
                       pct, (int) (xp - lo), (int) (hi - lo), level);
        }
        sb_append(&sb, "</div><div class='statTabB4 hide'>");
        medal_tab_html(&sb);
        if (!g_medal_counts) backend_social_send_text("[\"mdl\"]");
        sb_append(&sb, "</div></div>");
    } else if (g_tab_index == 1) {
        // v10's friends tab: mutual follows from the friends event bus (v10_social.c)
        sb_append(&sb, "<div class='setBodH' style='padding-top:10px;padding-bottom:10px;margin-bottom:20px;'>Friends can send private messages and see eachothers online status.<div style='height:10px;'></div>You can add new friends by following eachother.</div>");
        char *friends = v10_social_window_friends_html();
        sb_append(&sb, friends ? friends : "<div id='friendList'>Loading Friends...</div>");
        free(friends);
    } else if (g_tab_index == 2) {
        sb_append(&sb, "<div class='setHed' style='margin-top:10px;'>Krunker Partner Benefits</div><div class='setBodH'><div class='settName' style='font-size:17px;'>You get <span style='color:#fff !important'>20-40% of Revenue</span> from KR Sales on your Partner Code in the Shop</div><div class='settName' style='font-size:17px;'>You <span style='color:#fff !important;'>earn money</span> when someone Plays your Custom Games/Maps</div><div class='settName' style='font-size:17px;'>Every time a referred player plays the game, you get a <span style='color:#fff !important;'>cut of the Ad Revenue</span></div></div></div><div class='setHed' style='padding-bottom:10px'>Partner Application</div><div class='setBodH' style='padding-top:10px;padding-bottom:10px;'><div style='font-size:19px;color:rgba(255,255,255,0.4);margin-top:5px;margin-bottom:10px;'>By submitting an Application you agree to the <a href='https://frvr.com/legal/krunker#TermsofService'>Partner Terms</a></div><input type='text' id='crtrCode' class='smlInput' style='width:100%;margin-bottom:8px' placeholder='Partner Code Name' /><input type='text' id='crtrEmail' class='smlInput' style='width:86%;margin-bottom:8px' placeholder='PayPal Email for Payouts' /><a class='menuLink' style='margin-left:20px;vertical-align:middle' onclick='submitCrtrCod()'>Submit</a></div><div id='ctrtRes' style='color:rgba(255,255,255,0.5)'></div></div>");
    } else if (g_tab_index == 3) {
        sb_append(&sb, "<div class='setHedS' style='margin-top:10px;'>Account Settings</div>");
        if (g_acc.premium) {
            sb_append(&sb, "<div class='setBodH'><div class='settName'>Display Name<div class='updDisN' onclick='clearName();' style='background-color: var(--red);'>Clear</div><div class='updDisN' onclick='changeName();'>Update</div><input type='search' autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false' id='displayN' class='smlInput' placeholder='");
            sb_append_esc(&sb, g_acc.alias);
            sb_append(&sb, "'><div id='aliasRes'></div></div>");
        }
        const int flg = g_acc.stats && cJSON_IsNumber(cJSON_GetObjectItem(g_acc.stats, "flg")) ? (int) stat_num("flg") : -1;
        sb_appendf(&sb, "<div class='settName'>Country Flag<select onchange='selectFlag(this.value)' class='inputGrey2' style='padding:4px;margin-top:4px'><option value='-1' %s>None</option>", flg == -1 ? "selected" : "");
        for (int i = 0; i < ACCOUNT_FLAG_COUNT; i++) sb_appendf(&sb, "<option value='%d' %s>%s</option>", i, flg == i ? "selected" : "", ACCOUNT_FLAGS[i].name);
        sb_append(&sb, "</select></div>");
        sb_append(&sb, "<div class='settName'>Custom Badge<div class='updDisN' onclick='selectBadge(-1);' style='background-color: var(--red);'>Clear</div><div class='updDisN' onclick='selectBadgePop();'>Select</div>");
        if (badge_get(stat_bdg())) { // v7 index.js:5651: the selected badge's image
            char url[256];
            badge_url(stat_bdg(), url, sizeof(url));
            sb_appendf(&sb, "<img draggable='false' src='%s' class='customBadge'>", url);
        }
        sb_append(&sb, "</div>");
        sb_appendf(&sb, "<div class='settName'>Hide Detailed Online Status<label class='switch' style='margin-left:10px'><input type='checkbox' onclick='toggleStatus(this.checked)'%s><span class='slider'></span></label></div></div>", stat_num("hds") ? " checked" : "");
        if (!g_acc.hack) {
            sb_append(&sb, "<div class='setHedS'>Account Security</div><div class='setBodH' style='padding-bottom:10px;'><div style='font-size:21px;color:rgba(255,255,255,0.4);margin-top:5px'>You can update your password every 30 minutes. <span style='color:#fff'>Never share your password</span> with anyone and keep it in a secure place</div><input type='password' id='oldPass' class='smlInput' style='width:100%;margin-bottom:8px' placeholder='Current Password' /><input type='password' id='newPass' class='smlInput' style='width:100%;margin-bottom:8px' placeholder='New Password' /><input type='password' id='repNPass' class='smlInput' style='width:82%;margin-bottom:8px' placeholder='Repeat Password' /><div class='subPassChg' onclick='changePass()'>Submit</div><div id='newPRes' style='color:rgba(255,255,255,0.5)'></div>");
            // 2FA: v7 offers it from level 20 (or dev / partner / featured); KrunkNative always does
            if (backend_enabled() || g_scenario) email_html(&sb); // KrunkNative email verification (v7.1.4 has none)
            sb_appendf(&sb, "<div class='settName' style='margin-bottom:20px;'></div><div style='font-size:21px;color:rgba(255,255,255,0.4);margin-top:5px;margin-bottom: 10px;'>To further protect your account, you can add enable <span style='color:#fff'>2FA</span>:<div class='subPassChg' onclick='%s' style='float: right;margin-top: -8px;'>%s</div></div></div>",
                       g_acc.has_2fa ? "dact2FA()" : "act2FA()", g_acc.has_2fa ? "Disable" : "Enable");
        }
        sb_append(&sb, "<div class='setHedS'>Account Privacy</div><div class='setBodH' style='padding-bottom:15px;'><div style='font-size:21px;color:rgba(255,255,255,0.4);margin-top:5px'>You may request to delete your account, if the request is not cancelled within 14 days the account and all associated data will be permanently removed.</div><div style='height:10px;'></div><div style='margin-top:10px'>Delete Account <a class='menuLink' onclick='requestDeletion(1)' style='float:right;font-size:20px'>Request Deletion</a></div></div>");
    } else {
        sb_append(&sb, "<div style='margin-bottom:10px;color:rgba(255,255,255,0.5);'>You have No Promo Codes</div>");
    }
    sb_appendf(&sb, "<div style='height:15px;margin-top:30px;border-top:4px solid rgba(255, 255, 255, 0.8);'></div><a class='menuLink' onclick='logoutAcc()'>Logout</a>%s",
               g_logout_sessions ? "" : "<a style='float:right' class='menuLink' onclick='logoutSessions(this)'>Logout of all Sessions</a>");
    return sb_finish(&sb);
}

// renderRequirement(level, text)
static char *requirement(const int level, const char *text) {
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='reqW'><i class='material-icons reqWIcon'>clear</i><div class='reqWLvl'>LEVEL %d REQUIRED</div><div class='reqWTxt'>%s</div></div>", level, text);
    return sb_finish(&sb);
}

// ------------------------------------------------------------------ WP-E part 2c: badges

// genericPopup(html, cls): #genericPop over #popupBack
static void generic_popup(const char *html, const char *cls) {
    ui_loadout_clear_pops();
    set_display("popupHolder", "block");
    HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", cls);
    html_dom_set_attr(pop, "style", "");
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

static void badge_url(const int index, char *out, const size_t n) {
    char tex[96];
    html_dom_asset_url(badge_texture(index, tex, sizeof(tex)), out, n); // Game.badges[i].url
}

static int badge_claimable(const int index) {
    const BadgeDef *b = badge_get(index);
    return b && b->claimable && !account_owns_badge(index) && badge_requirement_met(index, g_acc.stats);
}

// window.selectBadgePop(): the Badge Collection (claimable first, then owned, then the rest;
// v7 sorts twice with a stable sort, so each group keeps the badges.js order)
static void select_badge_pop(void) {
    if (!g_logged_in) return;
    int claimable = 0, owned = 0;
    for (int i = 0; i < BADGE_COUNT; i++) {
        claimable += badge_claimable(i);
        owned += account_owns_badge(i);
    }
    if (!owned && claimable <= 0) {
        popup_response(0, "You dont own any badges", NULL); // io_showPopup(null, ...)
        return;
    }
    StrBuf sb = {0};
    sb_append(&sb, "<div class='imageListHeader'>Badge Collection</div><div id='skinList' style='margin-top:18px;text-align: center;'>");
    for (int pass = 0; pass < 3; pass++) {
        for (int i = 0; i < BADGE_COUNT; i++) {
            const int own = account_owns_badge(i), claim = badge_claimable(i);
            if ((pass == 0 && !claim) || (pass == 1 && (claim || !own)) || (pass == 2 && (claim || own))) continue;
            const BadgeDef *b = badge_get(i);
            char url[256];
            badge_url(i, url, sizeof(url));
            sb_append(&sb, "<div class='badgeCard'");
            if (own) sb_appendf(&sb, " onclick='selectBadge(%d)'", i);
            else if (claim) sb_appendf(&sb, " onclick='claimBadge(%d)' style='outline: 2px inset #FBC02D;'", i);
            sb_appendf(&sb, "><img class='badgeImg' src='%s'><div class='badgeName'>", url);
            sb_append_esc(&sb, b->name && b->name[0] ? b->name : "Unknown");
            sb_append(&sb, "</div><div class='badgeDesc'>");
            sb_append_esc(&sb, b->desc && b->desc[0] ? b->desc : "No Description");
            sb_append(&sb, "</div>");
            if (claim) sb_append(&sb, "<div class='badgeBound'><span class='material-icons' style='font-size:30px;color:#FBC02D;'>lock_open</span></div>");
            else if (!own) sb_append(&sb, "<div class='badgeBound'><span class='material-icons' style='font-size:30px;color:#fff;'>lock</span></div>");
            sb_append(&sb, "</div>");
        }
    }
    sb_append(&sb, "</div>");
    char *html = sb_finish(&sb);
    generic_popup(html, "badgePop");
    free(html);
}

// ------------------------------------------------------------------ WP-E part 2c: challenges

static const char *weapon_name_at(const int index) {
    return index >= 0 && index < WEAPON_COUNT && rules_weapon(index) ? rules_weapon(index)->name : NULL;
}

// challenges.updateNames(): N or i18n chall.name.<ky>
static void chal_name(const cJSON *c, char *out, const size_t n) {
    challenge_name(c, weapon_name_at, out, n);
    if (!out[0]) { // i18n.t of a missing key prints the key
        const cJSON *dat = cJSON_GetObjectItem(c, "DAT");
        const char *ky = cJSON_GetStringValue(cJSON_GetObjectItem(dat, "ky"));
        snprintf(out, n, "chall.name.%s", ky ? ky : "");
    }
}

static double cnum(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsBool(v) ? cJSON_IsTrue(v) : 0.0;
}

static int chas(const cJSON *o, const char *k) {
    const cJSON *v = o ? cJSON_GetObjectItem(o, k) : NULL;
    return cJSON_IsNumber(v) || cJSON_IsBool(v);
}

static const char *ctype(const cJSON *c) {
    const char *t = cJSON_GetStringValue(cJSON_GetObjectItem(c, "T"));
    return t ? t : "";
}

static void cap_first(StrBuf *sb, const char *s) { // UTILS.capFirst
    if (!s || !s[0]) return;
    char first[2] = {(char) toupper((unsigned char) s[0]), 0};
    sb_append_esc(sb, first);
    sb_append_esc(sb, s + 1);
}

// windows[32].genChallengeCard(challenge, marginTop)
static void chal_card(StrBuf *sb, const cJSON *c, const int margin) {
    const cJSON *dat = cJSON_GetObjectItem(c, "DAT"), *rew = cJSON_GetObjectItem(c, "REW");
    const int boss = cnum(dat, "boss") != 0.0;
    const int done = cnum(c, "PT") != 0.0;
    const char *ky = cJSON_GetStringValue(cJSON_GetObjectItem(dat, "ky"));
    const int time_key = ky && !strcmp(ky, "time");
    const double val = cnum(dat, "val"), pv = cnum(c, "PV");
    // REW.sk: up to 3 reward skins, shown last to first
    int skins[3] = {-1, -1, -1}, skin_count = 0;
    const cJSON *sk = cJSON_GetObjectItem(rew, "sk");
    if (cJSON_IsNumber(sk)) skins[skin_count++] = sk->valueint;
    const cJSON *it;
    cJSON_ArrayForEach(it, (cJSON_IsArray(sk) ? sk : NULL)) if (skin_count < 3 && cJSON_IsNumber(it)) skins[skin_count++] = it->valueint;
    char icon[256], path[64], name[160], text[64];
    snprintf(path, sizeof(path), "img/chals/icon_%d.png", (int) cnum(c, "IC"));
    ui_img_url(path, icon, sizeof(icon)); // UTILS.assetsUrl: KrunkNative's ui/ copy (fetch_season_images.sh)
    chal_name(c, name, sizeof(name));
    sb_appendf(sb, "<div class='chalCard' style='margin-top:%dpx;%s'><img class='chalImgC' src='%s'  style='%s%s' />", margin,
               boss ? "box-shadow: 0 0 14px #ffa000;background:rgb(79 50 0 / 40%);border:4px solid #ff9c00;" : "", icon,
               boss ? "border: 4px solid #ff9c00;" : "", done ? "filter:grayscale(1);brightness(0.8);" : "");
    if (done) sb_append(sb, "<div class='challComplB'><span class='material-icons' style='font-size:110px;color:#38cd3a;'>done</span></div>");
    sb_append(sb, "<div class='chalRight'>");
    sb_append_esc(sb, name);
    if (cnum(c, "TR") > 0) {
        readable_time(text, sizeof(text), cnum(c, "TR"));
        sb_appendf(sb, "<div class='chalExpr'>%s left</div>", text);
    }
    if (done) snprintf(text, sizeof(text), "Completed");
    else if (time_key) readable_time(text, sizeof(text), val);
    else snprintf(text, sizeof(text), "%.15g/%.15g", pv, val);
    sb_appendf(sb, "<div class='chalXPBar'><div class='chalXPVal'>%s</div><div class='chalXPBarC' style='width:%.15g%%;'></div></div></div>", text,
               done ? 100.0 : time_key || val == 0.0 ? 0.0 : pv / val * 100.0);
    const double kr = cnum(rew, "kr"), jnk = cnum(rew, "jnk");
    sb_appendf(sb, "<div class='chalRews' style='%s'><div class='chalRewV' style='%s'><span style='color:white'>+</span>%.15g <span style='color:#fbc02d'>KR</span></div>",
               boss ? "border-left: 3px solid #ff9c00;" : "", kr ? "" : "filter:grayscale(1);", kr);
    sb_appendf(sb, "<div class='chalRewV' style='%s'><span style='color:white'>+</span>%.15g <span class='material-icons' style='color:#cc51f8;font-size:29px;vertical-align:top;'>plumbing</span></div><div>",
               jnk ? "" : "filter:grayscale(1);", jnk);
    for (int i = 0; i < 3; i++) {
        const int index = skins[2 - i];
        const ViewerSkin *skin = index >= 0 ? viewer_skin(index) : NULL;
        sb_append(sb, "<div class='chalRewSkn' style='");
        if (skin) {
            char url[256];
            store_skin_preview(index, url, sizeof(url));
            const int r = skin->rarity >= 0 && skin->rarity < g_viewer_rarity_count ? skin->rarity : 0;
            sb_appendf(sb, "background-image:url(%s);border:2px solid %s;' onclick='viewItem(%d)'>", url, g_viewer_rarities[r].color, index);
        } else {
            sb_append(sb, "' >");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div></div>");
}

// (boss ? 2 : PT) descending, stable (v7's Array.sort)
static int chal_sorted(const cJSON **out, const int max, const char *type, const char *field, const int value) {
    int n = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, g_chal) {
        if (strcmp(ctype(c), type) != 0 || n >= max) continue;
        if (field && (!chas(cJSON_GetObjectItem(c, "DAT"), field) || (int) cnum(cJSON_GetObjectItem(c, "DAT"), field) != value)) continue;
        out[n++] = c;
    }
    for (int i = 1; i < n; i++) {
        const cJSON *x = out[i];
        const double kx = cnum(cJSON_GetObjectItem(x, "DAT"), "boss") ? 2.0 : cnum(x, "PT");
        int j = i - 1;
        while (j >= 0 && (cnum(cJSON_GetObjectItem(out[j], "DAT"), "boss") ? 2.0 : cnum(out[j], "PT")) < kx) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = x;
    }
    return n;
}

static void chal_header(StrBuf *sb, const char *image, const int margin, const char *title, const int done, const int total, const int back,
                        const char *extra_style) {
    char url[256], path[96];
    snprintf(path, sizeof(path), "img/chals/%s.png", image);
    ui_img_url(path, url, sizeof(url));
    sb_appendf(sb, "<div class='challHeadrI' style='background-image:url(%s);margin-top:%dpx;%s'>", url, margin, extra_style ? extra_style : "");
    if (back) sb_append(sb, "<div class='challHeadrBck' onclick='windows[32].closeChallengeCat();'><span class='material-icons challBackArr'>arrow_back</span></div>");
    sb_append(sb, "<div class='challHeadrT'>");
    sb_append_esc(sb, title);
    sb_appendf(sb, "</div><div class='challHeadrS'>%d<span style='color:#fff;'>/%d</span></div></div>", done, total);
}

#define CHAL_MSG "<div class='challCrdMsg' style='margin-bottom:25px;font-size:27px;color:#9f9f9f;'>Abandoning a match will result in the loss of challenge progress.</div>"
#define CHAL_LIST_MAX 256

// The cards of one list (the v7 category views and KrunkNative's daily / weekly sections).
static void chal_cards(StrBuf *sb, const char *type, const char *field, const int value, const char *empty) {
    const cJSON *list[CHAL_LIST_MAX];
    const int n = chal_sorted(list, CHAL_LIST_MAX, type, field, value);
    sb_append(sb, "<div style='margin-top:10px;'>");
    for (int i = 0; i < n; i++) chal_card(sb, list[i], i ? 20 : 0);
    if (!n && empty) sb_appendf(sb, "<div class='challCrdMsg'>%s</div>", empty);
    sb_append(sb, "</div>");
}

static void chal_count(const char *type, const char *field, const int value, int *done, int *total) {
    *done = *total = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, g_chal) {
        if (strcmp(ctype(c), type) != 0) continue;
        const cJSON *dat = cJSON_GetObjectItem(c, "DAT");
        if (field && (!chas(dat, field) || (int) cnum(dat, field) != value)) continue;
        (*total)++;
        *done += cnum(c, "PT") != 0.0;
    }
}

// windows[32].gen() (v7.1.4 index.js:8742) for an account; guests keep the static LOGIN REQUIRED
// page. config.minChalLevel is 1 in v7 (30 in v3.1.1), so every account gets the list. The
// daily challenges (and KrunkNative's weekly ones) are in the Battle Pass window's Challenges
// tab, as in v7 (battlepass_ui.c). v7's Event Challenges section needs an active
// config.events entry (UTILS.hasActiveEvents); v7.1.4's config.events is empty, so it never
// shows and is not ported.
static char *gen_challenges(void) {
    if (g_acc.level < 1) return requirement(1, "for Challenges");
    if (!g_chal) {
        if (!g_chal_requested) {
            g_chal_requested = 1;
            backend_social_send_text("[\"chg\"]");
        }
        return strdup("Loading");
    }
    if (cJSON_GetArraySize(g_chal) == 0) return strdup("<span style='color: #fff;'>No active challenges.</span>");
    StrBuf sb = {0};
    int done, total;
    if (!g_chal_typ[0]) {
        const int margin = 0;
        // Map Challenges: one card per map that has some
        chal_count("map", NULL, 0, &done, &total);
        chal_header(&sb, "hed_1", margin, "Map Challenges", done, total, 0, NULL);
        sb_append(&sb, "<div style='margin-top:10px;text-align:center;'>");
        for (int i = 0; i < MAP_CATALOG_OFFICIAL_COUNT; i++) {
            const MapCatalogEntry *m = map_catalog_at((size_t) i);
            int d, t;
            chal_count("map", "mp", i, &d, &t);
            if (!m || !t) continue;
            char path[64], url[256];
            ui_img_url(map_catalog_thumb(m, path, sizeof(path)), url, sizeof(url));
            sb_appendf(&sb, "<div class='challMapCrd' style='background-image:url(%s)' onmouseenter='playTick()' onclick='windows[32].openChallengeCat(&quot;map&quot;,%d);'><div class='challMapCvr'></div><div class='challMapName blackShad'>", url, i);
            cap_first(&sb, m->name);
            sb_appendf(&sb, "</div><div class='challMapPrg'>%d<span style='color:#fff;'>/%d</span></div></div>", d, t);
        }
        sb_append(&sb, "</div>");
        chal_count("weapon", NULL, 0, &done, &total);
        chal_header(&sb, "hed_2", 30, "Weapon Challenges", done, total, 0, NULL);
        sb_append(&sb, "<div style='margin-top:10px;text-align:center;'>");
        for (int i = 0; i < WEAPON_COUNT; i++) {
            const Weapon *w = rules_weapon(i);
            int d, t;
            chal_count("weapon", "wep", i, &d, &t);
            if (!w || w->type == 3 || w->custom_only || !t) continue; // weaponTypes.EQUIPMENT, customOnly
            char path[96], url[256];
            snprintf(path, sizeof(path), "img/chals/%s.png", w->src ? w->src : "noimg");
            ui_img_url(path, url, sizeof(url));
            sb_appendf(&sb, "<div class='challMapCrd' onmouseenter='playTick()' onclick='windows[32].openChallengeCat(&quot;weapon&quot;,%d);'><img class='challRotImg' style='background-image:url(%s);background-size:contain;background-repeat:no-repeat;' /><div class='challMapCvr'></div><div class='challMapName blackShad'>", i, url);
            cap_first(&sb, w->name);
            sb_appendf(&sb, "</div><div class='challMapPrg'>%d<span style='color:#fff;'>/%d</span></div></div>", d, t);
        }
        sb_append(&sb, "</div>");
        chal_count("nightmare", NULL, 0, &done, &total);
        chal_header(&sb, "hed_4", 30, "Nightmare Challenges", done, total, 0, NULL);
        chal_cards(&sb, "nightmare", NULL, 0, "Check back later for new nightmare challanges!"); // sic (v7)
    } else if (!strcmp(g_chal_typ, "map")) {
        const MapCatalogEntry *m = g_chal_ind >= 0 && g_chal_ind < MAP_CATALOG_OFFICIAL_COUNT ? map_catalog_at((size_t) g_chal_ind) : NULL;
        char title[96];
        snprintf(title, sizeof(title), "%s Challenges", m ? m->name : "");
        chal_count("map", "mp", g_chal_ind, &done, &total);
        chal_header(&sb, "hed_1", 0, title, done, total, 1, NULL);
        sb_append(&sb, CHAL_MSG);
        chal_cards(&sb, "map", "mp", g_chal_ind, "Check back later for new challenges!");
    } else if (!strcmp(g_chal_typ, "weapon")) {
        const char *wname = weapon_name_at(g_chal_ind);
        const Weapon *w = g_chal_ind >= 0 && g_chal_ind < WEAPON_COUNT ? rules_weapon(g_chal_ind) : NULL;
        char title[96];
        snprintf(title, sizeof(title), "%s Challenges", wname ? wname : "");
        chal_count("weapon", "wep", g_chal_ind, &done, &total);
        chal_header(&sb, w && w->src ? w->src : "noimg", 0, title, done, total, 1, "filter: saturate(0.8);");
        sb_append(&sb, CHAL_MSG);
        chal_cards(&sb, "weapon", "wep", g_chal_ind, "Check back later for new challenges!");
    }
    return sb_finish(&sb);
}

const cJSON *account_challenges(void) { return g_logged_in ? g_chal : NULL; }

void account_challenges_request(void) {
    if (!g_logged_in || g_chal || g_chal_requested) return;
    g_chal_requested = 1;
    backend_social_send_text("[\"chg\"]");
}

void account_challenge_name(const cJSON *c, char *out, const size_t n) { chal_name(c, out, n); }

static cJSON *chal_by_ci(const double ci) {
    cJSON *c;
    cJSON_ArrayForEach(c, g_chal) if (cnum(c, "CI") == ci) return c;
    return NULL;
}

static void chal_set(cJSON *c, const char *key, const double v) {
    if (cJSON_GetObjectItem(c, key)) cJSON_ReplaceItemInObject(c, key, cJSON_CreateNumber(v));
    else cJSON_AddNumberToObject(c, key, v);
}

// ------------------------------------------------------------------ WP-E part 2c: medal counts

// KrunkNative: the account's medal counts (kn_brain "mdl") as the profile's fifth stat tab
static void medal_tab_html(StrBuf *sb) {
    if (!g_medal_counts) {
        sb_append(sb, "<div class='pSt'>Medals<strong>...</strong></div>");
        return;
    }
    int any = 0;
    for (int i = 0; i < PROGRESSION_MEDAL_COUNT; i++) {
        const cJSON *v = cJSON_GetObjectItem(g_medal_counts, g_progression_medals[i]);
        if (!cJSON_IsNumber(v) || v->valuedouble <= 0) continue;
        pst_num(sb, g_progression_medals[i], v->valuedouble, 0);
        any = 1;
    }
    if (!any) sb_append(sb, "<div style='color:rgba(255,255,255,0.5);font-size:20px;padding:10px;'>No medals yet</div>");
}

static void medal_tab_update(void) {
    HtmlElem tab = html_dom_query(0, ".statTabB4");
    if (!tab) return;
    StrBuf sb = {0};
    medal_tab_html(&sb);
    char *html = sb_finish(&sb);
    html_dom_set_html(tab, html);
    free(html);
}

// ------------------------------------------------------------------ WP-E part 2c: frames

// kn_brain's badge / challenge / medal frames (doc/backend.md 7.4); 1 = handled.
static int progress_frame(const char *label, const cJSON *frame) {
    const cJSON *a1 = cJSON_GetArrayItem(frame, 1);
    if (!strcmp(label, "bdgr")) { // io_badgeUpdate(bdg)
        if (g_logged_in && cJSON_IsNumber(a1)) set_stat("bdg", a1->valueint);
        ui_loadout_clear_pops(); // clearPops()
        menu_rerender_if_open(5);
        menu_update_class_box();
    } else if (!strcmp(label, "cbdgr")) { // io_badgeClaimResponse(badge)
        if (g_logged_in && cJSON_IsNumber(a1)) add_badge(a1->valueint);
        popup_response(1, NULL, "Badge Claimed!");
    } else if (!strcmp(label, "nbdgr")) { // io_badgeClaimMultiRes(badges): checkBadges at the match end
        const cJSON *b;
        cJSON_ArrayForEach(b, (g_logged_in ? a1 : NULL)) if (cJSON_IsNumber(b)) add_badge(b->valueint);
        popup_response(1, NULL, "You unlocked new badges");
    } else if (!strcmp(label, "chgL")) { // the "chg" list: account.activeChallenges
        g_chal_requested = 0;
        if (!g_logged_in || !cJSON_IsArray(a1)) return 1;
        cJSON_Delete(g_chal);
        g_chal = cJSON_Duplicate(a1, 1);
        menu_rerender_if_open(33);
        battlepass_ui_challenges_changed(); // windows[5] tab 1: the daily (and weekly) cards
    } else if (!strcmp(label, "chg")) { // io_updateChallenges([done, CI, PV, ...])
        for (int i = 0; g_chal && i + 2 < cJSON_GetArraySize(a1); i += 3) {
            cJSON *c = chal_by_ci(cJSON_GetNumberValue(cJSON_GetArrayItem(a1, i + 1)));
            if (!c) continue;
            chal_set(c, "PV", cJSON_GetNumberValue(cJSON_GetArrayItem(a1, i + 2)));
            chal_set(c, "PT", cJSON_GetNumberValue(cJSON_GetArrayItem(a1, i)));
        }
        menu_rerender_if_open(33); // if (!inGame) updateWindow(33)
        battlepass_ui_challenges_changed();
    } else if (!strcmp(label, "chgC")) { // io_challengeCompleted: the end screen's "Completed!" cards
        const cJSON *c;
        cJSON_ArrayForEach(c, a1) {
            char name[160];
            chal_name(c, name, sizeof(name));
            hud_ui_end_challenge_completed((int) cnum(c, "IC"), name);
            const cJSON *sk = cJSON_GetObjectItem(cJSON_GetObjectItem(c, "REW"), "sk"), *it; // matchRewardSummary.skins
            if (cJSON_IsNumber(sk)) hud_ui_add_reward_skin(sk->valueint);
            cJSON_ArrayForEach(it, (cJSON_IsArray(sk) ? sk : NULL)) if (cJSON_IsNumber(it)) hud_ui_add_reward_skin(it->valueint);
            cJSON *mine = chal_by_ci(cnum(c, "CI"));
            if (mine) chal_set(mine, "PT", 1);
        }
    } else if (!strcmp(label, "chlR")) { // io_updateChallengeRewards(skins, funds, junk, jnk, kr)
        if (!g_logged_in) return 1;
        const cJSON *funds = cJSON_GetArrayItem(frame, 2), *it;
        if (cJSON_IsNumber(funds)) account_set_funds(funds->valuedouble);
        cJSON_ArrayForEach(it, (cJSON_IsArray(a1) ? a1 : NULL)) if (cJSON_IsNumber(it)) account_add_skin(it->valueint);
    } else if (!strcmp(label, "mdl")) {
        if (!g_logged_in || !cJSON_IsObject(a1)) return 1;
        cJSON_Delete(g_medal_counts);
        g_medal_counts = cJSON_Duplicate(a1, 1);
        medal_tab_update();
    } else {
        return 0;
    }
    return 1;
}

// windows[32] and the badge handlers; -1 = not ours
static int progress_handle(const char *name, const HtmlArg *args, const int argc) {
    const int a0 = argc > 0 && (args[0].kind == HTML_ARG_NUM || args[0].kind == HTML_ARG_BOOL) ? (int) args[0].num
                 : argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? atoi(args[0].str) : -1;
    char frame[48];
    if (!strcmp(name, "selectBadge")) { // if (account) io.send("bdg", parseInt(i))
        if (g_logged_in) {
            snprintf(frame, sizeof(frame), "[\"bdg\",%d]", a0);
            backend_social_send_text(frame);
        }
    } else if (!strcmp(name, "claimBadge")) { // io.send("cbdg", parseInt(i))
        if (g_logged_in) {
            snprintf(frame, sizeof(frame), "[\"cbdg\",%d]", a0);
            backend_social_send_text(frame);
        }
    } else if (!strcmp(name, "selectBadgePop")) {
        select_badge_pop();
    } else if (!strcmp(name, "windows.32.openChallengeCat")) { // (typ, ind)
        const char *typ = argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? args[0].str : "";
        snprintf(g_chal_typ, sizeof(g_chal_typ), "%s", typ);
        g_chal_ind = argc > 1 && args[1].kind == HTML_ARG_NUM ? (int) args[1].num : 0;
        menu_show_window(33, 1); // menuWindow.scrollTop = 0; updateWindow(33)
    } else if (!strcmp(name, "windows.32.closeChallengeCat")) {
        g_chal_typ[0] = 0;
        menu_show_window(33, 1);
    } else if (!strcmp(name, "windows.32.openHome")) {
        g_chal_typ[0] = 0;
        menu_show_window(33, 0); // showWindow(33)
    } else {
        return -1;
    }
    return 1;
}

// client_bin --render-scenario (ui_scenario.c): state.account = {name, score, funds, clan, premium,
// badges [], stats {}, challenges [], medals {}} stands in for a logged-in account, so the
// account windows (Challenges, Account) can be rendered without kn_brain (WP-E part 2c).
void account_ui_scenario(const cJSON *account) {
    if (!cJSON_IsObject(account)) return;
    clear_account();
    g_logged_in = 1;
    g_scenario = 1;
    g_acc.id = 1;
    const char *name = cJSON_GetStringValue(cJSON_GetObjectItem(account, "name"));
    copy_str(g_acc.name, sizeof(g_acc.name), name ? name : "Player");
    copy_str(g_acc.alias, sizeof(g_acc.alias), g_acc.name);
    const char *clan = cJSON_GetStringValue(cJSON_GetObjectItem(account, "clan"));
    copy_str(g_acc.clan, sizeof(g_acc.clan), clan);
    g_acc.score = cnum(account, "score");
    g_acc.funds = cnum(account, "funds");
    g_acc.premium = cnum(account, "premium") != 0.0;
    update_level();
    const cJSON *stats = cJSON_GetObjectItem(account, "stats");
    g_acc.stats = cJSON_IsObject(stats) ? cJSON_Duplicate(stats, 1) : cJSON_CreateObject();
    set_badges(account);
    const cJSON *chal = cJSON_GetObjectItem(account, "challenges"), *medals = cJSON_GetObjectItem(account, "medals");
    g_chal = cJSON_IsArray(chal) ? cJSON_Duplicate(chal, 1) : NULL;
    g_medal_counts = cJSON_IsObject(medals) ? cJSON_Duplicate(medals, 1) : NULL;
    battlepass_ui_scenario(cJSON_GetObjectItem(account, "battlepass")); // {data, progress, tab, end, now}
    const cJSON *skins = cJSON_GetObjectItem(account, "skins");
    if (cJSON_IsArray(skins)) { // the inventory [{ind, cnt}] (data slot 10; funds 5, lastReward 11)
        g_acc.data = cJSON_CreateArray();
        for (int i = 0; i < 49; i++) cJSON_AddItemToArray(g_acc.data, cJSON_CreateNull());
        cJSON_ReplaceItemInArray(g_acc.data, 5, cJSON_CreateNumber(g_acc.funds));
        cJSON_ReplaceItemInArray(g_acc.data, 10, cJSON_Duplicate(skins, 1));
        cJSON_ReplaceItemInArray(g_acc.data, 11, cJSON_CreateNumber(0));
    }
    email_state(cJSON_GetObjectItem(account, "email")); // {email, verified, pending, mail}
    g_acc.has_2fa = cnum(account, "twofa") != 0.0;
    g_tab_index = (int) cnum(account, "tab"); // windows[4].tabIndex
    const cJSON *c2fa = cJSON_GetObjectItem(account, "c2fa"); // ["c2fa", 0, "a2FAc", "otpauth://..."]
    cJSON_Delete(g_scenario_c2fa);
    g_scenario_c2fa = cJSON_IsArray(c2fa) ? cJSON_Duplicate(c2fa, 1) : NULL;
    mark_header();
    menu_update_class_box();
}

void account_ui_scenario_post(void) {
    if (!g_scenario_c2fa) return;
    twofa_res(g_scenario_c2fa);
    cJSON_Delete(g_scenario_c2fa);
    g_scenario_c2fa = NULL;
}

char *account_window_gen(const int id, const char **header) {
    (void) header;
    if ((backend_available() != 1 && !g_scenario) || !g_logged_in) return NULL; // the guest captures (LOGIN REQUIRED cards, login form)
    if (id == 5) return gen_account();
    if (id == 33) return gen_challenges();
    if (id == 30) return gen_mail();
    return NULL;
}

void account_window_post(const int id) {
    if (id != 5) return;
    if (g_logged_in && (backend_available() == 1 || g_scenario)) {
        // windows[4].gen(): this.width = account ? 1280 : 955. The captured style is the guest's
        // 955 px, too narrow for the signed-in profile (its Name / Clan / Level box is 1004 px
        // beside the 181 px picture: it wrapped under it and its right column was clipped).
        const HtmlElem mw = html_dom_by_id("menuWindow");
        if (mw) html_dom_set_style(mw, "width", "1280px");
        return;
    }
    if (backend_available() != 1 || g_logged_in) return;
    // v3.1.1 prefills the login form with getSavedVal("krunker_username")
    char name[64];
    storage_get("krunker_username", name, sizeof(name));
    HtmlElem input = html_dom_by_id("accName");
    if (input) html_dom_set_value(input, name);
    // KrunkNative: an optional email for Register (kn_brain mails a verification link)
    HtmlElem pass = html_dom_by_id("accPass");
    if (pass && !html_dom_by_id("accEmail"))
        html_dom_insert_html(pass, HTML_INSERT_AFTER_END,
                             "<input id='accEmail' type='email' placeholder='Enter Email (optional, Register only)' class='accountInput'>");
    // KrunkNative: password reset by email (v7.1.4: "contact recovery@yendis.ch")
    HtmlElem mail = html_dom_by_id("accEmail");
    if (mail && !html_dom_by_id("knForgot"))
        html_dom_insert_html(mail, HTML_INSERT_AFTER_END,
                             "<div id='knForgot' style='text-align:right;margin-top:6px;font-size:17px;color:rgba(255,255,255,0.5)'>"
                             "<span onclick='knResetOpen()' style='cursor:pointer;color:rgba(255,255,255,0.8);text-decoration:underline'>"
                             "Forgot password?</span></div>");
    HtmlElem resp = html_dom_by_id("accResp");
    if (resp) html_dom_set_html(resp, "Forgot your password? Reset it with the email on your account.");
    if (g_reset.step > 0) reset_render(); // a re-render (account update) keeps the reset step
}
