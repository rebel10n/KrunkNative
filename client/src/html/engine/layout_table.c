/*
 * layout_table.c - `display: table / table-row / table-cell` subset
 * (inventory section 6: .menuTabsNew/.menuTabNew, .serverHeader/.sideTabs/
 * .centerTab; no <table> markup).  Span-1 cells only, auto table layout:
 * column min/max from cell min-/max-content, percentage and px cell widths,
 * extra space distributed to auto columns in proportion to their max-content
 * widths (Chrome's AutoTableLayout for these cases), cells of a row share the
 * row height, cell vertical-align top/middle/bottom (baseline ~ top),
 * border-collapse (spacing 0, cell borders halved) / border-spacing.
 * table-layout: fixed (with a non-auto table width): column widths come from the
 * first row's px / percentage cell widths, the rest is shared equally by the other
 * columns (Chrome's FixedTableLayout; social.css .profileTabs, #mapTabsP).
 */
#include "layout_internal.h"

#define TBL_MAX_COLS 64

typedef struct ColInfo {
    float min, max;
    float fixed;      /* px width (border box), <0 none */
    float pct;        /* percentage, <0 none */
    float w;
} ColInfo;

static void table_spacing(const Box *b, float *hs, float *vs)
{
    if (b->style->border_collapse == CSS_BORDER_COLLAPSE_COLLAPSE) { *hs = *vs = 0; return; }
    *hs = b->style->border_spacing_x.kind == CSS_LEN_VALUE ? b->style->border_spacing_x.px : 0;
    *vs = b->style->border_spacing_y.kind == CSS_LEN_VALUE ? b->style->border_spacing_y.px : 0;
}

static int table_columns(LayoutTree *t, Box *b, ColInfo *cols)
{
    int nc = 0;
    for (int i = 0; i < TBL_MAX_COLS; i++) { cols[i].min = cols[i].max = 0; cols[i].fixed = cols[i].pct = -1; }
    for (Box *r = b->first_child; r; r = r->next) {
        if (r->type != BOX_TABLE_ROW) continue;
        int ci = 0;
        for (Box *c = r->first_child; c && ci < TBL_MAX_COLS; c = c->next) {
            if (c->type != BOX_TABLE_CELL) continue;
            ColInfo *col = &cols[ci++];
            float mn = layout_intrinsic_width(t, c, 0);
            float mx = layout_intrinsic_width(t, c, 1);
            const CssLength *w = &c->style->width;
            if (w->kind == CSS_LEN_VALUE && w->pct == 0) {
                float f = kn_maxf(mn, layout_box_sizing_w(c, w->px));
                col->fixed = kn_maxf(col->fixed, f);
                mx = kn_maxf(mn, f);
            } else if (w->kind == CSS_LEN_VALUE && w->pct > 0) {
                col->pct = kn_maxf(col->pct, w->pct);
            }
            col->min = kn_maxf(col->min, mn);
            col->max = kn_maxf(col->max, mx);
        }
        if (ci > nc) nc = ci;
    }
    return nc;
}

static void distribute(ColInfo *cols, int nc, float avail)
{
    float sum_min = 0, sum_target = 0;
    float target[TBL_MAX_COLS];
    for (int i = 0; i < nc; i++) {
        if (cols[i].pct >= 0) target[i] = kn_maxf(cols[i].min, cols[i].pct * 0.01f * avail);
        else if (cols[i].fixed >= 0) target[i] = kn_maxf(cols[i].min, cols[i].fixed);
        else target[i] = cols[i].max;
        sum_min += cols[i].min; sum_target += target[i];
    }
    if (sum_target <= avail) {
        float extra = avail - sum_target;
        float auto_max = 0, fixed_sum = 0; int autos = 0, fixeds = 0;
        for (int i = 0; i < nc; i++) {
            cols[i].w = target[i];
            if (cols[i].pct < 0 && cols[i].fixed < 0) { auto_max += cols[i].max; autos++; }
            else if (cols[i].pct < 0) { fixed_sum += target[i]; fixeds++; }
        }
        if (autos) {
            for (int i = 0; i < nc; i++) {
                if (cols[i].pct >= 0 || cols[i].fixed >= 0) continue;
                cols[i].w += auto_max > 0 ? extra * cols[i].max / auto_max : extra / (float)autos;
            }
        } else if (fixeds) {
            for (int i = 0; i < nc; i++) {
                if (cols[i].pct >= 0) continue;
                cols[i].w += fixed_sum > 0 ? extra * target[i] / fixed_sum : extra / (float)fixeds;
            }
        } else if (nc) {
            for (int i = 0; i < nc; i++) cols[i].w += extra / (float)nc;
        }
    } else if (sum_min >= avail) {
        for (int i = 0; i < nc; i++) cols[i].w = cols[i].min;
    } else {
        float k = (avail - sum_min) / (sum_target - sum_min);
        for (int i = 0; i < nc; i++) cols[i].w = cols[i].min + (target[i] - cols[i].min) * k;
    }
}

/* table-layout: fixed: first-row widths only, content ignored. */
static int table_fixed(const Box *b)
{
    return b->style->table_layout == CSS_TABLE_LAYOUT_FIXED && b->style->width.kind == CSS_LEN_VALUE;
}

static void distribute_fixed(LayoutTree *t, Box *b, ColInfo *cols, int nc, float avail)
{
    (void)t;
    Box *row = NULL;
    for (Box *r = b->first_child; r && !row; r = r->next) if (r->type == BOX_TABLE_ROW) row = r;
    float used = 0;
    int autos = 0, ci = 0;
    for (int i = 0; i < nc; i++) cols[i].w = -1;
    for (Box *c = row ? row->first_child : NULL; c && ci < nc; c = c->next) {
        if (c->type != BOX_TABLE_CELL) continue;
        const CssLength *w = &c->style->width;
        if (w->kind == CSS_LEN_VALUE && w->pct > 0) cols[ci].w = w->pct * 0.01f * avail;
        else if (w->kind == CSS_LEN_VALUE) cols[ci].w = layout_box_sizing_w(c, w->px);
        ci++;
    }
    for (int i = 0; i < nc; i++) { if (cols[i].w < 0) autos++; else used += cols[i].w; }
    float share = autos ? kn_maxf(0, avail - used) / (float)autos : 0;
    for (int i = 0; i < nc; i++) if (cols[i].w < 0) cols[i].w = share;
    if (!autos && used > 0 && used < avail)
        for (int i = 0; i < nc; i++) cols[i].w += (avail - used) * cols[i].w / used;
}

/* Shift a cell's in-flow content down by dy (local coordinates, pre-finalize). */
static void shift_inline_frags(Box *container, Box *b, float dy)
{
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->type == BOX_INLINE && c->container == container) {
            for (int i = 0; i < c->frag_count; i++) c->frags[i].y += dy;
            shift_inline_frags(container, c, dy);
        }
    }
}

static void shift_contents(Box *cell, float dy)
{
    if (dy == 0) return;
    for (Box *c = cell->first_child; c; c = c->next) {
        if (c->flags & BOXF_OUT_OF_FLOW) continue;
        if (c->type == BOX_TEXT || c->type == BOX_INLINE || c->type == BOX_LINEBREAK) continue;
        if ((c->flags & BOXF_INLINE_LEVEL) && (cell->flags & BOXF_HAS_LINES)) { c->ly += dy; continue; }
        c->ly += dy;
    }
    for (int i = 0; i < cell->line_count; i++) {
        LineBox *ln = &cell->lines[i];
        ln->y += dy; ln->baseline += dy;
        for (int j = 0; j < ln->item_count; j++) {
            InlineItem *it = &ln->items[j];
            it->y += dy; it->baseline += dy;
            if (it->run) it->run->y += dy;
        }
    }
    shift_inline_frags(cell, cell, dy);
    if (cell->first_baseline >= 0) cell->first_baseline += dy;
    if (cell->baseline_offset >= 0) cell->baseline_offset += dy;
}

/* The cell's border-box height its in-flow content needs (line boxes and in-flow children,
 * plus the end padding and border): vertical-align distributes row_h minus this, not
 * minus c->h, which a specified `height` (Krunker's `td { height: 50px }`) already grew to
 * the row height. */
static float cell_content_h(const Box *c)
{
    float bottom = box_ct(c);
    for (int i = 0; i < c->line_count; i++) bottom = kn_maxf(bottom, c->lines[i].y + c->lines[i].h);
    for (const Box *k = c->first_child; k; k = k->next) {
        if (k->flags & BOXF_OUT_OF_FLOW) continue;
        if (k->type == BOX_TEXT || k->type == BOX_INLINE || k->type == BOX_LINEBREAK) continue;
        if ((k->flags & BOXF_INLINE_LEVEL) && (c->flags & BOXF_HAS_LINES)) continue; /* in the line boxes */
        bottom = kn_maxf(bottom, k->ly + k->h + k->margin[CSS_BOTTOM]);
    }
    return bottom + c->padding[CSS_BOTTOM] + c->border[CSS_BOTTOM];
}

static void layout_cell(LayoutTree *t, Box *c, float w, float h, float cb_w)
{
    LayoutInput in;
    memset(&in, 0, sizeof(in));
    in.cb_w = cb_w; in.cb_h = LAYOUT_INDEFINITE; in.avail_w = w;
    in.override_w = w; in.override_h = h; in.definite_h = 0;
    LayoutOutput o;
    layout_box(t, c, &in, &o);
}

void layout_table_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h)
{
    (void)in;
    ColInfo cols[TBL_MAX_COLS];
    float hs, vs;
    table_spacing(b, &hs, &vs);
    int nc = table_columns(t, b, cols);
    float cx = box_cl(b), cy = box_ct(b), cw = box_cw(b);
    float avail = kn_maxf(0, cw - hs * (float)(nc + 1));
    if (table_fixed(b)) distribute_fixed(t, b, cols, nc, avail);
    else distribute(cols, nc, avail);
    float colx[TBL_MAX_COLS];
    float x = hs;
    for (int i = 0; i < nc; i++) { colx[i] = x; x += cols[i].w + hs; }

    for (Box *c = b->first_child; c; c = c->next)
        if (c->flags & BOXF_OUT_OF_FLOW) { c->static_x = cx; c->static_y = cy; c->static_fx = c->static_fy = 0; }

    float y = vs;
    int any_row = 0;
    b->first_baseline = -1;
    for (Box *r = b->first_child; r; r = r->next) {
        if (r->type != BOX_TABLE_ROW) continue;
        any_row = 1;
        layout_resolve_edges(t, r, cw);
        float row_h = 0;
        if (r->style->height.kind == CSS_LEN_VALUE && r->style->height.pct == 0 && !(r->flags & BOXF_ANONYMOUS)) row_h = r->style->height.px;
        int ci = 0;
        for (Box *c = r->first_child; c && ci < TBL_MAX_COLS; c = c->next) {
            if (c->type != BOX_TABLE_CELL) continue;
            layout_cell(t, c, cols[ci].w, -1, cw);
            float ch = c->h;
            if (c->style->height.kind == CSS_LEN_VALUE && c->style->height.pct == 0)
                ch = kn_maxf(ch, layout_box_sizing_h(c, c->style->height.px));
            row_h = kn_maxf(row_h, ch);
            ci++;
        }
        ci = 0;
        for (Box *c = r->first_child; c && ci < TBL_MAX_COLS; c = c->next) {
            if (c->type != BOX_TABLE_CELL) continue;
            layout_cell(t, c, cols[ci].w, row_h, cw);
            const float natural = cell_content_h(c);
            float extra = row_h - natural;
            if (extra > 0) {
                if (c->vertical_align == CSS_VERTICAL_ALIGN_MIDDLE) shift_contents(c, extra * 0.5f);
                else if (c->vertical_align == CSS_VERTICAL_ALIGN_BOTTOM) shift_contents(c, extra);
            }
            c->lx = colx[ci];
            c->ly = 0;
            layout_relative_offset(c, cols[ci].w, LAYOUT_INDEFINITE);
            if (b->first_baseline < 0 && c->first_baseline >= 0) b->first_baseline = cy + y + c->first_baseline;
            ci++;
        }
        r->lx = cx; r->ly = cy + y;
        r->w = cw; r->h = row_h;
        r->margin[0] = r->margin[1] = r->margin[2] = r->margin[3] = 0;
        layout_reset_overflow(r);
        for (Box *c = r->first_child; c; c = c->next) if (c->type == BOX_TABLE_CELL) layout_overflow_union_child(r, c);
        r->dirty = 0;
        y += row_h + vs;
    }
    if (!any_row) y = 0;
    b->baseline_offset = b->first_baseline;
    *content_h = y;
}

void layout_table_intrinsic(LayoutTree *t, Box *b, float *min_w, float *max_w)
{
    ColInfo cols[TBL_MAX_COLS];
    float hs, vs;
    table_spacing(b, &hs, &vs);
    int nc = table_columns(t, b, cols);
    float mn = hs * (float)(nc + 1), mx = mn;
    for (int i = 0; i < nc; i++) {
        mn += cols[i].min;
        mx += cols[i].fixed >= 0 ? kn_maxf(cols[i].fixed, cols[i].min) : cols[i].max;
    }
    *min_w = mn;
    *max_w = kn_maxf(mx, mn);
}
