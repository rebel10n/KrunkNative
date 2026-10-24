// Krunker v10.0.3 live events (shared/src/live_events.c) against config.events' readers in deob-10.0.3.js:
// updateEventIndex (:219733-219751), UTILS.hasActiveEvents / getActiveEvent (:28747-28775), the start handler's
// toggleMeshGroup("event_0") (:256309-256313), getItemByWeights (:220754-220806) and endGame's event reward
// (:221732-221752), with a scripted Math.random. genMatchList's event bucket (:222421-222463) is config_test's.
#include <live_events.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

typedef struct {
    const double *values;
    int count, next;
} Script;

static double scripted(void *user) {
    Script *s = user;
    return s->next < s->count ? s->values[s->next++] : 0.0;
}

static LiveEvents parse(const char *json) {
    LiveEvents events;
    cJSON *j = cJSON_Parse(json);
    live_events_parse(&events, j);
    cJSON_Delete(j);
    return events;
}

// a store of 10 skins: 0..3 rarity 2, 4..5 rarity 3, 6 rarity 5; 2, 4 and 6 are event 8's of season 10, 3 of season 9
static LiveEventSkin g_store[10];
static void store_init(void) {
    for (int i = 0; i < 10; i++) g_store[i] = (LiveEventSkin) {i, i < 4 ? 2 : i < 6 ? 3 : i == 6 ? 5 : 1, -1, -1};
    g_store[2].event_reward = g_store[4].event_reward = g_store[6].event_reward = 8;
    g_store[2].season = g_store[4].season = g_store[6].season = 10;
    g_store[3].event_reward = 8;
    g_store[3].season = 9;
}

static int g_owned[10];
static int owns(void *user, const int skin) {
    (void) user;
    return skin >= 0 && skin < 10 && g_owned[skin];
}

static int store_rarity(void *user, const int skin) {
    (void) user;
    return skin >= 0 && skin < 10 ? g_store[skin].rarity : -1;
}

static int reward(const LiveEvent *e, const int index, const int map, const double *rolls, const int n, int *used) {
    Script s = {rolls, n, 0};
    const int skin = live_events_reward(e, index, map, 10, g_store, 10, owns, NULL, scripted, &s);
    if (used) *used = s.next;
    return skin;
}

static void test_parse(void) {
    LiveEvents evs = parse("[{\"id\":8,\"name\":\"Halloween\",\"startsAt\":\"2026-10-25T00:00:00Z\",\"endsAt\":\"2026-11-02T00:00:00Z\","
                           "\"reveal\":86400000,\"imgExt\":\"webp\",\"longDesc\":[\"Hunt\",\"Collect\"],\"noChallenges\":1,"
                           "\"rewards\":{\"skins\":[7540,\"7541\",-3],\"weights\":[0,0,50,50]},\"rewardChance\":25,"
                           "\"eligibleMaps\":[0,3],\"limitedTimeMode\":true,\"eventObjects\":true},"
                           "{\"id\":4,\"name\":\"<b>x\",\"start\":1000,\"end\":2000,\"imgExt\":\"../a\",\"longDesc\":\"one\\ntwo\"},"
                           "{\"id\":\"x\"}, 7]");
    CHECK(evs.count == 3, "three objects kept, the number dropped: %d", evs.count);
    const LiveEvent *h = &evs.events[0];
    CHECK(h->id == 8 && !strcmp(h->name, "Halloween") && h->start_ms == 1792886400000.0 && h->end_ms == 1793577600000.0,
          "id / name / startsAt / endsAt: %d %s %.0f %.0f", h->id, h->name, h->start_ms, h->end_ms);
    CHECK(h->reveal_ms == 86400000.0 && !strcmp(h->img_ext, "webp") && h->long_desc_list && !strcmp(h->long_desc, "Hunt\nCollect"),
          "reveal, imgExt, longDesc list: %.0f %s '%s'", h->reveal_ms, h->img_ext, h->long_desc);
    CHECK(h->no_challenges && h->limited_time_mode && h->event_objects, "noChallenges / limitedTimeMode / eventObjects");
    CHECK(h->has_reward_skins && h->reward_skin_count == 3 && h->reward_skins[0] == 7540 && h->reward_skins[1] == 7541 && h->reward_skins[2] == -3,
          "rewards.skins as given (parseInt, a bad one kept for the empty tile)");
    CHECK(h->has_weights && h->reward_weights[2] == 50.0 && h->reward_weights[3] == 50.0 && h->reward_weights[4] == 0.0, "rewards.weights");
    CHECK(h->reward_chance == 25 && h->has_eligible_maps && h->eligible_map_count == 2 && h->eligible_maps[1] == 3, "rewardChance, eligibleMaps");
    const LiveEvent *a = &evs.events[1];
    CHECK(a->start_ms == 1000.0 && a->end_ms == 2000.0, "v10's start / end names are read too");
    CHECK(!a->img_ext[0] && !a->long_desc_list && !strcmp(a->long_desc, "one two"), "a path imgExt is dropped, newlines of a text become spaces");
    CHECK(a->reward_chance == 100 && !a->has_reward_skins && !a->has_eligible_maps && !a->event_objects, "rewardChance || 100, no lists");
    CHECK(evs.events[2].id == 0, "a non-numeric id is 0");
    char path[64];
    live_event_image(h, 8, path, sizeof(path));
    CHECK(!strcmp(path, "img/events/event_8.webp"), "img/events/event_<id>.<imgExt>: %s", path);
    live_event_image(a, 4, path, sizeof(path));
    CHECK(!strcmp(path, "img/events/event_4.png"), "imgExt || png: %s", path);
    live_event_image(NULL, 3, path, sizeof(path));
    CHECK(!strcmp(path, "img/events/event_3.png"), "an unknown event: event_<id>.png: %s", path);
}

static void test_index(void) {
    LiveEvents evs = parse("[{\"id\":4,\"startsAt\":2000,\"endsAt\":3000},{\"id\":7,\"startsAt\":1000,\"endsAt\":5000,\"reveal\":0},"
                           "{\"id\":9,\"startsAt\":10000,\"endsAt\":20000,\"reveal\":4000},{\"id\":11,\"startsAt\":1000}]");
    CHECK(live_events_index(&evs, 1500.0) == 7, "1500: event 7 (4 not started)");
    CHECK(live_events_index(&evs, 2500.0) == 4, "2500: event 4, the first in list order");
    CHECK(live_events_index(&evs, 5000.0) == 7, "5000: now <= end, still 7");
    CHECK(live_events_index(&evs, 7000.0) == 11, "7000: 11, open-ended");
    CHECK(live_events_index(&evs, 500.0) == 0, "500: none");
    CHECK(live_events_any_running(&evs, 2500.0) && !live_events_any_running(&evs, 500.0), "hasActiveEvents");
    // getActiveEvent: by id at any time, else a revealed one
    CHECK(live_events_active(&evs, 4, 100000.0) == &evs.events[1 - 1], "getActiveEvent(4): found by id whatever the time");
    CHECK(live_events_active(&evs, 0, 5000.0) == NULL, "5000: 9 starts in 5000 > reveal 4000: none");
    CHECK(live_events_active(&evs, 0, 6000.0) == &evs.events[2], "6000: 9 starts within its reveal: shown");
    CHECK(live_events_active(&evs, 0, 15000.0) == &evs.events[2], "15000: 9 runs: still the revealed one");
    CHECK(live_events_active(&evs, 0, 20000.0) == NULL, "20000: 9 has ended (end - now > 0 fails)");
    CHECK(live_events_active(&evs, 3, 6000.0) == &evs.events[2], "an unknown id falls back to the revealed event");
    CHECK(live_events_limited_time_mode(&evs, 4) == 0, "limitedTimeMode unset");
}

static void test_objects(void) {
    LiveEvents evs = parse("[{\"id\":5,\"eventObjects\":true},{\"id\":6}]");
    CHECK(live_events_show_objects(&evs, 3, 0), "event 3 shows the EVENT prefab (v10 eventIndex == 3)");
    CHECK(!live_events_show_objects(&evs, 3, 1), "event 3 while spectating: hidden (toggleMeshGroup(event_0, !spectating))");
    CHECK(live_events_show_objects(&evs, 5, 0), "KN eventObjects: event 5 shows it too");
    CHECK(!live_events_show_objects(&evs, 6, 0) && !live_events_show_objects(&evs, 0, 0) && !live_events_show_objects(NULL, 7, 0),
          "another event, none, an unknown one: hidden");
}

static void test_weights(void) {
    store_init();
    const int items[] = {0, 1, 4, 6};
    // rarities present: 2, 3, 5. lootRars: 70.395 + 25.49 + 0.105 = 95.99; roll = r * (95.99 - 0.0001) + 0.0001
    const double low[] = {0.5, 0.75}; // 47.99: rarity 2, then randInt(0, 1) = floor(0.75 * 2) = 1 -> skin 1
    Script s = {low, 2, 0};
    CHECK(live_events_pick_by_weights(items, 4, NULL, store_rarity, NULL, scripted, &s) == 1, "lootRars: rarity 2, the second of it");
    const double mid[] = {0.8, 0.0}; // 76.79: past 70.395, under 95.885: rarity 3 -> skin 4
    s = (Script) {mid, 2, 0};
    CHECK(live_events_pick_by_weights(items, 4, NULL, store_rarity, NULL, scripted, &s) == 4, "lootRars: rarity 3");
    const double top[] = {0.99995, 0.0}; // 95.985: past 95.885: rarity 5 -> skin 6
    s = (Script) {top, 2, 0};
    CHECK(live_events_pick_by_weights(items, 4, NULL, store_rarity, NULL, scripted, &s) == 6, "lootRars: rarity 5");
    const double w[LIVE_EVENT_RARITIES] = {0, 0, 0, 0, 0, 0, 0};
    s = (Script) {low, 2, 0};
    CHECK(live_events_pick_by_weights(items, 4, w, store_rarity, NULL, scripted, &s) == -1, "all weights 0: -1");
    CHECK(live_events_pick_by_weights(items, 0, NULL, store_rarity, NULL, scripted, &s) == -1, "no items: -1");
}

static void test_reward(void) {
    store_init();
    LiveEvents evs = parse("[{\"id\":8,\"rewardChance\":50,\"eligibleMaps\":[2]},{\"id\":9,\"rewards\":{\"skins\":[7,2,99],\"weights\":[0,1,1,1,1,1,1]}}]");
    const LiveEvent *e8 = &evs.events[0], *e9 = &evs.events[1];
    int used = 0;
    const double r1[] = {0.0, 0.0, 0.0}; // randInt(1, 100) = 1 <= 50, rarity pick 0.0001: rarity 2 -> skin 2
    CHECK(reward(e8, 8, 2, r1, 3, &used) == 2 && used == 3, "event 8 on map 2: eventReward 8 of season 10, rarity 2 first (used %d)", used);
    const double miss[] = {0.5, 0.0, 0.0}; // randInt = 51 > 50: nothing
    CHECK(reward(e8, 8, 2, miss, 3, &used) == -1 && used == 1, "rewardChance 50: a roll of 51 pays nothing");
    CHECK(reward(e8, 8, 0, r1, 3, &used) == -1 && used == 0, "map 0 is not eligible: no candidates, no roll");
    g_owned[2] = 1;
    const double r2[] = {0.0, 0.0, 0.0};
    const int got = reward(e8, 8, 2, r2, 3, &used);
    CHECK(got == 4, "skin 2 owned: rarity 3 / 5 left, the first draw rarity 3 -> 4 (%d)", got);
    g_owned[4] = g_owned[6] = 1;
    CHECK(reward(e8, 8, 2, r2, 3, &used) == -1, "every candidate owned: nothing");
    memset(g_owned, 0, sizeof(g_owned));
    CHECK(reward(e8, 9, 2, r1, 3, &used) == -1, "the event's id must be the eventIndex's store tag: event 9 has no store skins");
    // KN: rewards.skins are candidates (skin 7 rarity 1, skin 2 rarity 2; 99 not in the store), each once
    const double r3[] = {0.0, 0.0, 0.0}; // weights 1 each over rarities 1, 2: roll 0.0001 -> rarity 1 -> skin 7
    CHECK(reward(e9, 9, 5, r3, 3, &used) == 7, "event 9's rewards.skins pay with its weights (no eligibleMaps: any map)");
    CHECK(reward(NULL, 8, 2, r1, 3, &used) == -1 && reward(e8, 0, 2, r1, 3, &used) == -1, "no event, no eventIndex: nothing");
}

int main(void) {
    test_parse();
    test_index();
    test_objects();
    test_weights();
    test_reward();
    if (g_failures) {
        printf("live_events_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("live_events_test: %d checks OK\n", g_checks);
    return 0;
}
