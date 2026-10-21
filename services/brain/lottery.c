/*
 * services/brain/lottery.c - Krunker v10.0.2's lottery routes and the settlement timer (the procs are
 * in db_lottery.c, the tables in migration 0348_lottery; the rules are that file's header).
 *
 * Routes on the brain's /api prefix, v10's shape ({data: ...} on success; {error, message} on refusal):
 *   GET  /api/lottery/:id                  the view. A token (?token= or Bearer) adds the account's own
 *                                          entries; without one the view is the guest's. 400 "lottery not found".
 *   POST /api/lottery/:id/participate      body {cost_type: "skin" | "currency", cost_ref, cost_quantity}
 *                                          (cost_quantity = bundles). Needs a token (401 LoginRequired).
 *                                          Replies {data: {player_entries, total_entries}}.
 * The timer (brain.lottery.settle_check_ms, default 60 s; brain.lottery.enabled) settles every window that
 * has ended (P_LOTTERY_SETTLE): one draw each, one transaction per pass, never two passes at once.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain_internal.h"
#include "db_procs.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/db.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "lottery"

static struct {
    Brain *b;
    int enabled;
    int settling;             /* one settlement pass in flight */
    kn_timer_id timer;
} L;

static void reply_error(kn_http_req *req, int status, const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    cJSON_AddStringToObject(o, "message", msg);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static int db_ready(void) { return L.b && L.b->db && L.enabled; }

/* a request's pending DB call: the view or the entry, after the token's account is known */
typedef struct Pending {
    kn_http_req *req;
    int64_t lottery;
    int participate;         /* 0 view, 1 participate */
    int64_t account;         /* 0 for a guest view */
    char cost_type[16];
    char cost_ref[32];
    int64_t bundles;
} Pending;

static void pending_done(Pending *q) {
    free(q);
}

static void on_db(kn_db_result *res, void *ud) {
    Pending *q = ud;
    if (!kn_db_ok(res)) {
        const char *msg = kn_db_user_message(res);
        if (!msg) KN_WARN(MOD, "%s failed: %s", q->participate ? "participate" : "view", kn_db_error(res));
        reply_error(q->req, msg ? 400 : 500, msg ? msg : "internal");
    } else {
        const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : NULL;
        cJSON *data = t ? kn_json_parse(t, strlen(t)) : NULL;
        cJSON *o = cJSON_CreateObject();
        cJSON_AddItemToObject(o, "data", data ? data : cJSON_CreateNull());
        if (!q->participate) kn_http_set_header(q->req, "Cache-Control", "no-cache, no-store, must-revalidate");
        brain_reply_json(q->req, 200, o);
        cJSON_Delete(o);
    }
    pending_done(q);
}

static void run_db(Pending *q, const int64_t now) {
    char lot[24], acc[24], bundles[24], now_s[24];
    snprintf(lot, sizeof lot, "%lld", (long long) q->lottery);
    snprintf(acc, sizeof acc, "%lld", (long long) q->account);
    snprintf(bundles, sizeof bundles, "%lld", (long long) q->bundles);
    snprintf(now_s, sizeof now_s, "%lld", (long long) now);
    if (q->participate)
        kn_db_call(L.b->db, &P_LOTTERY_PARTICIPATE, KN_DB_ARGS(acc, lot, q->cost_type, q->cost_ref, bundles, now_s), on_db, q);
    else
        kn_db_call(L.b->db, &P_LOTTERY_VIEW, KN_DB_ARGS(lot, acc, now_s), on_db, q);
}

static int64_t now_s(void) { return kn_wall_ms() / 1000; }

static void on_account(void *ud, const char *err, const cJSON *acc) {
    Pending *q = ud;
    const int64_t id = acc && !err ? kn_json_int(acc, "id", 0) : 0;
    if (id <= 0) {
        if (!q->participate) {
            q->account = 0; /* a dead token reads as a guest's view */
            run_db(q, now_s());
            return;
        }
        reply_error(q->req, 401, "LoginRequired");
        pending_done(q);
        return;
    }
    q->account = id;
    run_db(q, now_s());
}

/* the token of a request: ?token= first, then Bearer (turf's order) */
static int token_of(kn_http_req *req, char *out, size_t cap) {
    out[0] = 0;
    if (kn_http_query_get(req, "token", out, cap) <= 0) {
        const char *bearer = kn_http_bearer(req);
        snprintf(out, cap, "%s", bearer ? bearer : "");
    }
    return out[0] != 0;
}

static void lottery_id_of(kn_http_req *req, Pending *q) {
    const char *s = kn_http_param(req, "id");
    q->lottery = s ? strtoll(s, NULL, 10) : 0;
}

/* a body field as text: v10 sends the cost_ref as a string, a number is accepted too */
static void body_text(const cJSON *body, const char *key, char *out, size_t cap) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(body, key);
    out[0] = 0;
    if (cJSON_IsString(v)) snprintf(out, cap, "%s", v->valuestring);
    else if (cJSON_IsNumber(v)) snprintf(out, cap, "%lld", (long long) v->valuedouble);
}

static void h_view(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) {
        reply_error(req, 503, "LotteryUnavailable");
        return;
    }
    Pending *q = calloc(1, sizeof *q);
    if (!q) {
        reply_error(req, 500, "internal");
        return;
    }
    q->req = req;
    lottery_id_of(req, q);
    char token[512];
    if (!token_of(req, token, sizeof token)) {
        run_db(q, now_s());
        return;
    }
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(L.b, "token", args, on_account, q);
    cJSON_Delete(args);
}

static void h_participate(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) {
        reply_error(req, 503, "LotteryUnavailable");
        return;
    }
    Pending *q = calloc(1, sizeof *q);
    if (!q) {
        reply_error(req, 500, "internal");
        return;
    }
    q->req = req;
    q->participate = 1;
    lottery_id_of(req, q);
    cJSON *body = kn_http_body_json(req);
    body_text(body, "cost_type", q->cost_type, sizeof q->cost_type);
    body_text(body, "cost_ref", q->cost_ref, sizeof q->cost_ref);
    const cJSON *bundles = cJSON_GetObjectItemCaseSensitive(body, "cost_quantity");
    q->bundles = cJSON_IsNumber(bundles) ? (int64_t) bundles->valuedouble : 0;
    cJSON_Delete(body);
    char token[512];
    if (!token_of(req, token, sizeof token)) {
        reply_error(req, 401, "LoginRequired");
        free(q);
        return;
    }
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(L.b, "token", args, on_account, q);
    cJSON_Delete(args);
}

/* ------------------------------------------------------------------ settlement timer */

static void settle_done(kn_db_result *res, void *ud) {
    (void) ud;
    L.settling = 0;
    if (!kn_db_ok(res)) {
        KN_WARN(MOD, "settle failed: %s", kn_db_error(res));
        return;
    }
    const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : NULL;
    cJSON *o = t ? kn_json_parse(t, strlen(t)) : NULL;
    const int64_t drawn = o ? kn_json_int(o, "drawn", 0) : 0;
    if (drawn > 0) KN_INFO(MOD, "settled %lld lottery draw(s)", (long long) drawn);
    cJSON_Delete(o);
}

static void tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (!db_ready() || L.settling) return;
    L.settling = 1;
    char now[24];
    snprintf(now, sizeof now, "%lld", (long long) now_s());
    kn_db_call(L.b->db, &P_LOTTERY_SETTLE, KN_DB_ARGS(now), settle_done, NULL);
}

/* ------------------------------------------------------------------ module */

int lottery_init(Brain *b) {
    L.b = b;
    L.enabled = kn_config_bool(b->cfg, "brain.lottery.enabled", 1);
    kn_http_route(b->http, "GET", "/api/lottery/:id", h_view, b);
    kn_http_route(b->http, "POST", "/api/lottery/:id/participate", h_participate, b);
    /* the v10 root paths (v10.js 26566-26575) */
    kn_http_route(b->http, "GET", "/lottery/:id", h_view, b);
    kn_http_route(b->http, "POST", "/lottery/:id/participate", h_participate, b);
    if (!b->db || !L.enabled) return 0;
    L.timer = kn_loop_add_timer(b->loop, 2000, (uint64_t) kn_config_int(b->cfg, "brain.lottery.settle_check_ms", 60000), tick, NULL);
    KN_INFO(MOD, "lottery on /api/lottery/*");
    return 0;
}

void lottery_shutdown(Brain *b) {
    if (L.timer) kn_loop_cancel_timer(b->loop, L.timer);
    L.timer = 0;
    L.enabled = 0;
}
