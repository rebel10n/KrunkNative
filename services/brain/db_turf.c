/*
 * services/brain/db_turf.c - Krunker v10.0.2 Turf Wars on the tables of migration 0206_turf_wars
 * (doc/v10/ranked-impl.md). C on every driver; the rules are shared/src/turf_wars.c.
 *
 *   P_TURF_RECORD (day, map, records json [{player_id, clan_name, kills, deaths}]) -> {recorded}
 *                 the clan is resolved by name now (a player who left the clan meanwhile still
 *                 counts for the clan the server saw; unknown clans are dropped)
 *   P_TURF_SETTLE (day, kr per kill) -> {day, maps}: settles a finished day once (lock + turf_days),
 *                 pays the clans' cuts by their loot configs (players' KR, the clan bank)
 *   P_TURF_LOOT_SET (clan, account, json) -> the stored config (owner or role 2 only)
 */
#include "db_procs.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/json.h"
#include "turf_wars.h"

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

static kn_db_result *run_turf_record(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t day = PI(0), map = PI(1);
    cJSON *recs = cJSON_Parse(nz(P(2)));
    int recorded = 0;
    const cJSON *r;
    if (!turf_map_name((int) map)) {
        cJSON_Delete(recs);
        dbx_refuse(&x, "not a turf map");
        return dbx_finish(&x, NULL);
    }
    cJSON_ArrayForEach(r, recs) {
        const int64_t acc = (int64_t) jnum(r, "player_id");
        const int kills = (int) jnum(r, "kills"), deaths = (int) jnum(r, "deaths");
        if (acc <= 0 || kills < 0 || deaths < 0 || kills > 1000 || deaths > 1000 || (!kills && !deaths)) continue;
        const int64_t clan = dbx_int(&x, SQL(.any = "SELECT clan_id FROM clans WHERE lower(clan_name) = lower($1)"), KN_DB_ARGS(jstr(r, "clan_name")), 0);
        if (clan <= 0) continue;
        if (dbx_exec(&x, SQL(.any = "UPDATE turf_kills SET kills = kills + {int:$5}, deaths = deaths + {int:$6}, games = games + 1, updated_at = {now}"
                                    " WHERE day = {int:$1} AND map_id = {int:$2} AND account_id = $3 AND clan_id = {int:$4}"),
                     KN_DB_ARGS(I64S(day), I64S(map), I64S(acc), I64S(clan), I64S(kills), I64S(deaths))) == 0)
            dbx_exec(&x, SQL(.any = "INSERT INTO turf_kills (day, map_id, account_id, clan_id, kills, deaths, games)"
                                    " VALUES ({int:$1}, {int:$2}, $3, {int:$4}, {int:$5}, {int:$6}, 1)"),
                     KN_DB_ARGS(I64S(day), I64S(map), I64S(acc), I64S(clan), I64S(kills), I64S(deaths)));
        recorded++;
    }
    cJSON_Delete(recs);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "recorded", recorded);
    return dbx_json(&x, "record", o);
}
const kn_proc P_TURF_RECORD = {"turf_record", NULL, run_turf_record};

static TurfLoot loot_of(Dbx *x, const int64_t clan) {
    TurfLoot l = turf_loot_default();
    cJSON *row = dbx_row(x, SQL(.any = "SELECT distribution_mode, clan_pct AS a__i, top_player_count AS b__i, top_player_pct AS c__i,"
                                       " remainder_pct AS d__i, min_games_required AS e__i FROM turf_loot_config WHERE clan_id = {int:$1}"),
                         KN_DB_ARGS(I64S(clan)));
    if (row) {
        const int mode = turf_loot_mode(jstr(row, "distribution_mode"));
        l.mode = mode >= 0 ? mode : TURF_LOOT_CLAN_BANK;
        l.clan_pct = (int) jnum(row, "a");
        l.top_player_count = (int) jnum(row, "b");
        l.top_player_pct = (int) jnum(row, "c");
        l.remainder_pct = (int) jnum(row, "d");
        l.min_games = (int) jnum(row, "e");
        turf_loot_clamp(&l);
        cJSON_Delete(row);
    }
    return l;
}

/* one clan's cut on one map and day: members by kills (top 20, >= min games) */
static void pay_clan(Dbx *x, const int64_t day, const int64_t map, const int64_t clan, const int64_t cut) {
    const TurfLoot loot = loot_of(x, clan);
    cJSON *members = dbx_rows(x, SQL(.any = "SELECT account_id AS a__i, kills AS k__i FROM turf_kills"
                                            " WHERE day = {int:$1} AND map_id = {int:$2} AND clan_id = {int:$3} AND games >= {int:$4}"
                                            " ORDER BY kills DESC, account_id LIMIT 20"),
                              KN_DB_ARGS(I64S(day), I64S(map), I64S(clan), I64S(loot.min_games)));
    const int count = cJSON_GetArraySize(members);
    int kills[TURF_TOP_CONTRIBUTORS];
    int64_t share[TURF_TOP_CONTRIBUTORS];
    for (int i = 0; i < count && i < TURF_TOP_CONTRIBUTORS; i++) kills[i] = (int) jnum(cJSON_GetArrayItem(members, i), "k");
    const int64_t bank = turf_loot_split(&loot, cut, kills, count < TURF_TOP_CONTRIBUTORS ? count : TURF_TOP_CONTRIBUTORS, share);
    char ref[48];
    snprintf(ref, sizeof ref, "turf:%lld:%lld", (long long) day, (long long) map);
    for (int i = 0; i < count && i < TURF_TOP_CONTRIBUTORS; i++) {
        if (share[i] <= 0) continue;
        const int64_t acc = (int64_t) jnum(cJSON_GetArrayItem(members, i), "a");
        if (db_add_funds(x, acc, share[i], "turf", ref, NULL) < 0) continue;
        dbx_exec(x, SQL(.any = "INSERT INTO turf_payouts (day, map_id, clan_id, account_id, kr) VALUES ({int:$1}, {int:$2}, {int:$3}, $4, $5)"),
                 KN_DB_ARGS(I64S(day), I64S(map), I64S(clan), I64S(acc), I64S(share[i])));
    }
    if (bank > 0) {
        dbx_exec(x, SQL(.any = "UPDATE clans SET clan_funds = clan_funds + {int:$2} WHERE clan_id = {int:$1}"), KN_DB_ARGS(I64S(clan), I64S(bank)));
        dbx_exec(x, SQL(.any = "INSERT INTO turf_payouts (day, map_id, clan_id, account_id, kr) VALUES ({int:$1}, {int:$2}, {int:$3}, NULL, $4)"),
                 KN_DB_ARGS(I64S(day), I64S(map), I64S(clan), I64S(bank)));
    }
    cJSON_Delete(members);
}

/* the settlement lock: postgres advisory xact lock, sqlite BEGIN IMMEDIATE, mysql the mm_ranked_lock row */
static int turf_lock(Dbx *x) {
    if (dbx_is_pg(x))
        return dbx_int(x, SQL(.pg = "SELECT CASE WHEN pg_try_advisory_xact_lock(kn_rq_lock() + 16) THEN 1 ELSE 0 END"), 0, NULL, 0) == 1;
    if (dbx_is_mysql(x)) {
        kn_db_result *r = kn_dbconn_exec_sql(x->c, SQL(.any = "SELECT id FROM mm_ranked_lock WHERE id = 1 FOR UPDATE NOWAIT"), 0, NULL);
        const int got = kn_db_ok(r) && kn_db_rows(r) > 0;
        kn_db_result_free(r);
        return got;
    }
    return 1;
}

static kn_db_result *run_turf_settle(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t day = PI(0);
    const int64_t per_kill = PI(1) > 0 ? PI(1) : TURF_KR_PER_KILL;
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "day", (double) day);
    int settled = 0;
    if (!turf_lock(&x)) {
        cJSON_AddNumberToObject(o, "maps", 0);
        return dbx_json(&x, "settle", o);
    }
    for (int mi = 0; mi < TURF_MAP_COUNT && !dbx_failed(&x); mi++) {
        const int64_t map = g_turf_maps[mi];
        if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM turf_days WHERE day = {int:$1} AND map_id = {int:$2}"), KN_DB_ARGS(I64S(day), I64S(map))))
            continue;
        /* per clan: the sum of its top 20 contributors' kills */
        cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT clan_id AS c__i, kills AS k__i FROM turf_kills"
                                              " WHERE day = {int:$1} AND map_id = {int:$2} ORDER BY clan_id, kills DESC, account_id"),
                               KN_DB_ARGS(I64S(day), I64S(map)));
        int64_t clans[256], totals[256];
        int nclans = 0, rank_in_clan = 0;
        int64_t cur = -1, all = 0;
        const cJSON *r;
        cJSON_ArrayForEach(r, rows) {
            const int64_t clan = (int64_t) jnum(r, "c");
            if (clan != cur) {
                if (nclans == 256) break;
                cur = clan;
                rank_in_clan = 0;
                clans[nclans] = clan;
                totals[nclans++] = 0;
            }
            if (rank_in_clan++ < TURF_TOP_CONTRIBUTORS) {
                totals[nclans - 1] += (int64_t) jnum(r, "k");
                all += (int64_t) jnum(r, "k");
            }
        }
        cJSON_Delete(rows);
        /* order clans by kills (ties: the lower clan id) */
        for (int i = 1; i < nclans; i++)
            for (int j = i; j > 0 && (totals[j] > totals[j - 1] || (totals[j] == totals[j - 1] && clans[j] < clans[j - 1])); j--) {
                const int64_t tc = clans[j], tt = totals[j];
                clans[j] = clans[j - 1];
                totals[j] = totals[j - 1];
                clans[j - 1] = tc;
                totals[j - 1] = tt;
            }
        const int64_t pool = all * per_kill;
        if (nclans && totals[0] > 0)
            dbx_exec(&x, SQL(.any = "INSERT INTO turf_days (day, map_id, clan_id, kills, total_kills, pool) VALUES ({int:$1}, {int:$2}, {int:$3}, {int:$4}, {int:$5}, $6)"),
                     KN_DB_ARGS(I64S(day), I64S(map), I64S(clans[0]), I64S(totals[0]), I64S(all), I64S(pool)));
        else
            dbx_exec(&x, SQL(.any = "INSERT INTO turf_days (day, map_id, clan_id, kills, total_kills, pool) VALUES ({int:$1}, {int:$2}, NULL, 0, 0, 0)"),
                     KN_DB_ARGS(I64S(day), I64S(map)));
        for (int i = 0; i < nclans; i++) {
            const int64_t cut = turf_cut(pool, i);
            dbx_exec(&x, SQL(.any = "INSERT INTO turf_clan_days (day, map_id, clan_id, position, kills, kr) VALUES ({int:$1}, {int:$2}, {int:$3}, {int:$4}, {int:$5}, $6)"),
                     KN_DB_ARGS(I64S(day), I64S(map), I64S(clans[i]), I64S(i + 1), I64S(totals[i]), I64S(cut)));
            if (cut > 0) pay_clan(&x, day, map, clans[i], cut);
        }
        settled++;
    }
    cJSON_AddNumberToObject(o, "maps", settled);
    return dbx_json(&x, "settle", o);
}
const kn_proc P_TURF_SETTLE = {"turf_settle", NULL, run_turf_settle};

/* (clan, account, json) */
static kn_db_result *run_turf_loot_set(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t clan = PI(0), acc = PI(1);
    const int owner = dbx_exists(&x, SQL(.any = "SELECT 1 FROM clans WHERE clan_id = {int:$1} AND clan_playerid = {int:$2}"), KN_DB_ARGS(I64S(clan), I64S(acc)));
    const int commander = dbx_exists(&x, SQL(.any = "SELECT 1 FROM clan_members WHERE clan_id = {int:$1} AND account_id = {int:$2} AND rank >= 2"),
                                     KN_DB_ARGS(I64S(clan), I64S(acc)));
    if (!owner && !commander) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Only the clan owner or a Commander can change the loot config");
        return dbx_finish(&x, NULL);
    }
    cJSON *in = cJSON_Parse(nz(P(2)));
    TurfLoot l = turf_loot_default();
    const int mode = turf_loot_mode(jstr(in, "distribution_mode"));
    if (mode >= 0) l.mode = mode;
    if (cJSON_HasObjectItem(in, "clan_pct")) l.clan_pct = (int) jnum(in, "clan_pct");
    if (cJSON_HasObjectItem(in, "top_player_count")) l.top_player_count = (int) jnum(in, "top_player_count");
    if (cJSON_HasObjectItem(in, "top_player_pct")) l.top_player_pct = (int) jnum(in, "top_player_pct");
    if (cJSON_HasObjectItem(in, "remainder_pct")) l.remainder_pct = (int) jnum(in, "remainder_pct");
    if (cJSON_HasObjectItem(in, "min_games_required")) l.min_games = (int) jnum(in, "min_games_required");
    cJSON_Delete(in);
    turf_loot_clamp(&l);
    char *name = dbx_text(&x, SQL(.any = "SELECT username FROM accounts WHERE id = {int:$1}"), KN_DB_ARGS(I64S(acc)));
    dbx_exec(&x, SQL(.any = "DELETE FROM turf_loot_config WHERE clan_id = {int:$1}"), KN_DB_ARGS(I64S(clan)));
    dbx_exec(&x, SQL(.any = "INSERT INTO turf_loot_config (clan_id, distribution_mode, clan_pct, top_player_count, top_player_pct, remainder_pct,"
                            " min_games_required, updated_by) VALUES ({int:$1}, $2, {int:$3}, {int:$4}, {int:$5}, {int:$6}, {int:$7}, $8)"),
             8, (const char *const[]){I64S(clan), turf_loot_mode_name(l.mode), I64S(l.clan_pct), I64S(l.top_player_count), I64S(l.top_player_pct),
                                      I64S(l.remainder_pct), I64S(l.min_games), nz(name)});
    free(name);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", 1);
    cJSON_AddStringToObject(o, "distribution_mode", turf_loot_mode_name(l.mode));
    cJSON_AddNumberToObject(o, "clan_pct", l.clan_pct);
    cJSON_AddNumberToObject(o, "top_player_count", l.top_player_count);
    cJSON_AddNumberToObject(o, "top_player_pct", l.top_player_pct);
    cJSON_AddNumberToObject(o, "remainder_pct", l.remainder_pct);
    cJSON_AddNumberToObject(o, "min_games_required", l.min_games);
    return dbx_json(&x, "loot", o);
}
const kn_proc P_TURF_LOOT_SET = {"turf_loot_set", NULL, run_turf_loot_set};
