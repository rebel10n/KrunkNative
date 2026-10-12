#pragma once
// Krunker v7.1.4 ranked: the rating rules shared by the brain (services/brain/ranked.c, the
// authority), the client (the Ranked window's rank card) and the tests (doc/backend.md 6.5).
//
// v7.1.4's src/config.js keeps the ranked constants (eloPer 120, eloK 32, minRankedLevel 15,
// rankedCooldown 7200000, rankedDeduction 30, rankedClasses) but its client bundle no longer
// reads them and its reconstructed server has no ranked code. The formula is the last one
// Krunker shipped readable, v3.1.1 src/data/game.js updateELO / checkLeave: each player against
// the average rating of the other team, divisor 330, K = eloK, ratings floored at 0, an
// abandoner loses rankedDeduction. Ranks are v3.1.1 UTILS.eloIconId (eloPer 120, maxELOLevel 8).

#define RANKED_ELO_K 32.0            // config.eloK
#define RANKED_ELO_DIVISOR 330.0     // v3.1.1 game.js updateELO: 10^((opponents - player) / 330)
#define RANKED_ELO_PER 120           // config.eloPer: rating per rank
#define RANKED_MAX_LEVEL 8           // v3.1.1 config.maxELOLevel (dropped from v7 with the icons)
#define RANKED_MIN_LEVEL 15          // config.minRankedLevel
#define RANKED_COOLDOWN_MS 7200000   // config.rankedCooldown: queue ban after abandoning
#define RANKED_DEDUCTION 30          // config.rankedDeduction: rating lost by an abandoner
#define RANKED_TEAM_SIZE 4           // "Krunker Competitive" 4v4 (hostTypes[1] overDat minPlayers 8, tmSize 3)

// config.rankedClasses: the classes a comp lobby allows (classLim<i> defC 1)
#define RANKED_CLASS_COUNT 11
extern const int g_ranked_classes[RANKED_CLASS_COUNT];
int ranked_class_allowed(int class_index);

// Expected score of a player rated `rating` against a team averaging `opponents`.
double ranked_expected(double rating, double opponents);
// Rating change: K * (score - expected), score 1 win / 0 loss / 0.5 draw; an abandoner loses
// RANKED_DEDUCTION instead. The result never takes the rating below 0.
double ranked_delta(double rating, double opponents, double score, int abandoned, double k);
// UTILS.eloIconId: 0..RANKED_MAX_LEVEL-1 (rank 1 below 2 x eloPer, then one per eloPer).
int ranked_rank_index(double rating);
// "Rank N" display number of ranked_rank_index (1-based).
int ranked_rank_number(double rating);
