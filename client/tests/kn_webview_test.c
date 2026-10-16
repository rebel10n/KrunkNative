// kn_webview_test: the captcha webview's pure decisions (client/src/ui/kn_webview.c, built here without
// GTK, so it runs in every build): where a reveal puts the challenge (the game UI, or its window: the
// state after a reveal), the game UI's stall watchdog, and cairo's premultiplied ARGB32 frames converted
// to the straight RGBA8 the canvas takes. The live run (WebKitGTK, a display) is kn_webview_live_test.
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
    // the reveal: the game UI, unless the run is a window run or the game UI failed to draw it
    CHECK(kn_webview_reveal_target(1, 0) == KN_WEBVIEW_VIEW_INLINE);
    CHECK(kn_webview_reveal_target(1, 1) == KN_WEBVIEW_VIEW_WINDOW); // the watchdog's fallback
    CHECK(kn_webview_reveal_target(0, 0) == KN_WEBVIEW_VIEW_WINDOW); // a window run (KRUNKNATIVE_CAPTCHA_WINDOW=1)
    CHECK(kn_webview_reveal_target(0, 1) == KN_WEBVIEW_VIEW_WINDOW);

    // the watchdog: the game UI that drew no frame within the limit hands the challenge to its window
    CHECK(!kn_webview_inline_stalled(0, 4999, 5000));
    CHECK(kn_webview_inline_stalled(0, 5000, 5000));
    CHECK(kn_webview_inline_stalled(0, 60000, 5000));
    CHECK(!kn_webview_inline_stalled(1, 60000, 5000)); // it drew: no fallback, however long it has been up
    CHECK(!kn_webview_inline_stalled(3, 0, 5000));

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
