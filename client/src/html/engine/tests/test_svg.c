/*
 * test_svg.c - standalone tests for svg.c (SVG images over nanosvg).
 *
 * Build (throwaway, from the repository root):
 *   cc -std=c11 -g -Wall -Wextra -Iclient/src/html/engine \
 *      client/src/html/engine/svg.c client/src/html/engine/tests/test_svg.c -lm -o test_svg
 *   ./test_svg [repo_root]
 *
 * Checks sniffing, intrinsic sizes, the viewBox -> viewport mapping that
 * Chrome applies to SVG images (preserveAspectRatio meet/slice/none, the
 * synthesised viewBox of a root without one), <style> class fills, strokes,
 * and the page's own icons (ui/img/ *.svg).  Pixel expectations were taken
 * from headless Chromium screenshots of the same documents.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "svg.h"

static int g_fail, g_pass;

#define CHECK(cond) do { \
    if (cond) g_pass++; \
    else { g_fail++; fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

static SvgImage *mk(const char *text) { return svg_image_create((const unsigned char *) text, strlen(text)); }

static const unsigned char *px_at(const unsigned char *px, int w, int x, int y) { return px + ((size_t) y * w + x) * 4; }

/* Alpha-covered bounding box of a raster: x0, y0, x1, y1 (inclusive), -1 when empty. */
static void cover_box(const unsigned char *px, int w, int h, int box[4]) {
    box[0] = box[1] = box[2] = box[3] = -1;
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++)
            if (px_at(px, w, x, y)[3] > 127) {
                if (box[0] < 0 || x < box[0]) box[0] = x;
                if (box[1] < 0) box[1] = y;
                if (x > box[2]) box[2] = x;
                box[3] = y;
            }
}

static void test_sniff(void) {
    const char *a = "\xef\xbb\xbf<?xml version=\"1.0\"?>\n<!-- c --><svg xmlns=\"http://www.w3.org/2000/svg\"/>";
    CHECK(svg_sniff((const unsigned char *) a, strlen(a)));
    const char *b = " <svg>";
    CHECK(svg_sniff((const unsigned char *) b, strlen(b)));
    const char *c = "\x89PNG\r\n";
    CHECK(!svg_sniff((const unsigned char *) c, strlen(c)));
    const char *d = "<html><body></body></html>";
    CHECK(!svg_sniff((const unsigned char *) d, strlen(d)));
    const char *e = "<svgx>";
    CHECK(!svg_sniff((const unsigned char *) e, strlen(e)));
    CHECK(mk("<html/>") == NULL);
}

static void test_sizes(void) {
    float w, h;
    SvgImage *s = mk("<svg viewBox=\"0 0 512 512\" width=\"512\" height=\"512\"></svg>");
    svg_image_size(s, &w, &h);
    CHECK(w == 512 && h == 512);
    svg_image_destroy(s);
    s = mk("<svg viewBox=\"0 0 2 2\"><path d=\"M0 0H1V1z\"/></svg>");     /* pickr checkerboard */
    svg_image_size(s, &w, &h);
    CHECK(w == 2 && h == 2);
    svg_image_destroy(s);
    s = mk("<svg viewBox=\"0 0 100 50\" width=\"40\"></svg>");            /* height from the ratio */
    svg_image_size(s, &w, &h);
    CHECK(w == 40 && h == 20);
    svg_image_destroy(s);
    s = mk("<svg viewBox=\"0 0 100 50\" height=\"10px\"></svg>");
    svg_image_size(s, &w, &h);
    CHECK(w == 20 && h == 10);
    svg_image_destroy(s);
    s = mk("<svg width=\"12pt\" height=\"1in\"></svg>");
    svg_image_size(s, &w, &h);
    CHECK(fabsf(w - 16) < 1e-3f && fabsf(h - 96) < 1e-3f);
    svg_image_destroy(s);
    s = mk("<svg width=\"100%\" height=\"auto\"></svg>");                 /* replaced-element default */
    svg_image_size(s, &w, &h);
    CHECK(w == 300 && h == 150);
    svg_image_destroy(s);
}

static void test_mapping(void) {
    int box[4];
    /* default xMidYMid meet: a full-viewBox rect letterboxes in a wide viewport */
    SvgImage *s = mk("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 10 10\" width=\"10\" height=\"10\">"
                     "<rect width=\"10\" height=\"10\" fill=\"#f00\"/></svg>");
    unsigned char *px = svg_image_rasterize(s, 40, 20);
    CHECK(px != NULL);
    if (px) {
        cover_box(px, 40, 20, box);
        CHECK(box[0] == 10 && box[2] == 29 && box[1] == 0 && box[3] == 19);
        const unsigned char *c = px_at(px, 40, 20, 10);
        CHECK(c[0] == 255 && c[1] == 0 && c[2] == 0 && c[3] == 255);   /* straight RGBA */
    }
    free(px);
    svg_image_destroy(s);

    /* xMinYMin slice: scaled to cover, anchored top-left (Chrome: yellow square fills the left half) */
    s = mk("<svg viewBox=\"0 0 10 10\" preserveAspectRatio=\"xMinYMin slice\">"
           "<rect width=\"5\" height=\"5\" fill=\"#ff0\"/><rect x=\"5\" y=\"5\" width=\"5\" height=\"5\" fill=\"#0ff\"/></svg>");
    px = svg_image_rasterize(s, 80, 40);
    if (px) {
        cover_box(px, 80, 40, box);
        CHECK(box[0] == 0 && box[1] == 0 && box[2] == 39 && box[3] == 39);
        CHECK(px_at(px, 80, 20, 20)[0] == 255 && px_at(px, 80, 20, 20)[2] == 0);
    } else CHECK(0);
    free(px);
    svg_image_destroy(s);

    /* xMaxYMax meet */
    s = mk("<svg viewBox=\"0 0 10 10\" preserveAspectRatio=\"xMaxYMax meet\"><rect width=\"10\" height=\"10\"/></svg>");
    px = svg_image_rasterize(s, 30, 10);
    if (px) {
        cover_box(px, 30, 10, box);
        CHECK(box[0] == 20 && box[2] == 29);
    } else CHECK(0);
    free(px);
    svg_image_destroy(s);

    /* none: stretched */
    s = mk("<svg viewBox=\"0 0 10 10\" preserveAspectRatio=\"none\"><rect width=\"10\" height=\"10\"/></svg>");
    px = svg_image_rasterize(s, 30, 10);
    if (px) {
        cover_box(px, 30, 10, box);
        CHECK(box[0] == 0 && box[2] == 29 && box[1] == 0 && box[3] == 9);
    } else CHECK(0);
    free(px);
    svg_image_destroy(s);

    /* no viewBox: Blink synthesises "0 0 w h", so the content scales (not clipped) */
    s = mk("<svg width=\"10\" height=\"10\"><rect x=\"5\" y=\"5\" width=\"5\" height=\"5\" fill=\"#00f\"/></svg>");
    px = svg_image_rasterize(s, 40, 40);
    if (px) {
        cover_box(px, 40, 40, box);
        CHECK(box[0] == 20 && box[1] == 20 && box[2] == 39 && box[3] == 39);
    } else CHECK(0);
    free(px);
    svg_image_destroy(s);

    /* viewBox origin offset */
    s = mk("<svg viewBox=\"5 5 10 10\"><rect x=\"5\" y=\"5\" width=\"5\" height=\"5\"/></svg>");
    px = svg_image_rasterize(s, 20, 20);
    if (px) {
        cover_box(px, 20, 20, box);
        CHECK(box[0] == 0 && box[1] == 0 && box[2] == 9 && box[3] == 9);
    } else CHECK(0);
    free(px);
    svg_image_destroy(s);

    /* placed: a 10 x 10 viewport at (0.5, 0.25) inside an 11 x 11 buffer keeps
     * the fractional position (half-covered first column, 3/4-covered first row) */
    s = mk("<svg viewBox=\"0 0 10 10\"><rect width=\"10\" height=\"10\" fill=\"#fff\"/></svg>");
    px = svg_image_rasterize_placed(s, 11, 11, 10, 10, 0.5f, 0.25f);
    if (px) {
        const int a0 = px_at(px, 11, 0, 5)[3], a1 = px_at(px, 11, 5, 0)[3], am = px_at(px, 11, 5, 5)[3];
        CHECK(a0 > 100 && a0 < 156);        /* ~50 % */
        CHECK(a1 > 160 && a1 < 220);        /* ~75 % */
        CHECK(am == 255 && px_at(px, 11, 10, 5)[3] > 100 && px_at(px, 11, 10, 5)[3] < 156);   /* right column ~50 % */
        CHECK(px_at(px, 11, 5, 10)[3] > 35 && px_at(px, 11, 5, 10)[3] < 95);                   /* bottom row ~25 % */
    } else CHECK(0);
    free(px);
    svg_image_destroy(s);

    CHECK(svg_image_rasterize(NULL, 4, 4) == NULL);
    s = mk("<svg viewBox=\"0 0 1 1\"/>");
    CHECK(svg_image_rasterize(s, 0, 4) == NULL && svg_image_rasterize(s, 5000, 4) == NULL);
    svg_image_destroy(s);
}

static void test_paint(void) {
    /* <style> class fill (the page's icons use .shp0 { fill: #FFFFFF }) */
    SvgImage *s = mk("<svg viewBox=\"0 0 4 4\"><style>tspan { white-space:pre } .shp0 { fill: #FFFFFF }</style>"
                     "<path class=\"shp0\" d=\"M0 0H4V4H0Z\"/></svg>");
    unsigned char *px = svg_image_rasterize(s, 4, 4);
    if (px) {
        const unsigned char *c = px_at(px, 4, 2, 2);
        CHECK(c[0] == 255 && c[1] == 255 && c[2] == 255 && c[3] == 255);
    } else CHECK(0);
    free(px);
    svg_image_destroy(s);

    /* stroke-only diagonal (pickr's close icon): colour on the diagonal, nothing in the corner */
    s = mk("<svg xmlns=\"http://www.w3.org/2000/svg\" viewBox=\"0 0 50 50\" stroke=\"#42445A\" stroke-width=\"5px\" "
           "stroke-linecap=\"round\"><path d=\"M45,45L5,5\"></path><path d=\"M45,5L5,45\"></path></svg>");
    px = svg_image_rasterize(s, 50, 50);
    if (px) {
        const unsigned char *c = px_at(px, 50, 25, 25);
        CHECK(c[3] == 255 && c[0] == 0x42 && c[1] == 0x44 && c[2] == 0x5a);
        CHECK(px_at(px, 50, 25, 2)[3] == 0);
    } else CHECK(0);
    free(px);
    svg_image_destroy(s);
}

static unsigned char *read_file(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *b = malloc((size_t) n + 1);
    if (b && fread(b, 1, (size_t) n, f) != (size_t) n) { free(b); b = NULL; }
    fclose(f);
    *len = (size_t) n;
    return b;
}

/* ui/img icons: intrinsic size and the drawn coverage at the page's sizes;
 * boxes measured on headless Chromium 154 screenshots (alpha > 50 %). */
static void test_page_icons(const char *root) {
    static const struct { const char *file; float w, h; int dw, dh; int box[4]; } icons[] = {
        /* hub Clan Wars nav icon: <img style="width:60px"> */
        {"ui/img/crossed_swords_0.svg", 512, 512, 60, 60, {12, 14, 47, 47}},
        {"ui/img/shaking_hands.svg", 980, 628, 100, 100, {3, 21, 95, 78}},   /* letterboxed (meet) */
        {"ui/img/medals/badge_0.svg", 400, 400, 40, 40, {-1, -1, -1, -1}}, /* v3.1.1 img/medal.svg, moved in v7.1.4 */
    };
    char path[1024];
    for (size_t i = 0; i < sizeof icons / sizeof icons[0]; i++) {
        size_t len = 0;
        snprintf(path, sizeof path, "%s/%s", root, icons[i].file);
        unsigned char *bytes = read_file(path, &len);
        if (!bytes) { fprintf(stderr, "missing %s\n", path); g_fail++; continue; }
        CHECK(svg_sniff(bytes, len));
        SvgImage *s = svg_image_create(bytes, len);
        free(bytes);
        float w = 0, h = 0;
        svg_image_size(s, &w, &h);
        CHECK(w == icons[i].w && h == icons[i].h);
        unsigned char *px = svg_image_rasterize(s, icons[i].dw, icons[i].dh);
        CHECK(px != NULL);
        if (px) {
            int box[4];
            cover_box(px, icons[i].dw, icons[i].dh, box);
            CHECK(box[0] >= 0);                         /* something drawn */
            for (int k = 0; k < 4; k++) {
                if (icons[i].box[k] < 0) continue;
                if (abs(box[k] - icons[i].box[k]) > 1) {
                    fprintf(stderr, "  %s box[%d] = %d, expected %d\n", icons[i].file, k, box[k], icons[i].box[k]);
                    g_fail++;
                } else g_pass++;
            }
        }
        free(px);
        svg_image_destroy(s);
    }
}

/* rgba() paints (svg.c svg_rewrite_rgba): nanosvg alone paints them opaque grey */
static void test_rgba(void) {
    static const struct { const char *doc; int r, g, b, a; } cases[] = {
        /* v10's k-ID header pattern: 5 % white */
        {"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><path fill=\"rgba(255,255,255,0.05)\" d=\"M0 0H10V10H0Z\"/></svg>", 255, 255, 255, 12},
        /* the colour's alpha times the element's own fill-opacity; a percentage alpha */
        {"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><rect fill-opacity=\"0.5\" fill=\"rgba(255, 0, 0, 50%)\" width=\"10\" height=\"10\"/></svg>", 255, 0, 0, 63},
        /* a container's rgba() is left alone: the child's own opaque colour wins */
        {"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><g fill=\"rgba(0,0,255,0.5)\"><rect fill=\"red\" width=\"10\" height=\"10\"/></g></svg>", 255, 0, 0, 255},
        /* only the stroke has an rgba(): the fill keeps full opacity */
        {"<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"10\" height=\"10\"><rect stroke=\"rgba(0,0,0,.2)\" fill=\"rgb(0,128,0)\" width=\"10\" height=\"10\"/></svg>", 0, 128, 0, 255},
    };
    for (size_t i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        SvgImage *s = mk(cases[i].doc);
        unsigned char *px = s ? svg_image_rasterize(s, 10, 10) : NULL;
        CHECK(px != NULL);
        if (px) {
            const unsigned char *c = px_at(px, 10, 5, 5);
            const int ok = abs(c[0] - cases[i].r) <= 1 && abs(c[1] - cases[i].g) <= 1 && abs(c[2] - cases[i].b) <= 1 && abs(c[3] - cases[i].a) <= 1;
            if (!ok) fprintf(stderr, "  rgba case %zu: got %d %d %d %d\n", i, c[0], c[1], c[2], c[3]);
            CHECK(ok);
        }
        free(px);
        svg_image_destroy(s);
    }
}

int main(int argc, char **argv) {
    const char *root = argc > 1 ? argv[1] : ".";
    test_sniff();
    test_sizes();
    test_mapping();
    test_paint();
    test_rgba();
    test_page_icons(root);
    printf("test_svg: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail ? 1 : 0;
}
