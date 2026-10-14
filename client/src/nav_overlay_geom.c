// The spectate bot overlay's geometry (client/include/nav_overlay_geom.h): GL-free line lists
// from a NET_EXT_KN_BOT_PATH packet, drawn by client/src/nav_overlay.c.
#include <nav_overlay_geom.h>
#include <bot_path.h> // BotPathKind
#include <math.h>
#include <stdlib.h>
#include <string.h>

// path magenta, jump arcs a lighter magenta, goal yellow, target orange, last known light grey
const int nav_bot_layer_hex[NAV_BOT_LAYERS] = {0xff00ff, 0xff40ff, 0xffe100, 0xff7a00, 0xd0d0d0};

static int verts_push(NavGeomVerts *b, const float x, const float y, const float z) {
    if (b->n + 3 > b->cap) {
        const size_t cap = b->cap ? b->cap * 2 : 3 * 64;
        float *grown = realloc(b->v, cap * sizeof(float));
        if (!grown) return 0;
        b->v = grown;
        b->cap = cap;
    }
    b->v[b->n++] = x;
    b->v[b->n++] = y;
    b->v[b->n++] = z;
    return 1;
}

static void line(NavGeomVerts *b, const vec3 a, const vec3 c) {
    verts_push(b, a.x, a.y, a.z);
    verts_push(b, c.x, c.y, c.z);
}

static vec3 at(const float x, const float y, const float z) { return (vec3) {x, y, z}; }
static vec3 of3(const float *p) { return (vec3) {p[0], p[1], p[2]}; }

// a square ring of radius r around `c`, lifted
static void ring(NavGeomVerts *b, const vec3 c, const float r, const float lift) {
    const float y = c.y + lift;
    line(b, at(c.x - r, y, c.z - r), at(c.x + r, y, c.z - r));
    line(b, at(c.x + r, y, c.z - r), at(c.x + r, y, c.z + r));
    line(b, at(c.x + r, y, c.z + r), at(c.x - r, y, c.z + r));
    line(b, at(c.x - r, y, c.z + r), at(c.x - r, y, c.z - r));
}

// a diamond (octahedron outline) of radius r centred on `c`
static void diamond(NavGeomVerts *b, const vec3 c, const float r) {
    const vec3 top = at(c.x, c.y + r, c.z), bottom = at(c.x, c.y - r, c.z);
    const vec3 px = at(c.x + r, c.y, c.z), nx = at(c.x - r, c.y, c.z), pz = at(c.x, c.y, c.z + r), nz = at(c.x, c.y, c.z - r);
    line(b, px, pz);
    line(b, pz, nx);
    line(b, nx, nz);
    line(b, nz, px);
    line(b, top, px);
    line(b, top, nx);
    line(b, top, pz);
    line(b, top, nz);
    line(b, bottom, px);
    line(b, bottom, nx);
    line(b, bottom, pz);
    line(b, bottom, nz);
}

// a 3-axis cross of half-size r at `c`
static void cross3(NavGeomVerts *b, const vec3 c, const float r) {
    line(b, at(c.x - r, c.y, c.z), at(c.x + r, c.y, c.z));
    line(b, at(c.x, c.y - r, c.z), at(c.x, c.y + r, c.z));
    line(b, at(c.x, c.y, c.z - r), at(c.x, c.y, c.z + r));
}

void nav_bot_geom_clear(NavBotGeom *g) {
    for (int i = 0; i < NAV_BOT_LAYERS; i++) g->layers[i].n = 0;
}

void nav_bot_geom_free(NavBotGeom *g) {
    for (int i = 0; i < NAV_BOT_LAYERS; i++) {
        free(g->layers[i].v);
        g->layers[i].v = NULL;
        g->layers[i].n = g->layers[i].cap = 0;
    }
}

size_t nav_bot_geom_build(const NetBotPathPacket *p, const vec3 *bot_feet, const vec3 *target_feet, NavBotGeom *out) {
    nav_bot_geom_clear(out);
    if (!p) return 0;
    NavGeomVerts *path = &out->layers[NAV_BOT_PATH], *jump = &out->layers[NAV_BOT_JUMP];
    NavGeomVerts *goal = &out->layers[NAV_BOT_GOAL], *target = &out->layers[NAV_BOT_TARGET], *lkp = &out->layers[NAV_BOT_LAST_KNOWN];
    const int alive = (p->flags & NET_BOT_PATH_ACTIVE) != 0;

    // the route: point 0 is the bot's feet as sent; the live feet replace it
    if (alive && p->n >= 2) {
        vec3 prev = bot_feet ? *bot_feet : of3(&p->points[0].x);
        for (int i = 1; i < p->n; i++) {
            const vec3 to = of3(&p->points[i].x);
            const vec3 a = at(prev.x, prev.y + NAV_BOT_PATH_LIFT, prev.z), c = at(to.x, to.y + NAV_BOT_PATH_LIFT, to.z);
            if (p->points[i].kind == BOT_PATH_JUMP) { // an arc over the straight segment
                vec3 last = a;
                for (int k = 1; k <= 4; k++) {
                    const float t = (float) k / 4.0f;
                    const vec3 m = at(a.x + (c.x - a.x) * t, a.y + (c.y - a.y) * t + NAV_BOT_JUMP_APEX * 4.0f * t * (1.0f - t), a.z + (c.z - a.z) * t);
                    line(jump, last, m);
                    last = m;
                }
            } else {
                line(path, a, c);
            }
            line(path, at(to.x, to.y + NAV_BOT_PATH_LIFT, to.z), at(to.x, to.y + NAV_BOT_TICK_HEIGHT, to.z)); // the waypoint tick
            prev = to;
        }
    }

    if (p->flags & NET_BOT_PATH_HAS_GOAL) {
        const vec3 g = of3(p->goal);
        line(goal, at(g.x, g.y, g.z), at(g.x, g.y + NAV_BOT_GOAL_POLE, g.z));
        diamond(goal, at(g.x, g.y + NAV_BOT_GOAL_POLE, g.z), NAV_BOT_GOAL_RADIUS);
        ring(goal, g, NAV_BOT_GOAL_RADIUS, 0.6f);
    }

    const int has_target = (p->flags & NET_BOT_PATH_HAS_TARGET) != 0;
    const int sees = has_target && (p->flags & NET_BOT_PATH_SEES_TARGET);
    vec3 t = {0};
    if (has_target) {
        t = target_feet ? *target_feet : of3(p->target);
        ring(target, t, NAV_BOT_TARGET_RADIUS, 0.5f);
        cross3(target, at(t.x, t.y + NAV_BOT_TARGET_CHEST, t.z), 1.5f);
        if (sees && alive) {
            const vec3 eye = bot_feet ? *bot_feet : of3(&p->points[0].x);
            if (bot_feet || p->n) line(target, at(eye.x, eye.y + NAV_BOT_EYE, eye.z), at(t.x, t.y + NAV_BOT_TARGET_CHEST, t.z));
        }
    }

    if (p->flags & NET_BOT_PATH_HAS_LAST_KNOWN) {
        const vec3 l = of3(p->last_known);
        const int near_seen = sees && hypotf(l.x - t.x, l.z - t.z) < NAV_BOT_LKP_NEAR && fabsf(l.y - t.y) < NAV_BOT_LKP_NEAR;
        if (!near_seen) {
            ring(lkp, l, NAV_BOT_LKP_RADIUS, 0.5f);
            const float y = l.y + 0.5f, r = NAV_BOT_LKP_RADIUS;
            line(lkp, at(l.x - r, y, l.z - r), at(l.x + r, y, l.z + r));
            line(lkp, at(l.x + r, y, l.z - r), at(l.x - r, y, l.z + r));
            line(lkp, at(l.x, l.y, l.z), at(l.x, l.y + NAV_BOT_LKP_POLE, l.z));
        }
    }

    size_t total = 0;
    for (int i = 0; i < NAV_BOT_LAYERS; i++) total += out->layers[i].n / 3;
    return total;
}
