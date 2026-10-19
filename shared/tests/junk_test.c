// junk_test: the Junk amounts (shared/src/junk.c, doc/v10/junk.md). The wallet keeps hundredths; the
// text every surface shows is two places, and a Junk value read from text is parseFloat's (decimals,
// exponents, no hex, NaN and garbage 0).
#include <junk.h>

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static int g_checks, g_failures;

#define CHECK(cond, ...)                 \
    do {                                 \
        g_checks++;                      \
        if (!(cond)) {                   \
            g_failures++;                \
            printf("FAIL  ");            \
            printf(__VA_ARGS__);         \
            printf("\n");                \
        }                                \
    } while (0)

static void expect_text(const int64_t hundredths, const char *want) {
    char text[24];
    junk_format(hundredths, text, sizeof(text));
    CHECK(!strcmp(text, want), "junk_format(%lld) = \"%s\", want \"%s\"", (long long) hundredths, text, want);
}

int main(void) {
    // the wallet's unit: llround(amount * 100); nothing for NaN, zero or negatives
    CHECK(junk_hundredths(0.05) == 5, "0.05 = 5 hundredths");
    CHECK(junk_hundredths(0.1) == 10, "0.1 = 10 hundredths (0.1 * 100 is 10.000000000000002)");
    CHECK(junk_hundredths(0.3) == 30, "0.3 = 30 hundredths");
    CHECK(junk_hundredths(2.5) == 250, "2.5 = 250 hundredths");
    CHECK(junk_hundredths(1.0) == 100, "1 = 100 hundredths");
    CHECK(junk_hundredths(0.0049) == 0, "below half a hundredth: nothing");
    CHECK(junk_hundredths(0.0) == 0 && junk_hundredths(-1.0) == 0, "zero and negatives: nothing");
    CHECK(junk_hundredths(NAN) == 0, "NaN: nothing");
    CHECK(junk_hundredths(1e300) == INT64_MAX, "huge amounts saturate");

    // the text: two places, always
    expect_text(1234, "12.34");
    expect_text(5, "0.05");
    expect_text(0, "0.00");
    expect_text(100, "1.00");
    expect_text(1000000000, "10000000.00");
    expect_text(-5, "0.00");

    // parsing: parseFloat's reading, rounded to hundredths
    CHECK(junk_parse("12.34") == 1234, "parse \"12.34\"");
    CHECK(junk_parse("12") == 1200, "parse \"12\"");
    CHECK(junk_parse("12.3") == 1230, "parse \"12.3\"");
    CHECK(junk_parse("0.05") == 5, "parse \"0.05\"");
    CHECK(junk_parse("  7 ") == 700, "parse with blanks \"  7 \"");
    CHECK(junk_parse("1e2") == 10000, "parse \"1e2\" (parseFloat reads exponents)");
    CHECK(junk_parse("-3") == 0, "parse \"-3\": 0");
    CHECK(junk_parse("0x10") == 0, "parse \"0x10\": parseFloat gives 0");
    CHECK(junk_parse("abc") == 0 && junk_parse("") == 0 && junk_parse(NULL) == 0, "garbage, empty and NULL: 0");
    CHECK(junk_parse("inf") == 0 && junk_parse("nan") == 0, "inf and nan: 0 (no credit)");

    // round trip: every value the text shows reads back to the same hundredths
    for (int64_t h = 0; h <= 100000; h += 7) {
        char text[24];
        junk_format(h, text, sizeof(text));
        if (junk_parse(text) != h) {
            CHECK(0, "round trip of %lld (\"%s\") gives %lld", (long long) h, text, (long long) junk_parse(text));
            break;
        }
    }
    CHECK(fabs(junk_value(1234) - 12.34) < 1e-9 && junk_value(-5) == 0.0, "junk_value: hundredths as a double");

    if (g_failures) {
        printf("%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
