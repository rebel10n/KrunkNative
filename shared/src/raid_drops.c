/*
 * shared/src/raid_drops.c - Krunker v10.0.4's raid boss drops (raid_drops.h).
 */
#include <raid_drops.h>
#include <double_xp.h> // dxp_schedule: the Saturday / Sunday schedule of isDoubleRaidDrops

#include <limits.h>
#include <math.h>
#include <string.h>

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

// JS parseInt(s) (no radix) of one comma-separated entry [s, end): leading white space, a sign, "0x" hex, then digits;
// 0 = NaN (no digit)
static int js_parse_int(const char *s, const char *end, long long *out) {
    while (s < end && (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r' || *s == '\v' || *s == '\f')) s++;
    int neg = 0;
    if (s < end && (*s == '+' || *s == '-')) neg = *s++ == '-';
    int base = 10;
    if (end - s >= 2 && s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) {
        base = 16;
        s += 2;
    }
    long long v = 0;
    int digits = 0;
    for (; s < end; s++) {
        const int c = (unsigned char) *s;
        const int d = c >= '0' && c <= '9' ? c - '0' : base == 16 && c >= 'a' && c <= 'f' ? c - 'a' + 10 : base == 16 && c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1;
        if (d < 0) break;
        if (v < 1000000000000LL) v = v * base + d; // larger than any skin id: kept as is, never a store skin
        digits++;
    }
    if (!digits) return 0;
    *out = neg ? -v : v;
    return 1;
}

int raid_reward_parse_map(const char *csv, int *out, const int max) {
    if (!csv || !out || max <= 0) return 0;
    int n = 0;
    for (const char *p = csv;;) {
        const char *comma = strchr(p, ',');
        const char *end = comma ? comma : p + strlen(p);
        long long v;
        if (n < max && js_parse_int(p, end, &v)) out[n++] = v > INT_MAX ? INT_MAX : v < -1000000 ? -1000000 : (int) v; // -n: n empty slots
        if (!comma) break;
        p = comma + 1;
    }
    return n;
}

int raid_reward_parse_script(const char *csv, int *out, const int max) {
    if (!csv || !out || max <= 0) return 0;
    int n = 0;
    for (const char *p = csv;;) {
        const char *comma = strchr(p, ',');
        const char *end = comma ? comma : p + strlen(p);
        int digits = end > p; // /^\d+$/: one digit or more, nothing else
        for (const char *q = p; q < end && digits; q++) digits = *q >= '0' && *q <= '9';
        long long v = 0;
        if (digits && n < max && js_parse_int(p, end, &v)) out[n++] = v > 1000000 ? 1000000 : (int) v; // limit(parseInt || 0, 0, 1e6)
        if (!comma) break;
        p = comma + 1;
    }
    return n;
}

int raid_reward_eligible(const int req, const int is_bot, const int active, const int damaged, const int last_hitter) {
    if (is_bot) return 0;
    if (req == RAID_REQ_ACTIVE) return active;
    if (req == RAID_REQ_DAMAGED) return active && damaged;
    if (req == RAID_REQ_LAST_HIT) return active && last_hitter;
    return 1; // no requirement matches another value (rewardReq is limited to 0..2 where it is set)
}

int raid_drop_spread(RaidRandom random, void *random_user) {
    const double sign_draw = random(random_user) - 0.5;
    const int magnitude = (int) floor(random(random_user) * 6) + 3;
    return sign_draw > 0 ? magnitude : sign_draw < 0 ? -magnitude : 0;
}
