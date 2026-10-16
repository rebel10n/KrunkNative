// The game time total (client/src/ui/game_time.c) without a window: v7.1.4's readable time
// (UTILS.getReadableTime(ms, true)), the local count (only while a game is on screen), the stored
// value (loaded once, written every GAME_TIME_FLUSH_MS of play and when a game ends), and that nothing
// depends on an account.
//   cmake-build-debug/game_time_test
#include <game_time.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_fail, g_pass;
#define CHECK(cond, ...) do { if (cond) g_pass++; else { g_fail++; printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); } } while (0)

static void readable(const double ms, const char *want) {
    char out[64];
    game_time_readable(out, sizeof(out), ms);
    CHECK(!strcmp(out, want), "readable(%.0f) = '%s', want '%s'", ms, out, want);
}

static void format_tests(void) {
    readable(0, "0m 0s");
    readable(5000, "0m 5s");
    readable(59999, "0m 59s");
    readable(60000, "1m 0s");
    readable(8073900, "2h 14m 33s");   // the example in the request
    readable(3600000, "1h 0m 0s");
    readable(86400000, "1d 0m 0s");    // a day: no hours part
    readable(90061000, "1d 1h 1m 1s");
    readable(365.0 * 86400000.0 + 3 * 86400000.0 + 4 * 3600000.0 + 5 * 60000.0 + 6000.0, "1y 3d 4h 5m 6s");
    readable(-5000, "0m 0s");          // never negative
}

// the HUD clock in ms; one frame of `dt` ms
static double g_now;
static int frame(GameTime *t, const int session, const int counting, const double dt) {
    g_now += dt;
    return game_time_step(t, session, counting, g_now);
}

static void store_tests(void) {
    GameTime t;
    game_time_load(&t, NULL);
    CHECK(game_time_ms(&t) == 0.0, "no stored value: 0");
    game_time_load(&t, "8073900");
    CHECK(game_time_ms(&t) == 8073900.0, "stored 8073900 loads as 8073900 ms (got %.0f)", game_time_ms(&t));
    game_time_load(&t, "junk");
    CHECK(game_time_ms(&t) == 0.0, "junk loads as 0");
    game_time_load(&t, "-5");
    CHECK(game_time_ms(&t) == 0.0, "negative loads as 0");
    game_time_load(&t, "1234.9");
    CHECK(game_time_ms(&t) == 1234.0, "fractions are dropped on load");

    char buf[32];
    game_time_load(&t, "8073900");
    game_time_store(&t, buf, sizeof(buf));
    CHECK(!strcmp(buf, "8073900") && game_time_ms(&t) == 8073900.0, "store writes the whole total: '%s'", buf);
}

static void count_tests(void) {
    GameTime t;
    game_time_load(&t, "8073900");
    g_now = 0.0;

    // the game is not up: nothing counts, nothing to store
    CHECK(frame(&t, 0, 0, 16.667) == 0 && game_time_ms(&t) == 8073900.0, "no game: nothing counts");

    // a game starts (the first frame counts nothing): 2 s of play on screen
    CHECK(frame(&t, 1, 1, 16.667) == 0, "a game starts: no store");
    for (int i = 0; i < 120; i++) frame(&t, 1, 1, 16.667);
    CHECK(fabs(game_time_ms(&t) - (8073900.0 + 120 * 16.667)) < 0.01, "counts the play: %.1f ms", game_time_ms(&t));

    // the ESC menu or the end screen: the game is on but nothing counts
    for (int i = 0; i < 180; i++) frame(&t, 1, 0, 16.667);
    CHECK(fabs(game_time_ms(&t) - (8073900.0 + 120 * 16.667)) < 0.01, "paused over the menu / end screen: %.1f ms", game_time_ms(&t));

    // the game ends with play to keep: stored now
    CHECK(frame(&t, 0, 0, 16.667) == 1, "the game ends: store the play");
    char buf[32];
    game_time_store(&t, buf, sizeof(buf));
    CHECK(atof(buf) >= 8073900.0 + 120 * 16.667 - 1.0 && atof(buf) <= 8073900.0 + 120 * 16.667 + 1.0, "stored at the end: %s", buf);
    CHECK(frame(&t, 0, 0, 16.667) == 0, "already stored: nothing to store");

    // the store every GAME_TIME_FLUSH_MS of play while a game runs
    game_time_load(&t, "0");
    frame(&t, 1, 1, 16.667);
    int stores = 0;
    for (int i = 0; i < 1000; i++) {
        if (frame(&t, 1, 1, 16.667)) { // the caller stores the total when asked
            game_time_store(&t, buf, sizeof(buf));
            stores++;
        }
    }
    CHECK(stores == 1, "one store per %.0f s of play: %d in 16.7 s", GAME_TIME_FLUSH_MS / 1000.0, stores);
    CHECK(fabs(game_time_ms(&t) - 1000 * 16.667) < 1.0, "the total is the play, stored or pending (whole ms per store): %.1f", game_time_ms(&t));

    // a long stall between frames of a game is counted (the play went on)
    game_time_load(&t, "0");
    frame(&t, 1, 1, 16.667);
    frame(&t, 1, 1, 4000.0);
    CHECK(fabs(game_time_ms(&t) - 4000.0) < 0.01, "a stall counts: %.1f ms", game_time_ms(&t));

    // a new game after one: no time counted across the gap between them
    frame(&t, 0, 0, 16.667);
    frame(&t, 1, 1, 60000.0);
    CHECK(fabs(game_time_ms(&t) - 4000.0) < 0.01, "a gap between games is not counted: %.1f ms", game_time_ms(&t));
}

int main(void) {
    format_tests();
    store_tests();
    count_tests();
    printf("game_time_test: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
