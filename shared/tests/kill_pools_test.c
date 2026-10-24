// Event kill pools (shared/src/kill_pools.c) against Krunker v10.0.3's getKillReward
// (deob-10.0.3.js:220807-220924) and endGame's draws (:221797-221807), with a scripted Math.random.
#include <kill_pools.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;

#define CHECK(cond, ...) do { g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// Math.random: the scripted values in order, then 0
typedef struct {
    const double *values;
    int count, next;
} Script;

static double scripted(void *user) {
    Script *s = user;
    return s->next < s->count ? s->values[s->next++] : 0.0;
}

static int draw(const KillPoolEvents *events, const double *values, const int count, KillReward *out, int *used) {
    Script s = {values, count, 0};
    memset(out, 0, sizeof(*out));
    const int got = kill_pools_draw(events, scripted, &s, out);
    if (used) *used = s.next;
    return got;
}

static KillPoolEvents parse(const char *json) {
    KillPoolEvents events;
    cJSON *j = cJSON_Parse(json);
    kill_pools_parse(&events, j);
    cJSON_Delete(j);
    return events;
}

int main(void) {
    printf("kill pools...\n");
    KillReward r;
    int used;

    // no events, an event without pools or without weight: no draw, no random
    KillPoolEvents none = parse("[]");
    CHECK(!draw(&none, NULL, 0, &r, &used) && !kill_pools_any(&none), "no ongoing events: null");
    KillPoolEvents empty = parse("[{\"startsAt\":0,\"killPools\":[],\"killPoolsWeight\":100},"
                                 "{\"startsAt\":0,\"killPools\":[{\"weight\":100,\"contents\":[{\"content_type\":\"skin\",\"content_ref\":5}]}]}]");
    CHECK(empty.count == 2 && kill_pools_any(&empty), "an event with pools makes endGame draw");
    CHECK(!draw(&empty, (double[]) {0.0, 0.0, 0.0}, 3, &r, &used) && used == 0, "no pools / killPoolsWeight 0: filtered out, null");

    // one event at 100%, one pool at 100: always an item, no "nothing" roll (total 100), icon as r
    KillPoolEvents one = parse("[{\"id\":1,\"startsAt\":\"2026-10-24T18:00:00Z\",\"killPoolsWeight\":100,\"killPoolsIcon\":\"halloween_bag\","
                               "\"killPools\":[{\"weight\":100,\"contents\":[{\"content_type\":\"skin\",\"content_ref\":\"512\"},"
                               "{\"content_type\":\"skin\",\"content_ref\":77}]}]}]");
    CHECK(one.events[0].starts_at_ms == 1792864800000.0, "startsAt ISO -> epoch ms (%.0f)", one.events[0].starts_at_ms);
    CHECK(draw(&one, (double[]) {0.5, 0.2}, 2, &r, &used) && r.skin == 512 && !strcmp(r.icon, "halloween_bag") && used == 2,
          "total 100: randFloat(0, total) then randInt over the contents: skin \"512\" (parseInt), r = the icon (%d %s, %d randoms)", r.skin,
          r.icon, used);
    CHECK(draw(&one, (double[]) {0.5, 0.99}, 2, &r, NULL) && r.skin == 77, "the last content (%d)", r.skin);

    // 50%: the draw pays only when randFloat(0, 100) <= 50
    KillPoolEvents half = parse("[{\"startsAt\":0,\"killPoolsWeight\":50,"
                                "\"killPools\":[{\"weight\":100,\"contents\":[{\"content_type\":\"skin\",\"content_ref\":9}]}]}]");
    CHECK(!draw(&half, (double[]) {0.51, 0.0, 0.0}, 3, &r, &used) && used == 1, "killPoolsWeight 50: 51 > 50 is nothing");
    CHECK(draw(&half, (double[]) {0.3, 0.9, 0.0}, 3, &r, NULL) && r.skin == 9 && !r.icon[0], "30 <= 50: the item, no icon (no r)");

    // currency and bad refs draw nothing (v10 returns null for them)
    KillPoolEvents odd = parse("[{\"startsAt\":0,\"killPoolsWeight\":100,\"killPools\":[{\"weight\":100,\"contents\":["
                               "{\"content_type\":\"currency\",\"content_ref\":\"kr\"},{\"content_type\":\"skin\",\"content_ref\":\"abc\"},"
                               "{\"content_type\":\"skin\",\"content_ref\":-4}]}]}]");
    CHECK(!draw(&odd, (double[]) {0.5, 0.0}, 2, &r, NULL), "currency: null");
    CHECK(!draw(&odd, (double[]) {0.5, 0.4}, 2, &r, NULL), "content_ref NaN: null");
    CHECK(!draw(&odd, (double[]) {0.5, 0.9}, 2, &r, NULL), "content_ref < 0: null");

    // two events, given latest first: sorted by startsAt. A (100%, pools 60 / 40) then B (dilute, 50%, pool
    // 100): A's entries shrink to 30 / 20, B adds 50, total 100
    const char *two = "[{\"startsAt\":\"2026-10-25T00:00:00Z\",\"killPoolsWeight\":50,\"killPoolsMode\":\"%s\",\"killPoolsIcon\":\"b\","
                      "\"killPools\":[{\"weight\":100,\"contents\":[{\"content_type\":\"skin\",\"content_ref\":3}]}]},"
                      "{\"startsAt\":\"2026-10-20T00:00:00Z\",\"killPoolsWeight\":100,\"killPoolsIcon\":\"a\",\"killPools\":["
                      "{\"weight\":60,\"contents\":[{\"content_type\":\"skin\",\"content_ref\":1}]},"
                      "{\"weight\":40,\"contents\":[{\"content_type\":\"skin\",\"content_ref\":2}]}]}]";
    char json[1024];
    snprintf(json, sizeof(json), two, "dilute");
    KillPoolEvents dilute = parse(json);
    CHECK(draw(&dilute, (double[]) {0.25, 0.0}, 2, &r, &used) && r.skin == 1 && !strcmp(r.icon, "a") && used == 2, "dilute: 25 of 100 is A's first pool (30)");
    CHECK(draw(&dilute, (double[]) {0.45, 0.0}, 2, &r, NULL) && r.skin == 2, "dilute: 45 is A's second pool (30..50)");
    CHECK(draw(&dilute, (double[]) {0.51, 0.0}, 2, &r, NULL) && r.skin == 3 && !strcmp(r.icon, "b"), "dilute: 51 is B's pool, B's icon");
    // the same without dilute: 60 + 40 + 50 = 150 (no "nothing" roll over 100)
    snprintf(json, sizeof(json), two, "add");
    KillPoolEvents add = parse(json);
    CHECK(draw(&add, (double[]) {0.5, 0.0}, 2, &r, &used) && r.skin == 2 && used == 2, "add: 75 of 150 is A's second pool (60..100)");
    CHECK(draw(&add, (double[]) {0.7, 0.0}, 2, &r, NULL) && r.skin == 3, "add: 105 of 150 is B's");

    // endGame: one draw per kill, at most 30
    KillReward items[64];
    Script s = {(double[]) {0.0}, 0, 0};
    CHECK(kill_pools_draw_kills(&one, 7, scripted, &s, items, 64) == 7, "7 kills: 7 draws");
    s.next = 0;
    CHECK(kill_pools_draw_kills(&one, 45, scripted, &s, items, 64) == KILL_POOL_DRAWS_MAX, "45 kills: 30 draws (Math.min(kills, 30))");
    CHECK(kill_pools_draw_kills(&none, 45, scripted, &s, items, 64) == 0, "no kill pools: no draws");
    // the default random (pcg32): half of the 50% draws pay, roughly
    int paid = 0;
    for (int i = 0; i < 1000; i++) paid += kill_pools_draw(&half, NULL, NULL, &r);
    CHECK(paid > 400 && paid < 600, "killPoolsWeight 50 pays about half the draws (%d of 1000)", paid);

    // the icon becomes ./img/<icon>.webp: a path or markup is dropped
    KillPoolEvents bad_icon = parse("[{\"startsAt\":0,\"killPoolsWeight\":100,\"killPoolsIcon\":\"../x\\\"><b>\",\"killPools\":[]}]");
    CHECK(!bad_icon.events[0].icon[0], "an icon with a path or markup is dropped");

    // updateEventIndex's search (deob-10.0.3.js:219733-219751): the first event running now, its id; 0 none
    KillPoolEvents evs = parse("[{\"id\":4,\"startsAt\":2000,\"endsAt\":3000},{\"id\":7,\"startsAt\":1000,\"endsAt\":5000},"
                               "{\"id\":9,\"startsAt\":1000}]");
    CHECK(evs.events[0].id == 4 && evs.events[1].ends_at_ms == 5000.0 && evs.events[2].ends_at_ms == 0.0, "id / endsAt parsed");
    CHECK(kill_pools_event_index(&evs, 1500.0) == 7, "1500: event 7 (4 not started)");
    CHECK(kill_pools_event_index(&evs, 2500.0) == 4, "2500: event 4, the first in list order");
    CHECK(kill_pools_event_index(&evs, 6000.0) == 9, "6000: 9, open-ended");
    CHECK(kill_pools_event_index(&evs, 500.0) == 0 && kill_pools_event_index(&none, 2500.0) == 0, "none running / no events: 0");

    if (g_failures) {
        printf("kill_pools_test: %d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("kill_pools_test: %d checks OK\n", g_checks);
    return 0;
}
