/*
 * svg.c - SVG image documents over nanosvg (see svg.h for the contract).
 */
#include "svg.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* nanosvg (client/lib/nanosvg, unmodified upstream 239e102, zlib licence):
 * the implementation is compiled into this translation unit only. */
#if defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#pragma GCC diagnostic ignored "-Wsign-compare"
#pragma GCC diagnostic ignored "-Wshadow"
#pragma GCC diagnostic ignored "-Wconversion"
#pragma GCC diagnostic ignored "-Wfloat-conversion"
#pragma GCC diagnostic ignored "-Wdouble-promotion"
#endif
#define NANOSVG_IMPLEMENTATION
#define NANOSVGRAST_IMPLEMENTATION
#include "../../../lib/nanosvg/nanosvg.h"
#include "../../../lib/nanosvg/nanosvgrast.h"
#if defined(__GNUC__)
#pragma GCC diagnostic pop
#endif

#define SVG_MAX_DIM 4096
#define SVG_SNIFF_LIMIT 4096

struct SvgImage {
    char *head;        /* document text before the root's attributes ("...<svg") */
    char *tail;        /* the root's attributes (width/height blanked) and the rest */
    size_t head_len, tail_len;
    int has_viewbox;
    float vb[4];       /* viewBox, or the synthesised "0 0 w h" */
    int align_x, align_y; /* preserveAspectRatio: 0 min, 1 mid, 2 max; -1 = none */
    int slice;
    float w, h;        /* intrinsic size, CSS px */
};

static int svg_space(const char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }

/* Root "<svg" start tag: the first "<svg" followed by whitespace, '>' or '/'. */
static const char *svg_find_root(const char *s, const char *end) {
    for (const char *p = s; p + 4 < end; p++) {
        if (p[0] == '<' && p[1] == 's' && p[2] == 'v' && p[3] == 'g' && (svg_space(p[4]) || p[4] == '>' || p[4] == '/'))
            return p;
    }
    return NULL;
}

int svg_sniff(const unsigned char *bytes, const size_t len) {
    if (!bytes) return 0;
    size_t i = len >= 3 && bytes[0] == 0xef && bytes[1] == 0xbb && bytes[2] == 0xbf ? 3 : 0;
    while (i < len && svg_space((char) bytes[i])) i++;
    if (i >= len || bytes[i] != '<') return 0;
    const size_t limit = len < SVG_SNIFF_LIMIT ? len : SVG_SNIFF_LIMIT;
    return svg_find_root((const char *) bytes + i, (const char *) bytes + limit) != NULL;
}

/* One attribute of the root tag [tag, tag_end): returns the value span and,
 * through attr_start / attr_end, the whole `name="value"` span. */
static const char *svg_root_attr(const char *tag, const char *tag_end, const char *name, size_t *len,
                                 const char **attr_start, const char **attr_end) {
    const size_t n = strlen(name);
    const char *p = tag + 4;
    while (p < tag_end) {
        while (p < tag_end && svg_space(*p)) p++;
        const char *nm = p;
        while (p < tag_end && !svg_space(*p) && *p != '=' && *p != '>' && *p != '/') p++;
        const size_t nl = (size_t) (p - nm);
        if (nl == 0) { p++; continue; }
        while (p < tag_end && svg_space(*p)) p++;
        const char *v = p, *v_end = p, *a_end = p;
        if (p < tag_end && *p == '=') {
            p++;
            while (p < tag_end && svg_space(*p)) p++;
            if (p < tag_end && (*p == '"' || *p == '\'')) {
                const char q = *p++;
                v = p;
                while (p < tag_end && *p != q) p++;
                v_end = p;
                if (p < tag_end) p++;
            } else {
                v = p;
                while (p < tag_end && !svg_space(*p) && *p != '>') p++;
                v_end = p;
            }
            a_end = p;
        }
        if (nl == n && !memcmp(nm, name, n)) {
            *len = (size_t) (v_end - v);
            if (attr_start) *attr_start = nm;
            if (attr_end) *attr_end = a_end;
            return v;
        }
    }
    return NULL;
}

/* <length> in CSS px for width/height: number + px / absolute unit.  0 for
 * percentages, font-relative units, "auto" or garbage (no intrinsic size). */
static float svg_length(const char *v, const size_t len) {
    char buf[64];
    if (!v || len == 0 || len >= sizeof buf) return 0.0f;
    memcpy(buf, v, len);
    buf[len] = 0;
    char *unit = NULL;
    const double num = strtod(buf, &unit);
    if (unit == buf || !(num > 0.0)) return 0.0f;
    while (svg_space(*unit)) unit++;
    static const struct { const char *unit; double px; } units[] = {
        {"", 1.0}, {"px", 1.0}, {"pt", 4.0 / 3.0}, {"pc", 16.0}, {"in", 96.0}, {"cm", 96.0 / 2.54}, {"mm", 96.0 / 25.4},
    };
    for (size_t i = 0; i < sizeof units / sizeof units[0]; i++) {
        const char *u = units[i].unit;
        size_t ul = strlen(u);
        if (!strncmp(unit, u, ul)) {
            const char *rest = unit + ul;
            while (svg_space(*rest)) rest++;
            if (!*rest) return (float) (num * units[i].px);
        }
    }
    return 0.0f;
}

static int svg_viewbox(const char *v, const size_t len, float out[4]) {
    char buf[128];
    if (!v || len == 0 || len >= sizeof buf) return 0;
    memcpy(buf, v, len);
    buf[len] = 0;
    char *p = buf;
    for (int i = 0; i < 4; i++) {
        while (svg_space(*p) || *p == ',') p++;
        char *e = NULL;
        out[i] = strtof(p, &e);
        if (e == p) return 0;
        p = e;
    }
    return out[2] > 0.0f && out[3] > 0.0f;
}

/* preserveAspectRatio = [defer] <align> [meet | slice]; anything invalid
 * keeps the default xMidYMid meet. */
static void svg_parse_par(SvgImage *svg, const char *v, const size_t len) {
    svg->align_x = svg->align_y = 1;
    svg->slice = 0;
    char buf[64];
    if (!v || len == 0 || len >= sizeof buf) return;
    memcpy(buf, v, len);
    buf[len] = 0;
    char *p = buf;
    while (svg_space(*p)) p++;
    if (!strncmp(p, "defer", 5)) p += 5;
    while (svg_space(*p)) p++;
    if (!strncmp(p, "none", 4)) {
        svg->align_x = svg->align_y = -1;
        return;
    }
    static const char *const pos[3] = {"Min", "Mid", "Max"};
    int ax = -1, ay = -1;
    for (int i = 0; i < 3; i++) {
        if (p[0] == 'x' && !strncmp(p + 1, pos[i], 3)) ax = i;
        if (p[4] == 'Y' && !strncmp(p + 5, pos[i], 3)) ay = i;
    }
    if (ax < 0 || ay < 0) return;
    p += 8;
    while (svg_space(*p)) p++;
    if (*p && strncmp(p, "meet", 4) && strncmp(p, "slice", 5)) return;
    svg->align_x = ax;
    svg->align_y = ay;
    svg->slice = !strncmp(p, "slice", 5);
}

/* ---- rgba() paints ----------------------------------------------------------
 * nanosvg reads no rgba(): it falls back to its named colours and paints opaque grey
 * (v10's k-ID header pattern, fill="rgba(255,255,255,0.05)", drew as grey diamonds).  On
 * the leaf elements below, a fill / stroke / stop-color attribute of rgba(r, g, b, a) is
 * rewritten to rgb(r, g, b) with its alpha multiplied into fill-opacity / stroke-opacity /
 * stop-opacity, which is what the colour's alpha means there (both multiply the paint).
 * Containers (<g>, <svg>) are left alone: an opacity attribute set there would be
 * inherited by children that name their own opaque colour. */

typedef struct SvgBuf {
    char *p;
    size_t n, cap;
    int oom;
} SvgBuf;

static void svg_buf_put(SvgBuf *b, const char *s, const size_t n) {
    if (b->oom || !n) return;
    if (b->n + n + 1 > b->cap) {
        size_t cap = b->cap ? b->cap : 256;
        while (b->n + n + 1 > cap) cap *= 2;
        char *grown = realloc(b->p, cap);
        if (!grown) {
            b->oom = 1;
            return;
        }
        b->p = grown;
        b->cap = cap;
    }
    memcpy(b->p + b->n, s, n);
    b->n += n;
    b->p[b->n] = 0;
}

static void svg_buf_str(SvgBuf *b, const char *s) { svg_buf_put(b, s, strlen(s)); }

static int svg_name_char(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == ':';
}

/* rgba(c, c, c, a) / rgba(c c c / a): the three channel tokens and the alpha (0..1). */
static int svg_parse_rgba(const char *v, const size_t len, char chans[3][24], float *alpha) {
    char buf[96];
    if (len >= sizeof buf) return 0;
    memcpy(buf, v, len);
    buf[len] = 0;
    char *p = buf;
    while (svg_space(*p)) p++;
    if (strncmp(p, "rgba(", 5)) return 0;
    p += 5;
    for (int i = 0; i < 4; i++) {
        while (svg_space(*p) || *p == ',' || (i == 3 && *p == '/')) p++;
        char *e = NULL;
        const float f = strtof(p, &e);
        if (e == p) return 0;
        const int pct = *e == '%';
        if (i < 3) {
            const size_t tl = (size_t) (e + pct - p);
            if (tl >= sizeof chans[i]) return 0;
            memcpy(chans[i], p, tl);
            chans[i][tl] = 0;
        } else {
            *alpha = pct ? f / 100.0f : f;
        }
        p = e + pct;
    }
    while (svg_space(*p)) p++;
    if (*p != ')') return 0;
    /* nanosvg's rgb() takes all-integer or all-percent channels: anything else stays as is */
    const int pct0 = strchr(chans[0], '%') != NULL;
    for (int i = 0; i < 3; i++) {
        if ((strchr(chans[i], '%') != NULL) != pct0 || (!pct0 && strchr(chans[i], '.'))) return 0;
    }
    if (*alpha < 0.0f) *alpha = 0.0f;
    if (*alpha > 1.0f) *alpha = 1.0f;
    return 1;
}

/* One start tag [tag, tag_end) of a leaf element, rebuilt into `out`. */
static void svg_rewrite_tag(SvgBuf *out, const char *tag, const char *tag_end, const char *name_end) {
    static const char *const paints[3] = {"fill", "stroke", "stop-color"};
    static const char *const opac[3] = {"fill-opacity", "stroke-opacity", "stop-opacity"};
    float alpha[3] = {1, 1, 1}, own[3] = {1, 1, 1};
    int has_alpha[3] = {0, 0, 0};
    /* pass 0 reads the paints and opacities, pass 1 writes the tag */
    for (int pass = 0; pass < 2; pass++) {
        const char *p = name_end;
        if (pass) svg_buf_put(out, tag, (size_t) (name_end - tag));
        while (p < tag_end) {
            const char *ws = p;
            while (p < tag_end && svg_space(*p)) p++;
            const char *nm = p;
            while (p < tag_end && svg_name_char(*p)) p++;
            const size_t nl = (size_t) (p - nm);
            if (!nl) {
                /* "/" or ">" (or junk): the rest of the tag is copied verbatim */
                if (pass) {
                    for (int k = 0; k < 3; k++) {
                        if (!has_alpha[k]) continue;
                        char a[64];
                        snprintf(a, sizeof a, " %s=\"%.6g\"", opac[k], (double) (alpha[k] * own[k]));
                        svg_buf_str(out, a);
                    }
                    svg_buf_put(out, ws, (size_t) (tag_end - ws));
                }
                break;
            }
            const char *q = p;
            while (q < tag_end && svg_space(*q)) q++;
            const char *v = NULL, *v_end = NULL;
            if (q < tag_end && *q == '=') {
                q++;
                while (q < tag_end && svg_space(*q)) q++;
                if (q < tag_end && (*q == '"' || *q == '\'')) {
                    const char quote = *q++;
                    v = q;
                    while (q < tag_end && *q != quote) q++;
                    v_end = q;
                    if (q < tag_end) q++;
                }
            }
            int handled = 0;
            for (int k = 0; k < 3 && v; k++) {
                char chans[3][24];
                float a = 1;
                if (strlen(paints[k]) == nl && !memcmp(nm, paints[k], nl) && svg_parse_rgba(v, (size_t) (v_end - v), chans, &a)) {
                    if (!pass) {
                        has_alpha[k] = 1;
                        alpha[k] = a;
                    } else {
                        char attr[128];
                        snprintf(attr, sizeof attr, "%.*s%s=\"rgb(%s,%s,%s)\"", (int) (nm - ws), ws, paints[k], chans[0], chans[1], chans[2]);
                        svg_buf_str(out, attr);
                    }
                    handled = 1;
                } else if (strlen(opac[k]) == nl && !memcmp(nm, opac[k], nl)) {
                    if (!pass) own[k] = strtof(v, NULL);
                    handled = has_alpha[k]; /* folded into the rewritten one */
                }
            }
            if (pass && !handled) svg_buf_put(out, ws, (size_t) (q - ws));
            p = q;
        }
    }
}

/* The document with the rgba() paints of its leaf elements rewritten; NULL when it has
 * none (or on allocation failure: the original is used). */
static char *svg_rewrite_rgba(const char *text, const size_t len, size_t *out_len) {
    static const char *const leaves[] = {"path", "rect", "circle", "ellipse", "line", "polyline", "polygon", "stop"};
    const char *end = text + len;
    int found = 0;
    for (const char *p = text; p + 5 <= end && !found; p++) found = !memcmp(p, "rgba(", 5);
    if (!found) return NULL;
    SvgBuf out = {0};
    const char *copied = text;
    for (const char *p = text; p < end; p++) {
        if (*p != '<') continue;
        const char *nm = p + 1, *ne = nm;
        while (ne < end && svg_name_char(*ne)) ne++;
        int leaf = 0;
        for (size_t i = 0; i < sizeof leaves / sizeof leaves[0]; i++) {
            const size_t l = strlen(leaves[i]);
            if ((size_t) (ne - nm) == l && !memcmp(nm, leaves[i], l)) leaf = 1;
        }
        if (!leaf) continue;
        const char *te = ne;
        char quote = 0;
        while (te < end && (quote || *te != '>')) {
            if (quote && *te == quote) quote = 0;
            else if (!quote && (*te == '"' || *te == '\'')) quote = *te;
            te++;
        }
        if (te >= end) break;
        te++; /* past '>' */
        svg_buf_put(&out, copied, (size_t) (p - copied));
        svg_rewrite_tag(&out, p, te, ne);
        copied = te;
        p = te - 1;
    }
    svg_buf_put(&out, copied, (size_t) (end - copied));
    if (out.oom || !out.p) {
        free(out.p);
        return NULL;
    }
    *out_len = out.n;
    return out.p;
}

static SvgImage *svg_image_parse(const char *text, const size_t len) {
    const char *end = text + len;
    const char *root = svg_find_root(text, end);
    if (!root) return NULL;
    const char *tag_end = memchr(root, '>', (size_t) (end - root));
    if (!tag_end) return NULL;

    SvgImage *svg = calloc(1, sizeof *svg);
    if (!svg) return NULL;
    svg->head_len = (size_t) (root + 4 - text);
    svg->tail_len = (size_t) (end - (root + 4));
    svg->head = malloc(svg->head_len + 1);
    svg->tail = malloc(svg->tail_len + 1);
    if (!svg->head || !svg->tail) {
        svg_image_destroy(svg);
        return NULL;
    }
    memcpy(svg->head, text, svg->head_len);
    svg->head[svg->head_len] = 0;
    memcpy(svg->tail, root + 4, svg->tail_len);
    svg->tail[svg->tail_len] = 0;

    size_t wl = 0, hl = 0, vl = 0;
    const char *ws, *we, *hs, *he;
    const char *wv = svg_root_attr(root, tag_end, "width", &wl, &ws, &we);
    const char *hv = svg_root_attr(root, tag_end, "height", &hl, &hs, &he);
    const char *vv = svg_root_attr(root, tag_end, "viewBox", &vl, NULL, NULL);
    size_t pl = 0;
    const char *pv = svg_root_attr(root, tag_end, "preserveAspectRatio", &pl, NULL, NULL);
    svg_parse_par(svg, pv, pl);
    float vb[4] = {0, 0, 0, 0};
    svg->has_viewbox = svg_viewbox(vv, vl, vb);
    float w = svg_length(wv, wl), h = svg_length(hv, hl);
    if (svg->has_viewbox) {
        if (w <= 0 && h > 0) w = h * vb[2] / vb[3];
        else if (h <= 0 && w > 0) h = w * vb[3] / vb[2];
        else if (w <= 0 && h <= 0) { w = vb[2]; h = vb[3]; }
    }
    svg->w = w > 0 ? w : 300.0f;
    svg->h = h > 0 ? h : 150.0f;
    /* Blink synthesises a viewBox for SVG images without one */
    if (svg->has_viewbox) memcpy(svg->vb, vb, sizeof vb);
    else {
        svg->vb[2] = svg->w;
        svg->vb[3] = svg->h;
        svg->align_x = svg->align_y = 1;
        svg->slice = 0;
    }

    /* blank width/height in the tail copy: each raster pass sets its own */
    if (wv) memset(svg->tail + (ws - (root + 4)), ' ', (size_t) (we - ws));
    if (hv) memset(svg->tail + (hs - (root + 4)), ' ', (size_t) (he - hs));
    return svg;
}

SvgImage *svg_image_create(const unsigned char *bytes, const size_t len) {
    if (!bytes || !len) return NULL;
    size_t rewritten_len = 0;
    char *rewritten = svg_rewrite_rgba((const char *) bytes, len, &rewritten_len);
    SvgImage *svg = rewritten ? svg_image_parse(rewritten, rewritten_len) : svg_image_parse((const char *) bytes, len);
    free(rewritten);
    return svg;
}

void svg_image_destroy(SvgImage *svg) {
    if (!svg) return;
    free(svg->head);
    free(svg->tail);
    free(svg);
}

void svg_image_size(const SvgImage *svg, float *w, float *h) {
    *w = svg ? svg->w : 0.0f;
    *h = svg ? svg->h : 0.0f;
}

unsigned char *svg_image_rasterize(const SvgImage *svg, const int w, const int h) {
    return svg_image_rasterize_placed(svg, w, h, (float) w, (float) h, 0.0f, 0.0f);
}

unsigned char *svg_image_rasterize_placed(const SvgImage *svg, const int w, const int h, const float vpw, const float vph,
                                          const float ox, const float oy) {
    if (!svg || w < 1 || h < 1 || w > SVG_MAX_DIM || h > SVG_MAX_DIM || !(vpw > 0.0f) || !(vph > 0.0f)) return NULL;
    /* The viewBox -> viewport mapping is done here, not by nanosvg: its
     * default is preserveAspectRatio="none" and it misreads the Y alignment
     * of xMidYMid-style values.  For uniform scaling the document is parsed
     * with a viewport of the viewBox size (identity mapping) and rasterised
     * with scale + offset; for "none" nanosvg stretches it to w x h itself. */
    const float vw = svg->vb[2], vh = svg->vb[3];
    float scale = 1.0f, tx = ox, ty = oy;
    char attrs[200];
    int al;
    if (svg->align_x < 0) {
        al = snprintf(attrs, sizeof attrs, " width=\"%.9g\" height=\"%.9g\"", (double) vpw, (double) vph); /* has a viewBox */
    } else {
        const float sx = vpw / vw, sy = vph / vh;
        scale = svg->slice ? fmaxf(sx, sy) : fminf(sx, sy);
        tx += (vpw - vw * scale) * 0.5f * (float) svg->align_x;
        ty += (vph - vh * scale) * 0.5f * (float) svg->align_y;
        al = snprintf(attrs, sizeof attrs, " width=\"%.9g\" height=\"%.9g\"", (double) vw, (double) vh);
    }
    if (al > 0 && !svg->has_viewbox) {
        const int more = snprintf(attrs + al, sizeof attrs - (size_t) al, " viewBox=\"0 0 %.9g %.9g\"", (double) vw, (double) vh);
        al = more > 0 ? al + more : -1;
    }
    if (al <= 0 || (size_t) al >= sizeof attrs) return NULL;

    /* nsvgParse() tokenises in place: compose a fresh copy per pass */
    const size_t n = svg->head_len + (size_t) al + svg->tail_len;
    char *doc = malloc(n + 1);
    if (!doc) return NULL;
    memcpy(doc, svg->head, svg->head_len);
    memcpy(doc + svg->head_len, attrs, (size_t) al);
    memcpy(doc + svg->head_len + al, svg->tail, svg->tail_len);
    doc[n] = 0;
    NSVGimage *image = nsvgParse(doc, "px", 96.0f);
    free(doc);
    if (!image) return NULL;

    unsigned char *px = malloc((size_t) w * (size_t) h * 4);
    NSVGrasterizer *rast = px ? nsvgCreateRasterizer() : NULL;
    if (rast) nsvgRasterize(rast, image, tx, ty, scale, px, w, h, w * 4);
    else {
        free(px);
        px = NULL;
    }
    if (rast) nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    return px;
}
