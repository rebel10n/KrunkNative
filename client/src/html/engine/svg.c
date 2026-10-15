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
#define SVG_MAX_REFS 16    /* <mask> / <clipPath> definitions */
#define SVG_MAX_FILTERS 8  /* <filter> definitions */
#define SVG_MAX_FX 32      /* elements with filter= / clip-path= / mask= */
#define SVG_MAX_SHADOWS 4  /* shadow passes of one filter */

/* <mask> (luminance x alpha) or <clipPath> (alpha): its content as a document
 * of its own, rasterised in the root's user space. */
typedef struct {
    char id[64];
    int clip;
    char *tail;        /* the root's attributes + the definition's content + "</svg>" */
    size_t tail_len;
    int has_rect;      /* maskUnits="userSpaceOnUse" x/y/width/height */
    float rect[4];
} SvgRef;

/* A <filter> read as the shadows Figma / Inkscape export: per feOffset (or
 * feDropShadow) an offset, an optional feGaussianBlur, an feComposite
 * ("out": outside the source only; arithmetic k2=-1 k3=1: an inner shadow)
 * and the colour of the last feColorMatrix / flood. Other primitives are
 * ignored (the source is drawn as is). */
typedef struct {
    float dx, dy, sd;
    float rgba[4];
    int out, inner;
} SvgShadow;
typedef struct {
    char id[64];
    int count;
    SvgShadow sh[SVG_MAX_SHADOWS];
} SvgFilter;

/* An element drawn through filter= / clip-path= / mask= (indices, -1 = none):
 * its shapes sit between two sentinel shapes "__knfxb<i>" / "__knfxe<i>". */
typedef struct {
    int filter, clip, mask;
    float ctm[6];      /* the element's user space in the root's (its own transform and its
                        * ancestors'): where its mask / clip content (userSpaceOnUse) and its
                        * filter's offsets and blur live (nanosvg layout: x' = a x + c y + e) */
} SvgFx;

struct SvgImage {
    char *head;        /* document text before the root's attributes ("...<svg") */
    char *tail;        /* the root's attributes (width/height blanked) and the rest */
    size_t head_len, tail_len;
    int has_viewbox;
    float vb[4];       /* viewBox, or the synthesised "0 0 w h" */
    int align_x, align_y; /* preserveAspectRatio: 0 min, 1 mid, 2 max; -1 = none */
    int slice;
    float w, h;        /* intrinsic size, CSS px */
    SvgRef refs[SVG_MAX_REFS];
    int nrefs;
    SvgFilter filters[SVG_MAX_FILTERS];
    int nfilters;
    SvgFx fx[SVG_MAX_FX];
    int nfx;
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
static const char *svg_attr_from(const char *p, const char *tag_end, const char *name, size_t *len,
                                 const char **attr_start, const char **attr_end) {
    const size_t n = strlen(name);
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

static const char *svg_root_attr(const char *tag, const char *tag_end, const char *name, size_t *len,
                                 const char **attr_start, const char **attr_end) {
    return svg_attr_from(tag + 4, tag_end, name, len, attr_start, attr_end);
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

/* ------------------------------------------------------------------ masks,
 * clip paths and shadow filters.  nanosvg has none of them (it even draws the
 * shapes inside <mask> / <clipPath> as ordinary ones), so at load time the
 * definitions are cut out of the document and every element that uses one is
 * wrapped in two sentinel shapes; a raster pass then draws the shapes between
 * them into a layer of their own, applies the filter, the clip path and the
 * mask (that order, as SVG does) and composites the layer.  The definitions
 * and the filter's offsets / blur live in the user space of the element using
 * them (SvgFx.ctm: its transform and its ancestors', as SVG's userSpaceOnUse
 * says; Krunker Strike's ticket-outline.svg masks a path inside a scaled <g>).
 * Limits: mask/clip content units are userSpaceOnUse, and a filter is read as
 * its shadows (SvgFilter). */

typedef struct {
    const char *start, *end; /* '<' .. past '>' */
    const char *name;
    size_t name_len;
    int closing, self_closing;
} SvgTag;

static const char *svg_find(const char *p, const char *end, const char *what) {
    const size_t n = strlen(what);
    for (; p + n <= end; p++)
        if (!memcmp(p, what, n)) return p;
    return NULL;
}

/* The next element tag at or after p (comments, CDATA, <? ?> and <! >
 * declarations skipped); 0 at the end. */
static int svg_next_tag(const char *p, const char *end, SvgTag *t) {
    while (p < end) {
        const char *lt = memchr(p, '<', (size_t) (end - p));
        if (!lt) return 0;
        const char *skip = NULL;
        if (end - lt >= 4 && !memcmp(lt, "<!--", 4)) skip = svg_find(lt + 4, end, "-->");
        else if (end - lt >= 9 && !memcmp(lt, "<![CDATA[", 9)) skip = svg_find(lt + 9, end, "]]>");
        else if (lt + 1 < end && (lt[1] == '?' || lt[1] == '!')) skip = memchr(lt, '>', (size_t) (end - lt));
        if (skip || (lt + 1 < end && (lt[1] == '?' || lt[1] == '!'))) {
            if (!skip) return 0;
            p = skip + 1;
            continue;
        }
        const char *q = lt + 1;
        t->closing = q < end && *q == '/';
        if (t->closing) q++;
        t->name = q;
        while (q < end && !svg_space(*q) && *q != '>' && *q != '/') q++;
        t->name_len = (size_t) (q - t->name);
        char quote = 0;
        while (q < end && (quote || *q != '>')) {
            if (quote) {
                if (*q == quote) quote = 0;
            } else if (*q == '"' || *q == '\'') quote = *q;
            q++;
        }
        if (q >= end) return 0;
        t->start = lt;
        t->end = q + 1;
        t->self_closing = !t->closing && q[-1] == '/';
        return 1;
    }
    return 0;
}

static int svg_tag_is(const SvgTag *t, const char *name) {
    const size_t n = strlen(name);
    return t->name_len == n && !memcmp(t->name, name, n);
}

static const char *svg_tag_attr(const SvgTag *t, const char *name, size_t *len) {
    return svg_attr_from(t->name + t->name_len, t->end - 1, name, len, NULL, NULL);
}

/* Up to max numbers of an attribute (separated by spaces / commas); the count read. */
static int svg_tag_nums(const SvgTag *t, const char *name, float *out, const int max) {
    size_t len = 0;
    const char *v = svg_tag_attr(t, name, &len);
    char buf[512];
    if (!v || len >= sizeof buf) return 0;
    memcpy(buf, v, len);
    buf[len] = 0;
    char *p = buf;
    int n = 0;
    while (n < max) {
        while (svg_space(*p) || *p == ',') p++;
        char *e = NULL;
        const float f = strtof(p, &e);
        if (e == p) break;
        out[n++] = f;
        p = e;
    }
    return n;
}

static float svg_tag_num(const SvgTag *t, const char *name, const float def) {
    float v;
    return svg_tag_nums(t, name, &v, 1) == 1 ? v : def;
}

static int svg_tag_val_is(const SvgTag *t, const char *name, const char *want) {
    size_t len = 0;
    const char *v = svg_tag_attr(t, name, &len);
    return v && len == strlen(want) && !memcmp(v, want, len);
}

/* "url(#id)" -> id; 0 when the attribute is absent or not a local reference. */
static int svg_tag_url(const SvgTag *t, const char *name, char *id, const size_t size) {
    size_t len = 0;
    const char *v = svg_tag_attr(t, name, &len);
    if (!v) return 0;
    const char *e = v + len;
    while (v < e && svg_space(*v)) v++;
    if (e - v < 6 || memcmp(v, "url(", 4)) return 0;
    v += 4;
    while (v < e && (svg_space(*v) || *v == '"' || *v == '\'')) v++;
    if (v >= e || *v != '#') return 0;
    v++;
    size_t n = 0;
    while (v + n < e && v[n] != ')' && v[n] != '"' && v[n] != '\'' && !svg_space(v[n])) n++;
    if (!n || n >= size) return 0;
    memcpy(id, v, n);
    id[n] = 0;
    return 1;
}

static void svg_tag_id(const SvgTag *t, char *id, const size_t size) {
    size_t len = 0;
    const char *v = svg_tag_attr(t, "id", &len);
    id[0] = 0;
    if (v && len < size) {
        memcpy(id, v, len);
        id[len] = 0;
    }
}

/* flood-color / flood-opacity: #rgb, #rrggbb or black / white. */
static int svg_tag_flood(const SvgTag *t, float rgba[4]) {
    size_t len = 0;
    const char *v = svg_tag_attr(t, "flood-color", &len);
    float c[3] = {0, 0, 0};
    if (v && len >= 4 && v[0] == '#') {
        char hex[7] = {0};
        if (len == 4) for (int i = 0; i < 3; i++) hex[2 * i] = hex[2 * i + 1] = v[1 + i];
        else if (len == 7) memcpy(hex, v + 1, 6);
        for (int i = 0; i < 3 && hex[0]; i++) {
            char b[3] = {hex[2 * i], hex[2 * i + 1], 0};
            c[i] = (float) strtol(b, NULL, 16) / 255.0f;
        }
    } else if (v && len == 5 && !memcmp(v, "white", 5)) c[0] = c[1] = c[2] = 1.0f;
    rgba[0] = c[0];
    rgba[1] = c[1];
    rgba[2] = c[2];
    rgba[3] = svg_tag_num(t, "flood-opacity", 1.0f);
    return rgba[3] > 0.0f;
}

static int svg_find_ref(const SvgImage *svg, const char *id, const int clip) {
    for (int i = 0; i < svg->nrefs; i++)
        if (svg->refs[i].clip == clip && !strcmp(svg->refs[i].id, id)) return i;
    return -1;
}

static int svg_find_filter(const SvgImage *svg, const char *id) {
    for (int i = 0; i < svg->nfilters; i++)
        if (!strcmp(svg->filters[i].id, id)) return i;
    return -1;
}

typedef struct {
    size_t pos;
    int fx;
    char kind; /* 'b' / 'e' */
} SvgInsert;

typedef struct {
    size_t a, b;
} SvgCut;

/* Cuts the definitions out of svg->tail and wraps the elements using them in
 * sentinels (see above).  Nothing changes when the document has none. */
static void svg_fx_prepare(SvgImage *svg) {
    const char *text = svg->tail, *end = text + svg->tail_len;
    const char *root_end = memchr(text, '>', svg->tail_len);
    if (!root_end) return;
    const size_t attrs_len = (size_t) (root_end + 1 - text);

    struct {
        int def;           /* 1 mask, 2 clipPath, 3 filter, 4 defs, 0 other */
        int index;         /* ref / filter index */
        int fx;            /* fx index or -1 */
        const char *start; /* '<' of the start tag */
        const char *content;
        float xf[6];       /* the element's user space in the root's */
    } stack[64];
    int depth = 0, in_def = 0, in_filter = -1;
    SvgCut cuts[SVG_MAX_REFS + SVG_MAX_FILTERS];
    int ncuts = 0;
    SvgInsert ins[SVG_MAX_FX * 2];
    int nins = 0;
    struct {
        const char *start;
        char filter[64], clip[64], mask[64];
    } pend[SVG_MAX_FX];
    float flood[4] = {0, 0, 0, 0}, pend_blur = 0.0f;
    int has_flood = 0;

    SvgTag t;
    const char *p = root_end + 1;
    while (svg_next_tag(p, end, &t)) {
        p = t.end;
        if (t.closing) {
            int k = depth - 1;
            while (k >= 0 && !(stack[k].start && t.name_len && !memcmp(stack[k].start + 1, t.name, t.name_len)
                               && (svg_space(stack[k].start[1 + t.name_len]) || stack[k].start[1 + t.name_len] == '>')))
                k--;
            if (k < 0) continue;
            while (depth > k) {
                depth--;
                const int d = stack[depth].def, ix = stack[depth].index;
                if (d) in_def--;
                if (d == 3) in_filter = -1;
                if ((d == 1 || d == 2) && ix >= 0) {
                    /* the content inherits the definition's own presentation attributes (Figma's
                     * <mask fill="black">): a <g> carries them; a clip path is its geometry, filled */
                    SvgRef *r = &svg->refs[ix];
                    const size_t cl = depth == k ? (size_t) (t.start - stack[depth].content) : 0;
                    const char *ga = stack[depth].start + 1 + (d == 1 ? 4 : 8);
                    const size_t gl = (size_t) (stack[depth].content - ga);
                    const char *open = d == 2 ? "<g fill=\"black\"><g" : "<g", *close = d == 2 ? "</g></g></svg>" : "</g></svg>";
                    r->tail_len = attrs_len + strlen(open) + gl + cl + strlen(close);
                    r->tail = malloc(r->tail_len + 1);
                    if (r->tail)
                        snprintf(r->tail, r->tail_len + 1, "%.*s%s%.*s%.*s%s", (int) attrs_len, text, open, (int) gl, ga, (int) cl,
                                 stack[depth].content, close);
                }
                if ((d == 1 || d == 2 || d == 3) && ncuts < (int) (sizeof cuts / sizeof cuts[0])) {
                    cuts[ncuts].a = (size_t) (stack[depth].start - text);
                    cuts[ncuts].b = (size_t) (t.end - text);
                    ncuts++;
                }
                if (stack[depth].fx >= 0 && nins < SVG_MAX_FX * 2) {
                    ins[nins].pos = (size_t) (t.end - text);
                    ins[nins].fx = stack[depth].fx;
                    ins[nins].kind = 'e';
                    nins++;
                }
            }
            continue;
        }

        int def = 0, index = -1, fx = -1;
        /* this element's user space: its transform, then its parent's */
        float xf[6];
        nsvg__xformIdentity(xf);
        {
            size_t tl = 0;
            const char *tv = svg_tag_attr(&t, "transform", &tl);
            char tb[512];
            if (tv && tl < sizeof tb) {
                memcpy(tb, tv, tl);
                tb[tl] = 0;
                nsvg__parseTransform(xf, tb);
            }
            if (depth > 0) nsvg__xformMultiply(xf, stack[depth - 1].xf);
        }
        if (svg_tag_is(&t, "mask") || svg_tag_is(&t, "clipPath")) {
            def = svg_tag_is(&t, "mask") ? 1 : 2;
            if (svg->nrefs < SVG_MAX_REFS && !t.self_closing) {
                SvgRef *r = &svg->refs[svg->nrefs];
                memset(r, 0, sizeof *r);
                svg_tag_id(&t, r->id, sizeof r->id);
                r->clip = def == 2;
                float v[4];
                if (def == 1 && svg_tag_val_is(&t, "maskUnits", "userSpaceOnUse") && (v[0] = svg_tag_num(&t, "x", NAN), v[1] = svg_tag_num(&t, "y", NAN),
                    v[2] = svg_tag_num(&t, "width", NAN), v[3] = svg_tag_num(&t, "height", NAN), !isnan(v[0] + v[1] + v[2] + v[3]))) {
                    r->has_rect = 1;
                    memcpy(r->rect, v, sizeof v);
                }
                index = svg->nrefs++;
            }
        } else if (svg_tag_is(&t, "filter")) {
            def = 3;
            if (svg->nfilters < SVG_MAX_FILTERS && !t.self_closing) {
                SvgFilter *f = &svg->filters[svg->nfilters];
                memset(f, 0, sizeof *f);
                svg_tag_id(&t, f->id, sizeof f->id);
                index = svg->nfilters++;
                in_filter = index;
                has_flood = 0;
                pend_blur = 0.0f;
            }
        } else if (svg_tag_is(&t, "defs")) {
            def = 4;
        } else if (in_filter >= 0) {
            SvgFilter *f = &svg->filters[in_filter];
            SvgShadow *last = f->count ? &f->sh[f->count - 1] : NULL;
            if ((svg_tag_is(&t, "feOffset") || svg_tag_is(&t, "feDropShadow")) && f->count < SVG_MAX_SHADOWS) {
                SvgShadow *s = &f->sh[f->count++];
                memset(s, 0, sizeof *s);
                s->dx = svg_tag_num(&t, "dx", svg_tag_is(&t, "feDropShadow") ? 2.0f : 0.0f);
                s->dy = svg_tag_num(&t, "dy", svg_tag_is(&t, "feDropShadow") ? 2.0f : 0.0f);
                s->sd = svg_tag_is(&t, "feDropShadow") ? svg_tag_num(&t, "stdDeviation", 2.0f) : pend_blur;
                s->rgba[3] = 1.0f;
                if (svg_tag_is(&t, "feDropShadow")) svg_tag_flood(&t, s->rgba);
                else if (has_flood) memcpy(s->rgba, flood, sizeof flood);
                pend_blur = 0.0f;
            } else if (svg_tag_is(&t, "feGaussianBlur")) {
                if (last) last->sd = svg_tag_num(&t, "stdDeviation", 0.0f);
                else pend_blur = svg_tag_num(&t, "stdDeviation", 0.0f);
            } else if (svg_tag_is(&t, "feComposite") && last) {
                float k[4];
                if (svg_tag_val_is(&t, "operator", "out")) last->out = 1;
                else if (svg_tag_val_is(&t, "operator", "arithmetic") && (k[0] = svg_tag_num(&t, "k2", 0), k[1] = svg_tag_num(&t, "k3", 0), k[0] < 0 && k[1] > 0))
                    last->inner = 1;
            } else if (svg_tag_is(&t, "feColorMatrix") && last && !svg_tag_val_is(&t, "in", "SourceAlpha")) {
                float m[20];
                if (svg_tag_nums(&t, "values", m, 20) == 20) {
                    last->rgba[0] = m[4];
                    last->rgba[1] = m[9];
                    last->rgba[2] = m[14];
                    last->rgba[3] = m[18];
                }
            } else if (svg_tag_is(&t, "feFlood")) {
                float c[4];
                if (svg_tag_flood(&t, c)) {
                    memcpy(flood, c, sizeof c);
                    has_flood = 1;
                    if (last) memcpy(last->rgba, c, sizeof c);
                }
            }
        } else if (!in_def && svg->nfx < SVG_MAX_FX) {
            char a[64] = "", b[64] = "", c[64] = "";
            const int hf = svg_tag_url(&t, "filter", a, sizeof a);
            const int hc = svg_tag_url(&t, "clip-path", b, sizeof b);
            const int hm = svg_tag_url(&t, "mask", c, sizeof c);
            if (hf || hc || hm) {
                fx = svg->nfx++;
                memcpy(svg->fx[fx].ctm, xf, sizeof xf);
                pend[fx].start = t.start;
                memcpy(pend[fx].filter, a, sizeof a);
                memcpy(pend[fx].clip, b, sizeof b);
                memcpy(pend[fx].mask, c, sizeof c);
                if (nins < SVG_MAX_FX * 2) ins[nins++] = (SvgInsert) {(size_t) (t.start - text), fx, 'b'};
                if (t.self_closing && nins < SVG_MAX_FX * 2) ins[nins++] = (SvgInsert) {(size_t) (t.end - text), fx, 'e'};
            }
        }
        if (t.self_closing) continue;
        if (depth >= (int) (sizeof stack / sizeof stack[0])) break;
        stack[depth].def = def;
        stack[depth].index = index;
        stack[depth].fx = fx;
        stack[depth].start = t.start;
        stack[depth].content = t.end;
        memcpy(stack[depth].xf, xf, sizeof xf);
        depth++;
        if (def) in_def++;
    }

    /* the references resolve once every definition is known (masks often follow their users) */
    for (int i = 0; i < svg->nfx; i++) {
        svg->fx[i].filter = pend[i].filter[0] ? svg_find_filter(svg, pend[i].filter) : -1;
        svg->fx[i].clip = pend[i].clip[0] ? svg_find_ref(svg, pend[i].clip, 1) : -1;
        svg->fx[i].mask = pend[i].mask[0] ? svg_find_ref(svg, pend[i].mask, 0) : -1;
    }
    if (!ncuts && !nins) return;

    /* rewrite: definitions cut, sentinels inserted (inserts are in document order already) */
    static const char sentinel[] = "<path id=\"__knfx%c%d\" d=\"M0 0L0 0\" fill=\"none\" stroke=\"none\"/>";
    const size_t cap = svg->tail_len + (size_t) nins * (sizeof sentinel + 8) + 1;
    char *out = malloc(cap);
    if (!out) return;
    size_t o = 0, i = 0;
    int ii = 0;
    while (i <= svg->tail_len) {
        while (ii < nins && ins[ii].pos == i) {
            o += (size_t) snprintf(out + o, cap - o, sentinel, ins[ii].kind, ins[ii].fx);
            ii++;
        }
        if (i == svg->tail_len) break;
        int cut = -1;
        for (int c = 0; c < ncuts; c++)
            if (cuts[c].a == i) cut = c;
        if (cut >= 0) {
            i = cuts[cut].b; /* a cut never holds an insert: users are outside definitions */
            continue;
        }
        out[o++] = text[i++];
    }
    out[o] = 0;
    free(svg->tail);
    svg->tail = out;
    svg->tail_len = o;
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
    svg_fx_prepare(svg);
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
    for (int i = 0; i < svg->nrefs; i++) free(svg->refs[i].tail);
    free(svg);
}

void svg_image_size(const SvgImage *svg, float *w, float *h) {
    *w = svg ? svg->w : 0.0f;
    *h = svg ? svg->h : 0.0f;
}

/* nsvgParse() tokenises in place: compose a fresh copy per pass */
static NSVGimage *svg_parse_tail(const SvgImage *svg, const char *tail, const size_t tail_len, const char *attrs, const size_t al) {
    const size_t n = svg->head_len + al + tail_len;
    char *doc = malloc(n + 1);
    if (!doc) return NULL;
    memcpy(doc, svg->head, svg->head_len);
    memcpy(doc + svg->head_len, attrs, al);
    memcpy(doc + svg->head_len + al, tail, tail_len);
    doc[n] = 0;
    NSVGimage *image = nsvgParse(doc, "px", 96.0f);
    free(doc);
    return image;
}

typedef struct {
    const SvgImage *svg;
    NSVGrasterizer *rast;
    NSVGimage *image;
    int w, h;
    float tx, ty, rs;     /* nsvgRasterize's offset and scale */
    float sx, sy;         /* user space -> device px scale (rs, unless "none": nanosvg maps the viewBox) */
    const char *attrs;
    size_t al;
    unsigned char *tmp;   /* straight RGBA of one nsvgRasterize pass */
} SvgCtx;

static unsigned char svg_mul8(const unsigned v, const unsigned a) { return (unsigned char) ((v * a + 127) / 255); }

/* dst (premultiplied) = src over dst; src straight or premultiplied */
static void svg_over(unsigned char *dst, const unsigned char *src, const size_t pixels, const int premul) {
    for (size_t i = 0; i < pixels; i++, dst += 4, src += 4) {
        const unsigned sa = src[3];
        if (!sa) continue;
        const unsigned inv = 255 - sa;
        for (int c = 0; c < 3; c++) dst[c] = (unsigned char) ((premul ? src[c] : svg_mul8(src[c], sa)) + svg_mul8(dst[c], inv));
        dst[3] = (unsigned char) (sa + svg_mul8(dst[3], inv));
    }
}

static void svg_unpremultiply(unsigned char *px, const size_t pixels) {
    for (size_t i = 0; i < pixels; i++, px += 4) {
        const unsigned a = px[3];
        if (!a || a == 255) continue;
        for (int c = 0; c < 3; c++) {
            const unsigned v = (px[c] * 255u + a / 2) / a;
            px[c] = (unsigned char) (v > 255 ? 255 : v);
        }
    }
}

/* "__knfx<kind><i>" -> i, else -1 */
static int svg_sentinel(const NSVGshape *s, const char kind) {
    if (strncmp(s->id, "__knfx", 6) || s->id[6] != kind) return -1;
    return atoi(s->id + 7);
}

/* One straight box blur pass of radius r along rows (stride 1) or columns (stride w). */
static void svg_box(const float *in, float *out, const int w, const int h, const int r, const int vertical) {
    const int len = vertical ? h : w, lines = vertical ? w : h;
    const float k = 1.0f / (float) (2 * r + 1);
    for (int l = 0; l < lines; l++) {
        const size_t base = vertical ? (size_t) l : (size_t) l * (size_t) w;
        const size_t step = vertical ? (size_t) w : 1;
        float sum = 0.0f;
        for (int i = -r; i <= r; i++)
            if (i >= 0 && i < len) sum += in[base + (size_t) i * step];
        for (int i = 0; i < len; i++) {
            out[base + (size_t) i * step] = sum * k;
            const int add = i + r + 1, sub = i - r;
            if (add < len) sum += in[base + (size_t) add * step];
            if (sub >= 0) sum -= in[base + (size_t) sub * step];
        }
    }
}

/* feGaussianBlur as three box blurs (the spec's approximation, d = sigma * 3 sqrt(2 pi) / 4) */
static void svg_blur(float *a, float *tmp, const int w, const int h, const float sigma) {
    const int d = (int) floorf(sigma * 3.0f * sqrtf(2.0f * 3.14159265f) / 4.0f + 0.5f);
    const int r = d / 2;
    if (r < 1) return;
    for (int pass = 0; pass < 3; pass++) {
        svg_box(a, tmp, w, h, r, 0);
        svg_box(tmp, a, w, h, r, 1);
    }
}

static void svg_apply_filter(const SvgCtx *c, const SvgFilter *f, const float *ctm, unsigned char *layer) {
    const size_t n = (size_t) c->w * (size_t) c->h;
    float *alpha = malloc(n * sizeof *alpha), *sh = malloc(n * sizeof *sh), *tmp = malloc(n * sizeof *tmp);
    unsigned char *out = calloc(1, n * 4), *spx = malloc(n * 4);
    if (!alpha || !sh || !tmp || !out || !spx) goto done;
    for (size_t i = 0; i < n; i++) alpha[i] = (float) layer[i * 4 + 3] / 255.0f;
    for (int pass = 0; pass < 2; pass++) { /* drop shadows under the source, then inner ones over it */
        if (pass == 1) svg_over(out, layer, n, 1);
        for (int s = 0; s < f->count; s++) {
            const SvgShadow *d = &f->sh[s];
            if (d->inner != pass) continue;
            /* dx / dy / stdDeviation are in the element's user space (primitiveUnits userSpaceOnUse) */
            const float ox = (ctm[0] * d->dx + ctm[2] * d->dy) * c->sx, oy = (ctm[1] * d->dx + ctm[3] * d->dy) * c->sy;
            const float ks = sqrtf(fabsf(ctm[0] * ctm[3] - ctm[1] * ctm[2]));
            for (int y = 0; y < c->h; y++)
                for (int x = 0; x < c->w; x++) { /* the source alpha moved by (ox, oy), bilinear */
                    const float fx = (float) x - ox, fy = (float) y - oy;
                    const int x0 = (int) floorf(fx), y0 = (int) floorf(fy);
                    const float ax = fx - (float) x0, ay = fy - (float) y0;
                    float v = 0.0f;
                    for (int j = 0; j < 2; j++)
                        for (int i = 0; i < 2; i++) {
                            const int xx = x0 + i, yy = y0 + j;
                            if (xx < 0 || yy < 0 || xx >= c->w || yy >= c->h) continue;
                            v += alpha[(size_t) yy * (size_t) c->w + (size_t) xx] * (i ? ax : 1.0f - ax) * (j ? ay : 1.0f - ay);
                        }
                    sh[(size_t) y * (size_t) c->w + (size_t) x] = v;
                }
            if (d->sd > 0.0f) svg_blur(sh, tmp, c->w, c->h, d->sd * ks * 0.5f * (c->sx + c->sy));
            for (size_t i = 0; i < n; i++) {
                const float hard = fminf(1.0f, alpha[i] * 127.0f); /* the exporters' hardAlpha */
                float a = d->inner ? fmaxf(0.0f, hard - sh[i]) : sh[i] * (d->out ? 1.0f - hard : 1.0f);
                a = fminf(1.0f, fmaxf(0.0f, a * d->rgba[3]));
                for (int k = 0; k < 3; k++) spx[i * 4 + k] = (unsigned char) (fminf(1.0f, fmaxf(0.0f, d->rgba[k])) * a * 255.0f + 0.5f);
                spx[i * 4 + 3] = (unsigned char) (a * 255.0f + 0.5f);
            }
            svg_over(out, spx, n, 1);
        }
    }
    memcpy(layer, out, n * 4);
done:
    free(alpha);
    free(sh);
    free(tmp);
    free(out);
    free(spx);
}

/* The definition's content placed in the user space of the element using it (ctm): the root's
 * attributes, then the content in a <g transform="matrix(ctm)">. */
static NSVGimage *svg_parse_ref(const SvgCtx *c, const SvgRef *r, const float *ctm) {
    if (!r->tail) return NULL;
    const char *gt = memchr(r->tail, '>', r->tail_len);
    static const char close[] = "</svg>";
    if (!gt || r->tail_len < sizeof close - 1 + (size_t) (gt + 1 - r->tail)) return NULL;
    const size_t head = (size_t) (gt + 1 - r->tail), body = r->tail_len - head - (sizeof close - 1);
    char g[200];
    const int gl = snprintf(g, sizeof g, "<g transform=\"matrix(%.9g %.9g %.9g %.9g %.9g %.9g)\">", (double) ctm[0], (double) ctm[1],
                            (double) ctm[2], (double) ctm[3], (double) ctm[4], (double) ctm[5]);
    const size_t n = head + (size_t) gl + body + 4 + sizeof close - 1;
    char *t = malloc(n + 1);
    if (!t) return NULL;
    snprintf(t, n + 1, "%.*s%s%.*s</g>%s", (int) head, r->tail, g, (int) body, gt + 1, close);
    NSVGimage *img = svg_parse_tail(c->svg, t, n, c->attrs, c->al);
    free(t);
    return img;
}

/* layer *= the mask's luminance x alpha, or the clip path's alpha */
static void svg_apply_ref(const SvgCtx *c, const SvgRef *r, const float *ctm, unsigned char *layer) {
    const size_t n = (size_t) c->w * (size_t) c->h;
    NSVGimage *img = svg_parse_ref(c, r, ctm);
    float inv[6];
    nsvg__xformInverse(inv, (float *) ctm);
    unsigned char *m = img ? malloc(n * 4) : NULL;
    if (m) nsvgRasterize(c->rast, img, c->tx, c->ty, c->rs, m, c->w, c->h, c->w * 4);
    for (int y = 0; y < c->h; y++)
        for (int x = 0; x < c->w; x++) {
            const size_t i = (size_t) y * (size_t) c->w + (size_t) x;
            unsigned v = 0;
            if (m) {
                const unsigned char *q = m + i * 4;
                v = r->clip ? q[3] : (unsigned) ((0.2125f * (float) q[0] + 0.7154f * (float) q[1] + 0.0721f * (float) q[2]) * (float) q[3] / 255.0f + 0.5f);
            }
            if (r->has_rect) {
                const float ux = ((float) x + 0.5f - c->tx) / c->sx, uy = ((float) y + 0.5f - c->ty) / c->sy;
                const float px = inv[0] * ux + inv[2] * uy + inv[4], py = inv[1] * ux + inv[3] * uy + inv[5]; /* the element's space */
                if (px < r->rect[0] || py < r->rect[1] || px > r->rect[0] + r->rect[2] || py > r->rect[1] + r->rect[3]) v = 0;
            }
            for (int k = 0; k < 4; k++) layer[i * 4 + k] = svg_mul8(layer[i * 4 + k], v);
        }
    free(m);
    if (img) nsvgDelete(img);
}

/* A masked element nanosvg draws nothing of (a pattern or <image> fill, e.g.
 * battlepass/unlocked_front.svg): the mask's own shapes stand in for it, as
 * they did before masks were applied, rather than nothing at all. */
static void svg_ref_stand_in(const SvgCtx *c, const SvgRef *r, const float *ctm, unsigned char *layer) {
    NSVGimage *img = svg_parse_ref(c, r, ctm);
    if (!img) return;
    nsvgRasterize(c->rast, img, c->tx, c->ty, c->rs, c->tmp, c->w, c->h, c->w * 4);
    svg_over(layer, c->tmp, (size_t) c->w * (size_t) c->h, 0);
    nsvgDelete(img);
}

/* Draws the shapes from s on into dst (premultiplied) until the end sentinel of
 * fx `stop` (-1: the end); returns the shape after it. */
static NSVGshape *svg_render_seq(SvgCtx *c, NSVGshape *s, const int stop, unsigned char *dst) {
    const size_t pixels = (size_t) c->w * (size_t) c->h;
    NSVGshape *run = NULL, *last = NULL;
    for (;;) {
        int k = -1;
        const int is_end = s && (k = svg_sentinel(s, 'e')) >= 0;
        const int is_begin = s && !is_end && (k = svg_sentinel(s, 'b')) >= 0 && k < c->svg->nfx;
        if (!s || is_end || is_begin) {
            if (run) { /* the plain shapes so far, in one pass */
                NSVGshape *keep = last->next, *head = c->image->shapes;
                last->next = NULL;
                c->image->shapes = run;
                nsvgRasterize(c->rast, c->image, c->tx, c->ty, c->rs, c->tmp, c->w, c->h, c->w * 4);
                c->image->shapes = head;
                last->next = keep;
                svg_over(dst, c->tmp, pixels, 0);
                run = last = NULL;
            }
            if (!s) return NULL;
            if (is_end) {
                if (k == stop) return s->next;
                s = s->next; /* a stray end */
                continue;
            }
            unsigned char *layer = calloc(1, pixels * 4);
            if (!layer) return NULL;
            NSVGshape *next = svg_render_seq(c, s->next, k, layer);
            const SvgFx *fx = &c->svg->fx[k];
            if (fx->filter >= 0) svg_apply_filter(c, &c->svg->filters[fx->filter], fx->ctm, layer);
            if (fx->clip >= 0) svg_apply_ref(c, &c->svg->refs[fx->clip], fx->ctm, layer);
            if (fx->mask >= 0) {
                size_t i = 0;
                while (i < pixels && !layer[i * 4 + 3]) i++;
                if (i == pixels) svg_ref_stand_in(c, &c->svg->refs[fx->mask], fx->ctm, layer);
                else svg_apply_ref(c, &c->svg->refs[fx->mask], fx->ctm, layer);
            }
            svg_over(dst, layer, pixels, 1);
            free(layer);
            s = next;
            continue;
        }
        if (!run) run = s;
        last = s;
        s = s->next;
    }
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

    NSVGimage *image = svg_parse_tail(svg, svg->tail, svg->tail_len, attrs, (size_t) al);
    if (!image) return NULL;

    unsigned char *px = malloc((size_t) w * (size_t) h * 4);
    NSVGrasterizer *rast = px ? nsvgCreateRasterizer() : NULL;
    if (rast && svg->nfx) {
        /* layers: the shapes between sentinels go through their filter / clip / mask */
        const size_t n = (size_t) w * (size_t) h * 4;
        unsigned char *acc = calloc(1, n), *tmp = malloc(n);
        if (acc && tmp) {
            SvgCtx ctx = {svg, rast, image, w, h, tx, ty, scale, scale, scale, attrs, (size_t) al, tmp};
            if (svg->align_x < 0) { /* "none": nanosvg maps the viewBox itself */
                ctx.sx = vpw / vw;
                ctx.sy = vph / vh;
            }
            svg_render_seq(&ctx, image->shapes, -1, acc);
            svg_unpremultiply(acc, (size_t) w * (size_t) h);
            memcpy(px, acc, n);
        } else {
            nsvgRasterize(rast, image, tx, ty, scale, px, w, h, w * 4);
        }
        free(acc);
        free(tmp);
    } else if (rast) {
        nsvgRasterize(rast, image, tx, ty, scale, px, w, h, w * 4);
    } else {
        free(px);
        px = NULL;
    }
    if (rast) nsvgDeleteRasterizer(rast);
    nsvgDelete(image);
    return px;
}
