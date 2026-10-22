/*
 * services/brain/shop.c - v10's shop API for the Junk-priced products (db_shop.c's procs; migration
 * 0349_shop_junk; doc/v10/junk.md section 6). Routes on the brain's /api prefix, v10's shape ({data}
 * on success, {error, message} on refusal):
 *   GET  /api/shop/displays?tag=limited     the display (token optional: adds the account's purchase_count)
 *   POST /api/shop/purchase                 body {product_id, quantity}; needs a token (401 LoginRequired)
 *                                           -> {data: {success, message, rewards, currency_id, price_paid, junk}}
 * "No display found with this tag" is 404 (v10's answer for a tag with no products); the other
 * refusals are 400 with the proc's message ("Not enough Junk", "Purchase limit reached", ...).
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain_internal.h"
#include "db_procs.h"
#include "knsvc/config.h"
#include "knsvc/db.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "shop"

static struct {
    Brain *b;
    int enabled;
} S;

static void reply_error(kn_http_req *req, int status, const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    cJSON_AddStringToObject(o, "message", msg);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static int db_ready(void) { return S.b && S.b->db && S.enabled; }

typedef struct Pending {
    kn_http_req *req;
    int purchase;            /* 0 display, 1 purchase */
    int bundles;             /* 1: P_SHOP_BUNDLES (no account, no tag) */
    int64_t account;
    int64_t product;
    int64_t quantity;
    char tag[32];
} Pending;

static void on_db(kn_db_result *res, void *ud) {
    Pending *q = ud;
    if (!kn_db_ok(res)) {
        const char *msg = kn_db_user_message(res);
        if (!msg) KN_WARN(MOD, "%s failed: %s", q->purchase ? "purchase" : "display", kn_db_error(res));
        if (msg && !strcmp(msg, "No display found with this tag")) reply_error(q->req, 404, msg);
        else reply_error(q->req, msg ? 400 : 500, msg ? msg : "internal");
    } else {
        const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : NULL;
        cJSON *data = t ? kn_json_parse(t, strlen(t)) : NULL;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddItemToObject(o, "data", data ? data : cJSON_CreateNull());
        if (!q->purchase) kn_http_set_header(q->req, "Cache-Control", "no-cache, no-store, must-revalidate");
        brain_reply_json(q->req, 200, o);
        cJSON_Delete(o);
    }
    free(q);
}

static void run_db(Pending *q) {
    char acc[24], prod[24], qty[24];
    snprintf(acc, sizeof acc, "%lld", (long long) q->account);
    if (q->bundles) {
        kn_db_call(S.b->db, &P_SHOP_BUNDLES, KN_DB_ARGS("1"), on_db, q);
    } else if (q->purchase) {
        snprintf(prod, sizeof prod, "%lld", (long long) q->product);
        snprintf(qty, sizeof qty, "%lld", (long long) q->quantity);
        kn_db_call(S.b->db, &P_SHOP_PURCHASE, KN_DB_ARGS(acc, prod, qty), on_db, q);
    } else {
        kn_db_call(S.b->db, &P_SHOP_DISPLAY, KN_DB_ARGS(q->tag, acc), on_db, q);
    }
}

static void on_account(void *ud, const char *err, const cJSON *acc) {
    Pending *q = ud;
    const int64_t id = acc && !err ? kn_json_int(acc, "id", 0) : 0;
    if (id <= 0) {
        if (!q->purchase) {
            q->account = 0; /* a dead token reads as a guest's display */
            run_db(q);
            return;
        }
        reply_error(q->req, 401, "LoginRequired");
        free(q);
        return;
    }
    q->account = id;
    run_db(q);
}

static int token_of(kn_http_req *req, char *out, size_t cap) {
    out[0] = 0;
    if (kn_http_query_get(req, "token", out, cap) <= 0) {
        const char *bearer = kn_http_bearer(req);
        snprintf(out, cap, "%s", bearer ? bearer : "");
    }
    return out[0] != 0;
}

static void h_display(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) {
        reply_error(req, 503, "ShopUnavailable");
        return;
    }
    Pending *q = calloc(1, sizeof *q);
    if (!q) {
        reply_error(req, 500, "internal");
        return;
    }
    q->req = req;
    /* v10's /shop/displays/:tag (v10.js 26520) names the tag in the path; ?tag= (v10.js 26525) and KN's
     * /api/shop/displays in the query */
    const char *seg = kn_http_param(req, "tag");
    if (seg && seg[0]) snprintf(q->tag, sizeof q->tag, "%s", seg);
    else if (kn_http_query_get(req, "tag", q->tag, sizeof q->tag) <= 0) snprintf(q->tag, sizeof q->tag, "limited");
    char token[512];
    if (!token_of(req, token, sizeof token)) {
        run_db(q);
        return;
    }
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(S.b, "token", args, on_account, q);
    cJSON_Delete(args);
}

static void h_purchase(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) {
        reply_error(req, 503, "ShopUnavailable");
        return;
    }
    Pending *q = calloc(1, sizeof *q);
    if (!q) {
        reply_error(req, 500, "internal");
        return;
    }
    q->req = req;
    q->purchase = 1;
    cJSON *body = kn_http_body_json(req);
    const cJSON *pid = cJSON_GetObjectItemCaseSensitive(body, "product_id");
    const cJSON *qty = cJSON_GetObjectItemCaseSensitive(body, "quantity");
    q->product = cJSON_IsNumber(pid) ? (int64_t) pid->valuedouble : 0;
    q->quantity = cJSON_IsNumber(qty) ? (int64_t) qty->valuedouble : 1;
    cJSON_Delete(body);
    char token[512];
    if (!token_of(req, token, sizeof token)) {
        reply_error(req, 401, "LoginRequired");
        free(q);
        return;
    }
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(S.b, "token", args, on_account, q);
    cJSON_Delete(args);
}

/* GET /api/shop/bundles: the store's bundle grid (v10's ALL_BUNDLE_DATA rows; public, no account) */
static void h_bundles(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) {
        reply_error(req, 503, "ShopUnavailable");
        return;
    }
    Pending *q = calloc(1, sizeof *q);
    if (!q) {
        reply_error(req, 500, "internal");
        return;
    }
    q->req = req;
    q->bundles = 1;
    run_db(q);
}

int shop_init(Brain *b) {
    S.b = b;
    S.enabled = kn_config_bool(b->cfg, "brain.shop.enabled", 1);
    kn_http_route(b->http, "GET", "/api/shop/displays", h_display, b);
    kn_http_route(b->http, "POST", "/api/shop/purchase", h_purchase, b);
    kn_http_route(b->http, "GET", "/api/shop/bundles", h_bundles, b);
    /* the v10 root paths (v10.js 26520-26535) */
    kn_http_route(b->http, "GET", "/shop/displays/:tag", h_display, b);
    kn_http_route(b->http, "GET", "/shop/displays", h_display, b);
    kn_http_route(b->http, "POST", "/shop/purchase", h_purchase, b);
    if (b->db && S.enabled) KN_INFO(MOD, "Junk shop on /api/shop/*");
    return 0;
}

void shop_shutdown(Brain *b) {
    (void) b;
    S.enabled = 0;
}
