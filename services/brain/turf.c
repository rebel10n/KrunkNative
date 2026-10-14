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
 *   /api/territorial-wars/status                          [{map_id, map_name, clan_id, clan_name, clan_logo,
 *        kills, total_kills_today, guaranteed_reward, estimated_reward, leaderboard [{position, clan_*, kills}]}]
 *   /api/territorial-wars/clan/:id                        {clan_id, clan_name, clan_logo, current_day,
 *        maps [today per map: position, kills, deaths, games, comparison_*], historical_days [3]}
 *   /api/territorial-wars/clan/:id/players?day&map_id     [{player_id, player_name, kills, deaths, kd, games}]
 *   /api/territorial-wars/clan/:id/earnings               [{day, map_id, map_name, reward}]
 *   /api/territorial-wars/clan/:id/payouts[?day]          {total, payouts [{player_id, player_name, total_kr, map_name?}]}
 *   /api/territorial-wars/leaderboards/top-performing-clans?day&map_id&clan_name   [{position, clan_*, kills, deaths, kd, games}]
 *   /api/territorial-wars/leaderboards/top-grossing-clans?clan_name                 [{position, clan_*, total_kr, distributed, bank}]
 *   /api/territorial-wars/leaderboards/top-performing-players?map_id&player_name    [{position, player_*, clan_name, kills, ..., earnings}]
 *   /api/territorial-wars/player/:id                      {player_id, player_name, current_clan(_id, _logo), total_kills,
 *        total_deaths, kd, total_games, total_kr}
 *   /api/territorial-wars/player/:id/contributions[?day]  [{day, map_name, clan_id, clan_name, kills, deaths, kd, games}]
 *   /api/territorial-wars/player/:id/earnings             [{day, map_name, clan_id, clan_name, amount}]
 *   GET|PUT /api/clans/:id/loot-config (PUT: token)       the clan's loot distribution
 *   /api/clans/recruitment?page&limit&region&language     {posts, total, page, limit} (0208)
 *   /api/clans/my-applications?token                      {clan_ids}
 *   GET|POST|PUT|DELETE /api/clans/:id/recruitment        the clan's post (owner / Commander; DELETE also staff)
 *   POST /api/clans/:id/apply?token                       a join request (clan_requests)
 * Every payload is v10's (the client reads body.data); :id is a clan id or (KrunkNative) a clan tag
 * / player name. Views are built by P_TURF_VIEW (db_turf.c) on every driver.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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


static const char *nz(const char *s) { return s ? s : ""; }

/* ---- v10's views: P_TURF_VIEW (db_turf.c) builds the payload, the route wraps it {data: ...}
 * (v10's api client returns body.data; errors {error, message}) */

typedef struct View {
    kn_http_req *req;
} View;

/* "updated_ms" (epoch ms) -> v10's "updated_at" (ISO 8601 UTC) */
static void iso_updated(cJSON *o) {
    cJSON *ms = cJSON_GetObjectItemCaseSensitive(o, "updated_ms");
    if (!cJSON_IsNumber(ms)) return;
    const time_t t = (time_t) (ms->valuedouble / 1000.0);
    struct tm tm;
    char iso[40] = "";
#ifdef _WIN32
    const int ok = gmtime_s(&tm, &t) == 0;
#else
    const int ok = gmtime_r(&t, &tm) != NULL;
#endif
    if (ms->valuedouble > 0 && ok) {
        char base[32];
        strftime(base, sizeof base, "%Y-%m-%dT%H:%M:%S", &tm);
        snprintf(iso, sizeof iso, "%s.%03dZ", base, (int) ((long long) ms->valuedouble % 1000));
    }
    cJSON_DeleteItemFromObjectCaseSensitive(o, "updated_ms");
    if (iso[0]) cJSON_AddStringToObject(o, "updated_at", iso);
    else cJSON_AddNullToObject(o, "updated_at");
}

static void on_view(kn_db_result *res, void *ud) {
    View *v = ud;
    if (!kn_db_ok(res)) {
        const char *msg = kn_db_user_message(res);
        if (!msg) log_err("turf view", res);
        reply_error(v->req, msg && !strcmp(msg, "Not found") ? 404 : msg ? 400 : 500, msg ? msg : "internal");
    } else {
        const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : NULL;
        cJSON *o = cJSON_CreateObject();
        cJSON *data = t ? kn_json_parse(t, strlen(t)) : NULL;
        if (cJSON_IsObject(data)) iso_updated(data);
        cJSON_AddItemToObject(o, "data", data ? data : cJSON_CreateNull());
        kn_http_set_header(v->req, "Cache-Control", "no-cache, no-store, must-revalidate");
        brain_reply_json(v->req, 200, o);
        cJSON_Delete(o);
    }
    free(v);
}

#define S(v) i2s((char[24]){0}, (v))
static const char *i2s(char *b, const long long v) {
    snprintf(b, 24, "%lld", v);
    return b;
}

/* (view, a, b, c, d) */
static void view(kn_http_req *req, const char *name, const char *a, const char *b, const char *c, const char *d) {
    if (!db_ready()) return reply_error(req, 503, "TurfWarsUnavailable");
    View *v = calloc(1, sizeof(*v));
    if (!v) return reply_error(req, 500, "internal");
    v->req = req;
    kn_db_call(T.b->db, &P_TURF_VIEW, KN_DB_ARGS(name, S(current_day()), nz(a), nz(b), nz(c), nz(d)), on_view, v);
}

static void query(kn_http_req *req, const char *key, char *out, size_t n) {
    out[0] = 0;
    if (kn_http_query_get(req, key, out, n) <= 0) out[0] = 0;
}

static void h_status(kn_http_req *req, void *ud) {
    (void) ud;
    char per[16], elapsed[24];
    snprintf(per, sizeof per, "%d", T.kr_per_kill);
    snprintf(elapsed, sizeof elapsed, "%lld", (long long) ((kn_wall_ms() / 1000) % 86400)); /* seconds into the UTC day */
    view(req, "status", per, elapsed, NULL, NULL);
}

static void h_clan(kn_http_req *req, void *ud) {
    (void) ud;
    view(req, "clan", kn_http_param(req, "id"), NULL, NULL, NULL);
}

static void h_clan_players(kn_http_req *req, void *ud) {
    (void) ud;
    char day[16], map[16];
    query(req, "day", day, sizeof day);
    query(req, "map_id", map, sizeof map);
    view(req, "players", kn_http_param(req, "id"), day, map, NULL);
}

static void h_clan_earnings(kn_http_req *req, void *ud) {
    (void) ud;
    view(req, "earnings", kn_http_param(req, "id"), NULL, NULL, NULL);
}

static void h_clan_payouts(kn_http_req *req, void *ud) {
    (void) ud;
    char day[16];
    query(req, "day", day, sizeof day); /* none: the all-time totals */
    view(req, "payouts", kn_http_param(req, "id"), day, NULL, NULL);
}

static void h_top_clans(kn_http_req *req, void *ud) {
    (void) ud;
    char day[16], map[16], name[40];
    query(req, "day", day, sizeof day);
    query(req, "map_id", map, sizeof map);
    query(req, "clan_name", name, sizeof name);
    view(req, "top_clans", day, map, name, NULL);
}

static void h_top_grossing(kn_http_req *req, void *ud) {
    (void) ud;
    char name[40];
    query(req, "clan_name", name, sizeof name);
    view(req, "top_grossing", name, NULL, NULL, NULL);
}

static void h_top_players(kn_http_req *req, void *ud) {
    (void) ud;
    char map[16], name[40];
    query(req, "map_id", map, sizeof map);
    query(req, "player_name", name, sizeof name);
    view(req, "top_players", map, name, NULL, NULL);
}

static void h_player(kn_http_req *req, void *ud) {
    (void) ud;
    view(req, "player", kn_http_param(req, "id"), NULL, NULL, NULL);
}

static void h_player_contrib(kn_http_req *req, void *ud) {
    (void) ud;
    char day[16];
    query(req, "day", day, sizeof day);
    view(req, "contributions", kn_http_param(req, "id"), day, NULL, NULL);
}

static void h_player_earnings(kn_http_req *req, void *ud) {
    (void) ud;
    view(req, "player_earnings", kn_http_param(req, "id"), NULL, NULL, NULL);
}

/* ---- recruitment (0208_clan_recruitment): GET /api/clans/recruitment?page&limit&region&language */

static void h_recruitment_list(kn_http_req *req, void *ud) {
    (void) ud;
    char page[16], limit[16], region[8], lang[8];
    query(req, "page", page, sizeof page);
    query(req, "limit", limit, sizeof limit);
    query(req, "region", region, sizeof region);
    query(req, "language", lang, sizeof lang);
    view(req, "recruitment", page, limit, region, lang);
}

static void h_recruitment_get(kn_http_req *req, void *ud) {
    (void) ud;
    view(req, "recruitment_of", kn_http_param(req, "id"), NULL, NULL, NULL);
}

/* the signed-in account of a request (token query or Bearer), then fn */
typedef struct Authed {
    kn_http_req *req;
    char clan[64];
    char *body;
    int op;            /* 0 my-applications, 1 recruitment set, 2 recruitment delete, 3 apply */
} Authed;

static void authed_done(Authed *a) {
    free(a->body);
    free(a);
}

static void on_authed_proc(kn_db_result *res, void *ud) {
    Authed *a = ud;
    View *v = calloc(1, sizeof(*v));
    if (v) {
        v->req = a->req;
        on_view(res, v); /* frees v */
    } else {
        reply_error(a->req, 500, "internal");
    }
    authed_done(a);
}

static void on_authed_account(void *ud, const char *err, const cJSON *acc) {
    Authed *a = ud;
    const int64_t id = acc && !err ? kn_json_int(acc, "id", 0) : 0;
    if (id <= 0) {
        reply_error(a->req, 401, "LoginRequired");
        authed_done(a);
        return;
    }
    char acc_id[24], extra[24];
    snprintf(acc_id, sizeof acc_id, "%lld", (long long) id);
    switch (a->op) {
        case 0: {
            View *v = calloc(1, sizeof(*v));
            if (!v) {
                reply_error(a->req, 500, "internal");
                break;
            }
            v->req = a->req;
            kn_db_call(T.b->db, &P_TURF_VIEW, KN_DB_ARGS("my_applications", S(current_day()), acc_id, "", "", ""), on_view, v);
            break;
        }
        case 1:
            kn_db_call(T.b->db, &P_TURF_RECRUIT_SET, KN_DB_ARGS(a->clan, acc_id, a->body ? a->body : "{}"), on_authed_proc, a);
            return;
        case 2:
            snprintf(extra, sizeof extra, "%d", kn_json_int(acc, "mod", 0) > 0 || kn_json_int(acc, "dev", 0) > 0 ||
                                                       cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(acc, "mod")) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(acc, "dev")));
            kn_db_call(T.b->db, &P_TURF_RECRUIT_DEL, KN_DB_ARGS(a->clan, acc_id, extra), on_authed_proc, a);
            return;
        default:
            snprintf(extra, sizeof extra, "%lld", (long long) kn_json_int(acc, "level", 0));
            kn_db_call(T.b->db, &P_TURF_APPLY, KN_DB_ARGS(a->clan, acc_id, extra), on_authed_proc, a);
            return;
    }
    authed_done(a);
}

static void authed(kn_http_req *req, const int op) {
    if (!db_ready()) return reply_error(req, 503, "TurfWarsUnavailable");
    char token[512] = "";
    if (kn_http_query_get(req, "token", token, sizeof token) <= 0) {
        const char *bearer = kn_http_bearer(req);
        snprintf(token, sizeof token, "%s", bearer ? bearer : "");
    }
    if (!token[0]) return reply_error(req, 401, "LoginRequired");
    Authed *a = calloc(1, sizeof(*a));
    if (!a) return reply_error(req, 500, "internal");
    a->req = req;
    a->op = op;
    snprintf(a->clan, sizeof a->clan, "%s", nz(kn_http_param(req, "id")));
    if (op == 1) {
        cJSON *body = kn_http_body_json(req);
        if (!body) { /* GET-style clients: ?post=<json> */
            char post[1400];
            if (kn_http_query_get(req, "post", post, sizeof post) > 0) body = kn_json_parse(post, strlen(post));
        }
        a->body = body ? kn_json_print(body) : NULL;
        cJSON_Delete(body);
    }
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(T.b, "token", args, on_authed_account, a);
    cJSON_Delete(args);
}

static void h_my_applications(kn_http_req *req, void *ud) {
    (void) ud;
    authed(req, 0);
}
static void h_recruitment_set(kn_http_req *req, void *ud) {
    (void) ud;
    authed(req, 1);
}
static void h_recruitment_del(kn_http_req *req, void *ud) {
    (void) ud;
    authed(req, 2);
}
static void h_apply(kn_http_req *req, void *ud) {
    (void) ud;
    authed(req, 3);
}

/* ---- loot config */

/* v10's flat {data: {distribution_mode, clan_pct, top_player_count, top_player_pct, remainder_pct,
 * min_games_required, updated_by_name, updated_at}}; plus {config: [row], current_day}, the shape
 * the older clan window read */
static void on_loot_get(kn_db_result *res, void *ud) {
    View *v = ud;
    if (!kn_db_ok(res)) {
        log_err("loot config", res);
        reply_error(v->req, 500, "internal");
    } else if (kn_db_rows(res) < 1) {
        reply_error(v->req, 404, "Clan not found");
    } else {
        cJSON *o = cJSON_CreateObject();
        cJSON *data = kn_db_row_to_json(res, 0);
        iso_updated(data);
        cJSON_AddItemToObject(o, "data", data);
        cJSON_AddNumberToObject(data, "current_day", current_day());
        cJSON *config = cJSON_AddArrayToObject(data, "config");
        cJSON *row = cJSON_Duplicate(data, 1);
        cJSON_DeleteItemFromObjectCaseSensitive(row, "config");
        cJSON_DeleteItemFromObjectCaseSensitive(row, "current_day");
        cJSON_AddItemToArray(config, row);
        kn_http_set_header(v->req, "Cache-Control", "no-cache, no-store, must-revalidate");
        brain_reply_json(v->req, 200, o);
        cJSON_Delete(o);
    }
    free(v);
}

static void h_loot_get(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) return reply_error(req, 503, "TurfWarsUnavailable");
    View *v = calloc(1, sizeof(*v));
    if (!v) return reply_error(req, 500, "internal");
    v->req = req;
    static const kn_sql sql = {.any = "SELECT c.clan_id AS clan_id__i, coalesce(l.distribution_mode, 'clan_bank') AS distribution_mode,"
                                      " coalesce(l.clan_pct, 0) AS clan_pct__i, coalesce(l.top_player_count, 20) AS top_player_count__i,"
                                      " coalesce(l.top_player_pct, 80) AS top_player_pct__i, coalesce(l.remainder_pct, 20) AS remainder_pct__i,"
                                      " coalesce(l.min_games_required, 1) AS min_games_required__i, coalesce(l.updated_by, '') AS updated_by,"
                                      " coalesce(l.updated_by, '') AS updated_by_name, {epoch_ms:l.updated_at} AS updated_ms__i"
                                      " FROM clans c LEFT JOIN turf_loot_config l ON l.clan_id = c.clan_id"
                                      " WHERE c.clan_id = (SELECT clan_id FROM clans WHERE CAST(clan_id AS TEXT) = $1 OR lower(clan_name) = lower($1) LIMIT 1)"};
    kn_db_query_sql(T.b->db, &sql, KN_DB_ARGS(nz(kn_http_param(req, "id"))), on_loot_get, v);
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
        cJSON *data = kn_json_parse(t, strlen(t));
        if (cJSON_IsObject(data)) iso_updated(data); /* v10's {updated_by_name, updated_at} */
        cJSON_AddItemToObject(o, "data", data);
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
    kn_http_route(b->http, "GET", "/api/clans/recruitment", h_recruitment_list, b);
    kn_http_route(b->http, "GET", "/api/clans/my-applications", h_my_applications, b);
    kn_http_route(b->http, "GET", "/api/clans/:id/recruitment", h_recruitment_get, b);
    kn_http_route(b->http, "POST", "/api/clans/:id/recruitment", h_recruitment_set, b);
    kn_http_route(b->http, "PUT", "/api/clans/:id/recruitment", h_recruitment_set, b);
    kn_http_route(b->http, "DELETE", "/api/clans/:id/recruitment", h_recruitment_del, b);
    kn_http_route(b->http, "POST", "/api/clans/:id/apply", h_apply, b);
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
