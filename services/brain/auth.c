/*
 * services/brain/auth.c - central auth role (roles.h BRAIN_ROLE_AUTH): accounts and
 * session tokens over HTTPS JSON (OpenResty / Cloudflare terminate TLS), plus the
 * Mojang-style join handshake (doc/backend.md "Central auth"):
 *
 *   POST /auth/register {username, password}   -> {ok, token, expiresAt, account}
 *   POST /auth/login    {username, password}   -> same
 *   POST /auth/refresh  {token} | Bearer       -> same (sliding expiry, token kept)
 *   POST /auth/logout   {token} | Bearer       -> {ok}
 *   GET  /auth/me       Bearer | ?token=       -> {ok, account}
 *   POST /auth/join     {token, serverHash}    -> {ok}       (client, before JOIN)
 *   GET  /auth/hasJoined?username=&serverHash=[&ip=] -> {id, username, profile} | 204
 *
 * Passwords, hashing, sessions and bans are accounts.c's (acct_auth, acct_revoke_token);
 * this file adds no SQL. Join records live in memory for AUTH_JOIN_TTL_MS
 * (auth_state.c). Errors: {"ok":false,"error":"<user-facing message>"}.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "acct_internal.h"
#include "auth_state.h"
#include "brain_internal.h"
#include "roles.h"
#include "knsvc/config.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "auth"
#define HASH_MAX 64

static struct {
    Brain *b;
    AuthJoins *joins;
    AuthLimiter *login, *reg, *join, *has_joined;
    kn_timer_id purge_timer;
    int check_ip;
} g_auth;

/* ---------------------------------------------------------------- helpers */

static void reply(kn_http_req *req, int status, cJSON *o) {
    brain_reply_json(req, status, o);
    cJSON_Delete(o);
}

static void reply_err(kn_http_req *req, int status, const char *msg) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 0);
    cJSON_AddStringToObject(o, "error", msg ? msg : "Server error.");
    reply(req, status, o);
}

static int err_status(const char *err) {
    if (!strcmp(err, "Accounts are unavailable.")) return 503;
    if (!strncmp(err, "Server", 6)) return 503;
    if (!strcmp(err, "Please Try Later") || strstr(err, "Too many")) return 429;
    if (!strncmp(err, "Banned", 6) || !strcmp(err, "Account banned.")) return 403;
    if (!strcmp(err, "Username already taken.")) return 409;
    if (!strcmp(err, "Invalid login.") || !strcmp(err, "Invalid credentials.") || !strcmp(err, ACCT_ERR_2FA))
        return 401;
    return 400;
}

static const char *body_str(const cJSON *body, const char *a, const char *b) {
    const char *v = kn_json_str(body, a, NULL);
    if ((!v || !*v) && b) v = kn_json_str(body, b, NULL);
    return v && *v ? v : NULL;
}

/* Token of a request: JSON body "token", ?token=, or Authorization: Bearer. */
static void req_token(kn_http_req *req, const cJSON *body, char *out, size_t cap) {
    out[0] = 0;
    const char *t = kn_json_str(body, "token", NULL);
    if (t && *t) {
        snprintf(out, cap, "%s", t);
        return;
    }
    if (kn_http_query_get(req, "token", out, cap) > 0) return;
    out[0] = 0;
    const char *bearer = kn_http_bearer(req);
    if (bearer) snprintf(out, cap, "%s", bearer);
}

static int limited(AuthLimiter *l, kn_http_req *req) {
    if (auth_limiter_hit(l, req->remote_addr ? req->remote_addr : "", kn_now_ms())) return 0;
    reply_err(req, 429, "Too many requests");
    return 1;
}

/* The public profile handed to game servers / coordinators (no token, no private data). */
static cJSON *profile_of(const cJSON *account) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "id", (double) kn_json_int(account, "id", 0));
    cJSON_AddStringToObject(p, "username", kn_json_str(account, "username", ""));
    const char *alias = kn_json_str(account, "alias", NULL);
    if (alias && *alias) cJSON_AddStringToObject(p, "alias", alias);
    else cJSON_AddNullToObject(p, "alias");
    cJSON_AddNumberToObject(p, "premium", (double) kn_json_int(account, "premium", 0));
    cJSON_AddNumberToObject(p, "dev", (double) kn_json_int(account, "dev", 0));
    cJSON_AddNumberToObject(p, "mod", (double) kn_json_int(account, "mod", 0));
    cJSON_AddNumberToObject(p, "banned", (double) kn_json_int(account, "banned", 0));
    const char *reason = kn_json_str(account, "ban_reason", NULL);
    if (reason) cJSON_AddStringToObject(p, "ban_reason", reason);
    else cJSON_AddNullToObject(p, "ban_reason");
    return p;
}

/* ---------------------------------------------------------------- async contexts */

typedef struct AuthReq {
    kn_http_req *req;
    char server_hash[HASH_MAX + 8];
    char ip[64];
} AuthReq;

static AuthReq *auth_req_new(kn_http_req *req) {
    AuthReq *a = calloc(1, sizeof(*a));
    if (!a) {
        reply_err(req, 503, "Server error.");
        return NULL;
    }
    a->req = req;
    snprintf(a->ip, sizeof(a->ip), "%s", req->remote_addr ? req->remote_addr : "");
    return a;
}

/* login / register / refresh -> {ok, token, expiresAt, account} */
static void on_session(void *ud, const char *err, const cJSON *account) {
    AuthReq *a = ud;
    if (err || !account) {
        reply_err(a->req, err_status(err ? err : "Invalid login."), err ? err : "Invalid login.");
        free(a);
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    cJSON_AddStringToObject(o, "token", kn_json_str(account, "token", ""));
    cJSON_AddNumberToObject(o, "expiresAt", (double) (time(NULL) + g_acct.session_ttl_s));
    cJSON_AddItemToObject(o, "account", profile_of(account));
    reply(a->req, 200, o);
    free(a);
}

static void password_auth(kn_http_req *req, int action) {
    if (limited(action == 0 ? g_auth.reg : g_auth.login, req)) return;
    cJSON *body = kn_http_body_json(req);
    const char *name = body_str(body, "username", "name");
    const char *pass = body_str(body, "password", "pass");
    if (!name || !pass) {
        cJSON_Delete(body);
        reply_err(req, 400, "Invalid credentials.");
        return;
    }
    AuthReq *a = auth_req_new(req);
    if (!a) {
        cJSON_Delete(body);
        return;
    }
    AcctAuthReq rq = {.action = action, .name = name, .pass = pass, .ip = a->ip, .origin = "http",
                      .twofa = NULL};
    acct_auth(&rq, on_session, a); /* copies what it keeps */
    cJSON_Delete(body);
}

static void h_register(kn_http_req *req, void *ud) {
    (void) ud;
    password_auth(req, 0);
}

static void h_login(kn_http_req *req, void *ud) {
    (void) ud;
    password_auth(req, 1);
}

static void token_auth(kn_http_req *req, AcctReplyFn cb, AuthReq *a) {
    cJSON *body = kn_http_body_json(req);
    char token[128];
    req_token(req, body, token, sizeof(token));
    cJSON_Delete(body);
    if (!token[0]) {
        cb(a, "Invalid login.", NULL);
        return;
    }
    AcctAuthReq rq = {.action = 1, .token = token, .ip = a->ip, .origin = "http"};
    acct_auth(&rq, cb, a);
}

static void h_refresh(kn_http_req *req, void *ud) {
    (void) ud;
    AuthReq *a = auth_req_new(req);
    if (a) token_auth(req, on_session, a);
}

static void on_me(void *ud, const char *err, const cJSON *account) {
    AuthReq *a = ud;
    if (err || !account) {
        reply_err(a->req, err_status(err ? err : "Invalid login."), err ? err : "Invalid login.");
    } else {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", 1);
        cJSON_AddItemToObject(o, "account", profile_of(account));
        reply(a->req, 200, o);
    }
    free(a);
}

static void h_me(kn_http_req *req, void *ud) {
    (void) ud;
    AuthReq *a = auth_req_new(req);
    if (a) token_auth(req, on_me, a);
}

static void h_logout(kn_http_req *req, void *ud) {
    (void) ud;
    cJSON *body = kn_http_body_json(req);
    char token[128];
    req_token(req, body, token, sizeof(token));
    cJSON_Delete(body);
    if (token[0]) acct_revoke_token(token);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "ok", 1);
    reply(req, 200, o);
}

/* ---------------------------------------------------------------- join / hasJoined */

static int valid_hash(const char *h) {
    if (!h) return 0;
    size_t n = strlen(h);
    if (n < 16 || n > HASH_MAX) return 0;
    for (size_t i = 0; i < n; i++) {
        const char c = h[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return 0;
    }
    return 1;
}

static void on_join_account(void *ud, const char *err, const cJSON *account) {
    AuthReq *a = ud;
    if (err || !account) {
        reply_err(a->req, err_status(err ? err : "Invalid login."), err ? err : "Invalid login.");
        free(a);
        return;
    }
    cJSON *profile = profile_of(account);
    const int rc = auth_joins_put(g_auth.joins, a->server_hash, kn_json_int(account, "id", 0),
                                  kn_json_str(account, "username", ""), a->ip, profile, kn_now_ms());
    cJSON_Delete(profile);
    if (rc) {
        reply_err(a->req, 503, "Server error.");
    } else {
        KN_DEBUG(MOD, "join %s -> %.12s...", kn_json_str(account, "username", "?"), a->server_hash);
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "ok", 1);
        cJSON_AddNumberToObject(o, "ttl", AUTH_JOIN_TTL_MS / 1000);
        reply(a->req, 200, o);
    }
    free(a);
}

static void h_join(kn_http_req *req, void *ud) {
    (void) ud;
    if (limited(g_auth.join, req)) return;
    cJSON *body = kn_http_body_json(req);
    const char *hash = body_str(body, "serverHash", "serverId");
    if (!valid_hash(hash)) {
        cJSON_Delete(body);
        reply_err(req, 400, "Invalid serverHash.");
        return;
    }
    char token[128];
    req_token(req, body, token, sizeof(token));
    AuthReq *a = auth_req_new(req);
    if (!a) {
        cJSON_Delete(body);
        return;
    }
    snprintf(a->server_hash, sizeof(a->server_hash), "%s", hash);
    cJSON_Delete(body);
    if (!token[0]) {
        on_join_account(a, "Invalid login.", NULL);
        return;
    }
    AcctAuthReq rq = {.action = 1, .token = token, .ip = a->ip, .origin = "http"};
    acct_auth(&rq, on_join_account, a);
}

static void h_has_joined(kn_http_req *req, void *ud) {
    (void) ud;
    if (limited(g_auth.has_joined, req)) return;
    char name[64] = "", hash[HASH_MAX + 8] = "", ip[64] = "";
    if (kn_http_query_get(req, "username", name, sizeof(name)) <= 0 ||
        kn_http_query_get(req, "serverHash", hash, sizeof(hash)) <= 0 || !valid_hash(hash)) {
        reply_err(req, 400, "username and serverHash are required");
        return;
    }
    kn_http_query_get(req, "ip", ip, sizeof(ip));
    const cJSON *profile = auth_joins_get(g_auth.joins, hash, name, ip, g_auth.check_ip, kn_now_ms());
    if (!profile) {
        brain_cors_headers(req);
        kn_http_respond(req, 204, NULL, NULL, 0); /* Mojang: 204 = not joined */
        return;
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) kn_json_int(profile, "id", 0));
    cJSON_AddStringToObject(o, "username", kn_json_str(profile, "username", name));
    cJSON_AddItemToObject(o, "profile", cJSON_Duplicate(profile, 1));
    reply(req, 200, o);
}

/* ---------------------------------------------------------------- module */

static void purge_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    auth_joins_purge(g_auth.joins, kn_now_ms());
}

int auth_init(Brain *b) {
    g_auth.b = b;
    const struct kn_config *cfg = b->cfg;
    g_auth.joins = auth_joins_new();
    g_auth.login = auth_limiter_new((int) kn_config_int(cfg, "auth.login_per_min", 10), 60 * 1000);
    g_auth.reg = auth_limiter_new((int) kn_config_int(cfg, "auth.register_per_hour", 5), 3600 * 1000);
    g_auth.join = auth_limiter_new((int) kn_config_int(cfg, "auth.join_per_min", 60), 60 * 1000);
    g_auth.has_joined = auth_limiter_new((int) kn_config_int(cfg, "auth.has_joined_per_min", 1200), 60 * 1000);
    g_auth.check_ip = kn_config_bool(cfg, "auth.check_ip", 0);
    if (!g_auth.joins || !g_auth.login || !g_auth.reg || !g_auth.join || !g_auth.has_joined) return -1;
    g_auth.purge_timer = kn_loop_add_timer(b->loop, 5000, 5000, purge_tick, NULL);
    kn_http_route(b->http, "POST", "/auth/register", h_register, b);
    kn_http_route(b->http, "POST", "/auth/login", h_login, b);
    kn_http_route(b->http, "POST", "/auth/refresh", h_refresh, b);
    kn_http_route(b->http, "POST", "/auth/logout", h_logout, b);
    kn_http_route(b->http, "GET", "/auth/me", h_me, b);
    kn_http_route(b->http, "POST", "/auth/join", h_join, b);
    kn_http_route(b->http, "GET", "/auth/hasJoined", h_has_joined, b);
    kn_http_route(b->http, "GET", "/auth/has-joined", h_has_joined, b);
    if (!b->db) KN_WARN(MOD, "no database: /auth/* answer \"Accounts are unavailable.\"");
    KN_INFO(MOD, "central auth ready (join ttl %d s, ip check %s)", AUTH_JOIN_TTL_MS / 1000,
            g_auth.check_ip ? "on" : "off");
    return 0;
}

void auth_shutdown(Brain *b) {
    if (g_auth.purge_timer) kn_loop_cancel_timer(b->loop, g_auth.purge_timer);
    g_auth.purge_timer = 0;
    auth_joins_free(g_auth.joins);
    auth_limiter_free(g_auth.login);
    auth_limiter_free(g_auth.reg);
    auth_limiter_free(g_auth.join);
    auth_limiter_free(g_auth.has_joined);
    memset(&g_auth, 0, sizeof(g_auth));
}
