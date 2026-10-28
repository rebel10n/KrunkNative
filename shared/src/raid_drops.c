/*
 * shared/src/raid_drops.c - Krunker v10.0.4's raid boss drops (raid_drops.h).
 */
#include <raid_drops.h>
#include <double_xp.h> // dxp_schedule: the Saturday / Sunday schedule of isDoubleRaidDrops

#include <math.h>

// store.lootRars (deob-10.0.4.js:103699): getItemByChance's weights when the boss gives none
static const double k_loot_rars[RAID_RARITIES] = {0, 0, 70.395, 25.49, 4, 0.105, 0.01};

// JS `x || 0` for a weight: NaN and undefined count as nothing
static double weight_at(const double *weights, const int r) {
    const double w = weights ? weights[r] : k_loot_rars[r];
    return isfinite(w) ? w : 0.0;
}

int raid_reward_chance_parse(const double *in, const int count, double out[RAID_RARITIES]) {
    // deob-10.0.4.js:25057: map(Number).filter(finite, 0..100), then the length must equal the rarity count and
    // the sum must be 100 within RAID_REWARD_CHANCE_TOLERANCE (the v10.0.3 check demanded exact equality)
    if (!in || count < 0) return 0;
    double kept[RAID_RARITIES];
    int n = 0;
    double sum = 0;
    for (int i = 0; i < count; i++) {
        const double v = in[i];
        if (!isfinite(v) || v < 0 || v > 100) continue;
        if (n == RAID_RARITIES) return 0; // more valid values than rarities
        kept[n++] = v;
        sum += v;
    }
    if (n != RAID_RARITIES || fabs(sum - 100.0) > RAID_REWARD_CHANCE_TOLERANCE) return 0;
    for (int r = 0; r < RAID_RARITIES; r++) out[r] = kept[r];
    return 1;
}

int raid_double_drops(const int is_pub_type, const int64_t now_ms) {
    // isDoubleRaidDrops = isPubType() && iiìîíìî() (deob-10.0.4.js:195385, weekend check at :24851)
    return is_pub_type && dxp_schedule(now_ms).double_raid_drops;
}

int raid_drop_rolls(const int double_drops) {
    return double_drops ? 2 : 1; // deob-10.0.4.js:183020
}

int raid_roll_rarity(const double roll, const double *weights) {
    // getItemByChance: `roll < (total += weights[r] || 0)` (deob-10.0.4.js:193984); 0 when no running total passes it
    double total = 0;
    for (int r = 0; r < RAID_RARITIES; r++) {
        total += weight_at(weights, r);
        if (roll < total) return r;
    }
    return 0;
}

int raid_pick_drop(const int *reward, const int count, const double *weights, int (*rarity_of)(void *user, int skin), void *user,
                   RaidRandom random, void *random_user) {
    // getItemByChance draws the roll first, then the pick, whether or not there is anything to pick from
    const double roll = RAID_RANDOM_MIN + random(random_user) * (RAID_RANDOM_MAX - RAID_RANDOM_MIN);
    const int rarity = raid_roll_rarity(roll, weights);
    // J: the reward items of that rarity, a negative entry -n is n empty slots (-1)
    int len = 0;
    for (int i = 0; i < count; i++) {
        if (reward[i] < 0) len += -reward[i];
        else if (rarity_of(user, reward[i]) == rarity) len++;
    }
    // randInt(0, len - 1) = floor(random * len); with len 0 the pick is undefined (NaN in JS): no drop
    int k = (int) floor(random(random_user) * len);
    if (!len || k >= len) return -1;
    for (int i = 0; i < count; i++) {
        if (reward[i] < 0) {
            if (k < -reward[i]) return -1; // an empty slot
            k -= -reward[i];
        } else if (rarity_of(user, reward[i]) == rarity) {
            if (k == 0) return reward[i];
            k--;
        }
    }
    return -1; // not reached: the pick is one of the len entries counted above
}
