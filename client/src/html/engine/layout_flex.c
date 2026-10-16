/*
 * layout_flex.c - flexbox subset used by the Krunker UI (inventory section 6):
 * row/column (+reverse), nowrap/wrap, justify-content (start/end/center/
 * space-between/space-around/space-evenly), align-items (stretch/start/end/
 * center; baseline treated as start), flex-grow/flex-shrink/flex-basis,
 * automatic minimum size (min-content), auto margins on the main axis,
 * align-content (normal/stretch, start, end, center, space-*) for wrapped lines
 * with a definite cross size,
 * align-self overriding align-items per item, row-gap / column-gap (the
 * main-axis gap between items of a line, the cross-axis gap between lines).
 */
#include "layout_internal.h"

typedef struct FItem {
    Box *b;
    float base, hypo;          /* border-box main sizes                       */
    float inner_base;          /* content-box flex base size (shrink weight)  */
    float min_main, max_main;  /* border-box                                  */
    float main;                /* final border-box main size                  */
    float cross;               /* border-box cross size                       */
    float mm0, mm1;            /* main-axis margins (start, end)              */
    float cm0, cm1;            /* cross-axis margins                          */
    uint8_t auto_m0, auto_m1;  /* auto main margins                           */
    uint8_t auto_c0, auto_c1;
    uint8_t frozen;
    float target;
    float pos_main, pos_cross;
} FItem;

typedef struct FLine { int first, count; float cross; float pos; } FLine;

/* display: -webkit-box: Blink's flexbox emulation takes the direction from -webkit-box-orient
 * (flex-direction does not apply; -webkit-box-direction is not supported: normal) */
static int is_row(const Box *b)
{
    if (b->style->display_legacy_box) return b->style->box_orient != CSS_BOX_ORIENT_VERTICAL;
    return b->style->flex_direction == CSS_FLEX_DIRECTION_ROW || b->style->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE;
}
static int is_reverse(const Box *b)
{
    if (b->style->display_legacy_box) return 0;
    return b->style->flex_direction == CSS_FLEX_DIRECTION_ROW_REVERSE || b->style->flex_direction == CSS_FLEX_DIRECTION_COLUMN_REVERSE;
}

static int count_items(const Box *b)
{
    int n = 0;
    for (Box *c = b->first_child; c; c = c->next) if (!(c->flags & BOXF_OUT_OF_FLOW)) n++;
    return n;
}

/* Lay the item out with the given border-box sizes (-1: auto). */
static void item_layout(LayoutTree *t, Box *c, float cw, float ch_def, float w, float h, int shrink, float avail, int measure,
                        int definite)
{
    LayoutInput in;
    memset(&in, 0, sizeof(in));
    in.cb_w = cw; in.cb_h = ch_def; in.avail_w = avail;
    in.override_w = w; in.override_h = h;
    in.definite_h = h >= 0 && definite;
    in.shrink_to_fit = shrink;
    in.is_flex_item = 1;
    in.measure_only = measure;
    LayoutOutput o;
    layout_box(t, c, &in, &o);
}

/* The item's used cross-axis alignment: align-self, or the container's align-items when auto. */
static int item_align(const CssComputed *s, const CssComputed *cs)
{
    return cs->align_self != CSS_ALIGN_ITEMS_AUTO ? cs->align_self : s->align_items;
}

static float clampf_mm(float v, float mn, float mx) { if (v > mx) v = mx; if (v < mn) v = mn; return v; }

/* row-gap / column-gap in px ("normal" is 0 in flex layout; a percentage needs a definite base). */
static float flex_gap(const CssLength *l, float base) { return kn_maxf(0, layout_resolve_len(l, base, 0)); }

void layout_flex_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h)
{
    const CssComputed *s = b->style;
    int row = is_row(b), rev = is_reverse(b);
    int wrap = s->flex_wrap != CSS_FLEX_WRAP_NOWRAP;
    float cx = box_cl(b), cy = box_ct(b);
    float cw = box_cw(b);
    float ch = in->cb_h;                      /* own content height, or -1 */
    int n = count_items(b);

    /* static position of abs children: as if each were the sole flex item
     * (CSS Flexbox 4.1), aligned by justify-content / align-items once sized */
    for (Box *c = b->first_child; c; c = c->next) {
        if (!(c->flags & BOXF_OUT_OF_FLOW)) continue;
        float fj, fa;
        switch (s->justify_content) {
        case CSS_JUSTIFY_CONTENT_FLEX_END: fj = 1; break;
        case CSS_JUSTIFY_CONTENT_CENTER: case CSS_JUSTIFY_CONTENT_SPACE_AROUND: case CSS_JUSTIFY_CONTENT_SPACE_EVENLY: fj = 0.5f; break;
        default: fj = 0; break;
        }
        if (rev) fj = 1 - fj;
        switch (item_align(s, c->style)) {
        case CSS_ALIGN_ITEMS_FLEX_END: fa = 1; break;
        case CSS_ALIGN_ITEMS_CENTER: fa = 0.5f; break;
        default: fa = 0; break;
        }
        if (s->flex_wrap == CSS_FLEX_WRAP_WRAP_REVERSE) fa = 1 - fa;
        c->static_x = cx; c->static_y = cy;
        c->static_fx = row ? fj : fa;
        c->static_fy = row ? fa : fj;
        c->static_aw = cw;
        c->static_ah = -1;   /* filled below once the content height is known */
    }
    if (!n) {
        *content_h = 0; b->baseline_offset = -1;
        for (Box *c = b->first_child; c; c = c->next) if (c->flags & BOXF_OUT_OF_FLOW) c->static_ah = ch >= 0 ? ch : 0;
        return;
    }

    FItem *items = calloc((size_t)n, sizeof(FItem));
    FLine *lines = calloc((size_t)n, sizeof(FLine));
    if (!items || !lines) { free(items); free(lines); *content_h = 0; return; }
    int k = 0;
    float main_avail = row ? cw : ch;         /* -1 when indefinite (column) */
    const float gap_main = flex_gap(row ? &s->column_gap : &s->row_gap, main_avail);
    const float gap_cross = flex_gap(row ? &s->row_gap : &s->column_gap, row ? ch : cw);

    for (Box *c = b->first_child; c; c = c->next) {
        if (c->flags & BOXF_OUT_OF_FLOW) continue;
        FItem *it = &items[k++];
        it->b = c;
        const CssComputed *cs = c->style;
        layout_resolve_edges(t, c, cw);
        int m0 = row ? CSS_LEFT : CSS_TOP, m1 = row ? CSS_RIGHT : CSS_BOTTOM;
        int c0 = row ? CSS_TOP : CSS_LEFT, c1 = row ? CSS_BOTTOM : CSS_RIGHT;
        it->auto_m0 = cs->margin[m0].kind == CSS_LEN_AUTO; it->auto_m1 = cs->margin[m1].kind == CSS_LEN_AUTO;
        it->auto_c0 = cs->margin[c0].kind == CSS_LEN_AUTO; it->auto_c1 = cs->margin[c1].kind == CSS_LEN_AUTO;
        it->mm0 = it->auto_m0 ? 0 : c->margin[m0]; it->mm1 = it->auto_m1 ? 0 : c->margin[m1];
        it->cm0 = it->auto_c0 ? 0 : c->margin[c0]; it->cm1 = it->auto_c1 ? 0 : c->margin[c1];

        /* content-based size (row: max-content width; column: laid-out height) */
        float content_main;
        float content_min;
        if (row && box_is_replaced(c) && ch >= 0 && cs->height.kind == CSS_LEN_VALUE && (cs->height.pct != 0.0f || cs->height.pct_term)) {
            /* a percentage height against the container's definite height: the width follows
             * through the ratio (Strike's Customize box: an <img height:100%> in a flex box clamped
             * to 54 base px by max-height is 54 x 54, not as wide as the box) */
            float rw, rh;
            layout_replaced_size(t, c, cw, ch, &rw, &rh);
            content_main = content_min = rw;
        } else if (row) {
            content_main = layout_intrinsic_width(t, c, 1);
            /* the content size suggestion of the automatic minimum size (CSS Flexbox 4.5) is the
             * min-content size of the item's contents: its own width (layout_intrinsic_width) would
             * make a width: Npx item refuse to shrink below N (a Strike friend row's Join button
             * pushed off the card); a replaced item's is its natural size */
            content_min = box_is_replaced(c) ? layout_intrinsic_width(t, c, 0)
                                             : layout_content_kw_width(t, c, CSS_LEN_MIN_CONTENT, -1.0f);
        } else {
            int stretch = item_align(s, cs) == CSS_ALIGN_ITEMS_STRETCH && cs->width.kind == CSS_LEN_AUTO && !it->auto_c0 && !it->auto_c1;
            float ow = stretch ? kn_maxf(0, cw - it->cm0 - it->cm1) : -1;
            item_layout(t, c, cw, LAYOUT_INDEFINITE, ow, -1, !stretch, cw, 1, 1); /* the cross pass lays it out again */
            content_main = c->h;
            /* the content size suggestion: the height its contents give it, not its own height */
            content_min = box_is_replaced(c) ? c->h : c->content_auto_h;
        }
        /* flex base size */
        const CssLength *basis = &cs->flex_basis;
        const CssLength *msize = row ? &cs->width : &cs->height;
        int have = 0;
        float base = 0;
        /* a percentage basis (also flex: 1's 0%) in an indefinite container is content-sized */
        if (basis->kind == CSS_LEN_VALUE && !((basis->pct != 0 || basis->pct_term) && main_avail < 0)) {
            float v = layout_resolve_len(basis, main_avail, 0);
            base = row ? layout_box_sizing_w(c, v) : layout_box_sizing_h(c, v);
            have = 1;
        } else if (msize->kind == CSS_LEN_VALUE && !(msize->pct != 0 && main_avail < 0) && !box_is_replaced(c)) {
            float v = layout_resolve_len(msize, main_avail, 0);
            base = row ? layout_box_sizing_w(c, v) : layout_box_sizing_h(c, v);
            have = 1;
        }
        if (!have) base = content_main;
        it->base = base;
        it->inner_base = kn_maxf(0.0f, base - (row ? box_border_padding_w(c) : box_border_padding_h(c)));
        /* min / max (border box) */
        const CssLength *mn = row ? &cs->min_width : &cs->min_height;
        const CssLength *mx = row ? &cs->max_width : &cs->max_height;
        if (mn->kind == CSS_LEN_VALUE) {
            float v = layout_resolve_len(mn, main_avail < 0 ? 0 : main_avail, 0);
            it->min_main = row ? layout_box_sizing_w(c, v) : layout_box_sizing_h(c, v);
        } else if (!(c->flags & BOXF_SCROLL_CONTAINER)) {
            /* automatic minimum size: content size suggestion, capped by a definite specified size */
            float sugg = content_min;
            if (msize->kind == CSS_LEN_VALUE && !(msize->pct != 0 && main_avail < 0)) {
                float v = layout_resolve_len(msize, main_avail, 0);
                float spec = row ? layout_box_sizing_w(c, v) : layout_box_sizing_h(c, v);
                if (spec < sugg) sugg = spec;
            }
            if (mx->kind == CSS_LEN_VALUE) {
                float v = layout_resolve_len(mx, main_avail < 0 ? 0 : main_avail, LAYOUT_INF);
                float cap = row ? layout_box_sizing_w(c, v) : layout_box_sizing_h(c, v);
                if (cap < sugg) sugg = cap;
            }
            it->min_main = sugg;
        } else {
            it->min_main = row ? box_border_padding_w(c) : box_border_padding_h(c);
        }
        it->max_main = LAYOUT_INF;
        if (mx->kind == CSS_LEN_VALUE && !(mx->pct != 0 && main_avail < 0)) {
            float v = layout_resolve_len(mx, main_avail, LAYOUT_INF);
            it->max_main = row ? layout_box_sizing_w(c, v) : layout_box_sizing_h(c, v);
        }
        /* content keywords of min-width / max-width (row containers; a column's are height) */
        if (row && (layout_len_is_content_kw(mx) || layout_len_is_content_kw(mn))) {
            float fit_avail = main_avail < 0 ? -1.0f : kn_maxf(0.0f, main_avail - it->mm0 - it->mm1);
            if (layout_len_is_content_kw(mx)) it->max_main = layout_content_kw_width(t, c, mx->kind, fit_avail);
            if (layout_len_is_content_kw(mn)) it->min_main = layout_content_kw_width(t, c, mn->kind, fit_avail);
        }
        it->hypo = clampf_mm(base, it->min_main, it->max_main);
    }

    /* ---- lines ---- */
    int nl = 0;
    {
        int i = 0;
        while (i < n) {
            FLine *ln = &lines[nl++];
            ln->first = i; ln->count = 0;
            float used = 0;
            while (i < n) {
                FItem *it = &items[i];
                float outer = it->hypo + it->mm0 + it->mm1 + (ln->count ? gap_main : 0);
                if (wrap && ln->count > 0 && main_avail >= 0 && used + outer > main_avail + 0.001f) break;
                used += outer; ln->count++; i++;
            }
        }
    }

    /* ---- resolve flexible lengths ---- */
    float container_main = main_avail;
    if (container_main < 0) {
        /* indefinite column: content-sized, min/max-height of the container clamp later */
        float mx = 0;
        for (int l = 0; l < nl; l++) {
            float sum = gap_main * (float)(lines[l].count - 1);
            for (int j = 0; j < lines[l].count; j++) { FItem *it = &items[lines[l].first + j]; sum += it->hypo + it->mm0 + it->mm1; }
            if (sum > mx) mx = sum;
        }
        /* max-height of the container gives a definite upper bound for shrinking (css-flexbox
           9.2 step 4: the auto main size clamped by min/max), a percentage one against the
           container's own containing block like layout_box applies it (#menuWindow's
           max-height: calc(100% - 230px) over the Object Manager's flex column) */
        float bh = mx + layout_vertical_extras(b);
        layout_apply_min_max_h(b, in->self_cb_h, &bh);
        container_main = kn_maxf(0, bh - layout_vertical_extras(b));
    }
    for (int l = 0; l < nl; l++) {
        FLine *ln = &lines[l];
        const float line_gaps = gap_main * (float)(ln->count - 1);
        float sum_hypo = line_gaps;
        for (int j = 0; j < ln->count; j++) { FItem *it = &items[ln->first + j]; sum_hypo += it->hypo + it->mm0 + it->mm1; }
        int grow = sum_hypo < container_main;
        for (int j = 0; j < ln->count; j++) {
            FItem *it = &items[ln->first + j];
            const CssComputed *cs = it->b->style;
            it->target = it->base;
            it->frozen = 0;
            float f = grow ? cs->flex_grow : cs->flex_shrink;
            if (f == 0 || (grow && it->base > it->hypo) || (!grow && it->base < it->hypo)) { it->frozen = 1; it->target = it->hypo; }
        }
        float init_free = 0;
        for (int iter = 0; iter < 16; iter++) {
            float used = line_gaps, sum_f = 0, sum_scaled = 0;
            int unfrozen = 0;
            for (int j = 0; j < ln->count; j++) {
                FItem *it = &items[ln->first + j];
                if (it->frozen) used += it->target + it->mm0 + it->mm1;
                else {
                    used += it->base + it->mm0 + it->mm1;
                    unfrozen++;
                    sum_f += grow ? it->b->style->flex_grow : it->b->style->flex_shrink;
                    sum_scaled += it->b->style->flex_shrink * it->inner_base;
                }
            }
            if (!unfrozen) break;
            float free_sp = container_main - used;
            if (iter == 0) init_free = free_sp;
            /* CSS Flexbox 9.7 step 4b: unfrozen flex factors summing below 1 take only that share of
             * the initial free space (growing and shrinking alike) */
            if (sum_f < 1.0f && fabsf(sum_f * init_free) < fabsf(free_sp)) free_sp = sum_f * init_free;
            float total_violation = 0;
            for (int j = 0; j < ln->count; j++) {
                FItem *it = &items[ln->first + j];
                if (it->frozen) continue;
                float v;
                if (grow) v = it->base + (sum_f > 0 ? free_sp * it->b->style->flex_grow / sum_f : 0);
                else v = it->base + (sum_scaled > 0 ? free_sp * (it->b->style->flex_shrink * it->inner_base) / sum_scaled : 0);
                float cl = clampf_mm(v, it->min_main, it->max_main);
                total_violation += cl - v;
                it->target = cl;
            }
            int any = 0;
            for (int j = 0; j < ln->count; j++) {
                FItem *it = &items[ln->first + j];
                if (it->frozen) continue;
                float v_unclamped;
                if (grow) v_unclamped = it->base + (sum_f > 0 ? free_sp * it->b->style->flex_grow / sum_f : 0);
                else v_unclamped = it->base + (sum_scaled > 0 ? free_sp * (it->b->style->flex_shrink * it->inner_base) / sum_scaled : 0);
                float d = it->target - v_unclamped;
                if (fabsf(total_violation) < 0.001f || (total_violation > 0 && d > 0) || (total_violation < 0 && d < 0)) { it->frozen = 1; any = 1; }
            }
            if (fabsf(total_violation) < 0.001f || !any) {
                for (int j = 0; j < ln->count; j++) items[ln->first + j].frozen = 1;
                break;
            }
        }
        for (int j = 0; j < ln->count; j++) { FItem *it = &items[ln->first + j]; it->main = it->target; }
    }

    /* ---- cross sizes ---- */
    float cross_avail = row ? ch : cw;           /* -1: indefinite (row with auto height) */
    for (int i = 0; i < n; i++) {
        FItem *it = &items[i];
        Box *c = it->b;
        const CssComputed *cs = c->style;
        if (row) {
            /* measured only when the stretch pass below lays it out again (same condition) */
            const int again = item_align(s, cs) == CSS_ALIGN_ITEMS_STRETCH && cs->height.kind == CSS_LEN_AUTO && !it->auto_c0 && !it->auto_c1;
            item_layout(t, c, cw, ch, it->main, -1, 0, cw, again, 1);
            it->cross = c->h;
        } else {
            int stretch = item_align(s, cs) == CSS_ALIGN_ITEMS_STRETCH && cs->width.kind == CSS_LEN_AUTO && !it->auto_c0 && !it->auto_c1;
            float ow = stretch ? kn_maxf(box_border_padding_w(c), cw - it->cm0 - it->cm1) : -1;
            /* the flexed height is definite only in a container whose own height is (CSS Flexbox
             * 9.8.4): a column capped by max-height alone leaves its items' percentages auto. A
             * nested flex / grid container still lays its own items out in the flexed height
             * (the Object Manager's .om column under #menuWindow's max-height) */
            item_layout(t, c, cw, ch, ow, it->main, !stretch, cw, 0,
                        main_avail >= 0 || c->type == BOX_FLEX || c->type == BOX_GRID);
            it->cross = c->w;
        }
    }
    float total_cross = 0;
    for (int l = 0; l < nl; l++) {
        FLine *ln = &lines[l];
        float mx = 0;
        for (int j = 0; j < ln->count; j++) { FItem *it = &items[ln->first + j]; mx = kn_maxf(mx, it->cross + it->cm0 + it->cm1); }
        ln->cross = mx;
        total_cross += mx + (l ? gap_cross : 0);
    }
    float lines_lead = 0, lines_between = 0;      /* align-content of a multi-line container */
    if (nl == 1 && cross_avail >= 0) { total_cross = cross_avail; lines[0].cross = cross_avail; }
    else if (nl == 1 && row) {
        /* single-line with an indefinite cross size: the line's cross size is clamped to the
           container's min / max cross size (css-flexbox 9.4 step 8), so align-items centers /
           ends / stretches inside a min-height like Chrome (v10's 45px .tutorialNavItem) */
        float bh = total_cross + box_border_padding_h(b);
        layout_apply_min_max_h(b, -1, &bh);
        total_cross = lines[0].cross = kn_maxf(0, bh - box_border_padding_h(b));
    }
    else if (cross_avail >= 0 && total_cross < cross_avail && nl > 1) {
        const float free_c = cross_avail - total_cross;
        switch (s->align_content) {
        case CSS_JUSTIFY_CONTENT_FLEX_START: break;
        case CSS_JUSTIFY_CONTENT_FLEX_END: lines_lead = free_c; break;
        case CSS_JUSTIFY_CONTENT_CENTER: lines_lead = free_c * 0.5f; break;
        case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN: lines_between = free_c / (float)(nl - 1); break;
        case CSS_JUSTIFY_CONTENT_SPACE_AROUND: lines_between = free_c / (float)nl; lines_lead = lines_between * 0.5f; break;
        case CSS_JUSTIFY_CONTENT_SPACE_EVENLY: lines_between = lines_lead = free_c / (float)(nl + 1); break;
        default: { /* normal (stretch): the lines share the free space; gaps stay */
            float extra = free_c / (float)nl;
            for (int l = 0; l < nl; l++) lines[l].cross += extra;
        } break;
        }
        total_cross = cross_avail;
    }
    /* stretch (row: heights) */
    if (row) {
        for (int l = 0; l < nl; l++) {
            FLine *ln = &lines[l];
            for (int j = 0; j < ln->count; j++) {
                FItem *it = &items[ln->first + j];
                const CssComputed *cs = it->b->style;
                if (item_align(s, cs) == CSS_ALIGN_ITEMS_STRETCH && cs->height.kind == CSS_LEN_AUTO && !it->auto_c0 && !it->auto_c1) {
                    float hh = kn_maxf(0, ln->cross - it->cm0 - it->cm1);
                    layout_apply_min_max_h(it->b, ch, &hh);
                    if (fabsf(hh - it->cross) > 0.001f || 1) {
                        item_layout(t, it->b, cw, ch, it->main, hh, 0, cw, 0, 1);
                        it->cross = it->b->h;
                    }
                }
            }
        }
    }

    /* ---- main axis positions ---- */
    for (int l = 0; l < nl; l++) {
        FLine *ln = &lines[l];
        float used = gap_main * (float)(ln->count - 1); int autos = 0;
        for (int j = 0; j < ln->count; j++) {
            FItem *it = &items[ln->first + j];
            used += it->main + it->mm0 + it->mm1;
            autos += it->auto_m0 + it->auto_m1;
        }
        float free_sp = container_main - used;
        float lead = 0, between = 0;
        if (free_sp > 0 && autos) {
            float each = free_sp / (float)autos;
            for (int j = 0; j < ln->count; j++) {
                FItem *it = &items[ln->first + j];
                if (it->auto_m0) it->mm0 = each;
                if (it->auto_m1) it->mm1 = each;
            }
            free_sp = 0;
        }
        int jc = s->justify_content;
        int cnt = ln->count;
        switch (jc) {
        case CSS_JUSTIFY_CONTENT_FLEX_END: lead = free_sp; break;
        case CSS_JUSTIFY_CONTENT_CENTER: lead = free_sp * 0.5f; break;
        case CSS_JUSTIFY_CONTENT_SPACE_BETWEEN:
            if (cnt > 1 && free_sp > 0) between = free_sp / (float)(cnt - 1);
            else if (free_sp < 0) lead = 0;
            break;
        case CSS_JUSTIFY_CONTENT_SPACE_AROUND:
            if (free_sp > 0) { between = free_sp / (float)cnt; lead = between * 0.5f; }
            else lead = free_sp * 0.5f;
            break;
        case CSS_JUSTIFY_CONTENT_SPACE_EVENLY:
            if (free_sp > 0) { between = free_sp / (float)(cnt + 1); lead = between; }
            else lead = free_sp * 0.5f;
            break;
        default: break;
        }
        float pos = lead;
        for (int j = 0; j < cnt; j++) {
            FItem *it = &items[ln->first + j];
            pos += it->mm0;
            it->pos_main = rev ? container_main - pos - it->main : pos;
            pos += it->main + it->mm1 + between + gap_main;
        }
    }

    /* ---- cross axis positions ---- */
    float cpos = lines_lead;
    for (int l = 0; l < nl; l++) {
        FLine *ln = &lines[l];
        ln->pos = cpos;
        for (int j = 0; j < ln->count; j++) {
            FItem *it = &items[ln->first + j];
            float space = ln->cross - (it->cross + it->cm0 + it->cm1);
            float off = 0;
            if (it->auto_c0 && it->auto_c1) off = space * 0.5f;
            else if (it->auto_c0) off = space;
            else if (!it->auto_c1) {
                switch (item_align(s, it->b->style)) {
                case CSS_ALIGN_ITEMS_FLEX_END: off = space; break;
                case CSS_ALIGN_ITEMS_CENTER: off = space * 0.5f; break;
                default: off = 0; break;
                }
            }
            it->pos_cross = cpos + off + it->cm0;
        }
        cpos += ln->cross + gap_cross + lines_between;
    }
    if (s->flex_wrap == CSS_FLEX_WRAP_WRAP_REVERSE) {
        for (int i = 0; i < n; i++) items[i].pos_cross = total_cross - items[i].pos_cross - items[i].cross;
    }

    /* ---- place ---- */
    b->baseline_offset = -1; b->first_baseline = -1;
    for (int i = 0; i < n; i++) {
        FItem *it = &items[i];
        Box *c = it->b;
        if (row) { c->lx = cx + it->pos_main; c->ly = cy + it->pos_cross; }
        else { c->lx = cx + it->pos_cross; c->ly = cy + it->pos_main; }
        if (row) { c->margin[CSS_LEFT] = it->mm0; c->margin[CSS_RIGHT] = it->mm1; }
        else { c->margin[CSS_TOP] = it->mm0; c->margin[CSS_BOTTOM] = it->mm1; }
        layout_relative_offset(c, cw, ch);
        if (b->first_baseline < 0) {
            float bl = c->first_baseline >= 0 ? c->first_baseline : c->baseline_offset;
            if (bl >= 0 && !(c->flags & BOXF_SCROLL_CONTAINER)) b->first_baseline = c->ly - c->rel_y + bl;
        }
    }
    if (b->first_baseline < 0 && n) {
        /* synthesized from the first item's border-box bottom */
        b->first_baseline = items[0].b->ly + items[0].b->h;
    }
    b->baseline_offset = b->first_baseline;   /* flex containers export their first baseline */

    *content_h = row ? total_cross : (main_avail >= 0 ? main_avail : container_main);
    if (!row && main_avail < 0) {
        float used = 0;
        for (int l = 0; l < nl; l++) {
            float sum = gap_main * (float)(lines[l].count - 1);
            for (int j = 0; j < lines[l].count; j++) { FItem *it = &items[lines[l].first + j]; sum += it->main + it->mm0 + it->mm1; }
            used = kn_maxf(used, sum);
        }
        *content_h = used;
    }
    for (Box *c = b->first_child; c; c = c->next)
        if ((c->flags & BOXF_OUT_OF_FLOW) && c->static_ah < 0) c->static_ah = ch >= 0 ? ch : *content_h;
    free(items);
    free(lines);
}

void layout_flex_intrinsic(LayoutTree *t, Box *b, float *min_w, float *max_w)
{
    int row = is_row(b);
    int wrap = b->style->flex_wrap != CSS_FLEX_WRAP_NOWRAP;
    float mn = 0, mx = 0;
    const float gap = row ? flex_gap(&b->style->column_gap, -1) : 0;
    int items = 0;
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->flags & BOXF_OUT_OF_FLOW) continue;
        if (row && items++) { mx += gap; if (!wrap) mn += gap; }
        float cmin = layout_intrinsic_margin_width(t, c, 0, 0.0f);
        float cmax = layout_intrinsic_margin_width(t, c, 1, 0.0f);
        if (row) {
            /* a fixed flex-basis contributes like a width */
            const CssLength *basis = &c->style->flex_basis;
            if (basis->kind == CSS_LEN_VALUE && basis->pct == 0) {
                float m = cmax - layout_intrinsic_width(t, c, 1);
                float bw = layout_box_sizing_w(c, basis->px) + m;
                if (c->style->flex_grow == 0) cmax = kn_maxf(cmax, bw); else cmax = kn_maxf(bw, cmax);
                if (c->style->flex_shrink == 0) cmin = kn_maxf(cmin, bw);
            }
            mx += cmax;
            if (wrap) mn = kn_maxf(mn, cmin); else mn += cmin;
        } else {
            mx = kn_maxf(mx, cmax);
            mn = kn_maxf(mn, cmin);
        }
    }
    *min_w = mn;
    *max_w = kn_maxf(mx, mn);
}
