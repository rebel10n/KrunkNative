/*
 * paint.h - display-list producer for the KrunkNative HTML/CSS engine.
 *
 * paint_build() walks the layout Box tree (layout.h) and records DlItems
 * (paint_types.h) in CSS 2.1 Appendix E painting order with stacking
 * contexts, overflow / clip-path clips, scroll offsets, opacity / filter
 * groups and transforms.  gl_backend.c consumes the resulting DisplayList.
 *
 * Coordinate model
 * ----------------
 * Every stacking context is recorded in its own local frame whose origin is
 * the context root's border-box origin.  The parent context wraps the
 * child's items in DL_PUSH_TRANSFORM(translate) / DL_POP_TRANSFORM, so a
 * child's list depends only on its own subtree.  A root with `transform`
 * additionally pushes its CSS matrix (transform-origin folded in) inside
 * that frame.  Scroll containers emit DL_PUSH_CLIP_* (padding box minus
 * scrollbars, inner radii) + DL_SCROLL_OFFSET(-scroll) around their scrolled
 * content; the surrounding DL_POP_CLIP undoes the offset.
 *
 * The root box's overflow clip is the viewport (0, 0, viewport_w, viewport_h)
 * per CSS Overflow 3 §3.3; when <html> has overflow: visible, <body>'s
 * overflow (PaintCtx.body) propagates to the viewport and <body> does not
 * clip itself.
 *
 * Conventions relied on (paint_types.h): CSS px, y down; straight-alpha
 * CssColor (backend premultiplies); image textures are premultiplied
 * (image.h) and flagged as such; DL_BORDER draws the per-edge trapezoids
 * (transparent-border triangles work); DL_BOX_SHADOW inset uses inset_pad.
 * Pointers in items (glyph quads, polygon points) live in per-context arenas
 * owned by the PaintCtx and stay valid until that context is re-recorded or
 * evicted: submit `out` before the next paint_build().
 *
 * Caching
 * -------
 * Each context keeps its recorded list.  It is re-recorded when the hash of
 * its subtree (geometry, style records, text, lines, scroll state; nested
 * context roots contribute identity + z-order fields) changes, when
 * paint_mark_dirty() was called for a box inside it, when the effective
 * device scale at its position changed, when the glyph atlas generation
 * changed, or after paint_invalidate_all().  The cached lists are flattened
 * into `out` every frame.  Caches of contexts no longer reachable are freed.
 * When the atlas is rebuilt while recording (it filled up), paint_build()
 * re-records everything once, so `out` never mixes atlas generations.
 */
#ifndef KN_PAINT_H
#define KN_PAINT_H

#include <stddef.h>
#include <stdint.h>
#include "paint_types.h"
#include "layout.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct TextSystem TextSystem;
typedef struct ImageCache ImageCache;
typedef struct PaintCtx PaintCtx;

/* Scrollbar part style resolved from the ::-webkit-scrollbar* pseudo rules
 * of the scroller (fallbacks: the ui inventory values). */
typedef struct PaintScrollbarStyle {
    float width;               /* ::-webkit-scrollbar width (px) */
    CssColor track_color;      /* ::-webkit-scrollbar-track background-color */
    float track_radius;
    CssShadow track_shadow;    /* box-shadow of the track (inset) */
    uint8_t track_shadow_count;
    CssColor thumb_color;      /* ::-webkit-scrollbar-thumb background */
    float thumb_radius;
    CssShadow thumb_shadow;
    uint8_t thumb_shadow_count;
    float thumb_min;           /* min-height of the thumb (px) */
} PaintScrollbarStyle;

/* Form-control paint call (widgets.c plugs in through PaintHost.widget_paint).
 * (x, y) is the border-box origin of the control in the local space of `out`
 * (border/padding widths are in box->border / box->padding).  Items must be
 * appended to `out`; memory referenced by items must come from
 * paint_alloc(ctx, ...).  device_scale is the effective device px per CSS px
 * at this point of the tree (glyph rasterisation size). */
typedef struct PaintWidgetArgs {
    PaintCtx *ctx;
    Box *box;
    float x, y;
    DisplayList *out;
    float device_scale;
} PaintWidgetArgs;

/* Services the engine provides.  Any pointer may be NULL. */
typedef struct PaintHost {
    void *user;
    /* Resolved `src` of a replaced <img> box (NULL = none / paint nothing). */
    const char *(*image_src)(void *user, const Box *box);
    /* Fill *st from cascade_pseudo_computed(elem, CSS_PE_SCROLLBAR*) for the
     * scroller box.  Return 0 to keep the defaults already in *st. */
    int (*scrollbar_style)(void *user, const Box *box, PaintScrollbarStyle *st);
    /* Paints control internals of an input/select box (widgets_paint).  When
     * NULL, inputs are painted as plain boxes (background + border only). */
    void (*widget_paint)(void *user, const PaintWidgetArgs *args);
    /* The bitmap of a <canvas> box (premultiplied texture, size in px); 0 = paint nothing.
     * *flip_y = 1: the texture's row 0 is the bottom (a GL render target, html_dom_canvas_gl). */
    int (*canvas_image)(void *user, const Box *box, uint32_t *tex, float *w, float *h, int *flip_y);
    /* Selected byte range [*start, *end) of `run` (relative to run->text) of text box `tb`, the
     * highlight colour and, when *has_fg, the selected text's colour (selection.c).  0 = none. */
    int (*selection_range)(void *user, const Box *tb, const TextRun *run, uint32_t *start, uint32_t *end,
                           CssColor *bg, CssColor *fg, int *has_fg);
} PaintHost;

struct PaintCtx {
    /* inputs, set by the caller before paint_build() */
    float device_scale;        /* framebuffer px per CSS px */
    float viewport_w, viewport_h; /* CSS px */
    TextSystem *text;          /* shaping + glyph atlas (text_shape / text_glyph_quad) */
    ImageCache *images;        /* image_get / image_texture */
    const char *css_base_url;  /* unused: background url()s resolve against CssImage.base (the
                                  declaring sheet) or doc_base_url for inline style="" */
    const char *doc_base_url;  /* base for <img src> ("" = ui root) */
    const Box *body;           /* <body>'s box (overflow propagation to the viewport); NULL = none */
    int hide_scrollbars;       /* 1: paint no scrollbars (headless Chromium `--hide-scrollbars`,
                                  which the doc/ref captures used); layout must then reserve 0 px */
    PaintHost host;
    /* The top layer (html_dom_set_top_layer: the developer console): when it is a stacking
     * context of the root's (a positioned body child with a z-index), paint_build() records it
     * into *top_out instead of the page's list, so the client can draw GL over the page and
     * still have the top layer above that (html_ui_render_page / html_ui_render_top). NULL
     * (either): one list. */
    const Box *top_layer;
    DisplayList *top_out;

    /* private */
    struct PaintCacheTable *caches;
    uint32_t frame;
    uint32_t atlas_generation;
    unsigned image_loads;      /* image_cache_loads() when the lists were last built */
    int last_hide_scrollbars;
    int stats_rebuilt;         /* contexts re-recorded in the last build */
    int stats_contexts;        /* contexts visited in the last build */
};

void paint_ctx_init(PaintCtx *ctx);
void paint_ctx_free(PaintCtx *ctx);

/* Forces every context to be re-recorded on the next build. */
void paint_invalidate_all(PaintCtx *ctx);

/* Marks the stacking context containing `box` for re-recording (state not
 * visible in the Box tree changed: caret blink, image finished loading...). */
void paint_mark_dirty(PaintCtx *ctx, const Box *box);

/* Records dirty stacking contexts, then flattens all cached lists into *out
 * (cleared first).  Returns 0 on success, -1 on allocation failure. */
int paint_build(PaintCtx *ctx, Box *root, DisplayList *out);

/* Memory valid for the lifetime of the stacking context currently being
 * recorded (for widgets / hosts that hand pointers to DlItems).  NULL
 * outside a widget_paint callback. */
void *paint_alloc(PaintCtx *ctx, size_t size);

/* Helpers shared with widgets.c / tests. */
Mat2D paint_box_transform(const Box *box);        /* box-local matrix incl. transform-origin; identity when none */
DlRadii paint_border_radii(const Box *box);       /* resolved + clamped outer radii */
int paint_is_stacking_context(const Box *box);
CssColor paint_resolve_color(CssColor c, const CssComputed *style);

/* Default scrollbar style (ui inventory: 12px, track #f5f5f5 r10 inset 0 0 6px
 * rgba(0,0,0,.3), thumb #868686 r10 inset 0 0 2px rgba(0,0,0,.6)). */
void paint_scrollbar_defaults(PaintScrollbarStyle *st);

#ifdef __cplusplus
}
#endif

#endif /* KN_PAINT_H */
