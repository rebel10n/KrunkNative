/*
 * layout_block.c - block formatting contexts, box sizing, floats, absolute
 * positioning, sticky, scroll containers, intrinsic widths and the
 * layout_run() driver with layout boundaries.
 */
#include "layout_internal.h"

/* ------------------------------------------------------------------------- */
/* lengths                                                                    */
/* ------------------------------------------------------------------------- */

float layout_resolve_len(const CssLength *l, float base, float fallback)
{
    if (l->kind != CSS_LEN_VALUE) return fallback;
    if (l->pct != 0.0f) {
        if (base < 0) return fallback;   /* percentage against an indefinite base */
        return l->px + l->pct * 0.01f * base;
    }
    return l->px;
}

float layout_resolve_len_auto(const CssLength *l, float base, int *is_auto)
{
    if (l->kind != CSS_LEN_VALUE || (l->pct != 0.0f && base < 0)) { *is_auto = 1; return 0; }
    *is_auto = 0;
    return l->px + l->pct * 0.01f * base;
}

float layout_horizontal_extras(const Box *b) { return box_border_padding_w(b) + b->scrollbar_w; }
float layout_vertical_extras(const Box *b)   { return box_border_padding_h(b) + b->scrollbar_h; }

void layout_set_bars(Box *b)
{
    b->scrollbar_w = (b->flags & BOXF_VBAR) ? b->bar_w : 0.0f;
    b->scrollbar_h = (b->flags & BOXF_HBAR) ? b->bar_w : 0.0f;
}

float layout_box_sizing_w(const Box *b, float css_w)
{
    if (b->style->box_sizing == CSS_BOX_SIZING_BORDER_BOX) return kn_maxf(css_w, box_border_padding_w(b));
    return css_w + box_border_padding_w(b);
}

float layout_box_sizing_h(const Box *b, float css_h)
{
    if (b->style->box_sizing == CSS_BOX_SIZING_BORDER_BOX) return kn_maxf(css_h, box_border_padding_h(b));
    return css_h + box_border_padding_h(b);
}

void layout_resolve_edges(LayoutTree *t, Box *b, float cb_w)
{
    (void)t;
    const CssComputed *s = b->style;
    float base = cb_w < 0 ? 0 : cb_w;
    for (int i = 0; i < 4; i++) {
        b->margin[i] = layout_resolve_len(&s->margin[i], base, 0.0f);   /* auto -> 0 for now */
        b->padding[i] = kn_maxf(0.0f, layout_resolve_len(&s->padding[i], base, 0.0f));
        b->border[i] = (s->border_style[i] == CSS_BORDER_STYLE_NONE || s->border_style[i] == CSS_BORDER_STYLE_HIDDEN) ? 0.0f : s->border_width[i];
    }
    if (b->type == BOX_TEXT || b->type == BOX_LINEBREAK) {
        memset(b->margin, 0, sizeof(b->margin));
        memset(b->padding, 0, sizeof(b->padding));
        memset(b->border, 0, sizeof(b->border));
    }
    if (b->type == BOX_TABLE_CELL && b->parent && b->parent->parent && b->parent->parent->type == BOX_TABLE
        && b->parent->parent->style->border_collapse == CSS_BORDER_COLLAPSE_COLLAPSE) {
        for (int i = 0; i < 4; i++) b->border[i] *= 0.5f;
    }
}

void layout_apply_min_max_w(const Box *b, float cb_w, float *border_w)
{
    const CssComputed *s = b->style;
    float base = cb_w < 0 ? 0 : cb_w;
    if (s->max_width.kind == CSS_LEN_VALUE && !(s->max_width.pct != 0 && cb_w < 0)) {
        float mx = layout_box_sizing_w(b, kn_maxf(0.0f, layout_resolve_len(&s->max_width, base, LAYOUT_INF)));
        if (*border_w > mx) *border_w = mx;
    }
    if (s->min_width.kind == CSS_LEN_VALUE && !(s->min_width.pct != 0 && cb_w < 0)) {
        float mn = layout_box_sizing_w(b, kn_maxf(0.0f, layout_resolve_len(&s->min_width, base, 0)));
        if (*border_w < mn) *border_w = mn;
    }
    if (*border_w < box_border_padding_w(b)) *border_w = box_border_padding_w(b);
}

void layout_apply_min_max_h(const Box *b, float cb_h, float *border_h)
{
    const CssComputed *s = b->style;
    if (s->max_height.kind == CSS_LEN_VALUE && !(s->max_height.pct != 0 && cb_h < 0)) {
        float mx = layout_box_sizing_h(b, kn_maxf(0.0f, layout_resolve_len(&s->max_height, cb_h, LAYOUT_INF)));
        if (*border_h > mx) *border_h = mx;
    }
    if (s->min_height.kind == CSS_LEN_VALUE && !(s->min_height.pct != 0 && cb_h < 0)) {
        float mn = layout_box_sizing_h(b, kn_maxf(0.0f, layout_resolve_len(&s->min_height, cb_h, 0)));
        if (*border_h < mn) *border_h = mn;
    }
    if (*border_h < box_border_padding_h(b)) *border_h = box_border_padding_h(b);
}

/* ------------------------------------------------------------------------- */
/* per-box arrays                                                             */
/* ------------------------------------------------------------------------- */

void layout_free_lines(Box *b)
{
    if (b->lines) {
        for (int i = 0; i < b->line_count; i++) free(b->lines[i].items);
        free(b->lines);
    }
    b->lines = NULL;
    b->line_count = 0;
}

void layout_free_runs(Box *b)
{
    for (int i = 0; i < b->run_count; i++) free(b->runs[i].glyphs);
    free(b->runs);
    b->runs = NULL; b->run_count = b->run_cap = 0;
}

void layout_free_box_arrays(Box *b)
{
    layout_free_lines(b);
    layout_free_runs(b);
    free(b->frags); b->frags = NULL; b->frag_count = b->frag_cap = 0;
    free(b->abs_children); b->abs_children = NULL; b->abs_count = b->abs_cap = 0;
}

/* ------------------------------------------------------------------------- */
/* floats                                                                     */
/* ------------------------------------------------------------------------- */

void floats_add(FloatCtx *f, Box *box, uint8_t side, float x, float y, float w, float h)
{
    if (f->len == f->cap) {
        size_t nc = f->cap ? f->cap * 2 : 8;
        FloatEntry *n = realloc(f->data, nc * sizeof(FloatEntry));
        if (!n) return;
        f->data = n; f->cap = nc;
    }
    FloatEntry *e = &f->data[f->len++];
    e->box = box; e->side = side; e->x = x; e->y = y; e->w = w; e->h = h;
}

void floats_avail(const FloatCtx *f, float y, float height, float left, float right, float *out_left, float *out_right)
{
    float l = left, r = right;
    if (f) {
        float y1 = y + kn_maxf(height, 0.01f);
        for (size_t i = 0; i < f->len; i++) {
            const FloatEntry *e = &f->data[i];
            if (e->h <= 0 && e->w <= 0) continue;
            if (e->y >= y1 || e->y + e->h <= y) continue;
            if (e->side == CSS_FLOAT_LEFT) { if (e->x + e->w > l) l = e->x + e->w; }
            else { if (e->x < r) r = e->x; }
        }
    }
    *out_left = l; *out_right = r;
}

float floats_clear_y(const FloatCtx *f, int clear)
{
    float y = -LAYOUT_INF;
    if (!f) return y;
    for (size_t i = 0; i < f->len; i++) {
        const FloatEntry *e = &f->data[i];
        if (clear == CSS_CLEAR_BOTH || (clear == CSS_CLEAR_LEFT && e->side == CSS_FLOAT_LEFT)
            || (clear == CSS_CLEAR_RIGHT && e->side == CSS_FLOAT_RIGHT))
            if (e->y + e->h > y) y = e->y + e->h;
    }
    return y;
}

float floats_bottom(const FloatCtx *f)
{
    float y = -LAYOUT_INF;
    if (!f) return y;
    for (size_t i = 0; i < f->len; i++) if (f->data[i].y + f->data[i].h > y) y = f->data[i].y + f->data[i].h;
    return y;
}

float floats_next_y(const FloatCtx *f, float y)
{
    float best = LAYOUT_INF;
    if (!f) return y;
    for (size_t i = 0; i < f->len; i++) {
        const FloatEntry *e = &f->data[i];
        float bottom = e->y + e->h;
        if (bottom > y + 0.001f && bottom < best) best = bottom;
    }
    return best == LAYOUT_INF ? y : best;
}

void floats_place(LayoutTree *t, FloatCtx *f, Box *fb, float y_min, float cb_left, float cb_right)
{
    (void)t;
    float mw = box_margin_w(fb), mh = box_margin_h(fb);
    float y = y_min;
    /* a float's top may not be above the top of earlier floats */
    for (size_t i = 0; i < f->len; i++) if (f->data[i].y > y) y = f->data[i].y;
    for (int guard = 0; guard < 1000; guard++) {
        float l, r;
        floats_avail(f, y, kn_maxf(mh, 0.01f), cb_left, cb_right, &l, &r);
        if (r - l >= mw - 0.001f || (l <= cb_left + 0.001f && r >= cb_right - 0.001f)) {
            float x = (fb->style->float_ == CSS_FLOAT_LEFT) ? l : r - mw;
            if (x < cb_left && fb->style->float_ == CSS_FLOAT_LEFT) x = cb_left;
            floats_add(f, fb, (uint8_t)fb->style->float_, x, y, mw, mh);
            return;
        }
        float ny = floats_next_y(f, y);
        if (ny <= y) { /* nothing more to wait for */
            float x = (fb->style->float_ == CSS_FLOAT_LEFT) ? l : r - mw;
            floats_add(f, fb, (uint8_t)fb->style->float_, x, y, mw, mh);
            return;
        }
        y = ny;
    }
}

/* ------------------------------------------------------------------------- */
/* overflow                                                                   */
/* ------------------------------------------------------------------------- */

void layout_reset_overflow(Box *b)
{
    b->overflow_l = 0; b->overflow_t = 0; b->overflow_r = b->w; b->overflow_b = b->h;
}

void layout_overflow_union_child(Box *parent, const Box *child)
{
    /* child's overflow rect is relative to child's border box; translate (local coords) */
    float l = child->lx + kn_minf(0.0f, child->overflow_l);
    float tp = child->ly + kn_minf(0.0f, child->overflow_t);
    float r = child->lx + kn_maxf(child->w, child->overflow_r);
    float bt = child->ly + kn_maxf(child->h, child->overflow_b);
    if (child->flags & BOXF_HAS_TRANSFORM) {
        /* bounding box of the transformed rect */
        float cx[4] = { l, r, r, l }, cy[4] = { tp, tp, bt, bt };
        float nl = LAYOUT_INF, nt = LAYOUT_INF, nr = -LAYOUT_INF, nb = -LAYOUT_INF;
        for (int i = 0; i < 4; i++) {
            float lx = cx[i] - child->lx, ly = cy[i] - child->ly;
            float px = child->xform.a * lx + child->xform.c * ly + child->xform.e + child->lx;
            float py = child->xform.b * lx + child->xform.d * ly + child->xform.f + child->ly;
            nl = kn_minf(nl, px); nt = kn_minf(nt, py); nr = kn_maxf(nr, px); nb = kn_maxf(nb, py);
        }
        l = nl; tp = nt; r = nr; bt = nb;
    }
    if (l < parent->overflow_l) parent->overflow_l = l;
    if (tp < parent->overflow_t) parent->overflow_t = tp;
    if (r > parent->overflow_r) parent->overflow_r = r;
    if (bt > parent->overflow_b) parent->overflow_b = bt;
}

/* items_abs: lines/frags are already in absolute space (post-finalize).
 *
 * Two results:
 *  - visual overflow (overflow_l/t/r/b, local): border box U children's
 *    overflow, used by ancestors; reset to the border box when b clips.
 *  - scrollable overflow (scroll_w/h): CSS Overflow 3 - the content extent
 *    (in-flow children's margin boxes in the block axis, their descendants'
 *    unclipped overflow, line content, abs boxes) measured from the padding
 *    edge, plus the block-end/inline-end padding ONCE, at least the client box. */
static void compute_overflow(Box *b, int items_abs)
{
    layout_reset_overflow(b);
    int clips = (b->flags & BOXF_CLIPS) != 0;
    float ox = items_abs ? b->x : 0, oy = items_abs ? b->y : 0;
    float ext_r = box_cl(b), ext_b = box_ct(b);        /* in-flow content extent, local */
    float abs_r = 0, abs_b = 0;                          /* out-of-flow extent (no end padding) */
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->type == BOX_TEXT || c->type == BOX_LINEBREAK) continue;
        if (c->type == BOX_INLINE) {
            for (int i = 0; i < c->frag_count; i++) {
                BoxFrag *f = &c->frags[i];
                float l = f->x - ox, t = f->y - oy;
                if (l < b->overflow_l) b->overflow_l = l;
                if (t < b->overflow_t) b->overflow_t = t;
                if (l + f->w > b->overflow_r) b->overflow_r = l + f->w;
                if (t + f->h > b->overflow_b) b->overflow_b = t + f->h;
                ext_r = kn_maxf(ext_r, l + f->w);
                ext_b = kn_maxf(ext_b, t + f->h);
            }
            continue;
        }
        if (c->container != b) continue;
        layout_overflow_union_child(b, c);
        if (c->flags & BOXF_OUT_OF_FLOW) continue;   /* counted from abs_children below */
        float cr = c->lx + kn_maxf(c->w, c->overflow_r);
        float cbt = c->ly + kn_maxf(c->h, c->overflow_b);
        if (box_is_inflow(c) && !(c->flags & BOXF_INLINE_LEVEL)) cbt = kn_maxf(cbt, c->ly + c->h + c->margin[CSS_BOTTOM]);
        ext_r = kn_maxf(ext_r, cr);
        ext_b = kn_maxf(ext_b, cbt);
    }
    for (int i = 0; i < b->abs_count; i++) {
        Box *a = b->abs_children[i];
        if (a->parent != b) layout_overflow_union_child(b, a);
        abs_r = kn_maxf(abs_r, a->lx + kn_maxf(a->w, a->overflow_r));
        abs_b = kn_maxf(abs_b, a->ly + kn_maxf(a->h, a->overflow_b));
    }
    /* line boxes (text extents) */
    for (int i = 0; i < b->line_count; i++) {
        LineBox *ln = &b->lines[i];
        ext_b = kn_maxf(ext_b, ln->y - oy + ln->h);
        for (int j = 0; j < ln->item_count; j++) {
            InlineItem *it = &ln->items[j];
            if (it->kind != ITEM_TEXT) continue;
            float l = it->x - ox, t = it->y - oy;
            if (l < b->overflow_l) b->overflow_l = l;
            if (l + it->w > b->overflow_r) b->overflow_r = l + it->w;
            if (t < b->overflow_t) b->overflow_t = t;
            if (t + it->h > b->overflow_b) b->overflow_b = t + it->h;
            ext_r = kn_maxf(ext_r, l + it->w);
        }
    }
    float client_x = b->border[CSS_LEFT] + ((b->flags & BOXF_RTL) ? b->scrollbar_w : 0);
    float client_y = b->border[CSS_TOP];
    b->scroll_w = kn_maxf(kn_maxf(box_client_w(b), ext_r - client_x + b->padding[CSS_RIGHT]), abs_r - client_x);
    b->scroll_h = kn_maxf(kn_maxf(box_client_h(b), ext_b - client_y + b->padding[CSS_BOTTOM]), abs_b - client_y);
    if (clips) {
        /* a clipping box does not leak its content's overflow */
        layout_reset_overflow(b);
    }
}

/* ------------------------------------------------------------------------- */
/* transforms                                                                 */
/* ------------------------------------------------------------------------- */

static void mat_mul(Mat2D *o, const Mat2D *A, const Mat2D *B)   /* o = A * B (B applied first) */
{
    Mat2D r;
    r.a = A->a * B->a + A->c * B->b;
    r.b = A->b * B->a + A->d * B->b;
    r.c = A->a * B->c + A->c * B->d;
    r.d = A->b * B->c + A->d * B->d;
    r.e = A->a * B->e + A->c * B->f + A->e;
    r.f = A->b * B->e + A->d * B->f + A->f;
    *o = r;
}

void layout_update_transform(Box *b)
{
    const CssComputed *s = b->style;
    Mat2D m = { 1, 0, 0, 1, 0, 0 };
    if (s->transform.count == 0) { b->xform = m; b->flags &= ~BOXF_HAS_TRANSFORM; return; }
    b->flags |= BOXF_HAS_TRANSFORM;
    float ox = layout_resolve_len(&s->transform_origin_x, b->w, b->w * 0.5f);
    float oy = layout_resolve_len(&s->transform_origin_y, b->h, b->h * 0.5f);
    if (s->transform_origin_x.kind != CSS_LEN_VALUE) ox = b->w * 0.5f;
    if (s->transform_origin_y.kind != CSS_LEN_VALUE) oy = b->h * 0.5f;
    Mat2D T1 = { 1, 0, 0, 1, ox, oy };
    Mat2D T2 = { 1, 0, 0, 1, -ox, -oy };
    m = T1;
    for (int i = 0; i < s->transform.count; i++) {
        const CssTransformOp *op = &s->transform.ops[i];
        Mat2D k = { 1, 0, 0, 1, 0, 0 };
        switch (op->kind) {
        case CSS_TRANSFORM_TRANSLATE:
            k.e = layout_resolve_len(&op->tx, b->w, 0);
            k.f = layout_resolve_len(&op->ty, b->h, 0);
            break;
        case CSS_TRANSFORM_SCALE: k.a = op->sx; k.d = op->sy; break;
        case CSS_TRANSFORM_ROTATE: {
            float r = op->angle_deg * 3.14159265358979f / 180.0f;
            k.a = cosf(r); k.b = sinf(r); k.c = -sinf(r); k.d = cosf(r);
            break; }
        case CSS_TRANSFORM_SKEW: {
            k.c = tanf(op->angle_deg * 3.14159265358979f / 180.0f);
            k.b = tanf(op->angle2_deg * 3.14159265358979f / 180.0f);
            break; }
        case CSS_TRANSFORM_MATRIX: k = op->m; break;
        default: break;
        }
        mat_mul(&m, &m, &k);
    }
    mat_mul(&m, &m, &T2);
    b->xform = m;
}

/* ------------------------------------------------------------------------- */
/* intrinsic widths                                                           */
/* ------------------------------------------------------------------------- */

static void replaced_size(LayoutTree *t, Box *b, float cb_w, float cb_h, float *out_w, float *out_h);

float layout_intrinsic_width(LayoutTree *t, Box *b, int max_content)
{
    const CssComputed *s = b->style;
    if (b->intrinsic_gen == t->generation && b->min_content_w >= 0 && b->max_content_w >= 0)
        return max_content ? b->max_content_w : b->min_content_w;

    layout_resolve_edges(t, b, 0.0f);   /* percentages -> 0 in intrinsic sizing */
    float minw, maxw;
    if (box_is_replaced(b)) {
        float w, h;
        replaced_size(t, b, -1.0f, -1.0f, &w, &h);
        minw = maxw = w;
    } else if (s->width.kind == CSS_LEN_VALUE && s->width.pct == 0.0f) {
        minw = maxw = layout_box_sizing_w(b, s->width.px);
    } else {
        float cmin = 0, cmax = 0;
        switch (b->type) {
        case BOX_FLEX:  layout_flex_intrinsic(t, b, &cmin, &cmax); break;
        case BOX_GRID:  layout_grid_intrinsic(t, b, &cmin, &cmax); break;
        case BOX_TABLE: layout_table_intrinsic(t, b, &cmin, &cmax); break;
        default:
            if (b->flags & BOXF_HAS_LINES) layout_inline_intrinsic(t, b, &cmin, &cmax);
            else {
                float fl_min = 0, fl_max = 0;
                for (Box *c = b->first_child; c; c = c->next) {
                    if (c->flags & BOXF_OUT_OF_FLOW) continue;
                    if (c->type == BOX_TEXT) continue;
                    float cw_min = layout_intrinsic_margin_width(t, c, 0, 0.0f);
                    float cw_max = layout_intrinsic_margin_width(t, c, 1, 0.0f);
                    if (c->flags & BOXF_FLOAT) {
                        /* consecutive floats sit side by side */
                        fl_min = kn_maxf(fl_min, cw_min);
                        fl_max += cw_max;
                        continue;
                    }
                    if (fl_max > cmax) cmax = fl_max;
                    if (fl_min > cmin) cmin = fl_min;
                    fl_max = 0; fl_min = 0;
                    if (cw_min > cmin) cmin = cw_min;
                    if (cw_max > cmax) cmax = cw_max;
                }
                if (fl_max > cmax) cmax = fl_max;
                if (fl_min > cmin) cmin = fl_min;
            }
        }
        float extras = box_border_padding_w(b) + (s->overflow_y == CSS_OVERFLOW_SCROLL ? t->ctx->scrollbar_default_w : 0);
        if (s->width.kind == CSS_LEN_MIN_CONTENT) cmax = cmin;
        else if (s->width.kind == CSS_LEN_MAX_CONTENT) cmin = cmax;
        minw = cmin + extras;
        maxw = cmax + extras;
    }
    layout_apply_min_max_w(b, 0.0f, &minw);
    layout_apply_min_max_w(b, 0.0f, &maxw);
    if (maxw < minw) maxw = minw;
    b->min_content_w = minw;
    b->max_content_w = maxw;
    b->intrinsic_gen = t->generation;
    return max_content ? maxw : minw;
}

float layout_intrinsic_margin_width(LayoutTree *t, Box *b, int max_content, float cb_w)
{
    float w = layout_intrinsic_width(t, b, max_content);
    const CssComputed *s = b->style;
    float base = cb_w < 0 ? 0 : cb_w;
    float ml = layout_resolve_len(&s->margin[CSS_LEFT], base, 0);
    float mr = layout_resolve_len(&s->margin[CSS_RIGHT], base, 0);
    return w + ml + mr;
}

/* ------------------------------------------------------------------------- */
/* replaced sizing                                                            */
/* ------------------------------------------------------------------------- */

static void replaced_size(LayoutTree *t, Box *b, float cb_w, float cb_h, float *out_w, float *out_h)
{
    (void)t;
    const CssComputed *s = b->style;
    int w_auto, h_auto;
    float w = layout_resolve_len_auto(&s->width, cb_w, &w_auto);
    float h = layout_resolve_len_auto(&s->height, cb_h, &h_auto);
    /* presentational width/height attributes (img) act as CSS sizes */
    if (w_auto && b->attr_w >= 0) { w = b->attr_w; w_auto = 0; }
    if (h_auto && b->attr_h >= 0) { h = b->attr_h; h_auto = 0; }
    float iw = b->intrinsic_w, ih = b->intrinsic_h;
    if (b->flags & BOXF_INTRINSIC_BORDER) {
        /* UA sizes are border-box with the UA border/padding folded in */
        iw = kn_maxf(0.0f, iw - box_border_padding_w(b));
        ih = kn_maxf(0.0f, ih - box_border_padding_h(b));
    }
    /* form controls have no intrinsic aspect ratio (Chrome): auto height stays the intrinsic height */
    float ratio = (iw > 0 && ih > 0 && b->type != BOX_INPUT && b->type != BOX_SELECT) ? ih / iw : 0;
    if (w_auto && h_auto) { w = iw; h = ih; }
    else if (w_auto) {
        /* width from height via ratio; border-box height given */
        float ch = s->box_sizing == CSS_BOX_SIZING_BORDER_BOX ? kn_maxf(0, h - box_border_padding_h(b)) : h;
        w = ratio > 0 ? ch / ratio : iw;
        if (s->box_sizing == CSS_BOX_SIZING_BORDER_BOX) w += box_border_padding_w(b);
    } else if (h_auto) {
        float cw = s->box_sizing == CSS_BOX_SIZING_BORDER_BOX ? kn_maxf(0, w - box_border_padding_w(b)) : w;
        h = ratio > 0 ? cw * ratio : ih;   /* content height; bh below adds border + padding */
    }
    float bw = (w_auto && h_auto) || (w_auto) ? (s->box_sizing == CSS_BOX_SIZING_BORDER_BOX && !w_auto ? w : (w_auto ? w + box_border_padding_w(b) : w)) : layout_box_sizing_w(b, w);
    float bh = (h_auto) ? h + box_border_padding_h(b) : layout_box_sizing_h(b, h);
    if (w_auto && !h_auto) bw = w + box_border_padding_w(b) - (s->box_sizing == CSS_BOX_SIZING_BORDER_BOX ? box_border_padding_w(b) : 0);
    layout_apply_min_max_w(b, cb_w, &bw);
    layout_apply_min_max_h(b, cb_h, &bh);
    *out_w = bw; *out_h = bh;
}

/* ------------------------------------------------------------------------- */
/* width of block-level boxes                                                 */
/* ------------------------------------------------------------------------- */

static float shrink_to_fit(LayoutTree *t, Box *b, float available)
{
    float minw = layout_intrinsic_width(t, b, 0);
    float maxw = layout_intrinsic_width(t, b, 1);
    return kn_minf(kn_maxf(minw, available), maxw);
}

/* Computes b->w (border box) and resolves auto margins for an in-flow block
 * or a shrink-to-fit box.  `avail` is the width available to the margin box. */
static void compute_width(LayoutTree *t, Box *b, const LayoutInput *in)
{
    const CssComputed *s = b->style;
    float cb_w = in->cb_w;
    float avail = in->avail_w;
    int ml_auto = s->margin[CSS_LEFT].kind == CSS_LEN_AUTO;
    int mr_auto = s->margin[CSS_RIGHT].kind == CSS_LEN_AUTO;
    float extras = layout_horizontal_extras(b);
    float bw;

    if (box_is_replaced(b)) {
        float h;
        replaced_size(t, b, cb_w, in->cb_h, &bw, &h);
    } else if (s->width.kind == CSS_LEN_VALUE) {
        bw = layout_box_sizing_w(b, kn_maxf(0.0f, layout_resolve_len(&s->width, cb_w, 0)));
        layout_apply_min_max_w(b, cb_w, &bw);
    } else if (s->width.kind == CSS_LEN_MIN_CONTENT || s->width.kind == CSS_LEN_MAX_CONTENT || s->width.kind == CSS_LEN_FIT_CONTENT
               || in->shrink_to_fit || b->type == BOX_TABLE) {
        float avail_border = avail - (ml_auto ? 0 : b->margin[CSS_LEFT]) - (mr_auto ? 0 : b->margin[CSS_RIGHT]);
        if (s->width.kind == CSS_LEN_MIN_CONTENT) bw = layout_intrinsic_width(t, b, 0);
        else if (s->width.kind == CSS_LEN_MAX_CONTENT) bw = layout_intrinsic_width(t, b, 1);
        else bw = shrink_to_fit(t, b, avail_border);
        layout_apply_min_max_w(b, cb_w, &bw);
    } else {
        /* auto: fill */
        float mlv = ml_auto ? 0 : b->margin[CSS_LEFT], mrv = mr_auto ? 0 : b->margin[CSS_RIGHT];
        bw = avail - mlv - mrv;
        if (bw < extras) bw = extras;
        layout_apply_min_max_w(b, cb_w, &bw);
        b->margin[CSS_LEFT] = mlv; b->margin[CSS_RIGHT] = mrv;
        b->w = bw;
        return;
    }
    b->w = bw;
    if (in->shrink_to_fit) {
        if (ml_auto) b->margin[CSS_LEFT] = 0;
        if (mr_auto) b->margin[CSS_RIGHT] = 0;
        return;
    }
    /* auto margins / over-constrained for a block-level box with a definite width */
    float rem = avail - bw;
    if (ml_auto && mr_auto) { b->margin[CSS_LEFT] = b->margin[CSS_RIGHT] = kn_maxf(rem, 0) * 0.5f; }
    else if (ml_auto) b->margin[CSS_LEFT] = rem - b->margin[CSS_RIGHT];
    else if (mr_auto) b->margin[CSS_RIGHT] = rem - b->margin[CSS_LEFT];
    else {
        if (b->flags & BOXF_RTL) b->margin[CSS_LEFT] = rem - b->margin[CSS_RIGHT];
        else b->margin[CSS_RIGHT] = rem - b->margin[CSS_LEFT];
    }
    if (ml_auto && mr_auto && rem < 0) {
        /* negative free space: treat auto as 0 and over-constrain like ltr */
        b->margin[CSS_LEFT] = 0; b->margin[CSS_RIGHT] = rem;
    }
}

int layout_is_definite_height_child(const Box *b, const LayoutInput *in)
{
    const CssComputed *s = b->style;
    if (in->override_h >= 0 && in->definite_h) return 1;
    if (s->height.kind == CSS_LEN_VALUE && !box_is_replaced(b)) {
        if (s->height.pct == 0.0f) return 1;
        return in->cb_h >= 0;
    }
    if (b->flags & BOXF_OUT_OF_FLOW) {
        return s->inset[CSS_TOP].kind == CSS_LEN_VALUE && s->inset[CSS_BOTTOM].kind == CSS_LEN_VALUE;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* scrollbars                                                                 */
/* ------------------------------------------------------------------------- */

static float scrollbar_width_for(LayoutTree *t, Box *b)
{
    float w = t->ctx->scrollbar_default_w;
    if (t->ctx->scrollbar_width) w = t->ctx->scrollbar_width(t->ctx->user, b->node, w);
    return w;
}

void layout_finish_scroll(LayoutTree *t, Box *b)
{
    (void)t;
    float max_x = kn_maxf(0.0f, b->scroll_w - box_content_w(b));
    float max_y = kn_maxf(0.0f, b->scroll_h - box_content_h(b));
    b->scroll_x = kn_clampf(b->scroll_x, 0, max_x);
    b->scroll_y = kn_clampf(b->scroll_y, 0, max_y);
}

/* ------------------------------------------------------------------------- */
/* the box                                                                    */
/* ------------------------------------------------------------------------- */

static void layout_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h, LayoutOutput *out)
{
    switch (b->type) {
    case BOX_FLEX:  layout_flex_contents(t, b, in, content_h); break;
    case BOX_GRID:  layout_grid_contents(t, b, in, content_h); break;
    case BOX_TABLE: layout_table_contents(t, b, in, content_h); break;
    case BOX_IMAGE: case BOX_CANVAS: case BOX_VIDEO: case BOX_INPUT: case BOX_SELECT: *content_h = 0; break;
    default:
        if (b->flags & BOXF_HAS_LINES) layout_inline_contents(t, b, in, content_h);
        else layout_block_contents(t, b, in, content_h, out);
        break;
    }
}

void layout_box(LayoutTree *t, Box *b, const LayoutInput *in_, LayoutOutput *out)
{
    LayoutInput in = *in_;
    const CssComputed *s = b->style;
    LayoutOutput o;
    memset(&o, 0, sizeof(o));
    if (b->dirty & BOX_DIRTY_SELF || !b->font) layout_resolve_font(t, b);
    layout_free_lines(b);
    b->frag_count = 0;
    b->paint_dirty = 1;
    layout_resolve_edges(t, b, in.cb_w);
    b->flags &= ~(BOXF_VBAR | BOXF_HBAR);
    b->bar_w = 0;
    if (b->flags & BOXF_SCROLL_CONTAINER) {
        b->bar_w = scrollbar_width_for(t, b);
        if (s->overflow_y == CSS_OVERFLOW_SCROLL) b->flags |= BOXF_VBAR;
        if (s->overflow_x == CSS_OVERFLOW_SCROLL) b->flags |= BOXF_HBAR;
    }
    layout_set_bars(b);

    /* width */
    if (in.override_w >= 0) {
        b->w = in.override_w;
        if (s->margin[CSS_LEFT].kind == CSS_LEN_AUTO) b->margin[CSS_LEFT] = 0;
        if (s->margin[CSS_RIGHT].kind == CSS_LEN_AUTO) b->margin[CSS_RIGHT] = 0;
    } else {
        compute_width(t, b, &in);
    }
    if (s->margin[CSS_TOP].kind == CSS_LEN_AUTO) b->margin[CSS_TOP] = 0;
    if (s->margin[CSS_BOTTOM].kind == CSS_LEN_AUTO) b->margin[CSS_BOTTOM] = 0;

    /* tentative definite height for percentage children */
    int h_definite = layout_is_definite_height_child(b, &in);
    float tentative_h = -1.0f;
    if (in.override_h >= 0) tentative_h = in.override_h;
    else if (s->height.kind == CSS_LEN_VALUE && !(s->height.pct != 0 && in.cb_h < 0))
        tentative_h = layout_box_sizing_h(b, kn_maxf(0.0f, layout_resolve_len(&s->height, in.cb_h, 0)));
    if (tentative_h >= 0) layout_apply_min_max_h(b, in.cb_h, &tentative_h);

    for (int pass = 0; pass < 3; pass++) {
        float content_h = 0;
        LayoutInput cin = in;
        if (tentative_h >= 0 && h_definite) { cin.cb_h = tentative_h - layout_vertical_extras(b); cin.definite_h = 1; }
        else { cin.cb_h = LAYOUT_INDEFINITE; cin.definite_h = 0; }
        if (b->flags & BOXF_BFC_ROOT) { cin.floats = NULL; }
        memset(&o, 0, sizeof(o));
        if (box_is_replaced(b)) {
            float w, h;
            replaced_size(t, b, in.cb_w, in.cb_h, &w, &h);
            if (in.override_w < 0) b->w = w;
            content_h = h - box_border_padding_h(b);
        } else {
            layout_contents(t, b, &cin, &content_h, &o);
        }

        /* height */
        float bh;
        if (in.override_h >= 0) bh = in.override_h;
        else if (tentative_h >= 0) bh = tentative_h;
        else if (box_is_replaced(b)) bh = content_h + box_border_padding_h(b);
        else bh = content_h + layout_vertical_extras(b);
        if (in.override_h < 0) layout_apply_min_max_h(b, in.cb_h, &bh);
        b->h = bh;

        compute_overflow(b, 0);
        if (!(b->flags & BOXF_SCROLL_CONTAINER) || pass == 2) break;
        int need_relayout = 0;
        if (s->overflow_y == CSS_OVERFLOW_AUTO && !(b->flags & BOXF_VBAR) && b->scroll_h > box_client_h(b) + 0.01f) {
            b->flags |= BOXF_VBAR; need_relayout = 1;
        }
        if (s->overflow_x == CSS_OVERFLOW_AUTO && !(b->flags & BOXF_HBAR) && b->scroll_w > box_client_w(b) + 0.01f) {
            b->flags |= BOXF_HBAR; need_relayout = 1;
        }
        if (!need_relayout) break;
        layout_set_bars(b);
        /* the bar reduces the content box; auto widths stay (bars live inside the border box) */
        layout_free_lines(b);
        b->frag_count = 0;
    }

    /* own margins into the output */
    float mt = b->margin[CSS_TOP], mb = b->margin[CSS_BOTTOM];
    int can_collapse = b->type == BOX_BLOCK && !(b->flags & BOXF_BFC_ROOT) && box_is_inflow(b) && !(b->flags & BOXF_ATOMIC);
    if (can_collapse && o.collapses_through && b->h <= 0.001f && b->style->height.kind != CSS_LEN_VALUE
        && b->style->min_height.kind != CSS_LEN_VALUE && !(b->flags & BOXF_HAS_LINES)) {
        float pos = kn_maxf(kn_maxf(o.top_pos, o.bot_pos), kn_maxf(kn_maxf(mt, mb), 0));
        float neg = kn_minf(kn_minf(o.top_neg, o.bot_neg), kn_minf(kn_minf(mt, mb), 0));
        out->top_pos = out->bot_pos = pos;
        out->top_neg = out->bot_neg = neg;
        out->collapses_through = 1;
    } else {
        int top_open = can_collapse && b->border[CSS_TOP] == 0 && b->padding[CSS_TOP] == 0;
        int bot_open = can_collapse && b->border[CSS_BOTTOM] == 0 && b->padding[CSS_BOTTOM] == 0
                    && b->style->height.kind != CSS_LEN_VALUE && b->style->min_height.kind != CSS_LEN_VALUE;
        out->top_pos = kn_maxf(mt, 0); out->top_neg = kn_minf(mt, 0);
        out->bot_pos = kn_maxf(mb, 0); out->bot_neg = kn_minf(mb, 0);
        if (top_open) { out->top_pos = kn_maxf(out->top_pos, o.top_pos); out->top_neg = kn_minf(out->top_neg, o.top_neg); }
        if (bot_open) { out->bot_pos = kn_maxf(out->bot_pos, o.bot_pos); out->bot_neg = kn_minf(out->bot_neg, o.bot_neg); }
        out->collapses_through = 0;
    }

    layout_update_transform(b);
    layout_position_abs_children(t, b);
    if (b->flags & BOXF_SCROLL_CONTAINER) {
        if (b->abs_count) compute_overflow(b, 0);   /* abs boxes extend the scrollable overflow */
        layout_finish_scroll(t, b);
        layout_update_sticky(b);
    }
    b->dirty = 0;
}

/* ------------------------------------------------------------------------- */
/* block children                                                             */
/* ------------------------------------------------------------------------- */

typedef struct MarginSet { float pos, neg; } MarginSet;
static inline void ms_add(MarginSet *m, float pos, float neg) { if (pos > m->pos) m->pos = pos; if (neg < m->neg) m->neg = neg; }
static inline float ms_value(const MarginSet *m) { return m->pos + m->neg; }

void layout_block_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h, LayoutOutput *out)
{
    FloatCtx own_floats;
    memset(&own_floats, 0, sizeof(own_floats));
    FloatCtx *floats = in->floats;
    int own_bfc = (b->flags & BOXF_BFC_ROOT) || !floats;
    if (own_bfc) floats = &own_floats;

    float content_x = box_cl(b), content_y = box_ct(b);
    float content_w = box_cw(b);
    float bfc_x = own_bfc ? 0 : in->bfc_x;     /* b's border box in BFC coords */
    float bfc_y = own_bfc ? 0 : in->bfc_y;
    float cb_h = in->cb_h;

    float y = 0;                               /* relative to content top */
    MarginSet pending = { 0, 0 };
    int top_open = (b->type == BOX_BLOCK) && !own_bfc && b->border[CSS_TOP] == 0 && b->padding[CSS_TOP] == 0 && box_is_inflow(b);
    int any_content = 0;
    MarginSet top_out = { 0, 0 };

    for (Box *c = b->first_child; c; c = c->next) {
        if (c->flags & BOXF_OUT_OF_FLOW) {
            c->static_x = content_x;
            c->static_y = content_y + y + (top_open ? 0 : ms_value(&pending)); c->static_fx = c->static_fy = 0;
            continue;
        }
        if (c->type == BOX_TEXT || c->type == BOX_LINEBREAK) continue;   /* whitespace leftovers */
        layout_resolve_edges(t, c, content_w);

        if (c->flags & BOXF_FLOAT) {
            LayoutInput fin = *in;
            fin.cb_w = content_w; fin.cb_h = cb_h; fin.avail_w = content_w;
            fin.floats = NULL; fin.shrink_to_fit = 1; fin.override_w = fin.override_h = -1;
            fin.is_flex_item = 0;
            LayoutOutput fo;
            layout_box(t, c, &fin, &fo);
            float y_flow = y + (top_open ? 0 : ms_value(&pending));
            float clear_y = floats_clear_y(floats, c->style->clear);
            float y_min = bfc_y + content_y + y_flow;
            if (clear_y > y_min) y_min = clear_y;
            floats_place(t, floats, c, y_min, bfc_x + content_x, bfc_x + content_x + content_w);
            FloatEntry *e = &floats->data[floats->len - 1];
            c->lx = e->x - bfc_x + c->margin[CSS_LEFT];
            c->ly = e->y - bfc_y + c->margin[CSS_TOP];
            layout_relative_offset(c, content_w, cb_h);
            continue;
        }

        /* block-level in-flow child */
        float mt = c->margin[CSS_TOP];
        MarginSet est = pending;
        ms_add(&est, kn_maxf(mt, 0), kn_minf(mt, 0));
        float est_y = top_open ? y : y + ms_value(&est);
        /* clearance */
        float clear_y = floats_clear_y(floats, c->style->clear);
        int cleared = 0;
        if (c->style->clear != CSS_CLEAR_NONE && clear_y > -LAYOUT_INF / 2) {
            float border_top_bfc = bfc_y + content_y + est_y;
            if (border_top_bfc < clear_y) { est_y = clear_y - bfc_y - content_y; cleared = 1; }
        }
        LayoutInput cin;
        memset(&cin, 0, sizeof(cin));
        cin.cb_w = content_w; cin.cb_h = cb_h; cin.avail_w = content_w;
        cin.floats = floats;
        cin.override_w = cin.override_h = -1;
        /* in-flow blocks next to floats: a BFC root shrinks to the space beside floats */
        if ((c->flags & BOXF_BFC_ROOT) && floats->len) {
            float l, r;
            float yy = bfc_y + content_y + est_y;
            floats_avail(floats, yy, 1.0f, bfc_x + content_x, bfc_x + content_x + content_w, &l, &r);
            if (r - l < content_w) {
                /* try the fitting band like Chrome: if the box does not fit beside floats, move it down */
                float need = layout_intrinsic_width(t, c, 0) + c->margin[CSS_LEFT] + c->margin[CSS_RIGHT];
                while (r - l < need - 0.001f && floats_next_y(floats, yy) > yy) {
                    yy = floats_next_y(floats, yy);
                    floats_avail(floats, yy, 1.0f, bfc_x + content_x, bfc_x + content_x + content_w, &l, &r);
                }
                est_y = yy - bfc_y - content_y; cleared = 1;
                cin.avail_w = r - l;
                cin.bfc_x = bfc_x + content_x + (l - (bfc_x + content_x));
            }
        }
        LayoutOutput co;
        int relayouts = 0;
        float y_border;
        /* floats the child adds to the shared BFC list; a re-layout must not add them twice */
        size_t float_mark = floats->len;
        for (;;) {
            floats->len = float_mark;
            cin.bfc_x = bfc_x + content_x + c->margin[CSS_LEFT] + (cin.avail_w < content_w ? cin.bfc_x - (bfc_x + content_x) : 0);
            cin.bfc_y = bfc_y + content_y + est_y;
            layout_box(t, c, &cin, &co);
            /* true collapsed top margin set */
            MarginSet top = pending;
            ms_add(&top, co.top_pos, co.top_neg);
            if (cleared) { y_border = est_y; break; }
            y_border = top_open ? y : y + ms_value(&top);
            if (co.collapses_through) { y_border = top_open ? y : y + ms_value(&top); break; }
            if (fabsf(y_border - est_y) < 0.01f || relayouts++ >= 2) break;
            est_y = y_border;
        }
        /* horizontal position */
        float cx = content_x + c->margin[CSS_LEFT];
        if (cin.avail_w < content_w) cx = cin.bfc_x - bfc_x;
        if ((b->flags & BOXF_RTL) && !(c->flags & BOXF_BFC_ROOT)) {
            /* rtl block: over-constrained margins already pushed the box right via margin-left */
        }
        c->lx = cx;
        c->ly = content_y + y_border;

        if (co.collapses_through && !cleared) {
            ms_add(&pending, co.top_pos, co.top_neg);
            ms_add(&pending, co.bot_pos, co.bot_neg);
            continue;
        }
        if (top_open) {
            top_out = pending;
            ms_add(&top_out, co.top_pos, co.top_neg);
            top_open = 0;
            if (cleared) {
                /* clearance: the margins above are applied before the clearance, i.e. moved out */
            }
        }
        any_content = 1;
        y = y_border + c->h;
        pending.pos = kn_maxf(co.bot_pos, 0); pending.neg = kn_minf(co.bot_neg, 0);
        pending.pos = co.bot_pos; pending.neg = co.bot_neg;
        layout_relative_offset(c, content_w, cb_h);
    }

    int bot_open = (b->type == BOX_BLOCK) && !own_bfc && b->border[CSS_BOTTOM] == 0 && b->padding[CSS_BOTTOM] == 0
                && b->style->height.kind != CSS_LEN_VALUE && b->style->min_height.kind != CSS_LEN_VALUE && box_is_inflow(b);
    float h = y;
    if (!any_content) {
        /* everything collapsed through (or no children) */
        out->collapses_through = (b->type == BOX_BLOCK) && !own_bfc && box_is_inflow(b);
        out->top_pos = out->bot_pos = pending.pos;
        out->top_neg = out->bot_neg = pending.neg;
        if (!top_open && !out->collapses_through) { h = y + ms_value(&pending); out->top_pos = out->top_neg = out->bot_pos = out->bot_neg = 0; }
        if (top_open && !bot_open) { /* margins go out the top only */ h = 0; out->bot_pos = out->bot_neg = 0; out->collapses_through = 0; }
    } else {
        out->collapses_through = 0;
        out->top_pos = top_out.pos; out->top_neg = top_out.neg;
        if (bot_open) { out->bot_pos = pending.pos; out->bot_neg = pending.neg; }
        else { h = y + ms_value(&pending); out->bot_pos = out->bot_neg = 0; }
    }
    if (own_bfc) {
        /* a BFC root's auto height contains its floats */
        float fb = floats_bottom(floats);
        if (fb > -LAYOUT_INF / 2) {
            float fh = fb - content_y;
            if (fh > h) h = fh;
        }
    }
    *content_h = kn_maxf(h, 0);
    if (own_bfc) free(own_floats.data);

    /* Baselines (CSS 2.1 10.8.1, LayoutNG): a block container's first/last
     * baseline is the first/last line box baseline found through its in-flow
     * block-level descendants (flex/grid/table children export theirs).  Used
     * by inline-block alignment and by flex/grid/table baseline lookups. */
    b->first_baseline = -1.0f;
    b->baseline_offset = -1.0f;
    for (Box *c = b->first_child; c; c = c->next) {
        if (!box_is_inflow(c) || c->type == BOX_TEXT || c->type == BOX_LINEBREAK || box_is_replaced(c)) continue;
        float bl = c->first_baseline >= 0 ? c->first_baseline : c->baseline_offset;
        if (bl >= 0) { b->first_baseline = c->ly - c->rel_y + bl; break; }
    }
    for (Box *c = b->last_child; c; c = c->prev) {
        if (!box_is_inflow(c) || c->type == BOX_TEXT || c->type == BOX_LINEBREAK || box_is_replaced(c)) continue;
        if (c->flags & BOXF_SCROLL_CONTAINER) continue;   /* its content may be scrolled away */
        float bl = c->baseline_offset >= 0 ? c->baseline_offset : c->first_baseline;
        if (bl >= 0) { b->baseline_offset = c->ly - c->rel_y + bl; break; }
    }
}

/* relative positioning offset (does not affect flow); folded into lx/ly */
void layout_relative_offset(Box *c, float cb_w, float cb_h)
{
    c->rel_x = c->rel_y = 0;
    if (c->style->position != CSS_POSITION_RELATIVE) return;
    int a;
    float l = layout_resolve_len_auto(&c->style->inset[CSS_LEFT], cb_w, &a);
    if (a) { l = -layout_resolve_len_auto(&c->style->inset[CSS_RIGHT], cb_w, &a); if (a) l = 0; }
    float tp = layout_resolve_len_auto(&c->style->inset[CSS_TOP], cb_h, &a);
    if (a) { tp = -layout_resolve_len_auto(&c->style->inset[CSS_BOTTOM], cb_h, &a); if (a) tp = 0; }
    c->rel_x = l; c->rel_y = tp;
    c->lx += l; c->ly += tp;
}

/* ------------------------------------------------------------------------- */
/* absolute positioning                                                       */
/* ------------------------------------------------------------------------- */

static void static_pos_in_cb(Box *a, Box *cb, float *sx, float *sy)
{
    /* static_x/y are in the coordinates of a's static parent: nearest non-inline ancestor */
    Box *p = a->parent;
    while (p && p->type == BOX_INLINE) p = p->parent;
    float x = a->static_x, y = a->static_y;
    while (p && p != cb) {
        x += p->lx; y += p->ly;
        p = p->container;
    }
    *sx = x; *sy = y;
}

void layout_position_abs_children(LayoutTree *t, Box *cb)
{
    if (!cb->abs_count) return;
    float cbx, cby, cbw, cbh;
    int is_icb = (cb == t->root) && !(cb->flags & BOXF_POSITIONED) && !(cb->flags & BOXF_HAS_TRANSFORM);
    if (is_icb) { cbx = 0; cby = 0; cbw = t->viewport_w; cbh = t->viewport_h; }
    else {
        cbx = cb->border[CSS_LEFT]; cby = cb->border[CSS_TOP];
        cbw = box_client_w(cb);
        cbh = box_client_h(cb);
        if (cb->flags & BOXF_RTL) cbx += cb->scrollbar_w;
    }
    for (int i = 0; i < cb->abs_count; i++) {
        Box *a = cb->abs_children[i];
        const CssComputed *s = a->style;
        layout_resolve_edges(t, a, cbw);
        int l_auto, r_auto, t_auto, b_auto;
        float left = layout_resolve_len_auto(&s->inset[CSS_LEFT], cbw, &l_auto);
        float right = layout_resolve_len_auto(&s->inset[CSS_RIGHT], cbw, &r_auto);
        float top = layout_resolve_len_auto(&s->inset[CSS_TOP], cbh, &t_auto);
        float bottom = layout_resolve_len_auto(&s->inset[CSS_BOTTOM], cbh, &b_auto);
        int w_auto = s->width.kind != CSS_LEN_VALUE && !box_is_replaced(a);
        int h_auto = s->height.kind != CSS_LEN_VALUE && !box_is_replaced(a);
        int ml_auto = s->margin[CSS_LEFT].kind == CSS_LEN_AUTO, mr_auto = s->margin[CSS_RIGHT].kind == CSS_LEN_AUTO;
        int mt_auto = s->margin[CSS_TOP].kind == CSS_LEN_AUTO, mb_auto = s->margin[CSS_BOTTOM].kind == CSS_LEN_AUTO;
        float sx, sy;
        static_pos_in_cb(a, cb, &sx, &sy);
        sx -= cbx; sy -= cby;
        int rtl = (cb->flags & BOXF_RTL) != 0;

        LayoutInput in;
        memset(&in, 0, sizeof(in));
        in.cb_w = cbw; in.cb_h = cbh; in.override_w = in.override_h = -1;
        in.floats = NULL;
        /* horizontal */
        if (l_auto && r_auto) {
            if (rtl) { right = cbw - sx; r_auto = 0; } else { left = sx; l_auto = 0; }
        }
        float ml = ml_auto ? 0 : a->margin[CSS_LEFT], mr = mr_auto ? 0 : a->margin[CSS_RIGHT];
        if (w_auto && !l_auto && !r_auto) {
            float bw = cbw - left - right - ml - mr;
            if (bw < box_border_padding_w(a)) bw = box_border_padding_w(a);
            layout_apply_min_max_w(a, cbw, &bw);
            in.override_w = bw;
            in.avail_w = bw + ml + mr;
        } else {
            in.shrink_to_fit = w_auto;
            in.avail_w = cbw - (l_auto ? 0 : left) - (r_auto ? 0 : right) - ml - mr;
            if (!w_auto) in.avail_w = cbw;   /* definite width: no shrink */
        }
        /* vertical */
        if (t_auto && b_auto) { top = sy; t_auto = 0; }
        float mt = mt_auto ? 0 : a->margin[CSS_TOP], mb = mb_auto ? 0 : a->margin[CSS_BOTTOM];
        if (h_auto && !t_auto && !b_auto) {
            float bh = cbh - top - bottom - mt - mb;
            if (bh < box_border_padding_h(a)) bh = box_border_padding_h(a);
            layout_apply_min_max_h(a, cbh, &bh);
            in.override_h = bh;
            in.definite_h = 1;
        }
        LayoutOutput o;
        layout_box(t, a, &in, &o);
        /* flex static position: align the margin box inside the container's content area */
        if (s->inset[CSS_LEFT].kind == CSS_LEN_AUTO && s->inset[CSS_RIGHT].kind == CSS_LEN_AUTO && a->static_fx != 0) {
            float free_w = a->static_aw - (a->w + ml + mr);
            if (rtl) right -= a->static_fx * free_w; else left += a->static_fx * free_w;
        }
        if (s->inset[CSS_TOP].kind == CSS_LEN_AUTO && s->inset[CSS_BOTTOM].kind == CSS_LEN_AUTO && a->static_fy != 0 && a->static_ah >= 0) {
            float free_h = a->static_ah - (a->h + mt + mb);
            top += a->static_fy * free_h;
        }
        /* resolve remaining auto margins / insets */
        if (!l_auto && !r_auto && !w_auto) {
            float rem = cbw - left - right - a->w;
            if (ml_auto && mr_auto) { ml = mr = rem * 0.5f; if (rem < 0) { ml = 0; mr = rem; } }
            else if (ml_auto) ml = rem - mr;
            else if (mr_auto) mr = rem - ml;
            else { if (rtl) left = cbw - right - a->w - ml - mr; else right = cbw - left - a->w - ml - mr; }
        } else if (!l_auto && !r_auto && w_auto) {
            /* width stretched */
        } else if (l_auto) {
            left = cbw - right - a->w - ml - mr;
        }
        if (!t_auto && !b_auto && !h_auto) {
            float rem = cbh - top - bottom - a->h;
            if (mt_auto && mb_auto) { mt = mb = rem * 0.5f; if (rem < 0) { mt = 0; mb = rem; } }
            else if (mt_auto) mt = rem - mb;
            else if (mb_auto) mb = rem - mt;
        } else if (t_auto) {
            top = cbh - bottom - a->h - mt - mb;
        }
        a->margin[CSS_LEFT] = ml; a->margin[CSS_RIGHT] = mr; a->margin[CSS_TOP] = mt; a->margin[CSS_BOTTOM] = mb;
        a->lx = cbx + left + ml;
        a->ly = cby + top + mt;
        a->rel_x = a->rel_y = 0;
    }
    /* abs children may extend the container's overflow (only when it's not clipping) */
    for (int i = 0; i < cb->abs_count; i++) layout_overflow_union_child(cb, cb->abs_children[i]);
    if (cb->flags & BOXF_CLIPS) layout_reset_overflow(cb);
}

/* ------------------------------------------------------------------------- */
/* sticky                                                                     */
/* ------------------------------------------------------------------------- */

static void box_offset_to_ancestor(const Box *b, const Box *anc, float *ox, float *oy)
{
    float x = 0, y = 0;
    for (const Box *p = b; p && p != anc; p = p->container) { x += p->lx; y += p->ly; }
    *ox = x; *oy = y;
}

static void update_sticky_rec(Box *scroller, Box *b)
{
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->flags & BOXF_SCROLL_CONTAINER) { update_sticky_rec(c, c); continue; }
        if ((c->flags & BOXF_STICKY) && c->scroller == scroller) {
            const CssComputed *s = c->style;
            float ox, oy;
            box_offset_to_ancestor(c, scroller, &ox, &oy);  /* in scroller border-box coords, unscrolled */
            float port_x = scroller->border[CSS_LEFT] + ((scroller->flags & BOXF_RTL) ? scroller->scrollbar_w : 0);
            float port_y = scroller->border[CSS_TOP];
            float port_w = box_client_w(scroller);
            float port_h = box_client_h(scroller);
            /* containing block = parent's content box */
            Box *cbp = c->container;
            float cbx, cby;
            box_offset_to_ancestor(cbp, scroller, &cbx, &cby);
            float cb_top = cby + box_ct(cbp);
            float cb_bot = cb_top + box_ch(cbp);
            float cb_left = cbx + box_cl(cbp);
            float cb_right = cb_left + box_cw(cbp);
            float dx = 0, dy = 0;
            int a;
            float tp = layout_resolve_len_auto(&s->inset[CSS_TOP], port_h, &a);
            if (!a) {
                float want = (port_y + scroller->scroll_y + tp) - (oy - c->margin[CSS_TOP]) ;
                float max_shift = cb_bot - (oy + c->h + c->margin[CSS_BOTTOM]);
                dy = kn_clampf(want, 0, kn_maxf(max_shift, 0));
            }
            float bt = layout_resolve_len_auto(&s->inset[CSS_BOTTOM], port_h, &a);
            if (!a && dy == 0) {
                float want = (oy + c->h + c->margin[CSS_BOTTOM]) - (port_y + scroller->scroll_y + port_h - bt);
                float max_shift = (oy - c->margin[CSS_TOP]) - cb_top;
                dy = -kn_clampf(want, 0, kn_maxf(max_shift, 0));
            }
            float lf = layout_resolve_len_auto(&s->inset[CSS_LEFT], port_w, &a);
            if (!a) {
                float want = (port_x + scroller->scroll_x + lf) - (ox - c->margin[CSS_LEFT]);
                float max_shift = cb_right - (ox + c->w + c->margin[CSS_RIGHT]);
                dx = kn_clampf(want, 0, kn_maxf(max_shift, 0));
            }
            float rt = layout_resolve_len_auto(&s->inset[CSS_RIGHT], port_w, &a);
            if (!a && dx == 0) {
                float want = (ox + c->w + c->margin[CSS_RIGHT]) - (port_x + scroller->scroll_x + port_w - rt);
                float max_shift = (ox - c->margin[CSS_LEFT]) - cb_left;
                dx = -kn_clampf(want, 0, kn_maxf(max_shift, 0));
            }
            c->sticky_dx = dx; c->sticky_dy = dy;
        }
        update_sticky_rec(scroller, c);
    }
}

void layout_update_sticky(Box *scroller)
{
    update_sticky_rec(scroller, scroller);
}

void layout_set_scroll(LayoutTree *t, Box *scroller, float x, float y)
{
    if (!scroller || !(scroller->flags & BOXF_SCROLL_CONTAINER)) return;
    scroller->scroll_x = x; scroller->scroll_y = y;
    layout_finish_scroll(t, scroller);
    layout_update_sticky(scroller);
    scroller->paint_dirty = 1;
    /* sticky shifts are folded into absolute positions: refresh the subtree */
    for (Box *c = scroller->first_child; c; c = c->next) layout_finalize(t, c);
}

/* ------------------------------------------------------------------------- */
/* local -> absolute                                                          */
/* ------------------------------------------------------------------------- */

static void finalize_rec(LayoutTree *t, Box *b)
{
    float nx, ny;
    if (!b->container) { nx = b->lx; ny = b->ly; }
    else { nx = b->container->x + b->lx + b->sticky_dx; ny = b->container->y + b->ly + b->sticky_dy; }
    if (nx != b->x || ny != b->y) b->paint_dirty = 1;
    b->x = nx; b->y = ny;
    /* lines / items / runs are relative to the owning block container (b);
     * applied_x/y remembers what was added already so this is idempotent */
    if (b->line_count && b->type != BOX_INLINE) {
        float dx = nx - b->applied_x, dy = ny - b->applied_y;
        if (dx != 0 || dy != 0) {
            for (int i = 0; i < b->line_count; i++) {
                LineBox *ln = &b->lines[i];
                ln->x += dx; ln->y += dy; ln->baseline += dy;
                for (int j = 0; j < ln->item_count; j++) {
                    InlineItem *it = &ln->items[j];
                    it->x += dx; it->y += dy; it->baseline += dy;
                    if (it->run) { it->run->x += dx; it->run->y += dy; it->run->ellipsis_x += dx; }
                }
            }
        }
        b->applied_x = nx; b->applied_y = ny;
    }
    for (Box *c = b->first_child; c; c = c->next) finalize_rec(t, c);
    if (b->type == BOX_TEXT && b->run_count) {
        float l = LAYOUT_INF, tp = LAYOUT_INF, r = -LAYOUT_INF, bt = -LAYOUT_INF;
        for (int i = 0; i < b->run_count; i++) {
            TextRun *rn = &b->runs[i];
            l = kn_minf(l, rn->x); r = kn_maxf(r, rn->x + rn->width + (rn->ellipsis ? rn->ellipsis_w : 0));
            tp = kn_minf(tp, rn->y - b->fm.ascent); bt = kn_maxf(bt, rn->y + b->fm.descent);
        }
        b->x = l; b->y = tp; b->w = r - l; b->h = bt - tp;
    }
    if (b->type == BOX_INLINE && b->container) {
        float cx = b->container->x, cy = b->container->y;
        float dx = cx - b->applied_x, dy = cy - b->applied_y;
        for (int i = 0; i < b->frag_count; i++) { b->frags[i].x += dx; b->frags[i].y += dy; }
        b->applied_x = cx; b->applied_y = cy;
        if (b->frag_count) {
            float l = LAYOUT_INF, tp = LAYOUT_INF, r = -LAYOUT_INF, bt = -LAYOUT_INF;
            for (int i = 0; i < b->frag_count; i++) {
                BoxFrag *f = &b->frags[i];
                l = kn_minf(l, f->x); tp = kn_minf(tp, f->y); r = kn_maxf(r, f->x + f->w); bt = kn_maxf(bt, f->y + f->h);
            }
            b->x = l; b->y = tp; b->w = r - l; b->h = bt - tp;
            b->lx = l - cx; b->ly = tp - cy;
        }
    }
}

void layout_finalize(LayoutTree *t, Box *b)
{
    finalize_rec(t, b);
}

/* ------------------------------------------------------------------------- */
/* driver                                                                     */
/* ------------------------------------------------------------------------- */

Box *layout_find_boundary(Box *b)
{
    for (Box *p = b->parent; p; p = p->parent)
        if ((p->flags & BOXF_LAYOUT_BOUNDARY) || !p->parent) return p;
    return b;
}

void layout_invalidate(LayoutTree *t, Box *box, int geometry_only)
{
    if (!box) return;
    if (geometry_only) { layout_update_transform(box); return; }
    /* refresh the style pointer and font (the cascade may have replaced the record) */
    if (box->node && !box->pseudo && !(box->flags & BOXF_ANONYMOUS)) {
        ElemData *ed = layout_elem_data(box->node);
        if (ed && ed->computed) box->style = ed->computed;
    } else if (box->node && box->pseudo) {
        ElemData *ed = layout_elem_data(box->node);
        if (ed) { const CssComputed *ps = box->pseudo == 1 ? ed->before : ed->after; if (ps) box->style = ps; }
    }
    box->dirty |= BOX_DIRTY_SELF | BOX_DIRTY_CHILD;
    layout_restyle_anon_children(box);
    for (Box *p = box->parent; p; p = p->parent) {
        if (p->dirty & BOX_DIRTY_CHILD) { p->dirty |= BOX_DIRTY_CHILD; }
        p->dirty |= BOX_DIRTY_CHILD;
    }
    (void)t;
}

void layout_invalidate_node(LayoutTree *t, lxb_dom_node_t *node, int geometry_only)
{
    Box *b = layout_box_for_node(node);
    if (b) layout_invalidate(t, b, geometry_only);
}

typedef struct BoxVec { Box **data; size_t len, cap; } BoxVec;

static void collect_boundaries(Box *b, Box *enclosing, BoxVec *out)
{
    if (b->dirty & BOX_DIRTY_SELF) {
        for (size_t i = 0; i < out->len; i++) if (out->data[i] == enclosing) return;
        KN_VEC_PUSH(out, enclosing);
        return;
    }
    if (!(b->dirty & BOX_DIRTY_CHILD)) return;
    Box *enc = (b->flags & BOXF_LAYOUT_BOUNDARY) ? b : enclosing;
    for (Box *c = b->first_child; c; c = c->next) collect_boundaries(c, enc, out);
}

static void clear_dirty(Box *b)
{
    b->dirty = 0;
    for (Box *c = b->first_child; c; c = c->next) clear_dirty(c);
}

static int is_ancestor(const Box *anc, const Box *b)
{
    for (const Box *p = b->parent; p; p = p->parent) if (p == anc) return 1;
    return 0;
}

static void full_layout(LayoutTree *t)
{
    Box *root = t->root;
    LayoutInput in;
    memset(&in, 0, sizeof(in));
    in.cb_w = t->viewport_w; in.cb_h = t->viewport_h; in.avail_w = t->viewport_w;
    in.override_w = in.override_h = -1;
    in.definite_h = 1;
    LayoutOutput o;
    root->lx = 0; root->ly = 0;
    layout_box(t, root, &in, &o);
    root->lx = root->margin[CSS_LEFT]; root->ly = root->margin[CSS_TOP];
    layout_finalize(t, root);
    clear_dirty(root);
}

static void relayout_boundary(LayoutTree *t, Box *b)
{
    LayoutInput in;
    memset(&in, 0, sizeof(in));
    Box *cb = b->container;
    float cbw, cbh;
    int is_icb = (cb == t->root) && !(cb->flags & BOXF_POSITIONED) && !(cb->flags & BOXF_HAS_TRANSFORM);
    if (!cb || is_icb) { cbw = t->viewport_w; cbh = t->viewport_h; }
    else { cbw = box_client_w(cb); cbh = box_client_h(cb); }
    in.cb_w = cbw; in.cb_h = cbh; in.avail_w = cbw;
    in.override_w = b->w; in.override_h = b->h; in.definite_h = 1;
    float lx = b->lx, ly = b->ly;
    float m[4]; memcpy(m, b->margin, sizeof(m));
    LayoutOutput o;
    layout_box(t, b, &in, &o);
    b->lx = lx; b->ly = ly;
    memcpy(b->margin, m, sizeof(m));
    layout_finalize(t, b);
    clear_dirty(b);
    /* re-derive overflow up the container chain (scrollable overflow of ancestors) */
    for (Box *p = b->container; p; p = p->container) {
        compute_overflow(p, 1);
        for (int i = 0; i < p->abs_count; i++) layout_overflow_union_child(p, p->abs_children[i]);
        if (p->flags & BOXF_CLIPS) layout_reset_overflow(p);
        if (p->flags & BOXF_SCROLL_CONTAINER) { layout_finish_scroll(t, p); layout_update_sticky(p); }
    }
}

int layout_run(LayoutTree *t, float viewport_w, float viewport_h)
{
    if (!t || !t->root) return 0;
    t->generation++;
    int full = !t->laid_out_once || viewport_w != t->viewport_w || viewport_h != t->viewport_h;
    t->viewport_w = viewport_w; t->viewport_h = viewport_h;
    if (full || (t->root->dirty & BOX_DIRTY_SELF)) {
        full_layout(t);
        t->laid_out_once = 1;
        return 1;
    }
    if (!(t->root->dirty & BOX_DIRTY_CHILD)) return 0;
    BoxVec v = { 0 };
    collect_boundaries(t->root, t->root, &v);
    int n = 0;
    /* drop boundaries nested inside another collected one */
    for (size_t i = 0; i < v.len; i++) {
        Box *b = v.data[i];
        int nested = 0;
        for (size_t j = 0; j < v.len; j++) if (j != i && is_ancestor(v.data[j], b)) { nested = 1; break; }
        if (nested) continue;
        if (b == t->root) { full_layout(t); n++; break; }
        relayout_boundary(t, b);
        n++;
    }
    KN_VEC_FREE(&v);
    clear_dirty(t->root);
    return n;
}
