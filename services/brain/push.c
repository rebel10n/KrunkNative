/*
 * services/brain/push.c - Web Push subscriptions of the Krunker Box Office web client: the subscribe side
 * (the page's service worker and the browser subscription are web_dom.c / ui/sw.js; the send is a later step).
 *
 * Routes (public listener):
 *   GET  /api/push/vapid-key    {ok, publicKey}: the VAPID public key the page subscribes with. 404
 *                               PushUnavailable when vapid.public_key (KN_VAPID_PUBLIC_KEY) is unset or malformed.
 *   POST /api/push/subscribe    {token, endpoint, keys: {p256dh, auth}} -> {ok:true}. Stores the browser's
 *                               subscription for the account (upsert by endpoint; 5 per account, oldest dropped).
 *   POST /api/push/unsubscribe  {token, endpoint} -> {ok:true}. Deletes that account's row for the endpoint.
 *
 * Auth of the two POSTs: the session token ("token" in the JSON body, Authorization: Bearer, or ?token=),
 * resolved through accounts_internal_call("token") like avatar.c. Errors: 400 BadRequest (endpoint or keys
 * do not pass push_rules.h), 401 LoginRequired, 503 PushUnavailable (no database), 500 internal.
 *
 * Config: vapid.public_key only. The private key and the subject are the send step's and are not read here.
 * The rules are push_rules.c; the SQL is db_push.c (migration 0344_push_subscriptions).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "accounts.h"
#include "brain.h"
#include "db_procs.h"
#include "push_rules.h"
#include "v10_api.h"
#include "knsvc/config.h"
#include "knsvc/db.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "push"
#define TOKEN_MAX 256

static struct {
    Brain *b;
    char vapid[PUSH_P256DH_LEN + 1]; /* the public key ("" when unset: GET answers 404) */
} G;

static void reply_json(kn_http_req *req, int status, cJSON *o) {
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static void reply_error(kn_http_req *req, int status, const char *name) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "error", name);
    reply_json(req, status, o);
}

/* a refusal: KN's {ok: 0, error} on the api push routes, v10's {error: {code, message}} on the root aliases */
static void refuse(kn_http_req *req, const int v10, const int status, const char *name) {
    if (v10) v10_reply_error(req, status, name, name);
    else reply_error(req, status, name);
}

/* one subscribe / unsubscribe on its way: the validated fields, answered once */
typedef struct {
    kn_http_req *req;
    int unsubscribe;
    int v10;
    char endpoint[PUSH_ENDPOINT_MAX + 1];
    char p256dh[PUSH_P256DH_LEN + 1];
    char auth[PUSH_AUTH_LEN + 1];
} PushReq;

static void db_done(kn_db_result *res, void *ud) {
    PushReq *r = ud;
    if (!kn_db_ok(res)) {
        refuse(r->req, r->v10, 500, "internal");
    } else if (r->v10) {
        /* v10.js webpush.subscribe reads data.success and keeps data.registration_id (unused by KN's unsubscribe) */
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "success", 1);
        cJSON_AddNullToObject(d, "registration_id");
        v10_reply_data(r->req, 200, d);
    } else {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", 1);
        reply_json(r->req, 200, o);
    }
    free(r);
}

static void on_account(void *ud, const char *err, const cJSON *acc) {
    PushReq *r = ud;
    const int64_t id = acc && !err ? kn_json_int(acc, "id", 0) : 0;
    if (id <= 0) {
        refuse(r->req, r->v10, 401, "LoginRequired");
        free(r);
        return;
    }
    char idtxt[24], now[24];
    snprintf(idtxt, sizeof(idtxt), "%lld", (long long) id);
    snprintf(now, sizeof(now), "%lld", (long long) kn_wall_ms()); /* created_at: epoch ms, kept across restarts */
    int rc;
    if (r->unsubscribe) {
        rc = kn_db_call(G.b->db, &P_PUSH_UNSUBSCRIBE, KN_DB_ARGS(idtxt, r->endpoint), db_done, r);
    } else {
        rc = kn_db_call(G.b->db, &P_PUSH_SUBSCRIBE, KN_DB_ARGS(idtxt, r->endpoint, r->p256dh, r->auth, now), db_done, r);
    }
    if (rc != 0) {
        refuse(r->req, r->v10, 503, "PushUnavailable");
        free(r);
    }
}

/* the session token: the JSON body's "token", else ?token=, else Authorization: Bearer */
static void token_of(kn_http_req *req, const cJSON *body, char *out, const size_t n) {
    out[0] = 0;
    const char *t = kn_json_str(body, "token", "");
    if (t[0]) {
        snprintf(out, n, "%s", t);
        return;
    }
    if (kn_http_query_get(req, "token", out, n) > 0) return;
    out[0] = 0;
    const char *bearer = kn_http_bearer(req);
    if (bearer && bearer[0]) snprintf(out, n, "%s", bearer);
}

/* POST /api/push/subscribe and /unsubscribe: validate the body, then the account, then the row */
static void start(kn_http_req *req, const int unsubscribe, const int v10) {
    if (!G.b || !G.b->db) {
        refuse(req, v10, 503, "PushUnavailable");
        return;
    }
    cJSON *body = kn_http_body_json(req);
    char token[TOKEN_MAX];
    token_of(req, body, token, sizeof(token));
    if (!token[0]) {
        cJSON_Delete(body);
        refuse(req, v10, 401, "LoginRequired");
        return;
    }
    /* v10's subscribe nests the subscription: {subscription: {endpoint, keys}} (v10.js 146596); its unsubscribe
     * is flat, {registration_id, endpoint} (146632), as KN's */
    const cJSON *sub = body;
    if (v10 && cJSON_IsObject(cJSON_GetObjectItemCaseSensitive(body, "subscription")))
        sub = cJSON_GetObjectItemCaseSensitive(body, "subscription");
    /* the raw strings are checked before any copy: a long endpoint must not be cut to a valid one */
    const char *endpoint = kn_json_str(sub, "endpoint", "");
    const cJSON *keys = sub ? cJSON_GetObjectItemCaseSensitive(sub, "keys") : NULL;
    const char *p256dh = kn_json_str(keys, "p256dh", "");
    const char *auth = kn_json_str(keys, "auth", "");
    const int ok = push_endpoint_ok(endpoint) && (unsubscribe || (push_b64url_ok(p256dh, PUSH_P256DH_BYTES) &&
                                                                  push_b64url_ok(auth, PUSH_AUTH_BYTES)));
    if (!ok) {
        cJSON_Delete(body);
        refuse(req, v10, 400, "BadRequest");
        return;
    }
    PushReq *r = calloc(1, sizeof(*r));
    if (!r) {
        cJSON_Delete(body);
        refuse(req, v10, 500, "internal");
        return;
    }
    r->req = req;
    r->unsubscribe = unsubscribe;
    r->v10 = v10;
    snprintf(r->endpoint, sizeof(r->endpoint), "%s", endpoint);
    snprintf(r->p256dh, sizeof(r->p256dh), "%s", p256dh);
    snprintf(r->auth, sizeof(r->auth), "%s", auth);
    cJSON_Delete(body);

    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(G.b, "token", args, on_account, r); /* on failure it has answered and freed r */
    cJSON_Delete(args);
}

static void h_subscribe(kn_http_req *req, void *ud) {
    (void) ud;
    start(req, 0, 0);
}

static void h_unsubscribe(kn_http_req *req, void *ud) {
    (void) ud;
    start(req, 1, 0);
}

/* the v10 root paths (v10.js 26396-26407): the nested subscription, the v10 envelope */
static void h_subscribe_v10(kn_http_req *req, void *ud) {
    (void) ud;
    start(req, 0, 1);
}

static void h_unsubscribe_v10(kn_http_req *req, void *ud) {
    (void) ud;
    start(req, 1, 1);
}

/* ---- v10 POST /webpush/status and /webpush/subscription-change (v10.js 26408, 26414; v10_account.c routes them):
 * the caller's own rows of push_subscriptions. The v10 consumers read data.is_subscribed (145259) and
 * data.success / data.registration_id (145274). */

static const char *arg_or_empty(int n, const char *const *p, int i) { return i < n && p[i] ? p[i] : ""; }

/* (account, old endpoint, new endpoint, p256dh, auth, now_ms) -> {n}: the account's row for the old endpoint takes
 * the new endpoint and keys (a row that holds the new endpoint, of any account, is dropped first). n 0: the account
 * holds no row for the old endpoint, so nothing changes. One transaction. */
static kn_db_result *run_push_change(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *account = arg_or_empty(n, p, 0), *old_ep = arg_or_empty(n, p, 1), *new_ep = arg_or_empty(n, p, 2);
    const char *p256dh = arg_or_empty(n, p, 3), *auth = arg_or_empty(n, p, 4);
    int64_t moved = 0;
    if (dbx_int(&x, SQL(.any = "SELECT count(*) FROM push_subscriptions WHERE account_id = $1 AND endpoint = $2"),
                KN_DB_ARGS(account, old_ep), 0) > 0) {
        dbx_exec(&x, SQL(.any = "DELETE FROM push_subscriptions WHERE endpoint = $1 AND endpoint <> $2"), KN_DB_ARGS(new_ep, old_ep));
        moved = dbx_exec(&x,
                         SQL(.any = "UPDATE push_subscriptions SET endpoint = $3, p256dh = $4, auth = $5"
                                    " WHERE account_id = $1 AND endpoint = $2"),
                         KN_DB_ARGS(account, old_ep, new_ep, p256dh, auth));
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", (double) (moved > 0 ? moved : 0));
    return dbx_json(&x, "push_change", o);
}
static const kn_proc P_PUSH_CHANGE = {"push_change", NULL, run_push_change};

/* the request's fields until its answer: the endpoint of a status, the old and new subscription of a change */
typedef struct {
    kn_http_req *req;
    int change;
    char old_ep[PUSH_ENDPOINT_MAX + 1];
    char endpoint[PUSH_ENDPOINT_MAX + 1];
    char p256dh[PUSH_P256DH_LEN + 1];
    char auth[PUSH_AUTH_LEN + 1];
} V10Push;

static void v10_status_row(kn_db_result *res, void *ud) {
    V10Push *r = ud;
    if (!kn_db_ok(res)) {
        v10_reply_error(r->req, 500, "internal", "Box Office could not read the subscription");
    } else {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "is_subscribed", kn_db_get_int64(res, 0, 0, 0) > 0);
        v10_reply_data(r->req, 200, d);
    }
    free(r);
}

static void v10_change_row(kn_db_result *res, void *ud) {
    V10Push *r = ud;
    if (!kn_db_ok(res)) {
        v10_reply_error(r->req, 500, "internal", "Box Office could not change the subscription");
    } else {
        const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : "{}";
        cJSON *o = kn_json_parse(t, strlen(t));
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "success", kn_json_int(o, "n", 0) > 0);
        cJSON_AddNullToObject(d, "registration_id"); /* KrunkNative keeps no push registration id */
        cJSON_Delete(o);
        v10_reply_data(r->req, 200, d);
    }
    free(r);
}

static void v10_account(void *ud, const char *err, const cJSON *acc) {
    V10Push *r = ud;
    const int64_t id = acc && !err ? kn_json_int(acc, "id", 0) : 0;
    if (id <= 0) {
        v10_reply_error(r->req, 401, "LOGIN_REQUIRED", "You must be logged in.");
        free(r);
        return;
    }
    char idtxt[24], now[24];
    snprintf(idtxt, sizeof(idtxt), "%lld", (long long) id);
    snprintf(now, sizeof(now), "%lld", (long long) kn_wall_ms());
    static const kn_sql q_status = {.any = "SELECT count(*) FROM push_subscriptions WHERE account_id = $1 AND endpoint = $2"};
    int rc;
    if (r->change) rc = kn_db_call(G.b->db, &P_PUSH_CHANGE, KN_DB_ARGS(idtxt, r->old_ep, r->endpoint, r->p256dh, r->auth, now), v10_change_row, r);
    else rc = kn_db_query_sql(G.b->db, &q_status, KN_DB_ARGS(idtxt, r->endpoint), v10_status_row, r);
    if (rc != 0) {
        v10_reply_error(r->req, 503, "PushUnavailable", "Box Office has no database for Web Push");
        free(r);
    }
}

/* the caller's token (the body's, ?token=, or Authorization: Bearer), then the account, then the rows */
static void v10_start(kn_http_req *req, const int change) {
    if (!G.b || !G.b->db) {
        v10_reply_error(req, 503, "PushUnavailable", "Box Office has no database for Web Push");
        return;
    }
    cJSON *body = kn_http_body_json(req);
    char token[TOKEN_MAX];
    token_of(req, body, token, sizeof(token));
    if (!token[0]) {
        cJSON_Delete(body);
        v10_reply_error(req, 401, "LOGIN_REQUIRED", "You must be logged in.");
        return;
    }
    /* status: {endpoint}; subscription-change: {old_subscription: {endpoint}, new_subscription: {endpoint, keys}} */
    const cJSON *sub = change ? cJSON_GetObjectItemCaseSensitive(body, "new_subscription") : body;
    const char *endpoint = kn_json_str(sub, "endpoint", "");
    const cJSON *keys = sub ? cJSON_GetObjectItemCaseSensitive(sub, "keys") : NULL;
    const char *p256dh = kn_json_str(keys, "p256dh", "");
    const char *auth = kn_json_str(keys, "auth", "");
    const char *old = kn_json_str(cJSON_GetObjectItemCaseSensitive(body, "old_subscription"), "endpoint", "");
    const int ok = push_endpoint_ok(endpoint) &&
                   (!change || (push_endpoint_ok(old) && push_b64url_ok(p256dh, PUSH_P256DH_BYTES) && push_b64url_ok(auth, PUSH_AUTH_BYTES)));
    if (!ok) {
        cJSON_Delete(body);
        v10_reply_error(req, 400, "BadRequest", "BadRequest");
        return;
    }
    V10Push *r = calloc(1, sizeof(*r));
    if (!r) {
        cJSON_Delete(body);
        v10_reply_error(req, 500, "internal", "out of memory");
        return;
    }
    r->req = req;
    r->change = change;
    snprintf(r->endpoint, sizeof(r->endpoint), "%s", endpoint);
    snprintf(r->old_ep, sizeof(r->old_ep), "%s", old);
    snprintf(r->p256dh, sizeof(r->p256dh), "%s", p256dh);
    snprintf(r->auth, sizeof(r->auth), "%s", auth);
    cJSON_Delete(body);
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(G.b, "token", args, v10_account, r); /* on failure it has answered and freed r */
    cJSON_Delete(args);
}

void push_v10_status(kn_http_req *req) { v10_start(req, 0); }

void push_v10_subscription_change(kn_http_req *req) { v10_start(req, 1); }

static void h_vapid_key(kn_http_req *req, void *ud) {
    (void) ud;
    if (!G.vapid[0]) {
        reply_error(req, 404, "PushUnavailable");
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddStringToObject(o, "publicKey", G.vapid);
    reply_json(req, 200, o);
}

int push_init(Brain *b) {
    memset(&G, 0, sizeof(G));
    G.b = b;
    const char *key = kn_config_str(b->cfg, "vapid.public_key", ""); /* KN_VAPID_PUBLIC_KEY, then the config file */
    if (key[0] && push_b64url_ok(key, PUSH_P256DH_BYTES)) {
        snprintf(G.vapid, sizeof(G.vapid), "%s", key);
    } else if (key[0]) {
        KN_WARN(MOD, "vapid.public_key is not an unpadded base64url P-256 point: Web Push is off (vapid-key answers 404)");
    }
    kn_http_route(b->http, "GET", "/api/push/vapid-key", h_vapid_key, NULL);
    kn_http_route(b->http, "POST", "/api/push/subscribe", h_subscribe, NULL);
    kn_http_route(b->http, "POST", "/api/push/unsubscribe", h_unsubscribe, NULL);
    kn_http_route(b->http, "POST", "/webpush/subscribe", h_subscribe_v10, NULL);
    kn_http_route(b->http, "POST", "/webpush/unsubscribe", h_unsubscribe_v10, NULL);
    KN_INFO(MOD, "web push: /api/push/subscribe, /api/push/unsubscribe, /api/push/vapid-key (%s)",
            G.vapid[0] ? "VAPID key set" : "no VAPID key: the page cannot subscribe");
    return 0;
}
