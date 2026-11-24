#pragma once
/* JS parseInt(text): leading spaces, an optional sign, then digits. It stops at the
   first character that is not a digit, so "1e3" is 1, not 1000.
   js_floor_tenth is the clan deposit preview: Math.floor(0.1 * Number(text)), and a
   negative amount reads as 0 (deob-10.0.7.js, windows[12].calcFee).
   A gift amount is Number(text) (so "1e2" is 100) and its fee is Math.ceil(0.1 * amount)
   (deob-10.0.7.js, the gift dialog). js_locale_en is Number.toLocaleString('en-US'):
   groups of three, at most three fraction digits. */
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline long js_parse_int(const char *s) {
    if (!s) return 0;
    while (*s && isspace((unsigned char) *s)) s++;
    return strtol(s, 0, 10);
}

static inline long js_floor_tenth(const char *s) {
    double v = s && *s ? atof(s) : 0;
    if (v < 0) v = 0;
    return (long) floor(v * 0.1);
}

static inline double js_gift_amount(const char *s) {
    double v = s && *s ? atof(s) : 0;
    return isfinite(v) ? v : 0;
}

static inline double js_gift_fee(double amount) {
    if (!isfinite(amount)) return 0;
    return ceil(amount * 0.1);
}

/* A KR giveaway's total (deob-10.0.7.js compose): Number(text) || 0, plus
   Math.ceil(0.1 * amount). The label is that sum, so "10.4" reads 12.4.
   js_number_text is that number plus '' (ToString), not toLocaleString. */
static inline double js_giveaway_total(const char *s) {
    double amt = s && *s ? atof(s) : 0;
    if (amt != amt) return 0;
    if (!isfinite(amt)) return amt;
    return amt + ceil(amt * 0.1);
}

static inline void js_number_text(char *out, size_t n, double v) {
    if (!out || !n) return;
    if (v != v) {
        snprintf(out, n, "NaN");
        return;
    }
    if (!isfinite(v)) {
        snprintf(out, n, "%s", signbit(v) ? "-Infinity" : "Infinity");
        return;
    }
    snprintf(out, n, "%.15g", v);
}

/* Number.toLocaleString('en-US') for a gift-sized number. */
static inline void js_locale_en(char *out, size_t n, double v) {
    if (!out || !n) return;
    out[0] = 0;
    if (!isfinite(v)) {
        snprintf(out, n, "%s", signbit(v) ? "-Infinity" : "Infinity");
        return;
    }
    char raw[64], frac[8] = "";
    snprintf(raw, sizeof raw, "%.3f", fabs(v));
    char *dot = strchr(raw, '.');
    if (dot) {
        char *e = raw + strlen(raw) - 1;
        while (e > dot && *e == '0') *e-- = 0;
        if (e > dot) snprintf(frac, sizeof frac, "%s", dot + 1);
        *dot = 0;
    }
    size_t o = 0, len = strlen(raw);
    if (signbit(v) && v != 0 && o + 1 < n) out[o++] = '-';
    for (size_t i = 0; i < len && o + 1 < n; i++) {
        if (i && (len - i) % 3 == 0) {
            if (o + 2 >= n) break;
            out[o++] = ',';
        }
        out[o++] = raw[i];
    }
    if (frac[0] && o + 1 < n) {
        out[o++] = '.';
        for (size_t i = 0; frac[i] && o + 1 < n; i++) out[o++] = frac[i];
    }
    if (o < n) out[o] = 0;
    else out[n - 1] = 0;
}

/* String.prototype.trim (compose and reply, deob-10.0.7.js): both ends, including a tab,
   NBSP and the other WhiteSpace / LineTerminator characters. A line of tabs is empty. */
static inline int js_utf8_space(const unsigned char *p, size_t *n) {
    if (!p || !*p) return 0;
    if (*p == ' ' || (*p >= 0x09 && *p <= 0x0d)) return *n = 1, 1;
    if (p[0] == 0xc2 && p[1] == 0xa0) return *n = 2, 1;                                 /* NBSP */
    if (p[0] == 0xef && p[1] == 0xbb && p[2] == 0xbf) return *n = 3, 1;                 /* BOM */
    if (p[0] == 0xe1 && p[1] == 0x9a && p[2] == 0x80) return *n = 3, 1;                 /* U+1680 */
    if (p[0] == 0xe3 && p[1] == 0x80 && p[2] == 0x80) return *n = 3, 1;                 /* U+3000 */
    if (p[0] == 0xe2 && p[1] == 0x80 && ((p[2] >= 0x80 && p[2] <= 0x8a) || p[2] == 0xa8 || p[2] == 0xa9 || p[2] == 0xaf))
        return *n = 3, 1;                                                               /* U+2000-200A, 2028, 2029, 202F */
    if (p[0] == 0xe2 && p[1] == 0x81 && p[2] == 0x9f) return *n = 3, 1;                 /* U+205F */
    return 0;
}

static inline void js_trim_copy(const char *s, char *out, size_t cap) {
    if (!out || !cap) return;
    out[0] = 0;
    if (!s) return;
    const unsigned char *p = (const unsigned char *) s;
    size_t n = 1;
    while (*p && js_utf8_space(p, &n)) p += n;
    const unsigned char *e = p + strlen((const char *) p);
    while (e > p) {
        const unsigned char *t = e - 1;
        while (t > p && (*t & 0xc0) == 0x80) t--;
        if (!js_utf8_space(t, &n) || t + n != e) break;
        e = t;
    }
    size_t len = (size_t) (e - p);
    if (len >= cap) len = cap - 1;
    memcpy(out, p, len);
    out[len] = 0;
}
