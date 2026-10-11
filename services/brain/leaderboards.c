/*
 * services/brain/leaderboards.c - the social page's "leaders" page (Krunker
 * v3.1.1 config.leaderQueries / leaderCal, src/social.js io_0 "leaders").
 *
 * ["r", "leaders", <query>, id, name, _, region] -> ["0", "leaders", <query>, rows]
 * <query> is a qs_player column (player_score, player_kills, ...) or player_clan
 * (clan rows). region = config.socialRegions index (0 = all). Results are cached
 * for accounts.leaderboard_cache_s seconds (default 30) per (query, region).
 */
#include "acct_internal.h"

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

static void cache_store(const char *query, int64_t region, const cJSON *rows) {
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
}

static void leaders_done(SocialSession *s, kn_db_result *res, void *extra) {
    LeaderCtx *c = extra;
    cJSON *rows = NULL;
    if (kn_db_ok(res)) {
        rows = kn_db_result_to_json(res);
        if (rows) cache_store(c->query, c->region, rows);
    } else {
        acct_db_error(res, "leaders");
    }
    send_rows(s, c->query, rows);
}

void leaders_page(SocialSession *s, const char *query, int64_t region) {
    const LeaderQuery *q = NULL;
    for (size_t i = 0; i < sizeof k_queries / sizeof k_queries[0]; i++)
        if (!strcmp(k_queries[i].name, query)) q = &k_queries[i];
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
    LeaderCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    snprintf(c->query, sizeof c->query, "%s", q->name);
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
