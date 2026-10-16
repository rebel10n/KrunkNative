/*
 * layout_table.c - `display: table / table-row / table-cell` subset
 * (inventory section 6: .menuTabsNew/.menuTabNew, .serverHeader/.sideTabs/
 * .centerTab; no <table> markup).  colspan (cell_span_at) widens the columns it covers;
 * rowspan (cell_rowspan_at) takes the grid cells below it and grows its rows when it is
 * taller than they are (table_grow_span).  Auto table layout:
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

/* <tr height="N">: the presentational hint maps to the row's CSS height (HTML 15.3.x: a
 * dimension value, so "10px;" reads as 10, like Chrome). 0 = none. */
static float row_attr_height(const Box *r)
{
    lxb_dom_node_t *node = r->node;
    if (!node || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return 0;
    size_t len = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(lxb_dom_interface_element(node), (const lxb_char_t *)"height", 6, &len);
    if (!v || !len) return 0;
    char buf[32];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, v, n); buf[n] = 0;
    char *end;
    double d = strtod(buf, &end);
    if (end == buf || d <= 0 || *end == '%') return 0;
    const float z = r->style && r->style->zoom > 0.0f ? r->style->zoom : 1.0f;
    return (float)d * z;
}

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

/* A cell's integer presentational attribute (colspan / rowspan); dflt when missing or junk. */
static long cell_attr_int(const Box *c, const char *name, size_t nlen, long dflt)
{
    lxb_dom_node_t *node = c->node;
    if (!node || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return dflt;
    size_t len = 0;
    const lxb_char_t *v = lxb_dom_element_get_attribute(lxb_dom_interface_element(node), (const lxb_char_t *)name, nlen, &len);
    if (!v || !len) return dflt;
    char buf[32];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, v, n); buf[n] = 0;
    char *end;
    long d = strtol(buf, &end, 10);
    return end == buf ? dflt : d;
}

/* colspan (HTML 4.4.6): 0 or junk reads as 1; clamped to the columns left from column ci
 * (ci < TBL_MAX_COLS). */
static int cell_span_at(const Box *c, int ci)
{
    long d = cell_attr_int(c, "colspan", 7, 1);
    if (d < 1) d = 1;
    return d < TBL_MAX_COLS - ci ? (int)d : TBL_MAX_COLS - ci;
}

/* rowspan (HTML 4.4.6): 0 spans to the table's last row; junk reads as 1; clamped to the rows
 * left from the cell's row (rows_left >= 1). */
static int cell_rowspan_at(const Box *c, int rows_left)
{
    long d = cell_attr_int(c, "rowspan", 7, 1);
    if (d == 0) d = rows_left;
    else if (d < 1) d = 1;
    return d < rows_left ? (int)d : rows_left;
}

/* The cells of one row placed on the grid: each cell takes the first column no rowspan from an
 * earlier row holds (busy[col] counts the rows, this one included, that still hold it). */
typedef struct RowCells {
    Box *cell[TBL_MAX_COLS];
    int col[TBL_MAX_COLS], span[TBL_MAX_COLS], rspan[TBL_MAX_COLS];
    int n;
} RowCells;

static void table_row_cells(Box *r, int rows_left, int *busy, RowCells *rc)
{
    rc->n = 0;
    int col = 0;
    for (Box *c = r->first_child; c; c = c->next) {
        if (c->type != BOX_TABLE_CELL) continue;
        while (col < TBL_MAX_COLS && busy[col] > 0) col++;
        if (col >= TBL_MAX_COLS) break;
        const int span = cell_span_at(c, col), rs = cell_rowspan_at(c, rows_left);
        const int k = rc->n++;
        rc->cell[k] = c; rc->col[k] = col; rc->span[k] = span; rc->rspan[k] = rs;
        for (int i = 0; i < span; i++) busy[col + i] = rs;
        col += span;
    }
}

/* Leaving a row: every rowspan that reached this row has one row fewer to hold its columns. */
static void table_row_done(int *busy)
{
    for (int i = 0; i < TBL_MAX_COLS; i++) if (busy[i] > 0) busy[i]--;
}

static int table_row_count(const Box *b)
{
    int n = 0;
    for (const Box *r = b->first_child; r; r = r->next) if (r->type == BOX_TABLE_ROW) n++;
    return n;
}

static Box *next_table_row(Box *r)
{
    for (Box *n = r->next; n; n = n->next) if (n->type == BOX_TABLE_ROW) return n;
    return NULL;
}

/* The border-box width of the columns [ci, ci + span) and the spacing between them. */
static float span_width(const ColInfo *cols, int ci, int span, float hs)
{
    float w = hs * (float)(span - 1);
    for (int k = 0; k < span; k++) w += cols[ci + k].w;
    return w;
}

/* A spanning cell's min/max must fit in its columns: any shortfall is shared by them in
 * proportion to their max-content widths (equally when those are 0). Spacing between the
 * columns counts toward the cell's width, so it is taken off first. */
static void widen_span(ColInfo *cols, int span, float need_min, float need_max)
{
    float sum_min = 0, sum_max = 0;
    for (int k = 0; k < span; k++) { sum_min += cols[k].min; sum_max += cols[k].max; }
    if (need_min > sum_min) {
        const float extra = need_min - sum_min;
        for (int k = 0; k < span; k++) cols[k].min += sum_max > 0 ? extra * cols[k].max / sum_max : extra / (float)span;
        for (int k = 0; k < span; k++) cols[k].max = kn_maxf(cols[k].max, cols[k].min);
    }
    sum_max = 0;
    for (int k = 0; k < span; k++) sum_max += cols[k].max;
    if (need_max > sum_max) {
        const float extra = need_max - sum_max;
        for (int k = 0; k < span; k++) cols[k].max += sum_max > 0 ? extra * cols[k].max / sum_max : extra / (float)span;
    }
}

/* One cell's min/max widths: a single-column cell sets its column; a spanning cell widens
 * the columns it covers (its px width counts as its max, like a single column's). Percentage
 * widths on spanning cells are ignored (they act as auto). */
static void table_cell_widths(LayoutTree *t, Box *c, int ci, int span, float hs, ColInfo *cols)
{
    float mn = layout_intrinsic_width(t, c, 0);
    float mx = layout_intrinsic_width(t, c, 1);
    const CssLength *w = &c->style->width;
    if (span > 1) {
        if (w->kind == CSS_LEN_VALUE && w->pct == 0) mx = kn_maxf(mn, layout_box_sizing_w(c, w->px));
        const float spacing = hs * (float)(span - 1);
        widen_span(cols + ci, span, mn - spacing, mx - spacing);
        return;
    }
    ColInfo *col = &cols[ci];
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

/* The column count (the widest row's columns, rowspans included) and the columns' min/max. */
static int table_columns(LayoutTree *t, Box *b, float hs, ColInfo *cols)
{
    int nc = 0;
    const int nrows = table_row_count(b);
    for (int i = 0; i < TBL_MAX_COLS; i++) { cols[i].min = cols[i].max = 0; cols[i].fixed = cols[i].pct = -1; }
    /* the single-column cells first (pass 0), then the spanning cells (pass 1) */
    for (int pass = 0; pass < 2; pass++) {
        int busy[TBL_MAX_COLS];
        memset(busy, 0, sizeof(busy));
        int ri = 0;
        for (Box *r = b->first_child; r; r = r->next) {
            if (r->type != BOX_TABLE_ROW) continue;
            RowCells rc;
            table_row_cells(r, nrows - ri, busy, &rc);
            for (int k = 0; k < rc.n; k++) {
                if ((rc.span[k] > 1) == (pass == 1)) table_cell_widths(t, rc.cell[k], rc.col[k], rc.span[k], hs, cols);
                if (rc.col[k] + rc.span[k] > nc) nc = rc.col[k] + rc.span[k];
            }
            table_row_done(busy);
            ri++;
        }
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

static void distribute_fixed(LayoutTree *t, Box *b, ColInfo *cols, int nc, float avail, float hs)
{
    (void)t;
    Box *row = NULL;
    for (Box *r = b->first_child; r && !row; r = r->next) if (r->type == BOX_TABLE_ROW) row = r;
    float used = 0;
    int autos = 0, ci = 0;
    for (int i = 0; i < nc; i++) cols[i].w = -1;
    for (Box *c = row ? row->first_child : NULL; c && ci < nc; c = c->next) {
        if (c->type != BOX_TABLE_CELL) continue;
        const int span = cell_span_at(c, ci);
        const CssLength *w = &c->style->width;
        float cw = -1;
        if (w->kind == CSS_LEN_VALUE && w->pct > 0) cw = w->pct * 0.01f * avail;
        else if (w->kind == CSS_LEN_VALUE) cw = layout_box_sizing_w(c, w->px);
        /* a spanning cell's width (less the spacing) is split evenly over its columns */
        if (cw >= 0) for (int k = 0; k < span; k++) cols[ci + k].w = kn_maxf(0, cw - hs * (float)(span - 1)) / (float)span;
        ci += span;
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

/* A cell's border-box height its specified `height` asks for (after layout at its column width). */
static float cell_need_h(const Box *c)
{
    float ch = c->h;
    if (c->style->height.kind == CSS_LEN_VALUE && c->style->height.pct == 0)
        ch = kn_maxf(ch, layout_box_sizing_h(c, c->style->height.px));
    return ch;
}

/* The height of the rows first.. first + rspan - 1 with the spacing between them. */
static float table_span_h(Box *first, int rspan, float first_h, float vs)
{
    float h = first_h;
    Box *r = first;
    for (int k = 1; k < rspan && r; k++) {
        r = next_table_row(r);
        if (r) h += vs + r->h;
    }
    return h;
}

/* A rowspan cell taller than its rows gets the difference, shared by those rows in proportion
 * to their heights (equally when they are all 0), like the column widths of a spanning cell. */
static void table_grow_span(Box *first, const Box *c, int rspan, float vs)
{
    const float need = cell_need_h(c);
    const float have = table_span_h(first, rspan, first->h, vs);
    if (need <= have) return;
    const float extra = need - have;
    float sum = 0;
    int n = 0;
    for (Box *r = first; r && n < rspan; r = next_table_row(r), n++) sum += r->h;
    Box *r = first;
    for (int k = 0; k < n && r; k++, r = next_table_row(r))
        r->h += sum > 0 ? extra * r->h / sum : extra / (float)n;
}

void layout_table_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h)
{
    (void)in;
    ColInfo cols[TBL_MAX_COLS];
    float hs, vs;
    table_spacing(b, &hs, &vs);
    int nc = table_columns(t, b, hs, cols);
    float cx = box_cl(b), cy = box_ct(b), cw = box_cw(b);
    float avail = kn_maxf(0, cw - hs * (float)(nc + 1));
    if (table_fixed(b)) distribute_fixed(t, b, cols, nc, avail, hs);
    else distribute(cols, nc, avail);
    float colx[TBL_MAX_COLS];
    float x = hs;
    for (int i = 0; i < nc; i++) { colx[i] = x; x += cols[i].w + hs; }

    for (Box *c = b->first_child; c; c = c->next)
        if (c->flags & BOXF_OUT_OF_FLOW) { c->static_x = cx; c->static_y = cy; c->static_fx = c->static_fy = 0; }

    const int nrows = table_row_count(b);
    int busy[TBL_MAX_COLS];
    int ri = 0;

    /* 1. each row's own height: its cells that do not span rows, laid out at their column widths
     * (rows[].h holds the result until step 3) */
    memset(busy, 0, sizeof(busy));
    for (Box *r = b->first_child; r; r = r->next) {
        if (r->type != BOX_TABLE_ROW) continue;
        layout_resolve_edges(t, r, cw);
        float row_h = 0;
        if (r->style->height.kind == CSS_LEN_VALUE && r->style->height.pct == 0 && !(r->flags & BOXF_ANONYMOUS)) row_h = r->style->height.px;
        else if (r->style->height.kind == CSS_LEN_AUTO && !(r->flags & BOXF_ANONYMOUS)) row_h = row_attr_height(r);
        RowCells rc;
        table_row_cells(r, nrows - ri, busy, &rc);
        for (int k = 0; k < rc.n; k++) {
            layout_cell(t, rc.cell[k], span_width(cols, rc.col[k], rc.span[k], hs), -1, cw);
            if (rc.rspan[k] == 1) row_h = kn_maxf(row_h, cell_need_h(rc.cell[k]));
        }
        r->h = row_h;
        table_row_done(busy);
        ri++;
    }

    /* 2. rowspan cells taller than the rows they cover make those rows taller */
    memset(busy, 0, sizeof(busy));
    ri = 0;
    for (Box *r = b->first_child; r; r = r->next) {
        if (r->type != BOX_TABLE_ROW) continue;
        RowCells rc;
        table_row_cells(r, nrows - ri, busy, &rc);
        for (int k = 0; k < rc.n; k++)
            if (rc.rspan[k] > 1) table_grow_span(r, rc.cell[k], rc.rspan[k], vs);
        table_row_done(busy);
        ri++;
    }

    /* 3. place the rows and their cells (a rowspan cell covers its rows) */
    float y = vs;
    int any_row = 0;
    b->first_baseline = -1;
    memset(busy, 0, sizeof(busy));
    ri = 0;
    for (Box *r = b->first_child; r; r = r->next) {
        if (r->type != BOX_TABLE_ROW) continue;
        any_row = 1;
        const float row_h = r->h;
        RowCells rc;
        table_row_cells(r, nrows - ri, busy, &rc);
        for (int k = 0; k < rc.n; k++) {
            Box *c = rc.cell[k];
            const float sw = span_width(cols, rc.col[k], rc.span[k], hs);
            const float cell_h = table_span_h(r, rc.rspan[k], row_h, vs);
            layout_cell(t, c, sw, cell_h, cw);
            const float natural = cell_content_h(c);
            float extra = cell_h - natural;
            if (extra > 0) {
                if (c->vertical_align == CSS_VERTICAL_ALIGN_MIDDLE) shift_contents(c, extra * 0.5f);
                else if (c->vertical_align == CSS_VERTICAL_ALIGN_BOTTOM) shift_contents(c, extra);
            }
            c->lx = colx[rc.col[k]];
            c->ly = 0;
            layout_relative_offset(c, sw, LAYOUT_INDEFINITE);
            if (b->first_baseline < 0 && c->first_baseline >= 0) b->first_baseline = cy + y + c->first_baseline;
        }
        r->lx = cx; r->ly = cy + y;
        r->w = cw; r->h = row_h;
        r->margin[0] = r->margin[1] = r->margin[2] = r->margin[3] = 0;
        layout_reset_overflow(r);
        for (Box *c = r->first_child; c; c = c->next) if (c->type == BOX_TABLE_CELL) layout_overflow_union_child(r, c);
        r->dirty = 0;
        y += row_h + vs;
        table_row_done(busy);
        ri++;
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
    int nc = table_columns(t, b, hs, cols);
    float mn = hs * (float)(nc + 1), mx = mn;
    for (int i = 0; i < nc; i++) {
        mn += cols[i].min;
        mx += cols[i].fixed >= 0 ? kn_maxf(cols[i].fixed, cols[i].min) : cols[i].max;
    }
    *min_w = mn;
    *max_w = kn_maxf(mx, mn);
}
