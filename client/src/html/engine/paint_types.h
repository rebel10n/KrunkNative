#pragma once
// Display-list types shared between paint.c (producer) and gl_backend.c
// (consumer). See doc/html-engine-plan.md "Data structures" / "GL rendering".
//
// Coordinate conventions
// - All geometry is in CSS px, y down, in the local space of the current
//   transform frame. The backend multiplies by the device scale given to
//   glb_begin() and applies the transform stack on the CPU.
// - Rects are (x, y, w, h). Radii are circular per corner, in CSS px, already
//   clamped by the producer so that adjacent radii never exceed the box size.
// - Colors are straight-alpha CssColor values; the backend premultiplies.
// - Pointers inside items (glyph quads, polygon points) are NOT owned by the
//   DisplayList; the producer keeps them alive until the list is cleared.
//
// Stack semantics
// - PUSH_CLIP_* / POP_CLIP, PUSH_TRANSFORM / POP_TRANSFORM and
//   PUSH_GROUP / POP_GROUP must be balanced within one submitted list.
// - A POP_CLIP also restores the transform that was current at its PUSH_CLIP
//   (so SCROLL_OFFSET emitted inside a scroller's clip is undone by its
//   POP_CLIP without an extra transform frame).
// - SCROLL_OFFSET translates every following item by (dx, dy) (paint passes
//   -scrollLeft, -scrollTop). It modifies the current transform frame.
// - PUSH_TRANSFORM composes: new = current * m (m maps the pushed local space
//   into the enclosing local space, like CSS `transform` with its origin
//   already folded into m). Mat2D is css_types.h's matrix(a,b,c,d,e,f).
#include <stdint.h>
#include <stdlib.h>
#include "css_types.h"

typedef enum {
    DL_RECT,            // solid rectangle
    DL_RRECT,           // solid rounded rectangle
    DL_BORDER,          // CSS border between outer and inner rounded rects
    DL_IMAGE,           // textured quad (image / canvas / video frame)
    DL_GRADIENT,        // linear / radial CSS gradient
    DL_GLYPHS,          // run of glyph quads from the R8 text atlas
    DL_BOX_SHADOW,      // analytic blurred rounded-rect shadow (outset / inset)
    DL_TEXT_SHADOW,     // one text-shadow layer: offset (+ blurred) glyph quads
    DL_PUSH_CLIP_RECT,
    DL_PUSH_CLIP_RRECT,
    DL_PUSH_CLIP_POLY,  // clip-path: polygon(...)
    DL_POP_CLIP,
    DL_PUSH_TRANSFORM,
    DL_POP_TRANSFORM,
    DL_PUSH_GROUP,      // opacity / filter group -> offscreen FBO
    DL_POP_GROUP,
    DL_SCROLL_OFFSET,
    DL_BACKDROP,        // backdrop-filter: filters what is already drawn under rect (clipped to radii)
    DL_MASK_BEGIN,      // inside a masked group: the items up to its POP_GROUP draw the mask
    DL_OP_COUNT
} DlOp;

typedef struct { float x, y, w, h; } DlRect;

// Per-corner radii, CSS order: top-left, top-right, bottom-right, bottom-left.
typedef struct { float tl, tr, br, bl; } DlRadii;

typedef enum {
    DL_BORDER_NONE = 0,
    DL_BORDER_SOLID,
    DL_BORDER_DASHED,
    DL_BORDER_DOTTED
} DlBorderStyle;

// Edge order everywhere: top, right, bottom, left (CSS order).
enum { DL_EDGE_TOP = 0, DL_EDGE_RIGHT, DL_EDGE_BOTTOM, DL_EDGE_LEFT };

// One glyph quad already resolved against the atlas (text_glyph_quad()).
// uv = (u0, v0, u1, v1) in atlas texture space; (x, y, w, h) is the quad in
// CSS px relative to the current local space (baseline/phase already applied).
typedef struct {
    uint32_t tex;           // GL texture of the atlas page (R8)
    float uv[4];
    float x, y, w, h;
} DlGlyphQuad;

// Opacity / filter / mask / perspective group. Filters are applied in list
// order; runs of color matrix filters (grayscale, brightness, contrast,
// saturate, hue-rotate, opacity, invert, sepia) fold into one 4x5 matrix,
// drop-shadow and blur() run separable blur passes.
// masked: a DL_MASK_BEGIN inside the group (at its nesting level) ends the
// content; the items after it draw the mask (CSS mask-image layers, or the
// glyphs of background-clip: text) into a second offscreen target, and the
// group composites as content x mask alpha (after its filters).
// projective: the content is recorded untransformed (local space) and `h`
// (row-major 3x3, (x, y, 1) -> (X, Y, W)) maps it into the enclosing local
// space: a 3D transform under `perspective` that no Mat2D can express. The
// backend renders the content offscreen and draws it as a subdivided mesh.
typedef struct {
    DlRect rect;            // bounds of the group's content in local space
    float opacity;          // 1 = opaque
    CssFilterOp filters[CSS_MAX_FILTER_OPS];
    int n_filters;
    uint8_t masked;
    uint8_t projective;
    float h[9];
} DlGroup;

typedef struct {
    DlOp op;
    union {
        struct { DlRect rect; CssColor color; } rect;                               // DL_RECT
        struct { DlRect rect; DlRadii radii; CssColor color; } rrect;               // DL_RRECT
        struct {                                                                    // DL_BORDER
            DlRect rect;            // border box
            DlRadii radii;          // outer radii
            float width[4];         // top, right, bottom, left (CSS px)
            CssColor color[4];      // per edge
            uint8_t style[4];       // DlBorderStyle per edge
        } border;
        struct {                                                                    // DL_IMAGE
            uint32_t tex;           // GL_TEXTURE_2D, RGBA, straight or premultiplied alpha
            float uv[4];            // u0, v0, u1, v1 (no flip; v0 maps to dst top)
            DlRect dst;
            float opacity;          // multiplies the image (1 = as is)
            uint8_t nearest;        // image-rendering: pixelated / crisp-edges
            uint8_t premultiplied;  // 1 when the texture already holds premultiplied alpha
            const void *vector;     // SVG source (const ImageEntry *) or NULL: the backend's
                                    // image resolver swaps in a raster of the drawn device size
            const void *lazy;       // lazy raster (const ImageEntry *, image.h) or NULL: `tex` is 0
                                    // and the backend's lazy resolver decodes / looks it up per draw
        } image;
        struct {                                                                    // DL_GRADIENT
            DlRect rect;            // area painted
            DlRect box;             // gradient box (background-size/position tile); w <= 0 -> rect
            CssGradient gradient;
        } gradient;
        struct {                                                                    // DL_GLYPHS
            const DlGlyphQuad *quads;
            int n;
            CssColor color;
        } glyphs;
        struct {                                                                    // DL_BOX_SHADOW
            DlRect rect;            // border box of the element
            DlRadii radii;          // border radii of the element
            float dx, dy;           // offset
            float blur;             // CSS blur radius (sigma = blur / 2)
            float spread;
            CssColor color;
            uint8_t inset;
            float inset_pad[4];     // inset only: border widths top,right,bottom,left (padding box = rect - pad)
        } box_shadow;
        struct {                                                                    // DL_TEXT_SHADOW
            const DlGlyphQuad *quads;
            int n;
            float dx, dy;
            float blur;             // 0 -> plain offset copy, > 0 -> blurred through an FBO
            CssColor color;
            DlRect bounds;          // bounds of the quads in local space (for the blur FBO)
        } text_shadow;
        struct { DlRect rect; } clip_rect;                                          // DL_PUSH_CLIP_RECT
        struct { DlRect rect; DlRadii radii; } clip_rrect;                          // DL_PUSH_CLIP_RRECT
        struct { const float *pts; int n; } clip_poly;                              // DL_PUSH_CLIP_POLY, pts = x0,y0,x1,y1,... (n points)
        struct { Mat2D m; } transform;                                              // DL_PUSH_TRANSFORM
        DlGroup group;                                                              // DL_PUSH_GROUP
        struct {                                                                    // DL_BACKDROP
            DlRect rect;            // border box of the element (local space)
            DlRadii radii;          // its border radii: the filtered backdrop is clipped to them
            float opacity;          // the element's opacity (applies to the filtered backdrop too)
            CssFilterOp filters[CSS_MAX_FILTER_OPS];
            int n_filters;
        } backdrop;
        struct { float dx, dy; } scroll;                                            // DL_SCROLL_OFFSET
    } u;
} DlItem;

typedef struct {
    DlItem *items;
    int n, cap;
} DisplayList;

// Appends a copy of *item. Returns 0 on success, -1 when out of memory.
static inline int dl_push(DisplayList *dl, const DlItem *item) {
    if (dl->n == dl->cap) {
        const int cap = dl->cap ? dl->cap * 2 : 64;
        DlItem *items = (DlItem *) realloc(dl->items, (size_t) cap * sizeof(DlItem));
        if (!items) return -1;
        dl->items = items;
        dl->cap = cap;
    }
    dl->items[dl->n++] = *item;
    return 0;
}

static inline void dl_clear(DisplayList *dl) {
    dl->n = 0;
}

static inline void dl_free(DisplayList *dl) {
    free(dl->items);
    dl->items = NULL;
    dl->n = dl->cap = 0;
}

static inline DlRect dl_rect(float x, float y, float w, float h) {
    DlRect r = {x, y, w, h};
    return r;
}

static inline DlRadii dl_radii(float tl, float tr, float br, float bl) {
    DlRadii r = {tl, tr, br, bl};
    return r;
}

static inline DlRadii dl_radii_all(float r) {
    return dl_radii(r, r, r, r);
}

static inline Mat2D dl_mat2d_identity(void) {
    Mat2D m = {1, 0, 0, 1, 0, 0};
    return m;
}

static inline CssColor dl_color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) {
    CssColor c = {r, g, b, a, CSS_COLOR_RGBA};
    return c;
}
