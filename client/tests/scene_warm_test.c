// The match-start warm's stop rule (client/include/scene_warm_rule.h, v10.0.4 warmScene deob-10.0.4.js:204997-205054):
// the warm frames last until SCENE_WARM_STABLE frames in a row added no mesh, or SCENE_WARM_MAX_MS.
// `scene_warm_test off`: with KRUNKNATIVE_SCENE_WARM=0 nothing warms.
#include <scene_warm_rule.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_checks, g_fails;
#define CHECK(cond, ...)                                                                   \
    do {                                                                                   \
        g_checks++;                                                                        \
        if (!(cond)) {                                                                     \
            g_fails++;                                                                     \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);                           \
            fprintf(stderr, __VA_ARGS__);                                                  \
            fputc('\n', stderr);                                                           \
        }                                                                                  \
    } while (0)

// frames at 60 Hz with this mesh count each; returns how many returned 1 (warm frames) before the first 0
static int warm_frames(const size_t *counts, const int n, double *now) {
    int warm = 0;
    for (int i = 0; i < n; i++) {
        *now += 16.0;
        if (!scene_warm_frame_count(counts[i], *now)) break;
        warm++;
    }
    return warm;
}

static void test_stable(void) {
    double now = 1000.0;
    scene_warm_begin_count(10, now);
    CHECK(scene_warm_active(), "a map load starts the warm");
    const size_t counts[] = {10, 10, 10, 10, 10, 10};
    CHECK(warm_frames(counts, 6, &now) == SCENE_WARM_STABLE, "a stable scene warms %d frames", SCENE_WARM_STABLE);
    CHECK(!scene_warm_active(), "then the warm is over (the card may go)");
    CHECK(!scene_warm_frame_count(10, now + 16.0), "and later frames draw culled");
}

static void test_growing(void) {
    double now = 0.0;
    scene_warm_begin_count(10, now);
    // a model arrives on the 2nd and the 4th frame: the count of stable frames starts again after each
    const size_t counts[] = {10, 12, 12, 15, 15, 15, 15, 15, 15};
    const int warm = warm_frames(counts, 9, &now);
    CHECK(warm == 4 + SCENE_WARM_STABLE, "warm until %d frames after the last new mesh (%d)", SCENE_WARM_STABLE, warm);
    CHECK(!scene_warm_active(), "over once stable");
}

static void test_timeout(void) {
    double now = 0.0;
    scene_warm_begin_count(0, now);
    int warm = 0;
    for (size_t i = 1; i < 1000 && scene_warm_frame_count(i, now += 16.0); i++) warm++; // a mesh every frame
    CHECK(!scene_warm_active(), "a scene that never settles still ends");
    CHECK(now > SCENE_WARM_MAX_MS && now <= SCENE_WARM_MAX_MS + 16.0, "at SCENE_WARM_MAX_MS (%.0f ms)", now);
    CHECK(warm == (int) (SCENE_WARM_MAX_MS / 16.0), "every frame until then warms (%d)", warm);
}

static void test_reload(void) {
    double now = 0.0;
    scene_warm_begin_count(5, now);
    scene_warm_frame_count(5, now += 16.0);
    scene_warm_begin_count(40, now); // a newer map load restarts it
    const size_t counts[] = {40, 40, 40, 40};
    CHECK(warm_frames(counts, 4, &now) == SCENE_WARM_STABLE, "a newer load warms afresh");
}

static void test_off(void) {
    setenv("KRUNKNATIVE_SCENE_WARM", "0", 1);
    CHECK(!scene_warm_enabled(), "KRUNKNATIVE_SCENE_WARM=0 turns it off");
    scene_warm_begin_count(10, 0.0);
    CHECK(!scene_warm_active(), "no warm after a load");
    CHECK(!scene_warm_frame_count(10, 16.0), "every frame draws culled");
}

int main(int argc, char **argv) {
    if (argc > 1 && !strcmp(argv[1], "off")) {
        test_off();
    } else {
        unsetenv("KRUNKNATIVE_SCENE_WARM");
        CHECK(scene_warm_enabled(), "on by default");
        CHECK(!scene_warm_frame_count(3, 16.0), "nothing warms before a load");
        test_stable();
        test_growing();
        test_timeout();
        test_reload();
    }
    printf("scene_warm_test: %d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
