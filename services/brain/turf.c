/*
 * services/brain/turf.c - Krunker v10.0.2 Turf Wars ("territorialWars", v10's clan war;
 * doc/v10/ranked-impl.md). Rules: shared/include/turf_wars.h; tables: 0206_turf_wars; procs:
 * db_turf.c.
 *
 * Game servers send "turf-kills" over the internal WS at the end of each eligible round (official
 * public FFA / TDM on Burg / Sandstorm / Subzero, server/src/accounts.c); the day is this brain's
 * UTC clock (authoritative). Every brain checks once a minute whether a finished day is unsettled
 * and settles it (P_TURF_SETTLE: one lock, once per day and map).
 *
 * Routes: v10's gapi territorialWars client, on the brain's /api prefix (the edge relays proxy it):
 *   /api/territorial-wars/status                          [{map_id, map_name, clan_id, clan_name,
 *        clan_logo, kills, total_kills_today, guaranteed_reward, estimated_reward, leaderboard}]
 *   /api/territorial-wars/clan/:id                        {clan, current_day, maps, totals, comparison}
 *   /api/territorial-wars/clan/:id/players?day&map_id     {players}
 *   /api/territorial-wars/clan/:id/earnings               {total_kr, last_30_days, days}
 *   /api/territorial-wars/clan/:id/payouts?day            {payouts}
 *   /api/territorial-wars/leaderboards/top-performing-clans?day&map_id&clan_name   {leaderboard}
 *   /api/territorial-wars/leaderboards/top-grossing-clans?clan_name                 {leaderboard}
 *   /api/territorial-wars/leaderboards/top-performing-players?map_id&player_name    {leaderboard}
 *   /api/territorial-wars/player/:id                      {player, clan, totals}
 *   /api/territorial-wars/player/:id/contributions?day    {contributions}
 *   /api/territorial-wars/player/:id/earnings             {total_kr, payouts}
 *   GET|PUT /api/clans/:id/loot-config (PUT: token)       the clan's loot distribution
 * :id is a clan id or (KrunkNative) a clan tag / player name.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain_internal.h"
#include <turf_wars.h>
#include "knsvc/config.h"
#include "knsvc/db.h"
#include "db_procs.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "turf"

static struct {
    Brain *b;
    int enabled;
    int kr_per_kill;
    int day_override;              /* brain.turf.day (-1: the clock) */
    kn_timer_id timer;
    int settling;
    int settled_day;               /* the last day this brain saw settled */
} T;

static int db_ready(void) { return T.b && T.b->db && T.enabled; }

static int current_day(void) { return T.day_override >= 0 ? T.day_override : turf_day_at(kn_wall_ms() / 1000); }

static void log_err(const char *what, kn_db_result *res) { KN_WARN(MOD, "%s failed: %s", what, kn_db_error(res)); }

static void reply_error(kn_http_req *req, int status, const char *name) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", name);
    cJSON_AddStringToObject(o, "message", name);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static int query_int(kn_http_req *req, const char *key, int def) {
    char v[32];
    return kn_http_query_get(req, key, v, sizeof v) > 0 ? atoi(v) : def;
}

static const char *nz(const char *s) { return s ? s : ""; }

/* ---- generic "rows -> {key: rows}" replies */

typedef struct Q {
    kn_http_req *req;
    const char *key;
    int day, map;
    char id[64];
    cJSON *out;
    int step;
} Q;

static Q *q_new(kn_http_req *req, const char *key) {
    Q *q = calloc(1, sizeof(*q));
    if (!q) {
        reply_error(req, 500, "internal");
        return NULL;
    }
    q->req = req;
    q->key = key;
    q->day = query_int(req, "day", current_day());
    q->map = query_int(req, "map_id", -1);
    const char *id = kn_http_param(req, "id");
    snprintf(q->id, sizeof q->id, "%s", nz(id));
    q->out = cJSON_CreateObject();
    cJSON_AddNumberToObject(q->out, "current_day", current_day());
    return q;
}

static void q_done(Q *q) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "data", q->out); /* v10's api client unwraps .data */
    kn_http_set_header(q->req, "Cache-Control", "no-cache, no-store, must-revalidate");
    brain_reply_json(q->req, 200, o);
    cJSON_Delete(o);
    free(q);
}

static void q_fail(Q *q, kn_db_result *res) {
    log_err(q->key, res);
    reply_error(q->req, 500, "internal");
    cJSON_Delete(q->out);
    free(q);
}

static void on_rows(kn_db_result *res, void *ud) {
    Q *q = ud;
    if (!kn_db_ok(res)) return q_fail(q, res);
    cJSON_AddItemToObject(q->out, q->key, kn_db_result_to_json(res));
    q_done(q);
}

static void q_run(Q *q, const kn_sql *sql, int n, const char *const *p) {
    if (!db_ready()) {
        reply_error(q->req, 503, "TurfWarsUnavailable");
        cJSON_Delete(q->out);
        free(q);
        return;
    }
    kn_db_query_sql(T.b->db, sql, n, p, on_rows, q);
}

#define S(v) i2s((char[24]){0}, (v))
static const char *i2s(char *b, const long long v) {
    snprintf(b, 24, "%lld", v);
    return b;
}

/* a clan by id / tag (case-insensitive) */
#define CLAN_OF_1 "(SELECT clan_id FROM clans WHERE CAST(clan_id AS TEXT) = $1 OR lower(clan_name) = lower($1) LIMIT 1)"
/* a player by id / name */
#define PLAYER_OF_1 "(SELECT id FROM accounts WHERE CAST(id AS TEXT) = $1 OR lower(username) = lower($1) LIMIT 1)"

/* ---- status: today's race per map (the controller is yesterday's winner) */

static void on_status_today(kn_db_result *res, void *ud) {
    Q *q = ud;
    if (!kn_db_ok(res)) return q_fail(q, res);
    cJSON *rows = kn_db_result_to_json(res);
    cJSON *maps = cJSON_GetObjectItemCaseSensitive(q->out, "maps");
    cJSON *m;
    cJSON_ArrayForEach(m, maps) {
        const int map = (int) kn_json_num(m, "map_id", -1);
        cJSON *lb = cJSON_AddArrayToObject(m, "leaderboard");
        int64_t total = 0;
        int pos = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, rows) {
            if ((int) kn_json_num(r, "map_id", -2) != map) continue;
            total += (int64_t) kn_json_num(r, "kills", 0);
            if (pos >= 10) continue;
            cJSON *e = cJSON_Duplicate(r, 1);
            cJSON_DeleteItemFromObjectCaseSensitive(e, "map_id");
            cJSON_AddNumberToObject(e, "position", ++pos);
            cJSON_AddItemToArray(lb, e);
        }
        cJSON_AddNumberToObject(m, "total_kills_today", (double) total);
        /* guaranteed: the cut the leader holds now; estimated: today's pool so far */
        const int64_t pool = total * T.kr_per_kill;
        cJSON_AddNumberToObject(m, "estimated_reward", (double) pool);
        cJSON_AddNumberToObject(m, "guaranteed_reward", (double) turf_cut(pool, 0));
    }
    cJSON_Delete(rows);
    cJSON *data = cJSON_DetachItemFromObjectCaseSensitive(q->out, "maps");
    cJSON_Delete(q->out);
    q->out = data;
    q_done(q);
}

static void on_status_control(kn_db_result *res, void *ud) {
    Q *q = ud;
    if (!kn_db_ok(res)) return q_fail(q, res);
    cJSON *rows = kn_db_result_to_json(res);
    cJSON *maps = cJSON_AddArrayToObject(q->out, "maps");
    for (int i = 0; i < TURF_MAP_COUNT; i++) {
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "map_id", g_turf_maps[i]);
        cJSON_AddStringToObject(m, "map_name", turf_map_name(g_turf_maps[i]));
        const cJSON *r, *hit = NULL;
        cJSON_ArrayForEach(r, rows) if ((int) kn_json_num(r, "map_id", -1) == g_turf_maps[i]) hit = r;
        const cJSON *cid = hit ? cJSON_GetObjectItemCaseSensitive(hit, "clan_id") : NULL;
        if (cJSON_IsNumber(cid)) {
            cJSON_AddNumberToObject(m, "clan_id", cid->valuedouble);
            cJSON_AddStringToObject(m, "clan_name", kn_json_str(hit, "clan_name", ""));
            cJSON_AddNumberToObject(m, "kills", kn_json_num(hit, "kills", 0));
        } else {
            cJSON_AddNullToObject(m, "clan_id");
            cJSON_AddNullToObject(m, "clan_name");
            cJSON_AddNumberToObject(m, "kills", 0);
        }
        cJSON_AddNullToObject(m, "clan_logo"); /* KrunkNative clans have no uploaded logos */
        cJSON_AddItemToArray(maps, m);
    }
    cJSON_Delete(rows);
    static const kn_sql today = {.any = "SELECT tk.map_id AS map_id__i, tk.clan_id AS clan_id__i, c.clan_name, sum(tk.kills) AS kills__i"
                                        " FROM turf_kills tk JOIN clans c ON c.clan_id = tk.clan_id WHERE tk.day = {int:$1}"
                                        " GROUP BY tk.map_id, tk.clan_id, c.clan_name ORDER BY tk.map_id, sum(tk.kills) DESC, tk.clan_id"};
    kn_db_query_sql(T.b->db, &today, KN_DB_ARGS(S(current_day())), on_status_today, q);
}

static void h_status(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "status");
    if (!q) return;
    static const kn_sql control = {.any = "SELECT d.map_id AS map_id__i, d.clan_id AS clan_id__i, c.clan_name, d.kills AS kills__i FROM turf_days d"
                                          " LEFT JOIN clans c ON c.clan_id = d.clan_id WHERE d.day = {int:$1}"};
    if (!db_ready()) return q_run(q, &control, 0, NULL);
    kn_db_query_sql(T.b->db, &control, KN_DB_ARGS(S(current_day() - 1)), on_status_control, q);
}

/* ---- clan */

static void on_clan_maps(kn_db_result *res, void *ud) {
    Q *q = ud;
    if (!kn_db_ok(res)) return q_fail(q, res);
    cJSON_AddItemToObject(q->out, "days", kn_db_result_to_json(res));
    q_done(q);
}

static void on_clan_row(kn_db_result *res, void *ud) {
    Q *q = ud;
    if (!kn_db_ok(res)) return q_fail(q, res);
    if (kn_db_rows(res) < 1) {
        reply_error(q->req, 404, "Clan not found");
        cJSON_Delete(q->out);
        free(q);
        return;
    }
    cJSON *clan = kn_db_row_to_json(res, 0);
    cJSON_AddItemToObject(q->out, "clan", clan);
    snprintf(q->id, sizeof q->id, "%lld", (long long) kn_json_int(clan, "clan_id", 0));
    /* per day and map: kills, position, KR, control (the last 30 days) */
    static const kn_sql days = {.any = "SELECT cd.day AS day__i, cd.map_id AS map_id__i, cd.position AS position__i, cd.kills AS kills__i,"
                                       " cd.kr AS kr__i, (d.clan_id = cd.clan_id) AS won__b FROM turf_clan_days cd"
                                       " JOIN turf_days d ON d.day = cd.day AND d.map_id = cd.map_id"
                                       " WHERE cd.clan_id = {int:$1} AND cd.day >= {int:$2} ORDER BY cd.day DESC, cd.map_id"};
    kn_db_query_sql(T.b->db, &days, KN_DB_ARGS(q->id, S(current_day() - 30)), on_clan_maps, q);
}

static void h_clan(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "clan");
    if (!q) return;
    static const kn_sql sql = {.any = "SELECT c.clan_id AS clan_id__i, c.clan_name, NULL AS clan_logo,"
                                      " (SELECT coalesce(sum(kills), 0) FROM turf_kills WHERE clan_id = c.clan_id AND day = {int:$2}) AS kills_today__i,"
                                      " (SELECT coalesce(sum(kills), 0) FROM turf_kills WHERE clan_id = c.clan_id) AS total_kills__i,"
                                      " (SELECT coalesce(sum(deaths), 0) FROM turf_kills WHERE clan_id = c.clan_id) AS total_deaths__i,"
                                      " (SELECT coalesce(sum(kr), 0) FROM turf_clan_days WHERE clan_id = c.clan_id) AS total_kr__i,"
                                      " (SELECT count(*) FROM turf_days WHERE clan_id = c.clan_id) AS days_won__i"
                                      " FROM clans c WHERE c.clan_id = " CLAN_OF_1};
    if (!db_ready()) return q_run(q, &sql, 0, NULL);
    kn_db_query_sql(T.b->db, &sql, KN_DB_ARGS(q->id, S(current_day())), on_clan_row, q);
}

static void h_clan_players(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "players");
    if (!q) return;
    static const kn_sql sql = {.any = "SELECT tk.account_id AS player_id__i, a.username AS player_name, sum(tk.kills) AS kills__i,"
                                      " sum(tk.deaths) AS deaths__i, sum(tk.games) AS games__i FROM turf_kills tk JOIN accounts a ON a.id = tk.account_id"
                                      " WHERE tk.clan_id = " CLAN_OF_1 " AND tk.day = {int:$2} AND ({int:$3} < 0 OR tk.map_id = {int:$3})"
                                      " GROUP BY tk.account_id, a.username ORDER BY sum(tk.kills) DESC, tk.account_id LIMIT 100"};
    q_run(q, &sql, KN_DB_ARGS(q->id, S(q->day), S(q->map)));
}

static void on_earn_days(kn_db_result *res, void *ud) {
    Q *q = ud;
    if (!kn_db_ok(res)) return q_fail(q, res);
    cJSON *days = kn_db_result_to_json(res);
    double total = 0, last30 = 0;
    const cJSON *d;
    cJSON_ArrayForEach(d, days) {
        total += kn_json_num(d, "kr", 0);
        if (kn_json_num(d, "day", 0) >= current_day() - 30) last30 += kn_json_num(d, "kr", 0);
    }
    cJSON_AddNumberToObject(q->out, "total_kr", total);
    cJSON_AddNumberToObject(q->out, "last_30_days", last30);
    cJSON_AddItemToObject(q->out, "days", days);
    q_done(q);
}

static void h_clan_earnings(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "earnings");
    if (!q) return;
    static const kn_sql sql = {.any = "SELECT day AS day__i, sum(kr) AS kr__i FROM turf_clan_days WHERE clan_id = " CLAN_OF_1
                                      " GROUP BY day ORDER BY day DESC LIMIT 365"};
    if (!db_ready()) return q_run(q, &sql, 0, NULL);
    kn_db_query_sql(T.b->db, &sql, KN_DB_ARGS(q->id), on_earn_days, q);
}

static void h_clan_payouts(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "payouts");
    if (!q) return;
    if (q->day >= current_day()) q->day = current_day() - 1; /* payouts exist for settled days */
    static const kn_sql sql = {.any = "SELECT p.account_id AS player_id__i, coalesce(a.username, '') AS player_name, p.map_id AS map_id__i,"
                                      " p.kr AS kr__i FROM turf_payouts p LEFT JOIN accounts a ON a.id = p.account_id"
                                      " WHERE p.clan_id = " CLAN_OF_1 " AND p.day = {int:$2} ORDER BY p.kr DESC, p.id"};
    cJSON_AddNumberToObject(q->out, "day", q->day);
    q_run(q, &sql, KN_DB_ARGS(q->id, S(q->day)));
}

/* ---- leaderboards */

static void h_top_clans(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "leaderboard");
    if (!q) return;
    char name[32] = "";
    kn_http_query_get(req, "clan_name", name, sizeof name);
    static const kn_sql sql = {.any = "SELECT tk.clan_id AS clan_id__i, c.clan_name, NULL AS clan_logo, sum(tk.kills) AS kills__i,"
                                      " sum(tk.deaths) AS deaths__i, count(DISTINCT tk.account_id) AS players__i"
                                      " FROM turf_kills tk JOIN clans c ON c.clan_id = tk.clan_id"
                                      " WHERE tk.day = {int:$1} AND ({int:$2} < 0 OR tk.map_id = {int:$2}) AND ($3 = '' OR lower(c.clan_name) = lower($3))"
                                      " GROUP BY tk.clan_id, c.clan_name ORDER BY sum(tk.kills) DESC, tk.clan_id LIMIT 100"};
    q_run(q, &sql, KN_DB_ARGS(S(q->day), S(q->map), name));
}

static void h_top_grossing(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "leaderboard");
    if (!q) return;
    char name[32] = "";
    kn_http_query_get(req, "clan_name", name, sizeof name);
    static const kn_sql sql = {.any = "SELECT cd.clan_id AS clan_id__i, c.clan_name, NULL AS clan_logo, sum(cd.kr) AS total_kr__i,"
                                      " count(DISTINCT cd.day) AS days__i FROM turf_clan_days cd JOIN clans c ON c.clan_id = cd.clan_id"
                                      " WHERE ($1 = '' OR lower(c.clan_name) = lower($1)) GROUP BY cd.clan_id, c.clan_name"
                                      " HAVING sum(cd.kr) > 0 ORDER BY sum(cd.kr) DESC, cd.clan_id LIMIT 100"};
    q_run(q, &sql, KN_DB_ARGS(name));
}

static void h_top_players(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "leaderboard");
    if (!q) return;
    char name[40] = "";
    kn_http_query_get(req, "player_name", name, sizeof name);
    static const kn_sql sql = {.any = "SELECT tk.account_id AS player_id__i, a.username AS player_name, sum(tk.kills) AS kills__i,"
                                      " sum(tk.deaths) AS deaths__i, sum(tk.games) AS games__i FROM turf_kills tk JOIN accounts a ON a.id = tk.account_id"
                                      " WHERE ({int:$1} < 0 OR tk.map_id = {int:$1}) AND ($2 = '' OR lower(a.username) = lower($2))"
                                      " GROUP BY tk.account_id, a.username ORDER BY sum(tk.kills) DESC, tk.account_id LIMIT 100"};
    q_run(q, &sql, KN_DB_ARGS(S(q->map), name));
}

/* ---- player */

static void h_player(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "player");
    if (!q) return;
    static const kn_sql sql = {.any = "SELECT a.id AS player_id__i, a.username AS player_name, c.clan_id AS clan_id__i, c.clan_name,"
                                      " (SELECT coalesce(sum(kills), 0) FROM turf_kills WHERE account_id = a.id) AS kills__i,"
                                      " (SELECT coalesce(sum(deaths), 0) FROM turf_kills WHERE account_id = a.id) AS deaths__i,"
                                      " (SELECT coalesce(sum(games), 0) FROM turf_kills WHERE account_id = a.id) AS games__i,"
                                      " (SELECT coalesce(sum(kr), 0) FROM turf_payouts WHERE account_id = a.id) AS total_kr__i"
                                      " FROM accounts a LEFT JOIN clan_members m ON m.account_id = a.id LEFT JOIN clans c ON c.clan_id = m.clan_id"
                                      " WHERE a.id = " PLAYER_OF_1};
    q_run(q, &sql, KN_DB_ARGS(q->id));
}

static void h_player_contrib(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "contributions");
    if (!q) return;
    char day[16] = "";
    const int all = kn_http_query_get(req, "day", day, sizeof day) <= 0;
    static const kn_sql sql = {.any = "SELECT tk.day AS day__i, tk.map_id AS map_id__i, c.clan_name, tk.kills AS kills__i, tk.deaths AS deaths__i,"
                                      " tk.games AS games__i FROM turf_kills tk JOIN clans c ON c.clan_id = tk.clan_id"
                                      " WHERE tk.account_id = " PLAYER_OF_1 " AND ({int:$3} = 1 OR tk.day = {int:$2})"
                                      " ORDER BY tk.day DESC, tk.map_id LIMIT 200"};
    q_run(q, &sql, KN_DB_ARGS(q->id, S(q->day), S(all)));
}

static void h_player_earnings(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "payouts");
    if (!q) return;
    static const kn_sql sql = {.any = "SELECT p.day AS day__i, p.map_id AS map_id__i, c.clan_name, p.kr AS kr__i FROM turf_payouts p"
                                      " JOIN clans c ON c.clan_id = p.clan_id WHERE p.account_id = " PLAYER_OF_1
                                      " ORDER BY p.day DESC, p.id DESC LIMIT 200"};
    q_run(q, &sql, KN_DB_ARGS(q->id));
}

/* ---- loot config */

static void h_loot_get(kn_http_req *req, void *ud) {
    (void) ud;
    Q *q = q_new(req, "config");
    if (!q) return;
    static const kn_sql sql = {.any = "SELECT c.clan_id AS clan_id__i, coalesce(l.distribution_mode, 'clan_bank') AS distribution_mode,"
                                      " coalesce(l.clan_pct, 0) AS clan_pct__i, coalesce(l.top_player_count, 20) AS top_player_count__i,"
                                      " coalesce(l.top_player_pct, 80) AS top_player_pct__i, coalesce(l.remainder_pct, 20) AS remainder_pct__i,"
                                      " coalesce(l.min_games_required, 1) AS min_games_required__i, coalesce(l.updated_by, '') AS updated_by"
                                      " FROM clans c LEFT JOIN turf_loot_config l ON l.clan_id = c.clan_id WHERE c.clan_id = " CLAN_OF_1};
    q_run(q, &sql, KN_DB_ARGS(q->id));
}

typedef struct LootSet {
    kn_http_req *req;
    char clan[64];
    char *body;
} LootSet;

static void on_loot_set(kn_db_result *res, void *ud) {
    LootSet *l = ud;
    if (!kn_db_ok(res)) {
        const char *msg = kn_db_user_message(res);
        if (!msg) log_err("loot config", res);
        reply_error(l->req, msg ? 403 : 500, msg ? msg : "internal");
    } else {
        const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : "{}";
        cJSON *o = cJSON_CreateObject();
        cJSON_AddItemToObject(o, "data", kn_json_parse(t, strlen(t)));
        brain_reply_json(l->req, 200, o);
        cJSON_Delete(o);
    }
    free(l->body);
    free(l);
}

static void on_loot_account(void *ud, const char *err, const cJSON *acc) {
    LootSet *l = ud;
    const int64_t id = acc && !err ? kn_json_int(acc, "id", 0) : 0;
    if (id <= 0) {
        reply_error(l->req, 401, "LoginRequired");
        free(l->body);
        free(l);
        return;
    }
    /* the clan id: the route's :id (numeric) */
    kn_db_call(T.b->db, &P_TURF_LOOT_SET, KN_DB_ARGS(l->clan, S(id), l->body ? l->body : "{}"), on_loot_set, l);
}

static void h_loot_put(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "TurfWarsUnavailable");
    char token[512] = "";
    if (kn_http_query_get(req, "token", token, sizeof token) <= 0) {
        const char *bearer = kn_http_bearer(req);
        snprintf(token, sizeof token, "%s", bearer ? bearer : "");
    }
    if (!token[0]) return reply_error(req, 401, "LoginRequired");
    const char *id = kn_http_param(req, "id");
    if (!id || strspn(id, "0123456789") != strlen(id) || !id[0]) return reply_error(req, 400, "BadClanId");
    LootSet *l = calloc(1, sizeof(*l));
    if (!l) return reply_error(req, 500, "internal");
    l->req = req;
    snprintf(l->clan, sizeof l->clan, "%s", id);
    cJSON *body = kn_http_body_json(req);
    if (!body) { /* GET-only clients (the native UI): ?config=<json> */
        char cfg[512];
        if (kn_http_query_get(req, "config", cfg, sizeof cfg) > 0) body = kn_json_parse(cfg, strlen(cfg));
    }
    l->body = body ? kn_json_print(body) : NULL;
    cJSON_Delete(body);
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(T.b, "token", args, on_loot_account, l);
    cJSON_Delete(args);
}

/* ================================================================ game servers + settlement */

typedef struct RecordCtx {
    RankedResultCb cb;
    void *ud;
} RecordCtx;

static void on_recorded(kn_db_result *res, void *ud) {
    RecordCtx *r = ud;
    cJSON *reply = cJSON_CreateObject();
    cJSON_AddBoolToObject(reply, "ok", kn_db_ok(res));
    if (!kn_db_ok(res) && !kn_db_user_message(res)) log_err("turf record", res);
    else if (kn_db_rows(res) > 0) {
        cJSON *o = kn_json_parse(kn_db_get(res, 0, 0), strlen(kn_db_get(res, 0, 0)));
        cJSON_AddNumberToObject(reply, "recorded", (double) kn_json_int(o, "recorded", 0));
        cJSON_Delete(o);
    }
    if (r->cb) r->cb(r->ud, kn_db_ok(res) ? NULL : "db error", reply);
    cJSON_Delete(reply);
    free(r);
}

void turf_record(Brain *b, const cJSON *frame, RankedResultCb cb, void *ud) {
    if (!b->db || !T.enabled) {
        cJSON *reply = cJSON_CreateObject();
        cJSON_AddBoolToObject(reply, "ok", 1); /* acknowledged: nothing to keep */
        if (cb) cb(ud, NULL, reply);
        cJSON_Delete(reply);
        return;
    }
    RecordCtx *r = calloc(1, sizeof(*r));
    if (!r) {
        if (cb) cb(ud, "oom", NULL);
        return;
    }
    r->cb = cb;
    r->ud = ud;
    char *recs = kn_json_print(cJSON_GetObjectItemCaseSensitive(frame, "records"));
    kn_db_call(b->db, &P_TURF_RECORD, KN_DB_ARGS(S(current_day()), S(kn_json_int(frame, "mapId", -1)), recs ? recs : "[]"), on_recorded, r);
    free(recs);
}

static void on_settled(kn_db_result *res, void *ud) {
    const int day = (int) (intptr_t) ud;
    T.settling = 0;
    if (!kn_db_ok(res)) return log_err("turf settle", res);
    const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : "{}";
    cJSON *o = kn_json_parse(t, strlen(t));
    const int64_t maps = kn_json_int(o, "maps", 0);
    if (maps > 0) KN_INFO(MOD, "day %d settled (%lld map(s))", day, (long long) maps);
    T.settled_day = day;
    cJSON_Delete(o);
}

static void tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (!db_ready() || T.b->draining || T.settling) return;
    const int yesterday = current_day() - 1;
    if (yesterday < 0 || yesterday <= T.settled_day) return;
    T.settling = 1;
    kn_db_call(T.b->db, &P_TURF_SETTLE, KN_DB_ARGS(S(yesterday), S(T.kr_per_kill)), on_settled, (void *) (intptr_t) yesterday);
}

static void route(Brain *b, const char *path, kn_http_handler h) {
    kn_http_route(b->http, "GET", path, h, b);
}

int turf_init(Brain *b) {
    T.b = b;
    T.enabled = kn_config_bool(b->cfg, "brain.turf.enabled", 1);
    T.kr_per_kill = (int) kn_config_int(b->cfg, "brain.turf.kr_per_kill", TURF_KR_PER_KILL);
    T.day_override = (int) kn_config_int(b->cfg, "brain.turf.day", -1);
    T.settled_day = -1;
    route(b, "/api/territorial-wars/status", h_status);
    route(b, "/api/territorial-wars/clan/:id", h_clan);
    route(b, "/api/territorial-wars/clan/:id/players", h_clan_players);
    route(b, "/api/territorial-wars/clan/:id/earnings", h_clan_earnings);
    route(b, "/api/territorial-wars/clan/:id/payouts", h_clan_payouts);
    route(b, "/api/territorial-wars/leaderboards/top-performing-clans", h_top_clans);
    route(b, "/api/territorial-wars/leaderboards/top-grossing-clans", h_top_grossing);
    route(b, "/api/territorial-wars/leaderboards/top-performing-players", h_top_players);
    route(b, "/api/territorial-wars/player/:id", h_player);
    route(b, "/api/territorial-wars/player/:id/contributions", h_player_contrib);
    route(b, "/api/territorial-wars/player/:id/earnings", h_player_earnings);
    kn_http_route(b->http, "GET", "/api/clans/:id/loot-config", h_loot_get, b);
    kn_http_route(b->http, "PUT", "/api/clans/:id/loot-config", h_loot_put, b);
    kn_http_route(b->http, "POST", "/api/clans/:id/loot-config", h_loot_put, b);
    if (!b->db || !T.enabled) return 0;
    T.timer = kn_loop_add_timer(b->loop, 2000, (uint64_t) kn_config_int(b->cfg, "brain.turf.settle_check_ms", 60000), tick, NULL);
    KN_INFO(MOD, "Turf Wars on /api/territorial-wars/* (day %d, %d KR per kill)", current_day(), T.kr_per_kill);
    return 0;
}

void turf_shutdown(Brain *b) {
    if (T.timer) kn_loop_cancel_timer(b->loop, T.timer);
    T.timer = 0;
    T.enabled = 0;
}
