// raid_drops_test: Krunker v10.0.4's raid boss drops (shared/include/raid_drops.h).
//   - AI rarity chances: a sum within 1e-6 of 100 is accepted (the v10.0.4 fix; 99.99999999999999 was rejected by v10.0.3)
//   - double drops: two rolls on a Saturday or Sunday in a public round, one otherwise
//   - getItemByChance: rarity from the weights (lootRars by default), a uniform pick, empty slots and no-match drop nothing
#include <raid_drops.h>

#include <math.h>
#include <stdio.h>

static int g_fail;
#define CHECK(c, ...)                                         \
    do {                                                      \
        if (!(c)) {                                           \
            g_fail++;                                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);                     \
            fprintf(stderr, "\n");                            \
        }                                                     \
    } while (0)

// Math.random() stubs: a fixed value, then the values in order
static double fixed_value;
static double fixed_random(void *user) {
    (void) user;
    return fixed_value;
}

static double g_seq[4];
static int g_seq_at;
static double seq_random(void *user) {
    (void) user;
    return g_seq[g_seq_at++ & 3];
}

static int rarity_of_skin(void *user, const int skin) {
    const int *rarities = user;
    return skin >= 0 && skin < 16 ? rarities[skin] : -1;
}

int main(void) {
    // rarities of store.skins 0..15 (Common 0 .. Unobtainable 6); -1 entries are not skins
    static const int rarities[16] = {0, 1, 2, 2, 3, 3, 4, 5, 6, 0, 1, 2, 3, 4, 5, 6};
    double out[RAID_RARITIES];

    // rewardChance: the bundle's exact check misses 33.3 + 33.3 + 33.3 + 0.1 (99.99999999999999)
    const double near[RAID_RARITIES] = {33.3, 33.3, 33.3, 0.1, 0, 0, 0};
    double sum = 0;
    for (int i = 0; i < RAID_RARITIES; i++) sum += near[i];
    CHECK(sum != 100.0, "fixture must be inexact: %.17g", sum);
    CHECK(raid_reward_chance_parse(near, RAID_RARITIES, out) == 1, "a sum within 1e-6 of 100 is accepted");
    CHECK(out[3] == 0.1 && out[0] == 33.3, "the accepted chances are kept in order");

    const double exact[RAID_RARITIES] = {0, 0, 50, 50, 0, 0, 0};
    CHECK(raid_reward_chance_parse(exact, RAID_RARITIES, out) == 1, "an exact 100 is accepted");

    const double low[RAID_RARITIES] = {0, 0, 50, 49, 0, 0, 0};
    CHECK(raid_reward_chance_parse(low, RAID_RARITIES, out) == 0, "a sum of 99 is rejected");
    const double big[RAID_RARITIES] = {0, 0, 50, 50, 0, 0, 0.5};
    CHECK(raid_reward_chance_parse(big, RAID_RARITIES, out) == 0, "a sum of 100.5 is rejected");
    const double over[RAID_RARITIES] = {0, 0, 101, 0, 0, 0, 0};
    CHECK(raid_reward_chance_parse(over, RAID_RARITIES, out) == 0, "a value over 100 is dropped, leaving too few");
    const double with_nan[RAID_RARITIES + 1] = {0, 0, 50, 50, 0, 0, 0, NAN};
    CHECK(raid_reward_chance_parse(with_nan, RAID_RARITIES + 1, out) == 1, "a NaN entry is filtered out, leaving seven valid");
    CHECK(raid_reward_chance_parse(exact, 6, out) == 0, "six values are rejected");
    CHECK(raid_reward_chance_parse(NULL, RAID_RARITIES, out) == 0, "no array is rejected");

    // double drops: Saturday 2026-10-31 and Sunday 2026-11-01 (UTC) in a public round, Monday 2026-10-26 otherwise
    // (12:00 UTC on each day: Date.UTC(2026, 9, 31, 12) and so on)
    const int64_t saturday = 1793448000000LL;
    const int64_t sunday = 1793534400000LL;
    const int64_t monday = 1793016000000LL;
    CHECK(raid_double_drops(1, saturday) == 1, "a public Saturday doubles");
    CHECK(raid_double_drops(1, sunday) == 1, "a public Sunday doubles");
    CHECK(raid_double_drops(1, monday) == 0, "a public Monday does not");
    CHECK(raid_double_drops(0, saturday) == 0, "a private round does not, even on a weekend");
    CHECK(raid_drop_rolls(1) == 2 && raid_drop_rolls(0) == 1, "two rolls with double drops, else one");

    // getItemByChance's rarity: lootRars when the boss gives no weights (cumulative 0, 0, 70.395, 95.885, 99.885, ...)
    CHECK(raid_roll_rarity(0.0001, NULL) == 2, "a roll of 0.0001 lands on Rare (lootRars[2] starts at 0)");
    CHECK(raid_roll_rarity(70.394, NULL) == 2, "70.394 is still in the 70.395 band");
    CHECK(raid_roll_rarity(70.396, NULL) == 3, "70.396 passes into the next band");
    CHECK(raid_roll_rarity(99.9999, NULL) == 6, "the top band holds the last weight");
    const double only_mythic[RAID_RARITIES] = {0, 0, 0, 0, 0, 0, 100};
    CHECK(raid_roll_rarity(50.0, only_mythic) == 6, "custom weights replace lootRars");

    // rewards 0 1 2 3 7 9 with all weight on rarity 0: the roll is 50.00005 (rarity 0), the pick is
    // floor(0.5 * 2) = 1 over the rarity-0 items (skins 0 and 9), so 9
    static const int reward_a[] = {0, 1, 2, 3, 7, 9};
    const double rarity0[RAID_RARITIES] = {100, 0, 0, 0, 0, 0, 0};
    fixed_value = 0.5;
    const int pick0 = raid_pick_drop(reward_a, 6, rarity0, rarity_of_skin, (void *) rarities, fixed_random, NULL);
    CHECK(pick0 == 9, "the rarity-0 pool (skins 0 and 9): pick floor(0.5 * 2) = 1 -> 9, got %d", pick0);

    // a pool with nothing of the drawn rarity drops nothing (-1)
    static const int reward_b[] = {2, 3};
    const double all_common[RAID_RARITIES] = {100, 0, 0, 0, 0, 0, 0};
    CHECK(raid_pick_drop(reward_b, 2, all_common, rarity_of_skin, (void *) rarities, fixed_random, NULL) == -1,
          "a rarity-0 roll over rare-only rewards drops nothing");

    // an empty slot (-n) is a drop of nothing when picked
    static const int reward_empty[] = {-3};
    CHECK(raid_pick_drop(reward_empty, 1, all_common, rarity_of_skin, (void *) rarities, fixed_random, NULL) == -1,
          "empty slots drop nothing");

    // a map AI's rew (deob-10.0.4.js:168538, read by parseInt at :193993): "12a" is 12, " 7" is 7, "-3" is 3 empty slots,
    // "abc" and "" are NaN (left out), "0x1f" is 31 (parseInt without a radix reads hex)
    int rew[RAID_REWARD_MAX];
    int n = raid_reward_parse_map("12a, 7,-3,abc,,0x1f", rew, RAID_REWARD_MAX);
    CHECK(n == 4 && rew[0] == 12 && rew[1] == 7 && rew[2] == -3 && rew[3] == 31, "rew as parseInt reads it (%d: %d %d %d %d)", n, rew[0],
          n > 1 ? rew[1] : 0, n > 2 ? rew[2] : 0, n > 3 ? rew[3] : 0);
    CHECK(raid_reward_parse_map("", rew, RAID_REWARD_MAX) == 0 && raid_reward_parse_map(NULL, rew, RAID_REWARD_MAX) == 0, "no entries");
    CHECK(raid_reward_parse_map("1,2,3", rew, 2) == 2, "at most max entries");

    // KrunkScript AI.spawn's reward (:186973-186980): /^\d+$/ entries only, limit(parseInt || 0, 0, 1e6)
    n = raid_reward_parse_script("5,12a, 7,-3,2000000,0", rew, RAID_REWARD_MAX);
    CHECK(n == 3 && rew[0] == 5 && rew[1] == 1000000 && rew[2] == 0, "reward keeps /^\\d+$/ entries, limited to 1e6 (%d: %d %d %d)", n,
          rew[0], n > 1 ? rew[1] : 0, n > 2 ? rew[2] : 0);
    CHECK(raid_reward_parse_script("x,y", rew, RAID_REWARD_MAX) == 0, "a reward with no digit entry is none");

    // the loop's player test (:183019): bots never; 0 active, 1 active damager, 2 active last hitter; another rewardReq (a
    // map's unclamped rewr) passes every test
    CHECK(!raid_reward_eligible(RAID_REQ_ACTIVE, 1, 1, 1, 1), "a bot is never eligible");
    CHECK(raid_reward_eligible(RAID_REQ_ACTIVE, 0, 1, 0, 0) && !raid_reward_eligible(RAID_REQ_ACTIVE, 0, 0, 1, 1), "0: active");
    CHECK(raid_reward_eligible(RAID_REQ_DAMAGED, 0, 1, 1, 0) && !raid_reward_eligible(RAID_REQ_DAMAGED, 0, 1, 0, 1) &&
              !raid_reward_eligible(RAID_REQ_DAMAGED, 0, 0, 1, 0), "1: active and in dmgList");
    CHECK(raid_reward_eligible(RAID_REQ_LAST_HIT, 0, 1, 0, 1) && !raid_reward_eligible(RAID_REQ_LAST_HIT, 0, 1, 1, 0) &&
              !raid_reward_eligible(RAID_REQ_LAST_HIT, 0, 0, 0, 1), "2: active and lastHit.doer");
    CHECK(raid_reward_eligible(5, 0, 0, 0, 0), "rewardReq 5: no test applies (an inactive player passes, as in the bundle)");

    // dropSkin's spread iîïïîïï(3, 8) (:23809): sign(r1 - 0.5) * (floor(r2 * 6) + 3)
    static const double draws[][3] = {{0.9, 0.0, 3}, {0.9, 0.999, 8}, {0.1, 0.5, -6}, {0.5, 0.7, 0}};
    for (size_t i = 0; i < sizeof(draws) / sizeof(draws[0]); i++) {
        g_seq[0] = draws[i][0];
        g_seq[1] = draws[i][1];
        g_seq_at = 0;
        const int spread = raid_drop_spread(seq_random, NULL);
        CHECK(spread == (int) draws[i][2], "spread for draws %.3f %.3f is %d, got %d", draws[i][0], draws[i][1], (int) draws[i][2], spread);
    }

    if (g_fail) {
        fprintf(stderr, "raid_drops_test: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("raid_drops_test: OK\n");
    return 0;
}
