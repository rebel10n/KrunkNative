/*
 * layout_build.c - DOM -> box tree.
 *
 * Responsibilities: one Box per styled element (display:none -> none),
 * ::before/::after boxes, text boxes with collapsed/transformed text,
 * anonymous block wrappers (block/inline mixing, block-in-inline splitting),
 * anonymous flex/grid items, anonymous table rows/cells, replaced-element
 * intrinsic sizes, box flags (positioning, stacking, clipping, boundaries),
 * container / scroller assignment and the absolute-children lists.
 */
#include "layout_internal.h"
#include "css_values.h"
#include "image.h"     /* image_resolve_url: content: url() on <img> */

/* ------------------------------------------------------------------------- */
/* ctx                                                                        */
/* ------------------------------------------------------------------------- */

void layout_ctx_init(LayoutCtx *ctx)
{
    memset(ctx, 0, sizeof(*ctx));
    ctx->scrollbar_default_w = 12.0f;
    ctx->scrollbar_min_thumb = 0.0f;
    ctx->relaxed_boundaries = 1;
    ctx->ua_canvas_w = 300.0f;  ctx->ua_canvas_h = 150.0f;
    ctx->ua_input_text_w = 153.0f; ctx->ua_input_text_h = 21.0f;
    ctx->ua_input_checkbox = 13.0f;
    ctx->ua_input_range_w = 129.0f; ctx->ua_input_range_h = 16.0f;
    ctx->ua_input_color_w = 50.0f;  ctx->ua_input_color_h = 27.0f;
    ctx->ua_select_h = 20.0f;       ctx->ua_select_arrow_w = 20.0f;
}

/* ------------------------------------------------------------------------- */
/* box allocation / tree ops                                                  */
/* ------------------------------------------------------------------------- */

Box *layout_new_box(LayoutTree *t, uint8_t type, lxb_dom_node_t *node, const CssComputed *style)
{
    Box *b;
    if (t->free_list) {
        b = t->free_list;
        t->free_list = b->next;
        memset(b, 0, sizeof(*b));
    } else {
        b = kn_arena_alloc(&t->arena, sizeof(Box));
    }
    b->type = type;
    b->node = node;
    b->style = style ? style : &t->anon_style;
    b->attr_w = b->attr_h = -1.0f;
    b->min_content_w = b->max_content_w = -1.0f;
    b->baseline_offset = -1.0f;
    b->first_baseline = -1.0f;
    b->dirty = BOX_DIRTY_SELF | BOX_DIRTY_CHILD;
    return b;
}

void layout_append_child(Box *parent, Box *child)
{
    child->parent = parent;
    child->next = NULL;
    child->prev = parent->last_child;
    if (parent->last_child) parent->last_child->next = child;
    else parent->first_child = child;
    parent->last_child = child;
}

static void detach_all_children(Box *parent, Box **first_out)
{
    *first_out = parent->first_child;
    parent->first_child = parent->last_child = NULL;
}

int layout_elem_tag(const Box *b)
{
    if (!b->node || b->node->type != LXB_DOM_NODE_TYPE_ELEMENT) return 0;
    return (int)b->node->local_name;
}

static int node_tag(const lxb_dom_node_t *n)
{
    return (n && n->type == LXB_DOM_NODE_TYPE_ELEMENT) ? (int)n->local_name : 0;
}

static const char *elem_attr(lxb_dom_node_t *node, const char *name, size_t *len_out)
{
    size_t len = 0;
    if (!node || node->type != LXB_DOM_NODE_TYPE_ELEMENT) { if (len_out) *len_out = 0; return NULL; }
    const lxb_char_t *v = lxb_dom_element_get_attribute(lxb_dom_interface_element(node),
                                                        (const lxb_char_t *)name, strlen(name), &len);
    if (len_out) *len_out = len;
    return (const char *)v;
}

static float attr_number(lxb_dom_node_t *node, const char *name)
{
    size_t len;
    const char *v = elem_attr(node, name, &len);
    if (!v || !len) return -1.0f;
    char buf[32];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    memcpy(buf, v, n); buf[n] = 0;
    char *end;
    double d = strtod(buf, &end);
    if (end == buf || d < 0) return -1.0f;
    return (float)d;
}

/* ------------------------------------------------------------------------- */
/* fonts                                                                      */
/* ------------------------------------------------------------------------- */

void layout_resolve_font(LayoutTree *t, Box *b)
{
    const CssComputed *s = b->style;
    b->font = NULL;
    memset(&b->fm, 0, sizeof(b->fm));
    if (s->font_size <= 0.0f) {          /* font-size: 0 -> no ink, no advance, no height */
        return;
    }
    if (t->ctx->text) {
        char fam[512];
        size_t o = 0;
        for (int i = 0; i < s->font_family_count && i < CSS_MAX_FONT_FAMILIES; i++) {
            const char *f = s->font_families[i];
            if (!f) continue;
            size_t n = strlen(f);
            if (o + n + 3 >= sizeof(fam)) break;
            if (o) { fam[o++] = ','; fam[o++] = ' '; }
            memcpy(fam + o, f, n); o += n;
        }
        fam[o] = 0;
        b->font = text_font(t->ctx->text, o ? fam : NULL, s->font_size, s->font_weight,
                            s->font_style != CSS_FONT_STYLE_NORMAL);
    }
    if (b->font) {
        b->fm = *text_font_metrics(b->font);
    } else {
        /* no text system: approximate with typical Latin metrics */
        b->fm.size = s->font_size;
        b->fm.ascent = roundf(s->font_size * 0.9f);
        b->fm.descent = roundf(s->font_size * 0.25f);
        b->fm.line_gap = 0;
        b->fm.line_height = b->fm.ascent + b->fm.descent;
        b->fm.x_height = s->font_size * 0.5f;
        b->fm.ascent_raw = b->fm.ascent; b->fm.descent_raw = b->fm.descent;
    }
}

/* ------------------------------------------------------------------------- */
/* text                                                                       */
/* ------------------------------------------------------------------------- */

static int is_collapsible_ws(uint32_t cp) { return cp == ' ' || cp == '\t' || cp == '\n' || cp == '\r' || cp == '\f'; }
static int is_zero_width(uint32_t cp) { return cp == 0x200E || cp == 0x200F || cp == 0x200B || cp == 0xFEFF || cp == 0x200D || cp == 0x200C; }

static uint32_t transform_cp(uint32_t cp, int tt)
{
    if (tt == CSS_TEXT_TRANSFORM_UPPERCASE) {
        if (cp >= 'a' && cp <= 'z') return cp - 32;
        if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) return cp - 32;
    } else if (tt == CSS_TEXT_TRANSFORM_LOWERCASE) {
        if (cp >= 'A' && cp <= 'Z') return cp + 32;
        if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) return cp + 32;
    }
    return cp;
}

/* Collapse white space (white-space: normal/nowrap) and apply text-transform.
 * Returns an arena string; *out_len receives the byte length. */
static const char *build_text(LayoutTree *t, const char *src, size_t len, const CssComputed *s, uint32_t *out_len)
{
    (void)t;
    char *dst = malloc(len * 2 + 4);   /* case mapping may widen; freed with the box */
    if (!dst) { *out_len = 0; return NULL; }
    size_t o = 0, i = 0;
    int collapse = s->white_space == CSS_WHITE_SPACE_NORMAL || s->white_space == CSS_WHITE_SPACE_NOWRAP
                || s->white_space == CSS_WHITE_SPACE_PRE_LINE;
    int prev_space = 0;
    int capitalize_next = 1;
    while (i < len) {
        uint32_t cp;
        int n = kn_utf8_decode(src + i, len - i, &cp);
        i += (size_t)n;
        if (is_zero_width(cp)) continue;
        if (collapse && is_collapsible_ws(cp)) {
            if (s->white_space == CSS_WHITE_SPACE_PRE_LINE && cp == '\n') { dst[o++] = '\n'; prev_space = 1; continue; }
            if (prev_space) continue;
            dst[o++] = ' ';
            prev_space = 1;
            capitalize_next = 1;
            continue;
        }
        prev_space = 0;
        if (s->text_transform == CSS_TEXT_TRANSFORM_CAPITALIZE) {
            if (capitalize_next) cp = transform_cp(cp, CSS_TEXT_TRANSFORM_UPPERCASE);
            capitalize_next = 0;
        } else {
            cp = transform_cp(cp, s->text_transform);
        }
        o += (size_t)kn_utf8_encode(cp, dst + o);
    }
    dst[o] = 0;
    *out_len = (uint32_t)o;
    return dst;
}

static int text_is_whitespace_only(const char *s, uint32_t len)
{
    for (uint32_t i = 0; i < len; i++)
        if (!is_collapsible_ws((unsigned char)s[i])) return 0;
    return 1;
}

/* ------------------------------------------------------------------------- */
/* replaced elements                                                          */
/* ------------------------------------------------------------------------- */

static uint8_t replaced_type_for_tag(int tag)
{
    switch (tag) {
    case LXB_TAG_IMG: return BOX_IMAGE;
    case LXB_TAG_CANVAS: return BOX_CANVAS;
    case LXB_TAG_VIDEO: return BOX_VIDEO;
    case LXB_TAG_IFRAME: return BOX_VIDEO;     /* no nested browsing context: an empty 300x150 box */
    case LXB_TAG_INPUT: return BOX_INPUT;
    case LXB_TAG_TEXTAREA: return BOX_INPUT;   /* widgets.c WIDGET_TEXTAREA paints the text */
    case LXB_TAG_SELECT: return BOX_SELECT;
    default: return 0;
    }
}

static int input_type_is_hidden(lxb_dom_node_t *node)
{
    size_t len;
    const char *ty = elem_attr(node, "type", &len);
    return ty && kn_str_ieq(ty, len, "hidden");
}

static int replaced_intrinsics_unzoomed(LayoutTree *t, Box *b);

/* Natural sizes, width/height attributes and the UA control sizes are CSS px:
 * they scale with the effective zoom like every computed length.  The
 * widget_size callback already returns zoomed sizes (font-based or scaled). */
static void replaced_intrinsics(LayoutTree *t, Box *b)
{
    const int zoomed = replaced_intrinsics_unzoomed(t, b);
    const float z = b->style && b->style->zoom > 0.0f ? b->style->zoom : 1.0f;
    if (z == 1.0f) return;
    if (!zoomed) {
        b->intrinsic_w *= z;
        b->intrinsic_h *= z;
    }
    if (b->attr_w > 0) b->attr_w *= z;
    if (b->attr_h > 0) b->attr_h *= z;
}

/* Returns 1 when intrinsic_w/h came from widget_size (already zoomed). */
static int replaced_intrinsics_unzoomed(LayoutTree *t, Box *b)
{
    LayoutCtx *ctx = t->ctx;
    int tag = layout_elem_tag(b);
    b->attr_w = attr_number(b->node, "width");
    b->attr_h = attr_number(b->node, "height");
    if (tag == LXB_TAG_IMG) {
        size_t len;
        const char *src = elem_attr(b->node, "src", &len);
        float w = 0, h = 0;
        char key[512];
        if (b->style && b->style->content.kind == CSS_CONTENT_URL && b->style->content.str) {
            /* content: url(...) replaces the image: resolved against its stylesheet, root-relative */
            key[0] = '/';
            if (image_resolve_url(b->style->content.str, b->style->content.base, key + 1, sizeof(key) - 1)) { src = key; len = strlen(key); }
        }
        if (src && len) {
            char *s = malloc(len + 1);
            if (s) { memcpy(s, src, len); s[len] = 0; }
            b->src = s;
        }
        if (b->src && ctx->image_size) {
            if (!ctx->image_size(ctx->user, b->src, ctx->base_url, &w, &h)) w = h = 0;
        }
        b->intrinsic_w = w; b->intrinsic_h = h;
        if (w <= 0 && h <= 0) {
            /* missing image: Chrome lays out 0x0 (no alt) unless attributes give a size */
            b->intrinsic_w = b->attr_w > 0 ? b->attr_w : 0;
            b->intrinsic_h = b->attr_h > 0 ? b->attr_h : 0;
        }
    } else if (tag == LXB_TAG_CANVAS) {
        b->intrinsic_w = b->attr_w >= 0 ? b->attr_w : ctx->ua_canvas_w;
        b->intrinsic_h = b->attr_h >= 0 ? b->attr_h : ctx->ua_canvas_h;
        b->attr_w = b->attr_h = -1.0f;   /* canvas attributes are the bitmap size, not presentational hints */
    } else if (tag == LXB_TAG_VIDEO || tag == LXB_TAG_IFRAME) {
        b->intrinsic_w = ctx->ua_canvas_w;
        b->intrinsic_h = ctx->ua_canvas_h;
    } else if (tag == LXB_TAG_INPUT) {
        size_t len;
        const char *ty = elem_attr(b->node, "type", &len);
        float w = ctx->ua_input_text_w, h = ctx->ua_input_text_h;
        if (ty) {
            if (kn_str_ieq(ty, len, "checkbox") || kn_str_ieq(ty, len, "radio")) w = h = ctx->ua_input_checkbox;
            else if (kn_str_ieq(ty, len, "range")) { w = ctx->ua_input_range_w; h = ctx->ua_input_range_h; }
            else if (kn_str_ieq(ty, len, "color")) { w = ctx->ua_input_color_w; h = ctx->ua_input_color_h; }
        }
        if (!ty || !(kn_str_ieq(ty, len, "checkbox") || kn_str_ieq(ty, len, "radio") || kn_str_ieq(ty, len, "range") ||
                     kn_str_ieq(ty, len, "color") || kn_str_ieq(ty, len, "hidden")))
            b->flags |= BOXF_TEXT_CONTROL;
        /* UA sizes are border-box with the UA border/padding folded in */
        b->intrinsic_w = w;
        b->intrinsic_h = h;
        b->flags |= BOXF_INTRINSIC_BORDER;
        b->attr_w = b->attr_h = -1.0f;
        float ww, hh;
        if (ctx->widget_size && ctx->widget_size(ctx->user, b->node, b->style, &ww, &hh)) {
            b->intrinsic_w = ww; b->intrinsic_h = hh;
            b->flags &= ~BOXF_INTRINSIC_BORDER;
            return 1;
        }
    } else if (tag == LXB_TAG_TEXTAREA) {
        /* cols x rows from widgets_intrinsic_size(); fallback 20em x 2 lines */
        b->intrinsic_w = 20.0f * (b->style ? b->style->font_size : 16.0f);
        b->intrinsic_h = 2.0f * b->fm.line_height;
        b->attr_w = b->attr_h = -1.0f;
        float ww, hh;
        if (ctx->widget_size && ctx->widget_size(ctx->user, b->node, b->style, &ww, &hh)) {
            b->intrinsic_w = ww; b->intrinsic_h = hh;
        }
        return 1;
    } else if (tag == LXB_TAG_SELECT) {
        float longest = 0;
        for (lxb_dom_node_t *c = b->node->first_child; c; c = c->next) {
            lxb_dom_node_t *opt = c;
            if (node_tag(c) != LXB_TAG_OPTION) {
                /* optgroup: one level deeper */
                for (lxb_dom_node_t *g = c->first_child; g; g = g->next) {
                    if (node_tag(g) != LXB_TAG_OPTION) continue;
                    lxb_dom_node_t *tx = g->first_child;
                    if (tx && tx->type == LXB_DOM_NODE_TYPE_TEXT) {
                        lexbor_str_t *d = &lxb_dom_interface_text(tx)->char_data.data;
                        float adv = layout_text_width(t, b->font, (const char *)d->data, d->length, 0);
                        if (adv > longest) longest = adv;
                    }
                }
                continue;
            }
            lxb_dom_node_t *tx = opt->first_child;
            if (tx && tx->type == LXB_DOM_NODE_TYPE_TEXT) {
                lexbor_str_t *d = &lxb_dom_interface_text(tx)->char_data.data;
                size_t l = d->length;
                const char *s = (const char *)d->data;
                s = kn_str_trim(s, &l);
                float adv = layout_text_width(t, b->font, s, l, 0);
                if (adv > longest) longest = adv;
            }
        }
        /* text widths are already zoomed (zoomed font): fold z out of them */
        const float z = b->style && b->style->zoom > 0.0f ? b->style->zoom : 1.0f;
        b->intrinsic_w = ceilf(longest) / z + ctx->ua_select_arrow_w;
        b->intrinsic_h = kn_maxf(ctx->ua_select_h, b->fm.line_height / z);
        b->flags |= BOXF_TEXT_CONTROL;
        b->attr_w = b->attr_h = -1.0f;
        float ww, hh;
        if (ctx->widget_size && ctx->widget_size(ctx->user, b->node, b->style, &ww, &hh)) {
            b->intrinsic_w = ww; b->intrinsic_h = hh;
            return 1;
        }
    }
    return 0;
}

/* ------------------------------------------------------------------------- */
/* display -> box type / flags                                                */
/* ------------------------------------------------------------------------- */

typedef struct BuildCtx {
    LayoutTree *t;
} BuildCtx;

static int display_is_inline_level(int d)
{
    return d == CSS_DISPLAY_INLINE || d == CSS_DISPLAY_INLINE_BLOCK || d == CSS_DISPLAY_INLINE_FLEX
        || d == CSS_DISPLAY_INLINE_GRID || d == CSS_DISPLAY_INLINE_TABLE;
}

/* Blockified display for floats / out-of-flow / flex & grid items. */
static int blockify(int d)
{
    switch (d) {
    case CSS_DISPLAY_INLINE: case CSS_DISPLAY_INLINE_BLOCK: return CSS_DISPLAY_BLOCK;
    case CSS_DISPLAY_INLINE_FLEX: return CSS_DISPLAY_FLEX;
    case CSS_DISPLAY_INLINE_GRID: return CSS_DISPLAY_GRID;
    case CSS_DISPLAY_INLINE_TABLE: return CSS_DISPLAY_TABLE;
    case CSS_DISPLAY_TABLE_CELL: case CSS_DISPLAY_TABLE_ROW: case CSS_DISPLAY_TABLE_ROW_GROUP: return CSS_DISPLAY_BLOCK;
    default: return d;
    }
}

static uint8_t type_for_display(int d, int tag)
{
    uint8_t rt = replaced_type_for_tag(tag);
    if (rt) return rt;
    switch (d) {
    case CSS_DISPLAY_INLINE: return BOX_INLINE;
    case CSS_DISPLAY_FLEX: case CSS_DISPLAY_INLINE_FLEX: return BOX_FLEX;
    case CSS_DISPLAY_GRID: case CSS_DISPLAY_INLINE_GRID: return BOX_GRID;
    case CSS_DISPLAY_TABLE: case CSS_DISPLAY_INLINE_TABLE: return BOX_TABLE;
    case CSS_DISPLAY_TABLE_ROW: case CSS_DISPLAY_TABLE_ROW_GROUP: return BOX_TABLE_ROW;
    case CSS_DISPLAY_TABLE_CELL: return BOX_TABLE_CELL;
    default: return BOX_BLOCK;
    }
}

/* A transform (incl. translateZ(0)/3D-only lists, which leave an empty 2D op
 * list) or will-change: transform makes the box a stacking context and the
 * containing block for fixed descendants, as in Chrome (and paint.c). */
static int style_has_transform(const CssComputed *s)
{
    return s->transform.count > 0 || s->transform.has_3d || (s->will_change & CSS_WILL_CHANGE_TRANSFORM);
}

static int len_is_definite(const CssLength *l)
{
    return l->kind == CSS_LEN_VALUE;
}

static void set_dynamic_flags(LayoutTree *t, Box *b, int parent_is_flex_or_grid);

static void set_flags_from_style(LayoutTree *t, Box *b, int parent_is_flex_or_grid)
{
    const CssComputed *s = b->style;
    int d = s->display;
    int positioned = s->position != CSS_POSITION_STATIC;
    int oof = s->position == CSS_POSITION_ABSOLUTE || s->position == CSS_POSITION_FIXED;
    int floated = !oof && s->float_ != CSS_FLOAT_NONE;

    if (positioned) b->flags |= BOXF_POSITIONED;
    if (oof) b->flags |= BOXF_OUT_OF_FLOW;
    if (floated) b->flags |= BOXF_FLOAT;
    if (s->position == CSS_POSITION_STICKY) b->flags |= BOXF_STICKY;
    if (oof || floated || parent_is_flex_or_grid) d = blockify(d);

    b->vertical_align = s->vertical_align;
    b->z_auto = s->z_index_auto;
    b->z_index = s->z_index_auto ? 0 : s->z_index;

    if (display_is_inline_level(d)) {
        b->flags |= BOXF_INLINE_LEVEL;
        if (d != CSS_DISPLAY_INLINE || box_is_replaced(b)) b->flags |= BOXF_ATOMIC;
    } else {
        b->flags |= BOXF_BLOCK_LEVEL;
    }

    if (b->type != BOX_INLINE && b->type != BOX_TEXT && b->type != BOX_LINEBREAK) {
        if (s->overflow_x != CSS_OVERFLOW_VISIBLE || s->overflow_y != CSS_OVERFLOW_VISIBLE) {
            b->flags |= BOXF_SCROLL_CONTAINER | BOXF_CLIPS;
        }
        if (s->clip_path.kind != CSS_CLIP_PATH_NONE) b->flags |= BOXF_CLIPS;
        /* BFC roots: floats, abs, inline-blocks, overflow != visible, flex/grid items, table cells, flex/grid containers */
        if (oof || floated || (b->flags & BOXF_ATOMIC) || (b->flags & BOXF_SCROLL_CONTAINER)
            || parent_is_flex_or_grid || b->type == BOX_TABLE_CELL || b->type == BOX_FLEX || b->type == BOX_GRID
            || b->type == BOX_TABLE || !b->parent)
            b->flags |= BOXF_BFC_ROOT;
    }
    set_dynamic_flags(t, b, parent_is_flex_or_grid);
}

/* Flags that follow non-structural properties (refreshed in place on restyle). */
#define BOXF_DYNAMIC (BOXF_RTL | BOXF_HIDDEN | BOXF_NO_POINTER | BOXF_HAS_TRANSFORM | BOXF_ELLIPSIS \
                      | BOXF_STACKING_CONTEXT | BOXF_LAYOUT_BOUNDARY)

static void set_dynamic_flags(LayoutTree *t, Box *b, int parent_is_flex_or_grid)
{
    const CssComputed *s = b->style;
    int positioned = s->position != CSS_POSITION_STATIC;
    int oof = s->position == CSS_POSITION_ABSOLUTE || s->position == CSS_POSITION_FIXED;
    b->flags &= ~(uint32_t)BOXF_DYNAMIC;
    b->vertical_align = s->vertical_align;
    b->z_auto = s->z_index_auto;
    b->z_index = s->z_index_auto ? 0 : s->z_index;
    if (s->direction == CSS_DIRECTION_RTL) b->flags |= BOXF_RTL;
    if (s->visibility != CSS_VISIBILITY_VISIBLE) b->flags |= BOXF_HIDDEN;
    if (s->pointer_events == CSS_POINTER_EVENTS_NONE) b->flags |= BOXF_NO_POINTER;
    if (style_has_transform(s)) b->flags |= BOXF_HAS_TRANSFORM;
    if (s->text_overflow == CSS_TEXT_OVERFLOW_ELLIPSIS && (s->overflow_x != CSS_OVERFLOW_VISIBLE))
        b->flags |= BOXF_ELLIPSIS;

    /* stacking context */
    int sc = 0;
    if (!b->parent) sc = 1;
    if (positioned && !s->z_index_auto) sc = 1;
    if (s->position == CSS_POSITION_FIXED || s->position == CSS_POSITION_STICKY) sc = 1;
    if (s->opacity < 1.0f) sc = 1;
    if (style_has_transform(s) || s->filter_count > 0 || s->clip_path.kind != CSS_CLIP_PATH_NONE) sc = 1;
    if (parent_is_flex_or_grid && !s->z_index_auto) sc = 1;
    if (sc) b->flags |= BOXF_STACKING_CONTEXT;

    /* layout boundary */
    if (oof && b->type != BOX_TABLE_CELL && b->type != BOX_TABLE_ROW) {
        int w_def = len_is_definite(&s->width) || (len_is_definite(&s->inset[CSS_LEFT]) && len_is_definite(&s->inset[CSS_RIGHT]));
        int h_def = len_is_definite(&s->height) || (len_is_definite(&s->inset[CSS_TOP]) && len_is_definite(&s->inset[CSS_BOTTOM]));
        int clips = (b->flags & BOXF_SCROLL_CONTAINER) != 0;
        if (w_def && h_def && (clips || t->ctx->relaxed_boundaries)) b->flags |= BOXF_LAYOUT_BOUNDARY;
    }
}

/* ------------------------------------------------------------------------- */
/* building                                                                   */
/* ------------------------------------------------------------------------- */

static void build_children(LayoutTree *t, Box *parent, lxb_dom_node_t *elem);
static Box *build_element(LayoutTree *t, Box *parent, lxb_dom_node_t *node, const CssComputed *style, int pseudo);

static int tag_is_skipped(int tag)
{
    return tag == LXB_TAG_HEAD || tag == LXB_TAG_SCRIPT || tag == LXB_TAG_STYLE || tag == LXB_TAG_TITLE
        || tag == LXB_TAG_META || tag == LXB_TAG_LINK || tag == LXB_TAG_TEMPLATE || tag == LXB_TAG_NOSCRIPT
        || tag == LXB_TAG_OPTION || tag == LXB_TAG_OPTGROUP;
}

/* `style` is the text's own (inherited) style: the parent box's, or a display:contents
 * element's when the text is a child of one (NULL = the parent box's). */
static Box *build_text_box_styled(LayoutTree *t, Box *parent, lxb_dom_node_t *node, const char *src, size_t len, const CssComputed *style)
{
    const int own = style && style != parent->style;
    if (!style) style = parent->style;
    Box *b = layout_new_box(t, BOX_TEXT, node, style);
    b->text = build_text(t, src, len, style, &b->text_len);
    b->flags |= BOXF_INLINE_LEVEL;
    if (own) {
        b->parent = parent;
        layout_resolve_font(t, b);
    } else {
        b->font = parent->font;
        b->fm = parent->fm;
    }
    if (style->visibility != CSS_VISIBILITY_VISIBLE) b->flags |= BOXF_HIDDEN;
    if (style->pointer_events == CSS_POINTER_EVENTS_NONE) b->flags |= BOXF_NO_POINTER;
    layout_append_child(parent, b);
    return b;
}

static Box *build_text_box(LayoutTree *t, Box *parent, lxb_dom_node_t *node, const char *src, size_t len)
{
    return build_text_box_styled(t, parent, node, src, len, NULL);
}

static void build_pseudo(LayoutTree *t, Box *parent, lxb_dom_node_t *node, CssComputed *style, int pseudo)
{
    if (!style || style->display == CSS_DISPLAY_NONE) return;
    if (style->content.kind != CSS_CONTENT_STRING && style->content.kind != CSS_CONTENT_ATTR) return;
    Box *b = build_element(t, parent, node, style, pseudo);
    if (!b) return;
    const char *text = NULL; size_t len = 0;
    if (style->content.kind == CSS_CONTENT_STRING) { text = style->content.str; len = text ? strlen(text) : 0; }
    else if (style->content.str) text = elem_attr(node, style->content.str, &len);
    if (text && len) build_text_box(t, b, NULL, text, len);
}

static Box *build_element(LayoutTree *t, Box *parent, lxb_dom_node_t *node, const CssComputed *style, int pseudo)
{
    int tag = pseudo ? 0 : node_tag(node);
    int replaced = !pseudo && replaced_type_for_tag(tag) != 0;
    if (replaced && tag == LXB_TAG_INPUT && input_type_is_hidden(node)) return NULL;

    int parent_fg = parent && (parent->type == BOX_FLEX || parent->type == BOX_GRID);
    int d = style->display;
    int oof = style->position == CSS_POSITION_ABSOLUTE || style->position == CSS_POSITION_FIXED;
    if (oof || style->float_ != CSS_FLOAT_NONE || parent_fg) d = blockify(d);
    if (d == CSS_DISPLAY_CONTENTS) d = CSS_DISPLAY_INLINE;

    if (tag == LXB_TAG_BR) {
        /* <br> (and the `</br>` Lexbor already turns into one): forced line break */
        Box *br = layout_new_box(t, BOX_LINEBREAK, node, style);
        br->flags |= BOXF_INLINE_LEVEL;
        layout_resolve_font(t, br);
        if (parent) layout_append_child(parent, br);
        ElemData *bed = layout_elem_data(node);
        if (bed && !bed->box) bed->box = br;
        return br;
    }
    Box *b = layout_new_box(t, type_for_display(d, replaced ? tag : 0), node, style);
    b->pseudo = (uint8_t)pseudo;
    if (pseudo == 1) b->flags |= BOXF_PSEUDO_BEFORE;
    if (pseudo == 2) b->flags |= BOXF_PSEUDO_AFTER;
    b->parent = parent;                /* needed by set_flags (root test) */
    layout_resolve_font(t, b);
    set_flags_from_style(t, b, parent_fg);
    if (replaced) {
        b->flags &= ~BOXF_BLOCK_LEVEL;
        if (display_is_inline_level(d) && !oof && style->float_ == CSS_FLOAT_NONE && !parent_fg) b->flags |= BOXF_INLINE_LEVEL | BOXF_ATOMIC;
        else b->flags |= BOXF_BLOCK_LEVEL;
        b->flags |= BOXF_BFC_ROOT;
        replaced_intrinsics(t, b);
    }
    if (parent) layout_append_child(parent, b);
    if (!pseudo) {
        ElemData *ed = layout_elem_data(node);
        if (ed && !ed->box) ed->box = b;
        if (!replaced) build_children(t, b, node);
    }
    return b;
}

/* `text_style`: the style of text children (NULL = the parent box's; set while expanding a
 * display:contents element, whose children are built into the nearest box). */
static void build_children_styled(LayoutTree *t, Box *parent, lxb_dom_node_t *elem, const CssComputed *text_style)
{
    ElemData *ed = layout_elem_data(elem);
    if (ed && ed->before) build_pseudo(t, parent, elem, ed->before, 1);
    for (lxb_dom_node_t *c = elem->first_child; c; c = c->next) {
        if (c->type == LXB_DOM_NODE_TYPE_TEXT) {
            lexbor_str_t *d = &lxb_dom_interface_text(c)->char_data.data;
            if (d->length) build_text_box_styled(t, parent, c, (const char *)d->data, d->length, text_style);
        } else if (c->type == LXB_DOM_NODE_TYPE_ELEMENT) {
            const int tag = node_tag(c);
            if (tag_is_skipped(tag)) continue;
            ElemData *ced = layout_elem_data(c);
            if (!ced || !ced->computed) continue;
            if (ced->computed->display == CSS_DISPLAY_NONE) continue;
            /* display: contents (CSS Display 3 2.5): no box of its own, its children (and
             * ::before/::after) are laid out as children of the parent box, e.g. flex items
             * of v7's #headerRight. Replaced elements keep their box (they behave like
             * display: none there in Chrome, but none of the pages uses that). */
            if (ced->computed->display == CSS_DISPLAY_CONTENTS && !replaced_type_for_tag(tag)) {
                build_children_styled(t, parent, c, ced->computed);
                continue;
            }
            build_element(t, parent, c, ced->computed, 0);
        }
    }
    if (ed && ed->after) build_pseudo(t, parent, elem, ed->after, 2);
}

static void build_children(LayoutTree *t, Box *parent, lxb_dom_node_t *elem)
{
    build_children_styled(t, parent, elem, NULL);
}

/* ------------------------------------------------------------------------- */
/* anonymous boxes                                                            */
/* ------------------------------------------------------------------------- */

/* Anonymous boxes inherit only inherited properties; everything else is
 * initial (CSS 2.1 9.2.1.1 / 17.2.1), with the display their role needs. */
void layout_anon_inherit(Box *a, const CssComputed *parent_style)
{
    if (!a->own_style) return;
    css_computed_init_defaults(a->own_style);
    if (parent_style) css_computed_inherit(a->own_style, parent_style);
    uint8_t d = CSS_DISPLAY_BLOCK;
    if (a->type == BOX_TABLE) d = CSS_DISPLAY_TABLE;
    else if (a->type == BOX_TABLE_ROW) d = CSS_DISPLAY_TABLE_ROW;
    else if (a->type == BOX_TABLE_CELL) d = CSS_DISPLAY_TABLE_CELL;
    a->own_style->display = d;
    a->style = a->own_style;
}

void layout_restyle_anon_children(Box *b)
{
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->flags & BOXF_ANONYMOUS) {
            layout_anon_inherit(c, b->style);
            c->dirty |= BOX_DIRTY_SELF;
            c->paint_dirty = 1;
            layout_restyle_anon_children(c);
        } else if (c->type == BOX_TEXT) {
            c->style = b->style;
            c->dirty |= BOX_DIRTY_SELF;
        }
    }
}

static Box *new_anon(LayoutTree *t, uint8_t type, Box *parent, uint32_t extra_flags)
{
    Box *a = layout_new_box(t, type, NULL, parent->style);
    a->flags |= BOXF_ANONYMOUS | extra_flags;
    a->own_style = malloc(sizeof(CssComputed));
    if (a->own_style) layout_anon_inherit(a, parent->style);
    a->font = parent->font;
    a->fm = parent->fm;
    return a;
}

static int is_ws_text(const Box *b) { return b->type == BOX_TEXT && text_is_whitespace_only(b->text, b->text_len); }

static void release_rec(LayoutTree *t, Box *b);
/* A box removed from the tree during wrapping (whitespace text, empty anonymous blocks). */
static void drop_box(LayoutTree *t, Box *b)
{
    b->parent = NULL;
    b->prev = NULL;
    release_rec(t, b);
}

/* Does an inline subtree contain a block-level in-flow descendant? */
static int inline_has_block_child(const Box *b)
{
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->type == BOX_INLINE) { if (inline_has_block_child(c)) return 1; }
        else if ((c->flags & BOXF_BLOCK_LEVEL) && box_is_inflow(c)) return 1;
    }
    return 0;
}

/* Split block-level children out of an inline chain.  `<span><div/>x</span>`
 * becomes: span-shell, div, span-shell' [x] inside the block container, like
 * Chrome's inline continuations.  Shells are clones of the inline boxes; the
 * element's ElemData.box keeps pointing at the first one. */
#define FLATTEN_MAX_DEPTH 16
typedef struct InlineChain {
    Box *src[FLATTEN_MAX_DEPTH];
    Box *cur[FLATTEN_MAX_DEPTH];
    int depth;
} InlineChain;

static Box *clone_inline_shell(LayoutTree *t, const Box *src)
{
    Box *c = layout_new_box(t, BOX_INLINE, src->node, src->style);
    c->flags = src->flags;
    c->font = src->font; c->fm = src->fm;
    c->vertical_align = src->vertical_align;
    c->z_index = src->z_index; c->z_auto = src->z_auto;
    c->pseudo = src->pseudo;
    return c;
}

static Box *chain_ensure(LayoutTree *t, Box *out, InlineChain *ch)
{
    for (int i = 0; i < ch->depth; i++) {
        if (ch->cur[i]) continue;
        Box *shell = clone_inline_shell(t, ch->src[i]);
        layout_append_child(i == 0 ? out : ch->cur[i - 1], shell);
        ch->cur[i] = shell;
        for (int j = i + 1; j < ch->depth; j++) ch->cur[j] = NULL;
    }
    return ch->cur[ch->depth - 1];
}

static void chain_close(InlineChain *ch)
{
    for (int i = 0; i < ch->depth; i++) ch->cur[i] = NULL;
}

static void flatten_children(LayoutTree *t, Box *out, InlineChain *ch, Box *src)
{
    Box *first;
    detach_all_children(src, &first);
    for (Box *c = first; c;) {
        Box *next = c->next;
        c->parent = NULL; c->next = c->prev = NULL;
        if (c->type == BOX_INLINE && inline_has_block_child(c) && ch->depth < FLATTEN_MAX_DEPTH) {
            ch->src[ch->depth] = c;
            ch->cur[ch->depth] = NULL;
            ch->depth++;
            flatten_children(t, out, ch, c);
            ch->depth--;
        } else if ((c->flags & BOXF_BLOCK_LEVEL) && box_is_inflow(c)) {
            chain_close(ch);
            layout_append_child(out, c);
        } else {
            layout_append_child(chain_ensure(t, out, ch), c);
        }
        c = next;
    }
}

static void wrap_anonymous(LayoutTree *t, Box *b);

/* Block container: if it has both block-level and inline-level in-flow
 * children, wrap inline runs in anonymous block boxes. */
static void wrap_block_container(LayoutTree *t, Box *b)
{
    /* first: flatten inline boxes containing blocks */
    int needs_flatten = 0;
    for (Box *c = b->first_child; c; c = c->next)
        if (c->type == BOX_INLINE && inline_has_block_child(c)) { needs_flatten = 1; break; }
    if (needs_flatten) {
        Box *first;
        detach_all_children(b, &first);
        for (Box *c = first; c;) {
            Box *next = c->next;
            c->parent = NULL; c->next = c->prev = NULL;
            if (c->type == BOX_INLINE && inline_has_block_child(c)) {
                InlineChain ch;
                memset(&ch, 0, sizeof(ch));
                ch.src[0] = c;
                ch.depth = 1;
                flatten_children(t, b, &ch, c);
            } else {
                layout_append_child(b, c);
            }
            c = next;
        }
    }

    int has_block = 0, has_inline = 0;
    for (Box *c = b->first_child; c; c = c->next) {
        if (!box_is_inflow(c)) continue;
        if (c->flags & BOXF_INLINE_LEVEL) { if (!is_ws_text(c)) has_inline = 1; }
        else has_block = 1;
    }
    if (has_block && has_inline) {
        Box *first;
        detach_all_children(b, &first);
        Box *anon = NULL;
        for (Box *c = first; c;) {
            Box *next = c->next;
            c->parent = NULL; c->next = c->prev = NULL;
            int inline_like = (c->flags & BOXF_INLINE_LEVEL) || !box_is_inflow(c);
            if (!(c->flags & BOXF_INLINE_LEVEL) && box_is_inflow(c)) inline_like = 0;
            if (inline_like) {
                if (is_ws_text(c) && !anon) { drop_box(t, c); c = next; continue; }   /* whitespace between blocks: dropped */
                if (!anon) {
                    anon = new_anon(t, BOX_BLOCK, b, BOXF_BLOCK_LEVEL);
                    layout_append_child(b, anon);
                }
                layout_append_child(anon, c);
            } else {
                layout_append_child(b, c);
                anon = NULL;
            }
            c = next;
        }
        /* drop anonymous blocks that ended up whitespace-only; normalise the rest */
        for (Box *c = b->first_child; c;) {
            Box *next = c->next;
            if ((c->flags & BOXF_ANONYMOUS) && c->type == BOX_BLOCK) {
                int empty = 1;
                for (Box *g = c->first_child; g; g = g->next) if (!is_ws_text(g)) { empty = 0; break; }
                if (empty) {
                    if (c->prev) c->prev->next = c->next; else b->first_child = c->next;
                    if (c->next) c->next->prev = c->prev; else b->last_child = c->prev;
                    /* its children (whitespace, maybe floats/abs) go too: keep out-of-flow ones */
                    Box *g = c->first_child;
                    c->first_child = c->last_child = NULL;
                    while (g) {
                        Box *gn = g->next;
                        if (!box_is_inflow(g) && g->type != BOX_TEXT) {
                            g->parent = b;
                            g->next = next;
                            g->prev = next ? next->prev : b->last_child;
                            if (g->prev) g->prev->next = g; else b->first_child = g;
                            if (next) next->prev = g; else b->last_child = g;
                        } else {
                            g->next = NULL;
                            drop_box(t, g);
                        }
                        g = gn;
                    }
                    c->next = NULL;
                    drop_box(t, c);
                } else {
                    wrap_block_container(t, c);
                }
            }
            c = next;
        }
    } else if (has_block) {
        /* remove whitespace-only text between blocks (and inline-level oof boxes stay) */
        for (Box *c = b->first_child; c;) {
            Box *next = c->next;
            if (is_ws_text(c)) {
                if (c->prev) c->prev->next = c->next; else b->first_child = c->next;
                if (c->next) c->next->prev = c->prev; else b->last_child = c->prev;
                c->next = NULL;
                drop_box(t, c);
            }
            c = next;
        }
    }
    /* block containers with only inline content: inline formatting */
    int all_inline = 1;
    for (Box *c = b->first_child; c; c = c->next)
        if (box_is_inflow(c) && !(c->flags & BOXF_INLINE_LEVEL)) { all_inline = 0; break; }
    if (all_inline && b->first_child) b->flags |= BOXF_HAS_LINES;
    else b->flags &= ~BOXF_HAS_LINES;
}

/* Flex / grid container: in-flow children are items; text runs are wrapped
 * in anonymous block items, whitespace-only text is dropped. */
static void wrap_flex_grid(LayoutTree *t, Box *b)
{
    Box *first;
    detach_all_children(b, &first);
    Box *anon = NULL;
    for (Box *c = first; c;) {
        Box *next = c->next;
        c->parent = NULL; c->next = c->prev = NULL;
        if (!box_is_inflow(c)) { layout_append_child(b, c); anon = NULL; c = next; continue; }
        if (c->type == BOX_TEXT || c->type == BOX_INLINE || c->type == BOX_LINEBREAK
            || ((c->flags & BOXF_INLINE_LEVEL) && !(c->flags & BOXF_ATOMIC))) {
            if (is_ws_text(c)) { drop_box(t, c); c = next; continue; }
            if (!anon) {
                anon = new_anon(t, BOX_BLOCK, b, BOXF_BLOCK_LEVEL | BOXF_BFC_ROOT);
                layout_append_child(b, anon);
            }
            layout_append_child(anon, c);
        } else {
            /* atomic inline-level items are blockified flex items */
            c->flags &= ~(BOXF_INLINE_LEVEL | BOXF_ATOMIC);
            c->flags |= BOXF_BLOCK_LEVEL | BOXF_BFC_ROOT;
            layout_append_child(b, c);
            anon = NULL;
        }
        c = next;
    }
    for (Box *c = b->first_child; c; c = c->next)
        if ((c->flags & BOXF_ANONYMOUS) && c->type == BOX_BLOCK) wrap_block_container(t, c);
}

/* Row groups (thead/tbody/tfoot, display: table-row-group) are transparent
 * for this table subset: their rows become direct rows of the table, in
 * document order.  The HTML parser always inserts <tbody> into <table>. */
static int is_row_group(const Box *c)
{
    return !(c->flags & BOXF_ANONYMOUS) && c->node && box_is_inflow(c)
        && c->style->display == CSS_DISPLAY_TABLE_ROW_GROUP;
}

static void hoist_row_groups(LayoutTree *t, Box *b)
{
    int any = 0;
    for (Box *c = b->first_child; c; c = c->next) if (is_row_group(c)) { any = 1; break; }
    if (!any) return;
    Box *first;
    detach_all_children(b, &first);
    for (Box *c = first; c;) {
        Box *next = c->next;
        c->parent = NULL; c->next = c->prev = NULL;
        if (is_row_group(c)) {
            Box *g = c->first_child;
            c->first_child = c->last_child = NULL;
            while (g) {
                Box *gn = g->next;
                g->parent = NULL; g->next = g->prev = NULL;
                layout_append_child(b, g);       /* nested groups are flattened by the recursion below */
                g = gn;
            }
            drop_box(t, c);
        } else {
            layout_append_child(b, c);
        }
        c = next;
    }
    hoist_row_groups(t, b);   /* nested row groups */
}

/* Table: children must be rows containing cells. */
static void wrap_table(LayoutTree *t, Box *b)
{
    hoist_row_groups(t, b);
    Box *first;
    detach_all_children(b, &first);
    Box *anon_row = NULL;
    for (Box *c = first; c;) {
        Box *next = c->next;
        c->parent = NULL; c->next = c->prev = NULL;
        if (!box_is_inflow(c)) { layout_append_child(b, c); c = next; continue; }
        if (c->type == BOX_TABLE_ROW) { layout_append_child(b, c); anon_row = NULL; }
        else {
            if (is_ws_text(c)) { drop_box(t, c); c = next; continue; }
            if (!anon_row) {
                anon_row = new_anon(t, BOX_TABLE_ROW, b, BOXF_BLOCK_LEVEL);
                layout_append_child(b, anon_row);
            }
            layout_append_child(anon_row, c);
        }
        c = next;
    }
    /* rows: children must be cells */
    for (Box *r = b->first_child; r; r = r->next) {
        if (r->type != BOX_TABLE_ROW) continue;
        Box *rf;
        detach_all_children(r, &rf);
        Box *anon_cell = NULL;
        for (Box *c = rf; c;) {
            Box *next = c->next;
            c->parent = NULL; c->next = c->prev = NULL;
            if (!box_is_inflow(c)) { layout_append_child(r, c); c = next; continue; }
            if (c->type == BOX_TABLE_CELL) { layout_append_child(r, c); anon_cell = NULL; }
            else {
                if (is_ws_text(c)) { drop_box(t, c); c = next; continue; }
                if (!anon_cell) {
                    anon_cell = new_anon(t, BOX_TABLE_CELL, b, BOXF_BLOCK_LEVEL | BOXF_BFC_ROOT);
                    layout_append_child(r, anon_cell);
                }
                /* cell content: blockify atomic inline-level? no: keep as inline content */
                layout_append_child(anon_cell, c);
            }
            c = next;
        }
        for (Box *c = r->first_child; c; c = c->next) wrap_block_container(t, c);
    }
}

/* Table cells / rows outside a table get an anonymous table wrapper. */
static void wrap_stray_table_parts(LayoutTree *t, Box *b)
{
    int has_part = 0;
    for (Box *c = b->first_child; c; c = c->next)
        if (box_is_inflow(c) && (c->type == BOX_TABLE_CELL || c->type == BOX_TABLE_ROW)) { has_part = 1; break; }
    if (!has_part) return;
    Box *first;
    detach_all_children(b, &first);
    Box *tbl = NULL;
    for (Box *c = first; c;) {
        Box *next = c->next;
        c->parent = NULL; c->next = c->prev = NULL;
        if (box_is_inflow(c) && (c->type == BOX_TABLE_CELL || c->type == BOX_TABLE_ROW)) {
            if (!tbl) {
                tbl = new_anon(t, BOX_TABLE, b, BOXF_BLOCK_LEVEL | BOXF_BFC_ROOT);
                layout_append_child(b, tbl);
            }
            layout_append_child(tbl, c);
        } else {
            if (tbl && is_ws_text(c)) { drop_box(t, c); c = next; continue; }
            layout_append_child(b, c);
            tbl = NULL;
        }
        c = next;
    }
}

static void wrap_anonymous(LayoutTree *t, Box *b)
{
    /* bottom-up so that children are normalised before the parent looks at them */
    for (Box *c = b->first_child; c; c = c->next) wrap_anonymous(t, c);
    switch (b->type) {
    case BOX_FLEX: case BOX_GRID: wrap_flex_grid(t, b); break;
    case BOX_TABLE: wrap_table(t, b); break;
    case BOX_BLOCK: case BOX_TABLE_CELL:
        wrap_stray_table_parts(t, b);
        for (Box *c = b->first_child; c; c = c->next)
            if ((c->flags & BOXF_ANONYMOUS) && c->type == BOX_TABLE) wrap_table(t, c);
        wrap_block_container(t, b);
        break;
    case BOX_TABLE_ROW:
        /* handled by the parent table; stray rows become blocks */
        break;
    default: break;
    }
}

/* ------------------------------------------------------------------------- */
/* containers / scrollers / abs lists                                         */
/* ------------------------------------------------------------------------- */

static void abs_list_push(Box *cb, Box *child)
{
    if (cb->abs_count == cb->abs_cap) {
        int ncap = cb->abs_cap ? cb->abs_cap * 2 : 4;
        Box **n = realloc(cb->abs_children, (size_t)ncap * sizeof(Box *));
        if (!n) return;
        cb->abs_children = n;
        cb->abs_cap = ncap;
    }
    cb->abs_children[cb->abs_count++] = child;
}

static void assign_containers(LayoutTree *t, Box *b, Box *root)
{
    if (b == root) {
        b->container = NULL;
        b->scroller = NULL;
    } else {
        Box *p = b->parent;
        if (b->flags & BOXF_OUT_OF_FLOW) {
            Box *cb = p;
            int fixed = b->style->position == CSS_POSITION_FIXED;
            while (cb && cb != root) {
                if ((cb->flags & BOXF_HAS_TRANSFORM) || (!fixed && (cb->flags & BOXF_POSITIONED))) break;
                cb = cb->parent;
            }
            if (!cb) cb = root;
            if (cb == root && !(root->flags & BOXF_HAS_TRANSFORM) && !(!fixed && (root->flags & BOXF_POSITIONED))) {
                /* the initial containing block: modelled by the root box */
                cb = root;
            }
            /* an inline containing block: use the nearest block container of it */
            while (cb->type == BOX_INLINE && cb->parent) cb = cb->parent;
            b->container = cb;
            abs_list_push(cb, b);
        } else {
            Box *cb = p;
            while (cb && cb->type == BOX_INLINE) cb = cb->parent;
            b->container = cb ? cb : root;
        }
        /* nearest scroll container ancestor along the container chain */
        Box *s = b->container;
        while (s && !(s->flags & BOXF_SCROLL_CONTAINER)) s = s->container;
        b->scroller = s;
    }
    for (Box *c = b->first_child; c; c = c->next) assign_containers(t, c, root);
}

/* ------------------------------------------------------------------------- */
/* entry                                                                      */
/* ------------------------------------------------------------------------- */

static void mark_dirty_tree(Box *b)
{
    b->dirty = BOX_DIRTY_SELF | BOX_DIRTY_CHILD;
    for (Box *c = b->first_child; c; c = c->next) mark_dirty_tree(c);
}

LayoutTree *layout_build(Dom *dom, LayoutCtx *ctx)
{
    LayoutTree *t = calloc(1, sizeof(LayoutTree));
    if (!t) return NULL;
    t->ctx = ctx;
    t->dom = dom;
    kn_arena_init(&t->arena, 256 * 1024);
    t->anon_style.font_size = 16.0f;
    t->anon_style.font_weight = 400;
    t->anon_style.line_height.kind = CSS_LEN_NORMAL;
    t->anon_style.display = CSS_DISPLAY_BLOCK;
    t->anon_style.max_width.kind = CSS_LEN_NONE;
    t->anon_style.max_height.kind = CSS_LEN_NONE;
    t->anon_style.z_index_auto = 1;
    t->anon_style.opacity = 1.0f;
    t->anon_style.flex_shrink = 1.0f;
    t->anon_style.flex_basis.kind = CSS_LEN_AUTO;
    t->anon_style.width.kind = t->anon_style.height.kind = CSS_LEN_AUTO;
    for (int i = 0; i < 4; i++) t->anon_style.inset[i].kind = CSS_LEN_AUTO;

    lxb_dom_document_t *doc = lxb_dom_interface_document(dom_document(dom));
    lxb_dom_element_t *html = lxb_dom_document_element(doc);
    if (!html) return t;
    lxb_dom_node_t *hn = lxb_dom_interface_node(html);
    ElemData *ed = layout_elem_data(hn);
    const CssComputed *style = (ed && ed->computed) ? ed->computed : &t->anon_style;
    if (style->display == CSS_DISPLAY_NONE) return t;

    Box *root = build_element(t, NULL, hn, style, 0);
    if (!root) return t;
    root->flags |= BOXF_BLOCK_LEVEL | BOXF_BFC_ROOT | BOXF_STACKING_CONTEXT;
    root->flags &= ~BOXF_INLINE_LEVEL;
    if (root->type == BOX_INLINE) root->type = BOX_BLOCK;
    t->root = root;
    wrap_anonymous(t, root);
    assign_containers(t, root, root);
    mark_dirty_tree(root);
    return t;
}

static void release_box_memory(Box *b)
{
    layout_free_box_arrays(b);
    free(b->own_style);
    b->own_style = NULL;
    if (b->type == BOX_TEXT) free((void *)b->text);
    b->text = NULL;
    free((void *)b->src);
    b->src = NULL;
    if (b->node && !b->pseudo) {
        ElemData *ed = layout_elem_data(b->node);
        if (ed && ed->box == b) ed->box = NULL;
    }
}

static void free_box_tree(Box *b)
{
    for (Box *c = b->first_child; c; c = c->next) free_box_tree(c);
    release_box_memory(b);
}

/* ------------------------------------------------------------------------- */
/* incremental rebuilds                                                       */
/* ------------------------------------------------------------------------- */

static void abs_list_remove(Box *cb, Box *child)
{
    for (int i = 0; i < cb->abs_count; i++) {
        if (cb->abs_children[i] != child) continue;
        memmove(cb->abs_children + i, cb->abs_children + i + 1, (size_t)(cb->abs_count - i - 1) * sizeof(Box *));
        cb->abs_count--;
        return;
    }
}

static void unlink_abs_rec(Box *b)
{
    for (Box *c = b->first_child; c; c = c->next) unlink_abs_rec(c);
    if ((b->flags & BOXF_OUT_OF_FLOW) && b->container) abs_list_remove(b->container, b);
}

static void release_rec(LayoutTree *t, Box *b)
{
    Box *c = b->first_child;
    while (c) { Box *n = c->next; release_rec(t, c); c = n; }
    release_box_memory(b);
    b->next = t->free_list;
    t->free_list = b;
}

/* Nearest box that can own a children rebuild: a non-inline, non-anonymous
 * element box (inline boxes take part in their container's anonymous wrapping). */
static Box *rebuild_root_for_node(lxb_dom_node_t *node)
{
    for (lxb_dom_node_t *n = node; n; n = n->parent) {
        if (n->type != LXB_DOM_NODE_TYPE_ELEMENT) continue;
        ElemData *ed = layout_elem_data(n);
        Box *b = ed ? (Box *)ed->box : NULL;
        if (!b) continue;
        if (b->type == BOX_INLINE || b->type == BOX_TEXT || b->type == BOX_LINEBREAK || (b->flags & BOXF_ANONYMOUS)) continue;
        if (b->type == BOX_TABLE_ROW || box_is_replaced(b)) continue;
        return b;
    }
    return NULL;
}

static void rebuild_children(LayoutTree *t, Box *r)
{
    for (Box *c = r->first_child; c; c = c->next) unlink_abs_rec(c);
    Box *c = r->first_child;
    while (c) { Box *n = c->next; release_rec(t, c); c = n; }
    r->first_child = r->last_child = NULL;
    layout_free_lines(r);
    r->flags &= ~BOXF_HAS_LINES;
    if (r->node && r->node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        ElemData *ed = layout_elem_data(r->node);
        if (ed && ed->computed && !r->pseudo) r->style = ed->computed;
        build_children(t, r, r->node);
    }
    wrap_anonymous(t, r);
    for (Box *k = r->first_child; k; k = k->next) assign_containers(t, k, t->root);
    layout_invalidate(t, r, 0);
}

static void rebuild_all(LayoutTree *t)
{
    Dom *dom = t->dom;
    if (t->root) {
        free_box_tree(t->root);
        t->root = NULL;
    }
    t->free_list = NULL;
    kn_arena_reset(&t->arena);
    t->laid_out_once = 0;
    lxb_dom_element_t *html = lxb_dom_document_element(lxb_dom_interface_document(dom_document(dom)));
    if (!html) return;
    lxb_dom_node_t *hn = lxb_dom_interface_node(html);
    ElemData *ed = layout_elem_data(hn);
    const CssComputed *style = (ed && ed->computed) ? ed->computed : &t->anon_style;
    if (style->display == CSS_DISPLAY_NONE) return;
    Box *root = build_element(t, NULL, hn, style, 0);
    if (!root) return;
    root->flags |= BOXF_BLOCK_LEVEL | BOXF_BFC_ROOT | BOXF_STACKING_CONTEXT;
    root->flags &= ~BOXF_INLINE_LEVEL;
    if (root->type == BOX_INLINE) root->type = BOX_BLOCK;
    t->root = root;
    wrap_anonymous(t, root);
    assign_containers(t, root, root);
    mark_dirty_tree(root);
}

static int rebuild_for_node(LayoutTree *t, lxb_dom_node_t *node)
{
    Box *r = rebuild_root_for_node(node);
    if (!r || r == t->root) { rebuild_all(t); return LAYOUT_RESTYLE_REBUILT; }
    rebuild_children(t, r);
    return LAYOUT_RESTYLE_REBUILT;
}

int layout_children_changed(LayoutTree *t, lxb_dom_node_t *node)
{
    if (!t || !node) return LAYOUT_RESTYLE_NONE;
    ElemData *ed = layout_elem_data(node);
    if (!ed || !ed->box) {
        /* display: contents: its children live in the nearest ancestor box */
        if (ed && ed->computed && ed->computed->display == CSS_DISPLAY_CONTENTS && rebuild_root_for_node(node))
            return rebuild_for_node(t, node);
        /* not rendered (display:none or inside one): nothing to do unless an ancestor renders it */
        return LAYOUT_RESTYLE_NONE;
    }
    return rebuild_for_node(t, node);
}

void layout_rebuild_all(LayoutTree *t)
{
    if (t) rebuild_all(t);
}

static int len_eq(const CssLength *a, const CssLength *b)
{
    return a->kind == b->kind && a->px == b->px && a->pct == b->pct;
}

static float used_border(const CssComputed *s, int i)
{
    return (s->border_style[i] == CSS_BORDER_STYLE_NONE || s->border_style[i] == CSS_BORDER_STYLE_HIDDEN) ? 0.0f : s->border_width[i];
}

/* Properties whose change needs a new box (sub)tree. */
static int structural_changed(const CssComputed *a, const CssComputed *b)
{
    return a->display != b->display || a->position != b->position || a->float_ != b->float_
        || a->overflow_x != b->overflow_x || a->overflow_y != b->overflow_y
        || (a->clip_path.kind != CSS_CLIP_PATH_NONE) != (b->clip_path.kind != CSS_CLIP_PATH_NONE)
        || a->content.kind != b->content.kind || a->content.str != b->content.str || a->content.base != b->content.base;
}

/* Properties layout reads (everything else is paint-only). */
static int layout_changed(const CssComputed *a, const CssComputed *b)
{
    if (a->font_family_count != b->font_family_count || memcmp(a->font_families, b->font_families, sizeof(a->font_families))) return 1;
    if (a->font_size != b->font_size || a->font_weight != b->font_weight || a->font_style != b->font_style) return 1;
    if (!len_eq(&a->line_height, &b->line_height) || !len_eq(&a->letter_spacing, &b->letter_spacing)) return 1;
    if (a->text_align != b->text_align || a->text_transform != b->text_transform || a->white_space != b->white_space
        || a->overflow_wrap != b->overflow_wrap || a->word_break != b->word_break || a->direction != b->direction
        || a->border_collapse != b->border_collapse || !len_eq(&a->border_spacing_x, &b->border_spacing_x)
        || !len_eq(&a->border_spacing_y, &b->border_spacing_y)) return 1;
    if (a->clear != b->clear || a->box_sizing != b->box_sizing || a->appearance != b->appearance || a->table_layout != b->table_layout) return 1;
    for (int i = 0; i < 4; i++) {
        if (!len_eq(&a->inset[i], &b->inset[i]) || !len_eq(&a->margin[i], &b->margin[i]) || !len_eq(&a->padding[i], &b->padding[i])) return 1;
        if (used_border(a, i) != used_border(b, i)) return 1;
    }
    if (!len_eq(&a->width, &b->width) || !len_eq(&a->height, &b->height) || !len_eq(&a->min_width, &b->min_width)
        || !len_eq(&a->min_height, &b->min_height) || !len_eq(&a->max_width, &b->max_width) || !len_eq(&a->max_height, &b->max_height)) return 1;
    if (a->text_overflow != b->text_overflow || a->vertical_align != b->vertical_align || !len_eq(&a->vertical_align_length, &b->vertical_align_length)) return 1;
    if (a->flex_direction != b->flex_direction || a->flex_wrap != b->flex_wrap || a->justify_content != b->justify_content || a->align_content != b->align_content
        || a->align_items != b->align_items || a->flex_grow != b->flex_grow || a->flex_shrink != b->flex_shrink
        || !len_eq(&a->flex_basis, &b->flex_basis)) return 1;
    if (a->grid_column_count != b->grid_column_count || !len_eq(&a->row_gap, &b->row_gap) || !len_eq(&a->column_gap, &b->column_gap)) return 1;
    for (int i = 0; i < a->grid_column_count; i++) {
        const CssTrack *ta = &a->grid_columns[i], *tb = &b->grid_columns[i];
        if (ta->kind != tb->kind || ta->fr != tb->fr || !len_eq(&ta->size, &tb->size) || !len_eq(&ta->max, &tb->max)) return 1;
    }
    if (a->grid_row_count != b->grid_row_count || memcmp(a->grid_lines, b->grid_lines, sizeof(a->grid_lines))
        || a->justify_items != b->justify_items || a->justify_self != b->justify_self || a->align_self != b->align_self) return 1;
    for (int i = 0; i < a->grid_row_count; i++) {
        const CssTrack *ta = &a->grid_rows[i], *tb = &b->grid_rows[i];
        if (ta->kind != tb->kind || ta->fr != tb->fr || !len_eq(&ta->size, &tb->size) || !len_eq(&ta->max, &tb->max)) return 1;
    }
    if (memcmp(a->grid_names, b->grid_names, sizeof(a->grid_names)) || a->grid_areas != b->grid_areas
        || a->grid_auto_flow != b->grid_auto_flow || memcmp(a->grid_auto_count, b->grid_auto_count, sizeof(a->grid_auto_count))) return 1;
    for (int x = 0; x < 2; x++)
        for (int i = 0; i < a->grid_auto_count[x]; i++) {
            const CssTrack *ta = &a->grid_auto_tracks[x][i], *tb = &b->grid_auto_tracks[x][i];
            if (ta->kind != tb->kind || ta->fr != tb->fr || !len_eq(&ta->size, &tb->size) || !len_eq(&ta->max, &tb->max)) return 1;
        }
    return 0;
}

static int transform_changed(const CssComputed *a, const CssComputed *b)
{
    return a->transform.count != b->transform.count
        || memcmp(a->transform.ops, b->transform.ops, sizeof(CssTransformOp) * a->transform.count)
        || !len_eq(&a->transform_origin_x, &b->transform_origin_x) || !len_eq(&a->transform_origin_y, &b->transform_origin_y);
}

static int subtree_has_fixed(const Box *b)
{
    for (const Box *c = b->first_child; c; c = c->next) {
        if (c->style && c->style->position == CSS_POSITION_FIXED && c->type != BOX_TEXT) return 1;
        if (subtree_has_fixed(c)) return 1;
    }
    return 0;
}

static int pseudo_mismatch(const Box *b, const ElemData *ed)
{
    int have_before = 0, have_after = 0;
    for (const Box *c = b->first_child; c; c = c->next) {
        if (c->flags & BOXF_PSEUDO_BEFORE) have_before = 1;
        if (c->flags & BOXF_PSEUDO_AFTER) have_after = 1;
    }
    int want_before = ed->before && ed->before->display != CSS_DISPLAY_NONE
                   && (ed->before->content.kind == CSS_CONTENT_STRING || ed->before->content.kind == CSS_CONTENT_ATTR);
    int want_after = ed->after && ed->after->display != CSS_DISPLAY_NONE
                   && (ed->after->content.kind == CSS_CONTENT_STRING || ed->after->content.kind == CSS_CONTENT_ATTR);
    /* pseudo boxes with content are rebuilt whenever their element restyles (cheap, small) */
    return have_before != want_before || have_after != want_after || have_before || have_after;
}

/* All boxes of an element: the principal box plus block-in-inline continuations. */
static void refresh_box_in_place(LayoutTree *t, Box *b, const CssComputed *s)
{
    b->style = s;
    int parent_fg = b->parent && (b->parent->type == BOX_FLEX || b->parent->type == BOX_GRID);
    set_dynamic_flags(t, b, parent_fg);
    layout_restyle_anon_children(b);
    for (Box *c = b->first_child; c; c = c->next) {
        if (c->type == BOX_TEXT || (c->flags & BOXF_ANONYMOUS)) {
            c->flags &= ~(uint32_t)(BOXF_HIDDEN | BOXF_NO_POINTER);
            if (s->visibility != CSS_VISIBILITY_VISIBLE) c->flags |= BOXF_HIDDEN;
            if (s->pointer_events == CSS_POINTER_EVENTS_NONE) c->flags |= BOXF_NO_POINTER;
            c->paint_dirty = 1;
        }
    }
    b->paint_dirty = 1;
}

int layout_restyled(LayoutTree *t, lxb_dom_node_t *node, const CssComputed *old_style)
{
    if (!t || !node || node->type != LXB_DOM_NODE_TYPE_ELEMENT) return LAYOUT_RESTYLE_NONE;
    ElemData *ed = layout_elem_data(node);
    if (!ed) return LAYOUT_RESTYLE_NONE;
    const CssComputed *s = ed->computed;
    Box *b = (Box *)ed->box;
    if (!b) {
        if (!s || s->display == CSS_DISPLAY_NONE) return LAYOUT_RESTYLE_NONE;
        /* newly displayed: only when its parent element is rendered (has a box; display:
         * contents builds one too). Inside a display:none ancestor nothing renders, so an
         * animation running there (the menu's #merchImg / #instructions while in game)
         * must not rebuild the nearest rendered ancestor's children on every tick. */
        lxb_dom_node_t *p = node->parent;
        for (;;) { /* display: contents ancestors render through their parent's box */
            while (p && p->type != LXB_DOM_NODE_TYPE_ELEMENT) p = p->parent;
            ElemData *ped = p ? layout_elem_data(p) : NULL;
            if (!ped || ped->box || !ped->computed || ped->computed->display != CSS_DISPLAY_CONTENTS) break;
            p = p->parent;
        }
        ElemData *ped = p ? layout_elem_data(p) : NULL;
        if (!ped || !ped->box) return LAYOUT_RESTYLE_NONE;
        if (!rebuild_root_for_node(node->parent)) return LAYOUT_RESTYLE_NONE;
        return rebuild_for_node(t, node->parent);
    }
    if (!s || s->display == CSS_DISPLAY_NONE || !old_style || structural_changed(old_style, s)) {
        if (b == t->root) { rebuild_all(t); return LAYOUT_RESTYLE_REBUILT; }
        return rebuild_for_node(t, node->parent ? node->parent : node);
    }
    int has_transform_old = old_style->transform.count > 0, has_transform_new = s->transform.count > 0;
    if (has_transform_old != has_transform_new && subtree_has_fixed(b))
        return rebuild_for_node(t, node->parent ? node->parent : node);
    if (pseudo_mismatch(b, ed)) {
        if (b->type == BOX_INLINE) return rebuild_for_node(t, node->parent ? node->parent : node);
        refresh_box_in_place(t, b, s);
        rebuild_children(t, b);
        return LAYOUT_RESTYLE_REBUILT;
    }
    /* in place: principal box + continuation shells (same node, inline) */
    refresh_box_in_place(t, b, s);
    if (b->type == BOX_INLINE && b->parent) {
        for (Box *sib = b->parent->first_child; sib; sib = sib->next)
            if (sib != b && sib->node == node && !sib->pseudo) refresh_box_in_place(t, sib, s);
    }
    if (layout_changed(old_style, s)) {
        layout_invalidate(t, b, 0);
        return LAYOUT_RESTYLE_LAYOUT;
    }
    if (transform_changed(old_style, s)) {
        layout_invalidate(t, b, 1);
        return LAYOUT_RESTYLE_TRANSFORM;
    }
    return LAYOUT_RESTYLE_PAINT;
}

void layout_tree_free(LayoutTree *t)
{
    if (!t) return;
    if (t->root) free_box_tree(t->root);
    for (Box *b = t->free_list; b; b = b->next) { /* memory already released */ }
    kn_arena_free(&t->arena);
    free(t);
}

Box *layout_tree_root(LayoutTree *t) { return t ? t->root : NULL; }
LayoutCtx *layout_tree_ctx(LayoutTree *t) { return t ? t->ctx : NULL; }

Box *layout_box_for_node(lxb_dom_node_t *node)
{
    ElemData *ed = layout_elem_data(node);
    return ed ? (Box *)ed->box : NULL;
}

/* ------------------------------------------------------------------------- */
/* debug dump                                                                 */
/* ------------------------------------------------------------------------- */

static const char *type_name(int t)
{
    static const char *n[] = { "block", "inline", "text", "br", "img", "canvas", "video", "input", "select",
                               "flex", "grid", "table", "row", "cell" };
    return (t >= 0 && t < BOX_TYPE_COUNT) ? n[t] : "?";
}

static void dump_rec(const Box *b, KnStrBuf *out, int depth)
{
    for (int i = 0; i < depth; i++) kn_sb_append_cstr(out, "  ");
    kn_sb_append_cstr(out, type_name(b->type));
    if (b->flags & BOXF_ANONYMOUS) kn_sb_append_cstr(out, "(anon)");
    if (b->pseudo) kn_sb_append_cstr(out, b->pseudo == 1 ? "::before" : "::after");
    if (b->node && b->node->type == LXB_DOM_NODE_TYPE_ELEMENT) {
        size_t len;
        const lxb_char_t *nm = lxb_dom_element_qualified_name(lxb_dom_interface_element(b->node), &len);
        kn_sb_append_cstr(out, " <");
        kn_sb_append(out, (const char *)nm, len);
        const char *id = elem_attr(b->node, "id", &len);
        if (id) { kn_sb_append_cstr(out, " #"); kn_sb_append(out, id, len); }
        const char *cls = elem_attr(b->node, "class", &len);
        if (cls) { kn_sb_append_cstr(out, " ."); kn_sb_append(out, cls, len); }
        kn_sb_append_cstr(out, ">");
    }
    if (b->type == BOX_TEXT) {
        kn_sb_append_cstr(out, " \"");
        uint32_t n = b->text_len < 24 ? b->text_len : 24;
        kn_sb_append(out, b->text, n);
        kn_sb_append_cstr(out, b->text_len > 24 ? "...\"" : "\"");
    }
    kn_sb_appendf(out, " @(%.1f,%.1f) %.1fx%.1f", b->x, b->y, b->w, b->h);
    if (b->flags & BOXF_SCROLL_CONTAINER) kn_sb_appendf(out, " scroll(%.0fx%.0f%s)", b->scroll_w, b->scroll_h, (b->flags & BOXF_VBAR) ? " vbar" : "");
    if (b->flags & BOXF_HAS_TRANSFORM) kn_sb_append_cstr(out, " xf");
    if (b->flags & BOXF_LAYOUT_BOUNDARY) kn_sb_append_cstr(out, " boundary");
    kn_sb_append_char(out, '\n');
    for (const Box *c = b->first_child; c; c = c->next) dump_rec(c, out, depth + 1);
}

void layout_dump(const Box *root, KnStrBuf *out)
{
    if (root) dump_rec(root, out, 0);
}
