/*
 * services/brain/guest.c - the signed-out guest's balance over HTTP (Krunker v10.0.2; the procs are in
 * db_guest.c, the table in migration 0350_guest_profiles). Routes at the brain's root, as v10 has them:
 *   GET  /guest/profile?guest=<id>     200 {data: {profile: {funds, score}}}. A valid unknown id reads
 *                                      zeros and creates nothing.
 *   POST /guest/claim                  body {"guest": "<id>"} -> 200 {data: {profile: {funds: 0, score: 0}}}
 *                                      (zeroes an existing balance, creates nothing).
 * A missing or bad id (not 32 lowercase hex digits) is 400 {error, message}; without a database 503
 * GuestUnavailable. The round's credit is not a route: the game servers send the guest_credit op (accounts.c)
 * through the internal WebSocket.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "brain_internal.h"
#include "db_procs.h"
#include "knsvc/db.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "guest"

static struct {
    Brain *b;
} G;

static void reply_error(kn_http_req *req, int status, const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", msg);
    cJSON_AddStringToObject(o, "message", msg);
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static int db_ready(void) { return G.b && G.b->db; }

/* the proc's one JSON column ({funds, score}) as {data: {profile: ...}} */
static void on_profile(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    if (!kn_db_ok(res)) {
        const char *msg = kn_db_user_message(res);
        if (!msg) KN_WARN(MOD, "profile failed: %s", kn_db_error(res));
        reply_error(req, msg ? 400 : 500, msg ? msg : "internal");
        return;
    }
    const char *t = kn_db_rows(res) > 0 ? kn_db_get(res, 0, 0) : NULL;
    cJSON *profile = t ? kn_json_parse(t, strlen(t)) : NULL;
    if (!profile) {
        reply_error(req, 500, "internal");
        return;
    }
    cJSON *data = cJSON_CreateObject();
    cJSON_AddItemToObject(data, "profile", profile);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "data", data);
    kn_http_set_header(req, "Cache-Control", "no-cache, no-store, must-revalidate");
    brain_reply_json(req, 200, o);
    cJSON_Delete(o);
}

static void h_profile(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) {
        reply_error(req, 503, "GuestUnavailable");
        return;
    }
    char guest[64];
    /* -1 absent, -2 too long for the buffer: both fail the 32-digit check below */
    if (kn_http_query_get(req, "guest", guest, sizeof guest) < 0 || !guest_id_valid(guest)) {
        reply_error(req, 400, "Bad guest id.");
        return;
    }
    kn_db_call(G.b->db, &P_GUEST_PROFILE, KN_DB_ARGS(guest), on_profile, req);
}

static void h_claim(kn_http_req *req, void *ud) {
    (void) ud;
    if (!db_ready()) {
        reply_error(req, 503, "GuestUnavailable");
        return;
    }
    char guest[64] = "";
    cJSON *body = kn_http_body_json(req);
    const cJSON *id = cJSON_GetObjectItemCaseSensitive(body, "guest");
    if (cJSON_IsString(id)) snprintf(guest, sizeof guest, "%s", id->valuestring);
    cJSON_Delete(body);
    if (!guest_id_valid(guest)) {
        reply_error(req, 400, "Bad guest id.");
        return;
    }
    kn_db_call(G.b->db, &P_GUEST_CLAIM, KN_DB_ARGS(guest), on_profile, req);
}

int guest_init(Brain *b) {
    G.b = b;
    kn_http_route(b->http, "GET", "/guest/profile", h_profile, b);
    kn_http_route(b->http, "POST", "/guest/claim", h_claim, b);
    KN_INFO(MOD, "guest balances on /guest/profile and /guest/claim");
    return 0;
}

void guest_shutdown(Brain *b) {
    (void) b;
    G.b = NULL;
}
