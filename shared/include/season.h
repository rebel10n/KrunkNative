#pragma once
// Krunker v7.1.4 seasons and the Battle Pass, shared by kn_brain (the authority: BPP awards,
// claims, KR purchases, season badges), the client (windows[5] "battlepass", the end screen's
// #endBPLayout, the menu's season label) and the tests.
//
// Sources: src/config.js (currentSeason 7, currentBattlepass 2, bppCapDay 300, bpTabs,
// levelSkipPrice), src/index.js windows[5] (gen, claimItem, increaseSkip, purchasePop),
// src/ui/game.js endScreen.tabs.performance (bpData), src/data/game.js endGame (tmpBPData:
// the per-match BPP), src/data/challenges.js finalize (REW.bpp), src/data/badges.js (season
// badges) and the v7.1.4 server's battle pass (server/DBClient.ts getBattlePassConfig /
// getBattlePassProgress, server/index.ts createLocalBattlePass, accounts_schema.sql
// qs_battlepass / qs_battlepass_transactions).
//
// KrunkNative differences (documented):
// - v7 sells the VIP tier and level skips for real money (Xsolla / Steam). KrunkNative has no
//   payments: both are paid in KR from the kn_brain wallet (kr_transactions), at the prices of
//   the battlepasses row (tier1_price_kr, skip_price_kr).
// - v7's battle pass rows (itemdata, dates, prices) lived in its database; KrunkNative seeds its
//   own Season 7 pass (migration 0108_seasons) from the store's season-7 "battlepass" skins and
//   the Season 10 pass (0355_season_10) from what the v10.0.3 bundle defines (80 levels, the
//   badges' BPP sums; its rewards come only over krunker.io's socket, so they are empty).
// - The "Founder" badges need the VIP tier bought within the first week of the pass
//   (startdate + 7 days) instead of v7's fixed 2022 / 2023 dates.
#include <stddef.h>
#include <stdint.h>

#define SEASON_CURRENT 10     // config.currentSeason (v10.0.3 deob L19368: 10; the brain's accounts.current_season default)
#define BATTLEPASS_CURRENT 3  // the Season 10 pass's bp_id (migration 0355_season_10; v7's config.currentBattlepass was 2)
#define BP_CAP_DAY 500        // v10.0.2 windows[5].gen 500 * days (v7.1.4 config.bppCapDay 300): the daily BPP cap
#define BP_MATCH_SCORE_MAX 20 // endGame: min(20, floor(score * rewardMlt * 0.01))
#define BP_MATCH_PLAY_MAX 30  // endGame: 30 x min(1, floor(playTime / elapsed * 10) / 10), the play term
#define BP_TRACKS 2           // windows[5].gen: row 0 "Free", row 1 "VIP" (tier >= 1)
#define BP_ITEMS_MAX 512
#define BP_LEVELS_MAX 256     // v10 leveldata: levels with their own BPP cost
#define BP_FOUNDER_DAYS 7     // badges.js "Purchase the Battle Pass in the first week"
#define BP_MASTER_BPP 15000   // badges.js S6 / S7 Battle Pass Master: BP.tier && bpp >= 15000
// v10.0.3's Season 10 pass badges (deob-10.0.3.js:116167-116190): S10 Battle Pass BP.bpp >= 25000 (no tier),
// S10 Supreme Battle Pass and S10 Master Battle Pass BP.tier && bpp >= 39000 / 55000
#define BP_S10_BPP 25000
#define BP_S10_SUPREME_BPP 39000
#define BP_S10_MASTER_BPP 55000

// One reward of itemdata ("id,tier,col,sk" groups, UTILS.arrayChunk(itemdata.split(","), 4)).
typedef struct {
    int id;    // claim id (bpclm)
    int tier;  // track: 0 free, 1 VIP
    int level; // column, 0-based: level = col + 1
    int skin;  // store.skins index, 0 = none (the card stays empty unless kr / jnk)
    int kr;    // v10 krdata "id:kr": the card's KR reward (0 none)
    double jnk; // v10 jnkdata "id:jnk": its junk (shown; KrunkNative has no junk economy)
} BattlePassItem;

// io_dataLoaded / "bpdl": the pass (qs_battlepass + its items).
typedef struct BattlePass {
    int id;
    int season;
    char name[64];        // bpSeasonH
    int bpp_per_level;
    int item_count;       // levels
    int64_t start_s;      // startdate (unix s): the daily cap counts from its UTC day
    int64_t end_s;        // enddate
    int tier1_price_kr;   // KrunkNative: the VIP tier in KR (v7: tier1price USD)
    int skip_price_kr;    // KrunkNative: one skipped level in KR (v7: config.levelSkipPrice USD)
    char tier1_name[16];  // tier1name ("VIP")
    int count;
    BattlePassItem items[BP_ITEMS_MAX];
    int level_bpp[BP_LEVELS_MAX]; // v10 leveldata: level n's BPP at [n - 1], 0 = bpp_per_level
} BattlePass;

// Parses itemdata into bp->items; returns the number of items.
int bp_parse_items(BattlePass *bp, const char *itemdata);
// Writes itemdata back ("id,tier,col,sk,..."); returns the length (truncated to size - 1).
size_t bp_format_items(const BattlePass *bp, char *out, size_t size);
// Krunker v10.0.2 (windows[5].gen, _52102 / _52111): leveldata "level:bpp,..." (the BPP a level
// costs; others cost bpp_perlevel) and krdata / jnkdata "itemId:amount,..." (a card's KR / junk
// reward). Parse after bp_parse_items; NULL / "" leave the v7.1.4 behaviour.
void bp_parse_levels(BattlePass *bp, const char *leveldata);
void bp_parse_rewards(BattlePass *bp, const char *krdata, const char *jnkdata);
// The BPP level `level` (1-based) costs.
int bp_level_need(const BattlePass *bp, int level);
// A claimable card: a skin, KR or junk (v10: a card with kr / jnk and no skin shows them).
int bp_item_has_reward(const BattlePassItem *it);

// windows[5].gen: battlepassLvl = floor(bpp / bpp_perlevel) + 1, capped at itemcount once
// complete; *points = bpp % bpp_perlevel (bpp_perlevel when complete); *complete = past the
// last level. Any out pointer may be NULL. v10 (_52111): the levels' own costs (leveldata);
// uniform levels give the same answer.
int bp_level(const BattlePass *bp, int64_t bpp, int64_t *points, int *complete);
// itemcount * bpp_perlevel (v10: the sum of the levels' costs)
int64_t bp_max_bpp(const BattlePass *bp);
// (floor((now - startdate) / 1 day) + 1) * bppCapDay: the BPP the cap allows by `now_s`.
int64_t bp_cap_total(const BattlePass *bp, int64_t now_s);
// "Available BPP": max(cap - bpp, 0).
int64_t bp_available(const BattlePass *bp, int64_t bpp, int64_t now_s);

// endGame's per-match BPP inputs (src/data/game.js endGame; v10.0.3 deob 221757-221776, doc/v10/rules-1003.md).
typedef struct {
    double score;        // player.score
    double reward_mlt;   // mode.rewardMlt (<= 0: 1)
    int pub_type;        // game.isPubType(): the block runs for public matches only (no host, or mimicPubs)
    int raid;            // mode.id === "raid": no play term
    int empty_lobby;     // game.isEmptyLobby: true from init (10.0.3 never clears it), so the play term is off
    double play_ms;      // player.playTime (ms)
    double elapsed_ms;   // mode clock run so far: gameTime - max(0, gameTimerP) (ms)
} BpMatchInput;
// The BPP a public match asks for before the daily cap (tmpBPData.unbpp): Math.round(min(20,
// floor(score * (rewardMlt || 1) * 0.01)) + play), play = 30 x min(1, floor(playTime / elapsed *
// 10) / 10) when !emptyLobby and not raid, else 0. 0 when not a public match.
int64_t bp_match_points(const BpMatchInput *in);

// battlePass.validateProgress(currentBpp, amount): {award, newAvailable} under the daily cap
// and the pass total.
typedef struct {
    int64_t award;
    int64_t remaining; // rbpp: what the cap still allows after the award
} BpAward;
BpAward bp_validate_progress(const BattlePass *bp, int64_t bpp, int64_t requested, int64_t now_s);

const BattlePassItem *bp_item(const BattlePass *bp, int id);
const BattlePassItem *bp_item_at(const BattlePass *bp, int tier, int level0);
// tmpBPData.re: the reward of the next level (column battlepassLvl), the VIP card first (the
// end screen locks it for a free player: #endBPRewardLock), else the free one; NULL when the
// pass is complete or the column is empty. (v7's server code is not in the tree; the v7.1.4
// reconstruction's getNextReward picked the current column, which the end screen labels "Next
// Level Reward".)
const BattlePassItem *bp_next_reward(const BattlePass *bp, int64_t bpp);

// items_claimed ("id,id,..."): 1 when `id` is in it.
int bp_is_claimed(const char *items_claimed, int id);
// Appends `id` to items_claimed (out may alias nothing; size of out).
void bp_add_claimed(const char *items_claimed, int id, char *out, size_t size);

typedef enum {
    BP_CLAIM_OK = 0,
    BP_CLAIM_NOT_FOUND,  // no such item (or an empty card)
    BP_CLAIM_LEVEL,      // level not reached
    BP_CLAIM_TIER,       // VIP item without the VIP tier
    BP_CLAIM_CLAIMED,    // already claimed
} BpClaimResult;
// The CLAIM button rule of windows[5].gen: (free || VIP tier) && level reached && unclaimed.
BpClaimResult bp_claim_check(const BattlePass *bp, int64_t bpp, int tier, const char *items_claimed, int id);
const char *bp_claim_error(BpClaimResult r);

// windows[5].increaseSkip: 1 .. max(1, itemcount - battlepassLvl) levels.
int bp_skip_max(const BattlePass *bp, int64_t bpp);
// The bpp after skipping `levels` (each adds bpp_perlevel, v10: the cost of the level it skips,
// capped at the pass total).
int64_t bp_skip_bpp(const BattlePass *bp, int64_t bpp, int levels);

// store.skins[].mapReward of `season`: the skin a finished public match on official map
// `map_index` drops with a 1 in 100 chance (game.js endGame: mapReward == mapIndex && seas ==
// currentSeason); 0 none.
int season_map_reward(int season, int map_index);
#define SEASON_MAP_REWARD_CHANCE 100 // UTILS.randInt(1, 100) == 100

// ------------------------------------------------------------------ season badges

// The account facts badges.js's rules read.
typedef struct {
    int season;          // config.currentSeason
    int64_t now_s;
    int bp_tier;         // account.BP.tier (the current pass)
    int64_t bpp;         // account.BP.bpp
    int64_t bp_tier_at_s; // when the VIP tier was bought (0 = unknown / never)
    int64_t bp_start_s;  // the pass's startdate (Founder: bought before start + 7 days)
    const int *badges;   // account.badges (earned indices)
    int badge_count;
    int (*has_skin)(void *ud, int skin); // account.hasSkin (NULL: none owned)
    void *ud;
} SeasonBadgeAccount;

// Rule of a season badge (index into g_badges: 22..28, and v10.0.3's 61..63): -1 when `index` is not
// a season badge, else 0 / 1. v7's dates are replaced as documented above. v10.0.3 folds the Season 6 /
// 7 rules to false (deob-10.0.3.js:115939-116011: its season is 10), which the season checks give too.
int season_badge_rule(int index, const SeasonBadgeAccount *a);
