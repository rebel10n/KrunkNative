/*
 * services/brain/db_turf.c - Krunker v10.0.2 Turf Wars on the tables of migration 0206_turf_wars
 * (doc/v10/ranked-impl.md). C on every driver; the rules are shared/src/turf_wars.c.
 *
 *   P_TURF_RECORD (day, map, records json [{player_id, clan_name, kills, deaths}], match id) -> {recorded}
 *                 a match id counted before records nothing (turf_rounds, 0212)
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
#include <strings.h>

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
    /* a round counts once: a game server resends its reliable frame when the reply was lost */
    if (P(3) && P(3)[0]) {
        const int64_t fresh = dbx_exec(&x, SQL(.any = "{insert_ignore} INTO turf_rounds (match_id, day, map_id) VALUES ($1, {int:$2}, {int:$3})"
                                                      " {ignore_conflicts}"),
                                       KN_DB_ARGS(P(3), I64S(day), I64S(map)));
        if (dbx_failed(&x)) {
            cJSON_Delete(recs);
            return dbx_finish(&x, NULL);
        }
        if (fresh == 0) {
            cJSON_Delete(recs);
            cJSON *dup = cJSON_CreateObject();
            cJSON_AddNumberToObject(dup, "recorded", 0);
            cJSON_AddBoolToObject(dup, "duplicate", 1);
            return dbx_json(&x, "record", dup);
        }
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
    /* v10's PUT answer: who saved it and when */
    char *by = dbx_text(&x, SQL(.any = "SELECT updated_by FROM turf_loot_config WHERE clan_id = {int:$1}"), KN_DB_ARGS(I64S(clan)));
    cJSON_AddStringToObject(o, "updated_by_name", by ? by : "");
    free(by);
    cJSON_AddNumberToObject(o, "updated_ms", (double) dbx_int(&x, SQL(.any = "SELECT {epoch_ms:updated_at} FROM turf_loot_config WHERE clan_id = {int:$1}"),
                                                             KN_DB_ARGS(I64S(clan)), 0));
    return dbx_json(&x, "loot", o);
}
const kn_proc P_TURF_LOOT_SET = {"turf_loot_set", NULL, run_turf_loot_set};

/* ================================================================ v10's views (P_TURF_VIEW)
 *
 * The payloads of v10's gapi territorialWars / clans clients (v10:26670-26860), built in C on every
 * driver: (view, current_day, a, b, c, d) -> one JSON column, the route's `data`. */

/* a clan by id / tag (case-insensitive) */
#define CLAN_OF(n) "(SELECT clan_id FROM clans WHERE {text:clan_id} = $" #n " OR lower(clan_name) = lower($" #n ") LIMIT 1)"
/* a player by id / name */
#define PLAYER_OF(n) "(SELECT id FROM accounts WHERE {text:id} = $" #n " OR lower(username) = lower($" #n ") LIMIT 1)"

static const char *map_label(int64_t map) {
    const char *name = turf_map_name((int) map);
    return name ? name : "";
}

static double kd_of(double kills, double deaths) { return deaths > 0 ? kills / deaths : kills; }

/* a clan's counted line of (day, map): the top TURF_TOP_CONTRIBUTORS contributors by kills only,
 * as the settlement counts them (run_turf_settle) */
typedef struct {
    int64_t day, map, clan, kills, deaths, games;
    const char *name; /* points into the rows it was read from */
} ClanDay;

static int by_clan_day(const void *pa, const void *pb) {
    const ClanDay *a = pa, *b = pb;
    if (a->day != b->day) return a->day < b->day ? -1 : 1;
    if (a->map != b->map) return a->map < b->map ? -1 : 1;
    if (a->kills != b->kills) return a->kills > b->kills ? -1 : 1;
    return a->clan < b->clan ? -1 : a->clan > b->clan;
}

/* the counted rows of [from, to] ordered day, map, kills desc, clan: {day, map_id, clan_id,
 * clan_name, kills, deaths, games} (NULL on a database error) */
static cJSON *clan_day_rows(Dbx *x, int64_t from, int64_t to) {
    cJSON *raw = dbx_rows(x, SQL(.any = "SELECT tk.day AS day__i, tk.map_id AS map_id__i, tk.clan_id AS clan_id__i, c.clan_name,"
                                        " tk.kills AS kills__i, tk.deaths AS deaths__i, tk.games AS games__i FROM turf_kills tk"
                                        " JOIN clans c ON c.clan_id = tk.clan_id WHERE tk.day >= {int:$1} AND tk.day <= {int:$2}"
                                        " ORDER BY tk.day, tk.map_id, tk.clan_id, tk.kills DESC, tk.account_id"),
                          KN_DB_ARGS(I64S(from), I64S(to)));
    if (!raw) return NULL;
    ClanDay *g = calloc((size_t) cJSON_GetArraySize(raw) + 1, sizeof *g);
    cJSON *out = cJSON_CreateArray();
    if (!g) {
        cJSON_Delete(raw);
        cJSON_Delete(out);
        return NULL;
    }
    int n = 0, counted = 0;
    cJSON *r;
    cJSON_ArrayForEach(r, raw) {
        const int64_t day = (int64_t) jnum(r, "day"), map = (int64_t) jnum(r, "map_id"), clan = (int64_t) jnum(r, "clan_id");
        if (n == 0 || g[n - 1].day != day || g[n - 1].map != map || g[n - 1].clan != clan) {
            g[n++] = (ClanDay) {day, map, clan, 0, 0, 0, jstr(r, "clan_name")};
            counted = 0;
        }
        if (counted++ >= TURF_TOP_CONTRIBUTORS) continue;
        g[n - 1].kills += (int64_t) jnum(r, "kills");
        g[n - 1].deaths += (int64_t) jnum(r, "deaths");
        g[n - 1].games += (int64_t) jnum(r, "games");
    }
    qsort(g, (size_t) n, sizeof *g, by_clan_day);
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "day", (double) g[i].day);
        cJSON_AddNumberToObject(o, "map_id", (double) g[i].map);
        cJSON_AddNumberToObject(o, "clan_id", (double) g[i].clan);
        cJSON_AddStringToObject(o, "clan_name", g[i].name);
        cJSON_AddNumberToObject(o, "kills", (double) g[i].kills);
        cJSON_AddNumberToObject(o, "deaths", (double) g[i].deaths);
        cJSON_AddNumberToObject(o, "games", (double) g[i].games);
        cJSON_AddItemToArray(out, o);
    }
    free(g);
    cJSON_Delete(raw);
    return out;
}

/* the clan's row of (day, map) in rows, its position, and the comparison clan (the leader, or the
 * runner-up when the clan leads) */
static const cJSON *rank_on(const cJSON *rows, int64_t day, int64_t map, int64_t clan, int *pos, const cJSON **cmp, int *cmp_pos) {
    const cJSON *r, *mine = NULL, *first = NULL, *second = NULL;
    int i = 0;
    *pos = 0;
    cJSON_ArrayForEach(r, rows) {
        if ((int64_t) jnum(r, "day") != day || (int64_t) jnum(r, "map_id") != map) continue;
        i++;
        if (i == 1) first = r;
        if (i == 2) second = r;
        if ((int64_t) jnum(r, "clan_id") == clan) {
            mine = r;
            *pos = i;
        }
    }
    *cmp = NULL;
    *cmp_pos = 0;
    if (mine && *pos == 1 && second) *cmp = second, *cmp_pos = 2;
    else if (mine && *pos > 1 && first) *cmp = first, *cmp_pos = 1;
    return mine;
}

static void add_comparison(cJSON *o, const cJSON *cmp, int cmp_pos, int full) {
    if (cmp) {
        cJSON_AddNumberToObject(o, "comparison_kills", jnum(cmp, "kills"));
        cJSON_AddStringToObject(o, "comparison_clan_name", jstr(cmp, "clan_name"));
        cJSON_AddNumberToObject(o, "comparison_clan_id", jnum(cmp, "clan_id"));
        cJSON_AddNullToObject(o, "comparison_clan_logo");
        if (full) {
            cJSON_AddNumberToObject(o, "comparison_deaths", jnum(cmp, "deaths"));
            cJSON_AddNumberToObject(o, "comparison_position", cmp_pos);
        }
    } else {
        cJSON_AddNullToObject(o, "comparison_kills");
        cJSON_AddNullToObject(o, "comparison_clan_name");
        cJSON_AddNullToObject(o, "comparison_clan_id");
    }
}

static cJSON *view_status(Dbx *x, int64_t cur, int64_t per_kill, int64_t elapsed_s) {
    cJSON *control = dbx_rows(x, SQL(.any = "SELECT d.map_id AS map_id__i, d.clan_id AS clan_id__i, c.clan_name, d.kills AS kills__i FROM turf_days d"
                                            " LEFT JOIN clans c ON c.clan_id = d.clan_id WHERE d.day = {int:$1}"),
                              KN_DB_ARGS(I64S(cur - 1)));
    cJSON *today = clan_day_rows(x, cur, cur);
    cJSON *out = cJSON_CreateArray();
    for (int i = 0; i < TURF_MAP_COUNT; i++) {
        const int64_t map = g_turf_maps[i];
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "map_id", (double) map);
        cJSON_AddStringToObject(m, "map_name", map_label(map));
        const cJSON *r, *hit = NULL;
        cJSON_ArrayForEach(r, control) if ((int64_t) jnum(r, "map_id") == map) hit = r;
        if (hit && cJSON_IsNumber(cJSON_GetObjectItemCaseSensitive(hit, "clan_id"))) {
            cJSON_AddNumberToObject(m, "clan_id", jnum(hit, "clan_id"));
            cJSON_AddStringToObject(m, "clan_name", jstr(hit, "clan_name"));
            cJSON_AddNumberToObject(m, "kills", jnum(hit, "kills"));
        } else {
            cJSON_AddNullToObject(m, "clan_id");
            cJSON_AddNullToObject(m, "clan_name");
            cJSON_AddNumberToObject(m, "kills", 0);
        }
        cJSON_AddNullToObject(m, "clan_logo"); /* KrunkNative clans have no uploaded logos */
        cJSON *lb = cJSON_AddArrayToObject(m, "leaderboard");
        int64_t total = 0;
        int pos = 0;
        cJSON_ArrayForEach(r, today) {
            if ((int64_t) jnum(r, "map_id") != map) continue;
            total += (int64_t) jnum(r, "kills");
            if (pos >= 10) continue;
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "position", ++pos);
            cJSON_AddNumberToObject(e, "clan_id", jnum(r, "clan_id"));
            cJSON_AddStringToObject(e, "clan_name", jstr(r, "clan_name"));
            cJSON_AddNullToObject(e, "clan_logo");
            cJSON_AddNumberToObject(e, "kills", jnum(r, "kills"));
            cJSON_AddItemToArray(lb, e);
        }
        /* guaranteed: the pool of today's kills so far; estimated: that pace to the end of the
         * UTC day (KrunkNative's projection, at least one hour in) */
        const int64_t pool = total * per_kill;
        const double frac = (double) (elapsed_s < 3600 ? 3600 : elapsed_s > 86400 ? 86400 : elapsed_s) / 86400.0;
        cJSON_AddNumberToObject(m, "total_kills_today", (double) total);
        cJSON_AddNumberToObject(m, "guaranteed_reward", (double) pool);
        cJSON_AddNumberToObject(m, "estimated_reward", floor((double) pool / frac));
        cJSON_AddItemToArray(out, m);
    }
    cJSON_Delete(control);
    cJSON_Delete(today);
    return out;
}

static cJSON *view_clan(Dbx *x, int64_t cur, const char *id) {
    cJSON *clan = dbx_row(x, SQL(.any = "SELECT clan_id AS clan_id__i, clan_name FROM clans WHERE clan_id = " CLAN_OF(1)), KN_DB_ARGS(id));
    if (!clan) return NULL;
    const int64_t cid = (int64_t) jnum(clan, "clan_id");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "clan_id", (double) cid);
    cJSON_AddStringToObject(o, "clan_name", jstr(clan, "clan_name"));
    cJSON_AddNullToObject(o, "clan_logo");
    cJSON_AddNumberToObject(o, "current_day", (double) cur);
    cJSON_Delete(clan);
    cJSON *rows = clan_day_rows(x, cur - 3, cur);
    /* today's performance per map */
    cJSON *maps = cJSON_AddArrayToObject(o, "maps");
    for (int i = 0; i < TURF_MAP_COUNT; i++) {
        int pos, cpos;
        const cJSON *cmp;
        const cJSON *mine = rank_on(rows, cur, g_turf_maps[i], cid, &pos, &cmp, &cpos);
        if (!mine) continue;
        cJSON *m = cJSON_CreateObject();
        cJSON_AddNumberToObject(m, "day", (double) cur);
        cJSON_AddNumberToObject(m, "map_id", g_turf_maps[i]);
        cJSON_AddStringToObject(m, "map_name", map_label(g_turf_maps[i]));
        cJSON_AddNumberToObject(m, "position", pos);
        cJSON_AddNumberToObject(m, "kills", jnum(mine, "kills"));
        cJSON_AddNumberToObject(m, "deaths", jnum(mine, "deaths"));
        cJSON_AddNumberToObject(m, "games", jnum(mine, "games"));
        add_comparison(m, cmp, cpos, 0);
        cJSON_AddItemToArray(maps, m);
    }
    /* the previous 3 days: the clan's top map each day */
    cJSON *hist = cJSON_AddArrayToObject(o, "historical_days");
    for (int64_t day = cur - 1; day >= cur - 3 && day >= 0; day--) {
        const cJSON *best = NULL, *r;
        cJSON_ArrayForEach(r, rows) {
            if ((int64_t) jnum(r, "day") != day || (int64_t) jnum(r, "clan_id") != cid) continue;
            if (!best || jnum(r, "kills") > jnum(best, "kills")) best = r;
        }
        if (!best) continue;
        int pos, cpos;
        const cJSON *cmp;
        rank_on(rows, day, (int64_t) jnum(best, "map_id"), cid, &pos, &cmp, &cpos);
        cJSON *h = cJSON_CreateObject();
        cJSON_AddNumberToObject(h, "day", (double) day);
        cJSON_AddStringToObject(h, "map_name", map_label((int64_t) jnum(best, "map_id")));
        cJSON_AddNumberToObject(h, "kills", jnum(best, "kills"));
        cJSON_AddNumberToObject(h, "deaths", jnum(best, "deaths"));
        cJSON_AddNumberToObject(h, "position", pos);
        add_comparison(h, cmp, cpos, 1);
        cJSON_AddItemToArray(hist, h);
    }
    cJSON_Delete(rows);
    return o;
}

static cJSON *with_kd_positions(cJSON *rows, int positions) {
    int i = 0;
    cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        if (positions) cJSON_AddNumberToObject(r, "position", ++i);
        if (cJSON_HasObjectItem(r, "kills")) cJSON_AddNumberToObject(r, "kd", kd_of(jnum(r, "kills"), jnum(r, "deaths")));
        if (cJSON_HasObjectItem(r, "map_id") && !cJSON_HasObjectItem(r, "map_name")) cJSON_AddStringToObject(r, "map_name", map_label((int64_t) jnum(r, "map_id")));
        if (cJSON_HasObjectItem(r, "clan_name") && !cJSON_HasObjectItem(r, "clan_logo") && cJSON_HasObjectItem(r, "clan_id")) cJSON_AddNullToObject(r, "clan_logo");
    }
    return rows;
}

static int by_clan_total(const void *pa, const void *pb) {
    const ClanDay *a = pa, *b = pb;
    if (a->kills != b->kills) return a->kills > b->kills ? -1 : 1;
    return a->clan < b->clan ? -1 : a->clan > b->clan;
}

/* the day's clans over the maps (map < 0: all) by counted kills, ties by clan id; name: a clan
 * name filter ("" = all). At most 100 rows. */
static cJSON *top_clans_view(Dbx *x, int64_t day, int64_t map, const char *name) {
    cJSON *rows = clan_day_rows(x, day, day);
    if (!rows) return NULL;
    ClanDay *t = calloc((size_t) cJSON_GetArraySize(rows) + 1, sizeof *t);
    if (!t) {
        cJSON_Delete(rows);
        return NULL;
    }
    int n = 0;
    cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        if (map >= 0 && (int64_t) jnum(r, "map_id") != map) continue;
        if (name && name[0] && strcasecmp(jstr(r, "clan_name"), name)) continue;
        const int64_t clan = (int64_t) jnum(r, "clan_id");
        int i = 0;
        while (i < n && t[i].clan != clan) i++;
        if (i == n) t[n++] = (ClanDay) {day, map, clan, 0, 0, 0, jstr(r, "clan_name")};
        t[i].kills += (int64_t) jnum(r, "kills");
        t[i].deaths += (int64_t) jnum(r, "deaths");
        t[i].games += (int64_t) jnum(r, "games");
    }
    qsort(t, (size_t) n, sizeof *t, by_clan_total);
    cJSON *out = cJSON_CreateArray();
    for (int i = 0; i < n && i < 100; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "clan_id", (double) t[i].clan);
        cJSON_AddStringToObject(o, "clan_name", t[i].name);
        cJSON_AddNumberToObject(o, "kills", (double) t[i].kills);
        cJSON_AddNumberToObject(o, "deaths", (double) t[i].deaths);
        cJSON_AddNumberToObject(o, "games", (double) t[i].games);
        cJSON_AddItemToArray(out, o);
    }
    free(t);
    cJSON_Delete(rows);
    return out;
}

static cJSON *view_payouts(Dbx *x, const char *id, int64_t day) {
    cJSON *o = cJSON_CreateObject();
    cJSON *rows;
    if (day < 0) { /* all-time totals per player */
        rows = dbx_rows(x, SQL(.any = "SELECT p.account_id AS player_id__i, coalesce(a.username, '') AS player_name, sum(p.kr) AS total_kr__i FROM turf_payouts p"
                                      " LEFT JOIN accounts a ON a.id = p.account_id WHERE p.clan_id = " CLAN_OF(1) " AND p.account_id IS NOT NULL"
                                      " GROUP BY p.account_id, a.username ORDER BY sum(p.kr) DESC, p.account_id"),
                        KN_DB_ARGS(id));
    } else {
        rows = dbx_rows(x, SQL(.any = "SELECT p.account_id AS player_id__i, coalesce(a.username, '') AS player_name, p.map_id AS map_id__i, p.kr AS total_kr__i"
                                      " FROM turf_payouts p LEFT JOIN accounts a ON a.id = p.account_id WHERE p.clan_id = " CLAN_OF(1)
                                      " AND p.day = {int:$2} AND p.account_id IS NOT NULL ORDER BY p.kr DESC, p.id"),
                        KN_DB_ARGS(id, I64S(day)));
        cJSON_AddNumberToObject(o, "day", (double) day);
    }
    if (!rows) rows = cJSON_CreateArray();
    double total = 0;
    cJSON *r;
    cJSON_ArrayForEach(r, rows) {
        total += jnum(r, "total_kr");
        if (cJSON_HasObjectItem(r, "map_id")) cJSON_AddStringToObject(r, "map_name", map_label((int64_t) jnum(r, "map_id")));
    }
    cJSON_AddNumberToObject(o, "total", total);
    cJSON_AddItemToObject(o, "payouts", rows);
    return o;
}

static cJSON *view_player(Dbx *x, const char *id) {
    cJSON *p = dbx_row(x, SQL(.any = "SELECT a.id AS player_id__i, a.username AS player_name, c.clan_id AS current_clan_id__i, c.clan_name AS current_clan,"
                                     " (SELECT coalesce(sum(kills), 0) FROM turf_kills WHERE account_id = a.id) AS total_kills__i,"
                                     " (SELECT coalesce(sum(deaths), 0) FROM turf_kills WHERE account_id = a.id) AS total_deaths__i,"
                                     " (SELECT coalesce(sum(games), 0) FROM turf_kills WHERE account_id = a.id) AS total_games__i,"
                                     " (SELECT coalesce(sum(kr), 0) FROM turf_payouts WHERE account_id = a.id) AS total_kr__i"
                                     " FROM accounts a LEFT JOIN clan_members m ON m.account_id = a.id LEFT JOIN clans c ON c.clan_id = m.clan_id"
                                     " WHERE a.id = " PLAYER_OF(1)),
                       KN_DB_ARGS(id));
    if (!p) return NULL;
    cJSON_AddNullToObject(p, "current_clan_logo");
    cJSON_AddNumberToObject(p, "kd", kd_of(jnum(p, "total_kills"), jnum(p, "total_deaths")));
    return p;
}

static cJSON *view_recruitment(Dbx *x, int64_t page, int64_t limit, const char *region, const char *lang) {
    if (page < 1) page = 1;
    if (limit < 1 || limit > 50) limit = 20;
    const int64_t total = dbx_int(x, SQL(.any = "SELECT count(*) FROM clan_recruitment r JOIN clans c ON c.clan_id = r.clan_id"
                                               " WHERE r.is_active = {true} AND ($1 = '' OR r.region = $1) AND ($2 = '' OR r.language = $2)"),
                                  KN_DB_ARGS(region, lang), 0);
    cJSON *posts = dbx_rows(x, SQL(.any = "SELECT r.clan_id AS recruitment_id__i, r.clan_id AS clan_id__i, c.clan_name, r.region, r.language, r.description,"
                                          " r.looking_for, r.min_level AS min_level__i, r.min_kd AS min_kd__f, coalesce(l.distribution_mode, 'clan_bank') AS loot_mode,"
                                          " coalesce(l.clan_pct, 0) AS loot_clan_pct__i, coalesce(l.top_player_count, 20) AS loot_top_player_count__i,"
                                          " coalesce(l.top_player_pct, 80) AS loot_top_player_pct__i, coalesce(l.min_games_required, 1) AS loot_min_games__i"
                                          " , c.clan_logo AS clan_logo__i" /* the logo version, 0 none: drawn when > 0 (deob-10.0.3.js:160977-160979) */
                                          " FROM clan_recruitment r JOIN clans c ON c.clan_id = r.clan_id LEFT JOIN turf_loot_config l ON l.clan_id = r.clan_id"
                                          " WHERE r.is_active = {true} AND ($1 = '' OR r.region = $1) AND ($2 = '' OR r.language = $2)"
                                          " ORDER BY r.updated_at DESC, r.clan_id LIMIT {int:$3} OFFSET {int:$4}"),
                            KN_DB_ARGS(region, lang, I64S(limit), I64S((page - 1) * limit)));
    if (!posts) posts = cJSON_CreateArray();
    cJSON *p;
    cJSON_ArrayForEach(p, posts) cJSON_AddBoolToObject(p, "is_active", 1);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddItemToObject(o, "posts", posts);
    cJSON_AddNumberToObject(o, "total", (double) total);
    cJSON_AddNumberToObject(o, "page", (double) page);
    cJSON_AddNumberToObject(o, "limit", (double) limit);
    return o;
}

/* a clan's post (active or removed), NULL without one */
static cJSON *recruitment_row(Dbx *x, const char *clan) {
    cJSON *r = dbx_row(x, SQL(.any = "SELECT r.clan_id AS recruitment_id__i, r.clan_id AS clan_id__i, c.clan_name, r.region, r.language, r.description, r.looking_for,"
                                     " r.min_level AS min_level__i, r.min_kd AS min_kd__f, r.is_active AS is_active__b, {epoch_ms:r.updated_at} AS updated_ms__i,"
                                     " c.clan_logo AS clan_logo__i"
                                     " FROM clan_recruitment r JOIN clans c ON c.clan_id = r.clan_id WHERE r.clan_id = " CLAN_OF(1)),
                       KN_DB_ARGS(clan));
    return r;
}

/* (view, current_day, a, b, c, d) */
static kn_db_result *run_turf_view(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const char *view = nz(P(0)), *a = nz(P(2)), *b = nz(P(3)), *cc = nz(P(4)), *d = nz(P(5));
    const int64_t cur = PI(1);
    cJSON *out = NULL;
    if (!strcmp(view, "status")) {
        out = view_status(&x, cur, pint(a) > 0 ? pint(a) : TURF_KR_PER_KILL, pint(b));
    } else if (!strcmp(view, "clan")) {
        out = view_clan(&x, cur, a);
    } else if (!strcmp(view, "players")) { /* (id, day, map) */
        out = dbx_rows(&x, SQL(.any = "SELECT tk.account_id AS player_id__i, a.username AS player_name, sum(tk.kills) AS kills__i, sum(tk.deaths) AS deaths__i,"
                                      " sum(tk.games) AS games__i FROM turf_kills tk JOIN accounts a ON a.id = tk.account_id"
                                      " WHERE tk.clan_id = " CLAN_OF(1) " AND tk.day = {int:$2} AND ({int:$3} < 0 OR tk.map_id = {int:$3})"
                                      " GROUP BY tk.account_id, a.username ORDER BY sum(tk.kills) DESC, tk.account_id LIMIT 100"),
                       KN_DB_ARGS(a, I64S(pint(b) >= 0 && b[0] ? pint(b) : cur), I64S(cc[0] ? pint(cc) : -1)));
        if (out) with_kd_positions(out, 0);
    } else if (!strcmp(view, "earnings")) { /* (id) */
        out = dbx_rows(&x, SQL(.any = "SELECT day AS day__i, map_id AS map_id__i, kr AS reward__i FROM turf_clan_days WHERE clan_id = " CLAN_OF(1)
                                      " AND kr > 0 ORDER BY day DESC, map_id LIMIT 1000"),
                       KN_DB_ARGS(a));
        if (out) with_kd_positions(out, 0);
    } else if (!strcmp(view, "payouts")) { /* (id, day | "") */
        out = view_payouts(&x, a, b[0] ? pint(b) : -1);
    } else if (!strcmp(view, "top_clans")) { /* (day, map, name) */
        out = top_clans_view(&x, a[0] ? pint(a) : cur, b[0] ? pint(b) : -1, cc);
        if (out) with_kd_positions(out, 1);
    } else if (!strcmp(view, "top_grossing")) { /* (name) */
        out = dbx_rows(&x, SQL(.any = "SELECT cd.clan_id AS clan_id__i, c.clan_name, sum(cd.kr) AS total_kr__i,"
                                      " (SELECT coalesce(sum(p.kr), 0) FROM turf_payouts p WHERE p.clan_id = cd.clan_id AND p.account_id IS NOT NULL) AS distributed__i"
                                      " FROM turf_clan_days cd JOIN clans c ON c.clan_id = cd.clan_id WHERE ($1 = '' OR lower(c.clan_name) = lower($1))"
                                      " GROUP BY cd.clan_id, c.clan_name HAVING sum(cd.kr) > 0 ORDER BY sum(cd.kr) DESC, cd.clan_id LIMIT 100"),
                       KN_DB_ARGS(a));
        cJSON *r;
        cJSON_ArrayForEach(r, out) cJSON_AddNumberToObject(r, "bank", jnum(r, "total_kr") - jnum(r, "distributed"));
        if (out) with_kd_positions(out, 1);
    } else if (!strcmp(view, "top_players")) { /* (map, name) */
        out = dbx_rows(&x, SQL(.any = "SELECT tk.account_id AS player_id__i, a.username AS player_name, coalesce(c.clan_name, '') AS clan_name, sum(tk.kills) AS kills__i,"
                                      " sum(tk.deaths) AS deaths__i, sum(tk.games) AS games__i,"
                                      " (SELECT coalesce(sum(p.kr), 0) FROM turf_payouts p WHERE p.account_id = tk.account_id) AS earnings__i"
                                      " FROM turf_kills tk JOIN accounts a ON a.id = tk.account_id LEFT JOIN clan_members m ON m.account_id = tk.account_id"
                                      " LEFT JOIN clans c ON c.clan_id = m.clan_id"
                                      " WHERE ({int:$1} < 0 OR tk.map_id = {int:$1}) AND ($2 = '' OR lower(a.username) = lower($2))"
                                      " GROUP BY tk.account_id, a.username, c.clan_name ORDER BY sum(tk.kills) DESC, tk.account_id LIMIT 100"),
                       KN_DB_ARGS(I64S(a[0] ? pint(a) : -1), b));
        if (out) with_kd_positions(out, 1);
    } else if (!strcmp(view, "player")) {
        out = view_player(&x, a);
    } else if (!strcmp(view, "contributions")) { /* (id, day | "") */
        out = dbx_rows(&x, SQL(.any = "SELECT tk.day AS day__i, tk.map_id AS map_id__i, tk.clan_id AS clan_id__i, c.clan_name, tk.kills AS kills__i,"
                                      " tk.deaths AS deaths__i, tk.games AS games__i FROM turf_kills tk JOIN clans c ON c.clan_id = tk.clan_id"
                                      " WHERE tk.account_id = " PLAYER_OF(1) " AND ({int:$3} = 1 OR tk.day = {int:$2})"
                                      " ORDER BY tk.day DESC, tk.map_id LIMIT 500"),
                       KN_DB_ARGS(a, I64S(b[0] ? pint(b) : 0), I64S(b[0] ? 0 : 1)));
        if (out) with_kd_positions(out, 0);
    } else if (!strcmp(view, "player_earnings")) {
        out = dbx_rows(&x, SQL(.any = "SELECT p.day AS day__i, p.map_id AS map_id__i, p.clan_id AS clan_id__i, c.clan_name, p.kr AS amount__i FROM turf_payouts p"
                                      " JOIN clans c ON c.clan_id = p.clan_id WHERE p.account_id = " PLAYER_OF(1) " ORDER BY p.day DESC, p.id DESC LIMIT 500"),
                       KN_DB_ARGS(a));
        if (out) with_kd_positions(out, 0);
    } else if (!strcmp(view, "recruitment")) { /* (page, limit, region, language) */
        out = view_recruitment(&x, pint(a), pint(b), cc, d);
    } else if (!strcmp(view, "recruitment_of")) { /* (clan) */
        out = recruitment_row(&x, a);
        if (!out && !dbx_failed(&x)) out = cJSON_CreateNull();
    } else if (!strcmp(view, "my_applications")) { /* (account) */
        cJSON *rows = dbx_rows(&x, SQL(.any = "SELECT clan_id AS clan_id__i FROM clan_requests WHERE account_id = {int:$1}"), KN_DB_ARGS(a));
        out = cJSON_CreateObject();
        cJSON *ids = cJSON_AddArrayToObject(out, "clan_ids");
        cJSON *r;
        cJSON_ArrayForEach(r, rows) cJSON_AddItemToArray(ids, cJSON_CreateNumber(jnum(r, "clan_id")));
        cJSON_Delete(rows);
    } else {
        dbx_refuse(&x, "unknown view");
        return dbx_finish(&x, NULL);
    }
    if (dbx_failed(&x)) {
        cJSON_Delete(out);
        return dbx_finish(&x, NULL);
    }
    if (!out) { /* a clan / player that does not exist */
        dbx_refuse(&x, "Not found");
        return dbx_finish(&x, NULL);
    }
    return dbx_json(&x, "data", out);
}
const kn_proc P_TURF_VIEW = {"turf_view", NULL, run_turf_view};

/* ================================================================ recruitment posts (0208_clan_recruitment) */

static const char *const k_rec_regions[] = {"NA", "EU", "AS", "OCE", "SA", "ME"};
static const char *const k_rec_langs[] = {"en", "es", "pt", "fr", "de", "ru", "zh", "ja", "ko", "ar", "tr", "pl", "it", "nl", "sv",
                                          "th", "vi", "id", "hi", "fil", "cs", "ro", "hu", "uk", "fi", "no", "da", "el", "he", "ms"};

static int in_list(const char *v, const char *const *list, size_t n) {
    for (size_t i = 0; i < n; i++)
        if (!strcmp(v, list[i])) return 1;
    return 0;
}

static int clan_leader(Dbx *x, int64_t clan, int64_t acc) {
    return dbx_exists(x, SQL(.any = "SELECT 1 FROM clans WHERE clan_id = {int:$1} AND clan_playerid = {int:$2}"), KN_DB_ARGS(I64S(clan), I64S(acc))) ||
           dbx_exists(x, SQL(.any = "SELECT 1 FROM clan_members WHERE clan_id = {int:$1} AND account_id = {int:$2} AND rank >= 2"), KN_DB_ARGS(I64S(clan), I64S(acc)));
}

#define RECRUIT_COST 1000 /* v10: "Creating a recruitment post costs 1,000 KR from clan bank." */

/* (clan, account, json): create / update the clan's post (owner or rank >= 2; once per 30 s).
 * Creating it (no post, or a removed one) costs RECRUIT_COST from the clan bank, in this
 * transaction, with a clan_funds_ledger row. */
static kn_db_result *run_recruit_set(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t acc = PI(1);
    const int64_t clan = dbx_int(&x, SQL(.any = "SELECT clan_id FROM clans WHERE clan_id = " CLAN_OF(1)), KN_DB_ARGS(nz(P(0))), 0);
    if (clan <= 0) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Clan not found");
        return dbx_finish(&x, NULL);
    }
    if (!clan_leader(&x, clan, acc)) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Only the clan owner or a Captain or higher can post recruitment");
        return dbx_finish(&x, NULL);
    }
    if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM clan_recruitment WHERE clan_id = {int:$1} AND is_active = {true} AND updated_at > {ago:30}"),
                   KN_DB_ARGS(I64S(clan)))) {
        dbx_refuse(&x, "You can update your recruitment post again in a moment");
        return dbx_finish(&x, NULL);
    }
    cJSON *in = cJSON_Parse(nz(P(2)));
    const char *region = jstr(in, "region"), *lang = jstr(in, "language");
    const double min_level = jnum(in, "min_level"), min_kd = jnum(in, "min_kd");
    const char *err = NULL;
    if (region[0] && !in_list(region, k_rec_regions, sizeof k_rec_regions / sizeof *k_rec_regions)) err = "Unknown region";
    else if (lang[0] && !in_list(lang, k_rec_langs, sizeof k_rec_langs / sizeof *k_rec_langs)) err = "Unknown language";
    else if (strlen(jstr(in, "description")) > 2000) err = "Description is too long (2000 characters max)";
    else if (strlen(jstr(in, "looking_for")) > 200) err = "\"Looking for\" is too long (200 characters max)";
    else if (min_level < 0 || min_level > 200) err = "Minimum level must be between 0 and 200";
    else if (min_kd < 0 || min_kd > 20) err = "Minimum K/D must be between 0 and 20";
    char reg[8], lg[8], kd[32];
    char *desc = strdup(jstr(in, "description")), *looking = strdup(jstr(in, "looking_for"));
    snprintf(reg, sizeof reg, "%s", region);
    snprintf(lg, sizeof lg, "%s", lang);
    snprintf(kd, sizeof kd, "%.2f", min_kd);
    cJSON_Delete(in);
    if (!err && (!desc || !looking)) err = "Out of memory";
    if (err) {
        free(desc);
        free(looking);
        dbx_refuse(&x, err);
        return dbx_finish(&x, NULL);
    }
    const int active = dbx_exists(&x, SQL(.any = "SELECT 1 FROM clan_recruitment WHERE clan_id = {int:$1} AND is_active = {true}"), KN_DB_ARGS(I64S(clan)));
    if (!active) { /* a new post: the clan bank pays */
        if (dbx_exec(&x, SQL(.any = "UPDATE clans SET clan_funds = clan_funds - {int:$2} WHERE clan_id = {int:$1} AND clan_funds >= {int:$2}"),
                     KN_DB_ARGS(I64S(clan), I64S(RECRUIT_COST))) != 1) {
            free(desc);
            free(looking);
            if (!dbx_failed(&x)) dbx_refuse(&x, "Not enough clan funds: a recruitment post costs 1,000 KR from the clan bank");
            return dbx_finish(&x, NULL);
        }
        dbx_exec(&x, SQL(.any = "INSERT INTO clan_funds_ledger (clan_id, account_id, amount, source, ref) VALUES ({int:$1}, {int:$2}, {int:$3}, 'recruitment', NULL)"),
                 KN_DB_ARGS(I64S(clan), I64S(acc), I64S(-RECRUIT_COST)));
    }
    dbx_exec(&x, SQL(.any = "DELETE FROM clan_recruitment WHERE clan_id = {int:$1}"), KN_DB_ARGS(I64S(clan)));
    dbx_exec(&x, SQL(.any = "INSERT INTO clan_recruitment (clan_id, region, language, description, looking_for, min_level, min_kd, created_by, is_active)"
                            " VALUES ({int:$1}, $2, $3, $4, $5, {int:$6}, {float:$7}, {int:$8}, {true})"),
             8, (const char *const[]){I64S(clan), reg, lg, desc, looking, I64S((int64_t) min_level), kd, I64S(acc)});
    free(desc);
    free(looking);
    char id[24];
    snprintf(id, sizeof id, "%lld", (long long) clan);
    cJSON *post = recruitment_row(&x, id);
    if (post) {
        cJSON_AddBoolToObject(post, "charged", !active);
        cJSON_AddNumberToObject(post, "clan_funds", (double) dbx_int(&x, SQL(.any = "SELECT clan_funds FROM clans WHERE clan_id = {int:$1}"), KN_DB_ARGS(I64S(clan)), 0));
    }
    return dbx_json(&x, "data", post ? post : cJSON_CreateObject());
}
const kn_proc P_TURF_RECRUIT_SET = {"turf_recruit_set", NULL, run_recruit_set};

/* (clan, account, staff): remove the post (owner / Commander, or staff taking it down) */
static kn_db_result *run_recruit_del(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t acc = PI(1), staff = PI(2);
    const int64_t clan = dbx_int(&x, SQL(.any = "SELECT clan_id FROM clans WHERE clan_id = " CLAN_OF(1)), KN_DB_ARGS(nz(P(0))), 0);
    if (clan <= 0) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Clan not found");
        return dbx_finish(&x, NULL);
    }
    if (!staff && !clan_leader(&x, clan, acc)) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Only the clan owner or a Commander can remove the recruitment post");
        return dbx_finish(&x, NULL);
    }
    /* the row stays inactive: re-posting costs again (v10's "You can re-post later (costs 1,000 KR)") */
    const int64_t removed = dbx_exec(&x, SQL(.any = "UPDATE clan_recruitment SET is_active = {false}, updated_at = {now} WHERE clan_id = {int:$1} AND is_active = {true}"),
                                     KN_DB_ARGS(I64S(clan)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "success", removed > 0);
    return dbx_json(&x, "data", o);
}
const kn_proc P_TURF_RECRUIT_DEL = {"turf_recruit_del", NULL, run_recruit_del};

/* recruit_kd_refused (db_procs.h): the active post's min_kd against the account's K/D as its profile shows it
 * (kd_of: lifetime kills over deaths, a player with no deaths counts its kills). */
int recruit_kd_refused(Dbx *x, int64_t clan, int64_t acc) {
    cJSON *r = dbx_row(x, SQL(.any = "SELECT r.min_kd AS min_kd__f, p.player_kills AS kills__i, p.player_deaths AS deaths__i"
                                     " FROM clan_recruitment r LEFT JOIN players p ON p.player_id = $2"
                                     " WHERE r.clan_id = $1 AND r.is_active = {true}"),
                       KN_DB_ARGS(I64S(clan), I64S(acc)));
    if (!r) return 0;
    const double min_kd = jnum(r, "min_kd"), kd = kd_of(jnum(r, "kills"), jnum(r, "deaths"));
    cJSON_Delete(r);
    return min_kd > 0 && kd < min_kd;
}

/* (clan, account, level): a join request (clan_requests, the clan's Requests list). The post's minimum level
 * and minimum K/D (v10.0.3: every join request) refuse it as v10's disabled button does. */
static kn_db_result *run_apply(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    const int64_t acc = PI(1), level = PI(2);
    const int64_t clan = dbx_int(&x, SQL(.any = "SELECT clan_id FROM clans WHERE clan_id = " CLAN_OF(1)), KN_DB_ARGS(nz(P(0))), 0);
    if (clan <= 0) {
        if (!dbx_failed(&x)) dbx_refuse(&x, "Clan not found");
        return dbx_finish(&x, NULL);
    }
    if (dbx_exists(&x, SQL(.any = "SELECT 1 FROM clan_members WHERE account_id = {int:$1}"), KN_DB_ARGS(I64S(acc)))) {
        dbx_refuse(&x, "You are already in a clan");
        return dbx_finish(&x, NULL);
    }
    const int64_t min_level = dbx_int(&x, SQL(.any = "SELECT min_level FROM clan_recruitment WHERE clan_id = {int:$1} AND is_active = {true}"), KN_DB_ARGS(I64S(clan)), 0);
    if (min_level > 0 && level < min_level) {
        dbx_refuse(&x, "Requirements Not Met");
        return dbx_finish(&x, NULL);
    }
    if (recruit_kd_refused(&x, clan, acc)) {
        dbx_refuse(&x, "Requirements Not Met");
        return dbx_finish(&x, NULL);
    }
    if (dbx_int(&x, SQL(.any = "SELECT count(*) FROM clan_requests WHERE account_id = {int:$1} AND created_at > {ago:60}"), KN_DB_ARGS(I64S(acc)), 0) >= 10) {
        dbx_refuse(&x, "Too many applications. Try again later.");
        return dbx_finish(&x, NULL);
    }
    dbx_exec(&x, SQL(.any = "{insert_ignore} INTO clan_requests (clan_id, account_id) VALUES ({int:$1}, {int:$2}) {ignore_conflicts}"),
             KN_DB_ARGS(I64S(clan), I64S(acc)));
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "message", "Application sent!");
    return dbx_json(&x, "data", o);
}
const kn_proc P_TURF_APPLY = {"turf_apply", NULL, run_apply};
