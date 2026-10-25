/*
 * services/brain/account_life.c - Krunker v10.0.3's account window (windows[4], showWindow(5)) behind kn_brain
 * (migration 0359_account_lifecycle; the client is client/src/ui/account_ui.c).
 *
 * Account Privacy (deob-10.0.3.js:247682-247684, 244591-244643):
 *   ["rqd", 1 | 0]  -> ["rqd", ok, error | null, flag]. v10's rpc(rqd, flag): 1 requests the deletion
 *                   (accounts.delete_requested_at, v10's player_cancelrequest; acc.reqDel = true, "Processing
 *                   Deletion"), 0 cancels it. v10's client logs out after a request and cancels it by itself at its
 *                   next login (`if (acc.reqDel) requestConfirm(0)`, 244449), as its popup says ("If you log back in
 *                   before the 14 days are up, the deletion request will be cancelled automatically", 158408).
 *                   KrunkNative: a request also ends every session of the account (the popup's "You have been
 *                   logged out" then holds on every device, and a password login is the one way back).
 *   An account whose request is older than accounts.deletion_grace_days (14, i18n "account.delete") and that has not
 *   logged in since is deleted by the hourly sweep (accounts.deletion_sweep_s, 3600; accounts.deletion_purge false
 *   turns it off): DELETE FROM accounts, every reference is ON DELETE CASCADE / SET NULL ("the account and all
 *   associated data will be permanently removed").
 *
 * Connections > Twitch (247697-247750, 261960-262004, 261606-261630):
 *   v10 sends the page itself to https://id.twitch.tv/oauth2/authorize with Krunker's client id and comes back to
 *   "/?twitch_auth&code=..." to send VERIFY_TWITCH(code). KrunkNative has no page origin to come back to (the native
 *   client), so the brain is the OAuth client, with the Twitch app of the Live Streams (streams.twitch.client_id /
 *   client_secret):
 *   ["twitchAuth"]  -> ["twitchAuth", url] (the authorize page, a single-use state of 10 minutes; the client opens it
 *                   in the browser), or ["twitchAuth", null, error]: "Twitch linking is currently unavailable." when no
 *                   client id / secret is configured (the account stays v10's "Twitch Account (none)").
 *   GET /twitch/callback?code&state  (the app's redirect URI, streams.twitch.redirect_uri, default
 *                   <brain.public_url>/twitch/callback): the code is exchanged (streams.twitch.auth_url), the Twitch
 *                   user read (streams.twitch.api_url /users), the link stored, and the account's sockets get v10's
 *                   ["twitchVerifyResponse", error | null, twitchName, twitchId] (TWITCH_VERIFY_RESPONSE). The page
 *                   says the outcome. Neither token is kept (KrunkNative claims no drops with them).
 *   ["removeTwitchLink"] -> ["twitchRemoveResponse"]: tw_userid / tw_twitchname cleared.
 *   ["getTwitchDrops"]   -> ["twitchDropsResponse", []]: Box Office has no Twitch drop campaigns (v10: "Nothing to
 *                   Claim").
 *   ["verifyTwitch", code] -> ["twitchVerifyResponse", "Twitch linking is currently unavailable."]: the code of a
 *                   redirect to a page is never valid here (the redirect URI is the brain's).
 *   The pair is permanent (twitch_links): an account keeps its Twitch user and a Twitch user its account after an
 *   unlink ("Twitch linking creates a permanent connection to this Krunker account regardless of unlinking",
 *   "Unlinking does not allow you to link a seperate twitch account").
 *
 * The account object gains "reqDel", "twitchId" and "raidStats" (db_account_life_extras), which the social socket's
 * account info carries (social_account_info).
 */
#include "account_life.h"

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
#include "hot.h"
#include "raids.h"
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

#define MOD "account-life"
#define STR_(x) #x
#define STR(x) STR_(x)
#define TWITCH_STATE_S 600      /* a started link's state lives 10 minutes */
#define TWITCH_MAX_BODY (64 * 1024)
#define TWITCH_UNAVAILABLE "Twitch linking is currently unavailable."

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static struct {
    Brain *b;
    kn_timer_id sweep_timer;
    int64_t grace_s;
    int purge;
    char client_id[128], client_secret[256];
    char authorize_url[256], token_url[256], api_url[256], redirect_uri[512], scope[128];
    int timeout_ms;
} L;

static atomic_int g_live;   /* Twitch exchanges running on worker threads */
static atomic_int g_cancel; /* shutdown: the running exchanges stop */

static void copy_str(char *dst, const size_t n, const char *src) { snprintf(dst, n, "%s", src ? src : ""); }

static int twitch_configured(void) {
#ifdef KN_HAVE_CURL
    return L.client_id[0] && L.client_secret[0] && L.redirect_uri[0];
#else
    return 0; /* built without libcurl: the code cannot be exchanged */
#endif
}

/* ================================================================== procs (database worker threads) */

/* (id, flag) -> {reqDel}: flag 1 sets delete_requested_at (kept when already set) and ends every session, 0 clears it */
static kn_db_result *run_delete_request(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int on = PI(1) != 0;
    int64_t hit;
    if (on) {
        hit = dbx_exec(&x, SQL(.any = "UPDATE accounts SET delete_requested_at = coalesce(delete_requested_at, {now}) WHERE id = $1"),
                       KN_DB_ARGS(P(0)));
        dbx_exec(&x, SQL(.any = "DELETE FROM sessions WHERE account_id = $1"), KN_DB_ARGS(P(0)));
    } else {
        hit = dbx_exec(&x, SQL(.any = "UPDATE accounts SET delete_requested_at = NULL WHERE id = $1"), KN_DB_ARGS(P(0)));
    }
    if (!dbx_failed(&x) && hit <= 0) dbx_refuse(&x, "Account not found.");
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "reqDel", on);
    return dbx_json(&x, "delete_request", o);
}
static const kn_proc P_DELETE_REQUEST = {"delete_request", NULL, run_delete_request};

/* (grace seconds) -> [id, ...]: the requests past the grace period whose account has not logged in since */
#define DUE_WHERE                                                                                                   \
    "delete_requested_at IS NOT NULL AND delete_requested_at <= {ago:$1}"                                           \
    " AND (last_login_at IS NULL OR last_login_at < delete_requested_at)"
static kn_db_result *run_deletion_due(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT id AS id__i FROM accounts WHERE " DUE_WHERE " ORDER BY id LIMIT 100"), KN_DB_ARGS(P(0)));
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    cJSON *ids = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) cJSON_AddItemToArray(ids, cJSON_CreateNumber(cJSON_GetNumberValue(cJSON_GetObjectItem(r, "id"))));
    cJSON_Delete(rows);
    return dbx_json(&x, "deletion_due", ids);
}
static const kn_proc P_DELETION_DUE = {"deletion_due", NULL, run_deletion_due};

/* (grace seconds, id) -> {username} | null: the account is deleted when it is still due (checked again here) */
static kn_db_result *run_account_delete(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    char *name = dbx_text(&x, SQL(.any = "SELECT username FROM accounts WHERE id = $2 AND " DUE_WHERE), KN_DB_ARGS(P(0), P(1)));
    if (!name || dbx_failed(&x)) {
        free(name);
        return dbx_failed(&x) ? dbx_finish(&x, NULL) : dbx_json(&x, "account_delete", NULL);
    }
    dbx_exec(&x, SQL(.any = "DELETE FROM accounts WHERE id = $1"), KN_DB_ARGS(P(1)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "username", name);
    free(name);
    return dbx_failed(&x) ? (cJSON_Delete(o), dbx_finish(&x, NULL)) : dbx_json(&x, "account_delete", o);
}
static const kn_proc P_ACCOUNT_DELETE = {"account_delete", NULL, run_account_delete};

/* (id, state) -> {linked}: the account's earlier states (and every stale one) go, the new one is stored hashed */
static kn_db_result *run_twitch_state_new(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    char *linked = dbx_text(&x, SQL(.any = "SELECT tw_userid FROM players WHERE player_id = $1"), KN_DB_ARGS(P(0)));
    if (linked && *linked) {
        free(linked);
        dbx_refuse(&x, "Twitch Account Already Linked");
        return dbx_finish(&x, NULL);
    }
    free(linked);
    dbx_exec(&x, SQL(.any = "DELETE FROM twitch_link_states WHERE account_id = $1 OR created_at < {ago:" STR(TWITCH_STATE_S) "}"),
             KN_DB_ARGS(P(0)));
    dbx_exec(&x, SQL(.any = "INSERT INTO twitch_link_states (state_hash, account_id) VALUES ({hash:$2}, $1)"), KN_DB_ARGS(P(0), P(1)));
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    return dbx_json(&x, "twitch_state", cJSON_CreateObject());
}
static const kn_proc P_TWITCH_STATE_NEW = {"twitch_state_new", NULL, run_twitch_state_new};

/* (state) -> {account}: a live state, used up */
static kn_db_result *run_twitch_state_take(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = dbx_int(&x, SQL(.any = "SELECT account_id FROM twitch_link_states WHERE state_hash = {hash:$1}"
                                              " AND created_at >= {ago:" STR(TWITCH_STATE_S) "}"),
                               KN_DB_ARGS(P(0)), 0);
    dbx_exec(&x, SQL(.any = "DELETE FROM twitch_link_states WHERE state_hash = {hash:$1}"), KN_DB_ARGS(P(0)));
    if (!dbx_failed(&x) && id <= 0) dbx_refuse(&x, "This link has expired. Start again from the Account window.");
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "account", (double) id);
    return dbx_json(&x, "twitch_state", o);
}
static const kn_proc P_TWITCH_STATE_TAKE = {"twitch_state_take", NULL, run_twitch_state_take};

/* (id, twitch id, twitch login) -> {twitchId, twitch, username}: the permanent pair, then the account's link */
static kn_db_result *run_twitch_link(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    char *mine = dbx_text(&x, SQL(.any = "SELECT twitch_id FROM twitch_links WHERE account_id = $1"), KN_DB_ARGS(P(0)));
    const int64_t owner = dbx_int(&x, SQL(.any = "SELECT account_id FROM twitch_links WHERE twitch_id = $1"), KN_DB_ARGS(P(1)), 0);
    const char *new_tw = P(1) ? P(1) : "";
    if (!dbx_failed(&x) && mine && strcmp(mine, new_tw) != 0) {
        dbx_refuse(&x, "This account is permanently linked to another Twitch account.");
    } else if (!dbx_failed(&x) && owner > 0 && owner != PI(0)) {
        dbx_refuse(&x, "This Twitch account is linked to another account.");
    }
    if (!dbx_failed(&x) && !mine)
        dbx_exec(&x, SQL(.any = "INSERT INTO twitch_links (account_id, twitch_id, twitch_name) VALUES ($1, $2, $3)"), KN_DB_ARGS(P(0), P(1), P(2)));
    free(mine);
    dbx_exec(&x, SQL(.any = "UPDATE twitch_links SET twitch_name = $2 WHERE account_id = $1"), KN_DB_ARGS(P(0), P(2)));
    dbx_exec(&x, SQL(.any = "UPDATE players SET tw_userid = $2, tw_twitchname = $3, updated_at = {now} WHERE player_id = $1"),
             KN_DB_ARGS(P(0), P(1), P(2)));
    char *user = dbx_text(&x, SQL(.any = "SELECT username FROM accounts WHERE id = $1"), KN_DB_ARGS(P(0)));
    if (dbx_failed(&x)) {
        free(user);
        return dbx_finish(&x, NULL);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "twitchId", P(1) ? P(1) : "");
    cJSON_AddStringToObject(o, "twitch", P(2) ? P(2) : "");
    cJSON_AddStringToObject(o, "username", user ? user : "");
    free(user);
    return dbx_json(&x, "twitch_link", o);
}
static const kn_proc P_TWITCH_LINK = {"twitch_link", NULL, run_twitch_link};

void db_account_life_extras(Dbx *x, const int64_t id, cJSON *account) {
    if (!account) return;
    cJSON *row = dbx_row(x, SQL(.any = "SELECT CASE WHEN a.delete_requested_at IS NULL THEN 0 ELSE 1 END AS req__i, p.tw_userid AS tw, p.tw_twitchname AS twn"
                                       " FROM accounts a JOIN players p ON p.player_id = a.id WHERE a.id = $1"),
                         KN_DB_ARGS(I64S(id)));
    const char *tw = cJSON_GetStringValue(cJSON_GetObjectItem(row, "tw"));
    const char *twn = cJSON_GetStringValue(cJSON_GetObjectItem(row, "twn"));
    cJSON_DeleteItemFromObjectCaseSensitive(account, "reqDel");
    cJSON_DeleteItemFromObjectCaseSensitive(account, "twitchId");
    cJSON_DeleteItemFromObjectCaseSensitive(account, "twitch");
    cJSON_DeleteItemFromObjectCaseSensitive(account, "raidStats");
    cJSON_AddBoolToObject(account, "reqDel", cJSON_GetNumberValue(cJSON_GetObjectItem(row, "req")) > 0);
    if (tw && *tw) cJSON_AddStringToObject(account, "twitchId", tw);
    else cJSON_AddNullToObject(account, "twitchId");
    if (twn && *twn) cJSON_AddStringToObject(account, "twitch", twn); /* acc.twitch (setData [23]) */
    else cJSON_AddNullToObject(account, "twitch");
    cJSON_Delete(row);
    cJSON *raids = db_raid_stats(x, id);
    cJSON_AddItemToObject(account, "raidStats", raids ? raids : cJSON_CreateArray());
}

/* ================================================================== "rqd" */

static void delete_request_done(SocialSession *s, kn_db_result *res, void *extra) {
    const int flag = extra ? *(const int *) extra : 0;
    if (!kn_db_ok(res)) {
        social_send_n(s, "rqd", 4, cJSON_CreateFalse(), cJSON_CreateString(acct_db_error(res, "rqd")), cJSON_CreateNull(),
                      cJSON_CreateNumber(flag));
        return;
    }
    if (flag) hot_session_forget_account(s->account_id); /* every session ended (P_DELETE_REQUEST) */
    KN_INFO(MOD, "account %lld %s its deletion", (long long) s->account_id, flag ? "requested" : "cancelled");
    social_send_n(s, "rqd", 4, cJSON_CreateTrue(), cJSON_CreateNull(), cJSON_CreateNull(), cJSON_CreateNumber(flag));
}

/* ["rqd", flag] (v10 rpc(rqd, flag), deob-10.0.3.js:244600) */
void sh_delete_request(SocialSession *s, const cJSON *f) {
    if (!social_require_auth(s)) return;
    int *flag = malloc(sizeof *flag); /* social_call frees the extra after delete_request_done */
    if (!flag) return;
    *flag = fa_int(f, 1, 0) != 0 || cJSON_IsTrue(fa(f, 1));
    char me[24], fl[4];
    snprintf(fl, sizeof fl, "%d", *flag);
    social_call(s, &P_DELETE_REQUEST, KN_DB_ARGS(social_viewer(s, me), fl), delete_request_done, flag);
}

/* ================================================================== the deletion sweep */

static void account_deleted(kn_db_result *res, void *ud) {
    const int64_t id = (int64_t) (intptr_t) ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!kn_db_ok(res)) KN_WARN(MOD, "deleting account %lld: %s", (long long) id, acct_db_error(res, "account_delete"));
    else if (o) {
        hot_session_forget_account(id);
        KN_INFO(MOD, "deleted account %lld (%s): its deletion request ran out", (long long) id, kn_json_str(o, "username", "?"));
    }
    cJSON_Delete(o);
}

static void deletion_due_done(kn_db_result *res, void *ud) {
    (void) ud;
    if (!L.b || !L.b->db) return;
    cJSON *ids = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!kn_db_ok(res)) KN_WARN(MOD, "deletion sweep: %s", acct_db_error(res, "deletion_due"));
    const cJSON *id;
    char grace[24], ids_text[24];
    acct_itoa(grace, L.grace_s);
    cJSON_ArrayForEach(id, ids) {
        const int64_t v = (int64_t) cJSON_GetNumberValue(id);
        if (v <= 0) continue;
        kn_db_call(L.b->db, &P_ACCOUNT_DELETE, KN_DB_ARGS(grace, acct_itoa(ids_text, v)), account_deleted, (void *) (intptr_t) v);
    }
    cJSON_Delete(ids);
}

static void sweep_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (!L.b || !L.b->db || !L.purge) return;
    char grace[24];
    kn_db_call(L.b->db, &P_DELETION_DUE, KN_DB_ARGS(acct_itoa(grace, L.grace_s)), deletion_due_done, NULL);
}

/* ================================================================== Twitch */

static void url_append_escaped(char *out, const size_t n, const char *s) {
    size_t o = strlen(out);
    static const char hex[] = "0123456789ABCDEF";
    for (; s && *s && o + 4 < n; s++) {
        const unsigned char ch = (unsigned char) *s;
        if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z') || (ch >= '0' && ch <= '9') || ch == '-' || ch == '_' || ch == '.' || ch == '~') {
            out[o++] = (char) ch;
        } else if (ch == ' ') {
            out[o++] = '+';
        } else {
            out[o++] = '%';
            out[o++] = hex[ch >> 4];
            out[o++] = hex[ch & 15];
        }
    }
    out[o] = 0;
}

typedef struct TwitchAuthCtx {
    char state[40];
} TwitchAuthCtx;

static void twitch_state_done(SocialSession *s, kn_db_result *res, void *extra) {
    TwitchAuthCtx *c = extra;
    if (!kn_db_ok(res)) {
        social_send_n(s, "twitchAuth", 2, cJSON_CreateNull(), cJSON_CreateString(acct_db_error(res, "twitchAuth")));
        return;
    }
    char url[2048];
    snprintf(url, sizeof url, "%s?response_type=code&client_id=", L.authorize_url);
    url_append_escaped(url, sizeof url, L.client_id);
    snprintf(url + strlen(url), sizeof url - strlen(url), "&scope=");
    url_append_escaped(url, sizeof url, L.scope);
    snprintf(url + strlen(url), sizeof url - strlen(url), "&redirect_uri=");
    url_append_escaped(url, sizeof url, L.redirect_uri);
    snprintf(url + strlen(url), sizeof url - strlen(url), "&state=%s", c->state);
    social_send_n(s, "twitchAuth", 1, cJSON_CreateString(url));
}

/* ["twitchAuth"] -> ["twitchAuth", url] | ["twitchAuth", null, error] */
void sh_twitch_auth(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    if (!twitch_configured()) {
        social_send_n(s, "twitchAuth", 2, cJSON_CreateNull(), cJSON_CreateString(TWITCH_UNAVAILABLE));
        return;
    }
    TwitchAuthCtx *c = calloc(1, sizeof *c);
    if (!c || kn_random_hex(c->state, sizeof c->state, 16) != 0) {
        free(c);
        social_send_n(s, "twitchAuth", 2, cJSON_CreateNull(), cJSON_CreateString("Server error."));
        return;
    }
    char me[24];
    social_call(s, &P_TWITCH_STATE_NEW, KN_DB_ARGS(social_viewer(s, me), c->state), twitch_state_done, c);
}

static void twitch_unlink_done(SocialSession *s, kn_db_result *res, void *extra) {
    (void) extra;
    if (!kn_db_ok(res)) {
        social_send_n(s, "pRes", 2, cJSON_CreateNull(), cJSON_CreateString(acct_db_error(res, "removeTwitchLink")));
        return;
    }
    social_send_n(s, "twitchRemoveResponse", 0);
}

/* ["removeTwitchLink"] -> ["twitchRemoveResponse"] (the permanent pair stays in twitch_links) */
void sh_twitch_unlink(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    char me[24];
    social_query(s, SQL(.any = "UPDATE players SET tw_userid = NULL, tw_twitchname = NULL, updated_at = {now} WHERE player_id = $1"),
                 KN_DB_ARGS(social_viewer(s, me)), twitch_unlink_done, NULL);
}

/* ["getTwitchDrops"] -> ["twitchDropsResponse", []]: no drop campaigns on Box Office */
void sh_twitch_drops(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    social_send_n(s, "twitchDropsResponse", 1, cJSON_CreateArray());
}

/* ["verifyTwitch", code]: v10's page-redirect code; the brain's redirect URI is the only one it accepts */
void sh_twitch_verify(SocialSession *s, const cJSON *f) {
    (void) f;
    if (!social_require_auth(s)) return;
    social_send_n(s, "twitchVerifyResponse", 1, cJSON_CreateString(TWITCH_UNAVAILABLE));
}

/* ---- GET /twitch/callback: the code exchange on a worker thread */

typedef struct TwitchJob {
    kn_http_req *req;
    int64_t account;
    char code[512];
    char twitch_id[64], login[64];
    char err[160];
} TwitchJob;

#ifdef KN_HAVE_CURL
typedef struct {
    char *data;
    size_t len;
} TwBuf;

static size_t tw_write(char *ptr, size_t size, size_t nmemb, void *ud) {
    TwBuf *b = ud;
    const size_t len = size * nmemb;
    if (b->len + len > TWITCH_MAX_BODY) return 0;
    char *grown = realloc(b->data, b->len + len + 1);
    if (!grown) return 0;
    memcpy(grown + b->len, ptr, len);
    b->len += len;
    grown[b->len] = 0;
    b->data = grown;
    return len;
}

static int tw_progress(void *ud, curl_off_t dl, curl_off_t dt, curl_off_t ul, curl_off_t ut) {
    (void) ud; (void) dl; (void) dt; (void) ul; (void) ut;
    return atomic_load(&g_cancel) ? 1 : 0;
}

static long tw_call(const char *url, const char *post, const char *bearer, TwBuf *out) {
    CURL *c = curl_easy_init();
    if (!c) return 0;
    struct curl_slist *h = NULL;
    char hdr[600];
    snprintf(hdr, sizeof hdr, "Client-Id: %s", L.client_id);
    h = curl_slist_append(h, hdr);
    if (bearer && *bearer) {
        snprintf(hdr, sizeof hdr, "Authorization: Bearer %s", bearer);
        h = curl_slist_append(h, hdr);
    }
    curl_easy_setopt(c, CURLOPT_URL, url);
    curl_easy_setopt(c, CURLOPT_HTTPHEADER, h);
    if (post) curl_easy_setopt(c, CURLOPT_POSTFIELDS, post);
    curl_easy_setopt(c, CURLOPT_WRITEFUNCTION, tw_write);
    curl_easy_setopt(c, CURLOPT_WRITEDATA, out);
    curl_easy_setopt(c, CURLOPT_USERAGENT, "kn_brain");
    curl_easy_setopt(c, CURLOPT_NOSIGNAL, 1L);
    curl_easy_setopt(c, CURLOPT_TIMEOUT_MS, (long) L.timeout_ms);
    curl_easy_setopt(c, CURLOPT_XFERINFOFUNCTION, tw_progress);
    curl_easy_setopt(c, CURLOPT_NOPROGRESS, 0L);
    long code = 0;
    if (curl_easy_perform(c) == CURLE_OK) curl_easy_getinfo(c, CURLINFO_RESPONSE_CODE, &code);
    curl_slist_free_all(h);
    curl_easy_cleanup(c);
    return code;
}
#endif

/* the token for the code, then the Twitch user (worker thread) */
static void twitch_exchange(TwitchJob *j) {
#ifdef KN_HAVE_CURL
    char *cid = curl_easy_escape(NULL, L.client_id, 0), *sec = curl_easy_escape(NULL, L.client_secret, 0);
    char *code = curl_easy_escape(NULL, j->code, 0), *redir = curl_easy_escape(NULL, L.redirect_uri, 0);
    char body[2048];
    snprintf(body, sizeof body, "client_id=%s&client_secret=%s&code=%s&grant_type=authorization_code&redirect_uri=%s",
             cid ? cid : "", sec ? sec : "", code ? code : "", redir ? redir : "");
    curl_free(cid);
    curl_free(sec);
    curl_free(code);
    curl_free(redir);
    TwBuf b = {0};
    long status = tw_call(L.token_url, body, NULL, &b);
    memset(body, 0, sizeof body);
    cJSON *tok = status == 200 && b.data ? cJSON_Parse(b.data) : NULL;
    free(b.data);
    const char *access = cJSON_GetStringValue(cJSON_GetObjectItem(tok, "access_token"));
    if (!access || !*access) {
        KN_WARN(MOD, "twitch token exchange failed (HTTP %ld)", status);
        copy_str(j->err, sizeof j->err, "Twitch did not accept the link. Try again.");
        cJSON_Delete(tok);
        return;
    }
    char bearer[256], url[512];
    copy_str(bearer, sizeof bearer, access);
    cJSON_Delete(tok);
    snprintf(url, sizeof url, "%s/users", L.api_url);
    b = (TwBuf){0};
    status = tw_call(url, NULL, bearer, &b);
    memset(bearer, 0, sizeof bearer);
    cJSON *users = status == 200 && b.data ? cJSON_Parse(b.data) : NULL;
    free(b.data);
    const cJSON *u = cJSON_GetArrayItem(cJSON_GetObjectItem(users, "data"), 0);
    const char *id = cJSON_GetStringValue(cJSON_GetObjectItem(u, "id"));
    const char *login = cJSON_GetStringValue(cJSON_GetObjectItem(u, "login"));
    if (!id || !*id || !login || !*login || strlen(id) >= sizeof j->twitch_id || strlen(login) >= sizeof j->login) {
        KN_WARN(MOD, "twitch user lookup failed (HTTP %ld)", status);
        copy_str(j->err, sizeof j->err, "Twitch did not answer. Try again.");
    } else {
        copy_str(j->twitch_id, sizeof j->twitch_id, id);
        copy_str(j->login, sizeof j->login, login);
    }
    cJSON_Delete(users);
#else
    copy_str(j->err, sizeof j->err, TWITCH_UNAVAILABLE);
#endif
}

static void buf_esc(kn_buf *b, const char *s) {
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

/* the browser tab the authorize page came back to: the outcome, then the player returns to the game */
static void callback_page(kn_http_req *req, const int status, const char *title, const char *text) {
    kn_buf b = {0};
    kn_buf_append_str(&b, "<!doctype html><html><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width\"><title>");
    buf_esc(&b, KN_PRODUCT_NAME);
    kn_buf_append_str(&b, "</title></head><body style=\"margin:0;background:#202020;color:#fff;font-family:sans-serif;text-align:center;"
                          "padding:80px 16px\"><h2 style=\"color:#6441a5\">");
    buf_esc(&b, title);
    kn_buf_append_str(&b, "</h2><p style=\"font-size:18px;color:rgba(255,255,255,0.8)\">");
    buf_esc(&b, text);
    kn_buf_append_str(&b, "</p></body></html>");
    kn_http_set_header(req, "Cache-Control", "no-store");
    kn_http_respond(req, status, "text/html; charset=utf-8", b.data ? b.data : "", b.len);
    kn_buf_free(&b);
}

static void push_verify(const int64_t account, const char *err, const char *name, const char *id) {
    cJSON *fr = social_frame("twitchVerifyResponse"); /* v10 TWITCH_VERIFY_RESPONSE(err, twitchName, twitchId) */
    cJSON_AddItemToArray(fr, err ? cJSON_CreateString(err) : cJSON_CreateNull());
    cJSON_AddItemToArray(fr, name ? cJSON_CreateString(name) : cJSON_CreateNull());
    cJSON_AddItemToArray(fr, id ? cJSON_CreateString(id) : cJSON_CreateNull());
    social_push_account_ex(account, fr, 1);
}

static void twitch_link_done(kn_db_result *res, void *ud) {
    TwitchJob *j = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!o) {
        const char *err = acct_db_error(res, "twitch_link");
        push_verify(j->account, err, NULL, NULL);
        callback_page(j->req, 409, "Twitch Account Not Linked", err);
    } else {
        push_verify(j->account, NULL, kn_json_str(o, "twitch", ""), kn_json_str(o, "twitchId", ""));
        char text[256];
        snprintf(text, sizeof text, "%s is linked to %s. You can close this window and return to the game.", kn_json_str(o, "twitch", ""),
                 kn_json_str(o, "username", ""));
        KN_INFO(MOD, "account %lld linked its Twitch account", (long long) j->account);
        callback_page(j->req, 200, "Twitch Account Linked", text);
    }
    cJSON_Delete(o);
    free(j);
}

static void twitch_job_done(kn_loop *loop, void *ud) {
    (void) loop;
    TwitchJob *j = ud;
    if (j->err[0] || !L.b || !L.b->db) {
        const char *err = j->err[0] ? j->err : TWITCH_UNAVAILABLE;
        push_verify(j->account, err, NULL, NULL);
        callback_page(j->req, 502, "Twitch Account Not Linked", err);
        free(j);
        return;
    }
    char id[24];
    kn_db_call(L.b->db, &P_TWITCH_LINK, KN_DB_ARGS(acct_itoa(id, j->account), j->twitch_id, j->login), twitch_link_done, j);
}

static void *twitch_thread(void *arg) {
    TwitchJob *j = arg;
    twitch_exchange(j);
    memset(j->code, 0, sizeof j->code);
    if (kn_loop_post_threadsafe(L.b->loop, twitch_job_done, j) != 0) free(j); /* the loop is gone: shutting down */
    atomic_fetch_sub(&g_live, 1);
    return NULL;
}

static void state_taken(kn_db_result *res, void *ud) {
    TwitchJob *j = ud;
    cJSON *o = kn_db_ok(res) ? acct_db_json(res) : NULL;
    j->account = kn_json_int(o, "account", 0);
    cJSON_Delete(o);
    if (j->account <= 0) {
        callback_page(j->req, 400, "Twitch Account Not Linked", kn_db_ok(res) ? "This link has expired." : acct_db_error(res, "twitch_state"));
        free(j);
        return;
    }
    pthread_t t;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    atomic_fetch_add(&g_live, 1);
    const int ok = pthread_create(&t, &attr, twitch_thread, j) == 0;
    pthread_attr_destroy(&attr);
    if (!ok) {
        atomic_fetch_sub(&g_live, 1);
        callback_page(j->req, 503, "Twitch Account Not Linked", "Server busy, try again.");
        free(j);
    }
}

static void h_twitch_callback(kn_http_req *req, void *ud) {
    (void) ud;
    char code[512] = "", state[64] = "", error[64] = "";
    kn_http_query_get(req, "error", error, sizeof error);
    const int has_code = kn_http_query_get(req, "code", code, sizeof code) > 0;
    const int has_state = kn_http_query_get(req, "state", state, sizeof state) == 32;
    if (!twitch_configured() || !L.b || !L.b->db) return callback_page(req, 503, "Twitch Account Not Linked", TWITCH_UNAVAILABLE);
    if (error[0] || !has_code) return callback_page(req, 400, "Twitch Account Not Linked", "Twitch linking was cancelled.");
    if (!has_state) return callback_page(req, 400, "Twitch Account Not Linked", "This link has expired.");
    TwitchJob *j = calloc(1, sizeof *j);
    if (!j) return callback_page(req, 503, "Twitch Account Not Linked", "Server busy, try again.");
    j->req = req;
    copy_str(j->code, sizeof j->code, code);
    memset(code, 0, sizeof code);
    kn_db_call(L.b->db, &P_TWITCH_STATE_TAKE, KN_DB_ARGS(state), state_taken, j);
}

/* ================================================================== module */

int account_life_init(Brain *b) {
    memset(&L, 0, sizeof L);
    L.b = b;
    const kn_config *cfg = b->cfg;
    L.grace_s = kn_config_int(cfg, "accounts.deletion_grace_days", 14) * 86400;
    if (L.grace_s < 0) L.grace_s = 0;
    L.purge = kn_config_bool(cfg, "accounts.deletion_purge", 1);
    int64_t sweep_s = kn_config_int(cfg, "accounts.deletion_sweep_s", 3600);
    if (sweep_s < 1) sweep_s = 3600;
    copy_str(L.client_id, sizeof L.client_id, kn_config_str(cfg, "streams.twitch.client_id", ""));
    copy_str(L.client_secret, sizeof L.client_secret, kn_config_str(cfg, "streams.twitch.client_secret", ""));
    copy_str(L.authorize_url, sizeof L.authorize_url, kn_config_str(cfg, "streams.twitch.authorize_url", "https://id.twitch.tv/oauth2/authorize"));
    copy_str(L.token_url, sizeof L.token_url, kn_config_str(cfg, "streams.twitch.auth_url", "https://id.twitch.tv/oauth2/token"));
    copy_str(L.api_url, sizeof L.api_url, kn_config_str(cfg, "streams.twitch.api_url", "https://api.twitch.tv/helix"));
    /* v10's scope (connectTwitchAcc, deob-10.0.3.js:262002) */
    copy_str(L.scope, sizeof L.scope, kn_config_str(cfg, "streams.twitch.link_scope", "user_read viewing_activity_read"));
    const char *redirect = kn_config_str(cfg, "streams.twitch.redirect_uri", "");
    if (redirect[0]) copy_str(L.redirect_uri, sizeof L.redirect_uri, redirect);
    else if (mm_settings()->public_url[0]) snprintf(L.redirect_uri, sizeof L.redirect_uri, "%s/twitch/callback", mm_settings()->public_url);
    L.timeout_ms = (int) kn_config_int(cfg, "streams.timeout_ms", 8000);
    atomic_store(&g_cancel, 0);
    if (b->http) kn_http_route(b->http, "GET", "/twitch/callback", h_twitch_callback, b);
    if (b->db && b->loop && L.purge) L.sweep_timer = kn_loop_add_timer(b->loop, 30000, (uint64_t) sweep_s * 1000, sweep_tick, NULL);
    KN_INFO(MOD, "deletion requests: %lld day grace%s; Twitch linking %s", (long long) (L.grace_s / 86400), L.purge ? "" : " (sweep off)",
            twitch_configured() ? "on" : "off (no Twitch app)");
    return 0;
}

void account_life_shutdown(Brain *b) {
    if (L.sweep_timer && b && b->loop) kn_loop_cancel_timer(b->loop, L.sweep_timer);
    L.sweep_timer = 0;
    atomic_store(&g_cancel, 1);
    for (int i = 0; i < 200 && atomic_load(&g_live) > 0; i++) {
        struct timespec ts = {0, 10 * 1000 * 1000};
        nanosleep(&ts, NULL);
    }
    L.b = NULL;
}
