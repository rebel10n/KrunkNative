// GAME.OVERLAY on Canvas2D: v7.1.4 createKrunkScriptApiContext.js OVERLAY (API:1883-2221),
// call for call. Every function mirrors the v7 body: argument defaults through UTILS.isNumber
// (finite number), isHexColor (/^#([0-9A-F]{3}){1,2}$/i), isString (non-empty string), then
// the same sequence of canvas operations, so browser quirks carry over (drawCircle's "center"
// offset of half the radius, lineDashOffset setting lineWidth, globalAlpha() = 0, ...).
#include <ks_overlay.h>
#include <shared.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static Canvas2D *g_canvas;
static double g_scale = 1.0;
static KsOverlayImageResolver g_resolver;
static KsOverlaySizeProvider g_size_provider;

static const char *const g_names[KS_OV_METHOD_COUNT] = {
    "getSize", "offset", "scale", "clear", "measureText",
    "drawCircle", "drawRect", "drawLine", "drawText", "drawImage",
    "beginPath", "closePath", "save", "restore", "translate",
    "transform", "setTransform", "moveTo", "lineTo", "fill",
    "fillStyle", "stroke", "strokeText", "strokeRect", "strokeStyle",
    "lineWidth", "lineDashOffset", "lineJoin", "ellipse", "arc",
    "arcTo", "bezierCurveTo", "quadraticCurveTo", "globalAlpha",
};

const char *ks_overlay_method_name(const KsOvMethod m) {
    return m >= 0 && m < KS_OV_METHOD_COUNT ? g_names[m] : NULL;
}

int ks_overlay_method_lookup(const char *name) {
    for (int i = 0; name && i < KS_OV_METHOD_COUNT; i++) {
        if (!strcmp(g_names[i], name)) return i;
    }
    return -1;
}

Canvas2D *ks_overlay_canvas(void) {
    if (!g_canvas) g_canvas = c2d_create();
    return g_canvas;
}

int ks_overlay_active(void) { return g_canvas && !c2d_is_empty(g_canvas); }

void ks_overlay_set_image_resolver(const KsOverlayImageResolver fn) { g_resolver = fn; }

void ks_overlay_set_size_provider(const KsOverlaySizeProvider fn) { g_size_provider = fn; }

// getSize()/clear() before the first frame (a global initialiser `obj bounds =
// GAME.OVERLAY.getSize();` runs at script load): size the canvas from the provider.
static void ensure_size(Canvas2D *c) {
    if (c2d_width(c) > 0 || !g_size_provider) return;
    int cw = 0, ch = 0, dw = 0, dh = 0;
    double scale = 1.0;
    if (!g_size_provider(&cw, &ch, &dw, &dh, &scale)) return;
    c2d_set_size(c, cw, ch, dw, dh);
    g_scale = isfinite(scale) && scale > 0 ? scale : 1.0;
}

/* ------------------------------------------------------------------ argument semantics */

static const KsOvArg *arg(const KsOvArg *args, const int argc, const int i) {
    static const KsOvArg undef = {KS_OV_UNDEF, 0, 0, NULL};
    return i < argc && args ? &args[i] : &undef;
}

static int is_num(const KsOvArg *a) { return a->type == KS_OV_NUM && isfinite(a->num); }
static int is_str(const KsOvArg *a) { return a->type == KS_OV_STR && a->str && a->str[0]; }

static int is_hex(const KsOvArg *a) {
    if (a->type != KS_OV_STR || !a->str || a->str[0] != '#') return 0;
    const size_t n = strlen(a->str + 1);
    if (n != 3 && n != 6) return 0;
    for (size_t i = 1; i <= n; i++) {
        const char ch = a->str[i];
        if (!((ch >= '0' && ch <= '9') || (ch >= 'a' && ch <= 'f') || (ch >= 'A' && ch <= 'F'))) return 0;
    }
    return 1;
}

// JS ToBoolean
static int truthy(const KsOvArg *a) {
    switch (a->type) {
        case KS_OV_BOOL: return a->boolean != 0;
        case KS_OV_NUM: return a->num != 0 && !isnan(a->num);
        case KS_OV_STR: return a->str && a->str[0];
        case KS_OV_OBJ: return 1;
        default: return 0;
    }
}

static double num_or(const KsOvArg *args, const int argc, const int i, const double def) {
    const KsOvArg *a = arg(args, argc, i);
    return is_num(a) ? a->num : def;
}

static const char *hex_or_white(const KsOvArg *a) { return is_hex(a) ? a->str : "#FFF"; }

double ks_overlay_js_parse_float(const char *s) {
    if (!s) return NAN;
    // StrWhiteSpaceChar: ASCII whitespace, NBSP, BOM, line/paragraph separators
    for (;;) {
        const unsigned char ch = (unsigned char) *s;
        if (ch == ' ' || (ch >= '\t' && ch <= '\r')) s++;
        else if (ch == 0xC2 && (unsigned char) s[1] == 0xA0) s += 2;
        else if (ch == 0xEF && (unsigned char) s[1] == 0xBB && (unsigned char) s[2] == 0xBF) s += 3;
        else if (ch == 0xE2 && (unsigned char) s[1] == 0x80 && ((unsigned char) s[2] == 0xA8 || (unsigned char) s[2] == 0xA9)) s += 3;
        else break;
    }
    const char *p = s;
    double sign = 1.0;
    if (*p == '+' || *p == '-') {
        if (*p == '-') sign = -1.0;
        p++;
    }
    if (!strncmp(p, "Infinity", 8)) return sign * INFINITY;
    const char *digits = p;
    int count = 0;
    while (*p >= '0' && *p <= '9') p++, count++;
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') p++, count++;
    }
    if (!count) return NAN;
    if (*p == 'e' || *p == 'E') {
        const char *e = p + 1;
        if (*e == '+' || *e == '-') e++;
        if (*e >= '0' && *e <= '9') {
            while (*e >= '0' && *e <= '9') e++;
            p = e;
        }
    }
    char buf[512];
    size_t len = (size_t) (p - digits);
    if (len >= sizeof(buf)) len = sizeof(buf) - 1; // absurdly long literal: precision is moot
    memcpy(buf, digits, len);
    buf[len] = 0;
    return sign * strtod(buf, NULL);
}

// parseFloat(x) || 0
static double parse_float_or_zero(const KsOvArg *a) {
    double v = NAN;
    if (a->type == KS_OV_NUM) v = a->num;
    else if (a->type == KS_OV_STR) v = ks_overlay_js_parse_float(a->str);
    return isnan(v) || v == 0 ? 0.0 : v;
}

static void set_fill(Canvas2D *c, const char *hex) {
    float rgba[4];
    if (c2d_parse_color(hex, rgba)) c2d_set_fill_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
}

static void set_stroke(Canvas2D *c, const char *hex) {
    float rgba[4];
    if (c2d_parse_color(hex, rgba)) c2d_set_stroke_color(c, rgba[0], rgba[1], rgba[2], rgba[3]);
}

// ctx.font = (size || 10) + "px " + family
static void set_font(Canvas2D *c, const double size, const char *family) {
    c2d_set_font(c, size != 0 ? size : 10.0, family);
}

static int default_image_path(const double id, char *path, const size_t size) {
    if (!(id >= 0) || id != floor(id) || id > 1e15) return 0;
    snprintf(path, size, "%suser-assets/%.0f/texture.png", assets_path(), id);
    return 1;
}

static KsOvResult undef_result(void) { return (KsOvResult) {KS_OV_RET_UNDEF, 0, 0, 0}; }

/* ------------------------------------------------------------------ the API */

KsOvResult ks_overlay_call(const KsOvMethod method, const KsOvArg *args, const int argc) {
    KsOvResult res = undef_result();
    Canvas2D *c = ks_overlay_canvas();
    if (!c) return res;
#define A(i) arg(args, argc, (i))
#define N(i, def) num_or(args, argc, (i), (def))

    switch (method) {
        case KS_OV_GET_SIZE:
            ensure_size(c);
            res.type = KS_OV_RET_SIZE;
            res.width = c2d_width(c) / g_scale;
            res.height = c2d_height(c) / g_scale;
            break;
        case KS_OV_OFFSET:
            c2d_translate(c, parse_float_or_zero(A(0)), parse_float_or_zero(A(1)));
            break;
        case KS_OV_SCALE: {
            const double s = parse_float_or_zero(A(0));
            c2d_scale(c, s, s);
            break;
        }
        case KS_OV_CLEAR:
            ensure_size(c);
            c2d_clear_rect(c, 0, 0, c2d_width(c) / g_scale, c2d_height(c) / g_scale);
            break;
        case KS_OV_MEASURE_TEXT: {
            const char *text = is_str(A(1)) ? A(1)->str : "";
            const double size = N(0, 1);
            const char *font = is_str(A(2)) ? A(2)->str : "GameFont";
            c2d_save(c);
            set_font(c, size, font);
            const double w = c2d_measure_text(c, text);
            c2d_restore(c);
            res.type = KS_OV_RET_NUM;
            res.num = isfinite(w) ? w : 0;
            break;
        }
        case KS_OV_DRAW_CIRCLE:
        case KS_OV_DRAW_RECT: {
            const double opacity = N(6, 1);
            const char *color = hex_or_white(A(5));
            const double x = N(0, 0), y = N(1, 0), w = N(2, 1), h = N(3, 1), rot = N(4, 0);
            const int center = truthy(A(7));
            c2d_save(c);
            c2d_set_global_alpha(c, opacity);
            set_fill(c, color);
            c2d_begin_path(c);
            c2d_translate(c, x, y);
            if (rot != 0) c2d_rotate(c, rot * M_PI / 180.0);
            if (method == KS_OV_DRAW_RECT) {
                c2d_rect(c, center ? -(w / 2) : 0, center ? -(h / 2) : 0, w, h);
            } else if (c2d_ellipse(c, center ? -(w / 2) : 0, center ? -(h / 2) : 0, w, h, 0, 0, M_PI * 2, 0) < 0) {
                res.type = KS_OV_RET_ERROR; // IndexSizeError: v7 throws before fill/restore
                break;
            }
            c2d_fill(c);
            c2d_restore(c);
            break;
        }
        case KS_OV_DRAW_LINE: {
            const double opacity = N(6, 1);
            const char *color = hex_or_white(A(5));
            const double x1 = N(0, 0), y1 = N(1, 0), x2 = N(2, 0), y2 = N(3, 0), thickness = N(4, 0);
            c2d_save(c);
            c2d_set_global_alpha(c, opacity);
            set_fill(c, color);
            set_stroke(c, color);
            c2d_set_line_width(c, thickness);
            c2d_begin_path(c);
            c2d_move_to(c, x1, y1);
            c2d_line_to(c, x2, y2);
            c2d_stroke(c);
            c2d_restore(c);
            break;
        }
        case KS_OV_DRAW_TEXT: {
            const char *text = is_str(A(0)) ? A(0)->str : "";
            const double opacity = N(7, 1);
            const char *color = hex_or_white(A(6));
            const double x = N(1, 0), y = N(2, 0), rot = N(3, 0), size = N(4, 1);
            const char *align = is_str(A(5)) ? A(5)->str : NULL;
            const char *font = is_str(A(8)) ? A(8)->str : "GameFont";
            c2d_save(c);
            c2d_set_global_alpha(c, opacity);
            set_fill(c, color);
            c2d_begin_path(c);
            c2d_translate(c, x, y);
            if (rot != 0) c2d_rotate(c, rot * M_PI / 180.0);
            set_font(c, size, font);
            c2d_set_text_baseline(c, C2D_BASELINE_MIDDLE);
            if (align) c2d_set_text_align_name(c, align); // invalid keywords are ignored
            c2d_fill_text(c, text, 0, 0, 0, 0);
            c2d_restore(c);
            break;
        }
        case KS_OV_DRAW_IMAGE: {
            const double id = N(0, 0), opacity = N(6, 1);
            const double x = N(1, 0), y = N(2, 0), w = N(3, 1), h = N(4, 1), rot = N(5, 0);
            c2d_save(c);
            c2d_set_global_alpha(c, opacity);
            char path[1024];
            const int ok = g_resolver ? g_resolver(id, path, sizeof(path)) : default_image_path(id, path, sizeof(path));
            C2DImage *img = ok ? c2d_image(c, path) : NULL;
            if (c2d_image_state(img) == 1) {
                c2d_translate(c, x, y);
                if (rot != 0) c2d_rotate(c, rot * M_PI / 180.0);
                c2d_draw_image(c, img, -(w / 2), -(h / 2), w, h);
            }
            c2d_restore(c);
            break;
        }
        case KS_OV_BEGIN_PATH: c2d_begin_path(c); break;
        case KS_OV_CLOSE_PATH: c2d_close_path(c); break;
        case KS_OV_SAVE: c2d_save(c); break;
        case KS_OV_RESTORE: c2d_restore(c); break;
        case KS_OV_TRANSLATE: c2d_translate(c, N(0, 0), N(1, 0)); break;
        case KS_OV_TRANSFORM: c2d_transform(c, N(0, 0), N(1, 0), N(2, 0), N(3, 0), N(4, 0), N(5, 0)); break;
        case KS_OV_SET_TRANSFORM: c2d_set_transform(c, N(0, 0), N(1, 0), N(2, 0), N(3, 0), N(4, 0), N(5, 0)); break;
        case KS_OV_MOVE_TO: c2d_move_to(c, N(0, 0), N(1, 0)); break;
        case KS_OV_LINE_TO: c2d_line_to(c, N(0, 0), N(1, 0)); break;
        case KS_OV_FILL: c2d_fill(c); break;
        case KS_OV_FILL_STYLE: set_fill(c, hex_or_white(A(0))); break;
        case KS_OV_STROKE: c2d_stroke(c); break;
        case KS_OV_STROKE_TEXT: {
            const char *text = is_str(A(0)) ? A(0)->str : "";
            const int has_max = is_num(A(3));
            c2d_stroke_text(c, text, N(1, 0), N(2, 0), has_max, has_max ? A(3)->num : 0);
            break;
        }
        case KS_OV_STROKE_RECT: c2d_stroke_rect(c, N(0, 0), N(1, 0), N(2, 0), N(3, 0)); break;
        case KS_OV_STROKE_STYLE: set_stroke(c, hex_or_white(A(0))); break;
        case KS_OV_LINE_WIDTH:
        case KS_OV_LINE_DASH_OFFSET: // v7 bug: lineDashOffset sets lineWidth
            c2d_set_line_width(c, N(0, 0));
            break;
        case KS_OV_LINE_JOIN:
            if (A(0)->type == KS_OV_STR) c2d_set_line_join_name(c, A(0)->str);
            break;
        case KS_OV_ELLIPSE:
            c2d_ellipse(c, N(0, 0), N(1, 0), fabs(N(2, 0)), fabs(N(3, 0)), N(4, 0), N(5, 0), N(6, 0), truthy(A(7)));
            break;
        case KS_OV_ARC:
            c2d_arc(c, N(0, 0), N(1, 0), fabs(N(2, 0)), N(3, 0), N(4, 0), truthy(A(5)));
            break;
        case KS_OV_ARC_TO:
            c2d_arc_to(c, N(0, 0), N(1, 0), N(2, 0), N(3, 0), fabs(N(4, 0)));
            break;
        case KS_OV_BEZIER_CURVE_TO:
            c2d_bezier_curve_to(c, N(0, 0), N(1, 0), N(2, 0), N(3, 0), N(4, 0), N(5, 0));
            break;
        case KS_OV_QUADRATIC_CURVE_TO:
            c2d_quadratic_curve_to(c, N(0, 0), N(1, 0), N(2, 0), N(3, 0));
            break;
        case KS_OV_GLOBAL_ALPHA: c2d_set_global_alpha(c, N(0, 0)); break;
        default: break;
    }
#undef A
#undef N
    return res;
}

/* ------------------------------------------------------------------ frame */

void ks_overlay_begin_frame(const int canvas_w, const int canvas_h, const int device_w, const int device_h, const double scale) {
    Canvas2D *c = ks_overlay_canvas();
    if (!c) return;
    g_scale = isfinite(scale) && scale > 0 ? scale : 1.0;
    c2d_set_size(c, canvas_w, canvas_h, device_w, device_h);
    // overlay.render: save; scale(scale); clearRect(0, 0, width / scale, height / scale); ...; restore
    c2d_save(c);
    c2d_scale(c, g_scale, g_scale);
    c2d_clear_rect(c, 0, 0, canvas_w / g_scale, canvas_h / g_scale);
    c2d_restore(c);
}

void ks_overlay_render_begin(void) {
    Canvas2D *c = ks_overlay_canvas();
    if (!c) return;
    c2d_save(c);
    c2d_scale(c, g_scale, g_scale);
}

void ks_overlay_render_end(void) {
    if (g_canvas) c2d_restore(g_canvas);
}

void ks_overlay_composite(void) {
    if (g_canvas) c2d_composite(g_canvas);
}

void ks_overlay_shutdown(void) {
    c2d_destroy(g_canvas);
    g_canvas = NULL;
}
