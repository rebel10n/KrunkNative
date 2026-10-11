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
#include <backend.h>
#include <backend_ui.h>
#include <app_mode.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "account_flags.h"

#define RANK_VAR 0.03            // config.rankVar
#define ACC_STR_WAIT "Please wait..." // generic.wait
#define REWARD_MIN_LVL 5             // config.rewardMinLvl
#define REWARD_TIME_MS 21600000.0    // config.rewardTime

static Client *g_client;
static AccountInfo g_acc;
static int g_logged_in;
static int g_header_dirty = 1;   // cK() pending (page not up yet / offsetWidth not laid out)
static int g_header_tries;
static int g_restore_sent;       // a token restore is in flight
static int g_tab_index;          // windows[4].tabIndex
static int g_store_tab;          // windows[13].tabIndex
static int g_logout_sessions;    // d1: "Logout of all Sessions" clicked
static char g_token[192];
static char g_guest_name[32];    // client->menu_name before the account took it over
static int g_reward_played;      // dc: a match ended this session (canClaimReward)
static char g_reward_text[16];   // d8: last #claimTimer text
// windows[29] (Krunker Mail)
static int g_mail_tab;
static cJSON *g_mail_rows;       // mailData.d (owned), NULL = not loaded
static int g_mail_loaded;        // mailData != null
// 2FA: d3 picked cells of the login grid
static int g_2fa_cells[25];
static int g_2fa_count;
static double g_auth_sent_at;    // a login/register is waiting for its "a" reply (glfwGetTime), 0 none
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

// UTILS.eloIconHTML(elo, style): config.eloPer 120, config.maxELOLevel 8
void account_elo_icon_html(char *out, const size_t n, const double elo, const char *style) {
    int id = (int) floor((elo > 0 ? elo : 0) / 120.0);
    if (id > 8) id = 8;
    if (id < 1) id = 1;
    snprintf(out, n, "<img class='accountEloIcon' style='%s' src='./img/ranks/icon_%d.png'>", style && style[0] ? style : "margin-bottom:-2px;", id - 1);
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

// ------------------------------------------------------------------ state

const AccountInfo *account_get(void) { return g_logged_in ? &g_acc : NULL; }

static void mark_header(void);
static void rerender_windows(void);

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

// Account.updateLevel()
static void update_level(void) {
    const double h = RANK_VAR * sqrt(g_acc.score > 0 ? g_acc.score : 0);
    g_acc.level = (int) floor(h);
    g_acc.level_prog = (int) lround((h - g_acc.level) * 100.0);
    if (g_acc.level < 1) g_acc.level = 1;
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
    g_acc.elo = num_at(h, 22);
    g_acc.elo2 = num_at(h, 23);
    g_acc.elo4 = num_at(h, 24);
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

static void update_header(void) {
    g_header_dirty = 0;
    if (!html_ui_available() || !html_dom_by_id("signedInHeaderBar")) {
        g_header_dirty = 1;
        return;
    }
    if (g_logged_in) {
        set_display("signedOutHeaderBar", "none");
        set_display("signedInHeaderBar", "");
        // acc.level >= config.rewardMinLvl: #claimHolder (daily reward) + dd(true)
        if (g_acc.level >= REWARD_MIN_LVL) {
            set_display("claimHolder", "block");
            g_reward_text[0] = 0;
        } else {
            set_display("claimHolder", "none");
            HtmlElem merch = html_dom_by_id("merchHolder");
            if (merch) html_dom_set_style(merch, "top", "110px");
        }
        HtmlElem el;
        if ((el = html_dom_by_id("menuAccountUsername"))) html_dom_set_text(el, g_acc.premium ? g_acc.alias : g_acc.name);
        if ((el = html_dom_by_id("menuKRCount"))) {
            char funds[32], html[96];
            comma_number(funds, sizeof(funds), g_acc.funds);
            snprintf(html, sizeof(html), "%s <span style='color:#fff'>KR</span>", funds);
            html_dom_set_html(el, html);
        }
        if ((el = html_dom_by_id("menuLevelBar"))) {
            char w[16];
            snprintf(w, sizeof(w), "%d%%", g_acc.level_prog);
            html_dom_set_style(el, "width", w);
        }
        if ((el = html_dom_by_id("mLevelCont"))) {
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
        const float w = bar ? html_dom_offset_width(bar) : 0.0f;
        if (w > 0.0f) {
            char left[24];
            html_dom_set_style(bar, "border-radius", "0px");
            snprintf(left, sizeof(left), "%dpx", (int) w);
            if ((el = html_dom_by_id("mLevelCont"))) html_dom_set_style(el, "left", left);
        } else if (g_header_tries++ < 30) {
            g_header_dirty = 1; // laid out on a later frame
        }
    } else {
        set_display("signedOutHeaderBar", "");
        set_display("signedInHeaderBar", "none");
        set_display("claimHolder", "none");
        HtmlElem merch = html_dom_by_id("merchHolder");
        if (merch) html_dom_set_style(merch, "top", "110px");
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

static void rerender_windows(void) {
    menu_rerender_if_open(5);
    menu_rerender_if_open(14);
    menu_rerender_if_open(27);
    menu_rerender_if_open(33);
    menu_rerender_if_open(13);
}

// ------------------------------------------------------------------ requests

// sendAccountRequest(action, payload)
static void send_account_request(const int action, const char *name, const char *pass, const char *third) {
    if (action >= 0) show_acc_resp(ACC_STR_WAIT);
    cJSON *frame = cJSON_CreateArray();
    cJSON_AddItemToArray(frame, cJSON_CreateString("a"));
    cJSON_AddItemToArray(frame, cJSON_CreateNumber(action));
    if (action >= 0) {
        cJSON *payload = cJSON_CreateArray();
        cJSON_AddItemToArray(payload, cJSON_CreateString(name ? name : ""));
        cJSON_AddItemToArray(payload, pass ? cJSON_CreateString(pass) : cJSON_CreateNull());
        cJSON_AddItemToArray(payload, third ? cJSON_CreateString(third) : cJSON_CreateNull());
        cJSON_AddItemToArray(frame, payload);
        cJSON_AddItemToArray(frame, cJSON_CreateNull()); // captcha key (no captcha in KrunkNative)
    }
    backend_social_send(frame);
    cJSON_Delete(frame);
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
    clan_ui_reset();
    cJSON_Delete(g_acc.stats);
    cJSON_Delete(g_acc.data);
    memset(&g_acc, 0, sizeof(g_acc));
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
static void mail_count(int count);
static void mail_read_data(const cJSON *rows);
static void twofa_res(const cJSON *frame);

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
        mark_header();
        rerender_windows();
    } else if (!strcmp(label, "spin")) {
        store_ui_frame(frame);
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

// dd(force): #claimTimer countdown to the next daily reward, "Play 1 Game" until a match
// ended this session, #claimImg ready / waiting.
static void reward_tick(const int force) {
    if (!g_logged_in || g_acc.level < REWARD_MIN_LVL) return;
    const double last = g_acc.data ? num_at(g_acc.data, 11) : 0.0;
    double db = REWARD_TIME_MS - ((double) time(NULL) * 1000.0 - last);
    const long long ms = db > 0 ? (long long) db : 0;
    char t[16];
    snprintf(t, sizeof(t), "%02lld:%02lld:%02lld", (ms / 3600000) % 24, (ms / 60000) % 60, (ms / 1000) % 60);
    if (!force && !strcmp(t, g_reward_text)) return;
    snprintf(g_reward_text, sizeof(g_reward_text), "%s", t);
    const int zero = !strcmp(t, "00:00:00");
    HtmlElem timer = html_dom_by_id("claimTimer");
    if (timer) {
        html_dom_set_text(timer, zero ? (g_reward_played ? "" : "Play 1 Game") : t);
        html_dom_set_style(timer, "font-size", zero ? (g_reward_played ? "18px" : "13px") : "18px");
    }
    HtmlElem merch = html_dom_by_id("merchHolder");
    if (merch) html_dom_set_style(merch, "top", zero && g_reward_played ? "205px" : "237px");
    HtmlElem img = html_dom_by_id("claimImg");
    const char *src = db <= 0 && g_reward_played ? "./img/claim_0.png" : "./img/claim_1.png";
    char cur[64] = "";
    if (img && (html_dom_get_attr(img, "src", cur, sizeof(cur)) < 0 || strcmp(cur, src))) html_dom_set_attr(img, "src", src);
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
    StrBuf sb = {0};
    sb_append(&sb, "<div>Your Notifications");
    if (g_mail_tab == 0) sb_append(&sb, "<div class='readAll' onclick='windows[29].readAll()'>Read All</div>");
    sb_append(&sb, "</div><div class='menuTabsNew'>");
    static const char *const tabs[2] = {"Unread", "Archive"}; // config.mailTabs
    for (int i = 0; i < 2; i++) sb_appendf(&sb, "<div class='menuTabNew%s' onclick='windows[29].switchTab(%d)' style='width: 50%%;'>%s</div>", g_mail_tab == i ? " tabANew" : "", i, tabs[i]);
    sb_append(&sb, "</div><div id='mailList'>");
    if (!g_mail_loaded) {
        char frame[32];
        snprintf(frame, sizeof(frame), "[\"kml\",%d]", g_mail_tab);
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
            if (archived != (g_mail_tab == 1)) continue;
            shown++;
            const int id = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(r, "km_id"));
            char ago[32];
            readable_time2(ago, sizeof(ago), now - parse_date_ms(cJSON_GetStringValue(cJSON_GetObjectItem(r, "km_datesent"))));
            sb_appendf(&sb, "<div class='mailObj%s' id='mail_%d' onclick='windows[29].readMail(this, %d)'%s><div style='position:relative;'><div class='mailDate'>%s</div></div><div class='mailFrom'>",
                       g_mail_tab == 0 ? " mailUnread" : "", id, id, g_mail_tab == 0 ? " style='border: 2px solid #5699eb;'" : "", ago);
            const char *from = cJSON_GetStringValue(cJSON_GetObjectItem(r, "km_playerfrom"));
            const char *subject = cJSON_GetStringValue(cJSON_GetObjectItem(r, "km_subject"));
            sb_append_esc(&sb, from ? from : "");
            sb_append(&sb, "</div>");
            sb_append_esc(&sb, subject ? subject : "");
            sb_append(&sb, "<div class='mailText'></div></div>");
        }
        if (!shown) sb_append(&sb, g_mail_tab == 0 ? "No messages" : "No archive messages");
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
        const HtmlElem a = html_dom_first_child(el), b = a ? html_dom_next(a) : 0, text = b ? html_dom_next(b) : 0;
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

// ---- 2FA (index.js d4 / pick2FAkey / twoFAres / sub2FA / act2FA)

static char *twofa_grid(const char *pattern) {
    StrBuf sb = {0};
    sb_append(&sb, "<table class='twoFATable'><tbody>");
    for (int cell = 0, r = 0; r < 5; r++) {
        sb_append(&sb, "<tr>");
        for (int c = 0; c < 5; c++, cell++) {
            int on = pattern && strchr(pattern, 'A' + cell) != NULL;
            for (int i = 0; i < g_2fa_count && !on; i++) on = g_2fa_cells[i] == cell;
            sb_appendf(&sb, "<td %s", on ? "class='twoFAkeyg'" : "");
            if (!pattern) sb_appendf(&sb, " onclick='pick2FAkey(%d)'", cell);
            sb_append(&sb, "><span style='opacity:0;padding-left:7px'>10</span></td>");
        }
        sb_append(&sb, "</tr>");
    }
    sb_append(&sb, "</tbody></table>");
    return sb_finish(&sb);
}

static void twofa_res(const cJSON *frame) {
    const cJSON *kind = cJSON_GetArrayItem(frame, 1);
    const int k = cJSON_IsNumber(kind) ? kind->valueint : 0;
    if (k == 1) { // the login needs the pattern
        set_display("twoFAPop", "block");
        HtmlElem btn = html_dom_by_id("twoFAbtn");
        if (btn) {
            html_dom_set_html(btn, "Submit");
            html_dom_set_attr(btn, "class", "");
        }
        g_2fa_count = 0;
        char *grid = twofa_grid(NULL);
        HtmlElem holder = html_dom_by_id("twoFAGholder");
        if (holder) html_dom_set_html(holder, grid);
        free(grid);
    } else if (k == 2) {
        set_display("twoFAPop", "none");
    } else { // act2FA reply: the new pattern, or an error
        HtmlElem b = html_dom_by_id("twoFAB");
        const char *pattern = str_at(frame, 3);
        if (b) html_dom_set_attr(b, "class", "menuLinkD floatR");
        if (pattern) {
            if (b) html_dom_set_text(b, "Enabled");
            g_acc.has_2fa = 1;
            HtmlElem grid_el = html_dom_by_id("twoFGridR");
            char *grid = twofa_grid(pattern);
            if (grid_el) {
                html_dom_set_display(grid_el, "block");
                html_dom_set_html(grid_el, grid);
            }
            free(grid);
            HtmlElem prom = html_dom_by_id("twoFAprom");
            if (prom) html_dom_set_html(prom, "Make sure to <b>take a picture</b> of the above pattern and to save it securely. You will need it to login again.");
        } else if (b) {
            html_dom_set_html(b, str_at(frame, 2) ? str_at(frame, 2) : "Error");
        }
    }
}

void account_ui_tick(Client *client) {
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
    store_ui_tick();
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

// No brain answering: v3.1.1's account requests would never be answered either; say so
// instead of pretending (the guest has no account without the backend).
// No account server: v3.1.1's registerAcc / loginAcc transport failure text ("Please try
// again." in #accResp); the reason goes to the log, the menu never pretends to log in.
static void auth_unreachable(void) {
    if (backend_enabled()) printf("[account] no account server at %s (GET /mm/status found no kn_brain): start it with make backend-up\n", backend_url());
    else printf("[account] no account server configured (--brain / KRUNKNATIVE_BRAIN_URL / knBrainUrl)\n");
    show_acc_resp("Please try again.");
    g_auth_sent_at = 0;
}

int account_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (name && (!strcmp(name, "registerAcc") || !strcmp(name, "loginAcc")) && (!backend_enabled() || !backend_social_is_open())) {
        if (backend_available() == 0) backend_probe_start(); // maybe the backend came up since: ask again
        else backend_social_open();
        auth_unreachable();
        return 1;
    }
    if (!name || !backend_enabled()) return -1;
    if (store_ui_handle(name, ev, args, argc) >= 0) return 1;
    if (clan_ui_handle(name, ev, args, argc) >= 0) return 1;
    if (ranked_ui_handle(name, ev, args, argc) >= 0) return 1;
    if (!strcmp(name, "registerAcc") || !strcmp(name, "loginAcc")) {
        char user[64], pass[128];
        read_input("accName", user, sizeof(user));
        read_input("accPass", pass, sizeof(pass));
        if (!strcmp(name, "registerAcc")) send_account_request(0, user, pass, NULL); // [name, pass, referralCode]
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
    if (!strcmp(name, "windows.4.switchTab")) {
        g_tab_index = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        menu_show_window(5, 1); // updateWindow(5)
        return 1;
    }
    if (!strcmp(name, "windows.13.switchTab") || !strcmp(name, "windows.13.premium") || !strcmp(name, "windows.13.purchase")) {
        const int tab = !strcmp(name, "windows.13.premium") ? 1 : !strcmp(name, "windows.13.purchase") ? 2
                      : argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        if (tab == 4) {
            app_route_url("/social.html?p=market"); // window.open("/social.html?p=market")
            return 1;
        }
        g_store_tab = tab;
        menu_show_window(14, 1);
        return 1;
    }
    if (!strcmp(name, "claimReward") && g_logged_in) {
        // v3.1.1 gates the reward behind an ad (alert "Ad Failed"); KrunkNative has no ads:
        // ask the brain once the timer ran out and a match was played
        const double last = g_acc.data ? num_at(g_acc.data, 11) : 0.0;
        if (g_reward_played && REWARD_TIME_MS - ((double) time(NULL) * 1000.0 - last) <= 0) backend_social_send_text("[\"clm\"]");
        return 1;
    }
    if (!strcmp(name, "windows.29.switchTab")) {
        const int tab = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
        const int force = argc > 1;
        if (g_mail_tab != tab || force) {
            g_mail_tab = tab;
            cJSON_Delete(g_mail_rows);
            g_mail_rows = NULL;
            g_mail_loaded = 0;
            menu_show_window(30, 1);
        }
        return 1;
    }
    if (!strcmp(name, "windows.29.readMail")) { // toggle the text; "Loading..." + io.send("rml", id)
        HtmlElem obj = ev ? ev->target : 0;
        const HtmlElem a = obj ? html_dom_first_child(obj) : 0, b = a ? html_dom_next(a) : 0, text = b ? html_dom_next(b) : 0;
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
        cJSON_Delete(g_mail_rows);
        g_mail_rows = NULL;
        g_mail_loaded = 0;
        mail_count(0);
        menu_show_window(30, 1);
        return 1;
    }
    if (!strcmp(name, "pick2FAkey")) {
        HtmlElem btn = html_dom_by_id("twoFAbtn");
        char t[32] = "";
        if (btn) html_dom_get_text(btn, t, sizeof(t));
        if (!strcmp(t, "Loading...")) return 1;
        const int cell = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : -1;
        if (cell < 0 || cell >= 25) return 1;
        int at = -1;
        for (int i = 0; i < g_2fa_count; i++) if (g_2fa_cells[i] == cell) at = i;
        if (at >= 0) g_2fa_cells[at] = g_2fa_cells[--g_2fa_count];
        else g_2fa_cells[g_2fa_count++] = cell;
        char *grid = twofa_grid(NULL);
        HtmlElem holder = html_dom_by_id("twoFAGholder");
        if (holder) html_dom_set_html(holder, grid);
        free(grid);
        return 1;
    }
    if (!strcmp(name, "sub2FA")) { // io.send("2fa", 1, d3)
        HtmlElem btn = html_dom_by_id("twoFAbtn");
        if (btn) {
            html_dom_set_html(btn, "Loading...");
            html_dom_set_attr(btn, "class", "twoFAbtnN");
        }
        cJSON *frame = cJSON_CreateArray();
        cJSON_AddItemToArray(frame, cJSON_CreateString("2fa"));
        cJSON_AddItemToArray(frame, cJSON_CreateNumber(1));
        cJSON *cells = cJSON_CreateArray();
        for (int i = 0; i < g_2fa_count; i++) cJSON_AddItemToArray(cells, cJSON_CreateNumber(g_2fa_cells[i]));
        cJSON_AddItemToArray(frame, cells);
        backend_social_send(frame);
        cJSON_Delete(frame);
        return 1;
    }
    if (!strcmp(name, "act2FA") && g_logged_in) { // io.send("2fa")
        HtmlElem b = html_dom_by_id("twoFAB");
        if (b) {
            html_dom_set_html(b, "Please wait...");
            html_dom_set_attr(b, "class", "menuLinkD floatR");
        }
        backend_social_send_text("[\"2fa\"]");
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

// windows[4].gen() for a signed-in account (the guest form is ui/windows/5.html).
static char *gen_account(void) {
    static const char *const tabs[] = {"Stats", "Partner", "Settings", "Twitch"}; // config.profileTabs
    StrBuf sb = {0};
    sb_append(&sb, "<div style='color:rgba(0,0,0,0.3);margin-top:-5px;margin-bottom:10px'>Choose a Display name &amp; more with <a href='javascript:;' onclick='windows[13].premium()'>Krunker Premium</a></div>");
    sb_append(&sb, "<div class='menuTabsNew'>");
    for (int i = 0; i < 4; i++) {
        sb_appendf(&sb, "<div class='menuTabNew%s' onclick='windows[4].switchTab(%d)'>%s</div>", g_tab_index == i ? " tabANew" : "", i, tabs[i]);
    }
    sb_append(&sb, "</div>");
    char a[64], b[256];
    if (g_tab_index == 0) {
        sb_append(&sb, "<div id='profilePicM' class='blurred'><img style='width: inherit;height: inherit;border-radius: inherit;' loading='lazy' src='./textures/classes/icon_0.png'><a class='pfpAction' onmouseenter='SOUND.play(`tick_0`,0.1)' onclick='pfpPopup();'>Change Picture</a></div>");
        sb_append(&sb, "<div style='display:inline-block;vertical-align:top;width:560px;margin-left:10px'><div class='settName' style='width:100%;margin-top:0px'>Name<a href='/social.html?p=profile&q=");
        sb_append_esc(&sb, g_acc.name);
        sb_append(&sb, "' class='floatR' target='_blank'>");
        sb_append_esc(&sb, g_acc.name);
        sb_append(&sb, "</a></div><div class='settName' style='width:100%'>Clan<a class='floatR' class='menuLink' onclick='showWindow(13)'>");
        if (g_acc.clan[0]) {
            sb_append(&sb, "[");
            sb_append_esc(&sb, g_acc.clan);
            sb_append(&sb, "]");
        } else {
            sb_append(&sb, "None");
        }
        sb_appendf(&sb, "</a></div><div class='settName' style='margin-top:0px'>Level<span class='floatR'>%d</span></div></div>", g_acc.level);
        xp_bar_text(a, sizeof(a));
        sb_appendf(&sb, "<div class='xpBar' style='margin-top:10px'><div class='xpBarB' style='width:%d%%'></div><span class='xpBarV'>%s</span></div>", g_acc.level_prog, a);
        stat_row_num(&sb, "Score", g_acc.score);
        comma_number(a, sizeof(a), g_acc.funds);
        snprintf(b, sizeof(b), "<span style='color:rgba(0,0,0,0.5)'>%s</span> KR", a);
        stat_row(&sb, "KR", b);
        stat_row_num(&sb, "Kills", g_acc.kills);
        stat_row_num(&sb, "Deaths", g_acc.deaths);
        stat_row_fixed(&sb, "KDR", g_acc.kills / (g_acc.deaths > 1 ? g_acc.deaths : 1));
        stat_row_fixed(&sb, "KPG", g_acc.kills / (g_acc.games > 1 ? g_acc.games : 1));
        stat_row_num(&sb, "Nukes", stat_num("n"));
        stat_row_num(&sb, "Melee", stat_num("mk"));
        stat_row_num(&sb, "Beatdowns", stat_num("fk"));
        stat_row_num(&sb, "Bullseyes", stat_num("tmk"));
        stat_row_num(&sb, "Headshots", stat_num("hs"));
        stat_row_num(&sb, "Wallbangs", stat_num("wb"));
        stat_row_num(&sb, "Games Played", g_acc.games);
        stat_row_num(&sb, "Games Won", g_acc.wins);
        stat_row_num(&sb, "Games Lost", g_acc.games - g_acc.wins > 0 ? g_acc.games - g_acc.wins : 0);
        const double losses = g_acc.games - g_acc.wins;
        stat_row_fixed(&sb, "W/L", g_acc.wins / (losses > 1 ? losses : 1));
        readable_time(a, sizeof(a), g_acc.time_played);
        stat_row(&sb, "Time Played", a);
        sb_append(&sb, "<div style='height:10px'></div>");
        // MMR rows: v3.1.1 appends UTILS.eloIconHTML (img/ranks/icon_N.png, not imported in ui/)
        sb_append(&sb, "<div style='font-size:26px;margin-top:10px;font-weight:bold'>Ranked</div>");
        static const char *const mmr[3] = {"MMR (1v1)", "MMR (2v2)", "MMR (4v4)"};
        const double elos[3] = {g_acc.elo, g_acc.elo2, g_acc.elo4};
        for (int i = 0; i < 3; i++) { // acc.elo.round(1) + UTILS.eloIconHTML(elo, "margin-left:4px;")
            char icon[160];
            account_elo_icon_html(icon, sizeof(icon), round(elos[i] * 10.0) / 10.0, "margin-left:4px;");
            snprintf(b, sizeof(b), "%g%s", round(elos[i] * 10.0) / 10.0, icon);
            stat_row(&sb, mmr[i], b);
        }
    } else if (g_tab_index == 1) {
        sb_append(&sb, "<div style='font-size:26px;margin-top:20px;font-weight:bold'>Krunker Partner</div><div style='color:rgba(0,0,0,0.6);margin-bottom:20px'>Krunker Partners earn Money through: <div class='partnerItem' style='margin-top:10px'><b>-</b> Getting KR Sales on their Partner Code</div><div class='partnerItem'><b>-</b> Plays on their Custom Games</div><div class='partnerItem'><b>-</b> Bringing new Players to Krunker</div></div><div style='font-size:26px;margin-top:20px;font-weight:bold'>Partner Application</div><div style='font-size:17px;color:rgba(0,0,0,0.4);margin-top:5px'>By submitting an Application you agree to the <a href='./docs/partner.txt'>Partner Terms</a></div><input type='text' id='crtrCode' class='smlInput' style='width:100%;margin-bottom:8px' placeholder='Partner Code Name' /><input type='text' id='crtrEmail' class='smlInput' style='width:80%;margin-bottom:8px' placeholder='PayPal Email for Payouts' /><a class='menuLink' style='margin-left:20px;vertical-align:middle' onclick='submitCrtrCod()'>Submit</a><div id='ctrtRes' style='color:rgba(0,0,0,0.5)'></div></div>");
    } else if (g_tab_index == 2) {
        sb_append(&sb, "<div style='font-size:26px;margin-top:12px;'>Account Settings</div>");
        if (g_acc.premium) {
            sb_append(&sb, "<div class='settName'>Display Name<div class='updDisN' onclick='changeName();'>Update</div><input type='search' autocomplete='off' autocorrect='off' autocapitalize='off' spellcheck='false' id='displayN' class='smlInput' placeholder='");
            sb_append_esc(&sb, g_acc.alias);
            sb_append(&sb, "'><div id='aliasRes'></div></div>");
        }
        // Country Flag (windows.loadout.flag): acc.stats.flg, Game.flags
        const int flg = g_acc.stats && cJSON_IsNumber(cJSON_GetObjectItem(g_acc.stats, "flg")) ? (int) stat_num("flg") : -1;
        sb_appendf(&sb, "<div class='settName'>Country Flag<select onchange='selectFlag(this.value)' class='inputGrey2' style='padding:4px;font-size:13px;margin-bottom:-18px'><option value='-1' %s>None</option>", flg == -1 ? "selected" : "");
        for (int i = 0; i < ACCOUNT_FLAG_COUNT; i++) sb_appendf(&sb, "<option value='%d' %s>%s</option>", i, flg == i ? "selected" : "", ACCOUNT_FLAGS[i].name);
        sb_append(&sb, "</select></div>");
        if (!g_acc.hack) {
            // KrunkNative: v3.1.1 ships act2FA()/twoFAres() but no button for them; this row exposes it
            sb_appendf(&sb, "<div class='settName'>2-Factor Authentication<a id='twoFAB' class='%s floatR' onclick='act2FA()'>%s</a></div><div id='twoFGridR' class='twoFAGrid' style='display:none'></div><div id='twoFAprom' style='font-size:17px;color:rgba(0,0,0,0.4)'></div>",
                       g_acc.has_2fa ? "menuLinkD" : "menuLink", g_acc.has_2fa ? "Enabled" : "Enable");
            sb_append(&sb, "<div style='font-size:26px;margin-top:20px;'>Account Security</div><div style='font-size:17px;color:rgba(0,0,0,0.4);margin-top:5px'>You can update your password every 30 minutes. <b>Never share your password</b> with anyone and keep it in a secure place</div><input type='password' id='oldPass' class='smlInput' style='width:100%;margin-bottom:8px' placeholder='Current Password' /><input type='password' id='newPass' class='smlInput' style='width:100%;margin-bottom:8px' placeholder='New Password' /><input type='password' id='repNPass' class='smlInput' style='width:82%;margin-bottom:8px' placeholder='Repeat Password' /><div class='subPassChg' onclick='changePass()'>Submit</div><div id='newPRes' style='color:rgba(0,0,0,0.5)'></div>");
        }
    } else {
        sb_append(&sb, "<div class='settName'>Twitch (none)<div class='lnkTw' onclick='linkTwitchPop();'>Link</div></div><div id='skinList'><div style='margin-bottom:10px;color:rgba(0,0,0,0.5);'>Twitch Drops will return soon</div></div>");
    }
    sb_append(&sb, "<div style='height:25px'></div><a class='menuLink' onclick='logoutAcc()'>Logout</a>");
    if (!g_logout_sessions) sb_append(&sb, "<a style='float:right' class='menuLink' onclick='logoutSessions(this)'>Logout of all Sessions</a>");
    return sb_finish(&sb);
}

// renderRequirement(level, text)
static char *requirement(const int level, const char *text) {
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class='reqW'><i class='material-icons reqWIcon'>clear</i><div class='reqWLvl'>LEVEL %d REQUIRED</div><div class='reqWTxt'>%s</div></div>", level, text);
    return sb_finish(&sb);
}

// windows[13].gen() for an account. KrunkNative has no payments, no prize-wheel table
// (Game.store) and no img/shop art: the tabs, the balance and the forms v3.1.1 shows
// are real, the card lists stay empty.
static char *gen_store(void) {
    static const char *const tabs[] = {"Prize Wheels", "Premium", "Purchase KR", "Redeem", "Marketplace"}; // config.shopTabs
    StrBuf sb = {0};
    sb_append(&sb, "<div class='menuTabsNew'>");
    for (int i = 0; i < 5; i++) sb_appendf(&sb, "<div class='menuTabNew%s' onclick='windows[13].switchTab(%d)'>%s</div>", g_store_tab == i ? " tabANew" : "", i, tabs[i]);
    sb_append(&sb, "</div>");
    if (g_store_tab == 0) {
        store_wheels_html(&sb); // prize wheels (store_ui.c)
    } else if (g_store_tab == 1) {
        sb_append(&sb, "<div style='color:rgba(0,0,0,0.3);margin-bottom:10px'>Choose a Display name &amp; more with <a href='javascript:;' onclick='showWindow(36)'>Krunker Premium</a></div><div id='skinList' style='margin-bottom:10px'></div><div id='premiumRes'></div>");
        char t[64];
        readable_time(t, sizeof(t), g_acc.premium ? num_at(g_acc.data, 27) : 0);
        sb_appendf(&sb, "Time Remaining<span id='premTime'>%s</span>", t);
    } else if (g_store_tab == 2) {
        sb_append(&sb, "<div style='color:rgba(0,0,0,0.3);font-size:19px;margin-bottom:-3px'>By purchasing you agree to the <a target='_blank' href='./docs/terms.txt'>Terms</a>. Support your favorite Content Creator by using a Creator Code</div>");
        sb_append(&sb, "<div><input type='text' id='creatorCodeI' class='smlInput' placeholder='Enter Creator Code' /><div class='creatorCodeBtn' style='background-color:#41c300' onclick='creatorCodeAction(1)'>Save</div></div><div id='skinList'></div>");
        sb_append(&sb, "<div style='font-size:9px;margin-top:10px'>* Prices in USD can be converted to your local currency at an exchange rate defined by the payment provider or bank</div>");
    } else {
        sb_append(&sb, "<div style='color:rgba(0,0,0,0.3);font-size:19px;margin-bottom:5px'>Get Vouchers from certain Streamers &amp; Events</div><div><input type='text' id='redVouch' class='smlInput' placeholder='Enter KR Code'/><div class='redVouchBtn' onclick='redeemVoucher()'>Redeem</div></div><div id='vouchRes' style='display:none'></div>");
    }
    return sb_finish(&sb);
}

// windows[32].gen(): config.minChalLevel = 30; the challenge list needs acc.stats.chgP
// (else io.send("chg") and "Loading" until the server answers)
static char *gen_challenges(void) {
    if (g_acc.level < 30) return requirement(30, "for Challenges");
    backend_social_send_text("[\"chg\"]");
    return strdup("Loading");
}

char *account_window_gen(const int id, const char **header) {
    (void) header;
    if (backend_available() != 1 || !g_logged_in) return NULL; // the guest captures (LOGIN REQUIRED cards, login form)
    if (id == 5) return gen_account();
    if (id == 14) return gen_store();
    if (id == 27) return ranked_window_gen();
    if (id == 33) return gen_challenges();
    if (id == 30) return gen_mail();
    return NULL;
}

void account_window_post(const int id) {
    if (id != 5 || backend_available() != 1 || g_logged_in) return;
    // v3.1.1 prefills the login form with getSavedVal("krunker_username")
    char name[64];
    storage_get("krunker_username", name, sizeof(name));
    HtmlElem input = html_dom_by_id("accName");
    if (input) html_dom_set_value(input, name);
}
