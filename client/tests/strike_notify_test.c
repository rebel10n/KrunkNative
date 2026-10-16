// Krunker Strike's notification queue (client/src/ui/strike_notify_queue.c, FRVR 1.37.2's OI / Ag):
// three shown at most, the rest queued in order, a dismissal promotes the first queued one with a timer
// of its own, the 15 s auto dismiss, and a full queue drops the newest. Run: ./<build>/strike_notify_test
#include <strike_notify.h>
#include <stdio.h>
#include <string.h>

static int g_failures, g_checks;
#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// the keys on screen, in order
static void shown_keys(const StrikeNotifyQueue *q, char *out, size_t n) {
    out[0] = 0;
    for (int i = 0; i < q->shown_count; i++) {
        char part[16];
        snprintf(part, sizeof(part), "%s%d", i ? "," : "", q->shown[i].key);
        strncat(out, part, n - strlen(out) - 1);
    }
}

int main(void) {
    StrikeNotifyQueue q;
    char keys[64];

    // 1. four pushes: three on screen, the fourth queued (OI: da.length < 3, else GC.push)
    strike_notify_queue_init(&q);
    int k1 = strike_notify_queue_push(&q, STRIKE_NOTIFY_INVITE, "Ada", "g1", 0.0);
    int k2 = strike_notify_queue_push(&q, STRIKE_NOTIFY_INVITE, "Bo", "g2", 0.0);
    int k3 = strike_notify_queue_push(&q, STRIKE_NOTIFY_INVITE, "Cy", "g3", 0.0);
    int k4 = strike_notify_queue_push(&q, STRIKE_NOTIFY_INVITE, "Dee", "g4", 0.0);
    CHECK(k1 == 1 && k2 == 2 && k3 == 3 && k4 == 4, "keys %d %d %d %d", k1, k2, k3, k4);
    CHECK(q.shown_count == 3 && q.queued_count == 1, "shown %d queued %d", q.shown_count, q.queued_count);
    shown_keys(&q, keys, sizeof(keys));
    CHECK(strcmp(keys, "1,2,3") == 0, "on screen %s", keys);
    CHECK(strcmp(q.shown[0].name, "Ada") == 0 && strcmp(q.shown[0].game_id, "g1") == 0, "fields %s %s", q.shown[0].name, q.shown[0].game_id);

    // 2. a dismissal (the close button of 2) shows the queued one at the end, its timer starts now
    unsigned before = q.version;
    CHECK(strike_notify_queue_dismiss(&q, 2, 1.0) == 1, "dismiss 2");
    shown_keys(&q, keys, sizeof(keys));
    CHECK(strcmp(keys, "1,3,4") == 0, "after the close: %s (the order of the rest, then the queued one)", keys);
    CHECK(q.queued_count == 0, "queue drained (%d left)", q.queued_count);
    CHECK(q.version != before, "a change bumps the version");
    CHECK(q.shown[2].expires_s == 16.0, "promoted timer starts at the dismissal (%.2f)", q.shown[2].expires_s);
    CHECK(strike_notify_queue_dismiss(&q, 2, 1.0) == 0, "a notification already gone is not dismissed again");
    CHECK(strike_notify_queue_dismiss(&q, 99, 1.0) == 0, "an unknown key is not dismissed");

    // 3. the timers: 1 and 3 were shown at 0 (end 15), 4 at 1 (end 16)
    strike_notify_queue_tick(&q, 14.9);
    CHECK(q.shown_count == 3, "nothing runs out before 15 s (%d shown)", q.shown_count);
    strike_notify_queue_tick(&q, 15.0);
    shown_keys(&q, keys, sizeof(keys));
    CHECK(strcmp(keys, "4") == 0, "at 15 s the two shown at 0 leave (%s left)", keys);
    strike_notify_queue_tick(&q, 16.0);
    CHECK(q.shown_count == 0, "and 4 at 16 s (%d left)", q.shown_count);

    // 4. a queued one that comes up on a timer gets a timer too
    strike_notify_queue_init(&q);
    for (int i = 0; i < 4; i++) strike_notify_queue_push(&q, STRIKE_NOTIFY_INVITE, "P", "g", 10.0 * i);
    // shown: 1 at 0 (end 15), 2 at 10 (25), 3 at 20 (35); queued: 4 (pushed at 30, shown from the tick at 15)
    strike_notify_queue_tick(&q, 15.0);
    shown_keys(&q, keys, sizeof(keys));
    CHECK(strcmp(keys, "2,3,4") == 0, "the tick at 15 s shows the queued 4 (%s)", keys);
    CHECK(q.shown[2].expires_s == 30.0, "4's timer runs from 15 s (%.2f)", q.shown[2].expires_s);

    // 5. a full queue drops the newest: three shown plus sixteen queued
    strike_notify_queue_init(&q);
    int accepted = 0;
    for (int i = 0; i < 3 + STRIKE_NOTIFY_QUEUE_MAX + 5; i++) accepted += strike_notify_queue_push(&q, STRIKE_NOTIFY_INVITE, "F", "g", 0.0) > 0;
    CHECK(accepted == 3 + STRIKE_NOTIFY_QUEUE_MAX, "accepted %d", accepted);
    CHECK(q.queued_count == STRIKE_NOTIFY_QUEUE_MAX, "queued %d", q.queued_count);

    // 6. the default name when the friend has none (N5: "Player Name ")
    strike_notify_queue_init(&q);
    strike_notify_queue_push(&q, STRIKE_NOTIFY_INVITE, NULL, NULL, 0.0);
    CHECK(strcmp(q.shown[0].name, "Player Name ") == 0, "name %s", q.shown[0].name);

    printf("%s: %d checks, %d failed\n", g_failures ? "FAILED" : "PASS", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
