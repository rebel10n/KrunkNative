/*
 * services/brain/oauth.c - "Login with Box Office": kn_brain as the OAuth 2.0 provider of Box Office accounts.
 *
 * Krunker v10.0.7 added "Login with Krunker" (third-party apps sign in with a Krunker account) and Settings >
 * Connected apps: the client's oauth API (deob-10.0.7.js:27656-27696) and the Connected apps component
 * (:97493-97673, mounted :219140-219142). This module answers those routes for Box Office accounts and is the
 * provider the apps talk to. The design (flows, scopes, tokens, limits) is doc/v10/oauth-provider.md.
 *
 * The client's routes (session bearer, the v10 envelope {data, error}):
 *   GET    /oauth/authorize/info?client_id&redirect_uri&scope&state&code_challenge&code_challenge_method
 *                                     the app, the requested scopes and the caller (authorizeInfo, :27658)
 *   POST   /oauth/authorize {client_id, redirect_uri, scope, state, code_challenge, code_challenge_method,
 *                            accept, twofa_code}  -> {redirect_url}  (authorize, :27661)
 *   GET    /account/apps                -> [{client_id, name, icon_url, homepage_url, developer, scopes, created_at,
 *                                           last_used_at}] (seconds; connectedApps, :27667; the list :97521-97526)
 *   DELETE /account/apps/:client_id     the app loses access at once (revokeApp, :27670; Disconnect :97528-97540)
 *   GET    /account/apps/:client_id/activity -> [{created_at, action, scope, result}] (appActivity, :27693)
 *   GET    /oauth/actions/:id           an app's pending action (actionInfo, :27675)
 *   POST   /oauth/actions/:id/approve {twofa_code} | /deny  (approveAction / denyAction, :27678-27691)
 * The apps' routes (RFC 6749 + 7636 + 7009; form or JSON bodies, {error, error_description} errors):
 *   GET    /oauth/authorize?...          the consent page (HTML, no script): sign in, review, authorize
 *   POST   /oauth/token                  authorization_code (PKCE S256, mandatory) | refresh_token (rotated)
 *   POST   /oauth/revoke                 a token (a refresh token takes its grant's access tokens with it)
 *   GET    /oauth/userinfo               profile
 *   GET    /oauth/api/stats              stats:read      GET /oauth/api/inventory   inventory:read
 *   GET    /oauth/api/friends            friends:read
 *   POST   /oauth/api/actions {kind, params}   a write the player approves with 2FA (gift.send, follow.add,
 *                                        follow.remove) -> {id, approve_url, expires_in}; GET /oauth/api/actions/:id
 * Staff (the brain.internal_secret bearer, mm_internal_authorized):
 *   POST   /mm/admin/oauth/clients {name, redirect_uris, scopes, developer?, icon_url?, homepage_url?, privacy_url?,
 *                                   tos_url?, min_level?, confidential?} -> {client_id, client_secret?}
 *   GET    /mm/admin/oauth/clients   POST /mm/admin/oauth/clients/:id/disable
 */
#include "oauth.h"
#include "brain.h" /* brain_utf16_prefix: the gift note is counted in characters */

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "accounts.h"
#include "acct_internal.h"
#include "auth_state.h"
#include "brain_internal.h"
#include "build_info.h"
#include "db_procs.h"
#include "hot.h"
#include "v10_api.h"
#include "knsvc/buf.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "oauth"
#define STR_(x) #x
#define STR(x) STR_(x)
#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))
#define COOKIE "kbo_oauth"
#define TOKEN_BYTES 32
#define MAX_SCOPES 24
#define ACTIVITY_ROWS 50

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

/* the account object has the score; the level is the game's (accounts.c) */
static int acc_level(const cJSON *acc) { return accounts_level(kn_json_int(acc, "score", 0), NULL); }

static struct {
    Brain *b;
    int enabled;
    int64_t access_ttl, refresh_ttl, code_ttl, action_ttl, cookie_ttl;
    char cookie_key[65]; /* the consent cookie's signing key (HMAC of brain.join_secret) */
    char public_url[512];
    int secure_cookie;
    AuthLimiter *limiter; /* per address: token, authorize, sign-in */
    int limit;
} O;

/* ================================================================== scopes (doc/v10/oauth-provider.md)
 * v10.0.7's scope names (the oauth.scope.* keys of deob-10.0.7.js:204284). `served`: a Box Office route reads or
 * acts with it; an app may only be registered and authorized with served scopes. A write scope needs the account's
 * 2FA at consent (oauth.twofa.desc) and its every write is an action the player approves with 2FA. */
typedef struct {
    const char *name;
    int write, served;
    const char *what; /* the consent page's line: oauth.scope.<name> (deob-10.0.7.js:204284), as Box Office serves it */
} Scope;

static const Scope k_scopes[] = {
    {"profile", 0, 1, "See your name, level, clan and badges"},
    {"stats:read", 0, 1, "See your stats, ranked rating and challenge progress"},
    {"notifications:read", 0, 0, "Read your mail and notifications"},
    {"inventory:read", 0, 1, "See your inventory and currencies"},
    {"inventory:write:skins", 1, 0, "Quick-sell, paint and lock your skins"},
    {"inventory:write:currency", 1, 1, "Send your KR to other players"},
    {"inventory:write", 1, 0, "Quick-sell, paint and lock your skins, and send your KR to other players"},
    {"trades:read", 0, 0, "See your trades"},
    {"trades:write", 1, 0, "Create, accept and cancel trades as you"},
    {"market:read", 0, 0, "See your market listings and sales"},
    {"market:write", 1, 0, "List, unlist and buy items on the market as you"},
    {"friends:read", 0, 1, "See who you follow and your followers"},
    {"friends:write", 1, 1, "Follow and unfollow players as you"},
    {"dms:read", 0, 0, "Read your direct messages"},
    {"dms:write", 1, 0, "Send direct messages to your friends as you"},
    {"clan:read", 0, 0, "See your clan and its members"},
    {"maps:read", 0, 0, "See your maps and their stats"},
    {"events:subscribe", 0, 0, "Get notified about activity on your account"},
};
#define SCOPE_COUNT (sizeof k_scopes / sizeof k_scopes[0])

static const Scope *scope_find(const char *name, size_t len) {
    for (size_t i = 0; i < SCOPE_COUNT; i++)
        if (strlen(k_scopes[i].name) == len && !strncmp(k_scopes[i].name, name, len)) return &k_scopes[i];
    return NULL;
}

/* "a b c" -> the scopes, each once, in catalogue order; -1 unknown / unserved / too many, else the count */
typedef struct {
    const Scope *s[MAX_SCOPES];
    int n;
    int write;
} ScopeSet;

static int scopes_parse(const char *text, ScopeSet *out) {
    memset(out, 0, sizeof *out);
    int seen[SCOPE_COUNT] = {0};
    for (const char *p = text ? text : ""; *p;) {
        while (*p == ' ' || *p == '+' || *p == ',') p++;
        const char *e = p;
        while (*e && *e != ' ' && *e != '+' && *e != ',') e++;
        if (e > p) {
            const Scope *s = scope_find(p, (size_t) (e - p));
            if (!s || !s->served) return -1;
            seen[s - k_scopes] = 1;
        }
        p = e;
    }
    for (size_t i = 0; i < SCOPE_COUNT; i++) {
        if (!seen[i]) continue;
        if (out->n >= MAX_SCOPES) return -1;
        out->s[out->n++] = &k_scopes[i];
        out->write |= k_scopes[i].write;
    }
    return out->n;
}

static void scopes_text(const ScopeSet *s, char *out, size_t cap) {
    out[0] = 0;
    for (int i = 0; i < s->n; i++) snprintf(out + strlen(out), cap - strlen(out), "%s%s", i ? " " : "", s->s[i]->name);
}

/* 1 when every scope of `want` is in the space-separated `have` */
static int scopes_within(const ScopeSet *want, const char *have) {
    ScopeSet h;
    if (scopes_parse(have, &h) < 0) return 0;
    for (int i = 0; i < want->n; i++) {
        int ok = 0;
        for (int j = 0; j < h.n && !ok; j++) ok = h.s[j] == want->s[i];
        if (!ok) return 0;
    }
    return 1;
}

static int scope_has(const char *have, const char *one) {
    const size_t n = strlen(one);
    for (const char *p = have ? have : ""; (p = strstr(p, one)); p += n)
        if ((p == have || p[-1] == ' ') && (p[n] == 0 || p[n] == ' ')) return 1;
    return 0;
}

static cJSON *scopes_array(const char *text) {
    cJSON *a = cJSON_CreateArray();
    ScopeSet s;
    if (scopes_parse(text, &s) >= 0)
        for (int i = 0; i < s.n; i++) cJSON_AddItemToArray(a, cJSON_CreateString(s.s[i]->name));
    return a;
}

/* ================================================================== small helpers */

static void copy_str(char *dst, const size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

static int new_token(char *out, size_t cap, const char *prefix) {
    const size_t pl = strlen(prefix);
    if (cap <= pl + 44) return -1;
    memcpy(out, prefix, pl);
    return kn_random_token(out + pl, cap - pl, TOKEN_BYTES);
}

/* base64url(sha256(verifier)) == challenge (RFC 7636 S256) */
static int pkce_ok(const char *verifier, const char *challenge) {
    const size_t n = verifier ? strlen(verifier) : 0;
    if (n < 43 || n > 128 || !challenge) return 0;
    uint8_t d[KN_SHA256_LEN];
    char enc[64];
    if (kn_sha256(verifier, n, d) != 0 || kn_base64_encode(d, sizeof d, enc, sizeof enc, 1, 0) < 0) return 0;
    return strlen(enc) == strlen(challenge) && kn_ct_streq(enc, challenge);
}

/* redirect_uri is one of the client's registered ones (a JSON array), compared exactly */
static int redirect_registered(const char *uris_json, const char *uri) {
    if (!uri || !*uri || strlen(uri) > 500) return 0;
    cJSON *a = cJSON_Parse(uris_json ? uris_json : "[]");
    int ok = 0;
    const cJSON *u;
    cJSON_ArrayForEach(u, a) ok |= cJSON_IsString(u) && !strcmp(u->valuestring, uri);
    cJSON_Delete(a);
    return ok;
}

static void url_append(kn_buf *b, const char *s) {
    static const char hex[] = "0123456789ABCDEF";
    for (; s && *s; s++) {
        const unsigned char ch = (unsigned char) *s;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || strchr("-_.~", ch)) {
            kn_buf_append_byte(b, ch);
        } else {
            kn_buf_append_byte(b, '%');
            kn_buf_append_byte(b, (uint8_t) hex[ch >> 4]);
            kn_buf_append_byte(b, (uint8_t) hex[ch & 15]);
        }
    }
}

/* redirect_uri + ?|& + code=..&state=.. (or error=..) */
static char *redirect_with(const char *uri, const char *k1, const char *v1, const char *state) {
    kn_buf b = {0};
    kn_buf_append_str(&b, uri);
    kn_buf_append_byte(&b, strchr(uri, '?') ? '&' : '?');
    kn_buf_append_str(&b, k1);
    kn_buf_append_byte(&b, '=');
    url_append(&b, v1);
    if (state && *state) {
        kn_buf_append_str(&b, "&state=");
        url_append(&b, state);
    }
    kn_buf_append_byte(&b, 0);
    return (char *) b.data;
}

/* a form-urlencoded or JSON body field (the token endpoint takes either) */
static int body_get(const kn_http_req *req, const cJSON *json, const char *key, char *out, size_t cap) {
    out[0] = 0;
    if (json) {
        const char *v = kn_json_str(json, key, NULL);
        if (v) copy_str(out, cap, v);
        return v ? (int) strlen(out) : -1;
    }
    const size_t kl = strlen(key);
    for (const char *p = req->body ? req->body : ""; *p;) {
        const char *e = strchr(p, '&');
        const size_t seg = e ? (size_t) (e - p) : strlen(p);
        const char *eq = memchr(p, '=', seg);
        if (eq && (size_t) (eq - p) == kl && !strncmp(p, key, kl)) {
            const int n = kn_url_decode(eq + 1, seg - kl - 1, out, cap, 1);
            if (n < 0) out[0] = 0;
            return n;
        }
        p = e ? e + 1 : p + seg;
    }
    return -1;
}

static cJSON *body_json(const kn_http_req *req) {
    const char *ct = kn_http_header_get(req, "Content-Type");
    return ct && strstr(ct, "json") ? kn_http_body_json(req) : NULL;
}

static int limited(kn_http_req *req, const char *what) {
    char key[128];
    snprintf(key, sizeof key, "%s:%s", what, req->remote_addr ? req->remote_addr : "");
    const int local = O.limiter ? auth_limiter_hit(O.limiter, key, (uint64_t) time(NULL) * 1000) : 1;
    if (hot_limit("oauth", key, O.limit, 60, local)) return 0;
    kn_http_set_header(req, "Retry-After", "60");
    return 1;
}

/* RFC 6749 5.2 error */
static void oauth_error(kn_http_req *req, int status, const char *code, const char *desc) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", code);
    if (desc) cJSON_AddStringToObject(o, "error_description", desc);
    kn_http_set_header(req, "Cache-Control", "no-store");
    kn_http_respond_json(req, status, o);
    cJSON_Delete(o);
}

/* ================================================================== procs (database worker threads) */

/* (client_id) -> the client (no secret) | null */
static kn_db_result *run_client(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT client_id, name, developer, icon_url, homepage_url, privacy_url, tos_url,"
                                        " redirect_uris AS redirect_uris__t, scopes AS scopes__t, min_level AS min_level__i,"
                                        " disabled AS disabled__b, (secret_hash IS NOT NULL) AS confidential__b"
                                        " FROM oauth_clients WHERE client_id = $1"),
                         KN_DB_ARGS(P(0)));
    return dbx_json(&x, "oauth_client", row);
}
static const kn_proc P_OA_CLIENT = {"oauth_client", NULL, run_client};

/* (account, client_id) -> {twofa, granted}: the consent's account side */
static kn_db_result *run_consent_state(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *o = dbx_row(&x, SQL(.any = "SELECT (a.twofa_secret IS NOT NULL) AS twofa__b, g.scopes AS granted__t FROM accounts a"
                                      " LEFT JOIN oauth_grants g ON g.account_id = a.id AND g.client_id = $2 WHERE a.id = {int:$1}"),
                       KN_DB_ARGS(P(0), P(1)));
    return dbx_json(&x, "oauth_consent", o ? o : cJSON_CreateObject());
}
static const kn_proc P_OA_CONSENT_STATE = {"oauth_consent_state", NULL, run_consent_state};

static void activity(Dbx *x, const char *account, const char *client, const char *action, const char *scope, const char *result) {
    dbx_exec(x, SQL(.any = "INSERT INTO oauth_activity (account_id, client_id, action, scope, result) VALUES ({int:$1}, $2, $3, nullif($4, ''), $5)"),
             KN_DB_ARGS(account, client, action, scope ? scope : "", result));
    dbx_exec(x, SQL(.any = "DELETE FROM oauth_activity WHERE account_id = {int:$1} AND client_id = $2 AND id NOT IN"
                           " (SELECT id FROM (SELECT id FROM oauth_activity WHERE account_id = {int:$1} AND client_id = $2"
                           " ORDER BY id DESC LIMIT " STR(ACTIVITY_ROWS) ") keep)"),
             KN_DB_ARGS(account, client));
}

/* (code, client_id, account, redirect_uri, scopes, challenge) -> {}: the grant (its scopes widened to the new ones),
 * the single-use code, the activity row */
static kn_db_result *run_grant(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    char *had = dbx_text(&x, SQL(.any = "SELECT scopes FROM oauth_grants WHERE account_id = {int:$1} AND client_id = $2"),
                         KN_DB_ARGS(P(2), P(1)));
    char merged[1024];
    copy_str(merged, sizeof merged, P(4));
    if (had) {
        char both[2048];
        snprintf(both, sizeof both, "%s %s", had, P(4) ? P(4) : "");
        ScopeSet m;
        if (scopes_parse(both, &m) >= 0) scopes_text(&m, merged, sizeof merged);
        dbx_exec(&x, SQL(.any = "UPDATE oauth_grants SET scopes = $3 WHERE account_id = {int:$1} AND client_id = $2"),
                 KN_DB_ARGS(P(2), P(1), merged));
        free(had);
    } else {
        dbx_exec(&x, SQL(.any = "INSERT INTO oauth_grants (account_id, client_id, scopes) VALUES ({int:$1}, $2, $3)"),
                 KN_DB_ARGS(P(2), P(1), merged));
    }
    dbx_exec(&x, SQL(.any = "INSERT INTO oauth_codes (code_hash, client_id, account_id, redirect_uri, scopes, code_challenge)"
                            " VALUES ({hash:$1}, $2, {int:$3}, $4, $5, $6)"),
             KN_DB_ARGS(P(0), P(1), P(2), P(3), P(4), P(5)));
    activity(&x, P(2), P(1), "authorize", P(4), "ok");
    return dbx_json(&x, "oauth_grant", cJSON_CreateObject());
}
static const kn_proc P_OA_GRANT = {"oauth_grant", NULL, run_grant};

static int client_secret_ok(Dbx *x, const char *client, const char *secret) {
    return dbx_exists(x, SQL(.any = "SELECT 1 FROM oauth_clients WHERE client_id = $1 AND NOT disabled"
                                    " AND (secret_hash IS NULL OR secret_hash = {hash:$2})"),
                      KN_DB_ARGS(client, secret ? secret : ""));
}

static void issue(Dbx *x, const char *access, const char *refresh, const char *client, const char *account, const char *scopes,
                  const char *access_ttl, const char *refresh_ttl) {
    dbx_exec(x, SQL(.any = "INSERT INTO oauth_tokens (token_hash, kind, client_id, account_id, scopes, expires_at)"
                           " VALUES ({hash:$1}, 'access', $2, {int:$3}, $4, {later:$5})"),
             KN_DB_ARGS(access, client, account, scopes, access_ttl));
    dbx_exec(x, SQL(.any = "INSERT INTO oauth_tokens (token_hash, kind, client_id, account_id, scopes, expires_at)"
                           " VALUES ({hash:$1}, 'refresh', $2, {int:$3}, $4, {later:$5})"),
             KN_DB_ARGS(refresh, client, account, scopes, refresh_ttl));
    dbx_exec(x, SQL(.any = "DELETE FROM oauth_tokens WHERE account_id = {int:$1} AND client_id = $2 AND expires_at <= {now}"),
             KN_DB_ARGS(account, client));
}

/* (code, client_id, client_secret, redirect_uri, verifier, access, refresh, access_ttl, refresh_ttl, code_ttl)
 *   -> {scope}: the code used up, then the tokens. Every failure is RFC 6749's invalid_grant / invalid_client. */
static kn_db_result *exchange_error(Dbx *x, const char *code) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "error", code);
    return dbx_json(x, "oauth_token", o);
}

static kn_db_result *run_code_exchange(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT client_id, account_id AS account__i, redirect_uri, scopes, code_challenge,"
                                        " (created_at > {ago:$2}) AS fresh__b FROM oauth_codes WHERE code_hash = {hash:$1}"),
                         KN_DB_ARGS(P(0), P(9)));
    dbx_exec(&x, SQL(.any = "DELETE FROM oauth_codes WHERE code_hash = {hash:$1} OR created_at <= {ago:$2}"), KN_DB_ARGS(P(0), P(9)));
    if (dbx_failed(&x)) {
        cJSON_Delete(row);
        return dbx_finish(&x, NULL);
    }
    if (!client_secret_ok(&x, P(1), P(2))) {
        cJSON_Delete(row);
        return exchange_error(&x, "invalid_client");
    }
    const char *why = !row ? "unknown code" : !cJSON_IsTrue(cJSON_GetObjectItem(row, "fresh")) ? "expired code"
                    : strcmp(kn_json_str(row, "client_id", ""), P(1) ? P(1) : "") ? "client mismatch"
                    : strcmp(kn_json_str(row, "redirect_uri", ""), P(3) ? P(3) : "") ? "redirect_uri mismatch"
                    : !pkce_ok(P(4), kn_json_str(row, "code_challenge", NULL)) ? "code_verifier mismatch" : NULL;
    if (why) {
        cJSON_Delete(row);
        return exchange_error(&x, "invalid_grant");
    }
    char account[24], scopes[1024];
    snprintf(account, sizeof account, "%lld", (long long) kn_json_int(row, "account", 0));
    copy_str(scopes, sizeof scopes, kn_json_str(row, "scopes", ""));
    cJSON_Delete(row);
    if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM oauth_grants WHERE account_id = {int:$1} AND client_id = $2"), KN_DB_ARGS(account, P(1))))
        return exchange_error(&x, "invalid_grant");
    issue(&x, P(5), P(6), P(1), account, scopes, P(7), P(8));
    activity(&x, account, P(1), "token", NULL, "ok");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "scope", scopes);
    return dbx_json(&x, "oauth_token", o);
}
static const kn_proc P_OA_CODE_EXCHANGE = {"oauth_code_exchange", NULL, run_code_exchange};

/* (refresh, client_id, client_secret, access, new refresh, access_ttl, refresh_ttl) -> {scope}: rotated (the old
 * refresh token is gone); its scopes narrowed to what the grant still holds */
static kn_db_result *run_refresh(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (!client_secret_ok(&x, P(1), P(2))) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "invalid_client");
        return dbx_finish(&x, NULL);
    }
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT t.account_id AS account__i, t.scopes, g.scopes AS granted FROM oauth_tokens t"
                                        " JOIN oauth_grants g ON g.account_id = t.account_id AND g.client_id = t.client_id"
                                        " WHERE t.token_hash = {hash:$1} AND t.kind = 'refresh' AND t.client_id = $2 AND t.expires_at > {now}"),
                         KN_DB_ARGS(P(0), P(1)));
    if (!row) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "invalid_grant");
        return dbx_finish(&x, NULL);
    }
    char account[24], scopes[1024] = "";
    snprintf(account, sizeof account, "%lld", (long long) kn_json_int(row, "account", 0));
    ScopeSet t, keep = {0};
    if (scopes_parse(kn_json_str(row, "scopes", ""), &t) >= 0)
        for (int i = 0; i < t.n; i++)
            if (scope_has(kn_json_str(row, "granted", ""), t.s[i]->name)) keep.s[keep.n++] = t.s[i];
    scopes_text(&keep, scopes, sizeof scopes);
    cJSON_Delete(row);
    dbx_exec(&x, SQL(.any = "DELETE FROM oauth_tokens WHERE token_hash = {hash:$1}"), KN_DB_ARGS(P(0)));
    issue(&x, P(3), P(4), P(1), account, scopes, P(5), P(6));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "scope", scopes);
    return dbx_json(&x, "oauth_token", o);
}
static const kn_proc P_OA_REFRESH = {"oauth_refresh", NULL, run_refresh};

/* (token, client_id) -> {}: RFC 7009 (an unknown token is no error); a refresh token takes its grant's tokens */
static kn_db_result *run_revoke(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT kind, account_id AS account__i FROM oauth_tokens WHERE token_hash = {hash:$1} AND client_id = $2"),
                         KN_DB_ARGS(P(0), P(1)));
    if (row && !strcmp(kn_json_str(row, "kind", ""), "refresh"))
        dbx_exec(&x, SQL(.any = "DELETE FROM oauth_tokens WHERE account_id = {int:$1} AND client_id = $2"),
                 KN_DB_ARGS(I64S(kn_json_int(row, "account", 0)), P(1)));
    else
        dbx_exec(&x, SQL(.any = "DELETE FROM oauth_tokens WHERE token_hash = {hash:$1} AND client_id = $2"), KN_DB_ARGS(P(0), P(1)));
    cJSON_Delete(row);
    return dbx_json(&x, "oauth_revoke", cJSON_CreateObject());
}
static const kn_proc P_OA_REVOKE = {"oauth_revoke", NULL, run_revoke};

/* (access token) -> {account, client_id, scopes} | null: a live access token of a grant that still exists */
static kn_db_result *run_bearer(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT t.account_id AS account__i, t.client_id, t.scopes FROM oauth_tokens t"
                                        " JOIN oauth_grants g ON g.account_id = t.account_id AND g.client_id = t.client_id"
                                        " JOIN oauth_clients k ON k.client_id = t.client_id"
                                        " WHERE t.token_hash = {hash:$1} AND t.kind = 'access' AND t.expires_at > {now} AND NOT k.disabled"),
                         KN_DB_ARGS(P(0)));
    if (row)
        dbx_exec(&x, SQL(.any = "UPDATE oauth_grants SET last_used_at = {now} WHERE account_id = {int:$1} AND client_id = $2"
                                " AND (last_used_at IS NULL OR last_used_at <= {ago:60})"),
                 KN_DB_ARGS(I64S(kn_json_int(row, "account", 0)), kn_json_str(row, "client_id", "")));
    return dbx_json(&x, "oauth_bearer", row);
}
static const kn_proc P_OA_BEARER = {"oauth_bearer", NULL, run_bearer};

/* (account, client, action, scope, result) -> {} */
static kn_db_result *run_activity_add(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    activity(&x, P(0), P(1), P(2), P(3), P(4));
    return dbx_json(&x, "oauth_activity", cJSON_CreateObject());
}
static const kn_proc P_OA_ACTIVITY_ADD = {"oauth_activity_add", NULL, run_activity_add};

/* (account) -> the connected apps, newest first */
static kn_db_result *run_apps(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT k.client_id, k.name, k.icon_url, k.homepage_url, k.developer, g.scopes,"
                                          " {epoch_ms:g.created_at} AS created__i, {epoch_ms:g.last_used_at} AS used__i"
                                          " FROM oauth_grants g JOIN oauth_clients k ON k.client_id = g.client_id"
                                          " WHERE g.account_id = {int:$1} ORDER BY g.created_at DESC"),
                           KN_DB_ARGS(P(0)));
    return dbx_json(&x, "oauth_apps", rows ? rows : cJSON_CreateArray());
}
static const kn_proc P_OA_APPS = {"oauth_apps", NULL, run_apps};

/* (account, client) -> {revoked}: the grant, its tokens and codes; its pending actions expire */
static kn_db_result *run_app_revoke(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t hit = dbx_exec(&x, SQL(.any = "DELETE FROM oauth_grants WHERE account_id = {int:$1} AND client_id = $2"), KN_DB_ARGS(P(0), P(1)));
    dbx_exec(&x, SQL(.any = "DELETE FROM oauth_tokens WHERE account_id = {int:$1} AND client_id = $2"), KN_DB_ARGS(P(0), P(1)));
    dbx_exec(&x, SQL(.any = "DELETE FROM oauth_codes WHERE account_id = {int:$1} AND client_id = $2"), KN_DB_ARGS(P(0), P(1)));
    dbx_exec(&x, SQL(.any = "UPDATE oauth_actions SET status = 'expired', decided_at = {now} WHERE account_id = {int:$1}"
                            " AND client_id = $2 AND status = 'pending'"),
             KN_DB_ARGS(P(0), P(1)));
    if (hit > 0) activity(&x, P(0), P(1), "disconnect", NULL, "ok");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "revoked", hit > 0);
    return dbx_json(&x, "oauth_app_revoke", o);
}
static const kn_proc P_OA_APP_REVOKE = {"oauth_app_revoke", NULL, run_app_revoke};

/* (account, client) -> the app's activity, newest first */
static kn_db_result *run_app_activity(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT {epoch_ms:created_at} AS created__i, action, scope, result FROM oauth_activity"
                                          " WHERE account_id = {int:$1} AND client_id = $2 ORDER BY id DESC LIMIT " STR(ACTIVITY_ROWS)),
                           KN_DB_ARGS(P(0), P(1)));
    return dbx_json(&x, "oauth_app_activity", rows ? rows : cJSON_CreateArray());
}
static const kn_proc P_OA_APP_ACTIVITY = {"oauth_app_activity", NULL, run_app_activity};

/* (id, client, account, kind, scope, params) -> {}: the grant must hold the action's scope */
static kn_db_result *run_action_new(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    char *granted = dbx_text(&x, SQL(.any = "SELECT scopes FROM oauth_grants WHERE account_id = {int:$1} AND client_id = $2"),
                             KN_DB_ARGS(P(2), P(1)));
    const int ok = granted && scope_has(granted, P(4) ? P(4) : "");
    free(granted);
    if (!dbx_failed(&x) && !ok) dbx_refuse(&x, "insufficient_scope");
    dbx_exec(&x, SQL(.any = "INSERT INTO oauth_actions (action_id, client_id, account_id, kind, scope, params) VALUES ($1, $2, {int:$3}, $4, $5, $6)"),
             KN_DB_ARGS(P(0), P(1), P(2), P(3), P(4), P(5)));
    activity(&x, P(2), P(1), P(3), P(4), "pending");
    return dbx_json(&x, "oauth_action_new", cJSON_CreateObject());
}
static const kn_proc P_OA_ACTION_NEW = {"oauth_action_new", NULL, run_action_new};

/* (id, ttl) -> the action, its app's name, its status ('expired' once a pending one is past the ttl) | null */
static kn_db_result *run_action_get(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT a.action_id, a.client_id, a.account_id AS account__i, a.kind, a.scope, a.params AS params__t,"
                                        " a.status, a.result, {epoch_ms:a.created_at} AS created__i, (a.created_at > {ago:$2}) AS fresh__b,"
                                        " k.name AS app_name FROM oauth_actions a JOIN oauth_clients k ON k.client_id = a.client_id"
                                        " WHERE a.action_id = $1"),
                         KN_DB_ARGS(P(0), P(1)));
    if (row && !strcmp(kn_json_str(row, "status", ""), "pending") && !cJSON_IsTrue(cJSON_GetObjectItem(row, "fresh"))) {
        dbx_exec(&x, SQL(.any = "UPDATE oauth_actions SET status = 'expired', decided_at = {now} WHERE action_id = $1 AND status = 'pending'"),
                 KN_DB_ARGS(P(0)));
        cJSON_ReplaceItemInObject(row, "status", cJSON_CreateString("expired"));
    }
    return dbx_json(&x, "oauth_action", row);
}
static const kn_proc P_OA_ACTION_GET = {"oauth_action_get", NULL, run_action_get};

/* (id, account, ttl, decision 'running' | 'denied') -> {claimed}: a pending, fresh action of the account whose
 * grant still holds its scope moves on, once (a second approve finds it no longer pending) */
static kn_db_result *run_action_claim(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t hit = dbx_exec(&x, SQL(.any = "UPDATE oauth_actions SET status = $4, decided_at = {now} WHERE action_id = $1"
                                                " AND account_id = {int:$2} AND status = 'pending' AND created_at > {ago:$3}"
                                                " AND EXISTS (SELECT 1 FROM oauth_grants g WHERE g.account_id = oauth_actions.account_id"
                                                " AND g.client_id = oauth_actions.client_id)"),
                                 KN_DB_ARGS(P(0), P(1), P(2), P(3)));
    if (hit > 0 && P(3) && !strcmp(P(3), "denied")) {
        cJSON *row = dbx_row(&x, SQL(.any = "SELECT client_id, kind, scope FROM oauth_actions WHERE action_id = $1"), KN_DB_ARGS(P(0)));
        if (row) activity(&x, P(1), kn_json_str(row, "client_id", ""), kn_json_str(row, "kind", ""), kn_json_str(row, "scope", ""), "denied");
        cJSON_Delete(row);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "claimed", hit > 0);
    return dbx_json(&x, "oauth_action_claim", o);
}
static const kn_proc P_OA_ACTION_CLAIM = {"oauth_action_claim", NULL, run_action_claim};

/* (id, status 'approved' | 'failed', result) -> {}: the outcome and the activity row */
static kn_db_result *run_action_finish(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    dbx_exec(&x, SQL(.any = "UPDATE oauth_actions SET status = $2, result = $3 WHERE action_id = $1 AND status = 'running'"),
             KN_DB_ARGS(P(0), P(1), P(2) ? P(2) : ""));
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT client_id, account_id AS account__i, kind, scope FROM oauth_actions WHERE action_id = $1"),
                         KN_DB_ARGS(P(0)));
    if (row)
        activity(&x, I64S(kn_json_int(row, "account", 0)), kn_json_str(row, "client_id", ""), kn_json_str(row, "kind", ""),
                 kn_json_str(row, "scope", ""), !strcmp(P(1) ? P(1) : "", "approved") ? "ok" : "failed");
    cJSON_Delete(row);
    return dbx_json(&x, "oauth_action_finish", cJSON_CreateObject());
}
static const kn_proc P_OA_ACTION_FINISH = {"oauth_action_finish", NULL, run_action_finish};

/* (account) -> {following, followers}: friends:read */
static kn_db_result *run_friends(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *ing = dbx_rows(&x, SQL(.any = "SELECT a.username FROM follows f JOIN accounts a ON a.id = f.followee_id"
                                         " WHERE f.follower_id = {int:$1} ORDER BY a.username LIMIT 1000"),
                          KN_DB_ARGS(P(0)));
    cJSON *ers = dbx_rows(&x, SQL(.any = "SELECT a.username FROM follows f JOIN accounts a ON a.id = f.follower_id"
                                         " WHERE f.followee_id = {int:$1} ORDER BY a.username LIMIT 1000"),
                          KN_DB_ARGS(P(0)));
    cJSON *o = cJSON_CreateObject();
    cJSON *a = cJSON_AddArrayToObject(o, "following"), *b = cJSON_AddArrayToObject(o, "followers");
    const cJSON *r;
    cJSON_ArrayForEach(r, ing) cJSON_AddItemToArray(a, cJSON_CreateString(kn_json_str(r, "username", "")));
    cJSON_ArrayForEach(r, ers) cJSON_AddItemToArray(b, cJSON_CreateString(kn_json_str(r, "username", "")));
    cJSON_Delete(ing);
    cJSON_Delete(ers);
    return dbx_json(&x, "oauth_friends", o);
}
static const kn_proc P_OA_FRIENDS = {"oauth_friends", NULL, run_friends};

/* (account, followee name, 1 follow | 0 unfollow) -> {changed} */
static kn_db_result *run_follow(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t to = dbx_int(&x, SQL(.any = "SELECT id FROM accounts WHERE lower(username) = lower($1)"), KN_DB_ARGS(P(1)), 0);
    if (!dbx_failed(&x) && to <= 0) dbx_refuse(&x, "Player not found");
    else if (!dbx_failed(&x) && to == PI(0)) dbx_refuse(&x, "You can't follow yourself");
    int64_t hit = 0;
    if (PI(2))
        hit = dbx_exec(&x, SQL(.any = "{insert_ignore} INTO follows (follower_id, followee_id) VALUES ({int:$1}, {int:$2}) {ignore_conflicts}"),
                       KN_DB_ARGS(P(0), I64S(to)));
    else
        hit = dbx_exec(&x, SQL(.any = "DELETE FROM follows WHERE follower_id = {int:$1} AND followee_id = {int:$2}"), KN_DB_ARGS(P(0), I64S(to)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "changed", hit > 0);
    return dbx_json(&x, "oauth_follow", o);
}
static const kn_proc P_OA_FOLLOW = {"oauth_follow", NULL, run_follow};

/* (fields...) -> {client_id}: staff registration */
static kn_db_result *run_client_new(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    dbx_exec(&x, SQL(.any = "INSERT INTO oauth_clients (client_id, secret_hash, name, developer, icon_url, homepage_url, privacy_url,"
                            " tos_url, redirect_uris, scopes, min_level) VALUES ($1, CASE WHEN $2 = '' THEN NULL ELSE {hash:$2} END,"
                            " $3, nullif($4, ''), nullif($5, ''), nullif($6, ''), nullif($7, ''), nullif($8, ''), $9, $10, {int:$11})"),
             KN_DB_ARGS(P(0), P(1) ? P(1) : "", P(2), P(3), P(4), P(5), P(6), P(7), P(8), P(9), P(10) ? P(10) : "0"));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "client_id", P(0) ? P(0) : "");
    return dbx_json(&x, "oauth_client_new", o);
}
static const kn_proc P_OA_CLIENT_NEW = {"oauth_client_new", NULL, run_client_new};

/* ================================================================== the callers */

/* the signed-in Box Office account of a session bearer (the client's routes) */
typedef void (*SessionFn)(kn_http_req *req, const cJSON *acc, void *ud);
typedef struct {
    kn_http_req *req;
    SessionFn fn;
    void *ud;
} SessionCtx;

static void on_session(void *ud, const char *err, const cJSON *acc) {
    SessionCtx *c = ud;
    if (err || kn_json_int(acc, "id", 0) <= 0) {
        v10_reply_error(c->req, 401, "LOGIN_REQUIRED", "You must be logged in.");
        free(c->ud);
    } else {
        c->fn(c->req, acc, c->ud);
    }
    free(c);
}

/* fn owns ud after the call (freed with free() when the caller is not signed in) */
static void with_session(kn_http_req *req, SessionFn fn, void *ud) {
    char bearer[256]; /* the session: ?token= (the client's convention, v10_account.c caller_token) or a Bearer header */
    if (kn_http_query_get(req, "token", bearer, sizeof bearer) <= 0) snprintf(bearer, sizeof bearer, "%s", kn_http_bearer(req) ? kn_http_bearer(req) : "");
    SessionCtx *c = bearer[0] ? calloc(1, sizeof *c) : NULL;
    if (!c) {
        v10_reply_error(req, 401, "LOGIN_REQUIRED", "You must be logged in.");
        free(ud);
        return;
    }
    *c = (SessionCtx){req, fn, ud};
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", bearer);
    accounts_internal_call(O.b, "token", args, on_session, c);
    cJSON_Delete(args);
}

/* an app's access token with the scope it needs (the apps' routes) */
typedef void (*AppFn)(kn_http_req *req, int64_t account, const char *client, const char *scopes, void *ud);
typedef struct {
    kn_http_req *req;
    char scope[48];
    AppFn fn;
    void *ud;
} AppCtx;

static void on_bearer(kn_db_result *res, void *ud) {
    AppCtx *c = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const int64_t account = kn_json_int(o, "account", 0);
    if (account <= 0) {
        kn_http_set_header(c->req, "WWW-Authenticate", "Bearer error=\"invalid_token\"");
        oauth_error(c->req, 401, "invalid_token", "The access token is invalid or expired.");
        free(c->ud);
    } else if (!scope_has(kn_json_str(o, "scopes", ""), c->scope)) {
        char h[128];
        snprintf(h, sizeof h, "Bearer error=\"insufficient_scope\", scope=\"%s\"", c->scope);
        kn_http_set_header(c->req, "WWW-Authenticate", h);
        oauth_error(c->req, 403, "insufficient_scope", c->scope);
        free(c->ud);
    } else {
        c->fn(c->req, account, kn_json_str(o, "client_id", ""), kn_json_str(o, "scopes", ""), c->ud);
    }
    cJSON_Delete(o);
    free(c);
}

static void with_app(kn_http_req *req, const char *scope, AppFn fn, void *ud) {
    const char *bearer = kn_http_bearer(req);
    AppCtx *c = bearer && !strncmp(bearer, "kbo_at_", 7) && strlen(bearer) < 128 ? calloc(1, sizeof *c) : NULL;
    if (!c) {
        kn_http_set_header(req, "WWW-Authenticate", "Bearer");
        oauth_error(req, 401, "invalid_token", "An access token is required.");
        free(ud);
        return;
    }
    c->req = req;
    copy_str(c->scope, sizeof c->scope, scope);
    c->fn = fn;
    c->ud = ud;
    kn_db_call(O.b->db, &P_OA_BEARER, KN_DB_ARGS(bearer), on_bearer, c);
}

static void log_activity(int64_t account, const char *client, const char *action, const char *scope, const char *result) {
    char id[24];
    kn_db_call(O.b->db, &P_OA_ACTIVITY_ADD, KN_DB_ARGS(acct_itoa(id, account), client, action, scope ? scope : "", result), NULL, NULL);
}

/* ================================================================== the authorization request */

/* the request's fields, from a query string, a JSON body or a form */
typedef struct {
    char client_id[80], redirect_uri[512], scope[512], state[256], challenge[160], method[16], response_type[16];
    char twofa[16];
    int accept;
} AuthzReq;

static void authz_from_query(kn_http_req *req, AuthzReq *a) {
    memset(a, 0, sizeof *a);
    kn_http_query_get(req, "client_id", a->client_id, sizeof a->client_id);
    kn_http_query_get(req, "redirect_uri", a->redirect_uri, sizeof a->redirect_uri);
    kn_http_query_get(req, "scope", a->scope, sizeof a->scope);
    kn_http_query_get(req, "state", a->state, sizeof a->state);
    kn_http_query_get(req, "code_challenge", a->challenge, sizeof a->challenge);
    kn_http_query_get(req, "code_challenge_method", a->method, sizeof a->method);
    kn_http_query_get(req, "response_type", a->response_type, sizeof a->response_type);
}

static void authz_from_body(kn_http_req *req, const cJSON *json, AuthzReq *a) {
    memset(a, 0, sizeof *a);
    body_get(req, json, "client_id", a->client_id, sizeof a->client_id);
    body_get(req, json, "redirect_uri", a->redirect_uri, sizeof a->redirect_uri);
    body_get(req, json, "scope", a->scope, sizeof a->scope);
    body_get(req, json, "state", a->state, sizeof a->state);
    body_get(req, json, "code_challenge", a->challenge, sizeof a->challenge);
    body_get(req, json, "code_challenge_method", a->method, sizeof a->method);
    body_get(req, json, "response_type", a->response_type, sizeof a->response_type);
    body_get(req, json, "twofa_code", a->twofa, sizeof a->twofa);
    char acc[16];
    a->accept = json ? cJSON_IsTrue(cJSON_GetObjectItem(json, "accept")) : body_get(req, NULL, "accept", acc, sizeof acc) > 0;
}

/* everything about the request that does not need the database; NULL when it is well formed */
static const char *authz_shape_error(const AuthzReq *a, ScopeSet *scopes) {
    if (!a->client_id[0]) return "client_id is missing.";
    if (a->response_type[0] && strcmp(a->response_type, "code")) return "Only response_type=code is supported.";
    if (strcmp(a->method, "S256") || strlen(a->challenge) != 43) return "PKCE with code_challenge_method=S256 is required.";
    if (scopes_parse(a->scope[0] ? a->scope : "profile", scopes) <= 0) return "The app asked for a scope Box Office does not offer.";
    return NULL;
}

/* the client checks: registered, enabled, the redirect and every scope allowed for it; NULL when all pass */
static const char *authz_client_error(const cJSON *client, const AuthzReq *a, const ScopeSet *scopes) {
    if (!client || cJSON_IsTrue(cJSON_GetObjectItem(client, "disabled"))) return "This app is not registered with Box Office.";
    if (!redirect_registered(kn_json_str(client, "redirect_uris", "[]"), a->redirect_uri)) return "The app's redirect address is not registered.";
    if (!scopes_within(scopes, kn_json_str(client, "scopes", ""))) return "The app asked for more than it is registered for.";
    return NULL;
}

static cJSON *client_public(const cJSON *client) {
    cJSON *o = cJSON_CreateObject();
    static const char *const keys[] = {"client_id", "name", "developer", "icon_url", "homepage_url", "privacy_url", "tos_url"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const char *v = kn_json_str(client, keys[i], NULL);
        if (v) cJSON_AddStringToObject(o, keys[i], v);
        else cJSON_AddNullToObject(o, keys[i]);
    }
    cJSON_AddNumberToObject(o, "min_level", (double) kn_json_int(client, "min_level", 0));
    return o;
}

static cJSON *scopes_detail(const ScopeSet *s) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < s->n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "scope", s->s[i]->name);
        cJSON_AddBoolToObject(o, "write", s->s[i]->write);
        cJSON_AddItemToArray(a, o);
    }
    return a;
}

/* The consent decision, shared by the JSON route and the page. Steps: the client, the account (level, 2FA), the
 * 2FA code of a write grant, then the code. done(ud, redirect_url | NULL, error) */
typedef void (*DecideFn)(void *ud, const char *redirect, const char *error);
typedef struct {
    AuthzReq a;
    ScopeSet scopes;
    int64_t account;
    int level;
    char code[80];
    DecideFn done;
    void *ud;
} Decide;

static void decide_fail(Decide *d, const char *err) {
    d->done(d->ud, NULL, err);
    free(d);
}

static void decide_granted(kn_db_result *res, void *ud) {
    Decide *d = ud;
    if (!kn_db_ok(res)) return decide_fail(d, acct_db_error(res, "oauth_grant"));
    char *url = redirect_with(d->a.redirect_uri, "code", d->code, d->a.state);
    KN_INFO(MOD, "account %lld authorized %s", (long long) d->account, d->a.client_id);
    d->done(d->ud, url, NULL);
    free(url);
    free(d);
}

static void decide_issue(Decide *d) {
    char scopes[512], acc[24];
    scopes_text(&d->scopes, scopes, sizeof scopes);
    if (new_token(d->code, sizeof d->code, "kbo_ac_") != 0) return decide_fail(d, "Server error.");
    kn_db_call(O.b->db, &P_OA_GRANT, KN_DB_ARGS(d->code, d->a.client_id, acct_itoa(acc, d->account), d->a.redirect_uri, scopes, d->a.challenge),
               decide_granted, d);
}

static void decide_twofa(kn_db_result *res, void *ud) {
    Decide *d = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const int ok = kn_json_int(o, "ok", 0) == 1;
    cJSON_Delete(o);
    if (!kn_db_ok(res)) return decide_fail(d, acct_db_error(res, "twofa"));
    if (!ok) return decide_fail(d, "Invalid Code");
    decide_issue(d);
}

static void decide_state(kn_db_result *res, void *ud) {
    Decide *d = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const int twofa = cJSON_IsTrue(cJSON_GetObjectItem(o, "twofa"));
    cJSON_Delete(o);
    if (!kn_db_ok(res)) return decide_fail(d, acct_db_error(res, "oauth_consent"));
    if (!d->scopes.write) return decide_issue(d);
    if (!d->a.accept) return decide_fail(d, "Accept the risks of write access to continue.");
    if (!twofa) return decide_fail(d, "Write access needs 2FA. Turn on 2FA in Box Office's Account Security first.");
    if (!d->a.twofa[0]) return decide_fail(d, "Enter the code from your authenticator app.");
    char acc[24];
    char now[24];
    kn_db_call(O.b->db, &P_TWOFA_LOGIN, KN_DB_ARGS(acct_itoa(acc, d->account), d->a.twofa, acct_itoa(now, (int64_t) time(NULL))), decide_twofa, d);
}

static void decide_client(kn_db_result *res, void *ud) {
    Decide *d = ud;
    cJSON *client = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const char *err = !kn_db_ok(res) ? acct_db_error(res, "oauth_client") : authz_client_error(client, &d->a, &d->scopes);
    const int64_t min_level = kn_json_int(client, "min_level", 0);
    cJSON_Delete(client);
    if (err) return decide_fail(d, err);
    if (min_level > 0 && d->level < min_level) {
        char msg[64];
        snprintf(msg, sizeof msg, "Requires level %lld", (long long) min_level);
        return decide_fail(d, msg);
    }
    char acc[24];
    kn_db_call(O.b->db, &P_OA_CONSENT_STATE, KN_DB_ARGS(acct_itoa(acc, d->account), d->a.client_id), decide_state, d);
}

static void decide_start(const AuthzReq *a, int64_t account, int level, DecideFn done, void *ud) {
    Decide *d = calloc(1, sizeof *d);
    if (!d) {
        done(ud, NULL, "Server error.");
        return;
    }
    d->a = *a;
    d->account = account;
    d->level = level;
    d->done = done;
    d->ud = ud;
    const char *err = authz_shape_error(&d->a, &d->scopes);
    if (err) return decide_fail(d, err);
    kn_db_call(O.b->db, &P_OA_CLIENT, KN_DB_ARGS(d->a.client_id), decide_client, d);
}

/* ---- GET /oauth/authorize/info (authorizeInfo, deob-10.0.7.js:27658) */

typedef struct {
    kn_http_req *req;
    AuthzReq a;
    ScopeSet scopes;
    int64_t account;
    char name[64];
    int level;
    cJSON *client;
} InfoCtx;

static void info_reply(InfoCtx *c, cJSON *account_state) {
    cJSON *d = cJSON_CreateObject();
    cJSON_AddItemToObject(d, "client", client_public(c->client));
    cJSON_AddItemToObject(d, "scopes", scopes_detail(&c->scopes));
    cJSON_AddBoolToObject(d, "write", c->scopes.write);
    if (c->account > 0) {
        cJSON *acc = cJSON_CreateObject();
        cJSON_AddStringToObject(acc, "name", c->name);
        cJSON_AddNumberToObject(acc, "level", c->level);
        cJSON_AddBoolToObject(acc, "twofa", cJSON_IsTrue(cJSON_GetObjectItem(account_state, "twofa")));
        const char *granted = kn_json_str(account_state, "granted", NULL);
        cJSON_AddBoolToObject(acc, "authorized", granted && scopes_within(&c->scopes, granted));
        cJSON_AddItemToObject(d, "account", acc);
    } else {
        cJSON_AddNullToObject(d, "account");
    }
    v10_reply_data(c->req, 200, d);
    cJSON_Delete(c->client);
    free(c);
}

static void info_state(kn_db_result *res, void *ud) {
    InfoCtx *c = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    info_reply(c, o);
    cJSON_Delete(o);
}

static void info_client(kn_db_result *res, void *ud) {
    InfoCtx *c = ud;
    c->client = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const char *err = !kn_db_ok(res) ? "Server error." : authz_client_error(c->client, &c->a, &c->scopes);
    if (err) {
        v10_reply_error(c->req, 400, "OAUTH_INVALID_REQUEST", err);
        cJSON_Delete(c->client);
        free(c);
        return;
    }
    if (c->account <= 0) return info_reply(c, NULL);
    char acc[24];
    kn_db_call(O.b->db, &P_OA_CONSENT_STATE, KN_DB_ARGS(acct_itoa(acc, c->account), c->a.client_id), info_state, c);
}

static void info_signed_in(void *ud, const char *err, const cJSON *acc) {
    InfoCtx *c = ud;
    if (!err && acc) {
        c->account = kn_json_int(acc, "id", 0);
        copy_str(c->name, sizeof c->name, kn_json_str(acc, "username", ""));
        c->level = acc_level(acc);
    }
    kn_db_call(O.b->db, &P_OA_CLIENT, KN_DB_ARGS(c->a.client_id), info_client, c);
}

static void h_authorize_info(kn_http_req *req, void *ud) {
    (void) ud;
    if (limited(req, "authorize")) return v10_reply_error(req, 429, "RATE_LIMITED", "Slow down.");
    InfoCtx *c = calloc(1, sizeof *c);
    if (!c) return v10_reply_error(req, 500, "internal", "out of memory");
    c->req = req;
    authz_from_query(req, &c->a);
    const char *err = authz_shape_error(&c->a, &c->scopes);
    if (err) {
        free(c);
        return v10_reply_error(req, 400, "OAUTH_INVALID_REQUEST", err);
    }
    const char *bearer = kn_http_bearer(req);
    if (!bearer || !*bearer) return info_signed_in(c, "signed out", NULL); /* the page may ask before the sign-in */
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", bearer);
    accounts_internal_call(O.b, "token", args, info_signed_in, c);
    cJSON_Delete(args);
}

/* ---- POST /oauth/authorize (authorize, :27661) */

static void authorize_done(void *ud, const char *redirect, const char *error) {
    kn_http_req *req = ud;
    if (!redirect) return v10_reply_error(req, 400, "OAUTH_DENIED", error ? error : "Not authorized.");
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "redirect_url", redirect);
    v10_reply_data(req, 200, d);
}

static void authorize_signed_in(kn_http_req *req, const cJSON *acc, void *ud) {
    AuthzReq *a = ud;
    decide_start(a, kn_json_int(acc, "id", 0), acc_level(acc), authorize_done, req);
    free(a);
}

static void h_authorize_post(kn_http_req *req, void *ud) {
    (void) ud;
    if (limited(req, "authorize")) return v10_reply_error(req, 429, "RATE_LIMITED", "Slow down.");
    AuthzReq *a = calloc(1, sizeof *a);
    if (!a) return v10_reply_error(req, 500, "internal", "out of memory");
    cJSON *json = kn_http_body_json(req);
    authz_from_body(req, json ? json : cJSON_CreateObject(), a);
    cJSON_Delete(json);
    with_session(req, authorize_signed_in, a);
}

/* ================================================================== the token endpoint (RFC 6749 4.1.3, 6) */

typedef struct {
    kn_http_req *req;
    char access[96], refresh[96];
} TokenCtx;

static void token_done(kn_db_result *res, void *ud) {
    TokenCtx *t = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const char *soft = kn_json_str(o, "error", NULL); /* the code exchange's failure (it is spent) */
    if (!o || soft) {
        const char *e = soft ? soft : acct_db_error(res, "oauth_token");
        const int client = !strcmp(e, "invalid_client");
        oauth_error(t->req, client ? 401 : 400, client ? "invalid_client" : !strcmp(e, "invalid_grant") ? "invalid_grant" : "server_error", NULL);
    } else {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "access_token", t->access);
        cJSON_AddStringToObject(r, "token_type", "Bearer");
        cJSON_AddNumberToObject(r, "expires_in", (double) O.access_ttl);
        cJSON_AddStringToObject(r, "refresh_token", t->refresh);
        cJSON_AddStringToObject(r, "scope", kn_json_str(o, "scope", ""));
        kn_http_set_header(t->req, "Cache-Control", "no-store");
        kn_http_set_header(t->req, "Pragma", "no-cache");
        kn_http_respond_json(t->req, 200, r);
        cJSON_Delete(r);
    }
    cJSON_Delete(o);
    memset(t, 0, sizeof *t);
    free(t);
}

/* the client's credentials: HTTP Basic (RFC 6749 2.3.1) or the body's client_id / client_secret */
static void client_credentials(kn_http_req *req, const cJSON *json, char *id, size_t idcap, char *secret, size_t seccap) {
    id[0] = secret[0] = 0;
    const char *auth = kn_http_header_get(req, "Authorization");
    if (auth && !strncasecmp(auth, "Basic ", 6)) {
        uint8_t raw[512];
        const int n = kn_base64_decode(auth + 6, strlen(auth + 6), raw, sizeof raw - 1);
        if (n > 0) {
            raw[n] = 0;
            char *colon = strchr((char *) raw, ':');
            if (colon) {
                *colon = 0;
                kn_url_decode((char *) raw, strlen((char *) raw), id, idcap, 1);
                kn_url_decode(colon + 1, strlen(colon + 1), secret, seccap, 1);
            }
        }
        memset(raw, 0, sizeof raw);
    }
    if (!id[0]) body_get(req, json, "client_id", id, idcap);
    if (!secret[0]) body_get(req, json, "client_secret", secret, seccap);
}

static void h_token(kn_http_req *req, void *ud) {
    (void) ud;
    if (limited(req, "token")) return oauth_error(req, 429, "slow_down", NULL);
    cJSON *json = body_json(req);
    char grant[32], id[80], secret[160], code[160], redirect[512], verifier[160], rt[160];
    body_get(req, json, "grant_type", grant, sizeof grant);
    client_credentials(req, json, id, sizeof id, secret, sizeof secret);
    TokenCtx *t = calloc(1, sizeof *t);
    if (!t || new_token(t->access, sizeof t->access, "kbo_at_") || new_token(t->refresh, sizeof t->refresh, "kbo_rt_")) {
        free(t);
        cJSON_Delete(json);
        return oauth_error(req, 500, "server_error", NULL);
    }
    t->req = req;
    char at[24], rtt[24], ct[24];
    acct_itoa(at, O.access_ttl);
    acct_itoa(rtt, O.refresh_ttl);
    acct_itoa(ct, O.code_ttl);
    if (!id[0]) {
        free(t);
        oauth_error(req, 401, "invalid_client", "client_id is missing.");
    } else if (!strcmp(grant, "authorization_code")) {
        body_get(req, json, "code", code, sizeof code);
        body_get(req, json, "redirect_uri", redirect, sizeof redirect);
        body_get(req, json, "code_verifier", verifier, sizeof verifier);
        if (!code[0] || !verifier[0]) {
            free(t);
            oauth_error(req, 400, "invalid_request", "code and code_verifier are required.");
        } else {
            kn_db_call(O.b->db, &P_OA_CODE_EXCHANGE, KN_DB_ARGS(code, id, secret, redirect, verifier, t->access, t->refresh, at, rtt, ct), token_done, t);
        }
    } else if (!strcmp(grant, "refresh_token")) {
        body_get(req, json, "refresh_token", rt, sizeof rt);
        if (!rt[0]) {
            free(t);
            oauth_error(req, 400, "invalid_request", "refresh_token is required.");
        } else {
            kn_db_call(O.b->db, &P_OA_REFRESH, KN_DB_ARGS(rt, id, secret, t->access, t->refresh, at, rtt), token_done, t);
        }
    } else {
        free(t);
        oauth_error(req, 400, "unsupported_grant_type", NULL);
    }
    memset(secret, 0, sizeof secret);
    memset(code, 0, sizeof code);
    memset(rt, 0, sizeof rt);
    cJSON_Delete(json);
}

static void revoke_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    if (!kn_db_ok(res)) return oauth_error(req, 500, "server_error", NULL);
    kn_http_set_header(req, "Cache-Control", "no-store");
    kn_http_respond(req, 200, "application/json", "{}", 2);
}

/* POST /oauth/revoke {token, client_id}: RFC 7009 (always 200 for a well-formed request) */
static void h_revoke(kn_http_req *req, void *ud) {
    (void) ud;
    if (limited(req, "token")) return oauth_error(req, 429, "slow_down", NULL);
    cJSON *json = body_json(req);
    char token[160], id[80], secret[160];
    body_get(req, json, "token", token, sizeof token);
    client_credentials(req, json, id, sizeof id, secret, sizeof secret);
    cJSON_Delete(json);
    memset(secret, 0, sizeof secret);
    if (!token[0] || !id[0]) return oauth_error(req, 400, "invalid_request", "token and client_id are required.");
    kn_db_call(O.b->db, &P_OA_REVOKE, KN_DB_ARGS(token, id), revoke_done, req);
}

/* ================================================================== the apps' data routes */

typedef struct {
    kn_http_req *req;
    char client[80];
    int what; /* 0 userinfo, 1 stats, 2 inventory */
} ReadCtx;

static void read_account(void *ud, const char *err, const cJSON *acc) {
    ReadCtx *c = ud;
    if (err || !acc) {
        oauth_error(c->req, 404, "not_found", "The account is gone.");
        free(c);
        return;
    }
    cJSON *o = cJSON_CreateObject();
    const int64_t id = kn_json_int(acc, "id", 0);
    if (c->what == 0) {
        char sub[24];
        cJSON_AddStringToObject(o, "sub", acct_itoa(sub, id));
        cJSON_AddNumberToObject(o, "id", (double) id);
        cJSON_AddStringToObject(o, "username", kn_json_str(acc, "username", ""));
        cJSON_AddNumberToObject(o, "level", acc_level(acc));
        const char *clan = kn_json_str(acc, "clan", NULL);
        if (clan && *clan) cJSON_AddStringToObject(o, "clan", clan);
        else cJSON_AddNullToObject(o, "clan");
        cJSON_AddBoolToObject(o, "premium", kn_json_int(acc, "premium", 0) > 0);
        const cJSON *badges = cJSON_GetObjectItemCaseSensitive(acc, "badges");
        cJSON_AddItemToObject(o, "badges", cJSON_IsArray(badges) ? cJSON_Duplicate(badges, 1) : cJSON_CreateArray());
    } else if (c->what == 1) {
        /* Account.getData() slots (db_account_json): 1 kills, 2 wins, 3 games, 4 deaths, 8 time played, 16 stats */
        const cJSON *d = cJSON_GetObjectItemCaseSensitive(acc, "data");
        static const struct { int slot; const char *key; } k[] = {{1, "kills"}, {2, "wins"}, {3, "games"}, {4, "deaths"}, {8, "time_played"}};
        for (size_t i = 0; i < sizeof k / sizeof k[0]; i++)
            cJSON_AddNumberToObject(o, k[i].key, cJSON_GetNumberValue(cJSON_GetArrayItem(d, k[i].slot)));
        const cJSON *st = cJSON_GetArrayItem(d, 16);
        cJSON_AddItemToObject(o, "stats", cJSON_IsObject(st) ? cJSON_Duplicate(st, 1) : cJSON_CreateObject());
        cJSON_AddNumberToObject(o, "level", acc_level(acc));
        cJSON_AddNumberToObject(o, "score", (double) kn_json_int(acc, "score", 0));
        static const char *const rated[] = {"elo", "elo2", "elo4", "chal"}; /* the ranked ratings, the challenge level */
        for (size_t i = 0; i < 4; i++) cJSON_AddNumberToObject(o, rated[i], (double) kn_json_int(acc, rated[i], 0));
    } else {
        cJSON_AddNumberToObject(o, "funds", (double) kn_json_int(acc, "funds", 0));
        cJSON_AddStringToObject(o, "junk", kn_json_str(acc, "junk", "0.00"));
        const cJSON *sk = cJSON_GetArrayItem(cJSON_GetObjectItemCaseSensitive(acc, "data"), 10); /* the owned skins slot */
        cJSON_AddItemToObject(o, "skins", cJSON_IsArray(sk) ? cJSON_Duplicate(sk, 1) : cJSON_CreateArray());
    }
    kn_http_set_header(c->req, "Cache-Control", "no-store");
    kn_http_respond_json(c->req, 200, o);
    cJSON_Delete(o);
    static const char *const scope[] = {"profile", "stats:read", "inventory:read"};
    log_activity(id, c->client, c->what == 0 ? "userinfo" : c->what == 1 ? "stats" : "inventory", scope[c->what], "ok");
    free(c);
}

static void read_app(kn_http_req *req, int64_t account, const char *client, const char *scopes, void *ud) {
    (void) scopes;
    ReadCtx *c = ud;
    c->req = req;
    copy_str(c->client, sizeof c->client, client);
    cJSON *args = cJSON_CreateObject();
    cJSON_AddNumberToObject(args, "id", (double) account);
    accounts_internal_call(O.b, "get", args, read_account, c);
    cJSON_Delete(args);
}

static void read_route(kn_http_req *req, int what) {
    static const char *const scope[] = {"profile", "stats:read", "inventory:read"};
    ReadCtx *c = calloc(1, sizeof *c);
    if (!c) return oauth_error(req, 500, "server_error", NULL);
    c->what = what;
    with_app(req, scope[what], read_app, c);
}

static void h_userinfo(kn_http_req *req, void *ud) { (void) ud; read_route(req, 0); }
static void h_api_stats(kn_http_req *req, void *ud) { (void) ud; read_route(req, 1); }
static void h_api_inventory(kn_http_req *req, void *ud) { (void) ud; read_route(req, 2); }

typedef struct {
    kn_http_req *req;
    int64_t account;
    char client[80];
} FriendsCtx;

static void friends_done(kn_db_result *res, void *ud) {
    FriendsCtx *c = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!o) oauth_error(c->req, 500, "server_error", NULL);
    else {
        kn_http_respond_json(c->req, 200, o);
        log_activity(c->account, c->client, "friends", "friends:read", "ok");
    }
    cJSON_Delete(o);
    free(c);
}

static void friends_app(kn_http_req *req, int64_t account, const char *client, const char *scopes, void *ud) {
    (void) scopes;
    FriendsCtx *c = ud;
    c->req = req;
    c->account = account;
    copy_str(c->client, sizeof c->client, client);
    char id[24];
    kn_db_call(O.b->db, &P_OA_FRIENDS, KN_DB_ARGS(acct_itoa(id, account)), friends_done, c);
}

static void h_api_friends(kn_http_req *req, void *ud) {
    (void) ud;
    FriendsCtx *c = calloc(1, sizeof *c);
    if (!c) return oauth_error(req, 500, "server_error", NULL);
    with_app(req, "friends:read", friends_app, c);
}

/* ================================================================== actions: an app's write, approved by the player
 * oauth.action.* (deob-10.0.7.js:204284): the app sends exactly what it wants done; the player sees it on the
 * approval page (or the client's actionInfo) and approves with a 2FA code; it runs once, as shown. */

typedef struct {
    const char *kind, *scope;
} ActionKind;
static const ActionKind k_actions[] = {
    {"gift.send", "inventory:write:currency"}, /* {to, amount, message?}: the KR gift (P_GIFT, its fee and limits) */
    {"follow.add", "friends:write"},           /* {name} */
    {"follow.remove", "friends:write"},        /* {name} */
};

static const ActionKind *action_kind(const char *kind) {
    for (size_t i = 0; i < sizeof k_actions / sizeof k_actions[0]; i++)
        if (kind && !strcmp(kind, k_actions[i].kind)) return &k_actions[i];
    return NULL;
}

/* the params an action keeps (validated, normalised): NULL + why when they are wrong */
static cJSON *action_params(const char *kind, const cJSON *in, const char **why) {
    cJSON *o = cJSON_CreateObject();
    *why = NULL;
    if (!strcmp(kind, "gift.send")) {
        const char *to = kn_json_str(in, "to", NULL);
        const double amt = cJSON_GetNumberValue(cJSON_GetObjectItem(in, "amount"));
        const char *msg = kn_json_str(in, "message", "");
        while (*msg && isspace((unsigned char) *msg)) msg++;
        size_t nlen = strlen(msg);
        while (nlen && isspace((unsigned char) msg[nlen - 1])) nlen--;
        /* the gift box trims, then stops at 500 characters (deob-10.0.7.js:111688, :111703), not 300 bytes */
        char *note = nlen ? malloc(nlen + 1) : NULL;
        if (nlen && note) {
            memcpy(note, msg, nlen);
            note[nlen] = 0;
        }
        if (!to || !*to || strlen(to) > 64) *why = "params.to is required.";
        else if (!(amt >= 1 && amt <= (double) g_acct.gift_max) || amt != (double) (int64_t) amt) *why = "params.amount must be a whole KR amount.";
        else if (nlen && (!note || brain_utf16_prefix(note, 500) < nlen)) *why = "params.message is too long.";
        else {
            cJSON_AddStringToObject(o, "to", to);
            cJSON_AddNumberToObject(o, "amount", amt);
            cJSON_AddStringToObject(o, "message", note ? note : "");
        }
        free(note);
    } else {
        const char *name = kn_json_str(in, "name", NULL);
        if (!name || !*name || strlen(name) > 64) *why = "params.name is required.";
        else cJSON_AddStringToObject(o, "name", name);
    }
    if (*why) {
        cJSON_Delete(o);
        return NULL;
    }
    return o;
}

static char *approve_url(const char *id) {
    char *u = malloc(strlen(O.public_url) + strlen(id) + 32);
    if (u) sprintf(u, "%s/oauth/approve/%s", O.public_url, id);
    return u;
}

typedef struct {
    kn_http_req *req;
    char id[64];
} NewActionCtx;

static void action_created(kn_db_result *res, void *ud) {
    NewActionCtx *c = ud;
    if (!kn_db_ok(res)) {
        const char *e = acct_db_error(res, "oauth_action_new");
        if (!strcmp(e, "insufficient_scope")) oauth_error(c->req, 403, "insufficient_scope", NULL);
        else oauth_error(c->req, 500, "server_error", NULL);
    } else {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "id", c->id);
        char *u = approve_url(c->id);
        cJSON_AddStringToObject(o, "approve_url", u ? u : "");
        free(u);
        cJSON_AddNumberToObject(o, "expires_in", (double) O.action_ttl);
        cJSON_AddStringToObject(o, "status", "pending");
        kn_http_respond_json(c->req, 201, o);
        cJSON_Delete(o);
    }
    free(c);
}

static void action_new_app(kn_http_req *req, int64_t account, const char *client, const char *scopes, void *ud) {
    cJSON *body = ud;
    const char *kind = kn_json_str(body, "kind", NULL);
    const ActionKind *k = action_kind(kind);
    const char *why = NULL;
    cJSON *params = k ? action_params(k->kind, cJSON_GetObjectItem(body, "params"), &why) : NULL;
    if (!k || !params) {
        cJSON_Delete(body);
        return oauth_error(req, 400, "invalid_request", !k ? "Unknown action kind." : why);
    }
    if (!scope_has(scopes, k->scope)) {
        cJSON_Delete(params);
        cJSON_Delete(body);
        return oauth_error(req, 403, "insufficient_scope", k->scope);
    }
    NewActionCtx *c = calloc(1, sizeof *c);
    char *ptext = cJSON_PrintUnformatted(params);
    if (!c || !ptext || new_token(c->id, sizeof c->id, "act_") != 0) {
        free(c);
        free(ptext);
        cJSON_Delete(params);
        cJSON_Delete(body);
        return oauth_error(req, 500, "server_error", NULL);
    }
    c->req = req;
    char acc[24];
    kn_db_call(O.b->db, &P_OA_ACTION_NEW, KN_DB_ARGS(c->id, client, acct_itoa(acc, account), k->kind, k->scope, ptext), action_created, c);
    free(ptext);
    cJSON_Delete(params);
    cJSON_Delete(body);
}

/* POST /oauth/api/actions {kind, params} (an app token holding the kind's write scope; a small per-address rate) */
static void h_api_action_new(kn_http_req *req, void *ud) {
    (void) ud;
    if (limited(req, "action")) return oauth_error(req, 429, "slow_down", NULL);
    cJSON *body = kn_http_body_json(req);
    if (!body) return oauth_error(req, 400, "invalid_request", "A JSON body is required.");
    const ActionKind *k = action_kind(kn_json_str(body, "kind", NULL));
    with_app(req, k ? k->scope : "profile", action_new_app, body);
}

/* the action for display: the client's actionInfo and the app's status poll */
static cJSON *action_public(const cJSON *row, int with_params) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "id", kn_json_str(row, "action_id", ""));
    cJSON_AddStringToObject(o, "kind", kn_json_str(row, "kind", ""));
    cJSON_AddStringToObject(o, "scope", kn_json_str(row, "scope", ""));
    cJSON_AddStringToObject(o, "status", kn_json_str(row, "status", ""));
    const char *r = kn_json_str(row, "result", NULL);
    if (r && *r) cJSON_AddStringToObject(o, "result", r);
    cJSON_AddNumberToObject(o, "created_at", (double) (kn_json_int(row, "created", 0) / 1000));
    if (with_params) {
        cJSON *app = cJSON_AddObjectToObject(o, "app");
        cJSON_AddStringToObject(app, "client_id", kn_json_str(row, "client_id", ""));
        cJSON_AddStringToObject(app, "name", kn_json_str(row, "app_name", ""));
        cJSON *pp = cJSON_Parse(kn_json_str(row, "params", "{}"));
        cJSON_AddItemToObject(o, "params", pp ? pp : cJSON_CreateObject());
    }
    return o;
}

typedef struct {
    kn_http_req *req;
    int64_t account;  /* the caller (the player's session or the app's grant) */
    char client[80];  /* the app (the app's poll: it may only read its own actions) */
    char id[64];
    char twofa[16];
    int mode;         /* 0 info (session), 1 approve, 2 deny, 3 the app's poll */
    void (*reply)(void *ud, int status, const char *code, const char *msg, const cJSON *action); /* the page's */
    void *reply_ud;
} ActCtx;

static void act_answer(ActCtx *c, int status, const char *code, const char *msg, const cJSON *row) {
    if (c->reply) {
        c->reply(c->reply_ud, status, code, msg, row);
    } else if (c->mode == 3) {
        if (row) {
            cJSON *o = action_public(row, 0);
            kn_http_respond_json(c->req, 200, o);
            cJSON_Delete(o);
        } else {
            oauth_error(c->req, status, code ? code : "not_found", msg);
        }
    } else if (row) {
        v10_reply_data(c->req, 200, action_public(row, 1));
    } else {
        v10_reply_error(c->req, status, code ? code : "OAUTH_ACTION_NOT_FOUND", msg ? msg : "This request is unknown.");
    }
    free(c);
}

static void act_reload_done(kn_db_result *res, void *ud) {
    ActCtx *c = ud;
    cJSON *row = kn_db_ok(res) ? acct_db_json(res) : NULL;
    act_answer(c, row ? 200 : 404, NULL, NULL, row);
    cJSON_Delete(row);
}

static void act_reload(ActCtx *c) {
    char ttl[24];
    kn_db_call(O.b->db, &P_OA_ACTION_GET, KN_DB_ARGS(c->id, acct_itoa(ttl, O.action_ttl)), act_reload_done, c);
}

typedef struct {
    ActCtx *c;
    char kind[32];
    char name[80];
} RunCtx;

static void act_finished(kn_db_result *res, void *ud) {
    ActCtx *c = ud;
    if (!kn_db_ok(res)) KN_WARN(MOD, "action %s: %s", c->id, acct_db_error(res, "oauth_action_finish"));
    act_reload(c);
}

static void act_finish(ActCtx *c, const char *status, const char *result) {
    kn_db_call(O.b->db, &P_OA_ACTION_FINISH, KN_DB_ARGS(c->id, status, result ? result : ""), act_finished, c);
}

static void act_ran(kn_db_result *res, void *ud) {
    RunCtx *r = ud;
    ActCtx *c = r->c;
    if (kn_db_ok(res)) {
        if (!strncmp(r->kind, "follow.", 7)) social_eb_follow_changed(c->account, r->name); /* v10 friends (social_eb.c) */
        KN_INFO(MOD, "account %lld approved %s (%s)", (long long) c->account, r->kind, c->id);
        act_finish(c, "approved", NULL);
    } else {
        act_finish(c, "failed", acct_db_error(res, r->kind));
    }
    free(r);
}

/* the approved action runs once, through the same operation the game uses */
static void act_run(ActCtx *c, const cJSON *row) {
    const char *kind = kn_json_str(row, "kind", "");
    cJSON *params = cJSON_Parse(kn_json_str(row, "params", "{}"));
    RunCtx *r = calloc(1, sizeof *r);
    if (!r) {
        cJSON_Delete(params);
        return act_finish(c, "failed", "Server error.");
    }
    r->c = c;
    copy_str(r->kind, sizeof r->kind, kind);
    char me[24];
    acct_itoa(me, c->account);
    if (!strcmp(kind, "gift.send")) {
        char key[96], amt[24], fee[24], lvl[24], mn[24], mx[24];
        snprintf(key, sizeof key, "oauth:%s", c->id); /* economy_requests: the gift cannot run twice */
        kn_db_call(O.b->db, &P_GIFT,
                   KN_DB_ARGS(me, key, "0", "gift", kn_json_str(params, "to", ""), acct_itoa(amt, kn_json_int(params, "amount", 0)),
                              acct_itoa(fee, g_acct.gift_fee_percent), acct_itoa(lvl, g_acct.gift_min_level), kn_json_str(params, "message", ""),
                              acct_itoa(mn, g_acct.gift_min), acct_itoa(mx, g_acct.gift_max)),
                   act_ran, r);
    } else if (!strcmp(kind, "follow.add") || !strcmp(kind, "follow.remove")) {
        copy_str(r->name, sizeof r->name, kn_json_str(params, "name", ""));
        kn_db_call(O.b->db, &P_OA_FOLLOW, KN_DB_ARGS(me, r->name, !strcmp(kind, "follow.add") ? "1" : "0"), act_ran, r);
    } else {
        free(r);
        act_finish(c, "failed", "Unknown action.");
    }
    cJSON_Delete(params);
}

static void act_loaded_for_run(kn_db_result *res, void *ud) {
    ActCtx *c = ud;
    cJSON *row = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!row || strcmp(kn_json_str(row, "status", ""), "running")) {
        cJSON_Delete(row);
        return act_reload(c);
    }
    act_run(c, row);
    cJSON_Delete(row);
}

static void act_claimed_run(kn_db_result *res, void *ud) {
    ActCtx *c = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const int claimed = cJSON_IsTrue(cJSON_GetObjectItem(o, "claimed"));
    cJSON_Delete(o);
    if (!kn_db_ok(res)) return act_answer(c, 500, "internal", acct_db_error(res, "oauth_action_claim"), NULL);
    if (!claimed || c->mode == 2) return act_reload(c);
    char ttl[24];
    kn_db_call(O.b->db, &P_OA_ACTION_GET, KN_DB_ARGS(c->id, acct_itoa(ttl, O.action_ttl * 2)), act_loaded_for_run, c);
}

static void act_claim(ActCtx *c, const char *to) {
    char acc[24], ttl[24];
    kn_db_call(O.b->db, &P_OA_ACTION_CLAIM, KN_DB_ARGS(c->id, acct_itoa(acc, c->account), acct_itoa(ttl, O.action_ttl), to), act_claimed_run, c);
}

static void act_twofa(kn_db_result *res, void *ud) {
    ActCtx *c = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const int ok = kn_json_int(o, "ok", 0) == 1;
    cJSON_Delete(o);
    if (!kn_db_ok(res)) return act_answer(c, 400, "OAUTH_TWOFA", acct_db_error(res, "twofa"), NULL);
    if (!ok) return act_answer(c, 400, "OAUTH_TWOFA", "Invalid Code", NULL);
    act_claim(c, "running");
}

static void act_loaded(kn_db_result *res, void *ud) {
    ActCtx *c = ud;
    cJSON *row = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const int mine = row && (c->mode == 3 ? !strcmp(kn_json_str(row, "client_id", ""), c->client) && kn_json_int(row, "account", 0) == c->account
                                          : kn_json_int(row, "account", 0) == c->account);
    if (!mine) {
        cJSON_Delete(row);
        return act_answer(c, 404, NULL, "This request is unknown.", NULL);
    }
    if (c->mode == 0 || c->mode == 3 || strcmp(kn_json_str(row, "status", ""), "pending")) {
        act_answer(c, 200, NULL, NULL, row); /* info, a poll, or a decided / expired action (its status says so) */
        cJSON_Delete(row);
        return;
    }
    cJSON_Delete(row);
    if (c->mode == 2) return act_claim(c, "denied");
    if (!c->twofa[0]) return act_answer(c, 400, "OAUTH_TWOFA", "Enter the code from your authenticator app.", NULL);
    char acc[24];
    char now[24];
    kn_db_call(O.b->db, &P_TWOFA_LOGIN, KN_DB_ARGS(acct_itoa(acc, c->account), c->twofa, acct_itoa(now, (int64_t) time(NULL))), act_twofa, c);
}

static void act_start(ActCtx *c) {
    char ttl[24];
    kn_db_call(O.b->db, &P_OA_ACTION_GET, KN_DB_ARGS(c->id, acct_itoa(ttl, O.action_ttl)), act_loaded, c);
}

static void act_session(kn_http_req *req, const cJSON *acc, void *ud) {
    ActCtx *c = ud;
    c->req = req;
    c->account = kn_json_int(acc, "id", 0);
    act_start(c);
}

static void act_route(kn_http_req *req, int mode) {
    if (mode == 1 && limited(req, "approve")) return v10_reply_error(req, 429, "RATE_LIMITED", "Slow down.");
    ActCtx *c = calloc(1, sizeof *c);
    if (!c) return v10_reply_error(req, 500, "internal", "out of memory");
    copy_str(c->id, sizeof c->id, kn_http_param(req, "id"));
    c->mode = mode;
    if (mode == 1) {
        cJSON *b = kn_http_body_json(req);
        copy_str(c->twofa, sizeof c->twofa, kn_json_str(b, "twofa_code", ""));
        cJSON_Delete(b);
    }
    with_session(req, act_session, c);
}

static void h_action_info(kn_http_req *req, void *ud) { (void) ud; act_route(req, 0); }
static void h_action_approve(kn_http_req *req, void *ud) { (void) ud; act_route(req, 1); }
static void h_action_deny(kn_http_req *req, void *ud) { (void) ud; act_route(req, 2); }

static void act_poll_app(kn_http_req *req, int64_t account, const char *client, const char *scopes, void *ud) {
    (void) scopes;
    ActCtx *c = ud;
    c->req = req;
    c->account = account;
    copy_str(c->client, sizeof c->client, client);
    act_start(c);
}

/* GET /oauth/api/actions/:id: the app's own action's status */
static void h_api_action_get(kn_http_req *req, void *ud) {
    (void) ud;
    ActCtx *c = calloc(1, sizeof *c);
    if (!c) return oauth_error(req, 500, "server_error", NULL);
    copy_str(c->id, sizeof c->id, kn_http_param(req, "id"));
    c->mode = 3;
    with_app(req, "profile", act_poll_app, c);
}

/* ================================================================== Settings > Connected apps (deob-10.0.7.js:97493-97673) */

static void apps_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    cJSON *rows = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!rows) return v10_reply_error(req, 500, "internal", acct_db_error(res, "oauth_apps"));
    cJSON *out = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "client_id", kn_json_str(r, "client_id", ""));
        cJSON_AddStringToObject(o, "name", kn_json_str(r, "name", ""));
        static const char *const opt[] = {"icon_url", "homepage_url", "developer"};
        for (size_t i = 0; i < 3; i++) {
            const char *v = kn_json_str(r, opt[i], NULL);
            if (v && *v) cJSON_AddStringToObject(o, opt[i], v);
            else cJSON_AddNullToObject(o, opt[i]);
        }
        cJSON_AddItemToObject(o, "scopes", scopes_array(kn_json_str(r, "scopes", "")));
        cJSON_AddNumberToObject(o, "created_at", (double) (kn_json_int(r, "created", 0) / 1000)); /* seconds (:97519) */
        const int64_t used = kn_json_int(r, "used", 0);
        if (used > 0) cJSON_AddNumberToObject(o, "last_used_at", (double) (used / 1000));
        else cJSON_AddNullToObject(o, "last_used_at");
        cJSON_AddItemToArray(out, o);
    }
    cJSON_Delete(rows);
    v10_reply_data(req, 200, out);
}

static void apps_session(kn_http_req *req, const cJSON *acc, void *ud) {
    (void) ud;
    char id[24];
    kn_db_call(O.b->db, &P_OA_APPS, KN_DB_ARGS(acct_itoa(id, kn_json_int(acc, "id", 0))), apps_done, req);
}

static void h_apps(kn_http_req *req, void *ud) {
    (void) ud;
    with_session(req, apps_session, NULL);
}

static void app_revoke_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!o) return v10_reply_error(req, 500, "internal", acct_db_error(res, "oauth_app_revoke"));
    v10_reply_data(req, 200, o);
}

static void app_revoke_session(kn_http_req *req, const cJSON *acc, void *ud) {
    (void) ud;
    char id[24];
    kn_db_call(O.b->db, &P_OA_APP_REVOKE, KN_DB_ARGS(acct_itoa(id, kn_json_int(acc, "id", 0)), kn_http_param(req, "id")), app_revoke_done, req);
}

static void h_app_revoke(kn_http_req *req, void *ud) {
    (void) ud;
    with_session(req, app_revoke_session, NULL);
}

static void app_activity_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    cJSON *rows = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!rows) return v10_reply_error(req, 500, "internal", acct_db_error(res, "oauth_app_activity"));
    cJSON *out = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "created_at", (double) (kn_json_int(r, "created", 0) / 1000));
        cJSON_AddStringToObject(o, "action", kn_json_str(r, "action", ""));
        const char *sc = kn_json_str(r, "scope", NULL);
        if (sc && *sc) cJSON_AddStringToObject(o, "scope", sc);
        else cJSON_AddNullToObject(o, "scope");
        cJSON_AddStringToObject(o, "result", kn_json_str(r, "result", ""));
        cJSON_AddItemToArray(out, o);
    }
    cJSON_Delete(rows);
    v10_reply_data(req, 200, out);
}

static void app_activity_session(kn_http_req *req, const cJSON *acc, void *ud) {
    (void) ud;
    char id[24];
    kn_db_call(O.b->db, &P_OA_APP_ACTIVITY, KN_DB_ARGS(acct_itoa(id, kn_json_int(acc, "id", 0)), kn_http_param(req, "id")), app_activity_done, req);
}

static void h_app_activity(kn_http_req *req, void *ud) {
    (void) ud;
    with_session(req, app_activity_session, NULL);
}

/* ================================================================== the browser pages (no script)
 * The consent page (the oauth.* screen of krunker.io, which is not in game.js) and the action approval page, both
 * plain forms. The player signs in on the page itself (a Box Office password and 2FA code); the page keeps only a
 * short signed cookie with the account id (COOKIE, Path=/oauth, HttpOnly, SameSite=Lax, O.cookie_ttl); the game
 * session the sign-in creates is revoked at once. Every form carries a token derived from that cookie (CSRF). */

static void esc(kn_buf *b, const char *s) {
    for (; s && *s; s++) {
        switch (*s) {
            case '<': kn_buf_append_str(b, "&lt;"); break;
            case '>': kn_buf_append_str(b, "&gt;"); break;
            case '&': kn_buf_append_str(b, "&amp;"); break;
            case '"': kn_buf_append_str(b, "&quot;"); break;
            case '\'': kn_buf_append_str(b, "&#39;"); break;
            default: kn_buf_append_byte(b, (uint8_t) *s);
        }
    }
}

static void page_open(kn_buf *b, const char *title) {
    kn_buf_append_str(b, "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\">"
                         "<meta name=\"referrer\" content=\"no-referrer\"><title>");
    esc(b, title);
    kn_buf_append_str(b, " - ");
    esc(b, KN_PRODUCT_NAME);
    kn_buf_append_str(b, "</title><style>body{margin:0;background:#202020;color:#fff;font-family:sans-serif}"
                         ".c{max-width:460px;margin:48px auto;padding:24px;background:#2b2b2b;border-radius:8px}"
                         "h1{font-size:22px;margin:0 0 16px}.m{color:rgba(255,255,255,.7);line-height:1.5}"
                         ".app{display:flex;gap:12px;align-items:center;margin-bottom:16px}.app img,.ic{width:48px;height:48px;border-radius:8px}"
                         ".ic{background:#444;display:flex;align-items:center;justify-content:center;font-size:24px}"
                         "ul{padding-left:20px;line-height:1.7}.w{color:#ffca28}.e{color:#ef5350;margin:12px 0}"
                         "input[type=text],input[type=password]{width:100%;box-sizing:border-box;padding:10px;margin:6px 0 12px;"
                         "background:#1b1b1b;border:1px solid #555;color:#fff;border-radius:4px}"
                         "button{padding:10px 18px;border:0;border-radius:4px;font-size:16px;cursor:pointer;margin-right:8px}"
                         ".ok{background:#4caf50;color:#fff}.no{background:#555;color:#fff}.f{font-size:13px;color:rgba(255,255,255,.5);margin-top:18px}"
                         "a{color:#90caf9}</style></head><body><div class=\"c\"><h1>");
    esc(b, title);
    kn_buf_append_str(b, "</h1>");
}

static void page_close(kn_buf *b) { kn_buf_append_str(b, "</div></body></html>"); }

static void page_send(kn_http_req *req, int status, kn_buf *b) {
    kn_http_set_header(req, "Cache-Control", "no-store");
    kn_http_set_header(req, "X-Frame-Options", "DENY");
    kn_http_set_header(req, "Content-Security-Policy", "default-src 'none'; style-src 'unsafe-inline'; img-src https: data:; form-action 'self' https: http:; frame-ancestors 'none'");
    kn_http_respond(req, status, "text/html; charset=utf-8", b->data ? (const void *) b->data : (const void *) "", b->len);
    kn_buf_free(b);
}

static void page_message(kn_http_req *req, int status, const char *title, const char *text) {
    kn_buf b = {0};
    page_open(&b, title);
    kn_buf_append_str(&b, "<p class=\"m\">");
    esc(&b, text);
    kn_buf_append_str(&b, "</p>");
    page_close(&b);
    page_send(req, status, &b);
}

/* the cookie: a signed token {acc, name, level}; 0 + the claims when valid */
static cJSON *cookie_claims(kn_http_req *req) {
    const char *h = kn_http_header_get(req, "Cookie");
    const char *p = h ? strstr(h, COOKIE "=") : NULL;
    if (!p) return NULL;
    p += strlen(COOKIE "=");
    char tok[1024];
    size_t n = strcspn(p, ";");
    if (n >= sizeof tok) return NULL;
    memcpy(tok, p, n);
    tok[n] = 0;
    cJSON *c = kn_signed_token_verify(O.cookie_key, tok, 0, NULL, 0);
    if (c && strcmp(kn_json_str(c, "use", ""), "oauth-consent")) {
        cJSON_Delete(c);
        return NULL;
    }
    return c;
}

/* the form token of a cookie: HMAC(cookie key, "csrf" + account + iat) */
static void csrf_of(const cJSON *claims, char out[65]) {
    char data[96];
    snprintf(data, sizeof data, "csrf:%lld:%lld", (long long) kn_json_int(claims, "acc", 0), (long long) kn_json_int(claims, "iat", 0));
    if (kn_hmac_sha256_hex(O.cookie_key, data, strlen(data), out) != 0) out[0] = 0;
}

static int csrf_ok(kn_http_req *req, const cJSON *claims) {
    char want[65], got[80];
    csrf_of(claims, want);
    body_get(req, NULL, "csrf", got, sizeof got);
    return want[0] && strlen(got) == 64 && kn_ct_streq(want, got);
}

static void hidden(kn_buf *b, const char *k, const char *v) {
    kn_buf_append_str(b, "<input type=\"hidden\" name=\"");
    esc(b, k);
    kn_buf_append_str(b, "\" value=\"");
    esc(b, v);
    kn_buf_append_str(b, "\">");
}

static void authz_hidden(kn_buf *b, const AuthzReq *a) {
    hidden(b, "client_id", a->client_id);
    hidden(b, "redirect_uri", a->redirect_uri);
    hidden(b, "scope", a->scope);
    hidden(b, "state", a->state);
    hidden(b, "code_challenge", a->challenge);
    hidden(b, "code_challenge_method", a->method);
    hidden(b, "response_type", a->response_type[0] ? a->response_type : "code");
}

static void app_header(kn_buf *b, const cJSON *client) {
    kn_buf_append_str(b, "<div class=\"app\">");
    const char *icon = kn_json_str(client, "icon_url", NULL);
    if (icon && !strncmp(icon, "https://", 8)) {
        kn_buf_append_str(b, "<img alt=\"\" referrerpolicy=\"no-referrer\" src=\"");
        esc(b, icon);
        kn_buf_append_str(b, "\">");
    } else {
        char first[2] = {kn_json_str(client, "name", "?")[0], 0};
        kn_buf_append_str(b, "<div class=\"ic\">");
        esc(b, first);
        kn_buf_append_str(b, "</div>");
    }
    kn_buf_append_str(b, "<div><b>");
    esc(b, kn_json_str(client, "name", ""));
    kn_buf_append_str(b, "</b>");
    const char *dev = kn_json_str(client, "developer", NULL);
    if (dev && *dev) {
        kn_buf_append_str(b, "<div class=\"m\">by ");
        esc(b, dev);
        kn_buf_append_str(b, "</div>");
    }
    kn_buf_append_str(b, "</div></div>");
}

static void app_links(kn_buf *b, const cJSON *client) {
    static const char *const keys[] = {"homepage_url", "privacy_url", "tos_url"};
    static const char *const labels[] = {"Website", "Privacy policy", "Terms"};
    int any = 0;
    for (int i = 0; i < 3; i++) {
        const char *u = kn_json_str(client, keys[i], NULL);
        if (!u || strncmp(u, "https://", 8)) continue;
        kn_buf_append_str(b, any++ ? " &middot; " : "<div class=\"f\">");
        kn_buf_append_str(b, "<a rel=\"noopener noreferrer\" target=\"_blank\" href=\"");
        esc(b, u);
        kn_buf_append_str(b, "\">");
        kn_buf_append_str(b, labels[i]);
        kn_buf_append_str(b, "</a>");
    }
    if (any) kn_buf_append_str(b, "</div>");
}

static void signin_form(kn_buf *b, const char *action, const AuthzReq *a, const char *action_id, const char *error) {
    kn_buf_append_str(b, "<p class=\"m\">Sign in with your Box Office account to continue.</p>");
    if (error) {
        kn_buf_append_str(b, "<div class=\"e\">");
        esc(b, error);
        kn_buf_append_str(b, "</div>");
    }
    kn_buf_append_str(b, "<form method=\"post\" action=\"");
    esc(b, action);
    kn_buf_append_str(b, "\">");
    if (a) authz_hidden(b, a);
    if (action_id) hidden(b, "action_id", action_id);
    kn_buf_append_str(b, "<label>Username<input type=\"text\" name=\"username\" autocomplete=\"username\" required></label>"
                         "<label>Password<input type=\"password\" name=\"password\" autocomplete=\"current-password\" required></label>"
                         "<label>2FA code (if you use 2FA)<input type=\"text\" name=\"code\" inputmode=\"numeric\" autocomplete=\"one-time-code\"></label>"
                         "<button class=\"ok\" type=\"submit\">Sign in</button></form>");
}

/* ---- GET /oauth/authorize: the consent page */

typedef struct {
    kn_http_req *req;
    AuthzReq a;
    ScopeSet scopes;
    cJSON *claims;
    cJSON *client;
    char error[200];
} PageCtx;

static void page_ctx_free(PageCtx *c) {
    cJSON_Delete(c->claims);
    cJSON_Delete(c->client);
    free(c);
}

static void consent_render(PageCtx *c, const cJSON *state) {
    kn_buf b = {0};
    page_open(&b, "Login with Box Office");
    app_header(&b, c->client);
    if (!c->claims) {
        signin_form(&b, "/oauth/authorize/signin", &c->a, NULL, c->error[0] ? c->error : NULL);
        app_links(&b, c->client);
        page_close(&b);
        page_send(c->req, 200, &b);
        return page_ctx_free(c);
    }
    const int twofa = cJSON_IsTrue(cJSON_GetObjectItem(state, "twofa"));
    const char *granted = kn_json_str(state, "granted", NULL);
    const int again = granted && scopes_within(&c->scopes, granted) && !c->scopes.write;
    kn_buf_append_str(&b, "<p class=\"m\"><b>");
    esc(&b, kn_json_str(c->client, "name", ""));
    kn_buf_append_str(&b, "</b> wants to access your Box Office account <b>");
    esc(&b, kn_json_str(c->claims, "name", ""));
    kn_buf_append_str(&b, "</b>.</p>");
    if (c->error[0]) {
        kn_buf_append_str(&b, "<div class=\"e\">");
        esc(&b, c->error);
        kn_buf_append_str(&b, "</div>");
    }
    kn_buf_append_str(&b, "<p class=\"m\">This will allow the app to:</p><ul>");
    for (int i = 0; i < c->scopes.n; i++)
        if (!c->scopes.s[i]->write) {
            kn_buf_append_str(&b, "<li>");
            esc(&b, c->scopes.s[i]->what);
            kn_buf_append_str(&b, "</li>");
        }
    kn_buf_append_str(&b, "</ul>");
    char csrf[65];
    csrf_of(c->claims, csrf);
    kn_buf_append_str(&b, "<form method=\"post\" action=\"/oauth/authorize/decide\">");
    authz_hidden(&b, &c->a);
    hidden(&b, "csrf", csrf);
    if (c->scopes.write) {
        kn_buf_append_str(&b, "<p class=\"w\"><b>Write access</b>. This will also allow the app to act as you (each action waits for your "
                              "approval with a 2FA code):</p><ul>");
        for (int i = 0; i < c->scopes.n; i++)
            if (c->scopes.s[i]->write) {
                kn_buf_append_str(&b, "<li class=\"w\">");
                esc(&b, c->scopes.s[i]->what);
                kn_buf_append_str(&b, "</li>");
            }
        kn_buf_append_str(&b, "</ul>");
        if (!twofa) {
            kn_buf_append_str(&b, "<div class=\"e\">Write access needs 2FA. Turn on 2FA in the game (Account &gt; Settings &gt; Account "
                                  "Security) first.</div>");
        } else {
            kn_buf_append_str(&b, "<p class=\"m\">This application is not made by Box Office. Box Office cannot undo what you approve it to do.</p>"
                                  "<label><input type=\"checkbox\" name=\"accept\" value=\"1\"> I understand and accept the risks</label>"
                                  "<label>2FA code<input type=\"text\" name=\"twofa_code\" inputmode=\"numeric\" autocomplete=\"one-time-code\"></label>");
        }
    }
    kn_buf_append_str(&b, "<button class=\"ok\" type=\"submit\" name=\"decision\" value=\"allow\"");
    if (c->scopes.write && !twofa) kn_buf_append_str(&b, " disabled");
    kn_buf_append_str(&b, ">");
    if (again) {
        kn_buf_append_str(&b, "Continue as ");
        esc(&b, kn_json_str(c->claims, "name", ""));
    } else {
        kn_buf_append_str(&b, "Authorize");
    }
    kn_buf_append_str(&b, "</button><button class=\"no\" type=\"submit\" name=\"decision\" value=\"deny\">Cancel</button></form>"
                          "<form method=\"post\" action=\"/oauth/signout\">");
    authz_hidden(&b, &c->a);
    hidden(&b, "csrf", csrf);
    kn_buf_append_str(&b, "<button class=\"no\" type=\"submit\" style=\"margin-top:12px;font-size:13px\">Not you? Sign out</button></form>"
                          "<div class=\"f\">You can disconnect apps at any time in Settings &gt; Connected apps.</div>");
    app_links(&b, c->client);
    page_close(&b);
    page_send(c->req, 200, &b);
    page_ctx_free(c);
}

static void consent_state(kn_db_result *res, void *ud) {
    PageCtx *c = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    consent_render(c, o);
    cJSON_Delete(o);
}

static void consent_client(kn_db_result *res, void *ud) {
    PageCtx *c = ud;
    c->client = kn_db_ok(res) ? acct_db_json(res) : NULL;
    const char *err = !kn_db_ok(res) ? "Server error." : authz_client_error(c->client, &c->a, &c->scopes);
    if (err) {
        /* RFC 6749 4.1.2.1: an unverified redirect is never followed; the page says why */
        page_message(c->req, 400, "Can't connect this app", err);
        return page_ctx_free(c);
    }
    const int64_t min = kn_json_int(c->client, "min_level", 0);
    if (c->claims && min > 0 && kn_json_int(c->claims, "level", 0) < min) {
        char msg[64];
        snprintf(msg, sizeof msg, "Requires level %lld", (long long) min);
        page_message(c->req, 403, "Can't connect this app", msg);
        return page_ctx_free(c);
    }
    if (!c->claims) return consent_render(c, NULL);
    char acc[24];
    kn_db_call(O.b->db, &P_OA_CONSENT_STATE, KN_DB_ARGS(acct_itoa(acc, kn_json_int(c->claims, "acc", 0)), c->a.client_id), consent_state, c);
}

static void consent_page(kn_http_req *req, const AuthzReq *a, cJSON *claims, const char *error) {
    PageCtx *c = calloc(1, sizeof *c);
    if (!c) {
        cJSON_Delete(claims);
        return page_message(req, 503, "Login with Box Office", "Server busy, try again.");
    }
    c->req = req;
    c->a = *a;
    c->claims = claims;
    if (error) copy_str(c->error, sizeof c->error, error);
    const char *err = authz_shape_error(&c->a, &c->scopes);
    if (err) {
        page_message(req, 400, "Can't connect this app", err);
        return page_ctx_free(c);
    }
    kn_db_call(O.b->db, &P_OA_CLIENT, KN_DB_ARGS(c->a.client_id), consent_client, c);
}

static void h_authorize_page(kn_http_req *req, void *ud) {
    (void) ud;
    if (!O.b->db) return page_message(req, 503, "Login with Box Office", "Unavailable.");
    AuthzReq a;
    authz_from_query(req, &a);
    consent_page(req, &a, cookie_claims(req), NULL);
}

/* ---- POST /oauth/authorize/signin and /oauth/approve/:id/signin: the page's sign-in */

typedef struct {
    kn_http_req *req;
    AuthzReq a;
    char action_id[64];
    char pass[256];
    char name[64], code[16];
} SigninCtx;

static void set_cookie(kn_http_req *req, int64_t account, const char *name, int level) {
    cJSON *cl = cJSON_CreateObject();
    cJSON_AddStringToObject(cl, "use", "oauth-consent");
    cJSON_AddNumberToObject(cl, "acc", (double) account);
    cJSON_AddStringToObject(cl, "name", name);
    cJSON_AddNumberToObject(cl, "level", level);
    char *tok = kn_signed_token_create(O.cookie_key, cl, O.cookie_ttl);
    cJSON_Delete(cl);
    if (!tok) return;
    char hdr[1400];
    snprintf(hdr, sizeof hdr, COOKIE "=%s; Path=/oauth; Max-Age=%lld; HttpOnly; SameSite=Lax%s", tok, (long long) O.cookie_ttl,
             O.secure_cookie ? "; Secure" : "");
    kn_http_set_header(req, "Set-Cookie", hdr);
    free(tok);
}

static void signed_in(void *ud, const char *err, const cJSON *acc) {
    SigninCtx *c = ud;
    memset(c->pass, 0, sizeof c->pass);
    const int64_t id = !err && acc ? kn_json_int(acc, "id", 0) : 0;
    if (id <= 0) {
        const char *why = err && !strcmp(err, ACCT_ERR_2FA) ? "This account uses 2FA: enter its code too." : err ? err : "Invalid login.";
        if (c->action_id[0]) {
            kn_buf b = {0};
            char act[128];
            snprintf(act, sizeof act, "/oauth/approve/%s/signin", c->action_id);
            page_open(&b, "Approve an app action");
            signin_form(&b, act, NULL, NULL, why);
            page_close(&b);
            page_send(c->req, 401, &b);
        } else {
            consent_page(c->req, &c->a, NULL, why);
        }
        free(c);
        return;
    }
    const char *session = kn_json_str(acc, "token", NULL);
    if (session && *session) acct_revoke_token(session); /* the page keeps its own cookie, not a game session */
    set_cookie(c->req, id, kn_json_str(acc, "username", ""), acc_level(acc));
    /* back to the page with a GET (the cookie is read there) */
    kn_buf loc = {0};
    if (c->action_id[0]) {
        kn_buf_append_str(&loc, "/oauth/approve/");
        url_append(&loc, c->action_id);
    } else {
        kn_buf_append_str(&loc, "/oauth/authorize?response_type=code&client_id=");
        url_append(&loc, c->a.client_id);
        kn_buf_append_str(&loc, "&redirect_uri=");
        url_append(&loc, c->a.redirect_uri);
        kn_buf_append_str(&loc, "&scope=");
        url_append(&loc, c->a.scope);
        kn_buf_append_str(&loc, "&state=");
        url_append(&loc, c->a.state);
        kn_buf_append_str(&loc, "&code_challenge=");
        url_append(&loc, c->a.challenge);
        kn_buf_append_str(&loc, "&code_challenge_method=");
        url_append(&loc, c->a.method);
    }
    kn_buf_append_byte(&loc, 0);
    kn_http_set_header(c->req, "Location", (const char *) loc.data);
    kn_http_set_header(c->req, "Cache-Control", "no-store");
    kn_http_respond(c->req, 303, "text/plain", "", 0);
    kn_buf_free(&loc);
    free(c);
}

static void signin_route(kn_http_req *req, const char *action_id) {
    if (limited(req, "signin")) return page_message(req, 429, "Login with Box Office", "Too many attempts. Wait a minute and try again.");
    SigninCtx *c = calloc(1, sizeof *c);
    if (!c) return page_message(req, 503, "Login with Box Office", "Server busy, try again.");
    c->req = req;
    if (action_id) copy_str(c->action_id, sizeof c->action_id, action_id);
    else authz_from_body(req, NULL, &c->a);
    body_get(req, NULL, "username", c->name, sizeof c->name);
    body_get(req, NULL, "password", c->pass, sizeof c->pass);
    body_get(req, NULL, "code", c->code, sizeof c->code);
    if (!c->name[0] || !c->pass[0]) return signed_in(c, "Invalid login.", NULL);
    char ip[64];
    copy_str(ip, sizeof ip, req->remote_addr);
    AcctAuthReq rq = {.action = 1, .name = c->name, .pass = c->pass, .ip = ip, .origin = "http", .twofa = c->code[0] ? c->code : NULL};
    acct_auth(&rq, signed_in, c);
}

static void h_authorize_signin(kn_http_req *req, void *ud) {
    (void) ud;
    signin_route(req, NULL);
}

/* ---- POST /oauth/authorize/decide: allow (the code, redirect) or deny (access_denied, redirect) */

typedef struct {
    kn_http_req *req;
    AuthzReq a;
    cJSON *claims;
} DecidePage;

static void page_redirect(kn_http_req *req, const char *url) {
    kn_http_set_header(req, "Location", url);
    kn_http_set_header(req, "Cache-Control", "no-store");
    kn_http_respond(req, 303, "text/plain", "", 0);
}

static void decide_page_done(void *ud, const char *redirect, const char *error) {
    DecidePage *d = ud;
    if (redirect) {
        page_redirect(d->req, redirect);
        cJSON_Delete(d->claims);
    } else {
        consent_page(d->req, &d->a, d->claims, error); /* again, with the reason */
    }
    free(d);
}

static void deny_checked(kn_db_result *res, void *ud) {
    DecidePage *d = ud;
    cJSON *client = kn_db_ok(res) ? acct_db_json(res) : NULL;
    ScopeSet s;
    const char *err = authz_shape_error(&d->a, &s);
    if (!err) err = authz_client_error(client, &d->a, &s);
    cJSON_Delete(client);
    if (err) {
        page_message(d->req, 400, "Can't connect this app", err);
    } else {
        char *url = redirect_with(d->a.redirect_uri, "error", "access_denied", d->a.state);
        page_redirect(d->req, url);
        free(url);
    }
    cJSON_Delete(d->claims);
    free(d);
}

static void h_authorize_decide(kn_http_req *req, void *ud) {
    (void) ud;
    if (limited(req, "authorize")) return page_message(req, 429, "Login with Box Office", "Slow down.");
    cJSON *claims = cookie_claims(req);
    DecidePage *d = calloc(1, sizeof *d);
    if (!d) {
        cJSON_Delete(claims);
        return page_message(req, 503, "Login with Box Office", "Server busy, try again.");
    }
    d->req = req;
    authz_from_body(req, NULL, &d->a);
    d->claims = claims;
    if (!claims || !csrf_ok(req, claims)) { /* signed out meanwhile, or not this page's form */
        cJSON_Delete(claims);
        d->claims = NULL;
        consent_page(req, &d->a, NULL, "Sign in again to continue.");
        free(d);
        return;
    }
    char decision[16];
    body_get(req, NULL, "decision", decision, sizeof decision);
    if (strcmp(decision, "allow")) { /* deny: back to a verified redirect only */
        kn_db_call(O.b->db, &P_OA_CLIENT, KN_DB_ARGS(d->a.client_id), deny_checked, d);
        return;
    }
    decide_start(&d->a, kn_json_int(claims, "acc", 0), (int) kn_json_int(claims, "level", 0), decide_page_done, d);
}

/* POST /oauth/signout: the cookie goes; back to the sign-in */
static void h_signout(kn_http_req *req, void *ud) {
    (void) ud;
    AuthzReq a;
    authz_from_body(req, NULL, &a);
    kn_http_set_header(req, "Set-Cookie", COOKIE "=; Path=/oauth; Max-Age=0; HttpOnly; SameSite=Lax");
    consent_page(req, &a, NULL, NULL);
}

/* ---- GET /oauth/approve/:id, POST /oauth/approve/:id (approve | deny with a 2FA code), POST .../signin */

static const char *action_line(const cJSON *row, char *out, size_t cap) {
    const char *kind = kn_json_str(row, "kind", "");
    cJSON *pp = cJSON_Parse(kn_json_str(row, "params", "{}"));
    if (!strcmp(kind, "gift.send"))
        snprintf(out, cap, "Send %lld KR to %s%s%s", (long long) kn_json_int(pp, "amount", 0), kn_json_str(pp, "to", ""),
                 *kn_json_str(pp, "message", "") ? ", with the message: " : "", kn_json_str(pp, "message", ""));
    else if (!strcmp(kind, "follow.add")) snprintf(out, cap, "Follow %s", kn_json_str(pp, "name", ""));
    else if (!strcmp(kind, "follow.remove")) snprintf(out, cap, "Unfollow %s", kn_json_str(pp, "name", ""));
    else snprintf(out, cap, "%s", kind);
    cJSON_Delete(pp);
    return out;
}

typedef struct {
    kn_http_req *req;
    cJSON *claims;
    char id[64];
} ApprovePage;

static void approve_render(void *ud, int status, const char *code, const char *msg, const cJSON *row) {
    ApprovePage *p = ud;
    (void) code;
    kn_buf b = {0};
    page_open(&b, "Approve an app action");
    if (!row) {
        kn_buf_append_str(&b, "<div class=\"e\">");
        esc(&b, msg ? msg : "This request is unknown.");
        kn_buf_append_str(&b, "</div>");
        if (status == 400) { /* a wrong code: the form again */
            char again[128];
            snprintf(again, sizeof again, "/oauth/approve/%s", p->id);
            kn_buf_append_str(&b, "<p class=\"m\"><a href=\"");
            esc(&b, again);
            kn_buf_append_str(&b, "\">Try again</a></p>");
        }
        page_close(&b);
        page_send(p->req, status, &b);
        cJSON_Delete(p->claims);
        free(p);
        return;
    }
    char line[512];
    kn_buf_append_str(&b, "<p class=\"m\"><b>");
    esc(&b, kn_json_str(row, "app_name", ""));
    kn_buf_append_str(&b, "</b> wants to do this on your Box Office account <b>");
    esc(&b, kn_json_str(p->claims, "name", ""));
    kn_buf_append_str(&b, "</b>:</p><p><b>");
    esc(&b, action_line(row, line, sizeof line));
    kn_buf_append_str(&b, "</b></p>");
    const char *st = kn_json_str(row, "status", "");
    if (!strcmp(st, "pending")) {
        char csrf[65];
        csrf_of(p->claims, csrf);
        kn_buf_append_str(&b, "<p class=\"m\">Only approve this if you asked the app to do it. It runs once, exactly as shown, and the app can't change it.</p><form method=\"post\">");
        hidden(&b, "csrf", csrf);
        kn_buf_append_str(&b, "<label>2FA code<input type=\"text\" name=\"twofa_code\" inputmode=\"numeric\" autocomplete=\"one-time-code\"></label>"
                              "<button class=\"ok\" type=\"submit\" name=\"decision\" value=\"approve\">Approve</button>"
                              "<button class=\"no\" type=\"submit\" name=\"decision\" value=\"deny\">Deny</button></form>");
    } else {
        const char *text = !strcmp(st, "approved") ? "Done. You can go back to the app."
                         : !strcmp(st, "denied")   ? "Request denied. You can go back to the app."
                         : !strcmp(st, "expired")  ? "This request expired. Ask the app to send it again."
                                                   : NULL;
        kn_buf_append_str(&b, "<p class=\"m\">");
        if (text) esc(&b, text);
        else {
            kn_buf_append_str(&b, "It didn't go through: ");
            esc(&b, kn_json_str(row, "result", ""));
        }
        kn_buf_append_str(&b, "</p>");
    }
    page_close(&b);
    page_send(p->req, 200, &b);
    cJSON_Delete(p->claims);
    free(p);
}

static void approve_page(kn_http_req *req, int decide) {
    const char *id = kn_http_param(req, "id");
    cJSON *claims = cookie_claims(req);
    if (!claims) {
        kn_buf b = {0};
        char act[128];
        snprintf(act, sizeof act, "/oauth/approve/%s/signin", id ? id : "");
        page_open(&b, "Approve an app action");
        signin_form(&b, act, NULL, NULL, NULL);
        page_close(&b);
        return page_send(req, 200, &b);
    }
    if (decide && !csrf_ok(req, claims)) {
        cJSON_Delete(claims);
        return page_message(req, 403, "Approve an app action", "This form expired. Open the link again.");
    }
    if (decide && limited(req, "approve")) {
        cJSON_Delete(claims);
        return page_message(req, 429, "Approve an app action", "Too many attempts. Wait a minute and try again.");
    }
    ApprovePage *p = calloc(1, sizeof *p);
    ActCtx *c = calloc(1, sizeof *c);
    if (!p || !c) {
        free(p);
        free(c);
        cJSON_Delete(claims);
        return page_message(req, 503, "Approve an app action", "Server busy, try again.");
    }
    p->req = req;
    p->claims = claims;
    copy_str(p->id, sizeof p->id, id);
    c->req = req;
    c->account = kn_json_int(claims, "acc", 0);
    copy_str(c->id, sizeof c->id, id);
    c->reply = approve_render;
    c->reply_ud = p;
    c->mode = 0;
    if (decide) {
        char decision[16];
        body_get(req, NULL, "decision", decision, sizeof decision);
        body_get(req, NULL, "twofa_code", c->twofa, sizeof c->twofa);
        c->mode = !strcmp(decision, "approve") ? 1 : 2;
    }
    act_start(c);
}

static void h_approve_get(kn_http_req *req, void *ud) { (void) ud; approve_page(req, 0); }
static void h_approve_post(kn_http_req *req, void *ud) { (void) ud; approve_page(req, 1); }
static void h_approve_signin(kn_http_req *req, void *ud) {
    (void) ud;
    signin_route(req, kn_http_param(req, "id"));
}

/* ================================================================== staff: client registration */

typedef struct {
    kn_http_req *req;
    char client_id[48], secret[96];
} ClientNewCtx;

static void client_new_done(kn_db_result *res, void *ud) {
    ClientNewCtx *c = ud;
    if (!kn_db_ok(res)) {
        kn_http_respond_error(c->req, 400, acct_db_error(res, "oauth_client_new"));
    } else {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "client_id", c->client_id);
        if (c->secret[0]) cJSON_AddStringToObject(o, "client_secret", c->secret); /* shown once; only its hash is kept */
        kn_http_set_header(c->req, "Cache-Control", "no-store");
        kn_http_respond_json(c->req, 201, o);
        cJSON_Delete(o);
    }
    memset(c, 0, sizeof *c);
    free(c);
}

static int https_or_empty(const char *u) { return !u || !*u || !strncmp(u, "https://", 8); }

static void h_admin_client_new(kn_http_req *req, void *ud) {
    (void) ud;
    if (!mm_internal_authorized(req, "oauth clients")) return;
    cJSON *b = kn_http_body_json(req);
    const char *name = kn_json_str(b, "name", NULL);
    const cJSON *uris = cJSON_GetObjectItem(b, "redirect_uris"), *sc = cJSON_GetObjectItem(b, "scopes");
    char scopes[1024] = "";
    int bad = !name || !*name || strlen(name) > 64 || !cJSON_IsArray(uris) || cJSON_GetArraySize(uris) < 1 || !cJSON_IsArray(sc);
    const cJSON *u;
    cJSON_ArrayForEach(u, uris) bad |= !cJSON_IsString(u) || strlen(u->valuestring) > 500 || strchr(u->valuestring, '#') ||
                                       (strncmp(u->valuestring, "https://", 8) && strncmp(u->valuestring, "http://127.0.0.1", 16) &&
                                        strncmp(u->valuestring, "http://localhost", 16));
    cJSON_ArrayForEach(u, sc) if (cJSON_IsString(u)) snprintf(scopes + strlen(scopes), sizeof scopes - strlen(scopes), "%s ", u->valuestring);
    ScopeSet ss;
    bad |= scopes_parse(scopes, &ss) <= 0;
    static const char *const links[] = {"icon_url", "homepage_url", "privacy_url", "tos_url"};
    for (int i = 0; i < 4; i++) bad |= !https_or_empty(kn_json_str(b, links[i], NULL));
    if (bad) {
        cJSON_Delete(b);
        return kn_http_respond_error(req, 400, "name, redirect_uris (https, or http://127.0.0.1 / localhost) and served scopes are required");
    }
    ClientNewCtx *c = calloc(1, sizeof *c);
    if (!c || kn_random_hex(c->client_id, sizeof c->client_id, 12) != 0 ||
        (cJSON_IsTrue(cJSON_GetObjectItem(b, "confidential")) && new_token(c->secret, sizeof c->secret, "kbo_cs_") != 0)) {
        free(c);
        cJSON_Delete(b);
        return kn_http_respond_error(req, 500, "server error");
    }
    c->req = req;
    scopes_text(&ss, scopes, sizeof scopes);
    char *uris_text = cJSON_PrintUnformatted(uris);
    char lvl[24];
    acct_itoa(lvl, kn_json_int(b, "min_level", 0));
    kn_db_call(O.b->db, &P_OA_CLIENT_NEW,
               KN_DB_ARGS(c->client_id, c->secret, name, kn_json_str(b, "developer", ""), kn_json_str(b, "icon_url", ""),
                          kn_json_str(b, "homepage_url", ""), kn_json_str(b, "privacy_url", ""), kn_json_str(b, "tos_url", ""),
                          uris_text ? uris_text : "[]", scopes, lvl),
               client_new_done, c);
    free(uris_text);
    cJSON_Delete(b);
}

static void admin_list_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    if (!kn_db_ok(res)) return kn_http_respond_error(req, 500, acct_db_error(res, "oauth_clients"));
    cJSON *rows = kn_db_result_to_json(res);
    kn_http_respond_json(req, 200, rows);
    cJSON_Delete(rows);
}

static void h_admin_clients(kn_http_req *req, void *ud) {
    (void) ud;
    if (!mm_internal_authorized(req, "oauth clients")) return;
    kn_db_query_sql(O.b->db, SQL(.any = "SELECT client_id, name, developer, redirect_uris AS redirect_uris__t, scopes, min_level AS min_level__i,"
                                        " disabled AS disabled__b, (secret_hash IS NOT NULL) AS confidential__b,"
                                        " (SELECT count(*) FROM oauth_grants g WHERE g.client_id = oauth_clients.client_id) AS grants__i"
                                        " FROM oauth_clients ORDER BY created_at"),
                    0, NULL, admin_list_done, req);
}

static void admin_disable_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    if (!kn_db_ok(res)) return kn_http_respond_error(req, 500, acct_db_error(res, "oauth_client_disable"));
    kn_http_respond_json_str(req, 200, kn_db_affected(res) > 0 ? "{\"disabled\":true}" : "{\"disabled\":false}");
}

static void h_admin_client_disable(kn_http_req *req, void *ud) {
    (void) ud;
    if (!mm_internal_authorized(req, "oauth clients")) return;
    const char *id = kn_http_param(req, "id");
    kn_db_query_sql(O.b->db, SQL(.any = "UPDATE oauth_clients SET disabled = {true} WHERE client_id = $1"), KN_DB_ARGS(id ? id : ""),
                    admin_disable_done, req);
}

/* ================================================================== module */

static void h_off(kn_http_req *req, void *ud) {
    (void) ud;
    v10_reply_error(req, 503, "OAUTH_UNAVAILABLE", "Login with Box Office is unavailable.");
}

int oauth_init(Brain *b) {
    memset(&O, 0, sizeof O);
    O.b = b;
    const kn_config *cfg = b->cfg;
    O.enabled = kn_config_bool(cfg, "oauth.enabled", 1) && b->db;
    O.access_ttl = kn_config_int(cfg, "oauth.access_ttl_s", 3600);
    O.refresh_ttl = kn_config_int(cfg, "oauth.refresh_ttl_s", 30 * 86400);
    O.code_ttl = kn_config_int(cfg, "oauth.code_ttl_s", 600);
    O.action_ttl = kn_config_int(cfg, "oauth.action_ttl_s", 900);
    O.cookie_ttl = kn_config_int(cfg, "oauth.consent_ttl_s", 900);
    O.limit = (int) kn_config_int(cfg, "oauth.rate_per_min", 30);
    if (O.access_ttl < 60) O.access_ttl = 60;
    if (O.code_ttl < 30) O.code_ttl = 30;
    if (O.limit < 1) O.limit = 30;
    copy_str(O.public_url, sizeof O.public_url, mm_settings()->public_url);
    O.secure_cookie = !strncmp(O.public_url, "https://", 8);
    static const char label[] = "kn-oauth-consent-cookie-v1";
    if (kn_hmac_sha256_hex(mm_settings()->join_secret, label, sizeof label - 1, O.cookie_key) != 0) O.enabled = 0;
    O.limiter = auth_limiter_new(O.limit, 60000);
    if (!b->http) return 0;
    if (!O.enabled) {
        static const char *const off[][2] = {{"GET", "/oauth/authorize/info"}, {"POST", "/oauth/authorize"}, {"GET", "/account/apps"}};
        for (size_t i = 0; i < 3; i++) kn_http_route(b->http, off[i][0], off[i][1], h_off, NULL);
        KN_INFO(MOD, "Login with Box Office off (%s)", b->db ? "oauth.enabled false" : "no database");
        return 0;
    }
    kn_http_route(b->http, "GET", "/oauth/authorize/info", h_authorize_info, NULL);
    kn_http_route(b->http, "POST", "/oauth/authorize", h_authorize_post, NULL);
    kn_http_route(b->http, "GET", "/oauth/authorize", h_authorize_page, NULL);
    kn_http_route(b->http, "POST", "/oauth/authorize/signin", h_authorize_signin, NULL);
    kn_http_route(b->http, "POST", "/oauth/authorize/decide", h_authorize_decide, NULL);
    kn_http_route(b->http, "POST", "/oauth/signout", h_signout, NULL);
    kn_http_route(b->http, "POST", "/oauth/token", h_token, NULL);
    kn_http_route(b->http, "POST", "/oauth/revoke", h_revoke, NULL);
    kn_http_route(b->http, "GET", "/oauth/userinfo", h_userinfo, NULL);
    kn_http_route(b->http, "GET", "/oauth/api/stats", h_api_stats, NULL);
    kn_http_route(b->http, "GET", "/oauth/api/inventory", h_api_inventory, NULL);
    kn_http_route(b->http, "GET", "/oauth/api/friends", h_api_friends, NULL);
    kn_http_route(b->http, "POST", "/oauth/api/actions", h_api_action_new, NULL);
    kn_http_route(b->http, "GET", "/oauth/api/actions/:id", h_api_action_get, NULL);
    kn_http_route(b->http, "GET", "/oauth/actions/:id", h_action_info, NULL);
    kn_http_route(b->http, "POST", "/oauth/actions/:id/approve", h_action_approve, NULL);
    kn_http_route(b->http, "POST", "/oauth/actions/:id/deny", h_action_deny, NULL);
    kn_http_route(b->http, "GET", "/oauth/approve/:id", h_approve_get, NULL);
    kn_http_route(b->http, "POST", "/oauth/approve/:id", h_approve_post, NULL);
    kn_http_route(b->http, "POST", "/oauth/approve/:id/signin", h_approve_signin, NULL);
    kn_http_route(b->http, "GET", "/account/apps", h_apps, NULL);
    kn_http_route(b->http, "DELETE", "/account/apps/:id", h_app_revoke, NULL);
    kn_http_route(b->http, "GET", "/account/apps/:id/activity", h_app_activity, NULL);
    kn_http_route(b->http, "POST", "/mm/admin/oauth/clients", h_admin_client_new, NULL);
    kn_http_route(b->http, "GET", "/mm/admin/oauth/clients", h_admin_clients, NULL);
    kn_http_route(b->http, "POST", "/mm/admin/oauth/clients/:id/disable", h_admin_client_disable, NULL);
    KN_INFO(MOD, "Login with Box Office: access %llds, refresh %llds, code %llds, actions %llds", (long long) O.access_ttl,
            (long long) O.refresh_ttl, (long long) O.code_ttl, (long long) O.action_ttl);
    return 0;
}

void oauth_shutdown(Brain *b) {
    (void) b;
    auth_limiter_free(O.limiter);
    O.limiter = NULL;
    O.b = NULL;
}
