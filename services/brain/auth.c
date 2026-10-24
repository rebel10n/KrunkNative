/*
 * services/brain/auth.c - central auth role (roles.h BRAIN_ROLE_AUTH): accounts and
 * session tokens over HTTPS JSON (OpenResty / Cloudflare terminate TLS), plus the
 * Mojang-style join handshake (doc/backend.md "Central auth"):
 *
 *   POST /auth/register {username, password}   -> {ok, token, expiresAt, account}
 *   POST /auth/login    {username | email, password} -> same (the username field may hold an email)
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
#include <netdb.h>
#include <pthread.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include "acct_internal.h"
#include "auth_state.h"
#include "captcha.h"
#include "hot.h"
#include "brain_internal.h"
#include "roles.h"
#include "knsvc/config.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#define MOD "auth"
#define HASH_MAX 64

/* A per-process limiter plus its shared window in the Redis hot tier (hot_limit). */
typedef struct Limit {
    AuthLimiter *l;
    const char *bucket;
    int limit, window_s;
} Limit;

static struct {
    Brain *b;
    AuthJoins *joins;
    Limit login, reg, join, has_joined;
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

static int limited(const Limit *l, kn_http_req *req) {
    const char *ip = req->remote_addr ? req->remote_addr : "";
    if (hot_limit(l->bucket, ip, l->limit, l->window_s, auth_limiter_hit(l->l, ip, kn_now_ms()))) return 0;
    reply_err(req, 429, "Too many requests");
    return 1;
}

/* No limiter without the auth role (auth_init not run): the accounts' own throttles still apply. */
int auth_login_limit(kn_http_req *req) { return !g_auth.login.l || !limited(&g_auth.login, req); }

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
    if (limited(action == 0 ? &g_auth.reg : &g_auth.login, req)) return;
    cJSON *body = kn_http_body_json(req);
    const char *name = body_str(body, "username", "name");
    if (!name && action == 1) name = body_str(body, "email", "login"); /* acct_auth: an email signs in too */
    const char *pass = body_str(body, "password", "pass");
    if (!name || !pass) {
        cJSON_Delete(body);
        reply_err(req, 400, "Invalid credentials.");
        return;
    }
    /* KrunkNative's {captcha}: the ALTCHA payload of GET /captcha/challenge (captcha.h); refused only with
     * captcha.enforce on, before the password is hashed */
    CaptchaResult cr;
    if (!captcha_gate(kn_json_str(body, "captcha", NULL), action == 0 ? "register" : "login", req->remote_addr, &cr)) {
        cJSON_Delete(body);
        reply_err(req, 400, captcha_refusal_text(cr));
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

/* Join records in the hot tier, so hasJoined finds a join made through another auth brain
 * (local record first; Redis only on a local miss). <prefix>join:<serverHash> = {id, username, ip, profile}. */
static void join_share(const char *server_hash, const cJSON *account, const char *ip, const cJSON *profile) {
    if (!hot_up()) return;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) kn_json_int(account, "id", 0));
    cJSON_AddStringToObject(o, "username", kn_json_str(account, "username", ""));
    cJSON_AddStringToObject(o, "ip", ip ? ip : "");
    cJSON_AddItemToObject(o, "profile", cJSON_Duplicate(profile, 1));
    char *text = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char k[HASH_MAX + 16];
    snprintf(k, sizeof k, "join:%s", server_hash);
    if (text) hot_put(k, text, AUTH_JOIN_TTL_MS);
    free(text);
}

typedef void (*JoinFoundFn)(void *ud, const cJSON *profile); /* NULL = not joined */

typedef struct JoinLookup {
    char hash[HASH_MAX + 8], name[64], ip[64];
    JoinFoundFn cb;
    void *ud;
} JoinLookup;

/* A shared record found: adopt it locally (with its remaining lifetime), then run the
 * usual username / IP checks of auth_joins_get. */
static void join_fetched(void *ud, const char *value, size_t len, int64_t pttl_ms) {
    JoinLookup *j = ud;
    const cJSON *profile = NULL;
    cJSON *o = value ? cJSON_ParseWithLength(value, len) : NULL;
    if (o && pttl_ms > 0) {
        const uint64_t now = kn_now_ms();
        const uint64_t age = pttl_ms < AUTH_JOIN_TTL_MS ? (uint64_t) (AUTH_JOIN_TTL_MS - pttl_ms) : 0;
        if (auth_joins_put(g_auth.joins, j->hash, kn_json_int(o, "id", 0), kn_json_str(o, "username", ""),
                           kn_json_str(o, "ip", ""), cJSON_GetObjectItemCaseSensitive(o, "profile"), now - age) == 0)
            profile = auth_joins_get(g_auth.joins, j->hash, j->name, j->ip, g_auth.check_ip, now);
    }
    j->cb(j->ud, profile);
    cJSON_Delete(o);
    free(j);
}

/* Local record, else the hot tier's; cb exactly once (maybe synchronously). */
static void join_lookup(const char *hash, const char *name, const char *ip, JoinFoundFn cb, void *ud) {
    const cJSON *p = auth_joins_get(g_auth.joins, hash, name, ip, g_auth.check_ip, kn_now_ms());
    if (p || !hot_up()) {
        cb(ud, p);
        return;
    }
    JoinLookup *j = calloc(1, sizeof *j);
    char k[HASH_MAX + 16];
    snprintf(k, sizeof k, "join:%s", hash);
    if (j) {
        snprintf(j->hash, sizeof j->hash, "%s", hash);
        snprintf(j->name, sizeof j->name, "%s", name);
        snprintf(j->ip, sizeof j->ip, "%s", ip ? ip : "");
        j->cb = cb;
        j->ud = ud;
        if (hot_get(k, join_fetched, j) == 0) return;
        free(j);
    }
    cb(ud, NULL);
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
    if (!rc) join_share(a->server_hash, account, a->ip, profile);
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
    if (limited(&g_auth.join, req)) return;
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

typedef struct HasJoinedReq {
    kn_http_req *req;
    char name[64];
} HasJoinedReq;

static void has_joined_found(void *ud, const cJSON *profile) {
    HasJoinedReq *h = ud;
    kn_http_req *req = h->req;
    if (!profile) {
        brain_cors_headers(req);
        kn_http_respond(req, 204, NULL, NULL, 0); /* Mojang: 204 = not joined */
    } else {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", (double) kn_json_int(profile, "id", 0));
        cJSON_AddStringToObject(o, "username", kn_json_str(profile, "username", h->name));
        cJSON_AddItemToObject(o, "profile", cJSON_Duplicate(profile, 1));
        reply(req, 200, o);
    }
    free(h);
}

static void h_has_joined(kn_http_req *req, void *ud) {
    (void) ud;
    if (limited(&g_auth.has_joined, req)) return;
    char name[64] = "", hash[HASH_MAX + 8] = "", ip[64] = "";
    if (kn_http_query_get(req, "username", name, sizeof(name)) <= 0 ||
        kn_http_query_get(req, "serverHash", hash, sizeof(hash)) <= 0 || !valid_hash(hash)) {
        reply_err(req, 400, "username and serverHash are required");
        return;
    }
    kn_http_query_get(req, "ip", ip, sizeof(ip));
    HasJoinedReq *h = calloc(1, sizeof *h);
    if (!h) {
        reply_err(req, 503, "Server error.");
        return;
    }
    h->req = req;
    snprintf(h->name, sizeof h->name, "%s", name);
    join_lookup(hash, name, ip, has_joined_found, h);
}

/* ---------------------------------------------------------------- hasJoined for the community role */

typedef struct RemoteHj {
    Brain *b;
    char url[768];
    int status;
    char *body;
    AuthHasJoinedCb cb;
    void *ud;
} RemoteHj;

static void remote_hj_done(kn_loop *loop, void *ud) {
    (void) loop;
    RemoteHj *r = ud;
    cJSON *res = r->status == 200 ? cJSON_Parse(r->body ? r->body : "") : NULL;
    if (r->status == 200 && cJSON_IsObject(res) && kn_json_int(res, "id", 0) > 0) {
        cJSON *profile = cJSON_GetObjectItemCaseSensitive(res, "profile");
        if (!cJSON_IsObject(profile)) profile = res;
        r->cb(r->ud, NULL, profile);
    } else {
        r->cb(r->ud, r->status == 204 ? "Failed to verify username" : r->status ? "auth service error" : "Authentication servers are down", NULL);
    }
    cJSON_Delete(res);
    free(r->body);
    free(r);
}

#ifdef KN_HAVE_CURL
#include <curl/curl.h>
static size_t hj_write(char *data, size_t size, size_t n, void *ud) {
    RemoteHj *r = ud;
    const size_t len = size * n, have = r->body ? strlen(r->body) : 0;
    if (have + len > 65536) return 0;
    char *grown = realloc(r->body, have + len + 1);
    if (!grown) return 0;
    memcpy(grown + have, data, len);
    grown[have + len] = 0;
    r->body = grown;
    return len;
}
#endif

static void *remote_hj_thread(void *arg) {
    RemoteHj *r = arg;
#ifdef KN_HAVE_CURL
    CURL *c = curl_easy_init();
    if (c) {
        curl_easy_setopt(c, CURLOPT_URL, r->url);
        curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, 8000L);
        curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, hj_write);
        curl_easy_setopt(c, CURLOPT_WRITEDATA, r);
        curl_easy_setopt(c, CURLOPT_USERAGENT, "kn_brain");
        if (curl_easy_perform(c) == CURLE_OK) {
            long code = 0;
            curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
            r->status = (int) code;
        }
        curl_easy_cleanup(c);
    }
#else
    /* http:// only (TLS needs libcurl): one GET, Connection: close */
    kn_url u;
    if (!strncmp(r->url, "http://", 7) && kn_url_parse(r->url, &u) == 0) {
        char port[8];
        snprintf(port, sizeof(port), "%d", u.port ? u.port : 80);
        struct addrinfo hints = {0}, *res = NULL;
        hints.ai_socktype = SOCK_STREAM;
        if (getaddrinfo(u.host, port, &hints, &res) == 0 && res) {
            const int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
            struct timeval tv = {8, 0};
            if (fd >= 0) {
                setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
                setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
            }
            if (fd >= 0 && connect(fd, res->ai_addr, res->ai_addrlen) == 0) {
                const char *path = strchr(r->url + 7, '/');
                char req[1024];
                const int n = snprintf(req, sizeof(req), "GET %s HTTP/1.1\r\nHost: %s\r\nConnection: close\r\nUser-Agent: kn_brain\r\n\r\n",
                                       path ? path : "/", u.host);
                if (n > 0 && send(fd, req, (size_t) n, MSG_NOSIGNAL) == n) {
                    char buf[16384];
                    size_t got = 0;
                    ssize_t k;
                    while (got + 1 < sizeof(buf) && (k = recv(fd, buf + got, sizeof(buf) - 1 - got, 0)) > 0) got += (size_t) k;
                    buf[got] = 0;
                    int status = 0;
                    const char *bodyp = strstr(buf, "\r\n\r\n");
                    if (sscanf(buf, "HTTP/%*s %d", &status) == 1 && bodyp) {
                        r->status = status;
                        r->body = strdup(bodyp + 4);
                    }
                }
            }
            if (fd >= 0) close(fd);
            freeaddrinfo(res);
        }
    }
#endif
    kn_loop_post_threadsafe(r->b->loop, remote_hj_done, r);
    return NULL;
}

typedef struct LocalHj {
    AuthHasJoinedCb cb;
    void *ud;
} LocalHj;

static void local_hj_found(void *ud, const cJSON *profile) {
    LocalHj *l = ud;
    l->cb(l->ud, profile ? NULL : "Failed to verify username", profile);
    free(l);
}

void auth_has_joined_async(Brain *b, const char *username, const char *server_hash, const char *ip,
                           AuthHasJoinedCb cb, void *ud) {
    if (!username || !*username || !valid_hash(server_hash)) {
        cb(ud, "Failed to verify username", NULL);
        return;
    }
    if (g_auth.joins) { /* this process is the auth service too */
        LocalHj *l = calloc(1, sizeof *l);
        if (!l) {
            cb(ud, "Server error.", NULL);
            return;
        }
        l->cb = cb;
        l->ud = ud;
        join_lookup(server_hash, username, ip, local_hj_found, l);
        return;
    }
    const char *base = kn_config_str(b->cfg, "community.auth_url", "https://coordinator.krunk.io");
    RemoteHj *r = calloc(1, sizeof(*r));
    if (!r) {
        cb(ud, "Server error.", NULL);
        return;
    }
    char user[128], ipenc[128];
    kn_url_encode(username, user, sizeof(user));
    kn_url_encode(ip ? ip : "", ipenc, sizeof(ipenc));
    size_t blen = strlen(base);
    while (blen && base[blen - 1] == '/') blen--;
    snprintf(r->url, sizeof(r->url), "%.*s/auth/hasJoined?username=%s&serverHash=%s%s%s", (int) blen, base, user,
             server_hash, ip && *ip ? "&ip=" : "", ip && *ip ? ipenc : "");
    r->b = b;
    r->cb = cb;
    r->ud = ud;
    pthread_t t;
    if (pthread_create(&t, NULL, remote_hj_thread, r) != 0) {
        free(r);
        cb(ud, "Server error.", NULL);
        return;
    }
    pthread_detach(t);
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
    const struct {
        Limit *l;
        const char *bucket, *key;
        int def, window_s;
    } limits[] = {
        {&g_auth.login, "auth_login", "auth.login_per_min", 10, 60},
        {&g_auth.reg, "auth_register", "auth.register_per_hour", 5, 3600},
        {&g_auth.join, "auth_join", "auth.join_per_min", 60, 60},
        {&g_auth.has_joined, "auth_has_joined", "auth.has_joined_per_min", 1200, 60},
    };
    for (size_t i = 0; i < sizeof limits / sizeof limits[0]; i++) {
        Limit *l = limits[i].l;
        l->bucket = limits[i].bucket;
        l->limit = (int) kn_config_int(cfg, limits[i].key, limits[i].def);
        l->window_s = limits[i].window_s;
        l->l = auth_limiter_new(l->limit, (uint64_t) l->window_s * 1000);
        if (!l->l) return -1;
    }
    g_auth.check_ip = kn_config_bool(cfg, "auth.check_ip", 0);
    if (!g_auth.joins) return -1;
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
    auth_limiter_free(g_auth.login.l);
    auth_limiter_free(g_auth.reg.l);
    auth_limiter_free(g_auth.join.l);
    auth_limiter_free(g_auth.has_joined.l);
    memset(&g_auth, 0, sizeof(g_auth));
}
