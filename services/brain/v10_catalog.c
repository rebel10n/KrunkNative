/*
 * services/brain/v10_catalog.c - the v10 catalog routes on the brain root (api.krunker.io and
 * gapi.svc.krunker.io both answer at "/"): the skins, the shop's wild offer and the published maps.
 * Route rows of doc/v10/api-inventory.md; the v10.0.3 wrappers are at decoded L5161, the consumers
 * are v10.js lines.
 *
 *   GET    /skins                              EMPTY {skins: []}: a custom skin is a Skin Manager
 *                                              record; KrunkNative has none (studios.c, db_studios.c)
 *   GET    /skins/:id                          NOT_FOUND: no custom skin has a KrunkNative id
 *   POST   /skins, PUT|DELETE /skins/:id,
 *   POST   /skins/:id/upload|duplicate         REFUSAL: the Skin Manager and its asset store do not exist
 *   GET    /shop/wild-offer                    the day's offer (wild_offer.c P_WILD_OFFER); {offer: null}
 *                                              with no session
 *   POST   /shop/wild-offer/purchase {offer_id}  P_WILD_OFFER_BUY: KR from the account's funds, the skin
 *                                              granted once; a refusal is {success: false, message}
 *   POST   /shop/wild-offer/refresh {offer_id}  P_WILD_OFFER_REFRESH: refresh_cost KR for the next offer
 *   GET    /maps/by-name/:name/leaderboard     the published map's Leaderboard Display (its LEADERBOARD
 *                                              object: ldt 0 Score / 1 Time, ldo 1 DESC, ldc a custom
 *                                              stat, which KrunkNative has no source for, so empty) over
 *                                              match_results.map; {entries, total}; an official map (no
 *                                              published row) has no board
 *   POST   /maps/:id/vote {vote}               creation_votes (kind 0, one per account; 0 removes it);
 *                                              published_maps.map_votes is their sum
 *   POST   /maps/:id/feature {featured}        developers (creations.c ftC) set map_featured
 *   DELETE /maps/:id                           soft delete (deleted_at) by the creator, developers and map
 *                                              mods (creations.c delC)
 *   GET    /maps/:id/versions                  EMPTY []: a map keeps one current map_data, no versions
 *   POST   /maps/:id/versions/:v/revert        REFUSAL: there are no versions to revert to
 *   POST   /maps/:id/funds {amount}            the creator moves KR between the account and the map's
 *                                              funds (map_funds; > 0 deposit, < 0 withdraw), the ledger
 *                                              rows source map_funds
 *   POST   /maps/:id/leaderboard {type, custom_type, order}  the creator's, developers' and map mods'
 *                                              LEADERBOARD settings written as creations.c's ulb does
 *   GET    /maps/:id/data?player=              the player's ks_storage rows of this map as {mk, kv}
 *                                              (the owner, developers and map mods; others see [])
 *   DELETE /maps/:id/data?key=[&player=]       one key of one player, or the key for every player
 *
 * Accounts: the Bearer (or ?token=) is resolved through accounts_internal_call, as ranked.c does. A write
 * with no session is 401 UNAUTHORIZED; a map the caller may not act on is 404 MAP_NOT_FOUND (the same
 * answer creations.c gives, so a map's staff are not revealed by an error).
 */
#include "v10_catalog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acct_internal.h"
#include "db_procs.h"
#include "shared.h" // PREFAB_LEADERBOARD (a map object's "i")
#include "v10_api.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/loop.h"

#define WHY_SKIN "Box Office has no Skin Manager: custom skins are not stored"
#define WHY_SKIN_ASSET "Box Office has no asset store for skin files"
#define WHY_VERSIONS "Box Office keeps no map versions to revert to"
#define MSG_NO_MAP "Map not found"
#define MSG_LOGIN "You must be logged in."
#define BOARD_PAGE 20

static Brain *g_b; /* the brain this process runs: the accounts bridge and the database */

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))
static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

/* ---- replies */

/* {success: false, message}: a shop refusal the consumer shows (v10.js 126583, 126704) */
static void reply_refused(kn_http_req *req, const char *msg) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "success", 0);
    cJSON_AddStringToObject(d, "message", msg);
    v10_reply_data(req, 200, d);
}

static void reply_no_db(kn_http_req *req) { v10_reply_error(req, 503, "UNAVAILABLE", "Box Office has no database"); }

/* a map refusal from a proc: "Map not found" is 404, the rest a 400 with the message */
static void reply_map_refused(kn_http_req *req, const char *msg) {
    if (!strcmp(msg, MSG_NO_MAP)) v10_reply_error(req, 404, "MAP_NOT_FOUND", MSG_NO_MAP);
    else if (strstr(msg, "Not enough")) v10_reply_error(req, 400, "INSUFFICIENT_FUNDS", msg);
    else v10_reply_error(req, 400, "REFUSED", msg);
}

/* ---- the caller: the Bearer (or ?token=) through the accounts bridge, as ranked.c's with_account */

typedef void (*CatAccFn)(kn_http_req *req, int64_t id, void *ud);

typedef struct CatAcc {
    kn_http_req *req;
    CatAccFn fn;
    void *ud;
} CatAcc;

static void on_account(void *ud, const char *err, const cJSON *acc) {
    CatAcc *c = ud;
    const int64_t id = !err && acc ? kn_json_int(acc, "id", 0) : 0;
    kn_http_req *req = c->req;
    CatAccFn fn = c->fn;
    void *arg = c->ud;
    free(c);
    fn(req, id, arg); /* id 0: no valid session */
}

static void with_account(kn_http_req *req, CatAccFn fn, void *ud) {
    char token[512] = "";
    if (kn_http_query_get(req, "token", token, sizeof token) <= 0) {
        const char *bearer = kn_http_bearer(req);
        snprintf(token, sizeof token, "%s", bearer ? bearer : "");
    }
    if (!token[0] || !g_b) {
        fn(req, 0, ud);
        return;
    }
    CatAcc *c = calloc(1, sizeof *c);
    if (!c) {
        fn(req, 0, ud); /* out of memory: the request goes on as signed out, and ud is still the callee's */
        return;
    }
    c->req = req;
    c->fn = fn;
    c->ud = ud;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(g_b, "token", args, on_account, c);
    cJSON_Delete(args);
}

/* ---- request inputs */

/* the route's :name parameter as a positive id, or -1 */
static int64_t path_id(kn_http_req *req, const char *name) {
    const char *s = kn_http_param(req, name);
    char *end = NULL;
    const long long v = s ? strtoll(s, &end, 10) : -1;
    return (s && end && end != s && *end == 0 && v > 0) ? (int64_t) v : -1;
}

/* a query value into out (cap bytes); 0 when absent */
static int query_str(kn_http_req *req, const char *key, char *out, size_t cap) {
    out[0] = 0;
    return kn_http_query_get(req, key, out, cap) > 0;
}

static int64_t query_int(kn_http_req *req, const char *key, int64_t def) {
    char v[24];
    return query_str(req, key, v, sizeof v) ? strtoll(v, NULL, 10) : def;
}

/* ---- skins: the Skin Manager does not exist here */

static void h_skins_list(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "skins", cJSON_CreateArray());
    v10_reply_data(req, 200, d);
}

static void h_skin_not_found(kn_http_req *req, void *ud) {
    (void) ud;
    v10_reply_error(req, 404, "NOT_FOUND", "Skin not found");
}

static void h_refuse(kn_http_req *req, void *ud) { v10_reply_error(req, 403, "NOT_IMPLEMENTED", ud); }

/* ---- the wild offer (wild_offer.c's procs; the account and the clock are the same as the social frames') */

typedef struct CatOffer {
    kn_http_req *req;
    int refresh;
    int64_t offer;
} CatOffer;

static void on_offer_get(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    cJSON *v = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!v) {
        v10_reply_error(req, 500, "INTERNAL_ERROR", acct_db_error(res, "wild offer"));
        return;
    }
    v10_reply_data(req, 200, v); /* {offer} */
}

static void offer_get_account(kn_http_req *req, int64_t id, void *ud) {
    (void) ud;
    if (id <= 0) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddNullToObject(d, "offer");
        v10_reply_data(req, 200, d);
        return;
    }
    char ids[24], now[24];
    acct_itoa(ids, id);
    acct_itoa(now, kn_wall_ms());
    kn_db_call(g_b->db, &P_WILD_OFFER, KN_DB_ARGS(ids, now), on_offer_get, req);
}

static void on_offer_op(kn_db_result *res, void *ud) {
    CatOffer *o = ud;
    kn_http_req *req = o->req;
    if (kn_db_ok(res)) {
        cJSON *v = acct_db_json(res);
        v10_reply_data(req, 200, v ? v : cJSON_CreateNull());
    } else if (kn_db_user_message(res)) {
        reply_refused(req, kn_db_user_message(res));
    } else {
        v10_reply_error(req, 500, "INTERNAL_ERROR", acct_db_error(res, "wild offer"));
    }
    free(o);
}

static void offer_op_account(kn_http_req *req, int64_t id, void *ud) {
    CatOffer *o = ud;
    if (id <= 0) {
        reply_refused(req, "Log in to buy or refresh a wild offer");
        free(o);
        return;
    }
    char ids[24], now[24], oid[24], cost[24];
    acct_itoa(ids, id);
    acct_itoa(now, kn_wall_ms());
    acct_itoa(oid, o->offer);
    acct_itoa(cost, wild_offer_refresh_cost());
    kn_db_call(g_b->db, o->refresh ? &P_WILD_OFFER_REFRESH : &P_WILD_OFFER_BUY, KN_DB_ARGS(ids, now, oid, cost),
               on_offer_op, o);
}

static void offer_route(kn_http_req *req, int refresh) {
    if (!g_b->db) { reply_no_db(req); return; }
    cJSON *body = kn_http_body_json(req);
    const cJSON *oid = body ? cJSON_GetObjectItemCaseSensitive(body, "offer_id") : NULL;
    if (!cJSON_IsNumber(oid)) {
        cJSON_Delete(body);
        reply_refused(req, "Invalid wild offer");
        return;
    }
    CatOffer *o = calloc(1, sizeof *o);
    if (!o) { v10_reply_error(req, 500, "INTERNAL_ERROR", "Server error."); return; }
    o->req = req;
    o->refresh = refresh;
    o->offer = (int64_t) oid->valuedouble;
    cJSON_Delete(body);
    with_account(req, offer_op_account, o);
}

static void h_wild_offer(kn_http_req *req, void *ud) {
    (void) ud;
    if (!g_b->db) { reply_no_db(req); return; }
    with_account(req, offer_get_account, NULL);
}

static void h_wild_purchase(kn_http_req *req, void *ud) {
    (void) ud;
    offer_route(req, 0);
}

static void h_wild_refresh(kn_http_req *req, void *ud) {
    (void) ud;
    offer_route(req, 1);
}

/* ---- the map procs: one transaction each (db_procs.h helpers, the db_storage.c pattern) */

/* may manage a map: its creator, a developer or a map mod (creations.c ulb / delC) */
#define MANAGE_SQL "(creator_id = $1 OR EXISTS (SELECT 1 FROM players WHERE player_id = $1 AND (player_developer > 0 OR player_mapmod > 0)))"

/* (me, map, vote -1|0|1) -> {vote, map_votes}: one vote per account on a published map (creation_votes kind 0) */
static kn_db_result *run_map_vote(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t vote = PI(2);
    if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM published_maps WHERE map_id = $1 AND deleted_at IS NULL"), KN_DB_ARGS(P(1)))) {
        if (!dbx_failed(&x)) dbx_refuse(&x, MSG_NO_MAP);
        return dbx_finish(&x, NULL);
    }
    if (dbx_exec(&x, SQL(.any = "DELETE FROM creation_votes WHERE kind = 0 AND creation_id = $2 AND account_id = $1"),
                 KN_DB_ARGS(P(0), P(1))) < 0)
        return dbx_finish(&x, NULL);
    if (vote != 0 && dbx_exec(&x, SQL(.any = "INSERT INTO creation_votes (kind, creation_id, account_id, vote) VALUES (0, $2, $1, {int:$3})"),
                              KN_DB_ARGS(P(0), P(1), I64S(vote))) < 0)
        return dbx_finish(&x, NULL);
    if (dbx_exec(&x, SQL(.any = "UPDATE published_maps SET map_votes = (SELECT coalesce(sum(v.vote), 0) FROM creation_votes v"
                                " WHERE v.kind = 0 AND v.creation_id = $1) WHERE map_id = $1"),
                 KN_DB_ARGS(P(1))) < 0)
        return dbx_finish(&x, NULL);
    const int64_t total = dbx_int(&x, SQL(.any = "SELECT map_votes FROM published_maps WHERE map_id = $1"), KN_DB_ARGS(P(1)), 0);
    cJSON *v = cJSON_CreateObject();
    cJSON_AddNumberToObject(v, "vote", (double) vote);
    cJSON_AddNumberToObject(v, "map_votes", (double) total);
    return dbx_json(&x, "v10_map_vote", v);
}
static const kn_proc P_MAP_VOTE = {"v10_map_vote", NULL, run_map_vote};

/* (me, map, amount) -> {map_funds, player_funds}: the creator's deposit (> 0) or withdrawal (< 0) */
static kn_db_result *run_map_funds(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t amount = PI(2);
    char ref[24];
    snprintf(ref, sizeof ref, "%s", P(1) ? P(1) : "");
    if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM published_maps WHERE map_id = $2 AND creator_id = $1 AND deleted_at IS NULL"),
                    KN_DB_ARGS(P(0), P(1)))) {
        if (!dbx_failed(&x)) dbx_refuse(&x, MSG_NO_MAP);
        return dbx_finish(&x, NULL);
    }
    int64_t player = 0;
    if (amount > 0) {
        if (db_add_funds(&x, PI(0), -amount, "map_funds", ref, &player) < 0) return dbx_finish(&x, NULL);
        if (dbx_exec(&x, SQL(.any = "UPDATE published_maps SET map_funds = map_funds + {int:$3} WHERE map_id = $2"),
                     KN_DB_ARGS(P(0), P(1), I64S(amount))) < 0)
            return dbx_finish(&x, NULL);
    } else {
        if (dbx_exec(&x, SQL(.any = "UPDATE published_maps SET map_funds = map_funds - {int:$3} WHERE map_id = $2 AND map_funds >= {int:$3}"),
                     KN_DB_ARGS(P(0), P(1), I64S(-amount))) == 0) {
            dbx_refuse(&x, "Not enough map funds");
            return dbx_finish(&x, NULL);
        }
        if (db_add_funds(&x, PI(0), -amount, "map_funds", ref, &player) < 0) return dbx_finish(&x, NULL);
    }
    const int64_t funds = dbx_int(&x, SQL(.any = "SELECT map_funds FROM published_maps WHERE map_id = $1"), KN_DB_ARGS(P(1)), 0);
    cJSON *v = cJSON_CreateObject();
    cJSON_AddNumberToObject(v, "map_funds", (double) funds);
    cJSON_AddNumberToObject(v, "player_funds", (double) player);
    return dbx_json(&x, "v10_map_funds", v);
}
static const kn_proc P_MAP_FUNDS = {"v10_map_funds", NULL, run_map_funds};

/* a LEADERBOARD object's setting: the key goes, and comes back only when v is not zero (creations.c) */
static void lb_set_number(cJSON *o, const char *key, int64_t v) {
    cJSON_DeleteItemFromObjectCaseSensitive(o, key);
    if (v) cJSON_AddNumberToObject(o, key, (double) v);
}

/* (me, map, type 0|1, order 0|1, custom) -> {leaderboard_type, order, custom_type}: the LEADERBOARD
 * objects of the map's data get ldt / ldc / ldo as creations.c's ulb writes them; the map republishes */
static kn_db_result *run_map_lb(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    char *text = dbx_text(&x, SQL(.any = "SELECT map_data FROM published_maps WHERE map_id = $2 AND deleted_at IS NULL AND " MANAGE_SQL),
                          KN_DB_ARGS(P(0), P(1)));
    if (!text) {
        if (!dbx_failed(&x)) dbx_refuse(&x, MSG_NO_MAP);
        return dbx_finish(&x, NULL);
    }
    cJSON *map = cJSON_Parse(text);
    free(text);
    const int64_t type = PI(2), order = PI(3);
    const char *custom = P(4) ? P(4) : "";
    int found = 0;
    cJSON *objs = map ? cJSON_GetObjectItemCaseSensitive(map, "objects") : NULL;
    cJSON *o;
    cJSON_ArrayForEach(o, objs) {
        if (!cJSON_IsObject(o) || kn_json_int(o, "i", -1) != PREFAB_LEADERBOARD) continue;
        lb_set_number(o, "ldt", type);
        cJSON_DeleteItemFromObjectCaseSensitive(o, "ldc");
        if (custom[0]) cJSON_AddStringToObject(o, "ldc", custom);
        lb_set_number(o, "ldo", order);
        found++;
    }
    if (!found) {
        cJSON_Delete(map);
        dbx_refuse(&x, "No leaderboard on this map");
        return dbx_finish(&x, NULL);
    }
    char *out = cJSON_PrintUnformatted(map);
    cJSON_Delete(map);
    if (!out) {
        dbx_refuse(&x, "Map error occured.");
        return dbx_finish(&x, NULL);
    }
    const int64_t written = dbx_exec(&x, SQL(.any = "UPDATE published_maps SET map_data = $2, map_updatecounter = map_updatecounter + 1"
                                                  " WHERE map_id = $1 AND deleted_at IS NULL"),
                                     KN_DB_ARGS(P(1), out));
    free(out);
    if (written < 0) return dbx_finish(&x, NULL);
    cJSON *v = cJSON_CreateObject();
    cJSON_AddNumberToObject(v, "leaderboard_type", (double) type);
    cJSON_AddNumberToObject(v, "order", (double) order);
    cJSON_AddStringToObject(v, "custom_type", custom);
    return dbx_json(&x, "v10_map_lb", v);
}
static const kn_proc P_MAP_LB = {"v10_map_lb", NULL, run_map_lb};

/* (me, map, key, player) -> {deleted}: one key of one player (or of every player, with no player) */
static kn_db_result *run_map_data_delete(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM published_maps WHERE map_id = $2 AND deleted_at IS NULL AND " MANAGE_SQL),
                    KN_DB_ARGS(P(0), P(1)))) {
        if (!dbx_failed(&x)) dbx_refuse(&x, MSG_NO_MAP);
        return dbx_finish(&x, NULL);
    }
    int64_t deleted = 0;
    if (P(3) && *P(3)) {
        const int64_t who = db_account_id(&x, P(3));
        if (who > 0)
            deleted = dbx_exec(&x, SQL(.any = "DELETE FROM ks_storage WHERE map_id = $1 AND skey = $2 AND account_id = $3"),
                               KN_DB_ARGS(P(1), P(2), I64S(who)));
    } else {
        deleted = dbx_exec(&x, SQL(.any = "DELETE FROM ks_storage WHERE map_id = $1 AND skey = $2"), KN_DB_ARGS(P(1), P(2)));
    }
    if (deleted < 0) return dbx_finish(&x, NULL);
    cJSON *v = cJSON_CreateObject();
    cJSON_AddNumberToObject(v, "deleted", (double) deleted);
    return dbx_json(&x, "v10_map_data_delete", v);
}
static const kn_proc P_MAP_DATA_DELETE = {"v10_map_data_delete", NULL, run_map_data_delete};

/* ---- the single-statement map writes and reads (kn_db_query_sql) */

/* featured by a developer (creations.c ftC); affected 0 when the caller is not one, or no such map */
static const kn_sql k_feature = {
    .any = "UPDATE published_maps SET map_featured = $3 WHERE map_id = $2 AND deleted_at IS NULL"
           " AND EXISTS (SELECT 1 FROM players WHERE player_id = $1 AND player_developer > 0)"};

/* the soft delete of creations.c's delC: the creator, developers and map mods */
#define DEL_MAP_SQL(NOW)                                                                                          \
    "UPDATE published_maps SET deleted_at = " NOW " WHERE map_id = $2 AND deleted_at IS NULL AND " MANAGE_SQL
static const kn_sql k_delete = {.pg = DEL_MAP_SQL("now()"), .any = DEL_MAP_SQL("{now}")};
#undef DEL_MAP_SQL

/* the player's rows of this map, for a caller who may manage the map; no rows otherwise */
static const kn_sql k_data_get = {
    .any = "SELECT s.skey, s.svalue FROM ks_storage s JOIN published_maps m ON m.map_id = s.map_id"
           " JOIN accounts a ON a.id = s.account_id"
           " WHERE s.map_id = $2 AND lower(a.username) = lower($3) AND m.deleted_at IS NULL AND " MANAGE_SQL
           " ORDER BY s.skey"};

/* the Leaderboard Display's settings of the published map (a name match, as the by-name route) */
static const kn_sql k_board_settings = {
    .any = "SELECT map_data FROM published_maps WHERE lower(map_name) = lower($1) AND deleted_at IS NULL"
           " ORDER BY map_id LIMIT 1"};

/* the board: match_results on the map, the best value per player (max for DESC, min for ASC) */
#define BOARD_SQL(METRIC, AGG, DIR, FILTER)                                                                      \
    "SELECT m.account_id AS id, a.username AS name, " AGG "(m." METRIC ") AS v FROM match_results m"             \
    " JOIN accounts a ON a.id = m.account_id WHERE lower(m.map) = lower($1)" FILTER                              \
    " GROUP BY m.account_id, a.username ORDER BY v " DIR ", m.account_id LIMIT $2 OFFSET $3"
#define COUNT_SQL(FILTER)                                                                                        \
    "SELECT count(*) FROM (SELECT m.account_id FROM match_results m WHERE lower(m.map) = lower($1)" FILTER        \
    " GROUP BY m.account_id) t"
/* [time 0|1][desc 0|1] */
static const kn_sql k_board[2][2] = {
    {{.any = BOARD_SQL("score", "min", "ASC", "")}, {.any = BOARD_SQL("score", "max", "DESC", "")}},
    {{.any = BOARD_SQL("time_ms", "min", "ASC", " AND m.time_ms > 0")}, {.any = BOARD_SQL("time_ms", "max", "DESC", " AND m.time_ms > 0")}},
};
static const kn_sql k_board_count[2] = {{.any = COUNT_SQL("")}, {.any = COUNT_SQL(" AND m.time_ms > 0")}};
#undef BOARD_SQL
#undef COUNT_SQL

/* ---- the map routes: one account step, then one DB step; the context follows the request */

enum { OP_VOTE, OP_FUNDS, OP_LB, OP_FEATURE, OP_DELETE, OP_DATA_GET, OP_DATA_DELETE };

typedef struct CatOp {
    kn_http_req *req;
    int kind;
    int64_t map;     /* the :id */
    int64_t n1, n2;  /* vote | amount | featured ; type, order */
    char s1[128];    /* custom type | key */
    char s2[160];    /* the player's name */
} CatOp;

static CatOp *op_new(kn_http_req *req, int kind, int64_t map) {
    CatOp *o = calloc(1, sizeof *o);
    if (!o) {
        v10_reply_error(req, 500, "INTERNAL_ERROR", "Server error.");
        return NULL;
    }
    o->req = req;
    o->kind = kind;
    o->map = map;
    return o;
}

static void on_map_proc(kn_db_result *res, void *ud) {
    CatOp *o = ud;
    kn_http_req *req = o->req;
    if (kn_db_ok(res)) {
        cJSON *v = acct_db_json(res);
        v10_reply_data(req, 200, v ? v : cJSON_CreateNull());
    } else if (kn_db_user_message(res)) {
        reply_map_refused(req, kn_db_user_message(res));
    } else {
        v10_reply_error(req, 500, "INTERNAL_ERROR", acct_db_error(res, "map"));
    }
    free(o);
}

static void on_feature(kn_db_result *res, void *ud) {
    CatOp *o = ud;
    kn_http_req *req = o->req;
    if (!kn_db_ok(res)) {
        v10_reply_error(req, 500, "INTERNAL_ERROR", acct_db_error(res, "map feature"));
    } else if (kn_db_affected(res) < 1) {
        reply_map_refused(req, MSG_NO_MAP);
    } else {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "featured", o->n1 != 0);
        cJSON_AddNumberToObject(d, "map_featured", o->n1 != 0 ? 1 : 0);
        v10_reply_data(req, 200, d);
    }
    free(o);
}

static void on_delete(kn_db_result *res, void *ud) {
    CatOp *o = ud;
    kn_http_req *req = o->req;
    if (!kn_db_ok(res)) {
        v10_reply_error(req, 500, "INTERNAL_ERROR", acct_db_error(res, "map delete"));
    } else if (kn_db_affected(res) < 1) {
        reply_map_refused(req, MSG_NO_MAP);
    } else {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "deleted", 1);
        v10_reply_data(req, 200, d);
    }
    free(o);
}

static void on_data_get(kn_db_result *res, void *ud) {
    CatOp *o = ud;
    kn_http_req *req = o->req;
    if (!kn_db_ok(res)) {
        v10_reply_error(req, 500, "INTERNAL_ERROR", acct_db_error(res, "map data"));
    } else {
        cJSON *rows = cJSON_CreateArray();
        for (int i = 0; i < kn_db_rows(res); i++) {
            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "mk", kn_db_get(res, i, 0));
            cJSON_AddStringToObject(row, "kv", kn_db_get(res, i, 1));
            cJSON_AddItemToArray(rows, row);
        }
        v10_reply_data(req, 200, rows);
    }
    free(o);
}

static void op_account(kn_http_req *req, int64_t id, void *ud) {
    CatOp *o = ud;
    if (id <= 0) {
        if (o->kind == OP_DATA_GET) v10_reply_data(req, 200, cJSON_CreateArray());
        else v10_reply_error(req, 401, "UNAUTHORIZED", MSG_LOGIN);
        free(o);
        return;
    }
    char me[24], map[24], n1[24], n2[24];
    acct_itoa(me, id);
    acct_itoa(map, o->map);
    acct_itoa(n1, o->n1);
    acct_itoa(n2, o->n2);
    kn_db *db = g_b->db;
    switch (o->kind) {
    case OP_VOTE:
        kn_db_call(db, &P_MAP_VOTE, KN_DB_ARGS(me, map, n1), on_map_proc, o);
        break;
    case OP_FUNDS:
        kn_db_call(db, &P_MAP_FUNDS, KN_DB_ARGS(me, map, n1), on_map_proc, o);
        break;
    case OP_LB:
        kn_db_call(db, &P_MAP_LB, KN_DB_ARGS(me, map, n1, n2, o->s1), on_map_proc, o);
        break;
    case OP_DATA_DELETE:
        kn_db_call(db, &P_MAP_DATA_DELETE, KN_DB_ARGS(me, map, o->s1, o->s2), on_map_proc, o);
        break;
    case OP_FEATURE:
        kn_db_query_sql(db, &k_feature, KN_DB_ARGS(me, map, n1), on_feature, o);
        break;
    case OP_DELETE:
        kn_db_query_sql(db, &k_delete, KN_DB_ARGS(me, map), on_delete, o);
        break;
    case OP_DATA_GET:
        kn_db_query_sql(db, &k_data_get, KN_DB_ARGS(me, map, o->s2), on_data_get, o);
        break;
    default:
        free(o);
        break;
    }
}

/* starts a map operation for the caller; the map id must be positive (else MAP_NOT_FOUND) */
static void map_route(kn_http_req *req, int kind, int64_t map, int64_t n1, int64_t n2, const char *s1, const char *s2) {
    if (!g_b->db) { reply_no_db(req); return; }
    if (map <= 0) { v10_reply_error(req, 404, "MAP_NOT_FOUND", MSG_NO_MAP); return; }
    CatOp *o = op_new(req, kind, map);
    if (!o) return;
    o->n1 = n1;
    o->n2 = n2;
    snprintf(o->s1, sizeof o->s1, "%s", s1 ? s1 : "");
    snprintf(o->s2, sizeof o->s2, "%s", s2 ? s2 : "");
    with_account(req, op_account, o);
}

/* POST /maps/:id/vote {vote}: 1, 0 (remove) or -1 */
static void h_map_vote(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *body = kn_http_body_json(req);
    const cJSON *v = body ? cJSON_GetObjectItemCaseSensitive(body, "vote") : NULL;
    if (!cJSON_IsNumber(v)) {
        cJSON_Delete(body);
        v10_reply_error(req, 400, "INVALID_VOTE", "A vote is 1, 0 or -1");
        return;
    }
    const int64_t vote = v->valuedouble > 0 ? 1 : v->valuedouble < 0 ? -1 : 0;
    cJSON_Delete(body);
    map_route(req, OP_VOTE, path_id(req, "id"), vote, 0, NULL, NULL);
}

/* POST /maps/:id/feature {featured}: developers */
static void h_map_feature(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *body = kn_http_body_json(req);
    const cJSON *f = body ? cJSON_GetObjectItemCaseSensitive(body, "featured") : NULL;
    int on = 0;
    if (cJSON_IsBool(f)) on = cJSON_IsTrue(f);
    else if (cJSON_IsNumber(f)) on = f->valuedouble != 0;
    else {
        cJSON_Delete(body);
        v10_reply_error(req, 400, "INVALID_FEATURED", "featured must be true or false");
        return;
    }
    cJSON_Delete(body);
    if (!g_b->db) { reply_no_db(req); return; }
    const int64_t map = path_id(req, "id");
    if (map <= 0) { v10_reply_error(req, 404, "MAP_NOT_FOUND", MSG_NO_MAP); return; }
    CatOp *o = op_new(req, OP_FEATURE, map);
    if (!o) return;
    o->n1 = on;
    with_account(req, op_account, o);
}

/* DELETE /maps/:id */
static void h_map_delete(kn_http_req *req, void *ud) {
    (void) ud;
    map_route(req, OP_DELETE, path_id(req, "id"), 0, 0, NULL, NULL);
}

/* POST /maps/:id/funds {amount}: > 0 deposits from the account, < 0 withdraws to it */
static void h_map_funds(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *body = kn_http_body_json(req);
    const cJSON *a = body ? cJSON_GetObjectItemCaseSensitive(body, "amount") : NULL;
    if (!cJSON_IsNumber(a) || (int64_t) a->valuedouble == 0) {
        cJSON_Delete(body);
        v10_reply_error(req, 400, "INVALID_AMOUNT", "An amount of KR is required");
        return;
    }
    const int64_t amount = (int64_t) a->valuedouble;
    cJSON_Delete(body);
    map_route(req, OP_FUNDS, path_id(req, "id"), amount, 0, NULL, NULL);
}

/* POST /maps/:id/leaderboard {type, custom_type, order} */
static void h_map_leaderboard(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *body = kn_http_body_json(req);
    const int64_t type = kn_json_int(body, "type", 0) == 1 ? 1 : 0;
    const int64_t order = kn_json_int(body, "order", 0) == 1 ? 1 : 0;
    char custom[128];
    snprintf(custom, sizeof custom, "%s", kn_json_str(body, "custom_type", ""));
    cJSON_Delete(body);
    map_route(req, OP_LB, path_id(req, "id"), type, order, custom, NULL);
}

/* GET /maps/:id/versions: no version store */
static void h_map_versions(kn_http_req *req, void *ud) {
    (void) ud;
    v10_reply_data(req, 200, cJSON_CreateArray());
}

/* POST /maps/:id/versions/:v/revert */
static void h_map_revert(kn_http_req *req, void *ud) {
    (void) ud;
    v10_reply_error(req, 403, "NOT_IMPLEMENTED", WHY_VERSIONS);
}

/* GET /maps/:id/data?player= */
static void h_map_data_get(kn_http_req *req, void *ud) {
    (void) ud;
    char player[160];
    query_str(req, "player", player, sizeof player);
    map_route(req, OP_DATA_GET, path_id(req, "id"), 0, 0, NULL, player);
}

/* DELETE /maps/:id/data?key=&player= */
static void h_map_data_delete(kn_http_req *req, void *ud) {
    (void) ud;
    char key[128], player[160];
    if (!query_str(req, "key", key, sizeof key)) { v10_reply_error(req, 400, "INVALID_KEY", "A key is required"); return; }
    query_str(req, "player", player, sizeof player);
    map_route(req, OP_DATA_DELETE, path_id(req, "id"), 0, 0, key, player);
}

/* ---- GET /maps/by-name/:name/leaderboard */

typedef struct CatBoard {
    kn_http_req *req;
    char name[160];
    int64_t offset;
    int time, desc;
    cJSON *entries;
} CatBoard;

static void board_empty(CatBoard *b) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "entries", cJSON_CreateArray());
    cJSON_AddNumberToObject(d, "total", 0);
    v10_reply_data(b->req, 200, d);
    cJSON_Delete(b->entries);
    free(b);
}

static void on_board_count(kn_db_result *res, void *ud) {
    CatBoard *b = ud;
    if (!kn_db_ok(res)) {
        v10_reply_error(b->req, 500, "INTERNAL_ERROR", acct_db_error(res, "map leaderboard"));
    } else {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddItemToObject(d, "entries", b->entries);
        b->entries = NULL;
        cJSON_AddNumberToObject(d, "total", (double) kn_db_get_int64(res, 0, 0, 0));
        v10_reply_data(b->req, 200, d);
    }
    cJSON_Delete(b->entries);
    free(b);
}

static void on_board_rows(kn_db_result *res, void *ud) {
    CatBoard *b = ud;
    if (!kn_db_ok(res)) {
        v10_reply_error(b->req, 500, "INTERNAL_ERROR", acct_db_error(res, "map leaderboard"));
        cJSON_Delete(b->entries);
        free(b);
        return;
    }
    for (int i = 0; i < kn_db_rows(res); i++) {
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "position", (double) (b->offset + i + 1));
        cJSON_AddStringToObject(e, "player_name", kn_db_get(res, i, 1));
        cJSON_AddNumberToObject(e, "value", (double) kn_db_get_int64(res, i, 2, 0));
        cJSON_AddItemToArray(b->entries, e);
    }
    char name[160];
    snprintf(name, sizeof name, "%s", b->name);
    kn_db_query_sql(g_b->db, &k_board_count[b->time], KN_DB_ARGS(name), on_board_count, b);
}

static void on_board_settings(kn_db_result *res, void *ud) {
    CatBoard *b = ud;
    if (!kn_db_ok(res)) {
        v10_reply_error(b->req, 500, "INTERNAL_ERROR", acct_db_error(res, "map leaderboard"));
        cJSON_Delete(b->entries);
        free(b);
        return;
    }
    cJSON *map = kn_db_rows(res) > 0 ? cJSON_Parse(kn_db_get(res, 0, 0)) : NULL;
    cJSON *lb = NULL;
    cJSON *objs = map ? cJSON_GetObjectItemCaseSensitive(map, "objects") : NULL;
    cJSON *o;
    cJSON_ArrayForEach(o, objs) {
        if (cJSON_IsObject(o) && kn_json_int(o, "i", -1) == PREFAB_LEADERBOARD) {
            lb = o;
            break;
        }
    }
    /* no Leaderboard object, or a custom stat (no KrunkNative source): the empty board */
    if (!lb || kn_json_str(lb, "ldc", "")[0]) {
        cJSON_Delete(map);
        board_empty(b);
        return;
    }
    b->time = kn_json_int(lb, "ldt", 0) == 1;
    b->desc = kn_json_int(lb, "ldo", 0) == 1;
    cJSON_Delete(map);
    char name[160], offset[24], limit[24];
    snprintf(name, sizeof name, "%s", b->name);
    acct_itoa(limit, BOARD_PAGE);
    acct_itoa(offset, b->offset);
    kn_db_query_sql(g_b->db, &k_board[b->time][b->desc], KN_DB_ARGS(name, limit, offset), on_board_rows, b);
}

static void h_map_board(kn_http_req *req, void *ud) {
    (void) ud;
    if (!g_b->db) { reply_no_db(req); return; }
    const char *name = kn_http_param(req, "name");
    CatBoard *b = calloc(1, sizeof *b);
    if (!b) { v10_reply_error(req, 500, "INTERNAL_ERROR", "Server error."); return; }
    const int64_t page = query_int(req, "page", 1);
    b->req = req;
    snprintf(b->name, sizeof b->name, "%s", name ? name : "");
    b->offset = (page > 1 ? page - 1 : 0) * BOARD_PAGE;
    b->entries = cJSON_CreateArray();
    char name_arg[160];
    snprintf(name_arg, sizeof name_arg, "%s", b->name);
    kn_db_query_sql(g_b->db, &k_board_settings, KN_DB_ARGS(name_arg), on_board_settings, b);
}

/* ---- the route table */

int v10_catalog_init(Brain *b) {
    g_b = b;
    if (!b->http) return 0;
    kn_http_route(b->http, "GET", "/skins", h_skins_list, NULL);
    kn_http_route(b->http, "GET", "/skins/:id", h_skin_not_found, NULL);
    kn_http_route(b->http, "POST", "/skins", h_refuse, (void *) WHY_SKIN);
    kn_http_route(b->http, "PUT", "/skins/:id", h_refuse, (void *) WHY_SKIN);
    kn_http_route(b->http, "DELETE", "/skins/:id", h_refuse, (void *) WHY_SKIN);
    kn_http_route(b->http, "POST", "/skins/:id/upload", h_refuse, (void *) WHY_SKIN_ASSET);
    kn_http_route(b->http, "POST", "/skins/:id/duplicate", h_refuse, (void *) WHY_SKIN);
    kn_http_route(b->http, "GET", "/shop/wild-offer", h_wild_offer, NULL);
    kn_http_route(b->http, "POST", "/shop/wild-offer/purchase", h_wild_purchase, NULL);
    kn_http_route(b->http, "POST", "/shop/wild-offer/refresh", h_wild_refresh, NULL);
    kn_http_route(b->http, "GET", "/maps/by-name/:name/leaderboard", h_map_board, NULL);
    kn_http_route(b->http, "POST", "/maps/:id/vote", h_map_vote, NULL);
    kn_http_route(b->http, "POST", "/maps/:id/feature", h_map_feature, NULL);
    kn_http_route(b->http, "DELETE", "/maps/:id", h_map_delete, NULL);
    kn_http_route(b->http, "GET", "/maps/:id/versions", h_map_versions, NULL);
    kn_http_route(b->http, "POST", "/maps/:id/versions/:v/revert", h_map_revert, NULL);
    kn_http_route(b->http, "POST", "/maps/:id/funds", h_map_funds, NULL);
    kn_http_route(b->http, "POST", "/maps/:id/leaderboard", h_map_leaderboard, NULL);
    kn_http_route(b->http, "GET", "/maps/:id/data", h_map_data_get, NULL);
    kn_http_route(b->http, "DELETE", "/maps/:id/data", h_map_data_delete, NULL);
    return 0;
}
