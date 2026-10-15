// Krunker v10.0.2 Turf Wars (doc/v10/ranked-impl.md): v10's _51011 popup (#menuBtnTurfWars,
// genericPopup({component: _51011}, 'defaultModal'), v10:141788-142713) on the v10 page, over the
// brain's /api/territorial-wars/* and /api/clans/* routes (services/brain/turf.c).
//
// Tabs (svelte-1o3qul6) Overview (svelte-102279m: the territory map, v10's ./img/territory_map.webp
// with the hovered / locked territory's ./img/turf/<map>-raised.png, the controllers' labels and
// the details panel with today's top clans and the reward pool), My Clan (svelte-51j5cz: today's
// performance, top contributors by day and map, the previous 3 days, earnings, individual payouts
// svelte-myp8tq), Leaderboards (svelte-raovk7: top performing clans / top grossing clans / top
// performing players with v10's filters), Recruitment (svelte-72bcy3: the clans' recruitment
// posts by region and language, Apply), How to Play (svelte-1sr7h97); a clan or player name opens
// the clan view (svelte-c92dbh) or the player view (svelte-7h12t9) in place of the tabs.
//
// v10 draws the territories as an inline <svg> of transparent paths (hit areas only: the visible
// map is the webp). The native engine has no inline SVG, so the same polygons become hit cells
// (a grid of absolutely positioned boxes, one run per row) with v10's hover / click behaviour.
#include <v10_ui.h>
#include <backend.h>
#include <backend_ui.h>
#include <html_ui.h>
#include <turf_wars.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define POPUP_CLASS "defaultModal"
#define TTL_MS 30000
#define VIEW_W 1264.0 // the SVG viewBox
#define VIEW_H 729.0

enum { TW_OVERVIEW, TW_CLAN, TW_BOARDS, TW_RECRUIT, TW_HOWTO, TW_TABS };
static const char *const k_tabs[TW_TABS] = {"Overview", "My Clan", "Leaderboards", "Recruitment", "How to Play"};
enum { VIEW_TABS, VIEW_CLAN, VIEW_PLAYER };

// v10's territories (_51038): path, label position
static const struct {
    int map;
    const char *id, *path;
    int lx, ly;
} k_territory[TURF_MAP_COUNT] = {
    {0, "burg", "m 156.9625,550.55785 1.18911,63.02283 183.12291,89.18323 130.80209,-64.21193 -4.75644,-32.10596 -28.53864,-9.51288 "
                "1.18911,-48.75351 -141.50407,-71.34659 z", 299, 564},
    {2, "sandstorm", "m 720.60056,422.13399 156.9625,86.80502 -36.86241,23.7822 16.64754,33.29507 49.94262,5.94555 265.17149,-143.88229 "
                     "-10.702,-30.91685 -52.3208,-14.26932 -29.7278,19.02575 -28.5386,-34.48418 -5.9456,-34.48419 -108.20895,-35.67329 "
                     "-180.74469,58.26638 z", 906, 391},
    {3, "subzero", "m 102.26345,203.33778 192.63579,-134.369404 155.77339,77.292134 65.40104,59.45549 45.18617,17.83665 v 39.24062 "
                   "L 359.11117,365.05672 101.07434,222.36354 Z", 313, 183},
};

static const char *const k_regions[][2] = {{"NA", "North America"}, {"EU", "Europe"}, {"AS", "Asia"}, {"OCE", "Oceania"}, {"SA", "South America"}, {"ME", "Middle East"}};
static const char *const k_langs[][2] = {{"en", "English"},   {"es", "Spanish"},    {"pt", "Portuguese"}, {"fr", "French"},   {"de", "German"},  {"ru", "Russian"},
                                         {"zh", "Chinese"},   {"ja", "Japanese"},   {"ko", "Korean"},     {"ar", "Arabic"},   {"tr", "Turkish"}, {"pl", "Polish"},
                                         {"it", "Italian"},   {"nl", "Dutch"},      {"sv", "Swedish"},    {"th", "Thai"},     {"vi", "Vietnamese"}, {"id", "Indonesian"},
                                         {"hi", "Hindi"},     {"fil", "Filipino"},  {"cs", "Czech"},      {"ro", "Romanian"}, {"hu", "Hungarian"}, {"uk", "Ukrainian"},
                                         {"fi", "Finnish"},   {"no", "Norwegian"},  {"da", "Danish"},     {"el", "Greek"},    {"he", "Hebrew"},  {"ms", "Malay"}};
static const char *const k_loot_modes[][2] = {{"clan_bank", "Clan Bank"}, {"even", "Even Split"}, {"top_weighted", "Top Weighted"}, {"performance", "Performance"},
                                              {"top_performance", "Top N by Performance"}};

typedef struct {
    int day;          // -1 = the current day
    int map;          // -1 = all maps
    int view_all;     // My Clan: View All Players
    int players_page; // clan view: 7 per page
    int earn_page;    // 20 (My Clan) / 7 (clan view) per page
    int pay_daily;    // payouts: Daily View
    int pay_day;      // payouts day (-1 = the current day)
} ClanFilters;

static struct {
    int tab, view;
    char clan[64], player[64]; // the clan / player view's id (clan id or tag, account id or name)
    int hover, locked, hover_timer; // overview territory index (-1 none)
    ClanFilters mine, other;
    int board;                 // -1 = the three cards, 0 clans, 1 grossing, 2 players
    char f_date[16];           // applied filters (Leaderboards)
    int f_map;
    char f_name[48];
    char rc_region[8], rc_lang[8];
    int rc_page, rc_applying;
    char rc_error[160];
    int pv_tab;                // player view: 0 contributions, 1 earnings
    int pv_c_day, pv_e_day;    // -1 = all
    int pv_c_page, pv_e_page;
    struct { char id[24]; int col, desc; } sorts[16];
    int scenario;
} T = {.hover = -1, .locked = -1, .board = -1, .f_map = -1, .rc_page = 1, .pv_c_day = -1, .pv_e_day = -1};

// ------------------------------------------------------------------ days (TERRITORY_WARS_INTRO_DATE 2026-03-17)

static int day_now(void) { return turf_day_at((int64_t) time(NULL)); }

static void day_date(char *out, const size_t n, const int day) {
    const time_t t = (time_t) (TURF_INTRO_UNIX + (int64_t) day * 86400);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    strftime(out, n, "%Y-%m-%d", &tm);
}

static int date_day(const char *s) { // "YYYY-MM-DD" -> day, -1 invalid
    int y, m, d;
    if (!s || sscanf(s, "%d-%d-%d", &y, &m, &d) != 3 || m < 1 || m > 12 || d < 1 || d > 31) return -1;
    // days from civil (Howard Hinnant), UTC
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned) (y - era * 400);
    const unsigned doy = (unsigned) ((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1);
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    const long long days = (long long) era * 146097 + (long long) doe - 719468;
    const long long day = (days * 86400 - TURF_INTRO_UNIX) / 86400;
    return day < 0 ? -1 : (int) day;
}

static const char *map_name(const int map) {
    const char *n = turf_map_name(map);
    return n ? n : "";
}

// _34422: staff clan tags are gold, the rest the given colour
static const char *clan_color(const char *tag, const char *def) {
    static const char *const gold[] = {"arae", "lain", "scte", "fame", "kpd", "jump", "art", "dev", "mmok", "nl"};
    for (size_t i = 0; tag && i < sizeof(gold) / sizeof(gold[0]); i++) {
        if (strlen(tag) == strlen(gold[i])) {
            int same = 1;
            for (size_t k = 0; same && tag[k]; k++) same = (tag[k] | 32) == gold[i][k];
            if (same) return "#FBC02D";
        }
    }
    return def;
}

static void esc_attr(StrBuf *sb, const char *s) { sb_append_esc(sb, s ? s : ""); }

static void loading(StrBuf *sb, const char *hash, const char *text) { sb_appendf(sb, "<div class=\"loading svelte-%s\">%s</div>", hash, text); }

static void error_box(StrBuf *sb, const char *hash, const V10Req *r, const char *fallback) {
    const char *msg = cJSON_GetStringValue(cJSON_GetObjectItem(r->json, "message"));
    if (!msg) msg = cJSON_GetStringValue(cJSON_GetObjectItem(r->json, "error"));
    sb_appendf(sb, "<div class=\"error svelte-%s\">", hash);
    sb_append_esc(sb, msg && msg[0] ? msg : fallback);
    sb_append(sb, "</div>");
}

static const cJSON *data_of(const V10Req *r) { return r->status == 200 ? cJSON_GetObjectItem(r->json, "data") : NULL; }

// ------------------------------------------------------------------ sortable tables

static int *sort_slot(const char *id, int **desc) {
    for (int i = 0; i < 16; i++) {
        if (!strcmp(T.sorts[i].id, id)) {
            *desc = &T.sorts[i].desc;
            return &T.sorts[i].col;
        }
    }
    for (int i = 0; i < 16; i++) {
        if (!T.sorts[i].id[0]) {
            snprintf(T.sorts[i].id, sizeof(T.sorts[i].id), "%s", id);
            T.sorts[i].col = -1;
            *desc = &T.sorts[i].desc;
            return &T.sorts[i].col;
        }
    }
    static int none = -1, nd;
    *desc = &nd;
    return &none;
}

static const char *g_key;
static int g_desc;
static int cmp_rows(const void *a, const void *b) {
    const cJSON *x = cJSON_GetObjectItem(*(const cJSON *const *) a, g_key), *y = cJSON_GetObjectItem(*(const cJSON *const *) b, g_key);
    if (!x || cJSON_IsNull(x)) return y && !cJSON_IsNull(y) ? 1 : 0;
    if (!y || cJSON_IsNull(y)) return -1;
    int c;
    if (cJSON_IsString(x) && cJSON_IsString(y)) c = strcmp(x->valuestring, y->valuestring);
    else c = x->valuedouble < y->valuedouble ? -1 : x->valuedouble > y->valuedouble ? 1 : 0;
    return g_desc ? -c : c;
}

// the rows of `arr` in the table's sort order (malloc'd; *n rows)
static const cJSON **sorted_rows(const char *table, const cJSON *arr, const char *const *keys, int *n, int *col, int *desc) {
    int *d;
    int *c = sort_slot(table, &d);
    *col = *c;
    *desc = *d;
    *n = cJSON_GetArraySize(arr);
    const cJSON **rows = calloc((size_t) (*n > 0 ? *n : 1), sizeof(*rows));
    if (!rows) {
        *n = 0;
        return NULL;
    }
    int k = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, arr) rows[k++] = r;
    if (*c >= 0 && keys[*c]) {
        g_key = keys[*c];
        g_desc = *d;
        qsort(rows, (size_t) k, sizeof(*rows), cmp_rows);
    }
    return rows;
}

static void sort_click(const char *table, const int col) { // _46124: a new column sorts desc, the same one toggles
    int *d;
    int *c = sort_slot(table, &d);
    if (*c == col) *d = !*d;
    else {
        *c = col;
        *d = 1;
    }
}

// ------------------------------------------------------------------ requests

static const V10Req *get_status(void) { return v10_get("/api/territorial-wars/status", TTL_MS); }

static void enc(char *out, const size_t n, const char *s) { backend_url_encode(out, n, s ? s : ""); }

static const V10Req *get_clan(const char *clan) {
    char id[128], path[256];
    enc(id, sizeof(id), clan);
    snprintf(path, sizeof(path), "/api/territorial-wars/clan/%s", id);
    return v10_get(path, TTL_MS);
}

static const V10Req *get_clan_players(const char *clan, const ClanFilters *f) {
    char id[128], path[300];
    enc(id, sizeof(id), clan);
    int o = snprintf(path, sizeof(path), "/api/territorial-wars/clan/%s/players", id);
    char sep = '?';
    if (f->day >= 0) o += snprintf(path + o, sizeof(path) - (size_t) o, "%cday=%d", sep, f->day), sep = '&';
    if (f->map >= 0) snprintf(path + o, sizeof(path) - (size_t) o, "%cmap_id=%d", sep, f->map);
    return v10_get(path, TTL_MS);
}

static const V10Req *get_clan_earnings(const char *clan) {
    char id[128], path[256];
    enc(id, sizeof(id), clan);
    snprintf(path, sizeof(path), "/api/territorial-wars/clan/%s/earnings", id);
    return v10_get(path, TTL_MS);
}

static const V10Req *get_clan_payouts(const char *clan, const int day) {
    char id[128], path[256];
    enc(id, sizeof(id), clan);
    if (day >= 0) snprintf(path, sizeof(path), "/api/territorial-wars/clan/%s/payouts?day=%d", id, day);
    else snprintf(path, sizeof(path), "/api/territorial-wars/clan/%s/payouts", id);
    return v10_get(path, TTL_MS);
}

static void render(void);

// ------------------------------------------------------------------ Overview (svelte-102279m)

typedef struct { double x, y; } Pt;
#define POLY_MAX 24
static Pt g_poly[TURF_MAP_COUNT][POLY_MAX];
static int g_poly_n[TURF_MAP_COUNT];

// SVG path data (m/M l/L v/V h/H z) to a polygon
static void parse_path(const char *d, Pt *out, int *n) {
    double x = 0, y = 0;
    char cmd = 'M';
    *n = 0;
    const char *p = d;
    while (*p && *n < POLY_MAX) {
        while (*p == ' ' || *p == ',') p++;
        if (!*p) break;
        if ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z')) {
            cmd = *p++;
            if (cmd == 'z' || cmd == 'Z') continue;
            continue;
        }
        char *end;
        const double a = strtod(p, &end);
        if (end == p) break;
        p = end;
        if (cmd == 'v' || cmd == 'V') {
            y = cmd == 'v' ? y + a : a;
        } else if (cmd == 'h' || cmd == 'H') {
            x = cmd == 'h' ? x + a : a;
        } else {
            while (*p == ' ' || *p == ',') p++;
            const double b = strtod(p, &end);
            p = end;
            const int rel = cmd >= 'a';
            x = rel && *n ? x + a : rel ? a : a;
            y = rel && *n ? y + b : rel ? b : b;
            if (cmd == 'm') cmd = 'l'; // further pairs after m are relative lines
            if (cmd == 'M') cmd = 'L';
        }
        out[(*n)++] = (Pt) {x, y};
    }
}

static int in_poly(const Pt *poly, const int n, const double x, const double y) {
    int c = 0;
    for (int i = 0, j = n - 1; i < n; j = i++)
        if ((poly[i].y > y) != (poly[j].y > y) && x < (poly[j].x - poly[i].x) * (y - poly[i].y) / (poly[j].y - poly[i].y) + poly[i].x) c = !c;
    return c;
}

// the hit cells of territory i: runs of 32 x 32 viewBox units per row, % of the wrapper
static void hit_cells(StrBuf *sb, const int i) {
    if (!g_poly_n[i]) parse_path(k_territory[i].path, g_poly[i], &g_poly_n[i]);
    const double cell = 16.0;
    for (double y = 0; y < VIEW_H; y += cell) {
        double run = -1;
        for (double x = 0; x <= VIEW_W; x += cell) {
            const int inside = x < VIEW_W && in_poly(g_poly[i], g_poly_n[i], x + cell / 2, y + cell / 2);
            if (inside && run < 0) run = x;
            if (!inside && run >= 0) {
                sb_appendf(sb, "<div class=\"kn-tw-hit\" style=\"left:%.3f%%;top:%.3f%%;width:%.3f%%;height:%.3f%%\" onmouseenter=\"knTwHover(%d)\" "
                               "onmouseleave=\"knTwHover(-1)\" onclick=\"knTwLock(%d)\"></div>",
                           run * 100.0 / VIEW_W, y * 100.0 / VIEW_H, (x - run) * 100.0 / VIEW_W, cell * 100.0 / VIEW_H, i, i);
                run = -1;
            }
        }
    }
}

static const cJSON *status_of(const cJSON *status, const int map) {
    const cJSON *m;
    cJSON_ArrayForEach(m, status) if ((int) v10_num(m, "map_id", -1) == map) return m;
    return NULL;
}

static void gen_overview(StrBuf *sb) {
    const V10Req *r = get_status();
    const cJSON *status = data_of(r);
    if (r->status == 0 && !status) {
        sb_append(sb, "<div style=\"padding: 40px; text-align: center; color: rgba(255, 255, 255, 0.7);\" class=\"svelte-102279m\">Loading turf wars data...</div>");
        return;
    }
    if (!cJSON_IsArray(status)) {
        const char *msg = v10_str(r->json, "message");
        sb_append(sb, "<div style=\"padding: 40px; text-align: center; color: #E85656;\" class=\"svelte-102279m\">Error: ");
        sb_append_esc(sb, msg[0] ? msg : "Failed to load turf wars data");
        sb_append(sb, "</div>");
        return;
    }
    const int sel = T.hover >= 0 ? T.hover : T.locked; // selectedTerritory = hovered || locked
    sb_append(sb, "<div class=\"territory-map-container svelte-102279m\"><div class=\"territory-map-wrapper svelte-102279m\">"
                  "<img alt=\"Territory Map\" class=\"territory-map svelte-102279m\" src=\"./img/territory_map.webp\"> ");
    if (sel >= 0)
        sb_appendf(sb, "<img alt=\"Raised Territory\" class=\"territory-overlay svelte-102279m\" src=\"./img/turf/%d-raised.png\" style=\"left: %g%%; top: %g%%;\">",
                   k_territory[sel].map, k_territory[sel].lx / VIEW_W * 100.0, k_territory[sel].ly / VIEW_H * 100.0);
    else sb_append(sb, "<!---->");
    // the <svg> hit layer: clicking the map's background unlocks the territory
    sb_append(sb, " <div class=\"territory-svg-overlay svelte-102279m\" role=\"button\" tabindex=\"-1\" aria-label=\"Map background - click to unlock territory\" "
                  "onclick=\"knTwUnlock()\">");
    for (int i = 0; i < TURF_MAP_COUNT; i++) hit_cells(sb, i);
    sb_append(sb, "</div> ");
    for (int i = 0; i < TURF_MAP_COUNT; i++) {
        const cJSON *m = status_of(status, k_territory[i].map);
        const char *clan = m ? v10_str(m, "clan_name") : "";
        if (!m || !clan[0] || !cJSON_IsNumber(cJSON_GetObjectItem(m, "clan_id"))) {
            sb_append(sb, "<!---->");
            continue;
        }
        sb_appendf(sb, "<div class=\"territory-label-container svelte-102279m%s\" style=\"left: %g%%; top: %g%%;\"><div class=\"territory-label-wrapper svelte-102279m\">"
                       "<div class=\"territory-clan svelte-102279m\"><!----> <span class=\"clan-name clickable-clan-name svelte-102279m\" style=\"color: %s;\" "
                       "onclick=\"knTwClan('%.0f')\">",
                   sel == i ? " selected" : "", k_territory[i].lx / VIEW_W * 100.0, k_territory[i].ly / VIEW_H * 100.0, clan_color(clan, "#ffffff"),
                   v10_num(m, "clan_id", 0));
        sb_append_esc(sb, clan);
        sb_append(sb, "</span></div></div></div>");
    }
    sb_append(sb, "</div> ");
    if (sel >= 0) {
        const cJSON *m = status_of(status, k_territory[sel].map);
        const char *clan = m ? v10_str(m, "clan_name") : "";
        const double guaranteed = v10_num(m, "guaranteed_reward", 0);
        sb_appendf(sb, "<div class=\"territory-details-panel svelte-102279m\" role=\"region\" aria-label=\"Territory details\" onmouseenter=\"knTwPanel(1)\" "
                       "onmouseleave=\"knTwPanel(0)\"><div class=\"panel-header svelte-102279m\"><h3 class=\"territory-title svelte-102279m\">");
        sb_append_esc(sb, m ? v10_str(m, "map_name") : map_name(k_territory[sel].map));
        sb_append(sb, "</h3> <button class=\"close-button svelte-102279m\" aria-label=\"Close details\" onclick=\"knTwClose()\"><i class=\"material-icons svelte-102279m\">close</i></button></div> "
                      "<div class=\"panel-content svelte-102279m\"><div class=\"detail-section svelte-102279m\"><div class=\"detail-label svelte-102279m\">Controlled by</div> "
                      "<div class=\"detail-value svelte-102279m\">");
        const int has_id = m && cJSON_IsNumber(cJSON_GetObjectItem(m, "clan_id"));
        sb_appendf(sb, "<span class=\"clickable-clan-name svelte-102279m\" style=\"color: %s;\"", clan_color(clan, "#ffffff"));
        if (has_id) sb_appendf(sb, " onclick=\"knTwClan('%.0f')\"", v10_num(m, "clan_id", 0));
        sb_append(sb, ">");
        sb_append_esc(sb, clan[0] ? clan : "Eterno");
        sb_append(sb, "</span></div></div> <div class=\"detail-section svelte-102279m\"><div class=\"detail-label svelte-102279m\">Top Clans Today</div> "
                      "<div class=\"leaderboard-list svelte-102279m\">");
        const cJSON *e;
        int k = 0;
        cJSON_ArrayForEach(e, cJSON_GetObjectItem(m, "leaderboard")) {
            if (k++ >= 5) break;
            const int pos = (int) v10_num(e, "position", k);
            const char *tag = v10_str(e, "clan_name");
            char kills[32];
            v10_fmt_int(kills, sizeof(kills), v10_num(e, "kills", 0));
            sb_appendf(sb, "<div class=\"leaderboard-entry svelte-102279m%s\"><span class=\"leaderboard-position svelte-102279m\">%d.</span> "
                           "<div class=\"leaderboard-clan-info svelte-102279m\"><!----> <span class=\"leaderboard-clan clickable-clan-name svelte-102279m\" style=\"color: %s;\"",
                       pos <= TURF_SPLIT_COUNT ? " paid" : "", pos, clan_color(tag, "#ffffff"));
            if (cJSON_IsNumber(cJSON_GetObjectItem(e, "clan_id"))) sb_appendf(sb, " onclick=\"knTwClan('%.0f')\"", v10_num(e, "clan_id", 0));
            sb_append(sb, ">");
            sb_append_esc(sb, tag[0] ? tag : "Unknown");
            sb_appendf(sb, "</span></div> <div class=\"leaderboard-right svelte-102279m\"><span class=\"leaderboard-kills svelte-102279m\">%s kills</span> ", kills);
            const double cut = pos >= 1 && pos <= TURF_SPLIT_COUNT ? floor(guaranteed * g_turf_split[pos - 1]) : 0;
            if (cut > 0) {
                char kr[32];
                v10_fmt_int(kr, sizeof(kr), cut);
                sb_appendf(sb, "<span class=\"leaderboard-reward svelte-102279m\">~%s KR</span>", kr);
            } else sb_append(sb, "<!---->");
            sb_append(sb, "</div></div>");
        }
        sb_append(sb, "</div></div> ");
        if (v10_num(m, "total_kills_today", 0) > 0) {
            char g[32], est[32];
            v10_fmt_int(g, sizeof(g), guaranteed);
            v10_fmt_int(est, sizeof(est), v10_num(m, "estimated_reward", 0));
            sb_appendf(sb, "<div class=\"detail-section svelte-102279m\"><div class=\"detail-label svelte-102279m\">Reward Pool</div> "
                           "<div class=\"reward-pool-note svelte-102279m\">Split across the top %d clans (%d%% / %d%% / %d%%)</div> <div class=\"reward-estimates svelte-102279m\">"
                           "<div class=\"reward-row svelte-102279m\"><span class=\"reward-label svelte-102279m\">Guaranteed so far</span> <span class=\"reward-value guaranteed svelte-102279m\">%s KR</span></div> "
                           "<div class=\"reward-row svelte-102279m\"><span class=\"reward-label svelte-102279m\">Estimated end of day</span> <span class=\"reward-value estimated svelte-102279m\">%s KR</span></div></div></div>",
                       TURF_SPLIT_COUNT, (int) lround(g_turf_split[0] * 100), (int) lround(g_turf_split[1] * 100), (int) lround(g_turf_split[2] * 100), g, est);
        } else sb_append(sb, "<!---->");
        sb_append(sb, "</div></div>");
    } else sb_append(sb, "<!---->");
    sb_append(sb, "</div>");
}

// ------------------------------------------------------------------ the clan stats (My Clan _50779 / clan view _50929)

static void day_selector(StrBuf *sb, const char *hash, const char *arrows_handler, const char *input_handler, const int day, const int max_day, const int nullable) {
    char value[16] = "", maxd[16];
    if (day >= 0 || !nullable) day_date(value, sizeof(value), day >= 0 ? day : max_day);
    day_date(maxd, sizeof(maxd), max_day);
    const int prev_off = day >= 0 && day <= 0, next_off = day < 0 || day >= max_day;
    sb_appendf(sb, "<div class=\"day-selector svelte-%s\"><button class=\"day-arrow svelte-%s\"%s onclick=\"%s(-1)\">\xe2\x97\x80</button> "
                   "<input type=\"date\" class=\"day-input svelte-%s\" value=\"%s\" min=\"2026-03-17\" max=\"%s\" onchange=\"%s(this.value)\"> "
                   "<button class=\"day-arrow svelte-%s\"%s onclick=\"%s(1)\">\xe2\x96\xb6</button>",
               hash, hash, prev_off ? " disabled" : "", arrows_handler, hash, value, maxd, input_handler, hash, next_off ? " disabled" : "", arrows_handler);
}

static void map_select(StrBuf *sb, const char *hash, const char *handler, const int map) {
    sb_appendf(sb, "<select class=\"map-filter-select svelte-%s\" onchange=\"%s(this.value)\"><option value=\"\"%s>All Maps</option>", hash, handler, map < 0 ? " selected" : "");
    for (int i = 0; i < TURF_MAP_COUNT; i++)
        sb_appendf(sb, "<option value=\"%d\"%s>%s</option>", g_turf_maps[i], g_turf_maps[i] == map ? " selected" : "", map_name(g_turf_maps[i]));
    sb_append(sb, "</select>");
}

// _50712: "+N vs TAG"
static void comparison_cell(StrBuf *sb, const cJSON *row) {
    const cJSON *ck = cJSON_GetObjectItem(row, "comparison_kills");
    const char *cname = v10_str(row, "comparison_clan_name");
    if (!cJSON_IsNumber(ck) || !cname[0]) {
        sb_append(sb, "\xe2\x80\x94");
        return;
    }
    const double diff = v10_num(row, "kills", 0) - ck->valuedouble;
    char d[32];
    v10_fmt_int(d, sizeof(d), diff);
    sb_appendf(sb, "<div class=\"comparison-cell svelte-phv9vf\"><span class=\"diff-text svelte-phv9vf\">%s%s vs</span> ", diff >= 0 ? "+" : "", d);
    if (cJSON_IsNumber(cJSON_GetObjectItem(row, "comparison_clan_id")))
        sb_appendf(sb, "<span class=\"clan-name clickable svelte-phv9vf\" style=\"color: %s;\" onclick=\"knTwClan('%.0f')\">", clan_color(cname, "#ffffff"),
                   v10_num(row, "comparison_clan_id", 0));
    else sb_appendf(sb, "<span class=\"clan-name\" style=\"color: %s;\">", clan_color(cname, "#ffffff"));
    sb_append_esc(sb, cname);
    sb_append(sb, "</span></div>");
}

static void maps_table(StrBuf *sb, const char *table, const cJSON *maps, const int medals) {
    static const char *const keys[] = {"map_name", "position", "kills", "comparison_diff", "deaths", "games"};
    static const V10Col cols[] = {{"Map", NULL, 0, 1}, {"Position", NULL, 1, 1}, {"Kills", NULL, 1, 1}, {"Comparison", NULL, 0, 1}, {"Deaths", NULL, 1, 1}, {"Games", NULL, 1, 1}};
    // comparison_diff = kills - comparison_kills (null without a comparison)
    cJSON *copy = cJSON_Duplicate(maps, 1);
    cJSON *m;
    cJSON_ArrayForEach(m, copy) {
        const cJSON *ck = cJSON_GetObjectItem(m, "comparison_kills");
        if (cJSON_IsNumber(ck)) cJSON_AddNumberToObject(m, "comparison_diff", v10_num(m, "kills", 0) - ck->valuedouble);
        else cJSON_AddNullToObject(m, "comparison_diff");
    }
    int n, col, desc;
    const cJSON **rows = sorted_rows(table, copy, keys, &n, &col, &desc);
    char fmt[64];
    snprintf(fmt, sizeof(fmt), "knTwSort('%s',%%d)", table);
    v10_table_begin(sb, cols, 6, col, desc, fmt);
    for (int i = 0; i < n; i++) {
        const cJSON *r = rows[i];
        char pos[48], kills[32], deaths[32], games[32];
        const cJSON *p = cJSON_GetObjectItem(r, "position");
        static const char *const medal[] = {"\xf0\x9f\xa5\x87", "\xf0\x9f\xa5\x88", "\xf0\x9f\xa5\x89"};
        const int medal_row = medals && cJSON_IsNumber(p) && p->valueint >= 1 && p->valueint <= TURF_SPLIT_COUNT;
        if (!cJSON_IsNumber(p)) snprintf(pos, sizeof(pos), "\xe2\x80\x94");
        else snprintf(pos, sizeof(pos), "#%d", p->valueint);
        v10_fmt_int(kills, sizeof(kills), v10_num(r, "kills", 0));
        if (cJSON_IsNumber(cJSON_GetObjectItem(r, "deaths"))) v10_fmt_int(deaths, sizeof(deaths), v10_num(r, "deaths", 0));
        else snprintf(deaths, sizeof(deaths), "N/A");
        if (cJSON_IsNumber(cJSON_GetObjectItem(r, "games"))) v10_fmt_int(games, sizeof(games), v10_num(r, "games", 0));
        else snprintf(games, sizeof(games), "N/A");
        v10_table_row(sb, NULL, 0);
        v10_table_cell(sb, &cols[0], v10_str(r, "map_name"));
        if (medal_row) { // "#1" and the medal image (v10 shows the medal emoji)
            StrBuf mb = {0};
            sb_appendf(&mb, "#%d ", p->valueint);
            v10_emoji(&mb, medal[p->valueint - 1], "");
            char *mh = sb_finish(&mb);
            v10_table_cell_html(sb, &cols[1], mh);
            free(mh);
        } else v10_table_cell(sb, &cols[1], pos);
        v10_table_cell(sb, &cols[2], kills);
        StrBuf c = {0};
        comparison_cell(&c, r);
        char *html = sb_finish(&c);
        v10_table_cell_html(sb, &cols[3], html);
        free(html);
        v10_table_cell(sb, &cols[4], deaths);
        v10_table_cell(sb, &cols[5], games);
        v10_table_row_end(sb);
    }
    v10_table_end(sb);
    free(rows);
    cJSON_Delete(copy);
}

static void players_table(StrBuf *sb, const char *table, const cJSON *players, const int from, const int count) {
    static const char *const keys[] = {"player_name", "kills", "deaths", "kd"};
    static const V10Col cols[] = {{"Player", NULL, 0, 1}, {"Kills", NULL, 1, 1}, {"Deaths", NULL, 1, 1}, {"K/D", NULL, 1, 1}};
    int n, col, desc;
    const cJSON **rows = sorted_rows(table, players, keys, &n, &col, &desc);
    char fmt[64];
    snprintf(fmt, sizeof(fmt), "knTwSort('%s',%%d)", table);
    v10_table_begin(sb, cols, 4, col, desc, fmt);
    for (int i = from; i < n && i < from + count; i++) {
        const cJSON *r = rows[i];
        char name[64], kills[32], deaths[32], kd[32], click[96];
        const char *pn = v10_str(r, "player_name");
        if (pn[0]) snprintf(name, sizeof(name), "%s", pn);
        else snprintf(name, sizeof(name), "Player %.0f", v10_num(r, "player_id", 0));
        v10_fmt_int(kills, sizeof(kills), v10_num(r, "kills", 0));
        v10_fmt_int(deaths, sizeof(deaths), v10_num(r, "deaths", 0));
        snprintf(kd, sizeof(kd), "%.2f", v10_num(r, "kd", 0));
        snprintf(click, sizeof(click), "playTick(0.5);knTwPlayer('%.0f')", v10_num(r, "player_id", 0));
        v10_table_row(sb, click, 0);
        v10_table_cell(sb, &cols[0], name);
        v10_table_cell(sb, &cols[1], kills);
        v10_table_cell(sb, &cols[2], deaths);
        v10_table_cell(sb, &cols[3], kd);
        v10_table_row_end(sb);
    }
    v10_table_end(sb);
    free(rows);
}

static void earnings_table(StrBuf *sb, const char *table, const cJSON *earn, const int from, const int count) {
    static const char *const keys[] = {"day", "map_name", "reward"};
    static const V10Col cols[] = {{"Day", NULL, 1, 1}, {"Map", NULL, 0, 1}, {"Reward", NULL, 1, 1}};
    int n, col, desc;
    const cJSON **rows = sorted_rows(table, earn, keys, &n, &col, &desc);
    char fmt[64];
    snprintf(fmt, sizeof(fmt), "knTwSort('%s',%%d)", table);
    v10_table_begin(sb, cols, 3, col, desc, fmt);
    for (int i = from; i < n && i < from + count; i++) {
        const cJSON *r = rows[i];
        char day[16], kr[40];
        snprintf(day, sizeof(day), "%.0f", v10_num(r, "day", 0));
        v10_fmt_int(kr, sizeof(kr), v10_num(r, "reward", 0));
        strncat(kr, " KR", sizeof(kr) - strlen(kr) - 1);
        v10_table_row(sb, NULL, 0);
        v10_table_cell(sb, &cols[0], day);
        v10_table_cell(sb, &cols[1], v10_str(r, "map_name"));
        v10_table_cell(sb, &cols[2], kr);
        v10_table_row_end(sb);
    }
    v10_table_end(sb);
    free(rows);
}

// _50603 payouts (svelte-myp8tq)
static void payouts(StrBuf *sb, const char *clan, ClanFilters *f, const int current_day, const char *who) {
    const V10Req *totals = get_clan_payouts(clan, -1);
    sb_append(sb, "<div class=\"payouts-container svelte-myp8tq\">");
    const cJSON *t = data_of(totals);
    if (totals->status == 0 && !t) {
        sb_append(sb, "<div class=\"payouts-loading svelte-myp8tq\">Loading payouts...</div></div>");
        return;
    }
    if (!cJSON_IsObject(t)) {
        sb_append(sb, "<div class=\"payouts-error svelte-myp8tq\">Failed to load payouts</div></div>");
        return;
    }
    const int day = f->pay_day >= 0 ? f->pay_day : current_day;
    sb_appendf(sb, "<div class=\"payouts-controls svelte-myp8tq\"><div class=\"view-tabs svelte-myp8tq\"><button class=\"view-tab svelte-myp8tq%s\" onclick=\"knTwPayView('%s',0)\">"
                   "All-Time Totals</button> <button class=\"view-tab svelte-myp8tq%s\" onclick=\"knTwPayView('%s',1)\">Daily View</button></div> ",
               f->pay_daily ? "" : " active", who, f->pay_daily ? " active" : "", who);
    if (f->pay_daily) {
        char value[16], maxd[16], h[48], hi[48];
        day_date(value, sizeof(value), day);
        day_date(maxd, sizeof(maxd), current_day);
        snprintf(h, sizeof(h), "knTwPayDay('%s',", who);
        snprintf(hi, sizeof(hi), "knTwPayDate('%s',", who);
        sb_appendf(sb, "<div class=\"day-selector svelte-myp8tq\"><button class=\"day-arrow svelte-myp8tq\"%s onclick=\"%s-1)\">&#9664;</button> "
                       "<input type=\"date\" class=\"day-input svelte-myp8tq\" value=\"%s\" min=\"2026-03-17\" max=\"%s\" onchange=\"%sthis.value)\"> "
                       "<button class=\"day-arrow svelte-myp8tq\"%s onclick=\"%s1)\">&#9654;</button></div>",
                   f->pay_day >= 0 && f->pay_day <= 0 ? " disabled" : "", h, value, maxd, hi, f->pay_day < 0 || f->pay_day >= current_day ? " disabled" : "", h);
    } else sb_append(sb, "<!---->");
    sb_append(sb, "</div> <div class=\"payouts-header svelte-myp8tq\"><span class=\"payouts-total svelte-myp8tq\">");
    char total[40];
    const cJSON *daily = NULL;
    const V10Req *dr = NULL;
    if (f->pay_daily) {
        dr = get_clan_payouts(clan, day);
        daily = data_of(dr);
    }
    if (!f->pay_daily) {
        v10_fmt_int(total, sizeof(total), v10_num(t, "total", 0));
        sb_appendf(sb, "Total Distributed: <strong class=\"svelte-myp8tq\">%s KR</strong>", total);
    } else if (cJSON_IsObject(daily)) {
        char date[16];
        day_date(date, sizeof(date), day);
        v10_fmt_int(total, sizeof(total), v10_num(daily, "total", 0));
        sb_appendf(sb, "%s Total: <strong class=\"svelte-myp8tq\">%s KR</strong>", date, total);
    } else sb_append(sb, "<!---->");
    sb_append(sb, "</span></div> ");
    if (!f->pay_daily) {
        const cJSON *rows = cJSON_GetObjectItem(t, "payouts");
        if (!cJSON_GetArraySize(rows)) sb_append(sb, "<div class=\"payouts-empty svelte-myp8tq\">No individual payouts recorded yet.</div>");
        else {
            sb_append(sb, "<div class=\"payouts-table svelte-myp8tq\"><div class=\"payouts-row payouts-row-header svelte-myp8tq\"><span class=\"payouts-cell payouts-cell-name svelte-myp8tq\">"
                          "Player</span> <span class=\"payouts-cell payouts-cell-kr svelte-myp8tq\">Total</span></div> ");
            const cJSON *r;
            cJSON_ArrayForEach(r, rows) {
                char kr[40];
                v10_fmt_int(kr, sizeof(kr), v10_num(r, "total_kr", 0));
                sb_appendf(sb, "<div class=\"payouts-row svelte-myp8tq\"><span class=\"payouts-cell payouts-cell-name svelte-myp8tq clickable-name\" onclick=\"knTwPlayer('%.0f')\">",
                           v10_num(r, "player_id", 0));
                sb_append_esc(sb, v10_str(r, "player_name"));
                sb_appendf(sb, "</span> <span class=\"payouts-cell payouts-cell-kr svelte-myp8tq\">%s KR</span></div>", kr);
            }
            sb_append(sb, "</div>");
        }
    } else if (dr && dr->status == 0 && !daily) {
        sb_append(sb, "<div class=\"payouts-loading svelte-myp8tq\">Loading...</div>");
    } else if (cJSON_IsObject(daily)) {
        const cJSON *rows = cJSON_GetObjectItem(daily, "payouts");
        if (!cJSON_GetArraySize(rows)) {
            char date[16];
            day_date(date, sizeof(date), day);
            sb_appendf(sb, "<div class=\"payouts-empty svelte-myp8tq\">No payouts for %s.</div>", date);
        } else {
            sb_append(sb, "<div class=\"payouts-table svelte-myp8tq\"><div class=\"payouts-row payouts-row-header svelte-myp8tq\"><span class=\"payouts-cell payouts-cell-name svelte-myp8tq\">"
                          "Player</span> <span class=\"payouts-cell payouts-cell-map svelte-myp8tq\">Map</span> <span class=\"payouts-cell payouts-cell-kr svelte-myp8tq\">Earned</span></div> ");
            const cJSON *r;
            cJSON_ArrayForEach(r, rows) {
                char kr[40];
                v10_fmt_int(kr, sizeof(kr), v10_num(r, "total_kr", 0));
                sb_appendf(sb, "<div class=\"payouts-row svelte-myp8tq\"><span class=\"payouts-cell payouts-cell-name svelte-myp8tq clickable-name\" onclick=\"knTwPlayer('%.0f')\">",
                           v10_num(r, "player_id", 0));
                sb_append_esc(sb, v10_str(r, "player_name"));
                sb_append(sb, "</span> <span class=\"payouts-cell payouts-cell-map svelte-myp8tq\">");
                sb_append_esc(sb, v10_str(r, "map_name"));
                sb_appendf(sb, "</span> <span class=\"payouts-cell payouts-cell-kr svelte-myp8tq\">%s KR</span></div>", kr);
            }
            sb_append(sb, "</div>");
        }
    } else if (dr && dr->status != 0) {
        sb_append(sb, "<div class=\"payouts-error svelte-myp8tq\">Failed to load daily payouts</div>");
    } else sb_append(sb, "<div class=\"payouts-empty svelte-myp8tq\">Select a date above to view payouts.</div>");
    sb_append(sb, " <!----></div>");
}

static void historical_day(StrBuf *sb, const cJSON *clan, const cJSON *d) {
    const char *tag = v10_str(clan, "clan_name");
    const double k = v10_num(d, "kills", 0), de = v10_num(d, "deaths", 0);
    char kills[32], deaths[32], kd[24];
    v10_fmt_int(kills, sizeof(kills), k);
    v10_fmt_int(deaths, sizeof(deaths), de);
    if (de > 0) snprintf(kd, sizeof(kd), "%.2f", k / de);
    else snprintf(kd, sizeof(kd), "%s", k > 0 ? "\xe2\x88\x9e" : "0.00");
    sb_appendf(sb, "<div class=\"historical-day svelte-51j5cz\"><div class=\"day-header svelte-51j5cz\"><span class=\"day-label svelte-51j5cz\">Day %.0f</span> "
                   "<span class=\"day-map svelte-51j5cz\">",
               v10_num(d, "day", 0));
    sb_append_esc(sb, v10_str(d, "map_name"));
    sb_appendf(sb, "</span></div> <div class=\"vs-comparison svelte-51j5cz\"><div class=\"clan-side our-clan svelte-51j5cz\"><div class=\"clan-header svelte-51j5cz\"><!----> "
                   "<div class=\"clan-name svelte-51j5cz\" style=\"color: %s;\">",
               clan_color(tag, "#5ce05a"));
    sb_append_esc(sb, tag);
    sb_appendf(sb, "</div> <div class=\"clan-position svelte-51j5cz\">#%.0f</div></div> <div class=\"clan-stats svelte-51j5cz\"><div class=\"stat-row svelte-51j5cz\">"
                   "<span class=\"stat-label svelte-51j5cz\">Kills</span> <span class=\"stat-value kills svelte-51j5cz\">%s</span></div> <div class=\"stat-row svelte-51j5cz\">"
                   "<span class=\"stat-label svelte-51j5cz\">Deaths</span> <span class=\"stat-value deaths svelte-51j5cz\">%s</span></div> <div class=\"stat-row svelte-51j5cz\">"
                   "<span class=\"stat-label svelte-51j5cz\">KD</span> <span class=\"stat-value kd svelte-51j5cz\">%s</span></div></div></div> "
                   "<div class=\"vs-divider svelte-51j5cz\"><div class=\"vs-text svelte-51j5cz\">VS</div></div> ",
               v10_num(d, "position", 0), kills, deaths, kd);
    const char *ctag = v10_str(d, "comparison_clan_name");
    if (ctag[0]) {
        const double ck = v10_num(d, "comparison_kills", 0), cd = v10_num(d, "comparison_deaths", 0);
        v10_fmt_int(kills, sizeof(kills), ck);
        v10_fmt_int(deaths, sizeof(deaths), cd);
        if (cd > 0) snprintf(kd, sizeof(kd), "%.2f", ck / cd);
        else snprintf(kd, sizeof(kd), "%s", ck > 0 ? "\xe2\x88\x9e" : "0.00");
        sb_appendf(sb, "<div class=\"clan-side comparison-clan svelte-51j5cz\"><div class=\"clan-header svelte-51j5cz\"><!----> <div class=\"clan-name clickable-clan-name svelte-51j5cz\" "
                       "style=\"color: %s;\"",
                   clan_color(ctag, "#ff6b6b"));
        if (cJSON_IsNumber(cJSON_GetObjectItem(d, "comparison_clan_id"))) sb_appendf(sb, " onclick=\"knTwClan('%.0f')\"", v10_num(d, "comparison_clan_id", 0));
        sb_append(sb, ">");
        sb_append_esc(sb, ctag);
        sb_appendf(sb, "</div> <div class=\"clan-position svelte-51j5cz\">#%.0f</div></div> <div class=\"clan-stats svelte-51j5cz\"><div class=\"stat-row svelte-51j5cz\">"
                       "<span class=\"stat-label svelte-51j5cz\">Kills</span> <span class=\"stat-value kills svelte-51j5cz\">%s</span></div> <div class=\"stat-row svelte-51j5cz\">"
                       "<span class=\"stat-label svelte-51j5cz\">Deaths</span> <span class=\"stat-value deaths svelte-51j5cz\">%s</span></div> <div class=\"stat-row svelte-51j5cz\">"
                       "<span class=\"stat-label svelte-51j5cz\">KD</span> <span class=\"stat-value kd svelte-51j5cz\">%s</span></div></div></div>",
                   v10_num(d, "comparison_position", 1), kills, deaths, kd);
    } else sb_append(sb, "<div class=\"clan-side comparison-clan no-comparison svelte-51j5cz\"><div class=\"no-comparison-text svelte-51j5cz\">No comparison data</div></div>");
    sb_append(sb, "</div></div>");
}

static double earnings_sum(const cJSON *earn, const int from_day, const int to_day) {
    double s = 0;
    const cJSON *e;
    cJSON_ArrayForEach(e, earn) {
        const int d = (int) v10_num(e, "day", 0);
        if (from_day < 0 || (d >= from_day && d <= to_day)) s += v10_num(e, "reward", 0);
    }
    return s;
}

static void pagination(StrBuf *sb, const char *hash, const char *handler, const int page, const int per, const int total) {
    sb_appendf(sb, "<div class=\"pagination svelte-%s\"><button class=\"page-btn svelte-%s\"%s onclick=\"%s(-1)\">Prev</button> <span class=\"page-info svelte-%s\">%d-%d of %d</span> "
                   "<button class=\"page-btn svelte-%s\"%s onclick=\"%s(1)\">Next</button></div>",
               hash, hash, page == 0 ? " disabled" : "", handler, hash, page * per + 1, (page + 1) * per < total ? (page + 1) * per : total, total, hash,
               (page + 1) * per >= total ? " disabled" : "", handler);
}

// My Clan (mine = 1, svelte-51j5cz) or the clan view (svelte-c92dbh)
static void clan_stats(StrBuf *sb, const char *clan_id, const int mine) {
    const char *h = mine ? "51j5cz" : "c92dbh";
    ClanFilters *f = mine ? &T.mine : &T.other;
    const V10Req *cr = get_clan(clan_id);
    const cJSON *clan = data_of(cr);
    sb_appendf(sb, "<div class=\"%s svelte-%s\">", mine ? "my-clan-tab" : "clan-profile", h);
    if (cr->status == 0 && !clan) {
        loading(sb, h, "Loading clan data...");
        sb_append(sb, "</div>");
        return;
    }
    if (!cJSON_IsObject(clan)) { // no stats: v10 says so (404 / no participation)
        if (mine)
            sb_append(sb, "<div class=\"no-participation svelte-51j5cz\"><div class=\"no-participation-content svelte-51j5cz\"><h2 class=\"no-participation-title svelte-51j5cz\">"
                          "No Participation Data</h2> <p class=\"no-participation-description svelte-51j5cz\">Your clan hasn't participated in Turf Wars yet. Start playing on eligible "
                          "maps to see your clan's statistics!</p> <div class=\"eligible-maps svelte-51j5cz\"><h3 class=\"svelte-51j5cz\">Eligible Maps:</h3> <ul class=\"svelte-51j5cz\">"
                          "<li class=\"svelte-51j5cz\">Burg</li> <li class=\"svelte-51j5cz\">Sandstorm</li> <li class=\"svelte-51j5cz\">Subzero</li></ul></div></div></div>");
        else {
            sb_append(sb, "<div class=\"no-participation svelte-c92dbh\"><div class=\"no-participation-content svelte-c92dbh\"><h2 class=\"no-participation-title svelte-c92dbh\">"
                          "No Participation Data</h2> <p class=\"no-participation-description svelte-c92dbh\">This clan hasn't participated in Turf Wars yet.</p> ");
            v10_back_button(sb, "knTwBack()");
            sb_append(sb, "</div></div>");
        }
        sb_append(sb, "</div>");
        return;
    }
    const char *tag = v10_str(clan, "clan_name");
    const int current = (int) v10_num(clan, "current_day", day_now());
    sb_appendf(sb, "<div class=\"clan-stats-container svelte-%s\"><div class=\"clan-header svelte-%s\"><div class=\"clan-info svelte-%s\"><!----> "
                   "<h2 class=\"clan-name svelte-%s\" style=\"color: %s;\">",
               h, h, h, h, clan_color(tag, "#ffffff"));
    sb_append_esc(sb, tag);
    sb_append(sb, "</h2></div>");
    if (!mine) {
        sb_append(sb, " <div class=\"header-actions svelte-c92dbh\">");
        v10_back_button(sb, "knTwBack()");
        sb_append(sb, "</div>");
    }
    sb_appendf(sb, "</div> <div class=\"stats-section svelte-%s\"><h3 class=\"section-title svelte-%s\">Today's Performance</h3> ", h, h);
    // today's maps: My Clan shows .maps; the clan view the entries of the latest day
    cJSON *maps = cJSON_CreateArray();
    double latest = -1;
    const cJSON *m;
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(clan, "maps")) if (v10_num(m, "day", 0) > latest) latest = v10_num(m, "day", 0);
    cJSON_ArrayForEach(m, cJSON_GetObjectItem(clan, "maps")) {
        if (mine || v10_num(m, "day", 0) == latest) cJSON_AddItemToArray(maps, cJSON_Duplicate(m, 1));
    }
    if (cJSON_GetArraySize(maps)) {
        sb_appendf(sb, "<div class=\"table-section svelte-%s\">", h);
        maps_table(sb, mine ? "mc-maps" : "cv-maps", maps, mine);
        sb_append(sb, "</div>");
    } else sb_appendf(sb, "<div class=\"no-data svelte-%s\">No stats available for today yet.</div>", h);
    cJSON_Delete(maps);
    sb_appendf(sb, "</div> <div class=\"stats-section svelte-%s\"><div class=\"section-header svelte-%s\"><h3 class=\"section-title svelte-%s\">Top Contributors</h3> "
                   "<div class=\"header-controls svelte-%s\">",
               h, h, h, h);
    day_selector(sb, h, mine ? "knTwDay" : "knTwCvDay", mine ? "knTwDate" : "knTwCvDate", f->day, current, 0);
    sb_append(sb, "</div> ");
    map_select(sb, h, mine ? "knTwMap" : "knTwCvMap", f->map);
    const V10Req *pr = get_clan_players(clan_id, f);
    const cJSON *players = data_of(pr);
    const int np = cJSON_GetArraySize(players);
    if (mine && !f->view_all && np > 5) sb_append(sb, " <button class=\"view-all-btn svelte-51j5cz\" onclick=\"knTwViewAll()\">View All Players</button>");
    else if (mine) sb_append(sb, " <!---->");
    sb_append(sb, "</div></div> ");
    if (np > 0) {
        if (mine) {
            sb_append(sb, "<div class=\"table-section svelte-51j5cz\">");
            players_table(sb, "mc-players", players, 0, f->view_all ? np : 5);
            sb_append(sb, "</div>");
        } else {
            sb_append(sb, "<div class=\"table-section svelte-c92dbh\">");
            players_table(sb, "cv-players", players, 7 * f->players_page, 7);
            sb_append(sb, "</div> ");
            if (np > 7) pagination(sb, "c92dbh", "knTwCvPlayersPage", f->players_page, 7, np);
            else sb_append(sb, "<!---->");
        }
    } else sb_appendf(sb, "<div class=\"no-data svelte-%s\">No player contributions found for the selected map.</div>", h);
    sb_append(sb, "</div> ");
    // Previous 3 Days (My Clan only)
    const cJSON *hist = cJSON_GetObjectItem(clan, "historical_days");
    if (mine && cJSON_GetArraySize(hist) > 0) {
        sb_append(sb, "<div class=\"stats-section svelte-51j5cz\"><h3 class=\"section-title svelte-51j5cz\">Previous 3 Days (Top Map)</h3> <div class=\"historical-days svelte-51j5cz\">");
        const cJSON *d;
        cJSON_ArrayForEach(d, hist) historical_day(sb, clan, d);
        sb_append(sb, "</div></div>");
    } else if (mine) sb_append(sb, "<!---->");
    const V10Req *er = get_clan_earnings(clan_id);
    const cJSON *earn = cJSON_IsArray(data_of(er)) ? data_of(er) : NULL;
    char last30[40], total[40];
    int ref_day = current;
    if (!mine) { // the clan view counts back from its latest map day
        ref_day = latest >= 0 ? (int) latest : 0;
    }
    v10_fmt_int(last30, sizeof(last30), earnings_sum(earn, ref_day - 30, ref_day));
    v10_fmt_int(total, sizeof(total), earnings_sum(earn, -1, 0));
    sb_appendf(sb, "%s<div class=\"stats-section svelte-%s\"><div class=\"section-header svelte-%s\"><h3 class=\"section-title svelte-%s\">Clan Earnings</h3> "
                   "<div class=\"earnings-summary svelte-%s\"><div class=\"earnings-item svelte-%s\"><span class=\"earnings-label svelte-%s\">Last 30 Days:</span> "
                   "<span class=\"earnings-amount svelte-%s\">%s KR</span></div> <div class=\"earnings-item svelte-%s\"><span class=\"earnings-label svelte-%s\">Total:</span> "
                   "<span class=\"earnings-amount svelte-%s\">%s KR</span></div></div></div> ",
               mine ? " " : "", h, h, h, h, h, h, h, last30, h, h, h, total);
    const int ne = cJSON_GetArraySize(earn), per = mine ? 20 : 7;
    if (ne > 0) {
        sb_appendf(sb, "<div class=\"table-section svelte-%s\">", h);
        earnings_table(sb, mine ? "mc-earn" : "cv-earn", earn, per * f->earn_page, per);
        sb_append(sb, "</div> ");
        if (ne > per) pagination(sb, h, mine ? "knTwEarnPage" : "knTwCvEarnPage", f->earn_page, per, ne);
        else sb_append(sb, "<!---->");
    } else sb_appendf(sb, "<div class=\"no-data svelte-%s\">No earnings recorded yet.</div>", h);
    sb_appendf(sb, "</div> <div class=\"stats-section svelte-%s\"><div class=\"section-header svelte-%s\"><h3 class=\"section-title svelte-%s\">Individual Payouts</h3></div> ", h, h, h);
    char id[32];
    snprintf(id, sizeof(id), "%.0f", v10_num(clan, "clan_id", 0));
    payouts(sb, id, f, current, mine ? "mine" : "other");
    sb_append(sb, "</div></div></div>");
}

static void gen_my_clan(StrBuf *sb) {
    const AccountInfo *a = account_get();
    if (!a || !a->clan[0]) {
        sb_append(sb, "<div class=\"my-clan-tab svelte-51j5cz\"><div class=\"no-clan-container svelte-51j5cz\"><div class=\"no-clan-content svelte-51j5cz\"><h2 class=\"no-clan-title svelte-51j5cz\">"
                      "Join the Turf Wars!</h2> <p class=\"no-clan-description svelte-51j5cz\">Create or join a clan to participate in Turf Wars and compete for control of maps!</p> "
                      "<div class=\"clan-actions svelte-51j5cz\"><div class=\"action-section svelte-51j5cz\"><h3 class=\"action-title svelte-51j5cz\">Create Clan</h3> "
                      "<p class=\"action-description svelte-51j5cz\">Start your own clan and lead the charge!</p> "
                      "<button class=\"main-button svelte-whnh40 main-button-accept\" type=\"button\" onclick=\"clearPops();showWindow(13)\">Create Clan</button></div> "
                      "<div class=\"action-section svelte-51j5cz\"><h3 class=\"action-title svelte-51j5cz\">Join Clan</h3> <p class=\"action-description svelte-51j5cz\">Browse recruitment "
                      "posts and find a clan!</p> <button class=\"main-button svelte-whnh40 main-button-accept-secondary\" type=\"button\" onclick=\"playSelect();knTwTab(3)\">Join Clan</button>"
                      "</div></div> <div class=\"clan-benefits svelte-51j5cz\"><h3 class=\"svelte-51j5cz\">Benefits of Joining a Clan:</h3> <ul class=\"svelte-51j5cz\"><li class=\"svelte-51j5cz\">"
                      "Compete in Turf Wars for map control</li> <li class=\"svelte-51j5cz\">Earn rewards for your clan's victories</li> <li class=\"svelte-51j5cz\">Track your clan's "
                      "performance and rankings</li> <li class=\"svelte-51j5cz\">Work together to dominate the battlefield</li></ul></div></div></div></div>");
        return;
    }
    clan_stats(sb, a->clan, 1);
}

// ------------------------------------------------------------------ Leaderboards (svelte-raovk7)

static void clan_cell(StrBuf *sb, const V10Col *col, const cJSON *r) {
    StrBuf c = {0};
    const char *tag = v10_str(r, "clan_name");
    sb_appendf(&c, "<div class=\"clan-name-cell svelte-1o1ab69 clickable\"><!----> <span class=\"clan-name svelte-1o1ab69\" style=\"color: %s;\">", clan_color(tag, "#ffffff"));
    sb_append_esc(&c, tag);
    sb_append(&c, "</span></div>");
    char *html = sb_finish(&c);
    v10_table_cell_html(sb, col, html);
    free(html);
}

static void filter_map_select(StrBuf *sb) {
    sb_appendf(sb, "<select id=\"map-filter\" class=\"filter-select svelte-49w24t\"><option value=\"\"%s>All Maps</option>", T.f_map < 0 ? " selected" : "");
    for (int i = 0; i < TURF_MAP_COUNT; i++)
        sb_appendf(sb, "<option value=\"%d\"%s>%s</option>", g_turf_maps[i], g_turf_maps[i] == T.f_map ? " selected" : "", map_name(g_turf_maps[i]));
    sb_append(sb, "</select>");
}

static void board_body(StrBuf *sb, const char *hash, const V10Req *r, const char *fail) {
    const cJSON *rows = data_of(r);
    if (r->status != 200 && r->status != 0) error_box(sb, hash, r, fail);
    else if (r->status == 0 && !rows) loading(sb, hash, "Loading leaderboard...");
    else if (!cJSON_GetArraySize(rows)) sb_appendf(sb, "<div class=\"no-data svelte-%s\">No data available for the selected filters.</div>", hash);
    else {
        sb_appendf(sb, "<div class=\"table-section svelte-%s\">", hash);
        const char *table = T.board == 0 ? "lb-clans" : T.board == 1 ? "lb-gross" : "lb-players";
        char fmt[64];
        snprintf(fmt, sizeof(fmt), "knTwSort('%s',%%d)", table);
        if (T.board == 0) {
            static const char *const keys[] = {"position", "clan_name", "kills", "deaths", "kd", "games"};
            static const V10Col cols[] = {{"Rank", NULL, 1, 1}, {"Clan", NULL, 0, 1}, {"Kills", NULL, 1, 1}, {"Deaths", NULL, 1, 1}, {"K/D", NULL, 1, 1}, {"Games", NULL, 1, 1}};
            int n, col, desc;
            const cJSON **s = sorted_rows(table, rows, keys, &n, &col, &desc);
            v10_table_begin(sb, cols, 6, col, desc, fmt);
            for (int i = 0; i < n; i++) {
                char pos[24], k[32], d[32], kd[24], g[32], click[96];
                snprintf(pos, sizeof(pos), "#%.0f", v10_num(s[i], "position", i + 1));
                v10_fmt_int(k, sizeof(k), v10_num(s[i], "kills", 0));
                v10_fmt_int(d, sizeof(d), v10_num(s[i], "deaths", 0));
                snprintf(kd, sizeof(kd), "%.2f", v10_num(s[i], "kd", 0));
                v10_fmt_int(g, sizeof(g), v10_num(s[i], "games", 0));
                snprintf(click, sizeof(click), "playTick(0.5);knTwClan('%.0f')", v10_num(s[i], "clan_id", 0));
                v10_table_row(sb, click, 0);
                v10_table_cell(sb, &cols[0], pos);
                clan_cell(sb, &cols[1], s[i]);
                v10_table_cell(sb, &cols[2], k);
                v10_table_cell(sb, &cols[3], d);
                v10_table_cell(sb, &cols[4], kd);
                v10_table_cell(sb, &cols[5], g);
                v10_table_row_end(sb);
            }
            v10_table_end(sb);
            free(s);
        } else if (T.board == 1) {
            static const char *const keys[] = {"position", "clan_name", "total_kr", "distributed", "bank"};
            static const V10Col cols[] = {{"Rank", NULL, 1, 1}, {"Clan", NULL, 0, 1}, {"Total KR", NULL, 1, 1}, {"Distributed", NULL, 1, 1}, {"Bank", NULL, 1, 1}};
            int n, col, desc;
            const cJSON **s = sorted_rows(table, rows, keys, &n, &col, &desc);
            v10_table_begin(sb, cols, 5, col, desc, fmt);
            for (int i = 0; i < n; i++) {
                char pos[24], t[40], d[40], b[40], click[96];
                snprintf(pos, sizeof(pos), "#%.0f", v10_num(s[i], "position", i + 1));
                v10_fmt_int(t, sizeof(t), v10_num(s[i], "total_kr", 0));
                v10_fmt_int(d, sizeof(d), v10_num(s[i], "distributed", 0));
                v10_fmt_int(b, sizeof(b), v10_num(s[i], "bank", 0));
                strncat(t, " KR", sizeof(t) - strlen(t) - 1);
                strncat(d, " KR", sizeof(d) - strlen(d) - 1);
                strncat(b, " KR", sizeof(b) - strlen(b) - 1);
                snprintf(click, sizeof(click), "playTick(0.5);knTwClan('%.0f')", v10_num(s[i], "clan_id", 0));
                v10_table_row(sb, click, 0);
                v10_table_cell(sb, &cols[0], pos);
                clan_cell(sb, &cols[1], s[i]);
                v10_table_cell(sb, &cols[2], t);
                v10_table_cell(sb, &cols[3], d);
                v10_table_cell(sb, &cols[4], b);
                v10_table_row_end(sb);
            }
            v10_table_end(sb);
            free(s);
        } else {
            static const char *const keys[] = {"position", "player_name", "kills", "deaths", "kd", "games", "earnings"};
            static const V10Col cols[] = {{"Rank", NULL, 1, 1},  {"Player", NULL, 0, 1}, {"Kills", NULL, 1, 1},   {"Deaths", NULL, 1, 1},
                                          {"K/D", NULL, 1, 1},   {"Games", NULL, 1, 1},  {"Earnings", NULL, 1, 1}};
            int n, col, desc;
            const cJSON **s = sorted_rows(table, rows, keys, &n, &col, &desc);
            v10_table_begin(sb, cols, 7, col, desc, fmt);
            for (int i = 0; i < n; i++) {
                char pos[24], name[96], k[32], d[32], kd[24], g[32], e[40], click[96];
                snprintf(pos, sizeof(pos), "#%.0f", v10_num(s[i], "position", i + 1));
                const char *clan = v10_str(s[i], "clan_name");
                if (clan[0]) snprintf(name, sizeof(name), "%s [%s]", v10_str(s[i], "player_name"), clan);
                else snprintf(name, sizeof(name), "%s", v10_str(s[i], "player_name"));
                v10_fmt_int(k, sizeof(k), v10_num(s[i], "kills", 0));
                v10_fmt_int(d, sizeof(d), v10_num(s[i], "deaths", 0));
                snprintf(kd, sizeof(kd), "%.2f", v10_num(s[i], "kd", 0));
                v10_fmt_int(g, sizeof(g), v10_num(s[i], "games", 0));
                v10_fmt_int(e, sizeof(e), v10_num(s[i], "earnings", 0));
                strncat(e, " KR", sizeof(e) - strlen(e) - 1);
                snprintf(click, sizeof(click), "playTick(0.5);knTwPlayer('%.0f')", v10_num(s[i], "player_id", 0));
                v10_table_row(sb, click, 0);
                v10_table_cell(sb, &cols[0], pos);
                v10_table_cell(sb, &cols[1], name);
                v10_table_cell(sb, &cols[2], k);
                v10_table_cell(sb, &cols[3], d);
                v10_table_cell(sb, &cols[4], kd);
                v10_table_cell(sb, &cols[5], g);
                v10_table_cell(sb, &cols[6], e);
                v10_table_row_end(sb);
            }
            v10_table_end(sb);
            free(s);
        }
        sb_append(sb, "</div>");
    }
}

static void gen_boards(StrBuf *sb) {
    sb_append(sb, "<div class=\"leaderboards-tab svelte-raovk7\">");
    if (T.board < 0) {
        sb_append(sb, "<div class=\"leaderboard-grid svelte-raovk7\"><button type=\"button\" class=\"leaderboard-card svelte-raovk7\" onclick=\"playSelect();knTwBoard(0)\">"
                      "<div class=\"card-icon svelte-raovk7\">\xf0\x9f\x8f\x86</div> <h3 class=\"card-title svelte-raovk7\">Top Performing Clans</h3> <p class=\"card-description svelte-raovk7\">"
                      "View clans ranked by performance.</p></button> <button type=\"button\" class=\"leaderboard-card svelte-raovk7\" onclick=\"playSelect();knTwBoard(1)\">"
                      "<div class=\"card-icon svelte-raovk7\">");
        v10_emoji(sb, "\xf0\x9f\x92\xb0", "");
        sb_append(sb, "</div> <h3 class=\"card-title svelte-raovk7\">Top Grossing Clans</h3> <p class=\"card-description svelte-raovk7\">"
                      "See which clans have earned the most KR from Turf Wars.</p></button> <button type=\"button\" class=\"leaderboard-card svelte-raovk7\" onclick=\"playSelect();knTwBoard(2)\">"
                      "<div class=\"card-icon svelte-raovk7\">\xe2\xad\x90</div> <h3 class=\"card-title svelte-raovk7\">Top Performing Players</h3> <p class=\"card-description svelte-raovk7\">"
                      "All-time leaderboard of top performing players.</p></button></div></div>");
        return;
    }
    static const char *const titles[] = {"Top Performing Clans", "Top Grossing Clans", "Top Performing Players"};
    sb_append(sb, "<div class=\"leaderboard-view svelte-raovk7\"><div class=\"view-header svelte-raovk7\">");
    v10_back_button(sb, "playSelect();knTwBoard(-1)");
    sb_appendf(sb, " <h2 class=\"view-title svelte-raovk7\">%s</h2></div> ", titles[T.board]);
    char path[320], name[160];
    enc(name, sizeof(name), T.f_name);
    if (T.board == 0) {
        char today[16];
        const time_t t = time(NULL);
        struct tm tm;
#ifdef _WIN32
        gmtime_s(&tm, &t);
#else
        gmtime_r(&t, &tm);
#endif
        strftime(today, sizeof(today), "%Y-%m-%d", &tm);
        const char *date = T.f_date[0] ? T.f_date : today;
        const int day = date_day(date);
        int o = snprintf(path, sizeof(path), "/api/territorial-wars/leaderboards/top-performing-clans");
        char sep = '?';
        if (day >= 0) o += snprintf(path + o, sizeof(path) - (size_t) o, "%cday=%d", sep, day), sep = '&';
        if (T.f_map >= 0) o += snprintf(path + o, sizeof(path) - (size_t) o, "%cmap_id=%d", sep, T.f_map), sep = '&';
        if (T.f_name[0]) snprintf(path + o, sizeof(path) - (size_t) o, "%cclan_name=%s", sep, name);
        sb_appendf(sb, "<div class=\"leaderboard-component svelte-49w24t\"><div class=\"filters-section svelte-49w24t\"><div class=\"filter-group svelte-49w24t\">"
                       "<label class=\"filter-label svelte-49w24t\" for=\"date-filter\">Date:</label> <input id=\"date-filter\" type=\"date\" class=\"filter-input svelte-49w24t\" "
                       "min=\"2026-03-17\" max=\"%s\" value=\"%s\"></div> <div class=\"filter-group svelte-49w24t\"><label class=\"filter-label svelte-49w24t\" for=\"map-filter\">Map:</label> ",
                   today, date);
        filter_map_select(sb);
        sb_append(sb, "</div> <div class=\"filter-group svelte-49w24t\"><label class=\"filter-label svelte-49w24t\" for=\"clan-name-filter\">Search by Clan Name:</label> "
                      "<input id=\"clan-name-filter\" type=\"text\" class=\"filter-input svelte-49w24t\" placeholder=\"Clan name\" value=\"");
        esc_attr(sb, T.f_name);
        sb_append(sb, "\"></div> <button class=\"main-button svelte-whnh40 main-button-accept\" type=\"button\" onclick=\"playSelect();knTwApply()\">Apply Filters</button></div> ");
        board_body(sb, "49w24t", v10_get(path, TTL_MS), "Failed to load top performing clans");
    } else if (T.board == 1) {
        if (T.f_name[0]) snprintf(path, sizeof(path), "/api/territorial-wars/leaderboards/top-grossing-clans?clan_name=%s", name);
        else snprintf(path, sizeof(path), "/api/territorial-wars/leaderboards/top-grossing-clans");
        sb_append(sb, "<div class=\"leaderboard-component svelte-1rpti7l\"><div class=\"filters-section svelte-1rpti7l\"><div class=\"filter-group svelte-1rpti7l\">"
                      "<label class=\"filter-label svelte-1rpti7l\" for=\"clan-name-filter\">Search by Clan Name:</label> <input id=\"clan-name-filter\" type=\"text\" "
                      "class=\"filter-input svelte-1rpti7l\" placeholder=\"Clan name\" value=\"");
        esc_attr(sb, T.f_name);
        sb_append(sb, "\"></div> <button class=\"main-button svelte-whnh40 main-button-accept\" type=\"button\" onclick=\"playSelect();knTwApply()\">Apply Filters</button></div> ");
        board_body(sb, "1rpti7l", v10_get(path, TTL_MS), "Failed to load top grossing clans");
    } else {
        int o = snprintf(path, sizeof(path), "/api/territorial-wars/leaderboards/top-performing-players");
        char sep = '?';
        if (T.f_map >= 0) o += snprintf(path + o, sizeof(path) - (size_t) o, "%cmap_id=%d", sep, T.f_map), sep = '&';
        if (T.f_name[0]) snprintf(path + o, sizeof(path) - (size_t) o, "%cplayer_name=%s", sep, name);
        sb_append(sb, "<div class=\"leaderboard-component svelte-49w24t\"><div class=\"filters-section svelte-49w24t\"><div class=\"filter-group svelte-49w24t\">"
                      "<label class=\"filter-label svelte-49w24t\" for=\"map-filter\">Map:</label> ");
        filter_map_select(sb);
        sb_append(sb, "</div> <div class=\"filter-group svelte-49w24t\"><label class=\"filter-label svelte-49w24t\" for=\"player-name-filter\">Search by Player Name (exact match):</label> "
                      "<input id=\"player-name-filter\" type=\"text\" class=\"filter-input svelte-49w24t\" placeholder=\"Player name\" value=\"");
        esc_attr(sb, T.f_name);
        sb_append(sb, "\"></div> <button class=\"main-button svelte-whnh40 main-button-accept\" type=\"button\" onclick=\"playSelect();knTwApply()\">Apply Filters</button></div> ");
        board_body(sb, "49w24t", v10_get(path, TTL_MS), "Failed to load top performing players");
    }
    sb_append(sb, "</div></div></div>");
}

// ------------------------------------------------------------------ Recruitment (svelte-72bcy3)

static const char *pair_label(const char *const (*pairs)[2], const size_t n, const char *key) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(pairs[i][0], key)) return pairs[i][1];
    return key;
}

static int g_applied_ids[64], g_applied_count;

static void gen_recruit(StrBuf *sb) {
    char path[256], region[16], lang[16];
    enc(region, sizeof(region), T.rc_region);
    enc(lang, sizeof(lang), T.rc_lang);
    int o = snprintf(path, sizeof(path), "/api/clans/recruitment?page=%d&limit=20", T.rc_page);
    if (T.rc_region[0]) o += snprintf(path + o, sizeof(path) - (size_t) o, "&region=%s", region);
    if (T.rc_lang[0]) snprintf(path + o, sizeof(path) - (size_t) o, "&language=%s", lang);
    const V10Req *r = v10_get(path, TTL_MS);
    const AccountInfo *a = account_get();
    const int staff = a && (a->is_dev || a->is_mod), no_clan = a && !a->clan[0];
    if (no_clan) { // getMyApplications
        char t[256], p2[400];
        v10_url_token(t, sizeof(t));
        snprintf(p2, sizeof(p2), "/api/clans/my-applications?token=%s", t);
        const V10Req *ar = v10_get(p2, TTL_MS);
        g_applied_count = 0;
        const cJSON *id;
        cJSON_ArrayForEach(id, cJSON_GetObjectItem(data_of(ar), "clan_ids")) if (g_applied_count < 64) g_applied_ids[g_applied_count++] = id->valueint;
    }
    sb_appendf(sb, "<div class=\"recruitment-browse svelte-72bcy3\"><div class=\"filters-section svelte-72bcy3\"><div class=\"filter-group svelte-72bcy3\">"
                   "<label class=\"filter-label svelte-72bcy3\" for=\"region-filter\">Region:</label> <select id=\"region-filter\" class=\"filter-input svelte-72bcy3\" "
                   "onchange=\"knTwRcRegion(this.value)\"><option value=\"\"%s>All Regions</option>",
               T.rc_region[0] ? "" : " selected");
    for (size_t i = 0; i < sizeof(k_regions) / sizeof(k_regions[0]); i++)
        sb_appendf(sb, "<option value=\"%s\"%s>%s</option>", k_regions[i][0], !strcmp(T.rc_region, k_regions[i][0]) ? " selected" : "", k_regions[i][1]);
    sb_appendf(sb, "</select></div> <div class=\"filter-group svelte-72bcy3\"><label class=\"filter-label svelte-72bcy3\" for=\"lang-filter\">Language:</label> "
                   "<select id=\"lang-filter\" class=\"filter-input svelte-72bcy3\" onchange=\"knTwRcLang(this.value)\"><option value=\"\"%s>All Languages</option>",
               T.rc_lang[0] ? "" : " selected");
    for (size_t i = 0; i < sizeof(k_langs) / sizeof(k_langs[0]); i++)
        sb_appendf(sb, "<option value=\"%s\"%s>%s</option>", k_langs[i][0], !strcmp(T.rc_lang, k_langs[i][0]) ? " selected" : "", k_langs[i][1]);
    sb_append(sb, "</select></div></div> ");
    const cJSON *d = data_of(r);
    const cJSON *posts = cJSON_GetObjectItem(d, "posts");
    if (r->status == 0 && !d) sb_append(sb, "<div class=\"loading svelte-72bcy3\">Loading recruitment posts...</div>");
    else if (T.rc_error[0] || (r->status != 200 && r->status != 0)) {
        sb_append(sb, "<div class=\"error svelte-72bcy3\">");
        sb_append_esc(sb, T.rc_error[0] ? T.rc_error : "Failed to load recruitment posts");
        sb_append(sb, "</div>");
    } else if (!cJSON_GetArraySize(posts)) sb_append(sb, "<div class=\"no-data svelte-72bcy3\">No recruitment posts found. Try adjusting filters.</div>");
    else {
        sb_append(sb, "<div class=\"posts-grid svelte-72bcy3\">");
        const cJSON *p;
        cJSON_ArrayForEach(p, posts) {
            const char *tag = v10_str(p, "clan_name");
            const int clan_id = (int) v10_num(p, "clan_id", 0);
            sb_appendf(sb, "<div class=\"post-card svelte-72bcy3\"><div class=\"post-header svelte-72bcy3\"><div class=\"clan-info svelte-72bcy3\"><!----> "
                           "<span class=\"clan-name svelte-72bcy3\" style=\"color: %s;\" onclick=\"knTwClan('%d')\">",
                       clan_color(tag, "#ffffff"), clan_id);
            sb_append_esc(sb, tag);
            sb_append(sb, "</span></div> <div class=\"post-tags svelte-72bcy3\">");
            const char *reg = v10_str(p, "region"), *lg = v10_str(p, "language");
            if (reg[0]) {
                sb_append(sb, "<span class=\"tag tag-region svelte-72bcy3\">");
                sb_append_esc(sb, pair_label(k_regions, sizeof(k_regions) / sizeof(k_regions[0]), reg));
                sb_append(sb, "</span>");
            } else sb_append(sb, "<!---->");
            sb_append(sb, " ");
            if (lg[0]) {
                sb_append(sb, "<span class=\"tag tag-lang svelte-72bcy3\">");
                sb_append_esc(sb, pair_label(k_langs, sizeof(k_langs) / sizeof(k_langs[0]), lg));
                sb_append(sb, "</span>");
            } else sb_append(sb, "<!---->");
            sb_append(sb, " ");
            if (staff) {
                sb_appendf(sb, "<button class=\"takedown-btn svelte-72bcy3\" title=\"Take down post (mod)\" onclick=\"knTwRcTakedown(%d,'", clan_id);
                sb_append_esc(sb, tag);
                sb_append(sb, "')\">&#x2716;</button>");
            }
            else sb_append(sb, "<!---->");
            sb_append(sb, "</div></div> ");
            if (v10_str(p, "description")[0]) {
                sb_append(sb, "<p class=\"post-description svelte-72bcy3\">");
                sb_append_esc(sb, v10_str(p, "description"));
                sb_append(sb, "</p>");
            } else sb_append(sb, "<!---->");
            sb_append(sb, " ");
            if (v10_str(p, "looking_for")[0]) {
                sb_append(sb, "<div class=\"looking-for svelte-72bcy3\"><span class=\"looking-for-label svelte-72bcy3\">Looking for:</span> ");
                sb_append_esc(sb, v10_str(p, "looking_for"));
                sb_append(sb, "</div>");
            } else sb_append(sb, "<!---->");
            sb_append(sb, " ");
            const char *mode = v10_str(p, "loot_mode");
            if (mode[0]) {
                sb_append(sb, "<div class=\"loot-info svelte-72bcy3\"><span class=\"loot-label svelte-72bcy3\">Loot:</span> <span class=\"loot-value svelte-72bcy3\">");
                sb_append_esc(sb, pair_label(k_loot_modes, sizeof(k_loot_modes) / sizeof(k_loot_modes[0]), mode));
                sb_append(sb, "</span> ");
                if (strcmp(mode, "clan_bank")) {
                    const double pct = v10_num(p, "loot_clan_pct", 0), tc = v10_num(p, "loot_top_player_count", 0), mg = v10_num(p, "loot_min_games", 0);
                    if (pct > 0) sb_appendf(sb, "<span class=\"loot-detail svelte-72bcy3\">%.0f%% clan</span>", pct);
                    else sb_append(sb, "<!---->");
                    sb_append(sb, " ");
                    if (!strcmp(mode, "top_weighted") && tc > 0)
                        sb_appendf(sb, "<span class=\"loot-detail svelte-72bcy3\">top %.0f get %.0f%%</span>", tc, v10_num(p, "loot_top_player_pct", 0));
                    else sb_append(sb, "<!---->");
                    sb_append(sb, " ");
                    if (!strcmp(mode, "top_performance") && tc > 0) sb_appendf(sb, "<span class=\"loot-detail svelte-72bcy3\">top %.0f by kills</span>", tc);
                    else sb_append(sb, "<!---->");
                    sb_append(sb, " ");
                    if (mg > 1) sb_appendf(sb, "<span class=\"loot-detail svelte-72bcy3\">min %.0f games</span>", mg);
                    else sb_append(sb, "<!---->");
                } else sb_append(sb, "<!---->");
                sb_append(sb, "</div>");
            } else sb_append(sb, "<!---->");
            sb_append(sb, " <div class=\"post-footer svelte-72bcy3\">");
            const double min_level = v10_num(p, "min_level", 0), min_kd = v10_num(p, "min_kd", 0);
            if (min_level > 0 || min_kd > 0) {
                sb_append(sb, "<div class=\"requirements svelte-72bcy3\">");
                if (min_level > 0) sb_appendf(sb, "<span class=\"req-badge svelte-72bcy3\">Level %.0f+</span>", min_level);
                else sb_append(sb, "<!---->");
                sb_append(sb, " ");
                if (min_kd > 0) sb_appendf(sb, "<span class=\"req-badge svelte-72bcy3\">K/D %g+</span>", min_kd);
                else sb_append(sb, "<!---->");
                sb_append(sb, "</div>");
            } else sb_append(sb, "<!---->");
            sb_append(sb, " ");
            if (no_clan) {
                int applied = 0;
                for (int i = 0; i < g_applied_count; i++) applied |= g_applied_ids[i] == clan_id;
                if (applied) sb_append(sb, "<button class=\"main-button svelte-whnh40 main-button-accept main-button-disabled\" type=\"button\" disabled>Applied</button>");
                else if (min_level > 0 && a->level < min_level)
                    sb_append(sb, "<button class=\"main-button svelte-whnh40 main-button-accept main-button-disabled\" type=\"button\" disabled>Requirements Not Met</button>");
                else
                    sb_appendf(sb, "<button class=\"main-button svelte-whnh40 main-button-accept%s\" type=\"button\"%s onclick=\"playSelect();knTwRcApply(%d)\">%s</button>",
                               T.rc_applying == clan_id ? " main-button-disabled" : "", T.rc_applying == clan_id ? " disabled" : "", clan_id,
                               T.rc_applying == clan_id ? "Applying..." : "Apply");
            } else sb_append(sb, "<!---->");
            sb_append(sb, "</div></div>");
        }
        sb_append(sb, "</div> ");
        const int total = (int) v10_num(d, "total", 0);
        if (total > 20) {
            const int pages = (total + 19) / 20;
            sb_appendf(sb, "<div class=\"pagination svelte-72bcy3\"><button class=\"main-button svelte-whnh40 main-button-accept-secondary%s\" type=\"button\"%s onclick=\"knTwRcPage(-1)\">"
                           "Prev</button> <span class=\"page-info svelte-72bcy3\">Page %d of %d</span> <button class=\"main-button svelte-whnh40 main-button-accept-secondary%s\" "
                           "type=\"button\"%s onclick=\"knTwRcPage(1)\">Next</button></div>",
                       T.rc_page <= 1 ? " main-button-disabled" : "", T.rc_page <= 1 ? " disabled" : "", T.rc_page, pages, 20 * T.rc_page >= total ? " main-button-disabled" : "",
                       20 * T.rc_page >= total ? " disabled" : "");
        } else sb_append(sb, "<!---->");
    }
    sb_append(sb, "</div>");
}

// ------------------------------------------------------------------ How to Play (svelte-1sr7h97)

// an emoji as its image HTML (v10_emoji) in a fresh string the caller frees
static char *emoji_html(const char *utf8) {
    StrBuf b = {0};
    v10_emoji(&b, utf8, "");
    return sb_finish(&b);
}

static void gen_howto(StrBuf *sb) {
    char split[64];
    snprintf(split, sizeof(split), "%d%% / %d%% / %d%%", (int) lround(g_turf_split[0] * 100), (int) lround(g_turf_split[1] * 100), (int) lround(g_turf_split[2] * 100));
    /* the Pro Tips icons sit inside the format below, so their image HTML is built first */
    char *tip_people = emoji_html("\xf0\x9f\x91\xa5"), *tip_chart = emoji_html("\xf0\x9f\x93\x8a");
    char *tip_pad = emoji_html("\xf0\x9f\x8e\xae"), *tip_sword = emoji_html("\xe2\x9a\x94\xef\xb8\x8f");
    sb_append(sb, "<div class=\"how-to-play-tab svelte-1sr7h97\"><div class=\"content-container svelte-1sr7h97\"><div class=\"header-section svelte-1sr7h97\">"
                  "<h1 class=\"main-title svelte-1sr7h97\">How to Play Turf Wars</h1> <p class=\"subtitle svelte-1sr7h97\">Compete with your clan to control territories and earn rewards!</p></div> "
                  "<div class=\"info-cards svelte-1sr7h97\"><div class=\"info-card svelte-1sr7h97\"><div class=\"card-icon svelte-1sr7h97\">");
    v10_emoji(sb, "\xf0\x9f\x8e\xaf", "");
    sb_append(sb, "</div> "
                  "<h2 class=\"card-title svelte-1sr7h97\">Earn Kills</h2> <p class=\"card-description svelte-1sr7h97\">Play on eligible maps in <strong class=\"svelte-1sr7h97\">Free for All</strong> "
                  "or <strong class=\"svelte-1sr7h97\">Team Deathmatch</strong> modes on official servers. Every kill you earn counts towards your clan's total!</p> "
                  "<div class=\"card-details svelte-1sr7h97\"><div class=\"detail-item svelte-1sr7h97\"><span class=\"detail-label svelte-1sr7h97\">Eligible Maps:</span> "
                  "<div class=\"detail-value svelte-1sr7h97\"><span class=\"map-badge svelte-1sr7h97\">Burg</span><span class=\"map-badge svelte-1sr7h97\">Sandstorm</span>"
                  "<span class=\"map-badge svelte-1sr7h97\">Subzero</span></div></div> <div class=\"detail-item svelte-1sr7h97\"><span class=\"detail-label svelte-1sr7h97\">Eligible Modes:</span> "
                  "<div class=\"detail-value svelte-1sr7h97\"><span class=\"mode-badge svelte-1sr7h97\">Free for All</span><span class=\"mode-badge svelte-1sr7h97\">Team Deathmatch</span>"
                  "</div></div> <div class=\"detail-note svelte-1sr7h97\"><span class=\"note-icon svelte-1sr7h97\">");
    v10_emoji(sb, "\xe2\x9a\xa0\xef\xb8\x8f", "");
    sb_append(sb, "</span> <span class=\"svelte-1sr7h97\">"
                  "Kills only count if you're playing against players from different clans!</span></div></div></div> <div class=\"info-card svelte-1sr7h97\">"
                  "<div class=\"card-icon svelte-1sr7h97\">\xf0\x9f\x8f\x86</div> <h2 class=\"card-title svelte-1sr7h97\">Control Territories</h2> <p class=\"card-description svelte-1sr7h97\">"
                  "At the end of each day, the clan with the most kills on each map wins control of that territory for the next\n          day.</p> <div class=\"card-details svelte-1sr7h97\">"
                  "<div class=\"detail-item svelte-1sr7h97\"><span class=\"detail-label svelte-1sr7h97\">Daily Reset:</span> <span class=\"detail-value svelte-1sr7h97\">Leaderboards reset every "
                  "day at midnight UTC</span></div> <div class=\"detail-item svelte-1sr7h97\"><span class=\"detail-label svelte-1sr7h97\">Victory Condition:</span> "
                  "<span class=\"detail-value svelte-1sr7h97\">Highest total kills across all clan members</span></div></div></div> <div class=\"info-card svelte-1sr7h97\">"
                  "<div class=\"card-icon svelte-1sr7h97\">");
    v10_emoji(sb, "\xf0\x9f\x92\xb0", "");
    sb_append(sb, "</div> <h2 class=\"card-title svelte-1sr7h97\">Earn Rewards</h2> <p class=\"card-description svelte-1sr7h97\">At the end "
                  "of each day, all kills made on a map are converted into a KR pool. The <strong class=\"svelte-1sr7h97\">");
    sb_appendf(sb, "top %d clans</strong> on that map split the pool \xe2\x80\x94 you don't have to win the territory to get paid!</p> <div class=\"card-details svelte-1sr7h97\">"
                   "<div class=\"detail-item svelte-1sr7h97\"><span class=\"detail-label svelte-1sr7h97\">Reward Split:</span> <div class=\"detail-value svelte-1sr7h97\">",
               TURF_SPLIT_COUNT);
    static const char *const places[] = {"1st", "2nd", "3rd", "4th", "5th"};
    for (int i = 0; i < TURF_SPLIT_COUNT; i++)
        sb_appendf(sb, "<span class=\"map-badge svelte-1sr7h97\">%s: %d%%</span>", places[i], (int) lround(g_turf_split[i] * 100));
    sb_appendf(sb, "</div></div> <div class=\"detail-item svelte-1sr7h97\"><span class=\"detail-label svelte-1sr7h97\">Distribution:</span> <span class=\"detail-value svelte-1sr7h97\">"
                   "Each clan shares its cut among members (set in Loot settings)</span></div></div></div></div> <div class=\"rules-section svelte-1sr7h97\">"
                   "<h2 class=\"section-title svelte-1sr7h97\">Important Rules</h2> <ul class=\"rules-list svelte-1sr7h97\"><li class=\"svelte-1sr7h97\"><span class=\"rule-icon svelte-1sr7h97\">"
                   "\xe2\x9c\x93</span> <span class=\"svelte-1sr7h97\">Only the top 20 contributors (by kills) per map per clan count towards wars</span></li> <li class=\"svelte-1sr7h97\">"
                   "<span class=\"rule-icon svelte-1sr7h97\">\xe2\x9c\x93</span> <span class=\"svelte-1sr7h97\">Only kills on eligible maps (Burg, Sandstorm, Subzero) count towards turf wars</span>"
                   "</li> <li class=\"svelte-1sr7h97\"><span class=\"rule-icon svelte-1sr7h97\">\xe2\x9c\x93</span> <span class=\"svelte-1sr7h97\">Only Free for All and Team Deathmatch game modes "
                   "are eligible</span></li> <li class=\"svelte-1sr7h97\"><span class=\"rule-icon svelte-1sr7h97\">\xe2\x9c\x93</span> <span class=\"svelte-1sr7h97\">Kills against players from "
                   "your own clan do not count</span></li> <li class=\"svelte-1sr7h97\"><span class=\"rule-icon svelte-1sr7h97\">\xe2\x9c\x93</span> <span class=\"svelte-1sr7h97\">You must be "
                   "playing on official servers for kills to count</span></li> <li class=\"svelte-1sr7h97\"><span class=\"rule-icon svelte-1sr7h97\">\xe2\x9c\x93</span> "
                   "<span class=\"svelte-1sr7h97\">Territory control is determined daily based on total clan kills</span></li> <li class=\"svelte-1sr7h97\"><span class=\"rule-icon svelte-1sr7h97\">"
                   "\xe2\x9c\x93</span> <span class=\"svelte-1sr7h97\">The top %d clans on each map split the daily KR pool (%s)</span></li></ul></div> <div class=\"tips-section svelte-1sr7h97\">"
                   "<h2 class=\"section-title svelte-1sr7h97\">Pro Tips</h2> <div class=\"tips-grid svelte-1sr7h97\"><div class=\"tip-card svelte-1sr7h97\"><div class=\"tip-icon svelte-1sr7h97\">%s</div> "
                   "<h3 class=\"tip-title svelte-1sr7h97\">Coordinate with Your Clan</h3> <p class=\"tip-text svelte-1sr7h97\">Work together with your clan members to "
                   "maximize kills on target maps!</p></div> <div class=\"tip-card svelte-1sr7h97\"><div class=\"tip-icon svelte-1sr7h97\">%s</div> <h3 class=\"tip-title svelte-1sr7h97\">"
                   "Track Your Progress</h3> <p class=\"tip-text svelte-1sr7h97\">Check the leaderboards regularly to see how your clan is performing!</p></div> <div class=\"tip-card svelte-1sr7h97\">"
                   "<div class=\"tip-icon svelte-1sr7h97\">%s</div> <h3 class=\"tip-title svelte-1sr7h97\">Focus on Eligible Maps</h3> <p class=\"tip-text svelte-1sr7h97\">Make sure "
                   "you're playing on Burg, Sandstorm, or Subzero to earn points!</p></div> <div class=\"tip-card svelte-1sr7h97\"><div class=\"tip-icon svelte-1sr7h97\">%s</div> "
                   "<h3 class=\"tip-title svelte-1sr7h97\">Defend Your Territories</h3> <p class=\"tip-text svelte-1sr7h97\">If your clan controls a map, keep playing to maintain control and "
                   "earn more rewards!</p></div></div></div></div></div>",
               TURF_SPLIT_COUNT, split, tip_people, tip_chart, tip_pad, tip_sword);
    free(tip_people);
    free(tip_chart);
    free(tip_pad);
    free(tip_sword);
}

// ------------------------------------------------------------------ player view (svelte-7h12t9)

static void gen_player_view(StrBuf *sb) {
    char id[128], path[256];
    enc(id, sizeof(id), T.player);
    snprintf(path, sizeof(path), "/api/territorial-wars/player/%s", id);
    const V10Req *pr = v10_get(path, TTL_MS);
    if (T.pv_c_day >= 0) snprintf(path, sizeof(path), "/api/territorial-wars/player/%s/contributions?day=%d", id, T.pv_c_day);
    else snprintf(path, sizeof(path), "/api/territorial-wars/player/%s/contributions", id);
    const V10Req *cr = v10_get(path, TTL_MS);
    snprintf(path, sizeof(path), "/api/territorial-wars/player/%s/earnings", id);
    const V10Req *er = v10_get(path, TTL_MS);
    const cJSON *p = data_of(pr);
    sb_append(sb, "<div class=\"tw-player-profile svelte-7h12t9\">");
    if (pr->status == 0 && !p) {
        sb_append(sb, "<div class=\"loading svelte-7h12t9\">Loading player profile...</div></div>");
        return;
    }
    if (!cJSON_IsObject(p)) {
        error_box(sb, "7h12t9", pr, "Failed to load player profile");
        sb_append(sb, "</div>");
        return;
    }
    const cJSON *contrib = data_of(cr), *earn = data_of(er);
    const int now = day_now();
    sb_append(sb, "<div class=\"profile-header svelte-7h12t9\" style=\"line-height:2\">");
    v10_back_button(sb, "knTwBack()");
    sb_append(sb, " <h2 class=\"player-name svelte-7h12t9\">");
    sb_append_esc(sb, v10_str(p, "player_name"));
    sb_append(sb, "</h2> ");
    const char *clan = v10_str(p, "current_clan");
    if (clan[0]) {
        const int has_id = cJSON_IsNumber(cJSON_GetObjectItem(p, "current_clan_id"));
        sb_appendf(sb, "<!----> <span class=\"player-clan svelte-7h12t9%s\"", has_id ? " clickable-clan" : "");
        if (has_id) sb_appendf(sb, " onclick=\"knTwClan('%.0f')\"", v10_num(p, "current_clan_id", 0));
        sb_append(sb, ">");
        sb_append_esc(sb, clan);
        sb_append(sb, "</span>");
    } else sb_append(sb, "<span class=\"player-clan no-clan svelte-7h12t9\">No Clan</span>");
    char k[32], d[32], g[32], kr[32];
    v10_fmt_int(k, sizeof(k), v10_num(p, "total_kills", 0));
    v10_fmt_int(d, sizeof(d), v10_num(p, "total_deaths", 0));
    v10_fmt_int(g, sizeof(g), v10_num(p, "total_games", 0));
    v10_fmt_int(kr, sizeof(kr), v10_num(p, "total_kr", 0));
    const int nc = cJSON_GetArraySize(contrib);
    // earnings filtered by day (client side, like v10)
    cJSON *earn_f = cJSON_CreateArray();
    const cJSON *e;
    cJSON_ArrayForEach(e, earn) if (T.pv_e_day < 0 || (int) v10_num(e, "day", -1) == T.pv_e_day) cJSON_AddItemToArray(earn_f, cJSON_Duplicate(e, 1));
    const int ne = cJSON_GetArraySize(earn), nef = cJSON_GetArraySize(earn_f);
    sb_appendf(sb, "</div> <div class=\"stats-cards svelte-7h12t9\"><div class=\"stat-card svelte-7h12t9\"><span class=\"stat-value svelte-7h12t9\">%s</span> <span class=\"stat-label svelte-7h12t9\">"
                   "Kills</span></div> <div class=\"stat-card svelte-7h12t9\"><span class=\"stat-value svelte-7h12t9\">%s</span> <span class=\"stat-label svelte-7h12t9\">Deaths</span></div> "
                   "<div class=\"stat-card svelte-7h12t9\"><span class=\"stat-value svelte-7h12t9\">%.2f</span> <span class=\"stat-label svelte-7h12t9\">K/D</span></div> "
                   "<div class=\"stat-card svelte-7h12t9\"><span class=\"stat-value svelte-7h12t9\">%s</span> <span class=\"stat-label svelte-7h12t9\">Games</span></div> "
                   "<div class=\"stat-card kr svelte-7h12t9\"><span class=\"stat-value svelte-7h12t9\">%s</span> <span class=\"stat-label svelte-7h12t9\">KR Earned</span></div></div> "
                   "<div class=\"tab-bar svelte-7h12t9\"><button class=\"tab svelte-7h12t9%s\" onclick=\"knTwPvTab(0)\">Contributions (%d)</button> "
                   "<button class=\"tab svelte-7h12t9%s\" onclick=\"knTwPvTab(1)\">Earnings (%d)</button></div> ",
               k, d, v10_num(p, "kd", 0), g, kr, T.pv_tab == 0 ? " active" : "", nc, T.pv_tab == 1 ? " active" : "", ne);
    const int sel = T.pv_tab == 0 ? T.pv_c_day : T.pv_e_day;
    char value[16] = "", maxd[16];
    if (sel >= 0) day_date(value, sizeof(value), sel);
    day_date(maxd, sizeof(maxd), now);
    const char *arrows = T.pv_tab == 0 ? "knTwPvCDay" : "knTwPvEDay", *input = T.pv_tab == 0 ? "knTwPvCDate" : "knTwPvEDate";
    sb_appendf(sb, "<div class=\"filters svelte-7h12t9\"><div class=\"day-selector svelte-7h12t9\"><button class=\"day-arrow svelte-7h12t9\"%s onclick=\"%s(-1)\">&#9664;</button> "
                   "<input type=\"date\" class=\"day-input svelte-7h12t9\" value=\"%s\" min=\"2026-03-17\" max=\"%s\" onchange=\"%s(this.value)\"> "
                   "<button class=\"day-arrow svelte-7h12t9\"%s onclick=\"%s(1)\">&#9654;</button> ",
               sel >= 0 && sel <= 0 ? " disabled" : "", arrows, value, maxd, input, sel < 0 || sel >= now ? " disabled" : "", arrows);
    if (sel >= 0) sb_appendf(sb, "<button class=\"clear-btn svelte-7h12t9\" onclick=\"%s('')\">&times;</button>", input);
    else sb_append(sb, "<!---->");
    sb_append(sb, "</div></div> ");
    if (T.pv_tab == 0) {
        if (cr->status == 0 && !contrib) sb_append(sb, "<div class=\"no-data svelte-7h12t9\">Loading...</div>");
        else if (nc > 0) {
            sb_append(sb, "<div class=\"custom-table svelte-7h12t9\"><div class=\"ct-row ct-header svelte-7h12t9\"><span class=\"ct-cell svelte-7h12t9\" style=\"width:120px\">Date</span> "
                          "<span class=\"ct-cell ct-flex svelte-7h12t9\">Clan</span> <span class=\"ct-cell svelte-7h12t9\">Map</span> <span class=\"ct-cell svelte-7h12t9\" style=\"width:60px\">"
                          "Kills</span> <span class=\"ct-cell svelte-7h12t9\" style=\"width:60px\">Deaths</span> <span class=\"ct-cell svelte-7h12t9\" style=\"width:60px\">K/D</span> "
                          "<span class=\"ct-cell svelte-7h12t9\" style=\"width:60px\">Games</span></div> ");
            int i = 0;
            cJSON_ArrayForEach(e, contrib) {
                if (i >= 7 * T.pv_c_page && i < 7 * (T.pv_c_page + 1)) {
                    char date[16];
                    day_date(date, sizeof(date), (int) v10_num(e, "day", 0));
                    sb_appendf(sb, "<div class=\"ct-row svelte-7h12t9\"><span class=\"ct-cell svelte-7h12t9\" style=\"width:120px\">%s</span> <span class=\"ct-cell ct-flex clickable-name "
                                   "svelte-7h12t9\" onclick=\"knTwClan('%.0f')\">",
                               date, v10_num(e, "clan_id", 0));
                    sb_append_esc(sb, v10_str(e, "clan_name"));
                    sb_append(sb, "</span> <span class=\"ct-cell svelte-7h12t9\">");
                    sb_append_esc(sb, v10_str(e, "map_name"));
                    sb_appendf(sb, "</span> <span class=\"ct-cell svelte-7h12t9\" style=\"width:60px\">%.0f</span> <span class=\"ct-cell svelte-7h12t9\" style=\"width:60px\">%.0f</span> "
                                   "<span class=\"ct-cell svelte-7h12t9\" style=\"width:60px\">%.2f</span> <span class=\"ct-cell svelte-7h12t9\" style=\"width:60px\">%.0f</span></div>",
                               v10_num(e, "kills", 0), v10_num(e, "deaths", 0), v10_num(e, "kd", 0), v10_num(e, "games", 0));
                }
                i++;
            }
            sb_append(sb, "</div> ");
            if (nc > 7) pagination(sb, "7h12t9", "knTwPvCPage", T.pv_c_page, 7, nc);
            else sb_append(sb, "<!---->");
        } else sb_appendf(sb, "<div class=\"no-data svelte-7h12t9\">No contributions%s recorded.</div>", T.pv_c_day >= 0 ? " for this date" : "");
    } else {
        if (nef > 0) {
            sb_append(sb, "<div class=\"custom-table svelte-7h12t9\"><div class=\"ct-row ct-header svelte-7h12t9\"><span class=\"ct-cell svelte-7h12t9\" style=\"width:120px\">Date</span> "
                          "<span class=\"ct-cell ct-flex svelte-7h12t9\">Clan</span> <span class=\"ct-cell svelte-7h12t9\">Map</span> <span class=\"ct-cell svelte-7h12t9\" style=\"width:100px\">"
                          "KR Earned</span></div> ");
            int i = 0;
            cJSON_ArrayForEach(e, earn_f) {
                if (i >= 7 * T.pv_e_page && i < 7 * (T.pv_e_page + 1)) {
                    char date[16], amt[32];
                    day_date(date, sizeof(date), (int) v10_num(e, "day", 0));
                    v10_fmt_int(amt, sizeof(amt), v10_num(e, "amount", 0));
                    sb_appendf(sb, "<div class=\"ct-row svelte-7h12t9\"><span class=\"ct-cell svelte-7h12t9\" style=\"width:120px\">%s</span> <span class=\"ct-cell ct-flex clickable-name "
                                   "svelte-7h12t9\" onclick=\"knTwClan('%.0f')\">",
                               date, v10_num(e, "clan_id", 0));
                    sb_append_esc(sb, v10_str(e, "clan_name"));
                    sb_append(sb, "</span> <span class=\"ct-cell svelte-7h12t9\">");
                    sb_append_esc(sb, v10_str(e, "map_name"));
                    sb_appendf(sb, "</span> <span class=\"ct-cell svelte-7h12t9\" style=\"width:100px\">%s KR</span></div>", amt);
                }
                i++;
            }
            sb_append(sb, "</div> ");
            if (nef > 7) pagination(sb, "7h12t9", "knTwPvEPage", T.pv_e_page, 7, nef);
            else sb_append(sb, "<!---->");
        } else sb_appendf(sb, "<div class=\"no-data svelte-7h12t9\">No earnings%s recorded.</div>", T.pv_e_day >= 0 ? " matching filter" : "");
    }
    cJSON_Delete(earn_f);
    sb_append(sb, "</div>");
}

// ------------------------------------------------------------------ the popup

static char *gen_popup(void) {
    StrBuf sb = {0};
    sb_appendf(&sb, "<div class=\"window-container svelte-623zut\"><div class=\"io-popup-content svelte-nys8k9\"><div class=\"tabs-view svelte-nys8k9%s\">",
               T.view != VIEW_TABS ? " hidden" : "");
    v10_tabs(&sb, k_tabs, TW_TABS, T.tab, "knTwTab");
    sb_append(&sb, " ");
    if (T.view == VIEW_TABS) {
        switch (T.tab) {
            case TW_CLAN: gen_my_clan(&sb); break;
            case TW_BOARDS: gen_boards(&sb); break;
            case TW_RECRUIT: gen_recruit(&sb); break;
            case TW_HOWTO: gen_howto(&sb); break;
            default: gen_overview(&sb); break;
        }
    }
    sb_append(&sb, "</div> ");
    if (T.view == VIEW_CLAN) {
        sb_append(&sb, "<div class=\"clan-view svelte-nys8k9\">");
        clan_stats(&sb, T.clan, 0);
        sb_append(&sb, "</div>");
    } else sb_append(&sb, "<!---->");
    sb_append(&sb, " ");
    if (T.view == VIEW_PLAYER) {
        sb_append(&sb, "<div class=\"player-view svelte-nys8k9\">");
        gen_player_view(&sb);
        sb_append(&sb, "</div>");
    } else sb_append(&sb, "<!---->");
    sb_append(&sb, "</div><!----></div>");
    return sb_finish(&sb);
}

static void render(void) {
    if (!v10_popup_open(POPUP_CLASS)) return;
    char *html = gen_popup();
    v10_popup_set(POPUP_CLASS, html);
    free(html);
}

static int is_turf_open(void) {
    return v10_popup_open(POPUP_CLASS) && html_dom_query(html_dom_by_id("genericPop"), ".svelte-nys8k9") != 0;
}

static void open_popup(void) {
    T.tab = TW_OVERVIEW;
    T.view = VIEW_TABS;
    T.hover = T.locked = -1;
    T.board = -1;
    v10_forget("/api/territorial-wars/");
    char *html = gen_popup();
    v10_popup_show(POPUP_CLASS, html, render);
    free(html);
}

// ------------------------------------------------------------------ handlers

static void hover_clear(void *user) {
    (void) user;
    T.hover_timer = 0;
    if (T.hover != -1) {
        T.hover = -1;
        render();
    }
}

static int arg_int(const HtmlArg *args, const int argc, const int i, const int def) {
    if (i >= argc) return def;
    if (args[i].kind == HTML_ARG_NUM || args[i].kind == HTML_ARG_BOOL) return (int) args[i].num;
    if (args[i].kind == HTML_ARG_STR && args[i].str) return args[i].str[0] ? atoi(args[i].str) : def;
    return def;
}

static void arg_str(const HtmlEvent *ev, const HtmlArg *args, const int argc, const int i, char *out, const size_t n) {
    out[0] = 0;
    if (i >= argc) return;
    if (args[i].kind == HTML_ARG_STR && args[i].str) snprintf(out, n, "%s", args[i].str);
    else if (args[i].kind == HTML_ARG_NUM) snprintf(out, n, "%.0f", args[i].num);
    else if (ev && ev->value && ev->value[0]) snprintf(out, n, "%s", ev->value);
    else if (ev && ev->target) html_dom_get_value(ev->target, out, n);
}

static void input_value(const char *id, char *out, const size_t n) {
    out[0] = 0;
    HtmlElem el = html_dom_by_id(id);
    if (el) html_dom_get_value(el, out, n);
}

static void on_apply(void *user, const int status, const cJSON *json) {
    (void) user;
    T.rc_applying = 0;
    if (status == 200) {
        v10_forget("/api/clans/my-applications");
        T.rc_error[0] = 0;
    } else {
        const char *e = v10_str(json, "message");
        if (!e[0]) e = v10_str(json, "error");
        snprintf(T.rc_error, sizeof(T.rc_error), "%s", e[0] ? e : "Failed to apply");
    }
    render();
}

static void takedown_closed(const int accepted, void *user) {
    const int clan = (int) (intptr_t) user;
    if (!accepted) return;
    char t[256], path[400];
    v10_url_token(t, sizeof(t));
    snprintf(path, sizeof(path), "/api/clans/%d/recruitment?token=%s", clan, t);
    v10_send("DELETE", path, NULL, NULL, NULL);
    v10_forget("/api/clans/recruitment");
    render();
}

// _34410 confirm dialog (_6696 io-popup): Take Down Post
static void confirm_takedown(const int clan, const char *tag) {
    StrBuf sb = {0};
    sb_append(&sb, "<div class=\"popup-container svelte-1wz7ymp\"><div class=\"popup svelte-1wz7ymp\" style=\"width: 450px;\"><div class=\"io-popup-container\">"
                   "<div class=\"io-popup-header io-popup-header--slim\">Take Down Post</div> <div class=\"io-popup-content\"><p>Take down recruitment post from clan \"");
    sb_append_esc(&sb, tag);
    sb_append(&sb, "\"?</p> <div class=\"io-button-group\"><button type=\"button\" class=\"io-button\" onclick=\"knViewPop(0)\">Cancel</button> "
                   "<button type=\"button\" class=\"io-button io-button--accept\" onclick=\"knViewPop(1)\">Take Down</button></div></div></div></div></div>");
    v10_view_push(sb_finish(&sb), NULL, takedown_closed, (void *) (intptr_t) clan);
}

static void day_step(ClanFilters *f, const int step, const int current) {
    const int cur = f->day >= 0 ? f->day : current;
    int d = cur + step;
    if (d < 0) d = 0;
    if (d > current) d = current;
    f->day = d == current ? -1 : d;
}

static int clan_current_day(const char *clan) {
    const cJSON *c = data_of(get_clan(clan));
    return (int) v10_num(c, "current_day", day_now());
}

int turf_ui_handle(const char *name, const HtmlEvent *ev, const HtmlArg *args, const int argc) {
    if (!name) return -1;
    if (!strcmp(name, "openTurfWars")) {
        open_popup();
        return 1;
    }
    if (strncmp(name, "knTw", 4)) return -1;
    const int a0 = arg_int(args, argc, 0, 0);
    char s[64];
    const AccountInfo *a = account_get();
    const char *mine = a ? a->clan : "";
    if (!strcmp(name, "knTwTab")) {
        if (a0 >= 0 && a0 < TW_TABS) T.tab = a0;
        T.view = VIEW_TABS;
    } else if (!strcmp(name, "knTwHover")) { // the territory hit cells
        if (a0 >= 0) {
            if (T.hover_timer) html_timer_cancel(T.hover_timer);
            T.hover_timer = 0;
            if (T.hover == a0) return 1;
            T.hover = a0;
        } else {
            if (!T.hover_timer) T.hover_timer = html_timer_add(100, hover_clear, NULL);
            return 1;
        }
    } else if (!strcmp(name, "knTwPanel")) { // the details panel keeps the hover alive
        if (a0) {
            if (T.hover_timer) html_timer_cancel(T.hover_timer);
            T.hover_timer = 0;
            return 1;
        }
        if (T.locked < 0 && T.hover >= 0) T.hover = -1;
        else return 1;
    } else if (!strcmp(name, "knTwLock")) {
        html_ui_stop_propagation();
        T.locked = T.locked == a0 ? -1 : a0;
        html_ui_dispatch("playTick", ev, NULL, 0);
    } else if (!strcmp(name, "knTwUnlock")) {
        T.locked = -1;
    } else if (!strcmp(name, "knTwClose")) {
        T.locked = T.hover = -1;
    } else if (!strcmp(name, "knTwClan")) {
        html_ui_stop_propagation();
        arg_str(ev, args, argc, 0, s, sizeof(s));
        if (!s[0] || !strcmp(s, "0")) return 1;
        snprintf(T.clan, sizeof(T.clan), "%s", s);
        T.view = VIEW_CLAN;
        memset(&T.other, 0, sizeof(T.other));
        T.other.day = T.other.map = T.other.pay_day = -1;
    } else if (!strcmp(name, "knTwPlayer")) {
        html_ui_stop_propagation();
        arg_str(ev, args, argc, 0, s, sizeof(s));
        if (!s[0] || !strcmp(s, "0")) return 1;
        snprintf(T.player, sizeof(T.player), "%s", s);
        T.view = VIEW_PLAYER;
        T.pv_tab = 0;
        T.pv_c_day = T.pv_e_day = -1;
        T.pv_c_page = T.pv_e_page = 0;
    } else if (!strcmp(name, "knTwBack")) {
        T.view = VIEW_TABS;
    } else if (!strcmp(name, "knTwSort")) {
        arg_str(ev, args, argc, 0, s, sizeof(s));
        sort_click(s, arg_int(args, argc, 1, 0));
    } else if (!strcmp(name, "knTwDay")) {
        day_step(&T.mine, a0, clan_current_day(mine));
    } else if (!strcmp(name, "knTwDate") || !strcmp(name, "knTwCvDate")) {
        ClanFilters *f = name[5] == 'C' ? &T.other : &T.mine;
        arg_str(ev, args, argc, 0, s, sizeof(s));
        const int current = clan_current_day(f == &T.mine ? mine : T.clan), d = date_day(s);
        if (d >= 0) f->day = d >= current ? -1 : d;
    } else if (!strcmp(name, "knTwCvDay")) {
        day_step(&T.other, a0, clan_current_day(T.clan));
    } else if (!strcmp(name, "knTwMap") || !strcmp(name, "knTwCvMap")) {
        ClanFilters *f = name[5] == 'C' ? &T.other : &T.mine;
        arg_str(ev, args, argc, 0, s, sizeof(s));
        f->map = s[0] ? atoi(s) : -1;
        f->players_page = 0;
    } else if (!strcmp(name, "knTwViewAll")) {
        T.mine.view_all = 1;
    } else if (!strcmp(name, "knTwEarnPage")) {
        T.mine.earn_page += a0;
        if (T.mine.earn_page < 0) T.mine.earn_page = 0;
    } else if (!strcmp(name, "knTwCvEarnPage")) {
        T.other.earn_page += a0;
        if (T.other.earn_page < 0) T.other.earn_page = 0;
    } else if (!strcmp(name, "knTwCvPlayersPage")) {
        T.other.players_page += a0;
        if (T.other.players_page < 0) T.other.players_page = 0;
    } else if (!strcmp(name, "knTwPayView") || !strcmp(name, "knTwPayDay") || !strcmp(name, "knTwPayDate")) {
        arg_str(ev, args, argc, 0, s, sizeof(s));
        ClanFilters *f = !strcmp(s, "other") ? &T.other : &T.mine;
        const int current = clan_current_day(f == &T.mine ? mine : T.clan);
        if (!strcmp(name, "knTwPayView")) {
            f->pay_daily = arg_int(args, argc, 1, 0) != 0;
            if (!f->pay_daily) f->pay_day = -1;
        } else if (!strcmp(name, "knTwPayDay")) {
            const int cur = f->pay_day >= 0 ? f->pay_day : current;
            int d = cur + arg_int(args, argc, 1, 0);
            f->pay_day = d < 0 ? 0 : d > current ? current : d;
        } else {
            char v[24];
            arg_str(ev, args, argc, 1, v, sizeof(v));
            const int d = date_day(v);
            if (d >= 0) f->pay_day = d > current ? current : d;
        }
    } else if (!strcmp(name, "knTwBoard")) {
        T.board = a0 >= -1 && a0 <= 2 ? a0 : -1;
        T.f_date[0] = T.f_name[0] = 0;
        T.f_map = -1;
    } else if (!strcmp(name, "knTwApply")) { // Apply Filters: read the filter inputs
        char v[64];
        input_value("date-filter", v, sizeof(v));
        snprintf(T.f_date, sizeof(T.f_date), "%s", v);
        input_value("map-filter", v, sizeof(v));
        T.f_map = v[0] ? atoi(v) : -1;
        input_value(T.board == 2 ? "player-name-filter" : "clan-name-filter", v, sizeof(v));
        // trim()
        char *b = v;
        while (*b == ' ') b++;
        size_t l = strlen(b);
        while (l && b[l - 1] == ' ') b[--l] = 0;
        snprintf(T.f_name, sizeof(T.f_name), "%s", b);
        v10_forget("/api/territorial-wars/leaderboards/");
    } else if (!strcmp(name, "knTwRcRegion") || !strcmp(name, "knTwRcLang")) {
        arg_str(ev, args, argc, 0, s, sizeof(s));
        if (name[6] == 'R') snprintf(T.rc_region, sizeof(T.rc_region), "%s", s);
        else snprintf(T.rc_lang, sizeof(T.rc_lang), "%s", s);
        T.rc_page = 1;
        T.rc_error[0] = 0;
    } else if (!strcmp(name, "knTwRcPage")) {
        T.rc_page += a0;
        if (T.rc_page < 1) T.rc_page = 1;
    } else if (!strcmp(name, "knTwRcApply")) {
        if (!a || T.rc_applying) return 1;
        char t[256], path[400];
        v10_url_token(t, sizeof(t));
        snprintf(path, sizeof(path), "/api/clans/%d/apply?token=%s", a0, t);
        T.rc_applying = a0;
        T.rc_error[0] = 0;
        v10_send("POST", path, NULL, on_apply, NULL);
    } else if (!strcmp(name, "knTwRcTakedown")) {
        char tag[48];
        arg_str(ev, args, argc, 1, tag, sizeof(tag));
        confirm_takedown(a0, tag);
        return 1;
    } else if (!strcmp(name, "knTwPvTab")) {
        T.pv_tab = a0 ? 1 : 0;
    } else if (!strcmp(name, "knTwPvCDay") || !strcmp(name, "knTwPvEDay")) {
        int *d = name[6] == 'C' ? &T.pv_c_day : &T.pv_e_day;
        const int now = day_now(), cur = *d >= 0 ? *d : now;
        int v = cur + a0;
        *d = v < 0 ? 0 : v > now ? now : v;
        if (name[6] == 'C') T.pv_c_page = 0;
        else T.pv_e_page = 0;
    } else if (!strcmp(name, "knTwPvCDate") || !strcmp(name, "knTwPvEDate")) {
        arg_str(ev, args, argc, 0, s, sizeof(s));
        int *d = name[6] == 'C' ? &T.pv_c_day : &T.pv_e_day;
        const int v = s[0] ? date_day(s) : -1;
        *d = v > day_now() ? day_now() : v;
        if (name[6] == 'C') T.pv_c_page = 0;
        else T.pv_e_page = 0;
    } else if (!strcmp(name, "knTwPvCPage")) {
        T.pv_c_page += a0;
        if (T.pv_c_page < 0) T.pv_c_page = 0;
    } else if (!strcmp(name, "knTwPvEPage")) {
        T.pv_e_page += a0;
        if (T.pv_e_page < 0) T.pv_e_page = 0;
    } else {
        return -1;
    }
    if (is_turf_open()) render();
    return 1;
}

// --render-scenario: state.turf {open, tab, view: "clan" | "player", id, board, locked}; the brain's
// answers come from state.v10responses (v10_competitive.c)
void turf_ui_scenario(const cJSON *state) {
    if (!cJSON_IsObject(state)) return;
    T.scenario = 1;
    if (cJSON_IsFalse(cJSON_GetObjectItem(state, "open"))) return;
    open_popup();
    T.tab = (int) v10_num(state, "tab", 0);
    if (T.tab < 0 || T.tab >= TW_TABS) T.tab = 0;
    T.board = (int) v10_num(state, "board", -1);
    T.locked = (int) v10_num(state, "locked", -1);
    const char *view = v10_str(state, "view");
    if (!strcmp(view, "clan")) {
        T.view = VIEW_CLAN;
        snprintf(T.clan, sizeof(T.clan), "%s", v10_str(state, "id"));
        T.other.day = T.other.map = T.other.pay_day = -1;
    } else if (!strcmp(view, "player")) {
        T.view = VIEW_PLAYER;
        snprintf(T.player, sizeof(T.player), "%s", v10_str(state, "id"));
    }
    render();
}
