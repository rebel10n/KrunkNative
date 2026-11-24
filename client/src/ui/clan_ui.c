// Krunker v10.0.2 Clans window (showWindow(13) = windows[12], v10:213330-213573) over the brain's
// social socket, plus its settings tab (the Svelte clan-settings component, v10:95700-96969).
//
// Socket frames (doc/backend.md 7.4): ["cln", action, value, extra] -> ["cln", msg, data, ok,
// action]: 1 create(tag) 2 request(tag) 3 my clan 4 leave/delete -> ["clnL"] 5 kick(id) 6 role(id,
// {r}) 7 funds(kr, {wd}) 9 Discord link; ["clnR", playerId, 1|0]; ["clnRA"] decline all; ["vcln"]
// on every render (OPEN_CLAN_TAB). The settings tab's gapi calls go through v10_gapi()
// (client/src/ui/v10_gapi.c, services/brain/clan_v10.c): /clans/<id>/motd | banner-color | logo |
// transfer. Deviation (Box Office): v10's ownership-transfer socket RPCs (clan-transfer-initiate /
// -cancel / -respond) are POST / DELETE /clans/<id>/transfer and POST /clans/<id>/transfer/respond. The
// Turf Wars loot config and the recruitment board are kn_brain HTTP routes (turf.c, ?token=).
// The logo, as v10.0.7's (deob-10.0.7.js:96058-96350): the drop target (the profile picture's file dialog,
// avatar_ui_pick_file, PNG only) selects a file, v10's checks run on it, and the BUY 20,000 KR button under it
// uploads it. Box Office: the upload is POST /clans/<id>/logo {file: base64} over the gapi tunnel (v10 sends the
// multipart `file`, as the krunker.io account's upload does); the approved logo is GET /api/clans/<id>/logo
// (clan_logo.c), not v10's user-assets CDN. Deviation (Box Office): the brain never keeps a picture that is
// under review, so there is no clans/<id>_pending.png; the uploader's own client shows the file it sent while it
// is pending, other clients the "Pending Approval" status alone.
//
// Every showWindow / updateWindow(13) mounts v10's Svelte components again (deob-10.0.7.js:223808-223850): the
// settings tab and the home tab's loot display load again and lose their local state. KrunkNative re-renders for
// a component's own state change too (clan_rerender): only the other renders count as a mount here.
//
// Permissions as v10: owner = clanData.clan_playerid; clanRole = the account's clan_members rank
// (v10 account slot player_clanrole), read from clanData.members. mgr = owner or Captain (2):
// tabs 3 / 4, member controls, requests, Decline All, withdraw, the Discord popup, logo,
// recruitment. mgr2 = owner or role >= 2: MOTD, banner colour, loot config.
// v10's Svelte views are rendered as HTML strings with the same markup and svelte-* classes
// (ui/css/svelte.css); the confirm prompts it pushes on its view stack use #genericPop.
//
// While krunker.io is the active account backend (an official krunker.io game) the window is the
// krunker.io account's clan, written as v10.0.7 writes it. Over the official game socket (the account
// bridge's held-sender route, krunker_net_send_account_frame): "vcln", the ["cln", 3] read and the
// window's writes with v10's encoding (create / join ["cln", 1 | 2, tag], leave / delete ["cln", 4, 1],
// kick ["cln", 5, id], role ["cln", 6, id, {r}], funds ["cln", 7, kr, {wd}], Discord ["cln", 9, link],
// ["clnR", id, ok], ["clnRA"]), whose "cln" / "clnL" answers come back to clan_ui_frame(), and the
// ownership transfer's rpcs clan-transfer-initiate / -cancel / -respond (deob-10.0.7.js:220556-220642),
// whose "_0" / "_1" answers come back to clan_ui_rpc(). The settings tab's reads are v10's own gapi
// calls (v10_gapi.c: GET /clans/<id>/motd, /loot-config, /recruitment, the id being clanData.clan_id as
// v10's), and so are its gapi writes, with v10.0.7's method, path and JSON body and the account's Bearer
// (deob-10.0.7.js:26838-26924): PUT / DELETE /clans/<id>/motd {message}, PUT /clans/<id>/loot-config,
// POST /clans/<id>/banner-color {paint_item_id}, DELETE /clans/<id>/logo, POST / DELETE
// /clans/<id>/recruitment, and the logo upload POST /clans/<id>/logo as v10's multipart FormData with
// the picked PNG as `file` (v10_gapi_upload, deob-10.0.7.js:96094-96120); their answers are applied as
// v10's settings components apply them.
#include <avatar_ui.h>
#include <clan_discord.h>
#include <clan_recruit.h>
#include <js_int.h>
#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <ui_logic.h>
#include <v10_social.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define CLAN_RANK_VAR 0.03            // config.rankVar
#define COL_PREMIUM "#FBC02D"         // COLORS.premium
#define COL_VERIFIED_PLAYER "#40C4FF" // COLORS.verified.player
#define LOGO_COST 20000               // clan logo: 20,000 KR from the clan bank
#define LOGO_MAX_BYTES 262144         // v10: "max 256KB" (the brain's CLAN_LOGO_MAX_BYTES)

// _7497: home / Members / Stats / Requests / settings
static const char *const k_clan_tabs[] = {
    "<span class='material-icons' style='color:#fbc02d;font-size:42px;vertical-align:bottom'>home</span>",
    "Members", "Stats", "Requests",
    "<span class='material-icons' style='color:#ffffff;font-size:42px;vertical-align:bottom'>settings</span>"};
#define CLAN_TABS 5
static const struct {
    const char *name;
    int max;
    int perms;
} k_clan_roles[] = {{"Recruit", 0, 0}, {"Soldier", 25, 0}, {"Captain", 5, 1}, {"Commander", 1, 2}}; // _7498
#define CLAN_ROLES ((int) (sizeof(k_clan_roles) / sizeof(k_clan_roles[0])))

// v10.0.2 paints (store skins of type 18, v10 skins 7776-7907): banner colours
static const struct {
    int index;
    const char *id, *name, *color; // color NULL: shader
    int shader;
} k_paints[] = {
    {7776, "red", "Red Paint", "#dc3c3c", -1},           {7777, "green", "Green Paint", "#50c864", -1},
    {7778, "blue", "Blue Paint", "#0457d1", -1},         {7779, "black", "Black Paint", "#1a1a1a", -1},
    {7780, "white", "White Paint", "#f0f0f0", -1},       {7781, "yellow", "Yellow Paint", "#f0d23c", -1},
    {7795, "spectrum", "Spectrum Paint", NULL, 0},       {7796, "synth", "Synth Paint", NULL, 1},
    {7831, "lavender", "Lavender Paint", "#A884E1", -1}, {7832, "pastelgreen", "Pastel Green Paint", "#84E18C", -1},
    {7833, "pastelblue", "Pastel Blue Paint", "#84D9E1", -1}, {7834, "pink", "Pink Paint", "#FF1FB8", -1},
    {7835, "0", "Terracota", "#c08552", -1},             {7836, "1", "Orange", "#ff9913", -1},
    {7837, "2", "Electric Purple", "#bf00ff", -1},       {7904, "gold", "Gold Paint", NULL, 2},
    {7905, "silver", "Silver Paint", NULL, 3},           {7906, "prism", "Prism Paint", NULL, 4},
    {7907, "codefall", "Codefall Paint", NULL, 5},
};
#define PAINTS ((int) (sizeof(k_paints) / sizeof(k_paints[0])))
static const char *const k_shader_names[] = {"Spectrum", "Synth", "Gold", "Silver", "Prism", "Codefall"}; // _32550

// loot modes (v10:96984-96990) and their descriptions
static const struct {
    const char *mode, *label, *short_label, *desc;
} k_loot_modes[] = {
    {"clan_bank", "Clan Bank (default)", "Clan Bank", "All rewards go to the clan treasury."},
    {"even", "Even Split", "Even Split", "Rewards are split equally among all eligible participants."},
    {"top_weighted", "Top Weighted", "Top Weighted", "Top players get a larger share of the rewards."},
    {"performance", "Performance (by kills)", "Performance", "Rewards are distributed proportionally to kills."},
    {"top_performance", "Top N by Performance", "Top N by Performance", NULL},
};
#define LOOT_MODES ((int) (sizeof(k_loot_modes) / sizeof(k_loot_modes[0])))

static const char *const k_regions[][2] = {{"", "Any"}, {"NA", "North America"}, {"EU", "Europe"}, {"AS", "Asia"},
                                           {"OCE", "Oceania"}, {"SA", "South America"}, {"ME", "Middle East"}};
static const char *const k_langs[][2] = {
    {"", "Any"},          {"en", "English"},   {"es", "Spanish"},  {"pt", "Portuguese"}, {"fr", "French"},  {"de", "German"},
    {"ru", "Russian"},    {"zh", "Chinese"},   {"ja", "Japanese"}, {"ko", "Korean"},     {"ar", "Arabic"},  {"tr", "Turkish"},
    {"pl", "Polish"},     {"it", "Italian"},   {"nl", "Dutch"},    {"sv", "Swedish"},    {"th", "Thai"},    {"vi", "Vietnamese"},
    {"id", "Indonesian"}, {"hi", "Hindi"},     {"fil", "Filipino"}, {"cs", "Czech"},     {"ro", "Romanian"}, {"hu", "Hungarian"},
    {"uk", "Ukrainian"},  {"fi", "Finnish"},   {"no", "Norwegian"}, {"da", "Danish"},    {"el", "Greek"},   {"he", "Hebrew"},
    {"ms", "Malay"}};

static cJSON *g_clan;   // windows[12].clanData (owned), NULL = not loaded
static int g_clan_tab;  // windows[12].tabIndex
static int g_req_sent;  // sendReq(3) in flight (the window asks once per load)
static int g_clan_timer; // the clan's own load (sendReq(3)) has a deadline (html_timer id, 0 none)
static int g_clan_failed; // that load got no answer (or was not sent): the window says so, with Retry
static int g_scenario;  // --render-scenario: no network
static int g_svelte;    // the render is a Svelte component's own update (clan_rerender), not a mount

// Every load here has a deadline (CLAN_WAIT_MS). The clan's own load (sendReq(3)) that gets no answer, or could not
// be sent (no socket, the brain is gone), says so with Retry instead of "LOADING..." for good; a settings load that
// gets none fails as v10's fetch does when the network fails ("Failed to fetch").
#define CLAN_WAIT_MS 15000
#define FETCH_FAILED "Failed to fetch" // a fetch() that could not reach the server (Chromium's TypeError message)

// settings tab / tab 0 state (the Svelte components' state)
static struct {
    // MOTD (svelte-15d5v89)
    cJSON *motd;          // {message, set_by_name, set_at} (owned), NULL = not loaded
    int motd_loading, motd_busy; // busy: 1 saving, 2 removing
    char motd_text[500 * 4 + 1], motd_err[96]; /* maxlength 500; one character can be four bytes */
    int motd_saved;       // "Saved!" for 3 s
    int64_t motd_for;     // clan id the MOTD belongs to
    int motd_timer;       // the load's deadline (html_timer id, 0 none)
    // loot config (svelte-1ydnong) + the tab 0 display
    cJSON *loot;          // flat config (owned), NULL = not loaded
    int loot_loading, loot_saving, loot_saved, loot_failed; // loot_failed: the load failed (the form's defaults)
    int loot_timer;
    char loot_err[96];
    char loot_mode[24];
    int loot_clan_pct, loot_top_count, loot_top_pct, loot_rem, loot_min_games;
    // recruitment (svelte-1o06kdk)
    cJSON *recruit;       // the clan's post (owned) or NULL
    int recruit_loaded, recruit_loading, recruit_saving;
    int recruit_timer;    // the load's deadline
    char recruit_err[96], recruit_ok[96];
    char r_desc[2000 * 4 + 1], r_looking[500 * 4 + 1], r_level[16], r_kd[16], r_region[8], r_lang[8];
    // banner colour
    int banner_sel;       // index in k_paints, -1 none
    int banner_busy;
    char banner_err[96];
    // logo: busy 1 uploading, 2 removing; logo_err the settings' error line
    int logo_busy;
    char logo_err[96];
    char *logo_file;      // the selected file (base64, owned), NULL none (v10's selectedFile)
    size_t logo_bytes;
    // the drop target's own state (svelte-145grbc): has-file with the file's name, or its error
    int logo_has_file;
    char logo_name[128], logo_drop_err[96];
    // transfer ownership
    int transfer_busy;
    char transfer_err[96], transfer_name[40];
} S = {.banner_sel = -1};
_Static_assert(sizeof S.motd_text > 500 * 3 && sizeof S.r_looking > 500 * 3 && sizeof S.r_desc > 2000 * 3,
               "a clan message keeps every character the box allows");

// Box Office: the file this client uploaded for the clan, shown as its pending logo while the review runs
static char *g_logo_sent;    // "data:image/png;base64,..." (owned), NULL none
static int g_logo_sent_clan; // the clan it was sent for

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsString(v)) return atof(v->valuestring);
    if (cJSON_IsTrue(v)) return 1;
    return 0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
    return s ? s : "";
}

// UTILS.commaNumber
static void comma(char *out, const size_t n, const double value) {
    char digits[32];
    snprintf(digits, sizeof(digits), "%.0f", value);
    const char *d = digits;
    size_t o = 0;
    if (*d == '-' && o + 1 < n) out[o++] = *d++;
    const size_t len = strlen(d);
    for (size_t i = 0; i < len && o + 2 < n; i++) {
        if (i && (len - i) % 3 == 0) out[o++] = ',';
        out[o++] = d[i];
    }
    out[o] = 0;
}

static int my_id(void) {
    const AccountInfo *a = active_account();
    return a ? a->id : 0;
}

static int clan_id(void) { return g_clan ? (int) jnum(g_clan, "clan_id") : 0; }

static int i_am_leader(void) { return g_clan && my_id() && (int) jnum(g_clan, "clan_playerid") == my_id(); }

// acc.clanRole: the account's rank in clanData.members, -1 when not listed
static int my_role(void) {
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(g_clan, "members")) if ((int) jnum(m, "i") == my_id() && my_id()) return (int) jnum(m, "r");
    return -1;
}

static int is_mgr(void) { return i_am_leader() || my_role() == 2; }
static int is_mgr2(void) { return i_am_leader() || my_role() >= 2; }

// 1 = sent; 0 = not sent (no socket, the brain is gone): no answer can come
static int send_cln(const int action, cJSON *value, cJSON *extra) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("cln"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(action));
    cJSON_AddItemToArray(f, value ? value : cJSON_CreateNumber(1));
    if (extra) cJSON_AddItemToArray(f, extra);
    const int sent = backend_social_send(f);
    cJSON_Delete(f);
    return sent;
}

static void settings_reset(void) {
    html_timer_cancel(S.motd_timer);
    html_timer_cancel(S.loot_timer);
    html_timer_cancel(S.recruit_timer);
    cJSON_Delete(S.motd);
    cJSON_Delete(S.loot);
    cJSON_Delete(S.recruit);
    free(S.logo_file);
    memset(&S, 0, sizeof(S));
    S.banner_sel = -1;
    snprintf(S.r_level, sizeof(S.r_level), "0"); // the recruitment form's initial min level / K/D
    snprintf(S.r_kd, sizeof(S.r_kd), "0");
}

static void logo_sent_clear(void) {
    free(g_logo_sent);
    g_logo_sent = NULL;
    g_logo_sent_clan = 0;
}

// A mount of the open tab's components (showWindow / updateWindow(13)): the settings tab's sections (tab 4) and the
// home tab's loot display (tab 0) start again with their initial state and load again. A load still in flight
// keeps its answer (it is not asked twice).
static void components_mount(void) {
    if (g_clan_tab == 0 && !S.loot_loading) {
        cJSON_Delete(S.loot);
        S.loot = NULL;
        S.loot_failed = 0;
        S.loot_err[0] = S.loot_mode[0] = 0;
    }
    if (g_clan_tab != 4) return;
    const int motd_loading = S.motd_loading, motd_timer = S.motd_timer, loot_loading = S.loot_loading,
              loot_timer = S.loot_timer, recruit_loading = S.recruit_loading, recruit_timer = S.recruit_timer;
    const int64_t motd_for = S.motd_for;
    cJSON_Delete(S.motd);
    cJSON_Delete(S.loot);
    cJSON_Delete(S.recruit);
    free(S.logo_file);
    memset(&S, 0, sizeof(S));
    S.banner_sel = -1;
    snprintf(S.r_level, sizeof(S.r_level), "0");
    snprintf(S.r_kd, sizeof(S.r_kd), "0");
    S.motd_loading = motd_loading, S.motd_timer = motd_timer, S.loot_loading = loot_loading, S.loot_timer = loot_timer;
    S.recruit_loading = recruit_loading, S.recruit_timer = recruit_timer;
    if (motd_loading) S.motd_for = motd_for;
}

void clan_ui_reset(void) {
    html_timer_cancel(g_clan_timer);
    g_clan_timer = 0;
    g_clan_failed = 0;
    cJSON_Delete(g_clan);
    g_clan = NULL;
    g_clan_tab = 0;
    g_req_sent = 0;
    settings_reset();
    logo_sent_clear();
}

// The active account changed (account_ui.c active_changed): its clanData goes. A --render-scenario's state.clan
// stays: the scenario's account is applied after the first frame, which counts as a change of account.
void clan_ui_account_changed(void) {
    if (!g_scenario) clan_ui_reset();
}

// a name inside onclick='openPlayerProfile("NAME")': quotes backslash-escaped, then HTML-escaped
static void sb_js_name(StrBuf *sb, const char *name) {
    char buf[96];
    size_t o = 0;
    for (const char *p = name; *p && o + 3 < sizeof(buf); p++) {
        if (*p == '\'' || *p == '"' || *p == '\\') buf[o++] = '\\';
        buf[o++] = *p;
    }
    buf[o] = 0;
    sb_append_esc(sb, buf);
}

// premium / verified icons + the profile link (v10: openPlayerProfile), shared by member rows
static void member_name(StrBuf *sb, const cJSON *m) {
    if (jnum(m, "su")) sb_append(sb, "<i class='material-icons' style='color:" COL_PREMIUM ";margin-top:-8px;font-size:28px;vertical-align:middle;margin-right:2px'>beenhere</i>");
    if (jnum(m, "ve")) sb_append(sb, "<i class='material-icons' style='color:" COL_VERIFIED_PLAYER ";margin-top:4px;font-size:28px;vertical-align:baseline;margin-right:2px'>check_circle</i>");
    sb_append(sb, "<a href='javascript:void(0)' onclick='openPlayerProfile(\"");
    sb_js_name(sb, jstr(m, "p"));
    sb_append(sb, jnum(m, "h") ? "\")' style='color:#E85656'>" : "\")' >");
    sb_append_esc(sb, jstr(m, "p"));
    sb_append(sb, "</a>");
}

static int cmp_desc(const cJSON *a, const cJSON *b, const char *key) {
    const double x = jnum(a, key), y = jnum(b, key);
    return x < y ? 1 : x > y ? -1 : 0;
}

// members sorted by `key` descending (Array.sort b - a, stable), borrowed pointers
static int sorted_members(const cJSON *members, const cJSON **out, const int max, const char *key) {
    int n = 0;
    const cJSON *m;
    cJSON_ArrayForEach(m, members) if (n < max) out[n++] = m;
    for (int i = 1; i < n; i++) {
        const cJSON *v = out[i];
        int j = i - 1;
        while (j >= 0 && cmp_desc(out[j], v, key) > 0) {
            out[j + 1] = out[j];
            j--;
        }
        out[j + 1] = v;
    }
    return n;
}

#define CLAN_MAX_MEMBERS 256

// ------------------------------------------------------------------ async data (settings / tab 0)

// a Svelte component's own state changed: the window is drawn again, its components keep their state
static int clan_rerender(void) {
    const int was = g_svelte;
    g_svelte = 1;
    menu_rerender_if_open(13);
    g_svelte = was;
    return 1;
}

// "Server error: <statusText>" (v10's gapi wrapper throws it for a 5xx, deob-10.0.7.js:26157)
static void server_error(char *out, const size_t n, const int status) {
    const char *t = status == 500 ? "Internal Server Error" : status == 501 ? "Not Implemented" : status == 502 ? "Bad Gateway"
                  : status == 503 ? "Service Unavailable" : status == 504 ? "Gateway Timeout" : "";
    snprintf(out, n, "Server error: %s", t);
}

// A settings load's failure text as v10's component shows it: no answer is the fetch's TypeError, a 5xx the wrapper's
// HttpError, else the answer's error.message || error || the wrapper's fallback (deob-10.0.7.js:96419-96430, :27774)
static void load_error(char *out, const size_t n, const int status, const char *message, const char *fallback) {
    if (status <= 0) snprintf(out, n, FETCH_FAILED);
    else if (status >= 500) server_error(out, n, status);
    else snprintf(out, n, "%s", message && message[0] ? message : fallback);
}

// the clan's own load (sendReq(3)) got no answer by the deadline: the window says so, and Retry asks again
static void clan_expired(void *user) {
    (void) user;
    g_clan_timer = 0;
    if (g_clan || !g_req_sent) return;
    g_req_sent = 0;
    g_clan_failed = 1;
    clan_rerender();
}

// the MOTD's load failed (a refusal, no answer, not sent): v10's catch shows the error above the section's form
// (deob-10.0.7.js:96429-96430, :96443-96448)
static void motd_load_fail(const int status, const char *message) {
    html_timer_cancel(S.motd_timer);
    S.motd_timer = 0;
    S.motd_loading = 0;
    if (!S.motd) S.motd = cJSON_CreateObject();
    load_error(S.motd_err, sizeof(S.motd_err), status, message, "Failed to fetch MOTD");
}

static void motd_expired(void *user) {
    (void) user;
    S.motd_timer = 0;
    if (!S.motd_loading) return;
    motd_load_fail(0, NULL);
    clan_rerender();
}

// error.message || error (a string) of a gapi answer, NULL when neither
static const char *gapi_message(const cJSON *error) {
    if (cJSON_IsString(error)) return error->valuestring;
    const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(error, "message"));
    return m && m[0] ? m : NULL;
}

static void saved_off(void *user);

static int kio_clan(void) { return account_backend() == ACCOUNT_BACKEND_KRUNKER; }

// acc.clan / acc.funds as windows[12] sets them, on the account the window belongs to
static void set_acc_clan(const char *tag) {
    if (kio_clan()) account_kio_set_clan(tag);
    else account_set_clan(tag);
}

static void set_acc_funds(const double funds) {
    if (kio_clan()) account_kio_set_balances(1, funds, 0, 0);
    else account_set_funds(funds);
}

// a krunker.io answer's error text as v10's callers throw it: error.message, else the fallback
static const char *kio_error_text(const cJSON *error, const char *fallback) {
    const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(error, "message"));
    return e && e[0] ? e : fallback;
}

// A gapi answer that succeeded. krunker.io: v10.0.7's wrapper (deob-10.0.7.js:26152) gives {data, error} for any
// status in [200, 500) and its callers throw only on error, so a 204 (no body) or a 4xx without an error is a success
// there. Box Office: the brain's gapi tunnel answers 200.
static int gapi_ok(const int status, const cJSON *error) {
    if (kio_clan()) return status >= 200 && status < 500 && !error;
    return status == 200 && !error;
}

static void on_motd(void *user, const int status, const cJSON *data, const cJSON *error) {
    const int kind = (int) (intptr_t) user; // 0 load, 1 save, 2 remove
    if (kind == 0) {
        html_timer_cancel(S.motd_timer);
        S.motd_timer = 0;
    }
    S.motd_loading = 0;
    S.motd_busy = 0;
    if (gapi_ok(status, error)) {
        if (kind == 2) { // text '', set_by / set_at null (deob-10.0.7.js:96403-96405)
            cJSON_Delete(S.motd);
            S.motd = cJSON_CreateObject();
        } else {
            cJSON_Delete(S.motd);
            S.motd = data ? cJSON_Duplicate(data, 1) : cJSON_CreateObject();
        }
        snprintf(S.motd_text, sizeof(S.motd_text), "%s", jstr(S.motd, "message"));
        S.motd_err[0] = 0;
        if (kind == 1) { // "Saved!" for 3 s from the answer (setTimeout 3000, :96383-96384)
            S.motd_saved = 1;
            html_timer_add(3000, saved_off, (void *) 1);
        }
    } else if (kind == 0) {
        motd_load_fail(status, gapi_message(error));
    } else {
        static const char *const fails[] = {"Failed to load MOTD", "Failed to set MOTD", "Failed to delete MOTD"};
        load_error(S.motd_err, sizeof(S.motd_err), status, gapi_message(error), fails[kind]);
        if (!S.motd) S.motd = cJSON_CreateObject();
    }
    clan_rerender();
}

static void fetch_motd(void) {
    if (g_scenario || !g_clan || S.motd_loading || (S.motd && S.motd_for == clan_id())) return;
    char path[64];
    snprintf(path, sizeof(path), "/clans/%d/motd", clan_id());
    S.motd_for = clan_id();
    S.motd_err[0] = 0;
    S.motd_loading = v10_gapi("GET", path, NULL, on_motd, (void *) 0) != 0;
    if (S.motd_loading) {
        html_timer_cancel(S.motd_timer);
        S.motd_timer = html_timer_add(CLAN_WAIT_MS, motd_expired, NULL);
    } else {
        motd_load_fail(0, NULL); // not sent: no answer can come (no re-render here: this runs while the window renders)
    }
}

// v10's `x ?? fallback`: a missing or null field is the fallback
static int num_or(const cJSON *o, const char *k, const int fallback) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return v && !cJSON_IsNull(v) ? (int) jnum(o, k) : fallback;
}

// the loot config form's state from a config (deob-10.0.7.js:96687-96692); an empty config is the form's defaults
static void loot_from(const cJSON *cfg) {
    const char *mode = jstr(cfg, "distribution_mode");
    snprintf(S.loot_mode, sizeof(S.loot_mode), "%s", mode[0] ? mode : "clan_bank");
    S.loot_clan_pct = num_or(cfg, "clan_pct", 0);
    S.loot_top_count = num_or(cfg, "top_player_count", 20);
    S.loot_top_pct = num_or(cfg, "top_player_pct", 80);
    S.loot_rem = num_or(cfg, "remainder_pct", 20);
    S.loot_min_games = num_or(cfg, "min_games_required", 1);
}

// turf.c answers {data: {config: [row]}} on GET (row.updated_by = the name) and {data: {...}} on
// PUT; v10's gapi answers a flat data {..., updated_by_name, updated_at}: both are read
static const cJSON *loot_row(const cJSON *data) {
    const cJSON *cfg = cJSON_GetArrayItem(cJSON_GetObjectItem(data, "config"), 0);
    return cfg ? cfg : data;
}

static void on_loot(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    const int saving = (int) (intptr_t) user;
    S.loot_loading = 0;
    S.loot_saving = 0;
    if (!saving) {
        html_timer_cancel(S.loot_timer);
        S.loot_timer = 0;
    }
    cJSON *res = body ? cJSON_Parse(body) : NULL;
    const cJSON *data = cJSON_GetObjectItem(res, "data");
    if (status == 200 && cJSON_IsObject(data)) {
        const cJSON *row = loot_row(data);
        if (saving) { // keep the form's values; take the "last updated" fields of the answer
            cJSON *cfg = S.loot ? S.loot : cJSON_CreateObject();
            S.loot = cfg;
            const char *by = jstr(row, "updated_by_name")[0] ? jstr(row, "updated_by_name") : jstr(row, "updated_by");
            cJSON_DeleteItemFromObject(cfg, "updated_by_name");
            cJSON_AddStringToObject(cfg, "updated_by_name", by);
            cJSON_DeleteItemFromObject(cfg, "updated_at");
            if (cJSON_GetObjectItem(row, "updated_at")) cJSON_AddItemToObject(cfg, "updated_at", cJSON_Duplicate(cJSON_GetObjectItem(row, "updated_at"), 1));
            S.loot_saved = 1;
            S.loot_err[0] = 0;
            html_timer_add(3000, saved_off, (void *) 2); // "Config saved successfully!" for 3 s (:96669-96672)
        } else {
            cJSON_Delete(S.loot);
            S.loot = cJSON_Duplicate(row, 1);
            S.loot_failed = 0;
            if (!jstr(S.loot, "updated_by_name")[0] && jstr(S.loot, "updated_by")[0])
                cJSON_AddStringToObject(S.loot, "updated_by_name", jstr(S.loot, "updated_by"));
            loot_from(S.loot);
        }
    } else {
        // error.message || error || the wrapper's fallback (deob-10.0.7.js:27777, :96656), a 5xx or no answer as
        // v10's fetch fails
        const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(res, "error"), "message"));
        if (!e || !e[0]) e = cJSON_GetStringValue(cJSON_GetObjectItem(res, "error"));
        if (!e || !e[0]) e = cJSON_GetStringValue(cJSON_GetObjectItem(res, "message"));
        load_error(S.loot_err, sizeof(S.loot_err), status, e, saving ? "Failed to update loot config" : "Failed to fetch loot config");
        if (!saving) { // the form keeps its defaults, with the error under it; the tab 0 display treats it as null
            S.loot_failed = 1;
            if (!S.loot) {
                S.loot = cJSON_CreateObject();
                loot_from(S.loot);
            }
        }
    }
    cJSON_Delete(res);
    clan_rerender();
}

static void on_recruit(void *user, int status, const char *body, size_t length);

// krunker.io account: the answer of v10's clans.getLootConfig / getRecruitment (gapi, deob-10.0.7.js:26852, :26894) and of
// its updateLootConfig / createOrUpdateRecruitment / removeRecruitment (:26855, :26897, :26903), rebuilt as the kn_brain
// route's body for the same handler. user: 0 loot config load, 1 recruitment load, 2 loot config save, 3 recruitment
// save, 4 recruitment removal. A success by v10's rule (gapi_ok) is the handler's 200. v10 reads a failed recruitment
// load as "no post" (its catch sets the post to null, :96880-96891), so an answered failure is the handler's 404 there.
static void kio_settings_read(void *user, const int status, const cJSON *data, const cJSON *error) {
    const int kind = (int) (intptr_t) user;
    cJSON *res = cJSON_CreateObject();
    if (data) cJSON_AddItemToObject(res, "data", cJSON_Duplicate(data, 1));
    if (error) cJSON_AddItemToObject(res, "error", cJSON_Duplicate(error, 1));
    char *body = cJSON_PrintUnformatted(res);
    int st = gapi_ok(status, error) ? 200 : status;
    if (kind == 1 && st != 200 && status > 0) st = 404;
    const size_t len = body ? strlen(body) : 0;
    if (kind == 0 || kind == 2) on_loot((void *) (intptr_t) (kind == 2), st, body, len);
    else on_recruit((void *) (intptr_t) (kind == 1 ? 0 : kind - 2), st, body, len);
    free(body);
    cJSON_Delete(res);
}

// the loot config's load got no answer or was not sent: v10's fetch failure (the form's defaults and the error)
static void loot_load_fail(void) {
    html_timer_cancel(S.loot_timer);
    S.loot_timer = 0;
    S.loot_loading = 0;
    S.loot_failed = 1;
    snprintf(S.loot_err, sizeof(S.loot_err), FETCH_FAILED);
    if (!S.loot) {
        S.loot = cJSON_CreateObject();
        loot_from(S.loot);
    }
}

static void loot_expired(void *user) {
    (void) user;
    S.loot_timer = 0;
    if (!S.loot_loading) return;
    loot_load_fail();
    clan_rerender();
}

static void fetch_loot(void) {
    if (g_scenario || S.loot || S.loot_loading || !g_clan) return;
    int sent = 0;
    S.loot_failed = 0;
    if (kio_clan()) {
        char path[64];
        snprintf(path, sizeof(path), "/clans/%d/loot-config", clan_id());
        sent = v10_gapi("GET", path, NULL, kio_settings_read, (void *) 0) != 0;
    } else if (backend_available() == 1) {
        char path[96];
        snprintf(path, sizeof(path), "/api/clans/%d/loot-config", clan_id());
        sent = backend_http_get(path, on_loot, (void *) 0) != 0;
    }
    if (!sent) { // not sent (the brain is not up): no answer can come
        loot_load_fail();
        return;
    }
    S.loot_loading = 1;
    html_timer_cancel(S.loot_timer);
    S.loot_timer = html_timer_add(CLAN_WAIT_MS, loot_expired, NULL);
}

static void on_recruit(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    const int kind = (int) (intptr_t) user; // 0 load, 1 save, 2 remove
    S.recruit_loading = 0;
    S.recruit_saving = 0;
    cJSON *res = body ? cJSON_Parse(body) : NULL;
    const cJSON *data = cJSON_GetObjectItem(res, "data");
    const int ok = status >= 200 && status < 300 && !cJSON_IsObject(cJSON_GetObjectItem(res, "error")) && !cJSON_IsString(cJSON_GetObjectItem(res, "error"));
    if (kind == 0) {
        html_timer_cancel(S.recruit_timer);
        S.recruit_timer = 0;
        S.recruit_loaded = 1;
        // v10's catch reads any failed load as "no post" (deob-10.0.7.js:96880-96891)
        cJSON_Delete(S.recruit);
        S.recruit = ok && cJSON_IsObject(data) ? cJSON_Duplicate(data, 1) : NULL;
        if (S.recruit) {
            snprintf(S.r_desc, sizeof(S.r_desc), "%s", jstr(S.recruit, "description"));
            snprintf(S.r_looking, sizeof(S.r_looking), "%s", jstr(S.recruit, "looking_for"));
            snprintf(S.r_level, sizeof(S.r_level), "%g", jnum(S.recruit, "min_level"));
            snprintf(S.r_kd, sizeof(S.r_kd), "%g", jnum(S.recruit, "min_kd"));
            snprintf(S.r_region, sizeof(S.r_region), "%s", jstr(S.recruit, "region"));
            snprintf(S.r_lang, sizeof(S.r_lang), "%s", jstr(S.recruit, "language"));
        }
    } else if (!ok) {
        const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(res, "error"), "message"));
        if (!e) e = cJSON_GetStringValue(cJSON_GetObjectItem(res, "error"));
        load_error(S.recruit_err, sizeof(S.recruit_err), status, e, kind == 1 ? "Failed to save recruitment post" : "Failed to remove recruitment post");
        S.recruit_ok[0] = 0;
    } else if (kind == 1) {
        const int had = S.recruit && jnum(S.recruit, "is_active");
        snprintf(S.recruit_ok, sizeof(S.recruit_ok), "%s", had ? "Recruitment post updated!" : "Recruitment post created! (1,000 KR deducted)");
        S.recruit_err[0] = 0;
        cJSON_Delete(S.recruit);
        S.recruit = cJSON_IsObject(data) ? cJSON_Duplicate(data, 1) : cJSON_CreateObject();
        if (!cJSON_GetObjectItem(S.recruit, "is_active")) cJSON_AddBoolToObject(S.recruit, "is_active", 1);
    } else {
        cJSON_Delete(S.recruit);
        S.recruit = NULL;
        S.r_desc[0] = S.r_looking[0] = S.r_region[0] = S.r_lang[0] = 0;
        snprintf(S.r_level, sizeof(S.r_level), "0");
        snprintf(S.r_kd, sizeof(S.r_kd), "0");
        snprintf(S.recruit_ok, sizeof(S.recruit_ok), "Recruitment post removed.");
        S.recruit_err[0] = 0;
    }
    cJSON_Delete(res);
    clan_rerender();
}

static void recruit_url(char *out, const size_t n) {
    char token[256];
    backend_url_encode(token, sizeof(token), account_session_token());
    snprintf(out, n, "%s/api/clans/%d/recruitment?token=%s", backend_url(), clan_id(), token);
}

// the recruitment load got no answer or was not sent: "no post", as v10's catch reads any failure
static void recruit_load_fail(void) {
    html_timer_cancel(S.recruit_timer);
    S.recruit_timer = 0;
    S.recruit_loading = 0;
    S.recruit_loaded = 1;
    cJSON_Delete(S.recruit);
    S.recruit = NULL;
}

static void recruit_expired(void *user) {
    (void) user;
    S.recruit_timer = 0;
    if (!S.recruit_loading) return;
    recruit_load_fail();
    clan_rerender();
}

static void fetch_recruit(void) {
    if (g_scenario || S.recruit_loaded || S.recruit_loading || !g_clan) return;
    int sent = 0;
    if (kio_clan()) {
        char path[64];
        snprintf(path, sizeof(path), "/clans/%d/recruitment", clan_id());
        sent = v10_gapi("GET", path, NULL, kio_settings_read, (void *) 1) != 0;
    } else if (backend_available() == 1) {
        char url[600];
        recruit_url(url, sizeof(url));
        sent = backend_http_request("GET", url, NULL, on_recruit, (void *) 0) != 0;
    }
    if (!sent) { // not sent (the brain is not up): no answer can come
        recruit_load_fail();
        return;
    }
    S.recruit_loading = 1;
    html_timer_cancel(S.recruit_timer);
    S.recruit_timer = html_timer_add(CLAN_WAIT_MS, recruit_expired, NULL);
}

// ------------------------------------------------------------------ tab 0 (home)

// Date.toLocaleDateString() en-US of epoch ms: M/D/YYYY (UTC; civil date from days since the epoch)
static void locale_date(const double ms, char *out, const size_t n) {
    long long z = (long long) floor(ms / 86400000.0) + 719468;
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
    const int day = (int) (doy - (153 * mp + 2) / 5 + 1), mon = (int) (mp < 10 ? mp + 3 : mp - 9);
    const int year = (int) (yoe + era * 400 + (mon <= 2));
    snprintf(out, n, "%d/%d/%d", mon, day, year);
}

// clan-ownership-transfer-banner (_33655, svelte-1xrfe28): the transfer's target only
static void gen_transfer_banner(StrBuf *sb) {
    const cJSON *t = cJSON_GetObjectItem(g_clan, "ownership_transfer");
    sb_append(sb, "<div id=\"clan-ownership-transfer-banner\">");
    if (cJSON_IsObject(t) && (int) jnum(t, "to_player_id") == my_id() && my_id()) {
        sb_append(sb, "<div class=\"clan-ownership-transfer-banner svelte-1xrfe28\"><p class=\"svelte-1xrfe28\"><span class=\"clan-ownership-transfer-banner__player-name svelte-1xrfe28\">");
        sb_append_esc(sb, jstr(t, "from_player_name"));
        sb_append(sb, "</span> wants to transfer ownership of <span class=\"clan-ownership-transfer-banner__clan-name svelte-1xrfe28\">");
        sb_append_esc(sb, jstr(g_clan, "clan_name")); // clanData.clan_name (deob-10.0.3.js:110218)
        sb_append(sb, "</span> to you.</p> <button class=\"main-button svelte-whnh40 main-button-accept\" type=\"button\" onclick=\"windows[12].transferRespond(1)\">Accept</button> "
                      "<button class=\"main-button svelte-whnh40 main-button-danger\" type=\"button\" onclick=\"windows[12].transferRespond(0)\">Decline</button></div>");
    }
    sb_append(sb, "</div>");
}

static int loot_mode_index(const char *mode) {
    for (int i = 0; i < LOOT_MODES; i++)
        if (!strcmp(mode, k_loot_modes[i].mode)) return i;
    return 0;
}

// clan-loot-mode-display (_33678): every member
static void gen_loot_display(StrBuf *sb) {
    fetch_loot();
    sb_append(sb, "<div id='clan-loot-mode-display'><div class=\"setHed\" style=\"margin-top:15px;font-size:16px\">Turf Wars Loot</div> <div class=\"setBodH\">");
    const cJSON *cfg = S.loot_failed ? NULL : S.loot;
    if (!S.loot && !g_scenario && S.loot_loading) {
        sb_append(sb, "<div class=\"settName\" style=\"font-size:16px\">Loading...</div>");
    } else if (!cfg || !strcmp(S.loot_mode, "clan_bank") || !S.loot_mode[0]) {
        sb_append(sb, "<div class=\"settName\" style=\"font-size:16px;border-bottom:none\">Mode<span class=\"floatR\">Clan Bank</span></div>");
    } else {
        const char *mode = jstr(cfg, "distribution_mode");
        sb_appendf(sb, "<div class=\"settName\" style=\"font-size:16px\">Mode<span class=\"floatR\">%s</span></div>", k_loot_modes[loot_mode_index(mode)].short_label);
        if (jnum(cfg, "clan_pct") > 0) sb_appendf(sb, " <div class=\"settName\" style=\"font-size:16px\">Clan Cut<span class=\"floatR\">%d%%</span></div>", (int) jnum(cfg, "clan_pct"));
        if (!strcmp(mode, "top_weighted"))
            sb_appendf(sb, " <div class=\"settName\" style=\"font-size:16px\">Top Players<span class=\"floatR\">%d get %d%%</span></div>", (int) jnum(cfg, "top_player_count"), (int) jnum(cfg, "top_player_pct"));
        if (!strcmp(mode, "top_performance"))
            sb_appendf(sb, " <div class=\"settName\" style=\"font-size:16px\">Top Players<span class=\"floatR\">top %d by kills</span></div>", (int) jnum(cfg, "top_player_count"));
        if (jnum(cfg, "min_games_required") > 1)
            sb_appendf(sb, " <div class=\"settName\" style=\"font-size:16px;border-bottom:none\">Min Games<span class=\"floatR\">%d</span></div>", (int) jnum(cfg, "min_games_required"));
    }
    sb_append(sb, "</div></div>");
}

static void gen_home(StrBuf *sb) {
    const AccountInfo *a = active_account();
    const int leader = i_am_leader();
    const double score = jnum(g_clan, "clan_score");
    const double lv = CLAN_RANK_VAR * sqrt(score);
    int level = (int) floor(lv);
    const int prog = (int) round(100 * (lv - level));
    if (level < 1) level = 1;
    const double base = level > 1 ? pow(level / CLAN_RANK_VAR, 2) : 0;
    char funds[32], total[32], cur[32], need[32];
    comma(funds, sizeof(funds), jnum(g_clan, "clan_funds"));
    comma(total, sizeof(total), score);
    comma(cur, sizeof(cur), (double) (long long) (score - base));
    comma(need, sizeof(need), (double) (long long) (pow((level + 1) / CLAN_RANK_VAR, 2) - base));
    gen_transfer_banner(sb);
    sb_append(sb, "<div class='setHed' style='margin-top:0px'><a href='javascript:void(0)' onclick='window.openClanView(&quot;");
    char tag[16] = "";
    size_t o = 0;
    for (const char *p = a ? a->clan : ""; *p && o + 1 < sizeof(tag); p++)
        if (*p != '"' && *p != '\'') tag[o++] = *p;
    tag[o] = 0;
    sb_append_esc(sb, tag);
    sb_append(sb, "&quot;)'>");
    StrBuf esc = {0};
    sb_append_esc(&esc, a ? a->clan : "");
    char *etag = sb_finish(&esc);
    char page[160];
    I18N_F(page, "windows.clans.page", "[{0}] Clan Page", etag);
    free(etag);
    sb_appendf(sb, "%s</a><a style='float:right' href='javascript:;' onclick='windows[12].delReq()' class='menuLink'>%s</a></div>", page,
               leader ? I18N("generic.delete", "Delete") : I18N("generic.leave", "Leave"));
    sb_append(sb, "<div class='setBodH'><div class='settName'>Rank<span class='floatR'>0</span></div>");
    sb_appendf(sb, "<div class='settName'>Clan Discord<a style='float:right' href='javascript:;' onclick='windows[12].discordPop()' class='menuLink'>%s</a></div>",
               leader ? "Update Link" : "Link Here");
    sb_appendf(sb, "<div class='settName'>Bank<span class='floatR'><span style='color:rgba(0,0,0,0.4)'><a href='javascript:;'  onclick='windows[12].mangFunds()' class='menuLink'>%s</a></span> KR</span></div>", funds);
    sb_appendf(sb, "<div class='settName'>%s<span class='floatR'>%d</span></div><div class='settName'>Total Score<span class='floatR'>%s</span></div>",
               I18N("generic.level", "Level"), level, total);
    sb_appendf(sb, "<div class='xpBar' style='margin-bottom:15px;margin-top:20px'><div class='xpBarB' style='width:%d%%'></div><span class='xpBarV'>%s / %s</span></div></div>", prog, cur, need);
    gen_loot_display(sb);
    sb_append(sb, "<div style=\"text-align: center;\">\n\t\t\t\t<span class=\"material-icons\" style=\"color: var(--yellow);font-size: 35px;margin-right: 10px;vertical-align: text-bottom;\">\n\t\t\t\t\twarning_amber\n\t\t\t\t</span>\n\t\t\t\tClan members will be limited to 100 in a Future Season\n\t\t\t</div>");
}

// ------------------------------------------------------------------ tabs 1-3

// tab 1: one section per role, Commander first (the creator is the Commander)
static void gen_members(StrBuf *sb) {
    const cJSON *mem[CLAN_MAX_MEMBERS];
    const int n = sorted_members(cJSON_GetObjectItem(g_clan, "members"), mem, CLAN_MAX_MEMBERS, "s");
    const int leader_id = (int) jnum(g_clan, "clan_playerid");
    const int mgr = is_mgr(), me = my_id(), captain = my_role() == 2 && !i_am_leader();
    for (int r = CLAN_ROLES - 1; r >= 0; r--) {
        int count = 0;
        StrBuf rows = {0};
        for (int i = 0; i < n; i++) {
            const cJSON *m = mem[i];
            const int id = (int) jnum(m, "i");
            // members[j].r == role (a member pushed by an accepted request has no r yet and is in no section)
            const cJSON *mr_v = cJSON_GetObjectItem(m, "r");
            const int has_r = mr_v && !cJSON_IsNull(mr_v);
            if (!((leader_id == id && k_clan_roles[r].perms == 2) || (has_r && (int) jnum(m, "r") == r && leader_id != id))) continue;
            count++;
            sb_append(&rows, "<div class='settName'>");
            member_name(&rows, m);
            const int mr = (int) jnum(m, "r");
            if (mgr && leader_id != id && (!captain || (id != me && mr != 2))) {
                sb_append(&rows, "<div style='float:right;'><i style='font-size:40px;color:rgba(255,0,0,0.9)'  class='material-icons h' onclick='windows[12].kickReq(&quot;");
                sb_append_esc(&rows, jstr(m, "p"));
                sb_appendf(&rows, "&quot;,%d)'>clear</i><select class='clanRolD' onchange='windows[12].updRol(%d, this)' id='clanRolD%d'>", id, id, id);
                for (int o = CLAN_ROLES - 1; o >= 0; o--) {
                    if (k_clan_roles[o].perms == 2 || (captain && o == 2)) continue;
                    sb_appendf(&rows, "<option value='%d' %s>%s</option>", o, mr == o ? "selected" : "", k_clan_roles[o].name);
                }
                sb_append(&rows, "</select></div>");
            }
            sb_append(&rows, "</div>");
        }
        char *body = sb_finish(&rows);
        sb_appendf(sb, "<div class='setHed' style='margin-top:20px'>%ss<span style='float:right;color:rgba(255,255,255,0.4)'>%d", k_clan_roles[r].name, count);
        if (k_clan_roles[r].max) sb_appendf(sb, "/%d", k_clan_roles[r].max);
        sb_append(sb, " <span style='color:#fff'>Members</span></span></div>");
        if (body[0]) sb_appendf(sb, "<div class='setBodH'>%s</div>", body);
        else sb_appendf(sb, "No %ss", k_clan_roles[r].name);
        free(body);
    }
}

// tab 2: Top Scorers (s7 = score of the last 7 days)
static void gen_stats(StrBuf *sb) {
    const cJSON *mem[CLAN_MAX_MEMBERS];
    const int n = sorted_members(cJSON_GetObjectItem(g_clan, "members"), mem, CLAN_MAX_MEMBERS, "s7");
    sb_append(sb, "<div class='setHed' style='margin-top:10px;'>Top Scorers <span style='float:right;color:rgba(255,255,255,0.4)'>Last 7 Days</span></div><div class='setBodH'>");
    for (int i = 0; i < n; i++) {
        char v[32];
        comma(v, sizeof(v), jnum(mem[i], "s7"));
        sb_append(sb, "<div class='settName'>");
        member_name(sb, mem[i]);
        sb_appendf(sb, "<div style='float:right;color:rgba(255,255,255,0.8)'>%s</div></div>", v);
    }
    sb_append(sb, "</div>");
}

// tab 3: Manage Requests
static void gen_requests(StrBuf *sb) {
    const cJSON *reqs = cJSON_GetObjectItem(g_clan, "requests");
    const int mgr = is_mgr();
    sb_append(sb, "<div class='setHed' style='margin-top:18px'>Manage Requests ");
    if (mgr) sb_append(sb, "<div id='clanDeclineAll' onclick='windows[12].declinePop()'>Decline All</div>");
    sb_appendf(sb, "<span style='float:right;color:rgba(255,255,255,0.4)'>%d</span></div><div class='setBodH'>", cJSON_GetArraySize(reqs));
    int i = 0;
    const cJSON *q;
    cJSON_ArrayForEach(q, reqs) {
        const int pid = (int) jnum(q, "cr_playerid");
        sb_append(sb, "<div class='settName'><a href='javascript:void(0)' onclick='openPlayerProfile(\"");
        sb_js_name(sb, jstr(q, "player_name"));
        sb_append(sb, "\")'>");
        sb_append_esc(sb, jstr(q, "player_name"));
        sb_append(sb, "</a>");
        if (mgr)
            sb_appendf(sb, "<span style='float:right'><i style='font-size:40px;color:rgba(255,0,0,0.9)'  class='material-icons h' onclick='windows[12].updReq(%d,%d, 0)'>clear</i><i style='font-size:40px;color:rgba(0,255,0,0.9);margin-left:10px' class='material-icons h' onclick='windows[12].updReq(%d,%d, 1)'>done</i></span>", i, pid, i, pid);
        sb_append(sb, "</div>");
        i++;
    }
    sb_append(sb, "</div>");
    if (!i) sb_appendf(sb, "<div style='margin-top:10px;color:rgba(255,255,255,0.4)'>%s</div>", I18N("windows.clans.requests.none", "No Requests"));
}

// ------------------------------------------------------------------ tab 4 (clan-settings)

// JS truthiness of a clanData field (a number other than 0, a non-empty string, true)
static int truthy(const cJSON *v) {
    if (cJSON_IsNumber(v)) return v->valuedouble != 0;
    if (cJSON_IsString(v)) return v->valuestring[0] != 0;
    return cJSON_IsTrue(v) || cJSON_IsObject(v) || cJSON_IsArray(v);
}

// a JSON value as JS writes it into a string (`${v}`)
static void json_text(const cJSON *v, char *out, const size_t n) {
    if (cJSON_IsString(v)) snprintf(out, n, "%s", v->valuestring);
    else if (cJSON_IsNumber(v)) snprintf(out, n, "%.17g", v->valuedouble);
    else snprintf(out, n, "%s", cJSON_IsTrue(v) ? "true" : "");
}

static int logo_pending(void) { return truthy(cJSON_GetObjectItem(g_clan, "clan_logo_pending")); }
static int logo_funds_ok(void) { return cJSON_IsNumber(cJSON_GetObjectItem(g_clan, "clan_funds")) && jnum(g_clan, "clan_funds") >= LOGO_COST; }

// v10's iíïiììï(clan_id, pending, version) (deob-10.0.7.js:27901): clans/<id>.png?v=<clan_logo> or
// clans/<id>_pending.png?v=<clan_logo_pending> on Krunker's user-assets host, while the field is set. Box Office:
// the approved logo is the brain's GET /api/clans/<id>/logo?v=<clan_logo> (clan_logo.c); a pending one is not kept by
// the brain, so it is the file this client sent, while it is pending. 0 = none (out untouched).
static int logo_src(StrBuf *out, const int pending) {
    const cJSON *v = cJSON_GetObjectItem(g_clan, pending ? "clan_logo_pending" : "clan_logo");
    if (!clan_id() || !truthy(v)) return 0;
    char ver[64];
    json_text(v, ver, sizeof(ver));
    if (kio_clan()) {
        sb_appendf(out, "https://user-assets.krunker.io/clans/%d%s.png?v=%s", clan_id(), pending ? "_pending" : "", ver);
    } else if (pending) {
        if (!g_logo_sent || g_logo_sent_clan != clan_id()) return 0;
        sb_append(out, g_logo_sent);
    } else {
        const char *base = backend_url();
        if (!base || !base[0]) return 0;
        sb_appendf(out, "%s/api/clans/%d/logo?v=%s", base, clan_id(), ver);
    }
    return 1;
}

// the buy button (iîíìiíi, deob-10.0.7.js:25837, svelte-1ov1s5h) at size medium for the logo's 20,000 KR; while
// purchasing, the hourglass and "Buying..."
static void logo_buy_button(StrBuf *sb, const int disabled, const int purchasing) {
    sb_appendf(sb, "<button class=\"buy-button svelte-1ov1s5h medium\" type=\"button\" tabindex=\"0\" style=\"padding: 8px 12px; font-size: 14px;\" onclick=\"windows[12].logoUpload()\"%s>",
               disabled ? " disabled" : "");
    if (purchasing) sb_append(sb, "<span class=\"material-icons svelte-1ov1s5h\" style=\"font-size: 24px;\">hourglass_empty</span> <span>Buying...</span>");
    else
        sb_append(sb, "BUY <div class=\"currency-display svelte-1r6bdx8\" style=\"--text-size: 14px; --icon-size: 16px;\"><span class=\"currency-amount svelte-1r6bdx8\" style=\"color: white;\">20,000</span> "
                      "<span class=\"currency-text svelte-1r6bdx8\" style=\"color: white;\">KR</span></div>");
    sb_append(sb, "</button>");
}

// the file drop target (iììiíïí, deob-10.0.7.js:11905-12030, svelte-145grbc) for a .png up to 256KB: has-file shows the
// picked file's name and its remove button, a refused file the target's own error
static void logo_drop_target(StrBuf *sb, const int disabled) {
    const char *label = I18N("windows.clans.settings.logo.upload.drop", "Drop PNG file here");
    const int has = S.logo_has_file, err = !has && S.logo_drop_err[0];
    sb_appendf(sb, "<div class=\"file-drop-target svelte-145grbc%s%s%s\" role=\"button\" tabindex=\"%d\" aria-label=\"%s\"%s><input type=\"file\" style=\"display: none;\" class=\"svelte-145grbc\" accept=\".png,image/png\"> <div class=\"file-drop-content svelte-145grbc\">",
               has ? " has-file" : "", err ? " error" : "", disabled ? " disabled" : "", disabled ? -1 : 0, label, disabled ? "" : " onclick=\"windows[12].logoPick()\"");
    if (has) {
        sb_append(sb, "<div class=\"file-success-state svelte-145grbc\"><i class=\"material-icons success-icon svelte-145grbc\">check_circle</i> <div class=\"file-name svelte-145grbc\">");
        sb_append_esc(sb, S.logo_name);
        sb_append(sb, "</div> <button class=\"remove-file-btn svelte-145grbc\" aria-label=\"Remove file\" onclick=\"event.stopPropagation();windows[12].logoUnset()\"><i class=\"material-icons svelte-145grbc\">close</i></button></div>");
    } else if (err) {
        sb_append(sb, "<div class=\"file-error-state svelte-145grbc\"><i class=\"material-icons error-icon svelte-145grbc\">error</i> <div class=\"error-message svelte-145grbc\">");
        sb_append_esc(sb, S.logo_drop_err);
        sb_append(sb, "</div> <div class=\"retry-hint svelte-145grbc\">Click to try again</div></div>");
    } else {
        sb_appendf(sb, "<div class=\"file-empty-state svelte-145grbc\"><i class=\"material-icons add-icon svelte-145grbc\">add</i> <div class=\"drop-label svelte-145grbc\">%s</div> <div class=\"accepted-types svelte-145grbc\">.png, image/png</div> <div class=\"size-limit svelte-145grbc\">Max: 256KB</div></div>",
                   label);
    }
    sb_append(sb, "</div></div>");
}

// the settings' Clan Logo section (deob-10.0.7.js:96234-96352): the current / pending logo, the upload (the drop target,
// then, with a file selected, the buy button and the funds warning) or the pending notice, Remove Logo, the error line
static void gen_logo(StrBuf *sb) {
    const int pending = logo_pending(), funds_ok = logo_funds_ok();
    StrBuf approved_src = {0}, pending_src = {0};
    const int approved = logo_src(&approved_src, 0), pend = logo_src(&pending_src, 1);
    char *approved_url = sb_finish(&approved_src), *pending_url = sb_finish(&pending_src);
    sb_appendf(sb, "<div class=\"setHed\" style=\"margin-top:20px\">%s</div> <div class=\"logo-upload-container svelte-1w0uz1r\"><div class=\"io-card\"><div class=\"io-input-container svelte-1w0uz1r\">",
               I18N("windows.clans.settings.logo.title", "Clan Logo"));
    // Box Office: a pending logo this client did not send has no picture; its status shows alone
    const int show_pending = pend || (pending && clan_id());
    if (approved || show_pending) {
        sb_appendf(sb, "<div class=\"logo-preview-section svelte-1w0uz1r\"><div class=\"label-row\"><span class=\"input-label\">%s</span></div> <div class=\"logo-preview-container svelte-1w0uz1r\">",
                   I18N("windows.clans.settings.logo.current", "Current Logo"));
        if (approved) {
            sb_append(sb, "<div class=\"logo-preview svelte-1w0uz1r\"><img alt=\"Clan logo\" class=\"logo-image svelte-1w0uz1r\" src=\"");
            sb_append_esc(sb, approved_url);
            sb_appendf(sb, "\"> <div class=\"logo-status approved svelte-1w0uz1r\">%s</div></div>", I18N("windows.clans.settings.logo.status.approved", "Approved"));
        } else {
            sb_append(sb, "<!---->");
        }
        sb_append(sb, " ");
        if (show_pending) {
            sb_append(sb, "<div class=\"logo-preview svelte-1w0uz1r\">");
            if (pend) {
                sb_append(sb, "<img alt=\"Pending logo\" class=\"logo-image svelte-1w0uz1r\" src=\"");
                sb_append_esc(sb, pending_url);
                sb_append(sb, "\"> ");
            }
            sb_appendf(sb, "<div class=\"logo-status pending svelte-1w0uz1r\">%s</div></div>", I18N("windows.clans.settings.logo.status.pending", "Pending Approval"));
        } else {
            sb_append(sb, "<!---->");
        }
        sb_append(sb, "</div></div> ");
    }
    free(approved_url);
    free(pending_url);
    if (!pending) {
        char funds[32] = "0";
        if (truthy(cJSON_GetObjectItem(g_clan, "clan_funds"))) comma(funds, sizeof(funds), jnum(g_clan, "clan_funds"));
        sb_appendf(sb, "<div class=\"logo-upload-section svelte-1w0uz1r\"><div class=\"label-row\"><span class=\"input-label\">%s</span></div> <div class=\"logo-upload-info svelte-1w0uz1r\"><p class=\"upload-requirements svelte-1w0uz1r\">%s</p> <p class=\"clan-funds svelte-1w0uz1r\">%s: \n                %s KR</p></div> ",
                   I18N("windows.clans.settings.logo.upload.label", "Upload Logo"),
                   I18N("windows.clans.settings.logo.upload.requirements", "Requirements: 512x512 PNG, max 256KB. Cost: 20,000 krunkies"),
                   I18N("windows.clans.settings.logo.clan.funds", "Clan Funds"), funds);
        logo_drop_target(sb, S.logo_busy == 1 || !funds_ok); // disabled: uploading, or the clan bank short of 20,000
        sb_append(sb, " ");
        if (S.logo_file) { // a file is selected: the buy button, and the funds warning when the bank is short
            sb_append(sb, "<div class=\"upload-button-container svelte-1w0uz1r\">");
            logo_buy_button(sb, S.logo_busy == 1 || !funds_ok || S.logo_err[0], S.logo_busy == 1);
            sb_append(sb, " ");
            if (!funds_ok)
                sb_appendf(sb, "<p class=\"insufficient-funds-warning svelte-1w0uz1r\">%s</p>",
                           I18N("windows.clans.settings.logo.insufficient.funds", "Insufficient clan funds. Need 20,000 krunkies."));
            else sb_append(sb, "<!---->");
            sb_append(sb, "</div>");
        } else {
            sb_append(sb, "<!---->");
        }
        sb_append(sb, "</div> ");
    } else {
        sb_appendf(sb, "<div class=\"pending-notice svelte-1w0uz1r\"><p class=\"svelte-1w0uz1r\">%s</p></div> ",
                   I18N("windows.clans.settings.logo.pending.notice", "A logo is pending approval. Please wait for admin review before uploading a new one."));
    }
    if (approved && !pending)
        sb_appendf(sb, "<button class=\"io-button io-button--danger\" onclick=\"windows[12].logoRemove()\"%s>%s</button> ", S.logo_busy == 2 ? " disabled" : "",
                   S.logo_busy == 2 ? I18N("windows.clans.settings.logo.removing", "Removing...") : I18N("windows.clans.settings.logo.remove.button", "Remove Logo"));
    if (S.logo_err[0]) {
        sb_append(sb, "<div class=\"io-message error-message svelte-1w0uz1r\">Error: ");
        sb_append_esc(sb, S.logo_err);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div></div>");
}

// v10's main button (iîíììîï, deob-10.0.7.js:11859, svelte-whnh40): variant / size classes, and while disabled both
// the disabled attribute and main-button-disabled
static void main_button(StrBuf *sb, const char *variant, const int slim, const int disabled, const char *onclick, const char *label) {
    sb_appendf(sb, "<button class=\"main-button svelte-whnh40%s%s%s%s\" type=\"button\"%s onclick=\"%s\">%s</button>", variant ? " main-button-" : "", variant ? variant : "",
               slim ? " main-button-slim" : "", disabled ? " main-button-disabled" : "", disabled ? " disabled" : "", onclick, label);
}

static int is_ws(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }

// String.trim() is not empty
static int has_text(const char *s) {
    for (; *s; s++)
        if (!is_ws(*s)) return 1;
    return 0;
}

// String.trim() into out
static void trim_to(char *out, const size_t n, const char *s) {
    while (*s && is_ws(*s)) s++;
    snprintf(out, n, "%s", s);
    size_t len = strlen(out);
    while (len && is_ws(out[len - 1])) out[--len] = 0;
}

static void motd_save_button(StrBuf *sb) {
    main_button(sb, "accept", 1, S.motd_busy || !has_text(S.motd_text), "windows[12].motdSave()", S.motd_busy == 1 ? "Saving..." : "Save");
}

static void motd_remove_button(StrBuf *sb) {
    main_button(sb, "danger", 1, S.motd_busy != 0, "windows[12].motdRemove()", S.motd_busy == 2 ? "Removing..." : "Remove");
}

// the MOTD (svelte-15d5v89, deob-10.0.7.js:96354-96520): its load's error above the form, or the message
static void gen_motd(StrBuf *sb) {
    fetch_motd();
    const int manage = is_mgr2();
    sb_append(sb, "<div class=\"setHed\" style=\"margin-top:20px\">Message of the Day</div> <div class=\"motd-section svelte-15d5v89\">");
    if (!S.motd && !g_scenario) {
        sb_append(sb, "<div class=\"motd-loading svelte-15d5v89\">Loading...</div></div>");
        return;
    }
    if (S.motd_err[0]) {
        sb_append(sb, "<div class=\"motd-error svelte-15d5v89\">");
        sb_append_esc(sb, S.motd_err);
        sb_append(sb, "</div> ");
    }
    if (manage) {
        sb_appendf(sb, "<textarea id=\"clanMotdInput\" class=\"motd-input svelte-15d5v89\" placeholder=\"Set a message for clan members...\" maxlength=\"500\" rows=\"3\" oninput=\"windows[12].motdInput()\"%s>",
                   S.motd_busy ? " disabled" : "");
        sb_append_esc(sb, S.motd_text);
        sb_append(sb, "</textarea> <div class=\"motd-actions svelte-15d5v89\">");
        motd_save_button(sb);
        sb_append(sb, " ");
        // Remove shows while the textarea holds text, saved or typed (deob-10.0.7.js:96472)
        if (S.motd_text[0]) motd_remove_button(sb);
        sb_append(sb, " ");
        if (S.motd_saved) sb_append(sb, "<span class=\"motd-saved svelte-15d5v89\">Saved!</span>");
        sb_append(sb, "</div>");
    } else if (S.motd_text[0]) {
        sb_append(sb, "<div class=\"motd-display svelte-15d5v89\">");
        sb_append_esc(sb, S.motd_text);
        sb_append(sb, "</div>");
    } else {
        sb_append(sb, "<div class=\"motd-empty svelte-15d5v89\">No message set</div>");
    }
    const double at = jnum(S.motd, "set_at");
    if (jstr(S.motd, "set_by_name")[0] && at > 0) {
        char d[32];
        locale_date(at, d, sizeof(d));
        sb_append(sb, " <div class=\"motd-meta svelte-15d5v89\">Set by ");
        sb_append_esc(sb, jstr(S.motd, "set_by_name"));
        sb_appendf(sb, " on %s</div>", d);
    }
    sb_append(sb, "</div>");
}

// owned paints: account data slot 10 [{ind, cnt}], counts summed per index
static int paint_count(const int index) {
    const AccountInfo *a = active_account();
    if (!a || !a->data) return 0;
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetArrayItem(a->data, 10)) if ((int) jnum(e, "ind") == index) n += (int) jnum(e, "cnt");
    return n;
}

static void gen_banner(StrBuf *sb) {
    const char *cur = jstr(g_clan, "clan_banner_color");
    sb_append(sb, "<div class=\"setHed\" style=\"margin-top:20px\">Banner Color</div> <div class=\"banner-color-container svelte-1w0uz1r\"><div class=\"io-card\"><div class=\"io-input-container svelte-1w0uz1r\">"
                  "<p class=\"banner-color-help svelte-1w0uz1r\">Spend one paint to set your clan's banner. Solid colors give a flat tint; shader paints (like Spectrum and Synth) animate. It recolors the clan flag in turf wars and the Clan Tabard body skin worn by clan members.</p> "
                  "<div class=\"banner-color-current svelte-1w0uz1r\"><span class=\"input-label\">Current</span> ");
    if (cur[0] == 's' && cur[1] >= '0' && cur[1] <= '9') {
        const int sh = atoi(cur + 1);
        const char *nm = sh >= 0 && sh < (int) (sizeof(k_shader_names) / sizeof(k_shader_names[0])) ? k_shader_names[sh] : "Shader";
        sb_appendf(sb, "<span class=\"banner-color-swatch banner-color-swatch--shader svelte-1w0uz1r\" title=\"%s\"></span> <span class=\"banner-color-hex svelte-1w0uz1r\">%s</span>", nm, nm);
    } else if (cur[0] == '#') {
        StrBuf c = {0};
        sb_append_esc(&c, cur);
        char *e = sb_finish(&c);
        sb_appendf(sb, "<span class=\"banner-color-swatch svelte-1w0uz1r\" title=\"%s\" style=\"background: %s;\"></span> <span class=\"banner-color-hex svelte-1w0uz1r\">%s</span>", e, e, e);
        free(e);
    } else {
        sb_append(sb, "<span class=\"banner-color-swatch banner-color-swatch--unset svelte-1w0uz1r\" title=\"No paint set\"></span> <span class=\"banner-color-hex banner-color-hex--unset svelte-1w0uz1r\">No paint set</span>");
    }
    sb_append(sb, "</div> ");
    int owned[PAINTS], any = 0;
    for (int i = 0; i < PAINTS; i++) any |= (owned[i] = paint_count(k_paints[i].index)) > 0;
    if (!any) {
        sb_append(sb, "<div class=\"banner-color-empty svelte-1w0uz1r\">You don't own any paints yet. Earn paints from rewards or the market to set a clan color.</div> ");
    } else {
        if (S.banner_sel < 0 || !owned[S.banner_sel]) { // default: the current paint, else the first owned
            S.banner_sel = -1;
            for (int i = 0; i < PAINTS && S.banner_sel < 0; i++) {
                if (!owned[i]) continue;
                char sv[8];
                snprintf(sv, sizeof(sv), "s%d", k_paints[i].shader);
                if ((k_paints[i].color && !strcasecmp(cur, k_paints[i].color)) || (!k_paints[i].color && !strcmp(cur, sv))) S.banner_sel = i;
            }
            for (int i = 0; i < PAINTS && S.banner_sel < 0; i++)
                if (owned[i]) S.banner_sel = i;
        }
        sb_append(sb, "<div class=\"swatch-grid svelte-1w0uz1r\" role=\"radiogroup\" aria-label=\"Available paints\">");
        for (int i = 0; i < PAINTS; i++) {
            if (!owned[i]) continue;
            // v10.0.7's paints have no thumbnail: a shader paint's animated fill, else its colour (deob-10.0.7.js:96572-96584)
            char fill[160];
            if (k_paints[i].color) snprintf(fill, sizeof(fill), "<span class=\"swatch-fill svelte-1w0uz1r\" style=\"background: %s;\"></span>", k_paints[i].color);
            else snprintf(fill, sizeof(fill), "<span class=\"swatch-fill swatch-fill--shader svelte-1w0uz1r\"></span>");
            sb_appendf(sb, "<button type=\"button\" class=\"swatch svelte-1w0uz1r%s\" role=\"radio\" aria-checked=\"%s\" aria-label=\"%s — %d owned\" onclick=\"windows[12].bannerPick(%d)\">"
                           "%s <span class=\"swatch-count svelte-1w0uz1r\">%d</span> <span class=\"swatch-name svelte-1w0uz1r\">%s</span></button>",
                       S.banner_sel == i ? " selected" : "", S.banner_sel == i ? "true" : "false", k_paints[i].name, owned[i], i, fill, owned[i], k_paints[i].name);
        }
        sb_append(sb, "</div> ");
    }
    if (S.banner_err[0]) {
        sb_append(sb, "<div class=\"io-message error-message svelte-1w0uz1r\">Error: ");
        sb_append_esc(sb, S.banner_err);
        sb_append(sb, "</div> ");
    }
    char label[64];
    if (S.banner_busy) snprintf(label, sizeof(label), "Applying…");
    else if (S.banner_sel >= 0) snprintf(label, sizeof(label), "Apply (%s)", k_paints[S.banner_sel].name);
    else snprintf(label, sizeof(label), "Apply Color");
    sb_appendf(sb, "<div class=\"banner-color-actions svelte-1w0uz1r\"><button class=\"io-button\" onclick=\"windows[12].bannerApply()\"%s>%s</button></div></div></div></div>",
               S.banner_busy || S.banner_sel < 0 ? " disabled" : "", label);
}

static void gen_loot(StrBuf *sb) {
    fetch_loot();
    const int manage = is_mgr2();
    sb_append(sb, "<div class=\"setHed\" style=\"margin-top:20px\">Turf Wars Loot Distribution</div> <div class=\"loot-config-container svelte-1ydnong\"><div class=\"io-card\"><div class=\"io-input-container\">");
    if (!S.loot && !g_scenario) {
        sb_append(sb, "<div class=\"loot-loading svelte-1ydnong\">Loading loot config...</div></div></div></div>");
        return;
    }
    // a failed load leaves the form at its defaults, with the error under it (deob-10.0.7.js:96695-96696, :96830-96835)
    if (!S.loot_mode[0]) loot_from(S.loot);
    const char *by = jstr(S.loot, "updated_by_name");
    if (by[0]) {
        sb_append(sb, "<div class=\"loot-updated-info svelte-1ydnong\">Last updated by <span class=\"loot-updated-name svelte-1ydnong\">");
        sb_append_esc(sb, by);
        sb_append(sb, "</span>");
        // "on " + new Date(updated_at).toLocaleDateString() (deob-10.0.3.js:109765): an ISO timestamp's date
        // (or epoch ms) as M/D/YYYY
        const cJSON *at = cJSON_GetObjectItem(S.loot, "updated_at");
        char d[32] = "";
        int y = 0, m = 0, day = 0;
        if (cJSON_IsNumber(at) && at->valuedouble > 0) locale_date(at->valuedouble, d, sizeof(d));
        else if (cJSON_IsString(at) && sscanf(at->valuestring, "%d-%d-%d", &y, &m, &day) == 3) snprintf(d, sizeof(d), "%d/%d/%d", m, day, y);
        if (d[0]) sb_appendf(sb, " <span class=\"loot-updated-time svelte-1ydnong\">on %s</span>", d);
        sb_append(sb, "</div> ");
    }
    const char *dis = manage ? "" : " disabled";
    const int mi = loot_mode_index(S.loot_mode);
    sb_appendf(sb, "<div class=\"loot-field svelte-1ydnong\"><div class=\"label-row\"><label for=\"lootMode\" class=\"input-label\">Distribution Mode</label></div> <select id=\"lootMode\" class=\"io-input-field\" onchange=\"windows[12].lootChange()\"%s>", dis);
    for (int i = 0; i < LOOT_MODES; i++) sb_appendf(sb, "<option value=\"%s\"%s>%s</option>", k_loot_modes[i].mode, i == mi ? " selected" : "", k_loot_modes[i].label);
    sb_append(sb, "</select> <p class=\"loot-mode-desc svelte-1ydnong\">");
    if (k_loot_modes[mi].desc) sb_append(sb, k_loot_modes[mi].desc);
    else sb_appendf(sb, "Only the top %d players are paid, in proportion to their kills.", S.loot_top_count);
    sb_append(sb, "</p></div> ");
    if (mi != 0)
        sb_appendf(sb, "<div class=\"loot-field svelte-1ydnong\"><div class=\"label-row\"><label for=\"lootClanPct\" class=\"input-label\">Clan Treasury Cut (%%)</label></div> <input id=\"lootClanPct\" class=\"io-input-field\" type=\"number\" min=\"0\" max=\"100\" value=\"%d\" oninput=\"windows[12].lootInput(0)\"%s> <p class=\"loot-mode-desc svelte-1ydnong\">Percentage of rewards kept by the clan treasury. Players split the remaining %d%%.</p></div> ",
                   S.loot_clan_pct, dis, 100 - S.loot_clan_pct);
    if (mi == 2 || mi == 4)
        sb_appendf(sb, "<div class=\"loot-field svelte-1ydnong\"><div class=\"label-row\"><label for=\"lootTopCount\" class=\"input-label\">Top Player Count</label></div> <input id=\"lootTopCount\" class=\"io-input-field\" type=\"number\" min=\"1\" max=\"100\" value=\"%d\" oninput=\"windows[12].lootInput(0)\"%s></div> ",
                   S.loot_top_count, dis);
    if (mi == 2)
        sb_appendf(sb, "<div class=\"loot-field svelte-1ydnong\"><div class=\"label-row\"><label for=\"lootTopPct\" class=\"input-label\">Top Players Share (%%)</label></div> <input id=\"lootTopPct\" class=\"io-input-field\" type=\"number\" min=\"0\" max=\"100\" value=\"%d\" oninput=\"windows[12].lootInput(1)\"%s></div> "
                       "<div class=\"loot-field svelte-1ydnong\"><div class=\"label-row\"><span class=\"input-label\">Remainder Share (%%)</span></div> <div id=\"lootRemainder\" class=\"loot-remainder-display svelte-1ydnong\">%d%%</div></div> ",
                   S.loot_top_pct, dis, S.loot_rem);
    if (mi != 0)
        sb_appendf(sb, "<div class=\"loot-field svelte-1ydnong\"><div class=\"label-row\"><label for=\"lootMinGames\" class=\"input-label\">Minimum Games Required</label></div> <input id=\"lootMinGames\" class=\"io-input-field\" type=\"number\" min=\"0\" max=\"100\" value=\"%d\"%s></div> ",
                   S.loot_min_games, dis);
    if (manage) {
        main_button(sb, NULL, 0, S.loot_saving, "windows[12].saveLoot()", S.loot_saving ? "Saving..." : "Save Loot Config");
        sb_append(sb, " ");
    }
    if (S.loot_saved) sb_append(sb, "<div class=\"io-message success-message svelte-1ydnong\">Config saved successfully!</div> ");
    if (S.loot_err[0]) {
        sb_append(sb, "<div class=\"io-message error-message svelte-1ydnong\">Error: ");
        sb_append_esc(sb, S.loot_err);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div></div>");
}

static void select_opts(StrBuf *sb, const char *const (*opts)[2], const int n, const char *cur) {
    for (int i = 0; i < n; i++) sb_appendf(sb, "<option value=\"%s\"%s>%s</option>", opts[i][0], !strcmp(cur, opts[i][0]) ? " selected" : "", opts[i][1]);
}

/* Number(text) (deob-10.0.7.js:96870-96873): spaces around the digits are still that number */
static int level_ok(const char *v) { return clan_recruit_level_ok(v); }

static int kd_ok(const char *v) { return clan_recruit_kd_ok(v); }

static void gen_recruit(StrBuf *sb) {
    fetch_recruit();
    sb_append(sb, "<div class=\"setHed\" style=\"margin-top:20px\">Recruitment Board</div> ");
    if (!S.recruit_loaded && !g_scenario) {
        sb_append(sb, "<div class=\"loading svelte-1o06kdk\">Loading recruitment post...</div>");
        return;
    }
    char funds[32];
    comma(funds, sizeof(funds), jnum(g_clan, "clan_funds"));
    sb_append(sb, "<div class=\"recruitment-manage svelte-1o06kdk\"><div class=\"io-card\"><div class=\"io-input-container svelte-1o06kdk\">");
    if (S.recruit_err[0]) {
        sb_append(sb, "<div class=\"io-message error-message svelte-1o06kdk\">Error: ");
        sb_append_esc(sb, S.recruit_err);
        sb_append(sb, "</div> ");
    }
    if (S.recruit_ok[0]) sb_appendf(sb, "<div class=\"io-message success-message svelte-1o06kdk\">%s</div> ", S.recruit_ok);
    const int active = S.recruit && jnum(S.recruit, "is_active");
    if (!S.recruit) sb_appendf(sb, "<p class=\"cost-info svelte-1o06kdk\">Creating a recruitment post costs 1,000 KR from clan bank. Clan funds: %s KR</p> ", funds);
    sb_append(sb, "<div class=\"label-row\"><label for=\"recruit-desc\" class=\"input-label\">Description</label></div> <textarea id=\"recruit-desc\" class=\"io-input-field\" placeholder=\"Tell players about your clan...\" rows=\"3\" maxlength=\"2000\">");
    sb_append_esc(sb, S.r_desc);
    sb_append(sb, "</textarea> <div class=\"label-row\"><label for=\"recruit-looking\" class=\"input-label\">Looking For</label></div> <input id=\"recruit-looking\" class=\"io-input-field\" type=\"text\" placeholder=\"Active players, snipers, objective players...\" maxlength=\"500\" value=\"");
    sb_append_esc(sb, S.r_looking);
    const int lv_ok = level_ok(S.r_level), kd_v = kd_ok(S.r_kd);
    sb_appendf(sb, "\"> <div class=\"row svelte-1o06kdk\"><div class=\"field svelte-1o06kdk\"><div class=\"label-row\"><span class=\"input-label\">Min Level</span></div> <input id=\"recruit-level\" class=\"io-input-field svelte-1o06kdk%s\" type=\"text\" placeholder=\"0\" oninput=\"windows[12].recruitInput()\" value=\"",
               lv_ok ? "" : " field-error");
    sb_append_esc(sb, S.r_level);
    sb_append(sb, "\">");
    if (!lv_ok) sb_append(sb, " <span class=\"field-error-msg svelte-1o06kdk\">Level must be a whole number between 0 and 200</span>");
    sb_appendf(sb, "</div> <div class=\"field svelte-1o06kdk\"><div class=\"label-row\"><span class=\"input-label\">Min K/D</span></div> <input id=\"recruit-kd\" class=\"io-input-field svelte-1o06kdk%s\" type=\"text\" placeholder=\"0.00\" oninput=\"windows[12].recruitInput()\" value=\"", kd_v ? "" : " field-error");
    sb_append_esc(sb, S.r_kd);
    sb_append(sb, "\">");
    if (!kd_v) sb_append(sb, " <span class=\"field-error-msg svelte-1o06kdk\">K/D must be between 0 and 99.99</span>");
    sb_append(sb, "</div></div> <div class=\"row svelte-1o06kdk\"><div class=\"field svelte-1o06kdk\"><div class=\"label-row\"><span class=\"input-label\">Region</span></div> <select id=\"recruit-region\" class=\"io-input-field\">");
    select_opts(sb, k_regions, (int) (sizeof(k_regions) / sizeof(k_regions[0])), S.r_region);
    sb_append(sb, "</select></div> <div class=\"field svelte-1o06kdk\"><div class=\"label-row\"><span class=\"input-label\">Language</span></div> <select id=\"recruit-lang\" class=\"io-input-field\">");
    select_opts(sb, k_langs, (int) (sizeof(k_langs) / sizeof(k_langs[0])), S.r_lang);
    sb_appendf(sb, "</select></div></div> <div class=\"actions svelte-1o06kdk\"><button class=\"io-button\" onclick=\"windows[12].recruitSave()\"%s>%s</button>",
               S.recruit_saving || !lv_ok || !kd_v ? " disabled" : "", S.recruit_saving ? "Saving..." : S.recruit ? "Update Post" : "Create Post (1,000 KR)");
    // Remove Post, disabled while a save or a removal runs (deob-10.0.7.js:97028-97031)
    if (active) sb_appendf(sb, " <button class=\"io-button io-button--danger\" onclick=\"windows[12].recruitRemove()\"%s>Remove Post</button>", S.recruit_saving ? " disabled" : "");
    sb_append(sb, "</div></div></div></div>");
}

static void gen_transfer(StrBuf *sb) {
    const cJSON *t = cJSON_GetObjectItem(g_clan, "ownership_transfer");
    sb_append(sb, "<div class=\"setHed\" style=\"margin-top:20px\">Transfer Ownership</div> <div class=\"transfer-ownership-container svelte-1w0uz1r\">");
    if (cJSON_IsObject(t)) {
        sb_append(sb, "<div class=\"io-card\"><div class=\"io-input-container svelte-1w0uz1r\"><div class=\"pending-transfer-info svelte-1w0uz1r\"><div class=\"label-row\"><span class=\"input-label\">Pending transfer to:</span></div> <div class=\"highlight-name svelte-1w0uz1r\">");
        sb_append_esc(sb, jstr(t, "to_player_name"));
        sb_append(sb, "</div></div> ");
        main_button(sb, "danger", 0, S.transfer_busy, "windows[12].transferCancel()", S.transfer_busy ? "Cancelling..." : "Cancel Transfer");
        sb_append(sb, "</div></div>");
    } else {
        sb_append(sb, "<div class=\"io-card\"><div class=\"io-input-container svelte-1w0uz1r\"><div class=\"warning-container svelte-1w0uz1r\"><span class=\"warning-badge svelte-1w0uz1r\">Danger</span> <p class=\"warning svelte-1w0uz1r\">Warning: The new owner will have full access to the clan, including clan bank funds. You will lose access to all clan ownership features. You can cancel the transfer at any time before it is accepted. Once accepted, the transfer is irreversible.</p></div> "
                      "<div class=\"label-row\"><label for=\"newOwnerName\" class=\"input-label\">Player Name</label></div> <input id=\"newOwnerName\" class=\"io-input-field\" type=\"text\" placeholder=\"Enter player name\" autocomplete=\"username\" value=\"");
        sb_append_esc(sb, S.transfer_name);
        // the name input is disabled while the transfer is sent (deob-10.0.7.js:97108)
        sb_appendf(sb, "\"%s> ", S.transfer_busy ? " disabled" : "");
        main_button(sb, "danger", 0, S.transfer_busy, "windows[12].transferStart()", S.transfer_busy ? "Transferring..." : "Transfer Ownership");
        sb_append(sb, "</div></div>");
    }
    if (S.transfer_err[0]) {
        sb_append(sb, " <div class=\"io-message error-message svelte-1w0uz1r\">Error: ");
        sb_append_esc(sb, S.transfer_err);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

static void gen_settings(StrBuf *sb) {
    const int mgr = is_mgr(), mgr2 = is_mgr2(), has_id = clan_id() > 0;
    sb_append(sb, "<div id=\"clan-settings\">");
    if (mgr) gen_logo(sb);
    sb_append(sb, " ");
    if (has_id) gen_motd(sb);
    sb_append(sb, " ");
    if (mgr2 && has_id) gen_banner(sb);
    sb_append(sb, " ");
    if (mgr2) gen_loot(sb);
    sb_append(sb, " ");
    if (mgr) gen_recruit(sb);
    sb_append(sb, " ");
    if (i_am_leader()) gen_transfer(sb);
    sb_append(sb, "</div>");
}

// ------------------------------------------------------------------ windows[12].gen()

// KrunkNative: the clan's own load got no answer (or could not be sent): the text and a Retry that asks again
static void load_failed(StrBuf *sb, const char *text, const char *retry) {
    sb_append(sb, "<div class=\"io-message error-message svelte-1w0uz1r\">");
    sb_append_esc(sb, text);
    sb_appendf(sb, "</div> <button class=\"io-button\" onclick=\"%s\">%s</button>", retry, I18N("kn.retry", "Retry"));
}

char *clan_window_gen(void) {
    StrBuf sb = {0};
    const AccountInfo *a = active_account();
    if (!a) {
        sb_appendf(&sb, "<div class='reqW login' onclick='showWindow(5)'><i class='material-icons reqWIcon'>clear</i><div class='reqWLvl'>LOGIN REQUIRED</div><div class='reqWTxt'>%s</div></div>",
                   I18N("windows.clans.login", "to create and join a clan!"));
        return sb_finish(&sb);
    }
    if (!a->clan[0]) {
        const char *name = I18N("windows.clans.name", "Clan Name");
        sb_appendf(&sb, "<div class='setHed' style='margin-top:-5px'>%s</div><input id='clanInp1' type='text' class='smlInput' maxlength='4' placeholder='%s' /><a class='clanSubmitBtn' style='margin-left:20px' onclick='windows[12].sendReq(1)'>%s</a><div class='setHed'>%s <a href='javascript:void(0)' onclick=\"window.openLeaderboards('clans')\" class='menuLink' style='float:right'>%s</a></div><input id='clanInp2' type='text' class='smlInput' maxlength='5' placeholder='%s' /><a class='clanSubmitBtn' style='margin-left:20px' onclick='windows[12].sendReq(2)'>%s</a><div id='clanErr' style='margin-top:10px'></div>",
                   I18N("windows.clans.create", "Create Clan"), name, I18N("generic.create", "Create"), I18N("windows.clans.join", "Join Clan"),
                   I18N("windows.clans.view", "View Clans"), name, I18N("generic.submit", "Submit"));
        return sb_finish(&sb);
    }
    // OPEN_CLAN_TAB on every gen (a Svelte component's own update is not one); a gen mounts the tab's components again
    if (!g_svelte && !g_scenario) {
        backend_social_send_text("[\"vcln\"]");
        components_mount();
    }
    const int mgr = g_clan && is_mgr();
    sb_append(&sb, "<div style='height:15px;'></div><div class='bigMenTabs'>");
    for (int i = 0; i < CLAN_TABS; i++) {
        if (i >= 3 && !mgr) continue;
        sb_appendf(&sb, "<div class='bigMenTab%s' style='font-size:16px' onmouseenter='playTick(0.1);' onclick='playSelect();windows[12].switchTab(%d)'>%s</div>", g_clan_tab == i ? " bigMenTabA" : "", i, k_clan_tabs[i]);
    }
    sb_append(&sb, "</div><div id='clanErr' style='margin-top:0px;color:rgba(0,0,0,0.5)'>");
    if (!g_clan) {
        if (!g_req_sent && !g_clan_failed && !g_scenario) {
            g_req_sent = 1;
            if (send_cln(3, NULL, NULL)) { // this.sendReq(3): its answer has a deadline
                html_timer_cancel(g_clan_timer);
                g_clan_timer = html_timer_add(CLAN_WAIT_MS, clan_expired, NULL);
            } else { // not sent (no socket, the brain is gone): no answer can come
                g_req_sent = 0;
                g_clan_failed = 1;
            }
        }
        if (g_clan_failed && !g_scenario) { // no answer by the deadline, or not sent: the window says so, with Retry
            load_failed(&sb, I18N("kn.clan.loadFailed", "Couldn't load the clan. Try again."), "windows[12].clanRetry()");
            sb_append(&sb, "</div>");
            return sb_finish(&sb);
        }
        sb_appendf(&sb, "%s</div>", I18N("generic.loading", "LOADING..."));
        return sb_finish(&sb);
    }
    switch (g_clan_tab) {
    case 1: gen_members(&sb); break;
    case 2: gen_stats(&sb); break;
    case 3: gen_requests(&sb); break;
    case 4: gen_settings(&sb); break;
    default: gen_home(&sb); break;
    }
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

// ------------------------------------------------------------------ popups (#genericPop)

static void set_disp(const char *id, const char *v) {
    HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_set_display(e, v);
}

// genericPopup(html, cls, style)
static void generic_popup(const char *html, const char *cls, const char *style) {
    ui_loadout_clear_pops();
    set_disp("popupHolder", "block");
    HtmlElem pop = html_dom_by_id("genericPop");
    if (!pop) return;
    html_dom_set_attr(pop, "class", cls ? cls : "");
    html_dom_set_attr(pop, "style", style ? style : "");
    html_dom_set_html(pop, html);
    html_dom_set_display(pop, "block");
}

// contPopup(html, width, border)
static void cont_popup(const char *html, const char *width, const char *border) {
    char style[96];
    snprintf(style, sizeof(style), "width:%s;border:%s;", width ? width : "700px", border ? border : "");
    generic_popup(html, "contPop", style);
}

static void wait_popup(void) { // _70159("Please Wait...")
    generic_popup("<div style='width:100%;text-align:center'><div class='lds-ring'><div></div><div></div><div></div><div></div></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>Please Wait...</div></div>", "loadingPop", NULL);
}

// _69540(kind, error, text): SUCCESS / error popup (responsePop)
static void result_popup(const int ok, const char *text) {
    StrBuf sb = {0};
    if (ok) sb_appendf(&sb, "<div style='width:100%%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#3be354'>done</i></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>%s</div></div>", text && text[0] ? text : "Success!");
    else {
        sb_append(&sb, "<div style='width:100%;text-align:center'><div><i class='material-icons' style='font-size:80px;color:#e33b3b'>clear</i></div><div style='font-size:20px;color:rgba(255,255,255,0.5)'>");
        sb_append_esc(&sb, text && text[0] ? text : I18N("social.market.failed", "Failed. Try Again"));
        sb_append(&sb, "</div></div>");
    }
    char *html = sb_finish(&sb);
    generic_popup(html, "responsePop", NULL);
    free(html);
}

// the shared confirm prompt (io-popup-container, v10:15109); v10 pushes it on its view stack
static void io_prompt(const char *title, const char *desc, const char *info, const char *confirm, const char *handler) {
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"io-popup-container\"><div class=\"io-popup-header\">%s</div> <div class=\"io-popup-content\"><div class=\"io-card\"><p>%s</p> <form><div class=\"io-input-container\">", title, desc);
    if (info) sb_appendf(&sb, "<div class=\"io-section info-container svelte-1xwmszg\"><div class=\"info-message svelte-1xwmszg\">%s</div></div> ", info);
    sb_appendf(&sb, "<div class=\"io-button-group\"><button type=\"button\" class=\"io-button\" onclick=\"clearPops()\">Cancel</button> <button type=\"button\" class=\"io-button io-button--accept\" onclick=\"%s\">%s</button></div></div></form></div></div></div>", handler, confirm);
    char *html = sb_finish(&sb);
    generic_popup(html, "ioPop", NULL);
    free(html);
}

static void clan_err(const char *msg, const int ok) {
    HtmlElem e = html_dom_by_id("clanErr");
    if (!e) return;
    html_dom_set_style(e, "color", ok ? "rgba(0,0,0,0.6)" : "rgba(255,0,0,0.6)");
    html_dom_set_html(e, msg);
}

static void send_req(const int action, const char *value) {
    HtmlElem e = html_dom_by_id("clanErr");
    if (e) {
        html_dom_set_style(e, "color", "rgba(0,0,0,0.6)");
        html_dom_set_html(e, I18N("generic.loading", "LOADING..."));
    }
    send_cln(action, value && value[0] ? cJSON_CreateString(value) : cJSON_CreateNumber(1), NULL);
}

static cJSON *member_by_id(const int id) {
    cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(g_clan, "members")) if ((int) jnum(m, "i") == id) return m;
    return NULL;
}

static int arg_num(const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return atoi(args[i].str);
    return def;
}

// mangFunds(): the Manage Clan Funds popup (contPop; Withdraw for the owner and Captains)
static void manage_funds(void) {
    char funds[32], html[1536];
    comma(funds, sizeof(funds), jnum(g_clan, "clan_funds"));
    const int mgr = is_mgr();
    snprintf(html, sizeof(html),
             "<div style='font-size:21px;color:#fff'>Manage Clan Funds<span style='float:right;color:rgba(255,255,255,0.6)'>%s <span style='color:#fff'>KR</span></span></div><div style='margin-top:20px;font-size:18px;color:#FFF'>Deposit Fee<span id='clnFeeVal' style='float:right;color:rgba(255,255,255,0.7)'>None</span></div><input type='number' oninput='windows[12].calcFee(this)' placeholder='Enter KR Amount' id='clanFundsIn'></input><div class='clanFndB' style='background-color:%s' %s>Withdraw</div><div class='clanFndB' style='margin-left:16px;background-color:#4ca9f5' onclick='windows[12].chngFunds(0)'>Deposit</div>",
             funds, mgr ? "#fc5a4e" : "#6e6e6e", mgr ? "onclick='windows[12].chngFunds(1)'" : "");
    generic_popup(html, "contPop", NULL);
}

// calcFee (deob-10.0.7.js, windows[12].calcFee): Math.floor(0.1 * Number(text)). "1e2" is 10, not 0.
static void calc_fee(void) {
    HtmlElem in = html_dom_by_id("clanFundsIn");
    char v[32] = "";
    if (in) html_dom_get_value(in, v, sizeof(v));
    const long long fee = js_floor_tenth(v);
    HtmlElem out = html_dom_by_id("clnFeeVal");
    if (!out) return;
    if (!fee) {
        html_dom_set_html(out, "None");
        return;
    }
    char f[32], html[192];
    comma(f, sizeof(f), (double) fee);
    snprintf(html, sizeof(html), "<span style='color:rgba(255,255,255,0.7)'>-%s <span style='color:#fff'>KR</span></span>", f);
    html_dom_set_html(out, html);
}

// ------------------------------------------------------------------ settings handlers

static void read_value(const char *id, char *out, const size_t n) {
    HtmlElem e = html_dom_by_id(id);
    if (e) html_dom_get_value(e, out, n);
}

static int read_int(const char *id, const int def) {
    char v[32] = "";
    HtmlElem e = html_dom_by_id(id);
    if (!e) return def;
    html_dom_get_value(e, v, sizeof(v));
    return v[0] ? atoi(v) : def;
}

static int clampi(const int v, const int lo, const int hi) { return v < lo ? lo : v > hi ? hi : v; }

// the inputs of the settings tab into the state, so a re-render keeps what was typed
static void sync_forms(void) {
    if (g_clan_tab != 4) return;
    if (html_dom_by_id("clanMotdInput")) read_value("clanMotdInput", S.motd_text, sizeof(S.motd_text));
    if (html_dom_by_id("lootMode")) read_value("lootMode", S.loot_mode, sizeof(S.loot_mode));
    // bound as typed (v10 sends them so); the top share is clamped and moves the remainder only on its own input
    S.loot_clan_pct = read_int("lootClanPct", S.loot_clan_pct);
    S.loot_top_count = read_int("lootTopCount", S.loot_top_count);
    S.loot_top_pct = clampi(read_int("lootTopPct", S.loot_top_pct), 0, 100);
    S.loot_min_games = read_int("lootMinGames", S.loot_min_games);
    if (html_dom_by_id("recruit-desc")) {
        read_value("recruit-desc", S.r_desc, sizeof(S.r_desc));
        read_value("recruit-looking", S.r_looking, sizeof(S.r_looking));
        read_value("recruit-level", S.r_level, sizeof(S.r_level));
        read_value("recruit-kd", S.r_kd, sizeof(S.r_kd));
        read_value("recruit-region", S.r_region, sizeof(S.r_region));
        read_value("recruit-lang", S.r_lang, sizeof(S.r_lang));
    }
    if (html_dom_by_id("newOwnerName")) read_value("newOwnerName", S.transfer_name, sizeof(S.transfer_name));
}

static void rerender_kept(void) {
    sync_forms();
    clan_rerender();
}

// the "Saved!" (1, the MOTD) / "Config saved successfully!" (2, the loot config) line goes after 3 s
static void saved_off(void *user) {
    if ((intptr_t) user == 1) S.motd_saved = 0;
    else S.loot_saved = 0;
    clan_rerender();
}

static void motd_send(const int remove) {
    if (!g_clan || S.motd_busy) return;
    sync_forms();
    char path[64];
    snprintf(path, sizeof(path), "/clans/%d/motd", clan_id());
    cJSON *body = NULL;
    if (!remove) { // setMotd(clanId, text.trim()) (deob-10.0.7.js:96368-96379)
        if (!has_text(S.motd_text)) return;
        char t[sizeof(S.motd_text)];
        trim_to(t, sizeof(t), S.motd_text);
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "message", t);
    }
    S.motd_err[0] = 0;
    if (!remove) S.motd_saved = 0;
    S.motd_busy = remove ? 2 : 1;
    if (!v10_gapi(remove ? "DELETE" : "PUT", path, body, on_motd, (void *) (intptr_t) (remove ? 2 : 1))) {
        S.motd_busy = 0;
        snprintf(S.motd_err, sizeof(S.motd_err), FETCH_FAILED); // not sent: v10's fetch could not reach the server
    }
    cJSON_Delete(body);
    clan_rerender();
}

static void on_banner(void *user, const int status, const cJSON *data, const cJSON *error) {
    const int paint = (int) (intptr_t) user;
    S.banner_busy = 0;
    if (gapi_ok(status, error) && g_clan) {
        const char *v = jstr(data, "clan_banner_color");
        char fb[8];
        if (!v[0]) {
            if (k_paints[paint].color) v = k_paints[paint].color;
            else {
                snprintf(fb, sizeof(fb), "s%d", k_paints[paint].shader);
                v = fb;
            }
        }
        cJSON_DeleteItemFromObject(g_clan, "clan_banner_color");
        cJSON_AddStringToObject(g_clan, "clan_banner_color", v);
        account_remove_skin(k_paints[paint].index);
        S.banner_err[0] = 0;
    } else {
        // error.message || the fallback; a fetch that failed throws its own message (deob-10.0.7.js:96196-96225)
        if (status <= 0) snprintf(S.banner_err, sizeof(S.banner_err), FETCH_FAILED);
        else if (status >= 500) server_error(S.banner_err, sizeof(S.banner_err), status);
        else snprintf(S.banner_err, sizeof(S.banner_err), "%s", kio_error_text(error, "Failed to apply banner color"));
    }
    rerender_kept();
}

static void banner_apply(void) {
    if (!g_clan || S.banner_busy || S.banner_sel < 0) return;
    if (paint_count(k_paints[S.banner_sel].index) <= 0) {
        snprintf(S.banner_err, sizeof(S.banner_err), "You don't have any of that paint left.");
        rerender_kept();
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), "/clans/%d/banner-color", clan_id());
    cJSON *body = cJSON_CreateObject();
    cJSON_AddNumberToObject(body, "paint_item_id", k_paints[S.banner_sel].index);
    S.banner_err[0] = 0;
    S.banner_busy = v10_gapi("POST", path, body, on_banner, (void *) (intptr_t) S.banner_sel);
    if (!S.banner_busy) snprintf(S.banner_err, sizeof(S.banner_err), FETCH_FAILED);
    cJSON_Delete(body);
    rerender_kept();
}

static void save_loot(void) {
    if (!g_clan || S.loot_saving) return;
    sync_forms();
    cJSON *cfg = cJSON_CreateObject();
    cJSON_AddStringToObject(cfg, "distribution_mode", S.loot_mode[0] ? S.loot_mode : "clan_bank");
    cJSON_AddNumberToObject(cfg, "clan_pct", S.loot_clan_pct);
    cJSON_AddNumberToObject(cfg, "top_player_count", S.loot_top_count);
    cJSON_AddNumberToObject(cfg, "top_player_pct", S.loot_top_pct);
    cJSON_AddNumberToObject(cfg, "remainder_pct", S.loot_rem);
    cJSON_AddNumberToObject(cfg, "min_games_required", S.loot_min_games);
    // keep the form's values as the config (the tab 0 display reads them too)
    if (S.loot) {
        cJSON *keep = cJSON_Duplicate(cfg, 1);
        cJSON *by = cJSON_GetObjectItem(S.loot, "updated_by_name"), *at = cJSON_GetObjectItem(S.loot, "updated_at");
        if (by) cJSON_AddItemToObject(keep, "updated_by_name", cJSON_Duplicate(by, 1));
        if (at) cJSON_AddItemToObject(keep, "updated_at", cJSON_Duplicate(at, 1));
        cJSON_Delete(S.loot);
        S.loot = keep;
    }
    S.loot_err[0] = 0;
    S.loot_saved = 0;
    S.loot_failed = 0;
    if (kio_clan()) { // v10.0.7 updateLootConfig: PUT /clans/<id>/loot-config, the six fields (deob-10.0.7.js:26855, :96654-96666)
        char path[64];
        snprintf(path, sizeof(path), "/clans/%d/loot-config", clan_id());
        S.loot_saving = v10_gapi("PUT", path, cfg, kio_settings_read, (void *) 2) != 0;
        cJSON_Delete(cfg);
        if (!S.loot_saving) snprintf(S.loot_err, sizeof(S.loot_err), FETCH_FAILED);
        clan_rerender();
        return;
    }
    char *json = cJSON_PrintUnformatted(cfg);
    cJSON_Delete(cfg);
    char token[256], url[600];
    backend_url_encode(token, sizeof(token), account_session_token());
    snprintf(url, sizeof(url), "%s/api/clans/%d/loot-config?token=%s", backend_url(), clan_id(), token);
    S.loot_saving = backend_http_request("PUT", url, json ? json : "{}", on_loot, (void *) 1) != 0;
    if (!S.loot_saving) snprintf(S.loot_err, sizeof(S.loot_err), FETCH_FAILED);
    free(json);
    clan_rerender();
}

static void recruit_save(void) {
    if (!g_clan || S.recruit_saving) return;
    sync_forms();
    if (!level_ok(S.r_level) || !kd_ok(S.r_kd)) {
        clan_rerender();
        return;
    }
    cJSON *body = cJSON_CreateObject();
    if (S.r_desc[0]) cJSON_AddStringToObject(body, "description", S.r_desc);
    if (S.r_looking[0]) cJSON_AddStringToObject(body, "looking_for", S.r_looking);
    cJSON_AddNumberToObject(body, "min_level", atof(S.r_level));
    cJSON_AddNumberToObject(body, "min_kd", atof(S.r_kd));
    if (S.r_region[0]) cJSON_AddStringToObject(body, "region", S.r_region);
    if (S.r_lang[0]) cJSON_AddStringToObject(body, "language", S.r_lang);
    S.recruit_err[0] = S.recruit_ok[0] = 0;
    if (kio_clan()) { // v10.0.7 createOrUpdateRecruitment: POST /clans/<id>/recruitment (deob-10.0.7.js:26897, :27834-27845)
        char path[64];
        snprintf(path, sizeof(path), "/clans/%d/recruitment", clan_id());
        S.recruit_saving = v10_gapi("POST", path, body, kio_settings_read, (void *) 3) != 0;
        cJSON_Delete(body);
        if (!S.recruit_saving) snprintf(S.recruit_err, sizeof(S.recruit_err), FETCH_FAILED);
        clan_rerender();
        return;
    }
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char url[600];
    recruit_url(url, sizeof(url));
    S.recruit_saving = backend_http_request("POST", url, json ? json : "{}", on_recruit, (void *) 1) != 0;
    if (!S.recruit_saving) snprintf(S.recruit_err, sizeof(S.recruit_err), FETCH_FAILED);
    free(json);
    clan_rerender();
}

static void recruit_remove(void) {
    ui_loadout_clear_pops();
    S.recruit_err[0] = S.recruit_ok[0] = 0;
    int sent;
    if (kio_clan()) { // v10.0.7 removeRecruitment: DELETE /clans/<id>/recruitment (deob-10.0.7.js:26903, :27873)
        char path[64];
        snprintf(path, sizeof(path), "/clans/%d/recruitment", clan_id());
        sent = v10_gapi("DELETE", path, NULL, kio_settings_read, (void *) 4) != 0;
    } else {
        char url[600];
        recruit_url(url, sizeof(url));
        sent = backend_http_request("DELETE", url, NULL, on_recruit, (void *) 2) != 0;
    }
    // the removal runs as a save does: both buttons are disabled and the main one reads "Saving..." (deob-10.0.7.js:27868)
    S.recruit_saving = sent;
    if (!sent) snprintf(S.recruit_err, sizeof(S.recruit_err), FETCH_FAILED);
    clan_rerender();
}

// the picked file's base64 -> its bytes (owned; NULL on a bad character)
static unsigned char *b64_decode(const char *in, size_t *out_n) {
    const size_t len = in ? strlen(in) : 0;
    unsigned char *out = malloc(len / 4 * 3 + 3);
    if (!out) return NULL;
    size_t o = 0;
    unsigned v = 0;
    int bits = 0;
    for (size_t i = 0; i < len; i++) {
        const char c = in[i];
        int d;
        if (c >= 'A' && c <= 'Z') d = c - 'A';
        else if (c >= 'a' && c <= 'z') d = c - 'a' + 26;
        else if (c >= '0' && c <= '9') d = c - '0' + 52;
        else if (c == '+' || c == '-') d = 62;
        else if (c == '/' || c == '_') d = 63;
        else if (c == '=' || c == '\r' || c == '\n') continue;
        else {
            free(out);
            return NULL;
        }
        v = v << 6 | (unsigned) d;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char) (v >> bits & 0xff);
        }
    }
    *out_n = o;
    return out;
}

// The drop target's click: the profile picture's file dialog, PNG only, up to 256 KB (iíïìîïì: no-op while disabled)
static void on_logo_picked(void *user, const char *b64, size_t bytes, const char *error);
static void logo_pick(void) {
    if (!g_clan || S.logo_busy == 1 || !logo_funds_ok() || logo_pending()) return;
    avatar_ui_pick_file(1, LOGO_MAX_BYTES, on_logo_picked, NULL);
}

static int ends_with_png(const char *name) {
    const size_t n = strlen(name);
    return n >= 4 && !strcasecmp(name + n - 4, ".png");
}

// A picked file. The drop target's own checks first (iîiiïií, deob-10.0.7.js:11921-11957): over 256KB, or neither a .png
// name nor an image/png type, is its error state (and the settings' error line, its onerror); otherwise it shows the
// file's name and hands it to the settings' onfileSelected (:96058-96093), which clears the error and checks the type,
// the size and the 512 x 512 picture; a failed check drops the selection (the target still shows the name).
// avatar_ui's checks by name: FileTooLarge (the size), UnsupportedType (not PNG bytes: a .png name is a PNG type the
// picture of which does not load), BadImage (the file could not be read: the picture does not load). A file passes the
// target's type check by its .png name (the browser's type for it is image/png, so onfileSelected's type check holds).
static void on_logo_picked(void *user, const char *b64, size_t bytes, const char *error) {
    (void) user;
    const char *name = avatar_ui_picked_name();
    const int typed = ends_with_png(name);
    const char *drop_err = NULL;
    if (error && !strcmp(error, "FileTooLarge")) drop_err = "File size exceeds 256KB limit";
    else if (!typed) drop_err = "File type not supported. Accepted types: .png, image/png";
    if (drop_err) {
        S.logo_has_file = 0;
        S.logo_name[0] = 0;
        snprintf(S.logo_drop_err, sizeof(S.logo_drop_err), "%s", drop_err);
        snprintf(S.logo_err, sizeof(S.logo_err), "%s", drop_err);
        rerender_kept();
        return;
    }
    S.logo_drop_err[0] = 0;
    S.logo_has_file = 1;
    snprintf(S.logo_name, sizeof(S.logo_name), "%s", name);
    S.logo_err[0] = 0;
    free(S.logo_file);
    S.logo_file = NULL;
    S.logo_bytes = 0;
    size_t n = 0;
    unsigned char *png = error ? NULL : b64_decode(b64, &n);
    const long w = png && n >= 24 ? (long) png[16] << 24 | (long) png[17] << 16 | png[18] << 8 | png[19] : -1;
    const long h = png && n >= 24 ? (long) png[20] << 24 | (long) png[21] << 16 | png[22] << 8 | png[23] : -1;
    if (!png || n < 24 || memcmp(png, "\x89PNG\r\n\x1a\n", 8) || memcmp(png + 12, "IHDR", 4))
        snprintf(S.logo_err, sizeof(S.logo_err), "Invalid image file");
    else if (w != 512 || h != 512) snprintf(S.logo_err, sizeof(S.logo_err), "Image must be exactly 512x512 pixels");
    else {
        S.logo_file = strdup(b64);
        S.logo_bytes = bytes;
    }
    free(png);
    rerender_kept();
}

// the drop target's remove button (onfileRemoved, :96124-96127): no file, no error
static void logo_unset(void) {
    if (S.logo_busy == 1 || !logo_funds_ok()) return; // the target is disabled
    free(S.logo_file);
    S.logo_file = NULL;
    S.logo_has_file = 0;
    S.logo_name[0] = S.logo_drop_err[0] = S.logo_err[0] = 0;
    rerender_kept();
}

static void on_logo_uploaded(void *user, const int status, const cJSON *data, const cJSON *error);

// the buy button (iÏïiíïï, deob-10.0.7.js:96094-96123): the funds, then a pending logo, then the upload
static void logo_upload(void) {
    if (!g_clan || !S.logo_file || !clan_id() || S.logo_busy == 1) return;
    if (!logo_funds_ok()) snprintf(S.logo_err, sizeof(S.logo_err), "Insufficient clan funds. Need 20,000 krunkies.");
    else if (logo_pending()) snprintf(S.logo_err, sizeof(S.logo_err), "A logo is already pending approval. Please wait for admin review.");
    else {
        S.logo_err[0] = 0;
        char path[64];
        snprintf(path, sizeof(path), "/clans/%d/logo", clan_id());
        if (kio_clan()) { // a FormData with the file as `file` (:96103-96110)
            size_t n = 0;
            unsigned char *png = b64_decode(S.logo_file, &n);
            S.logo_busy = png && v10_gapi_upload(path, "file", S.logo_name, "image/png", png, n, on_logo_uploaded, (void *) (intptr_t) clan_id()) ? 1 : 0;
            free(png);
        } else {
            cJSON *body = cJSON_CreateObject();
            cJSON_AddStringToObject(body, "file", S.logo_file);
            S.logo_busy = v10_gapi("POST", path, body, on_logo_uploaded, (void *) (intptr_t) clan_id()) ? 1 : 0;
            cJSON_Delete(body);
        }
        if (!S.logo_busy) snprintf(S.logo_err, sizeof(S.logo_err), FETCH_FAILED);
    }
    rerender_kept();
}

// the upload's answer: an error throws error.message || "Failed to upload logo"; else no file is selected and the
// clan is asked for again (sendReq(3)), whose answer mounts the settings again
static void on_logo_uploaded(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) data;
    S.logo_busy = 0;
    if (gapi_ok(status, error)) {
        S.logo_err[0] = 0;
        if (!kio_clan() && S.logo_file) { // Box Office: the file sent is this client's view of the pending logo
            logo_sent_clear();
            const size_t len = strlen(S.logo_file) + 32;
            g_logo_sent = malloc(len);
            if (g_logo_sent) {
                snprintf(g_logo_sent, len, "data:image/png;base64,%s", S.logo_file);
                g_logo_sent_clan = (int) (intptr_t) user;
            }
        }
        free(S.logo_file);
        S.logo_file = NULL;
        send_cln(3, NULL, NULL);
    } else if (status <= 0) {
        snprintf(S.logo_err, sizeof(S.logo_err), FETCH_FAILED);
    } else if (status >= 500) {
        server_error(S.logo_err, sizeof(S.logo_err), status);
    } else {
        snprintf(S.logo_err, sizeof(S.logo_err), "%s", kio_error_text(error, "Failed to upload logo"));
    }
    rerender_kept();
}

// Remove Logo's answer (iííìíïì, deob-10.0.7.js:95959-95973): an error throws error.message || "Failed to remove logo";
// else sendReq(3)
static void on_logo(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    (void) data;
    S.logo_busy = 0;
    if (gapi_ok(status, error)) {
        S.logo_err[0] = 0;
        send_cln(3, NULL, NULL); // windows[12].sendReq(3)
    } else if (status <= 0) {
        snprintf(S.logo_err, sizeof(S.logo_err), FETCH_FAILED);
    } else if (status >= 500) {
        server_error(S.logo_err, sizeof(S.logo_err), status);
    } else {
        snprintf(S.logo_err, sizeof(S.logo_err), "%s", kio_error_text(error, "Failed to remove logo"));
    }
    rerender_kept();
}

static void on_transfer(void *user, const int status, const cJSON *data, const cJSON *error) {
    const int kind = (int) (intptr_t) user; // 0 initiate, 1 cancel, 2 respond
    S.transfer_busy = 0;
    if (status == 200 && !error && g_clan) {
        S.transfer_err[0] = 0;
        cJSON_DeleteItemFromObject(g_clan, "ownership_transfer");
        if (kind == 0 && cJSON_IsObject(data)) cJSON_AddItemToObject(g_clan, "ownership_transfer", cJSON_Duplicate(data, 1));
        if (kind == 0) S.transfer_name[0] = 0;
        if (kind == 2) send_cln(3, NULL, NULL);
    } else if (kind != 2) { // v10: a failed respond shows nothing
        static const char *const fails[] = {"Failed to transfer ownership", "Failed to cancel transfer request"};
        snprintf(S.transfer_err, sizeof(S.transfer_err), "%s", error ? v10_gapi_error_text(error, fails[kind]) : fails[kind]);
    }
    rerender_kept();
}

// JS String(v) of an rpc result's error (v10 throws new Error(res.error)): its message
static void js_string(char *out, const size_t n, const cJSON *v) {
    if (cJSON_IsString(v)) snprintf(out, n, "%s", v->valuestring);
    else if (cJSON_IsNumber(v)) snprintf(out, n, "%.17g", v->valuedouble);
    else if (cJSON_IsTrue(v)) snprintf(out, n, "true");
    else if (cJSON_IsObject(v)) snprintf(out, n, "[object Object]");
    else out[0] = 0;
}

// The krunker.io account's transfer rpc answers (account_ui.c): ["_0", name, res] resolves with res = {data, error},
// ["_1", name, err] rejects with err. v10.0.7 (deob-10.0.7.js:220555-220642, the settings component :96025-96057):
// initiate: res.error throws it, else clanData.ownership_transfer = res.data and the name input is cleared; cancel:
// res.error throws, else ownership_transfer is deleted; respond: res.error throws (nothing shown), else
// ownership_transfer is deleted and sendReq(3). A throw shows error.message || the fallback.
void clan_ui_rpc(const char *label, const cJSON *frame) {
    const char *name = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1));
    if (!label || !name) return;
    const int kind = !strcmp(name, "clan-transfer-initiate") ? 0 : !strcmp(name, "clan-transfer-cancel") ? 1
                   : !strcmp(name, "clan-transfer-respond")  ? 2 : -1;
    if (kind < 0) return;
    S.transfer_busy = 0;
    const cJSON *res = cJSON_GetArrayItem(frame, 2);
    char err[96] = "";
    int failed = 1;
    if (!strcmp(label, "_0") && cJSON_IsObject(res)) {
        const cJSON *e = cJSON_GetObjectItem(res, "error");
        const int thrown = e && !cJSON_IsNull(e) && !cJSON_IsFalse(e) && !(cJSON_IsNumber(e) && e->valuedouble == 0) &&
                           !(cJSON_IsString(e) && !e->valuestring[0]);
        if (thrown) js_string(err, sizeof(err), e);
        failed = thrown;
    } else if (!strcmp(label, "_1")) {
        const char *m = cJSON_GetStringValue(cJSON_GetObjectItem(res, "message"));
        if (m) snprintf(err, sizeof(err), "%s", m);
    }
    if (!failed && g_clan) {
        S.transfer_err[0] = 0;
        cJSON_DeleteItemFromObject(g_clan, "ownership_transfer");
        const cJSON *data = cJSON_GetObjectItem(res, "data");
        if (kind == 0 && data && !cJSON_IsNull(data)) cJSON_AddItemToObject(g_clan, "ownership_transfer", cJSON_Duplicate(data, 1));
        if (kind == 0) S.transfer_name[0] = 0;
        if (kind == 2) send_cln(3, NULL, NULL);
    } else if (failed && kind != 2) {
        static const char *const fails[] = {"Failed to transfer ownership", "Failed to cancel transfer request"};
        snprintf(S.transfer_err, sizeof(S.transfer_err), "%s", err[0] ? err : fails[kind]);
    }
    rerender_kept();
}

static void transfer_send(const int kind, const int accept) {
    if (!g_clan || S.transfer_busy) return;
    sync_forms();
    if (kio_clan()) { // v10.0.7: the socket's rpc(name, ...args) (deob-10.0.7.js:220556, :220631, :220638)
        if (kind == 0 && !has_text(S.transfer_name)) { // name.trim() || "Please enter a player name" (:96030, :96043)
            snprintf(S.transfer_err, sizeof(S.transfer_err), "Please enter a player name");
            clan_rerender();
            return;
        }
        static const char *const names[] = {"clan-transfer-initiate", "clan-transfer-cancel", "clan-transfer-respond"};
        cJSON *f = cJSON_CreateArray();
        cJSON_AddItemToArray(f, cJSON_CreateString(names[kind]));
        if (kind == 0) cJSON_AddItemToArray(f, cJSON_CreateString(S.transfer_name));
        if (kind == 2) cJSON_AddItemToArray(f, cJSON_CreateBool(accept));
        if (kind != 2) S.transfer_err[0] = 0;
        S.transfer_busy = backend_social_send(f);
        cJSON_Delete(f);
        if (!S.transfer_busy && kind != 2) {
            static const char *const fails[] = {"Failed to transfer ownership", "Failed to cancel transfer request"};
            snprintf(S.transfer_err, sizeof(S.transfer_err), "%s", fails[kind]);
        }
        clan_rerender();
        return;
    }
    char path[64];
    snprintf(path, sizeof(path), kind == 2 ? "/clans/%d/transfer/respond" : "/clans/%d/transfer", clan_id());
    cJSON *body = NULL;
    if (kind == 0) {
        if (!has_text(S.transfer_name)) { // name.trim() || "Please enter a player name" (deob-10.0.7.js:96030, :96043)
            snprintf(S.transfer_err, sizeof(S.transfer_err), "Please enter a player name");
            clan_rerender();
            return;
        }
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "name", S.transfer_name);
    } else if (kind == 2) {
        body = cJSON_CreateObject();
        cJSON_AddBoolToObject(body, "accept", accept);
    }
    S.transfer_err[0] = 0;
    S.transfer_busy = v10_gapi(kind == 1 ? "DELETE" : "POST", path, body, on_transfer, (void *) (intptr_t) kind);
    cJSON_Delete(body);
    clan_rerender();
}

// ------------------------------------------------------------------ live inputs (Svelte's bound values)
// v10's bound inputs update their section as they are typed into; the section's other parts change in place here,
// so the field keeps its focus and caret.

// a main button's disabled state, both the attribute and main-button-disabled
static void set_main_disabled(HtmlElem b, const int disabled) {
    if (!b) return;
    if (disabled) html_dom_set_attr(b, "disabled", "");
    else html_dom_remove_attr(b, "disabled");
    html_dom_set_class(b, "main-button-disabled", disabled);
}

// the MOTD textarea (bind:value, deob-10.0.7.js:96495): Save turns on with text, Remove shows while there is text
static void motd_input(void) {
    HtmlElem in = html_dom_by_id("clanMotdInput");
    if (!in) return;
    html_dom_get_value(in, S.motd_text, sizeof(S.motd_text));
    HtmlElem actions = html_dom_query(0, "#clan-settings .motd-actions");
    if (!actions) return;
    HtmlElem save = html_dom_query(actions, ".main-button-accept"), remove = html_dom_query(actions, ".main-button-danger");
    set_main_disabled(save, S.motd_busy || !has_text(S.motd_text));
    if (S.motd_text[0] && !remove && save) {
        StrBuf sb = {0};
        sb_append(&sb, " ");
        motd_remove_button(&sb);
        char *html = sb_finish(&sb);
        html_dom_insert_html(save, HTML_INSERT_AFTER_END, html);
        free(html);
    } else if (!S.motd_text[0] && remove) {
        html_dom_remove(remove);
    }
}

// the loot config's inputs: the clan cut's hint and the Top N description follow their bound values; the top share's
// own input handler (iìïiïìï, deob-10.0.7.js:27781-27785) clamps it to 0-100 and sets the remainder to 100 - it
static void loot_input(const int top_share) {
    sync_forms();
    if (top_share) {
        S.loot_rem = 100 - S.loot_top_pct;
        HtmlElem in = html_dom_by_id("lootTopPct");
        char typed[32] = "", v[16];
        if (in) html_dom_get_value(in, typed, sizeof(typed));
        snprintf(v, sizeof(v), "%d", S.loot_top_pct);
        if (in && strcmp(typed, v)) html_dom_set_value(in, v);
        HtmlElem rem = html_dom_by_id("lootRemainder");
        snprintf(v, sizeof(v), "%d%%", S.loot_rem);
        if (rem) html_dom_set_text(rem, v);
    }
    char text[160];
    HtmlElem cut = html_dom_by_id("lootClanPct");
    const HtmlElem cut_field = cut ? html_dom_closest(cut, ".loot-field") : 0;
    HtmlElem hint = cut_field ? html_dom_query(cut_field, ".loot-mode-desc") : 0;
    snprintf(text, sizeof(text), "Percentage of rewards kept by the clan treasury. Players split the remaining %d%%.", 100 - S.loot_clan_pct);
    if (hint) html_dom_set_text(hint, text);
    HtmlElem mode = html_dom_by_id("lootMode");
    const HtmlElem mode_field = mode ? html_dom_closest(mode, ".loot-field") : 0;
    HtmlElem desc = mode_field ? html_dom_query(mode_field, ".loot-mode-desc") : 0;
    if (desc && !strcmp(S.loot_mode, "top_performance")) {
        snprintf(text, sizeof(text), "Only the top %d players are paid, in proportion to their kills.", S.loot_top_count);
        html_dom_set_text(desc, text);
    }
}

// one recruitment field's validation (field-error and its message under the input)
static void recruit_field(const char *id, const int ok, const char *message) {
    HtmlElem in = html_dom_by_id(id);
    if (!in) return;
    html_dom_set_class(in, "field-error", !ok);
    HtmlElem field = html_dom_closest(in, ".field");
    HtmlElem msg = field ? html_dom_query(field, ".field-error-msg") : 0;
    if (!ok && !msg) {
        char html[160];
        snprintf(html, sizeof(html), " <span class=\"field-error-msg svelte-1o06kdk\">%s</span>", message);
        html_dom_insert_html(in, HTML_INSERT_AFTER_END, html);
    } else if (ok && msg) {
        html_dom_remove(msg);
    }
}

// Min Level / Min K/D (bind:value): their checks show as typed and hold the Create / Update button
// (deob-10.0.7.js:96872-96874, :97034-97038)
static void recruit_input(void) {
    read_value("recruit-level", S.r_level, sizeof(S.r_level));
    read_value("recruit-kd", S.r_kd, sizeof(S.r_kd));
    const int lv = level_ok(S.r_level), kd = kd_ok(S.r_kd);
    recruit_field("recruit-level", lv, "Level must be a whole number between 0 and 200");
    recruit_field("recruit-kd", kd, "K/D must be between 0 and 99.99");
    HtmlElem save = html_dom_query(0, "#clan-settings .recruitment-manage .actions .io-button");
    if (!save) return;
    if (S.recruit_saving || !lv || !kd) html_dom_set_attr(save, "disabled", "");
    else html_dom_remove_attr(save, "disabled");
}

// ------------------------------------------------------------------ handler table

int clan_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name || (strncmp(name, "windows.12.", 11) && strcmp(name, "delClanConfirmed") && strcmp(name, "kickClanConfirmed"))) return -1;
    const char *m = name + 11;
    if (!strcmp(name, "delClanConfirmed")) { // _70159 + io.send("cln", 4, 1)
        wait_popup();
        send_cln(4, NULL, NULL);
    } else if (!strcmp(name, "kickClanConfirmed")) { // clearPops(); windows[12].kick(id)
        ui_loadout_clear_pops();
        const int id = arg_num(args, argc, 0, 0);
        send_cln(5, cJSON_CreateNumber(id), NULL);
        cJSON *members = cJSON_GetObjectItem(g_clan, "members");
        for (int i = cJSON_GetArraySize(members) - 1; i >= 0; i--)
            if ((int) jnum(cJSON_GetArrayItem(members, i), "i") == id) cJSON_DeleteItemFromArray(members, i);
        menu_rerender_if_open(13);
    } else if (!strcmp(m, "sendReq")) {
        const int action = arg_num(args, argc, 0, 1);
        char value[32] = "", id[16];
        snprintf(id, sizeof(id), "clanInp%d", action);
        read_value(id, value, sizeof(value));
        send_req(action, value);
    } else if (!strcmp(m, "switchTab")) { // tabIndex = i; updateWindow(13): the new tab's components mount
        g_clan_tab = arg_num(args, argc, 0, 0);
        menu_rerender_if_open(13);
    } else if (!strcmp(m, "kickReq")) { // confKickClan(name, id) (deob-10.0.7.js:223017-223020)
        const char *who = argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? args[0].str : "";
        StrBuf esc = {0};
        sb_append_esc(&esc, who);
        char *ewho = sb_finish(&esc);
        char text[256];
        I18N_F(text, "windows.clan.kick", "Are you sure you want kick {0} from the clan?", ewho);
        free(ewho);
        StrBuf sb = {0};
        sb_appendf(&sb, "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>%s</div><div id='confirmBtn' onclick='kickClanConfirmed(%d)' onmouseover='playTick(0.1)'>YES</div><div id='declineBtn' onclick='clearPops()' onmouseover='playTick(0.1)'>NO</div>",
                   text, arg_num(args, argc, 1, 0));
        char *html = sb_finish(&sb);
        generic_popup(html, "confPop", NULL);
        free(html);
    } else if (!strcmp(m, "delReq") && g_clan) { // confDelClan(isLeader) (deob-10.0.7.js:223009-223011)
        char html[640];
        snprintf(html, sizeof(html), "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>%s</div><div id='confirmBtn' onclick='delClanConfirmed()' onmouseover='playTick(0.1)'>YES</div><div id='declineBtn' onclick='clearPops()' onmouseover='playTick(0.1)'>NO</div>",
                 i_am_leader() ? I18N("windows.clan.delete", "Are you sure you want to delete the clan?") : I18N("windows.clan.leave", "Are you sure you want to leave the clan?"));
        generic_popup(html, "confPop", NULL);
    } else if (!strcmp(m, "updReq") && g_clan) { // io.send("clnR", id, ok); local list update
        const int index = arg_num(args, argc, 0, 0), pid = arg_num(args, argc, 1, 0), ok = arg_num(args, argc, 2, 0);
        char frame[64];
        snprintf(frame, sizeof(frame), "[\"clnR\",%d,%d]", pid, ok);
        backend_social_send_text(frame);
        cJSON *reqs = cJSON_GetObjectItem(g_clan, "requests");
        cJSON *members = cJSON_GetObjectItem(g_clan, "members");
        const cJSON *req = cJSON_GetArrayItem(reqs, index);
        if (req && ok == 1 && members) { // members.push({p}) (deob-10.0.7.js:220437-220441): no role until the clan is read again
            cJSON *nm = cJSON_CreateObject();
            cJSON_AddStringToObject(nm, "p", jstr(req, "player_name"));
            cJSON_AddItemToArray(members, nm);
        }
        if (req) cJSON_DeleteItemFromArray(reqs, index);
        menu_rerender_if_open(13);
    } else if (!strcmp(m, "declinePop") && g_clan && is_mgr()) {
        cont_popup("<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>Are you sure you want to Decline All clan requests?</div><div id='confirmBtn' onclick='windows[12].decAll();' onmouseover='playTick(0.1)'>YES</div><div id='declineBtn' onclick='clearPops()' onmouseover='playTick(0.1)'>NO</div>",
                   "433px", "2px solid #ffffff47");
    } else if (!strcmp(m, "decAll") && g_clan && is_mgr()) { // clearPops; ["clnRA"]; requests = []
        ui_loadout_clear_pops();
        backend_social_send_text("[\"clnRA\"]");
        cJSON_DeleteItemFromObject(g_clan, "requests");
        cJSON_AddItemToObject(g_clan, "requests", cJSON_CreateArray());
        menu_rerender_if_open(13);
    } else if (!strcmp(m, "discordPop") && g_clan) {
        if (is_mgr())
            cont_popup("<div style='font-size:20px;color:#fff'><div style='margin-top: 3px;font-size: 22px;color: rgba(255, 255, 255, 0.7);'>Discord Link:<input type='text' id='clanDiscordLink' class='inputGrey2' style='width: 300px;'></div><div class='mapFndB' onclick='windows[12].updateDis()' onmouseover='playTick(0.1)' style='background-color:#fc5a4e;width: 569px;margin-top: 10px;'>Update</div></div>", "588px", NULL);
        else if (jstr(g_clan, "clan_link4")[0]) {
            char url[160];
            snprintf(url, sizeof(url), "https://discord.gg/%s", jstr(g_clan, "clan_link4"));
            HtmlArg arg;
            memset(&arg, 0, sizeof(arg));
            arg.kind = HTML_ARG_STR;
            arg.str = url;
            html_ui_dispatch("openURL", ev, &arg, 1);
        }
    } else if (!strcmp(m, "updateDis") && g_clan && i_am_leader()) { // owner only (a Captain's Update does nothing)
        char v[256] = "";
        read_value("clanDiscordLink", v, sizeof(v));
        // deob-10.0.7.js:220483: the i flag, so Discord.GG is the same link as discord.gg.
        // The field is sent as typed; the brain keeps the invite code.
        if (!clan_discord_link_ok(v)) result_popup(0, "Invalid Discord");
        else send_cln(9, cJSON_CreateString(v), NULL);
    } else if (!strcmp(m, "updRol")) { // _70159 + io.send("cln", 6, id, {r})
        const int id = arg_num(args, argc, 0, 0);
        char sel[24], v[8] = "";
        snprintf(sel, sizeof(sel), "clanRolD%d", id);
        read_value(sel, v, sizeof(v));
        wait_popup();
        cJSON *extra = cJSON_CreateObject();
        cJSON_AddNumberToObject(extra, "r", atoi(v));
        send_cln(6, cJSON_CreateNumber(id), extra);
    } else if (!strcmp(m, "mangFunds") && g_clan) {
        manage_funds();
    } else if (!strcmp(m, "calcFee")) {
        calc_fee();
    } else if (!strcmp(m, "chngFunds")) { // _70159 + io.send("cln", 7, kr, {wd})
        char v[32] = "";
        read_value("clanFundsIn", v, sizeof(v));
        const long long kr = atoll(v);
        if (kr) {
            wait_popup();
            cJSON *extra = cJSON_CreateObject();
            cJSON_AddNumberToObject(extra, "wd", arg_num(args, argc, 0, 0));
            send_cln(7, cJSON_CreateNumber((double) kr), extra);
        }
    } else if (!strcmp(m, "transferRespond")) {
        transfer_send(2, arg_num(args, argc, 0, 0));
    } else if (!strcmp(m, "transferStart")) {
        transfer_send(0, 0);
    } else if (!strcmp(m, "transferCancel")) {
        transfer_send(1, 0);
    } else if (!strcmp(m, "motdSave")) {
        motd_send(0);
    } else if (!strcmp(m, "motdRemove")) {
        motd_send(1);
    } else if (!strcmp(m, "motdInput")) {
        motd_input();
    } else if (!strcmp(m, "clanRetry")) { // the clan's own load: sendReq(3) again on the render
        g_clan_failed = 0;
        g_req_sent = 0;
        clan_rerender();
    } else if (!strcmp(m, "bannerPick")) {
        const int i = arg_num(args, argc, 0, -1);
        if (i >= 0 && i < PAINTS && paint_count(k_paints[i].index) > 0) S.banner_sel = i;
        rerender_kept();
    } else if (!strcmp(m, "bannerApply")) {
        banner_apply();
    } else if (!strcmp(m, "lootChange")) {
        rerender_kept();
    } else if (!strcmp(m, "lootInput")) {
        loot_input(arg_num(args, argc, 0, 0));
    } else if (!strcmp(m, "recruitInput")) {
        recruit_input();
    } else if (!strcmp(m, "saveLoot")) {
        save_loot();
    } else if (!strcmp(m, "recruitSave")) {
        recruit_save();
    } else if (!strcmp(m, "recruitRemove")) {
        sync_forms();
        io_prompt("Remove Recruitment Post", "Are you sure you want to remove your recruitment post? Players will no longer see it in the recruitment board. You can re-post later (costs 1,000 KR).",
                  NULL, "Remove Post", "windows[12].recruitRemoveConfirmed()");
    } else if (!strcmp(m, "recruitRemoveConfirmed")) {
        recruit_remove();
    } else if (!strcmp(m, "logoPick")) {
        logo_pick();
    } else if (!strcmp(m, "logoUnset")) {
        logo_unset();
    } else if (!strcmp(m, "logoUpload")) {
        logo_upload();
    } else if (!strcmp(m, "logoRemove")) {
        io_prompt(I18N("windows.clans.settings.logo.remove.title", "Remove Clan Logo"),
                  I18N("windows.clans.settings.logo.remove.description", "Are you sure you want to remove the clan logo? This action cannot be undone."),
                  I18N("windows.clans.settings.logo.remove.warning", "The logo will be permanently removed from your clan."),
                  I18N("windows.clans.settings.logo.remove.confirm", "Remove Logo"), "windows[12].logoRemoveConfirmed()");
    } else if (!strcmp(m, "logoRemoveConfirmed")) {
        ui_loadout_clear_pops();
        char path[64];
        snprintf(path, sizeof(path), "/clans/%d/logo", clan_id());
        S.logo_err[0] = 0;
        S.logo_busy = v10_gapi("DELETE", path, NULL, on_logo, NULL) ? 2 : 0;
        if (!S.logo_busy) snprintf(S.logo_err, sizeof(S.logo_err), FETCH_FAILED);
        rerender_kept();
    } else {
        return -1;
    }
    return 1;
}

// windows[12].resp(msg, data, ok, action) / io_clnL()
void clan_ui_frame(const cJSON *frame) {
    const char *label = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 0));
    if (!label) return;
    if (!strcmp(label, "clnL")) { // del(): clanData = null; acc.clan = null; showWindow(5); tabIndex is kept
        const int tab = g_clan_tab;
        clan_ui_reset();
        g_clan_tab = tab;
        set_acc_clan(NULL);
        ui_loadout_clear_pops();
        menu_show_window(5, 1);
        return;
    }
    if (strcmp(label, "cln") || cJSON_GetArraySize(frame) < 5) return; // ["cln", msg, ok]: the hub page's join reply
    const char *msg = cJSON_GetStringValue(cJSON_GetArrayItem(frame, 1));
    const cJSON *data = cJSON_GetArrayItem(frame, 2);
    const int ok = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 3)) || cJSON_IsTrue(cJSON_GetArrayItem(frame, 3));
    const int action = (int) cJSON_GetNumberValue(cJSON_GetArrayItem(frame, 4));
    const int has_data = cJSON_IsObject(data);
    if (action == 7) {
        if (ok && has_data) {
            set_acc_funds(jnum(data, "pf")); // acc.funds = pf || 0
            if (g_clan) cJSON_ReplaceItemInObject(g_clan, "clan_funds", cJSON_CreateNumber(jnum(data, "cf")));
            menu_rerender_if_open(13);
            result_popup(1, "Success");
        } else {
            result_popup(0, msg && msg[0] ? msg : "Error"); // iíïííiì(null, msg || "Error")
        }
        return;
    }
    if (action == 6) {
        if (ok && has_data) {
            cJSON *mm = member_by_id((int) jnum(data, "i"));
            if (mm) {
                cJSON_DeleteItemFromObject(mm, "r");
                cJSON_AddNumberToObject(mm, "r", jnum(data, "r"));
                menu_rerender_if_open(13);
            }
            result_popup(1, "Success");
        } else {
            result_popup(0, msg && msg[0] ? msg : "Error");
        }
        return;
    }
    // any other action (the Discord link's 9 too: its popup stays open) is the generic answer (deob-10.0.7.js:220506-220509)
    if (action == 3) {
        g_req_sent = 0;
        html_timer_cancel(g_clan_timer);
        g_clan_timer = 0;
        g_clan_failed = 0;
        if (!has_data && !(msg && msg[0])) { // not in a clan any more (kicked, deleted): the create/join form
            set_acc_clan(NULL);
            menu_rerender_if_open(13);
            return;
        }
    }
    if (msg && msg[0] && html_dom_by_id("clanErr")) {
        clan_err(msg, ok);
    } else if (has_data) { // acc.clan = clan_name; clanData = data; updateWindow(13) (its components mount again)
        set_acc_clan(jstr(data, "clan_name")[0] ? jstr(data, "clan_name") : NULL);
        cJSON_Delete(g_clan);
        g_clan = cJSON_Duplicate(data, 1);
        if (!cJSON_GetObjectItem(g_clan, "members")) cJSON_AddItemToObject(g_clan, "members", cJSON_CreateArray());
        if (!cJSON_GetObjectItem(g_clan, "requests")) cJSON_AddItemToObject(g_clan, "requests", cJSON_CreateArray());
        // an answer without the clan's id (a new clan's create answer) asks for the clan, so the logo upload and the other
        // id-keyed calls work at once (v10.0.7: clan_id || sendReq(3), deob-10.0.7.js:220509: "uploading a clan logo right
        // after creating a clan")
        if (!clan_id() && !g_req_sent && !g_scenario) {
            g_req_sent = 1;
            if (send_cln(3, NULL, NULL)) {
                html_timer_cancel(g_clan_timer);
                g_clan_timer = html_timer_add(CLAN_WAIT_MS, clan_expired, NULL);
            } else {
                g_req_sent = 0;
            }
        }
        menu_rerender_if_open(13);
    }
}

// --render-scenario: state.clan = {data: clanData, tab, motd, loot, recruit} (no network)
void clan_ui_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state)) return;
    g_scenario = 1;
    clan_ui_reset();
    const cJSON *d = cJSON_GetObjectItem(state, "data");
    if (cJSON_IsObject(d)) g_clan = cJSON_Duplicate(d, 1);
    g_clan_tab = (int) jnum(state, "tab");
    const cJSON *motd = cJSON_GetObjectItem(state, "motd");
    S.motd = motd ? cJSON_Duplicate(motd, 1) : cJSON_CreateObject();
    S.motd_for = clan_id();
    snprintf(S.motd_text, sizeof(S.motd_text), "%s", jstr(S.motd, "message"));
    const cJSON *loot = cJSON_GetObjectItem(state, "loot");
    S.loot = loot ? cJSON_Duplicate(loot, 1) : cJSON_CreateObject();
    loot_from(S.loot);
    const cJSON *rec = cJSON_GetObjectItem(state, "recruit");
    S.recruit_loaded = 1;
    if (cJSON_IsObject(rec)) {
        S.recruit = cJSON_Duplicate(rec, 1);
        snprintf(S.r_desc, sizeof(S.r_desc), "%s", jstr(rec, "description"));
        snprintf(S.r_looking, sizeof(S.r_looking), "%s", jstr(rec, "looking_for"));
        snprintf(S.r_level, sizeof(S.r_level), "%g", jnum(rec, "min_level"));
        snprintf(S.r_kd, sizeof(S.r_kd), "%g", jnum(rec, "min_kd"));
        snprintf(S.r_region, sizeof(S.r_region), "%s", jstr(rec, "region"));
        snprintf(S.r_lang, sizeof(S.r_lang), "%s", jstr(rec, "language"));
    }
}
