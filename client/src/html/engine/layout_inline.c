/*
 * layout_inline.c - inline formatting contexts: line boxes, soft/forced
 * breaking (UAX#14 subset from text.c), white-space collapsing across boxes,
 * inline-block/replaced atomic items, vertical-align, line-height
 * (px/number/normal), floats and absolute static positions met in inline
 * flow, text-align, text-overflow: ellipsis, inline box fragments and the
 * min-/max-content widths of inline content.
 */
#include "layout_internal.h"

/* ------------------------------------------------------------------------- */
/* measuring                                                                  */
/* ------------------------------------------------------------------------- */

float layout_text_width(LayoutTree *t, TextFont *font, const char *s, size_t len, float letter_spacing)
{
    if (!len) return 0;
    if (!font) {
        if (t->ctx->text) return 0;    /* font-size: 0 */
        /* no text system: 0.6em per code point */
        size_t n = 0;
        for (size_t i = 0; i < len; i++) if (((unsigned char)s[i] & 0xC0) != 0x80) n++;
        return (float)n * (letter_spacing + 9.6f);
    }
    return text_measure(font, s, len, letter_spacing);
}

/* Blink keeps the computed line height in LayoutUnits (1/64 px): a number is stored as a
 * percentage (StyleBuilderConverter::ConvertLineHeight) and resolved against
 * LayoutUnit(font-size) by ValueForLength, whose LayoutUnit(float) truncates; a length goes
 * through LayoutUnit::FromFloatRound.  17px * 1.6 is then 27.1875, not 27.2, and the 1/64 lost
 * per line shows as a 1 px baseline step after a few lines (v10's k-ID description). */
static float lu_floor(const float px) { return floorf(px * 64.0f) / 64.0f; }
static float lu_round(const float px) { return roundf(px * 64.0f) / 64.0f; }

float layout_line_height(const Box *b)
{
    const CssLength *lh = &b->style->line_height;
    const float fs = lu_floor(b->style->font_size);
    switch (lh->kind) {
    case CSS_LEN_NORMAL: return b->fm.line_height > 0 ? b->fm.line_height : b->fm.ascent + b->fm.descent;
    case CSS_LEN_NUMBER: {
        /* Blink's float steps: the percentage first (1.2 -> 120 exactly), so 20px * 1.2 stays 24 */
        const float pct = lh->px * 100.0f;
        return lu_floor(fs * pct / 100.0f);
    }
    case CSS_LEN_VALUE: return lh->pct != 0.0f ? lu_floor(lh->px + lh->pct * 0.01f * fs) : lu_round(lh->px);
    default: return b->fm.line_height;
    }
}

static float letter_spacing_of(const Box *b)
{
    const CssLength *ls = &b->style->letter_spacing;
    return ls->kind == CSS_LEN_VALUE ? ls->px : 0.0f;
}

/* Baseline of an atomic box from its border-box top: last line box baseline
 * for inline-blocks with in-flow lines and visible overflow; otherwise none. */
float layout_box_baseline(const Box *b)
{
    if (b->flags & BOXF_TEXT_CONTROL) {
        /* Chrome: a text control's baseline is its inner editor's text
         * baseline (the line box centred in the content box), the same
         * position widgets.c paints the text at. */
        const float top = b->border[CSS_TOP] + b->padding[CSS_TOP];
        const float ch = b->h - b->border[CSS_TOP] - b->border[CSS_BOTTOM] - b->padding[CSS_TOP] - b->padding[CSS_BOTTOM];
        const float lh = layout_line_height(b);
        return top + (ch - lh) / 2 + (lh - (b->fm.ascent + b->fm.descent)) / 2 + b->fm.ascent;
    }
    if (box_is_replaced(b)) return -1.0f;
    if (b->flags & BOXF_SCROLL_CONTAINER) return -1.0f;
    if (b->baseline_offset >= 0) return b->baseline_offset;
    return -1.0f;
}

/* ------------------------------------------------------------------------- */
/* flattened inline nodes                                                     */
/* ------------------------------------------------------------------------- */

typedef enum { IN_OPEN = 0, IN_CLOSE, IN_TEXT, IN_ATOMIC, IN_BR, IN_FLOAT, IN_ABS } InKind;

typedef struct InNode {
    uint8_t kind;
    uint8_t ends_space;      /* TEXT: last byte is a collapsible space        */
    uint8_t starts_space;    /* TEXT: first byte is a collapsible space       */
    uint8_t breakable_after; /* TEXT: soft break allowed after this piece     */
    Box *box;
    uint32_t start, end;     /* TEXT: byte range in box->text                  */
    float w;                 /* TEXT: width incl. trailing space; ATOMIC: margin box */
    float w_trim;            /* TEXT: width without trailing spaces           */
} InNode;

typedef struct InNodeVec { InNode *data; size_t len, cap; } InNodeVec;

typedef struct Flattener {
    LayoutTree *t;
    InNodeVec nodes;
    const LayoutInput *in;
    int nowrap_default;
    int for_intrinsic;
} Flattener;

static int is_space_byte(char c) { return c == ' '; }

static void push_node(Flattener *f, InNode n) { KN_VEC_PUSH(&f->nodes, n); }

static void flatten_text(Flattener *f, Box *tb)
{
    if (!tb->text_len) return;
    Box *p = tb->parent;
    tb->font = p->font; tb->fm = p->fm; tb->style = p->style;
    float ls = letter_spacing_of(p);
    const CssComputed *s = p->style;
    int nowrap = s->white_space == CSS_WHITE_SPACE_NOWRAP || s->white_space == CSS_WHITE_SPACE_PRE;
    /* white-space: pre / pre-wrap preserve every space (css-text-3 white-space-collapse: preserve): a
     * run's leading and trailing spaces are not collapsible, so a line start or the previous run's
     * space keeps them and they count in the run's width (FRVR's victory title is "#1" + " VICTORY" in
     * two pre divs: the space between them is the second div's). */
    int preserve = s->white_space == CSS_WHITE_SPACE_PRE || s->white_space == CSS_WHITE_SPACE_PRE_WRAP;
    int break_all = s->word_break == CSS_WORD_BREAK_BREAK_ALL;
    uint8_t *flags = malloc(tb->text_len + 1);
    if (!flags) return;
    memset(flags, 0, tb->text_len + 1);
    text_break_opportunities(tb->text, tb->text_len, flags);
    uint32_t start = 0;
    for (uint32_t i = 1; i <= tb->text_len; i++) {
        int boundary = (i == tb->text_len);
        if (!boundary && !nowrap) {
            if (flags[i] & TEXT_BREAK_ALLOWED) boundary = 1;
            else if (break_all && ((unsigned char)tb->text[i] & 0xC0) != 0x80
                     && !is_space_byte(tb->text[i]) && !is_space_byte(tb->text[i - 1])) boundary = 1;
        }
        if (s->white_space == CSS_WHITE_SPACE_PRE_LINE && !boundary && tb->text[i] == '\n') boundary = 1;
        if (!boundary) continue;
        InNode n;
        memset(&n, 0, sizeof(n));
        n.kind = IN_TEXT; n.box = tb; n.start = start; n.end = i;
        n.starts_space = !preserve && is_space_byte(tb->text[start]);
        uint32_t e = i;
        if (!preserve) while (e > start && is_space_byte(tb->text[e - 1])) e--;
        n.ends_space = (e < i);
        n.breakable_after = !nowrap && (i < tb->text_len);
        n.w = layout_text_width(f->t, tb->font, tb->text + start, i - start, ls);
        n.w_trim = (e == i) ? n.w : layout_text_width(f->t, tb->font, tb->text + start, e - start, ls);
        push_node(f, n);
        start = i;
    }
    free(flags);
}

static void flatten_box(Flattener *f, Box *b);

static void flatten_children(Flattener *f, Box *b)
{
    for (Box *c = b->first_child; c; c = c->next) flatten_box(f, c);
}

static void flatten_box(Flattener *f, Box *c)
{
    InNode n;
    memset(&n, 0, sizeof(n));
    n.box = c;
    if (c->flags & BOXF_OUT_OF_FLOW) { n.kind = IN_ABS; push_node(f, n); return; }
    if (c->flags & BOXF_FLOAT) { n.kind = IN_FLOAT; push_node(f, n); return; }
    switch (c->type) {
    case BOX_TEXT: flatten_text(f, c); return;
    case BOX_LINEBREAK: n.kind = IN_BR; push_node(f, n); return;
    case BOX_INLINE:
        if (c->dirty & BOX_DIRTY_SELF || !c->font) layout_resolve_font(f->t, c);
        n.kind = IN_OPEN; push_node(f, n);
        flatten_children(f, c);
        n.kind = IN_CLOSE; push_node(f, n);
        return;
    default:
        n.kind = IN_ATOMIC; push_node(f, n);
        return;
    }
}

/* ------------------------------------------------------------------------- */
/* line building                                                              */
/* ------------------------------------------------------------------------- */

typedef struct IState {           /* one open inline box on the current line */
    Box *box;                     /* NULL for the root (block container)    */
    int parent;
    int group;                    /* index of the align group root state (0 root) or -1 own */
    float baseline;               /* relative to the group's baseline, y down */
    float A, D;                   /* font ascent / descent                  */
    float lead_top, lead_bot;     /* half-leadings                          */
    float start_x;                /* fragment start x on this line (pen)    */
    int first;                    /* carries the start edge                 */
    int has_content;
} IState;

typedef struct LItem {            /* provisional item on the current line   */
    uint8_t kind;                 /* InlineItemKind                         */
    Box *box;
    uint32_t start, end;
    float x, w;                   /* x from line left; w visible width      */
    int state;                    /* innermost IState                       */
    float A, D;                   /* extents around own baseline            */
    float baseline;               /* relative to group baseline             */
    int group;                    /* state index of align root, -1: own group (top/bottom atomic) */
    uint8_t own_align;            /* CSS_VERTICAL_ALIGN_TOP / BOTTOM for own group */
    float w_trim; uint8_t ends_space;
} LItem;

typedef struct LItemVec { LItem *data; size_t len, cap; } LItemVec;
typedef struct IStateVec { IState *data; size_t len, cap; } IStateVec;
typedef struct LineVec { LineBox *data; size_t len, cap; } LineVec;

typedef struct Lines {
    LayoutTree *t;
    Box *b;
    const LayoutInput *in;
    float content_x, content_y, content_w;
    FloatCtx *floats, own_floats;
    int own_bfc;
    float bfc_x, bfc_y;           /* b's border box in BFC coords           */
    float y;                      /* current line top, relative to content top */
    float line_left, line_right;  /* current line edges relative to b border box */
    float pen;                    /* from line_left                          */
    LItemVec items;
    IStateVec states;
    Box *open_stack[64];
    int open_depth;
    int stack[66];                /* indices into states of the boxes open at the pen */
    int sp;
    LineVec lines;
    int line_has_content;
    int prev_space;               /* last placed content ended with a space  */
    int prev_atomic;
    float strut_A, strut_D, strut_lt, strut_lb;
    float max_right;              /* for overflow                            */
    int rtl;
    uint8_t text_align;
    float last_baseline, first_baseline;
    float pending_open_w;         /* start edges waiting for content         */
    int ellipsis;
    int clamp;                    /* -webkit-line-clamp: lines kept (0: none) */
    int more_after;               /* end_line from break_line: content follows */
    Box *abs_after[16];           /* block-level abs boxes after line content: */
    int abs_after_n;              /* static position at the next line's top  */
} Lines;

static float resolve_vertical_align_offset(const Box *child, const Box *parent, float cA, float cD, float parent_lh_unused)
{
    (void)parent_lh_unused;
    const CssComputed *s = child->style;
    float pfs = parent->style->font_size;
    switch (child->vertical_align) {
    case CSS_VERTICAL_ALIGN_BASELINE: return 0;
    case CSS_VERTICAL_ALIGN_SUB: return pfs / 5.0f + 1.0f;
    case CSS_VERTICAL_ALIGN_SUPER: return -(pfs / 3.0f + 1.0f);
    case CSS_VERTICAL_ALIGN_TEXT_TOP: return cA - parent->fm.ascent;
    case CSS_VERTICAL_ALIGN_TEXT_BOTTOM: return parent->fm.descent - cD;
    case CSS_VERTICAL_ALIGN_MIDDLE: {
        float xh = parent->fm.x_height > 0 ? parent->fm.x_height : parent->fm.ascent * 0.5f;
        return (cA - cD) * 0.5f - xh * 0.5f;
    }
    case CSS_VERTICAL_ALIGN_LENGTH: {
        float lh = layout_line_height(child);
        return -layout_resolve_len(&s->vertical_align_length, lh, 0);
    }
    default: return 0;   /* top / bottom handled by groups */
    }
}

static void state_metrics(Box *b, float *A, float *D, float *lt, float *lb)
{
    float lh = layout_line_height(b);
    float a = b->fm.ascent, d = b->fm.descent;
    float lead = lh - (a + d);
    /* Blink (CalculateLeadingSpace): the ascent side gets floor(leading / 2),
     * the descent side the rest, so an odd or fractional leading never puts
     * the baseline on a fractional px (text would round differently). */
    float top = floorf(lead * 0.5f + 1e-4f);
    *A = a; *D = d; *lt = top; *lb = lead - top;
}

static int top_state(Lines *L) { return L->sp > 0 ? L->stack[L->sp - 1] : 0; }

static int push_state(Lines *L, Box *box)
{
    IState st;
    memset(&st, 0, sizeof(st));
    st.box = box;
    st.parent = top_state(L);
    IState *ps = &L->states.data[st.parent];
    state_metrics(box, &st.A, &st.D, &st.lead_top, &st.lead_bot);
    int va = box->vertical_align;
    if (va == CSS_VERTICAL_ALIGN_TOP || va == CSS_VERTICAL_ALIGN_BOTTOM) {
        st.group = (int)L->states.len;   /* own group root */
        st.baseline = 0;
    } else {
        st.group = ps->group;
        Box *pbox = ps->box ? ps->box : L->b;
        st.baseline = ps->baseline + resolve_vertical_align_offset(box, pbox, st.A + st.lead_top, st.D + st.lead_bot, 0);
    }
    st.start_x = L->pen;
    KN_VEC_PUSH(&L->states, st);
    int idx = (int)L->states.len - 1;
    if (L->sp < (int)(sizeof(L->stack) / sizeof(L->stack[0]))) L->stack[L->sp++] = idx;
    return idx;
}

static void begin_line(Lines *L)
{
    L->items.len = 0;
    L->states.len = 0;
    L->sp = 0;
    IState root;
    memset(&root, 0, sizeof(root));
    root.parent = -1; root.group = 0;
    root.A = L->strut_A; root.D = L->strut_D; root.lead_top = L->strut_lt; root.lead_bot = L->strut_lb;
    KN_VEC_PUSH(&L->states, root);
    L->stack[L->sp++] = 0;
    L->pen = 0;
    L->line_has_content = 0;
    L->prev_space = 1;      /* leading spaces collapse */
    L->prev_atomic = 0;
    L->pending_open_w = 0;
    /* edges from floats */
    float l = L->bfc_x + L->content_x, r = l + L->content_w;
    float yy = L->bfc_y + L->content_y + L->y;
    float est_h = L->strut_A + L->strut_D + L->strut_lt + L->strut_lb;
    float fl, fr;
    floats_avail(L->floats, yy, kn_maxf(est_h, 1.0f), l, r, &fl, &fr);
    L->line_left = fl - L->bfc_x;
    L->line_right = fr - L->bfc_x;
    /* re-open inline boxes continuing from the previous line */
    for (int i = 0; i < L->open_depth; i++) {
        int si = push_state(L, L->open_stack[i]);
        L->states.data[si].first = 0;
    }
}

static float avail_w(const Lines *L) { return L->line_right - L->line_left; }

static void add_frag(Box *ib, float x, float y, float w, float h, int first, int last)
{
    if (ib->frag_count == ib->frag_cap) {
        int nc = ib->frag_cap ? ib->frag_cap * 2 : 2;
        BoxFrag *n = realloc(ib->frags, (size_t)nc * sizeof(BoxFrag));
        if (!n) return;
        ib->frags = n; ib->frag_cap = nc;
    }
    BoxFrag *f = &ib->frags[ib->frag_count++];
    f->x = x; f->y = y; f->w = w; f->h = h; f->first = (uint8_t)first; f->last = (uint8_t)last;
}

static TextRun *new_run(Box *tb)
{
    if (tb->run_count == tb->run_cap) {
        int nc = tb->run_cap ? tb->run_cap * 2 : 2;
        TextRun *n = realloc(tb->runs, (size_t)nc * sizeof(TextRun));
        if (!n) return NULL;
        tb->runs = n; tb->run_cap = nc;
    }
    TextRun *r = &tb->runs[tb->run_count++];
    memset(r, 0, sizeof(*r));
    return r;
}

static void shape_run(LayoutTree *t, TextRun *r)
{
    free(r->glyphs); r->glyphs = NULL; r->glyph_count = 0;
    if (!r->font || !r->len) return;
    float total = 0;
    int n = text_shape(r->font, r->text, r->len, r->letter_spacing, NULL, 0, &total);
    if (n <= 0) return;
    r->glyphs = malloc((size_t)n * sizeof(Glyph));
    if (!r->glyphs) return;
    r->glyph_count = text_shape(r->font, r->text, r->len, r->letter_spacing, r->glyphs, n, &total);
    (void)t;
}

/* Finish the current line: vertical alignment, horizontal alignment, record. */
static void end_line(Lines *L, int forced)
{
    (void)forced;
    /* strip trailing spaces of the last text item */
    float content_end = L->pen;
    for (int i = (int)L->items.len - 1; i >= 0; i--) {
        LItem *it = &L->items.data[i];
        if (it->kind == ITEM_INLINE_CLOSE || it->kind == ITEM_INLINE_OPEN) continue;
        if (it->kind == ITEM_TEXT && it->ends_space) {
            float d = it->w - it->w_trim;
            it->w = it->w_trim;
            while (it->end > it->start && is_space_byte(it->box->text[it->end - 1])) it->end--;
            content_end -= d;
            /* shift following close edges */
            for (size_t j = (size_t)i + 1; j < L->items.len; j++) L->items.data[j].x -= d;
            L->pen -= d;
        }
        break;
    }
    if (!L->line_has_content && L->items.len == 0) {
        /* empty line: nothing recorded (unless forced by <br>, handled by caller) */
        return;
    }

    /* --- vertical alignment --- */
    size_t ns = L->states.len;
    /* group extents, indexed by state (group root) + items with own group */
    float *gmin = malloc(ns * sizeof(float)), *gmax = malloc(ns * sizeof(float));
    for (size_t i = 0; i < ns; i++) { gmin[i] = LAYOUT_INF; gmax[i] = -LAYOUT_INF; }
    /* root strut */
    gmin[0] = -(L->strut_A + L->strut_lt); gmax[0] = L->strut_D + L->strut_lb;
    for (size_t i = 1; i < ns; i++) {
        IState *st = &L->states.data[i];
        if (!st->has_content) {
            Box *ib = st->box;
            if (ib->padding[CSS_LEFT] + ib->padding[CSS_RIGHT] + ib->border[CSS_LEFT] + ib->border[CSS_RIGHT] <= 0) continue;
        }
        int g = st->group;
        float top = st->baseline - st->A - st->lead_top, bot = st->baseline + st->D + st->lead_bot;
        if (top < gmin[g]) gmin[g] = top;
        if (bot > gmax[g]) gmax[g] = bot;
    }
    float own_extra_top = 0, own_extra_bot = 0;   /* own-group atomics: max heights */
    for (size_t i = 0; i < L->items.len; i++) {
        LItem *it = &L->items.data[i];
        if (it->kind == ITEM_INLINE_OPEN || it->kind == ITEM_INLINE_CLOSE || it->kind == ITEM_LINEBREAK) continue;
        if (it->group < 0) {
            float h = it->A + it->D;
            if (it->own_align == CSS_VERTICAL_ALIGN_TOP) own_extra_top = kn_maxf(own_extra_top, h);
            else own_extra_bot = kn_maxf(own_extra_bot, h);
            continue;
        }
        float top = it->baseline - it->A, bot = it->baseline + it->D;
        if (top < gmin[it->group]) gmin[it->group] = top;
        if (bot > gmax[it->group]) gmax[it->group] = bot;
    }
    /* Blink: top/bottom-aligned boxes taller than the line extend it; a tall
     * `top` box extends the descent, a tall `bottom` box the ascent (which
     * pushes the baseline group down). */
    float asc = -gmin[0], desc = gmax[0];
    for (size_t i = 1; i < ns; i++) {
        IState *st = &L->states.data[i];
        if (st->group != (int)i || !(gmax[i] > gmin[i])) continue;
        float h = gmax[i] - gmin[i];
        if (h > asc + desc) {
            if (st->box->vertical_align == CSS_VERTICAL_ALIGN_TOP) desc += h - (asc + desc);
            else asc += h - (asc + desc);
        }
    }
    if (own_extra_top > asc + desc) desc += own_extra_top - (asc + desc);
    if (own_extra_bot > asc + desc) asc += own_extra_bot - (asc + desc);
    float line_h = asc + desc;
    float line_top = L->content_y + L->y;
    float root_baseline = line_top + asc;
    /* group baselines */
    float *gbase = malloc(ns * sizeof(float));
    gbase[0] = root_baseline;
    for (size_t i = 1; i < ns; i++) {
        IState *st = &L->states.data[i];
        if (st->group != (int)i) { gbase[i] = 0; continue; }
        if (st->box->vertical_align == CSS_VERTICAL_ALIGN_TOP) gbase[i] = line_top - gmin[i];
        else gbase[i] = line_top + line_h - gmax[i];
    }

    /* --- horizontal alignment --- */
    float free_w = avail_w(L) - content_end;
    float shift = 0;
    uint8_t ta = L->text_align;
    if (ta == CSS_TEXT_ALIGN_START) ta = L->rtl ? CSS_TEXT_ALIGN_RIGHT : CSS_TEXT_ALIGN_LEFT;
    if (ta == CSS_TEXT_ALIGN_END) ta = L->rtl ? CSS_TEXT_ALIGN_LEFT : CSS_TEXT_ALIGN_RIGHT;
    if (free_w > 0) {
        if (ta == CSS_TEXT_ALIGN_CENTER) shift = free_w * 0.5f;
        else if (ta == CSS_TEXT_ALIGN_RIGHT) shift = free_w;
    } else if (L->rtl && ta == CSS_TEXT_ALIGN_RIGHT) {
        shift = free_w;   /* rtl overflow goes to the left */
    }
    float line_x = L->line_left + shift;

    /* --- ellipsis --- */
    int ell_item = -1; float ell_w = 0; Glyph ell_glyphs[3]; int ell_count = 0; memset(ell_glyphs, 0, sizeof(ell_glyphs));
    /* -webkit-line-clamp: the last kept line ends in an ellipsis when content follows it (Blink
     * puts it after the line's content, truncating the text so both fit the line) */
    const int clamp_ell = L->clamp > 0 && L->more_after && (int)L->lines.len == L->clamp - 1;
    if ((L->ellipsis && content_end > avail_w(L) + 0.01f) || clamp_ell) {
        float limit = avail_w(L);
        for (int i = (int)L->items.len - 1; i >= 0; i--) {
            LItem *it = &L->items.data[i];
            if (it->kind != ITEM_TEXT) continue;
            if (it->x >= limit) continue;
            ell_item = i;
            break;
        }
        if (ell_item >= 0) {
            LItem *it = &L->items.data[ell_item];
            Box *tb = it->box;
            static const char ell[] = "\xE2\x80\xA6";
            float tot = 0;
            if (tb->font) {
                /* Blink NGLineTruncator: U+2026 when the primary font has it, else "..." */
                int n = text_shape(tb->font, ell, 3, 0, ell_glyphs, 1, &tot);
                if (n >= 1 && ell_glyphs[0].face == text_font_face(tb->font)) {
                    ell_count = 1;
                } else {
                    n = text_shape(tb->font, "...", 3, 0, ell_glyphs, 3, &tot);
                    ell_count = n > 3 ? 3 : n;
                }
                ell_w = tot;
            }
            float ls = letter_spacing_of(tb->parent);
            float room = limit - it->x - ell_w;
            uint32_t best = it->start;
            float best_w = 0;
            for (uint32_t e = it->start; e < it->end;) {
                uint32_t ne = e + 1;
                while (ne < it->end && ((unsigned char)tb->text[ne] & 0xC0) == 0x80) ne++;
                float w = layout_text_width(L->t, tb->font, tb->text + it->start, ne - it->start, ls);
                if (w > room) break;
                best = ne; best_w = w;
                e = ne;
            }
            it->end = best;
            it->w = best_w;
            /* items after it are clipped away */
            L->items.len = (size_t)ell_item + 1;
        }
    }

    /* --- emit --- */
    LineBox ln;
    memset(&ln, 0, sizeof(ln));
    ln.x = line_x; ln.y = line_top; ln.w = avail_w(L); ln.h = line_h; ln.baseline = root_baseline;
    ln.items = calloc(L->items.len ? L->items.len : 1, sizeof(InlineItem));
    int ni = 0;
    for (size_t i = 0; i < L->items.len; i++) {
        LItem *it = &L->items.data[i];
        InlineItem *o = &ln.items[ni];
        memset(o, 0, sizeof(*o));
        o->kind = it->kind; o->box = it->box;
        float bl;
        if (it->group < 0) {
            float h = it->A + it->D;
            bl = (it->own_align == CSS_VERTICAL_ALIGN_TOP) ? line_top + it->A : line_top + line_h - h + it->A;
        } else bl = gbase[it->group] + it->baseline;
        o->baseline = bl;
        o->x = line_x + it->x;
        o->w = it->w;
        if (it->kind == ITEM_TEXT) {
            Box *tb = it->box;
            o->y = bl - tb->fm.ascent;
            o->h = tb->fm.ascent + tb->fm.descent;
            TextRun *r = new_run(tb);
            if (r) {
                r->font = tb->font;
                r->face = tb->font ? text_font_face(tb->font) : NULL;
                r->synth = tb->font ? text_font_synth(tb->font) : 0;
                r->font_size = tb->style->font_size;
                r->text = tb->text + it->start;
                r->len = it->end - it->start;
                r->full_len = r->len;
                r->letter_spacing = letter_spacing_of(tb->parent);
                r->x = o->x; r->y = bl; r->width = it->w;
                if ((int)i == ell_item) {
                    r->ellipsis = 1; r->ellipsis_x = o->x + it->w; r->ellipsis_w = ell_w;
                    memcpy(r->ellipsis_glyphs, ell_glyphs, sizeof(ell_glyphs));
                    r->ellipsis_count = (uint8_t)ell_count;
                    r->ellipsis_glyph = ell_glyphs[0];
                    o->w = it->w + ell_w;
                }
                shape_run(L->t, r);
                o->run = r;   /* pointer fixed up below (realloc) */
            }
            L->states.data[it->state].has_content = 1;
        } else if (it->kind == ITEM_ATOMIC) {
            Box *ab = it->box;
            o->y = bl - it->A;
            o->h = it->A + it->D;
            ab->lx = o->x + ab->margin[CSS_LEFT];
            ab->ly = o->y + ab->margin[CSS_TOP];
            layout_relative_offset(ab, L->content_w, L->in->cb_h);
        } else if (it->kind == ITEM_LINEBREAK) {
            o->y = bl - it->box->parent->fm.ascent; o->h = 0;
        } else {
            o->y = bl; o->h = 0;
        }
        ni++;
    }
    /* every text item must own exactly one run (repoint_runs relies on it) */
    for (int i = 0; i < ni; i++) {
        InlineItem *o = &ln.items[i];
        if (o->kind == ITEM_TEXT && !o->run) { o->kind = ITEM_INLINE_OPEN; o->w = 0; }
    }
    ln.item_count = ni;

    /* inline box fragments for every open state on this line */
    for (size_t i = 1; i < ns; i++) {
        IState *st = &L->states.data[i];
        Box *ib = st->box;
        float bl = (st->group == (int)i ? gbase[i] : gbase[st->group]) + st->baseline;
        /* fragment x range: start_x .. end (pen at close, or line end) */
        float sx = st->start_x, ex = L->pen;
        /* find close item for this state to get its end */
        for (size_t k = 0; k < L->items.len; k++) {
            LItem *it = &L->items.data[k];
            if (it->kind == ITEM_INLINE_CLOSE && it->box == ib) { ex = it->x + it->w; break; }
        }
        int closed = 0;
        for (size_t k = 0; k < L->items.len; k++) if (L->items.data[k].kind == ITEM_INLINE_CLOSE && L->items.data[k].box == ib) closed = 1;
        float fx = line_x + sx;
        float fw = ex - sx;
        if (st->first) { fx += ib->margin[CSS_LEFT]; fw -= ib->margin[CSS_LEFT]; }
        if (closed) fw -= ib->margin[CSS_RIGHT];
        float fy = bl - st->A - ib->padding[CSS_TOP] - ib->border[CSS_TOP];
        float fh = st->A + st->D + ib->padding[CSS_TOP] + ib->border[CSS_TOP] + ib->padding[CSS_BOTTOM] + ib->border[CSS_BOTTOM];
        if (ib->style->position == CSS_POSITION_RELATIVE) {
            int a;
            float l = layout_resolve_len_auto(&ib->style->inset[CSS_LEFT], L->content_w, &a);
            if (a) { l = -layout_resolve_len_auto(&ib->style->inset[CSS_RIGHT], L->content_w, &a); if (a) l = 0; }
            float tp = layout_resolve_len_auto(&ib->style->inset[CSS_TOP], L->in->cb_h, &a);
            if (a) { tp = -layout_resolve_len_auto(&ib->style->inset[CSS_BOTTOM], L->in->cb_h, &a); if (a) tp = 0; }
            fx += l; fy += tp;
            /* relative inline: shift its content items too */
            for (int k = 0; k < ni; k++) {
                InlineItem *o = &ln.items[k];
                Box *pb = o->box;
                int inside = 0;
                for (Box *q = pb; q && q != L->b; q = q->parent) if (q == ib) { inside = 1; break; }
                if (!inside || o->kind == ITEM_INLINE_OPEN || o->kind == ITEM_INLINE_CLOSE) continue;
                o->x += l; o->y += tp; o->baseline += tp;
                if (o->run) { o->run->x += l; o->run->y += tp; o->run->ellipsis_x += l; }
                if (o->kind == ITEM_ATOMIC) { pb->lx += l; pb->ly += tp; }
            }
        }
        add_frag(ib, fx, fy, kn_maxf(fw, 0), fh, st->first, closed);
    }

    KN_VEC_PUSH(&L->lines, ln);
    if (L->first_baseline < 0) L->first_baseline = root_baseline;
    L->last_baseline = root_baseline;
    float right = line_x + content_end;
    if (right > L->max_right) L->max_right = right;
    L->y += line_h;
    for (int i = 0; i < L->abs_after_n; i++) L->abs_after[i]->static_y = L->content_y + L->y;
    L->abs_after_n = 0;
    free(gmin); free(gmax); free(gbase);
}

static void break_line(Lines *L)
{
    L->more_after = 1;
    end_line(L, 0);
    L->more_after = 0;
    begin_line(L);
}

/* Lay out an atomic inline-level box (inline-block, inline-flex, replaced). */
static void layout_atomic(Lines *L, Box *ab)
{
    LayoutInput in;
    memset(&in, 0, sizeof(in));
    in.cb_w = L->content_w; in.cb_h = L->in->cb_h; in.avail_w = L->content_w;
    in.override_w = in.override_h = -1;
    in.shrink_to_fit = 1;
    in.floats = NULL;
    LayoutOutput o;
    layout_box(L->t, ab, &in, &o);
}

static void place_float(Lines *L, Box *fb)
{
    LayoutInput in;
    memset(&in, 0, sizeof(in));
    in.cb_w = L->content_w; in.cb_h = L->in->cb_h; in.avail_w = L->content_w;
    in.override_w = in.override_h = -1;
    in.shrink_to_fit = 1;
    LayoutOutput o;
    layout_resolve_edges(L->t, fb, L->content_w);
    layout_box(L->t, fb, &in, &o);
    float mw = box_margin_w(fb);
    float cur_y = L->bfc_y + L->content_y + L->y;
    float y_min = cur_y;
    float clear_y = floats_clear_y(L->floats, fb->style->clear);
    if (clear_y > y_min) y_min = clear_y;
    int fits_here = (L->pen + L->pending_open_w + mw <= avail_w(L) + 0.001f);
    if (!fits_here && L->line_has_content) {
        /* below the current line: estimate the line height with the strut */
        y_min = kn_maxf(y_min, cur_y + L->strut_A + L->strut_D + L->strut_lt + L->strut_lb);
    }
    floats_place(L->t, L->floats, fb, y_min, L->bfc_x + L->content_x, L->bfc_x + L->content_x + L->content_w);
    FloatEntry *e = &L->floats->data[L->floats->len - 1];
    fb->lx = e->x - L->bfc_x + fb->margin[CSS_LEFT];
    fb->ly = e->y - L->bfc_y + fb->margin[CSS_TOP];
    layout_relative_offset(fb, L->content_w, L->in->cb_h);
    /* shrink the current line if the float landed in it */
    if (e->y < cur_y + 0.001f + (L->strut_A + L->strut_D)) {
        if (e->side == CSS_FLOAT_LEFT) {
            float nl = e->x + e->w - L->bfc_x;
            if (nl > L->line_left) {
                float d = nl - L->line_left;
                if (!L->line_has_content) L->line_left = nl;
                else { L->line_left = nl; for (size_t i = 0; i < L->items.len; i++) (void)d; }
            }
        } else {
            float nr = e->x - L->bfc_x;
            if (nr < L->line_right) L->line_right = nr;
        }
    }
}

static void add_item(Lines *L, LItem it) { KN_VEC_PUSH(&L->items, it); }


static void fill_atomic_item(Lines *L, LItem *it, Box *ab)
{
    it->kind = ITEM_ATOMIC;
    it->box = ab;
    it->state = top_state(L);
    float mh = box_margin_h(ab);
    float bl = layout_box_baseline(ab);
    float A = (bl >= 0) ? bl + ab->margin[CSS_TOP] : mh;
    float D = mh - A;
    it->A = A; it->D = D;
    int va = ab->vertical_align;
    if (va == CSS_VERTICAL_ALIGN_TOP || va == CSS_VERTICAL_ALIGN_BOTTOM) {
        it->group = -1; it->own_align = (uint8_t)va; it->baseline = 0;
    } else {
        IState *st = &L->states.data[it->state];
        Box *pbox = st->box ? st->box : L->b;
        it->group = st->group;
        it->baseline = st->baseline + resolve_vertical_align_offset(ab, pbox, A, D, 0);
    }
}

/* An out-of-flow box whose original (pre-blockification) display is block-level. */
static int abs_is_block_level(const Box *ab)
{
    switch (ab->style->display) {
    case CSS_DISPLAY_INLINE: case CSS_DISPLAY_INLINE_BLOCK: case CSS_DISPLAY_INLINE_FLEX:
    case CSS_DISPLAY_INLINE_GRID: case CSS_DISPLAY_INLINE_TABLE:
        return 0;
    default:
        return 1;
    }
}

/* CSS 2.1 9.5 (Blink NGInlineLayoutAlgorithm: the next layout opportunity): a line
 * box shortened by floats that cannot hold its first piece of content (`need_w`
 * more than the pen and the pending inline edges) moves down past the next float
 * bottom, its edges recomputed, until the content fits or no float shortens it any
 * more; a line that already holds content wraps instead (run_lines). Containers that
 * never wrap keep the line where it is (Blink: Style().AutoWrap()). The HUD's
 * #topRight: the stat counters and the kill feed stack under the float:right
 * leaderboard instead of landing on it. */
static void line_make_room(Lines *L, float need_w)
{
    if (L->line_has_content) return;
    float full_l = L->content_x, full_r = L->content_x + L->content_w;
    for (int guard = 0; guard < 1000; guard++) {
        int shortened = L->line_left > full_l + 0.001f || L->line_right < full_r - 0.001f;
        if (!shortened) return;
        if (L->pen + L->pending_open_w + need_w <= avail_w(L) + 0.001f) return;
        float yy = L->bfc_y + L->content_y + L->y;
        float ny = floats_next_y(L->floats, yy);
        if (ny <= yy + 0.001f) return;
        L->y += ny - yy;
        float l = L->bfc_x + L->content_x, r = l + L->content_w;
        float est_h = L->strut_A + L->strut_D + L->strut_lt + L->strut_lb;
        float fl, fr;
        floats_avail(L->floats, ny, kn_maxf(est_h, 1.0f), l, r, &fl, &fr);
        L->line_left = fl - L->bfc_x;
        L->line_right = fr - L->bfc_x;
    }
}

static void run_lines(Lines *L, InNodeVec *nodes)
{
    begin_line(L);
    for (size_t i = 0; i < nodes->len; i++) {
        InNode *n = &nodes->data[i];
        switch (n->kind) {
        case IN_OPEN: {
            Box *ib = n->box;
            layout_resolve_edges(L->t, ib, L->content_w);
            if (L->open_depth < 64) L->open_stack[L->open_depth++] = ib;
            int si = push_state(L, ib);
            L->states.data[si].first = 1;
            float edge = ib->margin[CSS_LEFT] + ib->border[CSS_LEFT] + ib->padding[CSS_LEFT];
            L->states.data[si].start_x = L->pen + L->pending_open_w;
            L->pending_open_w += edge;
            LItem it; memset(&it, 0, sizeof(it));
            it.kind = ITEM_INLINE_OPEN; it.box = ib; it.state = si; it.x = L->pen; it.w = 0; it.group = L->states.data[si].group;
            add_item(L, it);
            break;
        }
        case IN_CLOSE: {
            Box *ib = n->box;
            float edge = ib->border[CSS_RIGHT] + ib->padding[CSS_RIGHT] + ib->margin[CSS_RIGHT];
            L->pen += L->pending_open_w; L->pending_open_w = 0;
            LItem it; memset(&it, 0, sizeof(it));
            it.kind = ITEM_INLINE_CLOSE; it.box = ib; it.state = top_state(L); it.x = L->pen; it.w = edge;
            it.group = L->states.data[it.state].group;
            add_item(L, it);
            L->pen += edge;
            if (L->sp > 1) L->sp--;   /* the state stays: end_line still needs its extents */
            if (L->open_depth > 0) L->open_depth--;
            break;
        }
        case IN_BR: {
            LItem it; memset(&it, 0, sizeof(it));
            it.kind = ITEM_LINEBREAK; it.box = n->box; it.state = top_state(L); it.x = L->pen; it.group = L->states.data[it.state].group;
            n->box->lx = L->line_left + L->pen; n->box->ly = L->content_y + L->y; n->box->w = n->box->h = 0;
            add_item(L, it);
            L->line_has_content = 1;   /* a <br> always produces a line */
            L->states.data[it.state].has_content = 1;
            break_line(L);
            break;
        }
        case IN_ABS: {
            Box *ab = n->box;
            ab->static_fx = ab->static_fy = 0;
            ab->static_y = L->content_y + L->y;
            if (abs_is_block_level(ab)) {
                /* as if position: static: a block box after this line (Blink:
                 * line start when the line is still empty, else the next line) */
                ab->static_x = L->content_x;
                if (L->line_has_content && L->abs_after_n < (int)(sizeof(L->abs_after) / sizeof(L->abs_after[0])))
                    L->abs_after[L->abs_after_n++] = ab;
            } else {
                ab->static_x = L->line_left + L->pen + L->pending_open_w;
            }
            break;
        }
        case IN_FLOAT:
            place_float(L, n->box);
            break;
        case IN_ATOMIC: {
            Box *ab = n->box;
            layout_resolve_edges(L->t, ab, L->content_w);
            layout_atomic(L, ab);
            float mw = box_margin_w(ab);
            int nowrap = L->b->style->white_space == CSS_WHITE_SPACE_NOWRAP;
            IState *ts = &L->states.data[top_state(L)];
            if (ts->box) nowrap = ts->box->style->white_space == CSS_WHITE_SPACE_NOWRAP;
            if (L->line_has_content && !nowrap && L->pen + L->pending_open_w + mw > avail_w(L) + 0.001f) break_line(L);
            if (!nowrap) line_make_room(L, mw);
            L->pen += L->pending_open_w; L->pending_open_w = 0;
            LItem it; memset(&it, 0, sizeof(it));
            fill_atomic_item(L, &it, ab);
            it.x = L->pen; it.w = mw;
            add_item(L, it);
            L->pen += mw;
            L->line_has_content = 1;
            L->states.data[it.state].has_content = 1;
            L->prev_space = 0; L->prev_atomic = 1;
            break;
        }
        case IN_TEXT: {
            Box *tb = n->box;
            uint32_t start = n->start, end = n->end;
            float w = n->w, w_trim = n->w_trim;
            float ls = letter_spacing_of(tb->parent);
            /* collapse leading spaces after a space / at line start */
            if (L->prev_space && n->starts_space) {
                while (start < end && is_space_byte(tb->text[start])) start++;
                if (start == end) { /* the whole piece was spaces */ break; }
                w = layout_text_width(L->t, tb->font, tb->text + start, end - start, ls);
                uint32_t e = end;
                while (e > start && is_space_byte(tb->text[e - 1])) e--;
                w_trim = (e == end) ? w : layout_text_width(L->t, tb->font, tb->text + start, e - start, ls);
            }
            const CssComputed *ps = tb->parent->style;
            int nowrap = ps->white_space == CSS_WHITE_SPACE_NOWRAP || ps->white_space == CSS_WHITE_SPACE_PRE;
            int break_word = ps->overflow_wrap != CSS_OVERFLOW_WRAP_NORMAL || ps->word_break == CSS_WORD_BREAK_BREAK_WORD
                          || ps->word_break == CSS_WORD_BREAK_BREAK_ALL;
            float need = L->pen + L->pending_open_w + w_trim;
            if (!nowrap && L->line_has_content && need > avail_w(L) + 0.001f) {
                break_line(L);
                if (n->starts_space) {
                    while (start < end && is_space_byte(tb->text[start])) start++;
                    if (start == end) break;
                    w = layout_text_width(L->t, tb->font, tb->text + start, end - start, ls);
                    uint32_t e = end;
                    while (e > start && is_space_byte(tb->text[e - 1])) e--;
                    w_trim = (e == end) ? w : layout_text_width(L->t, tb->font, tb->text + start, e - start, ls);
                }
            }
            if (!nowrap && !L->line_has_content && end > start) {
                /* a word that breaks inside (break_word) only needs its first character */
                float first_w = w_trim;
                if (break_word) {
                    uint32_t ne = start + 1;
                    while (ne < end && ((unsigned char)tb->text[ne] & 0xC0) == 0x80) ne++;
                    first_w = layout_text_width(L->t, tb->font, tb->text + start, ne - start, ls);
                }
                line_make_room(L, first_w);
            }
            /* a piece wider than the line on its own: break inside the word */
            while (!nowrap && break_word && L->pen + L->pending_open_w + w_trim > avail_w(L) + 0.001f && end > start) {
                float room = avail_w(L) - L->pen - L->pending_open_w;
                uint32_t best = start; float best_w = 0;
                for (uint32_t e = start; e < end;) {
                    uint32_t ne = e + 1;
                    while (ne < end && ((unsigned char)tb->text[ne] & 0xC0) == 0x80) ne++;
                    float pw = layout_text_width(L->t, tb->font, tb->text + start, ne - start, ls);
                    if (pw > room + 0.001f) break;
                    best = ne; best_w = pw; e = ne;
                }
                if (best == start) {
                    if (L->line_has_content) { break_line(L); continue; }
                    /* at least one character per line */
                    best = start + 1;
                    while (best < end && ((unsigned char)tb->text[best] & 0xC0) == 0x80) best++;
                    best_w = layout_text_width(L->t, tb->font, tb->text + start, best - start, ls);
                }
                L->pen += L->pending_open_w; L->pending_open_w = 0;
                LItem it; memset(&it, 0, sizeof(it));
                it.kind = ITEM_TEXT; it.box = tb; it.start = start; it.end = best; it.x = L->pen; it.w = best_w; it.w_trim = best_w;
                it.state = top_state(L);
                it.A = tb->fm.ascent + L->states.data[it.state].lead_top; it.D = tb->fm.descent + L->states.data[it.state].lead_bot;
                it.baseline = L->states.data[it.state].baseline; it.group = L->states.data[it.state].group;
                add_item(L, it);
                L->pen += best_w;
                L->line_has_content = 1;
                L->states.data[it.state].has_content = 1;
                start = best;
                if (start < end) {
                    break_line(L);
                    w = layout_text_width(L->t, tb->font, tb->text + start, end - start, ls);
                    uint32_t e = end;
                    while (e > start && is_space_byte(tb->text[e - 1])) e--;
                    w_trim = (e == end) ? w : layout_text_width(L->t, tb->font, tb->text + start, e - start, ls);
                }
            }
            if (start >= end) break;
            L->pen += L->pending_open_w; L->pending_open_w = 0;
            LItem it; memset(&it, 0, sizeof(it));
            it.kind = ITEM_TEXT; it.box = tb; it.start = start; it.end = end; it.x = L->pen; it.w = w; it.w_trim = w_trim;
            it.ends_space = n->ends_space;
            it.state = top_state(L);
            IState *st = &L->states.data[it.state];
            it.A = tb->fm.ascent + st->lead_top; it.D = tb->fm.descent + st->lead_bot;
            it.baseline = st->baseline; it.group = st->group;
            /* merge with the previous item of the same text box (same state) */
            if (L->items.len) {
                LItem *prev = &L->items.data[L->items.len - 1];
                if (prev->kind == ITEM_TEXT && prev->box == tb && prev->end == start && prev->state == it.state) {
                    prev->end = end;
                    prev->w = (it.x - prev->x) + w;
                    prev->w_trim = (it.x - prev->x) + w_trim;
                    prev->ends_space = it.ends_space;
                    L->pen += w;
                    L->line_has_content = 1;
                    st->has_content = 1;
                    L->prev_space = n->ends_space; L->prev_atomic = 0;
                    break;
                }
            }
            add_item(L, it);
            L->pen += w;
            L->line_has_content = 1;
            st->has_content = 1;
            L->prev_space = n->ends_space; L->prev_atomic = 0;
            break;
        }
        }
    }
    /* flush */
    L->pen += L->pending_open_w; L->pending_open_w = 0;
    end_line(L, 0);
}

/* ------------------------------------------------------------------------- */
/* entry                                                                      */
/* ------------------------------------------------------------------------- */

static void free_text_runs_rec(Box *b)
{
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->type == BOX_TEXT) layout_free_runs(c);
        else if (c->type == BOX_INLINE) { c->frag_count = 0; c->applied_x = c->applied_y = 0; free_text_runs_rec(c); }
    }
}

/* Runs are appended per text box in line/item order; realloc may have moved
 * them, so item->run pointers are re-derived once all lines exist. */
static void repoint_runs(Box *b)
{
    for (int i = 0; i < b->line_count; i++)
        for (int j = 0; j < b->lines[i].item_count; j++) {
            InlineItem *it = &b->lines[i].items[j];
            if (it->kind == ITEM_TEXT) it->box->frag_count = 0;   /* text boxes never use frags: temp counter */
        }
    for (int i = 0; i < b->line_count; i++)
        for (int j = 0; j < b->lines[i].item_count; j++) {
            InlineItem *it = &b->lines[i].items[j];
            if (it->kind != ITEM_TEXT) continue;
            Box *tb = it->box;
            it->run = tb->frag_count < tb->run_count ? &tb->runs[tb->frag_count++] : NULL;
        }
    for (int i = 0; i < b->line_count; i++)
        for (int j = 0; j < b->lines[i].item_count; j++)
            if (b->lines[i].items[j].kind == ITEM_TEXT) b->lines[i].items[j].box->frag_count = 0;
}

/* -webkit-line-clamp of the element whose lines these are (through anonymous wrappers). */
static int line_clamp_of(const Box *b)
{
    const Box *o = b;
    while (o && (o->flags & BOXF_ANONYMOUS) && o->parent) o = o->parent;
    return o && layout_style_line_clamps(o->style) ? o->style->line_clamp : 0;
}

/* Blink ShouldTruncateOverflowingText: the block container that owns the
 * line boxes truncates when it (or, for an anonymous block wrapper, the
 * element it wraps content of) clips overflow and has text-overflow:
 * ellipsis.  The text itself may live in inline descendants (spans). */
static int should_truncate(const Box *b)
{
    const Box *o = b;
    while (o && (o->flags & BOXF_ANONYMOUS) && o->parent) o = o->parent;
    if (!o) return 0;
    const CssComputed *s = o->style;
    return s->text_overflow == CSS_TEXT_OVERFLOW_ELLIPSIS && s->overflow_x != CSS_OVERFLOW_VISIBLE;
}

void layout_inline_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h)
{
    Lines L;
    memset(&L, 0, sizeof(L));
    L.t = t; L.b = b; L.in = in;
    L.content_x = box_cl(b); L.content_y = box_ct(b); L.content_w = box_cw(b);
    L.own_bfc = (b->flags & BOXF_BFC_ROOT) || !in->floats;
    L.floats = L.own_bfc ? &L.own_floats : in->floats;
    L.bfc_x = L.own_bfc ? 0 : in->bfc_x;
    L.bfc_y = L.own_bfc ? 0 : in->bfc_y;
    L.rtl = (b->flags & BOXF_RTL) != 0;
    L.text_align = b->style->text_align;
    L.ellipsis = should_truncate(b);
    L.clamp = line_clamp_of(b);
    L.first_baseline = -1; L.last_baseline = -1;
    state_metrics(b, &L.strut_A, &L.strut_D, &L.strut_lt, &L.strut_lb);
    free_text_runs_rec(b);

    Flattener f;
    memset(&f, 0, sizeof(f));
    f.t = t; f.in = in;
    flatten_children(&f, b);
    run_lines(&L, &f.nodes);

    /* hand the lines over */
    layout_free_lines(b);
    b->lines = L.lines.data;
    b->line_count = (int)L.lines.len;
    b->applied_x = b->applied_y = 0;
    repoint_runs(b);
    b->first_baseline = L.first_baseline;
    b->baseline_offset = L.last_baseline;
    float h = L.y;
    if (L.clamp > 0 && (int)L.lines.len > L.clamp) {
        /* the box ends after the clamp's last line; later lines overflow it (clipped by the
         * overflow: hidden the legacy idiom pairs it with) */
        const LineBox *last = &L.lines.data[L.clamp - 1];
        h = last->y + last->h - L.content_y;
        b->baseline_offset = last->baseline;
    }
    if (L.own_bfc) {
        float fb = floats_bottom(L.floats);
        if (fb > -LAYOUT_INF / 2 && fb - L.content_y > h) h = fb - L.content_y;
        free(L.own_floats.data);
    }
    *content_h = kn_maxf(h, 0);
    KN_VEC_FREE(&L.items);
    KN_VEC_FREE(&L.states);
    KN_VEC_FREE(&f.nodes);
}

/* ------------------------------------------------------------------------- */
/* intrinsic widths                                                           */
/* ------------------------------------------------------------------------- */

void layout_inline_intrinsic(LayoutTree *t, Box *b, float *min_w, float *max_w)
{
    Flattener f;
    memset(&f, 0, sizeof(f));
    f.t = t; f.for_intrinsic = 1;
    LayoutInput dummy; memset(&dummy, 0, sizeof(dummy)); dummy.cb_h = LAYOUT_INDEFINITE;
    f.in = &dummy;
    /* make sure fonts of inline children are resolved */
    flatten_children(&f, b);
    float minw = 0, maxw = 0;
    float cur_max = 0;          /* current unbreakable-by-forced-break segment */
    float cur_min = 0;          /* current unbreakable (no soft break) segment */
    float pending_edge = 0;
    int prev_space = 1;
    for (size_t i = 0; i < f.nodes.len; i++) {
        InNode *n = &f.nodes.data[i];
        switch (n->kind) {
        case IN_OPEN: {
            Box *ib = n->box;
            layout_resolve_edges(t, ib, 0.0f);
            pending_edge += ib->margin[CSS_LEFT] + ib->border[CSS_LEFT] + ib->padding[CSS_LEFT];
            break;
        }
        case IN_CLOSE: {
            Box *ib = n->box;
            float e = ib->border[CSS_RIGHT] + ib->padding[CSS_RIGHT] + ib->margin[CSS_RIGHT];
            cur_max += pending_edge + e; cur_min += pending_edge + e; pending_edge = 0;
            break;
        }
        case IN_BR:
            if (cur_min > minw) minw = cur_min;
            if (cur_max > maxw) maxw = cur_max;
            cur_min = cur_max = 0; prev_space = 1;
            break;
        case IN_ABS: break;
        case IN_FLOAT: case IN_ATOMIC: {
            float mn = layout_intrinsic_margin_width(t, n->box, 0, 0.0f);
            float mx = layout_intrinsic_margin_width(t, n->box, 1, 0.0f);
            const CssComputed *ps = n->box->parent ? n->box->parent->style : b->style;
            int nowrap = ps->white_space == CSS_WHITE_SPACE_NOWRAP;
            cur_max += pending_edge + mx;
            if (nowrap) cur_min += pending_edge + mn;
            else { if (cur_min > minw) minw = cur_min; cur_min = pending_edge + mn; if (cur_min > minw) minw = cur_min; cur_min = 0; }
            pending_edge = 0;
            prev_space = 0;
            break;
        }
        case IN_TEXT: {
            Box *tb = n->box;
            const CssComputed *ps = tb->parent->style;
            int nowrap = ps->white_space == CSS_WHITE_SPACE_NOWRAP || ps->white_space == CSS_WHITE_SPACE_PRE;
            float w = n->w, w_trim = n->w_trim;
            uint32_t start = n->start;
            if (prev_space && n->starts_space) {
                while (start < n->end && is_space_byte(tb->text[start])) start++;
                if (start == n->end) break;
                float ls = letter_spacing_of(tb->parent);
                w = layout_text_width(t, tb->font, tb->text + start, n->end - start, ls);
                uint32_t e = n->end;
                while (e > start && is_space_byte(tb->text[e - 1])) e--;
                w_trim = (e == n->end) ? w : layout_text_width(t, tb->font, tb->text + start, e - start, ls);
            }
            cur_max += pending_edge + w;
            if (nowrap) cur_min += pending_edge + w;
            else {
                cur_min += pending_edge + w_trim;
                if (cur_min > minw) minw = cur_min;
                cur_min = 0;
                if (!n->ends_space && !n->breakable_after) {
                    /* no break after: the next piece continues this unbreakable run */
                    cur_min = pending_edge + w_trim;
                    /* but the width already counted; avoid double counting */
                    cur_min = w_trim;
                }
            }
            pending_edge = 0;
            prev_space = n->ends_space;
            break;
        }
        }
    }
    if (cur_min > minw) minw = cur_min;
    /* trailing spaces of the last segment do not count */
    if (cur_max > maxw) maxw = cur_max;
    if (f.nodes.len) {
        for (size_t i = f.nodes.len; i-- > 0;) {
            InNode *n = &f.nodes.data[i];
            if (n->kind == IN_OPEN || n->kind == IN_CLOSE) continue;
            if (n->kind == IN_TEXT && n->ends_space && maxw == cur_max) maxw -= (n->w - n->w_trim);
            break;
        }
    }
    *min_w = kn_maxf(minw, 0);
    *max_w = kn_maxf(maxw, *min_w);
    KN_VEC_FREE(&f.nodes);
}
