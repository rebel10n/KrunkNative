// CPU 2D rasterizer for the page's <canvas> elements; see client/include/cpu_canvas.h.
// Paths are rasterized with signed-area coverage accumulation (each edge adds its exact
// trapezoid area per pixel; a running sum along the row gives the covered fraction), so
// shapes are anti-aliased like Chromium's canvas. Edges are clamped to x in [0, w]: what lies
// left of the bitmap folds into column 0 and keeps the winding of the pixels inside right.
#include <cpu_canvas.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CC_MITER_LIMIT 10.0f // context.miterLimit default
#define CC_MAX_POINTS 512

int cc_resize(CpuCanvas *c, const int w, const int h) {
    if (w <= 0 || h <= 0) return 0;
    if (c->px && c->w == w && c->h == h) {
        cc_reset_transform(c);
        return 1;
    }
    uint8_t *px = malloc((size_t) w * (size_t) h * 4);
    float *acc = calloc((size_t) (w + 2) * (size_t) h, sizeof(float));
    if (!px || !acc) {
        free(px);
        free(acc);
        return 0;
    }
    free(c->px);
    free(c->acc);
    c->px = px;
    c->acc = acc;
    c->w = w;
    c->h = h;
    memset(c->px, 0, (size_t) w * (size_t) h * 4);
    cc_reset_transform(c);
    return 1;
}

void cc_free(CpuCanvas *c) {
    free(c->px);
    free(c->acc);
    memset(c, 0, sizeof(*c));
}

void cc_reset_transform(CpuCanvas *c) {
    const float id[6] = {1, 0, 0, 1, 0, 0};
    memcpy(c->m, id, sizeof(id));
    c->depth = 0;
    c->alpha = 1.0f;
}

void cc_save(CpuCanvas *c) {
    if (c->depth < CC_STACK_MAX) memcpy(c->stack[c->depth++], c->m, sizeof(c->m));
}

void cc_restore(CpuCanvas *c) {
    if (c->depth > 0) memcpy(c->m, c->stack[--c->depth], sizeof(c->m));
}

void cc_translate(CpuCanvas *c, const float x, const float y) {
    c->m[4] += c->m[0] * x + c->m[2] * y;
    c->m[5] += c->m[1] * x + c->m[3] * y;
}

void cc_rotate(CpuCanvas *c, const float a) {
    const float cs = cosf(a), sn = sinf(a);
    const float m0 = c->m[0], m1 = c->m[1], m2 = c->m[2], m3 = c->m[3];
    c->m[0] = m0 * cs + m2 * sn;
    c->m[1] = m1 * cs + m3 * sn;
    c->m[2] = -m0 * sn + m2 * cs;
    c->m[3] = -m1 * sn + m3 * cs;
}

static void cc_apply(const CpuCanvas *c, const float x, const float y, float *ox, float *oy) {
    *ox = c->m[0] * x + c->m[2] * y + c->m[4];
    *oy = c->m[1] * x + c->m[3] * y + c->m[5];
}

CcColor cc_rgba(const float r, const float g, const float b, const float a) {
    return (CcColor) {r, g, b, a};
}

static float hue_channel(const float p, const float q, float t) {
    if (t < 0.0f) t += 1.0f;
    if (t > 1.0f) t -= 1.0f;
    if (t < 1.0f / 6.0f) return p + (q - p) * 6.0f * t;
    if (t < 0.5f) return q;
    if (t < 2.0f / 3.0f) return p + (q - p) * (2.0f / 3.0f - t) * 6.0f;
    return p;
}

CcColor cc_hsl(const float h_deg, const float s_pct, const float l_pct) {
    const float h = fmodf(fmodf(h_deg, 360.0f) + 360.0f, 360.0f) / 360.0f;
    const float s = fminf(fmaxf(s_pct / 100.0f, 0.0f), 1.0f), l = fminf(fmaxf(l_pct / 100.0f, 0.0f), 1.0f);
    if (s == 0.0f) return (CcColor) {l * 255.0f, l * 255.0f, l * 255.0f, 1.0f};
    const float q = l < 0.5f ? l * (1.0f + s) : l + s - l * s, p = 2.0f * l - q;
    // Chromium rounds the channels of hsl() to bytes
    return (CcColor) {roundf(hue_channel(p, q, h + 1.0f / 3.0f) * 255.0f), roundf(hue_channel(p, q, h) * 255.0f),
                       roundf(hue_channel(p, q, h - 1.0f / 3.0f) * 255.0f), 1.0f};
}

CcColor cc_hex(const char *hex) {
    unsigned v = 0;
    if (!hex || hex[0] != '#') return (CcColor) {0, 0, 0, 1};
    const size_t n = strlen(hex + 1);
    if (sscanf(hex + 1, "%x", &v) != 1) return (CcColor) {0, 0, 0, 1};
    if (n == 3) return (CcColor) {(float) ((v >> 8 & 0xF) * 17), (float) ((v >> 4 & 0xF) * 17), (float) ((v & 0xF) * 17), 1};
    return (CcColor) {(float) (v >> 16 & 0xFF), (float) (v >> 8 & 0xFF), (float) (v & 0xFF), 1};
}

/* ------------------------------------------------------------ coverage */

typedef struct {
    int y0, y1; // touched rows [y0, y1)
    int x0, x1; // touched accumulation columns [x0, x1]
} Span;

#define SPAN_EMPTY(c) {(c)->h, 0, (c)->w + 2, -1}

static void acc_line(CpuCanvas *c, Span *span, float x0, float y0, float x1, float y1) {
    if (y0 == y1) return;
    float dir = 1.0f;
    if (y0 > y1) {
        float t = x0;
        x0 = x1;
        x1 = t;
        t = y0;
        y0 = y1;
        y1 = t;
        dir = -1.0f;
    }
    const float dxdy = (x1 - x0) / (y1 - y0);
    float x = x0;
    if (y0 < 0.0f) {
        x -= y0 * dxdy;
        y0 = 0.0f;
    }
    if (y1 > (float) c->h) y1 = (float) c->h;
    if (y0 >= y1) return;
    const int stride = c->w + 2;
    const float maxx = (float) c->w;
    const int ys = (int) floorf(y0), ye = (int) ceilf(y1);
    if (ys < span->y0) span->y0 = ys;
    if (ye > span->y1) span->y1 = ye;
    for (int y = ys; y < ye; y++) {
        float *row = c->acc + (size_t) y * (size_t) stride;
        const float dy = fminf((float) y + 1.0f, y1) - fmaxf((float) y, y0);
        const float xnext = x + dxdy * dy;
        const float d = dy * dir;
        float xa = fminf(fmaxf(fminf(x, xnext), 0.0f), maxx), xb = fminf(fmaxf(fmaxf(x, xnext), 0.0f), maxx);
        const float xa_floor = floorf(xa);
        const int xai = (int) xa_floor;
        const float xb_ceil = ceilf(xb);
        const int xbi = (int) xb_ceil;
        if (xai < span->x0) span->x0 = xai;
        if (xbi + 1 > span->x1) span->x1 = xbi + 1;
        if (xbi <= xai + 1) {
            const float xmf = 0.5f * (xa + xb) - xa_floor;
            row[xai] += d - d * xmf;
            row[xai + 1] += d * xmf;
        } else {
            const float s = 1.0f / (xb - xa);
            const float xaf = xa - xa_floor;
            const float a0 = 0.5f * s * (1.0f - xaf) * (1.0f - xaf);
            const float xbf = xb - xb_ceil + 1.0f;
            const float am = 0.5f * s * xbf * xbf;
            row[xai] += d * a0;
            if (xbi == xai + 2) {
                row[xai + 1] += d * (1.0f - a0 - am);
            } else {
                const float a1 = s * (1.5f - xaf);
                row[xai + 1] += d * (a1 - a0);
                for (int xi = xai + 2; xi < xbi - 1; xi++) row[xi] += d * s;
                const float a2 = a1 + (float) (xbi - xai - 3) * s;
                row[xbi - 1] += d * (1.0f - a2 - am);
            }
            row[xbi] += d * am;
        }
        x = xnext;
    }
}

// Adds one closed contour (device px) to the accumulation.
static void acc_contour(CpuCanvas *c, Span *span, const float *xy, const int n) {
    for (int i = 0; i < n; i++) {
        const int j = (i + 1) % n;
        acc_line(c, span, xy[i * 2], xy[i * 2 + 1], xy[j * 2], xy[j * 2 + 1]);
    }
}

typedef struct {
    const uint8_t *rgba; // straight RGBA8 image
    int iw, ih;
    float inv[6];        // device px -> image px
} ImageSampler;

// Bilinear sample (imageSmoothingEnabled) of the image at device pixel centre (x, y), edges clamped.
static CcColor sample_image(const ImageSampler *s, const int x, const int y) {
    const float dx = (float) x + 0.5f, dy = (float) y + 0.5f;
    float u = s->inv[0] * dx + s->inv[2] * dy + s->inv[4] - 0.5f;
    float v = s->inv[1] * dx + s->inv[3] * dy + s->inv[5] - 0.5f;
    u = fminf(fmaxf(u, 0.0f), (float) (s->iw - 1));
    v = fminf(fmaxf(v, 0.0f), (float) (s->ih - 1));
    const int x0 = (int) u, y0 = (int) v;
    const int x1 = x0 + 1 < s->iw ? x0 + 1 : x0, y1 = y0 + 1 < s->ih ? y0 + 1 : y0;
    const float fx = u - (float) x0, fy = v - (float) y0;
    float acc[4] = {0, 0, 0, 0};
    const int xs[4] = {x0, x1, x0, x1}, ys[4] = {y0, y0, y1, y1};
    const float ws[4] = {(1 - fx) * (1 - fy), fx * (1 - fy), (1 - fx) * fy, fx * fy};
    for (int k = 0; k < 4; k++) { // premultiplied filtering
        const uint8_t *p = s->rgba + ((size_t) ys[k] * (size_t) s->iw + (size_t) xs[k]) * 4;
        const float a = p[3] / 255.0f * ws[k];
        acc[0] += p[0] * a;
        acc[1] += p[1] * a;
        acc[2] += p[2] * a;
        acc[3] += a;
    }
    if (acc[3] <= 0.0f) return (CcColor) {0, 0, 0, 0};
    return (CcColor) {acc[0] / acc[3], acc[1] / acc[3], acc[2] / acc[3], acc[3]};
}

// Composites the accumulated coverage with `col` (or the image of `img`) and clears the accumulation.
static void acc_flush_any(CpuCanvas *c, const Span *span, CcColor col, const ImageSampler *img) {
    const int stride = c->w + 2;
    const float a = fminf(fmaxf(col.a * c->alpha, 0.0f), 1.0f);
    for (int y = span->y0 < 0 ? 0 : span->y0; y < span->y1 && y < c->h; y++) {
        float *row = c->acc + (size_t) y * (size_t) stride;
        uint8_t *px = c->px + (size_t) y * (size_t) c->w * 4;
        float sum = 0.0f;
        // columns left of x0 hold nothing; right of x1 the running sum is back to its row total
        const int xe = span->x1 + 1 < c->w ? span->x1 + 1 : c->w;
        for (int x = span->x0 < 0 ? 0 : span->x0; x < xe; x++) {
            sum += row[x];
            row[x] = 0.0f;
            float cov = fminf(fabsf(sum), 1.0f) * a;
            if (cov <= 0.0f) continue;
            if (img) {
                col = sample_image(img, x, y);
                cov *= col.a;
                if (cov <= 0.0f) continue;
            }
            px[x * 4 + 0] = (uint8_t) lroundf(px[x * 4 + 0] + (col.r - px[x * 4 + 0]) * cov);
            px[x * 4 + 1] = (uint8_t) lroundf(px[x * 4 + 1] + (col.g - px[x * 4 + 1]) * cov);
            px[x * 4 + 2] = (uint8_t) lroundf(px[x * 4 + 2] + (col.b - px[x * 4 + 2]) * cov);
            px[x * 4 + 3] = 255;
        }
        row[c->w] = row[c->w + 1] = 0.0f;
    }
}

static void acc_flush(CpuCanvas *c, const Span *span, const CcColor col) {
    acc_flush_any(c, span, col, NULL);
}

/* ------------------------------------------------------------ shapes */

static int transform_points(const CpuCanvas *c, const float *xy, const int n, float *out) {
    const int count = n > CC_MAX_POINTS ? CC_MAX_POINTS : n;
    for (int i = 0; i < count; i++) cc_apply(c, xy[i * 2], xy[i * 2 + 1], &out[i * 2], &out[i * 2 + 1]);
    return count;
}

void cc_fill_all(CpuCanvas *c, const CcColor col) {
    for (size_t i = 0, n = (size_t) c->w * (size_t) c->h; i < n; i++) {
        c->px[i * 4 + 0] = (uint8_t) col.r;
        c->px[i * 4 + 1] = (uint8_t) col.g;
        c->px[i * 4 + 2] = (uint8_t) col.b;
        c->px[i * 4 + 3] = 255;
    }
}

void cc_fill_poly(CpuCanvas *c, const float *xy, const int n, const CcColor col) {
    if (!c->px || n < 3) return;
    float pts[CC_MAX_POINTS * 2];
    const int count = transform_points(c, xy, n, pts);
    Span span = SPAN_EMPTY(c);
    acc_contour(c, &span, pts, count);
    acc_flush(c, &span, col);
}

void cc_fill_rect(CpuCanvas *c, const float x, const float y, const float w, const float h, const CcColor col) {
    const float xy[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    cc_fill_poly(c, xy, 4, col);
}

// The two offset contours of a closed path stroked `width` wide with miter joins (device px):
// the outline one way round and the inner one the other way, so nonzero leaves the ring.
static void stroke_contours(CpuCanvas *c, Span *span, const float *pts, const int n, const float width) {
    if (n < 2 || width <= 0.0f) return;
    const float hw = width / 2.0f;
    float outer[CC_MAX_POINTS * 2], inner[CC_MAX_POINTS * 2];
    for (int i = 0; i < n; i++) {
        const int p = (i + n - 1) % n, q = (i + 1) % n;
        float ex0 = pts[i * 2] - pts[p * 2], ey0 = pts[i * 2 + 1] - pts[p * 2 + 1];
        float ex1 = pts[q * 2] - pts[i * 2], ey1 = pts[q * 2 + 1] - pts[i * 2 + 1];
        const float l0 = sqrtf(ex0 * ex0 + ey0 * ey0), l1 = sqrtf(ex1 * ex1 + ey1 * ey1);
        if (l0 > 0.0f) { ex0 /= l0; ey0 /= l0; }
        if (l1 > 0.0f) { ex1 /= l1; ey1 /= l1; }
        // left normals of both edges, the miter along their sum
        const float n0x = -ey0, n0y = ex0, n1x = -ey1, n1y = ex1;
        float mx = n0x + n1x, my = n0y + n1y;
        const float ml = sqrtf(mx * mx + my * my);
        float len = hw;
        if (ml > 1e-6f) {
            mx /= ml;
            my /= ml;
            const float cosv = mx * n1x + my * n1y;
            len = cosv > 1e-3f ? hw / cosv : hw * CC_MITER_LIMIT;
            if (len > hw * CC_MITER_LIMIT) len = hw * CC_MITER_LIMIT;
        } else {
            mx = n1x;
            my = n1y;
        }
        outer[i * 2] = pts[i * 2] + mx * len;
        outer[i * 2 + 1] = pts[i * 2 + 1] + my * len;
        inner[(n - 1 - i) * 2] = pts[i * 2] - mx * len;
        inner[(n - 1 - i) * 2 + 1] = pts[i * 2 + 1] - my * len;
    }
    acc_contour(c, span, outer, n);
    acc_contour(c, span, inner, n);
}

void cc_stroke_poly(CpuCanvas *c, const float *xy, const int n, const float line_width, const CcColor col) {
    if (!c->px || n < 2) return;
    float pts[CC_MAX_POINTS * 2];
    const int count = transform_points(c, xy, n, pts);
    Span span = SPAN_EMPTY(c);
    stroke_contours(c, &span, pts, count, line_width);
    acc_flush(c, &span, col);
}

void cc_stroke_rect(CpuCanvas *c, const float x, const float y, const float w, const float h, const float line_width, const CcColor col) {
    const float xy[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    cc_stroke_poly(c, xy, 4, line_width, col);
}

static int circle_points(const float cx, const float cy, const float r, float *xy) {
    int n = (int) ceilf(2.0f * (float) M_PI * fabsf(r) / 1.5f);
    if (n < 16) n = 16;
    if (n > CC_MAX_POINTS) n = CC_MAX_POINTS;
    for (int i = 0; i < n; i++) {
        const float a = (float) i / (float) n * 2.0f * (float) M_PI;
        xy[i * 2] = cx + r * cosf(a);
        xy[i * 2 + 1] = cy + r * sinf(a);
    }
    return n;
}

void cc_fill_circle(CpuCanvas *c, const float cx, const float cy, const float r, const CcColor col) {
    float xy[CC_MAX_POINTS * 2];
    const int n = circle_points(cx, cy, r, xy);
    cc_fill_poly(c, xy, n, col);
}

void cc_stroke_circle(CpuCanvas *c, const float cx, const float cy, const float r, const float line_width, const CcColor col) {
    if (!c->px || r <= 0.0f) return;
    float xy[CC_MAX_POINTS * 2], pts[CC_MAX_POINTS * 2];
    const float hw = line_width / 2.0f;
    // exact ring: outer circle one way, inner circle (if any) the other way
    int n = circle_points(cx, cy, r + hw, xy);
    n = transform_points(c, xy, n, pts);
    Span span = SPAN_EMPTY(c);
    acc_contour(c, &span, pts, n);
    if (r - hw > 0.0f) {
        int m = circle_points(cx, cy, r - hw, xy);
        m = transform_points(c, xy, m, pts);
        for (int i = 0; i < m / 2; i++) { // reverse the winding
            const float tx = pts[i * 2], ty = pts[i * 2 + 1];
            pts[i * 2] = pts[(m - 1 - i) * 2];
            pts[i * 2 + 1] = pts[(m - 1 - i) * 2 + 1];
            pts[(m - 1 - i) * 2] = tx;
            pts[(m - 1 - i) * 2 + 1] = ty;
        }
        acc_contour(c, &span, pts, m);
    }
    acc_flush(c, &span, col);
}

void cc_draw_image(CpuCanvas *c, const uint8_t *rgba, const int iw, const int ih, const float x, const float y, const float w,
                   const float h) {
    if (!c->px || !rgba || iw <= 0 || ih <= 0 || w == 0.0f || h == 0.0f) return;
    // device -> image: invert (current transform * the rect's scale + offset)
    const float sx = w / (float) iw, sy = h / (float) ih;
    const float a = c->m[0] * sx, b = c->m[1] * sx, cc = c->m[2] * sy, d = c->m[3] * sy;
    const float e = c->m[0] * x + c->m[2] * y + c->m[4], f = c->m[1] * x + c->m[3] * y + c->m[5];
    const float det = a * d - b * cc;
    if (fabsf(det) < 1e-12f) return;
    ImageSampler img = {.rgba = rgba, .iw = iw, .ih = ih};
    img.inv[0] = d / det;
    img.inv[1] = -b / det;
    img.inv[2] = -cc / det;
    img.inv[3] = a / det;
    img.inv[4] = (cc * f - d * e) / det;
    img.inv[5] = (b * e - a * f) / det;
    const float xy[8] = {x, y, x + w, y, x + w, y + h, x, y + h};
    float pts[8];
    transform_points(c, xy, 4, pts);
    Span span = SPAN_EMPTY(c);
    acc_contour(c, &span, pts, 4);
    acc_flush_any(c, &span, (CcColor) {0, 0, 0, 1}, &img);
}
