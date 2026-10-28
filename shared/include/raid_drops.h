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
 *   - The boss death loop (deob-10.0.4.js:183020) rolls once per reward per player, twice when the match has
 *     isDoubleRaidDrops, in a public round, for each boss with a reward. The loop has no map test of its own, so
 *     which bosses double is set by their reward config (the changelog names Bastion's Hydra and Catacombs).
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

#endif
