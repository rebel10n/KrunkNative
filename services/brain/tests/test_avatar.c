/* Unit test for profile pictures (services/brain/avatar_verdict.c, avatar_image.c): the verdict
 * table (the fail-closed rules of abulxt's avatar-moderation.ts) and the intake of an upload
 * (type, size, dimensions, decode, square crop, WebP output). Pure: no database, no network.
 *
 *   kn_brain_avatar_test [scratch dir]   (the scratch dir receives the generated PNG fixtures)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "avatar_image.h"
#include "avatar_verdict.h"
#include "cJSON.h"
#include "png_write.h"
#ifdef KN_HAVE_WEBP
#include <webp/decode.h>
#include <webp/encode.h>
#endif

static int g_fail;
#define CHECK(c) do { if (!(c)) { fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #c); g_fail = 1; } } while (0)

static const char NONCE[] = "n0nce-test-value";

/* ---- verdict table */

static int parse(const char *raw, AvatarVerdict *v) { return avatar_verdict_parse(raw, NONCE, v); }

static void verdicts(void) {
    AvatarVerdict v;

    CHECK(parse("{\"nonce\":\"n0nce-test-value\",\"allowed\":true,\"categories\":[],\"reason\":\"Looks fine.\"}", &v) == AVATAR_VERDICT_APPROVED);
    CHECK(!strcmp(v.reason, "Looks fine."));
    CHECK(v.categories[0] == 0);

    /* a fenced answer is read the same way */
    CHECK(parse("```json\n{\"nonce\":\"n0nce-test-value\",\"allowed\":true,\"categories\":[],\"reason\":\"ok\"}\n```", &v) == AVATAR_VERDICT_APPROVED);

    /* approval needs the request's nonce */
    CHECK(parse("{\"nonce\":\"other\",\"allowed\":true,\"categories\":[],\"reason\":\"ok\"}", &v) == AVATAR_VERDICT_ERROR);
    CHECK(parse("{\"allowed\":true,\"categories\":[],\"reason\":\"ok\"}", &v) == AVATAR_VERDICT_ERROR);

    /* an approval that lists categories is not an approval */
    CHECK(parse("{\"nonce\":\"n0nce-test-value\",\"allowed\":true,\"categories\":[\"gore\"],\"reason\":\"ok\"}", &v) == AVATAR_VERDICT_ERROR);
    /* allowed must be the boolean true */
    CHECK(parse("{\"nonce\":\"n0nce-test-value\",\"allowed\":\"true\",\"categories\":[],\"reason\":\"ok\"}", &v) == AVATAR_VERDICT_ERROR);
    /* categories and reason must be there */
    CHECK(parse("{\"nonce\":\"n0nce-test-value\",\"allowed\":true,\"reason\":\"ok\"}", &v) == AVATAR_VERDICT_ERROR);
    CHECK(parse("{\"nonce\":\"n0nce-test-value\",\"allowed\":true,\"categories\":[]}", &v) == AVATAR_VERDICT_ERROR);
    /* no keys beyond the four asked for */
    CHECK(parse("{\"nonce\":\"n0nce-test-value\",\"allowed\":true,\"categories\":[],\"reason\":\"ok\",\"extra\":1}", &v) == AVATAR_VERDICT_ERROR);

    /* not an object, prose, broken JSON, empty, missing */
    CHECK(parse("[{\"nonce\":\"n0nce-test-value\",\"allowed\":true,\"categories\":[],\"reason\":\"ok\"}]", &v) == AVATAR_VERDICT_ERROR);
    CHECK(parse("Sure! {\"nonce\":\"n0nce-test-value\",\"allowed\":true,\"categories\":[],\"reason\":\"ok\"}", &v) == AVATAR_VERDICT_ERROR);
    CHECK(parse("{\"nonce\":\"n0nce-test-value\",\"allowed\":true,", &v) == AVATAR_VERDICT_ERROR);
    CHECK(parse("", &v) == AVATAR_VERDICT_ERROR);
    CHECK(parse(NULL, &v) == AVATAR_VERDICT_ERROR);
    CHECK(!strcmp(v.reason, "Automatic review was inconclusive. Try uploading the picture again."));

    /* a clear "not allowed" is honoured even when the rest is sloppy */
    CHECK(parse("{\"nonce\":\"n0nce-test-value\",\"allowed\":false,\"categories\":[\"gore\",\"violence\"],\"reason\":\"Shows a weapon.\"}", &v) == AVATAR_VERDICT_REJECTED);
    CHECK(!strcmp(v.categories, "gore,violence"));
    CHECK(!strcmp(v.reason, "Shows a weapon."));
    CHECK(parse("{\"nonce\":\"WRONG\",\"allowed\":false,\"categories\":[\"hate\"],\"reason\":\"x\"}", &v) == AVATAR_VERDICT_REJECTED);
    CHECK(!strcmp(v.categories, "hate"));

    /* labels are normalized, unknown ones dropped; a rejection with no known label is "other" */
    CHECK(parse("{\"nonce\":\"n0nce-test-value\",\"allowed\":false,\"categories\":[\"Self-Harm\",\"Personal Info\",\"bogus\"],\"reason\":\"x\"}", &v) == AVATAR_VERDICT_REJECTED);
    CHECK(!strcmp(v.categories, "self_harm,personal_info"));
    CHECK(parse("{\"nonce\":\"n0nce-test-value\",\"allowed\":false,\"categories\":[\"bogus\"],\"reason\":\"\"}", &v) == AVATAR_VERDICT_REJECTED);
    CHECK(!strcmp(v.categories, "other"));
    CHECK(!strcmp(v.reason, "This picture does not meet the community guidelines."));

    /* the reason shown to the owner: control characters and runs of spaces collapse, length is capped */
    CHECK(parse("{\"nonce\":\"n0nce-test-value\",\"allowed\":false,\"categories\":[\"gore\"],\"reason\":\"a\\u0007  b\\n\\tc  \"}", &v) == AVATAR_VERDICT_REJECTED);
    CHECK(!strcmp(v.reason, "a b c"));
    {
        char *long_reason = malloc(900);
        memset(long_reason, 'x', 899);
        long_reason[899] = 0;
        char *raw = malloc(1100);
        snprintf(raw, 1100, "{\"nonce\":\"n0nce-test-value\",\"allowed\":false,\"categories\":[\"gore\"],\"reason\":\"%s\"}", long_reason);
        CHECK(parse(raw, &v) == AVATAR_VERDICT_REJECTED);
        CHECK(strlen(v.reason) < sizeof(v.reason));
        free(raw);
        free(long_reason);
    }
}

static void prompt_and_nonce(void) {
    char nonce[40];
    CHECK(avatar_verdict_nonce(nonce, sizeof(nonce)) == 0);
    CHECK(strlen(nonce) == 18); /* kn_random_hex(9 bytes) */
    char nonce2[40];
    avatar_verdict_nonce(nonce2, sizeof(nonce2));
    CHECK(strcmp(nonce, nonce2) != 0);
    CHECK(avatar_verdict_nonce(nonce, 4) != 0);

    char prompt[4096];
    CHECK(avatar_verdict_prompt(prompt, sizeof(prompt), NONCE) == 0);
    CHECK(strstr(prompt, "\"nonce\":\"n0nce-test-value\"") != NULL);
    CHECK(strstr(prompt, "SECURITY:") != NULL);
    CHECK(strstr(prompt, "Respond with ONLY a single JSON object") != NULL);
    CHECK(avatar_verdict_prompt(prompt, 64, NONCE) != 0);
}

/* ---- intake */

static unsigned char *make_rgba(const int w, const int h) {
    unsigned char *px = malloc((size_t) w * (size_t) h * 4);
    for (int y = 0; y < h; y++)
        for (int x = 0; x < w; x++) {
            unsigned char *p = px + ((size_t) y * (size_t) w + (size_t) x) * 4;
            /* left half red, right half blue: the square crop must keep the middle of the picture */
            p[0] = x < w / 2 ? 255 : 0;
            p[1] = (unsigned char) (y * 255 / (h > 1 ? h - 1 : 1));
            p[2] = x < w / 2 ? 0 : 255;
            p[3] = 255;
        }
    return px;
}

static unsigned char *read_file(const char *path, size_t *n) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long len = ftell(f);
    fseek(f, 0, SEEK_SET);
    unsigned char *buf = malloc(len > 0 ? (size_t) len : 1);
    *n = fread(buf, 1, (size_t) len, f);
    fclose(f);
    return buf;
}

static unsigned char *png_fixture(const char *dir, const char *name, const int w, const int h, size_t *n) {
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    unsigned char *px = make_rgba(w, h);
    const int ok = png_write_rgba(path, px, w, h);
    free(px);
    if (!ok) return NULL;
    return read_file(path, n);
}

static void intake(const char *dir) {
    AvatarImage img;
    size_t n = 0;
    unsigned char *png;

    /* type: by the first bytes only */
    CHECK(avatar_sniff((const unsigned char *) "\x89PNG\r\n\x1a\nxxxx", 12) == AVATAR_FMT_PNG);
    CHECK(avatar_sniff((const unsigned char *) "\xff\xd8\xff\xe0xxxx", 8) == AVATAR_FMT_JPEG);
    CHECK(avatar_sniff((const unsigned char *) "RIFF\0\0\0\0WEBPVP8 ", 16) == AVATAR_FMT_WEBP);
    CHECK(avatar_sniff((const unsigned char *) "GIF89a\x01\x00\x01\x00", 10) == AVATAR_FMT_NONE);
    CHECK(avatar_sniff((const unsigned char *) "<svg xmlns=", 11) == AVATAR_FMT_NONE);
    CHECK(avatar_sniff((const unsigned char *) "\x89PNG", 4) == AVATAR_FMT_NONE); /* too short to be sure */

    /* a GIF and an SVG are refused as types, whatever their name says */
    CHECK(!strcmp(avatar_image_normalize((const unsigned char *) "GIF89a\x01\x00\x01\x00\x80\x00", 12, &img), "UnsupportedType"));
    CHECK(!strcmp(avatar_image_normalize((const unsigned char *) "<svg xmlns='http://www.w3.org/2000/svg'/>", 40, &img), "UnsupportedType"));

    /* size before anything is decoded */
    {
        unsigned char *big = calloc(1, AVATAR_MAX_BYTES + 1);
        memcpy(big, "\x89PNG\r\n\x1a\n", 8);
        CHECK(!strcmp(avatar_image_normalize(big, AVATAR_MAX_BYTES + 1, &img), "FileTooLarge"));
        free(big);
    }

    /* the decoding checks need the WebP build: without it every upload answers AvatarsUnavailable */
    if (!avatar_image_available()) {
        printf("  (WebP support missing: decode, dimension and output checks skipped)\n");
        return;
    }

    /* a signature with nothing behind it */
    CHECK(!strcmp(avatar_image_normalize((const unsigned char *) "\x89PNG\r\n\x1a\nnot really a png at all", 32, &img), "BadImage"));

    png = png_fixture(dir, "tiny.png", 16, 16, &n);
    CHECK(png != NULL);
    if (png) {
        CHECK(!strcmp(avatar_image_normalize(png, n, &img), "ImageTooSmall"));
        free(png);
    }

    /* 2100 px wide: over AVATAR_MAX_SRC_PX (2048); its file is small enough to reach the dimension check */
    png = png_fixture(dir, "wide.png", 2100, 40, &n);
    CHECK(png != NULL);
    if (png) {
        CHECK(n <= AVATAR_MAX_BYTES);
        CHECK(!strcmp(avatar_image_normalize(png, n, &img), "ImageTooLarge"));
        free(png);
    }

    /* a 300 x 200 picture: centre square, 256 x 256 WebP, the upload's size reported */
    png = png_fixture(dir, "wide-ok.png", 300, 200, &n);
    CHECK(png != NULL);
    if (png) {
        CHECK(avatar_sniff(png, n) == AVATAR_FMT_PNG);
        const char *err = avatar_image_normalize(png, n, &img);
        CHECK(err == NULL);
        if (!err) {
            CHECK(img.fmt == AVATAR_FMT_PNG && img.src_w == 300 && img.src_h == 200);
            CHECK(img.webp_len > 12 && !memcmp(img.webp, "RIFF", 4) && !memcmp(img.webp + 8, "WEBP", 4));
#ifdef KN_HAVE_WEBP
            int w = 0, h = 0;
            uint8_t *rgba = WebPDecodeRGBA(img.webp, img.webp_len, &w, &h);
            CHECK(rgba && w == AVATAR_PX && h == AVATAR_PX);
            if (rgba) {
                /* the crop is the centre square of x 50..249: the left half (red) stays left, the blue right */
                const unsigned char *left = rgba + (size_t) (AVATAR_PX / 2 - 8) * 4;
                const unsigned char *right = rgba + (size_t) (AVATAR_PX / 2 + 8) * 4;
                CHECK(left[0] > 200 && left[2] < 60);
                CHECK(right[2] > 200 && right[0] < 60);
                WebPFree(rgba);
            }
#endif
            avatar_image_free(&img);
        }
        free(png);
    }

    /* an already square WebP upload is accepted too */
#ifdef KN_HAVE_WEBP
    {
        unsigned char *px = make_rgba(64, 64);
        uint8_t *webp = NULL;
        const size_t len = WebPEncodeRGBA(px, 64, 64, 64 * 4, 80.0f, &webp);
        free(px);
        CHECK(len > 0 && webp);
        if (webp) {
            CHECK(avatar_sniff(webp, len) == AVATAR_FMT_WEBP);
            const char *err = avatar_image_normalize(webp, len, &img);
            CHECK(err == NULL);
            if (!err) {
                CHECK(img.fmt == AVATAR_FMT_WEBP && img.src_w == 64);
                avatar_image_free(&img);
            }
            WebPFree(webp);
        }
    }
#endif
}

int main(int argc, char **argv) {
    const char *dir = argc > 1 ? argv[1] : ".";
    verdicts();
    prompt_and_nonce();
    intake(dir);
    printf("avatar unit test: %s\n", g_fail ? "FAILED" : "OK");
    return g_fail ? 1 : 0;
}
