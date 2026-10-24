/*
 * services/brain/accounts.c - accounts, sessions and stats commits (Krunker
 * v3.1.1 server/accounts.ts + the account dbOps of matchmaker/dbBroker.ts) on
 * any store driver (knsvc/db.h). Schema: services/db/migrations/<dialect>/010x_*.sql;
 * multi-statement operations are kn_procs (db_procs.h, db_accounts.c).
 * Protocol: doc/backend.md "Accounts & social socket".
 */
#include "acct_internal.h"
#include "hot.h"
#include "live_trade.h" /* the game server's live trade ops: trade_inventories, live_trade (economy.c) */
#include "node_events.h" /* the game servers' v10.0.3 event ops: turf_clan, map_spin, studio_game, studio_charge */

#include <ctype.h>
#include <math.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "knsvc/config.h"
#include "knsvc/crypto.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "db_procs.h"
#include "progression.h" /* WP-E part 2c: v7 level progress */

#define MOD "accounts"

AcctConfig g_acct;
Brain *g_acct_brain;

static kn_timer_id s_cleanup_timer;

/* ------------------------------------------------------------------ config */

static int cfg_int(const kn_config *cfg, const char *key, int def) {
    char path[96];
    snprintf(path, sizeof path, "accounts.%s", key);
    return (int) kn_config_int(cfg, path, def);
}

void acct_config_load(Brain *b) {
    const kn_config *cfg = b->cfg;
    AcctConfig *c = &g_acct;
    memset(c, 0, sizeof *c);
    c->name_min = cfg_int(cfg, "name_min", 3);
    /* a new name: v10.0.3's config.maxNameLength 14 (deob-10.0.3.js:22725; the username input is maxlength 14,
     * 129253). Only a new name is held to it (register, a new display name): a stored name of up to 32 characters
     * (KrunkNative allowed 16 before) still signs in, resumes and mirrors (acct_name_stored_ok): no forced renames */
    c->name_max = cfg_int(cfg, "name_max", 14);
    c->pass_min = cfg_int(cfg, "password_min", 6);
    c->pass_max = cfg_int(cfg, "password_max", 100);
    c->alias_min = cfg_int(cfg, "alias_min", 3);
    c->alias_max = cfg_int(cfg, "alias_max", 14); /* the name shown in game: maxNameLength 14, as name_max */
    c->session_ttl_s = (int64_t) cfg_int(cfg, "session_ttl_days", 30) * 86400;
    c->max_sessions = cfg_int(cfg, "max_sessions", 20);
    c->starting_kr = kn_config_int(cfg, "accounts.starting_kr", 0);
    c->free_premium = kn_config_bool(cfg, "accounts.free_premium", 1);
    c->login_throttle_s = cfg_int(cfg, "login_throttle_s", 5);
    c->ip_block_failures = cfg_int(cfg, "ip_block_failures", 100);
    c->ip_block_days = cfg_int(cfg, "ip_block_days", 5);
    c->register_per_ip_hour = cfg_int(cfg, "register_per_ip_hour", 0);
    c->max_match_kr = cfg_int(cfg, "max_match_kr", 500);
    c->rank_var = kn_config_num(cfg, "accounts.rank_var", 0.03);
    c->trade_min_level = cfg_int(cfg, "trade_min_level", 10);   /* v10 tradeMinLvl (v7.1.4: 20) */
    c->gift_min_level = cfg_int(cfg, "gift_min_level", 10);       /* v10 giftMinLVl (v7.1.4: 30) */
    c->gift_fee_percent = cfg_int(cfg, "gift_fee_percent", 10);   /* config.giftFee 10/100 */
    c->gift_min = kn_config_int(cfg, "accounts.gift_min", 10);     /* config.giftMin */
    c->gift_max = kn_config_int(cfg, "accounts.gift_max", 10000000); /* config.giftMax */
    c->market_fee_percent = cfg_int(cfg, "market_fee_percent", 10); /* config.marketFee 10/100 */
    c->market_fee_bypass = kn_config_int(cfg, "accounts.market_fee_bypass", 0); /* config.marketFeeBypass */
    c->market_min_level = cfg_int(cfg, "market_min_level", 10);   /* v10 marketMinLVl (v7.1.4: 15) */
    c->sale_max = kn_config_int(cfg, "accounts.sale_max", 10000000);
    c->sale_grace_s = kn_config_int(cfg, "accounts.sale_grace_s", 300); /* config.saleGrace 300000 ms */
    c->max_listings = cfg_int(cfg, "max_listings", 40);
    c->trade_max_items = cfg_int(cfg, "trade_max_items", 5);     /* config.tradeMaxOut */
    c->trade_max_items_premium = cfg_int(cfg, "trade_max_items_premium", 20); /* config.tradeMaxOutP */
    c->trade_max_open = cfg_int(cfg, "trade_max_open", 50);
    c->trade_kr_max = kn_config_int(cfg, "accounts.trade_kr_max", 10000000);
    c->trade_ttl_s = kn_config_int(cfg, "accounts.trade_ttl_s", kn_config_int(cfg, "accounts.trade_ttl_h", 0) * 3600);
    c->rate_gift = cfg_int(cfg, "economy_rate_gift", 10);
    c->rate_list = cfg_int(cfg, "economy_rate_list", 30);
    c->rate_unlist = cfg_int(cfg, "economy_rate_unlist", 60);
    c->rate_buy = cfg_int(cfg, "economy_rate_buy", 30);
    c->rate_qsell = cfg_int(cfg, "economy_rate_qsell", 60);
    c->rate_trade = cfg_int(cfg, "economy_rate_trade", 10);
    c->rate_accept = cfg_int(cfg, "economy_rate_accept", 30);
    c->rate_close = cfg_int(cfg, "economy_rate_close", 60);
    if (c->gift_fee_percent < 0) c->gift_fee_percent = 0;
    if (c->market_fee_percent < 0) c->market_fee_percent = 0;
    if (c->gift_max <= 0) c->gift_max = 10000000;
    if (c->trade_kr_max < 0) c->trade_kr_max = 0;
    c->clan_tag_max = cfg_int(cfg, "clan_tag_max", 4);
    c->clan_max_members = cfg_int(cfg, "clan_max_members", 0);
    c->post_max_len = cfg_int(cfg, "post_max_len", 1024);
    c->comment_max_len = cfg_int(cfg, "comment_max_len", 500);
    c->chat_max_len = cfg_int(cfg, "chat_max_len", 300);
    c->page_size = cfg_int(cfg, "page_size", 100);
    if (c->name_min < 1) c->name_min = 1;
    if (c->name_max < c->name_min || c->name_max > 32) c->name_max = c->name_max > 32 ? 32 : c->name_min;
    if (c->rank_var <= 0) c->rank_var = 0.03;
    if (c->session_ttl_s <= 0) c->session_ttl_s = 30 * 86400;
    c->dev_count = kn_config_str_list(cfg, "accounts.developers", c->dev_names, 32);
    if (c->dev_count > 32) c->dev_count = 32;
    c->mod_count = kn_config_str_list(cfg, "accounts.moderators", c->mod_names, 32);
    if (c->mod_count > 32) c->mod_count = 32;
    int64_t iters = kn_config_int(cfg, "accounts.password_iterations", 0);
    if (iters > 0) kn_password_set_iterations((unsigned) iters);
}

/* ------------------------------------------------------------------ validation */

static int utf8_len(const char *s) {
    int n = 0;
    for (; *s; s++) n += ((unsigned char) *s & 0xC0) != 0x80;
    return n;
}

const char *acct_validate_name(const char *name) {
    static char msg[96];
    if (!name) return "Invalid username.";
    const int n = (int) strlen(name);
    if (n < g_acct.name_min || n > g_acct.name_max) {
        snprintf(msg, sizeof msg, "Username must be %d\xE2\x80\x93%d characters.", g_acct.name_min, g_acct.name_max);
        return msg;
    }
    for (const char *p = name; *p; p++) {
        if (!isalnum((unsigned char) *p) && *p != '_') return "Username may only contain letters, digits, and underscores.";
    }
    return NULL;
}

/* a name already stored (any length the accounts table holds, up to the 32 the config clamp allows): the charset
 * only, so a name made under an older, longer name_max is never refused where it is looked up or mirrored */
static int acct_name_stored_ok(const char *name) {
    const size_t n = name ? strlen(name) : 0;
    if (n < 1 || n > 32) return 0;
    for (const char *p = name; *p; p++)
        if (!isalnum((unsigned char) *p) && *p != '_') return 0;
    return 1;
}

const char *acct_validate_password(const char *pass) {
    static char msg[96];
    if (!pass) return "Invalid password.";
    if (utf8_len(pass) < g_acct.pass_min) {
        snprintf(msg, sizeof msg, "Password must be \xE2\x89\xA5 %d characters.", g_acct.pass_min);
        return msg;
    }
    if ((int) strlen(pass) > g_acct.pass_max) return "Password is too long.";
    return NULL;
}

const char *acct_validate_alias(const char *alias) {
    static char msg[96];
    if (!alias) return "Invalid display name.";
    while (*alias == ' ') alias++;
    int n = (int) strlen(alias);
    while (n > 0 && alias[n - 1] == ' ') n--;
    if (n < g_acct.alias_min || n > g_acct.alias_max) {
        snprintf(msg, sizeof msg, "Display name must be %d\xE2\x80\x93%d characters.", g_acct.alias_min, g_acct.alias_max);
        return msg;
    }
    for (int i = 0; i < n; i++) {
        const char ch = alias[i];
        if (!isalnum((unsigned char) ch) && ch != '_' && ch != '-' && ch != ' ')
            return "Display name may only contain letters, digits, spaces, _ and -.";
    }
    return NULL;
}

/* Account.updateLevel; levelProg is v7.1.4's getLevelProg (linear in score between the level's
 * bounds, WP-E part 2c; v3.1.1 took the fraction of rankVar * sqrt(score)), truncated here. */
int acct_level(int64_t score, int *prog) {
    if (prog) *prog = (int) progression_level_prog((double) score, g_acct.rank_var);
    return progression_level((double) score, g_acct.rank_var);
}

int accounts_level(int64_t score, int *prog_percent) {
    if (g_acct.rank_var <= 0) g_acct.rank_var = 0.03;
    return acct_level(score, prog_percent);
}

/* ------------------------------------------------------------------ DB helpers */

const char *acct_db_error(kn_db_result *res, const char *ctx) {
    static char msg[256];
    const char *primary = kn_db_user_message(res); /* P0001 refusal: shown to the user */
    if (primary && *primary) {
        snprintf(msg, sizeof msg, "%s", primary);
        return msg;
    }
    KN_ERROR(MOD, "%s: %s (%s)", ctx ? ctx : "query", kn_db_error(res), kn_db_sqlstate(res));
    return "Server error.";
}

cJSON *acct_db_json(kn_db_result *res) {
    if (!kn_db_ok(res) || kn_db_rows(res) < 1 || kn_db_isnull(res, 0, 0)) return NULL;
    const char *text = kn_db_get(res, 0, 0);
    return cJSON_Parse(text);
}

const char *acct_itoa(char buf[24], int64_t v) {
    snprintf(buf, 24, "%lld", (long long) v);
    return buf;
}

static int name_in(const char *name, const char *const *list, size_t n) {
    for (size_t i = 0; name && i < n; i++)
        if (list[i] && !strcasecmp(list[i], name)) return 1;
    return 0;
}

static void set_data_slot(cJSON *data, int idx, cJSON *value) {
    if (cJSON_IsArray(data) && cJSON_GetArraySize(data) > idx) cJSON_ReplaceItemInArray(data, idx, value);
    else cJSON_Delete(value);
}

void acct_decorate(cJSON *account) {
    if (!account) return;
    cJSON *data = cJSON_GetObjectItemCaseSensitive(account, "data");
    const char *name = kn_json_str(account, "username", NULL);
    if (g_acct.free_premium && kn_json_int(account, "premium", 0) < 1) {
        cJSON_ReplaceItemInObject(account, "premium", cJSON_CreateNumber(1));
        set_data_slot(data, 27, cJSON_CreateNumber(1));
    }
    if (name_in(name, g_acct.dev_names, g_acct.dev_count)) {
        cJSON_ReplaceItemInObject(account, "dev", cJSON_CreateNumber(1));
        cJSON_ReplaceItemInObject(account, "mod", cJSON_CreateNumber(1));
        set_data_slot(data, 12, cJSON_CreateNumber(1));
        set_data_slot(data, 21, cJSON_CreateNumber(1));
    } else if (name_in(name, g_acct.mod_names, g_acct.mod_count)) {
        cJSON_ReplaceItemInObject(account, "mod", cJSON_CreateNumber(1));
        set_data_slot(data, 21, cJSON_CreateNumber(1));
    }
    const int64_t score = kn_json_int(account, "score", 0);
    const int level = acct_level(score, NULL);
    cJSON_DeleteItemFromObject(account, "level");
    cJSON_DeleteItemFromObject(account, "levelProg");
    cJSON_AddNumberToObject(account, "level", level);
    cJSON_AddNumberToObject(account, "levelProg", progression_level_prog((double) score, g_acct.rank_var)); /* v7: a float */
    /* JSON booleans -> 0/1 like the rest of the Krunker fields */
    cJSON *banned = cJSON_GetObjectItemCaseSensitive(account, "banned");
    if (cJSON_IsBool(banned)) cJSON_ReplaceItemInObject(account, "banned", cJSON_CreateNumber(cJSON_IsTrue(banned)));
}

/* ------------------------------------------------------------------ hashing worker */

typedef struct HashJob {
    char *password;
    char *encoded;      /* NULL = hash */
    AcctHashFn cb;
    void *ud;
    int ok;
    char new_hash[KN_PASSWORD_HASH_MAX];
} HashJob;

static int s_hash_inflight;
#define HASH_MAX_INFLIGHT 32

static void hash_job_free(HashJob *job) {
    if (!job) return;
    if (job->password) {
        memset(job->password, 0, strlen(job->password));
        free(job->password);
    }
    free(job->encoded);
    memset(job->new_hash, 0, sizeof job->new_hash);
    free(job);
}

static void hash_job_done(kn_loop *loop, void *ud) {
    (void) loop;
    HashJob *job = ud;
    s_hash_inflight--;
    job->cb(job->ud, job->ok, job->new_hash[0] ? job->new_hash : NULL);
    hash_job_free(job);
}

static void *hash_job_run(void *arg) {
    HashJob *job = arg;
    if (job->encoded) {
        job->ok = kn_password_verify(job->password, job->encoded) == 1;
        if (job->ok && kn_password_needs_rehash(job->encoded) == 1 &&
            kn_password_hash(job->password, job->new_hash, sizeof job->new_hash) != 0)
            job->new_hash[0] = 0;
    } else {
        job->ok = kn_password_hash(job->password, job->new_hash, sizeof job->new_hash) == 0;
        if (!job->ok) job->new_hash[0] = 0;
    }
    if (kn_loop_post_threadsafe(g_acct_brain->loop, hash_job_done, job) != 0) {
        /* loop gone (shutdown): nothing can be answered anymore */
        hash_job_free(job);
    }
    return NULL;
}

int acct_hash_async(const char *password, const char *encoded, AcctHashFn cb, void *ud) {
    if (s_hash_inflight >= HASH_MAX_INFLIGHT) return -1;
    HashJob *job = calloc(1, sizeof *job);
    if (!job) return -1;
    job->password = strdup(password ? password : "");
    job->encoded = encoded ? strdup(encoded) : NULL;
    job->cb = cb;
    job->ud = ud;
    if (!job->password || (encoded && !job->encoded)) {
        hash_job_free(job);
        return -1;
    }
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_t th;
    const int rc = pthread_create(&th, &attr, hash_job_run, job);
    pthread_attr_destroy(&attr);
    if (rc != 0) {
        hash_job_free(job);
        return -1;
    }
    s_hash_inflight++;
    return 0;
}

/* ------------------------------------------------------------------ auth (login / register / token) */

typedef struct AuthCtx {
    int action;
    char name[64];      /* login: the username (an email resolves to its account's); the throttle key */
    char email[256];    /* register: the new account's email; login: the email signed in with */
    char *pass;
    char token[80];
    char ref[64];
    char ip[64];
    char origin[16];
    int account_id;
    const char *via;    /* the login audit's method ("password" when NULL, "email_code" for acct_session_issue) */
    int trim;           /* drop the oldest sessions past max_sessions (password logins do; acct_session_issue too) */
    char *twofa_hash;   /* the account's 2FA (TOTP) secret, NULL = no 2FA */
    char twofa[32];     /* code the client gave */
    uint64_t hot_t0;    /* session cache miss: Redis time of the lookup (hot.h) */
    char *rehash;       /* password rehash pending until 2FA passes */
    const char *fail_msg;
    AcctReplyFn cb;
    void *ud;
} AuthCtx;

static void auth_ctx_free(AuthCtx *c) {
    if (!c) return;
    if (c->twofa_hash) memset(c->twofa_hash, 0, strlen(c->twofa_hash));
    free(c->twofa_hash);
    free(c->rehash);
    if (c->pass) {
        memset(c->pass, 0, strlen(c->pass));
        free(c->pass);
    }
    free(c);
}

static void auth_fail(AuthCtx *c, const char *err) {
    c->cb(c->ud, err, NULL);
    auth_ctx_free(c);
}

static const char *ip_param(const AuthCtx *c) { return c->ip[0] ? c->ip : NULL; }

/* Drop the oldest sessions beyond accounts.max_sessions (fire and forget). */
static void trim_cb(kn_db_result *res, void *ud) {
    if (!kn_db_ok(res)) KN_WARN(MOD, "session trim: %s", kn_db_error(res));
    else if (kn_db_affected(res) > 0) hot_session_forget_account((int64_t) (intptr_t) ud);
}

static const kn_sql SQL_TRIM_SESSIONS = {
    .pg = "DELETE FROM sessions WHERE account_id = $1 AND token_hash NOT IN "
          "(SELECT token_hash FROM sessions WHERE account_id = $1 ORDER BY last_seen_at DESC LIMIT $2)",
    .sqlite = "DELETE FROM sessions WHERE account_id = $1 AND token_hash NOT IN "
              "(SELECT token_hash FROM sessions WHERE account_id = $1 ORDER BY last_seen_at DESC LIMIT $2)",
    /* no LIMIT in IN subqueries: delete everything past the newest $2 */
    .mysql = "DELETE s FROM sessions s JOIN (SELECT token_hash FROM sessions WHERE account_id = $1"
             " ORDER BY last_seen_at DESC LIMIT 18446744073709551615 OFFSET $2) old USING (token_hash)"};

static void trim_sessions(int account_id) {
    if (g_acct.max_sessions <= 0) return;
    char id[24], max[24];
    kn_db_query_sql(g_acct_brain->db, &SQL_TRIM_SESSIONS,
                    KN_DB_ARGS(acct_itoa(id, account_id), acct_itoa(max, g_acct.max_sessions)), trim_cb,
                    (void *) (intptr_t) account_id);
}

static const char *ban_message(const cJSON *account) {
    static char msg[256];
    const char *reason = kn_json_str(account, "ban_reason", "");
    if (reason && *reason) snprintf(msg, sizeof msg, "Banned: %s", reason);
    else snprintf(msg, sizeof msg, "Account banned.");
    return msg;
}

/* Final step: account object (with the token when one was issued). */
static void auth_done(kn_db_result *res, void *ud) {
    AuthCtx *c = ud;
    if (!kn_db_ok(res)) {
        auth_fail(c, kn_db_is_unique_violation(res) ? "Username already taken." : acct_db_error(res, "auth"));
        return;
    }
    cJSON *account = acct_db_json(res);
    if (!account) {
        auth_fail(c, "Invalid login.");
        return;
    }
    acct_decorate(account);
    if (kn_json_int(account, "banned", 0)) {
        /* token resumes of banned accounts end here (password logins are refused earlier) */
        const char *msg = ban_message(account);
        cJSON_Delete(account);
        auth_fail(c, msg);
        return;
    }
    if (c->token[0]) cJSON_AddStringToObject(account, "token", c->token);
    if (c->action == 0 || c->pass || c->trim) trim_sessions((int) kn_json_int(account, "id", 0));
    c->cb(c->ud, NULL, account);
    cJSON_Delete(account);
    auth_ctx_free(c);
}

static int new_token(AuthCtx *c) {
    return kn_random_hex(c->token, sizeof c->token, 16); /* 32 hex chars, as v3.1.1 */
}

static void login_success(AuthCtx *c, const char *new_hash) {
    char id[24], ttl[24];
    if (new_token(c) != 0) {
        auth_fail(c, "Server error.");
        return;
    }
    kn_db_call(g_acct_brain->db, &P_LOGIN_SUCCESS,
                KN_DB_ARGS(acct_itoa(id, c->account_id), ip_param(c), c->via ? c->via : "password", c->token,
                           acct_itoa(ttl, g_acct.session_ttl_s), c->origin, new_hash),
                auth_done, c);
}

/* Answer only once the failure is logged, so the next attempt sees the throttle. */
static void login_failed_cb(kn_db_result *res, void *ud) {
    AuthCtx *c = ud;
    if (!kn_db_ok(res)) KN_WARN(MOD, "failure log: %s", kn_db_error(res));
    auth_fail(c, c->fail_msg ? c->fail_msg : "Invalid login.");
}

static void login_failed(AuthCtx *c, const char *msg) {
    c->fail_msg = msg;
    kn_db_call(g_acct_brain->db, &P_LOGIN_FAILED, KN_DB_ARGS(c->name, ip_param(c)), login_failed_cb, c);
}

/* P_TWOFA_LOGIN: a wrong code is counted on the account (10 within 15 minutes lock the 2FA
 * step), not as a login failure: the password was right, so the (name, IP) throttle would only
 * make the player wait before typing the code again. */
static void twofa_checked(kn_db_result *res, void *ud) {
    AuthCtx *c = ud;
    if (!kn_db_ok(res)) {
        auth_fail(c, acct_db_error(res, "2fa"));
        return;
    }
    cJSON *o = acct_db_json(res);
    const int ok = (int) kn_json_int(o, "ok", 0);
    cJSON_Delete(o);
    if (!ok) {
        auth_fail(c, ACCT_ERR_2FA_BAD);
        return;
    }
    login_success(c, c->rehash);
}

static void login_verified(void *ud, int ok, const char *new_hash) {
    AuthCtx *c = ud;
    if (!ok) {
        login_failed(c, "Invalid login.");
        return;
    }
    if (c->twofa_hash) {
        /* v7.1.4 2FA: the authenticator code comes after a correct password */
        if (!c->twofa[0]) {
            auth_fail(c, ACCT_ERR_2FA);
            return;
        }
        if (new_hash) c->rehash = strdup(new_hash);
        char id[24], now[24];
        kn_db_call(g_acct_brain->db, &P_TWOFA_LOGIN,
                   KN_DB_ARGS(acct_itoa(id, c->account_id), c->twofa, acct_itoa(now, (int64_t) time(NULL))), twofa_checked, c);
        return;
    }
    login_success(c, new_hash);
}

/* A constant dummy hash so unknown names cost the same as wrong passwords. */
static char s_dummy_hash[KN_PASSWORD_HASH_MAX];

static void login_lookup_done(kn_db_result *res, void *ud) {
    AuthCtx *c = ud;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        auth_fail(c, acct_db_error(res, "login lookup"));
        return;
    }
    if (kn_db_get_bool(res, 0, kn_db_col(res, "ip_blocked"), 0)) {
        auth_fail(c, "Too many failed logins for this IP");
        return;
    }
    if (kn_db_get_bool(res, 0, kn_db_col(res, "throttled"), 0)) {
        auth_fail(c, "Please Try Later");
        return;
    }
    const int id_col = kn_db_col(res, "account_id");
    const int found = !kn_db_isnull(res, 0, id_col);
    c->account_id = found ? (int) kn_db_get_int64(res, 0, id_col, 0) : 0;
    if (found && kn_db_get_bool(res, 0, kn_db_col(res, "banned"), 0)) {
        static char msg[256];
        const char *reason = kn_db_get_by(res, 0, "ban_reason");
        if (reason && *reason) snprintf(msg, sizeof msg, "Banned: %s", reason);
        else snprintf(msg, sizeof msg, "Account banned.");
        auth_fail(c, msg);
        return;
    }
    const char *hash = found ? kn_db_get_by(res, 0, "password_hash") : NULL;
    const int tf = kn_db_col(res, "twofa");
    if (found && tf >= 0 && !kn_db_isnull(res, 0, tf)) c->twofa_hash = strdup(kn_db_get(res, 0, tf));
    if (!found) {
        snprintf(s_dummy_hash, sizeof s_dummy_hash, "$pbkdf2-sha256$i=%u$a3J1bmtuYXRpdmVkdW1teQ$"
                 "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA", kn_password_get_iterations());
        /* still pay for one PBKDF2 so timing does not reveal registered names */
        if (acct_hash_async(c->pass, s_dummy_hash, login_verified, c) != 0) auth_fail(c, "Server busy, try again.");
        else c->account_id = 0;
        return;
    }
    if (acct_hash_async(c->pass, hash, login_verified, c) != 0) auth_fail(c, "Server busy, try again.");
}

static void token_lookup_done(kn_db_result *res, void *ud) {
    AuthCtx *c = ud;
    if (!kn_db_ok(res)) {
        auth_fail(c, acct_db_error(res, "token lookup"));
        return;
    }
    cJSON *account = acct_db_json(res);
    if (!account) {
        auth_fail(c, "Invalid login.");
        return;
    }
    const char *uname = kn_json_str(account, "username", "");
    if (c->name[0] && strcasecmp(uname, c->name) != 0) {
        cJSON_Delete(account);
        auth_fail(c, "Invalid login.");
        return;
    }
    acct_decorate(account);
    if (kn_json_int(account, "banned", 0)) {
        const char *msg = ban_message(account);
        cJSON_Delete(account);
        auth_fail(c, msg);
        return;
    }
    if (c->hot_t0) hot_session_store(c->token, kn_json_int(account, "id", 0), c->hot_t0);
    cJSON_AddStringToObject(account, "token", c->token); /* keep the client's token (socialWs.ts) */
    c->cb(c->ud, NULL, account);
    cJSON_Delete(account);
    auth_ctx_free(c);
}

static void register_hashed(void *ud, int ok, const char *new_hash) {
    AuthCtx *c = ud;
    char kr[24], prem[24], ttl[24];
    if (!ok || !new_hash) {
        auth_fail(c, "Server error.");
        return;
    }
    if (new_token(c) != 0) {
        auth_fail(c, "Server error.");
        return;
    }
    kn_db_call(g_acct_brain->db, &P_REGISTER,
                KN_DB_ARGS(c->name, new_hash, ip_param(c), c->ref[0] ? c->ref : NULL,
                           acct_itoa(kr, g_acct.starting_kr), acct_itoa(prem, g_acct.free_premium ? 1 : 0),
                           c->token, acct_itoa(ttl, g_acct.session_ttl_s), c->origin),
                auth_done, c);
}

static void register_precheck_done(kn_db_result *res, void *ud) {
    AuthCtx *c = ud;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        auth_fail(c, acct_db_error(res, "register precheck"));
        return;
    }
    if (kn_db_get_bool(res, 0, kn_db_col(res, "taken"), 0)) {
        auth_fail(c, "Username already taken.");
        return;
    }
    if (kn_db_get_bool(res, 0, kn_db_col(res, "banned"), 0)) {
        auth_fail(c, "Banned from this server.");
        return;
    }
    if (kn_db_get_bool(res, 0, kn_db_col(res, "limited"), 0)) {
        auth_fail(c, "Too many accounts created from this IP. Please Try Later");
        return;
    }
    /* one account per email (the login modal signs in with it); the brain stores it once the
     * account exists (acct_email_after_register), where the same check runs again */
    if (kn_db_get_bool(res, 0, kn_db_col(res, "email_taken"), 0)) {
        auth_fail(c, ACCT_ERR_EMAIL_TAKEN);
        return;
    }
    if (acct_hash_async(c->pass, NULL, register_hashed, c) != 0) auth_fail(c, "Server busy, try again.");
}

/* Sign in with an email (Krunker v10's login modal: "Email" / "(use username instead)"): the
 * account holding the address. Registering and "cemail" refuse an address another account has,
 * but older rows may share one (0109's password reset mails up to 3 accounts per address): a
 * verified holder first, then the oldest. Indexed by 0122_email_login. MySQL compares in the
 * column's case-insensitive collation (its index is on email itself). */
static const kn_sql SQL_LOGIN_EMAIL = {
    .mysql = "SELECT username FROM accounts WHERE email = $1 ORDER BY (email_verified_at IS NULL), id LIMIT 1",
    .any = "SELECT username FROM accounts WHERE lower(email) = lower($1) ORDER BY (email_verified_at IS NULL), id LIMIT 1"};

static void login_lookup(AuthCtx *c) {
    char th[24], bm[24], bd[24];
    kn_db_call(g_acct_brain->db, &P_LOGIN_LOOKUP,
                KN_DB_ARGS(c->name, ip_param(c), acct_itoa(th, g_acct.login_throttle_s),
                           acct_itoa(bm, g_acct.ip_block_failures), acct_itoa(bd, g_acct.ip_block_days)),
                login_lookup_done, c);
}

/* An unknown email goes on as its own (truncated) name: no username holds an '@'
 * (acct_validate_name), so the lookup finds nobody, pays the dummy PBKDF2 and answers
 * "Invalid login." like a wrong password; the (name, IP) throttle and the IP block count it. */
static void login_email_done(kn_db_result *res, void *ud) {
    AuthCtx *c = ud;
    if (!kn_db_ok(res)) {
        auth_fail(c, acct_db_error(res, "login email"));
        return;
    }
    const char *user = kn_db_rows(res) > 0 && !kn_db_isnull(res, 0, 0) ? kn_db_get(res, 0, 0) : NULL;
    if (user && *user) snprintf(c->name, sizeof c->name, "%s", user);
    login_lookup(c);
}

/* Token resume: the hot tier's session cache first (a hit reads the account without
 * touching the sessions row), else the database (P_SESSION_ACCOUNT slides the expiry). */
static void token_session_db(AuthCtx *c) {
    char ttl[24];
    kn_db_call(g_acct_brain->db, &P_SESSION_ACCOUNT, KN_DB_ARGS(c->token, acct_itoa(ttl, g_acct.session_ttl_s)),
               token_lookup_done, c);
}

static void token_cache_done(void *ud, int64_t account_id, uint64_t t0_us) {
    AuthCtx *c = ud;
    if (account_id > 0) {
        char id[24];
        c->hot_t0 = 0;
        kn_db_call(g_acct_brain->db, &P_ACCOUNT_JSON, KN_DB_ARGS(acct_itoa(id, account_id)), token_lookup_done, c);
        return;
    }
    c->hot_t0 = t0_us;
    token_session_db(c);
}

void acct_auth(const AcctAuthReq *rq, AcctReplyFn cb, void *ud) {
    if (!g_acct_brain || !g_acct_brain->db) {
        cb(ud, "Accounts are unavailable.", NULL);
        return;
    }
    AuthCtx *c = calloc(1, sizeof *c);
    if (!c) {
        cb(ud, "Server error.", NULL);
        return;
    }
    c->action = rq->action;
    c->cb = cb;
    c->ud = ud;
    snprintf(c->name, sizeof c->name, "%s", rq->name ? rq->name : "");
    snprintf(c->ref, sizeof c->ref, "%s", rq->ref ? rq->ref : "");
    snprintf(c->ip, sizeof c->ip, "%s", rq->ip ? rq->ip : "");
    snprintf(c->origin, sizeof c->origin, "%s", rq->origin ? rq->origin : "social");
    snprintf(c->twofa, sizeof c->twofa, "%s", rq->twofa ? rq->twofa : "");

    if (rq->action == 0) {
        const char *err = acct_validate_name(rq->name);
        if (!err) err = acct_validate_password(rq->pass);
        if (err) {
            auth_fail(c, err);
            return;
        }
        c->pass = strdup(rq->pass);
        snprintf(c->email, sizeof c->email, "%s", rq->email ? rq->email : "");
        char lim[24];
        kn_db_call(g_acct_brain->db, &P_REGISTER_PRECHECK,
                    KN_DB_ARGS(c->name, ip_param(c), acct_itoa(lim, g_acct.register_per_ip_hour),
                               c->email[0] ? c->email : NULL),
                    register_precheck_done, c);
        return;
    }

    if (rq->pass && *rq->pass) {
        if (!rq->name || !*rq->name) {
            auth_fail(c, "Invalid credentials.");
            return;
        }
        if ((int) strlen(rq->pass) > g_acct.pass_max) {
            auth_fail(c, "Invalid login.");
            return;
        }
        c->pass = strdup(rq->pass);
        if (strchr(rq->name, '@')) {
            /* the login field takes a username or an email: the email, trimmed and in lower case */
            const char *s = rq->name;
            while (*s == ' ' || *s == '\t') s++;
            size_t n = strlen(s);
            while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t')) n--;
            if (n >= sizeof c->email) n = sizeof c->email - 1;
            for (size_t i = 0; i < n; i++) c->email[i] = (char) tolower((unsigned char) s[i]);
            c->email[n] = 0;
            snprintf(c->name, sizeof c->name, "%.*s", (int) (sizeof c->name - 1), c->email);
            kn_db_query_sql(g_acct_brain->db, &SQL_LOGIN_EMAIL, KN_DB_ARGS(c->email), login_email_done, c);
            return;
        }
        login_lookup(c);
        return;
    }

    if (rq->token && *rq->token && strlen(rq->token) < sizeof c->token) {
        snprintf(c->token, sizeof c->token, "%s", rq->token);
        if (hot_session_lookup(c->token, token_cache_done, c) != 0) token_session_db(c);
        return;
    }
    auth_fail(c, rq->name ? "Invalid login." : "Invalid credentials.");
}

/* The session of an account that passed another sign-in step (the emailed code, v10_auth.c): login_success
 * with the same audit and session rows as a password login, and the same account reply. */
void acct_session_issue(int64_t account_id, const char *ip, AcctReplyFn cb, void *ud) {
    if (!g_acct_brain || !g_acct_brain->db || account_id <= 0) {
        cb(ud, "Accounts are unavailable.", NULL);
        return;
    }
    AuthCtx *c = calloc(1, sizeof *c);
    if (!c) {
        cb(ud, "Server error.", NULL);
        return;
    }
    c->action = 1;
    c->account_id = (int) account_id;
    c->via = "email_code";
    c->trim = 1;
    c->cb = cb;
    c->ud = ud;
    snprintf(c->ip, sizeof c->ip, "%s", ip ? ip : "");
    snprintf(c->origin, sizeof c->origin, "%s", "http");
    login_success(c, NULL);
}

typedef struct RevokeCtx {
    char token[80];
    void (*done)(void *ud);
    void *ud;
} RevokeCtx;

static void revoke_cb(kn_db_result *res, void *ud) {
    RevokeCtx *r = ud;
    if (!kn_db_ok(res)) KN_WARN(MOD, "logout: %s", kn_db_error(res));
    else hot_session_forget(r->token); /* after the commit: hot.h */
    if (r->done) r->done(r->ud);
    memset(r->token, 0, sizeof r->token);
    free(r);
}

void acct_revoke_token_then(const char *token, void (*done)(void *ud), void *ud) {
    RevokeCtx *r = token && *token && strlen(token) < sizeof r->token && g_acct_brain && g_acct_brain->db
                       ? calloc(1, sizeof *r) : NULL;
    if (!r) {
        if (done) done(ud);
        return;
    }
    snprintf(r->token, sizeof r->token, "%s", token);
    r->done = done;
    r->ud = ud;
    static const kn_sql q = {.pg = "DELETE FROM sessions WHERE token_hash = sha256(convert_to($1, 'UTF8'))",
                             .any = "DELETE FROM sessions WHERE token_hash = {hash:$1}"};
    kn_db_query_sql(g_acct_brain->db, &q, KN_DB_ARGS(r->token), revoke_cb, r);
}

void acct_revoke_token(const char *token) { acct_revoke_token_then(token, NULL, NULL); }

/* ------------------------------------------------------------------ internal ops (game servers) */

typedef struct OpCtx {
    AcctReplyFn cb;
    void *ud;
    char what[24];
    int64_t id;
    char *pass_old, *pass_new;
    char token[80];
    char *progress; /* WP-E part 2c: the commit's P_PROGRESS payload (op "stats") */
    char *items;    /* the commit's itemRewards (v10.0.3 event kill pools), JSON, NULL none */
    char match[160];
} OpCtx;

static void op_ctx_free(OpCtx *o) {
    if (!o) return;
    if (o->pass_old) {
        memset(o->pass_old, 0, strlen(o->pass_old));
        free(o->pass_old);
    }
    if (o->pass_new) {
        memset(o->pass_new, 0, strlen(o->pass_new));
        free(o->pass_new);
    }
    free(o->progress);
    free(o->items);
    free(o);
}

static OpCtx *op_new(AcctReplyFn cb, void *ud, const char *what) {
    OpCtx *o = calloc(1, sizeof *o);
    if (!o) {
        cb(ud, "Server error.", NULL);
        return NULL;
    }
    o->cb = cb;
    o->ud = ud;
    snprintf(o->what, sizeof o->what, "%s", what);
    return o;
}

static void op_reply(OpCtx *o, const char *err, const cJSON *data) {
    o->cb(o->ud, err, data);
    op_ctx_free(o);
}

/* Result is one jsonb column holding an account object (decorated before replying). */
static void op_account_done(kn_db_result *res, void *ud) {
    OpCtx *o = ud;
    if (!kn_db_ok(res)) {
        op_reply(o, acct_db_error(res, o->what), NULL);
        return;
    }
    cJSON *account = acct_db_json(res);
    if (!account) {
        op_reply(o, "Account not found.", NULL);
        return;
    }
    acct_decorate(account);
    op_reply(o, NULL, account);
    cJSON_Delete(account);
}

/* Result is one jsonb column passed through as-is ({} when NULL). */
static void op_json_done(kn_db_result *res, void *ud) {
    OpCtx *o = ud;
    if (!kn_db_ok(res)) {
        op_reply(o, acct_db_error(res, o->what), NULL);
        return;
    }
    cJSON *data = acct_db_json(res);
    if (!data) data = cJSON_CreateObject();
    op_reply(o, NULL, data);
    cJSON_Delete(data);
}

/* junk_credit / junk_spend (db_junk.c): the reply, and the balance pushed to the account's social
 * sockets, so the navbar follows a credit the game server asked for (doc/v10/junk.md). */
static void junk_op_done(kn_db_result *res, void *ud) {
    OpCtx *o = ud;
    if (!kn_db_ok(res)) {
        op_reply(o, acct_db_error(res, o->what), NULL);
        return;
    }
    cJSON *data = acct_db_json(res);
    if (!data) data = cJSON_CreateObject();
    const cJSON *junk = cJSON_GetObjectItemCaseSensitive(data, "junk");
    if (cJSON_IsString(junk)) social_push_junk(o->id, junk->valuestring);
    op_reply(o, NULL, data);
    cJSON_Delete(data);
}

static void op_empty_done(kn_db_result *res, void *ud) {
    OpCtx *o = ud;
    if (!kn_db_ok(res)) {
        op_reply(o, acct_db_error(res, o->what), NULL);
        return;
    }
    cJSON *data = cJSON_CreateObject();
    op_reply(o, NULL, data);
    cJSON_Delete(data);
}

typedef struct AuthBridge {
    AcctReplyFn cb;
    void *ud;
} AuthBridge;

static void auth_bridge_cb(void *ud, const char *err, const cJSON *data) {
    AuthBridge *b = ud;
    b->cb(b->ud, err, data);
    free(b);
}

static void internal_auth(int action, const cJSON *args, AcctReplyFn cb, void *ud) {
    AuthBridge *bridge = malloc(sizeof *bridge);
    if (!bridge) {
        cb(ud, "Server error.", NULL);
        return;
    }
    bridge->cb = cb;
    bridge->ud = ud;
    AcctAuthReq rq = {
        .action = action,
        .name = kn_json_str(args, "name", kn_json_str(args, "username", NULL)),
        .pass = kn_json_str(args, "pass", kn_json_str(args, "password", NULL)),
        .token = kn_json_str(args, "token", NULL),
        .ref = kn_json_str(args, "ref", NULL),
        .ip = kn_json_str(args, "ip", NULL),
        .origin = "game",
    };
    if (action == 1 && rq.pass) rq.token = NULL;
    acct_auth(&rq, auth_bridge_cb, bridge);
}

static void password_set_done(kn_db_result *res, void *ud) {
    OpCtx *o = ud;
    if (kn_db_ok(res)) hot_session_forget_account(o->id); /* every older session was revoked */
    op_json_done(res, ud);
}

static void password_rehashed(void *ud, int ok, const char *new_hash) {
    OpCtx *o = ud;
    char id[24], ttl[24];
    if (!ok || !new_hash) {
        op_reply(o, "Server error.", NULL);
        return;
    }
    if (kn_random_hex(o->token, sizeof o->token, 16) != 0) {
        op_reply(o, "Server error.", NULL);
        return;
    }
    /* new hash, every other session revoked, one fresh session returned */
    kn_db_call(g_acct_brain->db, &P_SET_PASSWORD,
                KN_DB_ARGS(acct_itoa(id, o->id), new_hash, o->token, acct_itoa(ttl, g_acct.session_ttl_s)),
                password_set_done, o);
}

static void password_verified(void *ud, int ok, const char *new_hash) {
    (void) new_hash;
    OpCtx *o = ud;
    if (!ok) {
        op_reply(o, "Incorrect current password.", NULL);
        return;
    }
    if (acct_hash_async(o->pass_new, NULL, password_rehashed, o) != 0) op_reply(o, "Server busy, try again.", NULL);
}

static void password_lookup_done(kn_db_result *res, void *ud) {
    OpCtx *o = ud;
    if (!kn_db_ok(res) || kn_db_rows(res) < 1) {
        op_reply(o, kn_db_ok(res) ? "Account not found." : acct_db_error(res, "password"), NULL);
        return;
    }
    if (acct_hash_async(o->pass_old, kn_db_get(res, 0, 0), password_verified, o) != 0)
        op_reply(o, "Server busy, try again.", NULL);
}

/* Change password (game "upp" / social "upp"): verifies the old one. */
void acct_change_password(int64_t id, const char *old_pass, const char *new_pass, AcctReplyFn cb, void *ud) {
    const char *err = acct_validate_password(new_pass);
    if (err) {
        cb(ud, err, NULL);
        return;
    }
    if (!old_pass) {
        cb(ud, "Incorrect current password.", NULL);
        return;
    }
    OpCtx *o = op_new(cb, ud, "password");
    if (!o) return;
    o->id = id;
    o->pass_old = strdup(old_pass);
    o->pass_new = strdup(new_pass);
    char ids[24];
    static const kn_sql q = {.any = "SELECT password_hash FROM accounts WHERE id = $1"};
    kn_db_query_sql(g_acct_brain->db, &q, KN_DB_ARGS(acct_itoa(ids, id)), password_lookup_done, o);
}

void acct_set_alias(int64_t id, const char *alias, AcctReplyFn cb, void *ud) {
    const char *err = acct_validate_alias(alias);
    if (err) {
        cb(ud, err, NULL);
        return;
    }
    char trimmed[64];
    while (*alias == ' ') alias++;
    snprintf(trimmed, sizeof trimmed, "%s", alias);
    for (size_t n = strlen(trimmed); n > 0 && trimmed[n - 1] == ' '; n--) trimmed[n - 1] = 0;
    OpCtx *o = op_new(cb, ud, "alias");
    if (!o) return;
    char ids[24];
    kn_db_call(g_acct_brain->db, &P_SET_ALIAS, KN_DB_ARGS(acct_itoa(ids, id), trimmed), op_account_done, o);
}

void acct_get(int64_t id, AcctReplyFn cb, void *ud) {
    OpCtx *o = op_new(cb, ud, "get");
    if (!o) return;
    char ids[24];
    kn_db_call(g_acct_brain->db, &P_ACCOUNT_JSON, KN_DB_ARGS(acct_itoa(ids, id)), op_account_done, o);
}

void acct_ensure_central(int64_t central_id, const char *username, AcctReplyFn cb, void *ud) {
    if (!g_acct_brain || !g_acct_brain->db) {
        cb(ud, "Accounts are unavailable.", NULL);
        return;
    }
    if (central_id <= 0 || central_id > INT32_MAX || !acct_name_stored_ok(username)) { /* a stored central name */
        cb(ud, "Invalid account.", NULL);
        return;
    }
    OpCtx *o = op_new(cb, ud, "ensure_central");
    if (!o) return;
    char ids[24];
    kn_db_call(g_acct_brain->db, &P_ENSURE_CENTRAL, KN_DB_ARGS(acct_itoa(ids, central_id), username), op_account_done, o);
}

/* Normalise the two stats spellings (gameserver / matchmaker) into kn_commit_stats' input. */
static cJSON *stats_args(const cJSON *a) {
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "id", (double) kn_json_int(a, "id", 0));
    const char *match = kn_json_str(a, "match", kn_json_str(a, "match_id", NULL));
    if (match) cJSON_AddStringToObject(p, "match", match);
    cJSON_AddNumberToObject(p, "kills", (double) kn_json_int(a, "kills", 0));
    cJSON_AddNumberToObject(p, "deaths", (double) kn_json_int(a, "deaths", 0));
    cJSON_AddNumberToObject(p, "score", (double) kn_json_int(a, "score", 0));
    cJSON_AddNumberToObject(p, "xp", (double) kn_json_int(a, "xp", kn_json_int(a, "score", 0)));
    cJSON_AddNumberToObject(p, "won", kn_json_bool(a, "won", 0));
    cJSON_AddNumberToObject(p, "games", (double) kn_json_int(a, "games", 0));
    cJSON_AddNumberToObject(p, "time_ms", (double) kn_json_int(a, "time_ms", kn_json_int(a, "timePlayed", 0)));
    cJSON_AddNumberToObject(p, "kr", (double) kn_json_int(a, "kr", 0));
    cJSON_AddNumberToObject(p, "comp", (double) kn_json_bool(a, "comp", 0)); /* v10 isComp: no end-screen 2x XP (double_xp.h) */
    if (cJSON_GetObjectItemCaseSensitive(a, "classDouble")) /* the round's class XP flag (double_xp.h), when the server sent it */
        cJSON_AddNumberToObject(p, "classDouble", (double) kn_json_bool(a, "classDouble", 0));
    const int64_t cls = kn_json_int(a, "class", kn_json_int(a, "classIndex", -1));
    if (cls >= 0) cJSON_AddNumberToObject(p, "class", (double) cls);
    const char *keys[] = {"mode", "map", "region", "server"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, keys[i]);
        if (cJSON_IsString(v)) cJSON_AddStringToObject(p, keys[i], v->valuestring);
        else if (cJSON_IsNumber(v)) cJSON_AddNumberToObject(p, keys[i], v->valuedouble);
    }
    if (cJSON_HasObjectItem(a, "region_index")) {
        cJSON_AddNumberToObject(p, "region_index", (double) kn_json_int(a, "region_index", 0));
    } else {
        /* accounts.region_index maps game regions to config.socialRegions (leaderboard filter) */
        const char *region = kn_json_str(a, "region", NULL);
        char path[128];
        if (region && strlen(region) < 64 && !strchr(region, '.')) {
            snprintf(path, sizeof path, "accounts.region_index.%s", region);
            const int64_t idx = kn_config_int(g_acct_brain->cfg, path, -1);
            if (idx >= 0) cJSON_AddNumberToObject(p, "region_index", (double) idx);
        }
    }
    const cJSON *st = cJSON_GetObjectItemCaseSensitive(a, "stats");
    if (cJSON_IsObject(st)) {
        /* only numeric counters (hs, wb, mk, fk, n, tmk, s, h ...) */
        cJSON *clean = cJSON_CreateObject();
        const cJSON *it;
        cJSON_ArrayForEach(it, st) {
            /* not the class XP keys c<n> (the commit's class / xp set them); v7's crc, cad, ...
             * are counters (WP-E part 2c) */
            const int class_key = it->string && it->string[0] == 'c' && it->string[1] &&
                                  strspn(it->string + 1, "0123456789") == strlen(it->string + 1);
            if (cJSON_IsNumber(it) && it->string && strlen(it->string) < 16 && !class_key && it->valuedouble >= 0)
                cJSON_AddNumberToObject(clean, it->string, it->valuedouble);
        }
        cJSON_AddItemToObject(p, "stats", clean);
    }
    return p;
}

/* WP-E part 2c: P_PROGRESS after a commit -> the account's social sockets get v7's match-end
 * frames: ["chg", [done, CI, PV, ...]], ["chgC", [challenge...]] (KrunkNative: the completed
 * challenge objects, v7 sends one index per frame), ["chlR", skins, funds, junk, jnk, kr] and
 * ["nbdgr", [badge...]]. */
typedef struct {
    int64_t id;
} ProgressCtx;

static void progress_done(kn_db_result *res, void *ud) {
    ProgressCtx *c = ud;
    cJSON *out = kn_db_ok(res) ? acct_db_json(res) : NULL;
    if (!kn_db_ok(res)) KN_WARN("accounts", "progress for account %lld: %s", (long long) c->id, acct_db_error(res, "progress"));
    const cJSON *results = cJSON_GetObjectItemCaseSensitive(out, "results");
    const cJSON *completed = cJSON_GetObjectItemCaseSensitive(out, "completed");
    const cJSON *reward = cJSON_GetObjectItemCaseSensitive(out, "reward");
    const cJSON *badges = cJSON_GetObjectItemCaseSensitive(out, "newBadges");
    if (cJSON_GetArraySize(results) > 0) {
        cJSON *f = social_frame("chg");
        cJSON_AddItemToArray(f, cJSON_Duplicate(results, 1));
        social_push_account(c->id, f);
    }
    if (cJSON_GetArraySize(completed) > 0) {
        cJSON *f = social_frame("chgC");
        cJSON_AddItemToArray(f, cJSON_Duplicate(completed, 1));
        social_push_account(c->id, f);
    }
    const cJSON *skins = cJSON_GetObjectItemCaseSensitive(reward, "skins");
    const double kr = kn_json_int(reward, "kr", 0);
    if (kr > 0 || cJSON_GetArraySize(skins) > 0) {
        cJSON *f = social_frame("chlR");
        cJSON_AddItemToArray(f, skins ? cJSON_Duplicate(skins, 1) : cJSON_CreateArray());
        cJSON_AddItemToArray(f, cJSON_CreateNumber((double) kn_json_int(out, "funds", 0)));
        cJSON_AddItemToArray(f, cJSON_CreateNumber(0)); /* junk: not in KrunkNative's economy */
        cJSON_AddItemToArray(f, cJSON_CreateNumber(0));
        cJSON_AddItemToArray(f, cJSON_CreateNumber(kr));
        social_push_account(c->id, f);
    }
    if (cJSON_GetArraySize(badges) > 0) {
        cJSON *f = social_frame("nbdgr");
        cJSON_AddItemToArray(f, cJSON_Duplicate(badges, 1));
        social_push_account(c->id, f);
    }
    /* v7.1.4 Battle Pass: battlePass.progress -> "bppd" (account.BP) and endGame's tmpBPData
     * (v7 sends it with the "end" packet; KrunkNative's game server does not hold the pass, so
     * the end screen's #endBPLayout gets it from here: ["bpEnd", tmpBPData]) */
    const cJSON *bp_progress = cJSON_GetObjectItemCaseSensitive(out, "bpProgress");
    if (cJSON_IsObject(bp_progress)) {
        cJSON *f = social_frame("bppd");
        cJSON_AddItemToArray(f, cJSON_Duplicate(bp_progress, 1));
        social_push_account(c->id, f);
    }
    const cJSON *bp = cJSON_GetObjectItemCaseSensitive(out, "bp");
    if (cJSON_IsObject(bp)) {
        cJSON *f = social_frame("bpEnd");
        cJSON_AddItemToArray(f, cJSON_Duplicate(bp, 1));
        social_push_account(c->id, f);
    }
    cJSON_Delete(out);
    free(c);
}

static void progress_run(int64_t id, const char *payload) {
    ProgressCtx *c = calloc(1, sizeof *c);
    if (!c) return;
    c->id = id;
    char now[24], season[24];
    kn_db_call(g_acct_brain->db, &P_PROGRESS,
               KN_DB_ARGS(payload, acct_itoa(now, (int64_t) time(NULL)), acct_itoa(season, acct_current_season())), progress_done, c);
}

/* The P_PROGRESS payload of a commit from a v7 game server ("prog" / "medals" present), NULL else. */
static char *progress_payload(const cJSON *a) {
    const cJSON *prog = cJSON_GetObjectItemCaseSensitive(a, "prog"), *medals = cJSON_GetObjectItemCaseSensitive(a, "medals");
    if (!cJSON_IsArray(prog) && !cJSON_IsObject(medals)) return NULL;
    cJSON *p = cJSON_CreateObject();
    cJSON_AddNumberToObject(p, "id", (double) kn_json_int(a, "id", 0));
    /* + the season part (db_season.c): the match BPP and the season_stats totals */
    const char *keys[] = {"prog", "medals", "mapIndex", "mapId", "mode", "classIndex", "ranked", "ltOff", "final", "match",
                          "bpp", "kills", "deaths", "score", "won", "games", "timePlayed"};
    for (size_t i = 0; i < sizeof keys / sizeof keys[0]; i++) {
        const cJSON *v = cJSON_GetObjectItemCaseSensitive(a, keys[i]);
        if (v) cJSON_AddItemToObject(p, keys[i], cJSON_Duplicate(v, 1));
    }
    if (!cJSON_HasObjectItem(p, "classIndex") && cJSON_HasObjectItem(a, "class"))
        cJSON_AddNumberToObject(p, "classIndex", (double) kn_json_int(a, "class", -1));
    char *text = kn_json_print(p);
    cJSON_Delete(p);
    return text;
}

static void kill_item_done(kn_db_result *res, void *ud) {
    const int64_t id = (int64_t) (intptr_t) ud;
    if (!kn_db_ok(res)) KN_WARN("accounts", "kill pool item for account %lld: %s", (long long) id, acct_db_error(res, "give_skin_n"));
}

/* A match's event kill pool items (Krunker v10.0.3 updateAccounts {skins: [{id, quantity}]},
 * deob-10.0.3.js:217260-217282; the game server drew them, shared/src/kill_pools.c): into the inventory
 * once (the commit's duplicate check), at most 30 a match (endGame draws once per kill, up to 30). */
static void kill_items_grant(const int64_t id, const char *items, const char *match) {
    cJSON *list = items ? cJSON_Parse(items) : NULL;
    int left = 30;
    const cJSON *it;
    cJSON_ArrayForEach(it, list) {
        const int64_t skin = kn_json_int(it, "id", -1);
        int64_t quantity = kn_json_int(it, "quantity", 1);
        if (skin <= 0 || (acct_skin_count() > 0 && skin >= acct_skin_count()) || quantity <= 0) continue;
        if (quantity > left) quantity = left;
        if (quantity <= 0) break;
        left -= (int) quantity;
        char ids[24], skins[24], q[24];
        kn_db_call(g_acct_brain->db, &P_GIVE_SKIN_N,
                   KN_DB_ARGS(acct_itoa(ids, id), acct_itoa(skins, skin), acct_itoa(q, quantity), "killpool", match),
                   kill_item_done, (void *) (intptr_t) id);
    }
    cJSON_Delete(list);
}

static void op_stats_done(kn_db_result *res, void *ud) {
    OpCtx *o = ud;
    if (!kn_db_ok(res)) {
        op_reply(o, acct_db_error(res, o->what), NULL);
        return;
    }
    cJSON *account = acct_db_json(res);
    if (!account) {
        op_reply(o, "Account not found.", NULL);
        return;
    }
    acct_decorate(account);
    const int duplicate = (int) kn_json_int(account, "duplicate", 0);
    if (!duplicate) social_push_account_update(account); /* live header KR / level */
    char *progress = o->progress;
    o->progress = NULL;
    char *items = o->items;
    o->items = NULL;
    char match[sizeof o->match];
    snprintf(match, sizeof match, "%s", o->match);
    const int64_t id = o->id;
    op_reply(o, NULL, account);
    cJSON_Delete(account);
    if (progress && !duplicate) progress_run(id, progress); /* WP-E part 2c: challenges, medals, badges */
    if (items && !duplicate) kill_items_grant(id, items, match);
    free(progress);
    free(items);
}

static int op_stats(const cJSON *args, AcctReplyFn cb, void *ud) {
    if (kn_json_int(args, "id", 0) <= 0) {
        cb(ud, "Account not found.", NULL);
        return -1;
    }
    OpCtx *o = op_new(cb, ud, "stats");
    if (!o) return -1;
    o->id = kn_json_int(args, "id", 0);
    o->progress = progress_payload(args);
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(args, "itemRewards");
    if (cJSON_IsArray(items) && cJSON_GetArraySize(items) > 0) o->items = kn_json_print(items);
    snprintf(o->match, sizeof o->match, "%s", kn_json_str(args, "match", ""));
    cJSON *p = stats_args(args);
    char *text = kn_json_print(p);
    cJSON_Delete(p);
    char maxkr[24];
    kn_db_call(g_acct_brain->db, &P_COMMIT_STATS,
                KN_DB_ARGS(text, acct_itoa(maxkr, g_acct.max_match_kr)), op_stats_done, o);
    free(text);
    return 0;
}

/* rewards {items:[[id,kr],...]} -> {funds:[...]} (v3.1.1 SAVE_REWARDS) */
static int op_rewards(const cJSON *args, AcctReplyFn cb, void *ud) {
    const cJSON *items = cJSON_GetObjectItemCaseSensitive(args, "items");
    cJSON *ids = cJSON_CreateArray(), *krs = cJSON_CreateArray();
    const cJSON *it;
    cJSON_ArrayForEach(it, items) {
        if (!cJSON_IsArray(it) || cJSON_GetArraySize(it) < 2) continue;
        double kr = cJSON_GetArrayItem(it, 1)->valuedouble;
        if (kr < 0) kr = 0;
        if (kr > g_acct.max_match_kr) kr = g_acct.max_match_kr;
        cJSON_AddItemToArray(ids, cJSON_CreateNumber(cJSON_GetArrayItem(it, 0)->valuedouble));
        cJSON_AddItemToArray(krs, cJSON_CreateNumber(floor(kr)));
    }
    char *ids_t = kn_json_print(ids), *krs_t = kn_json_print(krs);
    cJSON_Delete(ids);
    cJSON_Delete(krs);
    OpCtx *o = op_new(cb, ud, "rewards");
    if (o) {
        kn_db_call(g_acct_brain->db, &P_REWARDS, KN_DB_ARGS(ids_t, krs_t), op_json_done, o);
    }
    free(ids_t);
    free(krs_t);
    return o ? 0 : -1;
}

int accounts_internal_call(Brain *b, const char *op, const cJSON *args, AcctReplyFn cb, void *ud) {
    (void) b;
    if (!op || !cb) {
        if (cb) cb(ud, "Unknown op.", NULL);
        return -1;
    }
    if (!g_acct_brain || !g_acct_brain->db) {
        cb(ud, "Accounts are unavailable.", NULL);
        return -1;
    }
    char ids[24], v1[24], v2[24];
    const int64_t id = kn_json_int(args, "id", 0);

    /* the signed-out guest's balance (db_guest.c, 0350_guest_profiles): a guest has no account id, so these
     * come before the account ops. guest_credit {guest, match, kr, xp} -> {funds, score, duplicate} (a match
     * pays once); guest_claim {guest} -> {funds: 0, score: 0} (a login). The guest id is checked, never logged. */
    if (!strcmp(op, "guest_credit") || !strcmp(op, "guest_claim")) {
        const int credit = !strcmp(op, "guest_credit");
        const char *guest = kn_json_str(args, "guest", NULL);
        if (!guest_id_valid(guest)) {
            cb(ud, "Bad guest id.", NULL);
            return 0;
        }
        const char *match = kn_json_str(args, "match", NULL);
        if (credit && !guest_match_valid(match)) {
            cb(ud, "Bad match.", NULL);
            return 0;
        }
        OpCtx *o = op_new(cb, ud, op);
        if (!o) return -1;
        if (credit)
            kn_db_call(g_acct_brain->db, &P_GUEST_CREDIT,
                       KN_DB_ARGS(guest, match, acct_itoa(v1, kn_json_int(args, "kr", 0)), acct_itoa(v2, kn_json_int(args, "xp", 0))),
                       op_json_done, o);
        else
            kn_db_call(g_acct_brain->db, &P_GUEST_CLAIM, KN_DB_ARGS(guest), op_json_done, o);
        return 0;
    }

    if (!strcmp(op, "login")) {
        internal_auth(1, args, cb, ud);
        return 0;
    }
    if (!strcmp(op, "register")) {
        internal_auth(0, args, cb, ud);
        return 0;
    }
    if (!strcmp(op, "token") || !strcmp(op, "session")) {
        const char *token = kn_json_str(args, "token", NULL);
        if (!token || !*token) {
            cb(ud, "Invalid login.", NULL);
            return 0;
        }
        AcctAuthReq rq = {.action = 1, .name = kn_json_str(args, "name", NULL), .token = token, .origin = "game"};
        AuthBridge *bridge = malloc(sizeof *bridge);
        if (!bridge) {
            cb(ud, "Server error.", NULL);
            return -1;
        }
        bridge->cb = cb;
        bridge->ud = ud;
        acct_auth(&rq, auth_bridge_cb, bridge);
        return 0;
    }
    if (!strcmp(op, "get")) {
        acct_get(id, cb, ud);
        return 0;
    }
    if (!strcmp(op, "logout")) {
        acct_revoke_token(kn_json_str(args, "token", NULL));
        cJSON *empty = cJSON_CreateObject();
        cb(ud, NULL, empty);
        cJSON_Delete(empty);
        return 0;
    }
    if (!strcmp(op, "alias")) {
        acct_set_alias(id, kn_json_str(args, "alias", NULL), cb, ud);
        return 0;
    }
    if (!strcmp(op, "password")) {
        acct_change_password(id, kn_json_str(args, "old", NULL), kn_json_str(args, "new", NULL), cb, ud);
        return 0;
    }
    /* Turf Wars clan, map prize wheel spins, monetized studio games and charges (node_events.c) */
    if (node_events_op(op, args, cb, ud)) return 0;
    if (!strcmp(op, "stats")) return op_stats(args, cb, ud);
    if (!strcmp(op, "rewards")) return op_rewards(args, cb, ud);
    /* the live trade of Trade mode (economy.c, doc/v10/trade.md): both inventories for the window, the settle */
    if (!strcmp(op, "trade_inventories")) return econ_live_trade_inventories(args, cb, ud);
    if (!strcmp(op, "live_trade")) return econ_live_trade(args, cb, ud);
    if (!strcmp(op, "give_skin")) {
        OpCtx *o = op_new(cb, ud, "give_skin");
        if (!o) return -1;
        const int64_t skin = kn_json_int(args, "skin", -1);
        if (skin <= 0 || (acct_skin_count() > 0 && skin >= acct_skin_count())) { /* store.skins[skin] must exist */
            op_reply(o, "Unknown skin.", NULL);
            return -1;
        }
        kn_db_call(g_acct_brain->db, &P_GIVE_SKIN,
                    KN_DB_ARGS(acct_itoa(ids, id), acct_itoa(v1, skin), kn_json_str(args, "source", "spin")),
                    op_json_done, o);
        return 0;
    }
    /* the Junk wallet (db_junk.c, doc/v10/junk.md): amounts in hundredths; the game servers' credits
     * (Reward Junk, Junk Packages) and spends. junk_spend refuses "Not enough Junk". */
    if (!strcmp(op, "junk_balance")) {
        OpCtx *o = op_new(cb, ud, "junk_balance");
        if (!o) return -1;
        kn_db_call(g_acct_brain->db, &P_JUNK_BALANCE, KN_DB_ARGS(acct_itoa(ids, id)), op_json_done, o);
        return 0;
    }
    if (!strcmp(op, "junk_credit") || !strcmp(op, "junk_spend")) {
        const int credit = !strcmp(op, "junk_credit");
        OpCtx *o = op_new(cb, ud, op);
        if (!o) return -1;
        o->id = id;
        kn_db_call(g_acct_brain->db, credit ? &P_JUNK_CREDIT : &P_JUNK_SPEND,
                   KN_DB_ARGS(acct_itoa(ids, id), acct_itoa(v1, kn_json_int(args, "amount", 0)),
                              kn_json_str(args, "reason", "game")),
                   junk_op_done, o);
        return 0;
    }
    if (!strcmp(op, "elo") && cJSON_HasObjectItem(args, "delta")) {
        /* atomic ranked update: {id, mode, delta} -> {elo} (clamped at 0) */
        const int64_t mode = kn_json_int(args, "mode", 1);
        const kn_proc *proc = mode == 4 ? &P_ELO_DELTA_4 : mode == 2 ? &P_ELO_DELTA_2 : &P_ELO_DELTA_1;
        OpCtx *o = op_new(cb, ud, "elo");
        if (!o) return -1;
        kn_db_call(g_acct_brain->db, proc, KN_DB_ARGS(acct_itoa(ids, id), acct_itoa(v1, kn_json_int(args, "delta", 0))),
                    op_json_done, o);
        return 0;
    }
    if (!strcmp(op, "elo")) {
        const int64_t mode = kn_json_int(args, "mode", 1);
        static const kn_sql set4 = {.any = "UPDATE players SET player_elo4 = $2 WHERE player_id = $1"},
                            set2 = {.any = "UPDATE players SET player_elo2 = $2 WHERE player_id = $1"},
                            set1 = {.any = "UPDATE players SET player_elo = $2 WHERE player_id = $1"};
        const kn_sql *sql = mode == 4 ? &set4 : mode == 2 ? &set2 : &set1;
        OpCtx *o = op_new(cb, ud, "elo");
        if (!o) return -1;
        kn_db_query_sql(g_acct_brain->db, sql, KN_DB_ARGS(acct_itoa(ids, id), acct_itoa(v1, kn_json_int(args, "elo", 0))),
                    op_empty_done, o);
        return 0;
    }
    if (!strcmp(op, "challenge")) {
        OpCtx *o = op_new(cb, ud, "challenge");
        if (!o) return -1;
        int64_t kr = kn_json_int(args, "kr", 0);
        if (kr < 0) kr = 0;
        if (kr > g_acct.max_match_kr) kr = g_acct.max_match_kr;
        kn_db_call(g_acct_brain->db, &P_CHALLENGE,
                    KN_DB_ARGS(acct_itoa(ids, id), acct_itoa(v1, kn_json_int(args, "chal", 0)), acct_itoa(v2, kr)),
                    op_json_done, o);
        return 0;
    }
    /* WP6 (doc/v7/krunkscript.md 2.6): GAME.STORAGE, the Save / Load Data triggers and
     * GAME.PAYMENTS.giveKR / Reward KR of a game server's published map (db_storage.c) */
    if (!strcmp(op, "storage")) {
        OpCtx *o = op_new(cb, ud, "storage");
        if (!o) return -1;
        char *data = kn_json_print(cJSON_GetObjectItemCaseSensitive(args, "data"));
        kn_db_call(g_acct_brain->db, &P_STORAGE_UPDATE,
                   KN_DB_ARGS(acct_itoa(ids, id), kn_json_str(args, "name", ""), kn_json_str(args, "map", ""),
                              acct_itoa(v1, kn_json_int(args, "type", 0)), acct_itoa(v2, kn_json_int(args, "scope", 0)), data),
                   op_json_done, o);
        free(data);
        return 0;
    }
    if (!strcmp(op, "storage_load")) {
        OpCtx *o = op_new(cb, ud, "storage_load");
        if (!o) return -1;
        kn_db_call(g_acct_brain->db, &P_STORAGE_LOAD,
                   KN_DB_ARGS(kn_json_str(args, "name", ""), kn_json_str(args, "map", ""), kn_json_str(args, "from", NULL)),
                   op_json_done, o);
        return 0;
    }
    if (!strcmp(op, "map_reward")) {
        OpCtx *o = op_new(cb, ud, "map_reward");
        if (!o) return -1;
        kn_db_call(g_acct_brain->db, &P_MAP_REWARD,
                   KN_DB_ARGS(acct_itoa(ids, id), acct_itoa(v1, kn_json_int(args, "amount", 0)), kn_json_str(args, "map", "")),
                   op_json_done, o);
        return 0;
    }
    if (!strcmp(op, "ban_check")) {
        OpCtx *o = op_new(cb, ud, "ban_check");
        if (!o) return -1;
        kn_db_call(g_acct_brain->db, &P_BAN_CHECK,
                    KN_DB_ARGS(acct_itoa(ids, id), kn_json_str(args, "ip", NULL)), op_json_done, o);
        return 0;
    }
    /* the in-game player list's K.P.D call (db_kpd.c): the game server reports a suspect
     * (kpd_call, args = the call), an officer rates it (kpd_rate, args {call, id, rating}), an
     * officer's lists (kpd_calls / kpd_log, args.limit; kpd_stats, args.id), an application (kpd_apply,
     * args {id, name, discord, reason}), the pending ones (kpd_apps, args.limit) and a developer's grant
     * or revoke of the officer flag (kpd_officer, args {actor, name, on}), the Demos window (kpd_demos,
     * args {off, limit, q}) and the Reviewers window (kpd_reviewers) */
    if (!strcmp(op, "kpd_call") || !strcmp(op, "kpd_rate") || !strcmp(op, "kpd_apply") || !strcmp(op, "kpd_officer")) {
        OpCtx *o = op_new(cb, ud, op);
        if (!o) return -1;
        char *json = args ? cJSON_PrintUnformatted(args) : NULL;
        const kn_proc *proc = !strcmp(op, "kpd_rate") ? &P_KPD_RATE : !strcmp(op, "kpd_apply") ? &P_KPD_APPLY
                            : !strcmp(op, "kpd_officer") ? &P_KPD_OFFICER : &P_KPD_CALL;
        kn_db_call(g_acct_brain->db, proc, KN_DB_ARGS(json ? json : "{}"), op_json_done, o);
        free(json);
        return 0;
    }
    if (!strcmp(op, "kpd_apps")) { /* the pending K.P.D applications (args.limit) */
        OpCtx *o = op_new(cb, ud, op);
        if (!o) return -1;
        kn_db_call(g_acct_brain->db, &P_KPD_APPS, KN_DB_ARGS(acct_itoa(v1, kn_json_int(args, "limit", 10))), op_json_done, o);
        return 0;
    }
    if (!strcmp(op, "kpd_calls") || !strcmp(op, "kpd_log")) {
        OpCtx *o = op_new(cb, ud, op);
        if (!o) return -1;
        kn_db_call(g_acct_brain->db, !strcmp(op, "kpd_log") ? &P_KPD_LOG : &P_KPD_CALLS,
                   KN_DB_ARGS(acct_itoa(v1, kn_json_int(args, "limit", 50))), op_json_done, o);
        return 0;
    }
    if (!strcmp(op, "kpd_stats")) { /* the officer's tags by range (args.id: the officer) */
        OpCtx *o = op_new(cb, ud, op);
        if (!o) return -1;
        kn_db_call(g_acct_brain->db, &P_KPD_STATS, KN_DB_ARGS(acct_itoa(v1, kn_json_int(args, "id", 0))), op_json_done, o);
        return 0;
    }
    if (!strcmp(op, "kpd_demos")) { /* the Demos window: a page of suspect groups (args off, limit, q: the name) */
        OpCtx *o = op_new(cb, ud, op);
        if (!o) return -1;
        kn_db_call(g_acct_brain->db, &P_KPD_DEMOS,
                   KN_DB_ARGS(acct_itoa(v1, kn_json_int(args, "off", 0)), acct_itoa(v2, kn_json_int(args, "limit", 30)),
                              kn_json_str(args, "q", "")),
                   op_json_done, o);
        return 0;
    }
    if (!strcmp(op, "kpd_reviewers")) { /* the Reviewers window: the officers' handled calls, month and all time */
        OpCtx *o = op_new(cb, ud, op);
        if (!o) return -1;
        kn_db_call(g_acct_brain->db, &P_KPD_REVIEWERS, 0, NULL, op_json_done, o);
        return 0;
    }
    cb(ud, "Unknown op.", NULL);
    return -1;
}

/* ------------------------------------------------------------------ module */

static void cleanup_cb(kn_db_result *res, void *ud) {
    (void) ud;
    if (!kn_db_ok(res)) KN_WARN(MOD, "cleanup: %s", kn_db_error(res));
}

static void cleanup_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (!g_acct_brain || !g_acct_brain->db) return;
    static const kn_sql q_sessions = {.pg = "DELETE FROM sessions WHERE expires_at < now()",
                                      .any = "DELETE FROM sessions WHERE expires_at < {now}"},
                        q_failures = {.pg = "DELETE FROM login_failures WHERE at < now() - interval '1 day'",
                                      .any = "DELETE FROM login_failures WHERE at < {ago:86400}"};
    static const kn_sql q_email_tokens = {.any = "DELETE FROM email_tokens WHERE expires_at < {ago:604800}"};
    kn_db_query_sql(g_acct_brain->db, &q_sessions, 0, NULL, cleanup_cb, NULL);
    kn_db_query_sql(g_acct_brain->db, &q_email_tokens, 0, NULL, cleanup_cb, NULL);
    kn_db_query_sql(g_acct_brain->db, &q_failures, 0, NULL, cleanup_cb, NULL);
}

/* accounts.developers / accounts.moderators -> players flags (staff checks run in SQL). */
static void staff_flags_apply(void) {
    for (int dev = 1; dev >= 0; dev--) {
        cJSON *names = cJSON_CreateArray();
        const size_t n = dev ? g_acct.dev_count : g_acct.mod_count;
        for (size_t i = 0; i < n; i++)
            cJSON_AddItemToArray(names, cJSON_CreateString(dev ? g_acct.dev_names[i] : g_acct.mod_names[i]));
        char *text = kn_json_print(names);
        cJSON_Delete(names);
        if (n && text) {
            kn_db_call(g_acct_brain->db, dev ? &P_STAFF_DEV : &P_STAFF_MOD, KN_DB_ARGS(text), cleanup_cb, NULL);
            if (dev) kn_db_call(g_acct_brain->db, &P_STAFF_DEV_BADGES, KN_DB_ARGS(text), cleanup_cb, NULL);
        }
        free(text);
    }
}

int accounts_init(Brain *b) {
    g_acct_brain = b;
    acct_config_load(b);
    if (!b->db) {
        KN_WARN(MOD, "no database: accounts and the social socket answer guests only");
        return 0;
    }
    s_cleanup_timer = kn_loop_add_timer(b->loop, 60 * 1000, 3600 * 1000, cleanup_tick, NULL);
    staff_flags_apply();
    season_init(); /* v7.1.4 seasons: the accounts.current_season row (season.c) */
    acct_security_init(b); /* 2FA, email verification, outbound mail (account_security.c) */
    economy_init(b);       /* trade offer expiry (economy.c) */
    store_init(b);         /* v10 wheels: store_spin_data rotation (store.c) */
    rewards_init(b);       /* v10.0.2 signup perks: the signup premium sweep (rewards.c) */
    KN_INFO(MOD, "ready (names %d-%d, sessions %lld days, password iterations %u)", g_acct.name_min,
            g_acct.name_max, (long long) (g_acct.session_ttl_s / 86400), kn_password_get_iterations());
    return 0;
}

void accounts_shutdown(Brain *b) {
    store_shutdown(b);
    rewards_shutdown(b);
    economy_shutdown(b);
    acct_security_shutdown(b);
    if (s_cleanup_timer) kn_loop_cancel_timer(b->loop, s_cleanup_timer);
    s_cleanup_timer = 0;
}
