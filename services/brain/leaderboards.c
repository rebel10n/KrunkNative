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
#include "hot.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/config.h"
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
