/*
 * services/brain/db_eggs.c - the Easter egg hunt's finds (Krunker v10.0.3 live event 3, doc/v10/events.md): the
 * "egg_found" op a game server asks when a player's shot or hit reaches an EVENT object (game.foundEventItem,
 * deob-10.0.3.js:220700-220728, its proc 68 call [1, accountId, key, 0, easterEggs]).
 *
 *   (account id, egg key, total) -> {re, cnt}
 *     a new find:  re "found" and cnt = the account's finds after it; re "success" when that makes `total`
 *                  (the hunt is complete: "event.easter.completed", the "eggs" challenge +10)
 *     found before: re "found", cnt 0 ("event.easter.found.old" to the finder)
 * Table event_eggs (migration 0363), one row per (account, egg_key).
 */
#include "db_procs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "knsvc/json.h"

#define P(i) (n > (i) ? p[i] : NULL)

static int64_t pint(const char *s) {
    return s ? strtoll(s, NULL, 10) : 0;
}

static kn_db_result *run_egg_found(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t id = pint(P(0)), total = pint(P(2));
    const char *key = P(1);
    if (id <= 0 || !key || !key[0] || strlen(key) > 32) {
        dbx_refuse(&x, "Invalid egg.");
        return dbx_finish(&x, NULL);
    }
    const int64_t added = dbx_exec(&x, SQL(.any = "{insert_ignore} INTO event_eggs (account_id, egg_key) VALUES ($1, $2) {ignore_conflicts}"),
                                   KN_DB_ARGS(I64S(id), key));
    cJSON *o = cJSON_CreateObject();
    int64_t count = 0;
    if (added > 0) count = dbx_int(&x, SQL(.any = "SELECT count(*) FROM event_eggs WHERE account_id = $1"), KN_DB_ARGS(I64S(id)), 0);
    cJSON_AddStringToObject(o, "re", added > 0 && total > 0 && count >= total ? "success" : "found");
    cJSON_AddNumberToObject(o, "cnt", (double) count);
    return dbx_json(&x, "jsonb_build_object", o);
}
const kn_proc P_EGG_FOUND = {"egg_found", NULL, run_egg_found};
