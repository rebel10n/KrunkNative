/*
 * services/brain/db_security.c - account security on the tables of migration 0109_account_security
 * (doc/backend.md 7.9): Krunker v7.1.4's authenticator 2FA and KrunkNative's email verification.
 * C on every driver (no .pg text), run inside the proc's transaction.
 *
 *   P_EMAIL_STATE   (id, cooldown_s, now_ms)  -> {email, verified, pending, sentAt, wait}
 *   P_EMAIL_REQUEST (id, email | NULL, ip, token, ttl_s, cooldown_s, acct_day, addr_day, ip_hour,
 *                    can_send, now_ms)
 *                   -> state + {username, send, logId, limited}: "cemail" (email = the new address,
 *                    stored unverified; a different address drops the old links) and "emailv"
 *                    (email NULL: resend). send = 1: a verify token row and a queued mail_log row
 *                    were written, the caller mails `token`. Rate limits do not refuse (the new
 *                    address stays stored): send = 0 and limited = the message.
 *   P_EMAIL_VERIFY  (token)                   -> {id, email, username}   the emailed link
 *   P_TWOFA_CONFIRM (id, code, now_s)         -> {ok}   "a2FAc": the pending secret becomes active
 *   P_TWOFA_DISABLE (id, code, now_s)         -> {ok}   "d2FAc"
 *   P_TWOFA_LOGIN   (id, code, now_s)         -> {ok}   the "2fa" step of a password login
 * The three 2FA procs refuse "Invalid Code" (wrong, reused or expired code; counted) and "Too many
 * attempts. Please Try Later" (10 wrong codes within 15 minutes).
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

#include "knsvc/crypto.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

#define TWOFA_MAX_FAILURES 10
#define TWOFA_LOCK_S 900
#define TWOFA_WINDOW 1          /* steps of clock drift accepted either way */

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

/* ------------------------------------------------------------------ email */

/* {email, verified, pending, sentAt, wait} of an account (NULL when unknown). */
static cJSON *email_state(Dbx *x, const char *id, int64_t cooldown_s, int64_t now_ms) {
    cJSON *row = dbx_row(x, SQL(.any = "SELECT username, email, (email_verified_at IS NOT NULL) AS verified__b FROM accounts"
                                       " WHERE id = {int:$1}"),
                         KN_DB_ARGS(id));
    if (!row) return NULL;
    const cJSON *em = cJSON_GetObjectItemCaseSensitive(row, "email");
    const int verified = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "verified"));
    const int pending = !verified && cJSON_IsString(em) &&
                        dbx_exists(x, SQL(.any = "SELECT 1 FROM email_tokens WHERE account_id = {int:$1} AND kind = 'verify'"
                                                 " AND used_at IS NULL AND expires_at > {now} AND lower(email) = lower($2)"),
                                   KN_DB_ARGS(id, em->valuestring));
    const int64_t last = dbx_int(x, SQL(.any = "SELECT {epoch_ms:created_at} FROM mail_log WHERE account_id = {int:$1}"
                                               " ORDER BY created_at DESC LIMIT 1"),
                                 KN_DB_ARGS(id), 0);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "email", cJSON_IsString(em) ? cJSON_CreateString(em->valuestring) : cJSON_CreateNull());
    cJSON_AddNumberToObject(o, "verified", verified);
    cJSON_AddNumberToObject(o, "pending", pending);
    cJSON_AddNumberToObject(o, "sentAt", (double) last);
    const int64_t left_ms = last > 0 ? cooldown_s * 1000 - (now_ms - last) : 0;
    cJSON_AddNumberToObject(o, "wait", (double) (left_ms > 0 ? (left_ms + 999) / 1000 : 0));
    cJSON_AddStringToObject(o, "username", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "username"))
                                               ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "username"))
                                               : "");
    cJSON_Delete(row);
    return o;
}

static kn_db_result *run_email_state(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *st = email_state(&x, P(0), PI(1), PI(2));
    if (!st && !dbx_failed(&x)) return kn_db_result_user_error("Account not found.");
    return dbx_json(&x, "state", st);
}
const kn_proc P_EMAIL_STATE = {"email_state", NULL, run_email_state};

static kn_db_result *run_email_request(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *id = P(0), *email = P(1), *ip = P(2), *token = P(3);
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT email, (email_verified_at IS NOT NULL) AS verified__b FROM accounts"
                                        " WHERE id = {int:$1} {for_update}"),
                         KN_DB_ARGS(id));
    if (!row) return dbx_finish(&x, dbx_failed(&x) ? NULL : kn_db_result_user_error("Account not found."));
    const char *cur = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "email"));
    const int verified = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "verified"));
    char addr[256];
    snprintf(addr, sizeof addr, "%s", email ? email : cur ? cur : "");
    const int same = cur && email && !strcasecmp(cur, email);
    cJSON_Delete(row);
    if (email && same && verified) return dbx_finish(&x, kn_db_result_user_error("Email already verified"));
    if (!email && (!cur || !*cur)) return dbx_finish(&x, kn_db_result_user_error("No email on this account"));
    if (!email && verified) return dbx_finish(&x, kn_db_result_user_error("Email already verified"));
    /* one account per address (sign-in by email; register checks it first: accounts.c) */
    if (email && !same &&
        dbx_exists(&x, SQL(.mysql = "SELECT 1 FROM accounts WHERE email = $2 AND id <> {int:$1}",
                           .any = "SELECT 1 FROM accounts WHERE lower(email) = lower($2) AND id <> {int:$1}"),
                   KN_DB_ARGS(id, email)))
        return dbx_finish(&x, kn_db_result_user_error("Email already in use"));
    if (email && !same) {
        dbx_exec(&x, SQL(.any = "UPDATE accounts SET email = $2, email_verified_at = NULL WHERE id = {int:$1}"),
                 KN_DB_ARGS(id, email));
        dbx_exec(&x, SQL(.any = "UPDATE email_tokens SET used_at = {now} WHERE account_id = {int:$1} AND used_at IS NULL"),
                 KN_DB_ARGS(id));
    }
    const char *limited = NULL;
    char wait_msg[96];
    const int64_t now_ms = PI(10), cooldown = PI(5);
    if (PI(9) == 0) limited = "Email is unavailable on this server";
    if (!limited) {
        const int64_t last = dbx_int(&x, SQL(.any = "SELECT {epoch_ms:created_at} FROM mail_log WHERE account_id = {int:$1}"
                                                    " ORDER BY created_at DESC LIMIT 1"),
                                     KN_DB_ARGS(id), 0);
        const int64_t left_ms = last > 0 ? cooldown * 1000 - (now_ms - last) : 0;
        if (left_ms > 0) {
            const long long wait = (left_ms + 999) / 1000;
            snprintf(wait_msg, sizeof wait_msg, "Please wait %lld second%s before requesting another email", wait, wait == 1 ? "" : "s");
            limited = wait_msg;
        }
    }
    if (!limited && PI(6) > 0 &&
        dbx_int(&x, SQL(.any = "SELECT count(*) FROM mail_log WHERE account_id = {int:$1} AND created_at > {ago:86400}"),
                KN_DB_ARGS(id), 0) >= PI(6))
        limited = "Too many emails today. Please Try Later";
    if (!limited && PI(7) > 0 &&
        dbx_int(&x, SQL(.any = "SELECT count(*) FROM mail_log WHERE lower(to_addr) = lower($1) AND created_at > {ago:86400}"),
                KN_DB_ARGS(addr), 0) >= PI(7))
        limited = "Too many emails to this address. Please Try Later";
    if (!limited && PI(8) > 0 && ip && *ip &&
        dbx_int(&x, SQL(.any = "SELECT count(*) FROM mail_log WHERE ip = $1 AND created_at > {ago:3600}"), KN_DB_ARGS(ip), 0) >=
            PI(8))
        limited = "Too many emails. Please Try Later";
    int64_t log_id = 0;
    if (!limited) {
        dbx_exec(&x, SQL(.any = "INSERT INTO email_tokens (token_hash, account_id, kind, email, expires_at)"
                                " VALUES ({hash:$1}, {int:$2}, 'verify', $3, {later:$4})"),
                 KN_DB_ARGS(token, id, addr, P(4)));
        if (dbx_is_pg(&x))
            log_id = dbx_int(&x, SQL(.pg = "INSERT INTO mail_log (account_id, kind, to_addr, ip) VALUES ($1::int, 'verify', $2, $3)"
                                           " RETURNING id"),
                             KN_DB_ARGS(id, addr, ip), 0);
        else
            log_id = dbx_insert_id(&x, SQL(.any = "INSERT INTO mail_log (account_id, kind, to_addr, ip) VALUES ($1, 'verify', $2, $3)"),
                                   KN_DB_ARGS(id, addr, ip));
    }
    cJSON *st = dbx_failed(&x) ? NULL : email_state(&x, id, cooldown, now_ms);
    if (st) {
        cJSON_AddNumberToObject(st, "send", limited ? 0 : 1);
        cJSON_AddNumberToObject(st, "logId", (double) log_id);
        cJSON_AddItemToObject(st, "limited", limited ? cJSON_CreateString(limited) : cJSON_CreateNull());
    }
    return dbx_json(&x, "request", st);
}
const kn_proc P_EMAIL_REQUEST = {"email_request", NULL, run_email_request};

static kn_db_result *run_email_verify(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *token = P(0);
    if (!token || strlen(token) < 20 || strlen(token) > 128) return kn_db_result_user_error("This link is invalid.");
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT t.account_id AS id__i, t.email, a.username, a.email AS current,"
                                        " (t.used_at IS NOT NULL) AS used__b, (t.expires_at <= {now}) AS expired__b,"
                                        " (a.email_verified_at IS NOT NULL) AS verified__b"
                                        " FROM email_tokens t JOIN accounts a ON a.id = t.account_id"
                                        " WHERE t.token_hash = {hash:$1} AND t.kind = 'verify' {for_update}"),
                         KN_DB_ARGS(token));
    if (!row) return dbx_finish(&x, dbx_failed(&x) ? NULL : kn_db_result_user_error("This link is invalid."));
    const char *email = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "email"));
    const char *cur = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "current"));
    const int used = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "used"));
    const int expired = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "expired"));
    const int verified = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "verified"));
    const int matches = email && cur && !strcasecmp(email, cur);
    const char *refusal = NULL;
    if (used && matches && verified) refusal = "This email is already verified.";
    else if (used || !matches) refusal = "This link is no longer valid.";
    else if (expired) refusal = "This link has expired. Request a new one in Krunker Box Office.";
    if (refusal) {
        cJSON_Delete(row);
        return dbx_finish(&x, kn_db_result_user_error(refusal));
    }
    char id[24];
    snprintf(id, sizeof id, "%lld", (long long) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "id")));
    dbx_exec(&x, SQL(.any = "UPDATE accounts SET email_verified_at = {now} WHERE id = {int:$1}"), KN_DB_ARGS(id));
    dbx_exec(&x, SQL(.any = "UPDATE email_tokens SET used_at = {now} WHERE account_id = {int:$1} AND used_at IS NULL"),
             KN_DB_ARGS(id));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) pint(id));
    cJSON_AddStringToObject(o, "email", email);
    cJSON_AddStringToObject(o, "username", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "username")));
    cJSON_Delete(row);
    return dbx_json(&x, "verified", o);
}
const kn_proc P_EMAIL_VERIFY = {"email_verify", NULL, run_email_verify};

/* ------------------------------------------------------------------ 2FA (TOTP) */

enum { TWOFA_CONFIRM, TWOFA_DISABLE, TWOFA_LOGIN };

/* The code against the pending (confirm) or active secret, with replay protection and the
 * failure lock. Writes the outcome (failure count, last step, the secret change). */
static kn_db_result *twofa_check(kn_dbconn *c, int n, const char *const *p, int what) {
    Dbx x = {c, NULL};
    const char *id = P(0), *code = P(1);
    const int64_t now_s = PI(2) > 0 ? PI(2) : (int64_t) time(NULL);
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT twofa_secret, twofa_pending, twofa_last_step AS last__i,"
                                        " twofa_failures AS failures__i,"
                                        " (twofa_failed_at IS NOT NULL AND twofa_failed_at > {ago:900}) AS recent__b"
                                        " FROM accounts WHERE id = {int:$1} {for_update}"),
                         KN_DB_ARGS(id));
    if (!row) return dbx_finish(&x, dbx_failed(&x) ? NULL : kn_db_result_user_error("Account not found."));
    const char *secret = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, what == TWOFA_CONFIRM ? "twofa_pending"
                                                                                                         : "twofa_secret"));
    int64_t last = (int64_t) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "last"));
    const int64_t failures = (int64_t) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "failures"));
    const int recent = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "recent"));
    char secret_copy[128];
    snprintf(secret_copy, sizeof secret_copy, "%s", secret ? secret : "");
    cJSON_Delete(row);
    if (!secret_copy[0]) {
        return dbx_finish(&x, kn_db_result_user_error(what == TWOFA_CONFIRM ? "2FA setup expired. Please try again."
                                                                            : "2FA is not enabled"));
    }
    if (recent && failures >= TWOFA_MAX_FAILURES)
        return dbx_finish(&x, kn_db_result_user_error("Too many attempts. Please Try Later"));
    uint8_t key[64];
    const int klen = kn_base32_decode(secret_copy, strlen(secret_copy), key, sizeof key);
    memset(secret_copy, 0, sizeof secret_copy);
    const int ok = klen > 0 && kn_totp_check(key, (size_t) klen, code, now_s, TWOFA_WINDOW, &last) == 1;
    memset(key, 0, sizeof key);
    if (!ok) {
        /* counted outside the refusal: a refused proc rolls its transaction back */
        dbx_exec(&x, SQL(.any = "UPDATE accounts SET twofa_failures = CASE WHEN twofa_failed_at IS NOT NULL"
                                " AND twofa_failed_at > {ago:900} THEN twofa_failures + 1 ELSE 1 END,"
                                " twofa_failed_at = {now} WHERE id = {int:$1}"),
                 KN_DB_ARGS(id));
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "ok", 0);
        return dbx_json(&x, "twofa", o);
    }
    char step[24];
    snprintf(step, sizeof step, "%lld", (long long) last);
    if (what == TWOFA_CONFIRM)
        dbx_exec(&x, SQL(.any = "UPDATE accounts SET twofa_secret = twofa_pending, twofa_pending = NULL, twofa_last_step = {int:$2},"
                                " twofa_failures = 0, twofa_failed_at = NULL WHERE id = {int:$1}"),
                 KN_DB_ARGS(id, step));
    else if (what == TWOFA_DISABLE)
        dbx_exec(&x, SQL(.any = "UPDATE accounts SET twofa_secret = NULL, twofa_pending = NULL, twofa_last_step = 0,"
                                " twofa_failures = 0, twofa_failed_at = NULL WHERE id = {int:$1}"),
                 KN_DB_ARGS(id));
    else
        dbx_exec(&x, SQL(.any = "UPDATE accounts SET twofa_last_step = {int:$2}, twofa_failures = 0, twofa_failed_at = NULL"
                                " WHERE id = {int:$1}"),
                 KN_DB_ARGS(id, step));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "ok", 1);
    return dbx_json(&x, "twofa", o);
}

static kn_db_result *run_twofa_confirm(kn_dbconn *c, int n, const char *const *p) { return twofa_check(c, n, p, TWOFA_CONFIRM); }
static kn_db_result *run_twofa_disable(kn_dbconn *c, int n, const char *const *p) { return twofa_check(c, n, p, TWOFA_DISABLE); }
static kn_db_result *run_twofa_login(kn_dbconn *c, int n, const char *const *p) { return twofa_check(c, n, p, TWOFA_LOGIN); }
const kn_proc P_TWOFA_CONFIRM = {"twofa_confirm", NULL, run_twofa_confirm};
const kn_proc P_TWOFA_DISABLE = {"twofa_disable", NULL, run_twofa_disable};
const kn_proc P_TWOFA_LOGIN = {"twofa_login", NULL, run_twofa_login};

/* ------------------------------------------------------------------ password reset (KrunkNative) */

/* Reset codes are emailed (12 base32 characters, shown as XXXX-XXXX-XXXX) and typed into the
 * client or opened as <link_base>/auth/reset-password?token=<code>. email_tokens rows of kind
 * 'reset' hold sha256 of the normalized code; the address must be the account's verified email. */

/* (who, ip, code1, code2, code3, ttl_s, cooldown_s, acct_day, addr_day, ip_hour, can_send, now_ms)
 * who = a username or a verified email (up to 3 accounts share one address: one mail, one code
 * each). -> {send, to, logId, ipLimited, accounts: [{id, username, code}]}. Never refuses: the
 * caller answers the same text whether or not an account matched (no account enumeration). */
static kn_db_result *run_reset_request(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *who = P(0), *ip = P(1);
    const int64_t now_ms = PI(11), cooldown = PI(6);
    cJSON *out = cJSON_CreateObject();
    cJSON *list = cJSON_AddArrayToObject(out, "accounts");
    cJSON_AddNumberToObject(out, "send", 0);
    if (!who || !*who) return dbx_json(&x, "reset", out);
    /* the per-IP cap first: it is the only limit the caller reveals */
    if (PI(9) > 0 && ip && *ip &&
        dbx_int(&x, SQL(.any = "SELECT count(*) FROM mail_log WHERE ip = $1 AND created_at > {ago:3600}"), KN_DB_ARGS(ip), 0) >= PI(9)) {
        cJSON_AddNumberToObject(out, "ipLimited", 1);
        return dbx_json(&x, "reset", out);
    }
    cJSON *rows = strchr(who, '@')
                      ? dbx_rows(&x, SQL(.any = "SELECT id AS id__i, username, email FROM accounts WHERE lower(email) = lower($1)"
                                                " AND email_verified_at IS NOT NULL ORDER BY id LIMIT 3"),
                                 KN_DB_ARGS(who))
                      : dbx_rows(&x, SQL(.any = "SELECT id AS id__i, username, email FROM accounts WHERE lower(username) = lower($1)"
                                                " AND email IS NOT NULL AND email_verified_at IS NOT NULL"),
                                 KN_DB_ARGS(who));
    const int count = cJSON_GetArraySize(rows);
    if (count <= 0 || PI(10) == 0) {
        cJSON_Delete(rows);
        return dbx_json(&x, "reset", out);
    }
    const char *to = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 0), "email"));
    char addr[256];
    snprintf(addr, sizeof addr, "%s", to ? to : "");
    char first_id[24];
    snprintf(first_id, sizeof first_id, "%lld", (long long) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(cJSON_GetArrayItem(rows, 0), "id")));
    /* silent limits: the account's cooldown and day cap, the address's day cap */
    const int64_t last = dbx_int(&x, SQL(.any = "SELECT {epoch_ms:created_at} FROM mail_log WHERE account_id = {int:$1}"
                                                " ORDER BY created_at DESC LIMIT 1"),
                                 KN_DB_ARGS(first_id), 0);
    int limited = last > 0 && cooldown * 1000 - (now_ms - last) > 0;
    if (!limited && PI(7) > 0)
        limited = dbx_int(&x, SQL(.any = "SELECT count(*) FROM mail_log WHERE account_id = {int:$1} AND created_at > {ago:86400}"),
                          KN_DB_ARGS(first_id), 0) >= PI(7);
    if (!limited && PI(8) > 0)
        limited = dbx_int(&x, SQL(.any = "SELECT count(*) FROM mail_log WHERE lower(to_addr) = lower($1) AND created_at > {ago:86400}"),
                          KN_DB_ARGS(addr), 0) >= PI(8);
    if (limited || !addr[0]) {
        cJSON_Delete(rows);
        return dbx_json(&x, "reset", out);
    }
    for (int i = 0; i < count && i < 3; i++) {
        const cJSON *r = cJSON_GetArrayItem(rows, i);
        const char *code = P(2 + i);
        if (!code || !*code) break;
        char id[24];
        snprintf(id, sizeof id, "%lld", (long long) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "id")));
        /* one live reset code per account: a new request drops the older ones */
        dbx_exec(&x, SQL(.any = "UPDATE email_tokens SET used_at = {now} WHERE account_id = {int:$1} AND kind = 'reset' AND used_at IS NULL"),
                 KN_DB_ARGS(id));
        dbx_exec(&x, SQL(.any = "INSERT INTO email_tokens (token_hash, account_id, kind, email, expires_at)"
                                " VALUES ({hash:$1}, {int:$2}, 'reset', $3, {later:$4})"),
                 KN_DB_ARGS(code, id, addr, P(5)));
        cJSON *a = cJSON_CreateObject();
        cJSON_AddNumberToObject(a, "id", (double) pint(id));
        cJSON_AddStringToObject(a, "username", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(r, "username")));
        cJSON_AddStringToObject(a, "code", code);
        cJSON_AddItemToArray(list, a);
    }
    cJSON_Delete(rows);
    int64_t log_id = 0;
    if (dbx_is_pg(&x))
        log_id = dbx_int(&x, SQL(.pg = "INSERT INTO mail_log (account_id, kind, to_addr, ip) VALUES ($1::int, 'reset', $2, $3) RETURNING id"),
                         KN_DB_ARGS(first_id, addr, ip), 0);
    else
        log_id = dbx_insert_id(&x, SQL(.any = "INSERT INTO mail_log (account_id, kind, to_addr, ip) VALUES ($1, 'reset', $2, $3)"),
                               KN_DB_ARGS(first_id, addr, ip));
    cJSON_ReplaceItemInObject(out, "send", cJSON_CreateNumber(cJSON_GetArraySize(list) > 0));
    cJSON_AddStringToObject(out, "to", addr);
    cJSON_AddNumberToObject(out, "logId", (double) log_id);
    return dbx_json(&x, "reset", out);
}
const kn_proc P_RESET_REQUEST = {"reset_request", NULL, run_reset_request};

/* The live reset code row of `code` (normalized) or a refusal recorded in x. */
static cJSON *reset_row(Dbx *x, const char *code) {
    if (!code || strlen(code) < 8 || strlen(code) > 64) {
        dbx_refuse(x, "Invalid or expired code.");
        return NULL;
    }
    cJSON *row = dbx_row(x, SQL(.any = "SELECT t.account_id AS id__i, a.username, (t.used_at IS NOT NULL) AS used__b,"
                                       " (t.expires_at <= {now}) AS expired__b, (lower(t.email) = lower(a.email)) AS same__b"
                                       " FROM email_tokens t JOIN accounts a ON a.id = t.account_id"
                                       " WHERE t.token_hash = {hash:$1} AND t.kind = 'reset' {for_update}"),
                         KN_DB_ARGS(code));
    if (!row) {
        if (!dbx_failed(x)) dbx_refuse(x, "Invalid or expired code.");
        return NULL;
    }
    if (cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "used")) || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "expired")) ||
        !cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(row, "same"))) {
        cJSON_Delete(row);
        dbx_refuse(x, "Invalid or expired code.");
        return NULL;
    }
    return row;
}

/* (code) -> {id, username}: the code is valid (nothing changes) */
static kn_db_result *run_reset_check(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = reset_row(&x, P(0));
    if (!row) return dbx_finish(&x, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "id")));
    cJSON_AddStringToObject(o, "username", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "username")));
    cJSON_Delete(row);
    return dbx_json(&x, "reset", o);
}
const kn_proc P_RESET_CHECK = {"reset_check", NULL, run_reset_check};

/* (code, new_hash, session_token, ttl_s) -> {id, username, token}: the code is used up, the new
 * password set, every session revoked and one fresh session issued (like P_SET_PASSWORD). */
static kn_db_result *run_reset_apply(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *row = reset_row(&x, P(0));
    if (!row) return dbx_finish(&x, NULL);
    char id[24];
    snprintf(id, sizeof id, "%lld", (long long) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(row, "id")));
    dbx_exec(&x, SQL(.any = "UPDATE accounts SET password_hash = $2 WHERE id = {int:$1}"), KN_DB_ARGS(id, P(1)));
    dbx_exec(&x, SQL(.any = "UPDATE email_tokens SET used_at = {now} WHERE account_id = {int:$1} AND kind = 'reset' AND used_at IS NULL"),
             KN_DB_ARGS(id));
    dbx_exec(&x, SQL(.any = "DELETE FROM sessions WHERE account_id = {int:$1}"), KN_DB_ARGS(id));
    dbx_exec(&x, SQL(.any = "INSERT INTO sessions (token_hash, account_id, expires_at, origin)"
                            " VALUES ({hash:$1}, {int:$2}, {later:$3}, 'password')"),
             KN_DB_ARGS(P(2), id, P(3)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) pint(id));
    cJSON_AddStringToObject(o, "username", cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(row, "username")));
    cJSON_AddStringToObject(o, "token", P(2) ? P(2) : "");
    cJSON_Delete(row);
    return dbx_json(&x, "reset", o);
}
const kn_proc P_RESET_APPLY = {"reset_apply", NULL, run_reset_apply};
