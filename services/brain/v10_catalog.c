/*
 * services/brain/v10_catalog.c - the v10 catalog routes on the brain root (api.krunker.io and
 * gapi.svc.krunker.io both answer at "/"): the skins, the shop's wild offer and the published maps.
 * Route rows of doc/v10/api-inventory.md; the v10.0.3 wrappers are at decoded L5161, the consumers
 * are v10.js lines.
 *
 *   GET    /skins?page&page_size&type&weapon&search, GET|PUT|DELETE /skins/:id, POST /skins,
 *   POST   /skins/:id/duplicate                the Skin Manager's custom skins of the caller (skins.c P_CUSTOM_SKIN,
 *                                              0394_custom_skins; the same proc as the gapi tunnel's skins_gapi)
 *   POST   /skins/:id/upload                   skins.uploadAsset's FormData {file, asset_type, category} -> {asset_path}
 *                                              (asset_upload.c asset_upload_skin_form; a file up to brain.max_body_bytes)
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
 *   GET    /maps/:id/versions                  the map's versions (map_versions, 0392): {version, created,
 *                                              data_size, current_version} newest first, for its managers
 *   POST   /maps/:id/versions/:v/revert        version :v goes live as a new version; {version} (managers)
 *   POST   /maps/:id/funds {amount}            the creator moves KR between the account and the map's
 *                                              funds (map_funds; > 0 deposit, < 0 withdraw). The Creations
 *                                              window's "Deposit fee" (deob-10.0.7.js:107627) is floor(10%)
 *                                              and comes out of what arrives; |amount| under 10 is refused.
 *                                              The ledger rows are source map_funds
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
#include <time.h>

#include "acct_internal.h"
#include "db_procs.h"
#include "shared.h" // PREFAB_LEADERBOARD (a map object's "i")
#include "asset_upload.h" // the Skin Manager's files
#include "skins.h"  // P_CUSTOM_SKIN: the Skin Manager's custom skins
#include "v10_api.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/loop.h"

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

/* ---- skins: the Skin Manager's custom skins (skins.c P_CUSTOM_SKIN, 0394_custom_skins). The list's refusals are
 * {error: {code, message}} (its service throws res.error.message, deob-10.0.7.js:124354-124361); every other call's are
 * a text error, since the service does `throw new Error(res.error)` (:124374-124426) and the editor shows
 * "Failed to save skin: " + message, so {code, message} would read "[object Object]". */

enum { SK_LIST, SK_GET, SK_CREATE, SK_UPDATE, SK_DELETE, SK_DUPLICATE };

typedef struct CatSkin {
    kn_http_req *req;
    int kind;
    char id[24];
    char *body;                 /* the JSON body (create / update) */
    char page[16], size[16], type[16], weapon[16], search[192];
} CatSkin;

static void skin_free(CatSkin *k) {
    free(k->body);
    free(k);
}

static void on_skin_proc(kn_db_result *res, void *ud) {
    CatSkin *k = ud;
    kn_http_req *req = k->req;
    if (kn_db_ok(res)) {
        cJSON *v = acct_db_json(res);
        if (k->kind == SK_DELETE || k->kind == SK_DUPLICATE)
            skins_after(k->kind == SK_DELETE ? "delete" : "duplicate", strtoll(k->id, NULL, 10), v);
        v10_reply_data(req, 200, v ? v : cJSON_CreateObject());
    } else {
        const char *code = kn_db_user_message(res) ? kn_db_user_message(res) : acct_db_error(res, "skins"), *message = NULL;
        const int status = skins_error(code, &message);
        if (k->kind == SK_LIST) v10_reply_error(req, status, status == 500 ? "INTERNAL_ERROR" : code, message);
        else v10_reply_text_error(req, status, message);
    }
    skin_free(k);
}

static void skin_account(kn_http_req *req, int64_t id, void *ud) {
    CatSkin *k = ud;
    if (id <= 0) {
        if (k->kind == SK_LIST) v10_reply_error(req, 401, "UNAUTHORIZED", MSG_LOGIN);
        else v10_reply_text_error(req, 401, MSG_LOGIN);
        skin_free(k);
        return;
    }
    char me[24];
    acct_itoa(me, id);
    static const char *const ops[] = {"list", "get", "create", "update", "delete", "duplicate"};
    const char *op = ops[k->kind];
    if (k->kind == SK_LIST)
        kn_db_call(g_b->db, &P_CUSTOM_SKIN, KN_DB_ARGS(op, me, k->page, k->size, k->type, k->weapon, k->search), on_skin_proc, k);
    else if (k->kind == SK_CREATE)
        kn_db_call(g_b->db, &P_CUSTOM_SKIN, KN_DB_ARGS(op, me, k->body ? k->body : "{}"), on_skin_proc, k);
    else if (k->kind == SK_UPDATE)
        kn_db_call(g_b->db, &P_CUSTOM_SKIN, KN_DB_ARGS(op, me, k->id, k->body ? k->body : "{}"), on_skin_proc, k);
    else
        kn_db_call(g_b->db, &P_CUSTOM_SKIN, KN_DB_ARGS(op, me, k->id), on_skin_proc, k);
}

static void skin_route(kn_http_req *req, int kind) {
    if (!g_b->db) { reply_no_db(req); return; }
    CatSkin *k = calloc(1, sizeof *k);
    if (!k) { v10_reply_error(req, 500, "INTERNAL_ERROR", "Server error."); return; }
    k->req = req;
    k->kind = kind;
    if (kind != SK_LIST && kind != SK_CREATE) {
        const int64_t id = path_id(req, "id");
        if (id <= 0) {
            v10_reply_text_error(req, 404, "Skin not found");
            skin_free(k);
            return;
        }
        acct_itoa(k->id, id);
    }
    if (kind == SK_LIST) {
        query_str(req, "page", k->page, sizeof k->page);
        query_str(req, "page_size", k->size, sizeof k->size);
        query_str(req, "type", k->type, sizeof k->type);
        query_str(req, "weapon", k->weapon, sizeof k->weapon);
        query_str(req, "search", k->search, sizeof k->search);
    }
    if (kind == SK_CREATE || kind == SK_UPDATE) {
        cJSON *body = kn_http_body_json(req);
        k->body = body ? cJSON_PrintUnformatted(body) : NULL;
        cJSON_Delete(body);
    }
    with_account(req, skin_account, k);
}

static void h_skins_list(kn_http_req *req, void *ud) { (void) ud; skin_route(req, SK_LIST); }
static void h_skin_get(kn_http_req *req, void *ud) { (void) ud; skin_route(req, SK_GET); }
static void h_skin_create(kn_http_req *req, void *ud) { (void) ud; skin_route(req, SK_CREATE); }
static void h_skin_update(kn_http_req *req, void *ud) { (void) ud; skin_route(req, SK_UPDATE); }
static void h_skin_delete(kn_http_req *req, void *ud) { (void) ud; skin_route(req, SK_DELETE); }
static void h_skin_duplicate(kn_http_req *req, void *ud) { (void) ud; skin_route(req, SK_DUPLICATE); }

/* skins.uploadAsset (models, textures, sounds, thumbnails): the FormData itself (asset_upload.c) */
static void skin_upload_account(kn_http_req *req, int64_t id, void *ud) {
    if (id <= 0) {
        v10_reply_text_error(req, 401, MSG_LOGIN);
        return;
    }
    asset_upload_skin_form(req, id, "", (int64_t) (intptr_t) ud);
}

static void h_skin_upload(kn_http_req *req, void *ud) {
    (void) ud;
    const int64_t id = path_id(req, "id");
    if (id <= 0) {
        v10_reply_text_error(req, 404, "Skin not found");
        return;
    }
    with_account(req, skin_upload_account, (void *) (intptr_t) id);
}

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

/* The hub's calcMapFee and the Creations window both show floor(10%) (deob-10.0.7.js:107627). */
#define MAP_FUNDS_FEE "10"

/* (me, map, amount, fee percent) -> {map_funds, player_funds}: the creator's deposit (> 0) or withdrawal (< 0).
 * The fee is floor(|amount| * percent / 100) and comes out of what arrives: a deposit takes the whole amount
 * from the account and the map gets the rest, a withdrawal takes the whole amount from the map and the account
 * gets the rest. Nobody receives the fee (the ledger row records it, as the market and gift fees). */
static kn_db_result *run_map_funds(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t amount = PI(2), pct = PI(3) > 0 ? PI(3) : 0;
    const int64_t fee = (amount < 0 ? -amount : amount) * pct / 100;
    char ref[24];
    snprintf(ref, sizeof ref, "%s", P(1) ? P(1) : "");
    if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM published_maps WHERE map_id = $2 AND creator_id = $1 AND deleted_at IS NULL"),
                    KN_DB_ARGS(P(0), P(1)))) {
        if (!dbx_failed(&x)) dbx_refuse(&x, MSG_NO_MAP);
        return dbx_finish(&x, NULL);
    }
    int64_t player = 0;
    if (amount > 0) {
        if (db_add_funds_ex(&x, PI(0), -amount, "map_funds", ref, 0, fee, &player) < 0) return dbx_finish(&x, NULL);
        /* only the parameters a statement uses: PostgreSQL cannot type an unused $n */
        if (dbx_exec(&x, SQL(.any = "UPDATE published_maps SET map_funds = map_funds + {int:$2} WHERE map_id = $1"),
                     KN_DB_ARGS(P(1), I64S(amount - fee))) < 0)
            return dbx_finish(&x, NULL);
    } else {
        if (dbx_exec(&x, SQL(.any = "UPDATE published_maps SET map_funds = map_funds - {int:$2} WHERE map_id = $1 AND map_funds >= {int:$2}"),
                     KN_DB_ARGS(P(1), I64S(-amount))) == 0) {
            dbx_refuse(&x, "Not enough map funds");
            return dbx_finish(&x, NULL);
        }
        if (db_add_funds_ex(&x, PI(0), -amount - fee, "map_funds", ref, 0, fee, &player) < 0) return dbx_finish(&x, NULL);
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
    if (written < 0 || (written > 0 && db_map_snapshot(&x, P(1)) < 0)) return dbx_finish(&x, NULL);
    cJSON *v = cJSON_CreateObject();
    /* the map record's leaderboard_type: "score", "time" or the custom key, a string (the Map Detail panel
     * stores data.leaderboard_type into it, deob-10.0.3.js:120948, and compares it with "time" at 121127) */
    cJSON_AddStringToObject(v, "leaderboard_type", custom[0] ? custom : type ? "time" : "score");
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

/* a version's time as the Versions tab's date reader takes it (deob-10.0.7.js:23916: split on "-", "T", ":", "." ...) */
static void iso_ms(int64_t ms, char *out, size_t cap) {
    const time_t t = (time_t) (ms / 1000);
    struct tm tm;
#ifdef _WIN32
    gmtime_s(&tm, &t);
#else
    gmtime_r(&t, &tm);
#endif
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min,
             tm.tm_sec, (int) (ms % 1000));
}

/* (me, map) -> [{version, created, data_size, current_version}] newest first: the map's versions (map_versions,
 * migration 0392) for a caller who may manage it (the Versions tab is the owner's, deob-10.0.7.js:107651-107652);
 * current_version is the live one (the tab reads it from the first row, 107649) */
static kn_db_result *run_map_versions(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t live = dbx_int(&x, SQL(.any = "SELECT map_updatecounter FROM published_maps WHERE map_id = $2 AND deleted_at IS NULL AND " MANAGE_SQL),
                                 KN_DB_ARGS(P(0), P(1)), -1);
    if (live < 0) {
        if (!dbx_failed(&x)) dbx_refuse(&x, MSG_NO_MAP);
        return dbx_finish(&x, NULL);
    }
    kn_db_result *r = dbx_q(&x, SQL(.any = "SELECT version, {epoch_ms:created_at}, data_size FROM map_versions WHERE map_id = $1"
                                           " ORDER BY version DESC"),
                            KN_DB_ARGS(P(1)));
    if (!r) return dbx_finish(&x, NULL);
    cJSON *list = cJSON_CreateArray();
    for (int i = 0; i < kn_db_rows(r); i++) {
        char when[40];
        iso_ms(kn_db_get_int64(r, i, 1, 0), when, sizeof when);
        cJSON *row = cJSON_CreateObject();
        cJSON_AddNumberToObject(row, "version", (double) kn_db_get_int64(r, i, 0, 0));
        cJSON_AddStringToObject(row, "created", when);
        cJSON_AddNumberToObject(row, "data_size", (double) kn_db_get_int64(r, i, 2, 0));
        cJSON_AddNumberToObject(row, "current_version", (double) live);
        cJSON_AddItemToArray(list, row);
    }
    kn_db_result_free(r);
    return dbx_json(&x, "v10_map_versions", list);
}
static const kn_proc P_MAP_VERSIONS = {"v10_map_versions", NULL, run_map_versions};

/* map_info's "m" (the modes the Games window tags, creations.c sh_upload_map) from the map data; NULL: unchanged */
static char *info_with_modes(const char *info_text, const char *data) {
    cJSON *map = cJSON_Parse(data);
    const cJSON *list = cJSON_GetObjectItemCaseSensitive(map, "modes");
    if (!cJSON_IsArray(list)) {
        cJSON_Delete(map);
        return NULL;
    }
    cJSON *info = info_text ? cJSON_Parse(info_text) : NULL;
    if (!cJSON_IsObject(info)) {
        cJSON_Delete(info);
        info = cJSON_CreateObject();
    }
    cJSON_DeleteItemFromObjectCaseSensitive(info, "m");
    cJSON *modes = cJSON_AddArrayToObject(info, "m");
    const cJSON *mode;
    cJSON_ArrayForEach(mode, list) {
        if (cJSON_IsNumber(mode)) cJSON_AddItemToArray(modes, cJSON_CreateNumber(mode->valuedouble));
    }
    cJSON_Delete(map);
    char *out = cJSON_PrintUnformatted(info);
    cJSON_Delete(info);
    return out;
}

/* (me, map, version) -> {version}: the version's data goes live as a new version (map_updatecounter + 1, so game
 * servers reload it), and the one that was live stays listed ("The version currently live is kept as a snapshot,
 * so you can undo this", deob-10.0.7.js:107816; "Live version is now v{0}" reads data.version, 107827) */
static kn_db_result *run_map_revert(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t live = dbx_int(&x, SQL(.any = "SELECT map_updatecounter FROM published_maps WHERE map_id = $2 AND deleted_at IS NULL AND " MANAGE_SQL),
                                 KN_DB_ARGS(P(0), P(1)), -1);
    if (live < 0) {
        if (!dbx_failed(&x)) dbx_refuse(&x, MSG_NO_MAP);
        return dbx_finish(&x, NULL);
    }
    if (PI(2) == live) {
        dbx_refuse(&x, "This version is already live");
        return dbx_finish(&x, NULL);
    }
    char *data = dbx_text(&x, SQL(.any = "SELECT map_data FROM map_versions WHERE map_id = $1 AND version = {int:$2}"),
                          KN_DB_ARGS(P(1), P(2)));
    if (!data) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Version not found");
        return dbx_finish(&x, NULL);
    }
    char *info_text = dbx_text(&x, SQL(.pg = "SELECT map_info::text FROM published_maps WHERE map_id = $1",
                                       .any = "SELECT map_info FROM published_maps WHERE map_id = $1"),
                               KN_DB_ARGS(P(1)));
    char *info = info_with_modes(info_text, data);
    free(info_text);
    int64_t written = dbx_exec(&x, SQL(.any = "UPDATE published_maps SET map_data = $2, map_updatecounter = map_updatecounter + 1,"
                                              " map_date = {now} WHERE map_id = $1 AND deleted_at IS NULL"),
                               KN_DB_ARGS(P(1), data));
    free(data);
    if (written > 0 && info)
        written = dbx_exec(&x, SQL(.pg = "UPDATE published_maps SET map_info = $2::jsonb WHERE map_id = $1",
                                   .any = "UPDATE published_maps SET map_info = $2 WHERE map_id = $1"),
                           KN_DB_ARGS(P(1), info));
    free(info);
    if (written < 0 || db_map_snapshot(&x, P(1)) < 0) return dbx_finish(&x, NULL);
    const int64_t now_live = dbx_int(&x, SQL(.any = "SELECT map_updatecounter FROM published_maps WHERE map_id = $1"), KN_DB_ARGS(P(1)), 0);
    cJSON *v = cJSON_CreateObject();
    cJSON_AddNumberToObject(v, "version", (double) now_live);
    return dbx_json(&x, "v10_map_revert", v);
}
static const kn_proc P_MAP_REVERT = {"v10_map_revert", NULL, run_map_revert};

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

enum { OP_VOTE, OP_FUNDS, OP_LB, OP_FEATURE, OP_DELETE, OP_DATA_GET, OP_DATA_DELETE, OP_VERSIONS, OP_REVERT };

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
        kn_db_call(db, &P_MAP_FUNDS, KN_DB_ARGS(me, map, n1, MAP_FUNDS_FEE), on_map_proc, o);
        break;
    case OP_LB:
        kn_db_call(db, &P_MAP_LB, KN_DB_ARGS(me, map, n1, n2, o->s1), on_map_proc, o);
        break;
    case OP_DATA_DELETE:
        kn_db_call(db, &P_MAP_DATA_DELETE, KN_DB_ARGS(me, map, o->s1, o->s2), on_map_proc, o);
        break;
    case OP_VERSIONS:
        kn_db_call(db, &P_MAP_VERSIONS, KN_DB_ARGS(me, map), on_map_proc, o);
        break;
    case OP_REVERT:
        kn_db_call(db, &P_MAP_REVERT, KN_DB_ARGS(me, map, n1), on_map_proc, o);
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
    /* deob-10.0.7.js:107768-107771: the Funds tab never sends |amount| under 10 */
    if (amount > -10 && amount < 10) {
        v10_reply_error(req, 400, "INVALID_AMOUNT", "Minimum is 10 KR");
        return;
    }
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

/* GET /maps/:id/versions (deob-10.0.7.js:26713-26717): the manager's list of the map's versions */
static void h_map_versions(kn_http_req *req, void *ud) {
    (void) ud;
    map_route(req, OP_VERSIONS, path_id(req, "id"), 0, 0, NULL, NULL);
}

/* POST /maps/:id/versions/:v/revert (deob-10.0.7.js:26719-26723): version :v goes live again */
static void h_map_revert(kn_http_req *req, void *ud) {
    (void) ud;
    const int64_t v = path_id(req, "v");
    if (v <= 0) {
        v10_reply_error(req, 404, "VERSION_NOT_FOUND", "Version not found");
        return;
    }
    map_route(req, OP_REVERT, path_id(req, "id"), v, 0, NULL, NULL);
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

/* ---- GET /maps/by-name/:name for a published map: the v10.0.3 Map Detail panel's record */

/* The panel (deob-10.0.3.js:121106-121532) reads map_id (thumbnail, every write), creator_name (the owner's
 * tabs, 121112), category (an index of the category list, 121111 / 21280), featured, votes (121378),
 * gameplays (121381), created_at / updated_at (121444, 121446, the date formatter at 27304), description
 * (121448, a text node: raw), funds (121532), leaderboard_type ("score", "time" or the custom key,
 * 121127-121137) and leaderboard_order (121138). */
static const kn_sql k_map_detail = {
    .any = "SELECT d.map_id, d.map_name, coalesce(a.username, ''), d.map_category, d.map_featured, d.map_votes,"
           " d.map_plays, d.map_funds, d.map_description, {epoch_ms:d.map_initialdate}, {epoch_ms:d.map_date},"
           " d.map_data FROM published_maps d LEFT JOIN accounts a ON a.id = d.creator_id"
           " WHERE lower(d.map_name) = lower($1) AND d.deleted_at IS NULL ORDER BY d.map_id LIMIT 1"};

static void on_map_detail(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    if (!kn_db_ok(res)) {
        v10_reply_error(req, 500, "INTERNAL_ERROR", acct_db_error(res, "map"));
        return;
    }
    if (kn_db_rows(res) < 1) {
        v10_reply_error(req, 404, "MAP_NOT_FOUND", "Map not found");
        return;
    }
    /* the LEADERBOARD object's settings (creations.c ulb, run_map_lb): ldc the custom key, ldt 1 time, ldo 1 */
    const char *lb_type = "score";
    int64_t lb_order = 0;
    char custom[64] = "";
    cJSON *map = cJSON_Parse(kn_db_get(res, 0, 11));
    cJSON *o;
    const cJSON *objs = map ? cJSON_GetObjectItemCaseSensitive(map, "objects") : NULL;
    cJSON_ArrayForEach(o, objs) {
        if (!cJSON_IsObject(o) || kn_json_int(o, "i", -1) != PREFAB_LEADERBOARD) continue;
        snprintf(custom, sizeof custom, "%s", kn_json_str(o, "ldc", ""));
        lb_type = custom[0] ? custom : kn_json_int(o, "ldt", 0) == 1 ? "time" : "score";
        lb_order = kn_json_int(o, "ldo", 0);
        break;
    }
    const int64_t category = kn_db_get_int64(res, 0, 3, 0);
    char when[40];
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "map_id", (double) kn_db_get_int64(res, 0, 0, 0));
    cJSON_AddStringToObject(d, "name", kn_db_get(res, 0, 1));
    cJSON_AddStringToObject(d, "creator_name", kn_db_get(res, 0, 2));
    cJSON_AddNumberToObject(d, "category", (double) (category > 0 ? category : 0)); /* -1 (none): "Other" */
    cJSON_AddBoolToObject(d, "featured", kn_db_get_int64(res, 0, 4, 0) != 0);
    cJSON_AddNumberToObject(d, "votes", (double) kn_db_get_int64(res, 0, 5, 0));
    cJSON_AddNumberToObject(d, "gameplays", (double) kn_db_get_int64(res, 0, 6, 0));
    cJSON_AddNumberToObject(d, "funds", (double) kn_db_get_int64(res, 0, 7, 0));
    cJSON_AddStringToObject(d, "description", kn_db_get(res, 0, 8));
    v10_iso_ms(kn_db_get_int64(res, 0, 9, 0), when, sizeof when);
    cJSON_AddStringToObject(d, "created_at", when);
    v10_iso_ms(kn_db_get_int64(res, 0, 10, 0), when, sizeof when);
    cJSON_AddStringToObject(d, "updated_at", when);
    cJSON_AddStringToObject(d, "leaderboard_type", lb_type);
    cJSON_AddNumberToObject(d, "leaderboard_order", (double) lb_order);
    cJSON_Delete(map);
    v10_reply_data(req, 200, d);
}

void v10_catalog_map_by_name(kn_http_req *req, const char *name) {
    if (!g_b || !g_b->db || !name || !*name) {
        v10_reply_error(req, 404, "MAP_NOT_FOUND", "Map not found");
        return;
    }
    char arg[160];
    snprintf(arg, sizeof arg, "%s", name);
    kn_db_query_sql(g_b->db, &k_map_detail, KN_DB_ARGS(arg), on_map_detail, req);
}

/* ---- the hub's map page over the social socket
 *
 * The hub (krunker.io/social.html, static/social-CURLIuB7.js, cited hub-10.0.7.js:<line> after the repo's webcrack
 * pass, doc/v10/parity-audit-2026-11-20-social.md) asks for the same rows and writes as the routes above, as frames:
 *   ["glb", mapName, ltyp, lord, ltyp2]  -> ["glbR", type, mapName, [name, value, ...]]   (hub-10.0.7.js:3168, 3804-3822)
 *   ["sd", mapName, player]              -> ["sdR", [{mk, kv}], player]                  (:3654-3680)
 *   ["dd", player, key, mapId]           -> ["ddR", key, player]                         (:3682-3696)
 *   ["dkd", mapId, key]                  -> ["po", "SUCCESS", null, text]                (:3649-3653)
 *   ["mfnds", mapId, 0 deposit | 1 withdraw, amount] -> ["mfr", playerFunds | null]     (:6673-6678, 3796-3803)
 * The leaderboard's settings come from the map's LEADERBOARD object (ulb / the HTTP route), not from the frame. A
 * refusal of dd / dkd / mfnds is the hub's "po" error. mfnds and POST /maps/:id/funds charge the fee both
 * screens show (calcMapFee, hub-10.0.7.js:6657-6672, and the Creations window's "Deposit fee",
 * deob-10.0.7.js:107627): floor(|amount| * 0.1), out of what arrives, in both directions. The window sends
 * the signed amount (updateMapFunds, :107777) and refuses |amount| under 10 before the call (:107768-107771). */

typedef struct HubBoard {
    char name[160];
    char type[64];
    int time, desc, custom;
} HubBoard;

static void hub_board_send(SocialSession *s, const HubBoard *h, cJSON *list) {
    social_send_n(s, "glbR", 3, cJSON_CreateString(h->type), cJSON_CreateString(h->name), list ? list : cJSON_CreateArray());
}

static void hub_board_rows(SocialSession *s, kn_db_result *res, void *extra) {
    const HubBoard *h = extra;
    cJSON *list = cJSON_CreateArray();
    if (kn_db_ok(res)) {
        for (int i = 0; i < kn_db_rows(res); i++) {
            cJSON_AddItemToArray(list, cJSON_CreateString(kn_db_get(res, i, 1)));
            cJSON_AddItemToArray(list, cJSON_CreateNumber((double) kn_db_get_int64(res, i, 2, 0)));
        }
    }
    hub_board_send(s, h, list);
}

static void hub_board_settings(SocialSession *s, kn_db_result *res, void *extra) {
    HubBoard *h = extra;
    cJSON *map = kn_db_ok(res) && kn_db_rows(res) > 0 ? cJSON_Parse(kn_db_get(res, 0, 0)) : NULL;
    const cJSON *objs = map ? cJSON_GetObjectItemCaseSensitive(map, "objects") : NULL;
    const cJSON *o, *lb = NULL;
    cJSON_ArrayForEach(o, objs) {
        if (cJSON_IsObject(o) && kn_json_int(o, "i", -1) == PREFAB_LEADERBOARD) {
            lb = o;
            break;
        }
    }
    if (!lb) {
        cJSON_Delete(map);
        hub_board_send(s, h, NULL); /* no Leaderboard Display: "No Records Yet" */
        return;
    }
    const char *custom = kn_json_str(lb, "ldc", "");
    h->custom = custom[0] != 0;
    h->time = kn_json_int(lb, "ldt", 0) == 1;
    h->desc = kn_json_int(lb, "ldo", 0) == 1;
    snprintf(h->type, sizeof h->type, "%s", h->custom ? custom : h->time ? "time" : "score");
    cJSON_Delete(map);
    if (h->custom) { /* a custom stat has no Box Office source (as GET /maps/by-name/:name/leaderboard) */
        hub_board_send(s, h, NULL);
        return;
    }
    HubBoard *next = malloc(sizeof *next);
    if (!next) {
        hub_board_send(s, h, NULL);
        return;
    }
    *next = *h;
    char limit[24], offset[24];
    acct_itoa(limit, BOARD_PAGE);
    acct_itoa(offset, 0);
    social_query(s, &k_board[next->time][next->desc], KN_DB_ARGS(next->name, limit, offset), hub_board_rows, next);
}

void sh_map_board(SocialSession *s, const cJSON *f) {
    HubBoard *h = calloc(1, sizeof *h);
    if (!h) return;
    snprintf(h->name, sizeof h->name, "%s", fa_str(f, 1) ? fa_str(f, 1) : "");
    snprintf(h->type, sizeof h->type, "score");
    social_query(s, &k_board_settings, KN_DB_ARGS(h->name), hub_board_settings, h);
}

/* the player's rows of a map found by name, for a caller who may manage it (k_data_get by name) */
static const kn_sql k_hub_data = {
    .any = "SELECT s.skey, s.svalue FROM ks_storage s JOIN published_maps m ON m.map_id = s.map_id"
           " JOIN accounts a ON a.id = s.account_id"
           " WHERE lower(m.map_name) = lower($2) AND lower(a.username) = lower($3) AND m.deleted_at IS NULL AND " MANAGE_SQL
           " ORDER BY s.skey"};

typedef struct HubData {
    char player[160];
    char key[128];
} HubData;

static void hub_data_rows(SocialSession *s, kn_db_result *res, void *extra) {
    const HubData *d = extra;
    cJSON *rows = cJSON_CreateArray();
    if (kn_db_ok(res)) {
        for (int i = 0; i < kn_db_rows(res); i++) {
            cJSON *row = cJSON_CreateObject();
            cJSON_AddStringToObject(row, "mk", kn_db_get(res, i, 0));
            cJSON_AddStringToObject(row, "kv", kn_db_get(res, i, 1));
            cJSON_AddItemToArray(rows, row);
        }
    }
    social_send_n(s, "sdR", 2, rows, cJSON_CreateString(d->player));
}

void sh_map_data_search(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    HubData *d = calloc(1, sizeof *d);
    if (!d) return;
    snprintf(d->player, sizeof d->player, "%s", fa_str(f, 2) ? fa_str(f, 2) : "");
    char me[24], map[160];
    snprintf(map, sizeof map, "%s", fa_str(f, 1) ? fa_str(f, 1) : "");
    social_query(s, &k_hub_data, KN_DB_ARGS(social_viewer(s, me), map, d->player), hub_data_rows, d);
}

static void hub_refused(SocialSession *s, kn_db_result *res, const char *what) {
    const char *msg = kn_db_user_message(res);
    social_send_n(s, "po", 2, cJSON_CreateNull(), cJSON_CreateString(msg ? msg : acct_db_error(res, what)));
}

static void hub_data_deleted(SocialSession *s, kn_db_result *res, void *extra) {
    const HubData *d = extra;
    if (!kn_db_ok(res)) {
        hub_refused(s, res, "map data");
        return;
    }
    if (d->player[0])
        social_send_n(s, "ddR", 2, cJSON_CreateString(d->key), cJSON_CreateString(d->player));
    else
        social_send_n(s, "po", 3, cJSON_CreateString("SUCCESS"), cJSON_CreateNull(), cJSON_CreateString("Key deleted for every player"));
}

static void hub_data_delete(SocialSession *s, int64_t map_id, const char *key, const char *player) {
    if (!social_require_auth(s)) return;
    if (map_id <= 0 || !key || !*key) {
        social_send_n(s, "po", 2, cJSON_CreateNull(), cJSON_CreateString(map_id <= 0 ? MSG_NO_MAP : "A key is required"));
        return;
    }
    HubData *d = calloc(1, sizeof *d);
    if (!d) return;
    snprintf(d->key, sizeof d->key, "%s", key);
    snprintf(d->player, sizeof d->player, "%s", player ? player : "");
    char me[24], map[24];
    acct_itoa(map, map_id);
    social_call(s, &P_MAP_DATA_DELETE, KN_DB_ARGS(social_viewer(s, me), map, d->key, d->player), hub_data_deleted, d);
}

/* ["dd", player, key, mapId]: one key of one player */
void sh_map_data_delete(SocialSession *s, const cJSON *f) {
    const char *player = fa_str(f, 1);
    if (!player || !*player) {
        social_send_n(s, "po", 2, cJSON_CreateNull(), cJSON_CreateString("A player is required"));
        return;
    }
    hub_data_delete(s, fa_int(f, 3, 0), fa_str(f, 2), player);
}

/* ["dkd", mapId, key]: the key of every player */
void sh_map_key_delete(SocialSession *s, const cJSON *f) { hub_data_delete(s, fa_int(f, 1, 0), fa_str(f, 2), NULL); }

static void hub_funds_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) {
        if (kn_db_user_message(res)) hub_refused(s, res, "map funds");
        else social_send_n(s, "mfr", 1, cJSON_CreateNull());
        return;
    }
    cJSON *v = acct_db_json(res);
    const cJSON *funds = v ? cJSON_GetObjectItemCaseSensitive(v, "player_funds") : NULL;
    social_send_n(s, "mfr", 1, cJSON_IsNumber(funds) ? cJSON_CreateNumber(funds->valuedouble) : cJSON_CreateNull());
    cJSON_Delete(v);
}

/* ["mfnds", mapId, 0 deposit | 1 withdraw, amount]: the creator's map funds (P_MAP_FUNDS, the signed amount)
 * less the 10% fee both screens that send it show (the hub's and the editor's calcMapFee, floor(amount * 0.1)) */
void sh_map_funds(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    const int64_t map_id = fa_int(f, 1, 0), amount = fa_int(f, 3, 0);
    if (map_id <= 0 || amount <= 0) {
        social_send_n(s, "po", 2, cJSON_CreateNull(), cJSON_CreateString(map_id <= 0 ? MSG_NO_MAP : "An amount of KR is required"));
        return;
    }
    char me[24], map[24], signed_amount[24];
    acct_itoa(map, map_id);
    acct_itoa(signed_amount, fa_int(f, 2, 0) == 1 ? -amount : amount);
    social_call(s, &P_MAP_FUNDS, KN_DB_ARGS(social_viewer(s, me), map, signed_amount, MAP_FUNDS_FEE), hub_funds_done, NULL);
}

/* ---- the route table */

int v10_catalog_init(Brain *b) {
    g_b = b;
    if (!b->http) return 0;
    kn_http_route(b->http, "GET", "/skins", h_skins_list, NULL);
    kn_http_route(b->http, "GET", "/skins/:id", h_skin_get, NULL);
    kn_http_route(b->http, "POST", "/skins", h_skin_create, NULL);
    kn_http_route(b->http, "PUT", "/skins/:id", h_skin_update, NULL);
    kn_http_route(b->http, "DELETE", "/skins/:id", h_skin_delete, NULL);
    kn_http_route(b->http, "POST", "/skins/:id/upload", h_skin_upload, NULL);
    kn_http_route(b->http, "POST", "/skins/:id/duplicate", h_skin_duplicate, NULL);
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
