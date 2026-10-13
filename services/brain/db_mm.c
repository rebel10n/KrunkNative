/*
 * services/brain/db_mm.c - ranked queue and federation operations (db_procs.h).
 * .pg = the statement ranked.c / federation.c always sent (functions of
 * postgres/0201, or the CTE statements of federation.c); run() = the same in
 * C for the other drivers.
 *
 * Postgres array literals ("{a,b}") stay the parameter format of the queue id
 * lists so both sides take identical parameters.
 */
#include "db_procs.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))

static int64_t pint(const char *s) { return s ? strtoll(s, NULL, 10) : 0; }

static const char *nz(const char *s) { return s ? s : ""; }

/* "{a,b}" -> JSON array of strings */
static cJSON *pg_array(const char *lit) {
    cJSON *a = cJSON_CreateArray();
    if (!lit) return a;
    const char *s = lit;
    if (*s == '{') s++;
    while (*s && *s != '}') {
        const char *e = s;
        while (*e && *e != ',' && *e != '}') e++;
        if (e > s) {
            char buf[128];
            snprintf(buf, sizeof buf, "%.*s", (int)(e - s), s);
            cJSON_AddItemToArray(a, cJSON_CreateString(buf));
        }
        s = *e == ',' ? e + 1 : e;
    }
    return a;
}

static int in_list(const cJSON *a, const char *s) {
    const cJSON *it;
    cJSON_ArrayForEach(it, a) {
        if (cJSON_IsString(it) && !strcmp(it->valuestring, s)) return 1;
    }
    return 0;
}

/* kn_rq_key (postgres/0204): the one v7 queue's members carry their season rating as "elo" */
static const char *queue_key(const char *q) {
    if (!q) return NULL;
    if (!strcmp(q, "ranked")) return "elo";
    return NULL;
}

static kn_db_result *json_text(Dbx *x, const char *name, cJSON *v) {
    char *s = v ? cJSON_PrintUnformatted(v) : NULL;
    cJSON_Delete(v);
    if (x->err) {
        free(s);
        return dbx_finish(x, NULL);
    }
    kn_db_result *r = kn_db_result_text(name, KN_DB_T_TEXT, s);
    free(s);
    return r;
}

static cJSON *status_json(const char *status) {
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", status);
    return o;
}

/* ------------------------------------------------------------------ queue */

/* kn_rq_touch: ready_since after any party change */
static void rq_touch(Dbx *x, const char *party) {
    kn_db_result *r = dbx_q(x, SQL(.any = "SELECT count(*), sum(CASE WHEN is_ready THEN 1 ELSE 0 END) FROM mm_ranked_members WHERE party_id = $1"),
                            KN_DB_ARGS(party));
    int64_t total = r ? kn_db_get_int64(r, 0, 0, 0) : 0, ready = r ? kn_db_get_int64(r, 0, 1, 0) : 0;
    kn_db_result_free(r);
    char *queues = dbx_text(x, SQL(.any = "SELECT queue_ids FROM mm_ranked_parties WHERE party_id = $1"), KN_DB_ARGS(party));
    cJSON *q = queues ? cJSON_Parse(queues) : NULL;
    free(queues);
    int all = total > 0 && ready == total && cJSON_GetArraySize(q) > 0;
    cJSON_Delete(q);
    if (all)
        dbx_exec(x, SQL(.any = "UPDATE mm_ranked_parties SET ready_since = coalesce(ready_since, {now}), updated_at = {now} WHERE party_id = $1"),
                 KN_DB_ARGS(party));
    else
        dbx_exec(x, SQL(.any = "UPDATE mm_ranked_parties SET ready_since = NULL, updated_at = {now} WHERE party_id = $1"),
                 KN_DB_ARGS(party));
}

/* (account, username, data json, region, queues literal, party, ready, class, client id, client key, new party, max party) */
static kn_db_result *run_rq_join(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t cd = dbx_int(&x, SQL(.any = "SELECT {epoch_ms:until} - {epoch_ms:{now}} FROM mm_ranked_bans WHERE account_id = $1 AND until > {now}"),
                         KN_DB_ARGS(P(0)), -1);
    if (cd >= 0) {
        cJSON *o = status_json("RankedCooldown");
        cJSON_AddNumberToObject(o, "cooldownMs", (double)cd);
        return json_text(&x, "kn_rq_join", o);
    }
    if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM mm_ranked_members WHERE account_id = $1"), KN_DB_ARGS(P(0))) ||
        dbx_exists(&x, SQL(.any = "SELECT 1 FROM mm_ranked_match_players mp JOIN mm_ranked_matches m ON m.match_id = mp.match_id"
                                  " WHERE mp.account_id = $1 AND m.status IN ('creating', 'live')"),
                   KN_DB_ARGS(P(0))))
        return json_text(&x, "kn_rq_join", status_json("AccountAlreadyPlaying"));
    const char *pid;
    if (P(5) && *P(5)) {
        kn_db_result *r = dbx_q(&x, SQL(.any = "SELECT status FROM mm_ranked_parties WHERE party_id = $1 {for_update}"), KN_DB_ARGS(P(5)));
        int ok = r && kn_db_rows(r) > 0 && !strcmp(kn_db_get(r, 0, 0), "Queued");
        kn_db_result_free(r);
        if (!ok) return json_text(&x, "kn_rq_join", status_json("PartyNotFound"));
        if (dbx_int(&x, SQL(.any = "SELECT count(*) FROM mm_ranked_members WHERE party_id = $1"), KN_DB_ARGS(P(5)), 0) >= PI(11))
            return json_text(&x, "kn_rq_join", status_json("PartyFull"));
        pid = P(5);
    } else {
        cJSON *q = pg_array(P(4));
        char *qs = cJSON_PrintUnformatted(q);
        cJSON_Delete(q);
        dbx_exec(&x, SQL(.any = "INSERT INTO mm_ranked_parties (party_id, region, leader_client, queue_ids) VALUES ($1, $2, $3, $4)"),
                 KN_DB_ARGS(P(10), P(3), P(8), qs ? qs : "[]"));
        free(qs);
        pid = P(10);
    }
    kn_db_result *ins = kn_dbconn_exec_sql(c,
        SQL(.any = "INSERT INTO mm_ranked_members (client_id, client_key, party_id, account_id, username, account_data,"
                   " class_index, is_ready) VALUES ($1, $2, $3, $4, $5, $6, $7, {bool:$8})"),
        8, (const char *const[]){P(8), P(9), pid, P(0), P(1), P(2) ? P(2) : "{}", P(7) ? P(7) : "0", P(6) ? P(6) : "f"});
    if (!kn_db_ok(ins)) {
        if (kn_db_is_unique_violation(ins)) { /* like plpgsql's EXCEPTION WHEN unique_violation */
            kn_db_result_free(ins);
            return json_text(&x, "kn_rq_join", status_json("AccountAlreadyPlaying"));
        }
        if (!x.err) x.err = ins;
        else kn_db_result_free(ins);
        return dbx_finish(&x, NULL);
    }
    kn_db_result_free(ins);
    rq_touch(&x, pid);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNullToObject(o, "status");
    cJSON_AddStringToObject(o, "partyId", pid);
    return json_text(&x, "kn_rq_join", o);
}
const kn_proc P_RQ_JOIN = {
    "rq_join",
    "SELECT kn_rq_join($1::bigint, $2::text, $3::jsonb, $4::text, $5::text[], $6::text,"
    " $7::boolean, $8::integer, $9::text, $10::text, $11::text, $12::integer)::text",
    run_rq_join};

/* kn_rq_state(key) -> poll response, NULL when the key is unknown */
static cJSON *rq_state(Dbx *x, const char *key) {
    if (dbx_exec(x, SQL(.any = "UPDATE mm_ranked_members SET last_poll = {now} WHERE client_key = $1"), KN_DB_ARGS(key)) <= 0)
        return NULL;
    cJSON *m = dbx_row(x, SQL(.any = "SELECT party_id, join_ticket FROM mm_ranked_members WHERE client_key = $1"), KN_DB_ARGS(key));
    const char *party = m ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "party_id")) : NULL;
    cJSON *pr = party ? dbx_row(x, SQL(.any = "SELECT party_id, region, leader_client, queue_ids, status, status_c,"
                                              " ready_since IS NOT NULL AS ready FROM mm_ranked_parties WHERE party_id = $1"),
                                KN_DB_ARGS(party))
                      : NULL;
    if (!pr) {
        cJSON_Delete(m);
        return NULL;
    }
    cJSON *queues = cJSON_DetachItemFromObjectCaseSensitive(pr, "queue_ids");
    if (!cJSON_IsArray(queues)) { /* text column */
        char *t = cJSON_IsString(queues) ? strdup(queues->valuestring) : NULL;
        cJSON_Delete(queues);
        queues = t ? cJSON_Parse(t) : NULL;
        free(t);
        if (!cJSON_IsArray(queues)) {
            cJSON_Delete(queues);
            queues = cJSON_CreateArray();
        }
    }
    const char *vkey = queue_key(cJSON_GetStringValue(cJSON_GetArrayItem(queues, 0)));
    if (!vkey) vkey = "elo";
    const char *status = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(pr, "status"));
    const char *region = cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(pr, "region"));
    cJSON *cobj;
    if (status && !strcmp(status, "Join")) {
        cJSON *sc = cJSON_DetachItemFromObjectCaseSensitive(pr, "status_c");
        if (cJSON_IsString(sc)) {
            cJSON *parsed = cJSON_Parse(sc->valuestring);
            cJSON_Delete(sc);
            sc = parsed;
        }
        cobj = cJSON_IsObject(sc) ? sc : (cJSON_Delete(sc), cJSON_CreateObject());
        const cJSON *jt = cJSON_GetObjectItemCaseSensitive(m, "join_ticket");
        cJSON_AddItemToObject(cobj, "clientId", cJSON_IsString(jt) ? cJSON_CreateString(jt->valuestring) : cJSON_CreateNull());
    } else if (status && !strcmp(status, "Queued")) {
        /* members of ready parties in this region sharing a queue (queue_ids && ...) */
        kn_db_result *r = dbx_q(x, SQL(.any = "SELECT p.queue_ids, (SELECT count(*) FROM mm_ranked_members m WHERE m.party_id = p.party_id)"
                                              " FROM mm_ranked_parties p WHERE p.status = 'Queued' AND p.region = $1 AND p.ready_since IS NOT NULL"),
                                KN_DB_ARGS(nz(region)));
        int64_t size = 0;
        for (int i = 0; r && i < kn_db_rows(r); i++) {
            cJSON *pq = cJSON_Parse(kn_db_get(r, i, 0));
            const cJSON *it;
            int share = 0;
            cJSON_ArrayForEach(it, pq) {
                if (cJSON_IsString(it) && in_list(queues, it->valuestring)) share = 1;
            }
            cJSON_Delete(pq);
            if (share) size += kn_db_get_int64(r, i, 1, 0);
        }
        kn_db_result_free(r);
        cobj = cJSON_CreateObject();
        cJSON_AddNumberToObject(cobj, "queueSize", (double)size);
    } else {
        cobj = cJSON_CreateObject();
    }
    cJSON *o = cJSON_CreateObject();
    cJSON *st = cJSON_CreateObject();
    cJSON_AddStringToObject(st, "t", nz(status));
    cJSON_AddItemToObject(st, "c", cobj);
    cJSON_AddItemToObject(o, "status", st);
    cJSON_AddStringToObject(o, "partyId", party);
    cJSON_AddItemToObject(o, "partyLeaderId", cJSON_Duplicate(cJSON_GetObjectItemCaseSensitive(pr, "leader_client"), 1));
    kn_db_result *mem = dbx_q(x, SQL(.any = "SELECT client_id, username, account_data, class_index, is_ready FROM mm_ranked_members"
                                            " WHERE party_id = $1 ORDER BY joined_at, client_id"),
                              KN_DB_ARGS(party));
    cJSON *members = cJSON_CreateArray();
    for (int i = 0; mem && i < kn_db_rows(mem); i++) {
        cJSON *d = cJSON_Parse(kn_db_get(mem, i, 2));
        cJSON *ad = cJSON_CreateObject();
        const cJSON *cv = cJSON_GetObjectItemCaseSensitive(d, "c"), *cr = cJSON_GetObjectItemCaseSensitive(d, "cR");
        const cJSON *e = cJSON_GetObjectItemCaseSensitive(d, vkey);
        cJSON_AddItemToObject(ad, "c", cv ? cJSON_Duplicate(cv, 1) : cJSON_CreateNull());
        cJSON_AddItemToObject(ad, "cR", cr ? cJSON_Duplicate(cr, 1) : cJSON_CreateNull());
        cJSON_AddItemToObject(ad, "e", e ? cJSON_Duplicate(e, 1) : cJSON_CreateNumber(0));
        char *ads = cJSON_PrintUnformatted(ad);
        cJSON_Delete(ad);
        cJSON_Delete(d);
        cJSON *mo = cJSON_CreateObject();
        cJSON_AddStringToObject(mo, "clientId", kn_db_get(mem, i, 0));
        cJSON_AddStringToObject(mo, "accountId", kn_db_get(mem, i, 1));
        cJSON_AddStringToObject(mo, "accountData", ads ? ads : "{}");
        free(ads);
        cJSON_AddNumberToObject(mo, "classIndex", (double)kn_db_get_int64(mem, i, 3, 0));
        cJSON_AddBoolToObject(mo, "isReady", kn_db_get_bool(mem, i, 4, 0));
        cJSON_AddItemToArray(members, mo);
    }
    kn_db_result_free(mem);
    cJSON_AddItemToObject(o, "partyMembers", members);
    cJSON_AddItemToObject(o, "queueIds", queues);
    cJSON_AddStringToObject(o, "region", nz(region));
    cJSON_Delete(pr);
    cJSON_Delete(m);
    return o;
}

static kn_db_result *run_rq_state(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    return json_text(&x, "kn_rq_state", rq_state(&x, nz(P(0))));
}
const kn_proc P_RQ_STATE = {"rq_state", "SELECT kn_rq_state($1::text)::text", run_rq_state};

/* (key, action, arg, valid queues literal) */
static kn_db_result *run_rq_action(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *key = nz(P(0)), *action = nz(P(1)), *arg = nz(P(2));
    cJSON *m = dbx_row(&x, SQL(.any = "SELECT client_id, party_id FROM mm_ranked_members WHERE client_key = $1"), KN_DB_ARGS(key));
    if (!m) return json_text(&x, "kn_rq_action", NULL);
    char me[160], party[160];
    snprintf(me, sizeof me, "%s", nz(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "client_id"))));
    snprintf(party, sizeof party, "%s", nz(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(m, "party_id"))));
    cJSON_Delete(m);
    cJSON *pr = dbx_row(&x, SQL(.any = "SELECT leader_client, status FROM mm_ranked_parties WHERE party_id = $1 {for_update}"),
                        KN_DB_ARGS(party));
    int leader = pr && !strcmp(nz(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(pr, "leader_client"))), me);
    int queued = pr && !strcmp(nz(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(pr, "status"))), "Queued");
    cJSON_Delete(pr);
    if (!strcmp(action, "leave")) {
        dbx_exec(&x, SQL(.any = "DELETE FROM mm_ranked_members WHERE client_id = $1"), KN_DB_ARGS(me));
        if (!dbx_exists(&x, SQL(.any = "SELECT 1 FROM mm_ranked_members WHERE party_id = $1"), KN_DB_ARGS(party))) {
            dbx_exec(&x, SQL(.any = "DELETE FROM mm_ranked_parties WHERE party_id = $1"), KN_DB_ARGS(party));
        } else {
            if (leader) {
                char *next = dbx_text(&x, SQL(.any = "SELECT client_id FROM mm_ranked_members WHERE party_id = $1 ORDER BY joined_at, client_id LIMIT 1"),
                                      KN_DB_ARGS(party));
                dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_parties SET leader_client = $2 WHERE party_id = $1"), KN_DB_ARGS(party, next));
                free(next);
            }
            rq_touch(&x, party);
        }
        cJSON *o = cJSON_CreateObject();
        cJSON_AddBoolToObject(o, "left", 1);
        return json_text(&x, "kn_rq_action", o);
    }
    if (queued) {
        if (!strcmp(action, "ready")) {
            dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_members SET is_ready = $2 WHERE client_id = $1"),
                     KN_DB_ARGS(me, (!strcmp(arg, "true") || !strcmp(arg, "1")) ? "1" : "0"));
        } else if (!strcmp(action, "class")) {
            int64_t ci = *arg ? strtoll(arg, NULL, 10) : 0;
            dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_members SET class_index = $2 WHERE client_id = $1"),
                     KN_DB_ARGS(me, I64S(ci > 0 ? ci : 0)));
        } else if (!strcmp(action, "queues") && leader) {
            cJSON *valid = pg_array(P(3)), *want = cJSON_Parse(arg), *out = cJSON_CreateArray();
            const cJSON *it;
            cJSON_ArrayForEach(it, want) {
                if (cJSON_IsString(it) && in_list(valid, it->valuestring) && !in_list(out, it->valuestring))
                    cJSON_AddItemToArray(out, cJSON_CreateString(it->valuestring));
            }
            char *qs = cJSON_PrintUnformatted(out);
            dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_parties SET queue_ids = $2 WHERE party_id = $1"), KN_DB_ARGS(party, qs ? qs : "[]"));
            free(qs);
            cJSON_Delete(valid);
            cJSON_Delete(want);
            cJSON_Delete(out);
        } else if (!strcmp(action, "kick") && leader && strcmp(arg, me)) {
            dbx_exec(&x, SQL(.any = "DELETE FROM mm_ranked_members WHERE client_id = $1 AND party_id = $2"), KN_DB_ARGS(arg, party));
        } else if (!strcmp(action, "promote") && leader) {
            if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM mm_ranked_members WHERE client_id = $1 AND party_id = $2"), KN_DB_ARGS(arg, party)))
                dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_parties SET leader_client = $2 WHERE party_id = $1"), KN_DB_ARGS(party, arg));
        }
        rq_touch(&x, party);
    }
    return json_text(&x, "kn_rq_action", rq_state(&x, key));
}
const kn_proc P_RQ_ACTION = {"rq_action", "SELECT kn_rq_action($1::text, $2::text, $3::text, $4::text[])::text", run_rq_action};

/* housekeeping (poll_s, creating_s, match_s) */
static kn_db_result *run_rq_expire(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    dbx_exec(&x, SQL(.any = "DELETE FROM mm_ranked_members WHERE last_poll < {ago:$1}"), KN_DB_ARGS(P(0)));
    dbx_exec(&x, SQL(.any = "DELETE FROM mm_ranked_parties WHERE NOT EXISTS"
                            " (SELECT 1 FROM mm_ranked_members m WHERE m.party_id = mm_ranked_parties.party_id)"),
             0, NULL);
    kn_db_result *lost = dbx_q(&x, SQL(.any = "SELECT party_id FROM mm_ranked_parties p WHERE NOT EXISTS"
                                              " (SELECT 1 FROM mm_ranked_members m WHERE m.client_id = p.leader_client)"),
                               0, NULL);
    for (int i = 0; lost && i < kn_db_rows(lost); i++) {
        const char *party = kn_db_get(lost, i, 0);
        char *next = dbx_text(&x, SQL(.any = "SELECT client_id FROM mm_ranked_members WHERE party_id = $1 ORDER BY joined_at, client_id LIMIT 1"),
                              KN_DB_ARGS(party));
        if (next) dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_parties SET leader_client = $2 WHERE party_id = $1"), KN_DB_ARGS(party, next));
        free(next);
    }
    kn_db_result_free(lost);
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_matches SET status = 'aborted', finished_at = {now}, result = '{\"reason\":\"host timeout\"}'"
                            " WHERE status = 'creating' AND created_at < {ago:$1}"),
             KN_DB_ARGS(P(1)));
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_parties SET status = 'Queued', status_c = '{}', match_id = NULL, updated_at = {now}"
                            " WHERE status = 'CreatingGame' AND NOT EXISTS (SELECT 1 FROM mm_ranked_matches x"
                            " WHERE x.match_id = mm_ranked_parties.match_id AND x.status = 'creating')"),
             0, NULL);
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_matches SET status = 'aborted', finished_at = {now}, result = '{\"reason\":\"no result\"}'"
                            " WHERE status = 'live' AND started_at < {ago:$1}"),
             KN_DB_ARGS(P(2)));
    dbx_exec(&x, SQL(.any = "DELETE FROM mm_ranked_bans WHERE until < {now}"), 0, NULL);
    return dbx_finish(&x, kn_db_result_command(0));
}
const kn_proc P_RQ_EXPIRE = {"rq_expire", "SELECT kn_rq_expire($1::integer, $2::integer, $3::integer)", run_rq_expire};

/* the matcher's lock, inside its transaction: sqlite's BEGIN IMMEDIATE already
 * holds the write lock; mysql locks the mm_ranked_lock row without waiting */
static kn_db_result *run_rq_lock(kn_dbconn *c, int n, const char *const *p) {
    int got = 1;
    if (!strcmp(kn_dbconn_dialect(c), "mysql")) {
        kn_db_result *r = kn_dbconn_exec_sql(c, SQL(.any = "SELECT id FROM mm_ranked_lock WHERE id = 1 FOR UPDATE NOWAIT"), 0, NULL);
        got = kn_db_ok(r) && kn_db_rows(r) > 0;
        kn_db_errclass e = kn_db_errclass_of(r);
        if (!kn_db_ok(r) && e != KN_DB_E_RETRY) return r; /* real failure */
        kn_db_result_free(r);
    }
    return kn_db_result_text("pg_try_advisory_xact_lock", KN_DB_T_BOOL, got ? "t" : "f");
}
const kn_proc P_RQ_LOCK = {"rq_lock", "SELECT pg_try_advisory_xact_lock(kn_rq_lock())", run_rq_lock};

/* mm_ranked_candidates: ready parties -> party_id, region, queues, wait_s, members */
static kn_db_result *run_rq_candidates(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    kn_db_result *ps = dbx_q(&x,
        SQL(.any = "SELECT party_id, region, queue_ids, ({epoch_ms:{now}} - {epoch_ms:ready_since}) / 1000.0 AS wait_s"
                   " FROM mm_ranked_parties WHERE status = 'Queued' AND ready_since IS NOT NULL AND ready_since <= {now}"),
        0, NULL);
    const char *names[5] = {"party_id", "region", "queues", "wait_s", "members"};
    const kn_db_type types[5] = {KN_DB_T_TEXT, KN_DB_T_TEXT, KN_DB_T_JSON, KN_DB_T_FLOAT, KN_DB_T_JSON};
    kn_db_result *out = kn_db_result_new(5, names, types);
    for (int i = 0; ps && out && i < kn_db_rows(ps); i++) {
        const char *party = kn_db_get(ps, i, 0);
        kn_db_result *m = dbx_q(&x, SQL(.any = "SELECT account_id, username, client_id, account_data FROM mm_ranked_members"
                                               " WHERE party_id = $1 ORDER BY joined_at, client_id"),
                                KN_DB_ARGS(party));
        cJSON *members = cJSON_CreateArray();
        for (int k = 0; m && k < kn_db_rows(m); k++) {
            cJSON *d = cJSON_Parse(kn_db_get(m, k, 3));
            cJSON *o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "a", (double)kn_db_get_int64(m, k, 0, 0));
            cJSON_AddStringToObject(o, "n", kn_db_get(m, k, 1));
            cJSON_AddStringToObject(o, "c", kn_db_get(m, k, 2));
            const char *keys[3] = {"elo", "elo2", "elo4"};
            for (int q = 0; q < 3; q++) {
                const cJSON *v = cJSON_GetObjectItemCaseSensitive(d, keys[q]);
                cJSON_AddNumberToObject(o, keys[q], cJSON_IsNumber(v) ? (double)(int64_t)v->valuedouble
                                                    : cJSON_IsString(v) ? (double)strtoll(v->valuestring, NULL, 10) : 0);
            }
            cJSON_Delete(d);
            cJSON_AddItemToArray(members, o);
        }
        kn_db_result_free(m);
        char *ms = cJSON_PrintUnformatted(members);
        cJSON_Delete(members);
        const char *row[5] = {party, kn_db_get(ps, i, 1), kn_db_get(ps, i, 2), kn_db_get(ps, i, 3), ms ? ms : "[]"};
        kn_db_result_add_row(out, row);
        free(ms);
    }
    kn_db_result_free(ps);
    return dbx_finish(&x, out);
}
const kn_proc P_RQ_CANDIDATES = {
    "rq_candidates",
    "SELECT party_id, region, to_jsonb(queue_ids) AS queues, wait_s, members FROM mm_ranked_candidates WHERE $1::text IS NOT NULL",
    run_rq_candidates};

/* (match, queue, region, brain, teams [[party ids], [party ids]]) -> bool */
static kn_db_result *run_rq_match_create(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *teams = P(4) ? cJSON_Parse(P(4)) : NULL;
    int total = 0, ok = 0;
    const cJSON *t, *pid;
    cJSON_ArrayForEach(t, teams) {
        cJSON_ArrayForEach(pid, t) {
            total++;
            if (cJSON_IsString(pid) &&
                dbx_exists(&x, SQL(.any = "SELECT 1 FROM mm_ranked_parties WHERE party_id = $1 AND status = 'Queued'"
                                          " AND ready_since IS NOT NULL AND ready_since <= {now}"),
                           KN_DB_ARGS(pid->valuestring)))
                ok++;
        }
    }
    if (ok != total || total == 0) {
        cJSON_Delete(teams);
        return dbx_finish(&x, kn_db_result_text("kn_rq_match_create", KN_DB_T_BOOL, "f"));
    }
    const char *key = queue_key(P(1));
    dbx_exec(&x, SQL(.any = "INSERT INTO mm_ranked_matches (match_id, queue_id, region, brain_id) VALUES ($1, $2, $3, $4)"),
             KN_DB_ARGS(P(0), P(1), P(2), P(3)));
    int idx = 0;
    cJSON_ArrayForEach(t, teams) {
        cJSON_ArrayForEach(pid, t) {
            kn_db_result *m = dbx_q(&x, SQL(.any = "SELECT account_id, username, account_data FROM mm_ranked_members WHERE party_id = $1"),
                                    KN_DB_ARGS(pid->valuestring));
            for (int k = 0; m && k < kn_db_rows(m); k++) {
                cJSON *d = cJSON_Parse(kn_db_get(m, k, 2));
                const cJSON *v = key ? cJSON_GetObjectItemCaseSensitive(d, key) : NULL;
                int64_t before = cJSON_IsNumber(v) ? (int64_t)v->valuedouble : cJSON_IsString(v) ? strtoll(v->valuestring, NULL, 10) : 0;
                cJSON_Delete(d);
                dbx_exec(&x, SQL(.any = "INSERT INTO mm_ranked_match_players (match_id, account_id, username, team, rating_key, rating_before)"
                                        " VALUES ($1, $2, $3, $4, $5, $6)"),
                         KN_DB_ARGS(P(0), kn_db_get(m, k, 0), kn_db_get(m, k, 1), I64S(idx), key ? key : "", I64S(before)));
            }
            kn_db_result_free(m);
            dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_parties SET status = 'CreatingGame', status_c = '{}', match_id = $2, updated_at = {now}"
                                    " WHERE party_id = $1"),
                     KN_DB_ARGS(pid->valuestring, P(0)));
        }
        idx++;
    }
    cJSON_Delete(teams);
    return dbx_finish(&x, kn_db_result_text("kn_rq_match_create", KN_DB_T_BOOL, "t"));
}
const kn_proc P_RQ_MATCH_CREATE = {"rq_match_create", "SELECT kn_rq_match_create($1::text, $2::text, $3::text, $4::text, $5::jsonb)",
                                   run_rq_match_create};

/* (match, game, status_c json, tickets {"<account>": ticket}) */
static kn_db_result *run_rq_match_ready(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_matches SET status = 'live', game_id = $2, started_at = {now} WHERE match_id = $1 AND status = 'creating'"),
             KN_DB_ARGS(P(0), P(1)));
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_parties SET status = 'Join', status_c = $2, updated_at = {now} WHERE match_id = $1"),
             KN_DB_ARGS(P(0), P(2) ? P(2) : "{}"));
    cJSON *tickets = P(3) ? cJSON_Parse(P(3)) : NULL;
    kn_db_result *m = dbx_q(&x, SQL(.any = "SELECT m.client_id, m.account_id FROM mm_ranked_members m JOIN mm_ranked_parties p"
                                           " ON p.party_id = m.party_id WHERE p.match_id = $1"),
                            KN_DB_ARGS(P(0)));
    for (int i = 0; m && i < kn_db_rows(m); i++) {
        const cJSON *tk = cJSON_GetObjectItemCaseSensitive(tickets, kn_db_get(m, i, 1));
        dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_members SET join_ticket = $2 WHERE client_id = $1"),
                 KN_DB_ARGS(kn_db_get(m, i, 0), cJSON_IsString(tk) ? tk->valuestring : NULL));
    }
    kn_db_result_free(m);
    cJSON_Delete(tickets);
    return dbx_finish(&x, kn_db_result_command(0));
}
const kn_proc P_RQ_MATCH_READY = {"rq_match_ready", "SELECT kn_rq_match_ready($1::text, $2::text, $3::jsonb, $4::jsonb)",
                                  run_rq_match_ready};

static kn_db_result *run_rq_match_failed(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "reason", nz(P(1)));
    char *rs = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_matches SET status = 'aborted', finished_at = {now}, result = $2"
                            " WHERE match_id = $1 AND status IN ('creating', 'live')"),
             KN_DB_ARGS(P(0), rs));
    free(rs);
    /* back in the queue, but not matched again for 10 s */
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_parties SET status = 'Queued', status_c = '{}', match_id = NULL, updated_at = {now},"
                            " ready_since = CASE WHEN ready_since IS NULL THEN NULL ELSE {later:10} END WHERE match_id = $1"),
             KN_DB_ARGS(P(0)));
    return dbx_finish(&x, kn_db_result_command(0));
}
const kn_proc P_RQ_MATCH_FAILED = {"rq_match_failed", "SELECT kn_rq_match_failed($1::text, $2::text)", run_rq_match_failed};

/* close a live match once -> its players, [] when already closed */
static kn_db_result *run_rq_match_finish(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t done = dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_matches SET status = 'done', finished_at = {now}, result = $2"
                                           " WHERE match_id = $1 AND status = 'live'"),
                            KN_DB_ARGS(P(0), P(1) ? P(1) : "{}"));
    cJSON *a = NULL;
    if (done > 0)
        a = dbx_rows(&x, SQL(.any = "SELECT account_id AS \"accountId\", username AS name, team, rating_key AS \"key\","
                                    " rating_before AS \"before\" FROM mm_ranked_match_players WHERE match_id = $1"),
                     KN_DB_ARGS(P(0)));
    return json_text(&x, "kn_rq_match_finish", a ? a : cJSON_CreateArray());
}
const kn_proc P_RQ_MATCH_FINISH = {"rq_match_finish", "SELECT kn_rq_match_finish($1::text, $2::jsonb)::text", run_rq_match_finish};

/* (match, ratings [{accountId, new}]) */
static kn_db_result *run_rq_ratings(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *a = P(1) ? cJSON_Parse(P(1)) : NULL;
    const cJSON *it;
    cJSON_ArrayForEach(it, a) {
        const cJSON *acc = cJSON_GetObjectItemCaseSensitive(it, "accountId"), *nw = cJSON_GetObjectItemCaseSensitive(it, "new");
        if (!cJSON_IsNumber(acc) || !cJSON_IsNumber(nw)) continue;
        dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_match_players SET rating_after = $3 WHERE match_id = $1 AND account_id = $2"),
                 KN_DB_ARGS(P(0), I64S((int64_t)acc->valuedouble), I64S((int64_t)nw->valuedouble)));
    }
    cJSON_Delete(a);
    return dbx_finish(&x, kn_db_result_command(0));
}
const kn_proc P_RQ_RATINGS = {
    "rq_ratings",
    "UPDATE mm_ranked_match_players mp SET rating_after = (x ->> 'new')::integer"
    "  FROM jsonb_array_elements($2::jsonb) x"
    " WHERE mp.match_id = $1::text AND mp.account_id = (x ->> 'accountId')::bigint",
    run_rq_ratings};

/* ------------------------------------------------------------------ federation */

/* Lobby tuples travel as json, not jsonb: jsonb reorders object keys, and the
 * game list must keep the game server's meta key order (0203_mm_tuple_json.sql). */
/* connected_at = the registry's connected_ms (the internal WS hello), not now():
 * persists run on pooled connections and finish in any order, and the other
 * brains list nodes by connected_at like registry.ts lists them by hello order. */
#define SQL_NODE_LOBBIES                                                                                         \
    "x AS (SELECT * FROM json_to_recordset($13::json) AS x(gid text, lid text, region text, players int,"         \
    "      max int, cs boolean, pr boolean, mode text, map text, v text, rs text, tuple json)),"                         \
    "del AS (DELETE FROM mm_lobbies WHERE node_id IN (SELECT node_id FROM n)"                                    \
    "        AND game_id NOT IN (SELECT gid FROM x)),"                                                           \
    "ins AS (INSERT INTO mm_lobbies (game_id, node_id, local_id, region, players, max_players, custom,"          \
    "        protected, mode, map, version, ruleset, tuple, updated_at)"                                         \
    "  SELECT gid, $1::text, lid, region, players, max, cs, pr, mode, map, v, coalesce(rs, 'v7'), tuple, now() FROM x"               \
    "  WHERE EXISTS (SELECT 1 FROM n)"                                                                           \
    "  ON CONFLICT (game_id) DO UPDATE SET node_id = EXCLUDED.node_id, local_id = EXCLUDED.local_id,"            \
    "   region = EXCLUDED.region, players = EXCLUDED.players, max_players = EXCLUDED.max_players,"               \
    "   custom = EXCLUDED.custom, protected = EXCLUDED.protected, mode = EXCLUDED.mode, map = EXCLUDED.map,"     \
    "   version = EXCLUDED.version, ruleset = EXCLUDED.ruleset, tuple = EXCLUDED.tuple, updated_at = now()"                                  \
    "  RETURNING 1)"                                                                                             \
    "SELECT (SELECT count(*) FROM n) AS owned"

/* lobbies of an owned node: drop the gone ones, upsert the rest (tuple text as sent) */
static void node_lobbies(Dbx *x, const char *node, const char *lobbies_json) {
    cJSON *a = lobbies_json ? cJSON_Parse(lobbies_json) : NULL;
    kn_db_result *have = dbx_q(x, SQL(.any = "SELECT game_id FROM mm_lobbies WHERE node_id = $1"), KN_DB_ARGS(node));
    for (int i = 0; have && i < kn_db_rows(have); i++) {
        const char *gid = kn_db_get(have, i, 0);
        int keep = 0;
        const cJSON *it;
        cJSON_ArrayForEach(it, a) {
            if (!strcmp(nz(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "gid"))), gid)) keep = 1;
        }
        if (!keep) dbx_exec(x, SQL(.any = "DELETE FROM mm_lobbies WHERE game_id = $1"), KN_DB_ARGS(gid));
    }
    kn_db_result_free(have);
    const cJSON *it;
    cJSON_ArrayForEach(it, a) {
        char *tuple = cJSON_PrintUnformatted(cJSON_GetObjectItemCaseSensitive(it, "tuple"));
        char players[24], max[24];
        dbx_i64s(players, (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(it, "players")));
        dbx_i64s(max, (int64_t)cJSON_GetNumberValue(cJSON_GetObjectItemCaseSensitive(it, "max")));
        const char *args[13] = {
            nz(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "gid"))), node,
            nz(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "lid"))),
            nz(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "region"))), players, max,
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "cs")) ? "1" : "0",
            cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(it, "pr")) ? "1" : "0",
            nz(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "mode"))),
            nz(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "map"))),
            nz(cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "v"))), tuple ? tuple : "[]",
            cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "rs")) ? cJSON_GetStringValue(cJSON_GetObjectItemCaseSensitive(it, "rs")) : "v7"};
        dbx_exec(x,
                 SQL(.sqlite = "INSERT INTO mm_lobbies (game_id, node_id, local_id, region, players, max_players, custom,"
                               " protected, mode, map, version, tuple, updated_at, ruleset) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9,"
                               " $10, $11, $12, {now}, $13) ON CONFLICT (game_id) DO UPDATE SET node_id = excluded.node_id,"
                               " local_id = excluded.local_id, region = excluded.region, players = excluded.players,"
                               " max_players = excluded.max_players, custom = excluded.custom, protected = excluded.protected,"
                               " mode = excluded.mode, map = excluded.map, version = excluded.version, tuple = excluded.tuple,"
                               " updated_at = excluded.updated_at, ruleset = excluded.ruleset",
                     .mysql = "INSERT INTO mm_lobbies (game_id, node_id, local_id, region, players, max_players, custom,"
                              " protected, mode, map, version, tuple, updated_at, ruleset) VALUES ($1, $2, $3, $4, $5, $6, $7, $8, $9,"
                              " $10, $11, $12, {now}, $13) ON DUPLICATE KEY UPDATE node_id = VALUES(node_id),"
                              " local_id = VALUES(local_id), region = VALUES(region), players = VALUES(players),"
                              " max_players = VALUES(max_players), custom = VALUES(custom), protected = VALUES(protected),"
                              " mode = VALUES(mode), map = VALUES(map), version = VALUES(version), tuple = VALUES(tuple),"
                              " updated_at = VALUES(updated_at), ruleset = VALUES(ruleset)"),
                 13, args);
        free(tuple);
    }
    cJSON_Delete(a);
}

static kn_db_result *owned_result(Dbx *x, int64_t owned) {
    const char *names[1] = {"owned"};
    const kn_db_type types[1] = {KN_DB_T_INT};
    kn_db_result *r = kn_db_result_new(1, names, types);
    const char *row[1] = {I64S(owned)};
    if (r) kn_db_result_add_row(r, row);
    return dbx_finish(x, r);
}

/* (node, brain, region, name, host, port, web, version, maxp, maxl, custom, draining, lobbies json, connected ms) */
static kn_db_result *run_node_takeover(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    dbx_exec(&x,
             SQL(.sqlite = "INSERT INTO mm_nodes (node_id, brain_id, region, name, public_host, public_port, web_url, version,"
                           " max_players, max_lobbies, custom, draining, connected_at, heartbeat_at) VALUES ($1, $2, $3, $4, $5,"
                           " $6, $7, $8, $9, $10, {bool:$11}, {bool:$12}, {from_ms:$14}, {now}) ON CONFLICT (node_id) DO UPDATE"
                           " SET brain_id = excluded.brain_id, region = excluded.region, name = excluded.name,"
                           " public_host = excluded.public_host, public_port = excluded.public_port, web_url = excluded.web_url,"
                           " version = excluded.version, max_players = excluded.max_players, max_lobbies = excluded.max_lobbies,"
                           " custom = excluded.custom, draining = excluded.draining, connected_at = excluded.connected_at,"
                           " heartbeat_at = excluded.heartbeat_at",
                 .mysql = "INSERT INTO mm_nodes (node_id, brain_id, region, name, public_host, public_port, web_url, version,"
                          " max_players, max_lobbies, custom, draining, connected_at, heartbeat_at) VALUES ($1, $2, $3, $4, $5,"
                          " $6, $7, $8, $9, $10, {bool:$11}, {bool:$12}, {from_ms:$14}, {now}) ON DUPLICATE KEY UPDATE"
                          " brain_id = VALUES(brain_id), region = VALUES(region), name = VALUES(name),"
                          " public_host = VALUES(public_host), public_port = VALUES(public_port), web_url = VALUES(web_url),"
                          " version = VALUES(version), max_players = VALUES(max_players), max_lobbies = VALUES(max_lobbies),"
                          " custom = VALUES(custom), draining = VALUES(draining), connected_at = VALUES(connected_at),"
                          " heartbeat_at = VALUES(heartbeat_at)"),
             n < 14 ? n : 14, p);
    node_lobbies(&x, P(0), P(12));
    return owned_result(&x, 1);
}
const kn_proc P_MM_NODE_TAKEOVER = {
    "node_takeover",
    "WITH n AS ("
    " INSERT INTO mm_nodes (node_id, brain_id, region, name, public_host, public_port, web_url, version,"
    "                       max_players, max_lobbies, custom, draining, connected_at, heartbeat_at)"
    " VALUES ($1::text, $2::text, $3::text, $4::text, $5::text, $6::int, $7::text, $8::text, $9::int, $10::int, $11::boolean, $12::boolean, to_timestamp($14::double precision / 1000), now())"
    " ON CONFLICT (node_id) DO UPDATE SET brain_id = EXCLUDED.brain_id, region = EXCLUDED.region,"
    "   name = EXCLUDED.name, public_host = EXCLUDED.public_host, public_port = EXCLUDED.public_port,"
    "   web_url = EXCLUDED.web_url, version = EXCLUDED.version, max_players = EXCLUDED.max_players,"
    "   max_lobbies = EXCLUDED.max_lobbies, custom = EXCLUDED.custom, draining = EXCLUDED.draining,"
    "   connected_at = EXCLUDED.connected_at, heartbeat_at = now()"
    " RETURNING node_id)," SQL_NODE_LOBBIES,
    run_node_takeover};

/* same, but only while this brain still owns the row (never resurrects a
 * deleted node and never steals one another brain took over) */
static kn_db_result *run_node_update(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t owned = dbx_exec(&x,
        SQL(.any = "UPDATE mm_nodes SET region = $3, name = $4, public_host = $5, public_port = $6, web_url = $7, version = $8,"
                   " max_players = $9, max_lobbies = $10, custom = {bool:$11}, draining = {bool:$12}, heartbeat_at = {now}"
                   " WHERE node_id = $1 AND brain_id = $2"),
        n < 12 ? n : 12, p);
    if (owned > 0) node_lobbies(&x, P(0), P(12));
    return owned_result(&x, owned > 0 ? 1 : 0);
}
const kn_proc P_MM_NODE_UPDATE = {
    "node_update",
    "WITH n AS ("
    " UPDATE mm_nodes SET region = $3::text, name = $4::text, public_host = $5::text, public_port = $6::int,"
    "   web_url = $7::text,"
    "   version = $8::text, max_players = $9::int, max_lobbies = $10::int, custom = $11::boolean,"
    "   draining = $12::boolean, heartbeat_at = now()"
    " WHERE node_id = $1::text AND brain_id = $2::text RETURNING node_id)," SQL_NODE_LOBBIES,
    run_node_update};

/* (brain, ids literal "{a,b}") */
static kn_db_result *run_node_heartbeat(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *ids = pg_array(P(1));
    const cJSON *it;
    cJSON_ArrayForEach(it, ids) {
        dbx_exec(&x, SQL(.any = "UPDATE mm_nodes SET heartbeat_at = {now} WHERE brain_id = $1 AND node_id = $2"),
                 KN_DB_ARGS(P(0), it->valuestring));
    }
    cJSON_Delete(ids);
    return dbx_finish(&x, kn_db_result_command(0));
}
const kn_proc P_MM_NODE_HEARTBEAT = {
    "node_heartbeat", "UPDATE mm_nodes SET heartbeat_at = now() WHERE brain_id = $1::text AND node_id = ANY($2::text[])",
    run_node_heartbeat};

/* (brain, ttl_s): live nodes of the other brains with their lobby tuples */
static kn_db_result *run_refresh_nodes(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    kn_db_result *ns = dbx_q(&x,
        SQL(.any = "SELECT node_id, brain_id, region, name, public_host, public_port, web_url, version, max_players, max_lobbies,"
                   " custom, draining, {epoch_ms:heartbeat_at} AS hb FROM mm_nodes"
                   " WHERE brain_id <> $1 AND heartbeat_at > {ago:$2} ORDER BY connected_at, node_id"),
        KN_DB_ARGS(P(0), P(1)));
    const char *names[14] = {"node_id", "brain_id", "region", "name", "public_host", "public_port", "web_url",
                             "version", "max_players", "max_lobbies", "custom", "draining", "hb", "lobbies"};
    const kn_db_type types[14] = {KN_DB_T_TEXT, KN_DB_T_TEXT, KN_DB_T_TEXT, KN_DB_T_TEXT, KN_DB_T_TEXT, KN_DB_T_INT, KN_DB_T_TEXT,
                                  KN_DB_T_TEXT, KN_DB_T_INT,  KN_DB_T_INT,  KN_DB_T_BOOL, KN_DB_T_BOOL, KN_DB_T_INT, KN_DB_T_TEXT};
    kn_db_result *out = kn_db_result_new(14, names, types);
    for (int i = 0; ns && out && i < kn_db_rows(ns); i++) {
        kn_db_result *ls = dbx_q(&x, SQL(.any = "SELECT tuple FROM mm_lobbies WHERE node_id = $1 ORDER BY game_id"),
                                 KN_DB_ARGS(kn_db_get(ns, i, 0)));
        size_t cap = 3, len = 1;
        for (int k = 0; ls && k < kn_db_rows(ls); k++) cap += strlen(kn_db_get(ls, k, 0)) + 1;
        char *lob = malloc(cap);
        if (lob) {
            lob[0] = '[';
            for (int k = 0; ls && k < kn_db_rows(ls); k++) {
                if (k) lob[len++] = ',';
                size_t l = strlen(kn_db_get(ls, k, 0));
                memcpy(lob + len, kn_db_get(ls, k, 0), l);
                len += l;
            }
            lob[len++] = ']';
            lob[len] = 0;
        }
        kn_db_result_free(ls);
        const char *row[14];
        for (int k = 0; k < 13; k++) row[k] = kn_db_isnull(ns, i, k) ? NULL : kn_db_get(ns, i, k);
        row[13] = lob ? lob : "[]";
        kn_db_result_add_row(out, row);
        free(lob);
    }
    kn_db_result_free(ns);
    return dbx_finish(&x, out);
}
const kn_proc P_MM_REFRESH_NODES = {
    "refresh_nodes",
    "SELECT n.node_id, n.brain_id, n.region, n.name, n.public_host, n.public_port, n.web_url, n.version,"
    "       n.max_players, n.max_lobbies, n.custom, n.draining,"
    "       (extract(epoch FROM n.heartbeat_at) * 1000)::bigint AS hb,"
    "       coalesce((SELECT json_agg(l.tuple ORDER BY l.game_id) FROM mm_lobbies l"
    "                 WHERE l.node_id = n.node_id), '[]'::json)::text AS lobbies"
    "  FROM mm_nodes n"
    " WHERE n.brain_id <> $1::text AND n.heartbeat_at > now() - make_interval(secs => $2::int)"
    " ORDER BY n.connected_at, n.node_id",
    run_refresh_nodes};

/* (from, target brain, node, region, config json, account, name) -> id */
static kn_db_result *run_host_request(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    int64_t acc = PI(5);
    int64_t id = dbx_insert_id(&x,
        SQL(.any = "INSERT INTO mm_host_requests (from_brain, target_brain, target_node, region, config, host_account, host_name)"
                   " VALUES ($1, $2, $3, $4, $5, $6, $7)"),
        7, (const char *const[]){P(0), P(1), P(2), nz(P(3)), P(4) ? P(4) : "{}", acc ? P(5) : NULL, nz(P(6))});
    return dbx_finish(&x, kn_db_result_int("id", id));
}
const kn_proc P_MM_HOST_REQUEST = {
    "host_request",
    "INSERT INTO mm_host_requests (from_brain, target_brain, target_node, region, config,"
    "                              host_account, host_name)"
    " VALUES ($1::text, $2::text, $3::text, $4::text, coalesce($5::jsonb, '{}'::jsonb), NULLIF($6::bigint, 0), $7::text) RETURNING id",
    run_host_request};

/* (brain): claim pending host requests -> id, from_brain, target_node, config, host_account, host_name */
static kn_db_result *run_host_claim(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    kn_db_result *r = dbx_q(&x,
        SQL(.any = "SELECT id, from_brain, target_node, config AS config__t, host_account, coalesce(host_name, '') AS host_name"
                   " FROM mm_host_requests WHERE target_brain = $1 AND status = 'pending' AND created_at > {ago:60} {for_update}"),
        KN_DB_ARGS(P(0)));
    for (int i = 0; r && i < kn_db_rows(r); i++)
        dbx_exec(&x, SQL(.any = "UPDATE mm_host_requests SET status = 'sent', updated_at = {now} WHERE id = $1"),
                 KN_DB_ARGS(kn_db_get(r, i, 0)));
    return dbx_finish(&x, r);
}
const kn_proc P_MM_HOST_CLAIM = {
    "host_claim",
    "UPDATE mm_host_requests SET status = 'sent', updated_at = now()"
    " WHERE target_brain = $1::text AND status = 'pending' AND created_at > now() - interval '1 minute'"
    " RETURNING id, from_brain, target_node, config::text, host_account, coalesce(host_name, '')",
    run_host_claim};
