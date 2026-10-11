/*
 * layout_grid.c - grid subset (inventory section 6): grid-template-columns /
 * grid-template-rows with <length-percentage> / calc / auto / min-content /
 * max-content / <flex> / fit-content() / minmax() / repeat(<integer>) tracks
 * (no areas, line names or auto-fill), row-gap / column-gap, line-based
 * placement (grid-row/column-start/end, grid-row, grid-column, grid-area with
 * <integer>, negative and `span <integer>` lines), implicit auto tracks,
 * row-major sparse auto-placement (CSS Grid 1 section 8.5), and
 * justify-self / align-self (+ justify-items / align-items) with auto
 * margins.  `normal` stretches every item in both axes except replaced
 * elements with a natural aspect ratio (img, canvas, video, input[type=color]:
 * start-aligned like Chrome; other form controls stretch).
 *
 * Track sizing follows CSS Grid 1 section 12 in both axes:
 *   1. base = fixed min breadth, or the items' min-content (auto/min-content
 *      min) / max-content (max-content min) contributions; growth limit =
 *      fixed max breadth, the max-content (auto/max-content) or min-content
 *      contributions, fit-content(L) = min(L, max-content); flexible tracks
 *      keep growth = base.  Items spanning several tracks are added after the
 *      single-span ones in increasing span order, their extra space shared
 *      equally by the spanned intrinsic tracks (flexible tracks: by fr).
 *      Percentages against an indefinite size behave as auto (the
 *      fit-content() limit as none).
 *   2. maximize: free space grows the non-flexible tracks equally up to
 *      their growth limits;
 *   3. expand flexible tracks (find the size of an fr, tracks whose base is
 *      larger than their share are treated as inflexible); with an
 *      indefinite size the flex fraction comes from the max-content
 *      contributions;
 *   4. stretch auto max tracks with what is left (justify/align-content:
 *      normal).
 * Columns are sized against the content width; row contributions are the
 * items' margin-box heights laid out in their column areas, rows are sized
 * against the content height when it is definite (fr rows fill it).
 * Intrinsic widths (min/max-content of the grid) use the bases (min) or the
 * growth limits with the flex fraction of the max-content contributions (max).
 */
#include "layout_internal.h"

#define GRID_MAX_COLS 32         /* explicit + implicit columns              */
#define GRID_MAX_ROWS 4096       /* explicit + implicit rows                 */
#define GRID_INF 1e30f

static float gap_px(const CssLength *g, float base)
{
    if (g->kind != CSS_LEN_VALUE) return 0;
    if (g->pct != 0 && base < 0) return 0;   /* % gap against an indefinite size */
    return layout_resolve_len(g, base < 0 ? 0 : base, 0);
}

typedef enum { BR_FIXED, BR_AUTO, BR_MIN, BR_MAX, BR_FIT, BR_FLEX } Breadth;

typedef struct {
    uint8_t mn, mx;        /* Breadth of the min / max sizing functions */
    float mn_px, mx_px;    /* BR_FIXED sizes; mx_px is the limit for BR_FIT */
    float fr;              /* BR_FLEX factor */
} TrackFn;

static Breadth breadth_of(const CssLength *l, float avail, float *px)
{
    *px = 0;
    switch (l->kind) {
    case CSS_LEN_VALUE:
        if (l->pct != 0 && avail < 0) return BR_AUTO;   /* indefinite percentage: auto */
        *px = kn_maxf(0, layout_resolve_len(l, avail, 0));
        return BR_FIXED;
    case CSS_LEN_MIN_CONTENT: return BR_MIN;
    case CSS_LEN_MAX_CONTENT: return BR_MAX;
    default: return BR_AUTO;
    }
}

/* Sizing functions of track i of an axis with `count` explicit tracks;
 * implicit tracks (i >= count) are auto (grid-auto-rows/columns unsupported). */
static TrackFn track_fn(const CssTrack *tracks, int count, int i, float avail)
{
    TrackFn f;
    memset(&f, 0, sizeof(f));
    if (i >= count) { f.mn = f.mx = BR_AUTO; return f; }
    const CssTrack *tr = &tracks[i];
    switch (tr->kind) {
    case CSS_TRACK_FIT_CONTENT: {
        f.mn = BR_AUTO;
        f.mx = BR_FIT;
        float lim;
        f.mx_px = breadth_of(&tr->size, avail, &lim) == BR_FIXED ? lim : GRID_INF;
        break;
    }
    case CSS_TRACK_FR:
        f.mn = BR_AUTO; f.mx = BR_FLEX; f.fr = tr->fr;
        break;
    case CSS_TRACK_MINMAX:
    case CSS_TRACK_MINMAX_FR:
        f.mn = (uint8_t)breadth_of(&tr->size, avail, &f.mn_px);
        if (tr->kind == CSS_TRACK_MINMAX_FR) { f.mx = BR_FLEX; f.fr = tr->fr; }
        else f.mx = (uint8_t)breadth_of(&tr->max, avail, &f.mx_px);
        if (f.mn == BR_FIXED && f.mx == BR_FIXED && f.mx_px < f.mn_px) f.mx_px = f.mn_px;
        break;
    default:
        f.mn = f.mx = (uint8_t)breadth_of(&tr->size, avail, &f.mn_px);
        f.mx_px = f.mn_px;
        break;
    }
    return f;
}

/* ------------------------------------------------------------------------- */
/* placement (CSS Grid 1 section 8)                                            */
/* ------------------------------------------------------------------------- */

typedef struct GridItem {
    Box *box;
    int r0, r1, c0, c1;      /* 0-based lines, end exclusive                 */
    float min_c, max_c;      /* contributions in the axis being sized        */
    uint8_t laid_out;        /* row measuring laid it out in its column area */
} GridItem;

typedef struct Grid {
    GridItem *items;
    int n;
    int ncols, nrows;
    uint32_t *occ;           /* occupied cells: one column bit mask per row  */
    int occ_rows;
} Grid;

/* 0-based line index of a <grid-line> integer against `explicit_n` tracks.
 * Lines before the explicit grid would add implicit tracks at the start
 * (unsupported): they clamp to line 0. */
static int line_index(int line, int explicit_n)
{
    int idx = line > 0 ? line - 1 : explicit_n + 1 + line;
    return idx < 0 ? 0 : idx;
}

/* Resolve one axis of an item: *p0 = start line (-1: auto), *span. */
static void resolve_axis(const CssGridLine *s, const CssGridLine *e, int explicit_n, int max_span, int *p0, int *span)
{
    int sdef = s->line != 0 && !s->span, edef = e->line != 0 && !e->span;
    int a = -1, n = 1;
    if (sdef && edef) {
        a = line_index(s->line, explicit_n);
        int b = line_index(e->line, explicit_n);
        if (b < a) { int x = a; a = b; b = x; }
        n = b > a ? b - a : 1;                   /* equal lines: span 1 */
    } else if (sdef) {
        a = line_index(s->line, explicit_n);
        n = e->span ? e->line : 1;
    } else if (edef) {
        int b = line_index(e->line, explicit_n);
        n = s->span ? s->line : 1;
        a = b - n;
        if (a < 0) { n = b > 0 ? b : 1; a = 0; }
    } else {
        n = s->span ? s->line : e->span ? e->line : 1;
    }
    if (n < 1) n = 1;
    if (n > max_span) n = max_span;
    *p0 = a;
    *span = n;
}

static int occ_ensure(Grid *g, int rows)
{
    if (rows <= g->occ_rows) return 1;
    int cap = g->occ_rows ? g->occ_rows : 16;
    while (cap < rows) cap *= 2;
    uint32_t *o = realloc(g->occ, (size_t)cap * sizeof(uint32_t));
    if (!o) return 0;
    memset(o + g->occ_rows, 0, (size_t)(cap - g->occ_rows) * sizeof(uint32_t));
    g->occ = o;
    g->occ_rows = cap;
    return 1;
}

static uint32_t col_mask(int c0, int c1) { return (c1 >= 32 ? 0xffffffffu : ((1u << c1) - 1u)) & ~((1u << c0) - 1u); }

static int occ_overlaps(Grid *g, int r0, int r1, int c0, int c1)
{
    uint32_t m = col_mask(c0, c1);
    for (int r = r0; r < r1 && r < g->occ_rows; r++) if (g->occ[r] & m) return 1;
    return 0;
}

static void occ_mark(Grid *g, GridItem *it)
{
    if (!occ_ensure(g, it->r1)) return;
    uint32_t m = col_mask(it->c0, it->c1);
    for (int r = it->r0; r < it->r1; r++) g->occ[r] |= m;
}

/* Place the in-flow children: definite items, then items locked to a row,
 * then the auto-placement cursor (sparse, row-major). */
static int grid_place(Box *b, Grid *g)
{
    const CssComputed *s = b->style;
    memset(g, 0, sizeof(*g));
    int n = 0;
    for (Box *c = b->first_child; c; c = c->next) if (!(c->flags & BOXF_OUT_OF_FLOW)) n++;
    if (!n) return 1;
    g->items = calloc((size_t)n, sizeof(GridItem));
    if (!g->items) return 0;
    int ner = s->grid_row_count, nec = s->grid_column_count;
    int *rspan = calloc((size_t)n, sizeof(int)), *cspan = calloc((size_t)n, sizeof(int));
    if (!rspan || !cspan) { free(rspan); free(cspan); return 0; }
    int ncols = nec > 0 ? nec : 1;
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->flags & BOXF_OUT_OF_FLOW) continue;
        GridItem *it = &g->items[g->n];
        const CssGridLine *gl = c->style->grid_lines;
        it->box = c;
        resolve_axis(&gl[CSS_GRID_ROW_START], &gl[CSS_GRID_ROW_END], ner, GRID_MAX_ROWS / 2, &it->r0, &rspan[g->n]);
        resolve_axis(&gl[CSS_GRID_COLUMN_START], &gl[CSS_GRID_COLUMN_END], nec, GRID_MAX_COLS, &it->c0, &cspan[g->n]);
        if (it->r0 > GRID_MAX_ROWS / 2) it->r0 = GRID_MAX_ROWS / 2;
        if (it->c0 >= 0 && it->c0 + cspan[g->n] > GRID_MAX_COLS) it->c0 = GRID_MAX_COLS - cspan[g->n];
        int need = it->c0 >= 0 ? it->c0 + cspan[g->n] : cspan[g->n];
        if (need > ncols) ncols = need;
        g->n++;
    }
    /* 1. both axes definite */
    for (int i = 0; i < g->n; i++) {
        GridItem *it = &g->items[i];
        if (it->r0 < 0 || it->c0 < 0) continue;
        it->r1 = it->r0 + rspan[i]; it->c1 = it->c0 + cspan[i];
        occ_mark(g, it);
    }
    /* 2. definite row, auto column: earliest free column past the items this
     * step already put in that row */
    int row_cursor[64];
    memset(row_cursor, 0, sizeof(row_cursor));
    for (int i = 0; i < g->n; i++) {
        GridItem *it = &g->items[i];
        if (it->r0 < 0 || it->c0 >= 0) continue;
        it->r1 = it->r0 + rspan[i];
        int col = it->r0 < 64 ? row_cursor[it->r0] : 0;
        while (col + cspan[i] <= GRID_MAX_COLS && occ_overlaps(g, it->r0, it->r1, col, col + cspan[i])) col++;
        if (col + cspan[i] > GRID_MAX_COLS) col = GRID_MAX_COLS - cspan[i];
        it->c0 = col; it->c1 = col + cspan[i];
        if (it->c1 > ncols) ncols = it->c1;
        if (it->r0 < 64) row_cursor[it->r0] = it->c1;
        occ_mark(g, it);
    }
    /* 4. auto-placement cursor */
    int cr = 0, cc = 0;
    for (int i = 0; i < g->n; i++) {
        GridItem *it = &g->items[i];
        if (it->r0 >= 0) continue;
        int rs = rspan[i];
        if (it->c0 >= 0) {
            it->c1 = it->c0 + cspan[i];
            if (it->c0 < cc) cr++;
            cc = it->c0;
            while (cr < GRID_MAX_ROWS && occ_overlaps(g, cr, cr + rs, it->c0, it->c1)) cr++;
        } else {
            int cs = cspan[i] > ncols ? ncols : cspan[i];
            for (;;) {
                while (cc + cs <= ncols && occ_overlaps(g, cr, cr + rs, cc, cc + cs)) cc++;
                if (cc + cs <= ncols || cr >= GRID_MAX_ROWS) break;
                cr++; cc = 0;
            }
            it->c0 = cc; it->c1 = cc + cs;
        }
        it->r0 = cr; it->r1 = cr + rs;
        occ_mark(g, it);
    }
    int nrows = ner;
    for (int i = 0; i < g->n; i++) if (g->items[i].r1 > nrows) nrows = g->items[i].r1;
    g->ncols = ncols;
    g->nrows = nrows > GRID_MAX_ROWS ? GRID_MAX_ROWS : nrows;
    for (int i = 0; i < g->n; i++) {
        GridItem *it = &g->items[i];
        if (it->r1 > g->nrows) it->r1 = g->nrows;
        if (it->r0 >= it->r1) it->r0 = it->r1 - 1;
    }
    free(rspan); free(cspan);
    free(g->occ); g->occ = NULL; g->occ_rows = 0;
    return 1;
}

static void grid_free(Grid *g) { free(g->items); free(g->occ); memset(g, 0, sizeof(*g)); }

/* ------------------------------------------------------------------------- */
/* track sizing (CSS Grid 1 section 12), shared by both axes                  */
/* ------------------------------------------------------------------------- */

typedef struct Axis {
    int n;                   /* track count                                  */
    TrackFn *fn;
    float *base, *growth, *size, *pos;
    float gap;
    int rows;                /* 1: the block axis (item r0/r1)               */
} Axis;

static int axis_init(Axis *ax, int n, const CssTrack *tracks, int count, float avail, float gap, int rows)
{
    memset(ax, 0, sizeof(*ax));
    ax->n = n; ax->gap = gap; ax->rows = rows;
    ax->fn = calloc((size_t)(n ? n : 1), sizeof(TrackFn));
    ax->base = calloc((size_t)(n ? n : 1) * 4, sizeof(float));
    if (!ax->fn || !ax->base) { free(ax->fn); free(ax->base); memset(ax, 0, sizeof(*ax)); return 0; }
    ax->growth = ax->base + n; ax->size = ax->growth + n; ax->pos = ax->size + n;
    for (int i = 0; i < n; i++) ax->fn[i] = track_fn(tracks, count, i, avail);
    return 1;
}

static void axis_free(Axis *ax) { free(ax->fn); free(ax->base); memset(ax, 0, sizeof(*ax)); }

static void item_span(const Axis *ax, const GridItem *it, int *a, int *b)
{
    *a = ax->rows ? it->r0 : it->c0;
    *b = ax->rows ? it->r1 : it->c1;
}

static int is_intrinsic_min(uint8_t mn) { return mn == BR_AUTO || mn == BR_MIN || mn == BR_MAX; }
static int is_intrinsic_max(uint8_t mx) { return mx == BR_AUTO || mx == BR_MIN || mx == BR_MAX || mx == BR_FIT; }

/* Whether an item's contributions matter for the tracks it spans. */
static int item_needs_contribution(const Axis *ax, const GridItem *it, int max_content_mode)
{
    int a, b;
    item_span(ax, it, &a, &b);
    for (int i = a; i < b; i++) {
        const TrackFn *f = &ax->fn[i];
        if (f->mn != BR_FIXED) return 1;
        if (f->mx == BR_FIXED) continue;
        if (f->mx == BR_FLEX && max_content_mode != 1) continue;
        return 1;
    }
    return 0;
}

/* Step 3: the used size of 1fr for `space` minus the non-flexible tracks. */
static float find_fr_size(int nc, const TrackFn *fn, const float *base, const float *cols, float space)
{
    uint8_t stack_flags[64];
    uint8_t *inflexible = nc <= 64 ? stack_flags : malloc((size_t)nc);
    if (!inflexible) return 0;
    for (int i = 0; i < nc; i++) inflexible[i] = fn[i].mx != BR_FLEX;
    float result = 0;
    for (int guard = 0; guard <= nc; guard++) {
        float left = space, sum_fr = 0;
        for (int i = 0; i < nc; i++) {
            if (inflexible[i]) left -= fn[i].mx == BR_FLEX ? base[i] : cols[i];
            else sum_fr += fn[i].fr;
        }
        float hyp = left / kn_maxf(1.0f, sum_fr);
        int changed = 0;
        for (int i = 0; i < nc; i++)
            if (!inflexible[i] && hyp * fn[i].fr < base[i]) { inflexible[i] = 1; changed = 1; }
        result = hyp;
        if (!changed) break;
    }
    if (inflexible != stack_flags) free(inflexible);
    return result;
}

/* Add the multi-span items' contributions, smallest spans first. */
static void distribute_spanning(Axis *ax, const Grid *g, const uint8_t *need)
{
    int max_span = 1;
    for (int k = 0; k < g->n; k++) {
        if (!need[k]) continue;
        int a, b;
        item_span(ax, &g->items[k], &a, &b);
        if (b - a > max_span) max_span = b - a;
    }
    for (int span = 2; span <= max_span; span++) {
        for (int k = 0; k < g->n; k++) {
            const GridItem *it = &g->items[k];
            int a, b;
            item_span(ax, it, &a, &b);
            if (!need[k] || b - a != span) continue;
            float gaps = ax->gap * (float)(span - 1);
            float sum_base = gaps, sum_growth = gaps, sum_fr = 0;
            int n_min = 0, n_max = 0, crosses_flex = 0;
            for (int i = a; i < b; i++) {
                sum_base += ax->base[i];
                sum_growth += ax->growth[i];
                if (ax->fn[i].mx == BR_FLEX) { crosses_flex = 1; if (is_intrinsic_min(ax->fn[i].mn)) sum_fr += ax->fn[i].fr; }
                n_min += is_intrinsic_min(ax->fn[i].mn) && ax->fn[i].mx != BR_FLEX;
                n_max += is_intrinsic_max(ax->fn[i].mx);
            }
            float extra = it->min_c - sum_base;
            if (crosses_flex) {
                /* only the flexible tracks with an intrinsic minimum grow, by fr */
                if (extra > 0 && sum_fr > 0)
                    for (int i = a; i < b; i++)
                        if (ax->fn[i].mx == BR_FLEX && is_intrinsic_min(ax->fn[i].mn)) ax->base[i] += extra * ax->fn[i].fr / sum_fr;
                for (int i = a; i < b; i++) if (ax->fn[i].mx == BR_FLEX) ax->growth[i] = ax->base[i];
                continue;
            }
            if (extra > 0 && n_min)
                for (int i = a; i < b; i++) if (is_intrinsic_min(ax->fn[i].mn)) ax->base[i] += extra / (float)n_min;
            float gextra = it->max_c - sum_growth;
            if (gextra > 0 && n_max)
                for (int i = a; i < b; i++) {
                    if (!is_intrinsic_max(ax->fn[i].mx)) continue;
                    ax->growth[i] += gextra / (float)n_max;
                    if (ax->fn[i].mx == BR_FIT) ax->growth[i] = kn_minf(ax->growth[i], kn_maxf(ax->fn[i].mx_px, ax->base[i]));
                }
            for (int i = a; i < b; i++) ax->growth[i] = kn_maxf(ax->growth[i], ax->base[i]);
        }
    }
}

/* Size the tracks of `ax` from the items' min_c/max_c (only items with
 * need[k] contribute).  avail: definite space or <0;
 * mode: 0 = lay out in avail, 1 = max-content, 2 = min-content. */
static void size_tracks(Axis *ax, const Grid *g, const uint8_t *need, float avail, int mode)
{
    int n = ax->n;
    const TrackFn *fn = ax->fn;
    float *base = ax->base, *growth = ax->growth, *cols = ax->size;
    float *minc = calloc((size_t)(n ? n : 1) * 2, sizeof(float));
    if (!minc) { for (int i = 0; i < n; i++) cols[i] = 0; return; }
    float *maxc = minc + n;
    int has_flex = 0;
    for (int i = 0; i < n; i++) has_flex |= fn[i].mx == BR_FLEX;
    for (int k = 0; k < g->n; k++) {
        const GridItem *it = &g->items[k];
        int a, b;
        item_span(ax, it, &a, &b);
        if (!need[k] || b - a != 1) continue;
        minc[a] = kn_maxf(minc[a], it->min_c);
        maxc[a] = kn_maxf(maxc[a], it->max_c);
    }
    /* 1. base sizes and growth limits */
    for (int i = 0; i < n; i++) {
        const TrackFn *f = &fn[i];
        base[i] = f->mn == BR_FIXED ? f->mn_px : f->mn == BR_MAX ? maxc[i] : minc[i];
        switch (f->mx) {
        case BR_FIXED: growth[i] = f->mx_px; break;
        case BR_MIN:   growth[i] = minc[i]; break;
        case BR_FIT:   growth[i] = kn_minf(f->mx_px, maxc[i]); break;
        case BR_FLEX:  growth[i] = base[i]; break;
        default:       growth[i] = maxc[i]; break;
        }
        growth[i] = kn_maxf(growth[i], base[i]);
    }
    distribute_spanning(ax, g, need);
    float total_gap = n > 1 ? ax->gap * (float)(n - 1) : 0;
    if (avail < 0 || mode) {
        if (mode == 2) {
            for (int i = 0; i < n; i++) cols[i] = base[i];
            free(minc);
            return;
        }
        /* max-content / indefinite: growth limits; flexible tracks share the
         * largest per-fr max-content contribution (the flex fraction) */
        float frac = 0;
        for (int i = 0; i < n; i++)
            if (fn[i].mx == BR_FLEX) frac = kn_maxf(frac, kn_maxf(base[i], maxc[i]) / kn_maxf(1.0f, fn[i].fr));
        for (int k = 0; k < g->n; k++) {
            const GridItem *it = &g->items[k];
            int a, b;
            item_span(ax, it, &a, &b);
            if (!need[k] || b - a < 2) continue;
            float rest = it->max_c - ax->gap * (float)(b - a - 1), sum_fr = 0;
            for (int i = a; i < b; i++) {
                if (fn[i].mx == BR_FLEX) sum_fr += fn[i].fr;
                else rest -= base[i];
            }
            if (sum_fr > 0) frac = kn_maxf(frac, rest / kn_maxf(1.0f, sum_fr));
        }
        for (int i = 0; i < n; i++)
            cols[i] = fn[i].mx == BR_FLEX ? kn_maxf(base[i], frac * fn[i].fr) : growth[i];
        free(minc);
        return;
    }
    float used = total_gap;
    for (int i = 0; i < n; i++) { cols[i] = base[i]; used += cols[i]; }
    float free_sp = avail - used;
    /* 2. maximize: grow tracks towards their growth limits, equally */
    for (int guard = 0; guard < 32 && free_sp > 0.001f; guard++) {
        int growable = 0;
        for (int i = 0; i < n; i++) if (cols[i] < growth[i] - 0.001f) growable++;
        if (!growable) break;
        float share = free_sp / (float)growable;
        for (int i = 0; i < n; i++) {
            if (cols[i] >= growth[i] - 0.001f) continue;
            float gr = kn_minf(share, growth[i] - cols[i]);
            cols[i] += gr; free_sp -= gr;
        }
    }
    /* 3. expand flexible tracks */
    if (has_flex) {
        float fr = find_fr_size(n, fn, base, cols, avail - total_gap);
        free_sp = avail - total_gap;
        for (int i = 0; i < n; i++) {
            if (fn[i].mx == BR_FLEX) cols[i] = kn_maxf(base[i], fr * fn[i].fr);
            free_sp -= cols[i];
        }
    }
    /* 4. stretch auto max tracks with what is left */
    if (free_sp > 0.001f) {
        int autos = 0;
        for (int i = 0; i < n; i++) autos += fn[i].mx == BR_AUTO;
        if (autos) {
            float share = free_sp / (float)autos;
            for (int i = 0; i < n; i++) if (fn[i].mx == BR_AUTO) cols[i] += share;
        }
    }
    free(minc);
}

static void axis_positions(Axis *ax)
{
    float x = 0;
    for (int i = 0; i < ax->n; i++) { ax->pos[i] = x; x += ax->size[i] + ax->gap; }
}

static float axis_total(const Axis *ax)
{
    float t = ax->n > 1 ? ax->gap * (float)(ax->n - 1) : 0;
    for (int i = 0; i < ax->n; i++) t += ax->size[i];
    return t;
}

/* Area size of tracks [a, b). */
static float axis_span_size(const Axis *ax, int a, int b)
{
    if (b <= a) return 0;
    return ax->pos[b - 1] + ax->size[b - 1] - ax->pos[a];
}

/* Column contributions: min/max-content margin-box widths. */
static void column_contributions(LayoutTree *t, Axis *ax, Grid *g, uint8_t *need, int mode)
{
    for (int k = 0; k < g->n; k++) {
        GridItem *it = &g->items[k];
        need[k] = (uint8_t)item_needs_contribution(ax, it, mode);
        if (!need[k]) continue;
        it->min_c = layout_intrinsic_margin_width(t, it->box, 0, 0.0f);
        it->max_c = layout_intrinsic_margin_width(t, it->box, 1, 0.0f);
    }
}

/* ------------------------------------------------------------------------- */
/* items                                                                      */
/* ------------------------------------------------------------------------- */

/* justify/align-self: normal stretches every item except replaced elements
 * with a natural aspect ratio (Chrome: inputs and selects stretch, <input
 * type=color> keeps its size). */
static int grid_item_stretches(const Box *it)
{
    if (it->type == BOX_IMAGE || it->type == BOX_CANVAS || it->type == BOX_VIDEO) return 0;
    if (it->type == BOX_INPUT && it->node) {
        size_t len = 0;
        const lxb_char_t *ty = lxb_dom_element_get_attribute(lxb_dom_interface_element(it->node), (const lxb_char_t *)"type", 4, &len);
        if (ty && kn_str_ieq((const char *)ty, len, "color")) return 0;
    }
    return 1;
}

static int item_justify(const Box *gb, const Box *it)
{
    int v = it->style->justify_self;
    return v == CSS_ALIGN_ITEMS_AUTO ? gb->style->justify_items : v;
}

static int item_align(const Box *gb, const Box *it)
{
    int v = it->style->align_self;
    return v == CSS_ALIGN_ITEMS_AUTO ? gb->style->align_items : v;
}

static int stretches_w(const Box *gb, const Box *it)
{
    const CssComputed *cs = it->style;
    return item_justify(gb, it) == CSS_ALIGN_ITEMS_STRETCH && cs->width.kind == CSS_LEN_AUTO
        && cs->margin[CSS_LEFT].kind != CSS_LEN_AUTO && cs->margin[CSS_RIGHT].kind != CSS_LEN_AUTO && grid_item_stretches(it);
}

static int stretches_h(const Box *gb, const Box *it)
{
    const CssComputed *cs = it->style;
    return item_align(gb, it) == CSS_ALIGN_ITEMS_STRETCH && cs->height.kind == CSS_LEN_AUTO
        && cs->margin[CSS_TOP].kind != CSS_LEN_AUTO && cs->margin[CSS_BOTTOM].kind != CSS_LEN_AUTO && grid_item_stretches(it);
}

static float non_auto_margin(const Box *it, int side)
{
    return it->style->margin[side].kind == CSS_LEN_AUTO ? 0 : it->margin[side];
}

/* Lay an item out in an area of aw x ah (ah < 0: indefinite, row sizing).
 * Width: stretched to the area, else fit-content in it; height: stretched to
 * a definite area.  stretch_only: the item is already laid out at this width
 * (row sizing) and only the height stretch remains. */
static void layout_item(LayoutTree *t, Box *gb, Box *it, float aw, float ah, int stretch_only)
{
    LayoutInput ci;
    memset(&ci, 0, sizeof(ci));
    ci.cb_w = aw; ci.cb_h = ah >= 0 ? ah : LAYOUT_INDEFINITE; ci.avail_w = aw;
    ci.override_h = -1;
    LayoutOutput o;
    if (!stretch_only) {
        layout_resolve_edges(t, it, aw);
        float ml = non_auto_margin(it, CSS_LEFT), mr = non_auto_margin(it, CSS_RIGHT);
        int sw = stretches_w(gb, it);
        ci.override_w = sw ? kn_maxf(0, aw - ml - mr) : -1;
        ci.shrink_to_fit = !sw;
        layout_box(t, it, &ci, &o);
    }
    if (ah >= 0 && stretches_h(gb, it)) {
        float hh = kn_maxf(0, ah - non_auto_margin(it, CSS_TOP) - non_auto_margin(it, CSS_BOTTOM));
        layout_apply_min_max_h(it, ah, &hh);
        if (fabsf(hh - it->h) > 0.001f) {
            ci.override_w = it->w; ci.override_h = hh; ci.definite_h = 1; ci.shrink_to_fit = 0;
            layout_box(t, it, &ci, &o);
        }
    }
}

/* Offset of an item of margin-box size `used` in an area of `space` along
 * one axis: auto margins absorb positive free space, else the alignment. */
static float align_offset(float space, float used, int auto0, int auto1, int align, float *m0, float *m1)
{
    float free_sp = space - used;
    if (auto0 || auto1) {
        float f = kn_maxf(free_sp, 0);
        if (auto0 && auto1) { *m0 = f * 0.5f; *m1 = f - *m0; return *m0; }
        if (auto0) { *m0 = f; return f; }
        *m1 = f;
        return 0;
    }
    switch (align) {
    case CSS_ALIGN_ITEMS_FLEX_END: return free_sp;
    case CSS_ALIGN_ITEMS_CENTER: return free_sp * 0.5f;
    default: return 0;
    }
}

/* The final layout an item needs after row sizing: 0 none (the row-measuring
 * layout stands), 1 only the height stretch, 2 a full layout in its area
 * (not measured, or percentage heights now resolve against the area). */
static int final_layout_kind(const Box *gb, const GridItem *it)
{
    const CssComputed *cs = it->box->style;
    int pct_h = (cs->height.kind == CSS_LEN_VALUE && cs->height.pct != 0) || (cs->min_height.kind == CSS_LEN_VALUE && cs->min_height.pct != 0)
             || (cs->max_height.kind == CSS_LEN_VALUE && cs->max_height.pct != 0);
    if (!it->laid_out || pct_h) return 2;
    return stretches_h(gb, it->box) ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* entry points                                                               */
/* ------------------------------------------------------------------------- */

void layout_grid_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h)
{
    const CssComputed *s = b->style;
    float cx = box_cl(b), cy = box_ct(b), cw = box_cw(b);
    float avail_h = in->definite_h && in->cb_h >= 0 ? in->cb_h : -1;

    /* abs children: static position at the padding-box start (sole grid item in
     * an area spanning the padding box, justify/align-self normal -> start) */
    for (Box *c = b->first_child; c; c = c->next)
        if (c->flags & BOXF_OUT_OF_FLOW) { c->static_x = b->border[CSS_LEFT]; c->static_y = b->border[CSS_TOP]; c->static_fx = c->static_fy = 0; }

    b->first_baseline = -1;
    b->baseline_offset = -1;
    *content_h = 0;
    Grid g;
    if (!grid_place(b, &g) || !g.n) { grid_free(&g); return; }
    uint8_t *need = calloc((size_t)g.n, 1);
    Axis cols, rows;
    if (!need || !axis_init(&cols, g.ncols, s->grid_columns, s->grid_column_count, cw, gap_px(&s->column_gap, cw), 0)) {
        free(need); grid_free(&g); return;
    }
    if (!axis_init(&rows, g.nrows, s->grid_rows, s->grid_row_count, avail_h, gap_px(&s->row_gap, avail_h), 1)) {
        axis_free(&cols); free(need); grid_free(&g); return;
    }

    /* columns */
    column_contributions(t, &cols, &g, need, 0);
    size_tracks(&cols, &g, need, cw, 0);
    axis_positions(&cols);

    /* rows: item heights laid out in their column areas */
    for (int k = 0; k < g.n; k++) {
        GridItem *it = &g.items[k];
        need[k] = (uint8_t)item_needs_contribution(&rows, it, avail_h < 0 ? 1 : 0);
        if (!need[k]) continue;
        layout_item(t, b, it->box, axis_span_size(&cols, it->c0, it->c1), -1, 0);
        it->laid_out = 1;
        it->min_c = it->max_c = box_margin_h(it->box);
    }
    size_tracks(&rows, &g, need, avail_h, avail_h < 0 ? 1 : 0);
    axis_positions(&rows);

    /* final layout and alignment */
    GridItem *first = NULL, *first_bl = NULL;
    for (int k = 0; k < g.n; k++) {
        GridItem *it = &g.items[k];
        Box *ib = it->box;
        float ax = cols.pos[it->c0], aw = axis_span_size(&cols, it->c0, it->c1);
        float ay = rows.pos[it->r0], ah = axis_span_size(&rows, it->r0, it->r1);
        int fl = final_layout_kind(b, it);
        if (fl) layout_item(t, b, ib, aw, ah, fl == 1);
        const CssComputed *cs = ib->style;
        float ml = non_auto_margin(ib, CSS_LEFT), mr = non_auto_margin(ib, CSS_RIGHT);
        float mt = non_auto_margin(ib, CSS_TOP), mb = non_auto_margin(ib, CSS_BOTTOM);
        int jl = cs->margin[CSS_LEFT].kind == CSS_LEN_AUTO, jr = cs->margin[CSS_RIGHT].kind == CSS_LEN_AUTO;
        int at = cs->margin[CSS_TOP].kind == CSS_LEN_AUTO, ab = cs->margin[CSS_BOTTOM].kind == CSS_LEN_AUTO;
        float am0 = 0, am1 = 0, bm0 = 0, bm1 = 0;
        float dx = align_offset(aw, ib->w + ml + mr, jl, jr, item_justify(b, ib), &am0, &am1);
        float dy = align_offset(ah, ib->h + mt + mb, at, ab, item_align(b, ib), &bm0, &bm1);
        /* used auto margins */
        ib->margin[CSS_LEFT] = jl ? am0 : ml;  ib->margin[CSS_RIGHT] = jr ? am1 : mr;
        ib->margin[CSS_TOP] = at ? bm0 : mt;   ib->margin[CSS_BOTTOM] = ab ? bm1 : mb;
        ib->lx = cx + ax + (jl ? dx : dx + ml);
        ib->ly = cy + ay + (at ? dy : dy + mt);
        layout_relative_offset(ib, aw, ah);
        if (it->r0 == 0) {
            if (!first || it->c0 < first->c0) first = it;
            if (ib->first_baseline >= 0 && (!first_bl || it->c0 < first_bl->c0)) first_bl = it;
        }
    }
    /* first baseline: the first-row item with a baseline; else synthesized
     * from the first item's border-box bottom (Blink) */
    if (first_bl) b->first_baseline = first_bl->box->ly + first_bl->box->first_baseline;
    else if (first) b->first_baseline = first->box->ly + first->box->h;
    b->baseline_offset = b->first_baseline;
    *content_h = axis_total(&rows);
    axis_free(&rows);
    axis_free(&cols);
    free(need);
    grid_free(&g);
}

void layout_grid_intrinsic(LayoutTree *t, Box *b, float *min_w, float *max_w)
{
    const CssComputed *s = b->style;
    *min_w = *max_w = 0;
    Grid g;
    if (!grid_place(b, &g)) return;
    Axis cols;
    uint8_t *need = calloc((size_t)(g.n ? g.n : 1), 1);
    float gap = gap_px(&s->column_gap, -1);
    if (!need || !axis_init(&cols, g.n ? g.ncols : (s->grid_column_count ? s->grid_column_count : 1), s->grid_columns, s->grid_column_count, -1, gap, 0)) {
        free(need); grid_free(&g); return;
    }
    column_contributions(t, &cols, &g, need, 2);
    size_tracks(&cols, &g, need, -1, 2);
    float mn = axis_total(&cols);
    column_contributions(t, &cols, &g, need, 1);
    size_tracks(&cols, &g, need, -1, 1);
    float mx = axis_total(&cols);
    *min_w = mn;
    *max_w = kn_maxf(mx, mn);
    axis_free(&cols);
    free(need);
    grid_free(&g);
}
