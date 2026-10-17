// hud_format_test: hud_format_number (client/src/ui/hud_format.c), the HUD's String(n) for the damage numbers,
// the health bar's widths and the K/D ratio. Two decimals at most and never an exponent: the old %.16g
// printed the double's noise for tenths values ("9.300000000000001", the hitmarker's "double") and a tiny
// value in exponent form.
//   hud_format_test
#include "hud_format.h"

#include <stdio.h>
#include <string.h>

static int failures, checks;
#define CHECK(c, ...) do { checks++; if (!(c)) { failures++; fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__); \
    fprintf(stderr, __VA_ARGS__); fputc('\n', stderr); } } while (0)

static void expect(const double value, const char *want) {
    char got[64];
    hud_format_number(value, got, sizeof(got));
    CHECK(!strcmp(got, want), "hud_format_number(%.17g) = \"%s\", want \"%s\"", value, got, want);
}

int main(void) {
    // the exact damage (dmgType "Float") and the whole number ("Default" / a HIT with no fraction)
    expect(23.4, "23.4");
    expect(23, "23");
    expect(7.2, "7.2");
    expect(0.1, "0.1");
    expect(0.01, "0.01");
    expect(0.5, "0.5");
    // the double noise of tenths values (%.16g printed 9.300000000000001, 8.199999999999999, 64.09999999999999)
    expect(9.3, "9.3");
    expect(8.2, "8.2");
    expect(64.1, "64.1");
    // a tiny or noisy value rounds to two decimals, never an exponent
    expect(0.000000023891, "0");
    expect(2.5e-8, "0");
    expect(-0.004, "0");
    expect(1.999, "2");
    // 1.005 is stored as 1.00499...: the nearest hundredth is 1.00 (JS (1.005).toFixed(2) is "1.00")
    expect(1.005, "1");
    // an exact tie rounds half away from zero, as JS toFixed does: (2.125).toFixed(2) is "2.13"
    expect(2.125, "2.13");
    // negative values (the HUD passes fabs for damage; the helper keeps the sign otherwise)
    expect(-7.25, "-7.25");
    expect(-0.0, "0");
    expect(1.0 / 3.0, "0.33");
    expect(123456789.0, "123456789");

    if (failures) {
        fprintf(stderr, "hud_format_test: %d of %d checks failed\n", failures, checks);
        return 1;
    }
    printf("hud_format_test: %d checks passed\n", checks);
    return 0;
}
