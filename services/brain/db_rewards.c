/*
 * services/brain/db_rewards.c - Krunker v10.0.2's one-time signup perks on the tables of
 * migration 0116_v10_rewards (doc/backend.md 7.9). C on every driver (no .pg text).
 *
 *   P_SIGNUP_REWARDS (id, skins "a,b,..", premium_ms, premium_s)
 *                    -> {granted, skins [..], premium}   once per account (signup_rewards'
 *                    primary key), for accounts created since signup_rewards_epoch.since; the
 *                    cosmetics go to the inventory (source 'signup'), premium_ms is added to
 *                    players.player_premium (premiumT) until now + premium_s
 *   P_SIGNUP_PREMIUM_SWEEP () -> {swept}   the signup premium that ran out is taken back
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/json.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static cJSON *not_granted(void) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "granted", 0);
    return o;
}

static kn_db_result *run_signup_rewards(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = PI(0), premium_ms = PI(2) > 0 ? PI(2) : 0, premium_s = PI(3) > 0 ? PI(3) : 0;
    const char *skins = P(1) ? P(1) : "";
    /* eligible: a player row, created at / after the epoch (accounts older than the perks keep
     * what they have), no grant yet */
    const int eligible = id > 0 && dbx_exists(&x, SQL(.any = "SELECT 1 FROM accounts a JOIN players pl ON pl.player_id = a.id"
                                                             " JOIN signup_rewards_epoch e ON e.id = 1"
                                                             " WHERE a.id = $1 AND a.created_at >= e.since"),
                                              KN_DB_ARGS(I64S(id)));
    if (dbx_failed(&x) || !eligible) return dbx_failed(&x) ? dbx_finish(&x, NULL) : dbx_json(&x, "signup", not_granted());
    /* the claim row first: a second login (or a concurrent one) inserts nothing and grants nothing */
    const int64_t claimed = dbx_exec(&x, SQL(.any = "{insert_ignore} INTO signup_rewards (account_id, skins, premium_ms, premium_until)"
                                                    " VALUES ($1, $2, {int:$3}, {later:$4}) {ignore_conflicts}"),
                                     KN_DB_ARGS(I64S(id), skins, I64S(premium_ms), I64S(premium_s)));
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    if (claimed <= 0) return dbx_json(&x, "signup", not_granted());
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "granted", 1);
    cJSON *list = cJSON_AddArrayToObject(out, "skins");
    for (const char *s = skins; *s && !dbx_failed(&x);) {
        char *end = NULL;
        const long skin = strtol(s, &end, 10);
        if (end != s && skin >= 0) {
            db_add_skin(&x, id, skin, 1, "signup", "signup", NULL);
            cJSON_AddItemToArray(list, cJSON_CreateNumber((double) skin));
        }
        s = end != s ? end : s + 1;
        while (*s == ',' || *s == ' ') s++;
    }
    if (premium_ms > 0)
        dbx_exec(&x, SQL(.any = "UPDATE players SET player_premium = player_premium + $2, updated_at = {now} WHERE player_id = $1"),
                 KN_DB_ARGS(I64S(id), I64S(premium_ms)));
    cJSON_AddBoolToObject(out, "premium", premium_ms > 0);
    if (dbx_failed(&x)) {
        cJSON_Delete(out);
        return dbx_finish(&x, NULL);
    }
    return dbx_json(&x, "signup", out);
}
const kn_proc P_SIGNUP_REWARDS = {"signup_rewards", NULL, run_signup_rewards};

static kn_db_result *run_signup_premium_sweep(kn_dbconn *c, int n, const char *const *p) {
    (void) n;
    (void) p;
    Dbx x = {c, NULL};
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT account_id AS id__i, premium_ms AS ms__i FROM signup_rewards"
                                          " WHERE premium_swept = 0 AND premium_ms > 0 AND premium_until < {now}"),
                           0, NULL);
    int swept = 0;
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        if (dbx_failed(&x)) break;
        const int64_t id = (int64_t) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "id"));
        const int64_t ms = (int64_t) cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(r, "ms"));
        dbx_exec(&x, SQL(.any = "UPDATE players SET player_premium = CASE WHEN player_premium > $2 THEN player_premium - $2 ELSE 0 END,"
                                " updated_at = {now} WHERE player_id = $1"),
                 KN_DB_ARGS(I64S(id), I64S(ms)));
        dbx_exec(&x, SQL(.any = "UPDATE signup_rewards SET premium_swept = 1 WHERE account_id = $1"), KN_DB_ARGS(I64S(id)));
        swept++;
    }
    cJSON_Delete(rows);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddNumberToObject(out, "swept", swept);
    return dbx_json(&x, "sweep", out);
}
const kn_proc P_SIGNUP_PREMIUM_SWEEP = {"signup_premium_sweep", NULL, run_signup_premium_sweep};
