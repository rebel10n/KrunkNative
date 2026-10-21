/*
 * services/brain/v10_account.c - the v10 account, player, ranked and wallet routes at the brain root (api.krunker.io
 * and gapi.svc.krunker.io both answer at "/"). Each route answers the v10 envelope; doc/v10/api-inventory.md has each
 * route's row. The v10.0.3 consumer is cited by its decoded line, the v10.js caller by the inventory.
 *
 * DATA (the caller's own data, with its KrunkNative source):
 *   GET  /match-history/me/season/:s/gameMode/:m/isRanked/:x[/region/:r]  decoded 4921 (v10.js 25940, 98633): the
 *        ranked matches of the caller (ranked.c, the mm_ranked tables; the history shape with historyEntries). Any
 *        isRanked other than "true" has no KrunkNative source in that shape: {matches: [], totalPages: 0}.
 *   GET  /player-profile/me/season/:s[/region/:r]        decoded 3734 (v10.js 25959, 98579): ranked.c's profile
 *   GET  /leaderboards/rankings/season/:s[/region/:r]    decoded 5160 (v10.js 25972, 98600): ranked.c's board
 *   POST /player-profile/me/claim-milestone/region/:r    decoded 5160 (v10.js 25976, 98741): ranked.c's milestone
 *   GET  /daily-spin/progress                            decoded 3573 (v10.js 25999, 117993): daily_spin.c's state
 *   POST /spins/execute                                  decoded 5160 (v10.js 27299; no caller): the socket's store
 *        spin (store.c, store_spin_v10), with {index, ticket, multi}
 *   GET  /player/me                                      decoded 5160 (v10.js 26312; no caller): the account
 *   GET  /player/wallet                                  decoded 4527 (v10.js 26317; no caller): {funds, junk}
 *   POST /webpush/status, /webpush/subscription-change   decoded 62, 3323 (v10.js 26408, 26414): push.c
 *   GET  /ipinfo                                         decoded 117 (v10.js 16440; no reader): {ip} of the peer
 *   GET  /krunkcup/wristband                             decoded 4919 (v10.js 27309; no caller): data null, no wristband
 * REFUSAL (403 NOT_IMPLEMENTED: a real write Box Office cannot do):
 *   payments, no payment backend: /shop/cancel-subscription 26006, /iap/checkout 26018, /iap/claim 26024,
 *   /my-api/create-payment/:id 209123, /my-api/execute-payment/:id 209131 (decoded 5160, 3546, 2704, 3576).
 *   /krunkcup/wristband/claim 27314: Box Office has no wristband to claim.
 * A route that needs the caller answers 401 LOGIN_REQUIRED without a session.
 */
#include "v10_account.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "acct_internal.h"
#include "accounts.h"
#include "push.h"
#include "v10_api.h"
#include "knsvc/http.h"
#include "knsvc/json.h"

#define WHY_PAYMENT "Box Office has no payment backend"
#define WHY_WRISTBAND "Box Office has no wristband to claim"

static Brain *s_brain;

/* ---- the caller: the bearer token (or ?token=) through the accounts' token op */

static void caller_token(kn_http_req *req, char *out, size_t cap) {
    if (kn_http_query_get(req, "token", out, cap) > 0) return;
    const char *bearer = kn_http_bearer(req);
    snprintf(out, cap, "%s", bearer ? bearer : "");
}

/* the account of a signed-in caller: fn(req, account, ud); else the 401. drop frees ud when no account is found */
typedef void (*SignedInFn)(kn_http_req *req, const cJSON *acc, void *ud);
typedef struct {
    kn_http_req *req;
    SignedInFn fn;
    void *ud;
    void (*drop)(void *ud);
} SignedIn;

static void on_signed_in(void *ud, const char *err, const cJSON *acc) {
    SignedIn *c = ud;
    const int64_t id = acc && !err ? kn_json_int(acc, "id", 0) : 0;
    if (id <= 0) {
        v10_reply_error(c->req, 401, "LOGIN_REQUIRED", "You must be logged in.");
        if (c->drop) c->drop(c->ud);
    } else {
        c->fn(c->req, acc, c->ud);
    }
    free(c);
}

static void with_signed_in(kn_http_req *req, SignedInFn fn, void *ud, void (*drop)(void *)) {
    char token[512];
    caller_token(req, token, sizeof token);
    if (!token[0]) {
        v10_reply_error(req, 401, "LOGIN_REQUIRED", "You must be logged in.");
        if (drop) drop(ud);
        return;
    }
    SignedIn *c = calloc(1, sizeof *c);
    if (!c) {
        v10_reply_error(req, 500, "internal", "out of memory");
        if (drop) drop(ud);
        return;
    }
    c->req = req;
    c->fn = fn;
    c->ud = ud;
    c->drop = drop;
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", token);
    accounts_internal_call(s_brain, "token", args, on_signed_in, c); /* on failure it has answered and freed c */
    cJSON_Delete(args);
}

/* ---- DATA: the account, the wallet, the IP */

/* GET /player/me: the signed-in account (the token op's account object, without the token) */
static void me_account(kn_http_req *req, const cJSON *acc, void *ud) {
    (void) ud;
    cJSON *d = cJSON_Duplicate(acc, 1);
    cJSON_DeleteItemFromObjectCaseSensitive(d, "token");
    v10_reply_data(req, 200, d);
}

static void h_me(kn_http_req *req, void *ud) {
    (void) ud;
    with_signed_in(req, me_account, NULL, NULL);
}

/* GET /player/wallet: the KR balance (the account's funds: players.player_funds, moved only by kr_transactions) and junk */
static void wallet_account(kn_http_req *req, const cJSON *acc, void *ud) {
    (void) ud;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddNumberToObject(d, "funds", (double) kn_json_int(acc, "funds", 0));
    cJSON_AddStringToObject(d, "junk", kn_json_str(acc, "junk", "0.00"));
    v10_reply_data(req, 200, d);
}

static void h_wallet(kn_http_req *req, void *ud) {
    (void) ud;
    with_signed_in(req, wallet_account, NULL, NULL);
}

/* GET /ipinfo: the caller's peer address (req->remote_addr; the forwarded one behind a trusted proxy) */
static void h_ipinfo(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "ip", req->remote_addr ? req->remote_addr : "");
    v10_reply_data(req, 200, d);
}

/* GET /krunkcup/wristband: Box Office has no wristband, so no status (data null, the v10 none value) */
static void h_wristband(kn_http_req *req, void *ud) {
    (void) ud;
    v10_reply_data(req, 200, NULL);
}

/* ---- ranked and daily spin: the caller's own reads */

static void h_profile(kn_http_req *req, void *ud) {
    (void) ud;
    ranked_v10_profile(req, kn_http_param(req, "s"), kn_http_param(req, "r"));
}

static void h_board(kn_http_req *req, void *ud) {
    (void) ud;
    ranked_v10_leaderboard(req, kn_http_param(req, "s"), kn_http_param(req, "r"));
}

/* GET .../isRanked/:x: only the ranked list has a source in v10's shape (the consumer asks for true) */
static void h_history(kn_http_req *req, void *ud) {
    (void) ud;
    const char *ranked = kn_http_param(req, "x");
    if (ranked && !strcmp(ranked, "true")) {
        ranked_v10_history(req, kn_http_param(req, "s"), kn_http_param(req, "r"));
        return;
    }
    cJSON *mh = cJSON_CreateObject();
    cJSON_AddItemToObject(mh, "matches", cJSON_CreateArray());
    cJSON_AddNumberToObject(mh, "totalPages", 0);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "matchHistory", mh);
    v10_reply_data(req, 200, d);
}

static void h_claim(kn_http_req *req, void *ud) {
    (void) ud;
    ranked_v10_claim_milestone(req, kn_http_param(req, "r"));
}

static void daily_account(kn_http_req *req, const cJSON *acc, void *ud) {
    (void) ud;
    daily_spin_progress_v10(req, kn_json_int(acc, "id", 0));
}

static void h_daily(kn_http_req *req, void *ud) {
    (void) ud;
    with_signed_in(req, daily_account, NULL, NULL);
}

/* ---- the store spin (POST /spins/execute {index, ticket, multi}) */

static void spin_account(kn_http_req *req, const cJSON *acc, void *ud) {
    cJSON *body = ud;
    const cJSON *multi = body ? cJSON_GetObjectItemCaseSensitive(body, "multi") : NULL;
    const int want_multi = cJSON_IsTrue(multi) || (cJSON_IsNumber(multi) && multi->valuedouble > 1);
    store_spin_v10(req, kn_json_int(acc, "id", 0), kn_json_int(body, "index", -1), kn_json_int(body, "ticket", -1),
                   want_multi);
    cJSON_Delete(body);
}

static void drop_json(void *ud) { cJSON_Delete(ud); }

static void h_spin(kn_http_req *req, void *ud) {
    (void) ud;
    with_signed_in(req, spin_account, kn_http_body_json(req), drop_json);
}

/* ---- web push (push.c): the caller's own rows */

static void h_webpush_status(kn_http_req *req, void *ud) {
    (void) ud;
    push_v10_status(req);
}

static void h_webpush_change(kn_http_req *req, void *ud) {
    (void) ud;
    push_v10_subscription_change(req);
}

/* ---- REFUSAL: ud is the reason (a string literal) */

static void h_refuse(kn_http_req *req, void *ud) {
    const char *why = ud;
    v10_reply_error(req, 403, "NOT_IMPLEMENTED", why);
}

/* ---- the route table: method, pattern (kn_http_route), handler, ud */

typedef struct {
    const char *method;
    const char *pattern;
    kn_http_handler handler;
    void *ud;
} AccountRoute;

static const AccountRoute k_routes[] = {
    {"GET", "/match-history/me/season/:s/gameMode/:m/isRanked/:x", h_history, NULL},
    {"GET", "/match-history/me/season/:s/gameMode/:m/isRanked/:x/region/:r", h_history, NULL},
    {"GET", "/player-profile/me/season/:s", h_profile, NULL},
    {"GET", "/player-profile/me/season/:s/region/:r", h_profile, NULL},
    {"GET", "/leaderboards/rankings/season/:s", h_board, NULL},
    {"GET", "/leaderboards/rankings/season/:s/region/:r", h_board, NULL},
    {"POST", "/player-profile/me/claim-milestone/region/:r", h_claim, NULL},
    {"GET", "/daily-spin/progress", h_daily, NULL},
    {"POST", "/shop/cancel-subscription", h_refuse, (void *) WHY_PAYMENT},
    {"POST", "/iap/checkout", h_refuse, (void *) WHY_PAYMENT},
    {"POST", "/iap/claim", h_refuse, (void *) WHY_PAYMENT},
    {"POST", "/my-api/create-payment/:id", h_refuse, (void *) WHY_PAYMENT},
    {"POST", "/my-api/execute-payment/:id", h_refuse, (void *) WHY_PAYMENT},
    {"GET", "/ipinfo", h_ipinfo, NULL},
    {"GET", "/player/me", h_me, NULL},
    {"GET", "/player/wallet", h_wallet, NULL},
    {"POST", "/webpush/status", h_webpush_status, NULL},
    {"POST", "/webpush/subscription-change", h_webpush_change, NULL},
    {"POST", "/spins/execute", h_spin, NULL},
    {"GET", "/krunkcup/wristband", h_wristband, NULL},
    {"POST", "/krunkcup/wristband/claim", h_refuse, (void *) WHY_WRISTBAND},
};

int v10_account_init(Brain *b) {
    s_brain = b;
    for (size_t i = 0; i < sizeof k_routes / sizeof k_routes[0]; i++) {
        kn_http_route(b->http, k_routes[i].method, k_routes[i].pattern, k_routes[i].handler, k_routes[i].ud);
    }
    return 0;
}
