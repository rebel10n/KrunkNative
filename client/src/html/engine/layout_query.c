/*
 * layout_query.c - geometry queries over a laid-out tree: transforms and
 * scroll offsets along the containing-block chain (getBoundingClientRect,
 * point mapping), offsetWidth/Height, scrollbar geometry, stacking-context
 * ordering and paint-order hit testing.
 */
#include "layout_internal.h"
#include "html_dom.h"

static const Mat2D k_identity = { 1, 0, 0, 1, 0, 0 };

static Mat2D mat_mul(const Mat2D *A, const Mat2D *B)   /* A * B: B applied first */
{
    Mat2D r;
    r.a = A->a * B->a + A->c * B->b;
    r.b = A->b * B->a + A->d * B->b;
    r.c = A->a * B->c + A->c * B->d;
    r.d = A->b * B->c + A->d * B->d;
    r.e = A->a * B->e + A->c * B->f + A->e;
    r.f = A->b * B->e + A->d * B->f + A->f;
    return r;
}

static Mat2D mat_translate(float x, float y) { Mat2D m = { 1, 0, 0, 1, x, y }; return m; }

static void mat_apply(const Mat2D *m, float x, float y, float *ox, float *oy)
{
    *ox = m->a * x + m->c * y + m->e;
    *oy = m->b * x + m->d * y + m->f;
}

static int mat_invert(const Mat2D *m, Mat2D *out)
{
    float det = m->a * m->d - m->b * m->c;
    if (fabsf(det) < 1e-12f) return 0;
    float id = 1.0f / det;
    out->a = m->d * id;  out->b = -m->b * id;
    out->c = -m->c * id; out->d = m->a * id;
    out->e = -(out->a * m->e + out->c * m->f);
    out->f = -(out->b * m->e + out->d * m->f);
    return 1;
}

void layout_box_matrix(const Box *box, Mat2D *out)
{
    *out = (box && (box->flags & BOXF_HAS_TRANSFORM)) ? box->xform : k_identity;
}

/* Own transform of `b` in absolute layout space: T(x,y) * xform * T(-x,-y). */
static Mat2D own_matrix(const Box *b)
{
    if (!(b->flags & BOXF_HAS_TRANSFORM)) return k_identity;
    Mat2D t1 = mat_translate(b->x, b->y), t2 = mat_translate(-b->x, -b->y);
    Mat2D m = mat_mul(&t1, &b->xform);
    return mat_mul(&m, &t2);
}

/* Maps absolute layout space of `b`'s content (include_self: b's own border box)
 * to viewport space, applying scroll offsets and transforms of the
 * containing-block chain. */
static Mat2D to_viewport(const Box *b, int include_self)
{
    Mat2D m = include_self ? own_matrix(b) : k_identity;
    for (const Box *c = b->container; c; c = c->container) {
        if (c->flags & BOXF_SCROLL_CONTAINER) {
            Mat2D s = mat_translate(-c->scroll_x, -c->scroll_y);
            m = mat_mul(&s, &m);
        }
        if (c->flags & BOXF_HAS_TRANSFORM) {
            Mat2D o = own_matrix(c);
            m = mat_mul(&o, &m);
        }
    }
    return m;
}

int layout_box_rect_in_viewport(const Box *box, HtmlRect *out)
{
    if (!box || !out) return 0;
    Mat2D m = to_viewport(box, 1);
    float xs[4] = { box->x, box->x + box->w, box->x + box->w, box->x };
    float ys[4] = { box->y, box->y, box->y + box->h, box->y + box->h };
    float l = LAYOUT_INF, t = LAYOUT_INF, r = -LAYOUT_INF, bt = -LAYOUT_INF;
    for (int i = 0; i < 4; i++) {
        float px, py;
        mat_apply(&m, xs[i], ys[i], &px, &py);
        l = kn_minf(l, px); r = kn_maxf(r, px); t = kn_minf(t, py); bt = kn_maxf(bt, py);
    }
    out->x = l; out->y = t; out->w = r - l; out->h = bt - t;
    return 1;
}

float layout_offset_width(const Box *box) { return box ? box->w : 0.0f; }
float layout_offset_height(const Box *box) { return box ? box->h : 0.0f; }

void layout_point_to_box(const Box *box, float vx, float vy, float *lx, float *ly)
{
    Mat2D m = to_viewport(box, 1), inv;
    if (!mat_invert(&m, &inv)) { *lx = vx; *ly = vy; return; }
    mat_apply(&inv, vx, vy, lx, ly);
}

/* ------------------------------------------------------------------------- */
/* scrollbars                                                                 */
/* ------------------------------------------------------------------------- */

int layout_scrollbar(const LayoutTree *tree, const Box *b, int vertical, LayoutScrollbar *out)
{
    if (!b || !(b->flags & BOXF_SCROLL_CONTAINER)) return 0;
    float min_thumb = tree && tree->ctx ? tree->ctx->scrollbar_min_thumb : 0.0f;
    memset(out, 0, sizeof(*out));
    if (vertical) {
        if (b->scrollbar_w <= 0) return 0;
        out->track_w = b->scrollbar_w;
        out->track_h = box_padding_h(b) - b->scrollbar_h;
        out->track_x = (b->flags & BOXF_RTL) ? b->x + b->border[CSS_LEFT] : b->x + b->w - b->border[CSS_RIGHT] - b->scrollbar_w;
        out->track_y = b->y + b->border[CSS_TOP];
        float client = box_client_h(b), total = kn_maxf(b->scroll_h, client);
        float len = total > 0 ? out->track_h * client / total : out->track_h;
        len = kn_minf(out->track_h, kn_maxf(len, min_thumb));
        float range = total - client;
        float pos = range > 0 ? (out->track_h - len) * b->scroll_y / range : 0;
        out->thumb_x = out->track_x; out->thumb_w = out->track_w;
        out->thumb_y = out->track_y + pos; out->thumb_h = len;
    } else {
        if (b->scrollbar_h <= 0) return 0;
        out->track_h = b->scrollbar_h;
        out->track_w = box_padding_w(b) - b->scrollbar_w;
        out->track_x = b->x + b->border[CSS_LEFT] + ((b->flags & BOXF_RTL) ? b->scrollbar_w : 0);
        out->track_y = b->y + b->h - b->border[CSS_BOTTOM] - b->scrollbar_h;
        float client = box_client_w(b), total = kn_maxf(b->scroll_w, client);
        float len = total > 0 ? out->track_w * client / total : out->track_w;
        len = kn_minf(out->track_w, kn_maxf(len, min_thumb));
        float range = total - client;
        float pos = range > 0 ? (out->track_w - len) * b->scroll_x / range : 0;
        out->thumb_y = out->track_y; out->thumb_h = out->track_h;
        out->thumb_x = out->track_x + pos; out->thumb_w = len;
    }
    return 1;
}

/* ------------------------------------------------------------------------- */
/* stacking order                                                             */
/* ------------------------------------------------------------------------- */

static int is_layer(const Box *b)
{
    return (b->flags & (BOXF_POSITIONED | BOXF_STACKING_CONTEXT)) && b->type != BOX_TEXT;
}

typedef struct LayerVec { Box **data; size_t len, cap; } LayerVec;

static void collect_layers(const Box *b, LayerVec *v)
{
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->type == BOX_TEXT || c->type == BOX_LINEBREAK) continue;
        if (is_layer(c)) {
            KN_VEC_PUSH(v, c);
            if (c->flags & BOXF_STACKING_CONTEXT) continue;   /* its descendants belong to it */
        }
        collect_layers(c, v);
    }
}

static int32_t layer_z(const Box *b)
{
    if (b->z_auto || !(b->flags & (BOXF_POSITIONED)) ) {
        /* z-index applies to positioned boxes (and flex/grid items, flagged as stacking contexts) */
        if ((b->flags & BOXF_STACKING_CONTEXT) && !b->z_auto) return b->z_index;
        return 0;
    }
    return b->z_index;
}

int layout_stacking_children(const Box *ctx, Box **out, int max)
{
    LayerVec v = { 0 };
    collect_layers(ctx, &v);
    int n = (int)v.len;
    /* stable insertion sort by z (tree order preserved for ties) */
    for (int i = 1; i < n; i++) {
        Box *x = v.data[i];
        int32_t z = layer_z(x);
        int j = i - 1;
        while (j >= 0 && layer_z(v.data[j]) > z) { v.data[j + 1] = v.data[j]; j--; }
        v.data[j + 1] = x;
    }
    if (out) for (int i = 0; i < n && i < max; i++) out[i] = v.data[i];
    KN_VEC_FREE(&v);
    return n;
}

/* ------------------------------------------------------------------------- */
/* hit testing                                                                */
/* ------------------------------------------------------------------------- */

typedef struct Hit {
    float vx, vy;
    int pe_aware;
    const LayoutTree *tree;
    const Box *body;          /* <body> box when its overflow propagates to the viewport */
} Hit;

/* CSS Overflow 3 3.3: the root's overflow applies to the viewport; when the
 * root is overflow:visible, <body>'s (a child of the root) propagates instead
 * and <body> itself does not clip.  Same rule as paint.c (PaintCtx.body). */
static const Box *propagating_body(const LayoutTree *t)
{
    const Box *root = t->root;
    if (!root || (root->flags & BOXF_SCROLL_CONTAINER)) return NULL;
    for (const Box *c = root->first_child; c; c = c->next)
        if (c->node && c->node->type == LXB_DOM_NODE_TYPE_ELEMENT && c->node->local_name == LXB_TAG_BODY
            && (c->flags & BOXF_SCROLL_CONTAINER)) return c;
    return NULL;
}

static int clips_to_viewport(const Hit *h, const Box *c)
{
    return c == h->tree->root || c == h->body;
}

static int point_in(float x, float y, float rx, float ry, float rw, float rh)
{
    return x >= rx && y >= ry && x < rx + rw && y < ry + rh;
}

/* clip-path: polygon(): is the point (lx, ly, the box's frame) inside the box's polygon? The same
 * points as paint.c's push_clip_path (the border box's origin plus each length), filled with the
 * polygon's rule (nonzero unless evenodd). A box without a polygon, and an anonymous box (it borrows
 * its element's style, so its lengths are not its own), always passes. */
static float clip_len(CssLength l, float ref) { return l.kind == CSS_LEN_VALUE ? l.px + l.pct * ref / 100.0f : 0.0f; }

static int in_clip_polygon(const Box *b, float lx, float ly)
{
    const CssClipPath *cp = b->style ? &b->style->clip_path : NULL;
    if (!cp || cp->kind != CSS_CLIP_PATH_POLYGON || cp->point_count < 3 || !b->node || (b->flags & BOXF_ANONYMOUS)) return 1;
    int winding = 0, crossings = 0;
    const int n = cp->point_count;
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        const float x0 = b->x + clip_len(cp->x[i], b->w), y0 = b->y + clip_len(cp->y[i], b->h);
        const float x1 = b->x + clip_len(cp->x[j], b->w), y1 = b->y + clip_len(cp->y[j], b->h);
        const float side = (x1 - x0) * (ly - y0) - (lx - x0) * (y1 - y0); /* > 0: left of the edge */
        if (y0 <= ly) {
            if (y1 > ly && side > 0) winding++;
        } else if (y1 <= ly && side < 0) {
            winding--;
        }
        if ((y0 > ly) != (y1 > ly) && lx < x0 + (ly - y0) * (x1 - x0) / (y1 - y0)) crossings++;
    }
    return cp->evenodd ? (crossings & 1) != 0 : winding != 0;
}

/* Clipped by an overflow ancestor (or a clip-path polygon) on the containing-block chain? */
static int clipped_out(const Hit *h, const Box *b)
{
    for (const Box *c = b->container; c; c = c->container) {
        if (!(c->flags & BOXF_CLIPS)) continue;
        if (clips_to_viewport(h, c)) {
            if (!point_in(h->vx, h->vy, 0, 0, h->tree->viewport_w, h->tree->viewport_h)) return 1;
            continue;
        }
        float lx, ly;
        layout_point_to_box(c, h->vx, h->vy, &lx, &ly);
        if (!point_in(lx, ly, box_padding_x(c), box_padding_y(c), box_padding_w(c), box_padding_h(c))) return 1;
        if (!in_clip_polygon(c, lx, ly)) return 1;
    }
    return 0;
}

static int hittable(const Hit *h, const Box *b)
{
    if (!h->pe_aware) return 1;
    /* text and anonymous boxes carry no style flags of their own: they take pointer-events /
     * visibility from their element (a pointer-events:none block's anonymous line boxes and
     * text are not targets either) */
    const Box *e = b;
    while (e->parent && (e->type == BOX_TEXT || (e->flags & BOXF_ANONYMOUS) || !e->node)) e = e->parent;
    return !((b->flags | e->flags) & (BOXF_NO_POINTER | BOXF_HIDDEN));
}

static Box *element_box(Box *b)
{
    while (b && (b->type == BOX_TEXT || (b->flags & BOXF_ANONYMOUS) || !b->node)) b = b->parent;
    return b;
}

/* Does the box's own border box (or inline fragments / text) contain the point? */
static int self_hit(const Hit *h, const Box *b)
{
    if (!hittable(h, b)) return 0;
    if (clipped_out(h, b)) return 0;
    float lx, ly;
    layout_point_to_box(b, h->vx, h->vy, &lx, &ly);
    if (b->type == BOX_INLINE) {
        for (int i = 0; i < b->frag_count; i++)
            if (point_in(lx, ly, b->frags[i].x, b->frags[i].y, b->frags[i].w, b->frags[i].h)) return 1;
        return 0;
    }
    if (b->type == BOX_LINEBREAK || b->type == BOX_TEXT) return 0;
    if (!in_clip_polygon(b, lx, ly)) return 0;
    return point_in(lx, ly, b->x, b->y, b->w, b->h);
}

/* Text of a block container's lines (painted above the block's background). */
static Box *lines_hit(const Hit *h, const Box *b)
{
    if (!b->line_count) return NULL;
    float lx, ly;
    /* line items live in b's content space (b's own transform applies) */
    layout_point_to_box(b, h->vx, h->vy, &lx, &ly);
    if (clipped_out(h, b) && 0) return NULL;
    for (int i = b->line_count - 1; i >= 0; i--) {
        const LineBox *ln = &b->lines[i];
        for (int j = ln->item_count - 1; j >= 0; j--) {
            const InlineItem *it = &ln->items[j];
            if (it->kind != ITEM_TEXT) continue;
            if (!point_in(lx, ly, it->x, it->y, it->w, it->h)) continue;
            Box *tb = it->box;
            if (h->pe_aware && (tb->flags & (BOXF_NO_POINTER | BOXF_HIDDEN))) continue;
            /* the text's element: walk inline ancestors that are hittable; the block whose
             * lines these are is a target only when it is hittable itself (pointer-events:
             * none on it, inherited by its text, lets the point through to what is below) */
            Box *p = tb->parent;
            while (p && p != b && h->pe_aware && !hittable(h, p)) p = p->parent;
            if (p == b && !hittable(h, b)) continue;
            return p;
        }
    }
    return NULL;
}

static Box *hit_layer(const Hit *h, Box *u);

/* Normal-flow content of `u`: non-layer descendants, front to back. */
static Box *hit_flow(const Hit *h, Box *u)
{
    for (Box *c = u->last_child; c; c = c->prev) {
        if (c->type == BOX_TEXT || c->type == BOX_LINEBREAK) continue;
        if (is_layer(c)) continue;
        Box *r = hit_flow(h, c);
        if (r) return r;
        if (c->line_count && !clipped_out(h, c)) { r = lines_hit(h, c); if (r) return r; }
        if (self_hit(h, c)) return c;
    }
    return NULL;
}

static Box *hit_layer(const Hit *h, Box *u)
{
    if (u->flags & BOXF_STACKING_CONTEXT) {
        int n = layout_stacking_children(u, NULL, 0);
        Box **list = n ? malloc((size_t)n * sizeof(Box *)) : NULL;
        if (n && !list) return NULL;
        layout_stacking_children(u, list, n);
        Box *r = NULL;
        int i = n - 1;
        for (; i >= 0 && !r; i--) {
            if (layer_z(list[i]) < 0) break;
            r = hit_layer(h, list[i]);
        }
        if (!r) r = hit_flow(h, u);
        if (!r && u->line_count && !clipped_out(h, u)) r = lines_hit(h, u);
        if (!r) for (; i >= 0 && !r; i--) r = hit_layer(h, list[i]);
        if (!r && self_hit(h, u)) r = u;
        free(list);
        return r;
    }
    Box *r = hit_flow(h, u);
    if (!r && u->line_count && !clipped_out(h, u)) r = lines_hit(h, u);
    if (!r && self_hit(h, u)) r = u;
    return r;
}

Box *layout_hit_test(LayoutTree *tree, float vx, float vy, int pointer_events_aware)
{
    if (!tree || !tree->root) return NULL;
    Hit h = { vx, vy, pointer_events_aware, tree, propagating_body(tree) };
    Box *r = hit_layer(&h, tree->root);
    if (!r) {
        /* like elementFromPoint: the root element when inside the viewport */
        if (vx >= 0 && vy >= 0 && vx < tree->viewport_w && vy < tree->viewport_h && hittable(&h, tree->root)) r = tree->root;
    }
    return element_box(r);
}
