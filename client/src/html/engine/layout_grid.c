/*
 * layout_grid.c - CSS grid layout (inventory section 6).
 *
 * Supported: grid-template-columns / grid-template-rows with
 * <length-percentage> / calc / auto / min-content / max-content / <flex> /
 * fit-content() / minmax() / repeat(<integer>) / repeat(auto-fill | auto-fit)
 * tracks and [line names]; grid-template-areas (named areas and their
 * implicit -start / -end lines); grid-auto-rows / grid-auto-columns (track
 * lists cycled over the implicit tracks after and, backwards, before the
 * explicit grid); grid-auto-flow row | column [dense]; row-gap /
 * column-gap; line-based placement (grid-row/column-start/end and their
 * shorthands: <integer>, negative lines, <custom-ident> [<integer>], span
 * <integer> | <custom-ident>), lines outside the explicit grid on both
 * sides (implicit tracks before it included); auto-placement (CSS Grid 1
 * section 8.5, sparse or dense); justify-self / align-self (+
 * justify-items / align-items) incl. auto margins and first-baseline
 * alignment of the items sharing a start row.  `normal` stretches every
 * item in both axes except replaced elements with a natural aspect ratio
 * (img, canvas, video, input[type=color]: start-aligned like Chrome; other
 * form controls stretch).  auto-fit collapses the empty repeated tracks
 * (and their gutters); justify-content / align-content (start, end,
 * center, space-between/around/evenly; normal and stretch stretch the auto
 * tracks).  Not supported: subgrid, masonry, `order`, last-baseline.
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
 * items' margin-box heights laid out in their column areas (baseline-aligned
 * items include their baseline shim), rows are sized against the content
 * height when it is definite (fr rows fill it).  Intrinsic widths
 * (min/max-content of the grid) use the bases (min) or the growth limits with
 * the flex fraction of the max-content contributions (max); an auto repeat
 * then repeats once.
 */
#include "layout_internal.h"

#define GRID_MAX_TRACKS 4096     /* per axis, explicit + implicit            */
#define GRID_MAX_LINE   2000     /* |line index| any placement may reach     */
#define GRID_INF 1e30f

enum { AX_ROW = CSS_GRID_AXIS_ROW, AX_COL = CSS_GRID_AXIS_COLUMN };

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

/* Sizing functions of one track size against `avail` (<0: indefinite). */
static TrackFn track_fn_of(const CssTrack *tr, float avail)
{
    TrackFn f;
    memset(&f, 0, sizeof(f));
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
/* the explicit grid of one axis: expanded template, named lines              */
/* ------------------------------------------------------------------------- */

typedef struct NamedLine { int line; const char *name; } NamedLine;

typedef struct GridLines {
    const CssComputed *s;
    int ax;
    float avail;             /* the axis' definite size, or <0               */
    int n_template;          /* tracks sized by the template (auto repeat expanded) */
    int n_explicit;          /* max(n_template, areas' track count)          */
    CssTrack *tmpl;          /* n_template                                   */
    int rep_at, rep_len;     /* auto-repeat tracks [rep_at, rep_at + rep_len) (rep_len 0: none) */
    int auto_fit;
    NamedLine *names;        /* explicit lines' names (incl. area -start/-end) */
    int n_names, cap_names;
} GridLines;

static void lines_add_name(GridLines *gl, int line, const char *name)
{
    if (gl->n_names == gl->cap_names) {
        int nc = gl->cap_names ? gl->cap_names * 2 : 16;
        NamedLine *p = realloc(gl->names, (size_t)nc * sizeof(NamedLine));
        if (!p) return;
        gl->names = p; gl->cap_names = nc;
    }
    gl->names[gl->n_names].line = line;
    gl->names[gl->n_names].name = name;
    gl->n_names++;
}

/* Adds every name of a space-separated atom to `line`. */
static void lines_add_names(GridLines *gl, int line, const char *joined)
{
    if (!joined) return;
    const char *p = joined;
    while (*p) {
        while (*p == ' ') p++;
        const char *q = p;
        while (*q && *q != ' ') q++;
        if (q > p) lines_add_name(gl, line, kn_atom(p, (size_t)(q - p)));
        p = q;
    }
}

/* Size of a track for counting auto repetitions: its max sizing function
 * when definite, else its min. */
static float repeat_track_px(const CssTrack *tr, float avail)
{
    TrackFn f = track_fn_of(tr, avail);
    if (f.mx == BR_FIXED) return f.mx_px;
    if (f.mn == BR_FIXED) return f.mn_px;
    return 0;
}

/* The named areas of grid-template-areas: calls fn for each area once with
 * its bounds (row/column lines, end exclusive). */
typedef void (*AreaFn)(void *user, const char *name, int r0, int r1, int c0, int c1);

static void grid_for_each_area(const CssComputed *s, AreaFn fn, void *user)
{
    if (!s->grid_areas || !s->grid_area_rows || !s->grid_area_cols) return;
    int nr = s->grid_area_rows, nc = s->grid_area_cols;
    const char **cell = calloc((size_t)(nr * nc), sizeof(const char *));
    if (!cell) return;
    const char *p = s->grid_areas;
    for (int k = 0; k < nr * nc && *p; k++) {
        while (*p == ' ' || *p == '\n') p++;
        const char *q = p;
        while (*q && *q != ' ' && *q != '\n') q++;
        cell[k] = (q - p == 1 && *p == '.') ? NULL : kn_atom(p, (size_t)(q - p));
        p = q;
    }
    for (int k = 0; k < nr * nc; k++) {
        const char *a = cell[k];
        if (!a) continue;
        int first = 1;
        for (int j = 0; j < k; j++) if (cell[j] == a) { first = 0; break; }
        if (!first) continue;
        int r0 = nr, r1 = 0, c0 = nc, c1 = 0;
        for (int j = 0; j < nr * nc; j++) {
            if (cell[j] != a) continue;
            int r = j / nc, c = j % nc;
            if (r < r0) r0 = r;
            if (r + 1 > r1) r1 = r + 1;
            if (c < c0) c0 = c;
            if (c + 1 > c1) c1 = c + 1;
        }
        fn(user, a, r0, r1, c0, c1);
    }
    free(cell);
}

static void area_names_cb(void *user, const char *name, int r0, int r1, int c0, int c1)
{
    GridLines *gl = user;
    char buf[256];
    int a = gl->ax == AX_ROW ? r0 : c0, b = gl->ax == AX_ROW ? r1 : c1;
    snprintf(buf, sizeof(buf), "%s-start", name);
    lines_add_name(gl, a, kn_atom_cstr(buf));
    snprintf(buf, sizeof(buf), "%s-end", name);
    lines_add_name(gl, b, kn_atom_cstr(buf));
}

/* Expands one axis' template against `avail` (the content size when
 * definite: it sets the auto-repeat count) and collects its line names. */
static int lines_init(GridLines *gl, const CssComputed *s, int ax, float avail, float gap)
{
    memset(gl, 0, sizeof(*gl));
    gl->s = s; gl->ax = ax; gl->avail = avail;
    const CssTrack *tr = ax == AX_ROW ? s->grid_rows : s->grid_columns;
    int count = ax == AX_ROW ? s->grid_row_count : s->grid_column_count;
    const CssGridNames *nm = &s->grid_names[ax];
    int reps = 1, rn = 0, ra = 0;
    if (count && nm->repeat && nm->repeat_n) {
        ra = nm->repeat_at; rn = nm->repeat_n;
        if (avail >= 0) {
            float out = 0, in = 0;
            for (int i = 0; i < count; i++) {
                float px = repeat_track_px(&tr[i], avail);
                if (i >= ra && i < ra + rn) in += px; else out += px;
            }
            int n_out = count - rn;
            float room = avail - out - (n_out > 0 ? gap * (float)(n_out - 1) : -gap);
            float per = in + gap * (float)rn;
            reps = per > 0.001f ? (int)floorf((room + 0.001f) / per) : 1;
            if (reps < 1) reps = 1;
            if (n_out + reps * rn > GRID_MAX_TRACKS / 2) reps = (GRID_MAX_TRACKS / 2 - n_out) / rn;
            if (reps < 1) reps = 1;
        }
        gl->rep_at = ra; gl->rep_len = reps * rn;
        gl->auto_fit = nm->repeat == CSS_GRID_REPEAT_AUTO_FIT;
    }
    int n = count + (rn ? (reps - 1) * rn : 0);
    gl->n_template = n;
    gl->tmpl = calloc((size_t)(n ? n : 1), sizeof(CssTrack));
    if (!gl->tmpl) return 0;
    for (int i = 0; i < n; i++) {
        int src = i;
        if (rn && i >= ra) src = i < ra + reps * rn ? ra + (i - ra) % rn : i - (reps - 1) * rn;
        gl->tmpl[i] = tr[src];
    }
    /* line names */
    for (int line = 0; line <= n && count; line++) {
        if (!rn || line < ra) { lines_add_names(gl, line, nm->line[line]); continue; }
        int rel = line - ra, end = reps * rn;
        if (rel > end) { lines_add_names(gl, line, nm->line[line - (reps - 1) * rn]); continue; }
        if (rel == 0) lines_add_names(gl, line, nm->line[ra]);
        if (rel == end) lines_add_names(gl, line, nm->line[ra + rn]);
        int q = rel % rn;
        if (q == 0) {
            if (rel > 0) lines_add_names(gl, line, nm->rep_last);
            if (rel < end) lines_add_names(gl, line, nm->rep_first);
        } else {
            lines_add_names(gl, line, nm->line[ra + q]);
        }
    }
    int n_area = ax == AX_ROW ? s->grid_area_rows : s->grid_area_cols;
    gl->n_explicit = n > n_area ? n : n_area;
    grid_for_each_area(s, area_names_cb, gl);
    return 1;
}

static void lines_free(GridLines *gl) { free(gl->tmpl); free(gl->names); memset(gl, 0, sizeof(*gl)); }

/* Sizing functions of the track at explicit index e (may be negative or past
 * the explicit grid: implicit tracks cycle grid-auto-rows/columns). */
static TrackFn lines_track_fn(const GridLines *gl, int e)
{
    if (e >= 0 && e < gl->n_template) return track_fn_of(&gl->tmpl[e], gl->avail);
    int k = gl->s->grid_auto_count[gl->ax];
    const CssTrack *at = gl->s->grid_auto_tracks[gl->ax];
    if (k < 1) {
        CssTrack a;
        memset(&a, 0, sizeof(a));
        a.size.kind = CSS_LEN_AUTO;
        return track_fn_of(&a, gl->avail);
    }
    int idx = e >= 0 ? (e - gl->n_template) % k : k - 1 - ((-e - 1) % k);
    return track_fn_of(&at[idx], gl->avail);
}

/* The n-th (1-based; negative counts from the end) explicit line named
 * `name`; missing lines are the implicit ones past (or before) the grid. */
static int lines_nth_named(const GridLines *gl, const char *name, int n)
{
    int cnt = 0;
    for (int i = 0; i < gl->n_names; i++) cnt += gl->names[i].name == name;
    int want = n > 0 ? n : cnt + n + 1;          /* 1-based from the start */
    if (n > 0 && n > cnt) return gl->n_explicit + (n - cnt);
    if (n < 0 && -n > cnt) return -(-n - cnt);
    /* names[] holds lines in increasing order per source, not globally: pick by sorting */
    int best = -1, seen = 0;
    for (int line = 0; line <= gl->n_explicit && seen < want; line++)
        for (int i = 0; i < gl->n_names; i++)
            if (gl->names[i].line == line && gl->names[i].name == name) { seen++; best = line; break; }
    return best;
}

static int lines_has_name(const GridLines *gl, int line, const char *name)
{
    for (int i = 0; i < gl->n_names; i++) if (gl->names[i].line == line && gl->names[i].name == name) return 1;
    return 0;
}

/* A definite <grid-line> (no span) -> explicit line index. */
static int lines_resolve(const GridLines *gl, const CssGridLine *l, int is_start)
{
    int v;
    if (!l->name) v = l->line > 0 ? l->line - 1 : gl->n_explicit + 1 + l->line;
    else if (l->line == 0) {
        char buf[256];
        snprintf(buf, sizeof(buf), "%s-%s", l->name, is_start ? "start" : "end");
        const char *edge = kn_atom_cstr(buf);
        int found = -1;
        for (int line = 0; line <= gl->n_explicit && found < 0; line++) if (lines_has_name(gl, line, edge)) found = line;
        v = found >= 0 ? found : lines_nth_named(gl, l->name, 1);
    } else {
        v = lines_nth_named(gl, l->name, l->line);
    }
    if (v > GRID_MAX_LINE) v = GRID_MAX_LINE;
    if (v < -GRID_MAX_LINE) v = -GRID_MAX_LINE;
    return v;
}

/* `span N name` from line `from` towards increasing (dir 1) or decreasing
 * (dir -1) lines: the explicit lines with that name, then the implicit lines
 * on that side of the explicit grid, which all carry every name. */
static int lines_span_named(const GridLines *gl, const char *name, int n, int from, int dir)
{
    int seen = 0;
    int line = from + dir;
    if (dir < 0 && line > gl->n_explicit) line = gl->n_explicit;
    if (dir > 0 && line < 0) line = 0;
    for (; line >= 0 && line <= gl->n_explicit; line += dir)
        if (lines_has_name(gl, line, name) && ++seen == n) return line;
    int rest = n - seen;
    if (dir > 0) return (from > gl->n_explicit ? from : gl->n_explicit) + rest;
    return (from < 0 ? from : 0) - rest;
}

/* Resolves one axis of an item: *p0 = start line (explicit coordinates) and
 * *definite, *span. */
static void resolve_axis(const GridLines *gl, const CssGridLine *s, const CssGridLine *e, int *p0, int *span, int *definite)
{
    int sdef = !s->span && (s->line || s->name), edef = !e->span && (e->line || e->name);
    int a = 0, b = 0, n = 1;
    *definite = 1;
    if (sdef && edef) {
        a = lines_resolve(gl, s, 1);
        b = lines_resolve(gl, e, 0);
        if (b < a) { int x = a; a = b; b = x; }
        if (b == a) b = a + 1;                   /* equal lines: span 1 */
    } else if (sdef) {
        a = lines_resolve(gl, s, 1);
        if (e->span && e->name) b = lines_span_named(gl, e->name, e->line, a, 1);
        else b = a + (e->span ? e->line : 1);
    } else if (edef) {
        b = lines_resolve(gl, e, 0);
        if (s->span && s->name) a = lines_span_named(gl, s->name, s->line, b, -1);
        else a = b - (s->span ? s->line : 1);
    } else {
        *definite = 0;
        /* a named span alone counts as span 1 */
        n = (s->span && !s->name) ? s->line : (!s->span && e->span && !e->name) ? e->line : 1;
        if (n < 1) n = 1;
        if (n > GRID_MAX_LINE) n = GRID_MAX_LINE;
        *p0 = 0; *span = n;
        return;
    }
    if (b - a > GRID_MAX_LINE) b = a + GRID_MAX_LINE;
    *p0 = a; *span = b - a;
}

/* ------------------------------------------------------------------------- */
/* placement (CSS Grid 1 section 8)                                            */
/* ------------------------------------------------------------------------- */

typedef struct GridItem {
    Box *box;
    int p0[2], p1[2];        /* AX_ROW / AX_COL lines in final track indices, end exclusive */
    float min_c, max_c;      /* contributions in the axis being sized        */
    float asc;               /* baseline items: margin-top + first baseline  */
    uint8_t laid_out;        /* row measuring laid it out in its column area */
    uint8_t baseline;        /* align-self: baseline                          */
} GridItem;

typedef struct Grid {
    GridItem *items;
    int n;
    int ntracks[2];          /* final track counts                           */
    int off[2];              /* implicit tracks before the explicit grid      */
    GridLines lines[2];
    uint8_t *collapsed[2];   /* auto-fit: empty repeated tracks              */
} Grid;

typedef struct Occ {
    uint8_t *cell;           /* [major * minor_cap + minor]                  */
    int minor_cap, major_cap;
} Occ;

static int occ_ensure(Occ *o, int major)
{
    if (major <= o->major_cap) return 1;
    int cap = o->major_cap ? o->major_cap : 16;
    while (cap < major) cap *= 2;
    uint8_t *p = realloc(o->cell, (size_t)cap * (size_t)o->minor_cap);
    if (!p) return 0;
    memset(p + (size_t)o->major_cap * (size_t)o->minor_cap, 0, (size_t)(cap - o->major_cap) * (size_t)o->minor_cap);
    o->cell = p;
    o->major_cap = cap;
    return 1;
}

static int occ_overlaps(const Occ *o, int M0, int M1, int m0, int m1)
{
    if (m1 > o->minor_cap) return 1;
    for (int M = M0; M < M1 && M < o->major_cap; M++)
        for (int m = m0; m < m1; m++) if (o->cell[(size_t)M * (size_t)o->minor_cap + (size_t)m]) return 1;
    return 0;
}

static void occ_mark(Occ *o, int M0, int M1, int m0, int m1)
{
    if (!occ_ensure(o, M1)) return;
    for (int M = M0; M < M1; M++)
        for (int m = m0; m < m1 && m < o->minor_cap; m++) o->cell[(size_t)M * (size_t)o->minor_cap + (size_t)m] = 1;
}

/* Place the in-flow children: definite items, then items locked to a major
 * line, then the auto-placement cursor (sparse or dense; major = rows for
 * grid-auto-flow: row, columns for column). */
static int grid_place(Box *b, Grid *g, float avail_w, float avail_h)
{
    const CssComputed *s = b->style;
    memset(g, 0, sizeof(*g));
    if (!lines_init(&g->lines[AX_ROW], s, AX_ROW, avail_h, gap_px(&s->row_gap, avail_h))
        || !lines_init(&g->lines[AX_COL], s, AX_COL, avail_w, gap_px(&s->column_gap, avail_w))) return 0;
    int n = 0;
    for (Box *c = b->first_child; c; c = c->next) if (!(c->flags & BOXF_OUT_OF_FLOW)) n++;
    g->items = calloc((size_t)(n ? n : 1), sizeof(GridItem));
    int *span = calloc((size_t)(n ? n : 1) * 2, sizeof(int));
    uint8_t *def = calloc((size_t)(n ? n : 1) * 2, 1);
    if (!g->items || !span || !def) { free(span); free(def); return 0; }
    /* resolve lines; offsets for implicit tracks before the explicit grid */
    int minline[2] = { 0, 0 };
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->flags & BOXF_OUT_OF_FLOW) continue;
        GridItem *it = &g->items[g->n];
        const CssGridLine *gl = c->style->grid_lines;
        it->box = c;
        for (int a = 0; a < 2; a++) {
            int d;
            resolve_axis(&g->lines[a], &gl[a], &gl[a + 2], &it->p0[a], &span[g->n * 2 + a], &d);
            def[g->n * 2 + a] = (uint8_t)d;
            if (d && it->p0[a] < minline[a]) minline[a] = it->p0[a];
        }
        g->n++;
    }
    for (int a = 0; a < 2; a++) g->off[a] = -minline[a];
    int major = (s->grid_auto_flow & CSS_GRID_FLOW_COLUMN) ? AX_COL : AX_ROW, minor = 1 - major;
    int dense = (s->grid_auto_flow & CSS_GRID_FLOW_DENSE) != 0;
    /* minor track count: explicit grid + definite placements + auto spans */
    int nminor = g->off[minor] + g->lines[minor].n_explicit, step2_extra = 0;
    if (nminor < 1) nminor = 1;
    for (int k = 0; k < g->n; k++) {
        GridItem *it = &g->items[k];
        for (int a = 0; a < 2; a++) if (def[k * 2 + a]) it->p0[a] += g->off[a];
        int sp = span[k * 2 + minor];
        int need = def[k * 2 + minor] ? it->p0[minor] + sp : sp;
        if (need > nminor) nminor = need;
        if (def[k * 2 + major] && !def[k * 2 + minor]) step2_extra += sp;
    }
    if (nminor > GRID_MAX_TRACKS) nminor = GRID_MAX_TRACKS;
    Occ o;
    memset(&o, 0, sizeof(o));
    o.minor_cap = nminor + step2_extra;
    if (o.minor_cap > GRID_MAX_TRACKS) o.minor_cap = GRID_MAX_TRACKS;
    /* 1. both axes definite */
    for (int k = 0; k < g->n; k++) {
        GridItem *it = &g->items[k];
        if (!def[k * 2] || !def[k * 2 + 1]) continue;
        for (int a = 0; a < 2; a++) it->p1[a] = it->p0[a] + span[k * 2 + a];
        occ_mark(&o, it->p0[major], it->p1[major], it->p0[minor], it->p1[minor]);
    }
    /* 2. definite major position, auto minor: the earliest free position (past
     * the items this step already put on that major line, unless dense) */
    int *cursor = calloc((size_t)GRID_MAX_LINE * 2 + 2, sizeof(int));
    for (int k = 0; k < g->n; k++) {
        GridItem *it = &g->items[k];
        if (!def[k * 2 + major] || def[k * 2 + minor]) continue;
        int sp = span[k * 2 + minor];
        it->p1[major] = it->p0[major] + span[k * 2 + major];
        int line = it->p0[major];
        int pos = (!dense && cursor && line < GRID_MAX_LINE * 2) ? cursor[line] : 0;
        while (pos + sp <= o.minor_cap && occ_overlaps(&o, it->p0[major], it->p1[major], pos, pos + sp)) pos++;
        if (pos + sp > o.minor_cap) pos = o.minor_cap - sp > 0 ? o.minor_cap - sp : 0;
        it->p0[minor] = pos; it->p1[minor] = pos + sp;
        if (it->p1[minor] > nminor) nminor = it->p1[minor];
        if (cursor && line < GRID_MAX_LINE * 2) cursor[line] = it->p1[minor];
        occ_mark(&o, it->p0[major], it->p1[major], pos, pos + sp);
    }
    free(cursor);
    /* 4. auto-placement cursor */
    int cM = 0, cm = 0;
    for (int k = 0; k < g->n; k++) {
        GridItem *it = &g->items[k];
        if (def[k * 2 + major]) continue;
        int sM = span[k * 2 + major];
        if (dense) { cM = 0; cm = 0; }
        if (def[k * 2 + minor]) {
            it->p1[minor] = it->p0[minor] + span[k * 2 + minor];
            if (!dense && it->p0[minor] < cm) cM++;
            cm = it->p0[minor];
            while (cM < GRID_MAX_TRACKS && occ_overlaps(&o, cM, cM + sM, it->p0[minor], it->p1[minor])) cM++;
        } else {
            int sm = span[k * 2 + minor] > nminor ? nminor : span[k * 2 + minor];
            for (;;) {
                while (cm + sm <= nminor && occ_overlaps(&o, cM, cM + sM, cm, cm + sm)) cm++;
                if (cm + sm <= nminor || cM >= GRID_MAX_TRACKS) break;
                cM++; cm = 0;
            }
            it->p0[minor] = cm; it->p1[minor] = cm + sm;
        }
        it->p0[major] = cM; it->p1[major] = cM + sM;
        occ_mark(&o, it->p0[major], it->p1[major], it->p0[minor], it->p1[minor]);
    }
    free(o.cell);
    free(span); free(def);
    /* track counts */
    for (int a = 0; a < 2; a++) {
        int nt = g->off[a] + g->lines[a].n_explicit;
        if (a == minor && nminor > nt) nt = nminor;
        for (int k = 0; k < g->n; k++) if (g->items[k].p1[a] > nt) nt = g->items[k].p1[a];
        if (nt > GRID_MAX_TRACKS) nt = GRID_MAX_TRACKS;
        if (nt < 1 && g->n) nt = 1;
        g->ntracks[a] = nt;
        for (int k = 0; k < g->n; k++) {
            GridItem *it = &g->items[k];
            if (it->p1[a] > nt) it->p1[a] = nt;
            if (it->p0[a] >= it->p1[a]) it->p0[a] = it->p1[a] - 1;
            if (it->p0[a] < 0) { it->p0[a] = 0; if (it->p1[a] < 1) it->p1[a] = 1; }
        }
        /* auto-fit: repeated tracks without items collapse */
        const GridLines *gl = &g->lines[a];
        if (gl->auto_fit && gl->rep_len && nt) {
            g->collapsed[a] = calloc((size_t)nt, 1);
            if (g->collapsed[a]) {
                for (int i = 0; i < gl->rep_len; i++) {
                    int tr = g->off[a] + gl->rep_at + i;
                    if (tr >= nt) break;
                    int used = 0;
                    for (int k = 0; k < g->n && !used; k++) used = g->items[k].p0[a] <= tr && tr < g->items[k].p1[a];
                    g->collapsed[a][tr] = (uint8_t)!used;
                }
            }
        }
    }
    return 1;
}

static void grid_free(Grid *g)
{
    free(g->items);
    for (int a = 0; a < 2; a++) { lines_free(&g->lines[a]); free(g->collapsed[a]); }
    memset(g, 0, sizeof(*g));
}

/* ------------------------------------------------------------------------- */
/* track sizing (CSS Grid 1 section 12), shared by both axes                  */
/* ------------------------------------------------------------------------- */

typedef struct Axis {
    int n;                   /* track count                                  */
    TrackFn *fn;
    float *base, *growth, *size, *pos;
    const uint8_t *collapsed;/* auto-fit collapsed tracks (NULL: none)       */
    int live;                /* tracks not collapsed                         */
    float gap;
    int a;                   /* AX_ROW / AX_COL                              */
    uint8_t distrib;         /* justify-content (columns) / align-content (rows): CssJustifyContent */
} Axis;

static int axis_init(Axis *ax, const Grid *g, int a, float gap)
{
    int n = g->ntracks[a];
    memset(ax, 0, sizeof(*ax));
    ax->n = n; ax->gap = gap; ax->a = a;
    ax->fn = calloc((size_t)(n ? n : 1), sizeof(TrackFn));
    ax->base = calloc((size_t)(n ? n : 1) * 4, sizeof(float));
    if (!ax->fn || !ax->base) { free(ax->fn); free(ax->base); memset(ax, 0, sizeof(*ax)); return 0; }
    ax->growth = ax->base + n; ax->size = ax->growth + n; ax->pos = ax->size + n;
    ax->collapsed = g->collapsed[a];
    for (int i = 0; i < n; i++) {
        if (ax->collapsed && ax->collapsed[i]) { ax->fn[i].mn = ax->fn[i].mx = BR_FIXED; continue; }
        ax->fn[i] = lines_track_fn(&g->lines[a], i - g->off[a]);
        ax->live++;
    }
    return 1;
}

static void axis_free(Axis *ax) { free(ax->fn); free(ax->base); memset(ax, 0, sizeof(*ax)); }

static float axis_total_gap(const Axis *ax) { return ax->live > 1 ? ax->gap * (float)(ax->live - 1) : 0; }

static void item_span(const Axis *ax, const GridItem *it, int *a, int *b)
{
    *a = it->p0[ax->a];
    *b = it->p1[ax->a];
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
    float total_gap = axis_total_gap(ax);
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
    /* 4. stretch auto max tracks with what is left (content distribution normal / stretch) */
    if (free_sp > 0.001f && ax->distrib == CSS_JUSTIFY_CONTENT_NORMAL) {
        int autos = 0;
        for (int i = 0; i < n; i++) autos += fn[i].mx == BR_AUTO;
        if (autos) {
            float share = free_sp / (float)autos;
            for (int i = 0; i < n; i++) if (fn[i].mx == BR_AUTO) cols[i] += share;
        }
    }
    free(minc);
}

static float axis_total(const Axis *ax);

/* Track offsets; `avail` >= 0 (the definite content size) applies justify-content /
 * align-content to the free space: a leading offset plus extra space between the live
 * tracks.  Negative free space keeps start for the space-* values (their fallback is
 * start / center: space-between -> start, space-around / evenly -> center) and overflows
 * both sides for center (unsafe, like Blink). */
static void axis_positions(Axis *ax, float avail)
{
    float lead = 0, between = 0;
    if (avail >= 0 && ax->distrib != CSS_JUSTIFY_CONTENT_NORMAL && ax->distrib != CSS_JUSTIFY_CONTENT_FLEX_START) {
        int live_n = 0;
        for (int i = 0; i < ax->n; i++) live_n += !(ax->collapsed && ax->collapsed[i]);
        float free_sp = avail - axis_total(ax);
        switch (ax->distrib) {
        case CSS_JUSTIFY_CONTENT_FLEX_END: lead = free_sp; break;
        case CSS_JUSTIFY_CONTENT_CENTER: lead = free_sp * 0.5f; break;
        case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN:
            if (free_sp > 0 && live_n > 1) between = free_sp / (float)(live_n - 1);
            break;
        case CSS_JUSTIFY_CONTENT_SPACE_AROUND:
            if (free_sp > 0 && live_n > 0) { between = free_sp / (float)live_n; lead = between * 0.5f; }
            else lead = free_sp * 0.5f;
            break;
        case CSS_JUSTIFY_CONTENT_SPACE_EVENLY:
            if (free_sp > 0) { between = free_sp / (float)(live_n + 1); lead = between; }
            else lead = free_sp * 0.5f;
            break;
        default: break;
        }
    }
    float x = lead;
    int any = 0;
    for (int i = 0; i < ax->n; i++) {
        int live = !(ax->collapsed && ax->collapsed[i]);
        if (live && any) x += ax->gap + between;
        ax->pos[i] = x;
        x += ax->size[i];
        any |= live;
    }
}

static float axis_total(const Axis *ax)
{
    float t = axis_total_gap(ax);
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
    /* an item with an aspect-ratio takes its height from its width: normal aligns it as start
     * (CSS Grid 1 / Box Alignment; the engine maps normal to stretch, so both do here) */
    return item_align(gb, it) == CSS_ALIGN_ITEMS_STRETCH && cs->height.kind == CSS_LEN_AUTO
        && cs->margin[CSS_TOP].kind != CSS_LEN_AUTO && cs->margin[CSS_BOTTOM].kind != CSS_LEN_AUTO && grid_item_stretches(it)
        && !(layout_pref_ratio(it) > 0.0f);
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

/* First baseline from the border-box top, synthesized from the border-box
 * bottom when the item has none (Blink). */
static float item_first_baseline(const Box *ib)
{
    if (ib->first_baseline >= 0 && !(ib->flags & BOXF_SCROLL_CONTAINER) && !box_is_replaced(ib)) return ib->first_baseline;
    return ib->h;
}

/* Baseline-sharing groups (items with align-self: baseline and the same start
 * row): the largest ascent of each group, written to row_asc[start row]. */
static void baseline_groups(const Grid *g, float *row_asc)
{
    for (int i = 0; i < g->ntracks[AX_ROW]; i++) row_asc[i] = 0;
    for (int k = 0; k < g->n; k++) {
        const GridItem *it = &g->items[k];
        if (it->baseline) row_asc[it->p0[AX_ROW]] = kn_maxf(row_asc[it->p0[AX_ROW]], it->asc);
    }
}

static void item_update_asc(GridItem *it)
{
    it->asc = non_auto_margin(it->box, CSS_TOP) + item_first_baseline(it->box);
}

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
    if (!grid_place(b, &g, cw, avail_h) || !g.n) {
        /* no items: the explicit grid still has its size (fixed tracks, gaps) */
        if (g.lines[AX_ROW].tmpl && g.lines[AX_ROW].n_explicit) {
            g.ntracks[AX_ROW] = g.off[AX_ROW] + g.lines[AX_ROW].n_explicit;
            Axis rows;
            if (axis_init(&rows, &g, AX_ROW, gap_px(&s->row_gap, avail_h))) {
                rows.distrib = s->align_content;
                size_tracks(&rows, &g, NULL, avail_h, avail_h < 0 ? 1 : 0);
                *content_h = axis_total(&rows);
                axis_free(&rows);
            }
        }
        grid_free(&g);
        return;
    }
    uint8_t *need = calloc((size_t)g.n, 1);
    float *row_asc = calloc((size_t)g.ntracks[AX_ROW], sizeof(float));
    Axis cols, rows;
    if (!need || !row_asc || !axis_init(&cols, &g, AX_COL, gap_px(&s->column_gap, cw))) {
        free(need); free(row_asc); grid_free(&g); return;
    }
    if (!axis_init(&rows, &g, AX_ROW, gap_px(&s->row_gap, avail_h))) {
        axis_free(&cols); free(need); free(row_asc); grid_free(&g); return;
    }

    cols.distrib = s->justify_content;
    rows.distrib = s->align_content;

    /* columns */
    column_contributions(t, &cols, &g, need, 0);
    size_tracks(&cols, &g, need, cw, 0);
    axis_positions(&cols, cw);

    /* rows: item heights laid out in their column areas, baseline-aligned
     * items shifted by their group's shim */
    for (int k = 0; k < g.n; k++) {
        GridItem *it = &g.items[k];
        it->baseline = item_align(b, it->box) == CSS_ALIGN_ITEMS_BASELINE;
        need[k] = (uint8_t)(item_needs_contribution(&rows, it, avail_h < 0 ? 1 : 0) || it->baseline);
        if (!need[k]) continue;
        layout_item(t, b, it->box, axis_span_size(&cols, it->p0[AX_COL], it->p1[AX_COL]), -1, 0);
        it->laid_out = 1;
        it->min_c = it->max_c = box_margin_h(it->box);
        if (it->baseline) item_update_asc(it);
    }
    baseline_groups(&g, row_asc);
    for (int k = 0; k < g.n; k++) {
        GridItem *it = &g.items[k];
        if (it->baseline) it->min_c = it->max_c = it->min_c + row_asc[it->p0[AX_ROW]] - it->asc;
        need[k] = (uint8_t)(need[k] && item_needs_contribution(&rows, it, avail_h < 0 ? 1 : 0));
    }
    size_tracks(&rows, &g, need, avail_h, avail_h < 0 ? 1 : 0);
    axis_positions(&rows, avail_h);

    /* final layout */
    for (int k = 0; k < g.n; k++) {
        GridItem *it = &g.items[k];
        float aw = axis_span_size(&cols, it->p0[AX_COL], it->p1[AX_COL]);
        float ah = axis_span_size(&rows, it->p0[AX_ROW], it->p1[AX_ROW]);
        int fl = final_layout_kind(b, it);
        if (fl) layout_item(t, b, it->box, aw, ah, fl == 1);
        if (it->baseline) item_update_asc(it);
    }
    baseline_groups(&g, row_asc);

    /* alignment */
    GridItem *first = NULL, *first_bl = NULL;
    for (int k = 0; k < g.n; k++) {
        GridItem *it = &g.items[k];
        Box *ib = it->box;
        int r0 = it->p0[AX_ROW], c0 = it->p0[AX_COL];
        float ax = cols.pos[c0], aw = axis_span_size(&cols, c0, it->p1[AX_COL]);
        float ay = rows.pos[r0], ah = axis_span_size(&rows, r0, it->p1[AX_ROW]);
        const CssComputed *cs = ib->style;
        float ml = non_auto_margin(ib, CSS_LEFT), mr = non_auto_margin(ib, CSS_RIGHT);
        float mt = non_auto_margin(ib, CSS_TOP), mb = non_auto_margin(ib, CSS_BOTTOM);
        int jl = cs->margin[CSS_LEFT].kind == CSS_LEN_AUTO, jr = cs->margin[CSS_RIGHT].kind == CSS_LEN_AUTO;
        int at = cs->margin[CSS_TOP].kind == CSS_LEN_AUTO, ab = cs->margin[CSS_BOTTOM].kind == CSS_LEN_AUTO;
        float am0 = 0, am1 = 0, bm0 = 0, bm1 = 0;
        float dx = align_offset(aw, ib->w + ml + mr, jl, jr, item_justify(b, ib), &am0, &am1);
        float dy = it->baseline && !at && !ab ? row_asc[r0] - it->asc
                 : align_offset(ah, ib->h + mt + mb, at, ab, item_align(b, ib), &bm0, &bm1);
        /* used auto margins */
        ib->margin[CSS_LEFT] = jl ? am0 : ml;  ib->margin[CSS_RIGHT] = jr ? am1 : mr;
        ib->margin[CSS_TOP] = at ? bm0 : mt;   ib->margin[CSS_BOTTOM] = ab ? bm1 : mb;
        ib->lx = cx + ax + (jl ? dx : dx + ml);
        ib->ly = cy + ay + (at ? dy : dy + mt);
        layout_relative_offset(ib, aw, ah);
        if (r0 == 0) {
            if (!first || c0 < first->p0[AX_COL]) first = it;
            if (ib->first_baseline >= 0 && (!first_bl || c0 < first_bl->p0[AX_COL])) first_bl = it;
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
    free(row_asc);
    grid_free(&g);
}

void layout_grid_intrinsic(LayoutTree *t, Box *b, float *min_w, float *max_w)
{
    const CssComputed *s = b->style;
    *min_w = *max_w = 0;
    Grid g;
    if (!grid_place(b, &g, -1, -1)) { grid_free(&g); return; }
    if (!g.n) g.ntracks[AX_COL] = g.off[AX_COL] + g.lines[AX_COL].n_explicit;
    Axis cols;
    uint8_t *need = calloc((size_t)(g.n ? g.n : 1), 1);
    if (!need || !axis_init(&cols, &g, AX_COL, gap_px(&s->column_gap, -1))) {
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
