// Krunker v10.0.2 Clans window (showWindow(13) = windows[12], v10:213330-213573) over the brain's
// social socket, plus its settings tab (the Svelte clan-settings component, v10:95700-96969).
//
// Socket frames (doc/backend.md 7.4): ["cln", action, value, extra] -> ["cln", msg, data, ok,
// action]: 1 create(tag) 2 request(tag) 3 my clan 4 leave/delete -> ["clnL"] 5 kick(id) 6 role(id,
// {r}) 7 funds(kr, {wd}) 9 Discord link; ["clnR", playerId, 1|0]; ["clnRA"] decline all; ["vcln"]
// on every render (OPEN_CLAN_TAB). The settings tab's gapi calls go through v10_gapi()
// (client/src/ui/v10_gapi.c, services/brain/clan_v10.c): /clans/<id>/motd | banner-color | logo |
// transfer. Deviation: v10's ownership-transfer socket RPCs (clan-transfer-initiate / -cancel /
// -respond) are POST / DELETE /clans/<id>/transfer and POST /clans/<id>/transfer/respond. The
// Turf Wars loot config and the recruitment board are kn_brain HTTP routes (turf.c, ?token=).
//
// Permissions as v10: owner = clanData.clan_playerid; clanRole = the account's clan_members rank
// (v10 account slot player_clanrole), read from clanData.members. mgr = owner or Captain (2):
// tabs 3 / 4, member controls, requests, Decline All, withdraw, the Discord popup, logo,
// recruitment. mgr2 = owner or role >= 2: MOTD, banner colour, loot config.
// v10's Svelte views are rendered as HTML strings with the same markup and svelte-* classes
// (ui/css/svelte.css); the confirm prompts it pushes on its view stack use #genericPop.
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
static int g_scenario;  // --render-scenario: no network

// settings tab / tab 0 state (the Svelte components' state)
static struct {
    // MOTD (svelte-15d5v89)
    cJSON *motd;          // {message, set_by_name, set_at} (owned), NULL = not loaded
    int motd_loading, motd_busy; // busy: 1 saving, 2 removing
    char motd_text[1024], motd_err[96];
    int motd_saved;       // "Saved!" for 3 s
    int64_t motd_for;     // clan id the MOTD belongs to
    // loot config (svelte-1ydnong) + the tab 0 display
    cJSON *loot;          // flat config (owned), NULL = not loaded
    int loot_loading, loot_saving, loot_saved, loot_failed;
    char loot_err[96];
    char loot_mode[24];
    int loot_clan_pct, loot_top_count, loot_top_pct, loot_rem, loot_min_games;
    // recruitment (svelte-1o06kdk)
    cJSON *recruit;       // the clan's post (owned) or NULL
    int recruit_loaded, recruit_loading, recruit_saving;
    char recruit_err[96], recruit_ok[96];
    char r_desc[2048], r_looking[512], r_level[16], r_kd[16], r_region[8], r_lang[8];
    // banner colour
    int banner_sel;       // index in k_paints, -1 none
    int banner_busy;
    char banner_err[96];
    // logo
    int logo_busy;
    char logo_err[96];
    // transfer ownership
    int transfer_busy;
    char transfer_err[96], transfer_name[40];
} S = {.banner_sel = -1};

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
    const AccountInfo *a = account_get();
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

static void send_cln(const int action, cJSON *value, cJSON *extra) {
    cJSON *f = cJSON_CreateArray();
    cJSON_AddItemToArray(f, cJSON_CreateString("cln"));
    cJSON_AddItemToArray(f, cJSON_CreateNumber(action));
    cJSON_AddItemToArray(f, value ? value : cJSON_CreateNumber(1));
    if (extra) cJSON_AddItemToArray(f, extra);
    backend_social_send(f);
    cJSON_Delete(f);
}

static void settings_reset(void) {
    cJSON_Delete(S.motd);
    cJSON_Delete(S.loot);
    cJSON_Delete(S.recruit);
    memset(&S, 0, sizeof(S));
    S.banner_sel = -1;
}

void clan_ui_reset(void) {
    cJSON_Delete(g_clan);
    g_clan = NULL;
    g_clan_tab = 0;
    g_req_sent = 0;
    settings_reset();
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

static int clan_rerender(void) {
    menu_rerender_if_open(13);
    return 1;
}

static void on_motd(void *user, const int status, const cJSON *data, const cJSON *error) {
    const int kind = (int) (intptr_t) user; // 0 load, 1 save, 2 remove
    S.motd_loading = 0;
    S.motd_busy = 0;
    if (status == 200 && !error) {
        cJSON_Delete(S.motd);
        S.motd = data ? cJSON_Duplicate(data, 1) : cJSON_CreateObject();
        snprintf(S.motd_text, sizeof(S.motd_text), "%s", jstr(S.motd, "message"));
        S.motd_err[0] = 0;
        if (kind == 1) S.motd_saved = 1;
    } else {
        static const char *const fails[] = {"Failed to load MOTD", "Failed to save MOTD", "Failed to delete MOTD"};
        snprintf(S.motd_err, sizeof(S.motd_err), "%s", error ? v10_gapi_error_text(error, fails[kind]) : fails[kind]);
        if (!S.motd) S.motd = cJSON_CreateObject();
    }
    clan_rerender();
}

static void fetch_motd(void) {
    if (g_scenario || !g_clan || S.motd_loading || (S.motd && S.motd_for == clan_id())) return;
    char path[64];
    snprintf(path, sizeof(path), "/clans/%d/motd", clan_id());
    S.motd_for = clan_id();
    S.motd_loading = v10_gapi("GET", path, NULL, on_motd, (void *) 0);
}

static void loot_from(const cJSON *cfg) {
    const char *mode = jstr(cfg, "distribution_mode");
    snprintf(S.loot_mode, sizeof(S.loot_mode), "%s", mode[0] ? mode : "clan_bank");
    S.loot_clan_pct = (int) jnum(cfg, "clan_pct");
    S.loot_top_count = cJSON_GetObjectItem(cfg, "top_player_count") ? (int) jnum(cfg, "top_player_count") : 20;
    S.loot_top_pct = cJSON_GetObjectItem(cfg, "top_player_pct") ? (int) jnum(cfg, "top_player_pct") : 80;
    S.loot_rem = 100 - S.loot_top_pct;
    S.loot_min_games = cJSON_GetObjectItem(cfg, "min_games_required") ? (int) jnum(cfg, "min_games_required") : 1;
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
        } else {
            cJSON_Delete(S.loot);
            S.loot = cJSON_Duplicate(row, 1);
            if (!jstr(S.loot, "updated_by_name")[0] && jstr(S.loot, "updated_by")[0])
                cJSON_AddStringToObject(S.loot, "updated_by_name", jstr(S.loot, "updated_by"));
            loot_from(S.loot);
        }
    } else {
        const char *e = cJSON_GetStringValue(cJSON_GetObjectItem(res, "message"));
        if (!e) e = cJSON_GetStringValue(cJSON_GetObjectItem(res, "error"));
        if (!e) e = cJSON_GetStringValue(cJSON_GetObjectItem(cJSON_GetObjectItem(res, "error"), "message"));
        snprintf(S.loot_err, sizeof(S.loot_err), "%s", e ? e : saving ? "Failed to save loot config" : "Failed to load loot config");
        if (!saving) {
            S.loot_failed = 1; // the tab 0 display treats it as null (clan bank)
            if (!S.loot) {
                S.loot = cJSON_CreateObject();
                loot_from(S.loot);
            }
        }
    }
    cJSON_Delete(res);
    clan_rerender();
}

static void fetch_loot(void) {
    if (g_scenario || S.loot || S.loot_loading || !g_clan || backend_available() != 1) return;
    char path[96];
    snprintf(path, sizeof(path), "/api/clans/%d/loot-config", clan_id());
    S.loot_loading = backend_http_get(path, on_loot, (void *) 0) != 0;
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
        S.recruit_loaded = 1; // errors are swallowed: "no post"
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
        snprintf(S.recruit_err, sizeof(S.recruit_err), "%s", e ? e : kind == 1 ? "Failed to save recruitment post" : "Failed to remove recruitment post");
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

static void fetch_recruit(void) {
    if (g_scenario || S.recruit_loaded || S.recruit_loading || !g_clan || backend_available() != 1) return;
    char url[600];
    recruit_url(url, sizeof(url));
    S.recruit_loading = backend_http_request("GET", url, NULL, on_recruit, (void *) 0) != 0;
}

// ------------------------------------------------------------------ tab 0 (home)

// clan-ownership-transfer-banner (_33655, svelte-1xrfe28): the transfer's target only
static void gen_transfer_banner(StrBuf *sb) {
    const cJSON *t = cJSON_GetObjectItem(g_clan, "ownership_transfer");
    sb_append(sb, "<div id=\"clan-ownership-transfer-banner\">");
    if (cJSON_IsObject(t) && (int) jnum(t, "to_player_id") == my_id() && my_id()) {
        sb_append(sb, "<div class=\"clan-ownership-transfer-banner svelte-1xrfe28\"><p class=\"svelte-1xrfe28\"><span class=\"clan-ownership-transfer-banner__player-name svelte-1xrfe28\">");
        sb_append_esc(sb, jstr(t, "from_player_name"));
        sb_append(sb, "</span> wants to transfer ownership of <span class=\"clan-ownership-transfer-banner__clan-name svelte-1xrfe28\">");
        sb_append_esc(sb, jstr(t, "clan_name"));
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
    const AccountInfo *a = account_get();
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
    sb_append(sb, "&quot;)'>[");
    sb_append_esc(sb, a ? a->clan : "");
    sb_appendf(sb, "] Clan Page</a><a style='float:right' href='javascript:;' onclick='windows[12].delReq()' class='menuLink'>%s</a></div>", leader ? "Delete" : "Leave");
    sb_append(sb, "<div class='setBodH'><div class='settName'>Rank<span class='floatR'>0</span></div>");
    sb_appendf(sb, "<div class='settName'>Clan Discord<a style='float:right' href='javascript:;' onclick='windows[12].discordPop()' class='menuLink'>%s</a></div>",
               leader ? "Update Link" : "Link Here");
    sb_appendf(sb, "<div class='settName'>Bank<span class='floatR'><span style='color:rgba(0,0,0,0.4)'><a href='javascript:;'  onclick='windows[12].mangFunds()' class='menuLink'>%s</a></span> KR</span></div>", funds);
    sb_appendf(sb, "<div class='settName'>Level<span class='floatR'>%d</span></div><div class='settName'>Total Score<span class='floatR'>%s</span></div>", level, total);
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
            if (!((leader_id == id && k_clan_roles[r].perms == 2) || ((int) jnum(m, "r") == r && leader_id != id))) continue;
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
    if (!i) sb_append(sb, "<div style='margin-top:10px;color:rgba(255,255,255,0.4)'>No Requests</div>");
}

// ------------------------------------------------------------------ tab 4 (clan-settings)

static void gen_logo(StrBuf *sb) {
    const int logo = (int) jnum(g_clan, "clan_logo"), pending = (int) jnum(g_clan, "clan_logo_pending");
    char funds[32];
    comma(funds, sizeof(funds), jnum(g_clan, "clan_funds"));
    sb_append(sb, "<div class=\"setHed\" style=\"margin-top:20px\">Clan Logo</div> <div class=\"logo-upload-container svelte-1w0uz1r\"><div class=\"io-card\"><div class=\"io-input-container svelte-1w0uz1r\">");
    if (logo || pending) {
        sb_append(sb, "<div class=\"logo-preview-section svelte-1w0uz1r\"><div class=\"label-row\"><span class=\"input-label\">Current Logo</span></div> <div class=\"logo-preview-container svelte-1w0uz1r\">");
        if (logo)
            sb_appendf(sb, "<div class=\"logo-preview svelte-1w0uz1r\"><img alt=\"Clan logo\" class=\"logo-image svelte-1w0uz1r\" src=\"https://user-assets.krunker.io/clans/%d.png?v=%d\"> <div class=\"logo-status approved svelte-1w0uz1r\">Approved</div></div>", clan_id(), logo);
        if (pending)
            sb_appendf(sb, "<div class=\"logo-preview svelte-1w0uz1r\"><img alt=\"Pending logo\" class=\"logo-image svelte-1w0uz1r\" src=\"https://user-assets.krunker.io/clans/%d_pending.png?v=%d\"> <div class=\"logo-status pending svelte-1w0uz1r\">Pending Approval</div></div>", clan_id(), pending);
        sb_append(sb, "</div></div> ");
    }
    if (!pending) {
        // KrunkNative: no file picker and no logo upload pipeline (clan_v10.c refuses POST /logo),
        // so the drop target is v10's disabled state
        sb_appendf(sb, "<div class=\"logo-upload-section svelte-1w0uz1r\"><div class=\"label-row\"><span class=\"input-label\">Upload Logo</span></div> <div class=\"logo-upload-info svelte-1w0uz1r\"><p class=\"upload-requirements svelte-1w0uz1r\">Requirements: 512x512 PNG, max 256KB. Cost: 20,000 krunkies</p> <p class=\"clan-funds svelte-1w0uz1r\">Clan Funds: %s KR</p></div> "
                       "<div class=\"file-drop-target svelte-145grbc disabled\" role=\"button\" tabindex=\"-1\" aria-label=\"Drop PNG file here\"><input type=\"file\" style=\"display: none;\" class=\"svelte-145grbc\" accept=\".png,image/png\"> <div class=\"file-drop-content svelte-145grbc\"><div class=\"file-empty-state svelte-145grbc\"><i class=\"material-icons add-icon svelte-145grbc\">add</i> <div class=\"drop-label svelte-145grbc\">Drop PNG file here</div> <div class=\"accepted-types svelte-145grbc\">.png, image/png</div> <div class=\"size-limit svelte-145grbc\">Max: 256KB</div></div></div></div></div> ",
                   funds);
    } else {
        sb_append(sb, "<div class=\"pending-notice svelte-1w0uz1r\"><p class=\"svelte-1w0uz1r\">A logo is pending approval. Please wait for admin review before uploading a new one.</p></div> ");
    }
    if (logo && !pending)
        sb_appendf(sb, "<button class=\"io-button io-button--danger\" onclick=\"windows[12].logoRemove()\"%s>%s</button> ", S.logo_busy ? " disabled" : "", S.logo_busy ? "Removing..." : "Remove Logo");
    if (S.logo_err[0]) {
        sb_append(sb, "<div class=\"io-message error-message svelte-1w0uz1r\">Error: ");
        sb_append_esc(sb, S.logo_err);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div></div>");
}

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
    const char *msg = jstr(S.motd, "message");
    if (manage) {
        sb_appendf(sb, "<textarea id=\"clanMotdInput\" class=\"motd-input svelte-15d5v89\" placeholder=\"Set a message for clan members...\" maxlength=\"500\" rows=\"3\"%s>", S.motd_busy ? " disabled" : "");
        sb_append_esc(sb, S.motd_text);
        sb_append(sb, "</textarea> <div class=\"motd-actions svelte-15d5v89\">");
        sb_appendf(sb, "<button class=\"main-button svelte-whnh40 main-button-accept main-button-slim%s\" type=\"button\" onclick=\"windows[12].motdSave()\">%s</button> ",
                   S.motd_busy || !S.motd_text[0] ? " main-button-disabled" : "", S.motd_busy == 1 ? "Saving..." : "Save");
        if (msg[0])
            sb_appendf(sb, "<button class=\"main-button svelte-whnh40 main-button-danger main-button-slim%s\" type=\"button\" onclick=\"windows[12].motdRemove()\">%s</button> ",
                       S.motd_busy ? " main-button-disabled" : "", S.motd_busy == 2 ? "Removing..." : "Remove");
        if (S.motd_saved) sb_append(sb, "<span class=\"motd-saved svelte-15d5v89\">Saved!</span>");
        sb_append(sb, "</div>");
    } else if (msg[0]) {
        sb_append(sb, "<div class=\"motd-display svelte-15d5v89\">");
        sb_append_esc(sb, msg);
        sb_append(sb, "</div>");
    } else {
        sb_append(sb, "<div class=\"motd-empty svelte-15d5v89\">No message set</div>");
    }
    const double at = jnum(S.motd, "set_at");
    if (jstr(S.motd, "set_by_name")[0] && at > 0) {
        // toLocaleDateString() en-US: M/D/YYYY (UTC; civil date from days since the epoch)
        long long z = (long long) floor(at / 86400000.0) + 719468;
        const long long era = (z >= 0 ? z : z - 146096) / 146097;
        const long long doe = z - era * 146097, yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
        const long long doy = doe - (365 * yoe + yoe / 4 - yoe / 100), mp = (5 * doy + 2) / 153;
        const int day = (int) (doy - (153 * mp + 2) / 5 + 1), mon = (int) (mp < 10 ? mp + 3 : mp - 9);
        const int year = (int) (yoe + era * 400 + (mon <= 2));
        char d[32];
        snprintf(d, sizeof(d), "%d/%d/%d", mon, day, year);
        sb_append(sb, " <div class=\"motd-meta svelte-15d5v89\">Set by ");
        sb_append_esc(sb, jstr(S.motd, "set_by_name"));
        sb_appendf(sb, " on %s</div>", d);
    }
    sb_append(sb, "</div>");
}

// owned paints: account data slot 10 [{ind, cnt}], counts summed per index
static int paint_count(const int index) {
    const AccountInfo *a = account_get();
    if (!a || !a->data) return 0;
    int n = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, cJSON_GetArrayItem(a->data, 10)) if ((int) jnum(e, "ind") == index) n += (int) jnum(e, "cnt");
    return n;
}

static void paint_thumb(const int i, char *out, const size_t n) {
    char path[96];
    snprintf(path, sizeof(path), "textures/paints/paint_%s.webp", k_paints[i].id);
    html_dom_asset_url(path, out, n);
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
            char thumb[256];
            paint_thumb(i, thumb, sizeof(thumb));
            sb_appendf(sb, "<button type=\"button\" class=\"swatch svelte-1w0uz1r%s\" role=\"radio\" aria-checked=\"%s\" aria-label=\"%s — %d owned\" onclick=\"windows[12].bannerPick(%d)\">"
                           "<img class=\"swatch-img svelte-1w0uz1r\" alt=\"\" draggable=\"false\" src=\"%s\"> <span class=\"swatch-count svelte-1w0uz1r\">%d</span> <span class=\"swatch-name svelte-1w0uz1r\">%s</span></button>",
                       S.banner_sel == i ? " selected" : "", S.banner_sel == i ? "true" : "false", k_paints[i].name, owned[i], i, thumb, owned[i], k_paints[i].name);
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
    if (!S.loot_mode[0]) loot_from(S.loot);
    const char *by = jstr(S.loot, "updated_by_name");
    if (by[0]) {
        sb_append(sb, "<div class=\"loot-updated-info svelte-1ydnong\">Last updated by <span class=\"loot-updated-name svelte-1ydnong\">");
        sb_append_esc(sb, by);
        sb_append(sb, "</span>");
        const char *at = jstr(S.loot, "updated_at");
        if (at[0]) {
            sb_append(sb, " <span class=\"loot-updated-time svelte-1ydnong\">on ");
            sb_append_esc(sb, at);
            sb_append(sb, "</span>");
        }
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
        sb_appendf(sb, "<div class=\"loot-field svelte-1ydnong\"><div class=\"label-row\"><label for=\"lootClanPct\" class=\"input-label\">Clan Treasury Cut (%%)</label></div> <input id=\"lootClanPct\" class=\"io-input-field\" type=\"number\" min=\"0\" max=\"100\" value=\"%d\" oninput=\"windows[12].lootInput()\"%s> <p class=\"loot-mode-desc svelte-1ydnong\">Percentage of rewards kept by the clan treasury. Players split the remaining %d%%.</p></div> ",
                   S.loot_clan_pct, dis, 100 - S.loot_clan_pct);
    if (mi == 2 || mi == 4)
        sb_appendf(sb, "<div class=\"loot-field svelte-1ydnong\"><div class=\"label-row\"><label for=\"lootTopCount\" class=\"input-label\">Top Player Count</label></div> <input id=\"lootTopCount\" class=\"io-input-field\" type=\"number\" min=\"1\" max=\"100\" value=\"%d\"%s></div> ",
                   S.loot_top_count, dis);
    if (mi == 2)
        sb_appendf(sb, "<div class=\"loot-field svelte-1ydnong\"><div class=\"label-row\"><label for=\"lootTopPct\" class=\"input-label\">Top Players Share (%%)</label></div> <input id=\"lootTopPct\" class=\"io-input-field\" type=\"number\" min=\"0\" max=\"100\" value=\"%d\" oninput=\"windows[12].lootInput()\"%s></div> "
                       "<div class=\"loot-field svelte-1ydnong\"><div class=\"label-row\"><span class=\"input-label\">Remainder Share (%%)</span></div> <div id=\"lootRemainder\" class=\"loot-remainder-display svelte-1ydnong\">%d%%</div></div> ",
                   S.loot_top_pct, dis, S.loot_rem);
    if (mi != 0)
        sb_appendf(sb, "<div class=\"loot-field svelte-1ydnong\"><div class=\"label-row\"><label for=\"lootMinGames\" class=\"input-label\">Minimum Games Required</label></div> <input id=\"lootMinGames\" class=\"io-input-field\" type=\"number\" min=\"0\" max=\"100\" value=\"%d\"%s></div> ",
                   S.loot_min_games, dis);
    if (manage)
        sb_appendf(sb, "<button class=\"main-button svelte-whnh40%s\" type=\"button\" onclick=\"windows[12].saveLoot()\">%s</button> ", S.loot_saving ? " main-button-disabled" : "",
                   S.loot_saving ? "Saving..." : "Save Loot Config");
    if (S.loot_saved) sb_append(sb, "<div class=\"io-message success-message svelte-1ydnong\">Config saved successfully!</div> ");
    if (S.loot_err[0] && !S.loot_failed) {
        sb_append(sb, "<div class=\"io-message error-message svelte-1ydnong\">Error: ");
        sb_append_esc(sb, S.loot_err);
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div></div></div>");
}

static void select_opts(StrBuf *sb, const char *const (*opts)[2], const int n, const char *cur) {
    for (int i = 0; i < n; i++) sb_appendf(sb, "<option value=\"%s\"%s>%s</option>", opts[i][0], !strcmp(cur, opts[i][0]) ? " selected" : "", opts[i][1]);
}

static int level_ok(const char *v) {
    if (!v[0]) return 1;
    char *end;
    const double d = strtod(v, &end);
    return !*end && d >= 0 && d <= 200 && floor(d) == d;
}

static int kd_ok(const char *v) {
    if (!v[0]) return 1;
    char *end;
    const double d = strtod(v, &end);
    return !*end && d >= 0 && d <= 99.99;
}

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
    sb_appendf(sb, "\"> <div class=\"row svelte-1o06kdk\"><div class=\"field svelte-1o06kdk\"><div class=\"label-row\"><span class=\"input-label\">Min Level</span></div> <input id=\"recruit-level\" class=\"io-input-field svelte-1o06kdk%s\" type=\"text\" placeholder=\"0\" value=\"",
               lv_ok ? "" : " field-error");
    sb_append_esc(sb, S.r_level);
    sb_append(sb, "\">");
    if (!lv_ok) sb_append(sb, " <span class=\"field-error-msg svelte-1o06kdk\">Level must be a whole number between 0 and 200</span>");
    sb_appendf(sb, "</div> <div class=\"field svelte-1o06kdk\"><div class=\"label-row\"><span class=\"input-label\">Min K/D</span></div> <input id=\"recruit-kd\" class=\"io-input-field svelte-1o06kdk%s\" type=\"text\" placeholder=\"0.00\" value=\"", kd_v ? "" : " field-error");
    sb_append_esc(sb, S.r_kd);
    sb_append(sb, "\">");
    if (!kd_v) sb_append(sb, " <span class=\"field-error-msg svelte-1o06kdk\">K/D must be between 0 and 99.99</span>");
    sb_append(sb, "</div></div> <div class=\"row svelte-1o06kdk\"><div class=\"field svelte-1o06kdk\"><div class=\"label-row\"><span class=\"input-label\">Region</span></div> <select id=\"recruit-region\" class=\"io-input-field\">");
    select_opts(sb, k_regions, (int) (sizeof(k_regions) / sizeof(k_regions[0])), S.r_region);
    sb_append(sb, "</select></div> <div class=\"field svelte-1o06kdk\"><div class=\"label-row\"><span class=\"input-label\">Language</span></div> <select id=\"recruit-lang\" class=\"io-input-field\">");
    select_opts(sb, k_langs, (int) (sizeof(k_langs) / sizeof(k_langs[0])), S.r_lang);
    sb_appendf(sb, "</select></div></div> <div class=\"actions svelte-1o06kdk\"><button class=\"io-button\" onclick=\"windows[12].recruitSave()\"%s>%s</button>",
               S.recruit_saving || !lv_ok || !kd_v ? " disabled" : "", S.recruit_saving ? "Saving..." : active ? "Update Post" : "Create Post (1,000 KR)");
    if (active) sb_append(sb, " <button class=\"io-button io-button--danger\" onclick=\"windows[12].recruitRemove()\">Remove Post</button>");
    sb_append(sb, "</div></div></div></div>");
}

static void gen_transfer(StrBuf *sb) {
    const cJSON *t = cJSON_GetObjectItem(g_clan, "ownership_transfer");
    sb_append(sb, "<div class=\"setHed\" style=\"margin-top:20px\">Transfer Ownership</div> <div class=\"transfer-ownership-container svelte-1w0uz1r\">");
    if (cJSON_IsObject(t)) {
        sb_append(sb, "<div class=\"io-card\"><div class=\"io-input-container svelte-1w0uz1r\"><div class=\"pending-transfer-info svelte-1w0uz1r\"><div class=\"label-row\"><span class=\"input-label\">Pending transfer to:</span></div> <div class=\"highlight-name svelte-1w0uz1r\">");
        sb_append_esc(sb, jstr(t, "to_player_name"));
        sb_appendf(sb, "</div></div> <button class=\"main-button svelte-whnh40 main-button-danger%s\" type=\"button\" onclick=\"windows[12].transferCancel()\">%s</button></div></div>",
                   S.transfer_busy ? " main-button-disabled" : "", S.transfer_busy ? "Cancelling..." : "Cancel Transfer");
    } else {
        sb_append(sb, "<div class=\"io-card\"><div class=\"io-input-container svelte-1w0uz1r\"><div class=\"warning-container svelte-1w0uz1r\"><span class=\"warning-badge svelte-1w0uz1r\">Danger</span> <p class=\"warning svelte-1w0uz1r\">Warning: The new owner will have full access to the clan, including clan bank funds. You will lose access to all clan ownership features. You can cancel the transfer at any time before it is accepted. Once accepted, the transfer is irreversible.</p></div> "
                      "<div class=\"label-row\"><label for=\"newOwnerName\" class=\"input-label\">Player Name</label></div> <input id=\"newOwnerName\" class=\"io-input-field\" type=\"text\" placeholder=\"Enter player name\" autocomplete=\"username\" value=\"");
        sb_append_esc(sb, S.transfer_name);
        sb_appendf(sb, "\"> <button class=\"main-button svelte-whnh40 main-button-danger%s\" type=\"button\" onclick=\"windows[12].transferStart()\">%s</button></div></div>",
                   S.transfer_busy ? " main-button-disabled" : "", S.transfer_busy ? "Transferring..." : "Transfer Ownership");
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

char *clan_window_gen(void) {
    StrBuf sb = {0};
    const AccountInfo *a = account_get();
    if (!a) {
        sb_append(&sb, "<div class='reqW login' onclick='showWindow(5)'><i class='material-icons reqWIcon'>clear</i><div class='reqWLvl'>LOGIN REQUIRED</div><div class='reqWTxt'>to create and join a clan!</div></div>");
        return sb_finish(&sb);
    }
    if (!a->clan[0]) {
        sb_append(&sb, "<div class='setHed' style='margin-top:-5px'>Create Clan</div><input id='clanInp1' type='text' class='smlInput' maxlength='4' placeholder='Clan Name' /><a class='clanSubmitBtn' style='margin-left:20px' onclick='windows[12].sendReq(1)'>Create</a><div class='setHed'>Join Clan <a href='javascript:void(0)' onclick=\"window.openLeaderboards('clans')\" class='menuLink' style='float:right'>View Clans</a></div><input id='clanInp2' type='text' class='smlInput' maxlength='5' placeholder='Clan Name' /><a class='clanSubmitBtn' style='margin-left:20px' onclick='windows[12].sendReq(2)'>Submit</a><div id='clanErr' style='margin-top:10px'></div>");
        return sb_finish(&sb);
    }
    if (!g_scenario) backend_social_send_text("[\"vcln\"]"); // OPEN_CLAN_TAB on every render
    const int mgr = g_clan && is_mgr();
    if (g_clan_tab >= 3 && !mgr && g_clan) g_clan_tab = 0; // KrunkNative: a stale manager tab after a demotion
    sb_append(&sb, "<div style='height:15px;'></div><div class='bigMenTabs'>");
    for (int i = 0; i < CLAN_TABS; i++) {
        if (i >= 3 && !mgr) continue;
        sb_appendf(&sb, "<div class='bigMenTab%s' style='font-size:16px' onmouseenter='playTick(0.1);' onclick='playSelect();windows[12].switchTab(%d)'>%s</div>", g_clan_tab == i ? " bigMenTabA" : "", i, k_clan_tabs[i]);
    }
    sb_append(&sb, "</div><div id='clanErr' style='margin-top:0px;color:rgba(0,0,0,0.5)'>");
    if (!g_clan) {
        sb_append(&sb, "LOADING...</div>");
        if (!g_req_sent && !g_scenario) {
            g_req_sent = 1;
            send_cln(3, NULL, NULL); // this.sendReq(3)
        }
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
        sb_append_esc(&sb, text && text[0] ? text : "Failed. Try Again");
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
        html_dom_set_html(e, "LOADING...");
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

// calcFee(input): 10% deposit fee preview
static void calc_fee(void) {
    HtmlElem in = html_dom_by_id("clanFundsIn");
    char v[32] = "";
    if (in) html_dom_get_value(in, v, sizeof(v));
    long long kr = atoll(v);
    if (kr < 0) kr = 0;
    const long long fee = kr / 10;
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
    S.loot_clan_pct = clampi(read_int("lootClanPct", S.loot_clan_pct), 0, 100);
    S.loot_top_count = clampi(read_int("lootTopCount", S.loot_top_count), 1, 100);
    S.loot_top_pct = clampi(read_int("lootTopPct", S.loot_top_pct), 0, 100);
    S.loot_rem = 100 - S.loot_top_pct;
    S.loot_min_games = clampi(read_int("lootMinGames", S.loot_min_games), 0, 100);
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

static void saved_off(void *user) {
    (void) user;
    S.motd_saved = 0;
    S.loot_saved = 0;
    clan_rerender();
}

static void motd_send(const int remove) {
    if (!g_clan || S.motd_busy) return;
    sync_forms();
    char path[64];
    snprintf(path, sizeof(path), "/clans/%d/motd", clan_id());
    cJSON *body = NULL;
    if (!remove) {
        char *t = S.motd_text;
        while (*t == ' ' || *t == '\n') t++;
        if (!*t) return;
        body = cJSON_CreateObject();
        cJSON_AddStringToObject(body, "message", t);
    }
    S.motd_err[0] = 0;
    S.motd_saved = 0;
    S.motd_busy = remove ? 2 : 1;
    if (!v10_gapi(remove ? "DELETE" : "PUT", path, body, on_motd, (void *) (intptr_t) (remove ? 2 : 1))) {
        S.motd_busy = 0;
        snprintf(S.motd_err, sizeof(S.motd_err), remove ? "Failed to delete MOTD" : "Failed to save MOTD");
    } else if (!remove) {
        html_timer_add(3000, saved_off, NULL);
    }
    cJSON_Delete(body);
    clan_rerender();
}

static void on_banner(void *user, const int status, const cJSON *data, const cJSON *error) {
    const int paint = (int) (intptr_t) user;
    S.banner_busy = 0;
    if (status == 200 && !error && g_clan) {
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
        snprintf(S.banner_err, sizeof(S.banner_err), "%s", error ? v10_gapi_error_text(error, "Failed to apply banner color") : "Failed to apply banner color");
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
    S.banner_busy = v10_gapi("POST", path, body, on_banner, (void *) (intptr_t) S.banner_sel);
    if (!S.banner_busy) snprintf(S.banner_err, sizeof(S.banner_err), "Failed to apply banner color");
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
    char *json = cJSON_PrintUnformatted(cfg);
    cJSON_Delete(cfg);
    char token[256], url[600];
    backend_url_encode(token, sizeof(token), account_session_token());
    snprintf(url, sizeof(url), "%s/api/clans/%d/loot-config?token=%s", backend_url(), clan_id(), token);
    S.loot_err[0] = 0;
    S.loot_saved = 0;
    S.loot_failed = 0;
    S.loot_saving = backend_http_request("PUT", url, json ? json : "{}", on_loot, (void *) 1) != 0;
    if (!S.loot_saving) snprintf(S.loot_err, sizeof(S.loot_err), "Failed to save loot config");
    else html_timer_add(3000, saved_off, NULL);
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
    char *json = cJSON_PrintUnformatted(body);
    cJSON_Delete(body);
    char url[600];
    recruit_url(url, sizeof(url));
    S.recruit_err[0] = S.recruit_ok[0] = 0;
    S.recruit_saving = backend_http_request("POST", url, json ? json : "{}", on_recruit, (void *) 1) != 0;
    if (!S.recruit_saving) snprintf(S.recruit_err, sizeof(S.recruit_err), "Failed to save recruitment post");
    free(json);
    clan_rerender();
}

static void recruit_remove(void) {
    ui_loadout_clear_pops();
    char url[600];
    recruit_url(url, sizeof(url));
    S.recruit_err[0] = S.recruit_ok[0] = 0;
    if (!backend_http_request("DELETE", url, NULL, on_recruit, (void *) 2)) snprintf(S.recruit_err, sizeof(S.recruit_err), "Failed to remove recruitment post");
    clan_rerender();
}

static void on_logo(void *user, const int status, const cJSON *data, const cJSON *error) {
    (void) user;
    (void) data;
    S.logo_busy = 0;
    if (status == 200 && !error) {
        S.logo_err[0] = 0;
        send_cln(3, NULL, NULL); // windows[12].sendReq(3)
    } else {
        snprintf(S.logo_err, sizeof(S.logo_err), "%s", error ? v10_gapi_error_text(error, "Failed to remove logo") : "Failed to remove logo");
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

static void transfer_send(const int kind, const int accept) {
    if (!g_clan || S.transfer_busy) return;
    sync_forms();
    char path[64];
    snprintf(path, sizeof(path), kind == 2 ? "/clans/%d/transfer/respond" : "/clans/%d/transfer", clan_id());
    cJSON *body = NULL;
    if (kind == 0) {
        if (!S.transfer_name[0]) {
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
    } else if (!strcmp(m, "switchTab")) {
        sync_forms();
        g_clan_tab = arg_num(args, argc, 0, 0);
        if (g_clan_tab == 4) { // the settings component mounts again: fresh loads
            cJSON_Delete(S.motd);
            S.motd = NULL;
            S.motd_err[0] = 0;
            S.recruit_loaded = 0;
            cJSON_Delete(S.loot);
            S.loot = NULL;
            S.loot_failed = 0;
            S.loot_err[0] = 0;
            S.loot_mode[0] = 0;
            S.banner_err[0] = S.logo_err[0] = S.transfer_err[0] = S.recruit_err[0] = S.recruit_ok[0] = 0;
        }
        menu_rerender_if_open(13);
    } else if (!strcmp(m, "kickReq")) { // confKickClan(name, id)
        const char *who = argc > 0 && args[0].kind == HTML_ARG_STR && args[0].str ? args[0].str : "";
        StrBuf sb = {0};
        sb_append(&sb, "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>Are you sure you want kick ");
        sb_append_esc(&sb, who);
        sb_appendf(&sb, " from the clan?</div><div id='confirmBtn' onclick='kickClanConfirmed(%d)' onmouseover='playTick(0.1)'>YES</div><div id='declineBtn' onclick='clearPops()' onmouseover='playTick(0.1)'>NO</div>", arg_num(args, argc, 1, 0));
        char *html = sb_finish(&sb);
        generic_popup(html, "confPop", NULL);
        free(html);
    } else if (!strcmp(m, "delReq") && g_clan) { // confDelClan(isLeader)
        char html[640];
        snprintf(html, sizeof(html), "<div style='color:#fff;font-size:20px;position:relative;margin-bottom:15px;'>Are you sure you want to %s the clan?</div><div id='confirmBtn' onclick='delClanConfirmed()' onmouseover='playTick(0.1)'>YES</div><div id='declineBtn' onclick='clearPops()' onmouseover='playTick(0.1)'>NO</div>",
                 i_am_leader() ? "delete" : "leave");
        generic_popup(html, "confPop", NULL);
    } else if (!strcmp(m, "updReq") && g_clan) { // io.send("clnR", id, ok); local list update
        const int index = arg_num(args, argc, 0, 0), pid = arg_num(args, argc, 1, 0), ok = arg_num(args, argc, 2, 0);
        char frame[64];
        snprintf(frame, sizeof(frame), "[\"clnR\",%d,%d]", pid, ok);
        backend_social_send_text(frame);
        cJSON *reqs = cJSON_GetObjectItem(g_clan, "requests");
        cJSON *members = cJSON_GetObjectItem(g_clan, "members");
        const cJSON *req = cJSON_GetArrayItem(reqs, index);
        if (req && ok == 1 && members) {
            cJSON *nm = cJSON_CreateObject();
            cJSON_AddStringToObject(nm, "p", jstr(req, "player_name"));
            cJSON_AddNumberToObject(nm, "i", pid);
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
        const char *p = strstr(v, "discord");
        int valid = 0;
        for (; p && !valid; p = strstr(p + 1, "discord")) {
            const char *q = p + 7;
            if (!strncmp(q, ".com/", 5)) q += 5;
            else if (!strncmp(q, "app.com/", 8)) q += 8;
            else if (!strncmp(q, ".gg/", 4)) q += 4;
            else continue;
            if (!strncmp(q, "invite/", 7)) q += 7;
            int len = 0;
            while (len < 32 && (q[len] == '-' || (q[len] >= '0' && q[len] <= '9') || ((q[len] | 32) >= 'a' && (q[len] | 32) <= 'z'))) len++;
            valid = len >= 2;
        }
        if (!valid) result_popup(0, "Invalid Discord");
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
    } else if (!strcmp(m, "bannerPick")) {
        const int i = arg_num(args, argc, 0, -1);
        if (i >= 0 && i < PAINTS && paint_count(k_paints[i].index) > 0) S.banner_sel = i;
        rerender_kept();
    } else if (!strcmp(m, "bannerApply")) {
        banner_apply();
    } else if (!strcmp(m, "lootChange")) {
        rerender_kept();
    } else if (!strcmp(m, "lootInput")) { // the remainder follows the top share; the clan cut's hint
        sync_forms();
        HtmlElem rem = html_dom_by_id("lootRemainder");
        if (rem) {
            char v[16];
            snprintf(v, sizeof(v), "%d%%", S.loot_rem);
            html_dom_set_text(rem, v);
        }
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
    } else if (!strcmp(m, "logoRemove")) {
        io_prompt("Remove Clan Logo", "Are you sure you want to remove the clan logo? This action cannot be undone.", "The logo will be permanently removed from your clan.",
                  "Remove Logo", "windows[12].logoRemoveConfirmed()");
    } else if (!strcmp(m, "logoRemoveConfirmed")) {
        ui_loadout_clear_pops();
        char path[64];
        snprintf(path, sizeof(path), "/clans/%d/logo", clan_id());
        S.logo_busy = v10_gapi("DELETE", path, NULL, on_logo, NULL);
        if (!S.logo_busy) snprintf(S.logo_err, sizeof(S.logo_err), "Failed to remove logo");
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
    if (!strcmp(label, "clnL")) { // del(): clanData = null; acc.clan = null; showWindow(5)
        clan_ui_reset();
        account_set_clan(NULL);
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
            account_set_funds(jnum(data, "pf"));
            if (g_clan) cJSON_ReplaceItemInObject(g_clan, "clan_funds", cJSON_CreateNumber(jnum(data, "cf")));
            menu_rerender_if_open(13);
            result_popup(1, "Success");
        } else {
            result_popup(0, msg ? msg : NULL);
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
            result_popup(0, msg ? msg : NULL);
        }
        return;
    }
    if (action == 9 && ok) ui_loadout_clear_pops(); // the Discord popup: the link is saved
    if (action == 9 && !ok && msg) {
        result_popup(0, msg);
        return;
    }
    if (action == 3) {
        g_req_sent = 0;
        if (!has_data && !(msg && msg[0])) { // not in a clan any more (kicked, deleted): the create/join form
            account_set_clan(NULL);
            menu_rerender_if_open(13);
            return;
        }
    }
    if (msg && msg[0] && html_dom_by_id("clanErr")) {
        clan_err(msg, ok);
    } else if (has_data && cJSON_GetObjectItem(data, "clan_name")) {
        account_set_clan(jstr(data, "clan_name"));
        sync_forms();
        cJSON_Delete(g_clan);
        g_clan = cJSON_Duplicate(data, 1);
        if (!cJSON_GetObjectItem(g_clan, "members")) cJSON_AddItemToObject(g_clan, "members", cJSON_CreateArray());
        if (!cJSON_GetObjectItem(g_clan, "requests")) cJSON_AddItemToObject(g_clan, "requests", cJSON_CreateArray());
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
