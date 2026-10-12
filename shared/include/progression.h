#pragma once
// player-viewer WP-E part 2c: Krunker v7.1.4 account progression shared by the game (per-match
// medal counts, stat and challenge events), the client (level progress, medal names, challenge
// names) and kn_brain (challenge evaluation at a stats commit, badge rules).
//
// Sources: src/data/account.js (updateLevel / getLevelProg), src/libs/game-overlay.js:141
// (overlay.medalsList), src/data/game.js incStat / incMedal / checkBadges, src/data/challenges.js
// (checkChallenge / finalize / updateNames) and i18n/en.js (chall.name.*).
//
// KrunkNative difference (documented): v7's game server keeps account.activeChallenges and runs
// checkChallenge on every event (players.js / kills.js / game.js call sites), then finalize() at
// the match end. KrunkNative's game server never holds the challenge rows: it tallies the
// events per (key, weapon, streak) (server/src/accounts.c) and commits the tallies with the
// match stats; kn_brain runs challenges_finalize() on them (services/brain/db_progress.c). The
// result is the same except that a challenge completes at the commit (the end of the match),
// so v7's in-match "Finish the game to complete" chat line is not sent.
#include <stddef.h>
#include <cJSON.h>

// ------------------------------------------------------------------ level (src/data/account.js)

#define PROGRESSION_RANK_VAR 0.03 // config.rankVar

// updateLevel(): max(1, floor(rankVar * sqrt(score))).
int progression_level(double score, double rank_var);

// getLevelProg() (v7.1.4): the progress to the next level in percent, linear in score between
// (level / rankVar)^2 (0 at level 1) and ((level + 1) / rankVar)^2, with v7's parseInt
// truncations. v3.1.1 used the fraction of rankVar * sqrt(score) instead.
double progression_level_prog(double score, double rank_var);

// ------------------------------------------------------------------ medals

// overlay.medalsList: the incMedal names that have an icon (img/medals/medal_<index>.png).
#define PROGRESSION_MEDAL_COUNT 56
extern const char *const g_progression_medals[PROGRESSION_MEDAL_COUNT];

// Index of `name` in overlay.medalsList, -1 when it is not an icon medal.
int progression_medal_index(const char *name);

// ------------------------------------------------------------------ stats (game.incStat)

// The account stat keys v7's incStat counts (transferStats adds them to account.stats).
// "sad" / "cad" (airdrops stolen / captured) included.
#define PROGRESSION_STAT_COUNT 20
extern const char *const g_progression_stats[PROGRESSION_STAT_COUNT];

// Index of `key` in g_progression_stats, -1 when unknown.
int progression_stat_index(const char *key);

// ------------------------------------------------------------------ challenges

typedef enum {
    CHALLENGE_KEY_ADD,     // vA754: progVal += value (47 keys)
    CHALLENGE_KEY_SET,     // vA755: progVal = value; kstk (max, see below), tower, pkr, island, time
    CHALLENGE_KEY_MAX,     // vA756: progVal = max(progVal, value): amc, uwk, rnd
    CHALLENGE_KEY_META,    // "chall": complete N other challenges (finalize)
    CHALLENGE_KEY_UNKNOWN, // never progresses
} ChallengeKeyKind;

ChallengeKeyKind challenge_key_kind(const char *key);

// The match a commit describes (checkChallenge's game fields).
typedef struct {
    int map_index;   // game.mapIndex (official map index), -1 none / custom
    int map_id;      // game.customMapData.id (published map id), -1 none
    int mode;        // game.modeIndex
    int class_index; // player.classIndex
    int ranked;      // game.isComp ("ranked" challenges need it)
    int quit;        // finalize(quit): a player leaving mid-match keeps only DAT.nem challenges
} ChallengeMatch;

// finalize() of one account for one match.
//   active:  the account's activeChallenges, objects {CI, T, DAT{ky, val, wep, cls, mp, mid, md,
//            stk, item, lesser, sm, nem, evt, seas}, REW{kr, jnk, sk, bpp}, PV, PT}; PV / PT are
//            updated in place.
//   events:  the match's tallies [[key, value, weapon, streak], ...]: one row per (key, weapon,
//            streak), weapon / streak -1 for none. ADD keys carry the sum, MAX / SET keys the
//            highest / last value. "kstk" rows with weapon -1 carry the kill streak (v7's
//            nonWep), with a weapon the weapon streak.
//   results: appended v7's chg triples [done, CI, PV] (vA759).
//   completed: appended the challenge objects completed by this match (chgC).
//   reward:  {kr, skins[]} summed over the completed challenges' REW (junk and Battle Pass
//            points are not part of KrunkNative's economy and are left out).
// Returns the number of challenges whose progress changed.
int challenges_finalize(cJSON *active, const cJSON *events, const ChallengeMatch *match, cJSON *results, cJSON *completed,
                        cJSON *reward);

// challenges.updateNames(): a challenge without a name N gets i18n "chall.name.<ky>"
// ("Get {val} headshots {with <weapon>}"); the weapon part only for "daily" challenges with a
// DAT.wep. `weapon_name` maps a weapon index to its name (NULL / unknown: no weapon part).
// Writes "" when the key has no template.
void challenge_name(const cJSON *challenge, const char *(*weapon_name)(int index), char *out, size_t size);

// ------------------------------------------------------------------ badges (src/data/badges.js)

// badges[index].claimable && badges[index].req(account) for an account whose stats object is
// `stats` (account.stats): Nuclear I-IV need stats.n >= 500 / 750 / 1000 / 1500. The season
// badges need a Battle Pass (out of scope, doc/v7/decisions.md): never.
int badge_requirement_met(int index, const cJSON *stats);
