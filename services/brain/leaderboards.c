/*
 * services/brain/leaderboards.c - the social page's "leaders" page (Krunker
 * v3.1.1 config.leaderQueries / leaderCal, src/social.js io_0 "leaders").
 *
 * ["r", "leaders", <query>, id, name, _, region] -> ["0", "leaders", <query>, rows]
 * <query> is a qs_player column (player_score, player_kills, ...) or player_clan
 * (clan rows). region = config.socialRegions index (0 = all). Results are cached
 * for accounts.leaderboard_cache_s seconds (default 30) per (query, region).
 *
 * KrunkNative (v7.1.4 seasons, season.c): season_score / season_kills / season_wins /
 * season_games / season_timeplayed rank the current season's season_stats (0108_seasons); the
 * rows carry the value under the query's name.
 */
#include "acct_internal.h"
#include "brain_internal.h"
#include "hot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/config.h"
#include "knsvc/http.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

typedef struct LeaderQuery {
    const char *name;     /* request value (config.leaderQueries) */
    const char *column;   /* player_rows column, NULL = clans */
} LeaderQuery;

static const LeaderQuery k_queries[] = {
    {"player_score", "player_score"},       {"player_elo", "player_elo"},
    {"player_elo2", "player_elo2"},         {"player_elo4", "player_elo4"},
    {"player_kills", "player_kills"},       {"player_wins", "player_wins"},
    {"player_timeplayed", "player_timeplayed"}, {"player_funds", "player_funds"},
    {"player_chal", "player_chal"},         {"player_followed", "player_followed"},
    {"player_deaths", "player_deaths"},     {"player_games_played", "player_games_played"},
    {"player_clan", NULL},                  {"clans", NULL},
};

/* v7.1.4 seasons: season_stats columns of accounts.current_season */
static const LeaderQuery k_season_queries[] = {
    {"season_score", "score"}, {"season_kills", "kills"}, {"season_wins", "wins"},
    {"season_games", "games"}, {"season_timeplayed", "time_played"},
};

#define CACHE_SLOTS 32

typedef struct CacheEntry {
    char query[32];
    int64_t region;
    uint64_t at_ms;
    cJSON *rows;
} CacheEntry;

static CacheEntry s_cache[CACHE_SLOTS];

typedef struct LeaderCtx {
    char query[32];
    int64_t region;
    char hot_key[96];   /* shared page in the Redis hot tier (hot.h), "" = none */
} LeaderCtx;

static void send_rows(SocialSession *s, const char *query, cJSON *rows) {
    cJSON *f = social_frame("0");
    cJSON_AddItemToArray(f, cJSON_CreateString("leaders"));
    cJSON_AddItemToArray(f, cJSON_CreateString(query));
    cJSON_AddItemToArray(f, rows ? rows : cJSON_CreateArray());
    social_send(s, f);
}

static int cache_ttl_ms(void) {
    return (int) kn_config_int(g_acct_brain->cfg, "accounts.leaderboard_cache_s", 30) * 1000;
}

static CacheEntry *cache_find(const char *query, int64_t region) {
    for (int i = 0; i < CACHE_SLOTS; i++)
        if (s_cache[i].rows && s_cache[i].region == region && !strcmp(s_cache[i].query, query)) return &s_cache[i];
    return NULL;
}

static CacheEntry *cache_store(const char *query, int64_t region, const cJSON *rows) {
    CacheEntry *e = cache_find(query, region);
    if (!e) {
        e = &s_cache[0];
        for (int i = 0; i < CACHE_SLOTS; i++) {
            if (!s_cache[i].rows) {
                e = &s_cache[i];
                break;
            }
            if (s_cache[i].at_ms < e->at_ms) e = &s_cache[i];
        }
    }
    cJSON_Delete(e->rows);
    snprintf(e->query, sizeof e->query, "%s", query);
    e->region = region;
    e->at_ms = kn_now_ms();
    e->rows = cJSON_Duplicate(rows, 1);
    return e;
}

static void leaders_done(SocialSession *s, kn_db_result *res, void *extra) {
    LeaderCtx *c = extra;
    cJSON *rows = NULL;
    if (kn_db_ok(res)) {
        rows = kn_db_result_to_json(res);
        if (rows) {
            cache_store(c->query, c->region, rows);
            if (c->hot_key[0]) { /* read-through: the other brains reuse this page until it expires */
                char *text = cJSON_PrintUnformatted(rows);
                if (text) hot_put(c->hot_key, text, cache_ttl_ms());
                free(text);
            }
        }
    } else {
        acct_db_error(res, "leaders");
    }
    send_rows(s, c->query, rows);
}

static void season_page(SocialSession *s, const LeaderQuery *q, int64_t region, const char *hot_key) {
    LeaderCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    snprintf(c->query, sizeof c->query, "%s", q->name);
    snprintf(c->hot_key, sizeof c->hot_key, "%s", hot_key ? hot_key : "");
    c->region = region;
    char reg[24], lim[24], season[24];
    acct_itoa(reg, region);
    acct_itoa(lim, g_acct.page_size > 0 ? g_acct.page_size : 100);
    acct_itoa(season, acct_current_season());
    /* column and alias come from the fixed table above, never from the request */
    char any[1024];
    snprintf(any, sizeof any,
             "SELECT r.player_id, r.player_name, r.player_featured, r.player_hack, r.player_premium, r.player_clan, r.clan_rank,"
             " ss.%s AS %s FROM season_stats ss JOIN player_rows r ON r.player_id = ss.account_id"
             " WHERE ss.season = {int:$3} AND ({int:$1} = 0 OR r.player_region = {int:$1}) AND ss.%s > 0"
             " ORDER BY ss.%s DESC, r.player_id LIMIT $2",
             q->column, q->name, q->column, q->column);
    const kn_sql text = {.any = any};
    social_query(s, &text, KN_DB_ARGS(reg, lim, season), leaders_done, c);
}

static void player_page(SocialSession *s, const LeaderQuery *q, int64_t region, const char *hot_key);

/* The page from the database (season tables or player_rows / clans). */
static void leaders_db(SocialSession *s, const LeaderQuery *q, int season, int64_t region, const char *hot_key) {
    if (season) season_page(s, q, region, hot_key);
    else player_page(s, q, region, hot_key);
}

typedef struct LeaderHot {
    SocialSession *s;
    const LeaderQuery *q;
    int season;
    int64_t region;
    char key[96];
} LeaderHot;

static void leaders_hot_done(void *ud, const char *value, size_t len, int64_t pttl_ms) {
    LeaderHot *h = ud;
    cJSON *rows = value ? cJSON_ParseWithLength(value, len) : NULL;
    if (cJSON_IsArray(rows)) {
        CacheEntry *e = cache_store(h->q->name, h->region, rows);
        const int ttl = cache_ttl_ms();
        if (e && pttl_ms >= 0 && pttl_ms < ttl) e->at_ms -= (uint64_t) (ttl - pttl_ms); /* expire with the shared copy */
        if (social_open(h->s)) send_rows(h->s, h->q->name, rows);
        else cJSON_Delete(rows);
    } else {
        cJSON_Delete(rows);
        if (social_open(h->s)) leaders_db(h->s, h->q, h->season, h->region, h->key);
    }
    social_unhold(h->s);
    free(h);
}

/* Local cache missed: the shared page in Redis (one database query per page and TTL
 * for every brain), else the database. */
static void leaders_fetch(SocialSession *s, const LeaderQuery *q, int season, int64_t region) {
    char k[96];
    if (season) snprintf(k, sizeof k, "lb:%s:%lld:s%d", q->name, (long long) region, acct_current_season());
    else snprintf(k, sizeof k, "lb:%s:%lld", q->name, (long long) region);
    if (hot_up()) {
        LeaderHot *h = calloc(1, sizeof *h);
        if (h) {
            h->s = social_hold(s);
            h->q = q;
            h->season = season;
            h->region = region;
            snprintf(h->key, sizeof h->key, "%s", k);
            if (hot_get(k, leaders_hot_done, h) == 0) return;
            social_unhold(s);
            free(h);
        }
    }
    leaders_db(s, q, season, region, hot_up() ? k : NULL);
}

void leaders_page(SocialSession *s, const char *query, int64_t region) {
    const LeaderQuery *q = NULL;
    for (size_t i = 0; i < sizeof k_queries / sizeof k_queries[0]; i++)
        if (!strcmp(k_queries[i].name, query)) q = &k_queries[i];
    const LeaderQuery *sq = NULL;
    for (size_t i = 0; i < sizeof k_season_queries / sizeof k_season_queries[0]; i++)
        if (!strcmp(k_season_queries[i].name, query)) sq = &k_season_queries[i];
    if (sq) {
        if (region < 0 || region > 16) region = 0;
        CacheEntry *e = cache_find(sq->name, region);
        if (e && kn_now_ms() - e->at_ms < (uint64_t) cache_ttl_ms()) {
            send_rows(s, sq->name, cJSON_Duplicate(e->rows, 1));
            return;
        }
        leaders_fetch(s, sq, 1, region);
        return;
    }
    if (!q) {
        send_rows(s, query, NULL);
        return;
    }
    if (region < 0 || region > 16) region = 0;
    CacheEntry *e = cache_find(q->name, region);
    if (e && kn_now_ms() - e->at_ms < (uint64_t) cache_ttl_ms()) {
        send_rows(s, q->name, cJSON_Duplicate(e->rows, 1));
        return;
    }
    leaders_fetch(s, q, 0, region);
}

static void player_page(SocialSession *s, const LeaderQuery *q, int64_t region, const char *hot_key) {
    LeaderCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    snprintf(c->query, sizeof c->query, "%s", q->name);
    snprintf(c->hot_key, sizeof c->hot_key, "%s", hot_key ? hot_key : "");
    c->region = region;
    char reg[24], lim[24];
    acct_itoa(reg, region);
    acct_itoa(lim, g_acct.page_size > 0 ? g_acct.page_size : 100);
    if (!q->column) {
#define CLAN_LEADERS(region_test)                                                                          \
    "SELECT c.clan_name AS player_clan, c.clan_name, c.clan_rank, c.clan_score, "                                 \
    "(SELECT count(*) FROM clan_members m WHERE m.clan_id = c.clan_id) AS clan_membercount, "                     \
    "(SELECT count(*) FROM clan_members m JOIN players p ON p.player_id = m.account_id "                          \
    " WHERE m.clan_id = c.clan_id AND p.player_hack > 0) AS clan_hackcount, "                                     \
    "a.username AS creatorname FROM clans c LEFT JOIN accounts a ON a.id = c.clan_playerid "                      \
    "WHERE " region_test " ORDER BY c.clan_score DESC, c.clan_id LIMIT $2"
        social_query(s,
                     SQL(.pg = CLAN_LEADERS("$1::int = 0 OR c.clan_region = $1::int"),
                         .any = CLAN_LEADERS("{int:$1} = 0 OR c.clan_region = {int:$1}")),
                     KN_DB_ARGS(reg, lim), leaders_done, c);
#undef CLAN_LEADERS
        return;
    }
    /* column comes from the fixed table above, never from the request */
    char sql[768], any[768];
    snprintf(sql, sizeof sql,
             "SELECT player_id, player_name, player_featured, player_hack, player_premium, player_clan, clan_rank, %s "
             "FROM player_rows WHERE ($1::int = 0 OR player_region = $1::int) "
             "ORDER BY %s DESC, player_id LIMIT $2",
             q->column, q->column);
    snprintf(any, sizeof any,
             "SELECT player_id, player_name, player_featured, player_hack, player_premium, player_clan, clan_rank, %s "
             "FROM player_rows WHERE ({int:$1} = 0 OR player_region = {int:$1}) "
             "ORDER BY %s DESC, player_id LIMIT $2",
             q->column, q->column);
    const kn_sql text = {.pg = sql, .any = any};
    social_query(s, &text, KN_DB_ARGS(reg, lim), leaders_done, c);
}

/* ================================================================ v10's HTTP leaderboards
 *
 * Krunker v10.0.2 reads its Leaderboards popup (_49281, the menu's #menuBtnLeaderboards) from gapi
 * (v10:26987-27036) instead of the social socket:
 *   GET /api/leaderboards/<type>?sort&season   {data: [{position, player_name, player_clan, value}]}
 *        type: player_score kills wins timeplayed funds followed nukes skinvalue eventcount
 *        ranked_matches (?season), player_clan (?sort=score|members: {position, clan_name,
 *        clan_score, member_count})
 *   GET /api/leaderboards/raids/<map>[?sort=completions], /raids/<map>/monthly?month=YYYY-MM
 *   GET /api/leaderboards/krunkcup/teams, /krunkcup/teams/<id>/contributors, /krunkcup/players
 * KrunkNative keeps no raid clear times, inventory values, event finds or KrunkCup teams: those
 * answer an empty list (the popup then says "No data available."). Pages are cached
 * accounts.leaderboard_cache_s per path, like the socket's. */

typedef struct HttpBoard {
    kn_http_req *req;
    char key[96];
    int clans;
} HttpBoard;

#define HTTP_CACHE_SLOTS 32
static int s_http_ttl_ms = 30000; /* accounts.leaderboard_cache_s */
static int s_http_limit = 100;    /* accounts.page_size */
static struct {
    char key[96];
    uint64_t at_ms;
    cJSON *data;
} s_http_cache[HTTP_CACHE_SLOTS];

static void http_reply(kn_http_req *req, const cJSON *data) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "data", data ? cJSON_Duplicate(data, 1) : cJSON_CreateArray());
    kn_http_set_header(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

static const cJSON *http_cached(const char *key) {
    for (int i = 0; i < HTTP_CACHE_SLOTS; i++)
        if (s_http_cache[i].data && !strcmp(s_http_cache[i].key, key) && kn_now_ms() - s_http_cache[i].at_ms < (uint64_t) s_http_ttl_ms)
            return s_http_cache[i].data;
    return NULL;
}

static void http_store(const char *key, const cJSON *data) {
    int slot = 0;
    for (int i = 0; i < HTTP_CACHE_SLOTS; i++) {
        if (!s_http_cache[i].data || !strcmp(s_http_cache[i].key, key)) {
            slot = i;
            break;
        }
        if (s_http_cache[i].at_ms < s_http_cache[slot].at_ms) slot = i;
    }
    cJSON_Delete(s_http_cache[slot].data);
    snprintf(s_http_cache[slot].key, sizeof s_http_cache[slot].key, "%s", key);
    s_http_cache[slot].at_ms = kn_now_ms();
    s_http_cache[slot].data = cJSON_Duplicate(data, 1);
}

static void on_http_board(kn_db_result *res, void *ud) {
    HttpBoard *h = ud;
    if (!kn_db_ok(res)) {
        acct_db_error(res, "leaderboards http");
        cJSON *o = cJSON_CreateObject();
        cJSON *e = cJSON_AddObjectToObject(o, "error");
        cJSON_AddStringToObject(e, "message", "Failed to fetch leaderboard");
        brain_reply_json(h->req, 500, o);
        cJSON_Delete(o);
        free(h);
        return;
    }
    cJSON *rows = kn_db_result_to_json(res);
    if (!rows) rows = cJSON_CreateArray();
    int pos = 0;
    cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        cJSON_AddNumberToObject(r, "position", ++pos);
        if (!h->clans && !cJSON_IsString(cJSON_GetObjectItemCaseSensitive(r, "player_clan"))) {
            cJSON_DeleteItemFromObjectCaseSensitive(r, "player_clan");
            cJSON_AddStringToObject(r, "player_clan", "");
        }
    }
    http_store(h->key, rows);
    http_reply(h->req, rows);
    cJSON_Delete(rows);
    free(h);
}

/* player_rows columns behind v10's player types (the request never names a column itself) */
static const struct {
    const char *type, *column;
} k_http_player[] = {
    {"player_score", "player_score"}, {"player_kills", "player_kills"},   {"player_wins", "player_wins"},
    {"player_timeplayed", "player_timeplayed"}, {"player_funds", "player_funds"}, {"player_followed", "player_followed"},
};

static void h_http_board(kn_http_req *req, void *ud) {
    Brain *b = ud;
    const char *type = kn_http_param(req, "type");
    char sort[16] = "", season[16] = "";
    kn_http_query_get(req, "sort", sort, sizeof sort);
    kn_http_query_get(req, "season", season, sizeof season);
    char key[96];
    snprintf(key, sizeof key, "%s|%s|%s", type ? type : "", sort, season);
    const cJSON *hit = http_cached(key);
    if (hit) return http_reply(req, hit);
    if (!b->db || !type) return http_reply(req, NULL);
    HttpBoard *h = calloc(1, sizeof *h);
    if (!h) return http_reply(req, NULL);
    h->req = req;
    snprintf(h->key, sizeof h->key, "%s", key);
    char lim[16];
    snprintf(lim, sizeof lim, "%d", s_http_limit);
    for (size_t i = 0; i < sizeof k_http_player / sizeof k_http_player[0]; i++) {
        if (strcmp(type, k_http_player[i].type)) continue;
        char any[512];
        snprintf(any, sizeof any,
                 "SELECT player_name, player_clan, %s AS value FROM player_rows WHERE %s > 0 AND player_hack = 0"
                 " ORDER BY %s DESC, player_id LIMIT {int:$1}",
                 k_http_player[i].column, k_http_player[i].column, k_http_player[i].column);
        const kn_sql q = {.any = any};
        kn_db_query_sql(b->db, &q, KN_DB_ARGS(lim), on_http_board, h);
        return;
    }
    if (!strcmp(type, "player_nukes")) { /* player_stats "n": Nukes called in */
        static const kn_sql q = {
            .pg = "SELECT r.player_name, r.player_clan, (p.player_stats->>'n')::bigint AS value FROM players p JOIN player_rows r ON r.player_id = p.player_id"
                  " WHERE (p.player_stats->>'n') IS NOT NULL AND (p.player_stats->>'n')::numeric > 0 AND r.player_hack = 0"
                  " ORDER BY (p.player_stats->>'n')::numeric DESC, p.player_id LIMIT $1::int",
            .sqlite = "SELECT r.player_name, r.player_clan, CAST(json_extract(p.player_stats, '$.n') AS INTEGER) AS value FROM players p"
                      " JOIN player_rows r ON r.player_id = p.player_id WHERE json_extract(p.player_stats, '$.n') > 0 AND r.player_hack = 0"
                      " ORDER BY json_extract(p.player_stats, '$.n') DESC, p.player_id LIMIT CAST($1 AS INTEGER)",
            .mysql = "SELECT r.player_name, r.player_clan, CAST(JSON_EXTRACT(p.player_stats, '$.n') AS SIGNED) AS value FROM players p"
                     " JOIN player_rows r ON r.player_id = p.player_id WHERE CAST(JSON_EXTRACT(p.player_stats, '$.n') AS DECIMAL(20,2)) > 0 AND r.player_hack = 0"
                     " ORDER BY CAST(JSON_EXTRACT(p.player_stats, '$.n') AS DECIMAL(20,2)) DESC, p.player_id LIMIT $1"};
        kn_db_query_sql(b->db, &q, KN_DB_ARGS(lim), on_http_board, h);
        return;
    }
    if (!strcmp(type, "player_ranked_matches")) { /* v10 leaderQueries: ranked matches (a season or all) */
        static const kn_sql q = {.any = "SELECT r.player_name, r.player_clan, sum(rp.wins + rp.losses + rp.draws) AS value FROM ranked_profiles rp"
                                        " JOIN player_rows r ON r.player_id = rp.account_id WHERE ($1 = '' OR rp.season = {int:$2})"
                                        " GROUP BY rp.account_id, r.player_name, r.player_clan HAVING sum(rp.wins + rp.losses + rp.draws) > 0"
                                        " ORDER BY sum(rp.wins + rp.losses + rp.draws) DESC, rp.account_id LIMIT {int:$3}"};
        kn_db_query_sql(b->db, &q, KN_DB_ARGS(season, season[0] ? season : "0", lim), on_http_board, h);
        return;
    }
    if (!strcmp(type, "player_clan") || !strcmp(type, "clans")) {
        h->clans = 1;
        static const kn_sql by_score = {.any = "SELECT c.clan_name, c.clan_score, (SELECT count(*) FROM clan_members m WHERE m.clan_id = c.clan_id) AS member_count"
                                               " FROM clans c ORDER BY c.clan_score DESC, c.clan_id LIMIT {int:$1}"};
        static const kn_sql by_members = {.any = "SELECT c.clan_name, c.clan_score, (SELECT count(*) FROM clan_members m WHERE m.clan_id = c.clan_id) AS member_count"
                                                 " FROM clans c ORDER BY (SELECT count(*) FROM clan_members m WHERE m.clan_id = c.clan_id) DESC, c.clan_score DESC, c.clan_id"
                                                 " LIMIT {int:$1}"};
        kn_db_query_sql(b->db, !strcmp(sort, "members") ? &by_members : &by_score, KN_DB_ARGS(lim), on_http_board, h);
        return;
    }
    free(h); /* player_skinvalue, player_eventcount, unknown types: nothing to rank */
    http_reply(req, NULL);
}

static void h_http_empty(kn_http_req *req, void *ud) {
    (void) ud;
    http_reply(req, NULL);
}

int leaderboards_http_init(Brain *b) {
    if (!b->http) return 0;
    s_http_ttl_ms = (int) kn_config_int(b->cfg, "accounts.leaderboard_cache_s", 30) * 1000;
    s_http_limit = (int) kn_config_int(b->cfg, "accounts.page_size", 100);
    if (s_http_limit <= 0 || s_http_limit > 500) s_http_limit = 100;
    kn_http_route(b->http, "GET", "/api/leaderboards/raids/:map", h_http_empty, b);
    kn_http_route(b->http, "GET", "/api/leaderboards/raids/:map/monthly", h_http_empty, b);
    kn_http_route(b->http, "GET", "/api/leaderboards/krunkcup/teams", h_http_empty, b);
    kn_http_route(b->http, "GET", "/api/leaderboards/krunkcup/teams/:id/contributors", h_http_empty, b);
    kn_http_route(b->http, "GET", "/api/leaderboards/krunkcup/players", h_http_empty, b);
    kn_http_route(b->http, "GET", "/api/leaderboards/:type", h_http_board, b);
    return 0;
}
