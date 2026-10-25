/*
 * services/brain/raids.h - Krunker v10.0.3's raid record (services/brain/raids.c, migration 0360_raid_stats): per
 * account and raid map, all time and per UTC month, apart from the profile stats (Raid is noProfileStats).
 */
#ifndef KN_BRAIN_RAIDS_H
#define KN_BRAIN_RAIDS_H

#include <stdint.h>

#include "cJSON.h"
#include "knsvc/http.h"

struct Dbx;
struct Brain;

/* One run of a raid from a game server's stats commit (op "stats" with "raid": {map, cleared, time, aiKills,
 * bossKills, deaths}), after the commit was not a duplicate (once per match). Fire and forget. */
void raids_record(int64_t account_id, const cJSON *raid);

/* v10's raid_stats rows of an account [{map_id, completions, attempts, best_time (ms | null), ai_kills, boss_kills,
 * deaths}] by map (acc.raidStats, the profile's raid_stats); a database worker thread. NULL after a failure. */
cJSON *db_raid_stats(struct Dbx *x, int64_t account_id);

/* GET /leaderboards/raids/<map>[?sort=completions] and /leaderboards/raids/<map>/monthly?month=YYYY-MM[&sort=best_time]
 * (v10.0.3 getRaidLeaderboard / getMonthlyRaidLeaderboard, deob-10.0.3.js:31026-31047): the SQL of the page and its
 * parameters for leaderboards.c, which runs, caches and answers it. 0 = ok, -1 = a bad request (empty list). */
typedef struct RaidBoardQuery {
    const char *sql;   /* driver-neutral (.any) text */
    char map[16], month[8], limit[16];
} RaidBoardQuery;
int raids_board_query(const char *map, int monthly, const char *month, const char *sort, int limit, RaidBoardQuery *out);

#endif
