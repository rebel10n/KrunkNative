/*
 * svg.h - SVG images (<img src="*.svg">, url(*.svg), data:image/svg+xml) for
 * the HTML/CSS engine, on top of nanosvg (client/lib/nanosvg, zlib licence).
 *
 * An SvgImage keeps the document text; every svg_image_rasterize() call
 * re-parses it with the root viewport set to the requested device size, so
 * the viewBox and preserveAspectRatio (default xMidYMid meet) map onto exactly
 * the pixels being drawn, like Blink rasterising an SVG image at its drawn
 * size.  As in Blink's SVG-as-image documents, a root without a viewBox gets
 * one synthesised from its intrinsic size ("0 0 w h"), so it scales instead of
 * clipping.  Supported: what nanosvg parses (paths and basic shapes, fill and
 * stroke, <g>/transforms, linear/radial gradients, simple `.class` <style>
 * rules); no text, filters, masks or images.
 *
 * Intrinsic size (CSS px): width/height attributes in px or absolute units;
 * a missing or percentage one is derived from the viewBox ratio, else the
 * viewBox size, else 300x150 (the replaced-element default).
 */
#ifndef KN_SVG_H
#define KN_SVG_H

#include <stddef.h>

typedef struct SvgImage SvgImage;

/* 1 when the bytes look like an SVG document: optional BOM / XML prolog /
 * comments, then an <svg root within the first 4 KiB. */
int svg_sniff(const unsigned char *bytes, size_t len);

/* Copies the document; NULL when no <svg> root is found or allocation fails. */
SvgImage *svg_image_create(const unsigned char *bytes, size_t len);
void svg_image_destroy(SvgImage *svg);

/* Intrinsic size in CSS px (always > 0). */
void svg_image_size(const SvgImage *svg, float *w, float *h);

/* Rasterises the document into a w x h viewport (device px, 1..4096).
 * Returns malloc'd straight (non-premultiplied) RGBA8, row 0 = top, or NULL
 * when the document does not parse. */
unsigned char *svg_image_rasterize(const SvgImage *svg, int w, int h);

/* Same into an out_w x out_h buffer, the viewport being vw x vh (fractional)
 * at (ox, oy): lets a caller keep a fractional device position exact while
 * drawing the raster texel-aligned (no resampling blur). */
unsigned char *svg_image_rasterize_placed(const SvgImage *svg, int out_w, int out_h, float vw, float vh, float ox, float oy);

#endif /* KN_SVG_H */
