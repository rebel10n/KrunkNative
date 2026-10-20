// kn_webview_test: the captcha webview's pure conversions (client/src/ui/kn_webview.c, built here without
// GTK, so it runs in every build): cairo's premultiplied ARGB32 frames converted to the straight RGBA8 the
// widget's canvas takes, and the challenge's place: never a window (no backend opens one). The live run
// (WebKitGTK, a display) is kn_webview_live_test.
#include <kn_webview.h>
#include <stdio.h>

static int failures = 0;

#define CHECK(cond)                                                       \
    do {                                                                  \
        if (!(cond)) {                                                    \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);        \
            failures++;                                                   \
        }                                                                 \
    } while (0)

// one pixel of straight RGBA8 at out[i]: the 4 bytes equal
static int px_is(const unsigned char *out, const int r, const int g, const int b, const int a) {
    return out[0] == r && out[1] == g && out[2] == b && out[3] == a;
}

int main(void) {
    // the challenge is the widget in the game, on every build: no window, whatever the run asked for
    CHECK(kn_webview_captcha_inline() == 0);    // nothing shown before a challenge
    CHECK(kn_webview_captcha_windowed() == 0);  // never a window
    CHECK(kn_webview_captcha_is_open() == 0);

    // premultiplied -> straight: 0x80402010 is alpha 128 with premultiplied (64, 32, 16): straight (128, 64, 32)
    unsigned char out[4 * 8] = {0};
    const unsigned int one = 0x80402010u;
    kn_webview_rgba_from_argb32((const unsigned char *) &one, 4, 1, 1, out);
    CHECK(px_is(out, 128, 64, 32, 128));

    // opaque white and fully transparent (colour 0 whatever the premultiplied bytes say)
    const unsigned int white = 0xffffffffu, clear = 0x00ab12cdu;
    kn_webview_rgba_from_argb32((const unsigned char *) &white, 4, 1, 1, out);
    CHECK(px_is(out, 255, 255, 255, 255));
    kn_webview_rgba_from_argb32((const unsigned char *) &clear, 4, 1, 1, out);
    CHECK(px_is(out, 0, 0, 0, 0));

    // rows follow the stride (a row of 2 px in a 12-byte stride: 4 bytes of padding after them)
    unsigned int src[2 * 2 + 1] = {0};
    src[0] = 0xff0000ffu; // opaque, red channel 0 / blue 255 (ARGB: blue in the low byte)
    src[1] = 0xff00ff00u; // opaque green
    src[3] = 0xff000000u; // row 1 starts after the 12-byte stride (3 words): opaque black
    src[4] = 0x00000000u;
    unsigned char rows[2 * 2 * 4] = {0};
    kn_webview_rgba_from_argb32((const unsigned char *) src, 12, 2, 2, rows);
    CHECK(px_is(rows + 0, 0, 0, 255, 255));  // row 0, px 0: blue
    CHECK(px_is(rows + 4, 0, 255, 0, 255));  // row 0, px 1: green
    CHECK(px_is(rows + 8, 0, 0, 0, 255));    // row 1, px 0: black
    CHECK(px_is(rows + 12, 0, 0, 0, 0));     // row 1, px 1: clear

    printf(failures ? "kn_webview_test: %d failure(s)\n" : "kn_webview_test: all passed\n", failures);
    return failures ? 1 : 0;
}
