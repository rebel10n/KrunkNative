// window_state_test: client/src/window_state.c (window_state.h), the window state F4 / the
// Settings relaunch hand to the new process and the one kept for a normal restart: the text
// round trip, malformed input, the startup precedence and the apply order.
#include <window_state.h>

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

enum { WINDOWED = 0, FULLSCREEN = 1, BORDERLESS = 2 }; // RS_* (render_settings.h)

static int same(const WindowState *a, const WindowState *b) {
    return a->mode == b->mode && a->x == b->x && a->y == b->y && a->w == b->w && a->h == b->h &&
           a->maximized == b->maximized && a->forced == b->forced;
}

static void test_round_trip(void) {
    const WindowState states[] = {
        {WINDOWED, 200, 150, 1000, 600, 0, 0},
        {WINDOWED, -1920, 24, 1280, 720, 1, 0}, // maximized on a monitor left of the primary
        {FULLSCREEN, 5, 29, 1280, 720, 0, 1},   // forced: --fullscreen (or F11 in a forced session)
        {BORDERLESS, 0, 0, 1920, 1080, 1, 0},
    };
    for (size_t i = 0; i < sizeof(states) / sizeof(states[0]); i++) {
        char text[WINDOW_STATE_TEXT_MAX];
        WindowState back;
        CHECK(window_state_encode(&states[i], text, sizeof(text)), "encode %zu", i);
        CHECK(window_state_decode(text, &back) && same(&states[i], &back), "round trip %zu: %s", i, text);
    }
    char text[WINDOW_STATE_TEXT_MAX];
    window_state_encode(&states[2], text, sizeof(text));
    CHECK(!strcmp(text, "1,5,29,1280,720,0,1"), "format: %s", text);
    char tiny[8];
    CHECK(!window_state_encode(&states[0], tiny, sizeof(tiny)) && tiny[0] == 0, "encode: too small");
}

static void test_decode(void) {
    WindowState s = {9, 9, 9, 9, 9, 9, 9};
    const WindowState untouched = s;
    CHECK(window_state_decode("2,10,20,800,600,1", &s), "six fields (no forced)");
    CHECK(s.mode == BORDERLESS && s.x == 10 && s.y == 20 && s.w == 800 && s.h == 600 && s.maximized == 1 && s.forced == 0,
          "six fields values");
    const char *bad[] = {
        "", "1", "1,2,3,4,5", "3,0,0,800,600,0", "-1,0,0,800,600,0", "0,0,0,10,600,0", "0,0,0,800,0,0",
        "0,0,0,800,600,2", "0,0,0,800,600,0,5", "0,0,0,800,600,0,0,0", "0,0,0,800,600,0x", "a,b,c,d,e,f",
        "0,99999,0,800,600,0", "0,0,0,800,99999,0",
    };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        s = untouched;
        CHECK(!window_state_decode(bad[i], &s) && same(&s, &untouched), "malformed accepted: \"%s\"", bad[i]);
    }
    CHECK(!window_state_decode(NULL, &s), "NULL text");
}

static void test_resolve(void) {
    WindowStartup out;
    const WindowState restore = {WINDOWED, 300, 200, 1000, 600, 1, 1};
    const WindowState persisted = {FULLSCREEN, 40, 50, 1600, 900, 1, 0};

    // nothing: windowed 1280x720 where the window manager puts it
    window_state_resolve(&(WindowStartupIn) {.cli_mode = -1, .stored_mode = -1}, &out);
    CHECK(out.state.mode == WINDOWED && out.state.w == 1280 && out.state.h == 720 && !out.has_pos && !out.restored &&
          !out.state.forced && !out.state.maximized, "defaults");

    // F4 after F11 in a --fullscreen session: the restored (windowed, forced) state wins over
    // the --fullscreen still in argv, the stored mode, KRUNKNATIVE_WINDOW_SIZE and the kept rect
    window_state_resolve(&(WindowStartupIn) {.restore = &restore, .cli_mode = FULLSCREEN, .stored_mode = BORDERLESS,
                                             .env_w = 640, .env_h = 480, .persisted = &persisted}, &out);
    CHECK(same(&out.state, &restore) && out.has_pos && out.restored, "restore wins");

    // CLI flag over the stored mode; forced
    window_state_resolve(&(WindowStartupIn) {.cli_mode = BORDERLESS, .stored_mode = FULLSCREEN}, &out);
    CHECK(out.state.mode == BORDERLESS && out.state.forced, "cli over stored");

    // stored mode + kept rect: the kept rect, its maximized flag, never its mode
    window_state_resolve(&(WindowStartupIn) {.cli_mode = -1, .stored_mode = WINDOWED, .persisted = &persisted}, &out);
    CHECK(out.state.mode == WINDOWED && !out.state.forced && out.state.x == 40 && out.state.y == 50 && out.state.w == 1600 &&
          out.state.h == 900 && out.state.maximized && out.has_pos && !out.restored, "persisted rect, stored mode");

    // KRUNKNATIVE_WINDOW_SIZE (tests, page windows) over the kept rect: no position, not maximized
    window_state_resolve(&(WindowStartupIn) {.cli_mode = -1, .stored_mode = FULLSCREEN, .env_w = 1100, .env_h = 760,
                                             .persisted = &persisted}, &out);
    CHECK(out.state.mode == FULLSCREEN && out.state.w == 1100 && out.state.h == 760 && !out.has_pos && !out.state.maximized,
          "env size over persisted");

    // an out-of-range stored mode is ignored
    window_state_resolve(&(WindowStartupIn) {.cli_mode = -1, .stored_mode = 7}, &out);
    CHECK(out.state.mode == WINDOWED, "bad stored mode");
}

static int steps_are(const WindowStartup *s, const WindowStep *want, const int n) {
    WindowStep got[WINDOW_STEPS_MAX];
    const int count = window_state_steps(s, got);
    if (count != n) return 0;
    for (int i = 0; i < n; i++) {
        if (got[i] != want[i]) return 0;
    }
    return 1;
}

static void test_steps(void) {
    WindowStartup s = {.state = {WINDOWED, 0, 0, 1280, 720, 0, 0}};
    CHECK(steps_are(&s, (WindowStep[]) {WINDOW_STEP_CREATE_HIDDEN, WINDOW_STEP_SHOW}, 2), "fresh windowed");

    s.has_pos = 1;
    s.state.maximized = 1;
    CHECK(steps_are(&s, (WindowStep[]) {WINDOW_STEP_CREATE_HIDDEN, WINDOW_STEP_POSITION, WINDOW_STEP_MAXIMIZE, WINDOW_STEP_SHOW}, 4),
          "maximized: placed at the windowed rect, maximized while hidden, then shown");

    // fullscreen / borderless keep the windowed rect underneath (placed first) and never
    // maximize; the mode is applied before the window is shown
    s.state.mode = FULLSCREEN;
    CHECK(steps_are(&s, (WindowStep[]) {WINDOW_STEP_CREATE_HIDDEN, WINDOW_STEP_POSITION, WINDOW_STEP_FULLSCREEN, WINDOW_STEP_SHOW}, 4),
          "fullscreen");
    s.state.mode = BORDERLESS;
    s.has_pos = 0;
    CHECK(steps_are(&s, (WindowStep[]) {WINDOW_STEP_CREATE_HIDDEN, WINDOW_STEP_BORDERLESS, WINDOW_STEP_SHOW}, 3), "borderless");
}

// A window without a kept position opens centred on the work area, decorations included.
static void test_center(void) {
    int x = 0, y = 0;
    window_state_center(0, 0, 1920, 1080, 1280, 720, NULL, &x, &y);
    CHECK(x == 320 && y == 180, "centred without decorations: %d,%d", x, y);
    // a panel on top (work area from y 32) and a 4/30/4/4 frame: the whole frame is centred
    const int frame[4] = {4, 30, 4, 4};
    window_state_center(0, 32, 1920, 1048, 1280, 720, frame, &x, &y);
    CHECK(x == 320 && y == 32 + (1048 - 754) / 2 + 30, "centred with a frame: %d,%d", x, y);
    // a second monitor right of the first
    window_state_center(1920, 0, 2560, 1440, 1280, 720, NULL, &x, &y);
    CHECK(x == 1920 + 640 && y == 360, "second monitor: %d,%d", x, y);
    // bigger than the work area: the frame's top-left at the work area's, so the title bar stays on it
    window_state_center(0, 24, 1280, 776, 1280, 800, frame, &x, &y);
    CHECK(x == 4 && y == 24 + 30, "oversized: %d,%d", x, y);
    // the monitor a page window opens on: the one under its opener's centre, else the primary
    const int wa[2][4] = {{0, 0, 1920, 1080}, {1920, 0, 2560, 1440}};
    CHECK(window_state_pick_monitor(wa, 2, 0, 1, 2500, 700) == 1, "the opener's monitor");
    CHECK(window_state_pick_monitor(wa, 2, 1, 1, -50, -50) == 1, "a point on no monitor: the primary");
    CHECK(window_state_pick_monitor(wa, 2, 0, 0, 2500, 700) == 0, "no point: the primary");
    CHECK(window_state_pick_monitor(wa, 0, 0, 0, 0, 0) == -1, "no monitors");
}

int main(void) {
    test_round_trip();
    test_decode();
    test_resolve();
    test_steps();
    test_center();
    printf("window_state_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
