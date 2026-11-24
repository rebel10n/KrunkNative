#pragma once
/* JS parseInt(text): leading spaces, an optional sign, then digits. It stops at the
   first character that is not a digit, so "1e3" is 1, not 1000. */
#include <ctype.h>
#include <stdlib.h>

static inline long js_parse_int(const char *s) {
    if (!s) return 0;
    while (*s && isspace((unsigned char) *s)) s++;
    return strtol(s, 0, 10);
}
