// KrunkScript runtime: JavaScript number <-> string conversions (doc/v7/krunkscript.md 1.12).
//
//   ks_num_format        ECMAScript Number::toString (shortest round-trip digits + ES layout)
//   ks_parse_float       parseFloat (the (num) cast, toNum, API args)
//   ks_parse_int         parseInt with no radix (array index N(), API parseInt)
//   ks_string_to_number  StringToNumber (ToNumber of a string: -, *, relational, bitwise)
//   ks_to_int32/uint32   bitwise operand conversion
//
// Inputs are UTF-8 (WTF-8 for lone surrogates); only JS whitespace and ASCII digits matter.
#include "ks_rt.h"

#include <locale.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---------------------------------------------------------------------------
// Number::toString

// strtod is locale dependent for the decimal point; the client never changes LC_NUMERIC,
// but be safe: rewrite '.' to the locale's point when it differs.
static double c_strtod(const char *s)
{
    const struct lconv *lc = localeconv();
    if (lc && lc->decimal_point && lc->decimal_point[0] != '.' && lc->decimal_point[0]) {
        char tmp[512];
        size_t n = strlen(s);
        if (n < sizeof tmp) {
            memcpy(tmp, s, n + 1);
            char *dot = strchr(tmp, '.');
            if (dot) *dot = lc->decimal_point[0];
            return strtod(tmp, NULL);
        }
    }
    return strtod(s, NULL);
}

// Digits of `x` (> 0, finite) rounded to `p` significant digits: writes the digits (no dot) to
// `dig` and returns the decimal exponent of the first digit (x ~= 0.d1d2.. * 10^(e+1)).
static int digits_p(double x, int p, char *dig)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%.*e", p - 1, x);
    int k = 0;
    const char *c = buf;
    for (; *c && *c != 'e'; c++)
        if (*c >= '0' && *c <= '9') dig[k++] = *c;
    dig[k] = 0;
    return atoi(c + 1);
}

static double digits_value(const char *dig, int e10)
{
    char buf[64];
    snprintf(buf, sizeof buf, "%c.%se%d", dig[0], dig + 1, e10);
    return c_strtod(buf);
}

// Adds `delta` (+1/-1) to the decimal digit string in place; returns the exponent change
// (+1 when 999 -> 1000, -1 when 100 -> 099 renormalised), digit count kept.
static int digits_bump(char *dig, int k, int delta)
{
    if (delta > 0) {
        int i = k - 1;
        while (i >= 0 && dig[i] == '9') dig[i--] = '0';
        if (i < 0) {
            dig[0] = '1';
            for (int j = 1; j < k; j++) dig[j] = '0';
            return 1;
        }
        dig[i]++;
        return 0;
    }
    int i = k - 1;
    while (i >= 0 && dig[i] == '0') dig[i--] = '9';
    if (i < 0) return 0; // cannot happen for a normalised non-zero string
    dig[i]--;
    if (dig[0] == '0') { // 1000 -> 0999: renormalise to 9999 one exponent down
        for (int j = 0; j < k - 1; j++) dig[j] = dig[j + 1];
        dig[k - 1] = '9';
        return -1;
    }
    return 0;
}

// Shortest digit string `dig` (k digits) and exponent n (value = 0.dig * 10^n) that round-trips.
static int shortest_digits(double x, char *dig, int *n_out)
{
    // Integers below 2^53 print exactly.
    if (x < 9007199254740992.0 && x == floor(x)) {
        char buf[32];
        snprintf(buf, sizeof buf, "%.0f", x);
        int len = (int)strlen(buf), k = len;
        while (k > 1 && buf[k - 1] == '0') k--;
        memcpy(dig, buf, (size_t)k);
        dig[k] = 0;
        *n_out = len;
        return k;
    }
    for (int p = 1; p <= 17; p++) {
        char d[32];
        int e = digits_p(x, p, d);
        if (digits_value(d, e) == x) {
            memcpy(dig, d, (size_t)p + 1);
            *n_out = e + 1;
            return p;
        }
        // The rounding interval of x is asymmetric at powers of two: a neighbour of the
        // nearest p-digit decimal may round-trip although the nearest does not.
        if (p >= 15) {
            for (int delta = -1; delta <= 1; delta += 2) {
                char nd[32];
                memcpy(nd, d, (size_t)p + 1);
                int ne = e + digits_bump(nd, p, delta);
                if (digits_value(nd, ne) == x) {
                    memcpy(dig, nd, (size_t)p + 1);
                    *n_out = ne + 1;
                    return p;
                }
            }
        }
    }
    // Unreachable: 17 digits always round-trip.
    int e = digits_p(x, 17, dig);
    *n_out = e + 1;
    return 17;
}

size_t ks_num_format(double x, char *out)
{
    if (isnan(x)) { memcpy(out, "NaN", 4); return 3; }
    if (x == 0) { memcpy(out, "0", 2); return 1; }
    size_t o = 0;
    if (x < 0) { out[o++] = '-'; x = -x; }
    if (isinf(x)) { memcpy(out + o, "Infinity", 9); return o + 8; }
    char dig[32];
    int n;
    int k = shortest_digits(x, dig, &n);
    if (k <= n && n <= 21) {
        memcpy(out + o, dig, (size_t)k); o += (size_t)k;
        for (int i = 0; i < n - k; i++) out[o++] = '0';
    } else if (0 < n && n <= 21) {
        memcpy(out + o, dig, (size_t)n); o += (size_t)n;
        out[o++] = '.';
        memcpy(out + o, dig + n, (size_t)(k - n)); o += (size_t)(k - n);
    } else if (-6 < n && n <= 0) {
        out[o++] = '0'; out[o++] = '.';
        for (int i = 0; i < -n; i++) out[o++] = '0';
        memcpy(out + o, dig, (size_t)k); o += (size_t)k;
    } else {
        int e = n - 1;
        out[o++] = dig[0];
        if (k > 1) {
            out[o++] = '.';
            memcpy(out + o, dig + 1, (size_t)(k - 1)); o += (size_t)(k - 1);
        }
        o += (size_t)sprintf(out + o, "e%c%d", e < 0 ? '-' : '+', e < 0 ? -e : e);
    }
    out[o] = 0;
    return o;
}

// ---------------------------------------------------------------------------
// Whitespace (StrWhiteSpaceChar = WhiteSpace | LineTerminator)

static uint32_t utf8_at(const char *s, size_t len, size_t i, size_t *adv)
{
    const unsigned char *u = (const unsigned char *)s;
    unsigned char c = u[i];
    if (c < 0x80 || i + 1 >= len) { *adv = 1; return c; }
    if ((c & 0xE0) == 0xC0) { *adv = 2; return ((uint32_t)(c & 0x1F) << 6) | (u[i + 1] & 0x3F); }
    if ((c & 0xF0) == 0xE0 && i + 2 < len) {
        *adv = 3;
        return ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(u[i + 1] & 0x3F) << 6) | (u[i + 2] & 0x3F);
    }
    if ((c & 0xF8) == 0xF0 && i + 3 < len) {
        *adv = 4;
        return ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(u[i + 1] & 0x3F) << 12) |
               ((uint32_t)(u[i + 2] & 0x3F) << 6) | (u[i + 3] & 0x3F);
    }
    *adv = 1;
    return c;
}

int ks_is_js_space(uint32_t cp)
{
    switch (cp) {
    case 0x09: case 0x0A: case 0x0B: case 0x0C: case 0x0D: case 0x20: case 0xA0:
    case 0x1680: case 0x2028: case 0x2029: case 0x202F: case 0x205F: case 0x3000: case 0xFEFF:
        return 1;
    default:
        return cp >= 0x2000 && cp <= 0x200A;
    }
}

size_t ks_skip_js_space(const char *s, size_t len, size_t i)
{
    while (i < len) {
        size_t adv;
        uint32_t cp = utf8_at(s, len, i, &adv);
        if (!ks_is_js_space(cp)) break;
        i += adv;
    }
    return i;
}

static size_t trim_js_space_end(const char *s, size_t start, size_t end)
{
    while (end > start) {
        // step back over one UTF-8 sequence
        size_t b = end - 1;
        while (b > start && ((unsigned char)s[b] & 0xC0) == 0x80) b--;
        size_t adv;
        uint32_t cp = utf8_at(s, end, b, &adv);
        if (!ks_is_js_space(cp)) break;
        end = b;
    }
    return end;
}

static int is_digit(char c) { return c >= '0' && c <= '9'; }

// Longest StrDecimalLiteral without sign at s[i..len): digits [. digits] [e [+-] digits] or
// . digits [exp]. Returns the end index (== i when there is none).
static size_t scan_decimal(const char *s, size_t len, size_t i)
{
    size_t j = i;
    int int_digits = 0, frac_digits = 0;
    while (j < len && is_digit(s[j])) { j++; int_digits++; }
    if (j < len && s[j] == '.') {
        size_t k = j + 1;
        while (k < len && is_digit(s[k])) { k++; frac_digits++; }
        if (int_digits || frac_digits) j = k;
    }
    if (!int_digits && !frac_digits) return i;
    if (j < len && (s[j] == 'e' || s[j] == 'E')) {
        size_t k = j + 1;
        if (k < len && (s[k] == '+' || s[k] == '-')) k++;
        if (k < len && is_digit(s[k])) {
            while (k < len && is_digit(s[k])) k++;
            j = k;
        }
    }
    return j;
}

static double strtod_range(const char *s, size_t a, size_t b, int neg)
{
    char stack[128];
    char *buf = stack;
    size_t n = b - a;
    if (n + 1 > sizeof stack) {
        buf = malloc(n + 1);
        if (!buf) return NAN;
    }
    memcpy(buf, s + a, n);
    buf[n] = 0;
    double v = c_strtod(buf);
    if (buf != stack) free(buf);
    return neg ? -v : v;
}

double ks_parse_float(const char *s, size_t len)
{
    size_t i = ks_skip_js_space(s, len, 0);
    int neg = 0;
    if (i < len && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    if (len - i >= 8 && memcmp(s + i, "Infinity", 8) == 0) return neg ? -INFINITY : INFINITY;
    size_t end = scan_decimal(s, len, i);
    if (end == i) return NAN;
    return strtod_range(s, i, end, neg);
}

static int digit_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'z') return c - 'a' + 10;
    if (c >= 'A' && c <= 'Z') return c - 'A' + 10;
    return 99;
}

double ks_parse_int(const char *s, size_t len)
{
    size_t i = ks_skip_js_space(s, len, 0);
    int neg = 0;
    if (i < len && (s[i] == '+' || s[i] == '-')) { neg = s[i] == '-'; i++; }
    int radix = 10;
    if (len - i >= 2 && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) { radix = 16; i += 2; }
    size_t j = i;
    while (j < len && digit_val(s[j]) < radix) j++;
    if (j == i) return NAN;
    double v;
    if (radix == 10) {
        v = strtod_range(s, i, j, 0);
    } else {
        v = 0;
        for (size_t k = i; k < j; k++) v = v * 16 + digit_val(s[k]);
    }
    return neg ? -v : v;
}

double ks_string_to_number(const char *s, size_t len)
{
    size_t a = ks_skip_js_space(s, len, 0);
    size_t b = trim_js_space_end(s, a, len);
    if (a == b) return 0;
    // NonDecimalIntegerLiteral: 0x / 0o / 0b (no sign)
    if (b - a > 2 && s[a] == '0') {
        int radix = 0;
        char c = s[a + 1];
        if (c == 'x' || c == 'X') radix = 16;
        else if (c == 'o' || c == 'O') radix = 8;
        else if (c == 'b' || c == 'B') radix = 2;
        if (radix) {
            double v = 0;
            for (size_t k = a + 2; k < b; k++) {
                int d = digit_val(s[k]);
                if (d >= radix) return NAN;
                v = v * radix + d;
            }
            return v;
        }
    }
    size_t i = a;
    int neg = 0;
    if (s[i] == '+' || s[i] == '-') { neg = s[i] == '-'; i++; }
    if (b - i == 8 && memcmp(s + i, "Infinity", 8) == 0) return neg ? -INFINITY : INFINITY;
    size_t end = scan_decimal(s, b, i);
    if (end == i || end != b) return NAN;
    return strtod_range(s, i, end, neg);
}

// ---------------------------------------------------------------------------
// Integer conversions and Math helpers

uint32_t ks_to_uint32(double x)
{
    if (!isfinite(x) || x == 0) return 0;
    double t = trunc(x);
    double m = fmod(t, 4294967296.0);
    if (m < 0) m += 4294967296.0;
    return (uint32_t)m;
}

int32_t ks_to_int32(double x)
{
    return (int32_t)ks_to_uint32(x);
}

double ks_js_round(double x)
{
    if (!isfinite(x) || x == 0) return x;
    double r = floor(x);
    if (x - r >= 0.5) r += 1;
    if (r == 0 && x < 0) return -0.0;
    return r;
}

// JS `%` (fmod keeps the dividend's sign; fmod(x, ±inf) = x, matching JS)
double ks_js_mod(double a, double b)
{
    return fmod(a, b);
}
