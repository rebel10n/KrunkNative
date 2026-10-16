/*
 * paint.c - display-list producer (CSS 2.1 Appendix E painting order).
 *
 * Structure
 *   1. math helpers
 *   2. box classification + coordinates (absolute layout space -> context frame)
 *   3. per-stacking-context cache table
 *   4. decorations: box-shadow, backgrounds, borders
 *   5. text runs, replaced content, widgets, scrollbars
 *   6. clips, transforms, groups
 *   7. painting phases and stacking contexts
 *   8. dirty tracking, build and flatten
 *
 * Coordinates: layout.h geometry is absolute (viewport origin, before
 * ancestor scroll offsets and transforms).  Each stacking context is
 * recorded in a frame whose origin is its root's border-box origin; the
 * parent wraps the child's items in DL_PUSH_TRANSFORM(translate).  Scroll
 * offsets of scroll containers inside the frame are either emitted as
 * DL_SCROLL_OFFSET (when their overflow clip is pushed around their content)
 * or folded into positions (for positioned descendants painted from the
 * context's layer lists, whose containing-block chain is walked instead).
 */
#include "paint.h"
#include "css_values.h"
#include "text.h"
#include "image.h"
#include "util.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define PAINT_MAX_BG_TILES 1024
#define PAINT_CACHE_ARENA_BLOCK (16 * 1024)
#define PAINT_MAX_SCROLL_STACK 32
#define PAINT_MAX_ANCESTOR_CLIPS 32

/* ------------------------------------------------------------------------- */
/* 1. Math                                                                    */
/* ------------------------------------------------------------------------- */

static Mat2D mat_identity(void)
{
    Mat2D m = {1, 0, 0, 1, 0, 0};
    return m;
}

/* r = a * b : apply b first, then a (CSS transform list order "a b"). */
static Mat2D mat_mul(Mat2D a, Mat2D b)
{
    Mat2D r;
    r.a = a.a * b.a + a.c * b.b;
    r.b = a.b * b.a + a.d * b.b;
    r.c = a.a * b.c + a.c * b.d;
    r.d = a.b * b.c + a.d * b.d;
    r.e = a.a * b.e + a.c * b.f + a.e;
    r.f = a.b * b.e + a.d * b.f + a.f;
    return r;
}

static Mat2D mat_translate(float tx, float ty)
{
    Mat2D m = {1, 0, 0, 1, tx, ty};
    return m;
}

static Mat2D mat_scale(float sx, float sy)
{
    Mat2D m = {sx, 0, 0, sy, 0, 0};
    return m;
}

static Mat2D mat_rotate(float deg)
{
    const float r = (float) (deg * M_PI / 180.0);
    const float c = cosf(r), s = sinf(r);
    Mat2D m = {c, s, -s, c, 0, 0};
    return m;
}

static Mat2D mat_skew(float ax_deg, float ay_deg)
{
    Mat2D m = {1, tanf((float) (ay_deg * M_PI / 180.0)), tanf((float) (ax_deg * M_PI / 180.0)), 1, 0, 0};
    return m;
}

static int mat_is_identity(Mat2D m)
{
    return m.a == 1 && m.b == 0 && m.c == 0 && m.d == 1 && m.e == 0 && m.f == 0;
}

static float mat_scale_factor(Mat2D m)
{
    return sqrtf(fabsf(m.a * m.d - m.b * m.c));
}

static void mat_apply(Mat2D m, float x, float y, float *ox, float *oy)
{
    *ox = m.a * x + m.c * y + m.e;
    *oy = m.b * x + m.d * y + m.f;
}

static float length_resolve(CssLength l, float ref)
{
    if (l.kind != CSS_LEN_VALUE) return 0;
    return l.px + l.pct * ref / 100.0f;
}

static int rect_empty(DlRect r) { return r.w <= 0 || r.h <= 0; }

static DlRect rect_union(DlRect a, DlRect b)
{
    if (rect_empty(a)) return b;
    if (rect_empty(b)) return a;
    const float x0 = fminf(a.x, b.x), y0 = fminf(a.y, b.y);
    const float x1 = fmaxf(a.x + a.w, b.x + b.w), y1 = fmaxf(a.y + a.h, b.y + b.h);
    return dl_rect(x0, y0, x1 - x0, y1 - y0);
}

static DlRect rect_offset(DlRect r, float dx, float dy) { return dl_rect(r.x + dx, r.y + dy, r.w, r.h); }
static DlRect rect_expand(DlRect r, float e) { return dl_rect(r.x - e, r.y - e, r.w + 2 * e, r.h + 2 * e); }

static DlRect rect_inset(DlRect r, float t, float rt, float b, float l)
{
    return dl_rect(r.x + l, r.y + t, r.w - l - rt, r.h - t - b);
}

static int rect_contains(DlRect outer, DlRect inner)
{
    return inner.x >= outer.x && inner.y >= outer.y &&
           inner.x + inner.w <= outer.x + outer.w && inner.y + inner.h <= outer.y + outer.h;
}

static DlRect rect_transform(Mat2D m, DlRect r)
{
    float xs[4], ys[4];
    mat_apply(m, r.x, r.y, &xs[0], &ys[0]);
    mat_apply(m, r.x + r.w, r.y, &xs[1], &ys[1]);
    mat_apply(m, r.x, r.y + r.h, &xs[2], &ys[2]);
    mat_apply(m, r.x + r.w, r.y + r.h, &xs[3], &ys[3]);
    float x0 = xs[0], x1 = xs[0], y0 = ys[0], y1 = ys[0];
    for (int i = 1; i < 4; i++) {
        x0 = fminf(x0, xs[i]); x1 = fmaxf(x1, xs[i]);
        y0 = fminf(y0, ys[i]); y1 = fmaxf(y1, ys[i]);
    }
    return dl_rect(x0, y0, x1 - x0, y1 - y0);
}

static int radii_zero(DlRadii r) { return r.tl <= 0 && r.tr <= 0 && r.br <= 0 && r.bl <= 0; }

/* Uniform radius `r` on a w x h rect, scaled so adjacent radii fit (CSS
 * Backgrounds 5.5; paint_types.h requires producer-clamped radii). */
static DlRadii radii_uniform_clamped(float r, float w, float h)
{
    if (r <= 0 || w <= 0 || h <= 0) return dl_radii_all(0);
    const float m = fminf(w, h) * 0.5f;
    return dl_radii_all(r > m ? m : r);
}

static CssColor color_rgba(uint8_t r, uint8_t g, uint8_t b, uint8_t a)
{
    CssColor c = {r, g, b, a, CSS_COLOR_RGBA};
    return c;
}

static CssColor color_scale_alpha(CssColor c, float k)
{
    float a = c.a * k;
    if (a < 0) a = 0;
    if (a > 255) a = 255;
    c.a = (uint8_t) (a + 0.5f);
    return c;
}

CssColor paint_resolve_color(CssColor c, const CssComputed *style)
{
    if (c.kind != CSS_COLOR_RGBA) {
        CssColor cur = style ? style->color : color_rgba(0, 0, 0, 255);
        cur.kind = CSS_COLOR_RGBA;
        return css_color_resolve(c, cur);   /* currentColor, color-mix(.. currentColor ..) */
    }
    return c;
}

/* ------------------------------------------------------------------------- */
/* 2. Box classification and coordinates                                      */
/* ------------------------------------------------------------------------- */

static int box_is_text(const Box *b) { return b->type == BOX_TEXT; }
static int box_is_inline(const Box *b) { return b->type == BOX_INLINE; }
/* box_is_replaced() comes from layout.h */
static int box_is_widget(const Box *b) { return b->type == BOX_INPUT || b->type == BOX_SELECT; }
static int box_is_image(const Box *b) { return b->type == BOX_IMAGE; }

/* Inline-level content is painted through line boxes, never from the block walks. */
static int box_inline_level(const Box *b)
{
    return b->type == BOX_INLINE || b->type == BOX_TEXT || b->type == BOX_LINEBREAK || (b->flags & BOXF_INLINE_LEVEL);
}

/* Anonymous boxes carry only inherited properties (CSS 2.1 §9.2.1.1), but
 * layout may hand them the parent's computed record: paint ignores every
 * non-inherited property of an anonymous box (background, border, shadow,
 * transform, opacity, filter, clip-path, overflow, position, z-index). */
static int box_anon(const Box *b) { return (b->flags & BOXF_ANONYMOUS) != 0 && b->parent != NULL; }

static int box_positioned(const Box *b) { return (b->flags & BOXF_POSITIONED) != 0 && !box_anon(b); }
static int box_fixed(const Box *b) { return !box_anon(b) && b->style && b->style->position == CSS_POSITION_FIXED; }
static int box_float(const Box *b) { return (b->flags & BOXF_FLOAT) != 0 && !(b->flags & BOXF_OUT_OF_FLOW) && !box_anon(b); }
static int box_visible(const Box *b) { return !b->style || b->style->visibility == CSS_VISIBILITY_VISIBLE; }
static int box_is_scroller(const Box *b) { return (b->flags & BOXF_SCROLL_CONTAINER) != 0; }

/* overflow != visible on the box itself (ignoring viewport propagation). */
static int box_own_overflow_clip(const Box *b)
{
    if (box_anon(b)) return 0;
    if (b->flags & BOXF_SCROLL_CONTAINER) return 1;
    const CssComputed *s = b->style;
    if (!s || box_is_text(b) || box_is_inline(b)) return 0;
    return s->overflow_x != CSS_OVERFLOW_VISIBLE || s->overflow_y != CSS_OVERFLOW_VISIBLE;
}

/* CSS Overflow 3 §3.3: the root element's overflow applies to the viewport;
 * when it is `visible`, <body>'s value propagates instead and <body> itself
 * no longer clips.  Returns the box whose overflow the viewport uses (NULL
 * when the viewport does not clip). */
static const Box *viewport_overflow_source(const PaintCtx *ctx, const Box *root)
{
    if (box_own_overflow_clip(root)) return root;
    const Box *body = ctx->body;
    if (body && body->parent == root && box_own_overflow_clip(body)) return body;
    return NULL;
}

static int is_root_box(const Box *b) { return b->parent == NULL; }

static int box_has_overflow_clip(const PaintCtx *ctx, const Box *b)
{
    if (is_root_box(b)) return viewport_overflow_source(ctx, b) != NULL;
    if (b == ctx->body && b->parent && is_root_box(b->parent) && !box_own_overflow_clip(b->parent)) return 0;
    return box_own_overflow_clip(b);
}

/* transform != none: 2D ops, or only 3D functions (translateZ(0) -> identity). */
static int box_has_transform(const Box *b)
{
    return !box_anon(b) && b->style && (b->style->transform.count > 0 || b->style->transform.has_3d);
}

/* Elements Chrome promotes to composited layers (translateZ / 3D transform,
 * will-change: transform): stacking context, fixed containing block and their
 * own pixel-snapping space (their origin itself is not snapped, see
 * flatten_context). */
static int box_composited(const Box *b)
{
    return !box_anon(b) && b->style &&
           (b->style->transform.has_3d || (b->style->will_change & CSS_WILL_CHANGE_TRANSFORM));
}

int paint_is_stacking_context(const Box *b)
{
    if (box_is_text(b)) return 0;
    if (!b->parent) return 1;
    if (box_anon(b)) return 0;
    if (b->flags & BOXF_STACKING_CONTEXT) return 1;
    const CssComputed *s = b->style;
    if (!s) return 0;
    if (box_positioned(b) && !b->z_auto) return 1;
    if (s->position == CSS_POSITION_FIXED || s->position == CSS_POSITION_STICKY) return 1;
    if (s->opacity < 1.0f || s->transform.count > 0 || s->transform.has_3d || s->filter_count > 0) return 1;
    if (s->will_change & CSS_WILL_CHANGE_TRANSFORM) return 1;
    if (s->clip_path.kind != CSS_CLIP_PATH_NONE) return 1;
    /* grouping properties (CSS Transforms 2 / Filter Effects 2 / Masking) */
    if (s->backdrop_filter_count > 0 || s->mask_layer_count > 0) return 1;
    if (s->mix_blend_mode != CSS_BLEND_NORMAL) return 1;
    if (s->transform_style == CSS_TRANSFORM_STYLE_PRESERVE_3D) return 1;
    return 0;
}

/* A box painted as its own unit outside the in-flow walks. */
static int box_layer_root(const Box *b)
{
    return box_positioned(b) || box_float(b) || paint_is_stacking_context(b);
}

/* z-index applies to positioned boxes and to flex / grid items. */
static int32_t box_z(const Box *b)
{
    if (b->z_auto || box_anon(b)) return 0;
    if (box_positioned(b)) return b->z_index;
    if (b->parent && (b->parent->type == BOX_FLEX || b->parent->type == BOX_GRID)) return b->z_index;
    return 0;
}

static int box_catches_fixed(const Box *b)
{
    return b->parent == NULL || box_has_transform(b) || box_composited(b);
}

/* Painter state ---------------------------------------------------------- */

typedef struct PaintCache PaintCache;

typedef struct Painter {
    PaintCtx *ctx;
    PaintCache *cache;
    DisplayList *dl;
    const Box *root;        /* context root: the frame origin is its border-box origin */
    float scale;            /* effective device px per CSS px inside this frame */
    Mat2D dev_frame;        /* frame (context root border-box origin) -> device px */
    Mat2D root_xform;       /* the context root's own transform, pushed first in the list */
    float snap_ox, snap_oy; /* frame origin inside the nearest transform space (pixel snapping) */
    const Box *scroll_emitted[PAINT_MAX_SCROLL_STACK];
    float scroll_snap_x[PAINT_MAX_SCROLL_STACK];  /* composited-scroller origin snap (CSS px), see */
    float scroll_snap_y[PAINT_MAX_SCROLL_STACK];  /* scroller_layer_snap(); part of DL_SCROLL_OFFSET */
    int n_scroll_emitted;
    int no_layer_snap;      /* scroller_layer_snap() off: positions in pure layout space */
    float text_layer_snap_y;/* folded scroller-layer snap in the current text run's y (paint_text_run) */
    /* inline boxes whose decorations were painted during the current line walk */
    const Box **seen;
    int n_seen, cap_seen;
    int glyph_only;         /* background-clip: text mask pass: text runs as opaque glyphs, nothing else */
    int oom;
} Painter;

static int scroll_is_emitted(const Painter *p, const Box *c)
{
    for (int i = 0; i < p->n_scroll_emitted; i++)
        if (p->scroll_emitted[i] == c) return 1;
    return 0;
}

/* In-list transform state: the root's own transform followed by every
 * DL_SCROLL_OFFSET currently in effect (all translations, order-free). */
static Mat2D painter_local_matrix(const Painter *p)
{
    float sx = 0, sy = 0;
    for (int i = 0; i < p->n_scroll_emitted; i++) {
        sx += p->scroll_emitted[i]->scroll_x - p->scroll_snap_x[i];
        sy += p->scroll_emitted[i]->scroll_y - p->scroll_snap_y[i];
    }
    return mat_mul(p->root_xform, mat_translate(-sx, -sy));
}

/* Scroll offsets that apply to `b` inside the current frame and were not
 * emitted as DL_SCROLL_OFFSET: scroll containers on b's containing-block
 * chain from its container up to (and including) the frame root. */
static void scroller_layer_snap(const Painter *p, const Box *b, float *cx, float *cy);

/* A scroll offset folded into positions instead of emitted: the scroll
 * itself plus the scroller's composited-layer origin snap, so positioned
 * descendants painted outside its clip land where its in-flow content does. */
static void fold_scroll(const Painter *p, const Box *c, float *dx, float *dy)
{
    float cx, cy;
    scroller_layer_snap(p, c, &cx, &cy);
    *dx += cx - c->scroll_x;
    *dy += cy - c->scroll_y;
}

static void scroll_shift(const Painter *p, const Box *b, float *dx, float *dy)
{
    *dx = 0;
    *dy = 0;
    if (box_fixed(b)) return;
    const Box *above = p->root ? p->root->container : NULL;
    /* The frame root's container is outside this frame (an inline context
     * root's lines live on it): scrolls above the frame were already applied
     * by the parent list's DL_SCROLL_OFFSET, so nothing to compensate. */
    if (above && b == above) return;
    for (const Box *c = b->container; c && c != above; c = c->container) {
        if (box_is_scroller(c) && !scroll_is_emitted(p, c)) fold_scroll(p, c, dx, dy);
        if (c == p->root) break;
    }
}

/* Absolute layout point -> current frame. */
static void to_frame(const Painter *p, float ax, float ay, float *x, float *y)
{
    *x = ax - (p->root ? p->root->x : 0);
    *y = ay - (p->root ? p->root->y : 0);
}

/* Border-box origin of `b` in the frame. */
static void pos_rel(const Painter *p, const Box *b, float *x, float *y)
{
    float sx, sy;
    to_frame(p, b->x, b->y, x, y);
    scroll_shift(p, b, &sx, &sy);
    *x += sx;
    *y += sy;
}

/* Chrome paints box decorations with pixel-snapped rects (x' = round(x),
 * w' = round(x + w) - round(x)) in the coordinate space of the nearest
 * transformed layer, i.e. in CSS px before #uiBase's scale.  Frames of
 * non-transformed nested contexts sit at (snap_ox, snap_oy) in that space. */
static DlRect snap_rect(const Painter *p, DlRect r)
{
    const float x0 = roundf(r.x + p->snap_ox), y0 = roundf(r.y + p->snap_oy);
    const float x1 = roundf(r.x + r.w + p->snap_ox), y1 = roundf(r.y + r.h + p->snap_oy);
    return dl_rect(x0 - p->snap_ox, y0 - p->snap_oy, x1 - x0, y1 - y0);
}

static DlRect box_rect(const Painter *p, const Box *b)
{
    float x, y;
    pos_rel(p, b, &x, &y);
    /* A box with a CSS filter, or inside one (a drop-shadow button and its :before / :after fills), paints its top
     * edge rounded to whole CSS px and its bottom edge where layout puts it: FRVR's live customize Back button (layout
     * 17.5..50.5 CSS px) paints device rows 36..100 at DPR 2, not 36..101 (measured on the live page, 2026-10-16). */
    if ((b->style && b->style->filter_count > 0) || (b->container && b->container->style && b->container->style->filter_count > 0)) {
        const DlRect snapped = snap_rect(p, dl_rect(x, y, b->w, b->h));
        const float y0 = roundf(y + p->snap_oy) - p->snap_oy;
        /* the bottom edge rounds to whole device px (at DPR 2 a half-CSS-px bottom is a whole device px: rows 36..100) */
        const float sc = p->scale > 0 ? p->scale : 1.0f;
        const float y1 = roundf((y + b->h + p->snap_oy) * sc) / sc - p->snap_oy;
        return dl_rect(snapped.x, y0, snapped.w, y1 - y0);
    }
    return snap_rect(p, dl_rect(x, y, b->w, b->h));
}

/* Shift for content owned by `c` (line boxes, fragments, runs): its own
 * scroll offset applies on top of its containing-block chain. */
static void content_shift(const Painter *p, const Box *c, float *dx, float *dy)
{
    scroll_shift(p, c, dx, dy);
    if (p->root && c == p->root->container) return;   /* outside the frame (see scroll_shift) */
    if (box_is_scroller(c) && !scroll_is_emitted(p, c)) fold_scroll(p, c, dx, dy);
}

static DlRect padding_rect_of(DlRect r, const Box *b)
{
    return rect_inset(r, b->border[CSS_TOP], b->border[CSS_RIGHT], b->border[CSS_BOTTOM], b->border[CSS_LEFT]);
}

static DlRect content_rect_of(DlRect r, const Box *b)
{
    DlRect c = rect_inset(padding_rect_of(r, b), b->padding[CSS_TOP], b->padding[CSS_RIGHT], b->padding[CSS_BOTTOM], b->padding[CSS_LEFT]);
    if (b->scrollbar_w > 0) {
        c.w -= b->scrollbar_w;
        if (b->flags & BOXF_RTL) c.x += b->scrollbar_w;
    }
    if (b->scrollbar_h > 0) c.h -= b->scrollbar_h;
    return c;
}

/* ------------------------------------------------------------------------- */
/* 3. Cache table                                                             */
/* ------------------------------------------------------------------------- */

typedef struct PaintChildRef {
    int index;      /* list index the child context is flattened before */
    Box *box;
    float dx, dy;   /* child frame origin in this frame */
    Mat2D local;    /* in-list transform state at the marker (root transform + emitted scroll offsets) */
    float scroll_x, scroll_y;   /* emitted scroll offsets in effect at the marker */
    float layer_snap_x, layer_snap_y; /* part of dx/dy from folded scroller-layer snaps (not layout space) */
} PaintChildRef;

struct PaintCache {
    Box *box;
    DisplayList list;
    KnArena arena;
    PaintChildRef *children;
    int n_children, cap_children;
    Mat2D dev_in;       /* frame -> device px the list was recorded for */
    float snap_ox, snap_oy;     /* frame origin in its transform space when recorded */
    uint64_t hash;      /* style/order hash the list was recorded for */
    uint32_t gen;       /* last frame this cache was used */
    uint8_t valid;
    uint8_t dirty;
};

struct PaintCacheTable {
    PaintCache **slots;
    size_t cap, count;
    PaintCache *current;    /* context being recorded */
};

static size_t cache_hash(const Box *b)
{
    uint64_t h = (uint64_t) (uintptr_t) b;
    h ^= h >> 33;
    h *= 0xff51afd7ed558ccdULL;
    h ^= h >> 33;
    return (size_t) h;
}

static void cache_free(PaintCache *c)
{
    dl_free(&c->list);
    kn_arena_free(&c->arena);
    free(c->children);
    free(c);
}

static PaintCache *table_find(struct PaintCacheTable *t, const Box *b)
{
    if (!t->cap) return NULL;
    size_t i = cache_hash(b) & (t->cap - 1);
    for (;;) {
        PaintCache *c = t->slots[i];
        if (!c) return NULL;
        if (c->box == b) return c;
        i = (i + 1) & (t->cap - 1);
    }
}

static void table_insert_raw(struct PaintCacheTable *t, PaintCache *c)
{
    size_t i = cache_hash(c->box) & (t->cap - 1);
    while (t->slots[i]) i = (i + 1) & (t->cap - 1);
    t->slots[i] = c;
    t->count++;
}

static int table_grow(struct PaintCacheTable *t)
{
    const size_t old_cap = t->cap;
    PaintCache **old = t->slots;
    const size_t cap = old_cap ? old_cap * 2 : 64;
    PaintCache **slots = (PaintCache **) calloc(cap, sizeof *slots);
    if (!slots) return -1;
    t->slots = slots;
    t->cap = cap;
    t->count = 0;
    for (size_t i = 0; i < old_cap; i++)
        if (old[i]) table_insert_raw(t, old[i]);
    free(old);
    return 0;
}

static PaintCache *table_get_or_create(struct PaintCacheTable *t, Box *b)
{
    PaintCache *c = table_find(t, b);
    if (c) return c;
    if ((t->count + 1) * 2 > t->cap && table_grow(t) != 0) return NULL;
    c = (PaintCache *) calloc(1, sizeof *c);
    if (!c) return NULL;
    c->box = b;
    kn_arena_init(&c->arena, PAINT_CACHE_ARENA_BLOCK);
    table_insert_raw(t, c);
    return c;
}

/* Removes every cache whose gen != frame (boxes gone from the tree). */
static void table_evict(struct PaintCacheTable *t, uint32_t frame)
{
    if (!t->cap) return;
    PaintCache **old = t->slots;
    const size_t cap = t->cap;
    PaintCache **slots = (PaintCache **) calloc(cap, sizeof *slots);
    if (!slots) return;
    t->slots = slots;
    t->count = 0;
    for (size_t i = 0; i < cap; i++) {
        if (!old[i]) continue;
        if (old[i]->gen == frame) table_insert_raw(t, old[i]);
        else cache_free(old[i]);
    }
    free(old);
}

static void table_free(struct PaintCacheTable *t)
{
    for (size_t i = 0; i < t->cap; i++)
        if (t->slots[i]) cache_free(t->slots[i]);
    free(t->slots);
    free(t);
}

/* Emission helpers --------------------------------------------------------- */

static void emit(Painter *p, const DlItem *it)
{
    if (dl_push(p->dl, it) != 0) p->oom = 1;
}

static void *parena(Painter *p, size_t size)
{
    void *m = kn_arena_alloc(&p->cache->arena, size);
    if (!m) p->oom = 1;
    return m;
}

void *paint_alloc(PaintCtx *ctx, size_t size)
{
    if (!ctx->caches || !ctx->caches->current) return NULL;
    return kn_arena_alloc(&ctx->caches->current->arena, size);
}

static void emit_rect(Painter *p, DlRect r, CssColor c)
{
    DlItem it;
    if (rect_empty(r) || c.a == 0) return;
    memset(&it, 0, sizeof it);
    it.op = DL_RECT;
    it.u.rect.rect = r;
    it.u.rect.color = c;
    emit(p, &it);
}

static void emit_rrect(Painter *p, DlRect r, DlRadii radii, CssColor c)
{
    DlItem it;
    if (rect_empty(r) || c.a == 0) return;
    if (radii_zero(radii)) {
        emit_rect(p, r, c);
        return;
    }
    memset(&it, 0, sizeof it);
    it.op = DL_RRECT;
    it.u.rrect.rect = r;
    it.u.rrect.radii = radii;
    it.u.rrect.color = c;
    emit(p, &it);
}

static void emit_push_clip(Painter *p, DlRect r, DlRadii radii)
{
    DlItem it;
    memset(&it, 0, sizeof it);
    if (radii_zero(radii)) {
        it.op = DL_PUSH_CLIP_RECT;
        it.u.clip_rect.rect = r;
    } else {
        it.op = DL_PUSH_CLIP_RRECT;
        it.u.clip_rrect.rect = r;
        it.u.clip_rrect.radii = radii;
    }
    emit(p, &it);
}

static void emit_simple(Painter *p, DlOp op)
{
    DlItem it;
    memset(&it, 0, sizeof it);
    it.op = op;
    emit(p, &it);
}

static void emit_transform(Painter *p, Mat2D m)
{
    DlItem it;
    memset(&it, 0, sizeof it);
    it.op = DL_PUSH_TRANSFORM;
    it.u.transform.m = m;
    emit(p, &it);
}

/* ------------------------------------------------------------------------- */
/* 4. Decorations                                                             */
/* ------------------------------------------------------------------------- */

static DlRadii radii_for_rect(const CssComputed *s, float w, float h)
{
    DlRadii r = dl_radii_all(0);
    if (!s) return r;
    const float ref = fminf(w, h);
    float v[4];
    for (int i = 0; i < 4; i++) {
        v[i] = length_resolve(s->border_radius[i], ref);
        if (v[i] < 0) v[i] = 0;
    }
    /* CSS Backgrounds 5.5: scale all radii so adjacent ones fit. */
    float f = 1.0f;
    const float sums[4] = {v[0] + v[1], v[3] + v[2], v[0] + v[3], v[1] + v[2]};
    const float lens[4] = {w, w, h, h};
    for (int i = 0; i < 4; i++)
        if (sums[i] > 0 && lens[i] / sums[i] < f) f = lens[i] / sums[i];
    if (f < 0) f = 0;
    r.tl = v[0] * f;
    r.tr = v[1] * f;
    r.br = v[2] * f;
    r.bl = v[3] * f;
    return r;
}

DlRadii paint_border_radii(const Box *b)
{
    return radii_for_rect(b->style, b->w, b->h);
}

/* Inner (padding-box) radii: outer minus the adjacent border widths. */
static DlRadii inner_radii(DlRadii r, const float bw[4])
{
    DlRadii i;
    i.tl = fmaxf(0, r.tl - fmaxf(bw[CSS_TOP], bw[CSS_LEFT]));
    i.tr = fmaxf(0, r.tr - fmaxf(bw[CSS_TOP], bw[CSS_RIGHT]));
    i.br = fmaxf(0, r.br - fmaxf(bw[CSS_BOTTOM], bw[CSS_RIGHT]));
    i.bl = fmaxf(0, r.bl - fmaxf(bw[CSS_BOTTOM], bw[CSS_LEFT]));
    return i;
}

/* Spread at which an outer box-shadow is a viewport cut-out overlay
 * (`0 0 0 4000px rgba(0,0,0,.5)` on store_ui.c's #knSpinHole). */
#define PAINT_CUTOUT_SPREAD 1000.0f

/* The cut-out must coincide with what the client draws under the page at
 * lroundf(x, y, w, h) of the viewport rect (hard edges).  So instead of the
 * CSS-px snapping, map the unsnapped box to device px, round x, y, w, h
 * independently the same way and map back: the hole's edges then sit on whole
 * device pixels (no partially dimmed seam). */
static DlRect cutout_rect(const Painter *p, const Box *b, DlRect snapped)
{
    float x, y;
    const Mat2D m = mat_mul(p->dev_frame, painter_local_matrix(p));
    if (fabsf(m.b) > 1e-6f || fabsf(m.c) > 1e-6f || fabsf(m.a) < 1e-6f || fabsf(m.d) < 1e-6f) return snapped;
    pos_rel(p, b, &x, &y);
    const float dx = roundf(m.a * x + m.e), dy = roundf(m.d * y + m.f);
    const float dw = roundf(m.a * b->w), dh = roundf(m.d * b->h);
    return dl_rect((dx - m.e) / m.a, (dy - m.f) / m.d, dw / m.a, dh / m.d);
}

static const CssShadow *box_shadow_at(const CssComputed *s, int i)
{
    return i == 0 ? &s->box_shadow : &s->box_shadow_more[i - 1];
}

static void paint_one_box_shadow(Painter *p, const Box *b, const CssShadow *sh, DlRect rect, DlRadii radii, int inset);

/* box-shadow list: the first shadow is on top, so emit back to front. */
static void paint_box_shadow(Painter *p, const Box *b, DlRect rect, DlRadii radii, int inset)
{
    const CssComputed *s = b->style;
    if (!s || !s->box_shadow_count) return;
    for (int i = (int)s->box_shadow_count - 1; i >= 0; i--)
        paint_one_box_shadow(p, b, box_shadow_at(s, i), rect, radii, inset);
}

static void paint_one_box_shadow(Painter *p, const Box *b, const CssShadow *sh, DlRect rect, DlRadii radii, int inset)
{
    const CssComputed *s = b->style;
    DlItem it;
    if ((sh->inset != 0) != (inset != 0)) return;
    if (!inset && sh->spread >= PAINT_CUTOUT_SPREAD && !box_is_inline(b)) rect = cutout_rect(p, b, rect);
    CssColor c = paint_resolve_color(sh->color, s);
    if (c.a == 0) return;
    memset(&it, 0, sizeof it);
    it.op = DL_BOX_SHADOW;
    it.u.box_shadow.rect = rect;
    it.u.box_shadow.radii = radii;
    it.u.box_shadow.dx = sh->x;
    it.u.box_shadow.dy = sh->y;
    it.u.box_shadow.blur = sh->blur;
    it.u.box_shadow.spread = sh->spread;
    it.u.box_shadow.color = c;
    it.u.box_shadow.inset = inset ? 1 : 0;
    if (inset)
        for (int i = 0; i < 4; i++) it.u.box_shadow.inset_pad[i] = b->border[i];
    emit(p, &it);
}

static uint8_t border_style_to_dl(uint8_t st)
{
    switch (st) {
    case CSS_BORDER_STYLE_NONE:
    case CSS_BORDER_STYLE_HIDDEN:
        return DL_BORDER_NONE;
    case CSS_BORDER_STYLE_DASHED:
        return DL_BORDER_DASHED;
    case CSS_BORDER_STYLE_DOTTED:
        return DL_BORDER_DOTTED;
    default:
        return DL_BORDER_SOLID;
    }
}

/* Blink's Color::Dark() / Color::Light() (shading of inset/outset/groove/
 * ridge edges): components in [0,1], scaled by nextafterf(256, 0), truncated. */
static uint8_t shade_channel(float v, float mult)
{
    float r = v * mult * 255.99998f;
    return (uint8_t) (r < 0 ? 0 : r > 255 ? 255 : r);
}

static CssColor color_shade(CssColor c, int light)
{
    const float r = c.r / 255.0f, g = c.g / 255.0f, b = c.b / 255.0f;
    const float v = fmaxf(r, fmaxf(g, b));
    float mult;
    if (light) {
        if (v == 0.0f) { c.r = c.g = c.b = 0x54; return c; }
        mult = fminf(1.0f, v + 0.33f) / v;
    } else {
        if (c.r == 255 && c.g == 255 && c.b == 255) { c.r = c.g = c.b = 0xab; return c; }
        mult = v == 0.0f ? 0.0f : fmaxf(0.0f, (v - 0.33f) / v);
    }
    c.r = shade_channel(r, mult); c.g = shade_channel(g, mult); c.b = shade_channel(b, mult);
    return c;
}

static int color_dist2(CssColor c, int v)
{
    const int r = c.r - v, g = c.g - v, b = c.b - v;
    return r * r + g * g + b * b;
}

/* The colour of edge `side` for a 3D style (Blink's CalculateBorderStyleColor):
 * the shadowed sides (top/left of inset, bottom/right of outset) are darkened
 * unless already within 0x202020 of black, the others lightened unless within
 * 0xebebeb of white; black cannot be darkened, so it is lightened once first.
 * A currentcolor border paints from #eeeeee (WebKit legacy, still in Chrome:
 * `border: 1px inset` and the UA <hr> are 154 / 238).  Measured on Chrome:
 * inset black = 84 / 168, gray = 44 / 212.  `outer_half` picks the half of
 * groove (= inset outside) and ridge (= outset outside). */
static CssColor border_3d_color(uint8_t style, int side, CssColor c, int outer_half)
{
    int inset;
    switch (style) {
    case CSS_BORDER_STYLE_INSET:  inset = 1; break;
    case CSS_BORDER_STYLE_OUTSET: inset = 0; break;
    case CSS_BORDER_STYLE_GROOVE: inset = outer_half; break;
    case CSS_BORDER_STYLE_RIDGE:  inset = !outer_half; break;
    default: return c;
    }
    const int dark_side = (side == CSS_TOP || side == CSS_LEFT) == inset;
    if (c.r == 0 && c.g == 0 && c.b == 0) {
        c = color_shade(c, 1);
        if (dark_side) return c;
    }
    if (dark_side) return color_dist2(c, 0) > 3 * 0x20 * 0x20 ? color_shade(c, 0) : c;
    return color_dist2(c, 255) > 3 * (255 - 0xeb) * (255 - 0xeb) ? color_shade(c, 1) : c;
}

static CssColor border_edge_color(const CssComputed *s, int i, int outer_half)
{
    const uint8_t st = s->border_style[i];
    CssColor c;
    if (st < CSS_BORDER_STYLE_GROOVE) return paint_resolve_color(s->border_color[i], s);
    if (s->border_color[i].kind == CSS_COLOR_CURRENT) {
        c = paint_resolve_color(s->border_color[i], s);
        c.r = c.g = c.b = 238;
        c.a = 255;
    } else {
        c = paint_resolve_color(s->border_color[i], s);
    }
    return border_3d_color(st, i, c, outer_half);
}

static int border_is_two_tone(uint8_t st) { return st == CSS_BORDER_STYLE_GROOVE || st == CSS_BORDER_STYLE_RIDGE; }

/* edge_mask: bit i set -> edge i (CSS order) is painted (inline fragments).
 * groove/ridge edges are painted as two solid halves (outer item, then an
 * inner one inset by the outer widths). */
/* Chrome's themed text fields and menulists: while a control (appearance
 * auto) keeps its UA border -- 2px inset #767676 for a text field, 1px solid
 * #767676 for a <select> -- the theme paints a 1px #767676 border with 2px
 * radius (zoomed) at the border-box edge, background clipped to it (measured,
 * Chrome 152).  Any author border change -> plain CSS painting. */
static float style_zoom(const CssComputed *s);

/* gray: the UA border grey (#767676, or #dddddd for a disabled menulist) */
static int ua_border_is(const CssComputed *s, uint8_t style, float width, uint8_t gray)
{
    for (int i = 0; i < 4; i++) {
        const CssColor c = s->border_color[i];
        if (s->border_style[i] != style || s->border_width[i] != width ||
            c.kind != CSS_COLOR_RGBA || c.r != gray || c.g != gray || c.b != gray || c.a != 255) return 0;
    }
    return 1;
}

static int text_field_themed(const Box *b)
{
    const CssComputed *s = b->style;
    if (!s || s->appearance == CSS_APPEARANCE_NONE) return 0;
    if (b->type == BOX_INPUT && (b->flags & BOXF_TEXT_CONTROL))
        return ua_border_is(s, CSS_BORDER_STYLE_INSET, 2.0f * style_zoom(s), 0x76);
    if (b->type == BOX_SELECT)
        return ua_border_is(s, CSS_BORDER_STYLE_SOLID, style_zoom(s), 0x76) ||
               ua_border_is(s, CSS_BORDER_STYLE_SOLID, style_zoom(s), 0xdd);
    return 0;
}

static void paint_border(Painter *p, const Box *b, DlRect rect, DlRadii radii, unsigned edge_mask)
{
    const CssComputed *s = b->style;
    DlItem it;
    int any = 0, two_tone = 0;
    if (!s) return;
    if (edge_mask == 0xfu && text_field_themed(b)) {
        memset(&it, 0, sizeof it);
        it.op = DL_BORDER;
        it.u.border.rect = rect;
        it.u.border.radii = radii_zero(radii) ? dl_radii_all(2.0f * style_zoom(s)) : radii;
        for (int i = 0; i < 4; i++) {
            it.u.border.width[i] = style_zoom(s);
            it.u.border.style[i] = DL_BORDER_SOLID;
            it.u.border.color[i] = s->border_color[i];
        }
        emit(p, &it);
        return;
    }
    memset(&it, 0, sizeof it);
    it.op = DL_BORDER;
    it.u.border.rect = rect;
    it.u.border.radii = radii;
    for (int i = 0; i < 4; i++) {
        const uint8_t st = border_style_to_dl(s->border_style[i]);
        float w = (st == DL_BORDER_NONE || !(edge_mask & (1u << i))) ? 0 : b->border[i];
        CssColor c = border_edge_color(s, i, 1);
        if (w > 0 && border_is_two_tone(s->border_style[i]) && w >= 2) { w = floorf(w * 0.5f + 0.5f); two_tone = 1; }
        it.u.border.width[i] = w;
        it.u.border.style[i] = w > 0 ? st : DL_BORDER_NONE;
        it.u.border.color[i] = c;
        if (w > 0 && c.a > 0) any = 1;
    }
    if (any) emit(p, &it);
    if (!two_tone) return;
    /* inner halves */
    DlItem in = it;
    const float *ow = it.u.border.width;
    in.u.border.rect = dl_rect(rect.x + ow[CSS_LEFT], rect.y + ow[CSS_TOP],
                               rect.w - ow[CSS_LEFT] - ow[CSS_RIGHT], rect.h - ow[CSS_TOP] - ow[CSS_BOTTOM]);
    memset(&in.u.border.radii, 0, sizeof in.u.border.radii);
    any = 0;
    for (int i = 0; i < 4; i++) {
        const int tt = border_is_two_tone(s->border_style[i]) && ow[i] > 0;
        in.u.border.width[i] = tt ? b->border[i] - ow[i] : 0;
        in.u.border.style[i] = in.u.border.width[i] > 0 ? DL_BORDER_SOLID : DL_BORDER_NONE;
        in.u.border.color[i] = border_edge_color(s, i, 0);
        if (in.u.border.width[i] > 0 && in.u.border.color[i].a > 0) any = 1;
    }
    if (any) emit(p, &in);
}

typedef struct PbImage {
    uint32_t tex;
    float w, h;
    uint8_t premultiplied;
    const void *vector;       /* SVG entry: rasterised at device size by the backend */
    const void *lazy;         /* lazy raster entry (image.h): decoded when drawn, tex 0 */
} PbImage;

static int lookup_image(PaintCtx *ctx, const char *url, const char *base, int nearest, PbImage *out)
{
    if (!url || !ctx->images) return 0;
    const ImageEntry *e = image_get(ctx->images, url, base);
    if (!e || e->failed || e->w <= 0 || e->h <= 0) return 0;
    out->w = (float) e->w;
    out->h = (float) e->h;
    out->premultiplied = 1;   /* image.h: textures hold premultiplied pixels */
    out->vector = e->vector ? (const void *) e : NULL;
    out->lazy = NULL;
    if (image_entry_lazy(e)) {
        out->tex = 0;         /* evictable: the backend resolves the texture per draw */
        out->lazy = e;
        return 1;
    }
    out->tex = image_texture(e, nearest);
    if (!out->tex) out->tex = e->tex;
    return out->tex != 0;
}

/* url() base of a background layer: the declaring sheet, or the document for
 * inline style="" (CssImage.base == NULL). */
static const char *layer_url_base(const PaintCtx *ctx, const CssBgLayer *L)
{
    return L->image.base ? L->image.base : ctx->doc_base_url;
}

/* Resolves one background layer's tile size against the positioning area. */
static void bg_tile_size(const CssBgLayer *L, float aw, float ah, float iw, float ih, float *tw, float *th)
{
    const int has_intrinsic = iw > 0 && ih > 0;
    switch (L->size.kind) {
    case CSS_BG_SIZE_COVER:
    case CSS_BG_SIZE_CONTAIN: {
        if (!has_intrinsic) { *tw = aw; *th = ah; return; }
        const float sx = aw / iw, sy = ah / ih;
        const float s = L->size.kind == CSS_BG_SIZE_COVER ? fmaxf(sx, sy) : fminf(sx, sy);
        *tw = iw * s;
        *th = ih * s;
        return;
    }
    default: {
        const int wa = L->size.w.kind != CSS_LEN_VALUE;
        const int ha = L->size.h.kind != CSS_LEN_VALUE;
        const float w = wa ? 0 : length_resolve(L->size.w, aw);
        const float h = ha ? 0 : length_resolve(L->size.h, ah);
        if (wa && ha) {
            *tw = has_intrinsic ? iw : aw;
            *th = has_intrinsic ? ih : ah;
        } else if (wa) {
            *th = h;
            *tw = has_intrinsic ? h * iw / ih : aw;
        } else if (ha) {
            *tw = w;
            *th = has_intrinsic ? w * ih / iw : ah;
        } else {
            *tw = w;
            *th = h;
        }
        return;
    }
    }
}

typedef struct BgTiling {
    float tw, th;       /* tile size */
    float x0, y0;       /* first tile origin */
    float px, py;       /* the positioned tile's origin (background-position in the area) */
    int nx, ny;         /* tiles per axis */
} BgTiling;

/* Computes the tile grid covering `paint` for a layer positioned in `area`. */
static void bg_tiling(const CssBgLayer *L, DlRect area, DlRect paint, float iw, float ih, BgTiling *t)
{
    bg_tile_size(L, area.w, area.h, iw, ih, &t->tw, &t->th);
    if (L->repeat_x == CSS_BG_REPEAT_ROUND && t->tw > 0) t->tw = area.w / fmaxf(1, roundf(area.w / t->tw));
    if (L->repeat_y == CSS_BG_REPEAT_ROUND && t->th > 0) t->th = area.h / fmaxf(1, roundf(area.h / t->th));
    const float px = area.x + length_resolve(L->x, area.w - t->tw);
    const float py = area.y + length_resolve(L->y, area.h - t->th);
    t->x0 = px;
    t->y0 = py;
    t->px = px;
    t->py = py;
    t->nx = t->ny = 1;
    if (t->tw <= 0 || t->th <= 0) {
        t->nx = t->ny = 0;
        return;
    }
    if (L->repeat_x != CSS_BG_REPEAT_NO_REPEAT) {
        const float first = px - ceilf((px - paint.x) / t->tw) * t->tw;
        t->x0 = first;
        t->nx = (int) ceilf((paint.x + paint.w - first) / t->tw);
    }
    if (L->repeat_y != CSS_BG_REPEAT_NO_REPEAT) {
        const float first = py - ceilf((py - paint.y) / t->th) * t->th;
        t->y0 = first;
        t->ny = (int) ceilf((paint.y + paint.h - first) / t->th);
    }
    if (t->nx < 0) t->nx = 0;
    if (t->ny < 0) t->ny = 0;
    if (t->nx * t->ny > PAINT_MAX_BG_TILES) t->nx = PAINT_MAX_BG_TILES / (t->ny ? t->ny : 1);
}

/* CSS zoom scales an image's natural size (background-size: auto). */
static float style_zoom(const CssComputed *s) { return s && s->zoom > 0.0f ? s->zoom : 1.0f; }

static void paint_bg_layer(Painter *p, const CssComputed *s, const CssBgLayer *L, DlRect area, DlRect paint, int nearest)
{
    DlItem it;
    if (L->image.kind == CSS_IMAGE_URL) {
        PbImage img;
        BgTiling t;
        if (!lookup_image(p->ctx, L->image.url, layer_url_base(p->ctx, L), nearest, &img)) return;
        bg_tiling(L, area, paint, img.w * style_zoom(s), img.h * style_zoom(s), &t);
        for (int j = 0; j < t.ny; j++) {
            for (int i = 0; i < t.nx; i++) {
                /* Blink pixel-snaps background image geometry (dest/tile rect) in
                 * the painting layer's space; edges keep the backend's AA.  Measured:
                 * top-right-bar 2.07 -> 0.53 %, menu-items 1.06 -> 0.29 %; snapping in
                 * device space instead regressed every image region. */
                const DlRect dst = snap_rect(p, dl_rect(t.x0 + i * t.tw, t.y0 + j * t.th, t.tw, t.th));
                if (dst.x >= paint.x + paint.w || dst.y >= paint.y + paint.h ||
                    dst.x + dst.w <= paint.x || dst.y + dst.h <= paint.y) continue;
                memset(&it, 0, sizeof it);
                it.op = DL_IMAGE;
                it.u.image.tex = img.tex;
                it.u.image.uv[0] = 0; it.u.image.uv[1] = 0;
                it.u.image.uv[2] = 1; it.u.image.uv[3] = 1;
                it.u.image.dst = dst;
                it.u.image.opacity = 1;
                it.u.image.nearest = (uint8_t) nearest;
                it.u.image.premultiplied = img.premultiplied;
                it.u.image.vector = img.vector;
                it.u.image.lazy = img.lazy;
                emit(p, &it);
            }
        }
    } else if (L->image.kind == CSS_IMAGE_GRADIENT) {
        BgTiling t;
        bg_tiling(L, area, paint, 0, 0, &t);
        if (!t.nx || !t.ny) return;
        memset(&it, 0, sizeof it);
        it.op = DL_GRADIENT;
        it.u.gradient.gradient = L->image.gradient;
        for (int i = 0; i < it.u.gradient.gradient.stop_count; i++)
            it.u.gradient.gradient.stops[i].color = paint_resolve_color(it.u.gradient.gradient.stops[i].color, s);
        /* Per axis: a no-repeat tile paints only itself (the lottery window's easter frame:
         * four `linear-gradient(#000, #000) 0 0 / 100% 30px no-repeat` mask layers made one
         * opaque mask over the whole box); tiles smaller than the area repeat, one item
         * each; a repeating tile as large as the area is the old single item whose gradient
         * line is the positioned tile's (background-origin, padding box by default), not
         * the first repeated tile's (with a border bg_tiling starts one tile to the left /
         * above, which put every pixel past the gradient's end): the border keeps the end
         * colour instead of a repeat. */
        const int rep_x = L->repeat_x != CSS_BG_REPEAT_NO_REPEAT, rep_y = L->repeat_y != CSS_BG_REPEAT_NO_REPEAT;
        const int tile_x = rep_x && t.tw < area.w - 0.01f, tile_y = rep_y && t.th < area.h - 0.01f;
        const int nx = tile_x ? t.nx : 1, ny = tile_y ? t.ny : 1;
        for (int j = 0; j < ny; j++) {
            for (int i = 0; i < nx; i++) {
                const float bx = tile_x ? t.x0 + i * t.tw : t.px, by = tile_y ? t.y0 + j * t.th : t.py;
                float x0 = rep_x && !tile_x ? paint.x : bx, x1 = rep_x && !tile_x ? paint.x + paint.w : bx + t.tw;
                float y0 = rep_y && !tile_y ? paint.y : by, y1 = rep_y && !tile_y ? paint.y + paint.h : by + t.th;
                x0 = fmaxf(x0, paint.x); x1 = fminf(x1, paint.x + paint.w);
                y0 = fmaxf(y0, paint.y); y1 = fminf(y1, paint.y + paint.h);
                if (x1 <= x0 || y1 <= y0) continue;
                it.u.gradient.rect = dl_rect(x0, y0, x1 - x0, y1 - y0);
                it.u.gradient.box = dl_rect(bx, by, t.tw, t.th);
                emit(p, &it);
            }
        }
    }
}

/* Does any tile of the layer fall outside the painting area? */
static int bg_layer_needs_clip(Painter *p, const CssComputed *s, const CssBgLayer *L, DlRect area, DlRect paint)
{
    BgTiling t;
    float iw = 0, ih = 0;
    if (L->image.kind == CSS_IMAGE_URL) {
        PbImage img;
        if (!lookup_image(p->ctx, L->image.url, layer_url_base(p->ctx, L), 0, &img)) return 0;
        iw = img.w * style_zoom(s);
        ih = img.h * style_zoom(s);
    } else if (L->image.kind != CSS_IMAGE_GRADIENT) {
        return 0;
    }
    bg_tiling(L, area, paint, iw, ih, &t);
    if (!t.nx || !t.ny) return 0;
    return !rect_contains(paint, dl_rect(t.x0, t.y0, t.nx * t.tw, t.ny * t.th));
}

static void walk_inline(Painter *p, Box *b);

static void paint_background_layers(Painter *p, const Box *b, DlRect border_rect, DlRadii radii);

/* background-clip: text (.vs-text): the background painted through the glyphs
 * of the box's in-flow text (Chrome's text-clip phase), as a masked group
 * whose mask is those glyphs.  The text itself is painted later as usual
 * (shadows + -webkit-text-fill-color on top). */
static void paint_background(Painter *p, const Box *b, DlRect border_rect, DlRadii radii)
{
    const CssComputed *s = b->style;
    DlItem it;
    if (!s) return;
    if (s->background_clip != CSS_BG_BOX_TEXT || box_is_inline(b) || box_anon(b) || box_is_text(b)) {
        paint_background_layers(p, b, border_rect, radii);
        return;
    }
    memset(&it, 0, sizeof it);
    it.op = DL_PUSH_GROUP;
    it.u.group.rect = border_rect;
    it.u.group.opacity = 1;
    it.u.group.masked = 1;
    emit(p, &it);
    paint_background_layers(p, b, border_rect, radii);
    emit_simple(p, DL_MASK_BEGIN);
    p->glyph_only = 1;
    walk_inline(p, (Box *) b);
    p->glyph_only = 0;
    emit_simple(p, DL_POP_GROUP);
}

static void paint_background_layers(Painter *p, const Box *b, DlRect border_rect, DlRadii radii)
{
    const CssComputed *s = b->style;
    emit_rrect(p, border_rect, radii, paint_resolve_color(s->background_color, s));
    if (!s->bg_layer_count) return;

    const DlRect area = padding_rect_of(border_rect, b);  /* background-origin: padding-box */
    const int nearest = s->image_rendering == CSS_IMAGE_RENDERING_PIXELATED ||
                        s->image_rendering == CSS_IMAGE_RENDERING_CRISP_EDGES;
    int need_clip = !radii_zero(radii);
    for (int i = 0; i < s->bg_layer_count && !need_clip; i++)
        need_clip = bg_layer_needs_clip(p, s, &s->bg_layers[i], area, border_rect);
    if (need_clip) emit_push_clip(p, border_rect, radii);
    /* first layer is on top: paint last */
    for (int i = s->bg_layer_count - 1; i >= 0; i--)
        paint_bg_layer(p, s, &s->bg_layers[i], area, border_rect, nearest);
    if (need_clip) emit_simple(p, DL_POP_CLIP);
}

static void paint_decorations_rect(Painter *p, const Box *b, DlRect rect, unsigned edge_mask)
{
    /* A cut-out box also paints its background inside the hole: same
     * device-pixel rect as the shadow's hole, or the two leave a seam. */
    const CssComputed *cs = b->style;
    if (cs && cs->box_shadow_count && !cs->box_shadow.inset && cs->box_shadow.spread >= PAINT_CUTOUT_SPREAD && !box_is_inline(b))
        rect = cutout_rect(p, b, rect);
    DlRadii radii = radii_for_rect(b->style, rect.w, rect.h);
    if (radii_zero(radii) && edge_mask == 0xfu && text_field_themed(b)) radii = dl_radii_all(2.0f * style_zoom(b->style));
    paint_box_shadow(p, b, rect, radii, 0);
    paint_background(p, b, rect, radii);
    paint_box_shadow(p, b, rect, radii, 1);
    paint_border(p, b, rect, radii, edge_mask);
}

/* Background + borders + shadows of one box (steps 1 / 4 / 7.2.1).  Inline
 * boxes paint one rect per line fragment. */
static void paint_decorations(Painter *p, const Box *b)
{
    if (p->glyph_only || !box_visible(b) || box_anon(b)) return;
    if (box_is_inline(b)) {
        float dx, dy;
        if (!b->container || b->frag_count <= 0) return;
        content_shift(p, b->container, &dx, &dy);
        for (int i = 0; i < b->frag_count; i++) {
            const BoxFrag *f = &b->frags[i];
            float x, y;
            unsigned mask = (1u << CSS_TOP) | (1u << CSS_BOTTOM);
            if (f->first) mask |= 1u << CSS_LEFT;
            if (f->last) mask |= 1u << CSS_RIGHT;
            to_frame(p, f->x, f->y, &x, &y);
            paint_decorations_rect(p, b, snap_rect(p, dl_rect(x + dx, y + dy, f->w, f->h)), mask);
        }
        return;
    }
    paint_decorations_rect(p, b, box_rect(p, b), 0xfu);
}

/* ------------------------------------------------------------------------- */
/* 5. Text, replaced content, widgets, scrollbars                             */
/* ------------------------------------------------------------------------- */

/* text-decoration propagates from the inline ancestors and the block. */
static uint8_t text_decoration_of(const Box *text_box)
{
    uint8_t bits = 0;
    for (const Box *b = text_box->parent; b; b = b->parent) {
        if (b->style) bits |= b->style->text_decoration;
        if (!box_is_inline(b)) break;
    }
    return bits;
}

/* Appends the quad of one glyph at pen x / baseline (frame CSS px). */
/* Like Chrome/Skia, under an axis-aligned device transform the baseline is
 * snapped to a whole device pixel (glyph rows start solid) and the pen x
 * keeps its 1/4-px subpixel phase, both measured in true device space (all
 * context transforms, scroll offsets and the device scale included).  Under
 * rotation/skew glyphs are placed unsnapped in the frame's scale. */
/* Glyph placement, measured against the doc/ref captures:
 * - x: Skia's subpixel rule (text_snap_origin: + 1/8, integer pixel + 1/4-px
 *   phase) applied in TRUE device space (frame -> device matrix incl. every
 *   context transform and scroll offset).  Measured best: frame-local x costs
 *   chat-rows 21.8 -> 45.7 %, timer 4.5 -> 9.0 %, class-name-tag 2.8 -> 5.2 %.
 * - y: left unrounded here; gl_backend rounds each quad's top once in final
 *   target space (y_off is integral, so that is Blink's baseline rounding).
 * Under rotation / skew the frame scale is used without device snapping. */
static int glyph_to_quad(Painter *p, const Glyph *g, unsigned synth, float font_size, float pen_x, float baseline, DlGlyphQuad *d)
{
    Glyph gl = *g;
    GlyphQuad q;
    float ix, iy;
    const float scale = p->scale;
    const Mat2D dm = mat_mul(p->dev_frame, painter_local_matrix(p));
    const int axis_aligned = fabsf(dm.b) < 1e-6f && fabsf(dm.c) < 1e-6f && dm.a > 1e-6f;
    if (axis_aligned) gl.phase = text_snap_origin(p->ctx->text, dm.a * pen_x + dm.e, 0, &ix, &iy);
    else gl.phase = text_snap_origin(p->ctx->text, pen_x * scale, 0, &ix, &iy);
    if (!text_glyph_quad(p->ctx->text, &gl, font_size * scale, synth, &q) || !q.texture) return 0;
    d->tex = q.texture;
    d->uv[0] = q.u0; d->uv[1] = q.v0; d->uv[2] = q.u1; d->uv[3] = q.v1;
    d->x = axis_aligned ? (ix + q.x_off - dm.e) / dm.a : (ix + q.x_off) / scale;
    /* Blink's TextFragmentPainter rounds a horizontal text origin's y to a
     * whole px in its local (transform) space, before any transform; the
     * device-space rounding in gl_backend then follows.  Under a scale
     * (#uiBase, the hub's body) the two roundings differ: rounding only in
     * device space put hub tab labels 1 px low. */
    const float ls = p->text_layer_snap_y;   /* not part of layout space */
    /* At a devicePixelRatio above 1 (the Strike page) this stays CSS px, like the font metrics and
     * half-leadings: FRVR's captures are Chrome under DevTools device emulation (capture-strike.js:
     * deviceScaleFactor 2), which lays the page out and rounds text in CSS px. Measured: a sweep of
     * Ode to Idle Gaming / ChakraPetch sizes and line-heights under that emulation matches CSS-px
     * rounding in every case; device-px rounding (a real 2x display) matched it in 43 of 60 and put
     * the Ode labels 1 device px low (strike main-menu play 0.23 -> 2.22 %, account-box 0.44 -> 1.52). */
    const float bl = roundf(baseline - ls + p->snap_oy) - p->snap_oy + ls;
    d->y = (bl * scale + q.y_off) / scale;
    d->w = q.w / scale;
    d->h = q.h / scale;
    return 1;
}

static int run_ellipsis_count(const TextRun *run)
{
    if (!run->ellipsis) return 0;
    if (run->ellipsis_count > 0) return run->ellipsis_count > 3 ? 3 : run->ellipsis_count;
    return 1;
}

/* Quads of one run with its pen origin at (rx + ox, ry + oy) (frame px). */
static int run_quads(Painter *p, const TextRun *run, unsigned synth, float fs, float rx, float ry, float ox, float oy,
                     DlGlyphQuad *out, DlRect *bounds)
{
    int m = 0;
    *bounds = dl_rect(0, 0, 0, 0);
    for (int i = 0; i < run->glyph_count; i++) {
        const Glyph *g = &run->glyphs[i];
        if (!glyph_to_quad(p, g, g->synth ? g->synth : synth, fs, rx + g->x + ox, ry + oy, &out[m])) continue;
        *bounds = rect_union(*bounds, dl_rect(out[m].x, out[m].y, out[m].w, out[m].h));
        m++;
    }
    if (run->ellipsis) {
        /* U+2026, or Blink's "..." fallback when the font lacks it (glyph x
         * relative to ellipsis_x); older runs carry only ellipsis_glyph */
        const float ex = rx + (run->ellipsis_x - run->x);
        const int ne = run_ellipsis_count(run);
        for (int i = 0; i < ne; i++) {
            const Glyph *g = run->ellipsis_count ? &run->ellipsis_glyphs[i] : &run->ellipsis_glyph;
            const float gx = run->ellipsis_count ? g->x : 0.0f;
            if (!glyph_to_quad(p, g, g->synth ? g->synth : synth, fs, ex + gx + ox, ry + oy, &out[m])) continue;
            *bounds = rect_union(*bounds, dl_rect(out[m].x, out[m].y, out[m].w, out[m].h));
            m++;
        }
    }
    return m;
}

/* Pen x of byte `off` (relative to run->text) inside a run, run-relative. */
static float run_offset_x(const TextRun *run, uint32_t off)
{
    if (off == 0) return 0.0f;
    if (off >= run->len) return run->width;
    for (int i = 0; i < run->glyph_count; i++) {
        if (run->glyphs[i].cluster >= off) return run->glyphs[i].x;
    }
    return run->width;
}

/* Selection highlight of bytes [a, b) of a run whose pen starts at (rx, ry) in
 * the frame: one line-height tall band centred on the text's content area
 * (Chrome paints the selection over the line's text fragment). */
static void paint_selection_rect(Painter *p, const Box *tb, const TextRun *run, uint32_t a, uint32_t b, float rx, float ry,
                                 CssColor bg)
{
    const CssComputed *s = tb->style;
    const CssColor c = paint_resolve_color(bg, s);
    if (c.a == 0) return;
    const float asc = tb->fm.ascent, desc = tb->fm.descent;
    float lh = asc + desc;
    switch (s->line_height.kind) {
    case CSS_LEN_NUMBER: lh = s->line_height.px * s->font_size; break;
    case CSS_LEN_VALUE: lh = s->line_height.px + s->line_height.pct * 0.01f * s->font_size; break;
    default: if (tb->fm.line_height > 0) lh = tb->fm.line_height; break;
    }
    if (lh < asc + desc) lh = asc + desc;
    const float top = ry - asc - (lh - (asc + desc)) * 0.5f;
    float x0 = run_offset_x(run, a), x1 = run_offset_x(run, b);
    if (b >= run->len && run->ellipsis) x1 = run->width + run->ellipsis_w;
    if (x1 <= x0) return;
    emit_rect(p, dl_rect(rx + x0, top, x1 - x0, lh), c);
}

static void paint_text_run(Painter *p, const Box *tb, const TextRun *run, float dx, float dy)
{
    const CssComputed *s = tb->style;
    DlItem it;
    if (!s || !p->ctx->text || p->scale <= 0) return;
    const int n = run->glyph_count + run_ellipsis_count(run);
    if (n <= 0) return;
    float rx, ry;
    to_frame(p, run->x, run->y, &rx, &ry);
    rx += dx;
    ry += dy;
    {   /* scroller-layer snaps folded into dy (positioned text outside its scroller's clip) */
        float sx, sy, ux, uy;
        scroll_shift(p, tb, &sx, &sy);
        p->no_layer_snap = 1;
        scroll_shift(p, tb, &ux, &uy);
        p->no_layer_snap = 0;
        p->text_layer_snap_y = sy - uy;
    }
    const CssColor color = paint_resolve_color(s->color, s);
    /* -webkit-text-fill-color fills the glyphs; `color` keeps decorations */
    const CssColor fill = s->text_fill_color.kind == CSS_COLOR_CURRENT ? color : paint_resolve_color(s->text_fill_color, s);
    const unsigned synth = run->synth;
    const float fs = run->font_size > 0 ? run->font_size : s->font_size;

    if (p->glyph_only) {    /* background-clip: text: the glyph shapes alone, opaque */
        DlGlyphQuad *gq = (DlGlyphQuad *) parena(p, (size_t) n * sizeof *gq);
        DlRect gb;
        if (!gq) return;
        const int gm = run_quads(p, run, synth, fs, rx, ry, 0, 0, gq, &gb);
        if (gm == 0) return;
        memset(&it, 0, sizeof it);
        it.op = DL_GLYPHS;
        it.u.glyphs.quads = gq;
        it.u.glyphs.n = gm;
        it.u.glyphs.color = color_rgba(255, 255, 255, 255);
        emit(p, &it);
        return;
    }

    /* the page selection (selection.c): highlight under the selected glyphs */
    uint32_t sel_a = 0, sel_b = 0;
    CssColor sel_bg = {0}, sel_fg = {0};
    int sel_has_fg = 0;
    const int selected = p->ctx->host.selection_range
        && p->ctx->host.selection_range(p->ctx->host.user, tb, run, &sel_a, &sel_b, &sel_bg, &sel_fg, &sel_has_fg);
    if (selected) paint_selection_rect(p, tb, run, sel_a, sel_b, rx, ry, sel_bg);

    if (text_decoration_of(tb) & CSS_TEXT_DECORATION_UNDERLINE) {
        const float thick = fmaxf(1.0f, floorf(fs / 18.0f));
        const float off = fmaxf(1.0f, roundf(fs * 0.09f));
        emit_rect(p, dl_rect(rx, ry + off, run->width + (run->ellipsis ? run->ellipsis_w : 0), thick), color);
    }


    DlGlyphQuad *quads = (DlGlyphQuad *) parena(p, (size_t) n * sizeof *quads);
    if (!quads) return;
    DlRect bounds;
    const int m = run_quads(p, run, synth, fs, rx, ry, 0, 0, quads, &bounds);
    if (m == 0) return;

    /* text-shadow: first layer on top -> paint last.  Zero-blur layers are
     * separate glyph runs offset before subpixel phase selection (as Skia
     * draws them); blurred layers reuse the main quads with dx/dy. */
    for (int i = s->text_shadow.count - 1; i >= 0; i--) {
        const CssShadow *sh = &s->text_shadow.items[i];
        CssColor c = paint_resolve_color(sh->color, s);
        if (c.a == 0) continue;
        memset(&it, 0, sizeof it);
        it.op = DL_TEXT_SHADOW;
        it.u.text_shadow.color = c;
        it.u.text_shadow.blur = sh->blur;
        if (sh->blur <= 0) {
            DlGlyphQuad *sq = (DlGlyphQuad *) parena(p, (size_t) n * sizeof *sq);
            if (!sq) return;
            DlRect sb;
            const int sm = run_quads(p, run, synth, fs, rx, ry, sh->x, sh->y, sq, &sb);
            if (sm == 0) continue;
            it.u.text_shadow.quads = sq;
            it.u.text_shadow.n = sm;
            it.u.text_shadow.bounds = sb;
        } else {
            it.u.text_shadow.quads = quads;
            it.u.text_shadow.n = m;
            it.u.text_shadow.dx = sh->x;
            it.u.text_shadow.dy = sh->y;
            it.u.text_shadow.bounds = bounds;
        }
        emit(p, &it);
    }
    if (selected && sel_has_fg) {
        /* ::selection { color }: the selected glyphs in their own pass */
        DlGlyphQuad *sq = (DlGlyphQuad *) parena(p, (size_t) n * sizeof *sq);
        if (!sq) return;
        int nu = 0, ns = 0;
        for (int i = 0; i < run->glyph_count; i++) {
            const Glyph *g = &run->glyphs[i];
            DlGlyphQuad q;
            if (!glyph_to_quad(p, g, g->synth ? g->synth : synth, fs, rx + g->x, ry, &q)) continue;
            if (g->cluster >= sel_a && g->cluster < sel_b) sq[ns++] = q;
            else quads[nu++] = q;
        }
        /* ellipsis glyphs are never selected: rebuild them after the unselected ones */
        if (run->ellipsis) {
            DlRect eb;
            DlGlyphQuad *all = (DlGlyphQuad *) parena(p, (size_t) n * sizeof *all);
            if (all) {
                const int ma = run_quads(p, run, synth, fs, rx, ry, 0, 0, all, &eb);
                for (int i = nu + ns; i < ma; i++) quads[nu++] = all[i];
            }
        }
        if (fill.a != 0 && nu > 0) {
            memset(&it, 0, sizeof it);
            it.op = DL_GLYPHS;
            it.u.glyphs.quads = quads;
            it.u.glyphs.n = nu;
            it.u.glyphs.color = fill;
            emit(p, &it);
        }
        const CssColor fg = paint_resolve_color(sel_fg, s);
        if (fg.a != 0 && ns > 0) {
            memset(&it, 0, sizeof it);
            it.op = DL_GLYPHS;
            it.u.glyphs.quads = sq;
            it.u.glyphs.n = ns;
            it.u.glyphs.color = fg;
            emit(p, &it);
        }
        return;
    }
    if (fill.a == 0) return;
    memset(&it, 0, sizeof it);
    it.op = DL_GLYPHS;
    it.u.glyphs.quads = quads;
    it.u.glyphs.n = m;
    it.u.glyphs.color = fill;
    emit(p, &it);
}

/* object-fit resolution: dst rect and uv for an image of (iw, ih) in `box`. */
static void object_fit_rect(uint8_t fit, DlRect box, float iw, float ih, DlRect *dst, float uv[4])
{
    float sw = box.w, sh = box.h;
    uv[0] = uv[1] = 0;
    uv[2] = uv[3] = 1;
    *dst = box;
    if (iw <= 0 || ih <= 0 || box.w <= 0 || box.h <= 0) return;
    switch (fit) {
    case CSS_OBJECT_FIT_CONTAIN:
    case CSS_OBJECT_FIT_SCALE_DOWN: {
        float s = fminf(box.w / iw, box.h / ih);
        if (fit == CSS_OBJECT_FIT_SCALE_DOWN && s > 1) s = 1;
        sw = iw * s;
        sh = ih * s;
        break;
    }
    case CSS_OBJECT_FIT_COVER: {
        const float s = fmaxf(box.w / iw, box.h / ih);
        sw = iw * s;
        sh = ih * s;
        break;
    }
    case CSS_OBJECT_FIT_NONE:
        sw = iw;
        sh = ih;
        break;
    default:
        return;
    }
    /* centre the scaled image; crop what exceeds the box into the uvs */
    const float x = box.x + (box.w - sw) / 2, y = box.y + (box.h - sh) / 2;
    const float x0 = fmaxf(x, box.x), y0 = fmaxf(y, box.y);
    const float x1 = fminf(x + sw, box.x + box.w), y1 = fminf(y + sh, box.y + box.h);
    *dst = dl_rect(x0, y0, x1 - x0, y1 - y0);
    uv[0] = (x0 - x) / sw;
    uv[1] = (y0 - y) / sh;
    uv[2] = (x1 - x) / sw;
    uv[3] = (y1 - y) / sh;
}

static void paint_replaced_content(Painter *p, const Box *b)
{
    const CssComputed *s = b->style;
    DlItem it;
    PbImage img;
    if (p->glyph_only) return;
    if (!s || !box_visible(b) || (!box_is_image(b) && b->type != BOX_CANVAS && b->type != BOX_VIDEO)) return;
    const PaintHost *h = &p->ctx->host;
    const int nearest = s->image_rendering == CSS_IMAGE_RENDERING_PIXELATED ||
                        s->image_rendering == CSS_IMAGE_RENDERING_CRISP_EDGES;
    int flip_y = 0;
    if (b->type == BOX_CANVAS || b->type == BOX_VIDEO) { /* html_dom_canvas_put / _gl's bitmap, none until the page draws */
        memset(&img, 0, sizeof img);
        if (h->canvas_image && h->canvas_image(h->user, b, &img.tex, &img.w, &img.h, &flip_y)) {
            img.premultiplied = 1;      /* a <video>: its current frame (engine.c, video.h) */
        } else if (b->type != BOX_VIDEO || !b->src || !lookup_image(p->ctx, b->src, p->ctx->doc_base_url, nearest, &img)) {
            return;                     /* a <video> with no frame: its poster (b->src) or nothing */
        }
    } else {
        const char *src = h->image_src ? h->image_src(h->user, b) : b->src;
        if (!lookup_image(p->ctx, src, p->ctx->doc_base_url, nearest, &img)) return;
    }
    const DlRect border = box_rect(p, b);
    const DlRect content = content_rect_of(border, b);
    DlRect dst;
    float uv[4];
    object_fit_rect(s->object_fit, snap_rect(p, content), img.w, img.h, &dst, uv);   /* Blink: snapped content rect */
    if (rect_empty(dst)) return;
    if (flip_y) { /* a GL render target: v = 1 is its top row */
        uv[1] = 1.0f - uv[1];
        uv[3] = 1.0f - uv[3];
    }
    const DlRadii radii = radii_for_rect(s, border.w, border.h);
    const int clip = !radii_zero(radii);
    if (clip) emit_push_clip(p, padding_rect_of(border, b), inner_radii(radii, b->border));
    memset(&it, 0, sizeof it);
    it.op = DL_IMAGE;
    it.u.image.tex = img.tex;
    memcpy(it.u.image.uv, uv, sizeof uv);
    it.u.image.dst = dst;
    it.u.image.opacity = 1;
    it.u.image.nearest = (uint8_t) nearest;
    it.u.image.premultiplied = img.premultiplied;
    it.u.image.vector = img.vector;
    it.u.image.lazy = img.lazy;
    emit(p, &it);
    if (clip) emit_simple(p, DL_POP_CLIP);
}

void paint_scrollbar_defaults(PaintScrollbarStyle *st)
{
    memset(st, 0, sizeof *st);
    st->width = 12;
    st->track_color = color_rgba(0xf5, 0xf5, 0xf5, 255);
    st->track_radius = 10;
    st->track_shadow.color = color_rgba(0, 0, 0, 77);  /* rgba(0,0,0,.3) */
    st->track_shadow.blur = 6;
    st->track_shadow.inset = 1;
    st->track_shadow_count = 1;
    st->thumb_color = color_rgba(0x86, 0x86, 0x86, 255);
    st->thumb_radius = 10;
    st->thumb_shadow.color = color_rgba(0, 0, 0, 153); /* rgba(0,0,0,.6) */
    st->thumb_shadow.blur = 2;
    st->thumb_shadow.inset = 1;
    st->thumb_shadow_count = 1;
    st->thumb_min = 0;
}

static void emit_part_shadow(Painter *p, DlRect r, float radius, const CssShadow *sh)
{
    DlItem it;
    if (sh->color.a == 0) return;
    memset(&it, 0, sizeof it);
    it.op = DL_BOX_SHADOW;
    it.u.box_shadow.rect = r;
    it.u.box_shadow.radii = radii_uniform_clamped(radius, r.w, r.h);
    it.u.box_shadow.dx = sh->x;
    it.u.box_shadow.dy = sh->y;
    it.u.box_shadow.blur = sh->blur;
    it.u.box_shadow.spread = sh->spread;
    it.u.box_shadow.color = sh->color;
    it.u.box_shadow.inset = sh->inset;
    emit(p, &it);
}

/* Chrome custom-scrollbar geometry: no buttons, the track spans the whole
 * padding-box edge, thumb length proportional to the visible fraction. */
static void paint_scrollbar_part(Painter *p, const PaintScrollbarStyle *st, DlRect track, int vertical,
                                 float scroll_pos, float scroll_len, float visible)
{
    emit_rrect(p, track, radii_uniform_clamped(st->track_radius, track.w, track.h), st->track_color);
    if (st->track_shadow_count) emit_part_shadow(p, track, st->track_radius, &st->track_shadow);
    if (scroll_len <= visible || scroll_len <= 0) return;
    const float track_len = vertical ? track.h : track.w;
    float thumb_len = track_len * visible / scroll_len;
    if (thumb_len < st->thumb_min) thumb_len = st->thumb_min;
    if (thumb_len > track_len) thumb_len = track_len;
    const float max_pos = scroll_len - visible;
    float t = max_pos > 0 ? scroll_pos / max_pos : 0;
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    const float thumb_pos = (track_len - thumb_len) * t;
    DlRect thumb = vertical ? dl_rect(track.x, track.y + thumb_pos, track.w, thumb_len)
                            : dl_rect(track.x + thumb_pos, track.y, thumb_len, track.h);
    emit_rrect(p, thumb, radii_uniform_clamped(st->thumb_radius, thumb.w, thumb.h), st->thumb_color);
    if (st->thumb_shadow_count) emit_part_shadow(p, thumb, st->thumb_radius, &st->thumb_shadow);
}

static void paint_scrollbars(Painter *p, const Box *b)
{
    PaintScrollbarStyle st;
    const float vw = b->scrollbar_w, hh = b->scrollbar_h;
    if ((vw <= 0 && hh <= 0) || !box_visible(b) || p->ctx->hide_scrollbars || p->glyph_only) return;
    paint_scrollbar_defaults(&st);
    const PaintHost *h = &p->ctx->host;
    if (h->scrollbar_style && !h->scrollbar_style(h->user, b, &st)) paint_scrollbar_defaults(&st);
    /* part colours may still be currentColor (pseudo styles inherit `color` from the scroller) */
    st.track_color = paint_resolve_color(st.track_color, b->style);
    st.thumb_color = paint_resolve_color(st.thumb_color, b->style);
    st.track_shadow.color = paint_resolve_color(st.track_shadow.color, b->style);
    st.thumb_shadow.color = paint_resolve_color(st.thumb_shadow.color, b->style);
    const DlRect pad = padding_rect_of(box_rect(p, b), b);
    const int rtl = (b->flags & BOXF_RTL) != 0;
    const float vis_h = pad.h - (hh > 0 ? hh : 0), vis_w = pad.w - (vw > 0 ? vw : 0);
    if (vw > 0) {
        DlRect track = dl_rect(rtl ? pad.x : pad.x + pad.w - vw, pad.y, vw, vis_h);
        paint_scrollbar_part(p, &st, track, 1, b->scroll_y, b->scroll_h, vis_h);
    }
    if (hh > 0) {
        DlRect track = dl_rect(rtl && vw > 0 ? pad.x + vw : pad.x, pad.y + pad.h - hh, vis_w, hh);
        paint_scrollbar_part(p, &st, track, 0, b->scroll_x, b->scroll_w, vis_w);
    }
    if (vw > 0 && hh > 0)
        emit_rect(p, dl_rect(rtl ? pad.x : pad.x + pad.w - vw, pad.y + pad.h - hh, vw, hh), st.track_color);
}

/* ------------------------------------------------------------------------- */
/* 6. Clips, transforms, groups                                               */
/* ------------------------------------------------------------------------- */

/* Clip rect for children of an overflow box: padding box minus scrollbars. */
static DlRect overflow_clip_rect(const Painter *p, const Box *b, DlRadii *radii)
{
    if (is_root_box(b) && p->ctx->viewport_w > 0 && p->ctx->viewport_h > 0) {
        /* the viewport: (0,0) of absolute layout space, viewport size */
        float x, y;
        to_frame(p, 0, 0, &x, &y);
        *radii = dl_radii_all(0);
        return dl_rect(x, y, p->ctx->viewport_w, p->ctx->viewport_h);
    }
    const DlRect border = box_rect(p, b);
    DlRect r = padding_rect_of(border, b);
    *radii = inner_radii(radii_for_rect(b->style, border.w, border.h), b->border);
    if (b->scrollbar_w > 0) {
        r.w -= b->scrollbar_w;
        if (b->flags & BOXF_RTL) r.x += b->scrollbar_w;
    }
    if (b->scrollbar_h > 0) r.h -= b->scrollbar_h;
    if (r.w < 0) r.w = 0;
    if (r.h < 0) r.h = 0;
    return r;
}

/* Overflow clip bookkeeping: `start` is the list length before the push,
 * `body` after it; a clip whose body stayed empty (and gained no nested
 * context marker) is rolled back on pop. */
typedef struct ClipMark {
    int pushed;
    int start, body, children;
} ClipMark;

/* A position:sticky box inside the scroller (bounded walk): Chrome layerizes
 * sticky boxes and what paints after them in the scroller, so the scroller's
 * contents are drawn snapped even without overflow (win-servers'
 * .serverHeader: rows below it 1 px off otherwise). */
static int has_sticky_descendant(const Box *b)
{
    int budget = 256;
    const Box *c = b->first_child;
    while (c && budget-- > 0) {
        if (c->style && c->style->position == CSS_POSITION_STICKY) return 1;
        if (c->first_child && !box_is_scroller(c)) { c = c->first_child; continue; }
        while (c && c != b && !c->next) c = c->parent;
        if (!c || c == b) break;
        c = c->next;
    }
    return 0;
}

/* Chrome composites a scroller with scrollable overflow into its own cc
 * layer, rastered in its own space and drawn at a whole device pixel: its
 * contents move by round(T) - T, T = the padding-box origin in device px.
 * Measured with a scaled body (hub): glyph baselines inside such scrollers
 * follow round(T) + round(y * s), not round(T + y * s).  Returned in CSS px
 * of the scroller's space (0 when it does not apply). */
static void scroller_layer_snap(const Painter *p, const Box *b, float *cx, float *cy)
{
    *cx = *cy = 0;
    if (p->no_layer_snap) return;
    /* user-scrollable vertically (overflow-y auto/scroll with overflow): Chrome
     * keeps overflow:hidden boxes in their parent layer, and the windows'
     * horizontal extents (scroll_w > client) do not match its layering */
    const CssComputed *st = b->style;
    if (!st || (st->overflow_y != CSS_OVERFLOW_AUTO && st->overflow_y != CSS_OVERFLOW_SCROLL)) return;
    if (b->scroll_h <= box_padding_h(b) + 0.5f && !has_sticky_descendant(b)) return;
    const Mat2D dm = mat_mul(p->dev_frame, painter_local_matrix(p));
    if (fabsf(dm.b) > 1e-6f || fabsf(dm.c) > 1e-6f || fabsf(dm.a) < 1e-6f || fabsf(dm.d) < 1e-6f) return;
    float x, y;
    pos_rel(p, b, &x, &y);
    const float tx = dm.a * (x + b->border[CSS_LEFT]) + dm.e, ty = dm.d * (y + b->border[CSS_TOP]) + dm.f;
    *cx = (roundf(tx) - tx) / dm.a;
    *cy = (roundf(ty) - ty) / dm.d;
}

/* Pushes the overflow clip (+ scroll offset) of `b`. */
static ClipMark push_overflow_clip(Painter *p, const Box *b)
{
    ClipMark m = {0, 0, 0, 0};
    DlRadii radii;
    DlItem it;
    if (!box_has_overflow_clip(p->ctx, b)) return m;
    m.pushed = 1;
    m.start = p->dl->n;
    m.children = p->cache->n_children;
    const DlRect r = overflow_clip_rect(p, b, &radii);
    emit_push_clip(p, r, radii);
    if (box_is_scroller(b) && p->n_scroll_emitted < PAINT_MAX_SCROLL_STACK) {
        float cx, cy;
        scroller_layer_snap(p, b, &cx, &cy);
        if (b->scroll_x != 0 || b->scroll_y != 0 || cx != 0 || cy != 0) {
            memset(&it, 0, sizeof it);
            it.op = DL_SCROLL_OFFSET;
            it.u.scroll.dx = cx - b->scroll_x;
            it.u.scroll.dy = cy - b->scroll_y;
            emit(p, &it);
        }
        p->scroll_snap_x[p->n_scroll_emitted] = cx;
        p->scroll_snap_y[p->n_scroll_emitted] = cy;
        p->scroll_emitted[p->n_scroll_emitted++] = b;
    }
    m.body = p->dl->n;
    return m;
}

static void pop_overflow_clip(Painter *p, const Box *b, ClipMark m)
{
    if (!m.pushed) return;
    if (p->n_scroll_emitted > 0 && p->scroll_emitted[p->n_scroll_emitted - 1] == b) p->n_scroll_emitted--;
    if (p->dl->n == m.body && p->cache->n_children == m.children) {
        p->dl->n = m.start;     /* nothing was clipped */
        return;
    }
    emit_simple(p, DL_POP_CLIP);
}

/* Pushes the clips of the containing blocks strictly between the frame root
 * and `d` (outermost first).  Returns the number pushed. */
static int push_ancestor_clips(Painter *p, const Box *d, const Box *chain[PAINT_MAX_ANCESTOR_CLIPS], ClipMark marks[PAINT_MAX_ANCESTOR_CLIPS])
{
    const Box *tmp[PAINT_MAX_ANCESTOR_CLIPS];
    int n = 0;
    if (box_fixed(d)) return 0;
    const Box *above = p->root ? p->root->container : NULL;
    for (const Box *a = d->container; a && a != p->root && a != above; a = a->container)
        if (box_has_overflow_clip(p->ctx, a) && n < PAINT_MAX_ANCESTOR_CLIPS) tmp[n++] = a;
    for (int i = 0; i < n; i++) {
        chain[i] = tmp[n - 1 - i];
        marks[i] = push_overflow_clip(p, chain[i]);
    }
    return n;
}

static void pop_ancestor_clips(Painter *p, const Box *chain[PAINT_MAX_ANCESTOR_CLIPS], const ClipMark marks[PAINT_MAX_ANCESTOR_CLIPS], int n)
{
    for (int i = n - 1; i >= 0; i--) pop_overflow_clip(p, chain[i], marks[i]);
}

/* ---- 3D transforms (CSS Transforms 2) -------------------------------------
 * 4x4 matrices are column-major (m[col * 4 + row]) like css_transform_to_mat4.
 * Without `perspective` a 3D transform of the z = 0 plane is an affine map of
 * x / y, so it flattens exactly into a Mat2D (.reward-card's rotateY flip:
 * the x axis scales by cos(angle)).  Under a perspective ancestor the result is
 * projective: the context is recorded untransformed and drawn through a
 * DlGroup homography (gl_backend.c renders it offscreen and maps it as a mesh).
 * Not modelled: depth sorting inside preserve-3d (DOM / z-order is kept), the
 * transform-origin z component, and preserve-3d chains deeper than one level
 * when flattening a child's matrix (backface-visibility walks the whole chain). */
static void m4_identity(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void m4_mul(const float a[16], const float b[16], float out[16])   /* out = a * b */
{
    float r[16];
    for (int c = 0; c < 4; c++)
        for (int row = 0; row < 4; row++) {
            float v = 0;
            for (int k = 0; k < 4; k++) v += a[k * 4 + row] * b[c * 4 + k];
            r[c * 4 + row] = v;
        }
    memcpy(out, r, sizeof r);
}

static void m4_translate(float tx, float ty, float out[16])
{
    m4_identity(out);
    out[12] = tx;
    out[13] = ty;
}

static void m4_from_mat2d(Mat2D m, float out[16])
{
    m4_identity(out);
    out[0] = m.a; out[1] = m.b; out[4] = m.c; out[5] = m.d; out[12] = m.e; out[13] = m.f;
}

/* Inverse (cofactor expansion); 0 when singular. */
static int m4_invert(const float m[16], float out[16])
{
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (fabsf(det) < 1e-12f) return 0;
    for (int i = 0; i < 16; i++) out[i] = inv[i] / det;
    return 1;
}

/* The z = 0 plane under m is affine (no w terms). */
static int m4_plane_affine(const float m[16])
{
    return fabsf(m[3]) < 1e-7f && fabsf(m[7]) < 1e-7f && fabsf(m[15]) > 1e-7f;
}

/* Flattened (z dropped) Mat2D of an affine m. */
static Mat2D m4_flatten(const float m[16])
{
    const float w = m[15];
    Mat2D r = {m[0] / w, m[1] / w, m[4] / w, m[5] / w, m[12] / w, m[13] / w};
    return r;
}

/* Nearest non-anonymous ancestor (the element's parent box). */
static const Box *style_parent(const Box *b)
{
    const Box *p = b->parent;
    while (p && box_anon(p)) p = p->parent;
    return p;
}

static void transform_origin_of(const Box *b, float *ox, float *oy)
{
    const CssComputed *s = b->style;
    *ox = s->transform_origin_x.kind == CSS_LEN_VALUE ? length_resolve(s->transform_origin_x, b->w) : b->w * 0.5f;
    *oy = s->transform_origin_y.kind == CSS_LEN_VALUE ? length_resolve(s->transform_origin_y, b->h) : b->h * 0.5f;
}

/* b's own 3D transform in its border-box space (transform-origin folded in),
 * preceded by its containing block's `perspective` (perspective-origin seen
 * from b).  0 when b's transform has no 3D function. */
static int box_transform_m4(const Box *b, float out[16])
{
    const CssComputed *s = b->style;
    if (!s || box_anon(b) || !css_transform_is_3d(&s->transform)) return 0;
    float m[16], t[16];
    float ox, oy;
    css_transform_to_mat4(&s->transform, b->w, b->h, m);
    transform_origin_of(b, &ox, &oy);
    m4_translate(ox, oy, t);
    m4_mul(t, m, out);
    m4_translate(-ox, -oy, t);
    m4_mul(out, t, out);
    const Box *cb = b->container;
    while (cb && box_anon(cb)) cb = cb->parent;
    if (cb && cb->style && cb->style->perspective.kind == CSS_LEN_VALUE && cb->style->perspective.px > 0) {
        const CssComputed *cs = cb->style;
        const float d = fmaxf(cs->perspective.px, 1.0f);   /* Chrome clamps perspective to >= 1px */
        const float qx = length_resolve(cs->perspective_origin_x, cb->w) - (b->x - cb->x);
        const float qy = length_resolve(cs->perspective_origin_y, cb->h) - (b->y - cb->y);
        float pm[16];
        m4_translate(qx, qy, t);
        m4_identity(pm);
        pm[11] = -1.0f / d;
        m4_mul(t, pm, pm);
        m4_translate(-qx, -qy, t);
        m4_mul(pm, t, pm);
        m4_mul(pm, out, out);
    }
    return 1;
}

/* b's own transform as a 4x4 (2D transforms included); 0 when none. */
static int box_own_m4(const Box *b, float out[16])
{
    if (box_transform_m4(b, out)) return 1;
    const CssComputed *s = b->style;
    if (!s || box_anon(b) || s->transform.count == 0) return 0;
    m4_from_mat2d(paint_box_transform(b), out);
    return 1;
}

/* The flattened Mat2D of a 3D-transformed box.  In a preserve-3d parent with
 * its own 3D transform P the child's matrix is composed with P before
 * flattening, expressed relative to the parent's already flattened frame:
 * X = T(-off) * flat(P)^-1 * flat(P * T(off) * C).  Returns 0 when b's
 * transform is not 3D, 2 when it is projective (*out = identity). */
static int box_transform_3d_flat(const Box *b, Mat2D *out)
{
    float c[16];
    if (!box_transform_m4(b, c)) return 0;
    *out = mat_identity();
    if (!m4_plane_affine(c)) return 2;
    Mat2D flat = m4_flatten(c);
    const Box *pb = style_parent(b);
    float pm[16];
    if (pb && pb->style && pb->style->transform_style == CSS_TRANSFORM_STYLE_PRESERVE_3D && box_transform_m4(pb, pm) &&
        m4_plane_affine(pm)) {
        const Mat2D fp = m4_flatten(pm);
        const float det = fp.a * fp.d - fp.b * fp.c;
        if (fabsf(det) > 1e-4f) {
            const float offx = b->x - pb->x, offy = b->y - pb->y;
            float t[16], full[16];
            m4_translate(offx, offy, t);
            m4_mul(pm, t, full);
            m4_mul(full, c, full);
            if (m4_plane_affine(full)) {
                const Mat2D inv = {fp.d / det, -fp.b / det, -fp.c / det, fp.a / det,
                                   (fp.c * fp.f - fp.d * fp.e) / det, (fp.b * fp.e - fp.a * fp.f) / det};
                flat = mat_mul(mat_translate(-offx, -offy), mat_mul(inv, m4_flatten(full)));
            }
        }
    }
    *out = flat;
    return 1;
}

/* Projective 3D transform of b: the homography (x, y, 1) -> (X, Y, W) into
 * its parent frame.  0 when b is not projective. */
static int box_transform_homography(const Box *b, float h[9])
{
    float m[16];
    if (!box_transform_m4(b, m) || m4_plane_affine(m)) return 0;
    h[0] = m[0]; h[1] = m[4]; h[2] = m[12];
    h[3] = m[1]; h[4] = m[5]; h[5] = m[13];
    h[6] = m[3]; h[7] = m[7]; h[8] = m[15];
    return 1;
}

/* backface-visibility: hidden on a box whose accumulated transform in its 3D
 * rendering context (through preserve-3d ancestors) shows its back: Chrome's
 * IsBackFaceVisible(), the inverse's z-z element is negative. */
static int box_backface_culled(const Box *b)
{
    const CssComputed *s = b->style;
    if (!s || box_anon(b) || s->backface_visibility != CSS_BACKFACE_HIDDEN) return 0;
    float acc[16], m[16], t[16], inv[16];
    if (!box_own_m4(b, acc)) m4_identity(acc);
    const Box *cur = b;
    for (const Box *pb = style_parent(cur); pb && pb->style && pb->style->transform_style == CSS_TRANSFORM_STYLE_PRESERVE_3D;
         cur = pb, pb = style_parent(pb)) {
        m4_translate(cur->x - pb->x, cur->y - pb->y, t);
        m4_mul(t, acc, acc);
        if (box_own_m4(pb, m)) m4_mul(m, acc, acc);
    }
    if (!m4_invert(acc, inv)) return 0;
    return inv[10] < 0.0f;
}

Mat2D paint_box_transform(const Box *b)
{
    const CssComputed *s = b->style;
    Mat2D m = mat_identity();
    if (!s || s->transform.count == 0 || box_anon(b)) return m;
    if (box_transform_3d_flat(b, &m)) return m;   /* 3D: layout's xform ignores rotateX/Y and perspective() */
    if (b->flags & BOXF_HAS_TRANSFORM) return b->xform;   /* layout_update_transform() */
    for (int i = 0; i < s->transform.count; i++) {
        const CssTransformOp *op = &s->transform.ops[i];
        Mat2D o;
        switch (op->kind) {
        case CSS_TRANSFORM_TRANSLATE:
            o = mat_translate(length_resolve(op->tx, b->w), length_resolve(op->ty, b->h));
            break;
        case CSS_TRANSFORM_SCALE:
            o = mat_scale(op->sx, op->sy);
            break;
        case CSS_TRANSFORM_ROTATE:
            o = mat_rotate(op->angle_deg);
            break;
        case CSS_TRANSFORM_SKEW:
            o = mat_skew(op->angle_deg, op->angle2_deg);
            break;
        case CSS_TRANSFORM_MATRIX:
            o = op->m;
            break;
        default:
            o = mat_identity();
            break;
        }
        m = mat_mul(m, o);
    }
    const float ox = s->transform_origin_x.kind == CSS_LEN_VALUE ? length_resolve(s->transform_origin_x, b->w) : b->w * 0.5f;
    const float oy = s->transform_origin_y.kind == CSS_LEN_VALUE ? length_resolve(s->transform_origin_y, b->h) : b->h * 0.5f;
    return mat_mul(mat_translate(ox, oy), mat_mul(m, mat_translate(-ox, -oy)));
}

static int push_clip_path(Painter *p, const Box *b)
{
    const CssComputed *s = b->style;
    DlItem it;
    if (!s || box_anon(b) || s->clip_path.kind != CSS_CLIP_PATH_POLYGON || s->clip_path.point_count < 3) return 0;
    const int n = s->clip_path.point_count;
    float *pts = (float *) parena(p, (size_t) n * 2 * sizeof(float));
    if (!pts) return 0;
    const DlRect r = box_rect(p, b);
    for (int i = 0; i < n; i++) {
        pts[i * 2 + 0] = r.x + length_resolve(s->clip_path.x[i], r.w);
        pts[i * 2 + 1] = r.y + length_resolve(s->clip_path.y[i], r.h);
    }
    memset(&it, 0, sizeof it);
    it.op = DL_PUSH_CLIP_POLY;
    it.u.clip_poly.pts = pts;
    it.u.clip_poly.n = n;
    it.u.clip_poly.evenodd = s->clip_path.evenodd;
    emit(p, &it);
    return 1;
}

static float text_shadow_extent(const CssComputed *s)
{
    float ext = 0;
    if (!s) return 0;
    for (int i = 0; i < s->text_shadow.count; i++) {
        const CssShadow *sh = &s->text_shadow.items[i];
        ext = fmaxf(ext, fmaxf(fabsf(sh->x), fabsf(sh->y)) + sh->blur);
    }
    return ext;
}

/* Painted extent of `b` alone (border box + overflow + shadows), frame space. */
static DlRect own_bounds(const Painter *p, const Box *b)
{
    const CssComputed *s = b->style;
    DlRect r = box_rect(p, b);
    if (box_is_text(b)) return rect_expand(r, text_shadow_extent(s));
    if (b->overflow_r > b->overflow_l && b->overflow_b > b->overflow_t)
        r = rect_union(r, dl_rect(r.x + b->overflow_l, r.y + b->overflow_t, b->overflow_r - b->overflow_l, b->overflow_b - b->overflow_t));
    if (s && s->outline_width > 0 && s->outline_style != CSS_BORDER_STYLE_NONE)
        r = rect_union(r, rect_expand(r, fmaxf(0, s->outline_offset + s->outline_width)));
    for (int i = 0; s && i < s->box_shadow_count; i++) {
        const CssShadow *sh = box_shadow_at(s, i);
        if (sh->inset) continue;
        const float ext = sh->blur + fmaxf(0, sh->spread);
        r = rect_union(r, rect_expand(rect_offset(r, sh->x, sh->y), ext));
    }
    if (s)
        for (int i = 0; i < s->filter_count; i++) {
            if (s->filters[i].kind == CSS_FILTER_BLUR) {    /* Skia's blur extent: 3 sigma */
                r = rect_expand(r, 3.0f * fmaxf(s->filters[i].amount, 0));
                continue;
            }
            if (s->filters[i].kind != CSS_FILTER_DROP_SHADOW) continue;
            const CssShadow *sh = &s->filters[i].shadow;
            r = rect_union(r, rect_expand(rect_offset(r, sh->x, sh->y), sh->blur * 2));
        }
    return r;
}

/* Union of the subtree's painted bounds in the current frame (group FBO
 * sizing: an over-estimate is fine). */
static DlRect subtree_bounds(Painter *p, const Box *b)
{
    DlRect r = own_bounds(p, b);
    for (const Box *c = b->first_child; c; c = c->next) {
        if (c != b && box_has_transform(c)) {
            Painter q = *p;
            q.root = c;
            q.n_scroll_emitted = 0;
            DlRect inner = rect_union(dl_rect(0, 0, c->w, c->h), subtree_bounds(&q, c));
            float cx, cy;
            pos_rel(p, c, &cx, &cy);
            r = rect_union(r, rect_offset(rect_transform(paint_box_transform(c), inner), cx, cy));
            continue;
        }
        r = rect_union(r, subtree_bounds(p, c));
    }
    return r;
}

static int push_group(Painter *p, const Box *b)
{
    const CssComputed *s = b->style;
    DlItem it;
    float h[9];
    if (!s || box_anon(b)) return 0;
    const int proj = box_transform_homography(b, h);
    const int masked = s->mask_layer_count > 0;
    const uint8_t blend = s->mix_blend_mode;
    if (s->opacity >= 1.0f && s->filter_count == 0 && !proj && !masked && blend == CSS_BLEND_NORMAL) return 0;
    memset(&it, 0, sizeof it);
    it.op = DL_PUSH_GROUP;
    it.u.group.rect = subtree_bounds(p, b);
    it.u.group.opacity = s->opacity;
    it.u.group.masked = (uint8_t) masked;
    it.u.group.blend = blend;
    it.u.group.projective = (uint8_t) proj;
    if (proj) memcpy(it.u.group.h, h, sizeof h);
    for (int i = 0; i < s->filter_count && it.u.group.n_filters < CSS_MAX_FILTER_OPS; i++) {
        CssFilterOp f = s->filters[i];
        if (f.kind == CSS_FILTER_DROP_SHADOW) f.shadow.color = paint_resolve_color(f.shadow.color, s);
        it.u.group.filters[it.u.group.n_filters++] = f;
    }
    emit(p, &it);
    return 1;
}

static int item_is_primitive(const DlItem *it)
{
    switch (it->op) {
    case DL_RECT: case DL_RRECT: case DL_BORDER: case DL_IMAGE: case DL_GRADIENT:
    case DL_GLYPHS: case DL_BOX_SHADOW: case DL_TEXT_SHADOW:
        return 1;
    default:
        return 0;
    }
}

static void item_scale_alpha(DlItem *it, float k)
{
    switch (it->op) {
    case DL_RECT: it->u.rect.color = color_scale_alpha(it->u.rect.color, k); break;
    case DL_RRECT: it->u.rrect.color = color_scale_alpha(it->u.rrect.color, k); break;
    case DL_BORDER:
        for (int i = 0; i < 4; i++) it->u.border.color[i] = color_scale_alpha(it->u.border.color[i], k);
        break;
    case DL_IMAGE: it->u.image.opacity *= k; break;
    case DL_GRADIENT:
        for (int i = 0; i < it->u.gradient.gradient.stop_count; i++)
            it->u.gradient.gradient.stops[i].color = color_scale_alpha(it->u.gradient.gradient.stops[i].color, k);
        break;
    case DL_GLYPHS: it->u.glyphs.color = color_scale_alpha(it->u.glyphs.color, k); break;
    case DL_BOX_SHADOW: it->u.box_shadow.color = color_scale_alpha(it->u.box_shadow.color, k); break;
    case DL_TEXT_SHADOW: it->u.text_shadow.color = color_scale_alpha(it->u.text_shadow.color, k); break;
    default: break;
    }
}

/* Closes the group opened at list index `start` (the DL_PUSH_GROUP item).
 * Fast path: a pure-opacity group whose content is exactly one primitive
 * (and no nested context) becomes that primitive with scaled alpha. */
static void pop_group(Painter *p, int start, int children_before)
{
    DisplayList *dl = p->dl;
    DlItem *g = &dl->items[start];
    if (g->u.group.n_filters == 0 && !g->u.group.masked && !g->u.group.projective && g->u.group.blend == CSS_BLEND_NORMAL &&
        p->cache->n_children == children_before) {
        int prims = 0, prim_index = -1, other = 0;
        for (int i = start + 1; i < dl->n; i++) {
            if (item_is_primitive(&dl->items[i])) {
                prims++;
                prim_index = i;
            } else {
                other++;
            }
        }
        if (prims == 1 && other == 0) {
            item_scale_alpha(&dl->items[prim_index], g->u.group.opacity);
            memmove(&dl->items[start], &dl->items[start + 1], (size_t) (dl->n - start - 1) * sizeof(DlItem));
            dl->n--;
            return;
        }
    }
    emit_simple(p, DL_POP_GROUP);
}

/* backdrop-filter: filters what was painted before the element (in the
 * current backdrop root: the page, or the enclosing group) under its border
 * box, clipped to its border radii.  Emitted before the element's own group so
 * the backend reads the parent target; the element's opacity rides along. */
static void paint_mask_layers(Painter *p, const Box *b);

static void paint_backdrop(Painter *p, const Box *b)
{
    const CssComputed *s = b->style;
    DlItem it;
    if (!s || box_anon(b) || !s->backdrop_filter_count || !box_visible(b)) return;
    const DlRect r = box_rect(p, b);
    if (rect_empty(r)) return;
    /* a mask applies to the filtered backdrop too (Chrome): the lottery window's easter frame
     * blurs a 30 px border, not the whole window. The backdrop goes into a masked group of its
     * own, reading the target under it. */
    const int masked = s->mask_layer_count > 0;
    if (masked) {
        memset(&it, 0, sizeof it);
        it.op = DL_PUSH_GROUP;
        it.u.group.rect = r;
        it.u.group.opacity = 1;
        it.u.group.masked = 1;
        emit(p, &it);
    }
    memset(&it, 0, sizeof it);
    it.op = DL_BACKDROP;
    it.u.backdrop.rect = r;
    it.u.backdrop.radii = radii_for_rect(s, r.w, r.h);
    it.u.backdrop.opacity = s->opacity;
    for (int i = 0; i < s->backdrop_filter_count && i < CSS_MAX_FILTER_OPS; i++) {
        CssFilterOp f = s->backdrop_filters[i];
        if (f.kind == CSS_FILTER_DROP_SHADOW) f.shadow.color = paint_resolve_color(f.shadow.color, s);
        it.u.backdrop.filters[it.u.backdrop.n_filters++] = f;
    }
    it.u.backdrop.from_parent = masked;
    emit(p, &it);
    if (masked) {
        paint_mask_layers(p, b);
        emit_simple(p, DL_POP_GROUP);
    }
}

/* mask-image layers (mask-origin / mask-clip: border-box): the group's mask,
 * drawn after DL_MASK_BEGIN like background layers (alpha; mask-composite
 * add == source-over of the layers). */
static void paint_mask_layers(Painter *p, const Box *b)
{
    const CssComputed *s = b->style;
    emit_simple(p, DL_MASK_BEGIN);
    const DlRect r = box_rect(p, b);
    const int n = s->mask_layer_count < CSS_MAX_MASK_LAYERS ? s->mask_layer_count : CSS_MAX_MASK_LAYERS;
    for (int i = n - 1; i >= 0; i--) paint_bg_layer(p, s, &s->mask_layers[i], r, r, 0);
}

/* outline + outline-offset: outside the border box at outline-offset,
 * following border-radius (Chrome >= 94: outer radius = radius + offset +
 * width, square corners stay square).  outline-style: auto (the UA focus
 * ring of focused inputs) is not drawn. */
static void paint_outline(Painter *p, const Box *b)
{
    const CssComputed *s = b->style;
    DlItem it;
    if (p->glyph_only || !s || box_anon(b) || box_is_text(b) || box_is_inline(b) || !box_visible(b)) return;
    if (s->outline_width <= 0 || s->outline_style == CSS_BORDER_STYLE_NONE || s->outline_style == CSS_BORDER_STYLE_HIDDEN ||
        s->outline_style == CSS_BORDER_STYLE_AUTO)
        return;
    const CssColor c = paint_resolve_color(s->outline_color, s);
    if (c.a == 0) return;
    const DlRect br = box_rect(p, b);
    const float w = s->outline_width, off = s->outline_offset;
    const DlRect r = rect_expand(br, off + w);
    if (rect_empty(r)) return;
    const DlRadii in = radii_for_rect(s, br.w, br.h);
    const float g = off + w;
    DlRadii rad;
    rad.tl = in.tl > 0 ? fmaxf(0, in.tl + g) : 0;
    rad.tr = in.tr > 0 ? fmaxf(0, in.tr + g) : 0;
    rad.br = in.br > 0 ? fmaxf(0, in.br + g) : 0;
    rad.bl = in.bl > 0 ? fmaxf(0, in.bl + g) : 0;
    memset(&it, 0, sizeof it);
    it.op = DL_BORDER;
    it.u.border.rect = r;
    it.u.border.radii = rad;
    for (int i = 0; i < 4; i++) {
        it.u.border.width[i] = w;
        it.u.border.color[i] = c;
        it.u.border.style[i] = border_style_to_dl(s->outline_style);
    }
    emit(p, &it);
}

/* Outlines of `b`'s in-flow block descendants (they paint after the content
 * they surround, inside the same overflow clips). */
static void walk_outlines(Painter *p, Box *b)
{
    for (Box *c = b->first_child; c; c = c->next) {
        if (box_layer_root(c) || box_inline_level(c)) continue;
        paint_outline(p, c);
        if (box_is_replaced(c)) continue;
        const ClipMark clip = push_overflow_clip(p, c);
        walk_outlines(p, c);
        pop_overflow_clip(p, c, clip);
    }
}

/* ------------------------------------------------------------------------- */
/* 7. Painting phases and stacking contexts                                   */
/* ------------------------------------------------------------------------- */

static void paint_pseudo_context(Painter *p, Box *b);
static void paint_content(Painter *p, Box *b);

/* Step 4: backgrounds/borders of in-flow non-positioned block-level
 * descendants, tree order. */
static void walk_blocks(Painter *p, Box *b)
{
    for (Box *c = b->first_child; c; c = c->next) {
        if (box_layer_root(c) || box_inline_level(c)) continue;
        paint_decorations(p, c);
        if (box_is_replaced(c)) continue;
        const ClipMark clip = push_overflow_clip(p, c);
        walk_blocks(p, c);
        pop_overflow_clip(p, c, clip);
    }
}

/* Step 5: non-positioned floats, tree order, each as a pseudo context. */
static void walk_floats(Painter *p, Box *b)
{
    for (Box *c = b->first_child; c; c = c->next) {
        if (box_positioned(c) || paint_is_stacking_context(c) || box_is_text(c)) continue;
        if (box_float(c)) {
            paint_pseudo_context(p, c);
            continue;
        }
        if (box_is_inline(c)) {
            walk_floats(p, c);          /* floats inside inline boxes */
            continue;
        }
        if (box_inline_level(c) || box_is_replaced(c)) continue;
        const ClipMark clip = push_overflow_clip(p, c);
        walk_floats(p, c);
        pop_overflow_clip(p, c, clip);
    }
}

/* Nearest layer root strictly between `b` (inclusive) and `block`. */
static const Box *layer_root_between(const Box *b, const Box *block)
{
    for (const Box *a = b; a && a != block; a = a->parent)
        if (!box_is_text(a) && box_layer_root(a)) return a;
    return NULL;
}

static int seen_inline(Painter *p, const Box *b)
{
    for (int i = 0; i < p->n_seen; i++)
        if (p->seen[i] == b) return 1;
    if (p->n_seen == p->cap_seen) {
        const int cap = p->cap_seen ? p->cap_seen * 2 : 32;
        const Box **v = (const Box **) realloc((void *) p->seen, (size_t) cap * sizeof *v);
        if (!v) return 1;
        p->seen = v;
        p->cap_seen = cap;
    }
    p->seen[p->n_seen++] = b;
    return 0;
}

/* Step 7.2: the line boxes of block container `block`.  Items are painted
 * when their nearest layer-root ancestor (below the block) is `within`
 * (NULL: in-flow content; an inline pseudo-root: its own content). */
static void walk_lines(Painter *p, Box *block, const Box *within)
{
    float dx, dy;
    if (!(block->flags & BOXF_HAS_LINES)) return;
    content_shift(p, block, &dx, &dy);
    const int seen_base = p->n_seen;
    for (int i = 0; i < block->line_count; i++) {
        const LineBox *L = &block->lines[i];
        for (int k = 0; k < L->item_count; k++) {
            const InlineItem *it = &L->items[k];
            Box *ib = it->box;
            if (!ib) continue;
            if (it->kind == ITEM_INLINE_CLOSE || it->kind == ITEM_LINEBREAK) continue;
            if (ib == within) continue;   /* the pseudo-root's own decorations are painted by its caller */
            if (layer_root_between(ib, block) != within) continue;
            switch (it->kind) {
            case ITEM_TEXT:
                if (it->run && box_visible(ib)) paint_text_run(p, ib, it->run, dx, dy);
                break;
            case ITEM_INLINE_OPEN:
                if (!seen_inline(p, ib)) paint_decorations(p, ib);
                break;
            case ITEM_ATOMIC:
                paint_pseudo_context(p, ib);
                break;
            default:
                break;
            }
        }
    }
    p->n_seen = seen_base;
}

/* Step 7: inline content of `b` and of its in-flow block descendants. */
static void walk_inline(Painter *p, Box *b)
{
    walk_lines(p, b, NULL);
    for (Box *c = b->first_child; c; c = c->next) {
        if (box_layer_root(c) || box_inline_level(c)) continue;
        if (box_is_replaced(c)) {
            paint_content(p, c);                /* block-level replaced / widget */
            continue;
        }
        const ClipMark clip = push_overflow_clip(p, c);
        walk_inline(p, c);
        pop_overflow_clip(p, c, clip);
        paint_scrollbars(p, c);                 /* in-flow scroller: bars above its content */
    }
}

static void paint_widget(Painter *p, Box *b)
{
    const PaintHost *h = &p->ctx->host;
    PaintWidgetArgs a;
    if (!h->widget_paint || !box_visible(b) || p->glyph_only) return;
    memset(&a, 0, sizeof a);
    a.ctx = p->ctx;
    a.box = b;
    pos_rel(p, b, &a.x, &a.y);
    a.out = p->dl;
    a.device_scale = p->scale;
    h->widget_paint(h->user, &a);
}

/* Content of a block-level box: replaced content, widget internals or the
 * three in-flow phases inside the overflow clip, then its scrollbars. */
static void paint_content(Painter *p, Box *b)
{
    if (box_is_widget(b)) {
        paint_widget(p, b);
        return;
    }
    if (box_is_replaced(b)) {
        paint_replaced_content(p, b);
        return;
    }
    const ClipMark clip = push_overflow_clip(p, b);
    walk_blocks(p, b);
    walk_floats(p, b);
    walk_inline(p, b);
    pop_overflow_clip(p, b, clip);
    paint_scrollbars(p, b);
}

/* Floats, positioned z:auto boxes and atomic inline-level boxes: painted as
 * if they created a stacking context, but their positioned / context
 * descendants belong to the enclosing real context. */
static void paint_pseudo_context(Painter *p, Box *b)
{
    if (box_backface_culled(b)) return;
    paint_decorations(p, b);
    if (box_is_inline(b)) {
        if (b->container) walk_lines(p, b->container, b);
        return;
    }
    paint_content(p, b);
    if (p->glyph_only || box_is_replaced(b)) {
        paint_outline(p, b);
        return;
    }
    const ClipMark clip = push_overflow_clip(p, b);
    walk_outlines(p, b);
    pop_overflow_clip(p, b, clip);
    paint_outline(p, b);
}

/* Collection of a real stacking context's layered descendants ------------ */

typedef struct LayerEntry {
    Box *box;
    int32_t z;
    int order;
    uint8_t is_context;
} LayerEntry;

typedef struct LayerList {
    LayerEntry *v;
    int n, cap;
} LayerList;

static void layer_push(LayerList *l, Box *b, int32_t z, int order, int is_context)
{
    if (l->n == l->cap) {
        const int cap = l->cap ? l->cap * 2 : 16;
        LayerEntry *v = (LayerEntry *) realloc(l->v, (size_t) cap * sizeof *v);
        if (!v) return;
        l->v = v;
        l->cap = cap;
    }
    l->v[l->n].box = b;
    l->v[l->n].z = z;
    l->v[l->n].order = order;
    l->v[l->n].is_context = (uint8_t) is_context;
    l->n++;
}

static int layer_cmp(const void *a, const void *b)
{
    const LayerEntry *x = (const LayerEntry *) a, *y = (const LayerEntry *) b;
    if (x->z != y->z) return x->z < y->z ? -1 : 1;
    return x->order < y->order ? -1 : x->order > y->order;
}

typedef struct Collector {
    LayerList neg, zero, pos;
    int order;
} Collector;

static void collector_free(Collector *c)
{
    free(c->neg.v);
    free(c->zero.v);
    free(c->pos.v);
}

static void collector_add(Collector *c, Box *b, int is_context)
{
    const int32_t z = box_z(b);
    const int order = c->order++;
    if (z < 0) layer_push(&c->neg, b, z, order, is_context);
    else if (z > 0) layer_push(&c->pos, b, z, order, is_context);
    else layer_push(&c->zero, b, z, order, is_context);
}

/* Collects the positioned / stacking-context descendants of `b`.  Fixed
 * boxes belong to the viewport (or the nearest transformed ancestor): when
 * this context does not catch them they are skipped here and surfaced by
 * gather_fixed*() in the catching context. */
static void collect(Collector *c, Box *b, int catches_fixed)
{
    for (Box *child = b->first_child; child; child = child->next) {
        if (box_is_text(child)) continue;
        if (box_fixed(child) && !catches_fixed) continue;
        if (paint_is_stacking_context(child)) {
            collector_add(c, child, 1);
            continue;
        }
        if (box_positioned(child)) collector_add(c, child, 0);
        collect(c, child, catches_fixed);
    }
}

/* Fixed descendants of a non-catching context (stops at fixed boxes and at
 * transformed contexts, which catch their own). */
static void gather_fixed(Box *b, LayerList *out)
{
    for (Box *child = b->first_child; child; child = child->next) {
        if (box_is_text(child)) continue;
        if (box_fixed(child)) {
            layer_push(out, child, 0, 0, 0);
            continue;
        }
        if (box_catches_fixed(child)) continue;
        gather_fixed(child, out);
    }
}

/* Within a catching context's own subtree: fixed boxes hiding inside nested
 * (non-transformed) contexts.  Direct fixed descendants were already added by
 * collect(..., 1). */
static void gather_fixed_in_contexts(Box *b, LayerList *out)
{
    for (Box *child = b->first_child; child; child = child->next) {
        if (box_is_text(child)) continue;
        if (paint_is_stacking_context(child)) {
            if (!box_catches_fixed(child)) gather_fixed(child, out);
            continue;
        }
        gather_fixed_in_contexts(child, out);
    }
}

static void add_child_ref(Painter *p, Box *child)
{
    PaintCache *c = p->cache;
    if (c->n_children == c->cap_children) {
        const int cap = c->cap_children ? c->cap_children * 2 : 8;
        PaintChildRef *v = (PaintChildRef *) realloc(c->children, (size_t) cap * sizeof *v);
        if (!v) { p->oom = 1; return; }
        c->children = v;
        c->cap_children = cap;
    }
    PaintChildRef *ref = &c->children[c->n_children++];
    ref->index = p->dl->n;
    ref->box = child;
    ref->local = painter_local_matrix(p);
    ref->scroll_x = ref->scroll_y = 0;
    for (int i = 0; i < p->n_scroll_emitted; i++) {
        ref->scroll_x += p->scroll_emitted[i]->scroll_x;
        ref->scroll_y += p->scroll_emitted[i]->scroll_y;
    }
    pos_rel(p, child, &ref->dx, &ref->dy);
    float ux, uy;
    p->no_layer_snap = 1;
    pos_rel(p, child, &ux, &uy);
    p->no_layer_snap = 0;
    ref->layer_snap_x = ref->dx - ux;
    ref->layer_snap_y = ref->dy - uy;
}

static void paint_layer_entry(Painter *p, const LayerEntry *e)
{
    const Box *chain[PAINT_MAX_ANCESTOR_CLIPS];
    ClipMark marks[PAINT_MAX_ANCESTOR_CLIPS];
    const int clips = push_ancestor_clips(p, e->box, chain, marks);
    if (e->is_context) add_child_ref(p, e->box);  /* marker: recorded separately */
    else paint_pseudo_context(p, e->box);
    pop_ancestor_clips(p, chain, marks, clips);
}

static void paint_layer_list(Painter *p, LayerList *l, int sort)
{
    if (sort && l->n > 1) qsort(l->v, (size_t) l->n, sizeof *l->v, layer_cmp);
    for (int i = 0; i < l->n; i++) paint_layer_entry(p, &l->v[i]);
}

/* Records one real stacking context into p->cache->list (its own frame). */
static void record_context(Painter *p, Box *b)
{
    Collector col;
    const int catches_fixed = box_catches_fixed(b);

    /* opacity: 0 paints nothing (the whole context, nested ones included) */
    if (b->style && b->style->opacity <= 0.0f && b->parent && !box_anon(b)) return;
    if (box_backface_culled(b)) return;

    memset(&col, 0, sizeof col);
    collect(&col, b, catches_fixed);
    if (catches_fixed) {
        LayerList fx;
        memset(&fx, 0, sizeof fx);
        gather_fixed_in_contexts(b, &fx);
        for (int i = 0; i < fx.n; i++) {
            Box *f = fx.v[i].box;
            if (paint_is_stacking_context(f)) {
                collector_add(&col, f, 1);
                gather_fixed(f, &fx);
            } else {
                collector_add(&col, f, 0);
                collect(&col, f, 1);
                gather_fixed_in_contexts(f, &fx);
            }
        }
        free(fx.v);
    }

    const int children_before = p->cache->n_children;
    const Mat2D m = paint_box_transform(b);
    const int tr = !mat_is_identity(m);
    if (tr) emit_transform(p, m);
    paint_backdrop(p, b);
    const int group_start = p->dl->n;
    const int grp = push_group(p, b);
    const int poly = push_clip_path(p, b);

    paint_decorations(p, b);
    if (box_is_inline(b)) {
        if (b->container) walk_lines(p, b->container, b);
        paint_layer_list(p, &col.neg, 1);
        paint_layer_list(p, &col.zero, 0);
        paint_layer_list(p, &col.pos, 1);
    } else if (box_is_replaced(b)) {
        paint_content(p, b);
    } else {
        const ClipMark clip = push_overflow_clip(p, b);
        paint_layer_list(p, &col.neg, 1);
        walk_blocks(p, b);
        walk_floats(p, b);
        walk_inline(p, b);
        paint_layer_list(p, &col.zero, 0);
        paint_layer_list(p, &col.pos, 1);
        walk_outlines(p, b);
        pop_overflow_clip(p, b, clip);
        paint_scrollbars(p, b);
    }
    paint_outline(p, b);

    if (poly) emit_simple(p, DL_POP_CLIP);
    if (grp && b->style->mask_layer_count > 0 && !box_anon(b)) paint_mask_layers(p, b);
    if (grp) pop_group(p, group_start, children_before);
    if (tr) emit_simple(p, DL_POP_TRANSFORM);
    collector_free(&col);
}

/* ------------------------------------------------------------------------- */
/* 8. Dirty tracking, build, flatten                                          */
/* ------------------------------------------------------------------------- */

void paint_ctx_init(PaintCtx *ctx)
{
    memset(ctx, 0, sizeof *ctx);
    ctx->device_scale = 1;
    ctx->css_base_url = "css/main.css";
    ctx->doc_base_url = "";
    ctx->caches = (struct PaintCacheTable *) calloc(1, sizeof *ctx->caches);
}

void paint_ctx_free(PaintCtx *ctx)
{
    if (ctx->caches) table_free(ctx->caches);
    ctx->caches = NULL;
}

void paint_invalidate_all(PaintCtx *ctx)
{
    struct PaintCacheTable *t = ctx->caches;
    if (!t) return;
    for (size_t i = 0; i < t->cap; i++)
        if (t->slots[i]) t->slots[i]->valid = 0;
}

static Box *context_root_of(const Box *b)
{
    Box *cur = (Box *) b;
    while (cur->parent && !paint_is_stacking_context(cur)) cur = cur->parent;
    return cur;
}

void paint_mark_dirty(PaintCtx *ctx, const Box *box)
{
    if (!ctx->caches || !box) return;
    Box *root = context_root_of(box);
    PaintCache *c = table_find(ctx->caches, root);
    if (c) c->dirty = 1;
    if (root == box && root->parent) {
        PaintCache *pc = table_find(ctx->caches, context_root_of(root->parent));
        if (pc) pc->dirty = 1;
    }
}

/* Per-box signature of what paint reads besides geometry (geometry changes
 * raise Box.paint_dirty in layout): style record identity + hashes, flags,
 * z-order, scroll state.  Cheap enough to recompute every frame. */
#define HASH_FIELD(h, v) ((h) = kn_hash_mem((h), &(v), sizeof(v)))

static uint64_t hash_box(uint64_t h, const Box *b)
{
    HASH_FIELD(h, b->type);
    HASH_FIELD(h, b->flags);
    HASH_FIELD(h, b->z_index);
    HASH_FIELD(h, b->z_auto);
    HASH_FIELD(h, b->style);
    if (b->style) {
        HASH_FIELD(h, b->style->hash_inherited);
        HASH_FIELD(h, b->style->hash_non_inherited);
    }
    HASH_FIELD(h, b->x);
    HASH_FIELD(h, b->y);
    HASH_FIELD(h, b->w);
    HASH_FIELD(h, b->h);
    HASH_FIELD(h, b->scroll_x);
    HASH_FIELD(h, b->scroll_y);
    HASH_FIELD(h, b->scroll_h);
    HASH_FIELD(h, b->scrollbar_w);
    HASH_FIELD(h, b->container);
    HASH_FIELD(h, b->src);
    HASH_FIELD(h, b->run_count);
    HASH_FIELD(h, b->line_count);
    HASH_FIELD(h, b->frag_count);
    return h;
}

/* Walks the context rooted at `root`: hashes it, collects paint_dirty flags
 * (clearing them) and recurses into nested contexts. */
typedef struct DirtyWalk {
    PaintCtx *ctx;
    uint64_t hash;
    int dirty;
} DirtyWalk;

static void dirty_walk(DirtyWalk *w, Box *b, Box *root)
{
    if (b->paint_dirty) {
        w->dirty = 1;
        b->paint_dirty = 0;
    }
    w->hash = hash_box(w->hash, b);
    for (Box *c = b->first_child; c; c = c->next) {
        if (c != root && !box_is_text(c) && paint_is_stacking_context(c)) {
            HASH_FIELD(w->hash, c);   /* identity + ordering fields are in hash_box(c) below */
            w->hash = hash_box(w->hash, c);
            PaintCache *cc = table_get_or_create(w->ctx->caches, c);
            DirtyWalk cw = {w->ctx, KN_FNV64_INIT, 0};
            if (c->paint_dirty) w->dirty = 1;   /* a moved/changed root also reorders its parent */
            dirty_walk(&cw, c, c);
            if (cc && (cw.dirty || cw.hash != cc->hash)) {
                cc->hash = cw.hash;
                cc->dirty = 1;
            }
            continue;
        }
        dirty_walk(w, c, root);
    }
}

static void painter_free(Painter *p)
{
    free((void *) p->seen);
    p->seen = NULL;
    p->n_seen = p->cap_seen = 0;
}

/* A recorded list stays valid while the frame's device mapping keeps its
 * linear part (glyph raster size) and the sub-pixel part of its translation
 * (glyph pixel + phase are chosen in device x; cut-out overlays round in
 * device space), and while the frame's offset in its transform space keeps
 * its fractional part (decoration pixel snapping).  Whole-pixel moves reuse
 * the list. */
static float frac_part(float v) { return v - floorf(v); }

static int dev_compatible(Mat2D a, Mat2D b)
{
    if (fabsf(a.a - b.a) > 1e-6f || fabsf(a.b - b.b) > 1e-6f || fabsf(a.c - b.c) > 1e-6f || fabsf(a.d - b.d) > 1e-6f) return 0;
    float fx = fabsf(frac_part(a.e) - frac_part(b.e)), fy = fabsf(frac_part(a.f) - frac_part(b.f));
    if (fx > 0.5f) fx = 1.0f - fx;
    if (fy > 0.5f) fy = 1.0f - fy;
    return fx < 1e-4f && fy < 1e-4f;   /* y: cut-out overlays round in device space */
}

static int record_if_needed(PaintCtx *ctx, Box *b, Mat2D dev_in, float snap_ox, float snap_oy)
{
    PaintCache *c = table_get_or_create(ctx->caches, b);
    Painter p;
    if (!c) return -1;
    c->gen = ctx->frame;
    ctx->stats_contexts++;
    if (c->valid && !c->dirty && dev_compatible(c->dev_in, dev_in) &&
        fabsf(frac_part(c->snap_ox) - frac_part(snap_ox)) < 1e-4f && fabsf(frac_part(c->snap_oy) - frac_part(snap_oy)) < 1e-4f) {
        c->snap_ox = snap_ox;
        c->snap_oy = snap_oy;
        return 0;
    }

    dl_clear(&c->list);
    kn_arena_reset(&c->arena);
    c->n_children = 0;
    c->dev_in = dev_in;
    c->snap_ox = snap_ox;
    c->snap_oy = snap_oy;

    memset(&p, 0, sizeof p);
    p.ctx = ctx;
    p.cache = c;
    p.dl = &c->list;
    p.dev_frame = dev_in;
    p.root_xform = paint_box_transform(b);
    p.snap_ox = snap_ox;
    p.snap_oy = snap_oy;
    p.scale = mat_scale_factor(mat_mul(dev_in, p.root_xform));
    p.root = b;
    ctx->caches->current = c;
    record_context(&p, b);
    ctx->caches->current = NULL;
    painter_free(&p);
    if (p.oom) {
        c->valid = 0;
        return -1;
    }
    c->valid = 1;
    c->dirty = 0;
    ctx->stats_rebuilt++;
    return 0;
}

static int push_translate(DisplayList *out, float dx, float dy)
{
    DlItem it;
    memset(&it, 0, sizeof it);
    it.op = DL_PUSH_TRANSFORM;
    it.u.transform.m = mat_translate(dx, dy);
    return dl_push(out, &it);
}

static int push_pop_transform(DisplayList *out)
{
    DlItem it;
    memset(&it, 0, sizeof it);
    it.op = DL_POP_TRANSFORM;
    return dl_push(out, &it);
}

/* top: the list the root's top-layer child context goes to (paint_build_once, the root only:
 * deeper contexts carry their ancestors' transforms and clips); NULL = none */
static int flatten_context(PaintCtx *ctx, Box *b, Mat2D dev_in, float snap_ox, float snap_oy, DisplayList *out,
                           DisplayList *top)
{
    if (record_if_needed(ctx, b, dev_in, snap_ox, snap_oy) != 0) return -1;
    PaintCache *c = table_find(ctx->caches, b);
    DisplayList *const page = out;
    int k = 0;
    for (int i = 0; i <= c->list.n; i++) {
        while (k < c->n_children && c->children[k].index == i) {
            const PaintChildRef *ref = &c->children[k++];
            out = top && ref->box == ctx->top_layer ? top : page;
            /* Composited layers (translateZ / will-change) are NOT origin-snapped:
             * Chrome carries the sub-pixel remainder into the layer contents.
             * Measured on ingame-*: rounding the layer origin to device px costs
             * health-value 8.2 -> 17.3 %, timer 14.1 -> 16.0 % (ammo 16.2 -> 14.4). */
            float rdx = ref->dx, rdy = ref->dy;
            /* A transformed box with a non-translation matrix (skew, rotate, scale) is placed at its origin rounded to
             * whole CSS px, as Blink does (Krunker Strike's skewed purchase buttons: 9.5 -> 10). Pure translations keep
             * their sub-pixel origin: rounding those moved 40 uidiff regions out of tolerance (om-tools, spin-item). */
            if (box_has_transform(ref->box) && !box_composited(ref->box) && ref->box->style && ref->box->style->position == CSS_POSITION_RELATIVE) {
                const Mat2D own = paint_box_transform(ref->box);
                if (fabsf(own.a - 1.0f) > 1e-6f || fabsf(own.d - 1.0f) > 1e-6f || fabsf(own.b) > 1e-6f || fabsf(own.c) > 1e-6f) {
                    rdx = roundf(rdx); rdy = roundf(rdy);
                }
            }
            const Mat2D child_dev = mat_mul(c->dev_in, mat_mul(ref->local, mat_translate(rdx, rdy)));
            const int tr = rdx != 0 || rdy != 0;
            if (tr && push_translate(out, rdx, rdy) != 0) return -1;
            /* a transformed / composited child starts a new snapping space at its
             * own origin; otherwise it continues ours */
            float cox = 0, coy = 0;
            if (!box_has_transform(ref->box) && !box_composited(ref->box)) {
                const int own = box_has_transform(b) || box_composited(b);
                cox = (own ? 0 : c->snap_ox) + ref->dx - ref->layer_snap_x - ref->scroll_x;
                coy = (own ? 0 : c->snap_oy) + ref->dy - ref->layer_snap_y - ref->scroll_y;
            }
            if (flatten_context(ctx, ref->box, child_dev, cox, coy, out, NULL) != 0) return -1;
            if (tr && push_pop_transform(out) != 0) return -1;
            out = page;
        }
        if (i < c->list.n && dl_push(out, &c->list.items[i]) != 0) return -1;
    }
    return 0;
}

static int paint_build_once(PaintCtx *ctx, Box *root, DisplayList *out)
{
    int rc;
    if (!ctx->caches) return -1;
    ctx->frame++;
    ctx->stats_rebuilt = 0;
    ctx->stats_contexts = 0;
    dl_clear(out);
    DisplayList *top = ctx->top_layer && ctx->top_out && ctx->top_out != out ? ctx->top_out : NULL;
    if (ctx->top_out && ctx->top_out != out) dl_clear(ctx->top_out);
    if (!root) return 0;

    if (ctx->hide_scrollbars != ctx->last_hide_scrollbars) {
        ctx->last_hide_scrollbars = ctx->hide_scrollbars;
        paint_invalidate_all(ctx);
    }

    /* glyph atlas rebuilt: every cached quad is stale */
    if (ctx->text) {
        const uint32_t gen = text_atlas_generation(ctx->text);
        if (gen != ctx->atlas_generation) {
            ctx->atlas_generation = gen;
            paint_invalidate_all(ctx);
        }
    }

    PaintCache *rc_cache = table_get_or_create(ctx->caches, root);
    if (!rc_cache) return -1;
    DirtyWalk w = {ctx, KN_FNV64_INIT, 0};
    dirty_walk(&w, root, root);
    if (w.dirty || w.hash != rc_cache->hash) {
        rc_cache->hash = w.hash;
        rc_cache->dirty = 1;
    }

    /* the root frame: translate by the root box position when non-zero */
    const int tr = root->x != 0 || root->y != 0;
    if (tr && push_translate(out, root->x, root->y) != 0) return -1;
    if (tr && top && push_translate(top, root->x, root->y) != 0) return -1;
    const Mat2D dev_root = mat_mul(mat_scale(ctx->device_scale, ctx->device_scale), mat_translate(root->x, root->y));
    rc = flatten_context(ctx, root, dev_root, 0, 0, out, top);
    if (tr && rc == 0 && push_pop_transform(out) != 0) rc = -1;
    if (tr && top && rc == 0 && push_pop_transform(top) != 0) rc = -1;
    table_evict(ctx->caches, ctx->frame);
    return rc;
}

/* text_glyph_quad() rebuilds a full glyph atlas on the spot (generation + 1) and
 * packs the new glyph from the top again.  When that happens while recording, the
 * quads recorded before it - in this list and in the cached contexts flattened
 * from earlier frames - point at atlas space the glyphs after it overwrite: that
 * text shows pieces of the new glyphs (the top rows of a shrinking "FIRST BLOOD"
 * popup's letters), and since the list is only rebuilt when something is dirty
 * it stays on screen until the next paint.  Re-record everything once against the
 * new atlas (the visible glyphs fit: the atlas held them all before). */
int paint_build(PaintCtx *ctx, Box *root, DisplayList *out)
{
    int rc = paint_build_once(ctx, root, out);
    if (rc == 0 && ctx->text && text_atlas_generation(ctx->text) != ctx->atlas_generation)
        rc = paint_build_once(ctx, root, out);
    return rc;
}
