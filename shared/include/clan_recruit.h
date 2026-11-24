#pragma once
/* The recruitment form's Min Level and Min K/D (deob-10.0.7.js:96870-96873).
   Number(text) allows spaces around the digits. parseFloat(text) || 0 is the value.
   A level is a whole number 0..200. A K/D is 0..99.99. An empty box is 0. */
#include <ctype.h>
#include <math.h>
#include <stdlib.h>

static inline const char *clan_recruit_skip(const char *s) {
    while (*s && isspace((unsigned char) *s)) s++;
    return s;
}

/* 1 and *out set when Number(text) is a finite number (spaces around it allowed). */
static inline int clan_recruit_number(const char *v, double *out) {
    if (!v || !v[0]) {
        *out = 0;
        return 1;
    }
    const char *s = clan_recruit_skip(v);
    if (!*s) {
        *out = 0; /* " " is Number 0 */
        return 1;
    }
    char *end = 0;
    const double d = strtod(s, &end);
    if (!end) return 0;
    if (*clan_recruit_skip(end)) return 0;
    *out = d;
    return 1;
}

static inline int clan_recruit_level_ok(const char *v) {
    double d = 0;
    return clan_recruit_number(v, &d) && d >= 0 && d <= 200 && floor(d) == d;
}

static inline int clan_recruit_kd_ok(const char *v) {
    double d = 0;
    return clan_recruit_number(v, &d) && d >= 0 && d <= 99.99;
}
