/*
 * services/brain/discord.c - Krunker v10.0.7's Discord link for Box Office accounts.
 *
 * The client's discord API (deob-10.0.7.js:26499-26523) and the Settings > Account Discord row (:97250-97480):
 *   GET  /discord/status              -> {configured, linked, id, username, globalName, avatar, inGuild, guildUrl}
 *   POST /discord/link {returnTo?}    -> {url} (the Discord authorize page) | DISCORD_NOT_CONFIGURED (:97352-97354)
 *                                        | DISCORD_DELETION_PENDING (an account being deleted can't change its link)
 *   GET  /discord/callback?code&state  the browser comes back here: the link, then returnTo?discord_link=<result>
 *                                        (linked | cancelled | expired | in_use | unavailable | failed, :216792) or a page
 *   ["dlnkR", result]                  v10's DISCORD_LINK_RESULT (the game socket's answer to the DISCORD_LINK_WAIT that
 *                                        onLinkStart sends, :219136-219137, handled at :216792-216796) on the account's
 *                                        social sockets, for every callback whose account is known, and "expired" when a
 *                                        started link's state runs out unused (STATE_S)
 *   POST /discord/join                -> {joined} (false = already a member) | DISCORD_NEEDS_RELINK | DISCORD_JOIN_FAILED
 *                                        (:97366-97368; PUT /guilds/:guild/members/:user with the bot token)
 *   POST /discord/unlink              -> {unlinked} | DISCORD_DELETION_PENDING (v10.0.7's new state, :97384)
 *
 * The Discord app's credentials are config (accounts.discord.*): client_id + client_secret turn linking on; bot_token +
 * guild_id turn on Join Server (guild_url is the Open Server link). Without them the row shows v10's unavailable
 * state (configured false). The link keeps the user's tokens sealed (kn_seal, accounts.secrets_key) for the
 * guilds.join call; unlinking revokes them at Discord. Migration 0366_account_links (discord_links, discord_link_states).
 */
#include "discord.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "acct_internal.h"
#include "brain_internal.h"
#include "build_info.h"
#include "db_procs.h"
#include "v10_api.h"
#include "knsvc/buf.h"
#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/http.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"

#ifdef KN_HAVE_CURL
#include <curl/curl.h>
#endif

#define MOD "discord"
#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))
#define STATE_S 600 /* a started link lives 10 minutes */
#define MAX_BODY (64 * 1024)
#define SEAL_ACCESS "discord.access"
#define SEAL_REFRESH "discord.refresh"
#define DELETION_PENDING "DISCORD_DELETION_PENDING"
#define MAX_ORIGINS 8

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }
static void copy_str(char *dst, const size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

static struct {
    Brain *b;
    char client_id[128], client_secret[256], redirect_uri[512];
    char bot_token[256], guild_id[64], guild_url[256];
    char authorize_url[256], api_url[256];
    /* accounts.discord.state_prefix: put before the state sent to Discord, taken off at the callback. A
     * redirect_uri that is another site's registered callback (the app's list is portal-only) can then be
     * shared: that host's edge sends a state with this prefix on to /discord/callback (OWNED_HOST.md). */
    char state_prefix[32];
    char origins[MAX_ORIGINS][256]; /* returnTo must start with one of them */
    int norigins;
    char seal[256];
    int timeout_ms;
} D;

/* the started links, one per account (a new link replaces the account's earlier states): STATE_S after the start
 * a still unused state is taken and the account told "expired" */
#define MAX_EXPIRY 256
static struct Expiry {
    int64_t account;
    char nonce[64];
    kn_timer_id timer;
} g_expiry[MAX_EXPIRY];

static atomic_int g_live;
static atomic_int g_cancel;

static int configured(void) {
#ifdef KN_HAVE_CURL
    return D.client_id[0] && D.client_secret[0] && D.redirect_uri[0] && D.seal[0];
#else
    return 0;
#endif
}

static int join_configured(void) { return configured() && D.bot_token[0] && D.guild_id[0]; }

/* ================================================================== procs */

/* (account) -> the link | null */
static kn_db_result *run_status(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT discord_id, username, global_name, avatar, in_guild AS in_guild__b FROM discord_links"
                                        " WHERE account_id = {int:$1}"),
                         KN_DB_ARGS(P(0)));
    return dbx_json(&x, "discord_status", row);
}
static const kn_proc P_DC_STATUS = {"discord_status", NULL, run_status};

static int deletion_pending(Dbx *x, const char *account) {
    return dbx_exists(x, SQL(.any = "SELECT 1 FROM accounts WHERE id = {int:$1} AND delete_requested_at IS NOT NULL"), KN_DB_ARGS(account));
}

/* (account, nonce, return_to) -> {}: the account's earlier states go; refused while the account is being deleted */
static kn_db_result *run_state_new(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (deletion_pending(&x, P(0))) {
        dbx_refuse(&x, DELETION_PENDING);
        return dbx_finish(&x, NULL);
    }
    dbx_exec(&x, SQL(.any = "DELETE FROM discord_link_states WHERE account_id = {int:$1} OR created_at <= {ago:600}"), KN_DB_ARGS(P(0)));
    dbx_exec(&x, SQL(.any = "INSERT INTO discord_link_states (state_hash, account_id, return_to) VALUES ({hash:$2}, {int:$1}, nullif($3, ''))"),
             KN_DB_ARGS(P(0), P(1), P(2) ? P(2) : ""));
    return dbx_json(&x, "discord_state", cJSON_CreateObject());
}
static const kn_proc P_DC_STATE_NEW = {"discord_state_new", NULL, run_state_new};

/* (nonce) -> {account, return_to, fresh} | null: single use */
static kn_db_result *run_state_take(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT account_id AS account__i, return_to, (created_at > {ago:600}) AS fresh__b"
                                        " FROM discord_link_states WHERE state_hash = {hash:$1}"),
                         KN_DB_ARGS(P(0)));
    dbx_exec(&x, SQL(.any = "DELETE FROM discord_link_states WHERE state_hash = {hash:$1}"), KN_DB_ARGS(P(0)));
    return dbx_json(&x, "discord_state_take", row);
}
static const kn_proc P_DC_STATE_TAKE = {"discord_state_take", NULL, run_state_take};

/* (account, discord id, username, global name, avatar, token_enc, refresh_enc, expires_in) -> {}:
 * refused "in_use" when another account has that Discord user, "deletion_pending" while this one is being deleted */
static kn_db_result *run_link(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (deletion_pending(&x, P(0))) {
        dbx_refuse(&x, "deletion_pending");
        return dbx_finish(&x, NULL);
    }
    if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM discord_links WHERE discord_id = $2 AND account_id <> {int:$1}"), KN_DB_ARGS(P(0), P(1)))) {
        dbx_refuse(&x, "in_use");
        return dbx_finish(&x, NULL);
    }
    dbx_exec(&x, SQL(.any = "DELETE FROM discord_links WHERE account_id = {int:$1}"), KN_DB_ARGS(P(0)));
    dbx_exec(&x, SQL(.any = "INSERT INTO discord_links (account_id, discord_id, username, global_name, avatar, token_enc, refresh_enc,"
                            " token_expires_at) VALUES ({int:$1}, $2, $3, nullif($4, ''), nullif($5, ''), $6, nullif($7, ''), {later:$8})"),
             KN_DB_ARGS(P(0), P(1), P(2), P(3) ? P(3) : "", P(4) ? P(4) : "", P(5), P(6) ? P(6) : "", P(7) ? P(7) : "0"));
    return dbx_json(&x, "discord_link", cJSON_CreateObject());
}
static const kn_proc P_DC_LINK = {"discord_link", NULL, run_link};

/* (account) -> {discord_id, token_enc, refresh_enc, expired} | null */
static kn_db_result *run_tokens(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT discord_id, token_enc, refresh_enc,"
                                        " (token_expires_at IS NULL OR token_expires_at <= {later:60}) AS expired__b"
                                        " FROM discord_links WHERE account_id = {int:$1}"),
                         KN_DB_ARGS(P(0)));
    return dbx_json(&x, "discord_tokens", row);
}
static const kn_proc P_DC_TOKENS = {"discord_tokens", NULL, run_tokens};

/* (account, in_guild 0|1, token_enc ('' = keep), refresh_enc, expires_in) -> {} */
static kn_db_result *run_joined(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (P(2) && *P(2))
        dbx_exec(&x, SQL(.any = "UPDATE discord_links SET token_enc = $2, refresh_enc = nullif($3, ''), token_expires_at = {later:$4}"
                                " WHERE account_id = {int:$1}"),
                 KN_DB_ARGS(P(0), P(2), P(3) ? P(3) : "", P(4) ? P(4) : "0"));
    if (PI(1)) dbx_exec(&x, SQL(.any = "UPDATE discord_links SET in_guild = {true} WHERE account_id = {int:$1}"), KN_DB_ARGS(P(0)));
    return dbx_json(&x, "discord_joined", cJSON_CreateObject());
}
static const kn_proc P_DC_JOINED = {"discord_joined", NULL, run_joined};

/* (account) -> {unlinked, token_enc}: refused DISCORD_DELETION_PENDING while the account is being deleted */
static kn_db_result *run_unlink(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (deletion_pending(&x, P(0))) {
        dbx_refuse(&x, DELETION_PENDING);
        return dbx_finish(&x, NULL);
    }
    char *tok = dbx_text(&x, SQL(.any = "SELECT token_enc FROM discord_links WHERE account_id = {int:$1}"), KN_DB_ARGS(P(0)));
    const int64_t hit = dbx_exec(&x, SQL(.any = "DELETE FROM discord_links WHERE account_id = {int:$1}"), KN_DB_ARGS(P(0)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "unlinked", hit > 0);
    if (tok) cJSON_AddStringToObject(o, "token_enc", tok);
    free(tok);
    return dbx_json(&x, "discord_unlink", o);
}
static const kn_proc P_DC_UNLINK = {"discord_unlink", NULL, run_unlink};

/* ================================================================== Discord's API (worker threads) */

typedef struct {
    char *data;
    size_t len;
} Body;

#ifdef KN_HAVE_CURL
static size_t on_body(char *ptr, size_t size, size_t nmemb, void *ud) {
    Body *b = ud;
    const size_t len = size * nmemb;
    if (b->len + len > MAX_BODY) return 0;
    char *grown = realloc(b->data, b->len + len + 1);
    if (!grown) return 0;
    memcpy(grown + b->len, ptr, len);
    b->len += len;
    grown[b->len] = 0;
    b->data = grown;
    return len;
}

static int on_progress(void *ud, curl_off_t a, curl_off_t b, curl_off_t c, curl_off_t d) {
    (void) ud; (void) a; (void) b; (void) c; (void) d;
    return atomic_load(&g_cancel) ? 1 : 0;
}

/* method NULL = GET (or POST when body is set); auth: a full Authorization value or NULL; json: the body is JSON */
static long dc_call(const char *method, const char *url, const char *auth, const char *body, int json, Body *out) {
    CURL *c = curl_easy_init();
    if (!c) return 0;
    struct curl_slist *h = NULL;
    char hdr[600];
    if (auth) {
        snprintf(hdr, sizeof hdr, "Authorization: %s", auth);
        h = curl_slist_append(h, hdr);
    }
    if (body) h = curl_slist_append(h, json ? "Content-Type: application/json" : "Content-Type: application/x-www-form-urlencoded");
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    if (method) curl_easy_setopt(c, CURLOPT_CUSTOMREQUEST, method);
    if (body) curl_easy_setopt(c, CURLOPT_POSTFIELDS, body);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, on_body);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "DiscordBot (https://krunk.io, 1) kn_brain");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long) D.timeout_ms);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, on_progress);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    long code = 0;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return code;
}

/* POST /oauth2/token: grant "authorization_code" (code) or "refresh_token"; -> the token JSON | NULL */
static cJSON *dc_token(const char *grant, const char *value) {
    char *cid = curl_easy_escape(NULL, D.client_id, 0), *sec = curl_easy_escape(NULL, D.client_secret, 0);
    char *val = curl_easy_escape(NULL, value, 0), *redir = curl_easy_escape(NULL, D.redirect_uri, 0);
    kn_buf form = {0};
    if (!strcmp(grant, "authorization_code"))
        kn_buf_appendf(&form, "client_id=%s&client_secret=%s&grant_type=authorization_code&code=%s&redirect_uri=%s", cid, sec, val, redir);
    else
        kn_buf_appendf(&form, "client_id=%s&client_secret=%s&grant_type=refresh_token&refresh_token=%s", cid, sec, val);
    kn_buf_append_byte(&form, 0);
    curl_free(cid);
    curl_free(sec);
    curl_free(val);
    curl_free(redir);
    char url[320];
    snprintf(url, sizeof url, "%s/oauth2/token", D.api_url);
    Body b = {0};
    const long st = dc_call(NULL, url, NULL, (const char *) form.data, 0, &b);
    memset(form.data, 0, form.len);
    kn_buf_free(&form);
    cJSON *t = st == 200 && b.data ? cJSON_Parse(b.data) : NULL;
    free(b.data);
    if (t && !kn_json_str(t, "access_token", NULL)) {
        cJSON_Delete(t);
        t = NULL;
    }
    return t;
}

static void dc_revoke(const char *token) {
    char *cid = curl_easy_escape(NULL, D.client_id, 0), *sec = curl_easy_escape(NULL, D.client_secret, 0), *tok = curl_easy_escape(NULL, token, 0);
    kn_buf form = {0};
    kn_buf_appendf(&form, "client_id=%s&client_secret=%s&token=%s&token_type_hint=access_token", cid, sec, tok);
    kn_buf_append_byte(&form, 0);
    curl_free(cid);
    curl_free(sec);
    curl_free(tok);
    char url[320];
    snprintf(url, sizeof url, "%s/oauth2/token/revoke", D.api_url);
    Body b = {0};
    dc_call(NULL, url, NULL, (const char *) form.data, 0, &b);
    free(b.data);
    memset(form.data, 0, form.len);
    kn_buf_free(&form);
}
#endif

/* the token JSON -> the sealed access / refresh tokens and the lifetime */
static void seal_tokens(const cJSON *t, char **access, char **refresh, char expires[24]) {
    const char *a = kn_json_str(t, "access_token", ""), *r = kn_json_str(t, "refresh_token", "");
    *access = *a ? kn_seal(D.seal, SEAL_ACCESS, a, strlen(a)) : NULL;
    *refresh = *r ? kn_seal(D.seal, SEAL_REFRESH, r, strlen(r)) : NULL;
    snprintf(expires, 24, "%lld", (long long) kn_json_int(t, "expires_in", 604800));
}

static char *unseal_str(const char *label, const char *sealed) {
    size_t n = 0;
    char *s = sealed && *sealed ? kn_unseal(D.seal, label, sealed, &n) : NULL;
    return s; /* NUL-terminated by kn_unseal */
}

/* a job: work() on a worker thread, done() back on the loop (the job is freed after done) */
typedef struct Job Job;
struct Job {
    void (*work)(Job *);
    void (*done)(Job *);
    kn_http_req *req;
    int64_t account;
    char code[256], return_to[512], nonce[64];
    char *token_enc, *refresh_enc; /* in: the stored ones; out: new ones */
    char expires[24];
    int expired, refreshed;
    char discord_id[64], username[64], global_name[64], avatar[128];
    char result[32]; /* linked | failed | ... ; join: joined | already | relink | failed */
};

static void job_free(Job *j) {
    if (!j) return;
    free(j->token_enc);
    free(j->refresh_enc);
    memset(j, 0, sizeof *j);
    free(j);
}

static void job_done(kn_loop *loop, void *ud) {
    (void) loop;
    Job *j = ud;
    if (j->done) j->done(j);
    job_free(j);
}

static void *job_thread(void *arg) {
    Job *j = arg;
    j->work(j);
    if (kn_loop_post_threadsafe(D.b->loop, job_done, j) != 0) job_free(j);
    atomic_fetch_sub(&g_live, 1);
    return NULL;
}

static int job_start(Job *j) {
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    atomic_fetch_add(&g_live, 1);
    const int ok = pthread_create(&t, &attr, job_thread, j) == 0;
    pthread_attr_destroy(&attr);
    if (!ok) atomic_fetch_sub(&g_live, 1);
    return ok ? 0 : -1;
}

/* ================================================================== the signed-in caller */

typedef void (*SessionFn)(kn_http_req *req, int64_t account, void *ud);
typedef struct {
    kn_http_req *req;
    SessionFn fn;
    void *ud;
} SessionCtx;

static void on_session(void *ud, const char *err, const cJSON *acc) {
    SessionCtx *c = ud;
    const int64_t id = err ? 0 : kn_json_int(acc, "id", 0);
    if (id <= 0) {
        v10_reply_error(c->req, 401, "LOGIN_REQUIRED", "You must be logged in.");
        free(c->ud);
    } else {
        c->fn(c->req, id, c->ud);
    }
    free(c);
}

static void with_session(kn_http_req *req, SessionFn fn, void *ud) {
    char bearer[256]; /* the session: ?token= (the client's convention, v10_account.c caller_token) or a Bearer header */
    if (kn_http_query_get(req, "token", bearer, sizeof bearer) <= 0) snprintf(bearer, sizeof bearer, "%s", kn_http_bearer(req) ? kn_http_bearer(req) : "");
    SessionCtx *c = bearer[0] && D.b->db ? calloc(1, sizeof *c) : NULL;
    if (!c) {
        v10_reply_error(req, 401, "LOGIN_REQUIRED", "You must be logged in.");
        free(ud);
        return;
    }
    *c = (SessionCtx){req, fn, ud};
    cJSON *args = cJSON_CreateObject();
    cJSON_AddStringToObject(args, "token", bearer);
    accounts_internal_call(D.b, "token", args, on_session, c);
    cJSON_Delete(args);
}

/* ================================================================== GET /discord/status */

static void status_done(kn_db_result *res, void *ud) {
    kn_http_req *req = ud;
    if (!kn_db_ok(res)) return v10_reply_error(req, 500, "internal", acct_db_error(res, "discord_status"));
    cJSON *row = acct_db_json(res);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "configured", configured());
    cJSON_AddBoolToObject(d, "linked", row != NULL);
    if (row) {
        cJSON_AddStringToObject(d, "id", kn_json_str(row, "discord_id", ""));
        cJSON_AddStringToObject(d, "username", kn_json_str(row, "username", ""));
        const char *g = kn_json_str(row, "global_name", NULL), *a = kn_json_str(row, "avatar", NULL);
        if (g && *g) cJSON_AddStringToObject(d, "globalName", g);
        else cJSON_AddNullToObject(d, "globalName");
        if (a && *a) cJSON_AddStringToObject(d, "avatar", a);
        else cJSON_AddNullToObject(d, "avatar");
        cJSON_AddBoolToObject(d, "inGuild", cJSON_IsTrue(cJSON_GetObjectItem(row, "in_guild")));
    }
    if (D.guild_url[0]) cJSON_AddStringToObject(d, "guildUrl", D.guild_url);
    else cJSON_AddNullToObject(d, "guildUrl");
    cJSON_AddBoolToObject(d, "canJoin", join_configured());
    cJSON_Delete(row);
    v10_reply_data(req, 200, d);
}

static void status_session(kn_http_req *req, int64_t account, void *ud) {
    (void) ud;
    char id[24];
    kn_db_call(D.b->db, &P_DC_STATUS, KN_DB_ARGS(acct_itoa(id, account)), status_done, req);
}

static void h_status(kn_http_req *req, void *ud) {
    (void) ud;
    with_session(req, status_session, NULL);
}

/* ================================================================== POST /discord/link */

static int return_ok(const char *to) {
    if (!to || !*to || strlen(to) > 500) return 0;
    for (int i = 0; i < D.norigins; i++) {
        const size_t n = strlen(D.origins[i]);
        if (n && !strncmp(to, D.origins[i], n) && (to[n] == 0 || to[n] == '/' || to[n] == '?')) return 1;
    }
    return 0;
}

typedef struct {
    kn_http_req *req;
    int64_t account;
    char nonce[64];
} LinkCtx;

/* ["dlnkR", result] (deob-10.0.7.js:216792-216796) to the account's social sockets */
static void push_result(const int64_t account, const char *result) {
    if (account <= 0) return;
    cJSON *fr = social_frame("dlnkR");
    cJSON_AddItemToArray(fr, cJSON_CreateString(result));
    social_push_account_ex(account, fr, 1);
}

static void expiry_taken(kn_db_result *res, void *ud) {
    (void) ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    push_result(kn_json_int(o, "account", 0), "expired"); /* nobody came back with it */
    cJSON_Delete(o);
}

static void expiry_fire(kn_loop *loop, void *ud) {
    (void) loop;
    struct Expiry *e = ud;
    char nonce[64];
    copy_str(nonce, sizeof nonce, e->nonce);
    memset(e, 0, sizeof *e);
    if (D.b && D.b->db) kn_db_call(D.b->db, &P_DC_STATE_TAKE, KN_DB_ARGS(nonce), expiry_taken, NULL);
}

static void expiry_clear(struct Expiry *e) {
    if (e->timer && D.b && D.b->loop) kn_loop_cancel_timer(D.b->loop, e->timer);
    memset(e, 0, sizeof *e);
}

static void expiry_start(const int64_t account, const char *nonce) {
    struct Expiry *slot = NULL;
    for (int i = 0; i < MAX_EXPIRY; i++) {
        if (g_expiry[i].account == account) {
            expiry_clear(&g_expiry[i]);
            slot = &g_expiry[i];
            break;
        }
        if (!slot && !g_expiry[i].account) slot = &g_expiry[i];
    }
    if (!slot || !D.b || !D.b->loop) return; /* full: a late callback still says "expired" */
    slot->account = account;
    copy_str(slot->nonce, sizeof slot->nonce, nonce);
    slot->timer = kn_loop_add_timer(D.b->loop, (uint64_t) (STATE_S + 5) * 1000, 0, expiry_fire, slot);
    if (!slot->timer) memset(slot, 0, sizeof *slot);
}

/* the callback took this state: its expiry is moot */
static void expiry_taken_by(const int64_t account, const char *nonce) {
    for (int i = 0; i < MAX_EXPIRY; i++)
        if (g_expiry[i].account == account && !strcmp(g_expiry[i].nonce, nonce)) expiry_clear(&g_expiry[i]);
}

static void link_state_done(kn_db_result *res, void *ud) {
    LinkCtx *c = ud;
    if (!kn_db_ok(res)) {
        const char *e = acct_db_error(res, "discord_state");
        if (!strcmp(e, DELETION_PENDING)) v10_reply_error(c->req, 409, DELETION_PENDING, "Your account is being deleted.");
        else v10_reply_error(c->req, 500, "internal", e);
        free(c);
        return;
    }
#ifdef KN_HAVE_CURL
    char *cid = curl_easy_escape(NULL, D.client_id, 0), *redir = curl_easy_escape(NULL, D.redirect_uri, 0);
    char url[1400];
    snprintf(url, sizeof url, "%s?response_type=code&client_id=%s&scope=identify%%20guilds.join&redirect_uri=%s&state=%s%s&prompt=consent",
             D.authorize_url, cid, redir, D.state_prefix, c->nonce);
    curl_free(cid);
    curl_free(redir);
    cJSON *d = cJSON_CreateObject();
    cJSON_AddStringToObject(d, "url", url);
    v10_reply_data(c->req, 200, d);
    expiry_start(c->account, c->nonce);
#else
    v10_reply_error(c->req, 503, "DISCORD_NOT_CONFIGURED", "Discord linking is currently unavailable.");
#endif
    free(c);
}

static void link_session(kn_http_req *req, int64_t account, void *ud) {
    char *to = ud;
    LinkCtx *c = calloc(1, sizeof *c);
    if (!c || kn_random_token(c->nonce, sizeof c->nonce, 32) != 0) {
        free(c);
        free(to);
        return v10_reply_error(req, 500, "internal", "Server error.");
    }
    c->req = req;
    c->account = account;
    char id[24];
    kn_db_call(D.b->db, &P_DC_STATE_NEW, KN_DB_ARGS(acct_itoa(id, account), c->nonce, return_ok(to) ? to : ""), link_state_done, c);
    free(to);
}

static void h_link(kn_http_req *req, void *ud) {
    (void) ud;
    if (!configured()) return v10_reply_error(req, 503, "DISCORD_NOT_CONFIGURED", "Discord linking is currently unavailable.");
    cJSON *b = kn_http_body_json(req);
    char *to = strdup(kn_json_str(b, "returnTo", ""));
    cJSON_Delete(b);
    with_session(req, link_session, to);
}

/* ================================================================== GET /discord/callback */

static void esc(kn_buf *b, const char *s) {
    for (; s && *s; s++) {
        switch (*s) {
            case '<': kn_buf_append_str(b, "&lt;"); break;
            case '>': kn_buf_append_str(b, "&gt;"); break;
            case '&': kn_buf_append_str(b, "&amp;"); break;
            case '"': kn_buf_append_str(b, "&quot;"); break;
            default: kn_buf_append_byte(b, (uint8_t) *s);
        }
    }
}

/* discord.result.<result> (deob-10.0.7.js:204284), the Box Office account's words */
static const char *result_text(const char *r) {
    if (!strcmp(r, "linked")) return "Discord account linked! You can close this window and return to the game.";
    if (!strcmp(r, "cancelled")) return "Discord linking was cancelled.";
    if (!strcmp(r, "expired")) return "The link request expired, please try again.";
    if (!strcmp(r, "in_use")) return "That Discord account is already linked to another Box Office account.";
    if (!strcmp(r, "unavailable")) return "Discord linking is currently unavailable.";
    if (!strcmp(r, "deletion_pending"))
        return "Your account is being deleted, so its Discord account can't be unlinked or changed. Cancel the deletion first.";
    return "Failed to link Discord, please try again.";
}

/* back to the game (returnTo?discord_link=<result>, :216789-216797), or the page for the native client's browser */
static void finish_callback(kn_http_req *req, const char *return_to, const char *result) {
    if (return_to && *return_to) {
        kn_buf loc = {0};
        kn_buf_append_str(&loc, return_to);
        kn_buf_append_str(&loc, strchr(return_to, '?') ? "&discord_link=" : "?discord_link=");
        kn_buf_append_str(&loc, result);
        kn_buf_append_byte(&loc, 0);
        kn_http_set_header(req, "Location", (const char *) loc.data);
        kn_http_set_header(req, "Cache-Control", "no-store");
        kn_http_respond(req, 303, "text/plain", "", 0);
        kn_buf_free(&loc);
        return;
    }
    kn_buf b = {0};
    kn_buf_append_str(&b, "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\"><title>");
    esc(&b, KN_PRODUCT_NAME);
    kn_buf_append_str(&b, "</title></head><body style=\"margin:0;background:#202020;color:#fff;font-family:sans-serif;text-align:center;"
                          "padding:80px 16px\"><h2 style=\"color:#5865f2\">Discord</h2><p style=\"font-size:18px;color:rgba(255,255,255,0.8)\">");
    esc(&b, result_text(result));
    kn_buf_append_str(&b, "</p></body></html>");
    kn_http_set_header(req, "Cache-Control", "no-store");
    kn_http_respond(req, !strcmp(result, "linked") ? 200 : 400, "text/html; charset=utf-8", b.data ? (const void *) b.data : "", b.len);
    kn_buf_free(&b);
}

/* the callback's answer, and the same result to the account's open game (["dlnkR", result]) */
static void finish_result(kn_http_req *req, const int64_t account, const char *return_to, const char *result) {
    push_result(account, result);
    finish_callback(req, return_to, result);
}

static void linked(kn_db_result *res, void *ud) {
    Job *j = ud;
    const char *r = "linked";
    if (!kn_db_ok(res)) {
        const char *e = acct_db_error(res, "discord_link");
        r = !strcmp(e, "in_use") ? "in_use" : !strcmp(e, "deletion_pending") ? "deletion_pending" : "failed";
    } else {
        KN_INFO(MOD, "account %lld linked Discord user %s", (long long) j->account, j->discord_id);
    }
    finish_result(j->req, j->account, j->return_to, r);
    job_free(j);
}

static void exchange_work(Job *j) {
#ifdef KN_HAVE_CURL
    cJSON *t = dc_token("authorization_code", j->code);
    memset(j->code, 0, sizeof j->code);
    if (!t) {
        copy_str(j->result, sizeof j->result, "failed");
        return;
    }
    char auth[600];
    snprintf(auth, sizeof auth, "Bearer %s", kn_json_str(t, "access_token", ""));
    char url[320];
    snprintf(url, sizeof url, "%s/users/@me", D.api_url);
    Body b = {0};
    const long st = dc_call(NULL, url, auth, NULL, 0, &b);
    memset(auth, 0, sizeof auth);
    cJSON *me = st == 200 && b.data ? cJSON_Parse(b.data) : NULL;
    free(b.data);
    const char *id = kn_json_str(me, "id", NULL);
    if (!id || !*id || strlen(id) >= sizeof j->discord_id) {
        copy_str(j->result, sizeof j->result, "failed");
    } else {
        copy_str(j->discord_id, sizeof j->discord_id, id);
        copy_str(j->username, sizeof j->username, kn_json_str(me, "username", ""));
        copy_str(j->global_name, sizeof j->global_name, kn_json_str(me, "global_name", ""));
        copy_str(j->avatar, sizeof j->avatar, kn_json_str(me, "avatar", ""));
        seal_tokens(t, &j->token_enc, &j->refresh_enc, j->expires);
        copy_str(j->result, sizeof j->result, j->token_enc ? "linked" : "failed");
    }
    cJSON_Delete(me);
    cJSON_Delete(t);
#else
    copy_str(j->result, sizeof j->result, "unavailable");
#endif
}

static void exchange_done(Job *j) {
    if (strcmp(j->result, "linked") || !D.b || !D.b->db)
        return finish_result(j->req, j->account, j->return_to, j->result[0] && strcmp(j->result, "linked") ? j->result : "failed");
    Job *copy = malloc(sizeof *copy); /* job_done frees j: the database call gets its own copy (the tokens move) */
    if (!copy) return finish_result(j->req, j->account, j->return_to, "failed");
    *copy = *j;
    j->token_enc = j->refresh_enc = NULL;
    char id[24];
    kn_db_call(D.b->db, &P_DC_LINK,
               KN_DB_ARGS(acct_itoa(id, copy->account), copy->discord_id, copy->username, copy->global_name, copy->avatar, copy->token_enc,
                          copy->refresh_enc ? copy->refresh_enc : "", copy->expires),
               linked, copy);
}

static void state_taken(kn_db_result *res, void *ud) {
    Job *j = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    j->account = kn_json_int(o, "account", 0);
    copy_str(j->return_to, sizeof j->return_to, kn_json_str(o, "return_to", ""));
    const int fresh = cJSON_IsTrue(cJSON_GetObjectItem(o, "fresh"));
    cJSON_Delete(o);
    if (j->account > 0) expiry_taken_by(j->account, j->nonce);
    if (j->account <= 0 || !fresh) {
        finish_result(j->req, j->account, j->return_to, "expired");
        return job_free(j);
    }
    if (!j->code[0]) { /* Discord sent error=access_denied: the player cancelled */
        finish_result(j->req, j->account, j->return_to, "cancelled");
        return job_free(j);
    }
    j->work = exchange_work;
    j->done = exchange_done;
    if (job_start(j) != 0) {
        finish_result(j->req, j->account, j->return_to, "failed");
        job_free(j);
    }
}

static void h_callback(kn_http_req *req, void *ud) {
    (void) ud;
    char state[128] = "", err[64] = "";
    Job *j = calloc(1, sizeof *j);
    if (!j) return finish_callback(req, NULL, "failed");
    j->req = req;
    kn_http_query_get(req, "state", state, sizeof state);
    kn_http_query_get(req, "code", j->code, sizeof j->code);
    kn_http_query_get(req, "error", err, sizeof err);
    if (err[0]) j->code[0] = 0;
    if (!configured() || !D.b->db) {
        job_free(j);
        return finish_callback(req, NULL, "unavailable");
    }
    const size_t np = strlen(D.state_prefix);
    const char *nonce = np && !strncmp(state, D.state_prefix, np) ? state + np : state;
    if (!nonce[0]) {
        job_free(j);
        return finish_callback(req, NULL, "expired");
    }
    copy_str(j->nonce, sizeof j->nonce, nonce);
    kn_db_call(D.b->db, &P_DC_STATE_TAKE, KN_DB_ARGS(nonce), state_taken, j);
}

/* ================================================================== POST /discord/join */

static void join_work(Job *j) {
#ifdef KN_HAVE_CURL
    char *access = unseal_str(SEAL_ACCESS, j->token_enc);
    if (j->expired || !access) { /* refresh first */
        char *refresh = unseal_str(SEAL_REFRESH, j->refresh_enc);
        cJSON *t = refresh ? dc_token("refresh_token", refresh) : NULL;
        if (refresh) {
            memset(refresh, 0, strlen(refresh));
            free(refresh);
        }
        if (t) {
            if (access) memset(access, 0, strlen(access));
            free(access);
            access = strdup(kn_json_str(t, "access_token", ""));
            free(j->token_enc);
            free(j->refresh_enc);
            seal_tokens(t, &j->token_enc, &j->refresh_enc, j->expires);
            j->refreshed = j->token_enc != NULL;
            cJSON_Delete(t);
        } else if (j->expired || !access) {
            free(access);
            copy_str(j->result, sizeof j->result, "relink");
            return;
        }
    }
    char url[400], auth[300], *body = NULL;
    snprintf(url, sizeof url, "%s/guilds/%s/members/%s", D.api_url, D.guild_id, j->discord_id);
    snprintf(auth, sizeof auth, "Bot %s", D.bot_token);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "access_token", access ? access : "");
    body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    if (access) memset(access, 0, strlen(access));
    free(access);
    Body b = {0};
    const long st = body ? dc_call("PUT", url, auth, body, 1, &b) : 0;
    if (body) memset(body, 0, strlen(body));
    free(body);
    free(b.data);
    memset(auth, 0, sizeof auth);
    /* 201 added, 204 already a member; 403 with the user's token refused (invalid_grant / scope) = link again */
    copy_str(j->result, sizeof j->result, st == 201 ? "joined" : st == 204 ? "already" : st == 401 || st == 403 ? "relink" : "failed");
#else
    copy_str(j->result, sizeof j->result, "failed");
#endif
}

static void join_saved(kn_db_result *res, void *ud) {
    (void) res;
    (void) ud;
}

static void join_done(Job *j) {
    const int in = !strcmp(j->result, "joined") || !strcmp(j->result, "already");
    if ((in || j->refreshed) && D.b && D.b->db) {
        char id[24];
        kn_db_call(D.b->db, &P_DC_JOINED,
                   KN_DB_ARGS(acct_itoa(id, j->account), in ? "1" : "0", j->refreshed ? j->token_enc : "", j->refresh_enc ? j->refresh_enc : "",
                              j->expires[0] ? j->expires : "0"),
                   join_saved, NULL);
    }
    if (in) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddBoolToObject(d, "joined", !strcmp(j->result, "joined"));
        v10_reply_data(j->req, 200, d);
    } else if (!strcmp(j->result, "relink")) {
        v10_reply_error(j->req, 409, "DISCORD_NEEDS_RELINK", "Unlink and link Discord again to join the server.");
    } else {
        v10_reply_error(j->req, 502, "DISCORD_JOIN_FAILED", "Couldn't join the Discord server. Please try again.");
    }
}

static void join_tokens(kn_db_result *res, void *ud) {
    Job *j = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!o) {
        v10_reply_error(j->req, kn_db_ok(res) ? 400 : 500, kn_db_ok(res) ? "DISCORD_NOT_LINKED" : "internal", "Link Discord first.");
        return job_free(j);
    }
    copy_str(j->discord_id, sizeof j->discord_id, kn_json_str(o, "discord_id", ""));
    const char *t = kn_json_str(o, "token_enc", NULL), *r = kn_json_str(o, "refresh_enc", NULL);
    j->token_enc = t && *t ? strdup(t) : NULL;
    j->refresh_enc = r && *r ? strdup(r) : NULL;
    j->expired = cJSON_IsTrue(cJSON_GetObjectItem(o, "expired"));
    cJSON_Delete(o);
    j->work = join_work;
    j->done = join_done;
    if (job_start(j) != 0) {
        v10_reply_error(j->req, 503, "DISCORD_JOIN_FAILED", "Server busy, try again.");
        job_free(j);
    }
}

static void join_session(kn_http_req *req, int64_t account, void *ud) {
    (void) ud;
    Job *j = calloc(1, sizeof *j);
    if (!j) return v10_reply_error(req, 500, "internal", "Server error.");
    j->req = req;
    j->account = account;
    char id[24];
    kn_db_call(D.b->db, &P_DC_TOKENS, KN_DB_ARGS(acct_itoa(id, account)), join_tokens, j);
}

static void h_join(kn_http_req *req, void *ud) {
    (void) ud;
    if (!join_configured()) return v10_reply_error(req, 503, "DISCORD_NOT_CONFIGURED", "Joining the Discord server is currently unavailable.");
    with_session(req, join_session, NULL);
}

/* ================================================================== POST /discord/unlink */

static void revoke_work(Job *j) {
#ifdef KN_HAVE_CURL
    char *access = unseal_str(SEAL_ACCESS, j->token_enc);
    if (access) {
        dc_revoke(access);
        memset(access, 0, strlen(access));
        free(access);
    }
#else
    (void) j;
#endif
}

typedef struct {
    kn_http_req *req;
    int64_t account;
} UnlinkCtx;

static void unlinked(kn_db_result *res, void *ud) {
    UnlinkCtx *c = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!o) {
        const char *e = acct_db_error(res, "discord_unlink");
        if (!strcmp(e, DELETION_PENDING))
            v10_reply_error(c->req, 409, DELETION_PENDING, "Your account is being deleted, so its Discord account can't be unlinked.");
        else
            v10_reply_error(c->req, 500, "internal", e);
        free(c);
        return;
    }
    const char *tok = kn_json_str(o, "token_enc", NULL);
    if (tok && *tok && configured()) { /* revoke at Discord; the link is already gone here */
        Job *j = calloc(1, sizeof *j);
        if (j) {
            j->token_enc = strdup(tok);
            j->work = revoke_work;
            if (!j->token_enc || job_start(j) != 0) job_free(j);
        }
    }
    cJSON *d = cJSON_CreateObject();
    cJSON_AddBoolToObject(d, "unlinked", cJSON_IsTrue(cJSON_GetObjectItem(o, "unlinked")));
    cJSON_Delete(o);
    if (cJSON_IsTrue(cJSON_GetObjectItem(d, "unlinked"))) KN_INFO(MOD, "account %lld unlinked Discord", (long long) c->account);
    v10_reply_data(c->req, 200, d);
    free(c);
}

static void unlink_session(kn_http_req *req, int64_t account, void *ud) {
    (void) ud;
    UnlinkCtx *c = calloc(1, sizeof *c);
    if (!c) return v10_reply_error(req, 500, "internal", "Server error.");
    c->req = req;
    c->account = account;
    char id[24];
    kn_db_call(D.b->db, &P_DC_UNLINK, KN_DB_ARGS(acct_itoa(id, account)), unlinked, c);
}

static void h_unlink(kn_http_req *req, void *ud) {
    (void) ud;
    with_session(req, unlink_session, NULL);
}

/* ================================================================== module */

static void origin_of(const char *url, char *out, size_t cap) {
    out[0] = 0;
    const char *s = url ? strstr(url, "://") : NULL;
    if (!s) return;
    const char *e = strchr(s + 3, '/');
    const size_t n = e ? (size_t) (e - url) : strlen(url);
    if (n < cap) {
        memcpy(out, url, n);
        out[n] = 0;
    }
}

int discord_init(Brain *b) {
    memset(&D, 0, sizeof D);
    memset(g_expiry, 0, sizeof g_expiry);
    D.b = b;
    atomic_store(&g_cancel, 0);
    const kn_config *cfg = b->cfg;
    copy_str(D.client_id, sizeof D.client_id, kn_config_str(cfg, "accounts.discord.client_id", ""));
    copy_str(D.client_secret, sizeof D.client_secret, kn_config_str(cfg, "accounts.discord.client_secret", ""));
    copy_str(D.bot_token, sizeof D.bot_token, kn_config_str(cfg, "accounts.discord.bot_token", ""));
    copy_str(D.guild_id, sizeof D.guild_id, kn_config_str(cfg, "accounts.discord.guild_id", ""));
    copy_str(D.guild_url, sizeof D.guild_url, kn_config_str(cfg, "accounts.discord.guild_url", ""));
    copy_str(D.authorize_url, sizeof D.authorize_url, kn_config_str(cfg, "accounts.discord.authorize_url", "https://discord.com/oauth2/authorize"));
    copy_str(D.api_url, sizeof D.api_url, kn_config_str(cfg, "accounts.discord.api_url", "https://discord.com/api/v10"));
    copy_str(D.state_prefix, sizeof D.state_prefix, kn_config_str(cfg, "accounts.discord.state_prefix", ""));
    D.timeout_ms = (int) kn_config_int(cfg, "accounts.discord.timeout_ms", 10000);
    char def[512];
    snprintf(def, sizeof def, "%s/discord/callback", mm_settings()->public_url);
    /* empty (config.example.json's value) = the default too, as the config comment says */
    const char *redir = kn_config_str(cfg, "accounts.discord.redirect_uri", "");
    copy_str(D.redirect_uri, sizeof D.redirect_uri, redir && *redir ? redir : def);
    copy_str(D.seal, sizeof D.seal, account_links_seal_material(b));
    /* returnTo: the brain's own origin and accounts.discord.return_origins (the web build's) */
    origin_of(mm_settings()->public_url, D.origins[0], sizeof D.origins[0]);
    D.norigins = D.origins[0][0] ? 1 : 0;
    const char *list[MAX_ORIGINS];
    const size_t n = kn_config_str_list(cfg, "accounts.discord.return_origins", list, MAX_ORIGINS - 1);
    for (size_t i = 0; i < n && D.norigins < MAX_ORIGINS; i++) copy_str(D.origins[D.norigins++], sizeof D.origins[0], list[i]);
    if (!b->http) return 0;
    kn_http_route(b->http, "GET", "/discord/status", h_status, NULL);
    kn_http_route(b->http, "POST", "/discord/link", h_link, NULL);
    kn_http_route(b->http, "GET", "/discord/callback", h_callback, NULL);
    kn_http_route(b->http, "POST", "/discord/join", h_join, NULL);
    kn_http_route(b->http, "POST", "/discord/unlink", h_unlink, NULL);
    if (configured())
        KN_INFO(MOD, "Discord linking on (Join Server %s)", join_configured() ? "on" : "off: accounts.discord.bot_token / guild_id unset");
    else
        KN_INFO(MOD, "Discord linking off (accounts.discord.client_id / client_secret unset)");
    return 0;
}

void discord_shutdown(Brain *b) {
    (void) b;
    for (int i = 0; i < MAX_EXPIRY; i++)
        if (g_expiry[i].account) expiry_clear(&g_expiry[i]);
    atomic_store(&g_cancel, 1);
    for (int i = 0; i < 200 && atomic_load(&g_live) > 0; i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
}
