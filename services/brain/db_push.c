/*
 * services/brain/db_push.c - Web Push subscriptions on push_subscriptions (migration 0344_push_subscriptions).
 * C on every driver (the SQL is written for all three dialects through SQL(...) and the Dbx helpers).
 *
 *   P_PUSH_SUBSCRIBE   (account_id, endpoint, p256dh, auth, now_ms) -> {n}: stores the browser's subscription
 *                      for the account. The endpoint is the key: a row of another account moves to this one.
 *                      Then the account's oldest rows beyond PUSH_MAX_PER_ACCOUNT are dropped. One transaction.
 *   P_PUSH_UNSUBSCRIBE (account_id, endpoint) -> {n}: deletes that account's row for the endpoint.
 */
#include "db_procs.h"

#include "push_rules.h"

/* the trim below keeps this many rows: its literal LIMIT must agree with the rule */
_Static_assert(PUSH_MAX_PER_ACCOUNT == 5, "db_push.c: the LIMIT in the trim is PUSH_MAX_PER_ACCOUNT");

#define P(i) (n > (i) ? p[i] : NULL)
#define PS(i) (P(i) ? P(i) : "")

static kn_db_result *run_push_subscribe(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *account = PS(0), *endpoint = PS(1), *p256dh = PS(2), *auth = PS(3), *now = PS(4);
    const int64_t stored = dbx_exec(
        &x,
        SQL(.any = "INSERT INTO push_subscriptions (endpoint, account_id, p256dh, auth, created_at)"
                   " VALUES ($2, $1, $3, $4, $5)"
                   " ON CONFLICT (endpoint) DO UPDATE SET account_id = excluded.account_id,"
                   " p256dh = excluded.p256dh, auth = excluded.auth",
            .mysql = "INSERT INTO push_subscriptions (endpoint, account_id, p256dh, auth, created_at)"
                     " VALUES ($2, $1, $3, $4, $5)"
                     " ON DUPLICATE KEY UPDATE account_id = VALUES(account_id), p256dh = VALUES(p256dh),"
                     " auth = VALUES(auth)"),
        KN_DB_ARGS(account, endpoint, p256dh, auth, now));
    /* keep the account's newest PUSH_MAX_PER_ACCOUNT (5) rows; the rows just stored are the newest */
    if (!dbx_failed(&x))
        dbx_exec(&x,
                 SQL(.any = "DELETE FROM push_subscriptions WHERE account_id = $1 AND endpoint NOT IN"
                            " (SELECT endpoint FROM push_subscriptions WHERE account_id = $1"
                            " ORDER BY created_at DESC, endpoint DESC LIMIT 5)",
                     .mysql = "DELETE FROM push_subscriptions WHERE account_id = $1 AND endpoint NOT IN"
                              " (SELECT endpoint FROM (SELECT endpoint FROM push_subscriptions WHERE account_id = $1"
                              " ORDER BY created_at DESC, endpoint DESC LIMIT 5) AS keep)"),
                 KN_DB_ARGS(account));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", (double) (stored > 0 ? stored : 0));
    return dbx_json(&x, "push_subscribe", o);
}

static kn_db_result *run_push_unsubscribe(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t del = dbx_exec(&x, SQL(.any = "DELETE FROM push_subscriptions WHERE account_id = $1 AND endpoint = $2"),
                                 KN_DB_ARGS(PS(0), PS(1)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "n", (double) (del > 0 ? del : 0));
    return dbx_json(&x, "push_unsubscribe", o);
}

const kn_proc P_PUSH_SUBSCRIBE = {"push_subscribe", NULL, run_push_subscribe};
const kn_proc P_PUSH_UNSUBSCRIBE = {"push_unsubscribe", NULL, run_push_unsubscribe};
