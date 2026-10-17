#include "hud_format.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void hud_format_number(const double value, char *out, const size_t size) {
    if (value == floor(value) && fabs(value) < 1e15) {
        snprintf(out, size, "%.0f", value + 0.0); // + 0.0: a -0 prints "0"
        return;
    }
    // Two decimals at most: the nearest hundredth (round half away from zero) through %.2f, then the
    // trailing zeros and a bare dot dropped. 1.005 is stored as 1.00499...: "1".
    snprintf(out, size, "%.2f", round(value * 100.0) / 100.0 + 0.0);
    char *dot = strchr(out, '.');
    if (!dot) return;

    char *end = out + strlen(out);
    while (end > dot && end[-1] == '0') *--end = 0;
    if (end == dot + 1) *dot = 0;
}
