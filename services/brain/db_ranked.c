/*
 * services/brain/db_ranked.c - Krunker v10.0.2 ranked on the tables of migration 0205_ranked_v10
 * (doc/v10/ranked-impl.md). C on every driver (no .pg text): the rating rules are
 * shared/src/ranked.c, the same code the client and the tests use, so a match is rated in one
 * transaction whatever the store.
 *
 *   P_RK_JOIN     (account, username, client key, regions csv, cooldown_s) ->
 *                 {code: null | COOLDOWN | ALREADY_IN_MATCH, cooldown (ms), serverUrl, assignment}
 *   P_RK_POLL     (client key) -> {status: QUEUED | MATCHED, assignment, queueSize, timeInQueue} | null
 *   P_RK_EXPIRE   (poll_s, creating_s, match_s) -> housekeeping (any brain, idempotent)
 *   P_RK_MATCH_RUN (brain, season, team size, window, growth, max, maps, ruleset) -> the matches formed
 *                 (one transaction holding the matcher lock: C procs cannot join an async tx)
 *   P_RK_MATCH_READY  (match, game id, {"<account>": assignment}) -> live, queue rows MATCHED
 *   P_RK_MATCH_FAILED (match, reason) -> aborted, the players queue again after 10 s
 *   P_RK_MATCH_FINISH (match, result) -> [{accountId, name, team, old, new, delta, rank, mmr, rp,
 *                 abandoned}] once ([] when the match is unknown / closed): TrueSkill, profiles,
 *                 Ranked Points, the abandoners' bans, players.player_elo = the new MMR
 *   P_RK_CLAIM    (account, season, region) -> {success, reward, balance} (next milestone)
 *   P_RK_SEASON_CLOSE (season) -> {season, accounts}: end-of-month RP conversion (best region)
 *   P_RK_PURCHASE (account, shop id, season) -> {success, balance, skin}
 */
#include "db_procs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/crypto.h"
#include "knsvc/json.h"
#include "ranked.h"

#define P(i) (n > (i) ? p[i] : NULL)
#define PI(i) pint(P(i))
#define MAX_PLAYERS 32

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

static char *fmt_double(char buf[40], const double v) {
    snprintf(buf, 40, "%.10g", v);
    return buf;
}
#define DS(v) fmt_double((char[40]){0}, (v))

/* ------------------------------------------------------------------ profiles */

/* The skill an account plays `season` / `region` with: its row, else a soft reset of its latest
 * earlier profile (any region; the v7 history is the Beta season's "na" rows of 0205), else the
 * default. *matches = the row's wins + losses + draws (0 without a row). */
static RankedSkill db_rk_skill(Dbx *x, const int64_t acc, const int season, const char *region, int *matches, int *exists) {
    if (exists) *exists = 0;
    if (matches) *matches = 0;
    cJSON *row = dbx_row(x, SQL(.any = "SELECT mu AS mu__f, sigma AS sigma__f, wins + losses + draws AS m__i FROM ranked_profiles"
                                       " WHERE account_id = $1 AND season = {int:$2} AND region = $3"),
                         KN_DB_ARGS(I64S(acc), I64S(season), region));
    if (row) {
        RankedSkill s = {jnum(row, "mu"), jnum(row, "sigma")};
        if (matches) *matches = (int) jnum(row, "m");
        if (exists) *exists = 1;
        cJSON_Delete(row);
        return s;
    }
    cJSON *prev = dbx_row(x, SQL(.any = "SELECT mu AS mu__f, sigma AS sigma__f FROM ranked_profiles"
                                        " WHERE account_id = $1 AND season < {int:$2}"
                                        " ORDER BY season DESC, (wins + losses + draws) DESC LIMIT 1"),
                          KN_DB_ARGS(I64S(acc), I64S(season)));
    if (prev) {
        const RankedSkill last = {jnum(prev, "mu"), jnum(prev, "sigma")};
        cJSON_Delete(prev);
        return ranked_season_seed(&last, -1);
    }
    return ranked_season_seed(NULL, -1);
}

static int64_t rp_add(Dbx *x, const int64_t acc, const int64_t amount, const char *source, const char *ref) {
    if (amount < 0) {
        if (dbx_exec(x, SQL(.any = "UPDATE ranked_points SET balance = balance + {int:$2}, updated_at = {now}"
                                   " WHERE account_id = $1 AND balance + {int:$2} >= 0"),
                     KN_DB_ARGS(I64S(acc), I64S(amount))) <= 0) {
            if (!dbx_failed(x)) dbx_refuse(x, "Not enough Ranked Points");
            return -1;
        }
    } else if (amount > 0) {
        dbx_exec(x, SQL(.pg = "INSERT INTO ranked_points (account_id, balance) VALUES ($1::bigint, $2::bigint)"
                              " ON CONFLICT (account_id) DO UPDATE SET balance = ranked_points.balance + EXCLUDED.balance, updated_at = now()",
                        .sqlite = "INSERT INTO ranked_points (account_id, balance) VALUES ($1, $2)"
                                  " ON CONFLICT (account_id) DO UPDATE SET balance = balance + excluded.balance, updated_at = {now}",
                        .mysql = "INSERT INTO ranked_points (account_id, balance) VALUES ($1, $2)"
                                 " ON DUPLICATE KEY UPDATE balance = balance + VALUES(balance), updated_at = {now}"),
                 KN_DB_ARGS(I64S(acc), I64S(amount)));
    }
    if (amount != 0)
        dbx_exec(x, SQL(.any = "INSERT INTO ranked_points_tx (account_id, amount, source, ref) VALUES ($1, $2, $3, $4)"),
                 KN_DB_ARGS(I64S(acc), I64S(amount), source, nz(ref)));
    return dbx_int(x, SQL(.any = "SELECT balance FROM ranked_points WHERE account_id = $1"), KN_DB_ARGS(I64S(acc)), 0);
}

/* ------------------------------------------------------------------ queue */

static kn_db_result *run_rk_join(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *acc = P(0);
    cJSON *o = cJSON_CreateObject();
    const int64_t cd = dbx_int(&x, SQL(.any = "SELECT {epoch_ms:until} - {epoch_ms:{now}} FROM mm_ranked_bans WHERE account_id = $1 AND until > {now}"),
                               KN_DB_ARGS(acc), -1);
    if (cd >= 0) {
        cJSON_AddStringToObject(o, "code", "COOLDOWN");
        cJSON_AddNumberToObject(o, "cooldown", (double) cd);
        return dbx_json(&x, "join", o);
    }
    cJSON *live = dbx_row(&x, SQL(.any = "SELECT mp.assignment FROM mm_ranked_match_players mp JOIN mm_ranked_matches m ON m.match_id = mp.match_id"
                                         " WHERE mp.account_id = $1 AND m.status IN ('creating', 'live') AND NOT mp.abandoned"
                                         " ORDER BY m.created_at DESC LIMIT 1"),
                          KN_DB_ARGS(acc));
    if (live) {
        cJSON_AddStringToObject(o, "code", "ALREADY_IN_MATCH");
        cJSON *as = cJSON_Parse(jstr(live, "assignment"));
        if (cJSON_IsObject(as)) {
            cJSON_AddStringToObject(o, "serverUrl", jstr(as, "connection"));
            cJSON_AddItemToObject(o, "assignment", as);
        } else {
            cJSON_Delete(as);
        }
        cJSON_Delete(live);
        return dbx_json(&x, "join", o);
    }
    /* a new search replaces the account's old one (v10: a new WebSocket) */
    dbx_exec(&x, SQL(.any = "DELETE FROM ranked_queue WHERE account_id = $1 AND (status = 'QUEUED' OR match_id IS NULL)"), KN_DB_ARGS(acc));
    if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM ranked_queue WHERE account_id = $1"), KN_DB_ARGS(acc))) {
        cJSON_AddStringToObject(o, "code", "ALREADY_IN_MATCH");
        return dbx_json(&x, "join", o);
    }
    dbx_exec(&x, SQL(.any = "INSERT INTO ranked_queue (account_id, username, client_key, regions) VALUES ($1, $2, $3, $4)"),
             KN_DB_ARGS(acc, nz(P(1)), nz(P(2)), nz(P(3))));
    cJSON_AddNullToObject(o, "code");
    return dbx_json(&x, "join", o);
}
const kn_proc P_RK_JOIN = {"rk_join", NULL, run_rk_join};

static kn_db_result *run_rk_poll(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    if (dbx_exec(&x, SQL(.any = "UPDATE ranked_queue SET last_poll = {now} WHERE client_key = $1"), KN_DB_ARGS(nz(P(0)))) <= 0)
        return dbx_json(&x, "poll", NULL);
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT status, assignment, regions, ({epoch_ms:{now}} - {epoch_ms:queued_at}) AS t__i"
                                        " FROM ranked_queue WHERE client_key = $1"),
                         KN_DB_ARGS(nz(P(0))));
    if (!row) return dbx_json(&x, "poll", NULL);
    cJSON *o = cJSON_CreateObject();
    const char *st = jstr(row, "status");
    const int matched = !strcmp(st, "MATCHED");
    cJSON_AddStringToObject(o, "status", matched ? "MATCHED" : "QUEUED");
    cJSON_AddNumberToObject(o, "timeInQueue", floor(jnum(row, "t") / 1000.0));
    if (matched) {
        cJSON *as = cJSON_Parse(jstr(row, "assignment"));
        cJSON_AddItemToObject(o, "assignment", cJSON_IsObject(as) ? as : (cJSON_Delete(as), cJSON_CreateObject()));
    }
    cJSON_AddNumberToObject(o, "queueSize",
                            (double) dbx_int(&x, SQL(.any = "SELECT count(*) FROM ranked_queue WHERE status = 'QUEUED' AND match_id IS NULL"), 0, NULL, 0));
    cJSON_Delete(row);
    return dbx_json(&x, "poll", o);
}
const kn_proc P_RK_POLL = {"rk_poll", NULL, run_rk_poll};

static kn_db_result *run_rk_expire(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    /* a searching client that stopped polling left (v10: the WebSocket closed) */
    dbx_exec(&x, SQL(.any = "DELETE FROM ranked_queue WHERE status = 'QUEUED' AND match_id IS NULL AND last_poll < {ago:$1}"), KN_DB_ARGS(P(0)));
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_matches SET status = 'aborted', finished_at = {now}, result = '{\"reason\":\"host timeout\"}'"
                            " WHERE status = 'creating' AND created_at < {ago:$1}"),
             KN_DB_ARGS(P(1)));
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_matches SET status = 'aborted', finished_at = {now}, result = '{\"reason\":\"no result\"}'"
                            " WHERE status = 'live' AND started_at < {ago:$1}"),
             KN_DB_ARGS(P(2)));
    /* queue rows of matches that are over (matched rows are kept until the match ends: rejoin) */
    dbx_exec(&x, SQL(.any = "DELETE FROM ranked_queue WHERE match_id IS NOT NULL AND NOT EXISTS (SELECT 1 FROM mm_ranked_matches m"
                            " WHERE m.match_id = ranked_queue.match_id AND m.status IN ('creating', 'live'))"),
             0, NULL);
    dbx_exec(&x, SQL(.any = "DELETE FROM mm_ranked_bans WHERE until < {now}"), 0, NULL);
    return dbx_finish(&x, kn_db_result_command(0));
}
const kn_proc P_RK_EXPIRE = {"rk_expire", NULL, run_rk_expire};

/* The matcher / season lock inside a proc's own transaction: postgres an advisory xact lock
 * (kn_rq_lock() + which), sqlite the BEGIN IMMEDIATE write lock it already holds, mysql the
 * mm_ranked_lock row without waiting. 1 = held. */
static int rk_lock(Dbx *x, const int which) {
    if (dbx_is_pg(x))
        return dbx_int(x, SQL(.pg = "SELECT CASE WHEN pg_try_advisory_xact_lock(kn_rq_lock() + $1::bigint) THEN 1 ELSE 0 END"),
                       KN_DB_ARGS(I64S(which + 1)), 0) == 1;
    if (dbx_is_mysql(x)) {
        kn_db_result *r = kn_dbconn_exec_sql(x->c, SQL(.any = "SELECT id FROM mm_ranked_lock WHERE id = 1 FOR UPDATE NOWAIT"), 0, NULL);
        const int got = kn_db_ok(r) && kn_db_rows(r) > 0;
        if (!kn_db_ok(r) && kn_db_errclass_of(r) != KN_DB_E_RETRY && !x->err) x->err = r;
        else kn_db_result_free(r);
        return got;
    }
    return 1;
}

typedef struct Cand {
    int64_t a;
    char name[64];
    char regions[32];
    double wait_s;
    RankedSkill skill;  /* in the region being matched */
    int used;
} Cand;

static int by_wait(const void *pa, const void *pb) {
    const Cand *const *a = pa, *const *b = pb;
    return (*a)->wait_s < (*b)->wait_s ? 1 : (*a)->wait_s > (*b)->wait_s ? -1 : 0;
}

static int by_mu(const void *pa, const void *pb) {
    const Cand *const *a = pa, *const *b = pb;
    return (*a)->skill.mu < (*b)->skill.mu ? 1 : (*a)->skill.mu > (*b)->skill.mu ? -1 : 0;
}

static int has_region(const char *list, const char *code) {
    char buf[32];
    snprintf(buf, sizeof buf, "%s", list);
    for (char *t = strtok(buf, ","); t; t = strtok(NULL, ","))
        if (!strcmp(t, code)) return 1;
    return 0;
}

/* (brain id, season, team size, window, window growth / s, window max, maps csv) ->
 * [{matchId, region, map, teams: [[{accountId, name, mu, sigma}], [...]]}]: every match formed
 * this round, already written (players reserved). Nothing when another brain holds the lock. */
static kn_db_result *run_rk_match_run(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *out = cJSON_CreateArray();
    if (!rk_lock(&x, 0)) return dbx_json(&x, "matches", out);
    const int season = (int) PI(1), size = PI(2) > 0 && PI(2) <= 8 ? (int) PI(2) : RANKED_TEAM_SIZE;
    const char *ws = P(3), *gs = P(4), *ms = P(5);
    const double window = ws ? strtod(ws, NULL) : 150, growth = gs ? strtod(gs, NULL) : 10, wmax = ms ? strtod(ms, NULL) : 1000;
    int maps[16], map_count = 0;
    {
        char buf[128];
        snprintf(buf, sizeof buf, "%s", nz(P(6)));
        for (char *t = strtok(buf, ","); t && map_count < 16; t = strtok(NULL, ",")) maps[map_count++] = atoi(t);
    }
    if (!map_count) maps[map_count++] = RANKED_ALWAYS_MAP;
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT account_id AS a__i, username AS n, regions,"
                                          " ({epoch_ms:{now}} - {epoch_ms:queued_at}) / 1000.0 AS wait_s__f FROM ranked_queue"
                                          " WHERE status = 'QUEUED' AND match_id IS NULL AND (hold_until IS NULL OR hold_until <= {now})"
                                          " ORDER BY queued_at LIMIT 512"),
                           0, NULL);
    const int count = cJSON_GetArraySize(rows);
    Cand *cand = count > 0 ? calloc((size_t) count, sizeof(Cand)) : NULL;
    Cand **order = count > 0 ? calloc((size_t) count, sizeof(Cand *)) : NULL;
    for (int i = 0; cand && order && i < count; i++) {
        const cJSON *r = cJSON_GetArrayItem(rows, i);
        cand[i].a = (int64_t) jnum(r, "a");
        snprintf(cand[i].name, sizeof cand[i].name, "%s", jstr(r, "n"));
        snprintf(cand[i].regions, sizeof cand[i].regions, "%s", jstr(r, "regions"));
        cand[i].wait_s = jnum(r, "wait_s");
    }
    for (int ri = 2; cand && order && ri <= 4 && !dbx_failed(&x); ri++) {
        const char *region = ranked_region_code(ri);
        int m = 0;
        for (int i = 0; i < count; i++) {
            if (cand[i].used || !has_region(cand[i].regions, region)) continue;
            cand[i].skill = db_rk_skill(&x, cand[i].a, season, region, NULL, NULL);
            order[m++] = &cand[i];
        }
        qsort(order, (size_t) m, sizeof(Cand *), by_wait);
        for (int ai = 0; ai < m; ai++) {
            Cand *anchor = order[ai];
            if (anchor->used) continue;
            double w = window + growth * anchor->wait_s;
            if (w > wmax) w = wmax;
            const int amr = ranked_display(ranked_conservative(anchor->skill));
            Cand *pick[16];
            int k = 0;
            pick[k++] = anchor;
            for (int bi = 0; bi < m && k < 2 * size; bi++) {
                Cand *o = order[bi];
                if (o == anchor || o->used) continue;
                if (fabs((double) (ranked_display(ranked_conservative(o->skill)) - amr)) > w) continue;
                pick[k++] = o;
            }
            if (k < 2 * size) continue;
            /* snake draft by mu: A B B A A B B A */
            qsort(pick, (size_t) k, sizeof(Cand *), by_mu);
            char hex[33], id[40];
            kn_random_hex(hex, sizeof hex, 16);
            snprintf(id, sizeof id, "%.8s-%.4s-%.4s-%.4s-%.12s", hex, hex + 8, hex + 12, hex + 16, hex + 20);
            const int map = maps[kn_random_u64() % (uint64_t) map_count];
            cJSON *mo = cJSON_CreateObject();
            cJSON_AddStringToObject(mo, "matchId", id);
            cJSON_AddStringToObject(mo, "region", region);
            cJSON_AddNumberToObject(mo, "map", map);
            cJSON_AddNumberToObject(mo, "season", season);
            cJSON *teams = cJSON_AddArrayToObject(mo, "teams");
            cJSON *t0 = cJSON_CreateArray(), *t1 = cJSON_CreateArray();
            cJSON_AddItemToArray(teams, t0);
            cJSON_AddItemToArray(teams, t1);
            dbx_exec(&x, SQL(.any = "INSERT INTO mm_ranked_matches (match_id, queue_id, region, brain_id, season, map_index, mode_index, ruleset)"
                                    " VALUES ($1, 'ranked10', $2, $3, {int:$4}, {int:$5}, 2, $6)"),
                     KN_DB_ARGS(id, region, nz(P(0)), I64S(season), I64S(map), P(7) && *P(7) ? P(7) : "v10"));
            for (int i = 0; i < k; i++) {
                const int t = (i % 4 == 0 || i % 4 == 3) ? 0 : 1;
                Cand *pl = pick[i];
                pl->used = 1;
                cJSON *e = cJSON_CreateObject();
                cJSON_AddNumberToObject(e, "accountId", (double) pl->a);
                cJSON_AddStringToObject(e, "name", pl->name);
                cJSON_AddNumberToObject(e, "mmr", ranked_display(ranked_conservative(pl->skill)));
                cJSON_AddItemToArray(t ? t1 : t0, e);
                dbx_exec(&x, SQL(.any = "INSERT INTO mm_ranked_match_players (match_id, account_id, username, team, rating_key, rating_before)"
                                        " VALUES ($1, $2, $3, {int:$4}, 'mmr', {int:$5})"),
                         KN_DB_ARGS(id, I64S(pl->a), pl->name, I64S(t), I64S(ranked_display(ranked_conservative(pl->skill)))));
                dbx_exec(&x, SQL(.any = "UPDATE ranked_queue SET match_id = $2 WHERE account_id = $1"), KN_DB_ARGS(I64S(pl->a), id));
            }
            cJSON_AddItemToArray(out, mo);
        }
    }
    free(cand);
    free(order);
    cJSON_Delete(rows);
    return dbx_json(&x, "matches", out);
}
const kn_proc P_RK_MATCH_RUN = {"rk_match_run", NULL, run_rk_match_run};

/* (match, game id, {"<account>": assignment}) */
static kn_db_result *run_rk_match_ready(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_matches SET status = 'live', game_id = $2, started_at = {now} WHERE match_id = $1 AND status = 'creating'"),
             KN_DB_ARGS(P(0), nz(P(1))));
    cJSON *as = cJSON_Parse(nz(P(2)));
    const cJSON *it;
    cJSON_ArrayForEach(it, as) {
        char *s = cJSON_PrintUnformatted(it);
        dbx_exec(&x, SQL(.any = "UPDATE ranked_queue SET status = 'MATCHED', assignment = $3 WHERE account_id = $1 AND match_id = $2"),
                 KN_DB_ARGS(it->string, P(0), nz(s)));
        dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_match_players SET assignment = $3 WHERE account_id = $1 AND match_id = $2"),
                 KN_DB_ARGS(it->string, P(0), nz(s)));
        free(s);
    }
    cJSON_Delete(as);
    return dbx_finish(&x, kn_db_result_command(0));
}
const kn_proc P_RK_MATCH_READY = {"rk_match_ready", NULL, run_rk_match_ready};

static kn_db_result *run_rk_match_failed(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *ro = cJSON_CreateObject();
    cJSON_AddStringToObject(ro, "reason", nz(P(1)));
    char *reason = cJSON_PrintUnformatted(ro);
    cJSON_Delete(ro);
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_matches SET status = 'aborted', finished_at = {now}, result = $2"
                            " WHERE match_id = $1 AND status IN ('creating', 'live')"),
             KN_DB_ARGS(P(0), nz(reason)));
    free(reason);
    /* back in the queue, not matched again for 10 s (no game server free: no hot loop) */
    dbx_exec(&x, SQL(.any = "UPDATE ranked_queue SET match_id = NULL, status = 'QUEUED', assignment = NULL, hold_until = {later:10}"
                            " WHERE match_id = $1"),
             KN_DB_ARGS(P(0)));
    return dbx_finish(&x, kn_db_result_command(0));
}
const kn_proc P_RK_MATCH_FAILED = {"rk_match_failed", NULL, run_rk_match_failed};
/* ------------------------------------------------------------------ results */

static const cJSON *result_player(const cJSON *result, const int64_t acc) {
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(result, "players")) {
        if ((int64_t) jnum(it, "accountId") == acc) return it;
    }
    return NULL;
}

static int result_abandoned(const cJSON *result, const int64_t acc) {
    const cJSON *it;
    cJSON_ArrayForEach(it, cJSON_GetObjectItemCaseSensitive(result, "abandoned")) {
        if (cJSON_IsNumber(it) && (int64_t) it->valuedouble == acc) return 1;
    }
    return 0;
}

/* (match, result {winner, players [{accountId, score, kills, deaths, assists, damage, objective,
 * timeMs, reward}], abandoned [ids]}, cooldown_s) */
static kn_db_result *run_rk_match_finish(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    cJSON *result = cJSON_Parse(nz(P(1)));
    if (!cJSON_IsObject(result)) {
        cJSON_Delete(result);
        result = cJSON_CreateObject();
    }
    cJSON *match = dbx_row(&x, SQL(.any = "SELECT region, season AS season__i FROM mm_ranked_matches WHERE match_id = $1 AND status = 'live'"),
                           KN_DB_ARGS(P(0)));
    cJSON *out = cJSON_CreateArray();
    if (!match) {
        cJSON_Delete(result);
        return dbx_json(&x, "finish", out);
    }
    char *stored = cJSON_PrintUnformatted(result);
    dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_matches SET status = 'done', finished_at = {now}, result = $2 WHERE match_id = $1 AND status = 'live'"),
             KN_DB_ARGS(P(0), nz(stored)));
    free(stored);
    char region[16];
    snprintf(region, sizeof region, "%s", jstr(match, "region"));
    const int season = (int) jnum(match, "season");
    cJSON_Delete(match);
    cJSON *players = dbx_rows(&x, SQL(.any = "SELECT account_id AS a__i, username AS n, team AS t__i FROM mm_ranked_match_players"
                                             " WHERE match_id = $1 ORDER BY team, account_id"),
                              KN_DB_ARGS(P(0)));
    const int count = cJSON_GetArraySize(players);
    if (count <= 0 || count > MAX_PLAYERS) {
        cJSON_Delete(players);
        cJSON_Delete(result);
        return dbx_json(&x, "finish", out);
    }
    RankedSkill skills[MAX_PLAYERS], before[MAX_PLAYERS];
    int team[MAX_PLAYERS], quit[MAX_PLAYERS], matches[MAX_PLAYERS], exists[MAX_PLAYERS];
    int64_t ids[MAX_PLAYERS];
    for (int i = 0; i < count; i++) {
        const cJSON *pl = cJSON_GetArrayItem(players, i);
        ids[i] = (int64_t) jnum(pl, "a");
        team[i] = (int) jnum(pl, "t") ? 1 : 0;
        quit[i] = result_abandoned(result, ids[i]);
        skills[i] = db_rk_skill(&x, ids[i], season, region, &matches[i], &exists[i]);
        /* "Your rating will decay after 7+ days of inactivity": sigma grows back */
        const int64_t idle_ms = exists[i] ? dbx_int(&x, SQL(.any = "SELECT {epoch_ms:{now}} - {epoch_ms:last_match_at} FROM ranked_profiles"
                                                                    " WHERE account_id = $1 AND season = {int:$2} AND region = $3 AND last_match_at IS NOT NULL"),
                                                     KN_DB_ARGS(I64S(ids[i]), I64S(season), region), 0)
                                          : 0;
        skills[i] = ranked_decay(skills[i], idle_ms / 86400000.0);
        before[i] = skills[i];
    }
    const int winner = (int) (cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(result, "winner")) ? jnum(result, "winner") : -1);
    ranked_update(skills, team, quit, count, winner == 0 || winner == 1 ? winner : -1);
    const int64_t cooldown_s = PI(2) > 0 ? PI(2) : RANKED_COOLDOWN_MS / 1000;
    for (int i = 0; i < count; i++) {
        const cJSON *pl = cJSON_GetArrayItem(players, i);
        const cJSON *st = result_player(result, ids[i]);
        const int won = !quit[i] && (winner == 0 || winner == 1) && team[i] == winner;
        const int lost = quit[i] || ((winner == 0 || winner == 1) && team[i] != winner);
        const int drew = !won && !lost;
        const double cr = ranked_conservative(skills[i]);
        const int old_mmr = ranked_display(ranked_conservative(before[i])), new_mmr = ranked_display(cr);
        const int kills = (int) jnum(st, "kills"), deaths = (int) jnum(st, "deaths"), assists = (int) jnum(st, "assists");
        const int score = (int) jnum(st, "score"), damage = (int) jnum(st, "damage"), objective = (int) jnum(st, "objective");
        const int64_t time_ms = (int64_t) jnum(st, "timeMs");
        const int rp = quit[i] ? 0 : ranked_match_points((int) jnum(st, "reward"));
        if (!exists[i])
            dbx_exec(&x, SQL(.any = "INSERT INTO ranked_profiles (account_id, season, region, mu, sigma, cr) VALUES ($1, {int:$2}, $3, {float:$4}, {float:$5}, {float:$6})"),
                     KN_DB_ARGS(I64S(ids[i]), I64S(season), region, DS(before[i].mu), DS(before[i].sigma), DS(ranked_conservative(before[i]))));
        dbx_exec(&x, SQL(.any = "UPDATE ranked_profiles SET mu = {float:$4}, sigma = {float:$5}, cr = {float:$6},"
                                " wins = wins + {int:$7}, losses = losses + {int:$8}, draws = draws + {int:$9}, abandons = abandons + {int:$10},"
                                " total_kills = total_kills + {int:$11}, total_deaths = total_deaths + {int:$12},"
                                " total_assists = total_assists + {int:$13}, total_score = total_score + {int:$14},"
                                " total_objective_score = total_objective_score + {int:$15}, total_damage_done = total_damage_done + {int:$16},"
                                " total_time_played = total_time_played + {int:$17},"
                                " highest_kill_count = {greatest:highest_kill_count, {int:$11}},"
                                " highest_damage_in_match = {greatest:highest_damage_in_match, {int:$16}},"
                                " peak_mmr = {greatest:peak_mmr, {int:$18}}, last_match_at = {now}, updated_at = {now}"
                                " WHERE account_id = $1 AND season = {int:$2} AND region = $3"),
                 18, (const char *const[]){I64S(ids[i]), I64S(season), region, DS(skills[i].mu), DS(skills[i].sigma), DS(cr), I64S(won),
                                           I64S(lost), I64S(drew), I64S(quit[i]), I64S(kills), I64S(deaths), I64S(assists), I64S(score),
                                           I64S(objective), I64S(damage), I64S(time_ms), I64S(new_mmr)});
        dbx_exec(&x, SQL(.any = "UPDATE mm_ranked_match_players SET rating_after = {int:$3}, kills = {int:$4}, deaths = {int:$5},"
                                " assists = {int:$6}, score = {int:$7}, damage_done = {int:$8}, victory = $9, abandoned = {bool:$10}, rp = {int:$11}"
                                " WHERE match_id = $1 AND account_id = $2"),
                 11, (const char *const[]){P(0), I64S(ids[i]), I64S(new_mmr), I64S(kills), I64S(deaths), I64S(assists), I64S(score),
                                           I64S(damage), won ? "1" : lost ? "0" : NULL, quit[i] ? "1" : "0", I64S(rp)});
        /* the account's player_elo (profile data, v10 ranked_rank): the new MMR */
        dbx_exec(&x, SQL(.any = "UPDATE players SET player_elo = {int:$2} WHERE player_id = $1"), KN_DB_ARGS(I64S(ids[i]), I64S(new_mmr)));
        if (rp > 0) rp_add(&x, ids[i], rp, "match", P(0));
        if (quit[i])
            dbx_exec(&x, SQL(.pg = "INSERT INTO mm_ranked_bans (account_id, until, reason) VALUES ($1::bigint, now() + make_interval(secs => $2::integer), 'abandon')"
                                   " ON CONFLICT (account_id) DO UPDATE SET until = EXCLUDED.until, reason = EXCLUDED.reason",
                             .sqlite = "INSERT INTO mm_ranked_bans (account_id, until, reason) VALUES ($1, {later:$2}, 'abandon')"
                                       " ON CONFLICT (account_id) DO UPDATE SET until = excluded.until, reason = excluded.reason",
                             .mysql = "INSERT INTO mm_ranked_bans (account_id, until, reason) VALUES ($1, {later:$2}, 'abandon')"
                                      " ON DUPLICATE KEY UPDATE until = VALUES(until), reason = VALUES(reason)"),
                     KN_DB_ARGS(I64S(ids[i]), I64S(cooldown_s)));
        cJSON *e = cJSON_CreateObject();
        cJSON_AddNumberToObject(e, "accountId", (double) ids[i]);
        cJSON_AddStringToObject(e, "name", jstr(pl, "n"));
        cJSON_AddNumberToObject(e, "team", team[i]);
        cJSON_AddNumberToObject(e, "season", season);
        cJSON_AddStringToObject(e, "region", region);
        cJSON_AddNumberToObject(e, "old", old_mmr);
        cJSON_AddNumberToObject(e, "new", new_mmr);
        cJSON_AddNumberToObject(e, "delta", new_mmr - old_mmr);
        cJSON_AddNumberToObject(e, "rank", ranked_rank_of(cr, matches[i] + 1));
        cJSON_AddNumberToObject(e, "matches", matches[i] + 1);
        cJSON_AddNumberToObject(e, "rp", rp);
        cJSON_AddNumberToObject(e, "mu", skills[i].mu);
        cJSON_AddNumberToObject(e, "sigma", skills[i].sigma);
        if (quit[i]) cJSON_AddBoolToObject(e, "abandoned", 1);
        cJSON_AddItemToArray(out, e);
    }
    dbx_exec(&x, SQL(.any = "DELETE FROM ranked_queue WHERE match_id = $1"), KN_DB_ARGS(P(0)));
    cJSON_Delete(players);
    cJSON_Delete(result);
    return dbx_json(&x, "finish", out);
}
const kn_proc P_RK_MATCH_FINISH = {"rk_match_finish", NULL, run_rk_match_finish};

/* ------------------------------------------------------------------ profile side */

/* (account, season, region index) -> {success, reward, balance, last}: the next milestone */
static kn_db_result *run_rk_claim(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t acc = PI(0);
    const int season = (int) PI(1);
    const char *region = ranked_region_code((int) PI(2));
    if (!region) {
        dbx_refuse(&x, "Unknown region");
        return dbx_finish(&x, NULL);
    }
    if (season == 0) {
        dbx_refuse(&x, "No milestones during Beta Season");
        return dbx_finish(&x, NULL);
    }
    cJSON *row = dbx_row(&x, SQL(.any = "SELECT wins + losses + draws AS m__i, last_milestone_claimed AS l__i FROM ranked_profiles"
                                        " WHERE account_id = $1 AND season = {int:$2} AND region = $3 {for_update}"),
                         KN_DB_ARGS(I64S(acc), I64S(season), region));
    const int matches = row ? (int) jnum(row, "m") : 0, last = row ? (int) jnum(row, "l") : -1;
    cJSON_Delete(row);
    const int next = last + 1;
    if (next >= RANKED_MILESTONE_COUNT || matches < g_ranked_milestones[next].matches) {
        if (!dbx_failed(&x)) dbx_refuse(&x, next >= RANKED_MILESTONE_COUNT ? "All milestones completed!" : "Milestone not reached");
        return dbx_finish(&x, NULL);
    }
    dbx_exec(&x, SQL(.any = "UPDATE ranked_profiles SET last_milestone_claimed = {int:$4} WHERE account_id = $1 AND season = {int:$2} AND region = $3"),
             KN_DB_ARGS(I64S(acc), I64S(season), region, I64S(next)));
    char ref[48];
    snprintf(ref, sizeof ref, "s%d:%s:%d", season, region, next);
    const int64_t balance = rp_add(&x, acc, g_ranked_milestones[next].reward, "milestone", ref);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 1);
    cJSON_AddNumberToObject(o, "reward", g_ranked_milestones[next].reward);
    cJSON_AddNumberToObject(o, "balance", (double) balance);
    cJSON_AddNumberToObject(o, "lastMilestoneClaimed", next);
    return dbx_json(&x, "claim", o);
}
const kn_proc P_RK_CLAIM = {"rk_claim", NULL, run_rk_claim};

/* (season) -> {season, accounts}: "Points awarded for your best region at end of month" */
static kn_db_result *run_rk_season_close(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int season = (int) PI(0);
    if (!rk_lock(&x, 1)) return dbx_json(&x, "close", NULL);
    cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT account_id AS a__i, region, cr AS cr__f, wins + losses + draws AS m__i FROM ranked_profiles"
                                          " WHERE season = {int:$1} AND NOT EXISTS (SELECT 1 FROM ranked_season_payouts s"
                                          " WHERE s.account_id = ranked_profiles.account_id AND s.season = {int:$1})"
                                          " ORDER BY account_id"),
                           KN_DB_ARGS(I64S(season)));
    int paid = 0;
    int64_t cur = -1, best_points = -1;
    char best_region[8] = "";
    const int count = cJSON_GetArraySize(rows);
    for (int i = 0; i <= count; i++) {
        const cJSON *r = i < count ? cJSON_GetArrayItem(rows, i) : NULL;
        const int64_t acc = r ? (int64_t) jnum(r, "a") : -2;
        if (acc != cur && cur >= 0 && best_points >= 0) {
            dbx_exec(&x, SQL(.any = "INSERT INTO ranked_season_payouts (account_id, season, region, points) VALUES ($1, {int:$2}, $3, {int:$4})"),
                     KN_DB_ARGS(I64S(cur), I64S(season), best_region, I64S(best_points)));
            char ref[16];
            snprintf(ref, sizeof ref, "s%d", season);
            if (best_points > 0) rp_add(&x, cur, best_points, "season", ref);
            paid++;
        }
        if (!r) break;
        if (acc != cur) {
            cur = acc;
            best_points = -1;
        }
        const int64_t pts = ranked_season_points(jnum(r, "cr"), (int) jnum(r, "m"), season);
        if (pts > best_points) {
            best_points = pts;
            snprintf(best_region, sizeof best_region, "%s", jstr(r, "region"));
        }
    }
    cJSON_Delete(rows);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "season", season);
    cJSON_AddNumberToObject(o, "accounts", paid);
    return dbx_json(&x, "close", o);
}
const kn_proc P_RK_SEASON_CLOSE = {"rk_season_close", NULL, run_rk_season_close};

/* (account, shop id, season) -> {success, balance, skin} */
static kn_db_result *run_rk_purchase(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t acc = PI(0), shop = PI(1);
    cJSON *item = dbx_row(&x, SQL(.any = "SELECT skin_id AS s__i, price AS p__i, max_purchase AS m__i, min_mmr AS lo__i, max_mmr AS hi__i,"
                                         " (expires_at IS NOT NULL AND expires_at < {now}) AS gone__b FROM ranked_shop WHERE id = {int:$1}"),
                          KN_DB_ARGS(I64S(shop)));
    if (!item || cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(item, "gone"))) {
        cJSON_Delete(item);
        if (!dbx_failed(&x)) dbx_refuse(&x, "Item not available");
        return dbx_finish(&x, NULL);
    }
    const int64_t bought = dbx_int(&x, SQL(.any = "SELECT count FROM ranked_shop_purchases WHERE account_id = $1 AND shop_id = {int:$2}"),
                                   KN_DB_ARGS(I64S(acc), I64S(shop)), 0);
    const int peak = (int) dbx_int(&x, SQL(.any = "SELECT max(peak_mmr) FROM ranked_profiles WHERE account_id = $1"), KN_DB_ARGS(I64S(acc)), 0);
    const cJSON *lo = cJSON_GetObjectItemCaseSensitive(item, "lo"), *hi = cJSON_GetObjectItemCaseSensitive(item, "hi");
    const char *refuse = bought >= (int64_t) jnum(item, "m") ? "Purchase limit reached"
                       : cJSON_IsNumber(lo) && peak < lo->valueint ? "Rating Required"
                       : cJSON_IsNumber(hi) && peak > hi->valueint ? "Rating Required" : NULL;
    if (refuse) {
        cJSON_Delete(item);
        dbx_refuse(&x, refuse);
        return dbx_finish(&x, NULL);
    }
    char ref[24];
    snprintf(ref, sizeof ref, "shop:%lld", (long long) shop);
    const int64_t balance = rp_add(&x, acc, -(int64_t) jnum(item, "p"), "purchase", ref);
    if (bought) dbx_exec(&x, SQL(.any = "UPDATE ranked_shop_purchases SET count = count + 1 WHERE account_id = $1 AND shop_id = {int:$2}"),
                         KN_DB_ARGS(I64S(acc), I64S(shop)));
    else dbx_exec(&x, SQL(.any = "INSERT INTO ranked_shop_purchases (account_id, shop_id, count) VALUES ($1, {int:$2}, 1)"),
                  KN_DB_ARGS(I64S(acc), I64S(shop)));
    const int skin = (int) jnum(item, "s");
    db_add_skin(&x, acc, skin, 1, "ranked", ref, NULL);
    cJSON_Delete(item);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 1);
    cJSON_AddNumberToObject(o, "balance", (double) balance);
    cJSON_AddNumberToObject(o, "skin", skin);
    return dbx_json(&x, "purchase", o);
}
const kn_proc P_RK_PURCHASE = {"rk_purchase", NULL, run_rk_purchase};
