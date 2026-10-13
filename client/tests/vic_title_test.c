// vic_title_test: the round-end title's fade and zoom (client/src/ui/vic_title.c, drawn by hud_ui.c
// on #knVicTitle) frame by frame against v7.1.4's own overlay.vicTitle code (game-overlay.js:318 and
// :2087, identical in v10.0.2), run verbatim in node over the same frame sequences:
// client/tests/data/vic_title_golden.json {case: [{show, dt, out: [shown, globalAlpha, scale]}]}.
// Plus the shape players see: at 60 Hz the title fades in from 0 and zooms from 2.5 to 1 in ~17
// frames, a round started after a reset (decrement 0.01) is quicker than the very first one
// (0.005), and the slow motion's shrinking dt stretches it.
//   vic_title_test <repo root>
#include "vic_title.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static char *slurp(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *s = n >= 0 ? malloc((size_t) n + 1) : NULL;
    if (s && fread(s, 1, (size_t) n, f) != (size_t) n) {
        free(s);
        s = NULL;
    }
    if (s) s[n] = 0;
    fclose(f);
    return s;
}

// Frames until the zoom lands on 1 (scale drawn == 1), -1 if it never does in `max`.
static int frames_to_land(VicTitle *t, const double dt, const int max) {
    for (int i = 0; i < max; i++) {
        double a, s;
        vic_title_frame(t, 1, dt, &a, &s);
        if (s == 1.0) return i;
    }
    return -1;
}

int main(int argc, char **argv) {
    char path[1024];
    snprintf(path, sizeof path, "%s/client/tests/data/vic_title_golden.json", argc > 1 ? argv[1] : ".");
    char *text = slurp(path);
    cJSON *root = text ? cJSON_Parse(text) : NULL;
    CHECK(cJSON_IsObject(root), "cannot read %s", path);
    int frames = 0;
    const cJSON *c;
    cJSON_ArrayForEach(c, root) {
        VicTitle t;
        vic_title_init(&t);
        int i = 0;
        const cJSON *f;
        cJSON_ArrayForEach(f, c) {
            const int show = (int) cJSON_GetNumberValue(cJSON_GetObjectItem(f, "show"));
            const double dt = cJSON_GetNumberValue(cJSON_GetObjectItem(f, "dt"));
            const cJSON *out = cJSON_GetObjectItem(f, "out");
            double a = -1.0, s = -1.0;
            const int shown = vic_title_frame(&t, show, dt, &a, &s);
            CHECK(shown == (int) cJSON_GetNumberValue(cJSON_GetArrayItem(out, 0)), "%s frame %d: shown %d", c->string, i, shown);
            if (shown) {
                const double ga = cJSON_GetNumberValue(cJSON_GetArrayItem(out, 1)), gs = cJSON_GetNumberValue(cJSON_GetArrayItem(out, 2));
                CHECK(fabs(a - ga) < 1e-9, "%s frame %d: globalAlpha %.9f, v7 %.9f", c->string, i, a, ga);
                CHECK(fabs(s - gs) < 1e-9, "%s frame %d: scale %.9f, v7 %.9f", c->string, i, s, gs);
            }
            i++;
            frames++;
        }
    }
    CHECK(frames > 150, "golden has %d frames", frames);

    // 60 Hz after the reset branch (every live END: the HUD ticks hidden frames first)
    VicTitle t;
    vic_title_init(&t);
    vic_title_frame(&t, 0, 16.0, NULL, NULL);
    CHECK(t.opacity == 0.0 && t.scale == 2.5 && t.decrement == 0.01, "reset: opacity %g scale %g decrement %g", t.opacity, t.scale, t.decrement);
    double prev_a = -1.0, prev_s = 3.0;
    for (int i = 0; i < 24; i++) {
        double a, s;
        vic_title_frame(&t, 1, 1000.0 / 60.0, &a, &s);
        CHECK(a >= prev_a && s <= prev_s, "frame %d: fade-in / zoom-out not monotonic (%g, %g)", i, a, s);
        if (i == 0) CHECK(a == 0.0 && s == 2.5, "first frame draws alpha 0 at scale 2.5 (%g, %g)", a, s);
        if (i == 8) CHECK(a > 0.4 && a < 0.45 && s > 2.05 && s < 2.15, "frame 8 mid-animation (%g, %g)", a, s);
        prev_a = a;
        prev_s = s;
    }
    CHECK(prev_a == 1.0 && prev_s == 1.0, "settled at alpha 1 scale 1 (%g, %g)", prev_a, prev_s);
    vic_title_init(&t);
    vic_title_frame(&t, 0, 16.0, NULL, NULL);
    const int land60 = frames_to_land(&t, 1000.0 / 60.0, 200);
    CHECK(land60 == 17, "60 Hz: the zoom lands on frame %d (v7: 17)", land60);
    // the scale drawn on frame 12 at 60 Hz: after a reset, without one (decrement 0.005: the very
    // first title is a little slower; live, the HUD always runs the reset branch first) and in the slow motion (dt shrinks from 16.7 ms to 0 over endAnim)
    double s_reset = 0.0, s_first = 0.0, s_slow = 0.0, a;
    vic_title_init(&t);
    vic_title_frame(&t, 0, 16.0, NULL, NULL);
    for (int i = 0; i <= 12; i++) vic_title_frame(&t, 1, 1000.0 / 60.0, &a, &s_reset);
    vic_title_init(&t);
    for (int i = 0; i <= 12; i++) vic_title_frame(&t, 1, 1000.0 / 60.0, &a, &s_first);
    vic_title_init(&t);
    vic_title_frame(&t, 0, 16.0, NULL, NULL);
    for (int i = 0; i <= 12; i++) vic_title_frame(&t, 1, 1000.0 / 60.0 * (1.0 - i * (1000.0 / 60.0) / 5000.0), &a, &s_slow);
    CHECK(s_first > s_reset, "the first title zooms slower than one after a reset (%g vs %g)", s_first, s_reset);
    CHECK(s_slow > s_reset, "slow motion stretches the zoom (%g vs %g)", s_slow, s_reset);
    // hiding mid-animation resets (the menu over the end screen, the results)
    vic_title_frame(&t, 0, 16.0, NULL, NULL);
    CHECK(t.opacity == 0.0 && t.scale == 2.5 && t.decrement == 0.01, "hidden: reset");

    // io_gameEnd's type for every result: the draw (didWin -1, e.g. a TDM tie) has its title too
    CHECK(!strcmp(vic_title_type(1), "vic") && !strcmp(vic_title_type(0), "def") && !strcmp(vic_title_type(-1), "dra"),
          "vicTitle.type: %s %s %s", vic_title_type(1), vic_title_type(0), vic_title_type(-1));

    cJSON_Delete(root);
    free(text);
    printf("vic_title_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
