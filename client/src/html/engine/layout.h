/*
 * layout.h - box tree and layout for the KrunkNative HTML/CSS engine.
 *
 * See doc/html-engine-plan.md ("Architecture", "Data structures", the GL
 * "60 fps" paragraph) and doc/ui-feature-inventory.md section 6.
 *
 * Pipeline
 *   layout_build(dom, ctx)            DOM (+ computed styles) -> Box tree
 *   layout_run(tree, vw, vh)          (re)layout dirty boundaries
 *   layout_box_rect_in_viewport(...)  getBoundingClientRect
 *   layout_hit_test(...)              elementFromPoint, paint order aware
 *
 * Coordinates.  After layout_run() every rect in this header (Box, BoxFrag,
 * LineBox, InlineItem, TextRun) is in absolute layout space: CSS px, y down,
 * origin at the viewport's top-left, BEFORE ancestor scroll offsets and
 * transforms (paint emits DL_SCROLL_OFFSET / DL_PUSH_TRANSFORM; the geometry
 * helpers below apply them).  Relative offsets and sticky shifts ARE folded
 * in.  `lx, ly` keep the same box offset relative to `container`'s border-box
 * origin (what layout computes; the absolute values are derived from them).
 *
 * `container` is the containing-block box: the tree parent for in-flow
 * block-level content, the nearest block container for inline content, the
 * nearest positioned (or transformed) ancestor for absolute boxes, the nearest
 * transformed ancestor or the root for fixed boxes.  The root box (<html>)
 * stands in for the initial containing block (the viewport).
 *
 * Ownership: every Box lives in the LayoutTree arena; per-layout arrays
 * (lines, items, frags, runs, glyphs, absolute lists) are malloc'd and freed
 * when the owning box is relaid out or the tree is destroyed.
 */
#ifndef KN_HTML_LAYOUT_H
#define KN_HTML_LAYOUT_H

#include <stddef.h>
#include <stdint.h>
#include "css_types.h"
#include "util.h"
#include "text.h"
#include "html_dom.h"         /* HtmlRect (client/include) */

#ifdef __cplusplus
extern "C" {
#endif

typedef struct lxb_dom_node lxb_dom_node_t;
typedef struct Dom Dom;             /* dom.h */

typedef struct Box Box;
typedef struct LineBox LineBox;
typedef struct InlineItem InlineItem;
typedef struct TextRun TextRun;
typedef struct LayoutTree LayoutTree;

/* ------------------------------------------------------------------------- */
/* Context                                                                    */
/* ------------------------------------------------------------------------- */

typedef struct LayoutCtx {
    TextSystem *text;                /* fonts, metrics, measuring, shaping (text.h) */
    const char *base_url;            /* document URL relative to ui/ (for image_size) */
    void *user;                      /* passed to the callbacks below          */
    /* Intrinsic size of an <img>; return 0 when missing/undecodable.  The engine
     * binds image_get(); tests may stub it (image_get needs a GL context). */
    int (*image_size)(void *user, const char *url, const char *base, float *w, float *h);
    /* Width of the custom scrollbar for a scroll container (the
     * ::-webkit-scrollbar width).  NULL: scrollbar_default_w. */
    float (*scrollbar_width)(void *user, lxb_dom_node_t *node, float default_w);
    /* Form-control intrinsic CONTENT-box size (<input>, <select>) from the
     * author font; the engine binds widgets_intrinsic_size().  Return 0 to
     * fall back to the ua_* constants below.  May be NULL. */
    int (*widget_size)(void *user, lxb_dom_node_t *node, const CssComputed *st, float *w, float *h);

    float scrollbar_default_w;       /* 12 (global ::-webkit-scrollbar)        */
    float scrollbar_min_thumb;       /* 0: Chrome custom scrollbars have none  */
    int relaxed_boundaries;          /* 1: abs+definite size is a boundary even with overflow:visible (the `grow` case) */
    /* Chrome UA intrinsic sizes for replaced elements (border-box px with the
     * UA border/padding; layout subtracts the computed border/padding). */
    float ua_canvas_w, ua_canvas_h;  /* 300 x 150 (also <video>)               */
    float ua_input_text_w, ua_input_text_h;    /* text/url/password/number: 153 x 21 */
    float ua_input_checkbox;         /* 13                                     */
    float ua_input_range_w, ua_input_range_h;  /* 129 x 16 (Chrome 152)        */
    float ua_input_color_w, ua_input_color_h;  /* 50 x 27                      */
    float ua_select_h, ua_select_arrow_w;      /* menulist height 20, chevron 20 */
} LayoutCtx;

void layout_ctx_init(LayoutCtx *ctx);   /* fills the defaults above; text/callbacks NULL */

/* ------------------------------------------------------------------------- */
/* Box tree                                                                   */
/* ------------------------------------------------------------------------- */

typedef enum BoxType {
    BOX_BLOCK = 0,     /* block container (block-level, inline-block, anonymous) */
    BOX_INLINE,        /* non-replaced inline box (<span>, <b>, <a> ...)      */
    BOX_TEXT,          /* text node                                            */
    BOX_LINEBREAK,     /* <br>                                                 */
    BOX_IMAGE,         /* <img>  (replaced)                                    */
    BOX_CANVAS,        /* <canvas> (replaced)                                  */
    BOX_VIDEO,         /* <video> (replaced)                                   */
    BOX_INPUT,         /* <input> (replaced; widgets.c paints it)              */
    BOX_SELECT,        /* <select> (replaced)                                  */
    BOX_FLEX,          /* flex container                                       */
    BOX_GRID,          /* grid container                                       */
    BOX_TABLE,         /* table wrapper (display: table / inline-table)        */
    BOX_TABLE_ROW,     /* (anonymous) row                                      */
    BOX_TABLE_CELL,    /* display: table-cell                                  */
    BOX_TYPE_COUNT
} BoxType;

enum {
    BOXF_INLINE_LEVEL      = 1u << 0,   /* participates in an inline formatting context */
    BOXF_ATOMIC            = 1u << 1,   /* inline-block / inline-flex / replaced inline */
    BOXF_ANONYMOUS         = 1u << 2,
    BOXF_PSEUDO_BEFORE     = 1u << 3,
    BOXF_PSEUDO_AFTER      = 1u << 4,
    BOXF_POSITIONED        = 1u << 5,   /* relative / absolute / fixed / sticky */
    BOXF_OUT_OF_FLOW       = 1u << 6,   /* absolute / fixed                     */
    BOXF_FLOAT             = 1u << 7,
    BOXF_SCROLL_CONTAINER  = 1u << 8,   /* overflow != visible on either axis   */
    BOXF_CLIPS             = 1u << 9,   /* clips descendants (overflow != visible / clip-path) */
    BOXF_STACKING_CONTEXT  = 1u << 10,
    BOXF_HAS_TRANSFORM     = 1u << 11,  /* xform valid                          */
    BOXF_BFC_ROOT          = 1u << 12,  /* establishes a block formatting context */
    BOXF_LAYOUT_BOUNDARY   = 1u << 13,
    BOXF_RTL               = 1u << 14,
    BOXF_CONTINUATION      = 1u << 15,  /* inline shell created by block-in-inline splitting */
    BOXF_INTRINSIC_BORDER  = 1u << 16,  /* intrinsic_w/h include the UA border+padding (input fallback) */
    BOXF_HAS_LINES         = 1u << 20,  /* block container with inline content  */
    BOXF_STICKY            = 1u << 21,
    BOXF_HIDDEN            = 1u << 22,  /* visibility: hidden (not hit-tested)  */
    BOXF_NO_POINTER        = 1u << 23,  /* pointer-events: none                 */
    BOXF_BLOCK_LEVEL       = 1u << 24,  /* outer display block (incl. blockified flex/grid items, floats, abs) */
    BOXF_VBAR              = 1u << 25,  /* vertical scrollbar present           */
    BOXF_HBAR              = 1u << 26,  /* horizontal scrollbar present         */
    BOXF_ELLIPSIS          = 1u << 27,  /* text-overflow: ellipsis applies      */
    BOXF_TEXT_CONTROL      = 1u << 28   /* text-like <input> / <select>: baseline = inner text baseline */
};

enum { BOX_DIRTY_SELF = 1, BOX_DIRTY_CHILD = 2 };

/* One fragment of an inline box (one per line it spans). */
typedef struct BoxFrag {
    float x, y, w, h;      /* border box of the fragment                      */
    uint8_t first, last;   /* carries the left / right margin+border+padding  */
} BoxFrag;

/* One shaped piece of a text node on one line.  Paint draws glyph i at pen
 * x = run.x + glyphs[i].x, baseline y = run.y (see text_glyph_quad). */
struct TextRun {
    TextFont *font;
    TextFace *face;        /* text_font_face(font)                            */
    unsigned synth;        /* text_font_synth(font)                           */
    float font_size;       /* CSS px                                          */
    const char *text;      /* UTF-8 after white-space collapsing / text-transform; NOT NUL terminated */
    uint32_t len;          /* bytes drawn (after ellipsis truncation)         */
    uint32_t full_len;     /* bytes of the fragment before truncation         */
    float letter_spacing;  /* px added after every character                  */
    float x, y;            /* pen start x, baseline y                          */
    float width;           /* advance of text[0..len) incl. letter spacing    */
    Glyph *glyphs;         /* text_shape() output, malloc'd                   */
    int glyph_count;
    uint8_t ellipsis;      /* 1: draw ellipsis_glyphs[0..ellipsis_count) at ellipsis_x */
    float ellipsis_x;      /* pen x of the ellipsis                           */
    Glyph ellipsis_glyph;  /* == ellipsis_glyphs[0] (kept for older callers)  */
    float ellipsis_w;      /* total advance of the ellipsis glyphs            */
    /* U+2026 from the run's font, or - like Blink - three '.' from that font
     * when it has no U+2026 glyph (gamefont).  Glyph.x is relative to
     * ellipsis_x. */
    Glyph ellipsis_glyphs[3];
    uint8_t ellipsis_count;
};

typedef enum InlineItemKind {
    ITEM_TEXT = 0,
    ITEM_ATOMIC,           /* inline-block / inline-flex / replaced           */
    ITEM_INLINE_OPEN,      /* start fragment of an inline box (bookkeeping)   */
    ITEM_INLINE_CLOSE,
    ITEM_LINEBREAK
} InlineItemKind;

struct InlineItem {
    uint8_t kind;          /* InlineItemKind                                  */
    Box *box;              /* TEXT: the BOX_TEXT; ATOMIC: the atomic box; OPEN/CLOSE: the inline box */
    float x, y, w, h;      /* TEXT: content area (ascent+descent tall); ATOMIC: margin box */
    float baseline;        /* y of the baseline                                */
    TextRun *run;          /* TEXT: &box->runs[i]                              */
};

struct LineBox {
    float x, y;            /* x already accounts for floats                   */
    float w, h;            /* available width used for alignment; line height  */
    float baseline;        /* y of the root inline box baseline                */
    InlineItem *items;
    int item_count;
};

struct Box {
    uint8_t type;          /* BoxType                                          */
    uint8_t pseudo;        /* 0 element/text, 1 ::before, 2 ::after            */
    uint8_t dirty;         /* BOX_DIRTY_*                                      */
    uint8_t paint_dirty;   /* set by layout when geometry changed; paint clears */
    uint8_t vertical_align;/* CssVerticalAlign                                 */
    uint8_t z_auto;
    uint32_t flags;        /* BOXF_*                                           */
    int32_t z_index;       /* resolved (0 when auto)                           */

    lxb_dom_node_t *node;  /* element or text node; NULL for anonymous boxes   */
    const CssComputed *style; /* never NULL (anonymous boxes borrow the parent's) */

    Box *parent, *first_child, *last_child, *prev, *next;
    Box *container;        /* containing block box (NULL for the root)         */
    Box *scroller;         /* nearest scroll container on the container chain  */

    /* geometry: border box */
    float x, y, w, h;      /* absolute layout space (see header comment)       */
    float lx, ly;          /* offset from container's border-box origin        */
    float margin[4], border[4], padding[4];   /* resolved px, CSS side order  */
    float static_x, static_y;                 /* abs static position, static-parent coords */
    float rel_x, rel_y;                       /* relative offset (already in lx/ly) */
    float sticky_dx, sticky_dy;               /* current sticky shift (already in x/y) */

    /* scroll container state */
    float scroll_x, scroll_y;                 /* current offsets (clamped)     */
    float scroll_w, scroll_h;                 /* scrollWidth / scrollHeight    */
    float scrollbar_w;                        /* vertical bar thickness, 0 when absent */
    float scrollbar_h;                        /* horizontal bar thickness, 0 when absent */
    float bar_w;                              /* configured thickness (::-webkit-scrollbar width) */

    /* overflow of contained content relative to this box's border-box origin (local) */
    float overflow_l, overflow_t, overflow_r, overflow_b;

    /* transform (BOXF_HAS_TRANSFORM), transform-origin folded in: for a point
     * p relative to the border-box origin, the rendered point is
     * (x, y) + xform * p  (the same space the untransformed box lives in). */
    Mat2D xform;

    /* text */
    TextFont *font;
    TextMetrics fm;        /* text_font_metrics(font) copy                     */
    const char *text;      /* BOX_TEXT: collapsed/transformed text (arena)     */
    uint32_t text_len;
    TextRun *runs;         /* BOX_TEXT: one per line fragment                  */
    int run_count, run_cap;

    /* replaced */
    const char *src;       /* BOX_IMAGE: src attribute (arena copy), may be NULL */
    float intrinsic_w, intrinsic_h;           /* content box; 0 when unknown  */
    float attr_w, attr_h;                     /* width=/height= attributes, <0 absent */

    /* inline formatting result (BOXF_HAS_LINES) */
    LineBox *lines;
    int line_count;
    /* inline box fragments (BOX_INLINE); x,y,w,h is their bounding box */
    BoxFrag *frags;
    int frag_count, frag_cap;

    /* absolutely positioned boxes whose containing block is this box */
    Box **abs_children;
    int abs_count, abs_cap;

    /* intrinsic size cache */
    float min_content_w, max_content_w;
    uint32_t intrinsic_gen;
    float baseline_offset;                    /* last line baseline from border top, <0 none */
    float first_baseline;                     /* first line baseline, <0 none */

    /* layout-private: offset already applied to lines (block) / frags (inline) */
    float applied_x, applied_y;
    /* layout-private: static position alignment inside a flex/grid container
     * (abs child placed as if it were the sole item): fraction of the free
     * space along x / y and the area it aligns in. */
    float static_fx, static_fy, static_aw, static_ah;
    /* layout-private: anonymous boxes own their style (initial values +
     * inherited properties of the parent box); `style` points at it */
    CssComputed *own_style;
};

static inline int box_is_replaced(const Box *b) {
    return b->type == BOX_IMAGE || b->type == BOX_CANVAS || b->type == BOX_VIDEO || b->type == BOX_INPUT || b->type == BOX_SELECT;
}

/* ------------------------------------------------------------------------- */
/* API                                                                        */
/* ------------------------------------------------------------------------- */

/* Build the box tree for the document.  Each element with computed style gets
 * a Box (ElemData.box points at it); display:none subtrees get none.  Call
 * layout_tree_free() on the previous tree first.  Returns NULL on OOM. */
LayoutTree *layout_build(Dom *dom, LayoutCtx *ctx);
void        layout_tree_free(LayoutTree *tree);
Box        *layout_tree_root(LayoutTree *tree);
LayoutCtx  *layout_tree_ctx(LayoutTree *tree);

/* Lay out dirty boundaries (everything on the first call or after a viewport
 * change).  Returns the number of boundary relayouts performed (0 = nothing
 * was dirty). */
int  layout_run(LayoutTree *tree, float viewport_w, float viewport_h);

/* Invalidation.  Marks the box; the next layout_run() relayouts from the
 * nearest enclosing layout boundary (absolutely positioned box with a
 * definite size [and overflow != visible unless relaxed_boundaries], or the
 * root).  Refreshes the style pointer from ElemData and the font.  Changes to
 * display/position/float/overflow/content need layout_build() again.
 * `geometry_only` (transform/transform-origin changed): only the cached matrix
 * is refreshed. */
void layout_invalidate(LayoutTree *tree, Box *box, int geometry_only);
void layout_invalidate_node(LayoutTree *tree, lxb_dom_node_t *node, int geometry_only);
Box *layout_box_for_node(lxb_dom_node_t *node);

/* Incremental restyle / DOM mutation (engine.c, after cascade_restyle).
 *
 * layout_restyled(): call for every element whose ElemData has
 * DOM_DIRTY_STYLE_CHANGED, with a copy of its computed style from before the
 * restyle (`old_style`; NULL = unknown, treated as structural).  It refreshes
 * the box in place when display/position/float/overflow/content/clip-path
 * are unchanged and returns what the change needs:
 *   NONE       element not rendered before or after
 *   PAINT      paint-only (colors, backgrounds, shadows, opacity, z-index,
 *              visibility, pointer-events ...): box->paint_dirty set
 *   TRANSFORM  transform/transform-origin only: matrix refreshed, paint dirty
 *   LAYOUT     marked dirty; layout_run() relayouts the nearest boundary
 *   REBUILT    box structure changed: the element's (or its parent's) box
 *              subtree was rebuilt; Box pointers inside it are invalid
 * layout_children_changed(): child list / text changed (innerHTML,
 * textContent, insertAdjacentHTML, append/remove): rebuilds the box subtree
 * of the nearest rendered block-level ancestor-or-self.  Returns REBUILT or
 * NONE (element not rendered).
 * layout_rebuild_all(): rebuild every box in place (same LayoutTree*).
 * After any REBUILT, Box pointers held outside layout (hover, pointer
 * capture, paint caches) for boxes in that subtree are stale: re-resolve
 * them through ElemData.box.  Released boxes are recycled. */
enum { LAYOUT_RESTYLE_NONE = 0, LAYOUT_RESTYLE_PAINT, LAYOUT_RESTYLE_TRANSFORM, LAYOUT_RESTYLE_LAYOUT, LAYOUT_RESTYLE_REBUILT };
int  layout_restyled(LayoutTree *tree, lxb_dom_node_t *node, const CssComputed *old_style);
int  layout_children_changed(LayoutTree *tree, lxb_dom_node_t *node);
void layout_rebuild_all(LayoutTree *tree);

/* Scrolling: sets (clamped) offsets, refreshes sticky descendants' positions
 * and marks paint dirty.  No relayout. */
void layout_set_scroll(LayoutTree *tree, Box *scroller, float x, float y);

/* Scrollbar geometry (Chrome custom-scrollbar metrics: no buttons; thumb =
 * max(min_thumb, visible / total * track)).  Rects are absolute layout space
 * like the box.  Returns 0 when there is no bar on that axis. */
typedef struct LayoutScrollbar {
    float track_x, track_y, track_w, track_h;
    float thumb_x, thumb_y, thumb_w, thumb_h;
} LayoutScrollbar;
int  layout_scrollbar(const LayoutTree *tree, const Box *scroller, int vertical, LayoutScrollbar *out);

/* Geometry helpers. */
void  layout_box_matrix(const Box *box, Mat2D *out);   /* Box.xform, identity when no transform */
int   layout_box_rect_in_viewport(const Box *box, HtmlRect *out);   /* getBoundingClientRect */
float layout_offset_width(const Box *box);              /* offsetWidth (border box, no transforms) */
float layout_offset_height(const Box *box);
/* Map a viewport point into `box`'s local coordinate space (absolute layout
 * space of the box, i.e. comparable with box->x/y). */
void  layout_point_to_box(const Box *box, float vx, float vy, float *lx, float *ly);

/* Top-most box at a viewport point by paint order.  With pointer_events_aware,
 * boxes with pointer-events:none (and visibility:hidden) are transparent
 * (their children may still hit).  Text and anonymous boxes are never
 * returned; the nearest element box is. */
Box  *layout_hit_test(LayoutTree *tree, float vx, float vy, int pointer_events_aware);

/* Positioned / stacking descendants that paint as units of the stacking
 * context `ctx` (not nested inside a deeper context), sorted: negative z
 * first (ascending), then z = 0 / auto in tree order, then positive z
 * (ascending, ties by tree order).  Returns the total count; `out` may be
 * NULL to query it. */
int   layout_stacking_children(const Box *ctx, Box **out, int max);

/* Resolved box edges (absolute layout space). */
static inline float box_padding_x(const Box *b) { return b->x + b->border[CSS_LEFT]; }
static inline float box_padding_y(const Box *b) { return b->y + b->border[CSS_TOP]; }
static inline float box_padding_w(const Box *b) { return b->w - b->border[CSS_LEFT] - b->border[CSS_RIGHT]; }
static inline float box_padding_h(const Box *b) { return b->h - b->border[CSS_TOP] - b->border[CSS_BOTTOM]; }
/* Content box excludes scrollbars (an rtl scroller's vertical bar sits on the left). */
static inline float box_content_x(const Box *b) {
    return b->x + b->border[CSS_LEFT] + b->padding[CSS_LEFT] + ((b->flags & BOXF_RTL) ? b->scrollbar_w : 0.0f);
}
static inline float box_content_y(const Box *b) { return b->y + b->border[CSS_TOP] + b->padding[CSS_TOP]; }
static inline float box_content_w(const Box *b) {
    return b->w - b->border[CSS_LEFT] - b->border[CSS_RIGHT] - b->padding[CSS_LEFT] - b->padding[CSS_RIGHT] - b->scrollbar_w;
}
static inline float box_content_h(const Box *b) {
    return b->h - b->border[CSS_TOP] - b->border[CSS_BOTTOM] - b->padding[CSS_TOP] - b->padding[CSS_BOTTOM] - b->scrollbar_h;
}
static inline float box_margin_w(const Box *b) { return b->w + b->margin[CSS_LEFT] + b->margin[CSS_RIGHT]; }
static inline float box_margin_h(const Box *b) { return b->h + b->margin[CSS_TOP] + b->margin[CSS_BOTTOM]; }
/* clientWidth / clientHeight: padding box minus scrollbars. */
static inline float box_client_w(const Box *b) { return box_padding_w(b) - b->scrollbar_w; }
static inline float box_client_h(const Box *b) { return box_padding_h(b) - b->scrollbar_h; }

/* Debug: dump the tree (box type, node name/id, rect) to a string builder. */
void layout_dump(const Box *root, KnStrBuf *out);

#ifdef __cplusplus
}
#endif

#endif /* KN_HTML_LAYOUT_H */
