// Junk amounts (doc/v10/junk.md): the rounding and the two-place text every Junk value is shown and
// stored with. Shared by kn_brain (the wallet and the account JSON), the game server (the pickup and
// package amounts) and the client (the navbar, the Account window, the "You found" line).
#include <junk.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int64_t junk_hundredths(const double amount) {
    if (!(amount > 0.0) || !isfinite(amount)) return 0; // NaN, infinities, zero and negatives
    if (amount >= (double) INT64_MAX / 100.0) return INT64_MAX;
    return (int64_t) llround(amount * 100.0);
}

void junk_format(const int64_t hundredths, char *out, const size_t size) {
    if (!out || size == 0) return;
    const int64_t v = hundredths > 0 ? hundredths : 0;
    snprintf(out, size, "%lld.%02lld", (long long) (v / 100), (long long) (v % 100));
}

int64_t junk_parse(const char *text) {
    if (!text) return 0;
    while (*text == ' ' || *text == '\t' || *text == '\n' || *text == '\r') text++;
    if (!*text) return 0;
    // parseFloat reads decimals only: "0x10" is 0, "inf" / "nan" NaN (strtod takes all three)
    const char *digits = text + (*text == '+' || *text == '-');
    if (digits[0] == '0' && (digits[1] == 'x' || digits[1] == 'X')) return 0;
    char *end = NULL;
    const double v = strtod(text, &end);
    if (end == text) return 0;
    return junk_hundredths(v);
}

double junk_value(const int64_t hundredths) { return (double) (hundredths > 0 ? hundredths : 0) / 100.0; }
