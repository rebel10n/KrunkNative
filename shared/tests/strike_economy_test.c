// strike_economy_test: Krunker Strike's economy tables and rules (shared/include/strike_economy.h).
//   - the catalogue: pools hold spinnable items of their rarity, the classes and defaults are there
//   - the wheels: odds are the bundle's crate tiers (Starter 75/22/3, Elite 50/30/15/5, Heroic
//     0/48/35/14/2.49/0.5/0.01), prices and currencies; a million rolls land within tolerance
//   - guaranteedIfSpinCountIs: every 10x Lucky Draw holds an Epic or better
//   - the account XP curve (100 + 100 * floor(L / 3.5)) and the golden spin meter (10)
//   - daily missions and deals: deterministic, distinct
#include <strike_economy.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_fail;
#define CHECK(c, ...)                         \
    do {                                      \
        if (!(c)) {                           \
            g_fail++;                         \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
            fprintf(stderr, __VA_ARGS__);     \
            fprintf(stderr, "\n");            \
        }                                     \
    } while (0)

static uint64_t g_rng = 0x1234567ULL;
static double rnd(void) {
    g_rng ^= g_rng << 13;
    g_rng ^= g_rng >> 7;
    g_rng ^= g_rng << 17;
    return (double) (g_rng >> 11) / 9007199254740992.0;
}

static void test_catalogue(void) {
    const StrikeEconomy *e = strike_economy();
    CHECK(!strcmp(e->game_version, "1.37.2"), "game version %s", e->game_version);
    CHECK(e->item_count > 600, "catalogue has %d items", e->item_count);
    CHECK(!strcmp(strike_currency_key(STRIKE_CUR_KR), "kr") && strike_currency_from_key("spin_golden_ticket") == STRIKE_CUR_GOLDEN_TICKET,
          "currency keys");
    CHECK(!strcmp(e->rarities[0].name, "Uncommon") && !strcmp(e->rarities[6].name, "Unobtainable") &&
              !strcmp(e->rarities[3].color, "#FBC02D"), "rarities");
    CHECK(e->pool_count == 7, "pools %d", e->pool_count);
    for (int p = 0; p < e->pool_count; p++) {
        const StrikePool *pool = &e->pools[p];
        CHECK(pool->count > 0, "pool %s is empty", pool->id);
        for (int k = 0; k < pool->count; k++) {
            const StrikeItem *it = &e->items[pool->items[k]];
            CHECK(it->spinnable && it->rarity == pool->rarity, "pool %s holds %s (rarity %d)", pool->id, it->id, it->rarity);
        }
    }
    // the six class weapons (weapon enum) have skins, a default each; skins name store indices
    const int weapons[] = {0, 1, 3, 4, 5, 8};
    for (size_t w = 0; w < sizeof weapons / sizeof *weapons; w++) {
        int skins = 0;
        for (int i = 0; i < e->item_count; i++) skins += e->items[i].type == STRIKE_ITEM_WEAPON && e->items[i].weapon == weapons[w] && e->items[i].skin >= 0;
        CHECK(skins >= 50, "weapon %d has %d skins", weapons[w], skins);
        CHECK(strike_default_item(STRIKE_ITEM_WEAPON, weapons[w]) != NULL, "weapon %d has no default", weapons[w]);
    }
    const StrikeItem *arctic = strike_item("s0"); // Arctic Hunt (Sniper), "the oldest weapon skin"
    CHECK(arctic && arctic->skin == 0 && arctic->weapon == 0 && arctic->rarity == 1 && !strcmp(arctic->name, "Arctic Hunt"),
          "s0 Arctic Hunt");
    CHECK(strike_item_by_skin(0) == arctic, "by skin");
    char slot[16];
    CHECK(strike_item_slot(arctic, slot, sizeof slot) && !strcmp(slot, "w0"), "weapon slot %s", slot);
    const StrikeItem *knife = strike_default_item(STRIKE_ITEM_MELEE, -1);
    CHECK(knife && strike_item_slot(knife, slot, sizeof slot) && !strcmp(slot, "t3"), "melee slot");
    const StrikeItem *hunter = strike_item("c1"), *shotgunner = strike_item("c4");
    CHECK(hunter && hunter->type == STRIKE_ITEM_CLASS && !strike_item_slot(hunter, slot, sizeof slot) && !strcmp(hunter->origin, "bundle"),
          "class item c1");
    CHECK(shotgunner && shotgunner->spinnable && !strcmp(shotgunner->origin, "spin_tickets"), "Shotgunner unlocks from spins");
    CHECK(e->starter_class_count == 1 && e->starter_classes[0] == 0, "Triggerman is the starter class");
    for (int i = 0; i < e->product_count; i++)
        CHECK(strcmp(e->products[i].price_type, "real_money") != 0, "product %s takes real money", e->products[i].id);
}

static void test_wheels(void) {
    static const struct { const char *id, *cur; int amount; double w[7]; } want[] = {
        {"starter", "kr", 50, {0.75, 0.22, 0.03}},
        {"elite", "kr", 100, {0.5, 0.3, 0.15, 0.05}},
        {"lucky_draw", "spin_ticket", 1, {0.5, 0.3, 0.15, 0.05}},
        {"golden_spin", "spin_golden_ticket", 1, {0.48, 0.35, 0.14, 0.0249, 0.005, 0.0001}},
        {"free", NULL, 0, {0.75, 0.22, 0.03}},
    };
    for (size_t k = 0; k < sizeof want / sizeof *want; k++) {
        const StrikeWheel *w = strike_wheel(want[k].id);
        CHECK(w != NULL, "wheel %s", want[k].id);
        if (!w) continue;
        CHECK(w->price_amount == want[k].amount && (want[k].cur ? w->price_currency && !strcmp(w->price_currency, want[k].cur) : !w->price_currency),
              "wheel %s price", w->id);
        double sum = 0.0;
        for (int s = 0; s < w->slice_count; s++) {
            sum += w->slices[s].weight;
            CHECK(fabs(w->slices[s].weight - want[k].w[s]) < 1e-9, "wheel %s slice %d weight %g", w->id, s, w->slices[s].weight);
        }
        CHECK(fabs(sum - 1.0) < 1e-6, "wheel %s weights sum %g", w->id, sum);
        // a million single spins: each slice within 0.25 percentage points (Heroic's 0.01%: within 0.005)
        enum { N = 1000000 };
        int hits[16] = {0};
        for (int r = 0; r < N; r++) {
            const double roll = rnd(), pick = rnd();
            int item, slice;
            CHECK(strike_wheel_spin(w, 1, &roll, &pick, &item, &slice) == 1, "spin");
            hits[slice]++;
            const StrikeItem *it = &strike_economy()->items[item];
            if (it->rarity != strike_pool(w->slices[slice].pool)->rarity) {
                CHECK(0, "wheel %s gave %s from pool %s", w->id, it->id, w->slices[slice].pool);
                break;
            }
        }
        for (int s = 0; s < w->slice_count; s++) {
            const double got = (double) hits[s] / N, tol = w->slices[s].weight < 0.001 ? 0.00005 : 0.0025;
            CHECK(fabs(got - w->slices[s].weight) < tol, "wheel %s slice %s: %.5f vs %.5f", w->id, w->slices[s].pool, got, w->slices[s].weight);
        }
    }
    // guaranteedIfSpinCountIs: 10x Lucky Draw always holds an Epic or better; a 1x spin does not force it
    const StrikeWheel *ld = strike_wheel("lucky_draw");
    int forced = 0;
    for (int t = 0; t < 20000; t++) {
        double rolls[10], picks[10];
        int items[10], slices[10], best = -1;
        for (int i = 0; i < 10; i++) rolls[i] = rnd() * 0.79, picks[i] = rnd(); // < 0.8: uncommon / rare only
        CHECK(strike_wheel_spin(ld, 10, rolls, picks, items, slices) == 10, "10x spin");
        for (int i = 0; i < 10; i++) best = strike_economy()->items[items[i]].rarity > best ? strike_economy()->items[items[i]].rarity : best;
        CHECK(best >= 2, "10x Lucky Draw without an Epic");
        forced += !strcmp(ld->slices[slices[9]].pool, "epic");
    }
    CHECK(forced == 20000, "the guarantee replaced the last roll %d times", forced);
    double r1 = 0.1, p1 = 0.5;
    int i1, s1;
    strike_wheel_spin(ld, 1, &r1, &p1, &i1, &s1);
    CHECK(strike_economy()->items[i1].rarity == 0, "1x spin keeps its roll");
}

static void test_levels(void) {
    const StrikeEconomy *e = strike_economy();
    CHECK(e->xp_base == 100 && e->xp_step == 100 && e->xp_divisor == 3.5, "xp curve");
    StrikeLevel l = strike_level_for_xp(0);
    CHECK(l.level == 1 && l.xp_in_level == 0 && l.xp_to_next == 100, "0 xp: level 1 0/100 (%d %lld/%lld)", l.level,
          (long long) l.xp_in_level, (long long) l.xp_to_next);
    l = strike_level_for_xp(99);
    CHECK(l.level == 1 && l.xp_in_level == 99, "99 xp: level 1");
    l = strike_level_for_xp(100);
    CHECK(l.level == 2 && l.xp_in_level == 0 && l.xp_to_next == 100, "100 xp: level 2");
    // levels 1-3 cost 100 each, level 4 (floor(4/3.5) = 1) 200, level 7 (floor(2)) 300
    CHECK(strike_xp_for_level(4) == 300 && strike_xp_for_level(5) == 500 && strike_xp_for_level(8) == 300 + 200 * 3 + 300,
          "xp_for_level %lld %lld %lld", (long long) strike_xp_for_level(4), (long long) strike_xp_for_level(5),
          (long long) strike_xp_for_level(8));
    for (int lv = 1; lv < 60; lv++) {
        const int64_t x = strike_xp_for_level(lv);
        CHECK(strike_level_for_xp(x).level == lv && strike_level_for_xp(x - 1).level == (lv > 1 ? lv - 1 : 1), "level %d boundary", lv);
    }
    int meter = 0;
    CHECK(strike_golden_meter_add(&meter, 9) == 0 && meter == 9, "meter 9");
    CHECK(strike_golden_meter_add(&meter, 1) == 1 && meter == 0, "meter full: a golden ticket");
    CHECK(strike_golden_meter_add(&meter, 25) == 2 && meter == 5, "meter 25: two tickets, 5 left");
    CHECK(e->track_count == e->max_level && strike_track_level(1)->player_choice && strike_track_slot("1_00") == strike_track_level(1),
          "track");
    const StrikeTrackSlot *t3 = strike_track_level(e->missions_unlock_level);
    CHECK(t3 && !strcmp(t3->rewards[0].type, "feature") && !strcmp(t3->rewards[0].feature, "missions"), "missions unlock on the track");
    CHECK(!strcmp(e->past_max_level_reward.currency, "spin_ticket") && e->past_max_level_reward.amount == 1, "past max level");
}

static void test_daily(void) {
    const StrikeEconomy *e = strike_economy();
    int a[8], b[8];
    const int n = strike_daily_missions(20000, 42, a, e->daily_mission_count);
    CHECK(n == 3 && strike_daily_missions(20000, 42, b, 3) == 3 && !memcmp(a, b, sizeof(int) * 3), "daily missions deterministic");
    CHECK(a[0] != a[1] && a[1] != a[2] && a[0] != a[2], "daily missions distinct");
    int differ = 0;
    for (int d = 0; d < 30; d++) {
        strike_daily_missions(20000 + d, 42, b, 3);
        differ += memcmp(a, b, sizeof(int) * 3) != 0;
    }
    CHECK(differ > 20, "missions change from day to day (%d/30)", differ);
    CHECK(strike_daily_deals(20000, a, 4) == 4 && strike_daily_deals(20000, b, 4) == 4 && !memcmp(a, b, sizeof(int) * 4), "deals");
    for (int i = 0; i < 4; i++) CHECK(e->items[a[i]].rarity >= 1 && e->items[a[i]].rarity <= 4, "deal rarity");
    CHECK(strike_day(86400000LL * 3 + 5) == 3, "strike_day");
}

int main(void) {
    test_catalogue();
    test_wheels();
    test_levels();
    test_daily();
    if (g_fail) {
        fprintf(stderr, "strike_economy_test: %d failure(s)\n", g_fail);
        return 1;
    }
    printf("strike_economy_test: OK (%d items, %d wheels)\n", strike_economy()->item_count, strike_economy()->wheel_count);
    return 0;
}
