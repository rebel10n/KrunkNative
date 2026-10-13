// Draggable / resizable window geometry (client/include/win_geom.h).
#include <win_geom.h>
#include <math.h>

static float finite_or_zero(const float v) {
    return isfinite(v) ? v : 0.0f;
}

static float clampf(const float v, const float lo, const float hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

static int moved(const WinGeom *a, const WinGeom *b) {
    return fabsf(a->x - b->x) > 0.01f || fabsf(a->y - b->y) > 0.01f || fabsf(a->w - b->w) > 0.01f || fabsf(a->h - b->h) > 0.01f;
}

int win_geom_fit(WinGeom *g, float vw, float vh, const WinGeomRules *rules) {
    if (!g || !rules) return 0;
    const WinGeom before = *g;
    vw = fmaxf(finite_or_zero(vw), 0.0f);
    vh = fmaxf(finite_or_zero(vh), 0.0f);
    g->x = finite_or_zero(g->x);
    g->y = finite_or_zero(g->y);
    g->w = finite_or_zero(g->w);
    g->h = finite_or_zero(g->h);
    const float min_w = fmaxf(finite_or_zero(rules->min_w), 0.0f);
    const float min_h = fmaxf(finite_or_zero(rules->min_h), 0.0f);

    // size: no larger than the viewport, no smaller than the window allows
    if (g->w > vw) g->w = vw;
    if (g->h > vh) g->h = vh;
    if (g->w < min_w) g->w = min_w;
    if (g->h < min_h) g->h = min_h;

    // position: the kept part inside, the top edge (titlebar) never above the viewport
    const float kw = rules->keep_w > 0.0f ? fminf(rules->keep_w, g->w) : g->w;
    const float kh = rules->keep_h > 0.0f ? fminf(rules->keep_h, g->h) : g->h;
    const float lo_x = kw - g->w, hi_x = vw - kw;
    g->x = hi_x < lo_x ? 0.0f : clampf(g->x, lo_x, hi_x);
    const float hi_y = vh - kh;
    g->y = hi_y < 0.0f ? 0.0f : clampf(g->y, 0.0f, hi_y);
    return moved(&before, g);
}

int win_geom_fit_resize(WinGeom *g, const float vw, const float vh, const WinGeomRules *rules) {
    if (!g || !rules) return 0;
    const WinGeom before = *g;
    const float x = finite_or_zero(g->x), y = finite_or_zero(g->y);
    const float room_w = finite_or_zero(vw) - (x > 0.0f ? x : 0.0f);
    const float room_h = finite_or_zero(vh) - (y > 0.0f ? y : 0.0f);
    if (finite_or_zero(g->w) > room_w) g->w = room_w;
    if (finite_or_zero(g->h) > room_h) g->h = room_h;
    win_geom_fit(g, vw, vh, rules);
    return moved(&before, g);
}
