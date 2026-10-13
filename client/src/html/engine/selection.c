/*
 * selection.c - text selection over laid-out text (see selection.h).
 */
#include "selection.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include <lexbor.h>
#include "css_cascade.h"
#include "css_sheet.h" /* CSS_PE_SELECTION */
#include "util.h"

void sel_init(Selection *s)
{
    memset(s, 0, sizeof(*s));
    s->last_press_ms = -1e9;
}

void sel_free(Selection *s)
{
    free(s->segs);
    free(s->entries);
    memset(s, 0, sizeof(*s));
}

int sel_clear(Selection *s)
{
    const int had = s->root != 0;
    s->root = 0;
    s->anchor.node = s->focus.node = NULL;
    s->word_a.node = s->word_b.node = NULL;
    s->dragging = 0;
    s->granularity = SEL_CHAR;
    s->n_entries = 0;
    s->n_segs = 0;
    return had;
}

int sel_active(const Selection *s)
{
    return s->root && s->anchor.node && s->focus.node && (s->anchor.node != s->focus.node || s->anchor.off != s->focus.off);
}

int sel_node_inside(const lxb_dom_node_t *node, const lxb_dom_node_t *root)
{
    if (!node || !root) return 0;
    for (const lxb_dom_node_t *n = node->parent; n; n = n->parent) {
        if (n == root) return 1;
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* Region                                                                     */
/* ------------------------------------------------------------------------- */

static int user_select_of(Dom *dom, HtmlElem e)
{
    ElemData *d = dom_elem_data(dom, e);
    return d && d->computed ? d->computed->user_select : CSS_USER_SELECT_NONE;
}

static int is_control(Dom *dom, HtmlElem e)
{
    const lxb_tag_id_t t = dom_tag_id(dom, e);
    return t == LXB_TAG_INPUT || t == LXB_TAG_TEXTAREA || t == LXB_TAG_SELECT || t == LXB_TAG_BUTTON;
}

HtmlElem sel_region_for(Dom *dom, HtmlElem target)
{
    if (!target || !dom_elem_data(dom, target)) return 0;
    for (HtmlElem e = target; e; e = dom_parent(dom, e)) {
        if (is_control(dom, e)) return 0; /* widgets.c owns a control's selection */
    }
    if (user_select_of(dom, target) == CSS_USER_SELECT_NONE) return 0;
    const HtmlElem body = dom_body(dom);
    HtmlElem r = target;
    while (r != body) {
        const HtmlElem p = dom_parent(dom, r);
        if (!p || p == dom_root(dom) || user_select_of(dom, p) == CSS_USER_SELECT_NONE) break;
        r = p;
    }
    return r;
}

/* ------------------------------------------------------------------------- */
/* Segments                                                                   */
/* ------------------------------------------------------------------------- */

static int push_seg(Selection *s, const SelSegment *g)
{
    if (s->n_segs == s->cap_segs) {
        const int cap = s->cap_segs ? s->cap_segs * 2 : 64;
        SelSegment *n = (SelSegment *) realloc(s->segs, (size_t) cap * sizeof(*n));
        if (!n) return 0;
        s->segs = n;
        s->cap_segs = cap;
    }
    s->segs[s->n_segs++] = *g;
    return 1;
}

static int selectable_text(const Box *tb)
{
    return tb && tb->type == BOX_TEXT && tb->node && tb->node->type == LXB_DOM_NODE_TYPE_TEXT && !tb->pseudo
        && tb->style && tb->style->user_select != CSS_USER_SELECT_NONE && !(tb->flags & BOXF_HIDDEN) && tb->text;
}

static void collect_box(Selection *s, Box *b)
{
    if (box_is_replaced(b)) return;
    if (b->flags & BOXF_HAS_LINES) {
        for (int i = 0; i < b->line_count; i++) {
            const LineBox *L = &b->lines[i];
            for (int k = 0; k < L->item_count; k++) {
                const InlineItem *it = &L->items[k];
                SelSegment g;
                memset(&g, 0, sizeof(g));
                g.line = L;
                g.block = b;
                if (it->kind == ITEM_LINEBREAK) {
                    push_seg(s, &g);
                } else if (it->kind == ITEM_TEXT && it->run && selectable_text(it->box)) {
                    g.tb = it->box;
                    g.run = it->run;
                    g.start = (uint32_t) (it->run->text - it->box->text);
                    g.end = g.start + it->run->full_len;
                    if (g.end > it->box->text_len) g.end = it->box->text_len;
                    push_seg(s, &g);
                }
            }
        }
    }
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->type == BOX_TEXT) continue;
        collect_box(s, c);
    }
}

int sel_collect(Selection *s, Box *root_box)
{
    s->n_segs = 0;
    if (root_box) collect_box(s, root_box);
    return s->n_segs;
}

/* Byte offset nearest to layout x in a segment (caret between glyphs). */
static uint32_t seg_off_at(const SelSegment *g, float lx)
{
    const TextRun *r = g->run;
    for (int i = 0; i < r->glyph_count; i++) {
        const Glyph *gl = &r->glyphs[i];
        if (lx < r->x + gl->x + gl->advance * 0.5f) return g->start + gl->cluster;
    }
    return g->run->len < g->end - g->start ? g->start + g->run->len : g->end;
}

int sel_point_at(const Selection *s, float vx, float vy, SelPoint *out)
{
    int best = -1;
    float best_dy = 0, best_dx = 0, best_lx = 0;
    const Box *last_tb = NULL;
    float lx = 0, ly = 0;
    for (int i = 0; i < s->n_segs; i++) {
        const SelSegment *g = &s->segs[i];
        if (!g->tb) continue;
        if (g->tb != last_tb) { /* the viewport point in the text box's layout space */
            layout_point_to_box(g->tb, vx, vy, &lx, &ly);
            last_tb = g->tb;
        }
        const float top = g->line->y, bot = g->line->y + g->line->h;
        const float dy = ly < top ? top - ly : ly > bot ? ly - bot : 0.0f;
        const float x0 = g->run->x, x1 = g->run->x + g->run->width;
        const float dx = lx < x0 ? x0 - lx : lx > x1 ? lx - x1 : 0.0f;
        if (best < 0 || dy < best_dy - 0.01f || (fabsf(dy - best_dy) <= 0.01f && dx < best_dx)) {
            best = i;
            best_dy = dy;
            best_dx = dx;
            best_lx = lx;
        }
    }
    if (best < 0) return 0;
    const SelSegment *g = &s->segs[best];
    out->node = g->tb->node;
    out->off = seg_off_at(g, best_lx);
    return 1;
}

/* Index of the segment holding point p (the first one whose range contains it,
 * a point at a run end belongs to that run).  -1 when not laid out. */
static int seg_index(const Selection *s, SelPoint p)
{
    int last = -1;
    for (int i = 0; i < s->n_segs; i++) {
        const SelSegment *g = &s->segs[i];
        if (!g->tb || g->tb->node != p.node) continue;
        if (p.off >= g->start && p.off < g->end) return i;
        if (p.off == g->end) return i;
        last = i;
    }
    if (last >= 0) return last; /* off past the text (it shrank): the node's last run */
    return -1;
}

/* <0 when a is before b. */
static int point_cmp(const Selection *s, SelPoint a, SelPoint b, int *ok)
{
    const int ia = seg_index(s, a), ib = seg_index(s, b);
    if (ia < 0 || ib < 0) {
        if (ok) *ok = 0;
        return 0;
    }
    if (ia != ib) {
        /* same node, different runs: offsets order them anyway */
        if (a.node == b.node) return a.off < b.off ? -1 : a.off > b.off ? 1 : 0;
        return ia < ib ? -1 : 1;
    }
    return a.off < b.off ? -1 : a.off > b.off ? 1 : 0;
}

static int is_word_cp(uint32_t cp)
{
    if (cp >= 0x80) return cp != 0xA0 && !(cp >= 0x2000 && cp <= 0x206F) && !(cp >= 0x3000 && cp <= 0x303F);
    return (cp >= '0' && cp <= '9') || (cp >= 'a' && cp <= 'z') || (cp >= 'A' && cp <= 'Z') || cp == '_';
}

static int is_space_cp(uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == 0xA0; }

/* Class of the character at byte i: 0 space, 1 word, 2 punctuation. */
static int char_class(const char *t, uint32_t len, uint32_t i)
{
    uint32_t cp = 0;
    if (i >= len) return -1;
    kn_utf8_decode(t + i, len - i, &cp);
    return is_space_cp(cp) ? 0 : is_word_cp(cp) ? 1 : 2;
}

static uint32_t utf8_prev(const char *t, uint32_t i)
{
    if (i == 0) return 0;
    i--;
    while (i > 0 && ((unsigned char) t[i] & 0xC0) == 0x80) i--;
    return i;
}

static uint32_t utf8_next(const char *t, uint32_t len, uint32_t i)
{
    if (i >= len) return len;
    i++;
    while (i < len && ((unsigned char) t[i] & 0xC0) == 0x80) i++;
    return i;
}

int sel_word_at(const Selection *s, SelPoint p, SelPoint *a, SelPoint *b)
{
    const int i = seg_index(s, p);
    if (i < 0) return 0;
    const Box *tb = s->segs[i].tb;
    const char *t = tb->text;
    const uint32_t len = tb->text_len;
    uint32_t at = p.off < len ? p.off : len;
    /* the character under the caret, or the one before it at the end of the text */
    if (at == len && at > 0) at = utf8_prev(t, at);
    const int cls = char_class(t, len, at);
    if (cls < 0) return 0;
    uint32_t lo = at, hi = utf8_next(t, len, at);
    if (cls != 2) { /* runs of word characters / of spaces; punctuation alone */
        while (lo > 0) {
            const uint32_t pv = utf8_prev(t, lo);
            if (char_class(t, len, pv) != cls) break;
            lo = pv;
        }
        while (hi < len && char_class(t, len, hi) == cls) hi = utf8_next(t, len, hi);
    }
    a->node = b->node = tb->node;
    a->off = lo;
    b->off = hi;
    return 1;
}

int sel_block_at(const Selection *s, SelPoint p, SelPoint *a, SelPoint *b)
{
    const int i = seg_index(s, p);
    if (i < 0) return 0;
    const Box *block = s->segs[i].block;
    int lo = i, hi = i;
    while (lo > 0 && s->segs[lo - 1].block == block) lo--;
    while (hi + 1 < s->n_segs && s->segs[hi + 1].block == block) hi++;
    while (lo <= hi && !s->segs[lo].tb) lo++;
    while (hi >= lo && !s->segs[hi].tb) hi--;
    if (lo > hi) return 0;
    a->node = s->segs[lo].tb->node;
    a->off = s->segs[lo].start;
    b->node = s->segs[hi].tb->node;
    b->off = s->segs[hi].end;
    return 1;
}

int sel_all(const Selection *s, SelPoint *a, SelPoint *b)
{
    int lo = 0, hi = s->n_segs - 1;
    while (lo <= hi && !s->segs[lo].tb) lo++;
    while (hi >= lo && !s->segs[hi].tb) hi--;
    if (lo > hi) return 0;
    a->node = s->segs[lo].tb->node;
    a->off = s->segs[lo].start;
    b->node = s->segs[hi].tb->node;
    b->off = s->segs[hi].end;
    return 1;
}

void sel_extend_to(Selection *s, SelPoint p)
{
    if (s->granularity == SEL_CHAR || !s->word_a.node) {
        s->focus = p;
        return;
    }
    SelPoint a, b;
    const int got = s->granularity == SEL_WORD ? sel_word_at(s, p, &a, &b) : sel_block_at(s, p, &a, &b);
    if (!got) {
        s->focus = p;
        return;
    }
    int ok = 1;
    if (point_cmp(s, p, s->word_a, &ok) < 0 && ok) { /* backwards: anchor at the first unit's end */
        s->anchor = s->word_b;
        s->focus = a;
    } else {
        s->anchor = s->word_a;
        s->focus = point_cmp(s, b, s->word_b, &ok) > 0 ? b : s->word_b;
    }
}

/* ------------------------------------------------------------------------- */
/* Resolution + colours                                                       */
/* ------------------------------------------------------------------------- */

static int push_entry(Selection *s, const SelEntry *e)
{
    if (s->n_entries == s->cap_entries) {
        const int cap = s->cap_entries ? s->cap_entries * 2 : 64;
        SelEntry *n = (SelEntry *) realloc(s->entries, (size_t) cap * sizeof(*n));
        if (!n) return 0;
        s->entries = n;
        s->cap_entries = cap;
    }
    s->entries[s->n_entries++] = *e;
    return 1;
}

/* ::selection of the nearest ancestor-or-self element with such a rule
 * (highlight inheritance); defaults when none. */
static void selection_colors(Dom *dom, const Box *tb, CssColor *bg, CssColor *fg, uint8_t *has_fg)
{
    bg->r = SEL_DEFAULT_BG_R;
    bg->g = SEL_DEFAULT_BG_G;
    bg->b = SEL_DEFAULT_BG_B;
    bg->a = 255;
    bg->kind = 0;
    *has_fg = 0;
    for (lxb_dom_node_t *n = tb->node ? tb->node->parent : NULL; n; n = n->parent) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        const HtmlElem e = dom_node_elem(dom, n);
        if (!e) continue;
        CssComputed st;
        if (!cascade_pseudo_computed(dom, e, CSS_PE_SELECTION, &st)) continue;
        *bg = st.background_color;
        *fg = st.color;
        *has_fg = 1;
        return;
    }
}

static int entry_cmp(const void *a, const void *b)
{
    const SelEntry *x = (const SelEntry *) a, *y = (const SelEntry *) b;
    return x->tb < y->tb ? -1 : x->tb > y->tb ? 1 : 0;
}

int sel_resolve(Selection *s, Dom *dom)
{
    s->n_entries = 0;
    if (!sel_active(s)) return s->root != 0;
    int ok = 1;
    SelPoint a = s->anchor, b = s->focus;
    if (point_cmp(s, a, b, &ok) > 0) {
        SelPoint t = a;
        a = b;
        b = t;
    }
    if (!ok) return 0;
    const int ia = seg_index(s, a), ib = seg_index(s, b);
    const lxb_dom_node_t *color_parent = NULL;
    CssColor bg = {0}, fg = {0};
    uint8_t has_fg = 0;
    for (int i = ia; i <= ib; i++) {
        const SelSegment *g = &s->segs[i];
        if (!g->tb) continue;
        uint32_t lo = g->start, hi = g->end;
        if (i == ia && g->tb->node == a.node && a.off > lo) lo = a.off;
        if (i == ib && g->tb->node == b.node && b.off < hi) hi = b.off;
        if (hi <= lo) continue;
        /* one entry per text box: widen the last one when the box continues */
        if (s->n_entries && s->entries[s->n_entries - 1].tb == g->tb) {
            SelEntry *e = &s->entries[s->n_entries - 1];
            if (lo < e->start) e->start = lo;
            if (hi > e->end) e->end = hi;
            continue;
        }
        if (g->tb->node->parent != color_parent) {
            color_parent = g->tb->node->parent;
            selection_colors(dom, g->tb, &bg, &fg, &has_fg);
        }
        SelEntry e = {g->tb, lo, hi, bg, fg, has_fg};
        push_entry(s, &e);
    }
    if (s->n_entries > 1) qsort(s->entries, (size_t) s->n_entries, sizeof(*s->entries), entry_cmp);
    return 1;
}

int sel_run_range(const Selection *s, const Box *tb, const TextRun *run, uint32_t *start, uint32_t *end, CssColor *bg,
                  CssColor *fg, int *has_fg)
{
    if (!s->n_entries || !tb || !tb->text) return 0;
    int lo = 0, hi = s->n_entries - 1;
    const SelEntry *e = NULL;
    while (lo <= hi) {
        const int mid = (lo + hi) / 2;
        if (s->entries[mid].tb == tb) {
            e = &s->entries[mid];
            break;
        }
        if (s->entries[mid].tb < tb) lo = mid + 1;
        else hi = mid - 1;
    }
    if (!e) return 0;
    const uint32_t r0 = (uint32_t) (run->text - tb->text), r1 = r0 + run->full_len;
    const uint32_t a = e->start > r0 ? e->start : r0, b = e->end < r1 ? e->end : r1;
    if (b <= a) return 0;
    *start = a - r0;
    *end = b - r0;
    *bg = e->bg;
    *fg = e->fg;
    *has_fg = e->has_fg;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* Text                                                                       */
/* ------------------------------------------------------------------------- */

char *sel_text(const Selection *s)
{
    if (!sel_active(s)) return NULL;
    int ok = 1;
    SelPoint a = s->anchor, b = s->focus;
    if (point_cmp(s, a, b, &ok) > 0) {
        SelPoint t = a;
        a = b;
        b = t;
    }
    if (!ok) return NULL;
    const int ia = seg_index(s, a), ib = seg_index(s, b);
    KnStrBuf sb = {NULL, 0, 0};
    const SelSegment *prev = NULL;
    uint32_t prev_hi = 0;
    for (int i = ia; i <= ib; i++) {
        const SelSegment *g = &s->segs[i];
        if (!g->tb) { /* <br> */
            if (prev) kn_sb_append(&sb, "\n", 1);
            continue;
        }
        uint32_t lo = g->start, hi = g->end;
        if (i == ia && g->tb->node == a.node && a.off > lo) lo = a.off;
        if (i == ib && g->tb->node == b.node && b.off < hi) hi = b.off;
        if (hi < lo) hi = lo;
        if (prev) {
            if (prev->tb == g->tb) {
                /* a wrap inside one text node: the source bytes between the runs */
                if (lo > prev_hi) kn_sb_append(&sb, g->tb->text + prev_hi, lo - prev_hi);
            } else if (prev->block != g->block) {
                kn_sb_append(&sb, "\n", 1);
            }
        }
        if (hi > lo) kn_sb_append(&sb, g->tb->text + lo, hi - lo);
        prev = g;
        prev_hi = hi;
    }
    if (!sb.data || !sb.len) {
        kn_sb_free(&sb);
        return NULL;
    }
    return sb.data; /* the caller frees */
}
