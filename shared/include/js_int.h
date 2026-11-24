#pragma once
/* JS parseInt(text): leading spaces, an optional sign, then digits. It stops at the
   first character that is not a digit, so "1e3" is 1, not 1000.
   js_floor_tenth is the clan deposit preview: Math.floor(0.1 * Number(text)), and a
   negative amount reads as 0 (deob-10.0.7.js, windows[12].calcFee). */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

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
