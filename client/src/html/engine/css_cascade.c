/*
 * css_cascade.c - matching, cascade, inheritance and computed values.  See
 * css_cascade.h.
 */
#include "css_cascade.h"

#include <stdlib.h>
#include <string.h>

#include "css_values.h"
#include "util.h"

/* ------------------------------------------------------------------------- */
/* Module state                                                               */
/* ------------------------------------------------------------------------- */

#define MAX_SHEETS 15          /* UA + 14 author sheets (4-bit field)        */
#define MAX_VAR_TOKENS 256

typedef struct SheetSet {
    CssSheet *sheets[MAX_SHEETS];
    int count;                 /* sheets[0] == UA (may be NULL)              */
} SheetSet;

/* One matched selector. */
typedef struct Match {
    uint32_t sheet;            /* index into SheetSet                        */
    uint32_t rule;             /* rule index inside the sheet                */
    uint32_t specificity;
    uint8_t pe;
} Match;

/* One declaration to apply, sortable by `key`. */
typedef struct Entry {
    uint64_t key;
    const CssDecl *decl;
    const char *base;          /* url() base: sheet name atom, NULL = document */
} Entry;

typedef struct MatchVec { Match *data; size_t len, cap; } MatchVec;
typedef struct EntryVec { Entry *data; size_t len, cap; } EntryVec;
typedef struct DeclVec { CssDecl *data; size_t len, cap; } DeclVec;

static struct {
    SheetSet set;
    lxb_selectors_t *selectors;
    MatchVec matches;
    EntryVec entries;
    DeclVec inline_decls;      /* scratch: parsed inline declarations        */
    uint32_t stamp;            /* restyle counter (rule dedupe)              */
    DomInvalidation *inv;      /* merged invalidation map of inv_set         */
    uint32_t inv_serials[MAX_SHEETS]; /* css_sheet_serial of the sheets `inv` was built from */
    int inv_count;
    uint32_t computed_count;
} g;

static lxb_selectors_t *matcher(void)
{
    if (g.selectors == NULL) {
        g.selectors = lxb_selectors_create();
        if (g.selectors != NULL && lxb_selectors_init(g.selectors) != LXB_STATUS_OK) {
            lxb_selectors_destroy(g.selectors, true);
            g.selectors = NULL;
        }
    }
    return g.selectors;
}

static void set_sheets(Dom *dom, CssSheet *ua, CssSheet **author, int n_author)
{
    g.set.count = 0;
    g.set.sheets[g.set.count++] = ua;
    for (int i = 0; i < n_author && g.set.count < MAX_SHEETS; i++) {
        g.set.sheets[g.set.count++] = author[i];
    }
    /* rebuild the merged invalidation map when the sheet set changes */
    int same = g.inv != NULL && g.inv_count == g.set.count;
    for (int i = 0; same && i < g.set.count; i++) {
        same = g.inv_serials[i] == css_sheet_serial(g.set.sheets[i]);
    }
    if (!same) {
        if (g.inv == NULL) {
            g.inv = dom_inv_create();
        } else {
            dom_inv_clear(g.inv);
        }
        for (int i = 0; i < g.set.count; i++) {
            if (g.set.sheets[i] != NULL) {
                dom_inv_merge(g.inv, css_sheet_invalidation(g.set.sheets[i]));
            }
        }
        for (int i = 0; i < g.set.count; i++) {
            g.inv_serials[i] = css_sheet_serial(g.set.sheets[i]);
        }
        g.inv_count = g.set.count;
    }
    dom_set_invalidation(dom, g.inv);
}

/* Cascade levels (3 bits). */
enum { LEVEL_UA = 0, LEVEL_AUTHOR = 1, LEVEL_INLINE = 2, LEVEL_AUTHOR_IMPORTANT = 3, LEVEL_INLINE_IMPORTANT = 4, LEVEL_UA_IMPORTANT = 5 };

static uint64_t make_key(unsigned level, uint32_t spec, unsigned sheet, uint32_t order, uint32_t decl_index)
{
    if (order > 0x7fff) {
        order = 0x7fff;
    }
    if (decl_index > 0xfff) {
        decl_index = 0xfff;
    }
    return ((uint64_t)level << 61) | ((uint64_t)(spec & 0x3fffffffu) << 31) | ((uint64_t)(sheet & 0xf) << 27)
         | ((uint64_t)order << 12) | (uint64_t)decl_index;
}

static int entry_cmp(const void *a, const void *b)
{
    uint64_t ka = ((const Entry *)a)->key;
    uint64_t kb = ((const Entry *)b)->key;
    return ka < kb ? -1 : ka > kb ? 1 : 0;
}

/* ------------------------------------------------------------------------- */
/* Matching                                                                   */
/* ------------------------------------------------------------------------- */

typedef struct MatchCtx {
    uint32_t spec;
    int hit;
} MatchCtx;

static lxb_status_t match_cb(lxb_dom_node_t *node, lxb_css_selector_specificity_t spec, void *ctx)
{
    MatchCtx *m = (MatchCtx *)ctx;
    (void)node;
    (void)spec;
    m->hit = 1;
    return LXB_STATUS_STOP;
}

static void try_selectors(lxb_dom_node_t *node, uint32_t sheet_index, const CssSheet *sheet,
                          const uint32_t *idx, uint32_t count)
{
    lxb_selectors_t *sel = matcher();
    for (uint32_t i = 0; i < count; i++) {
        const CssSelector *cs = css_sheet_selector(sheet, idx[i]);
        MatchCtx mc = { 0, 0 };
        Match m;
        if (cs == NULL || cs->list == NULL) {
            continue;
        }
        lxb_selectors_match_node(sel, node, cs->list, match_cb, &mc);
        if (!mc.hit) {
            continue;
        }
        m.sheet = sheet_index;
        m.rule = cs->rule;
        m.specificity = cs->specificity;
        m.pe = cs->pe;
        KN_VEC_PUSH(&g.matches, m);
    }
}

/* Collects every matching selector of every sheet for `node` into g.matches
 * (all pseudo-element kinds; callers filter by pe). */
static void collect_matches(lxb_dom_node_t *node)
{
    lxb_dom_element_t *el = lxb_dom_interface_element(node);
    size_t tag_len = 0;
    const char *tag = (const char *)lxb_dom_element_qualified_name(el, &tag_len);
    const char *id = NULL;
    size_t id_len = 0;
    const char *cls = NULL;
    size_t cls_len = 0;

    g.matches.len = 0;
    if (el->attr_id != NULL && el->attr_id->value != NULL) {
        id = (const char *)el->attr_id->value->data;
        id_len = el->attr_id->value->length;
    }
    if (el->attr_class != NULL && el->attr_class->value != NULL) {
        cls = (const char *)el->attr_class->value->data;
        cls_len = el->attr_class->value->length;
    }

    for (int s = 0; s < g.set.count; s++) {
        const CssSheet *sheet = g.set.sheets[s];
        const uint32_t *idx;
        uint32_t count;
        if (sheet == NULL) {
            continue;
        }
        if (id_len > 0) {
            idx = css_sheet_bucket(sheet, CSS_KEY_ID, id, id_len, &count);
            try_selectors(node, (uint32_t)s, sheet, idx, count);
        }
        for (size_t i = 0; i < cls_len;) {
            size_t start;
            while (i < cls_len && kn_is_space((unsigned char)cls[i])) {
                i++;
            }
            start = i;
            while (i < cls_len && !kn_is_space((unsigned char)cls[i])) {
                i++;
            }
            if (i > start) {
                idx = css_sheet_bucket(sheet, CSS_KEY_CLASS, cls + start, i - start, &count);
                try_selectors(node, (uint32_t)s, sheet, idx, count);
            }
        }
        if (tag_len > 0) {
            idx = css_sheet_bucket(sheet, CSS_KEY_TAG, tag, tag_len, &count);
            try_selectors(node, (uint32_t)s, sheet, idx, count);
        }
        idx = css_sheet_universal(sheet, &count);
        try_selectors(node, (uint32_t)s, sheet, idx, count);
    }
}

/* ------------------------------------------------------------------------- */
/* Declaration application                                                    */
/* ------------------------------------------------------------------------- */

static unsigned level_for(uint32_t sheet, int important)
{
    if (sheet == 0) {
        return important ? LEVEL_UA_IMPORTANT : LEVEL_UA;
    }
    return important ? LEVEL_AUTHOR_IMPORTANT : LEVEL_AUTHOR;
}

/* Appends the declarations of every match with pseudo-element `pe` to
 * g.entries. */
/* url() base of a sheet's declarations: its name (the root-relative key
 * the engine loaded it from, e.g. "css/main.css"; "index.html" for <style>). */
static const char *sheet_base(const CssSheet *sheet)
{
    const char *name = sheet ? css_sheet_name(sheet) : NULL;
    return name ? kn_atom_cstr(name) : NULL;
}

static void gather_entries(int pe)
{
    g.entries.len = 0;
    for (size_t i = 0; i < g.matches.len; i++) {
        const Match *m = &g.matches.data[i];
        const CssRule *rule;
        if (m->pe != pe) {
            continue;
        }
        rule = css_sheet_rule(g.set.sheets[m->sheet], m->rule);
        if (rule == NULL) {
            continue;
        }
        for (uint32_t d = 0; d < rule->ndecls; d++) {
            const CssDecl *decl = &rule->decls[d];
            Entry e;
            e.key = make_key(level_for(m->sheet, decl->important), m->specificity, m->sheet, rule->order, d);
            e.decl = decl;
            e.base = sheet_base(g.set.sheets[m->sheet]);
            KN_VEC_PUSH(&g.entries, e);
        }
    }
}

static void inline_decl_cb(void *user, int prop_id, const CssToken *tokens, int n, int important)
{
    DeclVec *v = (DeclVec *)user;
    CssDecl d;
    if (n < 0 || n > 65535) {
        return;
    }
    d.prop = prop_id;
    d.important = (uint8_t)(important != 0);
    d.ntok = (uint16_t)n;
    d.has_var = (uint8_t)(css_tokens_have_var(tokens, n) != 0);
    d.tokens = (CssToken *)malloc((size_t)(n > 0 ? n : 1) * sizeof(CssToken));
    if (d.tokens == NULL) {
        return;
    }
    memcpy(d.tokens, tokens, (size_t)n * sizeof(CssToken));
    KN_VEC_PUSH(v, d);
}

static void inline_decls_clear(void)
{
    for (size_t i = 0; i < g.inline_decls.len; i++) {
        free(g.inline_decls.data[i].tokens);
    }
    g.inline_decls.len = 0;
}

/* Parses the element's inline declarations into g.inline_decls and appends
 * them to g.entries at the inline levels. */
static void gather_inline(Dom *dom, HtmlElem e)
{
    size_t count = 0;
    const DomInlineDecl *decls = dom_inline_decls(dom, e, &count);
    inline_decls_clear();
    for (size_t i = 0; i < count; i++) {
        css_parse_declaration(decls[i].name, strlen(decls[i].name), decls[i].value, strlen(decls[i].value),
                              decls[i].important, inline_decl_cb, &g.inline_decls);
    }
    for (size_t i = 0; i < g.inline_decls.len; i++) {
        const CssDecl *d = &g.inline_decls.data[i];
        Entry en;
        en.key = make_key(d->important ? LEVEL_INLINE_IMPORTANT : LEVEL_INLINE, 0, 0, 0, (uint32_t)i);
        en.decl = d;
        en.base = NULL;   /* inline style="": url()s resolve against the document */
        KN_VEC_PUSH(&g.entries, en);
    }
}

typedef struct VarCtx {
    const CssComputed *self;
} VarCtx;

static const char *var_resolve(void *user, const char *name, size_t len)
{
    VarCtx *vc = (VarCtx *)user;
    return css_computed_custom_get(vc->self, name, len);
}

static int apply_decl(CssComputed *out, const CssDecl *d, const CssComputed *parent, float font_px)
{
    if (d->has_var) {
        CssToken buf[MAX_VAR_TOKENS];
        VarCtx vc = { out };
        int n = css_substitute_vars(d->tokens, d->ntok, buf, MAX_VAR_TOKENS, var_resolve, &vc);
        if (n < 0) {
            /* invalid at computed-value time: behaves as `unset` */
            CssToken unset;
            memset(&unset, 0, sizeof(unset));
            unset.type = CSS_TOK_IDENT;
            unset.text = "unset";
            unset.len = 5;
            return css_computed_apply(out, d->prop, &unset, 1, parent, font_px);
        }
        return css_computed_apply(out, d->prop, buf, n, parent, font_px);
    }
    return css_computed_apply(out, d->prop, d->tokens, d->ntok, parent, font_px);
}

/* Computed-value-time fixups that depend on several properties:
 * border-*-width / outline-width compute to 0 when the style is none or
 * hidden (CSS Backgrounds 3); overflow visible/clip on one axis computes to
 * auto/hidden when the other axis is hidden/scroll/auto (CSS Overflow 3). */
static void fixup_computed(CssComputed *c)
{
    const int vis_x = c->overflow_x == CSS_OVERFLOW_VISIBLE || c->overflow_x == CSS_OVERFLOW_CLIP;
    const int vis_y = c->overflow_y == CSS_OVERFLOW_VISIBLE || c->overflow_y == CSS_OVERFLOW_CLIP;
    if (vis_x != vis_y) {
        uint8_t *o = vis_x ? &c->overflow_x : &c->overflow_y;
        *o = *o == CSS_OVERFLOW_VISIBLE ? CSS_OVERFLOW_AUTO : CSS_OVERFLOW_HIDDEN;
    }
    for (int i = 0; i < 4; i++) {
        if (c->border_style[i] == CSS_BORDER_STYLE_NONE || c->border_style[i] == CSS_BORDER_STYLE_HIDDEN) {
            c->border_width[i] = 0.0f;
        }
    }
    if (c->outline_style == CSS_BORDER_STYLE_NONE) {
        c->outline_width = 0.0f;
    }
}

/* Chrome's used border widths (after zoom): (0,1) -> 1, else floored. */
static void snap_border_widths(CssComputed *c)
{
    for (int i = 0; i < 4; i++) c->border_width[i] = css_snap_border_width(c->border_width[i]);
    c->outline_width = css_snap_border_width(c->outline_width);
}

/* The element's own `zoom` (last declaration wins; 1 when none). */
static float own_zoom(CssComputed *out, const CssComputed *parent)
{
    float saved = out->zoom;
    out->zoom = 1.0f;
    for (size_t i = 0; i < g.entries.len; i++) {
        const CssDecl *d = g.entries.data[i].decl;
        if (d->prop == CSS_PROP_ZOOM) apply_decl(out, d, parent, out->font_size);
    }
    float z = out->zoom > 0.0f ? out->zoom : 1.0f;
    out->zoom = saved;
    return z;
}

/* Applies g.entries (sorted ascending) in three passes: custom properties,
 * font-size, everything else.
 *
 * zoom (Chrome semantics): the element is computed in unzoomed space (the
 * inherited values and the parent record divided by the parent's effective
 * zoom), then every length is multiplied by the element's effective zoom
 * (parent's x own).  So px/em/rem/keyword lengths all scale, percentages
 * resolve against the zoomed containing block at layout time, and inherited
 * lengths follow a child's own zoom (line-height: 20px under zoom .5 then .5
 * is 5px, as in Blink). */
static void apply_entries(CssComputed *out, const CssComputed *parent)
{
    if (g.entries.len > 1) {
        qsort(g.entries.data, g.entries.len, sizeof(Entry), entry_cmp);
    }
    const float pz = parent != NULL && parent->zoom > 0.0f ? parent->zoom : 1.0f;
    const float ez = pz * own_zoom(out, parent);
    static CssComputed unzoomed_parent;   /* only touched while computing one element */
    if (pz != 1.0f) {
        css_computed_zoom(out, 1.0f / pz, 1);
        if (parent != NULL) {
            unzoomed_parent = *parent;
            css_computed_zoom(&unzoomed_parent, 1.0f / pz, 0);
            parent = &unzoomed_parent;
        }
    }
    float parent_font = parent != NULL ? parent->font_size : out->font_size;

    for (size_t i = 0; i < g.entries.len; i++) {
        const CssDecl *d = g.entries.data[i].decl;
        if (CSS_PROP_IS_CUSTOM(d->prop)) {
            apply_decl(out, d, parent, parent_font);
        }
    }
    for (size_t i = 0; i < g.entries.len; i++) {
        const CssDecl *d = g.entries.data[i].decl;
        if (d->prop == CSS_PROP_FONT_SIZE) {
            apply_decl(out, d, parent, parent_font);
        }
    }
    for (size_t i = 0; i < g.entries.len; i++) {
        const CssDecl *d = g.entries.data[i].decl;
        if (d->prop == CSS_PROP_FONT_SIZE || d->prop == CSS_PROP_ZOOM || CSS_PROP_IS_CUSTOM(d->prop)) {
            continue;
        }
        css_values_set_url_base(g.entries.data[i].base);
        apply_decl(out, d, parent, out->font_size);
    }
    css_values_set_url_base(NULL);
    css_computed_zoom(out, ez, 0);
    out->zoom = ez;
    fixup_computed(out);
    snap_border_widths(out);
    css_computed_hash(out);
}

static void init_from_parent(CssComputed *out, const CssComputed *parent)
{
    css_computed_init_defaults(out);
    if (parent != NULL) {
        css_computed_inherit(out, parent);
    }
}

/* Computes the element's own style into *out.  g.matches is left holding
 * the element's matches for the pseudo-element passes. */
static void compute_element(Dom *dom, lxb_dom_node_t *node, const CssComputed *parent, CssComputed *out)
{
    init_from_parent(out, parent);
    collect_matches(node);
    gather_entries(CSS_PE_NONE);
    gather_inline(dom, dom_node_elem(dom, node));
    apply_entries(out, parent);
    g.computed_count++;
}

/* Computes a pseudo-element style from the matches already collected for
 * the element.  Returns the number of declarations applied. */
static uint32_t compute_pseudo_from_matches(const CssComputed *elem, int pe, CssComputed *out)
{
    init_from_parent(out, elem);
    gather_entries(pe);
    if (g.entries.len == 0) {
        css_computed_hash(out);
        return 0;
    }
    apply_entries(out, elem);
    return (uint32_t)g.entries.len;
}

static int computed_equal(const CssComputed *a, const CssComputed *b)
{
    return a != NULL && b != NULL && a->hash_inherited == b->hash_inherited
        && a->hash_non_inherited == b->hash_non_inherited
        && memcmp(a, b, CSS_COMPUTED_NON_INHERITED_END) == 0;
}

static int inherited_equal(const CssComputed *a, const CssComputed *b)
{
    return a != NULL && b != NULL && a->hash_inherited == b->hash_inherited
        && memcmp((const char *)a + CSS_COMPUTED_INHERITED_BEGIN, (const char *)b + CSS_COMPUTED_INHERITED_BEGIN,
                  CSS_COMPUTED_INHERITED_END - CSS_COMPUTED_INHERITED_BEGIN) == 0;
}

/* Stores *src into *slot (allocating when needed); returns 1 when changed. */
static int store_computed(CssComputed **slot, const CssComputed *src)
{
    if (*slot != NULL && computed_equal(*slot, src)) {
        return 0;
    }
    if (*slot == NULL) {
        *slot = (CssComputed *)malloc(sizeof(CssComputed));
        if (*slot == NULL) {
            return 0;
        }
    }
    memcpy(*slot, src, sizeof(CssComputed));
    return 1;
}

static int pseudo_has_content(const CssComputed *c)
{
    return c->content.kind == CSS_CONTENT_STRING || c->content.kind == CSS_CONTENT_ATTR;
}

/* Recomputes one element (and its ::before/::after) from the matches.
 * Returns bit 1 when the element's record changed, bit 2 when its inherited
 * values changed, bit 4 when a pseudo record changed. */
static int restyle_one(Dom *dom, ElemData *d, const CssComputed *parent)
{
    CssComputed tmp;
    CssComputed ptmp;
    int result = 0;
    int inherited_changed;

    compute_element(dom, d->node, parent, &tmp);
    inherited_changed = !inherited_equal(d->computed, &tmp);
    if (store_computed(&d->computed, &tmp)) {
        result |= 1;
    }
    if (inherited_changed) {
        result |= 2;
    }

    /* ::before / ::after only when some selector targets them */
    {
        int want_before = 0, want_after = 0;
        for (size_t i = 0; i < g.matches.len; i++) {
            want_before |= g.matches.data[i].pe == CSS_PE_BEFORE;
            want_after |= g.matches.data[i].pe == CSS_PE_AFTER;
        }
        if (want_before) {
            compute_pseudo_from_matches(d->computed, CSS_PE_BEFORE, &ptmp);
            want_before = pseudo_has_content(&ptmp);
        }
        if (want_before) {
            result |= store_computed(&d->before, &ptmp) ? 4 : 0;
        } else if (d->before != NULL) {
            free(d->before);
            d->before = NULL;
            result |= 4;
        }
        if (want_after) {
            compute_pseudo_from_matches(d->computed, CSS_PE_AFTER, &ptmp);
            want_after = pseudo_has_content(&ptmp);
        }
        if (want_after) {
            result |= store_computed(&d->after, &ptmp) ? 4 : 0;
        } else if (d->after != NULL) {
            free(d->after);
            d->after = NULL;
            result |= 4;
        }
    }
    return result;
}

/* ------------------------------------------------------------------------- */
/* Tree traversal                                                             */
/* ------------------------------------------------------------------------- */

static void restyle_subtree(Dom *dom, lxb_dom_node_t *node, const CssComputed *parent, int force)
{
    ElemData *d = dom_node_data(node);
    int need;
    int child_force = force;
    int was_none;
    lxb_dom_node_t *c;

    if (d == NULL) {
        return;
    }
    need = force || (d->dirty & DOM_DIRTY_STYLE) != 0 || d->computed == NULL;
    was_none = d->computed != NULL && d->computed->display == CSS_DISPLAY_NONE;
    if (need) {
        int r = restyle_one(dom, d, parent);
        if (r & (1 | 4)) {
            d->dirty |= DOM_DIRTY_STYLE_CHANGED;
        }
        if (r & 2) {
            child_force = 1;
        }
    }
    if (d->dirty & DOM_DIRTY_DESCENDANTS) {
        child_force = 1;
    }
    if (d->computed->display == CSS_DISPLAY_NONE) {
        /* keep the subtree's dirty state for when it is displayed again */
        d->dirty &= ~(DOM_DIRTY_STYLE | DOM_DIRTY_DESCENDANTS);
        if (child_force) {
            d->dirty |= DOM_DIRTY_DESCENDANTS;
        }
        return;
    }
    if (was_none && need) {
        child_force = 1; /* descendants were skipped while hidden */
    }
    if (child_force || (d->dirty & DOM_DIRTY_HAS_DIRTY)) {
        for (c = node->first_child; c != NULL; c = c->next) {
            if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
                restyle_subtree(dom, c, d->computed, child_force);
            }
        }
    }
    d->dirty &= ~(DOM_DIRTY_STYLE | DOM_DIRTY_DESCENDANTS | DOM_DIRTY_HAS_DIRTY);
}

uint32_t cascade_restyle(Dom *dom, CssSheet *ua, CssSheet **author, int n_author)
{
    HtmlElem root = dom_root(dom);
    ElemData *d = dom_elem_data(dom, root);
    set_sheets(dom, ua, author, n_author);
    g.stamp++;
    g.computed_count = 0;
    if (d == NULL) {
        return 0;
    }
    if ((d->dirty & (DOM_DIRTY_STYLE | DOM_DIRTY_DESCENDANTS | DOM_DIRTY_HAS_DIRTY)) == 0 && d->computed != NULL) {
        return 0;
    }
    restyle_subtree(dom, d->node, NULL, 0);
    return g.computed_count;
}

/* Makes sure every ancestor of `node` (and node itself) has an up-to-date
 * computed record, ignoring display:none skipping.  An element is
 * recomputed when it is dirty, has never been computed, or an ancestor was
 * recomputed in this walk / carries DOM_DIRTY_DESCENDANTS (inherited values
 * pending, e.g. under display:none). *recomputed reports the latter. */
static const CssComputed *ensure_computed_rec(Dom *dom, lxb_dom_node_t *node, int *recomputed)
{
    ElemData *d = dom_node_data(node);
    const CssComputed *parent = NULL;
    int parent_recomputed = 0;
    *recomputed = 0;
    if (d == NULL) {
        return NULL;
    }
    if (node->parent != NULL && node->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        ElemData *pd = dom_node_data(node->parent);
        parent = ensure_computed_rec(dom, node->parent, &parent_recomputed);
        if (pd != NULL && (pd->dirty & DOM_DIRTY_DESCENDANTS) != 0) {
            parent_recomputed = 1;
        }
    }
    if (d->computed == NULL || (d->dirty & DOM_DIRTY_STYLE) != 0 || parent_recomputed) {
        int r = restyle_one(dom, d, parent);
        if (r & (1 | 4)) {
            d->dirty |= DOM_DIRTY_STYLE_CHANGED;
        }
        d->dirty &= ~DOM_DIRTY_STYLE;
        *recomputed = 1;
    }
    return d->computed;
}

static const CssComputed *ensure_computed(Dom *dom, lxb_dom_node_t *node)
{
    int recomputed;
    return ensure_computed_rec(dom, node, &recomputed);
}

uint32_t cascade_restyle_elem(Dom *dom, HtmlElem e, CssSheet *ua, CssSheet **author, int n_author)
{
    lxb_dom_node_t *node = dom_elem_node(dom, e);
    const CssComputed *parent = NULL;
    set_sheets(dom, ua, author, n_author);
    g.stamp++;
    g.computed_count = 0;
    if (node == NULL) {
        return 0;
    }
    if (node->parent != NULL && node->parent->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        parent = ensure_computed(dom, node->parent);
    }
    restyle_subtree(dom, node, parent, 0);
    return g.computed_count;
}

const CssComputed *cascade_elem_computed(Dom *dom, HtmlElem e)
{
    lxb_dom_node_t *node = dom_elem_node(dom, e);
    if (node == NULL) {
        return NULL;
    }
    return ensure_computed(dom, node);
}

const CssComputed *cascade_elem_before(Dom *dom, HtmlElem e)
{
    ElemData *d = dom_elem_data(dom, e);
    if (d == NULL || cascade_elem_computed(dom, e) == NULL) {
        return NULL;
    }
    return d->before;
}

const CssComputed *cascade_elem_after(Dom *dom, HtmlElem e)
{
    ElemData *d = dom_elem_data(dom, e);
    if (d == NULL || cascade_elem_computed(dom, e) == NULL) {
        return NULL;
    }
    return d->after;
}

uint32_t cascade_pseudo_computed(Dom *dom, HtmlElem e, int pe, CssComputed *out)
{
    lxb_dom_node_t *node = dom_elem_node(dom, e);
    const CssComputed *self;
    if (node == NULL || out == NULL) {
        return 0;
    }
    self = ensure_computed(dom, node);
    if (self == NULL) {
        return 0;
    }
    collect_matches(node);
    return compute_pseudo_from_matches(self, pe, out);
}

const CssKeyframes *cascade_find_keyframes(const char *name)
{
    for (int s = g.set.count - 1; s >= 0; s--) {
        const CssKeyframes *kf;
        if (g.set.sheets[s] == NULL) {
            continue;
        }
        kf = css_sheet_keyframes(g.set.sheets[s], name);
        if (kf != NULL) {
            return kf;
        }
    }
    return NULL;
}

void cascade_shutdown(void)
{
    inline_decls_clear();
    KN_VEC_FREE(&g.inline_decls);
    KN_VEC_FREE(&g.matches);
    KN_VEC_FREE(&g.entries);
    if (g.selectors != NULL) {
        lxb_selectors_destroy(g.selectors, true);
        g.selectors = NULL;
    }
    g.set.count = 0;
    dom_inv_destroy(g.inv);
    g.inv = NULL;
    g.inv_count = 0;
}
