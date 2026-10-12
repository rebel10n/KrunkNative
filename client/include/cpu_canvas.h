#pragma once
// A small CPU 2D rasterizer for the page's <canvas> elements (html_dom_canvas_put): the subset
// of CanvasRenderingContext2D the v7.1.4 minimap draws (src/libs/Minimap.js) on an opaque
// ("alpha: false") bitmap. Paths are filled with exact area coverage (anti-aliased, nonzero
// winding for the shapes used here) and source-over blended in straight RGBA8.
//   - transform: save / restore / translate / rotate (canvas order: the last call applies first)
//   - fill: rect, polygon, circle; stroke: rect, closed polygon (miter joins), circle
// Coordinates are canvas px before the transform. Text is not drawn here: the caller puts DOM
// text over the canvas. (client/include/canvas2d.h is the GL canvas of KrunkScript's overlay,
// which composites over the framebuffer; the page's canvases need the pixels on the CPU.)
#include <stddef.h>
#include <stdint.h>

#define CC_STACK_MAX 8

typedef struct {
    float r, g, b, a; // 0..255 colour, a 0..1
} CcColor;

typedef struct {
    int w, h;
    uint8_t *px;      // RGBA8, row 0 on top (html_dom_canvas_put layout)
    float *acc;       // coverage accumulation, (w + 2) x h
    float m[6];       // a b c d e f: x' = a x + c y + e, y' = b x + d y + f
    float stack[CC_STACK_MAX][6];
    int depth;
    float alpha;      // globalAlpha
} CpuCanvas;

int  cc_resize(CpuCanvas*, int w, int h); // (re)allocates; 0 on failure. Transform reset.
void cc_free(CpuCanvas*);
void cc_save(CpuCanvas*);
void cc_restore(CpuCanvas*);
void cc_translate(CpuCanvas*, float x, float y);
void cc_rotate(CpuCanvas*, float radians);
void cc_reset_transform(CpuCanvas*);

CcColor cc_rgba(float r, float g, float b, float a);
CcColor cc_hsl(float h_deg, float s_pct, float l_pct); // CSS hsl()
CcColor cc_hex(const char *hex);                       // "#rgb" / "#rrggbb"

void cc_fill_all(CpuCanvas*, CcColor);                  // fillRect(0, 0, w, h) without the transform
void cc_fill_rect(CpuCanvas*, float x, float y, float w, float h, CcColor);
void cc_stroke_rect(CpuCanvas*, float x, float y, float w, float h, float line_width, CcColor);
void cc_fill_poly(CpuCanvas*, const float *xy, int n, CcColor);
void cc_stroke_poly(CpuCanvas*, const float *xy, int n, float line_width, CcColor); // closed path
void cc_fill_circle(CpuCanvas*, float cx, float cy, float r, CcColor);
void cc_stroke_circle(CpuCanvas*, float cx, float cy, float r, float line_width, CcColor);
// drawImage(img, x, y, w, h): straight RGBA8 iw x ih, bilinear (imageSmoothingEnabled).
void cc_draw_image(CpuCanvas*, const uint8_t *rgba, int iw, int ih, float x, float y, float w, float h);
