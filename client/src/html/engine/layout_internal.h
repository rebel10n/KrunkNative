/*
 * layout_internal.h - shared internals of layout_*.c.  Not a public header.
 *
 * During layout every position is computed in container-local coordinates
 * (Box.lx/ly, and line/item/frag/run rects relative to the block container
 * that owns them).  layout_finalize() converts the subtree to the absolute
 * layout space the public header promises.
 */
#ifndef KN_HTML_LAYOUT_INTERNAL_H
#define KN_HTML_LAYOUT_INTERNAL_H

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

#include <lexbor.h>
#include "dom.h"
#include "layout.h"

#define LAYOUT_INDEFINITE (-1.0f)
#define LAYOUT_INF        (1e9f)

struct LayoutTree {
    LayoutCtx *ctx;
    KnArena arena;
    Box *root;
    Dom *dom;
    float viewport_w, viewport_h;
    uint32_t generation;        /* bumped per layout_run, invalidates intrinsic caches */
    uint32_t run_seq;           /* bumped per layout_run: Box.bar_run names the run a bar state belongs to */
    int laid_out_once;
    CssComputed anon_style;     /* fallback style for anonymous boxes without a parent */
    Box *free_list;             /* released boxes (incremental rebuilds), linked via next */
    int memo_off;               /* 1: no memo is served (full_layout); results are still stored */
    /* while a flex row measures its items' content sizes (layout_flex.c): the container's width, which
       a percentage max-width resolves against (a grid's auto-repeat count, layout_grid_intrinsic) */
    int intrinsic_cb_set;
    float intrinsic_cb_w;
    /* KN_HTML_LAYOUT_TRACE=1: one stderr line per layout_run with the counters of LayoutRunStats
       (layout_block.c trace_end).  trace: 0 = env not read yet, 1 = off, 2 = on.  The counters are
       kept either way: the layout tests read them (layout_tree_stats). */
    int trace;
    LayoutRunStats stats;
};

/* Floats of one block formatting context, margin boxes in BFC-root border-box coords. */
typedef struct FloatEntry {
    Box *box;
    float x, y, w, h;
    uint8_t side;               /* CSS_FLOAT_LEFT / RIGHT                      */
} FloatEntry;

typedef struct FloatCtx {
    FloatEntry *data;
    size_t len, cap;
} FloatCtx;

/* Constraints handed to a box by its formatting context. */
typedef struct LayoutInput {
    float cb_w;                 /* containing block content width (% base)     */
    float cb_h;                 /* containing block height or LAYOUT_INDEFINITE */
    float avail_w;              /* width available for auto width (margin box) */
    float bfc_x, bfc_y;         /* box border-box origin in BFC-root coords    */
    FloatCtx *floats;           /* enclosing BFC floats (NULL: none / own BFC) */
    int shrink_to_fit;          /* auto width -> fit-content                   */
    float override_w;           /* border-box width imposed (<0: none)         */
    float override_h;           /* border-box height imposed (<0: none)        */
    int definite_h;             /* 1: override_h / own height is definite for children */
    int is_flex_item;           /* percentage heights resolve against flexed size */
    float self_cb_h;            /* layout_box -> its contents: the box's own containing block
                                   height (its % min/max-height base), LAYOUT_INDEFINITE */
    int measure_only;           /* the caller lays the box out again (its positions come from that
                                   call): only the size is wanted, see layout_box's memo */
} LayoutInput;

typedef struct LayoutOutput {
    float top_pos, top_neg;     /* margins collapsing through the top (incl. own) */
    float bot_pos, bot_neg;     /* margins collapsing through the bottom (incl. own) */
    int collapses_through;      /* empty: top and bottom margins are one set    */
} LayoutOutput;

/* content box relative to the border-box origin (valid during layout) */
static inline float box_cl(const Box *b) { return b->border[CSS_LEFT] + b->padding[CSS_LEFT] + ((b->flags & BOXF_RTL) ? b->scrollbar_w : 0.0f); }
static inline float box_ct(const Box *b) { return b->border[CSS_TOP] + b->padding[CSS_TOP]; }
static inline float box_cw(const Box *b) { return box_content_w(b); }
static inline float box_ch(const Box *b) { return box_content_h(b); }

/* ---- layout_block.c ----------------------------------------------------- */
void  layout_box(LayoutTree *t, Box *b, const LayoutInput *in, LayoutOutput *out);
void  layout_block_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h, LayoutOutput *out);
float layout_resolve_len(const CssLength *l, float base, float fallback);
float layout_resolve_len_auto(const CssLength *l, float base, int *is_auto);
void  layout_resolve_edges(LayoutTree *t, Box *b, float cb_w);
void  layout_apply_min_max_w(const Box *b, float cb_w, float *border_w);
void  layout_apply_min_max_h(const Box *b, float cb_h, float *border_h);
/* layout_apply_min_max_w plus the content keywords of min-width / max-width (min-content,
 * max-content, fit-content: CSS Sizing 3 5.1); avail_border is the border-box width a
 * fit-content keyword fits into (< 0: indefinite, max-content) */
void  layout_apply_min_max_w_kw(LayoutTree *t, Box *b, float cb_w, float avail_border, float *border_w);
float layout_box_sizing_w(const Box *b, float css_w);   /* content->border adjust */
float layout_box_sizing_h(const Box *b, float css_h);
float layout_horizontal_extras(const Box *b);            /* border + padding + scrollbar */
float layout_vertical_extras(const Box *b);
float layout_intrinsic_width(LayoutTree *t, Box *b, int max_content);  /* border-box width */
/* a replaced box's border-box size for a containing block (-1: indefinite) */
void  layout_replaced_size(LayoutTree *t, Box *b, float cb_w, float cb_h, float *out_w, float *out_h);
/* min-/max-content border-box width of a box's contents, ignoring its own width / min-width /
 * max-width (what the content-keyword sizes resolve against) */
float layout_content_width(LayoutTree *t, Box *b, int max_content);
/* min-content / max-content / fit-content (a CssLengthKind) as a border-box width;
 * fit-content fits avail_border (< 0: indefinite, max-content) */
float layout_content_kw_width(LayoutTree *t, Box *b, int kind, float avail_border);
int   layout_len_is_content_kw(const CssLength *l);
/* a replaced box's border-box width with its height resolved against cb_h (a percentage height
 * in a definite container transfers through the natural ratio) */
float layout_replaced_width(LayoutTree *t, Box *b, float cb_h);
/* aspect-ratio (CSS Sizing 4) of a non-replaced box (0: none; replaced boxes: replaced_size),
 * and the border-box size it transfers from the other one (< 0 without a ratio) */
float layout_pref_ratio(const Box *b);
float layout_ratio_height(const Box *b, float border_w);
float layout_ratio_width(const Box *b, float border_h);
float layout_intrinsic_margin_width(LayoutTree *t, Box *b, int max_content, float cb_w);
void  layout_position_abs_children(LayoutTree *t, Box *cb);
void  layout_update_transform(Box *b);
void  layout_update_sticky(Box *scroller);
void  layout_free_lines(Box *b);
void  layout_free_runs(Box *b);
void  layout_free_box_arrays(Box *b);
int   layout_is_definite_height_child(const Box *b, const LayoutInput *in);
void  layout_overflow_union_child(Box *parent, const Box *child);
void  layout_reset_overflow(Box *b);
void  layout_finish_scroll(LayoutTree *t, Box *b);
void  layout_set_bars(Box *b);
void  layout_finalize(LayoutTree *t, Box *b);            /* local -> absolute for the subtree */
void  layout_relative_offset(Box *c, float cb_w, float cb_h);

/* floats */
void  floats_add(FloatCtx *f, Box *box, uint8_t side, float x, float y, float w, float h);
void  floats_avail(const FloatCtx *f, float y, float height, float left, float right, float *out_left, float *out_right);
float floats_clear_y(const FloatCtx *f, int clear);
float floats_bottom(const FloatCtx *f);
float floats_next_y(const FloatCtx *f, float y);     /* next y where the band changes, or y */
void  floats_place(LayoutTree *t, FloatCtx *f, Box *fb, float y_min, float cb_left, float cb_right);

/* ---- layout_inline.c ---------------------------------------------------- */
void  layout_inline_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h);
void  layout_inline_intrinsic(LayoutTree *t, Box *b, float *min_w, float *max_w);
float layout_text_width(LayoutTree *t, TextFont *font, const char *s, size_t len, float letter_spacing);
float layout_box_baseline(const Box *b);        /* baseline from border-box top; <0: none */
float layout_line_height(const Box *b);         /* resolved line-height px for a box's style/font */

/* ---- layout_flex.c / layout_grid.c / layout_table.c --------------------- */
void  layout_flex_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h);
void  layout_flex_intrinsic(LayoutTree *t, Box *b, float *min_w, float *max_w);
void  layout_grid_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h);
void  layout_grid_intrinsic(LayoutTree *t, Box *b, float *min_w, float *max_w);
void  layout_table_contents(LayoutTree *t, Box *b, const LayoutInput *in, float *content_h);
void  layout_table_intrinsic(LayoutTree *t, Box *b, float *min_w, float *max_w);

/* ---- layout_build.c ----------------------------------------------------- */
Box  *layout_new_box(LayoutTree *t, uint8_t type, lxb_dom_node_t *node, const CssComputed *style);
void  layout_append_child(Box *parent, Box *child);
void  layout_resolve_font(LayoutTree *t, Box *b);
void  layout_anon_inherit(Box *anon, const CssComputed *parent_style);   /* refresh an anonymous box's style */
void  layout_restyle_anon_children(Box *b);                             /* re-inherit anon descendants of b */
int   layout_elem_tag(const Box *b);            /* LXB_TAG_* or 0            */
int   layout_style_line_clamps(const CssComputed *s);  /* -webkit-line-clamp applies (legacy vertical box) */

/* ---- helpers ------------------------------------------------------------ */
static inline float kn_maxf(float a, float b) { return a > b ? a : b; }
static inline float kn_minf(float a, float b) { return a < b ? a : b; }
static inline float kn_clampf(float v, float lo, float hi) { return v < lo ? lo : (v > hi ? hi : v); }
static inline int   box_is_inflow(const Box *b) { return !(b->flags & (BOXF_OUT_OF_FLOW | BOXF_FLOAT)); }
static inline float box_border_padding_w(const Box *b) { return b->border[CSS_LEFT] + b->border[CSS_RIGHT] + b->padding[CSS_LEFT] + b->padding[CSS_RIGHT]; }
static inline float box_border_padding_h(const Box *b) { return b->border[CSS_TOP] + b->border[CSS_BOTTOM] + b->padding[CSS_TOP] + b->padding[CSS_BOTTOM]; }
static inline ElemData *layout_elem_data(lxb_dom_node_t *node) { return dom_node_data(node); }

#endif /* KN_HTML_LAYOUT_INTERNAL_H */
