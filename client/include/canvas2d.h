#pragma once
// Canvas2D: an immediate-mode subset of the HTML Canvas 2D context on GL (client/src/ks/canvas2d.c),
// the drawing layer behind KrunkScript's GAME.OVERLAY (ks_overlay.h, krunkscript.md 3.1.10, WP5).
//
// Model: like a <canvas>, a Canvas2D is a persistent RGBA layer of canvas_w x canvas_h canvas
// pixels plus a 2D context state (transform, styles, font, path, save/restore stack). Calls are
// recorded on the CPU in device pixels; c2d_flush() replays them into the layer (an offscreen
// framebuffer, MSAA by default) and c2d_composite() draws the layer over the bound framebuffer
// with premultiplied source-over. Every GL call happens inside c2d_flush/c2d_composite/
// c2d_destroy, which save and restore the GL state they touch (framebuffer, viewport, program,
// VAO, array buffer, texture unit 0 binding, blend, depth, stencil, cull, scissor, colour mask,
// unpack alignment), so drawing calls are safe from any point of the frame.
//
// Resolution: the layer is rendered at device_w x device_h (the framebuffer, i.e. CSS px *
// devicePixelRatio) while all coordinates stay in canvas pixels (v7: canvas.width =
// window.innerWidth in CSS px), so high-DPI screens get sharp shapes and text.
//
// Semantics follow the HTML spec for the supported subset: nonzero fill, strokes with
// miter/round/bevel joins and butt/round/square caps, arcs/ellipses/arcTo, quadratic/cubic
// Béziers, rect, fillRect/strokeRect/clearRect, globalAlpha, fill/stroke colours, text
// (font size + family, textAlign, textBaseline, maxWidth), drawImage, save/restore,
// translate/rotate/scale/transform/setTransform. Invalid values are ignored exactly like a
// browser does (non-finite transform arguments, lineWidth <= 0, globalAlpha outside [0, 1]).
// Not supported: gradients/patterns, clip, line dashes, shadows, composite operations, image data.
//
// Text uses FreeType on the KN HUD font (assets css/fonts/font2.ttf = "GameFont"); unknown
// families fall back to it. Glyphs are rasterised at the device size into a glyph atlas
// (large text is filled from the outlines instead); strokeText strokes the outlines.
#include <stddef.h>

typedef struct Canvas2D Canvas2D;
typedef struct C2DImage C2DImage;

// Canvas matrix order: x' = a*x + c*y + e, y' = b*x + d*y + f
typedef struct {
    double a, b, c, d, e, f;
} C2DMatrix;

typedef enum { C2D_JOIN_MITER, C2D_JOIN_ROUND, C2D_JOIN_BEVEL } C2DLineJoin;
typedef enum { C2D_CAP_BUTT, C2D_CAP_ROUND, C2D_CAP_SQUARE } C2DLineCap;
typedef enum { C2D_ALIGN_START, C2D_ALIGN_END, C2D_ALIGN_LEFT, C2D_ALIGN_RIGHT, C2D_ALIGN_CENTER } C2DTextAlign;
typedef enum {
    C2D_BASELINE_ALPHABETIC, C2D_BASELINE_TOP, C2D_BASELINE_HANGING,
    C2D_BASELINE_MIDDLE, C2D_BASELINE_IDEOGRAPHIC, C2D_BASELINE_BOTTOM
} C2DTextBaseline;

// Renderer knobs, centralised. KRUNKNATIVE_CANVAS_MSAA=<n> overrides the sample count (0 = no
// MSAA: aliased path edges, text and images stay smooth).
#define C2D_DEFAULT_MSAA_SAMPLES 4
#define C2D_CURVE_TOLERANCE 0.1       // max flattening error of arcs/Béziers, device px
#define C2D_GLYPH_ATLAS_SIZE 2048     // R8 glyph atlas (texture unit 0 when drawn)
#define C2D_GLYPH_MAX_RASTER_PX 160.0 // larger device text is filled from the glyph outlines
#define C2D_MAX_SAVE_DEPTH 1024       // deeper save() calls are counted but not stored

// Creation does no GL work (resources are created at the first flush that needs them).
Canvas2D *c2d_create(void);
void c2d_destroy(Canvas2D*); // the GL context must be current if the canvas was ever flushed

// Canvas size in canvas px and the device (framebuffer) size the layer is rendered at. A size
// change clears the layer and resets the context state, like assigning canvas.width.
void c2d_set_size(Canvas2D*, int canvas_w, int canvas_h, int device_w, int device_h);
int c2d_width(const Canvas2D*);
int c2d_height(const Canvas2D*);

// State
void c2d_save(Canvas2D*);
void c2d_restore(Canvas2D*);
void c2d_reset(Canvas2D*); // default state, empty path, empty save stack (layer untouched)
void c2d_translate(Canvas2D*, double x, double y);
void c2d_rotate(Canvas2D*, double radians);
void c2d_scale(Canvas2D*, double x, double y);
void c2d_transform(Canvas2D*, double a, double b, double c, double d, double e, double f);
void c2d_set_transform(Canvas2D*, double a, double b, double c, double d, double e, double f);
C2DMatrix c2d_get_transform(const Canvas2D*);
void c2d_set_global_alpha(Canvas2D*, double alpha);
// Colours are straight (not premultiplied) RGBA in 0..1.
void c2d_set_fill_color(Canvas2D*, float r, float g, float b, float a);
void c2d_set_stroke_color(Canvas2D*, float r, float g, float b, float a);
// "#rgb", "#rgba", "#rrggbb", "#rrggbbaa" -> rgba (0..1). 1 on success.
int c2d_parse_color(const char *css, float rgba[4]);
void c2d_set_line_width(Canvas2D*, double width);
void c2d_set_line_join(Canvas2D*, C2DLineJoin);
void c2d_set_line_cap(Canvas2D*, C2DLineCap);
void c2d_set_miter_limit(Canvas2D*, double limit);
// ctx.font = "<px>px <family>": 0 (ignored, font unchanged) when px is not a positive finite number.
int c2d_set_font(Canvas2D*, double px, const char *family);
// ctx.font with a CSS font string; only "[style/weight words] <size>px <family>" forms are
// understood, anything else is ignored like an invalid font. 1 when applied.
int c2d_set_font_css(Canvas2D*, const char *font);
void c2d_set_text_align(Canvas2D*, C2DTextAlign);
void c2d_set_text_baseline(Canvas2D*, C2DTextBaseline);
// textAlign / textBaseline keywords; 0 (state unchanged) for anything else.
int c2d_set_text_align_name(Canvas2D*, const char *name);
int c2d_set_text_baseline_name(Canvas2D*, const char *name);
int c2d_set_line_join_name(Canvas2D*, const char *name);

// Path (the current default path; points are transformed by the current matrix when added)
void c2d_begin_path(Canvas2D*);
void c2d_close_path(Canvas2D*);
void c2d_move_to(Canvas2D*, double x, double y);
void c2d_line_to(Canvas2D*, double x, double y);
void c2d_quadratic_curve_to(Canvas2D*, double cpx, double cpy, double x, double y);
void c2d_bezier_curve_to(Canvas2D*, double c1x, double c1y, double c2x, double c2y, double x, double y);
// These return -1 (IndexSizeError, nothing added) for a negative radius, else 0.
int c2d_arc(Canvas2D*, double x, double y, double r, double start, double end, int ccw);
int c2d_arc_to(Canvas2D*, double x1, double y1, double x2, double y2, double r);
int c2d_ellipse(Canvas2D*, double x, double y, double rx, double ry, double rotation, double start, double end, int ccw);
void c2d_rect(Canvas2D*, double x, double y, double w, double h);
void c2d_fill(Canvas2D*);   // nonzero winding, fill colour * globalAlpha
void c2d_stroke(Canvas2D*); // pen = lineWidth under the current matrix

void c2d_fill_rect(Canvas2D*, double x, double y, double w, double h);
void c2d_stroke_rect(Canvas2D*, double x, double y, double w, double h);
void c2d_clear_rect(Canvas2D*, double x, double y, double w, double h);

// Text (UTF-8) at (x, y) with the current font/textAlign/textBaseline. With has_max_width,
// text wider than max_width is condensed horizontally to fit; max_width <= 0 or NaN draws nothing.
void c2d_fill_text(Canvas2D*, const char *text, double x, double y, int has_max_width, double max_width);
void c2d_stroke_text(Canvas2D*, const char *text, double x, double y, int has_max_width, double max_width);
double c2d_measure_text(Canvas2D*, const char *text); // advance width in canvas px (current font)

// Images: one cached entry per file path (full path, e.g. under assets_path()). Native loads
// the file at the first call; the web starts a background download and reports "loading"
// until it arrived (v7 HELPER.loadImg: drawImage is skipped until isReady).
C2DImage *c2d_image(Canvas2D*, const char *path);
int c2d_image_state(const C2DImage*); // 1 ready, 0 loading, -1 failed
void c2d_image_size(const C2DImage*, int *w, int *h);
// drawImage(img, dx, dy, dw, dh); not-ready images draw nothing.
void c2d_draw_image(Canvas2D*, C2DImage*, double dx, double dy, double dw, double dh);
// drawImage(img, sx, sy, sw, sh, dx, dy, dw, dh): the source rect (image px) into the destination.
void c2d_draw_image_sub(Canvas2D*, C2DImage*, double sx, double sy, double sw, double sh, double dx, double dy, double dw, double dh);

// Frame
int c2d_is_empty(const Canvas2D*);  // the layer shows nothing and nothing is pending
void c2d_flush(Canvas2D*);          // replay pending commands into the layer (GL)
// Draws the layer over the currently bound framebuffer, covering the current viewport
// (flushes first). No-op when empty.
void c2d_composite(Canvas2D*);

// Stats for tests/benchmarks: commands, vertices and GL draw calls of the last flush.
void c2d_last_flush_stats(const Canvas2D*, int *commands, int *vertices, int *draw_calls);
