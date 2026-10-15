/*
 * services/brain/db_kpd.c - the Krunker v10.0.2 K.P.D (Krunker Police Department) distress calls
 * on the kpd_calls table of migration 0209. C on every driver.
 *
 *   P_KPD_CALL  (json {id, name, level, suspect, suspect_name, lobby, node, region, map, mode, typ})
 *               -> {id, n, calls}: the call; "Already reported" while an open call about the same
 *               suspect in the same lobby is younger than a minute (v10 config.kpdAutoTime)
 *   P_KPD_CALLS (limit) -> [{id, can, con, gst, tim, gco, typ, jop, jor, cos}]: the open calls of the
 *               last day, newest first, with the reporter's rating (jop helpful / jor handled) and
 *               call count (cos), the fields v10's Distress Calls tab reads (policeRes code 1)
 *   P_KPD_LOG   (limit) -> [{modn, htag, hckn, dat}]: the rated calls (policeRes code 2)
 *   P_KPD_STATS (officer id) -> [{1dayEntries, 3dayEntries, ..., 90dayEntries, alltimeEntries}]: the
 *               officer's tags (calls it rated tagged) by range, v10's Stats tab (policeRes code 3)
 *   P_KPD_RATE  (json {call, id, rating}) -> {id, rating, n}: an officer's verdict on an open call
 *               (v10 "rkpd": 1 tagged, 0 not tagged): status 1 handled, handled_by / handled_at;
 *               "Case already closed" when it is not open any more. The reporter's Helpful Reports
 *               (players.jobrating / jobratingpositive, v10 jobRating / jobRatingPos) count it.
 *   P_KPD_APPLY (json {id, name, discord, reason}) -> {id}: a K.P.D application ("kpdA", migration
 *               0211): from 20 positive reports (players.jobratingpositive) and a rating above 0.7, not
 *               an officer, one pending
 *   P_KPD_APPS  (limit) -> [{id, n, discord, reason, jop, jor, tim}]: the pending applications
 *   P_KPD_OFFICER (json {actor, name, on}) -> {id, n, on}: grant (1) or revoke (0) the officer flag
 *               (players.player_moderator); actor 0 is the game server's console, else a developer.
 *               A grant closes the account's pending application; moderation_log keeps it.
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/json.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }
static const char *nz(const char *s) { return s ? s : ""; }

static double jnum(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : cJSON_IsString(v) && v->valuestring ? strtod(v->valuestring, NULL) : 0.0;
}

static const char *jstr(const cJSON *o, const char *k) {
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) && v->valuestring ? v->valuestring : "";
}

/* the reporter's rating (v10 jobRating / jobRatingPos): their calls an officer rated (jor), the ones
 * rated tagged (jop), and all their calls (cos) */
static void rating_of(Dbx *x, const int64_t reporter, int64_t *jop, int64_t *jor, int64_t *calls) {
    *jop = *jor = *calls = 0;
    if (reporter <= 0) return;
    cJSON *row = dbx_row(x, SQL(.any = "SELECT count(*) AS c__i, sum(CASE WHEN status = 1 THEN 1 ELSE 0 END) AS h__i,"
                                       " sum(CASE WHEN status = 1 AND rating = 1 THEN 1 ELSE 0 END) AS g__i FROM kpd_calls"
                                       " WHERE reporter_id = {int:$1}"),
                         KN_DB_ARGS(I64S(reporter)));
    if (row) {
        *calls = (int64_t) jnum(row, "c");
        *jor = (int64_t) jnum(row, "h");
        *jop = (int64_t) jnum(row, "g");
        cJSON_Delete(row);
    }
}

static kn_db_result *run_kpd_call(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *in = cJSON_Parse(nz(P(0)));
    const int64_t reporter = (int64_t) jnum(in, "id"), suspect = (int64_t) jnum(in, "suspect");
    const char *suspect_name = jstr(in, "suspect_name"), *lobby = jstr(in, "lobby");
    if (!cJSON_IsObject(in) || reporter <= 0) {
        cJSON_Delete(in);
        dbx_refuse(&x, "Sign in to call the K.P.D");
        return dbx_finish(&x, NULL);
    }
    if (!suspect_name[0]) {
        cJSON_Delete(in);
        dbx_refuse(&x, "Player not found");
        return dbx_finish(&x, NULL);
    }
    /* kpdAutoTime: the same suspect in the same lobby within a minute is one call */
    const int open = suspect > 0
        ? dbx_exists(&x, SQL(.any = "SELECT 1 FROM kpd_calls WHERE status = 0 AND suspect_id = {int:$1} AND lobby = $2 AND created_at > {ago:60}"),
                     KN_DB_ARGS(I64S(suspect), lobby))
        : dbx_exists(&x, SQL(.any = "SELECT 1 FROM kpd_calls WHERE status = 0 AND suspect_id IS NULL AND suspect_name = $1 AND lobby = $2"
                                    " AND created_at > {ago:60}"),
                     KN_DB_ARGS(suspect_name, lobby));
    if (open) {
        cJSON_Delete(in);
        if (!dbx_failed(&x)) dbx_refuse(&x, "Already reported to the K.P.D");
        return dbx_finish(&x, NULL);
    }
    const int64_t id = dbx_insert_id(
        &x,
        /* RETURNING id on postgres (verbatim text: plain casts) / sqlite, mysql's auto-increment id (dbx_insert_id) */
        SQL(.pg = "INSERT INTO kpd_calls (reporter_id, reporter_name, reporter_level, suspect_id, suspect_name, lobby, node, region,"
                  " map, mode, typ) VALUES ($1::int, $2, $3::int, $4::int, $5, $6, $7, $8, $9, $10::int, $11::int) RETURNING id",
            .sqlite = "INSERT INTO kpd_calls (reporter_id, reporter_name, reporter_level, suspect_id, suspect_name, lobby, node, region,"
                      " map, mode, typ) VALUES ({int:$1}, $2, {int:$3}, {int:$4}, $5, $6, $7, $8, $9, {int:$10}, {int:$11}) RETURNING id",
            .any = "INSERT INTO kpd_calls (reporter_id, reporter_name, reporter_level, suspect_id, suspect_name, lobby, node, region,"
                   " map, mode, typ) VALUES ({int:$1}, $2, {int:$3}, {int:$4}, $5, $6, $7, $8, $9, {int:$10}, {int:$11})"),
        KN_DB_ARGS(I64S(reporter), jstr(in, "name"), I64S((int64_t) jnum(in, "level")), suspect > 0 ? I64S(suspect) : NULL, suspect_name,
                   lobby, jstr(in, "node"), jstr(in, "region"), jstr(in, "map"), I64S((int64_t) jnum(in, "mode")),
                   I64S((int64_t) jnum(in, "typ"))));
    int64_t jop, jor, calls;
    rating_of(&x, reporter, &jop, &jor, &calls);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) id);
    cJSON_AddStringToObject(o, "n", suspect_name);
    cJSON_AddNumberToObject(o, "calls", (double) calls);
    cJSON_Delete(in);
    return dbx_json(&x, "call", o);
}
const kn_proc P_KPD_CALL = {"kpd_call", NULL, run_kpd_call};

static kn_db_result *run_kpd_calls(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t limit = PI(0);
    if (limit <= 0 || limit > 200) limit = 50;
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT id AS id__i, reporter_id AS rid__i, reporter_name AS can__t, suspect_id AS sid__i,"
                                          " suspect_name AS con__t, lobby AS gco__t, typ AS typ__i, map AS map__t, mode AS mode__i,"
                                          " {epoch_ms:created_at} AS tim__i FROM kpd_calls WHERE status = 0 AND created_at > {ago:86400}"
                                          " ORDER BY created_at DESC LIMIT {int:$1}"),
                           KN_DB_ARGS(I64S(limit)));
    cJSON *out = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "id", jnum(r, "id"));
        cJSON_AddStringToObject(o, "can", jstr(r, "can"));
        cJSON_AddStringToObject(o, "con", jstr(r, "con"));
        cJSON_AddNumberToObject(o, "gst", jnum(r, "sid") > 0 ? 0 : 1); /* a guest suspect */
        cJSON_AddNumberToObject(o, "tim", jnum(r, "tim") / 1000.0);  /* seconds */
        cJSON_AddStringToObject(o, "gco", jstr(r, "gco"));
        cJSON_AddNumberToObject(o, "typ", jnum(r, "typ"));
        cJSON_AddStringToObject(o, "map", jstr(r, "map"));
        cJSON_AddNumberToObject(o, "mode", jnum(r, "mode"));
        int64_t jop, jor, calls;
        rating_of(&x, (int64_t) jnum(r, "rid"), &jop, &jor, &calls);
        cJSON_AddNumberToObject(o, "jop", (double) jop);
        cJSON_AddNumberToObject(o, "jor", (double) jor);
        cJSON_AddNumberToObject(o, "cos", (double) calls);
        cJSON_AddItemToArray(out, o);
    }
    cJSON_Delete(rows);
    return dbx_json(&x, "calls", out);
}
const kn_proc P_KPD_CALLS = {"kpd_calls", NULL, run_kpd_calls};

static kn_db_result *run_kpd_log(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t limit = PI(0);
    if (limit <= 0 || limit > 200) limit = 50;
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT k.rating AS st__i, k.suspect_name AS hckn__t, {epoch_ms:k.handled_at} AS dat__i,"
                                          " coalesce(a.username, 'KPD Demo') AS modn__t FROM kpd_calls k LEFT JOIN accounts a"
                                          " ON a.id = k.handled_by WHERE k.status = 1 ORDER BY k.handled_at DESC LIMIT {int:$1}"),
                           KN_DB_ARGS(I64S(limit)));
    cJSON *out = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddStringToObject(o, "modn", jstr(r, "modn"));
        cJSON_AddNumberToObject(o, "htag", jnum(r, "st") == 1 ? 1 : 0);
        cJSON_AddStringToObject(o, "hckn", jstr(r, "hckn"));
        cJSON_AddNumberToObject(o, "dat", jnum(r, "dat") / 1000.0);
        cJSON_AddItemToArray(out, o);
    }
    cJSON_Delete(rows);
    return dbx_json(&x, "log", out);
}
const kn_proc P_KPD_LOG = {"kpd_log", NULL, run_kpd_log};

static kn_db_result *run_kpd_stats(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    static const char *const ranges[] = {"1day", "3day", "7day", "14day", "30day", "90day", "alltime"};
    static const char *const cols[] = {"d1", "d3", "d7", "d14", "d30", "d90", "al"};
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT sum(CASE WHEN handled_at > {ago:86400} THEN 1 ELSE 0 END) AS d1__i,"
                                        " sum(CASE WHEN handled_at > {ago:259200} THEN 1 ELSE 0 END) AS d3__i,"
                                        " sum(CASE WHEN handled_at > {ago:604800} THEN 1 ELSE 0 END) AS d7__i,"
                                        " sum(CASE WHEN handled_at > {ago:1209600} THEN 1 ELSE 0 END) AS d14__i,"
                                        " sum(CASE WHEN handled_at > {ago:2592000} THEN 1 ELSE 0 END) AS d30__i,"
                                        " sum(CASE WHEN handled_at > {ago:7776000} THEN 1 ELSE 0 END) AS d90__i,"
                                        " count(*) AS al__i FROM kpd_calls WHERE status = 1 AND rating = 1 AND handled_by = {int:$1}"),
                         KN_DB_ARGS(I64S(PI(0))));
    cJSON *o = cJSON_CreateObject();
    for (size_t i = 0; i < sizeof(ranges) / sizeof(ranges[0]); i++) {
        char key[24];
        snprintf(key, sizeof(key), "%sEntries", ranges[i]);
        cJSON_AddNumberToObject(o, key, row ? jnum(row, cols[i]) : 0.0);
    }
    cJSON_Delete(row);
    cJSON *out = cJSON_CreateArray();
    cJSON_AddItemToArray(out, o);
    return dbx_json(&x, "stats", out);
}
const kn_proc P_KPD_STATS = {"kpd_stats", NULL, run_kpd_stats};

static kn_db_result *run_kpd_rate(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *in = cJSON_Parse(nz(P(0)));
    const int64_t call = (int64_t) jnum(in, "call"), officer = (int64_t) jnum(in, "id");
    const int rating = jnum(in, "rating") != 0.0 ? 1 : 0;
    cJSON_Delete(in);
    if (call <= 0 || officer <= 0) {
        dbx_refuse(&x, "No K.P.D case");
        return dbx_finish(&x, NULL);
    }
    const int64_t changed = dbx_exec(&x, SQL(.any = "UPDATE kpd_calls SET status = 1, rating = {int:$2}, handled_by = {int:$3}, handled_at = {now}"
                                                    " WHERE id = {int:$1} AND status = 0"),
                                     KN_DB_ARGS(I64S(call), I64S(rating), I64S(officer)));
    if (changed == 0) {
        dbx_refuse(&x, "Case already closed");
        return dbx_finish(&x, NULL);
    }
    /* the reporter's Helpful Reports (v10 jobRating / jobRatingPos, the account's data slots 38 / 39) */
    dbx_exec(&x, SQL(.any = "UPDATE players SET jobrating = jobrating + 1, jobratingpositive = jobratingpositive + {int:$2}"
                            " WHERE player_id = (SELECT reporter_id FROM kpd_calls WHERE id = {int:$1})"),
             KN_DB_ARGS(I64S(call), I64S(rating)));
    char *name = dbx_text(&x, SQL(.any = "SELECT suspect_name FROM kpd_calls WHERE id = {int:$1}"), KN_DB_ARGS(I64S(call)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) call);
    cJSON_AddNumberToObject(o, "rating", rating);
    cJSON_AddStringToObject(o, "n", name ? name : "");
    free(name);
    return dbx_json(&x, "rate", o);
}
const kn_proc P_KPD_RATE = {"kpd_rate", NULL, run_kpd_rate};

/* the account's Helpful Reports, as its popup shows them (players.jobratingpositive / jobrating) */
static void helpful_of(Dbx *x, const int64_t id, int64_t *jop, int64_t *jor) {
    cJSON *row = dbx_row(x, SQL(.any = "SELECT jobratingpositive AS p__i, jobrating AS t__i FROM players WHERE player_id = {int:$1}"),
                         KN_DB_ARGS(I64S(id)));
    *jop = row ? (int64_t) jnum(row, "p") : 0;
    *jor = row ? (int64_t) jnum(row, "t") : 0;
    cJSON_Delete(row);
}

/* v10's application threshold: 20 positive reports and (pos || 1) / (all || 1) above 0.7 */
#define KPD_APPLY_REPORTS 20

static kn_db_result *run_kpd_apply(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *in = cJSON_Parse(nz(P(0)));
    const int64_t id = (int64_t) jnum(in, "id");
    const char *discord = jstr(in, "discord"), *reason = jstr(in, "reason");
    int64_t jop = 0, jor = 0;
    if (id > 0) helpful_of(&x, id, &jop, &jor);
    if (id <= 0) dbx_refuse(&x, "Sign in to apply for K.P.D");
    else if (!discord[0]) dbx_refuse(&x, "Invalid Discord");
    else if (!reason[0]) dbx_refuse(&x, "Invalid Reason");
    else if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM players WHERE player_id = {int:$1} AND (player_moderator > 0 OR player_developer > 0)"),
                        KN_DB_ARGS(I64S(id))))
        dbx_refuse(&x, "Already K.P.D");
    else if (jop < KPD_APPLY_REPORTS || (double) (jop ? jop : 1) / (double) (jor ? jor : 1) <= 0.7)
        dbx_refuse(&x, "When you reach 20 positive reports, you can apply to become part of K.P.D");
    else if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM kpd_applications WHERE account_id = {int:$1} AND status = 0"), KN_DB_ARGS(I64S(id))))
        dbx_refuse(&x, "Your application is already in review");
    int64_t app = 0;
    if (!dbx_failed(&x))
        app = dbx_insert_id(&x,
                            SQL(.pg = "INSERT INTO kpd_applications (account_id, name, discord, reason) VALUES ($1::int, $2, $3, $4) RETURNING id",
                                .sqlite = "INSERT INTO kpd_applications (account_id, name, discord, reason) VALUES ({int:$1}, $2, $3, $4) RETURNING id",
                                .any = "INSERT INTO kpd_applications (account_id, name, discord, reason) VALUES ({int:$1}, $2, $3, $4)"),
                            KN_DB_ARGS(I64S(id), jstr(in, "name"), discord, reason));
    cJSON_Delete(in);
    if (dbx_failed(&x)) return dbx_finish(&x, NULL);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) app);
    return dbx_json(&x, "apply", o);
}
const kn_proc P_KPD_APPLY = {"kpd_apply", NULL, run_kpd_apply};

static kn_db_result *run_kpd_apps(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t limit = PI(0);
    if (limit <= 0 || limit > 50) limit = 10;
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT id AS id__i, account_id AS aid__i, name AS n__t, discord AS d__t, reason AS r__t,"
                                          " {epoch_ms:created_at} AS tim__i FROM kpd_applications WHERE status = 0"
                                          " ORDER BY created_at LIMIT {int:$1}"),
                           KN_DB_ARGS(I64S(limit)));
    cJSON *out = cJSON_CreateArray();
    const cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        cJSON *o = cJSON_CreateObject();
        int64_t jop, jor;
        helpful_of(&x, (int64_t) jnum(r, "aid"), &jop, &jor);
        cJSON_AddNumberToObject(o, "id", jnum(r, "id"));
        cJSON_AddStringToObject(o, "n", jstr(r, "n"));
        cJSON_AddStringToObject(o, "discord", jstr(r, "d"));
        cJSON_AddStringToObject(o, "reason", jstr(r, "r"));
        cJSON_AddNumberToObject(o, "jop", (double) jop);
        cJSON_AddNumberToObject(o, "jor", (double) jor);
        cJSON_AddNumberToObject(o, "tim", jnum(r, "tim") / 1000.0);
        cJSON_AddItemToArray(out, o);
    }
    cJSON_Delete(rows);
    return dbx_json(&x, "apps", out);
}
const kn_proc P_KPD_APPS = {"kpd_apps", NULL, run_kpd_apps};

static kn_db_result *run_kpd_officer(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *in = cJSON_Parse(nz(P(0)));
    const int64_t actor = (int64_t) jnum(in, "actor");
    const int on = jnum(in, "on") != 0.0 ? 1 : 0;
    char name[64];
    snprintf(name, sizeof(name), "%s", jstr(in, "name"));
    cJSON_Delete(in);
    if (actor > 0 && !dbx_exists(&x, SQL(.any = "SELECT 1 FROM players WHERE player_id = {int:$1} AND player_developer > 0"), KN_DB_ARGS(I64S(actor)))) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Only developers can grant or revoke K.P.D");
        return dbx_finish(&x, NULL);
    }
    const int64_t target = name[0] ? db_account_id(&x, name) : 0;
    if (target <= 0) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Account not found");
        return dbx_finish(&x, NULL);
    }
    dbx_exec(&x, SQL(.any = "UPDATE players SET player_moderator = {int:$2} WHERE player_id = {int:$1}"), KN_DB_ARGS(I64S(target), I64S(on)));
    if (on)
        dbx_exec(&x, SQL(.any = "UPDATE kpd_applications SET status = 1, closed_at = {now} WHERE account_id = {int:$1} AND status = 0"),
                 KN_DB_ARGS(I64S(target)));
    if (actor > 0)
        dbx_exec(&x, SQL(.any = "INSERT INTO moderation_log (actor_id, target_id, action) VALUES ({int:$1}, {int:$2}, $3)"),
                 KN_DB_ARGS(I64S(actor), I64S(target), on ? "kpd_grant" : "kpd_revoke"));
    else /* the game server's console: no actor account */
        dbx_exec(&x, SQL(.any = "INSERT INTO moderation_log (target_id, action) VALUES ({int:$1}, $2)"),
                 KN_DB_ARGS(I64S(target), on ? "kpd_grant" : "kpd_revoke"));
    char *real = dbx_text(&x, SQL(.any = "SELECT username FROM accounts WHERE id = {int:$1}"), KN_DB_ARGS(I64S(target)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "id", (double) target);
    cJSON_AddStringToObject(o, "n", real ? real : name);
    cJSON_AddNumberToObject(o, "on", on);
    free(real);
    return dbx_json(&x, "officer", o);
}
const kn_proc P_KPD_OFFICER = {"kpd_officer", NULL, run_kpd_officer};
