/*
 * services/brain/raids.c - Krunker v10.0.3's raid record (migration 0360_raid_stats).
 *
 * v10.0.3's Raid mode is noProfileStats (deob-10.0.3.js:118448): updateAccounts adds no games, kills, deaths or wins for
 * it (217283-217296) and transferStats keeps its tmpStats off account.stats (217374). Its runs are a record of their own,
 * which the client reads in three places:
 *   - acc.raidStats (setData [52], 129021): the Account window's Raids tab (247596-247634);
 *   - the profile's raid_stats (125818-125839, 126487-126537): the player profile popup's Raids section;
 *   - /leaderboards/raids/<map> (?sort=completions) and /leaderboards/raids/<map>/monthly?month=YYYY-MM (&sort=best_time),
 *     getRaidLeaderboard / getMonthlyRaidLeaderboard (31026-31047): rows {position, player_name, player_clan, best_time,
 *     completions}; all time by best time (fastest first) unless sort=completions, a month by completions unless
 *     sort=best_time (155619-155715).
 * Each row is {map_id, completions, attempts, best_time (ms), ai_kills, boss_kills, deaths} per raid map (game.mapIndex:
 * Tortuga 18, Laboratory 29, Soul Sanctum 31, Khepri 35, Bastion 43).
 *
 * The bundle has no writer of the record (Krunker's backend keeps it), so KrunkNative's game servers send one run per
 * player in their stats commit (server/src/accounts.c: "raid" {map, cleared, time, aiKills, bossKills, deaths}, a
 * public-type Raid round on an official map): a run is an attempt, a won round (raid.winCondition) also a completion
 * whose time (player.timer, raid.onEnd, 118390-118400) can be the best; aiKills is player.aiKills (AI.kill, 206311),
 * bossKills player.bossKills (every active player when a boss dies, 206317-206323), deaths the run's. The commit's
 * duplicate check (match_results) makes it once per match.
 */
#include "raids.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "acct_internal.h"
#include "db_procs.h"
#include "knsvc/json.h"
#include "knsvc/log.h"

#define MOD "raids"
#define RAID_MAP_MAX 1000       /* game.mapIndex range kept */
#define RAID_TIME_MAX 21600000  /* 6 h: a run time past it is no time */
#define RAID_COUNT_MAX 100000   /* per run */

#define P(i) (n > (i) ? p[i] : NULL)

/* (account, map, cleared, time ms (0 none), ai kills, boss kills, deaths, month YYYY-MM): both rows of the run */
static kn_db_result *run_raid_record(kn_dbconn *c, int n, const char *const *p) {
    Dbx x = {c, NULL};
    dbx_exec(&x, SQL(.any = "{insert_ignore} INTO raid_stats (account_id, map_id) VALUES ($1, {int:$2}) {ignore_conflicts}"),
             KN_DB_ARGS(P(0), P(1)));
    dbx_exec(&x,
             SQL(.any = "UPDATE raid_stats SET completions = completions + {int:$3}, attempts = attempts + 1,"
                        " best_time = CASE WHEN {int:$4} > 0 AND (best_time IS NULL OR best_time > {int:$4}) THEN {int:$4} ELSE best_time END,"
                        " ai_kills = ai_kills + {int:$5}, boss_kills = boss_kills + {int:$6}, deaths = deaths + {int:$7}, updated_at = {now}"
                        " WHERE account_id = $1 AND map_id = {int:$2}"),
             KN_DB_ARGS(P(0), P(1), P(2), P(3), P(4), P(5), P(6)));
    dbx_exec(&x, SQL(.any = "{insert_ignore} INTO raid_stats_monthly (account_id, map_id, month) VALUES ($1, {int:$2}, $3) {ignore_conflicts}"),
             KN_DB_ARGS(P(0), P(1), P(7)));
    dbx_exec(&x,
             SQL(.any = "UPDATE raid_stats_monthly SET completions = completions + {int:$3}, attempts = attempts + 1,"
                        " best_time = CASE WHEN {int:$4} > 0 AND (best_time IS NULL OR best_time > {int:$4}) THEN {int:$4} ELSE best_time END,"
                        " ai_kills = ai_kills + {int:$5}, boss_kills = boss_kills + {int:$6}, deaths = deaths + {int:$7}, updated_at = {now}"
                        " WHERE account_id = $1 AND map_id = {int:$2} AND month = $8"),
             KN_DB_ARGS(P(0), P(1), P(2), P(3), P(4), P(5), P(6), P(7)));
    return dbx_failed(&x) ? dbx_finish(&x, NULL) : dbx_json(&x, "raid_record", cJSON_CreateObject());
}
static const kn_proc P_RAID_RECORD = {"raid_record", NULL, run_raid_record};

cJSON *db_raid_stats(Dbx *x, const int64_t account_id) {
    return dbx_rows(x, SQL(.any = "SELECT map_id AS map_id__i, completions AS completions__i, attempts AS attempts__i, best_time AS best_time__i,"
                                  " ai_kills AS ai_kills__i, boss_kills AS boss_kills__i, deaths AS deaths__i"
                                  " FROM raid_stats WHERE account_id = $1 ORDER BY map_id"),
                    KN_DB_ARGS(I64S(account_id)));
}

static void raid_recorded(kn_db_result *res, void *ud) {
    const int64_t id = (int64_t) (intptr_t) ud;
    if (!kn_db_ok(res)) KN_WARN(MOD, "raid run of account %lld: %s", (long long) id, acct_db_error(res, "raid_record"));
}

static int64_t clamp_count(const cJSON *raid, const char *key) {
    const int64_t v = kn_json_int(raid, key, 0);
    return v < 0 ? 0 : v > RAID_COUNT_MAX ? RAID_COUNT_MAX : v;
}

void raids_record(const int64_t account_id, const cJSON *raid) {
    if (account_id <= 0 || !cJSON_IsObject(raid) || !g_acct_brain || !g_acct_brain->db) return;
    const int64_t map = kn_json_int(raid, "map", -1);
    if (map < 0 || map >= RAID_MAP_MAX) return;
    const int cleared = kn_json_bool(raid, "cleared", 0);
    int64_t ms = kn_json_int(raid, "time", 0);
    if (!cleared || ms <= 0 || ms > RAID_TIME_MAX) ms = 0; /* only a clear has a time */
    char month[16];
    const time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    snprintf(month, sizeof month, "%04d-%02d", (int) (tm.tm_year + 1900) % 10000, (int) (tm.tm_mon + 1));
    char id[24], mp[24], cl[4], tm_ms[24], ai[24], boss[24], deaths[24];
    snprintf(cl, sizeof cl, "%d", cleared ? 1 : 0);
    kn_db_call(g_acct_brain->db, &P_RAID_RECORD,
               KN_DB_ARGS(acct_itoa(id, account_id), acct_itoa(mp, map), cl, acct_itoa(tm_ms, ms), acct_itoa(ai, clamp_count(raid, "aiKills")),
                          acct_itoa(boss, clamp_count(raid, "bossKills")), acct_itoa(deaths, clamp_count(raid, "deaths")), month),
               raid_recorded, (void *) (intptr_t) account_id);
}

/* the leaderboards' pages: player_rows names, hack-flagged accounts left out, as the other boards */
#define BOARD_COLS "SELECT r.player_name, r.player_clan, s.best_time AS best_time__i, s.completions AS completions__i FROM "
#define BOARD_JOIN " s JOIN player_rows r ON r.player_id = s.account_id WHERE s.map_id = {int:$1} AND r.player_hack = 0"

int raids_board_query(const char *map, const int monthly, const char *month, const char *sort, const int limit, RaidBoardQuery *out) {
    memset(out, 0, sizeof *out);
    char *end = NULL;
    const long m = map ? strtol(map, &end, 10) : -1;
    if (!map || !*map || (end && *end) || m < 0 || m >= RAID_MAP_MAX) return -1;
    snprintf(out->map, sizeof out->map, "%ld", m);
    snprintf(out->limit, sizeof out->limit, "%d", limit > 0 ? limit : 100);
    /* v10's defaults: all time by best time, a month by completions; the other order only when asked */
    const int by_time = monthly ? (sort && !strcmp(sort, "best_time")) : !(sort && !strcmp(sort, "completions"));
    if (monthly) {
        if (!month || strlen(month) != 7 || month[4] != '-') return -1;
        for (int i = 0; i < 7; i++)
            if (i != 4 && (month[i] < '0' || month[i] > '9')) return -1;
        snprintf(out->month, sizeof out->month, "%s", month);
        out->sql = by_time ? BOARD_COLS "raid_stats_monthly" BOARD_JOIN " AND s.month = $2 AND s.best_time IS NOT NULL"
                                        " ORDER BY s.best_time, s.updated_at, s.account_id LIMIT {int:$3}"
                           : BOARD_COLS "raid_stats_monthly" BOARD_JOIN " AND s.month = $2 AND s.completions > 0"
                                        " ORDER BY s.completions DESC, s.updated_at, s.account_id LIMIT {int:$3}";
    } else {
        out->sql = by_time ? BOARD_COLS "raid_stats" BOARD_JOIN " AND s.best_time IS NOT NULL ORDER BY s.best_time, s.updated_at, s.account_id"
                                        " LIMIT {int:$2}"
                           : BOARD_COLS "raid_stats" BOARD_JOIN " AND s.completions > 0 ORDER BY s.completions DESC, s.best_time, s.account_id"
                                        " LIMIT {int:$2}";
    }
    return 0;
}
