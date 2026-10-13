// Krunker v10.0.2 Turf Wars window (doc/v10/ranked-impl.md): v10's _51011 popup (#menuBtnTurfWars,
// icon flag) as a KrunkNative window (TURF_WINDOW). Tabs Overview / My Clan / Leaderboards / How to
// Play over the brain's /api/territorial-wars/* routes (services/brain/turf.c). The territory map
// is v10's: the three map polygons (paths and label positions from the bundle) drawn as an SVG
// image over a dark panel (v10's ./img/territory_map.webp backdrop is not shipped: it is a page-local
// image the engine cannot decode).
#include <backend.h>
#include <backend_ui.h>
#include <ui_logic.h>
#include <html_ui.h>
#include <turf_wars.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TURF_TTL_MS 30000

enum { TW_OVERVIEW, TW_CLAN, TW_BOARDS, TW_HOWTO, TW_TABS };
static const char *const k_tw_tabs[TW_TABS] = {"Overview", "My Clan", "Leaderboards", "How to Play"};

// v10 territory map: the per-map polygon, label position
static const struct {
    int map;
    const char *path;
    int lx, ly;
} k_territory[TURF_MAP_COUNT] = {
    {0, "m 156.9625,550.55785 1.18911,63.02283 183.12291,89.18323 130.80209,-64.21193 -4.75644,-32.10596 -28.53864,-9.51288 "
        "1.18911,-48.75351 -141.50407,-71.34659 z", 299, 564},
    {2, "m 720.60056,422.13399 156.9625,86.80502 -36.86241,23.7822 16.64754,33.29507 49.94262,5.94555 265.17149,-143.88229 "
        "-10.702,-30.91685 -52.3208,-14.26932 -29.7278,19.02575 -28.5386,-34.48418 -5.9456,-34.48419 -108.20895,-35.67329 "
        "-180.74469,58.26638 z", 906, 391},
    {3, "m 102.26345,203.33778 192.63579,-134.369404 155.77339,77.292134 65.40104,59.45549 45.18617,17.83665 v 39.24062 "
        "L 359.11117,365.05672 101.07434,222.36354 Z", 313, 183},
};

static struct {
    int tab;
    int board;          // 0 top performing clans, 1 top grossing, 2 top players
    cJSON *status, *clan, *clan_players, *board_data;
    int fresh[4], inflight[4];
} T;

enum { TD_STATUS, TD_CLAN, TD_PLAYERS, TD_BOARD };

static int num(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItem(o, k);
    return cJSON_IsNumber(v) ? v->valueint : 0;
}
static const char *str(const cJSON *o, const char *k) {
    const char *s = cJSON_GetStringValue(cJSON_GetObjectItem(o, k));
    return s ? s : "";
}

static cJSON **slot(const int which) {
    return which == TD_STATUS ? &T.status : which == TD_CLAN ? &T.clan : which == TD_PLAYERS ? &T.clan_players : &T.board_data;
}

static void expire(void *user) { T.fresh[(int) (intptr_t) user] = 0; }

static void on_data(void *user, const int status, const char *body, const size_t length) {
    (void) length;
    const int which = (int) (intptr_t) user;
    T.inflight[which] = 0;
    cJSON *res = status == 200 ? cJSON_Parse(body) : NULL;
    cJSON **s = slot(which);
    cJSON_Delete(*s);
    *s = res ? cJSON_DetachItemFromObject(res, "data") : NULL;
    if (!*s) *s = cJSON_CreateObject(); // a 404 clan / an error: shown as "no data"
    cJSON_Delete(res);
    T.fresh[which] = 1;
    html_timer_add(TURF_TTL_MS, expire, (void *) (intptr_t) which);
    menu_rerender_if_open(TURF_WINDOW);
}

static int g_turf_scenario;

static void fetch(const int which) {
    if (g_turf_scenario || T.inflight[which] || T.fresh[which] || backend_available() != 1) return;
    const AccountInfo *a = account_get();
    char path[256], clan[64] = "";
    if (a && a->clan[0]) backend_url_encode(clan, sizeof(clan), a->clan);
    switch (which) {
        case TD_STATUS: snprintf(path, sizeof(path), "/api/territorial-wars/status"); break;
        case TD_CLAN:
            if (!clan[0]) return;
            snprintf(path, sizeof(path), "/api/territorial-wars/clan/%s", clan);
            break;
        case TD_PLAYERS:
            if (!clan[0]) return;
            snprintf(path, sizeof(path), "/api/territorial-wars/clan/%s/players", clan);
            break;
        default:
            snprintf(path, sizeof(path), "/api/territorial-wars/leaderboards/%s",
                     T.board == 0 ? "top-performing-clans" : T.board == 1 ? "top-grossing-clans" : "top-performing-players");
            break;
    }
    T.inflight[which] = 1;
    if (backend_http_get(path, on_data, (void *) (intptr_t) which) != 0) T.inflight[which] = 0;
}

int turf_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    (void) ev;
    if (!name) return -1;
    const int arg0 = argc > 0 && args[0].kind == HTML_ARG_NUM ? (int) args[0].num : 0;
    if (!strcmp(name, "openTurfWars")) {
        for (int i = 0; i < 4; i++) T.fresh[i] = 0;
        menu_show_window(TURF_WINDOW, 1);
        return 1;
    }
    if (!strcmp(name, "turfTab")) {
        if (arg0 >= 0 && arg0 < TW_TABS) T.tab = arg0;
        menu_rerender_if_open(TURF_WINDOW);
        return 1;
    }
    if (!strcmp(name, "turfBoard")) {
        if (arg0 >= 0 && arg0 < 3 && arg0 != T.board) {
            T.board = arg0;
            T.fresh[TD_BOARD] = 0;
        }
        menu_rerender_if_open(TURF_WINDOW);
        return 1;
    }
    return -1;
}

// an SVG document as a data: URL (account_ui.c qr_data_url's encoding)
static char *turf_data_url(const char *svg) {
    static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    const size_t len = strlen(svg), cap = 32 + (len + 2) / 3 * 4;
    char *out = malloc(cap);
    if (!out) return NULL;
    size_t o = (size_t) snprintf(out, cap, "data:image/svg+xml;base64,");
    for (size_t i = 0; i < len; i += 3) {
        const unsigned v = (unsigned char) svg[i] << 16 | (i + 1 < len ? (unsigned char) svg[i + 1] << 8 : 0) | (i + 2 < len ? (unsigned char) svg[i + 2] : 0);
        out[o++] = b64[v >> 18 & 63];
        out[o++] = b64[v >> 12 & 63];
        out[o++] = i + 1 < len ? b64[v >> 6 & 63] : '=';
        out[o++] = i + 2 < len ? b64[v & 63] : '=';
    }
    out[o] = 0;
    return out;
}

static const cJSON *status_of(const int map) {
    const cJSON *m;
    cJSON_ArrayForEach(m, T.status) if (num(m, "map_id") == map) return m;
    return NULL;
}

static void gen_overview(StrBuf *sb) {
    fetch(TD_STATUS);
    if (!T.status) {
        sb_append(sb, "<div class='rkLoading'>Loading turf wars data...</div>");
        return;
    }
    // the territory map: an SVG image (the native engine rasterises SVG images with nanosvg; it has
    // no inline <svg>), labels as HTML over it at v10's label positions (viewBox 1250 x 760)
    StrBuf svg = {0};
    sb_append(&svg, "<svg xmlns='http://www.w3.org/2000/svg' width='1250' height='760' viewBox='0 0 1250 760'>"
                    "<rect width='1250' height='760' rx='16' fill='#1b2430'/>");
    for (int i = 0; i < TURF_MAP_COUNT; i++) {
        const cJSON *m = status_of(k_territory[i].map);
        const int owned = m && str(m, "clan_name")[0];
        sb_appendf(&svg, "<path d='%s' fill='%s' stroke='%s' stroke-width='4'/>", k_territory[i].path, owned ? "#2f6b2e" : "#3a4350",
                   owned ? "#5ce05a" : "#8a95a3");
    }
    sb_append(&svg, "</svg>");
    char *svg_text = sb_finish(&svg);
    char *url = turf_data_url(svg_text);
    free(svg_text);
    sb_append(sb, "<div class='twMapWrap'>");
    if (url) sb_appendf(sb, "<img class='twMap' alt='Territory Map' src='%s'>", url);
    free(url);
    for (int i = 0; i < TURF_MAP_COUNT; i++) {
        const cJSON *m = status_of(k_territory[i].map);
        const char *owner = m ? str(m, "clan_name") : "";
        sb_appendf(sb, "<div class='twLabel' style='left:%.2f%%;top:%.2f%%'>", k_territory[i].lx * 100.0 / 1250.0, k_territory[i].ly * 100.0 / 760.0);
        sb_append(sb, turf_map_name(k_territory[i].map));
        if (owner[0]) {
            sb_append(sb, "<div class='twLabelClan'>[");
            sb_append_esc(sb, owner);
            sb_append(sb, "]</div>");
        }
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div><div class='twCards'>");
    for (int i = 0; i < TURF_MAP_COUNT; i++) {
        const int map = g_turf_maps[i];
        const cJSON *m = status_of(map);
        sb_appendf(sb, "<div class='twCard'><div class='twCardHead'>%s</div><div class='twCtrl'>", turf_map_name(map));
        if (m && str(m, "clan_name")[0]) {
            sb_append(sb, "Territory controlled by <b>[");
            sb_append_esc(sb, str(m, "clan_name"));
            sb_append(sb, "]</b>");
        } else {
            sb_append(sb, "Uncontrolled");
        }
        sb_appendf(sb, "</div><div class='twReward'>%d kills today &middot; %d KR pool (%d guaranteed)</div><div class='twCtrl'>Top Clans Today</div>",
                   num(m, "total_kills_today"), num(m, "estimated_reward"), num(m, "guaranteed_reward"));
        const cJSON *lb = cJSON_GetObjectItem(m, "leaderboard"), *e;
        int n = 0;
        cJSON_ArrayForEach(e, lb) {
            if (n++ >= 5) break;
            sb_appendf(sb, "<div class='settName'>%d. [", num(e, "position"));
            sb_append_esc(sb, str(e, "clan_name"));
            sb_appendf(sb, "]<span class='rkVal'>%d</span></div>", num(e, "kills"));
        }
        if (!n) sb_append(sb, "<div class='twCtrl'>No kills yet today.</div>");
        sb_append(sb, "</div>");
    }
    sb_append(sb, "</div>");
}

static void gen_clan(StrBuf *sb) {
    const AccountInfo *a = account_get();
    if (!a || !a->clan[0]) {
        sb_append(sb, "<div class='rkEmpty'><h2>Join the Turf Wars!</h2>Create or join a clan to participate in Turf Wars and compete for control "
                      "of maps!<div style='margin-top:14px'><a class='menuLink' onclick='showWindow(13)'>Create Clan</a> &middot; "
                      "<a class='menuLink' onclick='showWindow(13)'>Join Clan</a></div></div>");
        return;
    }
    fetch(TD_CLAN);
    fetch(TD_PLAYERS);
    if (!T.clan) {
        sb_append(sb, "<div class='rkLoading'>Loading clan data...</div>");
        return;
    }
    const cJSON *c = cJSON_GetObjectItem(T.clan, "clan");
    if (!c || !num(c, "total_kills")) {
        sb_append(sb, "<div class='rkEmpty'><h2>No Participation Data</h2>Your clan hasn't participated in Turf Wars yet. Start playing on eligible "
                      "maps to see your clan's statistics!<div style='margin-top:10px'>Eligible Maps: Burg, Sandstorm, Subzero</div></div>");
        return;
    }
    sb_append(sb, "<div class='setBodH rkStats'>");
    sb_appendf(sb, "<div class='settName'>Kills Today<span class='rkVal'>%d</span></div><div class='settName'>Total Kills<span class='rkVal'>%d</span></div>"
                   "<div class='settName'>Total Deaths<span class='rkVal'>%d</span></div><div class='settName'>Territory Days Won<span class='rkVal'>%d</span></div>"
                   "<div class='settName'>Clan Earnings<span class='rkVal'>%d KR</span></div></div>",
               num(c, "kills_today"), num(c, "total_kills"), num(c, "total_deaths"), num(c, "days_won"), num(c, "total_kr"));
    sb_append(sb, "<div class='rkMapsHed'>Today's Performance</div><table class='rkTable'><tr><th>Player</th><th>Kills</th><th>Deaths</th><th>KD</th></tr>");
    const cJSON *p;
    cJSON_ArrayForEach(p, cJSON_GetObjectItem(T.clan_players, "players")) {
        const int k = num(p, "kills"), d = num(p, "deaths");
        sb_append(sb, "<tr><td>");
        sb_append_esc(sb, str(p, "player_name"));
        sb_appendf(sb, "</td><td>%d</td><td>%d</td><td>%.2f</td></tr>", k, d, (double) k / (d > 0 ? d : 1));
    }
    sb_append(sb, "</table>");
}

static void gen_boards(StrBuf *sb) {
    static const char *const names[3] = {"Top Performing Clans", "Top Grossing Clans", "Top Performing Players"};
    sb_append(sb, "<div class='rkControls'>");
    for (int i = 0; i < 3; i++)
        sb_appendf(sb, "<span class='rkRegionTab%s' onclick='playSelect(0.1);turfBoard(%d)'>%s</span>", T.board == i ? " active" : "", i, names[i]);
    sb_append(sb, "</div>");
    fetch(TD_BOARD);
    const cJSON *rows = cJSON_GetObjectItem(T.board_data, "leaderboard");
    if (!T.board_data) {
        sb_append(sb, "<div class='rkLoading'>Loading leaderboard...</div>");
        return;
    }
    if (!cJSON_GetArraySize(rows)) {
        sb_append(sb, "<div class='rkEmpty'>No data available for the selected filters.</div>");
        return;
    }
    sb_appendf(sb, "<table class='rkTable'><tr><th>#</th><th>%s</th><th>%s</th></tr>", T.board == 2 ? "Player" : "Clan", T.board == 1 ? "KR Earned" : "Kills");
    const cJSON *r;
    int pos = 0;
    cJSON_ArrayForEach(r, rows) {
        sb_appendf(sb, "<tr><td>%d</td><td>", ++pos);
        sb_append_esc(sb, T.board == 2 ? str(r, "player_name") : str(r, "clan_name"));
        sb_appendf(sb, "</td><td>%d</td></tr>", T.board == 1 ? num(r, "total_kr") : num(r, "kills"));
    }
    sb_append(sb, "</table>");
}

static void gen_howto(StrBuf *sb) {
    sb_append(sb, "<div class='setBodH'><div class='setHed'>How to Play Turf Wars</div>Compete with your clan to control territories and earn rewards!"
                  "<div class='setHed'>Earn Kills</div>Play on eligible maps in Free for All or Team Deathmatch modes on official servers. Every kill "
                  "you earn counts towards your clan's total! Kills only count if you're playing against players from different clans!"
                  "<div class='setHed'>Control Territories</div>At the end of each day, the clan with the most kills on each map wins control of "
                  "that territory for the next day. Leaderboards reset every day at midnight UTC."
                  "<div class='setHed'>Earn Rewards</div>At the end of each day, all kills made on a map are converted into a KR pool. The top 3 "
                  "clans on that map split the pool (60% / 30% / 10%) - you don't have to win the territory to get paid! Each clan shares its "
                  "cut among members (set in Loot settings).<div class='setHed'>Important Rules</div><ul class='twRules'>"
                  "<li>Only the top 20 contributors (by kills) per map per clan count towards wars</li>"
                  "<li>Only kills on eligible maps (Burg, Sandstorm, Subzero) count towards turf wars</li>"
                  "<li>Only Free for All and Team Deathmatch game modes are eligible</li>"
                  "<li>Kills against players from your own clan do not count</li>"
                  "<li>You must be playing on official servers for kills to count (clan wars you host do not count)</li></ul></div>");
}

char *turf_window_gen(void) {
    static int css;
    if (!css) css = html_ui_add_stylesheet("css/ranked-kn.css") || 1;
    StrBuf sb = {0};
    if (backend_available() != 1 && !g_turf_scenario) {
        sb_append(&sb, "<div class='rkEmpty'>Turf Wars needs the Krunker Box Office servers.</div>");
        return sb_finish(&sb);
    }
    sb_append(&sb, "<div class='rkWin'><div class='bigMenTabs rkTabs'>");
    for (int i = 0; i < TW_TABS; i++)
        sb_appendf(&sb, "<div class='bigMenTab%s' style='width:25%%' onclick='playSelect(0.1);turfTab(%d)' onmouseenter='playTick()'>%s</div>",
                   T.tab == i ? " bigMenTabA" : "", i, k_tw_tabs[i]);
    sb_append(&sb, "</div>");
    switch (T.tab) {
        case TW_CLAN: gen_clan(&sb); break;
        case TW_BOARDS: gen_boards(&sb); break;
        case TW_HOWTO: gen_howto(&sb); break;
        default: gen_overview(&sb); break;
    }
    sb_append(&sb, "</div>");
    return sb_finish(&sb);
}

// --render-scenario: state.turf {tab, status, clan, players, board}
void turf_ui_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state)) return;
    g_turf_scenario = 1;
    const char *keys[4] = {"status", "clan", "players", "board"};
    for (int i = 0; i < 4; i++) {
        cJSON **s = slot(i);
        cJSON_Delete(*s);
        const cJSON *v = cJSON_GetObjectItem(state, keys[i]);
        *s = v ? cJSON_Duplicate(v, 1) : NULL;
        T.fresh[i] = 1;
    }
    const cJSON *tab = cJSON_GetObjectItem(state, "tab");
    T.tab = cJSON_IsNumber(tab) ? tab->valueint : 0;
}
