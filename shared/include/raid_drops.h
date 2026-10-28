/*
 * shared/include/raid_drops.h - Krunker v10.0.4's raid boss drops (the rules; no AI, no clock, no RNG state).
 *
 * v10.0.4 (deob-10.0.4.js, the reference; the 10.0.3 rules are the same except where noted):
 *   - AI rarity chances (rewardChance): the boss's per-rarity drop odds, seven numbers (store.rarities) that must
 *     sum to 100. v10.0.4 checks the sum within 1e-6 (validator at deob-10.0.4.js:25057, the AI's assignment at
 *     deob-10.0.4.js:181990). v10.0.3 required the sum to be exactly 100, so a valid AI setting such as
 *     [33.3, 33.3, 33.3, 0.1] (99.99999999999999 in floating point) was dropped and the boss fell back to
 *     store.lootRars; a boss whose skins lack those rarities then dropped nothing. That is the v10.0.4 fix
 *     "raid bosses sometimes dropping nothing" (inferred: the only difference I found in the drop path between the
 *     bundles is this check; the reward loop, getItemByChance and the reward list parse are the same).
 *   - getItemByChance (deob-10.0.4.js:193984): a rarity is drawn from the weights (random(0.0001, 100), the first
 *     rarity whose running total exceeds it; 0 when none does), then one of the boss's reward items of that rarity
 *     is picked at random. A negative reward entry -n is n empty slots (-1). Nothing of that rarity: no drop.
 *   - AI.kill's reward loop (deob-10.0.4.js:183017-183038) runs for any AI with a reward (a boss or not) in a pub-type
 *     round: each eligible player (rewardReq) gets one getItemByChance, two when the match has isDoubleRaidDrops; a
 *     skin is given (rewardMethod 0; rewardUnique skips an owner), or dropped as a Loot Drop at the AI (1) or at a path
 *     node (2). The loop has no map test of its own: every rewarding AI doubles on a double-drop weekend (the changelog
 *     names Bastion's Hydra and Catacombs, the raids whose bosses have rewards). Wired in shared/src/ai.c.
 *   - isDoubleRaidDrops (deob-10.0.4.js:195385, iiìîíìî at deob-10.0.4.js:24851): a public round on a Saturday or
 *     Sunday (UTC), the same weekend schedule as dxp_schedule (double_xp.h). Set by updateEventFlags.
 */
#ifndef KN_SHARED_RAID_DROPS_H
#define KN_SHARED_RAID_DROPS_H

#include <stdint.h>

#define RAID_RARITIES 7                   /* store.rarities, the length of store.lootRars (deob-10.0.4.js:103699) */
#define RAID_REWARD_CHANCE_TOLERANCE 1e-6 /* the sum of rewardChance may differ from 100 by this much (v10.0.4) */
#define RAID_RANDOM_MIN 0.0001            /* getItemByChance's roll: random(0.0001, 100) */
#define RAID_RANDOM_MAX 100.0

typedef double (*RaidRandom)(void *user); /* Math.random(): [0, 1) */

/* The AI's rarity chances (rewardChance): `count` numbers in `in`, each 0..100 and finite, summing to 100 within
 * RAID_REWARD_CHANCE_TOLERANCE, `count` == RAID_RARITIES. Copies them to out and returns 1; returns 0 when invalid
 * (the boss then rolls store.lootRars). Non-finite and out-of-range values are dropped before the count check. */
int raid_reward_chance_parse(const double *in, int count, double out[RAID_RARITIES]);

/* isDoubleRaidDrops: a public round at now_ms (UTC) on a Saturday or Sunday. */
int raid_double_drops(int is_pub_type, int64_t now_ms);

/* The rolls of a boss's reward loop: 2 with double drops, else 1 (deob-10.0.4.js:183020). */
int raid_drop_rolls(int double_drops);

/* The rarity getItemByChance draws for roll (RAID_RANDOM_MIN..RAID_RANDOM_MAX) over weights (NULL = store.lootRars):
 * the first r whose running total of weights[r] || 0 exceeds roll, 0 when none does. */
int raid_roll_rarity(double roll, const double *weights);

/* One getItemByChance: the rarity for `roll` (see raid_roll_rarity), then a uniform pick among the reward items of
 * that rarity (a reward value < 0 contributes -value empty slots). rarity_of(user, skin) is the store skin's rarity
 * (-1: not a skin). Returns the skin index, or -1 for nothing (an empty slot, or no item of that rarity). The roll
 * and the pick draw from `random` (Math.random()). */
int raid_pick_drop(const int *reward, int count, const double *weights, int (*rarity_of)(void *user, int skin), void *user,
                   RaidRandom random, void *random_user);

/* ---- the AI's reward fields (the wiring: shared/src/ai.c, AI.kill) */
#define RAID_REWARD_MAX 64 /* reward entries kept per AI */
enum { RAID_METHOD_GIVE = 0, RAID_METHOD_DROP_AI = 1, RAID_METHOD_DROP_NODE = 2 };          /* rewardMethod */
enum { RAID_REQ_ACTIVE = 0, RAID_REQ_DAMAGED = 1, RAID_REQ_LAST_HIT = 2 };                  /* rewardReq */

/* A map AI's "rew" (addAI: isString(rew) ? rew.split(",") : null, deob-10.0.4.js:168538): each entry as getItemByChance's
 * parseInt reads it (deob-10.0.4.js:193993); an entry parseInt makes NaN contributes nothing and is left out. Returns
 * the entries kept (at most max). */
int raid_reward_parse_map(const char *csv, int *out, int max);
/* KrunkScript AI.spawn's reward (deob-10.0.4.js:186973-186980): split(","), entries matching /^\d+$/ only, each
 * limit(parseInt(v) || 0, 0, 1e6). Returns the entries kept (at most max); 0 = no reward (an empty list is none). */
int raid_reward_parse_script(const char *csv, int *out, int max);

/* The reward loop's player test (deob-10.0.4.js:183019): not a bot, and by rewardReq 0 active, 1 active and in the AI's
 * dmgList, 2 active and the AI's lastHit.doer. */
int raid_reward_eligible(int req, int is_bot, int active, int damaged, int last_hitter);

/* dropSkin's spread (iîïïîïï(3, 8), deob-10.0.4.js:23809, at :194163-194165): sign(random - 0.5) * (floor(random * 6) + 3),
 * two draws; 0 when the first draw is exactly 0.5 (Math.sign(0)). */
int raid_drop_spread(RaidRandom random, void *random_user);

#endif
