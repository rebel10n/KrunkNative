#pragma once
// Krunker v10.0.2 ranked: the rules shared by the brain (services/brain/ranked.c, the authority),
// the client (the Ranked window, client/src/ui/ranked_ui.c) and the tests. doc/v10/ranked-impl.md.
//
// v10's client shows a TrueSkill-like rating: profile.conservative_rating, displayed as
// 1000 + round(100 * conservative_rating), 14 ranks (Unranked, Bronze 1-3 ... Kracked) with
// thresholds 0 / 200 / ... / 4700, 6 placement matches ("N games remaining until ranked"),
// monthly seasons (season 1 = 2025-08 UTC), a monthly 5-map pool (getRankedMaps) and level 10.
// The rating server is not in the bundle, so the update rule here is KrunkNative's: two-team
// TrueSkill (Herbrich et al. 2007) with mu0 25, sigma0 25/3, beta 25/6, tau 25/300 and
// conservative_rating = mu - 3 sigma. The rank thresholds (v10's unused client table, 0 ... 4700)
// apply to the displayed MMR, the number v10 also compares with the shop's "Requires MMR": a new
// player (cr 0, MMR 1000) places at Silver 2.
//
// The v7.1.4 (v3.1.1 updateELO) Elo helpers stay for the seasons KrunkNative already stored
// (ranked_ratings): ranked_seed_mu turns such a rating into a v10 starting mu.

#include <stdint.h>

// ---- v7.1.4 / v3.1.1 Elo (ranked_ratings, kept for the history and the v10 seed)
#define RANKED_ELO_K 32.0            // config.eloK
#define RANKED_ELO_DIVISOR 330.0     // v3.1.1 game.js updateELO: 10^((opponents - player) / 330)
#define RANKED_ELO_PER 120           // config.eloPer: rating per rank
#define RANKED_MAX_LEVEL 8           // v3.1.1 config.maxELOLevel
#define RANKED_DEDUCTION 30          // config.rankedDeduction
double ranked_expected(double rating, double opponents);
double ranked_delta(double rating, double opponents, double score, int abandoned, double k);
int ranked_rank_index(double rating);
int ranked_rank_number(double rating);

// ---- v10.0.2
#define RANKED_MIN_LEVEL 10          // "You must be level 10 or higher to access Ranked Mode."
#define RANKED_COOLDOWN_MS 1800000   // KrunkNative: abandon queue ban (v10's ERROR COOLDOWN, length server-side)
#define RANKED_TEAM_SIZE 4           // comp host type overDat (minPlayers 8): 4v4 Hardpoint
#define RANKED_PLACEMENT_MATCHES 6   // "6 - total_matches games remaining until ranked", minMatchesForPoints 6
#define RANKED_MAX_MATCHES 100       // "100 matches = 100% conversion rate" (maxMatches 100)
#define RANKED_DECAY_DAYS 7          // "Your rating will decay after 7+ days of inactivity"
#define RANKED_TIME_FIRST_CONNECT_MS 60000 // config.rankedTimeForFirstConnect
#define RANKED_SEASON_START_YEAR 2025
#define RANKED_SEASON_START_MONTH 8
#define RANKED_ALWAYS_MAP 2          // Sandstorm
#define RANKED_ROTATION_SLOTS 4
#define RANKED_POOL_COUNT 7          // rankedMapPoolInfo
#define RANKED_SEASON_MAPS (RANKED_ROTATION_SLOTS + 1)

// TrueSkill parameters (KrunkNative)
#define RANKED_MU0 25.0
#define RANKED_SIGMA0 (25.0 / 3.0)
#define RANKED_BETA (25.0 / 6.0)
#define RANKED_TAU (25.0 / 300.0)
#define RANKED_DRAW_Z 0.12566       // draw margin = z * sqrt(players) * beta, z = Phi^-1((0.1 + 1) / 2): 10% draws
#define RANKED_ABANDON_MU 1.0        // an abandoner also loses 1 mu (100 display points)

// config.rankedClasses: the classes a comp lobby allows (classLim<i> defC 1)
#define RANKED_CLASS_COUNT 11
extern const int g_ranked_classes[RANKED_CLASS_COUNT];
int ranked_class_allowed(int class_index);

// Ranks (the v10 enum: Unranked 0, Bronze1 1 ... Kracked 13)
enum {
    RANK_UNRANKED = 0,
    RANK_BRONZE1, RANK_BRONZE2, RANK_BRONZE3,
    RANK_SILVER1, RANK_SILVER2, RANK_SILVER3,
    RANK_GOLD1, RANK_GOLD2, RANK_GOLD3,
    RANK_PLATINUM, RANK_DIAMOND, RANK_MASTER, RANK_KRACKED,
    RANK_COUNT
};
const char *ranked_rank_name(int rank);    // "Bronze 1", "Unknown" out of range
const char *ranked_rank_color(int rank);   // "#CD7F32"
const char *ranked_rank_icon(int rank);    // "img/ranked/ranks/rank_bronze.svg"
int ranked_rank_min(int rank);             // threshold on 100 * conservative_rating (-1 for Unranked)

typedef struct {
    double mu, sigma;
} RankedSkill;

static inline RankedSkill ranked_skill_default(void) {
    RankedSkill s = {RANKED_MU0, RANKED_SIGMA0};
    return s;
}
double ranked_conservative(RankedSkill s);           // mu - 3 sigma
int ranked_display(double conservative);             // 1000 + Math.round(100 * cr) (floor(x + 0.5))
// The rank of a profile: Unranked below the placement matches, else by the displayed MMR.
int ranked_rank_of(double conservative, int matches);

// One two-team match. skills[i] belongs to team[i] (0/1); outcome: winner team 0/1, -1 draw.
// Abandoners (abandoned[i] != 0) are rated as a loss of their team whatever the result, keep
// their sigma and lose RANKED_ABANDON_MU more (always below their rating before). Updates
// skills in place; n <= 32.
void ranked_update(RankedSkill *skills, const int *team, const int *abandoned, int n, int winner);
// Inactivity: sigma grows by tau per idle day past RANKED_DECAY_DAYS (capped at sigma0).
RankedSkill ranked_decay(RankedSkill s, double idle_days);
// A season's starting skill from the previous season's (soft reset: half the distance to mu0,
// sigma at least 2/3 sigma0), or from a v7 ranked_ratings Elo rating when elo >= 0.
RankedSkill ranked_season_seed(const RankedSkill *previous, int v7_elo);

// Seasons: 0 before 2025-08 (the Beta season), else 12 * (year - 2025) + (month - 8 + 1), UTC.
int ranked_season_at(int64_t unix_s);
// getRankedMaps(season): Sandstorm + 4 of the pool (seeded LCG, partial Fisher-Yates), ascending.
void ranked_season_maps(int season, int out[RANKED_SEASON_MAPS]);
// rankedMapPoolInfo keys. KrunkNative plays 2, 4, 11, 14, 17 only (KN choice, Q-MAP). Eterno Simulator (39)
// and Frontier (42) are official Krunker maps with catalogue entries (map_catalog.h: their mapIndex is their
// index), so they load; the season still lists them and the brain reports them unavailable (ranked.c
// add_season_maps), and the matcher never hosts them.
extern const int g_ranked_map_pool[RANKED_POOL_COUNT];
const char *ranked_pool_map_name(int map_index);       // rankedMapPoolInfo name, NULL when not in it

// End-of-season Ranked Points (one formula: the Ranked popup's estimate and the brain's payout,
// v10:112066-112067): 0 below 6 matches (the Beta season too), else floor(display MMR * rate) with
// rate min(matches / 100, 1), and 1 in the Beta season (season 0).
int64_t ranked_season_points(double conservative, int matches, int season);
// Per-match Ranked Points: clamp(floor(reward / 2), 0, 60) (v10 game.js endGame, ranked).
int ranked_match_points(int reward);

// Milestones (KrunkNative's table; v10 reads them from the profile response).
#define RANKED_MILESTONE_COUNT 5
typedef struct {
    int matches, reward;
} RankedMilestone;
extern const RankedMilestone g_ranked_milestones[RANKED_MILESTONE_COUNT];

// v10 ranked regions (the matchmaker's): as / eu / na, regionIndex 2 / 3 / 4.
#define RANKED_REGION_COUNT 3
#define RANKED_DEFAULT_REGION_INDEX 3 // eu: v10 _34037's default (no defaultRegion, or one the table does not hold)
const char *ranked_region_code(int index);   // 2 -> "as", NULL otherwise
int ranked_region_index(const char *code);   // "eu" -> 3, -1 unknown (accepts "3" too)
const char *ranked_region_name(const char *code); // "Europe"
// v10 _34037 fold of a game-server node id ("de-fra", "us-nj", ...) to its ranked region index (2 / 3 / 4),
// -1 when the table does not hold the node ("local"). The Ranked popup defaults to eu when -1.
int ranked_region_of_node(const char *node);
