// KrunkScript shared built-ins (doc/v7/krunkscript.md 2.8, 3.1.5, 3.1.7): GAME.log,
// GAME.UTILS.*, GAME.UTILS.math.*, GAME.TIME.now / getReadable. Installed on both sides:
// the VM uses these whenever the host leaves KsHost.api[id].fn NULL.
//
// Each function reproduces the v7 wrapper in createKrunkScriptApiContext.js plus the
// libs/utils.js helper it calls, including JS coercions (`|| 0`, `x + ""`, the `+` of
// lerp/randInt that concatenates when an argument is a string).
#include "ks_rt.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <sys/timeb.h>
#endif

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif
#ifndef M_E
#define M_E 2.7182818284590452354
#endif

#define ARG(i) ((i) < argc ? args[(i)] : ks_undef())
#define NUM(i) ks_to_number(ARG(i))

static KsValue or0(double d) { return ks_num((isnan(d) || d == 0) ? 0 : d); }

// JS `a + b` on arbitrary values (ToPrimitive, concatenation when either is a string).
static KsValue js_add(KsScript *s, KsValue a, KsValue b)
{
    if (a.tag == KS_NUM && b.tag == KS_NUM) return ks_num(a.as.num + b.as.num);
    if (a.tag >= KS_ARR) a = ks_str_value(ks_to_str(s, a));
    if (b.tag >= KS_ARR) b = ks_str_value(ks_to_str(s, b));
    if (a.tag == KS_STR || b.tag == KS_STR) return ks_str_value(ks_str_concat(s, ks_to_str(s, a), ks_to_str(s, b)));
    return ks_num(ks_to_number(a) + ks_to_number(b));
}

// `x || 0` on an arbitrary value
static KsValue or0_value(KsValue v) { return ks_truthy(v) ? v : ks_num(0); }

// UTILS.isNumber: a finite JS number (strings fail)
static int is_number(KsValue v) { return v.tag == KS_NUM && isfinite(v.as.num); }

static KsValue str_value(KsScript *s, const char *p) { return ks_str(s, p, strlen(p)); }

// ---------------------------------------------------------------------------
// GAME.log

static KsValue f_log(KsScript *s, void *user, const KsValue *args, int argc)
{
    (void)user;
    size_t cap = 256, len = 0;
    char *line = malloc(cap);
    if (!line) abort();
    line[0] = 0;
    for (int i = 0; i < argc; i++) {
        char *t;
        size_t n;
        if (!ks_log_format(s, args[i], &t, &n)) {
            free(line);
            ks_raise(s, "log");
            return ks_undef();
        }
        if (len + n + 2 > cap) {
            while (len + n + 2 > cap) cap *= 2;
            char *q = realloc(line, cap);
            if (!q) abort();
            line = q;
        }
        if (i) line[len++] = ' ';
        memcpy(line + len, t, n);
        len += n;
        line[len] = 0;
        free(t);
    }
    // Hosts get valid UTF-8: a lone surrogate (3-byte WTF-8) prints as U+FFFD, like a
    // browser console / Node stdout.
    for (size_t i = 0; i + 2 < len; i++) {
        if ((unsigned char)line[i] == 0xED && ((unsigned char)line[i + 1] & 0xE0) == 0xA0) {
            line[i] = (char)0xEF;
            line[i + 1] = (char)0xBF;
            line[i + 2] = (char)0xBD;
            i += 2;
        }
    }
    if (s->host && s->host->log) s->host->log(s->user, 0, line);
    free(line);
    return ks_undef();
}

// ---------------------------------------------------------------------------
// UTILS.math

#define MATH1(name, expr)                                                       \
    static KsValue name(KsScript *s, void *u, const KsValue *args, int argc)   \
    {                                                                           \
        (void)s; (void)u;                                                       \
        double x = NUM(0);                                                      \
        return or0(expr);                                                       \
    }
MATH1(m_sin, sin(x))
MATH1(m_sinh, sinh(x))
MATH1(m_cos, cos(x))
MATH1(m_cosh, cosh(x))
MATH1(m_tan, tan(x))
MATH1(m_tanh, tanh(x))
MATH1(m_asin, asin(x))
MATH1(m_asinh, asinh(x))
MATH1(m_acos, acos(x))
MATH1(m_acosh, acosh(x))
MATH1(m_atan, atan(x))
MATH1(m_atanh, atanh(x))
MATH1(m_log, log(x))
MATH1(m_exp, exp(x))
MATH1(m_abs, fabs(x))
MATH1(m_round, ks_js_round(x))
MATH1(m_floor, floor(x))
MATH1(m_ceil, ceil(x))
MATH1(m_sqrt, sqrt(x))
MATH1(m_trunc, trunc(x))
MATH1(m_torad, x * (M_PI / 180))
MATH1(m_todeg, x * (180 / M_PI))
#undef MATH1

static double js_pow(double x, double y)
{
    if (isnan(y)) return NAN;
    if (y == 0) return 1;
    if ((x == 1 || x == -1) && isinf(y)) return NAN;
    return pow(x, y);
}

static KsValue m_e(KsScript *s, void *u, const KsValue *a, int n) { (void)s; (void)u; (void)a; (void)n; return ks_num(M_E); }
static KsValue m_pi(KsScript *s, void *u, const KsValue *a, int n) { (void)s; (void)u; (void)a; (void)n; return ks_num(M_PI); }
static KsValue m_pi2(KsScript *s, void *u, const KsValue *a, int n) { (void)s; (void)u; (void)a; (void)n; return ks_num(M_PI * 2); }

static KsValue m_atan2(KsScript *s, void *u, const KsValue *args, int argc) { (void)s; (void)u; return or0(atan2(NUM(0), NUM(1))); }
static KsValue m_pow(KsScript *s, void *u, const KsValue *args, int argc) { (void)s; (void)u; return or0(js_pow(NUM(0), NUM(1))); }

static KsValue m_min(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)s; (void)u;
    double r = INFINITY;
    for (int i = 0; i < argc; i++) {
        double x = ks_to_number(args[i]);
        if (isnan(x)) r = NAN;
        else if (!isnan(r) && (x < r || (x == 0 && r == 0 && signbit(x)))) r = x;
    }
    return or0(r);
}

static KsValue m_max(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)s; (void)u;
    double r = -INFINITY;
    for (int i = 0; i < argc; i++) {
        double x = ks_to_number(args[i]);
        if (isnan(x)) r = NAN;
        else if (!isnan(r) && (x > r || (x == 0 && r == 0 && !signbit(x)))) r = x;
    }
    return or0(r);
}

static KsValue m_hypot(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)s; (void)u;
    int inf = 0, nan = 0;
    double r = 0;
    for (int i = 0; i < argc; i++) {
        double x = ks_to_number(args[i]);
        if (isinf(x)) inf = 1;
        else if (isnan(x)) nan = 1;
        else r = hypot(r, x);
    }
    if (inf) return ks_num(INFINITY);
    if (nan) return ks_num(0);
    return or0(r);
}

static KsValue m_lerp(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    // a + (b - a) * t
    double prod = (NUM(1) - NUM(0)) * NUM(2);
    return js_add(s, ARG(0), ks_num(prod));
}

static KsValue m_calcperc(KsScript *s, void *u, const KsValue *args, int argc) { (void)s; (void)u; return or0(NUM(0) / 100 * NUM(1)); }

static KsValue m_roundtonearest(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)s; (void)u;
    if (!is_number(ARG(0))) return ks_num(0);
    double n = ARG(0).as.num, near = NUM(1);
    double r = n > 0 ? ceil(n / near) * near : n < 0 ? floor(n / near) * near : n;
    return or0(r);
}

static KsValue m_rounddecimal(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)s; (void)u;
    if (!is_number(ARG(0))) return ks_num(0);
    double n = ARG(0).as.num;
    KsValue p = ks_truthy(ARG(1)) ? ARG(1) : ks_num(0);
    double scale = js_pow(10, ks_to_number(p));
    return ks_num(ks_js_round(n * scale) / scale);
}

// ---------------------------------------------------------------------------
// UTILS geometry / random

static KsValue u_dist2d(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)s; (void)u;
    double dx = NUM(2) - NUM(0), dy = NUM(3) - NUM(1);
    return or0(sqrt(dx * dx + dy * dy));
}

static KsValue u_dist3d(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)s; (void)u;
    double dx = NUM(0) - NUM(3), dy = NUM(1) - NUM(4), dz = NUM(2) - NUM(5);
    return or0(sqrt(dx * dx + dy * dy + dz * dz));
}

static KsValue u_angldst(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)s; (void)u;
    double a = NUM(0), b = NUM(1);
    return or0(atan2(sin(b - a), cos(a - b)));
}

static KsValue u_dir2d(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)s; (void)u;
    return or0(atan2(NUM(1) - NUM(3), NUM(0) - NUM(2)));
}

static KsValue u_dir3d(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    double dx = NUM(3) - NUM(0);
    double dy = ks_to_number(js_add(s, ARG(4), ks_num(0))) - NUM(1); // p2.y + (p3 || 0) - p1.y
    double dz = NUM(5) - NUM(2);
    double flat = sqrt(dx * dx + dz * dz);
    double pitch = -atan2(dy, flat);
    double yaw = -atan2(-dx, dz);
    KsValue o = ks_obj_new(s);
    ks_obj_set(s, o, "x", ks_num(yaw + M_PI / 2));
    ks_obj_set(s, o, "y", ks_num(pitch));
    return o;
}

static KsValue u_randint(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    double span = NUM(1) - NUM(0) + 1;
    return or0_value(js_add(s, ks_num(floor(ks_rand01(s) * span)), ARG(0)));
}

static KsValue u_randfloat(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    double span = NUM(1) - NUM(0);
    return or0_value(js_add(s, ks_num(ks_rand01(s) * span), ARG(0)));
}

// ---------------------------------------------------------------------------
// Colours

// (v).toString(16) of an int32 / uint32 value
static size_t hex_of(double v, char *out)
{
    char tmp[24];
    int neg = v < 0;
    uint64_t m = (uint64_t)(neg ? -v : v);
    int n = 0;
    do {
        tmp[n++] = "0123456789abcdef"[m & 15];
        m >>= 4;
    } while (m);
    size_t o = 0;
    if (neg) out[o++] = '-';
    while (n) out[o++] = tmp[--n];
    out[o] = 0;
    return o;
}

// utils.rgbToHex: (r << 16 | g << 8 | b).toString(16) left-padded with "0" to 6
static KsValue rgb_to_hex(KsScript *s, double r, double g, double b)
{
    int32_t v = (int32_t)(((uint32_t)ks_to_int32(r) << 16) | ((uint32_t)ks_to_int32(g) << 8) | (uint32_t)ks_to_int32(b));
    char hex[24], out[32];
    size_t n = hex_of(v, hex);
    size_t o = 0;
    out[o++] = '#';
    for (size_t i = n; i < 6; i++) out[o++] = '0';
    memcpy(out + o, hex, n + 1);
    return str_value(s, out);
}

static KsValue u_hexfromhue(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    double h = NUM(0) / 60;
    double x = (1 - fabs(ks_js_mod(h, 2) - 1)) * 255;
    double f = floor(h);
    double r = (f < 1 || f > 4) ? 255 : (f == 1 || f == 4) ? x : 0;
    double g = (f == 0 || f == 3) ? x : (f == 1 || f == 2) ? 255 : 0;
    double b = (f == 0 || f == 1) ? 0 : (f == 3 || f == 4) ? 255 : x;
    return rgb_to_hex(s, r, g, b);
}

static KsValue u_rgbtohex(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    double r = is_number(ARG(0)) ? ARG(0).as.num : 0;
    double g = is_number(ARG(1)) ? ARG(1).as.num : 0;
    double b = is_number(ARG(2)) ? ARG(2).as.num : 0;
    return rgb_to_hex(s, r, g, b);
}

static KsValue u_inverthex(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    KsStr *in = ks_to_str(s, ARG(0));
    // "0x1" + text with the first "#" removed
    size_t n = in->len;
    char *buf = malloc(n + 4);
    if (!buf) abort();
    memcpy(buf, "0x1", 3);
    size_t o = 3;
    int removed = 0;
    for (size_t i = 0; i < n; i++) {
        if (!removed && in->data[i] == '#') { removed = 1; continue; }
        buf[o++] = in->data[i];
    }
    double v = ks_string_to_number(buf, o);
    free(buf);
    int32_t x = ks_to_int32(v) ^ 16777215;
    char hex[24], out[32];
    size_t hn = hex_of(x, hex);
    size_t k = 0;
    out[k++] = '#';
    for (size_t i = 1; i < hn; i++) out[k++] = (char)(hex[i] >= 'a' && hex[i] <= 'f' ? hex[i] - 32 : hex[i]);
    out[k] = 0;
    return str_value(s, out);
}

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

static KsValue u_hextorgb(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    KsStr *in = ks_to_str(s, ARG(0));
    const char *h = in->data;
    int ok = (in->len == 4 || in->len == 7) && h[0] == '#';
    for (uint32_t i = 1; ok && i < in->len; i++) ok = hexval(h[i]) >= 0;
    char six[7];
    if (!ok) memcpy(six, "ffffff", 7);
    else if (in->len == 4) {
        for (int i = 0; i < 3; i++) six[i * 2] = six[i * 2 + 1] = h[1 + i];
        six[6] = 0;
    } else {
        memcpy(six, h + 1, 6);
        six[6] = 0;
    }
    KsValue o = ks_obj_new(s);
    ks_obj_set(s, o, "r", ks_num(hexval(six[0]) * 16 + hexval(six[1])));
    ks_obj_set(s, o, "g", ks_num(hexval(six[2]) * 16 + hexval(six[3])));
    ks_obj_set(s, o, "b", ks_num(hexval(six[4]) * 16 + hexval(six[5])));
    return o;
}

// ---------------------------------------------------------------------------
// Text

// Simple case mapping for the common scripts (ASCII, Latin-1, Latin Extended-A, Greek,
// Cyrillic); `upper` maps ß to "SS". Other code points are kept.
static size_t map_case(uint32_t cp, int upper, uint32_t out[2])
{
    if (cp < 0x80) {
        if (upper && cp >= 'a' && cp <= 'z') cp -= 32;
        else if (!upper && cp >= 'A' && cp <= 'Z') cp += 32;
        out[0] = cp;
        return 1;
    }
    if (upper) {
        if (cp == 0xDF) { out[0] = 'S'; out[1] = 'S'; return 2; }
        if (cp == 0xB5) cp = 0x39C;
        else if (cp >= 0xE0 && cp <= 0xFE && cp != 0xF7) cp -= 32;
        else if (cp == 0xFF) cp = 0x178;
        else if (cp >= 0x100 && cp <= 0x17F && cp != 0x130 && cp != 0x131 && cp != 0x138 && cp != 0x149 && cp != 0x17F) {
            int odd_upper = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
            if (odd_upper ? !(cp & 1) : (cp & 1)) cp -= 1;
        } else if (cp == 0x17F) cp = 'S';
        else if (cp == 0x131) cp = 'I';
        else if (cp >= 0x3B1 && cp <= 0x3C9 && cp != 0x3C2) cp -= 32;
        else if (cp == 0x3C2) cp = 0x3A3;
        else if (cp >= 0x3AC && cp <= 0x3AF) cp = cp == 0x3AC ? 0x386 : cp - 37;
        else if (cp >= 0x430 && cp <= 0x44F) cp -= 32;
        else if (cp >= 0x450 && cp <= 0x45F) cp -= 80;
        else if (((cp >= 0x460 && cp <= 0x481) || (cp >= 0x48A && cp <= 0x4BF)) && (cp & 1)) cp -= 1;
    } else {
        if (cp >= 0xC0 && cp <= 0xDE && cp != 0xD7) cp += 32;
        else if (cp == 0x178) cp = 0xFF;
        else if (cp >= 0x100 && cp <= 0x17F && cp != 0x130 && cp != 0x131 && cp != 0x138 && cp != 0x149 && cp != 0x17F) {
            int odd_upper = (cp >= 0x139 && cp <= 0x148) || (cp >= 0x179 && cp <= 0x17E);
            if (odd_upper ? (cp & 1) : !(cp & 1)) cp += 1;
        } else if (cp == 0x130) { out[0] = 'i'; out[1] = 0x307; return 2; }
        else if (cp >= 0x391 && cp <= 0x3A9 && cp != 0x3A2) cp += 32;
        else if (cp == 0x386) cp = 0x3AC;
        else if (cp >= 0x388 && cp <= 0x38A) cp += 37;
        else if (cp >= 0x410 && cp <= 0x42F) cp += 32;
        else if (cp >= 0x400 && cp <= 0x40F) cp += 80;
        else if (((cp >= 0x460 && cp <= 0x481) || (cp >= 0x48A && cp <= 0x4BF)) && !(cp & 1)) cp += 1;
    }
    out[0] = cp;
    return 1;
}

static size_t put_utf8(uint32_t cp, char *o)
{
    if (cp < 0x80) { o[0] = (char)cp; return 1; }
    if (cp < 0x800) { o[0] = (char)(0xC0 | (cp >> 6)); o[1] = (char)(0x80 | (cp & 0x3F)); return 2; }
    if (cp < 0x10000) {
        o[0] = (char)(0xE0 | (cp >> 12));
        o[1] = (char)(0x80 | ((cp >> 6) & 0x3F));
        o[2] = (char)(0x80 | (cp & 0x3F));
        return 3;
    }
    o[0] = (char)(0xF0 | (cp >> 18));
    o[1] = (char)(0x80 | ((cp >> 12) & 0x3F));
    o[2] = (char)(0x80 | ((cp >> 6) & 0x3F));
    o[3] = (char)(0x80 | (cp & 0x3F));
    return 4;
}

static KsValue change_case(KsScript *s, KsValue v, int upper)
{
    KsStr *in = ks_to_str(s, v); // UTILS.toLower(x + "")
    char *out = malloc((size_t)in->len * 2 + 4);
    if (!out) abort();
    size_t o = 0;
    const unsigned char *p = (const unsigned char *)in->data;
    for (uint32_t i = 0; i < in->len;) {
        uint32_t cp;
        int n;
        unsigned char c = p[i];
        if (c < 0x80) { cp = c; n = 1; }
        else if (c < 0xE0) { cp = ((c & 0x1Fu) << 6) | (p[i + 1] & 0x3Fu); n = 2; }
        else if (c < 0xF0) { cp = ((c & 0x0Fu) << 12) | ((p[i + 1] & 0x3Fu) << 6) | (p[i + 2] & 0x3Fu); n = 3; }
        else { cp = ((c & 0x07u) << 18) | ((p[i + 1] & 0x3Fu) << 12) | ((p[i + 2] & 0x3Fu) << 6) | (p[i + 3] & 0x3Fu); n = 4; }
        uint32_t m[2];
        size_t k = map_case(cp, upper, m);
        for (size_t j = 0; j < k; j++) o += put_utf8(m[j], out + o);
        i += (uint32_t)n;
    }
    KsValue r = ks_str(s, out, o);
    free(out);
    return r;
}

static KsValue u_tolower(KsScript *s, void *u, const KsValue *args, int argc) { (void)u; return change_case(s, ARG(0), 0); }
static KsValue u_toupper(KsScript *s, void *u, const KsValue *args, int argc) { (void)u; return change_case(s, ARG(0), 1); }

// ToIntegerOrInfinity clamped to [0, len] (String.prototype.substring)
static uint32_t clamp_index(double d, uint32_t len)
{
    if (isnan(d) || d <= 0) return 0;
    if (d >= len) return len;
    return (uint32_t)d;
}

static KsValue u_truncatetxt(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    double start = fabs(is_number(ARG(3)) ? ARG(3).as.num : 0);
    KsStr *text = ks_to_str(s, ARG(0));
    KsValue len = ARG(1);
    int no_dots = ks_truthy(ARG(2));
    if (!is_number(len) || !(text->u16len > len.as.num)) return ks_str_value(text);
    uint32_t a = clamp_index(start, text->u16len), b = clamp_index(len.as.num, text->u16len);
    if (a > b) { uint32_t t = a; a = b; b = t; }
    KsStr *sub = ks_str_substring(s, text, a, b);
    if (!no_dots) sub = ks_str_concat(s, sub, ks_str_new_raw(s, "...", 3, 3, 1));
    return ks_str_value(sub);
}

static KsValue u_replacetext(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    KsStr *text = ks_to_str(s, ARG(0)), *find = ks_to_str(s, ARG(1)), *with = ks_to_str(s, ARG(2));
    long at = ks_str_index_of(text, find, 0);
    if (at < 0) return ks_str_value(text);
    // GetSubstitution: $$ $& $` $'
    size_t cap = text->len + with->len * 2 + text->len + 16, o = 0;
    char *out = malloc(cap);
    if (!out) abort();
#define PUT(p, n)                                          \
    do {                                                   \
        size_t n_ = (n);                                   \
        if (o + n_ + 1 > cap) {                            \
            while (o + n_ + 1 > cap) cap *= 2;             \
            char *q_ = realloc(out, cap);                  \
            if (!q_) abort();                              \
            out = q_;                                      \
        }                                                  \
        memcpy(out + o, (p), n_);                          \
        o += n_;                                           \
    } while (0)
    PUT(text->data, (size_t)at);
    for (uint32_t i = 0; i < with->len; i++) {
        char c = with->data[i];
        if (c == '$' && i + 1 < with->len) {
            char d = with->data[i + 1];
            if (d == '$') { PUT("$", 1); i++; continue; }
            if (d == '&') { PUT(find->data, find->len); i++; continue; }
            if (d == '`') { PUT(text->data, (size_t)at); i++; continue; }
            if (d == '\'') { PUT(text->data + at + find->len, text->len - (size_t)at - find->len); i++; continue; }
        }
        PUT(&c, 1);
    }
    PUT(text->data + at + find->len, text->len - (size_t)at - find->len);
#undef PUT
    KsValue r = ks_str(s, out, o);
    free(out);
    return r;
}

static KsValue u_textcontains(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    KsStr *text = ks_to_str(s, ARG(0)), *find = ks_to_str(s, ARG(1));
    return ks_bool(ks_str_index_of(text, find, 0) >= 0);
}

// ---------------------------------------------------------------------------
// TIME

// Wall clock in ms. MinGW's C runtime has no C11 timespec_get/TIME_UTC: _ftime64 there.
static double wall_ms(void)
{
#ifdef _WIN32
    struct __timeb64 tb;
    _ftime64(&tb);
    return (double)tb.time * 1000.0 + (double)tb.millitm;
#else
    struct timespec ts;
    if (timespec_get(&ts, TIME_UTC) != TIME_UTC) return (double)time(NULL) * 1000.0;
    return floor((double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6);
#endif
}

static KsValue t_now(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u; (void)args; (void)argc;
    if (s->host && s->host->now_ms) return ks_num(s->host->now_ms(s->user));
    return ks_num(wall_ms());
}

// utils.getReadableTime(ms) with no flags: "[Ny ][Nd ][Nh ]Mm "
static KsValue t_getreadable(KsScript *s, void *u, const KsValue *args, int argc)
{
    (void)u;
    double ms = NUM(0);
    double sec = floor(ms / 1000);
    double min = floor(sec / 60);
    double hr = floor(min / 60);
    min = ks_js_mod(min, 60);
    double day = floor(hr / 24);
    hr = ks_js_mod(hr, 24);
    double yr = floor(day / 365);
    day = ks_js_mod(day, 365);
    char out[160], num[32];
    size_t o = 0;
#define PART(v, suffix)                                                  \
    do {                                                                 \
        if (!(isnan(v) || (v) == 0)) {                                   \
            size_t n_ = ks_num_format((v), num);                         \
            memcpy(out + o, num, n_);                                    \
            o += n_;                                                     \
            memcpy(out + o, suffix, sizeof suffix - 1);                  \
            o += sizeof suffix - 1;                                      \
        }                                                                \
    } while (0)
    PART(yr, "y ");
    PART(day, "d ");
    PART(hr, "h ");
#undef PART
    size_t n = ks_num_format((isnan(min) || min == 0) ? 0 : min, num);
    memcpy(out + o, num, n);
    o += n;
    memcpy(out + o, "m ", 2);
    o += 2;
    return ks_str(s, out, o);
}

// ---------------------------------------------------------------------------
// Table

KsApiFn ks_stdlib_fn(int id)
{
    switch (id) {
    case KS_API_LOG: return f_log;
    case KS_API_UTILS_MATH_E: return m_e;
    case KS_API_UTILS_MATH_PI: return m_pi;
    case KS_API_UTILS_MATH_PI2: return m_pi2;
    case KS_API_UTILS_MATH_SIN: return m_sin;
    case KS_API_UTILS_MATH_SINH: return m_sinh;
    case KS_API_UTILS_MATH_COS: return m_cos;
    case KS_API_UTILS_MATH_COSH: return m_cosh;
    case KS_API_UTILS_MATH_TAN: return m_tan;
    case KS_API_UTILS_MATH_TANH: return m_tanh;
    case KS_API_UTILS_MATH_ASIN: return m_asin;
    case KS_API_UTILS_MATH_ASINH: return m_asinh;
    case KS_API_UTILS_MATH_ACOS: return m_acos;
    case KS_API_UTILS_MATH_ACOSH: return m_acosh;
    case KS_API_UTILS_MATH_ATAN: return m_atan;
    case KS_API_UTILS_MATH_ATAN2: return m_atan2;
    case KS_API_UTILS_MATH_ATANH: return m_atanh;
    case KS_API_UTILS_MATH_LOG: return m_log;
    case KS_API_UTILS_MATH_EXP: return m_exp;
    case KS_API_UTILS_MATH_POW: return m_pow;
    case KS_API_UTILS_MATH_ABS: return m_abs;
    case KS_API_UTILS_MATH_ROUND: return m_round;
    case KS_API_UTILS_MATH_FLOOR: return m_floor;
    case KS_API_UTILS_MATH_CEIL: return m_ceil;
    case KS_API_UTILS_MATH_MIN: return m_min;
    case KS_API_UTILS_MATH_MAX: return m_max;
    case KS_API_UTILS_MATH_HYPOT: return m_hypot;
    case KS_API_UTILS_MATH_SQRT: return m_sqrt;
    case KS_API_UTILS_MATH_TRUNC: return m_trunc;
    case KS_API_UTILS_MATH_LERP: return m_lerp;
    case KS_API_UTILS_MATH_TORAD: return m_torad;
    case KS_API_UTILS_MATH_TODEG: return m_todeg;
    case KS_API_UTILS_MATH_CALCPERC: return m_calcperc;
    case KS_API_UTILS_MATH_ROUNDTONEAREST: return m_roundtonearest;
    case KS_API_UTILS_MATH_ROUNDDECIMAL: return m_rounddecimal;
    case KS_API_UTILS_GETDIST2D: return u_dist2d;
    case KS_API_UTILS_GETDIST3D: return u_dist3d;
    case KS_API_UTILS_ANGLDST: return u_angldst;
    case KS_API_UTILS_GETDIR2D: return u_dir2d;
    case KS_API_UTILS_GETDIR3D: return u_dir3d;
    case KS_API_UTILS_RANDINT: return u_randint;
    case KS_API_UTILS_RANDFLOAT: return u_randfloat;
    case KS_API_UTILS_HEXFROMHUE: return u_hexfromhue;
    case KS_API_UTILS_INVERTHEX: return u_inverthex;
    case KS_API_UTILS_RGBTOHEX: return u_rgbtohex;
    case KS_API_UTILS_HEXTORGB: return u_hextorgb;
    case KS_API_UTILS_TOLOWER: return u_tolower;
    case KS_API_UTILS_TOUPPER: return u_toupper;
    case KS_API_UTILS_TRUNCATETXT: return u_truncatetxt;
    case KS_API_UTILS_REPLACETEXT: return u_replacetext;
    case KS_API_UTILS_TEXTCONTAINS: return u_textcontains;
    case KS_API_TIME_NOW: return t_now;
    case KS_API_TIME_GETREADABLE: return t_getreadable;
    default: return NULL;
    }
}
