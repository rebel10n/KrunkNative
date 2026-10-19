/*
 * services/brain/db_junk.c - the Junk wallet (doc/v10/junk.md): one balance per account in hundredths
 * (players.player_junk, migration 0347_junk_wallet) and its ledger (junk_transactions, one row per
 * credit or spend, written in the same transaction as the balance).
 *
 * The C API (junk_balance / junk_spend / junk_credit, declared in db_procs.h) runs inside the caller's
 * transaction, like db_add_funds_ex: the crafting proc, the battle pass claim and the quick sell call
 * it in their own transaction. The three kn_procs at the bottom are the game servers' ops (accounts.c
 * "junk_balance" / "junk_credit" / "junk_spend"); each runs in its own transaction.
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "junk.h"
#include "knsvc/json.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

/* The ledger's source: at most 32 characters (kr_transactions' VARCHAR(32) on MySQL). */
static void junk_source(const char *reason, char out[33]) { snprintf(out, 33, "%s", reason ? reason : ""); }

int64_t junk_balance(Dbx *x, const int64_t account_id) {
    return dbx_int(x, SQL(.any = "SELECT player_junk FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(account_id)), 0);
}

int junk_credit(Dbx *x, const int64_t account_id, const int64_t amount, const char *reason, int64_t *balance) {
    if (dbx_failed(x)) return -1;
    if (amount <= 0 || amount > JUNK_CREDIT_MAX) return dbx_refuse(x, "Invalid amount.");
    char source[33];
    junk_source(reason, source);
    const int64_t rows = dbx_exec(x,
        SQL(.any = "UPDATE players SET player_junk = player_junk + {int:$2}, updated_at = {now} WHERE player_id = $1"),
        KN_DB_ARGS(I64S(account_id), I64S(amount)));
    if (rows < 0 || dbx_failed(x)) return -1;
    if (rows == 0) return dbx_refuse(x, "Account not found.");
    if (dbx_exec(x, SQL(.any = "INSERT INTO junk_transactions (account_id, amount, source, ref) VALUES ($1, $2, $3, NULL)"),
                 KN_DB_ARGS(I64S(account_id), I64S(amount), source)) < 0)
        return -1;
    if (balance) *balance = junk_balance(x, account_id);
    return dbx_failed(x) ? -1 : 0;
}

int junk_spend(Dbx *x, const int64_t account_id, const int64_t amount, const char *reason, int64_t *balance) {
    if (dbx_failed(x)) return -1;
    if (amount <= 0 || amount > JUNK_CREDIT_MAX) return dbx_refuse(x, "Invalid amount.");
    char source[33];
    junk_source(reason, source);
    // the guard in the WHERE clause: the row lock of this UPDATE makes two spends at one balance queue
    const int64_t rows = dbx_exec(x,
        SQL(.any = "UPDATE players SET player_junk = player_junk - {int:$2}, updated_at = {now}"
                   " WHERE player_id = $1 AND player_junk >= {int:$2}"),
        KN_DB_ARGS(I64S(account_id), I64S(amount)));
    if (rows < 0 || dbx_failed(x)) return -1;
    if (rows == 0) {
        if (!dbx_exists(x, SQL(.any = "SELECT 1 FROM players WHERE player_id = $1"), KN_DB_ARGS(I64S(account_id)))) {
            return dbx_refuse(x, "Account not found.");
        }
        if (dbx_failed(x)) return -1;
        if (balance) *balance = junk_balance(x, account_id);
        return JUNK_INSUFFICIENT; // not an error: nothing was written, the transaction commits as it is
    }
    if (dbx_exec(x, SQL(.any = "INSERT INTO junk_transactions (account_id, amount, source, ref) VALUES ($1, $2, $3, NULL)"),
                 KN_DB_ARGS(I64S(account_id), I64S(-amount), source)) < 0)
        return -1;
    if (balance) *balance = junk_balance(x, account_id);
    return dbx_failed(x) ? -1 : JUNK_OK;
}

/* {junk: "12.34"}: the balance as the wire and the account JSON show it */
cJSON *junk_json(const int64_t balance) {
    char text[24];
    junk_format(balance, text, sizeof text);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "junk", text);
    return o;
}

/* $0 account, $1 amount (hundredths), $2 reason -> {junk, credited} */
static kn_db_result *run_junk_credit(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t amount = PI(1);
    int64_t balance = 0;
    if (junk_credit(&x, PI(0), amount, P(2), &balance) < 0) return dbx_finish(&x, NULL);
    cJSON *o = junk_json(balance);
    cJSON_AddNumberToObject(o, "credited", (double) amount);
    return dbx_json(&x, "junk_credit", o);
}
const kn_proc P_JUNK_CREDIT = {"junk_credit", NULL, run_junk_credit};

/* $0 account, $1 amount (hundredths), $2 reason -> {junk} | refused "Not enough Junk" */
static kn_db_result *run_junk_spend(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t balance = 0;
    const int r = junk_spend(&x, PI(0), PI(1), P(2), &balance);
    if (r < 0) return dbx_finish(&x, NULL);
    if (r == JUNK_INSUFFICIENT) {
        dbx_refuse(&x, "Not enough Junk");
        return dbx_finish(&x, NULL);
    }
    return dbx_json(&x, "junk_spend", junk_json(balance));
}
const kn_proc P_JUNK_SPEND = {"junk_spend", NULL, run_junk_spend};

/* $0 account -> {junk} */
static kn_db_result *run_junk_balance(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM players WHERE player_id = $1"), KN_DB_ARGS(P(0)))) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Account not found.");
        return dbx_finish(&x, NULL);
    }
    const int64_t balance = junk_balance(&x, PI(0));
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    return dbx_json(&x, "junk_balance", junk_json(balance));
}
const kn_proc P_JUNK_BALANCE = {"junk_balance", NULL, run_junk_balance};
