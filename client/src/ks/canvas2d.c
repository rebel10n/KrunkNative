// Canvas2D on GL: see client/include/canvas2d.h for the model and the supported subset.
//
// Recording: every drawing call turns its geometry into triangles in device pixels (the
// canvas matrix scaled by device/canvas size) and appends a command:
//   CMD_DRAW     triangles drawn directly with blending (convex fills, single-segment
//                strokes, glyph quads, images); consecutive draws of one mode/texture merge
//   CMD_FILL     nonzero fill: fan triangles into the stencil (INCR_WRAP front faces,
//                DECR_WRAP back faces, colour writes off), then the bounding box drawn
//                where stencil != 0, which also resets the stencil to 0
//   CMD_UNION    stroke outlines: triangles set stencil = 1 (overlaps count once, so
//                translucent strokes blend once per pixel), then the cover pass
//   CMD_CLEAR    clearRect: the quad written with blending off (transparent black)
// Replay (c2d_flush) renders into the layer: a 4x MSAA colour + depth/stencil renderbuffer
// pair resolved into an RGBA8 texture (KRUNKNATIVE_CANVAS_MSAA=0: the texture directly with
// a single-sample stencil). The layer holds premultiplied colour; c2d_composite draws it with
// (ONE, ONE_MINUS_SRC_ALPHA). Sampler: `tex` on texture unit 0 (glyph atlas, image or layer).
//
// Shaders: assets/shaders/canvas2d.vert / canvas2d.frag (GLSL 4.50 core, valid GLSL ES 3.00).
#include <gl.h>
#include <client.h>
#include <canvas2d.h>

#include <freetype/ftadvanc.h>
#include <freetype/ftoutln.h>
#include <freetype/tttables.h>
#include <stb_image.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#define C2D_TAU (2.0 * M_PI)

typedef struct {
    float x, y;
} P2;

typedef struct {
    float x, y, u, v, r, g, b, a;
} Vtx;

typedef struct {
    size_t start, count;
    int closed;
    int has_segment; // a lineTo/curve was added (a zero-length one still gets round/square caps)
} SubPath;

typedef struct {
    P2 *pts;
    size_t npts, cap_pts;
    SubPath *subs;
    size_t nsubs, cap_subs;
} Path;

typedef struct {
    C2DMatrix m;
    float fill[4], stroke[4];
    double alpha, line_width, miter_limit;
    C2DLineJoin join;
    C2DLineCap cap;
    double font_px;
    C2DTextAlign align;
    C2DTextBaseline baseline;
} State;

enum { CMD_DRAW, CMD_FILL, CMD_UNION, CMD_CLEAR };
enum { MODE_SOLID = 0, MODE_GLYPH = 1, MODE_IMAGE = 2 };

typedef struct {
    uint8_t kind, mode;
    C2DImage *image; // MODE_IMAGE texture
    uint32_t first, count, cover_first;
} Cmd;

struct C2DImage {
    char *path;
    int state; // 1 ready, 0 loading, -1 failed
    unsigned char *pixels; // premultiplied RGBA, top row first, until uploaded
    int w, h;
    unsigned int tex;
    C2DImage *next;
};

typedef struct {
    uint32_t glyph;
    uint32_t size_q; // device px * 4
    int16_t x, y, w, h;     // atlas rect of the bitmap (without padding)
    int16_t left, top;      // FreeType bitmap_left / bitmap_top
    uint8_t used;
} GlyphSlot;

#define GLYPH_TABLE_SIZE 4096 // power of two
#define GLYPH_TABLE_MAX 3000

struct Canvas2D {
    int canvas_w, canvas_h, device_w, device_h;
    double sx, sy; // device px per canvas px

    State state;
    State *stack;
    int depth, stack_cap, overflow;
    Path path, scratch;
    P2 *tris; // stroker scratch (user space triangles)
    size_t ntris, cap_tris;

    Vtx *verts;
    size_t nverts, cap_verts;
    Cmd *cmds;
    size_t ncmds, cap_cmds;
    int pending_clear; // the layer must be cleared before the next replay
    int layer_dirty;   // the layer shows something

    // text
    FT_Library ft;
    FT_Face face;
    int font_failed;
    double upem, ascender, descender; // font units (descender positive, below the baseline)
    double typo_ascender, typo_descender; // OS/2 sTypo* (hhea when absent), descender positive
    int32_t *advances;                // per glyph index, font units, INT32_MIN unknown
    long nglyphs;
    unsigned char *atlas;             // CPU copy, C2D_GLYPH_ATLAS_SIZE^2
    int atlas_dirty_y0, atlas_dirty_y1;
    int shelf_x, shelf_y, shelf_h;
    GlyphSlot *glyph_table;
    int glyph_count;

    C2DImage *images;

    // GL
    int gl_ready, gl_failed;
    unsigned int program, vao, vbo, atlas_tex;
    int loc_viewport, loc_mode, loc_tex;
    size_t vbo_cap;
    int samples;
    int layer_w, layer_h;
    unsigned int msaa_fbo, msaa_color, msaa_ds;
    unsigned int layer_fbo, layer_tex, layer_ds;

    int stat_cmds, stat_verts, stat_draws;
};

/* ------------------------------------------------------------------ small helpers */

static int finite2(const double a, const double b) { return isfinite(a) && isfinite(b); }

static void *grow(void *ptr, size_t *cap, const size_t need, const size_t elem) {
    if (need <= *cap) return ptr;
    size_t cap2 = *cap ? *cap : 64;
    while (cap2 < need) cap2 *= 2;
    void *p = realloc(ptr, cap2 * elem);
    if (!p) return NULL;
    *cap = cap2;
    return p;
}

static C2DMatrix mat_identity(void) { return (C2DMatrix) {1, 0, 0, 1, 0, 0}; }

static C2DMatrix mat_mul(const C2DMatrix m, const C2DMatrix t) {
    return (C2DMatrix) {
        m.a * t.a + m.c * t.b, m.b * t.a + m.d * t.b,
        m.a * t.c + m.c * t.d, m.b * t.c + m.d * t.d,
        m.a * t.e + m.c * t.f + m.e, m.b * t.e + m.d * t.f + m.f,
    };
}

static P2 mat_apply(const C2DMatrix *m, const double x, const double y) {
    return (P2) {(float) (m->a * x + m->c * y + m->e), (float) (m->b * x + m->d * y + m->f)};
}

static int mat_invert(const C2DMatrix *m, C2DMatrix *out) {
    const double det = m->a * m->d - m->b * m->c;
    if (!isfinite(det) || fabs(det) < 1e-12) return 0;
    const double id = 1.0 / det;
    out->a = m->d * id;
    out->b = -m->b * id;
    out->c = -m->c * id;
    out->d = m->a * id;
    out->e = (m->c * m->f - m->d * m->e) * id;
    out->f = (m->b * m->e - m->a * m->f) * id;
    return 1;
}

// Largest singular value of the linear part: the most a unit length grows on screen.
static double mat_max_scale(const C2DMatrix *m) {
    const double p = m->a * m->a + m->b * m->b, q = m->c * m->c + m->d * m->d;
    const double r = m->a * m->c + m->b * m->d;
    const double tr = (p + q) * 0.5, disc = sqrt(fmax(0.0, (p - q) * (p - q) * 0.25 + r * r));
    return sqrt(tr + disc);
}

// Canvas matrix -> device px
static C2DMatrix device_matrix(const Canvas2D *c) {
    const C2DMatrix m = c->state.m;
    return (C2DMatrix) {m.a * c->sx, m.b * c->sy, m.c * c->sx, m.d * c->sy, m.e * c->sx, m.f * c->sy};
}

/* ------------------------------------------------------------------ path building (device px) */

static void path_reset(Path *p) {
    p->npts = 0;
    p->nsubs = 0;
}

static void path_free(Path *p) {
    free(p->pts);
    free(p->subs);
    memset(p, 0, sizeof(*p));
}

static int path_has_current(const Path *p) { return p->nsubs > 0; }

static P2 path_last(const Path *p) { return p->pts[p->npts - 1]; }

static void path_push(Path *p, const P2 pt) {
    P2 *pts = grow(p->pts, &p->cap_pts, p->npts + 1, sizeof(P2));
    if (!pts) return;
    p->pts = pts;
    p->pts[p->npts++] = pt;
    p->subs[p->nsubs - 1].count++;
}

static void path_new_sub(Path *p, const P2 pt) {
    SubPath *subs = grow(p->subs, &p->cap_subs, p->nsubs + 1, sizeof(SubPath));
    if (!subs) return;
    p->subs = subs;
    p->subs[p->nsubs++] = (SubPath) {p->npts, 0, 0, 0};
    path_push(p, pt);
}

static void path_line(Path *p, const P2 pt) {
    if (!path_has_current(p)) {
        path_new_sub(p, pt);
        return;
    }
    path_push(p, pt);
    p->subs[p->nsubs - 1].has_segment = 1;
}

// closePath: closes the subpath and starts a new one at its first point (HTML spec).
static void path_close(Path *p) {
    if (!path_has_current(p)) return;
    SubPath *s = &p->subs[p->nsubs - 1];
    if (s->closed) return;
    s->closed = 1;
    path_new_sub(p, p->pts[s->start]);
}

static int curve_steps(const double device_len, const double scale_k) {
    const double n = ceil(sqrt(scale_k * device_len / C2D_CURVE_TOLERANCE));
    return n < 1 ? 1 : n > 512 ? 512 : (int) n;
}

static void path_quad(Path *p, const P2 c1, const P2 to) {
    const P2 s = path_last(p);
    const double ddx = s.x - 2.0 * c1.x + to.x, ddy = s.y - 2.0 * c1.y + to.y;
    const int n = curve_steps(sqrt(ddx * ddx + ddy * ddy), 0.25); // Wang: sqrt(d/(4 tol))... (2*1/8)
    for (int i = 1; i <= n; i++) {
        const double t = (double) i / n, u = 1.0 - t;
        path_push(p, (P2) {(float) (u * u * s.x + 2 * u * t * c1.x + t * t * to.x),
                           (float) (u * u * s.y + 2 * u * t * c1.y + t * t * to.y)});
    }
    p->subs[p->nsubs - 1].has_segment = 1;
}

static void path_cubic(Path *p, const P2 c1, const P2 c2, const P2 to) {
    const P2 s = path_last(p);
    const double d1x = s.x - 2.0 * c1.x + c2.x, d1y = s.y - 2.0 * c1.y + c2.y;
    const double d2x = c1.x - 2.0 * c2.x + to.x, d2y = c1.y - 2.0 * c2.y + to.y;
    const double dd = fmax(sqrt(d1x * d1x + d1y * d1y), sqrt(d2x * d2x + d2y * d2y));
    const int n = curve_steps(dd, 0.75); // Wang's formula, degree 3: 3*2/8
    for (int i = 1; i <= n; i++) {
        const double t = (double) i / n, u = 1.0 - t;
        const double b0 = u * u * u, b1 = 3 * u * u * t, b2 = 3 * u * t * t, b3 = t * t * t;
        path_push(p, (P2) {(float) (b0 * s.x + b1 * c1.x + b2 * c2.x + b3 * to.x),
                           (float) (b0 * s.y + b1 * c1.y + b2 * c2.y + b3 * to.y)});
    }
    p->subs[p->nsubs - 1].has_segment = 1;
}

// Elliptical arc in user space (centre, radii, rotation, start angle, signed sweep) through
// the device matrix: connects to the current subpath with a line or starts one.
static void path_arc(Path *p, const C2DMatrix *dm, const double cx, const double cy, const double rx, const double ry,
                     const double rot, const double a0, const double sweep) {
    const double cr = cos(rot), sr = sin(rot);
    const double radius = mat_max_scale(dm) * fmax(rx, ry);
    int n;
    if (radius <= C2D_CURVE_TOLERANCE) n = (int) ceil(fabs(sweep) / (M_PI / 2.0));
    else n = (int) ceil(fabs(sweep) / (2.0 * acos(1.0 - C2D_CURVE_TOLERANCE / radius)));
    if (n < 1) n = 1;
    if (n > 4096) n = 4096;
    for (int i = 0; i <= n; i++) {
        const double a = a0 + sweep * i / n;
        const double ex = cos(a) * rx, ey = sin(a) * ry;
        const P2 pt = mat_apply(dm, cx + ex * cr - ey * sr, cy + ex * sr + ey * cr);
        if (i == 0 && !path_has_current(p)) path_new_sub(p, pt);
        else path_line(p, pt);
        if (sweep == 0.0) break;
    }
}

// Chromium's adjustEndAngle: the sweep the HTML spec's arc/ellipse angles describe.
static double arc_sweep(double start, double end, const int ccw) {
    if (!ccw && end - start >= C2D_TAU) return C2D_TAU;
    if (ccw && start - end >= C2D_TAU) return -C2D_TAU;
    if (!ccw && start > end) end = start + (C2D_TAU - fmod(start - end, C2D_TAU));
    else if (ccw && start < end) end = start - (C2D_TAU - fmod(end - start, C2D_TAU));
    return end - start;
}

static void path_rect(Path *p, const C2DMatrix *dm, const double x, const double y, const double w, const double h) {
    path_new_sub(p, mat_apply(dm, x, y));
    path_line(p, mat_apply(dm, x + w, y));
    path_line(p, mat_apply(dm, x + w, y + h));
    path_line(p, mat_apply(dm, x, y + h));
    path_close(p);
}

/* ------------------------------------------------------------------ recording */

static Vtx *push_verts(Canvas2D *c, const size_t n) {
    Vtx *v = grow(c->verts, &c->cap_verts, c->nverts + n, sizeof(Vtx));
    if (!v) return NULL;
    c->verts = v;
    Vtx *out = c->verts + c->nverts;
    c->nverts += n;
    return out;
}

static void add_cmd(Canvas2D *c, const int kind, const int mode, C2DImage *image, const uint32_t first,
                    const uint32_t count, const uint32_t cover_first) {
    if (kind == CMD_DRAW && c->ncmds) {
        Cmd *last = &c->cmds[c->ncmds - 1];
        if (last->kind == CMD_DRAW && last->mode == mode && last->image == image && last->first + last->count == first) {
            last->count += count;
            return;
        }
    }
    Cmd *cmds = grow(c->cmds, &c->cap_cmds, c->ncmds + 1, sizeof(Cmd));
    if (!cmds) return;
    c->cmds = cmds;
    c->cmds[c->ncmds++] = (Cmd) {(uint8_t) kind, (uint8_t) mode, image, first, count, cover_first};
}

static void premul(const float rgba[4], const double alpha, float out[4]) {
    const float a = (float) (rgba[3] * alpha);
    out[0] = rgba[0] * a;
    out[1] = rgba[1] * a;
    out[2] = rgba[2] * a;
    out[3] = a;
}

static void put(Vtx *v, const P2 p, const float u, const float t, const float col[4]) {
    *v = (Vtx) {p.x, p.y, u, t, col[0], col[1], col[2], col[3]};
}

// Bounding box quad of verts[first, first + count) as 6 vertices; 0 when nothing is on screen.
static int push_cover(Canvas2D *c, const size_t first, const size_t count, const float col[4]) {
    float x0 = INFINITY, y0 = INFINITY, x1 = -INFINITY, y1 = -INFINITY;
    for (size_t i = first; i < first + count; i++) {
        const Vtx *v = &c->verts[i];
        if (v->x < x0) x0 = v->x;
        if (v->y < y0) y0 = v->y;
        if (v->x > x1) x1 = v->x;
        if (v->y > y1) y1 = v->y;
    }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 > (float) c->device_w) x1 = (float) c->device_w;
    if (y1 > (float) c->device_h) y1 = (float) c->device_h;
    if (!(x1 > x0 && y1 > y0)) return 0;
    Vtx *v = push_verts(c, 6);
    if (!v) return 0;
    const P2 a = {x0, y0}, b = {x1, y0}, d = {x1, y1}, e = {x0, y1};
    put(v + 0, a, 0, 0, col);
    put(v + 1, b, 0, 0, col);
    put(v + 2, d, 0, 0, col);
    put(v + 3, a, 0, 0, col);
    put(v + 4, d, 0, 0, col);
    put(v + 5, e, 0, 0, col);
    return 1;
}

// Unique points of a subpath (consecutive duplicates and a closing duplicate dropped).
static size_t sub_points(const Path *p, const SubPath *s, const C2DMatrix *map, P2 **out, size_t *cap) {
    P2 *buf = grow(*out, cap, s->count + 1, sizeof(P2));
    if (!buf) return 0;
    *out = buf;
    size_t n = 0;
    for (size_t i = 0; i < s->count; i++) {
        P2 q = p->pts[s->start + i];
        if (map) q = mat_apply(map, q.x, q.y);
        if (n && fabsf(q.x - buf[n - 1].x) < 1e-4f && fabsf(q.y - buf[n - 1].y) < 1e-4f) continue;
        buf[n++] = q;
    }
    if (s->closed && n > 1 && fabsf(buf[0].x - buf[n - 1].x) < 1e-4f && fabsf(buf[0].y - buf[n - 1].y) < 1e-4f) n--;
    return n;
}

// Convex simple polygon test: turns all one way and the edge directions flip at most twice in x and y.
static int poly_convex(const P2 *p, const size_t n) {
    if (n < 3) return 0;
    int turn = 0, xs = 0, ys = 0, xflips = 0, yflips = 0, xfirst = 0, yfirst = 0;
    for (size_t i = 0; i < n; i++) {
        const P2 a = p[i], b = p[(i + 1) % n], d = p[(i + 2) % n];
        const double e1x = (double) b.x - a.x, e1y = (double) b.y - a.y;
        const double e2x = (double) d.x - b.x, e2y = (double) d.y - b.y;
        const double cross = e1x * e2y - e1y * e2x;
        const double mag = fabs(e1x * e2y) + fabs(e1y * e2x);
        if (fabs(cross) > 1e-9 * mag + 1e-12) {
            const int s = cross > 0 ? 1 : -1;
            if (!turn) turn = s;
            else if (s != turn) return 0;
        }
        if (e1x != 0) {
            const int s = e1x > 0 ? 1 : -1;
            if (!xs) xfirst = s;
            else if (s != xs) xflips++;
            xs = s;
        }
        if (e1y != 0) {
            const int s = e1y > 0 ? 1 : -1;
            if (!ys) yfirst = s;
            else if (s != ys) yflips++;
            ys = s;
        }
    }
    if (xs && xs != xfirst) xflips++;
    if (ys && ys != yfirst) yflips++;
    return turn != 0 && xflips <= 2 && yflips <= 2;
}

static P2 *g_pts_scratch;
static size_t g_pts_scratch_cap;

static void fill_path(Canvas2D *c, const Path *p, const float color[4]) {
    float col[4];
    premul(color, c->state.alpha, col);
    if (col[3] <= 0.0f) return;

    size_t fillable = 0, only = 0;
    for (size_t i = 0; i < p->nsubs; i++) {
        if (p->subs[i].count >= 3) {
            fillable++;
            only = i;
        }
    }
    if (!fillable) return;

    if (fillable == 1) {
        const size_t n = sub_points(p, &p->subs[only], NULL, &g_pts_scratch, &g_pts_scratch_cap);
        if (n < 3) return;
        if (poly_convex(g_pts_scratch, n)) {
            Vtx *v = push_verts(c, (n - 2) * 3);
            if (!v) return;
            const uint32_t first = (uint32_t) (v - c->verts);
            for (size_t i = 1; i + 1 < n; i++) {
                put(v++, g_pts_scratch[0], 0, 0, col);
                put(v++, g_pts_scratch[i], 0, 0, col);
                put(v++, g_pts_scratch[i + 1], 0, 0, col);
            }
            add_cmd(c, CMD_DRAW, MODE_SOLID, NULL, first, (uint32_t) ((n - 2) * 3), 0);
            return;
        }
    }

    const size_t first = c->nverts;
    for (size_t s = 0; s < p->nsubs; s++) {
        const SubPath *sub = &p->subs[s];
        if (sub->count < 3) continue;
        const P2 *pts = p->pts + sub->start;
        Vtx *v = push_verts(c, (sub->count - 2) * 3);
        if (!v) return;
        for (size_t i = 1; i + 1 < sub->count; i++) {
            put(v++, pts[0], 0, 0, col);
            put(v++, pts[i], 0, 0, col);
            put(v++, pts[i + 1], 0, 0, col);
        }
    }
    const size_t count = c->nverts - first;
    if (!count) return;
    const size_t cover = c->nverts;
    if (!push_cover(c, first, count, col)) {
        c->nverts = first;
        return;
    }
    add_cmd(c, CMD_FILL, MODE_SOLID, NULL, (uint32_t) first, (uint32_t) count, (uint32_t) cover);
}

/* ------------------------------------------------------------------ stroker (user space) */

static void tri(Canvas2D *c, const P2 a, const P2 b, const P2 d) {
    P2 *t = grow(c->tris, &c->cap_tris, c->ntris + 3, sizeof(P2));
    if (!t) return;
    c->tris = t;
    c->tris[c->ntris++] = a;
    c->tris[c->ntris++] = b;
    c->tris[c->ntris++] = d;
}

static P2 p2(const double x, const double y) { return (P2) {(float) x, (float) y}; }

static int round_steps(const double radius_device, const double angle) {
    int n;
    if (radius_device <= C2D_CURVE_TOLERANCE) n = (int) ceil(angle / (M_PI / 2.0));
    else n = (int) ceil(angle / (2.0 * acos(1.0 - C2D_CURVE_TOLERANCE / radius_device)));
    return n < 1 ? 1 : n > 256 ? 256 : n;
}

// Fan around `center` from angle a0 sweeping `sweep` at radius r.
static void fan(Canvas2D *c, const P2 center, const double r, const double a0, const double sweep, const double dev_scale) {
    const int n = round_steps(r * dev_scale, fabs(sweep));
    P2 prev = p2(center.x + cos(a0) * r, center.y + sin(a0) * r);
    for (int i = 1; i <= n; i++) {
        const double a = a0 + sweep * i / n;
        const P2 next = p2(center.x + cos(a) * r, center.y + sin(a) * r);
        tri(c, center, prev, next);
        prev = next;
    }
}

static void stroke_join(Canvas2D *c, const P2 p, const double d0x, const double d0y, const double d1x, const double d1y,
                        const double hw, const double dev_scale) {
    const double cross = d0x * d1y - d0y * d1x, dot = d0x * d1x + d0y * d1y;
    if (fabs(cross) < 1e-9 && dot > 0) return;
    const double side = cross > 0 ? -1.0 : 1.0; // outer side of the turn
    const double o0x = -d0y * hw * side, o0y = d0x * hw * side;
    const double o1x = -d1y * hw * side, o1y = d1x * hw * side;
    const P2 a = p2(p.x + o0x, p.y + o0y), b = p2(p.x + o1x, p.y + o1y);
    const C2DLineJoin join = c->state.join;
    if (join == C2D_JOIN_ROUND) {
        const double a0 = atan2(o0y, o0x), a1 = atan2(o1y, o1x);
        double sweep = a1 - a0;
        while (sweep > M_PI) sweep -= C2D_TAU;
        while (sweep < -M_PI) sweep += C2D_TAU;
        fan(c, p, hw, a0, sweep, dev_scale);
        return;
    }
    if (join == C2D_JOIN_MITER) {
        const double cos_half = sqrt(fmax(0.0, (1.0 + dot) * 0.5)); // sin of half the interior angle
        if (cos_half > 1e-9 && 1.0 / cos_half <= c->state.miter_limit) {
            double mx = o0x + o1x, my = o0y + o1y;
            const double ml = sqrt(mx * mx + my * my);
            if (ml > 1e-12) {
                const double len = hw / cos_half;
                mx = mx / ml * len;
                my = my / ml * len;
                const P2 tip = p2(p.x + mx, p.y + my);
                tri(c, p, a, tip);
                tri(c, p, tip, b);
                return;
            }
        }
    }
    tri(c, p, a, b); // bevel
}

static void stroke_cap(Canvas2D *c, const P2 p, const double dx, const double dy, const double hw, const double dev_scale) {
    // (dx, dy): unit direction pointing away from the line
    const double nx = -dy * hw, ny = dx * hw;
    if (c->state.cap == C2D_CAP_SQUARE) {
        const P2 a = p2(p.x + nx, p.y + ny), b = p2(p.x - nx, p.y - ny);
        const P2 a2 = p2(a.x + dx * hw, a.y + dy * hw), b2 = p2(b.x + dx * hw, b.y + dy * hw);
        tri(c, a, b, b2);
        tri(c, a, b2, a2);
    } else if (c->state.cap == C2D_CAP_ROUND) {
        fan(c, p, hw, atan2(ny, nx), -M_PI, dev_scale);
    }
}

// Stroke outline triangles of `path` (device points) mapped to user space by `inv`.
static void stroke_tris(Canvas2D *c, const Path *path, const C2DMatrix *inv, const double hw, const double dev_scale) {
    for (size_t s = 0; s < path->nsubs; s++) {
        const SubPath *sub = &path->subs[s];
        const size_t n = sub_points(path, sub, inv, &g_pts_scratch, &g_pts_scratch_cap);
        const P2 *q = g_pts_scratch;
        if (n == 0) continue;
        if (n == 1) {
            // zero-length subpath: only round/square caps paint (HTML spec)
            if (sub->has_segment || sub->closed) {
                if (c->state.cap == C2D_CAP_ROUND) fan(c, q[0], hw, 0, C2D_TAU, dev_scale);
                else if (c->state.cap == C2D_CAP_SQUARE) {
                    const P2 a = p2(q[0].x - hw, q[0].y - hw), b = p2(q[0].x + hw, q[0].y - hw);
                    const P2 d = p2(q[0].x + hw, q[0].y + hw), e = p2(q[0].x - hw, q[0].y + hw);
                    tri(c, a, b, d);
                    tri(c, a, d, e);
                }
            }
            continue;
        }
        const int closed = sub->closed && n > 2;
        const size_t segs = closed ? n : n - 1;
        double fdx = 0, fdy = 0, pdx = 0, pdy = 0;
        for (size_t i = 0; i < segs; i++) {
            const P2 a = q[i], b = q[(i + 1) % n];
            double dx = (double) b.x - a.x, dy = (double) b.y - a.y;
            const double len = sqrt(dx * dx + dy * dy);
            dx /= len;
            dy /= len;
            const double nx = -dy * hw, ny = dx * hw;
            const P2 a0 = p2(a.x + nx, a.y + ny), a1 = p2(a.x - nx, a.y - ny);
            const P2 b0 = p2(b.x + nx, b.y + ny), b1 = p2(b.x - nx, b.y - ny);
            tri(c, a0, a1, b1);
            tri(c, a0, b1, b0);
            if (i == 0) {
                fdx = dx;
                fdy = dy;
            } else {
                stroke_join(c, a, pdx, pdy, dx, dy, hw, dev_scale);
            }
            pdx = dx;
            pdy = dy;
        }
        if (closed) {
            stroke_join(c, q[0], pdx, pdy, fdx, fdy, hw, dev_scale);
        } else {
            stroke_cap(c, q[0], -fdx, -fdy, hw, dev_scale);
            stroke_cap(c, q[n - 1], pdx, pdy, hw, dev_scale);
        }
    }
}

static void stroke_path(Canvas2D *c, const Path *p, const float color[4]) {
    float col[4];
    premul(color, c->state.alpha, col);
    if (col[3] <= 0.0f || !p->nsubs) return;
    const C2DMatrix dm = device_matrix(c);
    C2DMatrix inv;
    if (!mat_invert(&dm, &inv)) return;
    const double hw = c->state.line_width * 0.5;
    c->ntris = 0;
    stroke_tris(c, p, &inv, hw, mat_max_scale(&dm));
    if (!c->ntris) return;

    // one straight butt/square segment is a convex quad: no stencil pass needed
    const int direct = c->ntris == 6 && p->nsubs <= 2 && c->state.cap != C2D_CAP_ROUND;
    const size_t first = c->nverts;
    Vtx *v = push_verts(c, c->ntris);
    if (!v) return;
    for (size_t i = 0; i < c->ntris; i++) put(v + i, mat_apply(&dm, c->tris[i].x, c->tris[i].y), 0, 0, col);
    if (direct) {
        add_cmd(c, CMD_DRAW, MODE_SOLID, NULL, (uint32_t) first, (uint32_t) c->ntris, 0);
        return;
    }
    const size_t cover = c->nverts;
    if (!push_cover(c, first, c->ntris, col)) {
        c->nverts = first;
        return;
    }
    add_cmd(c, CMD_UNION, MODE_SOLID, NULL, (uint32_t) first, (uint32_t) c->ntris, (uint32_t) cover);
}

/* ------------------------------------------------------------------ public: lifecycle, state */

static void state_default(State *s) {
    memset(s, 0, sizeof(*s));
    s->m = mat_identity();
    s->fill[3] = s->stroke[3] = 1.0f; // #000
    s->alpha = 1.0;
    s->line_width = 1.0;
    s->miter_limit = 10.0;
    s->join = C2D_JOIN_MITER;
    s->cap = C2D_CAP_BUTT;
    s->font_px = 10.0; // "10px sans-serif"
    s->align = C2D_ALIGN_START;
    s->baseline = C2D_BASELINE_ALPHABETIC;
}

Canvas2D *c2d_create(void) {
    Canvas2D *c = calloc(1, sizeof(Canvas2D));
    if (!c) return NULL;
    state_default(&c->state);
    c->sx = c->sy = 1.0;
    return c;
}

void c2d_reset(Canvas2D *c) {
    state_default(&c->state);
    c->depth = c->overflow = 0;
    path_reset(&c->path);
}

void c2d_set_size(Canvas2D *c, int canvas_w, int canvas_h, int device_w, int device_h) {
    if (canvas_w < 0) canvas_w = 0;
    if (canvas_h < 0) canvas_h = 0;
    if (device_w <= 0) device_w = canvas_w;
    if (device_h <= 0) device_h = canvas_h;
    if (canvas_w == c->canvas_w && canvas_h == c->canvas_h && device_w == c->device_w && device_h == c->device_h) return;
    c->canvas_w = canvas_w;
    c->canvas_h = canvas_h;
    c->device_w = device_w;
    c->device_h = device_h;
    c->sx = canvas_w ? (double) device_w / canvas_w : 1.0;
    c->sy = canvas_h ? (double) device_h / canvas_h : 1.0;
    c2d_reset(c);
    c->ncmds = c->nverts = 0;
    c->pending_clear = 1;
    c->layer_dirty = 0;
}

int c2d_width(const Canvas2D *c) { return c->canvas_w; }
int c2d_height(const Canvas2D *c) { return c->canvas_h; }

void c2d_save(Canvas2D *c) {
    if (c->depth >= C2D_MAX_SAVE_DEPTH) {
        c->overflow++;
        return;
    }
    if (c->depth >= c->stack_cap) {
        const int cap = c->stack_cap ? c->stack_cap * 2 : 16;
        State *s = realloc(c->stack, (size_t) cap * sizeof(State));
        if (!s) return;
        c->stack = s;
        c->stack_cap = cap;
    }
    c->stack[c->depth++] = c->state;
}

void c2d_restore(Canvas2D *c) {
    if (c->overflow) {
        c->overflow--;
        return;
    }
    if (c->depth <= 0) return;
    c->state = c->stack[--c->depth];
}

void c2d_transform(Canvas2D *c, double a, double b, double cc, double d, double e, double f) {
    if (!finite2(a, b) || !finite2(cc, d) || !finite2(e, f)) return;
    c->state.m = mat_mul(c->state.m, (C2DMatrix) {a, b, cc, d, e, f});
}

void c2d_set_transform(Canvas2D *c, double a, double b, double cc, double d, double e, double f) {
    if (!finite2(a, b) || !finite2(cc, d) || !finite2(e, f)) return;
    c->state.m = (C2DMatrix) {a, b, cc, d, e, f};
}

void c2d_translate(Canvas2D *c, double x, double y) { c2d_transform(c, 1, 0, 0, 1, x, y); }
void c2d_scale(Canvas2D *c, double x, double y) { c2d_transform(c, x, 0, 0, y, 0, 0); }

void c2d_rotate(Canvas2D *c, double r) {
    if (!isfinite(r)) return;
    const double cs = cos(r), sn = sin(r);
    c2d_transform(c, cs, sn, -sn, cs, 0, 0);
}

C2DMatrix c2d_get_transform(const Canvas2D *c) { return c->state.m; }

void c2d_set_global_alpha(Canvas2D *c, double a) {
    if (!isfinite(a) || a < 0.0 || a > 1.0) return;
    c->state.alpha = a;
}

static void set_rgba(float out[4], float r, float g, float b, float a) {
    out[0] = r;
    out[1] = g;
    out[2] = b;
    out[3] = a;
}

void c2d_set_fill_color(Canvas2D *c, float r, float g, float b, float a) { set_rgba(c->state.fill, r, g, b, a); }
void c2d_set_stroke_color(Canvas2D *c, float r, float g, float b, float a) { set_rgba(c->state.stroke, r, g, b, a); }

static int hexval(const char ch) {
    if (ch >= '0' && ch <= '9') return ch - '0';
    if (ch >= 'a' && ch <= 'f') return ch - 'a' + 10;
    if (ch >= 'A' && ch <= 'F') return ch - 'A' + 10;
    return -1;
}

int c2d_parse_color(const char *css, float rgba[4]) {
    if (!css || css[0] != '#') return 0;
    const size_t n = strlen(css + 1);
    if (n != 3 && n != 4 && n != 6 && n != 8) return 0;
    int v[8];
    for (size_t i = 0; i < n; i++) {
        if ((v[i] = hexval(css[1 + i])) < 0) return 0;
    }
    if (n <= 4) {
        for (size_t i = 0; i < 4; i++) rgba[i] = i < n ? (float) (v[i] * 17) / 255.0f : 1.0f;
    } else {
        for (size_t i = 0; i < 4; i++) rgba[i] = i * 2 < n ? (float) (v[i * 2] * 16 + v[i * 2 + 1]) / 255.0f : 1.0f;
    }
    return 1;
}

void c2d_set_line_width(Canvas2D *c, double w) {
    if (!isfinite(w) || w <= 0.0) return;
    c->state.line_width = w;
}

void c2d_set_line_join(Canvas2D *c, C2DLineJoin j) { c->state.join = j; }
void c2d_set_line_cap(Canvas2D *c, C2DLineCap cap) { c->state.cap = cap; }

void c2d_set_miter_limit(Canvas2D *c, double l) {
    if (!isfinite(l) || l <= 0.0) return;
    c->state.miter_limit = l;
}

int c2d_set_font(Canvas2D *c, double px, const char *family) {
    if (!isfinite(px) || px <= 0.0 || !family || !*family) return 0;
    c->state.font_px = px; // every family renders with the HUD font (GameFont)
    return 1;
}

int c2d_set_font_css(Canvas2D *c, const char *font) {
    if (!font) return 0;
    // "[words] <number>px <family...>": find the token ending in "px"
    const char *p = font;
    while (*p) {
        while (*p == ' ') p++;
        const char *tok = p;
        while (*p && *p != ' ') p++;
        const size_t len = (size_t) (p - tok);
        if (len > 2 && !strncasecmp(p - 2, "px", 2)) {
            char num[64];
            if (len - 2 >= sizeof(num)) return 0;
            memcpy(num, tok, len - 2);
            num[len - 2] = 0;
            char *end;
            const double px = strtod(num, &end);
            if (*end) return 0;
            while (*p == ' ') p++;
            return c2d_set_font(c, px, p);
        }
    }
    return 0;
}

void c2d_set_text_align(Canvas2D *c, C2DTextAlign a) { c->state.align = a; }
void c2d_set_text_baseline(Canvas2D *c, C2DTextBaseline b) { c->state.baseline = b; }

int c2d_set_text_align_name(Canvas2D *c, const char *name) {
    static const char *const names[] = {"start", "end", "left", "right", "center"};
    for (int i = 0; name && i < 5; i++) {
        if (!strcmp(name, names[i])) {
            c->state.align = (C2DTextAlign) i;
            return 1;
        }
    }
    return 0;
}

int c2d_set_text_baseline_name(Canvas2D *c, const char *name) {
    static const char *const names[] = {"alphabetic", "top", "hanging", "middle", "ideographic", "bottom"};
    for (int i = 0; name && i < 6; i++) {
        if (!strcmp(name, names[i])) {
            c->state.baseline = (C2DTextBaseline) i;
            return 1;
        }
    }
    return 0;
}

int c2d_set_line_join_name(Canvas2D *c, const char *name) {
    if (!name) return 0;
    if (!strcmp(name, "miter")) c->state.join = C2D_JOIN_MITER;
    else if (!strcmp(name, "round")) c->state.join = C2D_JOIN_ROUND;
    else if (!strcmp(name, "bevel")) c->state.join = C2D_JOIN_BEVEL;
    else return 0;
    return 1;
}

/* ------------------------------------------------------------------ public: path */

void c2d_begin_path(Canvas2D *c) { path_reset(&c->path); }
void c2d_close_path(Canvas2D *c) { path_close(&c->path); }

void c2d_move_to(Canvas2D *c, double x, double y) {
    if (!finite2(x, y)) return;
    const C2DMatrix dm = device_matrix(c);
    path_new_sub(&c->path, mat_apply(&dm, x, y));
}

void c2d_line_to(Canvas2D *c, double x, double y) {
    if (!finite2(x, y)) return;
    const C2DMatrix dm = device_matrix(c);
    path_line(&c->path, mat_apply(&dm, x, y));
}

void c2d_quadratic_curve_to(Canvas2D *c, double cpx, double cpy, double x, double y) {
    if (!finite2(cpx, cpy) || !finite2(x, y)) return;
    const C2DMatrix dm = device_matrix(c);
    const P2 cp = mat_apply(&dm, cpx, cpy);
    if (!path_has_current(&c->path)) path_new_sub(&c->path, cp);
    path_quad(&c->path, cp, mat_apply(&dm, x, y));
}

void c2d_bezier_curve_to(Canvas2D *c, double c1x, double c1y, double c2x, double c2y, double x, double y) {
    if (!finite2(c1x, c1y) || !finite2(c2x, c2y) || !finite2(x, y)) return;
    const C2DMatrix dm = device_matrix(c);
    const P2 cp1 = mat_apply(&dm, c1x, c1y);
    if (!path_has_current(&c->path)) path_new_sub(&c->path, cp1);
    path_cubic(&c->path, cp1, mat_apply(&dm, c2x, c2y), mat_apply(&dm, x, y));
}

int c2d_ellipse(Canvas2D *c, double x, double y, double rx, double ry, double rot, double start, double end, int ccw) {
    if (!finite2(x, y) || !finite2(rx, ry) || !finite2(rot, start) || !isfinite(end)) return 0;
    if (rx < 0 || ry < 0) return -1;
    const C2DMatrix dm = device_matrix(c);
    path_arc(&c->path, &dm, x, y, rx, ry, rot, start, arc_sweep(start, end, ccw));
    return 0;
}

int c2d_arc(Canvas2D *c, double x, double y, double r, double start, double end, int ccw) {
    return c2d_ellipse(c, x, y, r, r, 0.0, start, end, ccw);
}

int c2d_arc_to(Canvas2D *c, double x1, double y1, double x2, double y2, double r) {
    if (!finite2(x1, y1) || !finite2(x2, y2) || !isfinite(r)) return 0;
    if (r < 0) return -1;
    const C2DMatrix dm = device_matrix(c);
    if (!path_has_current(&c->path)) {
        path_new_sub(&c->path, mat_apply(&dm, x1, y1));
        return 0;
    }
    C2DMatrix inv;
    if (!mat_invert(&dm, &inv)) {
        path_line(&c->path, mat_apply(&dm, x1, y1));
        return 0;
    }
    const P2 last = path_last(&c->path);
    const P2 p0 = mat_apply(&inv, last.x, last.y);
    const double v1x = p0.x - x1, v1y = p0.y - y1, v2x = x2 - x1, v2y = y2 - y1;
    const double l1 = sqrt(v1x * v1x + v1y * v1y), l2 = sqrt(v2x * v2x + v2y * v2y);
    const double cross = v1x * v2y - v1y * v2x;
    if (l1 < 1e-9 || l2 < 1e-9 || r == 0.0 || fabs(cross) < 1e-9 * l1 * l2) {
        path_line(&c->path, mat_apply(&dm, x1, y1));
        return 0;
    }
    const double u1x = v1x / l1, u1y = v1y / l1, u2x = v2x / l2, u2y = v2y / l2;
    const double cosang = fmax(-1.0, fmin(1.0, u1x * u2x + u1y * u2y));
    const double half = acos(cosang) * 0.5;
    const double dist = r / tan(half);
    const double t1x = x1 + u1x * dist, t1y = y1 + u1y * dist;
    const double t2x = x1 + u2x * dist, t2y = y1 + u2y * dist;
    double bx = u1x + u2x, by = u1y + u2y;
    const double bl = sqrt(bx * bx + by * by);
    bx /= bl;
    by /= bl;
    const double cd = r / sin(half);
    const double cx = x1 + bx * cd, cy = y1 + by * cd;
    const double a0 = atan2(t1y - cy, t1x - cx), a1 = atan2(t2y - cy, t2x - cx);
    double sweep = a1 - a0;
    while (sweep > M_PI) sweep -= C2D_TAU;
    while (sweep < -M_PI) sweep += C2D_TAU;
    path_line(&c->path, mat_apply(&dm, t1x, t1y));
    path_arc(&c->path, &dm, cx, cy, r, r, 0.0, a0, sweep);
    return 0;
}

void c2d_rect(Canvas2D *c, double x, double y, double w, double h) {
    if (!finite2(x, y) || !finite2(w, h)) return;
    const C2DMatrix dm = device_matrix(c);
    path_rect(&c->path, &dm, x, y, w, h);
}

void c2d_fill(Canvas2D *c) { fill_path(c, &c->path, c->state.fill); }
void c2d_stroke(Canvas2D *c) { stroke_path(c, &c->path, c->state.stroke); }

void c2d_fill_rect(Canvas2D *c, double x, double y, double w, double h) {
    if (!finite2(x, y) || !finite2(w, h) || w == 0 || h == 0) return;
    const C2DMatrix dm = device_matrix(c);
    path_reset(&c->scratch);
    path_rect(&c->scratch, &dm, x, y, w, h);
    fill_path(c, &c->scratch, c->state.fill);
}

void c2d_stroke_rect(Canvas2D *c, double x, double y, double w, double h) {
    if (!finite2(x, y) || !finite2(w, h) || (w == 0 && h == 0)) return;
    const C2DMatrix dm = device_matrix(c);
    path_reset(&c->scratch);
    if (w == 0 || h == 0) {
        path_new_sub(&c->scratch, mat_apply(&dm, x, y));
        path_line(&c->scratch, mat_apply(&dm, x + w, y + h));
    } else {
        path_rect(&c->scratch, &dm, x, y, w, h);
    }
    stroke_path(c, &c->scratch, c->state.stroke);
}

void c2d_clear_rect(Canvas2D *c, double x, double y, double w, double h) {
    if (!finite2(x, y) || !finite2(w, h) || w == 0 || h == 0) return;
    const C2DMatrix dm = device_matrix(c);
    const P2 q[4] = {mat_apply(&dm, x, y), mat_apply(&dm, x + w, y), mat_apply(&dm, x + w, y + h), mat_apply(&dm, x, y + h)};
    if (dm.b == 0 && dm.c == 0) {
        const float x0 = fminf(q[0].x, q[2].x), x1 = fmaxf(q[0].x, q[2].x);
        const float y0 = fminf(q[0].y, q[2].y), y1 = fmaxf(q[0].y, q[2].y);
        if (x0 <= 0 && y0 <= 0 && x1 >= (float) c->device_w && y1 >= (float) c->device_h) {
            // the whole layer: drop everything pending, clear at the next flush
            c->ncmds = c->nverts = 0;
            c->pending_clear = 1;
            c->layer_dirty = 0;
            return;
        }
    }
    if (!c->layer_dirty && !c->ncmds) return; // nothing to erase
    const float zero[4] = {0, 0, 0, 0};
    Vtx *v = push_verts(c, 6);
    if (!v) return;
    const uint32_t first = (uint32_t) (v - c->verts);
    put(v + 0, q[0], 0, 0, zero);
    put(v + 1, q[1], 0, 0, zero);
    put(v + 2, q[2], 0, 0, zero);
    put(v + 3, q[0], 0, 0, zero);
    put(v + 4, q[2], 0, 0, zero);
    put(v + 5, q[3], 0, 0, zero);
    add_cmd(c, CMD_CLEAR, MODE_SOLID, NULL, first, 6, 0);
}

/* ------------------------------------------------------------------ text */

static int font_ready(Canvas2D *c) {
    if (c->face) return 1;
    if (c->font_failed) return 0;
    c->font_failed = 1;
    char *path = concat(assets_path(), "css/fonts/font2.ttf");
    if (!path) return 0;
    ensure_asset_file(path);
    if (FT_Init_FreeType(&c->ft) || FT_New_Face(c->ft, path, 0, &c->face)) {
        fprintf(stderr, "canvas2d: cannot load font %s\n", path);
        if (c->ft) FT_Done_FreeType(c->ft);
        c->ft = NULL;
        c->face = NULL;
        free(path);
        return 0;
    }
    free(path);
    c->upem = c->face->units_per_EM ? c->face->units_per_EM : 1000;
    c->ascender = c->face->ascender;
    c->descender = -c->face->descender;
    const TT_OS2 *os2 = FT_Get_Sfnt_Table(c->face, FT_SFNT_OS2);
    c->typo_ascender = os2 && os2->version != 0xFFFF ? os2->sTypoAscender : c->ascender;
    c->typo_descender = os2 && os2->version != 0xFFFF ? -os2->sTypoDescender : c->descender;
    if (c->typo_ascender + c->typo_descender <= 0) {
        c->typo_ascender = c->upem * 0.8;
        c->typo_descender = c->upem * 0.2;
    }
    c->nglyphs = c->face->num_glyphs;
    c->advances = malloc((size_t) (c->nglyphs > 0 ? c->nglyphs : 1) * sizeof(int32_t));
    if (!c->advances) return 0;
    for (long i = 0; i < c->nglyphs; i++) c->advances[i] = INT32_MIN;
    c->font_failed = 0;
    return 1;
}

static double glyph_advance(Canvas2D *c, const uint32_t g) {
    if ((long) g >= c->nglyphs) return 0;
    if (c->advances[g] == INT32_MIN) {
        FT_Fixed adv = 0;
        if (FT_Get_Advance(c->face, g, FT_LOAD_NO_SCALE, &adv)) adv = 0;
        c->advances[g] = (int32_t) adv;
    }
    return c->advances[g];
}

static uint32_t utf8_next(const char **s) {
    const unsigned char *p = (const unsigned char *) *s;
    uint32_t cp = *p++;
    int extra = 0;
    if (cp >= 0xF0) {
        cp &= 0x07;
        extra = 3;
    } else if (cp >= 0xE0) {
        cp &= 0x0F;
        extra = 2;
    } else if (cp >= 0xC0) {
        cp &= 0x1F;
        extra = 1;
    } else if (cp >= 0x80) {
        cp = 0xFFFD;
    }
    while (extra-- > 0) {
        if ((*p & 0xC0) != 0x80) {
            cp = 0xFFFD;
            break;
        }
        cp = (cp << 6) | (*p++ & 0x3F);
    }
    *s = (const char *) p;
    return cp;
}

typedef struct {
    uint32_t glyph;
    double x; // pen position, font units
} GlyphPos;

static GlyphPos *g_layout;
static size_t g_layout_cap;

// Glyphs of `text` with pen positions in font units; returns the count, *width in font units.
static size_t layout_text(Canvas2D *c, const char *text, double *width) {
    size_t n = 0;
    double pen = 0;
    uint32_t prev = 0;
    const int kern = FT_HAS_KERNING(c->face);
    while (text && *text) {
        uint32_t cp = utf8_next(&text);
        // HTML canvas text preparation: ASCII whitespace becomes a space
        if (cp == '\t' || cp == '\n' || cp == '\f' || cp == '\r') cp = ' ';
        const uint32_t g = FT_Get_Char_Index(c->face, cp);
        if (kern && prev && g) {
            FT_Vector k;
            if (!FT_Get_Kerning(c->face, prev, g, FT_KERNING_UNSCALED, &k)) pen += k.x;
        }
        GlyphPos *buf = grow(g_layout, &g_layout_cap, n + 1, sizeof(GlyphPos));
        if (!buf) break;
        g_layout = buf;
        g_layout[n++] = (GlyphPos) {g, pen};
        pen += glyph_advance(c, g);
        prev = g;
    }
    *width = pen;
    return n;
}

double c2d_measure_text(Canvas2D *c, const char *text) {
    if (!font_ready(c)) return 0;
    double width;
    layout_text(c, text, &width);
    return width * c->state.font_px / c->upem;
}

// Anchor offset (font px) of the text start and baseline for textAlign/textBaseline, as
// Blink's TextMetrics::GetFontBaseline: top/bottom/middle use the em square (OS/2 typo
// ascent/descent normalised to 1 em, LayoutUnit 1/64 px precision), hanging/ideographic the
// font ascent/descent (rounded to whole px like Skia's font metrics).
static void text_anchor(Canvas2D *c, const double width_px, double *ox, double *oy) {
    const double px = c->state.font_px, k = px / c->upem;
    const double asc = round(c->ascender * k), desc = round(c->descender * k);
    const double em_asc = round(px * c->typo_ascender / (c->typo_ascender + c->typo_descender) * 64.0) / 64.0;
    const double em_desc = round(px * 64.0) / 64.0 - em_asc;
    switch (c->state.align) {
        case C2D_ALIGN_END:
        case C2D_ALIGN_RIGHT: *ox = -width_px; break;
        case C2D_ALIGN_CENTER: *ox = -width_px * 0.5; break;
        default: *ox = 0; break;
    }
    switch (c->state.baseline) {
        case C2D_BASELINE_TOP: *oy = em_asc; break;
        case C2D_BASELINE_HANGING: *oy = asc * 0.8; break;
        case C2D_BASELINE_MIDDLE: *oy = (em_asc - em_desc) * 0.5; break;
        case C2D_BASELINE_IDEOGRAPHIC: *oy = -desc; break;
        case C2D_BASELINE_BOTTOM: *oy = -em_desc; break;
        default: *oy = 0; break;
    }
}

// FT outline -> path (user coordinates through the device matrix)
typedef struct {
    Path *path;
    const C2DMatrix *dm;
    double ox, oy, kx, ky; // user = (ox + fx * kx, oy - fy * ky)
    int open;
} OutlineCtx;

static P2 ol_point(const OutlineCtx *o, const FT_Vector *v) {
    return mat_apply(o->dm, o->ox + (double) v->x * o->kx, o->oy - (double) v->y * o->ky);
}

static int ol_move(const FT_Vector *to, void *user) {
    OutlineCtx *o = user;
    if (o->open) path_close(o->path);
    path_new_sub(o->path, ol_point(o, to));
    o->open = 1;
    return 0;
}

static int ol_line(const FT_Vector *to, void *user) {
    OutlineCtx *o = user;
    path_line(o->path, ol_point(o, to));
    return 0;
}

static int ol_conic(const FT_Vector *control, const FT_Vector *to, void *user) {
    OutlineCtx *o = user;
    path_quad(o->path, ol_point(o, control), ol_point(o, to));
    return 0;
}

static int ol_cubic(const FT_Vector *c1, const FT_Vector *c2, const FT_Vector *to, void *user) {
    OutlineCtx *o = user;
    path_cubic(o->path, ol_point(o, c1), ol_point(o, c2), ol_point(o, to));
    return 0;
}

// Glyph outlines of the laid-out text into c->scratch.
static void text_outlines(Canvas2D *c, const GlyphPos *gl, const size_t n, const C2DMatrix *dm, const double x,
                          const double y, const double hscale) {
    static const FT_Outline_Funcs funcs = {ol_move, ol_line, ol_conic, ol_cubic, 0, 0};
    const double k = c->state.font_px / c->upem;
    path_reset(&c->scratch);
    for (size_t i = 0; i < n; i++) {
        if (!gl[i].glyph) continue;
        if (FT_Load_Glyph(c->face, gl[i].glyph, FT_LOAD_NO_SCALE | FT_LOAD_NO_BITMAP | FT_LOAD_NO_HINTING)) continue;
        if (c->face->glyph->format != FT_GLYPH_FORMAT_OUTLINE) continue;
        OutlineCtx o = {&c->scratch, dm, x + gl[i].x * k * hscale, y, k * hscale, k, 0};
        FT_Outline_Decompose(&c->face->glyph->outline, &funcs, &o);
        if (o.open) path_close(&c->scratch);
    }
}

static void atlas_reset(Canvas2D *c) {
    c->shelf_x = c->shelf_y = c->shelf_h = 0;
    c->glyph_count = 0;
    if (c->glyph_table) memset(c->glyph_table, 0, GLYPH_TABLE_SIZE * sizeof(GlyphSlot));
}

static const GlyphSlot *glyph_raster(Canvas2D *c, const uint32_t g, const uint32_t size_q, const int allow_flush) {
    if (!c->glyph_table) {
        c->glyph_table = calloc(GLYPH_TABLE_SIZE, sizeof(GlyphSlot));
        c->atlas = calloc((size_t) C2D_GLYPH_ATLAS_SIZE * C2D_GLYPH_ATLAS_SIZE, 1);
        if (!c->glyph_table || !c->atlas) return NULL;
        c->atlas_dirty_y0 = C2D_GLYPH_ATLAS_SIZE;
        c->atlas_dirty_y1 = 0;
    }
    uint32_t h = (g * 2654435761u) ^ (size_q * 40503u);
    for (int probe = 0; probe < GLYPH_TABLE_SIZE; probe++) {
        GlyphSlot *s = &c->glyph_table[(h + probe) & (GLYPH_TABLE_SIZE - 1)];
        if (!s->used) break;
        if (s->glyph == g && s->size_q == size_q) return s;
    }

    FT_Set_Char_Size(c->face, 0, (FT_F26Dot6) (size_q * 16), 72, 72); // size_q / 4 px in 26.6
    if (FT_Load_Glyph(c->face, g, FT_LOAD_TARGET_LIGHT | FT_LOAD_RENDER)) return NULL;
    const FT_Bitmap *bm = &c->face->glyph->bitmap;
    const int w = (int) bm->width, rows = (int) bm->rows;
    if (w + 2 > C2D_GLYPH_ATLAS_SIZE || rows + 2 > C2D_GLYPH_ATLAS_SIZE) return NULL;

    if (c->shelf_x + w + 2 > C2D_GLYPH_ATLAS_SIZE) {
        c->shelf_y += c->shelf_h;
        c->shelf_x = 0;
        c->shelf_h = 0;
    }
    if (c->shelf_y + rows + 2 > C2D_GLYPH_ATLAS_SIZE || c->glyph_count >= GLYPH_TABLE_MAX) {
        if (!allow_flush) return NULL;
        c2d_flush(c); // queued glyph quads still point into the atlas
        atlas_reset(c);
        return glyph_raster(c, g, size_q, 0);
    }
    const int ax = c->shelf_x, ay = c->shelf_y;
    for (int y = 0; y < rows + 2; y++) memset(c->atlas + (size_t) (ay + y) * C2D_GLYPH_ATLAS_SIZE + ax, 0, (size_t) w + 2);
    for (int y = 0; y < rows; y++) {
        const unsigned char *src = bm->buffer + (bm->pitch >= 0 ? y * bm->pitch : (rows - 1 - y) * -bm->pitch);
        memcpy(c->atlas + (size_t) (ay + 1 + y) * C2D_GLYPH_ATLAS_SIZE + ax + 1, src, (size_t) w);
    }
    if (ay < c->atlas_dirty_y0) c->atlas_dirty_y0 = ay;
    if (ay + rows + 2 > c->atlas_dirty_y1) c->atlas_dirty_y1 = ay + rows + 2;
    c->shelf_x += w + 2;
    if (rows + 2 > c->shelf_h) c->shelf_h = rows + 2;

    GlyphSlot *s = NULL;
    for (int probe = 0; probe < GLYPH_TABLE_SIZE; probe++) {
        s = &c->glyph_table[(h + probe) & (GLYPH_TABLE_SIZE - 1)];
        if (!s->used) break;
    }
    *s = (GlyphSlot) {g, size_q, (int16_t) (ax + 1), (int16_t) (ay + 1), (int16_t) w, (int16_t) rows,
                      (int16_t) c->face->glyph->bitmap_left, (int16_t) c->face->glyph->bitmap_top, 1};
    c->glyph_count++;
    return s;
}

static void draw_text(Canvas2D *c, const char *text, double x, double y, const int has_max, const double max_width,
                      const int stroke) {
    if (!finite2(x, y) || !text || !*text) return;
    if (has_max && !(max_width > 0)) return;
    if (!font_ready(c)) return;
    float col[4];
    premul(stroke ? c->state.stroke : c->state.fill, c->state.alpha, col);
    if (col[3] <= 0.0f) return;

    double width_units;
    const size_t n = layout_text(c, text, &width_units);
    if (!n) return;
    const double k = c->state.font_px / c->upem;
    const double width = width_units * k;
    const double hscale = has_max && width > max_width ? max_width / width : 1.0;
    double ox, oy;
    text_anchor(c, width * hscale, &ox, &oy);
    x += ox;
    y += oy;

    const C2DMatrix dm = device_matrix(c);
    const double det = fabs(dm.a * dm.d - dm.b * dm.c);
    const double dev_px = c->state.font_px * sqrt(det);
    if (!(dev_px > 0.05)) return;

    if (stroke || dev_px > C2D_GLYPH_MAX_RASTER_PX) {
        // outline path: strokeText, and fills too large for the atlas
        text_outlines(c, g_layout, n, &dm, x, y, hscale);
        if (stroke) stroke_path(c, &c->scratch, c->state.stroke);
        else fill_path(c, &c->scratch, c->state.fill);
        return;
    }

    uint32_t size_q = (uint32_t) lround(dev_px * 4.0);
    if (size_q < 1) size_q = 1;
    const double raster_px = size_q / 4.0;
    // axis-aligned, uniform, unscaled text: glyph bitmaps land 1:1 on device pixels
    const int aligned = dm.b == 0 && dm.c == 0 && dm.a > 0 && dm.d > 0 && fabs(dm.a - dm.d) < 1e-6 * dm.a && hscale == 1.0;
    for (size_t i = 0; i < n; i++) {
        const GlyphPos gp = g_layout[i];
        if (!gp.glyph) continue;
        const GlyphSlot *s = glyph_raster(c, gp.glyph, size_q, 1);
        if (!s || !s->w || !s->h) continue;
        const float u0 = (float) s->x / C2D_GLYPH_ATLAS_SIZE, v0 = (float) s->y / C2D_GLYPH_ATLAS_SIZE;
        const float u1 = (float) (s->x + s->w) / C2D_GLYPH_ATLAS_SIZE, v1 = (float) (s->y + s->h) / C2D_GLYPH_ATLAS_SIZE;
        P2 q[4];
        if (aligned) {
            const P2 o = mat_apply(&dm, x + gp.x * k, y);
            const float gx = (float) (round(o.x) + s->left), gy = (float) (round(o.y) - s->top);
            q[0] = (P2) {gx, gy};
            q[1] = (P2) {gx + s->w, gy};
            q[2] = (P2) {gx + s->w, gy + s->h};
            q[3] = (P2) {gx, gy + s->h};
        } else {
            const double f = c->state.font_px / raster_px; // raster px -> font px
            const double gx = x + gp.x * k * hscale, l = s->left * f * hscale, r = (s->left + s->w) * f * hscale;
            const double t = -s->top * f, b = (s->h - s->top) * f;
            q[0] = mat_apply(&dm, gx + l, y + t);
            q[1] = mat_apply(&dm, gx + r, y + t);
            q[2] = mat_apply(&dm, gx + r, y + b);
            q[3] = mat_apply(&dm, gx + l, y + b);
        }
        Vtx *v = push_verts(c, 6);
        if (!v) return;
        const uint32_t first = (uint32_t) (v - c->verts);
        put(v + 0, q[0], u0, v0, col);
        put(v + 1, q[1], u1, v0, col);
        put(v + 2, q[2], u1, v1, col);
        put(v + 3, q[0], u0, v0, col);
        put(v + 4, q[2], u1, v1, col);
        put(v + 5, q[3], u0, v1, col);
        add_cmd(c, CMD_DRAW, MODE_GLYPH, NULL, first, 6, 0);
    }
}

void c2d_fill_text(Canvas2D *c, const char *text, double x, double y, int has_max, double max_width) {
    draw_text(c, text, x, y, has_max, max_width, 0);
}

void c2d_stroke_text(Canvas2D *c, const char *text, double x, double y, int has_max, double max_width) {
    draw_text(c, text, x, y, has_max, max_width, 1);
}

/* ------------------------------------------------------------------ images */

static void image_try_load(C2DImage *img) {
    if (img->state != 0) return;
    const int st = asset_prefetch(img->path); // web: background download; native: 1
    if (st == 0) return;
    if (st < 0 || !ensure_asset_file(img->path)) {
        img->state = -1;
        return;
    }
    stbi_set_flip_vertically_on_load_thread(0); // top row first (texture v = 0 at the top)
    int w, h, comp;
    unsigned char *px = stbi_load(img->path, &w, &h, &comp, 4);
    if (!px) {
        fprintf(stderr, "canvas2d: cannot load image %s: %s\n", img->path, stbi_failure_reason());
        img->state = -1;
        return;
    }
    for (size_t i = 0; i < (size_t) w * h; i++) { // premultiply: bilinear filtering without dark fringes
        unsigned char *p = px + i * 4;
        p[0] = (unsigned char) ((p[0] * p[3] + 127) / 255);
        p[1] = (unsigned char) ((p[1] * p[3] + 127) / 255);
        p[2] = (unsigned char) ((p[2] * p[3] + 127) / 255);
    }
    img->pixels = px;
    img->w = w;
    img->h = h;
    img->state = 1;
}

C2DImage *c2d_image(Canvas2D *c, const char *path) {
    if (!path || !*path) return NULL;
    for (C2DImage *i = c->images; i; i = i->next) {
        if (!strcmp(i->path, path)) {
            image_try_load(i);
            return i;
        }
    }
    C2DImage *img = calloc(1, sizeof(C2DImage));
    if (!img) return NULL;
    img->path = strdup(path);
    if (!img->path) {
        free(img);
        return NULL;
    }
    img->next = c->images;
    c->images = img;
    image_try_load(img);
    return img;
}

int c2d_image_state(const C2DImage *img) { return img ? img->state : -1; }

void c2d_image_size(const C2DImage *img, int *w, int *h) {
    if (w) *w = img && img->state == 1 ? img->w : 0;
    if (h) *h = img && img->state == 1 ? img->h : 0;
}

void c2d_draw_image(Canvas2D *c, C2DImage *img, double dx, double dy, double dw, double dh) {
    if (!img || img->state != 1) return;
    c2d_draw_image_sub(c, img, 0, 0, img->w, img->h, dx, dy, dw, dh);
}

void c2d_draw_image_sub(Canvas2D *c, C2DImage *img, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh) {
    if (!img || img->state != 1 || !finite2(dx, dy) || !finite2(dw, dh) || dw == 0 || dh == 0) return;
    if (!finite2(sx, sy) || !finite2(sw, sh) || sw == 0 || sh == 0 || img->w <= 0 || img->h <= 0) return;
    const float u0 = (float) (sx / img->w), v0 = (float) (sy / img->h);
    const float u1 = (float) ((sx + sw) / img->w), v1 = (float) ((sy + sh) / img->h);
    if (dw < 0) {
        dx += dw;
        dw = -dw;
    }
    if (dh < 0) {
        dy += dh;
        dh = -dh;
    }
    const float a = (float) c->state.alpha;
    if (a <= 0.0f) return;
    const float col[4] = {a, a, a, a};
    const C2DMatrix dm = device_matrix(c);
    Vtx *v = push_verts(c, 6);
    if (!v) return;
    const uint32_t first = (uint32_t) (v - c->verts);
    const P2 p0 = mat_apply(&dm, dx, dy), p1 = mat_apply(&dm, dx + dw, dy);
    const P2 p2_ = mat_apply(&dm, dx + dw, dy + dh), p3 = mat_apply(&dm, dx, dy + dh);
    put(v + 0, p0, u0, v0, col);
    put(v + 1, p1, u1, v0, col);
    put(v + 2, p2_, u1, v1, col);
    put(v + 3, p0, u0, v0, col);
    put(v + 4, p2_, u1, v1, col);
    put(v + 5, p3, u0, v1, col);
    add_cmd(c, CMD_DRAW, MODE_IMAGE, img, first, 6, 0);
}

/* ------------------------------------------------------------------ GL */

typedef struct {
    int draw_fbo, read_fbo, viewport[4], program, vao, array_buffer, active_texture, texture0, renderbuffer;
    int blend, depth, stencil, cull, scissor;
    int blend_src_rgb, blend_dst_rgb, blend_src_a, blend_dst_a, blend_eq_rgb, blend_eq_a;
    unsigned char color_mask[4];
    int stencil_writemask, unpack_alignment;
    float clear_color[4];
    int clear_stencil;
} GlSave;

static void gl_save(GlSave *s) {
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &s->draw_fbo);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &s->read_fbo);
    glGetIntegerv(GL_VIEWPORT, s->viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &s->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &s->vao);
    glGetIntegerv(GL_ARRAY_BUFFER_BINDING, &s->array_buffer);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &s->active_texture);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &s->texture0);
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &s->renderbuffer);
    s->blend = glIsEnabled(GL_BLEND);
    s->depth = glIsEnabled(GL_DEPTH_TEST);
    s->stencil = glIsEnabled(GL_STENCIL_TEST);
    s->cull = glIsEnabled(GL_CULL_FACE);
    s->scissor = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_BLEND_SRC_RGB, &s->blend_src_rgb);
    glGetIntegerv(GL_BLEND_DST_RGB, &s->blend_dst_rgb);
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &s->blend_src_a);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &s->blend_dst_a);
    glGetIntegerv(GL_BLEND_EQUATION_RGB, &s->blend_eq_rgb);
    glGetIntegerv(GL_BLEND_EQUATION_ALPHA, &s->blend_eq_a);
    GLboolean mask[4];
    glGetBooleanv(GL_COLOR_WRITEMASK, mask);
    for (int i = 0; i < 4; i++) s->color_mask[i] = mask[i];
    glGetIntegerv(GL_STENCIL_WRITEMASK, &s->stencil_writemask);
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &s->unpack_alignment);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, s->clear_color);
    glGetIntegerv(GL_STENCIL_CLEAR_VALUE, &s->clear_stencil);
}

static void gl_enable(const GLenum cap, const int on) {
    if (on) glEnable(cap);
    else glDisable(cap);
}

static void gl_restore(const GlSave *s) {
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, (GLuint) s->draw_fbo);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) s->read_fbo);
    glViewport(s->viewport[0], s->viewport[1], s->viewport[2], s->viewport[3]);
    glUseProgram((GLuint) s->program);
    glBindVertexArray((GLuint) s->vao);
    glBindBuffer(GL_ARRAY_BUFFER, (GLuint) s->array_buffer);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint) s->texture0);
    glActiveTexture((GLenum) s->active_texture);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint) s->renderbuffer);
    gl_enable(GL_BLEND, s->blend);
    gl_enable(GL_DEPTH_TEST, s->depth);
    gl_enable(GL_STENCIL_TEST, s->stencil);
    gl_enable(GL_CULL_FACE, s->cull);
    gl_enable(GL_SCISSOR_TEST, s->scissor);
    glBlendFuncSeparate((GLenum) s->blend_src_rgb, (GLenum) s->blend_dst_rgb, (GLenum) s->blend_src_a, (GLenum) s->blend_dst_a);
    glBlendEquationSeparate((GLenum) s->blend_eq_rgb, (GLenum) s->blend_eq_a);
    glColorMask(s->color_mask[0], s->color_mask[1], s->color_mask[2], s->color_mask[3]);
    glStencilMask((GLuint) s->stencil_writemask);
    glPixelStorei(GL_UNPACK_ALIGNMENT, s->unpack_alignment);
    glClearColor(s->clear_color[0], s->clear_color[1], s->clear_color[2], s->clear_color[3]);
    glClearStencil(s->clear_stencil);
}

static void gl_free_layer(Canvas2D *c) {
    if (c->msaa_fbo) glDeleteFramebuffers(1, &c->msaa_fbo);
    if (c->layer_fbo) glDeleteFramebuffers(1, &c->layer_fbo);
    if (c->msaa_color) glDeleteRenderbuffers(1, &c->msaa_color);
    if (c->msaa_ds) glDeleteRenderbuffers(1, &c->msaa_ds);
    if (c->layer_ds) glDeleteRenderbuffers(1, &c->layer_ds);
    if (c->layer_tex) glDeleteTextures(1, &c->layer_tex);
    c->msaa_fbo = c->layer_fbo = c->msaa_color = c->msaa_ds = c->layer_ds = c->layer_tex = 0;
    c->layer_w = c->layer_h = 0;
}

static int gl_make_layer(Canvas2D *c) {
    if (c->layer_tex && c->layer_w == c->device_w && c->layer_h == c->device_h) return 1;
    gl_free_layer(c);
    const int w = c->device_w, h = c->device_h;
    if (w <= 0 || h <= 0) return 0;

    glGenTextures(1, &c->layer_tex);
    glBindTexture(GL_TEXTURE_2D, c->layer_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glGenFramebuffers(1, &c->layer_fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, c->layer_fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, c->layer_tex, 0);

    if (c->samples > 1) {
        glGenRenderbuffers(1, &c->msaa_color);
        glBindRenderbuffer(GL_RENDERBUFFER, c->msaa_color);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, c->samples, GL_RGBA8, w, h);
        glGenRenderbuffers(1, &c->msaa_ds);
        glBindRenderbuffer(GL_RENDERBUFFER, c->msaa_ds);
        glRenderbufferStorageMultisample(GL_RENDERBUFFER, c->samples, GL_DEPTH24_STENCIL8, w, h);
        glGenFramebuffers(1, &c->msaa_fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, c->msaa_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, c->msaa_color);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c->msaa_ds);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "canvas2d: %dx MSAA layer incomplete, using no MSAA\n", c->samples);
            glDeleteFramebuffers(1, &c->msaa_fbo);
            glDeleteRenderbuffers(1, &c->msaa_color);
            glDeleteRenderbuffers(1, &c->msaa_ds);
            c->msaa_fbo = c->msaa_color = c->msaa_ds = 0;
            c->samples = 0;
        }
    }
    if (c->samples <= 1) {
        glGenRenderbuffers(1, &c->layer_ds);
        glBindRenderbuffer(GL_RENDERBUFFER, c->layer_ds);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
        glBindFramebuffer(GL_FRAMEBUFFER, c->layer_fbo);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, c->layer_ds);
    }
    glBindFramebuffer(GL_FRAMEBUFFER, c->layer_fbo);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "canvas2d: layer framebuffer incomplete\n");
        gl_free_layer(c);
        return 0;
    }
    c->layer_w = w;
    c->layer_h = h;
    c->pending_clear = 1;
    return 1;
}

static int gl_init(Canvas2D *c) {
    if (c->gl_ready) return 1;
    if (c->gl_failed) return 0;
    c->gl_failed = 1;
    char *vs = concat(assets_path(), "shaders/canvas2d.vert");
    char *fs = concat(assets_path(), "shaders/canvas2d.frag");
    if (vs && fs) {
        ensure_asset_file(vs);
        ensure_asset_file(fs);
        c->program = shader_compile(vs, fs);
    }
    free(vs);
    free(fs);
    if (!c->program) {
        fprintf(stderr, "canvas2d: shader compile failed\n");
        return 0;
    }
    c->loc_viewport = glGetUniformLocation(c->program, "viewport_size");
    c->loc_mode = glGetUniformLocation(c->program, "mode");
    c->loc_tex = glGetUniformLocation(c->program, "tex");

    glGenVertexArrays(1, &c->vao);
    glGenBuffers(1, &c->vbo);
    glBindVertexArray(c->vao);
    glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *) offsetof(Vtx, x));
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *) offsetof(Vtx, u));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vtx), (void *) offsetof(Vtx, r));
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);

    glGenTextures(1, &c->atlas_tex);
    glBindTexture(GL_TEXTURE_2D, c->atlas_tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, C2D_GLYPH_ATLAS_SIZE, C2D_GLYPH_ATLAS_SIZE, 0, GL_RED, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    if (c->atlas) c->atlas_dirty_y0 = 0, c->atlas_dirty_y1 = C2D_GLYPH_ATLAS_SIZE; // whole CPU copy

    int max_samples = 0;
    glGetIntegerv(GL_MAX_SAMPLES, &max_samples);
    int samples = C2D_DEFAULT_MSAA_SAMPLES;
    const char *env = getenv("KRUNKNATIVE_CANVAS_MSAA");
    if (env && *env) samples = atoi(env);
    if (samples > max_samples) samples = max_samples;
    c->samples = samples > 1 ? samples : 0;

    c->gl_ready = 1;
    c->gl_failed = 0;
    return 1;
}

static void gl_upload(Canvas2D *c) {
    if (c->atlas && c->atlas_dirty_y1 > c->atlas_dirty_y0) {
        glBindTexture(GL_TEXTURE_2D, c->atlas_tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
        glTexSubImage2D(GL_TEXTURE_2D, 0, 0, c->atlas_dirty_y0, C2D_GLYPH_ATLAS_SIZE, c->atlas_dirty_y1 - c->atlas_dirty_y0,
                        GL_RED, GL_UNSIGNED_BYTE, c->atlas + (size_t) c->atlas_dirty_y0 * C2D_GLYPH_ATLAS_SIZE);
        c->atlas_dirty_y0 = C2D_GLYPH_ATLAS_SIZE;
        c->atlas_dirty_y1 = 0;
    }
    for (C2DImage *img = c->images; img; img = img->next) {
        if (img->state != 1 || img->tex || !img->pixels) continue;
        glGenTextures(1, &img->tex);
        glBindTexture(GL_TEXTURE_2D, img->tex);
        glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, img->w, img->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->pixels);
        glGenerateMipmap(GL_TEXTURE_2D);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        stbi_image_free(img->pixels);
        img->pixels = NULL;
    }
    const size_t bytes = c->nverts * sizeof(Vtx);
    glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
    if (bytes > c->vbo_cap) {
        c->vbo_cap = bytes * 2;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) c->vbo_cap, NULL, GL_STREAM_DRAW);
    } else {
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) c->vbo_cap, NULL, GL_STREAM_DRAW); // orphan
    }
    if (bytes) glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr) bytes, c->verts);
}

static void bind_mode(Canvas2D *c, const int mode, const unsigned int texture, int *cur_mode, unsigned int *cur_tex) {
    if (mode != *cur_mode) {
        glUniform1i(c->loc_mode, mode);
        *cur_mode = mode;
    }
    if (texture && texture != *cur_tex) {
        glBindTexture(GL_TEXTURE_2D, texture);
        *cur_tex = texture;
    }
}

void c2d_flush(Canvas2D *c) {
    // A pending clear without new commands waits: the layer is not shown meanwhile (layer_dirty 0).
    if (!c->ncmds) return;
    if (c->device_w <= 0 || c->device_h <= 0) {
        c->ncmds = c->nverts = 0;
        return;
    }
    GlSave saved;
    gl_save(&saved);
    if (!gl_init(c) || !gl_make_layer(c)) {
        gl_restore(&saved);
        c->ncmds = c->nverts = 0;
        return;
    }
    const unsigned int target = c->samples > 1 ? c->msaa_fbo : c->layer_fbo;
    glBindFramebuffer(GL_FRAMEBUFFER, target);
    glViewport(0, 0, c->device_w, c->device_h);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glStencilMask(0xFF);
    if (c->pending_clear) {
        glClearColor(0, 0, 0, 0);
        glClearStencil(0);
        glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
        c->pending_clear = 0;
        c->layer_dirty = 0;
    }

    int draws = 0;
    if (c->ncmds) {
        glUseProgram(c->program);
        glBindVertexArray(c->vao);
        glActiveTexture(GL_TEXTURE0);
        gl_upload(c);
        glUniform2f(c->loc_viewport, (float) c->device_w, (float) c->device_h);
        glUniform1i(c->loc_tex, 0); // sampler: texture unit 0
        int cur_mode = -1;
        unsigned int cur_tex = 0;
        glEnable(GL_BLEND);
        glBlendEquation(GL_FUNC_ADD);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA); // premultiplied source-over

        for (size_t i = 0; i < c->ncmds; i++) {
            const Cmd *cmd = &c->cmds[i];
            const unsigned int tex = cmd->mode == MODE_GLYPH ? c->atlas_tex : cmd->mode == MODE_IMAGE ? cmd->image->tex : 0;
            bind_mode(c, cmd->mode, tex, &cur_mode, &cur_tex);
            switch (cmd->kind) {
                case CMD_DRAW:
                    glDrawArrays(GL_TRIANGLES, (GLint) cmd->first, (GLsizei) cmd->count);
                    draws++;
                    break;
                case CMD_CLEAR:
                    glDisable(GL_BLEND);
                    glDrawArrays(GL_TRIANGLES, (GLint) cmd->first, (GLsizei) cmd->count);
                    glEnable(GL_BLEND);
                    draws++;
                    break;
                case CMD_FILL:
                case CMD_UNION:
                    glEnable(GL_STENCIL_TEST);
                    glColorMask(GL_FALSE, GL_FALSE, GL_FALSE, GL_FALSE);
                    if (cmd->kind == CMD_FILL) {
                        glStencilFunc(GL_ALWAYS, 0, 0xFF);
                        glStencilOpSeparate(GL_FRONT, GL_KEEP, GL_KEEP, GL_INCR_WRAP);
                        glStencilOpSeparate(GL_BACK, GL_KEEP, GL_KEEP, GL_DECR_WRAP);
                    } else {
                        glStencilFunc(GL_ALWAYS, 1, 0xFF);
                        glStencilOp(GL_KEEP, GL_KEEP, GL_REPLACE);
                    }
                    glDrawArrays(GL_TRIANGLES, (GLint) cmd->first, (GLsizei) cmd->count);
                    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
                    glStencilFunc(GL_NOTEQUAL, 0, 0xFF);
                    glStencilOp(GL_ZERO, GL_ZERO, GL_ZERO); // cover resets the stencil
                    glDrawArrays(GL_TRIANGLES, (GLint) cmd->cover_first, 6);
                    glDisable(GL_STENCIL_TEST);
                    draws += 2;
                    break;
            }
        }
        c->layer_dirty = 1;
    }
    if (c->samples > 1) {
        glBindFramebuffer(GL_READ_FRAMEBUFFER, c->msaa_fbo);
        glBindFramebuffer(GL_DRAW_FRAMEBUFFER, c->layer_fbo);
        glBlitFramebuffer(0, 0, c->device_w, c->device_h, 0, 0, c->device_w, c->device_h, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    }
    gl_restore(&saved);
    c->stat_cmds = (int) c->ncmds;
    c->stat_verts = (int) c->nverts;
    c->stat_draws = draws;
    c->ncmds = c->nverts = 0;
}

int c2d_is_empty(const Canvas2D *c) { return !c->layer_dirty && !c->ncmds; }

void c2d_composite(Canvas2D *c) {
    c2d_flush(c);
    if (!c->layer_dirty || !c->gl_ready || !c->layer_tex) return;
    GlSave saved;
    gl_save(&saved);
    const float w = (float) saved.viewport[2], h = (float) saved.viewport[3];
    const float white[4] = {1, 1, 1, 1};
    Vtx quad[6];
    put(quad + 0, (P2) {0, 0}, 0, 1, white);
    put(quad + 1, (P2) {w, 0}, 1, 1, white);
    put(quad + 2, (P2) {w, h}, 1, 0, white);
    put(quad + 3, (P2) {0, 0}, 0, 1, white);
    put(quad + 4, (P2) {w, h}, 1, 0, white);
    put(quad + 5, (P2) {0, h}, 0, 0, white);
    glUseProgram(c->program);
    glBindVertexArray(c->vao);
    glBindBuffer(GL_ARRAY_BUFFER, c->vbo);
    if (c->vbo_cap < sizeof(quad)) {
        c->vbo_cap = sizeof(quad) * 64;
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr) c->vbo_cap, NULL, GL_STREAM_DRAW);
    }
    glBufferSubData(GL_ARRAY_BUFFER, 0, sizeof(quad), quad);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, c->layer_tex);
    glUniform2f(c->loc_viewport, w, h);
    glUniform1i(c->loc_tex, 0);
    glUniform1i(c->loc_mode, MODE_IMAGE);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_STENCIL_TEST);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glEnable(GL_BLEND);
    glBlendEquation(GL_FUNC_ADD);
    glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_ALPHA, GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glDrawArrays(GL_TRIANGLES, 0, 6);
    gl_restore(&saved);
}

void c2d_last_flush_stats(const Canvas2D *c, int *commands, int *vertices, int *draw_calls) {
    if (commands) *commands = c->stat_cmds;
    if (vertices) *vertices = c->stat_verts;
    if (draw_calls) *draw_calls = c->stat_draws;
}

void c2d_destroy(Canvas2D *c) {
    if (!c) return;
    if (c->gl_ready) {
        gl_free_layer(c);
        glDeleteProgram(c->program);
        gl_uniform_forget_program(c->program);
        glDeleteVertexArrays(1, &c->vao);
        glDeleteBuffers(1, &c->vbo);
        glDeleteTextures(1, &c->atlas_tex);
    }
    for (C2DImage *img = c->images; img;) {
        C2DImage *next = img->next;
        if (img->tex && c->gl_ready) glDeleteTextures(1, &img->tex);
        if (img->pixels) stbi_image_free(img->pixels);
        free(img->path);
        free(img);
        img = next;
    }
    if (c->face) FT_Done_Face(c->face);
    if (c->ft) FT_Done_FreeType(c->ft);
    free(c->advances);
    free(c->atlas);
    free(c->glyph_table);
    path_free(&c->path);
    path_free(&c->scratch);
    free(c->tris);
    free(c->verts);
    free(c->cmds);
    free(c->stack);
    free(c);
}
