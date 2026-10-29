// Krunker v10.0.4 counter skins: the digit and icon pixel tests (client/src/skin_counter.c, SKIN_COUNTER_PURE: the bitmaps
// iîîîïíí / iîìïííi and the lit rule iíïîíií, deob-10.0.4.js:196823-196859), no GL.
#define SKIN_COUNTER_PURE
#include "../src/skin_counter.c"
#include <stdio.h>

static int failures;
#define CHECK(cond)                                                                    \
    do {                                                                               \
        if (!(cond)) {                                                                 \
            fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);            \
            failures++;                                                                \
        }                                                                              \
    } while (0)

// The 3x5 digit drawn by `count` at `position`, as the bitmap string
static void digit_at(const long count, const int position, char out[16]) {
    for (int k = 0; k < 15; k++) out[k] = skin_counter_digit_lit(count, position, k) ? '1' : '0';
    out[15] = 0;
}

int main(void) {
    char d[16];
    // 1234: units 4, tens 3, hundreds 2, thousands 1; a fifth digit is past String(1234).length: dark
    digit_at(1234, 0, d);
    CHECK(!strcmp(d, "101101111001001"));
    digit_at(1234, 1, d);
    CHECK(!strcmp(d, "111001111001111"));
    digit_at(1234, 2, d);
    CHECK(!strcmp(d, "111001111100111"));
    digit_at(1234, 3, d);
    CHECK(!strcmp(d, "010110010010111"));
    digit_at(1234, 4, d);
    CHECK(!strcmp(d, "000000000000000"));
    // 0 draws a single 0 (length 1), nothing at position 1
    digit_at(0, 0, d);
    CHECK(!strcmp(d, "111101101101111"));
    digit_at(0, 1, d);
    CHECK(!strcmp(d, "000000000000000"));
    // 9 999 999 999 (10 digits) every position a 9
    for (int p = 0; p < 10; p++) {
        digit_at(9999999999L, p, d);
        CHECK(!strcmp(d, "111101111001111"));
    }
    CHECK(!skin_counter_digit_lit(-1, 0, 0) && !skin_counter_digit_lit(5, 0, 15) && !skin_counter_digit_lit(5, -1, 0));

    // icons: candy's corners (row 0: 1000001), an unknown stat counts kills (iíìîííî)
    CHECK(skin_counter_icon_lit("candy", 0) && !skin_counter_icon_lit("candy", 1) && skin_counter_icon_lit("candy", 6));
    CHECK(skin_counter_icon_lit("kills", 1) == skin_counter_icon_lit("nonsense", 1));
    CHECK(skin_counter_icon_lit("jumps", 3) && !skin_counter_icon_lit("jumps", 0));
    int lit = 0;
    for (int i = 0; i < 49; i++) lit += skin_counter_icon_lit("headshots", i);
    CHECK(lit == 29); // the 1s in iîìïííi.headshots
    CHECK(!skin_counter_icon_lit("kills", 49) && !skin_counter_icon_lit("kills", -1));

    // counter bases (NET_EXT_KN_SKIN_COPIES / the account's copy m[stat]): per player and store skin
    skin_counter_reset_bases();
    CHECK(skin_counter_base(7, 8071) == 0); // none known: 0
    skin_counter_set_base(7, 8071, 42);
    skin_counter_set_base(8, 8071, 5);
    CHECK(skin_counter_base(7, 8071) == 42 && skin_counter_base(8, 8071) == 5 && skin_counter_base(7, 7939) == 0);
    skin_counter_set_base(7, 8071, 43); // a later SKINS replaces it
    CHECK(skin_counter_base(7, 8071) == 43);
    skin_counter_set_base(7, 8071, -1); // -1: no counter on that slot any more
    CHECK(skin_counter_base(7, 8071) == 0 && skin_counter_base(8, 8071) == 5);
    skin_counter_set_base(9, -1, 10); // no skin: ignored
    CHECK(skin_counter_base(9, -1) == 0);
    for (int i = 0; i < 100; i++) skin_counter_set_base(100 + i, 7939, i); // a full table drops the oldest
    CHECK(skin_counter_base(199, 7939) == 99);
    skin_counter_reset_bases();
    CHECK(skin_counter_base(199, 7939) == 0);

    // the local player's jumps (Player.jump_events): nothing the first time, then the difference; a smaller value (a new
    // life, another player) starts again
    CHECK(skin_counter_take_jumps(1, 4) == 0);
    CHECK(skin_counter_take_jumps(1, 6) == 2);
    CHECK(skin_counter_take_jumps(1, 6) == 0);
    CHECK(skin_counter_take_jumps(1, 0) == 0 && skin_counter_take_jumps(1, 1) == 1);
    CHECK(skin_counter_take_jumps(2, 9) == 0 && skin_counter_take_jumps(2, 10) == 1);

    printf("skin_counter_test: %s (%d failures)\n", failures ? "FAILED" : "OK", failures);
    return failures ? 1 : 0;
}
