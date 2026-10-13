// win_geom_test: client/src/ui/win_geom.c (win_geom.h), the one rule every KrunkNative
// draggable / resizable window follows: a saved box from another monitor, a smaller window,
// F11 / F4 / F5, a drag or a grip resize past an edge all end inside the viewport.
#include <win_geom.h>

#include <math.h>
#include <stdio.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static int is(const WinGeom *g, const float x, const float y, const float w, const float h) {
    return fabsf(g->x - x) < 0.01f && fabsf(g->y - y) < 0.01f && fabsf(g->w - w) < 0.01f && fabsf(g->h - h) < 0.01f;
}
#define G(g) (double) (g).x, (double) (g).y, (double) (g).w, (double) (g).h

// the developer console: devConsole.ts clampPos keeps the whole box inside (min 280x160 + padding)
static const WinGeomRules CONSOLE = {294.0f, 174.0f, 0.0f, 0.0f};
// the Quick GUI: a titlebar strip stays reachable
static const WinGeomRules STRIP = {150.0f, 24.0f, WIN_GEOM_KEEP_W, WIN_GEOM_KEEP_H};

static void test_console(void) {
    WinGeom g = {5000, 5000, 758, 294}; // saved on a big monitor, far right
    CHECK(win_geom_fit(&g, 1280, 720, &CONSOLE) && is(&g, 522, 426, 758, 294), "off-screen: %g,%g %gx%g", G(g));
    g = (WinGeom) {-400, -300, 758, 294};
    CHECK(win_geom_fit(&g, 1280, 720, &CONSOLE) && is(&g, 0, 0, 758, 294), "negative: %g,%g %gx%g", G(g));
    g = (WinGeom) {100, 50, 3000, 2000}; // oversized: shrinks to the viewport
    CHECK(win_geom_fit(&g, 1280, 720, &CONSOLE) && is(&g, 0, 0, 1280, 720), "oversized: %g,%g %gx%g", G(g));
    g = (WinGeom) {500, 300, 758, 294};
    CHECK(!win_geom_fit(&g, 1280, 720, &CONSOLE) && is(&g, 500, 300, 758, 294), "inside: unchanged");
    // a resize to a tiny window: the minimum wins, pinned top-left
    g = (WinGeom) {500, 300, 758, 294};
    CHECK(win_geom_fit(&g, 200, 100, &CONSOLE) && is(&g, 0, 0, 294, 174), "tiny viewport: %g,%g %gx%g", G(g));
    // F11 back to a smaller window: still inside, the saved size kept when it fits
    g = (WinGeom) {1800, 900, 758, 294};
    CHECK(win_geom_fit(&g, 1024, 600, &CONSOLE) && is(&g, 266, 306, 758, 294), "smaller window: %g,%g %gx%g", G(g));
    // non-finite input (a broken store)
    g = (WinGeom) {NAN, INFINITY, NAN, 300};
    win_geom_fit(&g, 1280, 720, &CONSOLE);
    CHECK(is(&g, 0, 0, 294, 300), "non-finite: %g,%g %gx%g", G(g));
}

static void test_strip(void) {
    WinGeom g = {1250, 700, 150, 238}; // a drag past the bottom-right corner: a strip stays
    CHECK(win_geom_fit(&g, 1280, 720, &STRIP) && is(&g, 1160, 696, 150, 238), "past bottom-right: %g,%g", G(g));
    CHECK(g.x + WIN_GEOM_KEEP_W <= 1280.01f && g.y + WIN_GEOM_KEEP_H <= 720.01f, "strip visible");
    g = (WinGeom) {-140, -50, 150, 238}; // past the top-left: the titlebar never goes above 0
    CHECK(win_geom_fit(&g, 1280, 720, &STRIP) && is(&g, -30, 0, 150, 238), "past top-left: %g,%g", G(g));
    g = (WinGeom) {-20, 10, 150, 238}; // partly off the left edge, more than the strip visible: kept
    CHECK(!win_geom_fit(&g, 1280, 720, &STRIP), "partly off: unchanged");
}

static void test_resize(void) {
    WinGeom g = {1000, 400, 2000, 1000}; // a grip dragged far past the corner: stops at the edges
    CHECK(win_geom_fit_resize(&g, 1280, 720, &CONSOLE) && is(&g, 1000 - 14, 400 - 0, 294, 320),
          "grip past edge: %g,%g %gx%g", G(g));
    g = (WinGeom) {100, 100, 900, 400};
    CHECK(win_geom_fit_resize(&g, 1280, 720, &CONSOLE) == 0 && is(&g, 100, 100, 900, 400), "grip inside: unchanged");
    g = (WinGeom) {100, 100, 100, 50}; // below the minimum
    CHECK(win_geom_fit_resize(&g, 1280, 720, &CONSOLE) && is(&g, 100, 100, 294, 174), "grip minimum: %g,%g %gx%g", G(g));
}

int main(void) {
    test_console();
    test_strip();
    test_resize();
    printf("win_geom_test: %d checks, %d failures\n", checks, failures);
    return failures ? 1 : 0;
}
