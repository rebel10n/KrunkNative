/*
 * test_image.c - standalone test for client/src/html/engine/image.c.
 *
 * Built from a throwaway CMake project (no repository target yet), rendered
 * on llvmpipe under Xvfb:
 *
 *   R=/home/user/src/KrunkNative; B=/tmp/kn_image_test
 *   mkdir -p $B && cat > $B/CMakeLists.txt <<EOF
 *   cmake_minimum_required(VERSION 3.16)
 *   project(kn_image_test C)
 *   set(CMAKE_C_STANDARD 11)
 *   set(R $R)
 *   add_subdirectory(\${R}/client/lib/glad glad)
 *   add_subdirectory(\${R}/client/lib/glfw glfw)
 *   add_subdirectory(\${R}/shared/lib/cJSON cJSON)
 *   add_executable(test_image \${R}/client/src/html/engine/tests/test_image.c
 *       \${R}/client/src/html/engine/image.c \${R}/client/src/stb_image.c \${R}/shared/src/utils.c)
 *   target_include_directories(test_image PRIVATE \${R}/client/include \${R}/client/src/html/engine
 *       \${R}/shared/include \${R}/shared/lib/cJSON)
 *   target_link_libraries(test_image PRIVATE glad glfw cJSON m)
 *   EOF
 *   cmake -S $B -B $B/build -G Ninja -DGLFW_BUILD_EXAMPLES=OFF -DGLFW_BUILD_TESTS=OFF -DGLFW_BUILD_DOCS=OFF
 *   cmake --build $B/build
 *   LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a $B/build/test_image $R/ui/
 *
 * Expected sizes come from doc/ui-feature-inventory.md section 5 and
 * `identify` over ui/; reference pixels from `convert <png> -crop 1x1+X+Y txt:`
 * (straight alpha, converted to premultiplied here).
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <gl.h>
#include <GLFW/glfw3.h>
#include <stb_image.h>

#include "image.h"

static int g_failures;
static int g_checks;

#define CHECK(cond, ...)                                              \
    do {                                                              \
        g_checks++;                                                   \
        if (!(cond)) {                                                \
            g_failures++;                                             \
            fprintf(stderr, "FAIL %s:%d: ", __FILE__, __LINE__);      \
            fprintf(stderr, __VA_ARGS__);                             \
            fputc('\n', stderr);                                      \
        }                                                             \
    } while (0)

/* ---- expected assets ----------------------------------------------------- */

typedef struct Expected {
    const char *url;
    const char *base;
    int w, h;
} Expected;

/* <img src> references (inventory: 43 <img>, 38 with src) resolved from the
 * document root.  Window fragments (ui/windows/) are spliced into
 * index.html, so their <img> URLs resolve against the document too. */
static const Expected k_img_src[] = {
    {"img/android.png", "index.html", 646, 250},
    {"img/app_0.png", "index.html", 256, 256},
    {"img/app_1.png", "index.html", 256, 256},
    {"img/blood.png", "index.html", 256, 256},
    {"img/favicon.png", "index.html", 16, 16},
    {"img/ios.png", "index.html", 498, 167},
    {"img/linux.png", "index.html", 128, 128},
    {"img/logo_4.png", "index.html", 1024, 302},
    {"img/mac.png", "index.html", 128, 128},
    {"img/maps/map_0.png", "index.html", 200, 80},
    {"img/maps/map_1.png", "index.html", 200, 80},
    {"img/maps/map_10.png", "index.html", 200, 80},
    {"img/merch.png", "index.html", 512, 512},
    {"img/noimg.png", "index.html", 110, 50},
    {"img/phone_r.png", "index.html", 256, 512},
    {"img/police.png", "index.html", 128, 128},
    {"img/skull_0.png", "index.html", 32, 32},
    {"img/skull_1.png", "index.html", 64, 60},
    {"img/skull_2.png", "index.html", 64, 64},
    {"img/timer.png", "index.html", 32, 32},
    {"img/windows.png", "index.html", 128, 128},
    {"textures/ammo_0.png", "index.html", 128, 128},
    {"textures/classes/icon_0.png", "index.html", 8, 8},
    {"textures/classes/icon_1.png", "index.html", 8, 8},
    {"textures/classes/icon_2.png", "index.html", 8, 8},
    {"textures/classes/icon_3.png", "index.html", 8, 8},
    {"textures/classes/icon_4.png", "index.html", 8, 8},
    {"textures/classes/icon_5.png", "index.html", 8, 8},
    {"textures/classes/icon_6.png", "index.html", 8, 8},
    {"textures/classes/icon_7.png", "index.html", 8, 8},
    {"textures/classes/icon_8.png", "index.html", 8, 8},
    {"textures/classes/icon_9.png", "index.html", 8, 8},
    {"textures/classes/icon_10.png", "index.html", 8, 8},
    {"textures/classes/icon_11.png", "index.html", 8, 8},
    {"textures/classes/icon_12.png", "index.html", 8, 8},
    {"textures/classes/icon_13.png", "index.html", 8, 8},
    {"textures/hitmarker_3.png", "index.html", 150, 150},
    {"textures/recticle.png", "index.html", 1024, 1024},
    {"textures/reticles/reticle_0.png", "index.html", 32, 32},
    {"textures/weapons/icon_0.png", "index.html", 256, 128},
    {"textures/weapons/icon_1.png", "index.html", 256, 128},
    {"textures/weapons/icon_2.png", "index.html", 256, 128},
    {"textures/weapons/icon_3.png", "index.html", 256, 128},
    {"textures/weapons/icon_4.png", "index.html", 256, 128},
    {"textures/weapons/icon_5.png", "index.html", 256, 128},
    {"textures/weapons/icon_6.png", "index.html", 256, 128},
    {"textures/weapons/icon_7.png", "index.html", 256, 128},
    {"textures/weapons/icon_8.png", "index.html", 256, 128},
    {"textures/weapons/icon_9.png", "index.html", 256, 128},
    {"textures/weapons/icon_10.png", "index.html", 256, 128},
    {"textures/weapons/icon_11.png", "index.html", 256, 128},
    {"textures/weapons/icon_12.png", "index.html", 256, 128},
    {"textures/melee/icon_0.png", "index.html", 256, 128},
};

/* url() references written in ui/css/ (relative to the sheet). */
static const Expected k_css_url[] = {
    {"../img/blank_2.png", "css/main.css", 32, 32},
    {"../img/country-flag.png", "css/flag.css", 512, 512},
    {"../img/giveaway.png", "css/main.css", 1027, 578},
    {"../img/menu-icons/chall.png", "css/main.css", 128, 128},
    {"../img/menu-icons/chall-white.png", "css/main.css", 128, 128},
    {"../img/menu-icons/games.png", "css/main.css", 128, 128},
    {"../img/menu-icons/games-white.png", "css/main.css", 128, 128},
    {"../img/menu-icons/maps.png", "css/main.css", 128, 128},
    {"../img/menu-icons/maps-white.png", "css/main.css", 128, 128},
    {"../img/menu-icons/mods.png", "css/main.css", 128, 128},
    {"../img/menu-icons/mods-white.png", "css/main.css", 128, 128},
    {"../img/menu-icons/profile.png", "css/main.css", 128, 128},
    {"../img/menu-icons/profile-white.png", "css/main.css", 128, 128},
    {"../img/menu-icons/ranked.png", "css/main.css", 128, 128},
    {"../img/menu-icons/ranked-white.png", "css/main.css", 128, 128},
    {"../img/menu-icons/settings.png", "css/main.css", 128, 128},
    {"../img/menu-icons/settings-white.png", "css/main.css", 128, 128},
    {"../img/menu-icons/shop.png", "css/main.css", 128, 128},
    {"../img/menu-icons/shop-white.png", "css/main.css", 128, 128},
    {"../img/menu-icons/social.png", "css/main.css", 128, 128},
    {"../img/menu-icons/social-white.png", "css/main.css", 128, 128},
    {"../img/mobile/icon_0.png", "css/main.css", 256, 256},
    {"../img/mobile/icon_1.png", "css/main.css", 256, 256},
    {"../img/mobile/icon_2.png", "css/main.css", 256, 256},
    {"../img/mobile/icon_3.png", "css/main.css", 256, 256},
    {"../img/mobile/icon_4.png", "css/main.css", 256, 256},
    {"../img/mobile/icon_5.png", "css/main.css", 256, 256},
    {"../img/mobile/icon_6.png", "css/main.css", 256, 256},
    {"../img/social-buttons/discord-hover.png", "css/main.css", 80, 92},
    {"../img/social-buttons/discord-normal.png", "css/main.css", 80, 92},
    {"../img/social-buttons/discord-pressed.png", "css/main.css", 80, 92},
    {"../img/social-buttons/down_hover.png", "css/main.css", 80, 92},
    {"../img/social-buttons/down_normal.png", "css/main.css", 80, 92},
    {"../img/social-buttons/down_pressed.png", "css/main.css", 80, 92},
    {"../img/social-buttons/reddit-hover.png", "css/main.css", 80, 92},
    {"../img/social-buttons/reddit-normal.png", "css/main.css", 80, 92},
    {"../img/social-buttons/reddit-pressed.png", "css/main.css", 80, 92},
    {"../img/social-buttons/twitter-hover.png", "css/main.css", 80, 92},
    {"../img/social-buttons/twitter-normal.png", "css/main.css", 80, 92},
    {"../img/social-buttons/twitter-pressed.png", "css/main.css", 80, 92},
};

/* Referenced but absent from ui/ (inventory "Surprises" 11): must fail. */
static const Expected k_missing[] = {
    {"../img/world_0.png", "css/main.css", 0, 0},
    {"../img/chals/icon_0.png", "css/main.css", 0, 0},
    {"../img/social-buttons/fandom-hover.png", "css/main.css", 0, 0},
    {"../img/social-buttons/fandom-normal.png", "css/main.css", 0, 0},
    {"../img/social-buttons/fandom-pressed.png", "css/main.css", 0, 0},
};

/* ---- helpers --------------------------------------------------------------- */

static unsigned char *read_texture(const unsigned tex, const int w, const int h) {
    unsigned char *px = malloc((size_t) w * h * 4);
    glBindTexture(GL_TEXTURE_2D, tex);
    glPixelStorei(GL_PACK_ALIGNMENT, 4);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glBindTexture(GL_TEXTURE_2D, 0);
    return px;
}

static int premul(const int c, const int a) {
    return (int) floor(c * a / 255.0 + 0.5);
}

/* Compares texel (x, y) with a straight-alpha reference pixel, premultiplying
 * the reference and allowing +-1 of rounding on the color channels. */
static void expect_pixel(const char *name, const unsigned char *px, const int w, const int x, const int y,
                         const int r, const int g, const int b, const int a) {
    const unsigned char *p = px + ((size_t) y * w + x) * 4;
    const int er = premul(r, a), eg = premul(g, a), eb = premul(b, a);
    const int ok = abs(p[0] - er) <= 1 && abs(p[1] - eg) <= 1 && abs(p[2] - eb) <= 1 && p[3] == a;
    CHECK(ok, "%s (%d,%d): got (%d,%d,%d,%d) expected premultiplied (%d,%d,%d,%d) from straight (%d,%d,%d,%d)",
          name, x, y, p[0], p[1], p[2], p[3], er, eg, eb, a, r, g, b, a);
}

static double row_mean_alpha(const unsigned char *px, const int w, const int y) {
    double sum = 0;
    for (int x = 0; x < w; x++) sum += px[((size_t) y * w + x) * 4 + 3];
    return sum / w;
}

static void check_loaded(ImageCache *cache, const Expected *list, const int n, const char *what) {
    int loaded = 0;

    for (int i = 0; i < n; i++) {
        const ImageEntry *e = image_get(cache, list[i].url, list[i].base);

        CHECK(!e->failed, "%s %s failed to load", what, list[i].url);
        CHECK(e->tex != 0, "%s %s has no texture", what, list[i].url);
        CHECK(e->w == list[i].w && e->h == list[i].h, "%s %s size %dx%d, expected %dx%d", what, list[i].url, e->w,
              e->h, list[i].w, list[i].h);

        if (!e->failed) loaded++;
    }

    printf("  %-10s %d/%d loaded\n", what, loaded, n);
}

static int near(const float a, const float b) {
    return fabsf(a - b) < 0.01f;
}

/* ---- tests ------------------------------------------------------------------ */

static void test_url_resolution(void) {
    char out[256];

    CHECK(image_resolve_url("../img/x.png", "css/main.css", out, sizeof(out)) && !strcmp(out, "img/x.png"),
          "css relative: %s", out);
    CHECK(image_resolve_url("img/x.png", "index.html", out, sizeof(out)) && !strcmp(out, "img/x.png"),
          "document relative: %s", out);
    CHECK(image_resolve_url("img/x.png", NULL, out, sizeof(out)) && !strcmp(out, "img/x.png"), "no base: %s",
          out);
    CHECK(image_resolve_url("img/x.png", "", out, sizeof(out)) && !strcmp(out, "img/x.png"), "empty base: %s",
          out);
    CHECK(image_resolve_url("../textures/a.png", "windows/host.html", out, sizeof(out)) &&
              !strcmp(out, "textures/a.png"),
          "window relative: %s", out);
    CHECK(image_resolve_url("/img/x.png", "css/main.css", out, sizeof(out)) && !strcmp(out, "img/x.png"),
          "root absolute: %s", out);
    CHECK(image_resolve_url("img/x.png?build=123", "index.html", out, sizeof(out)) && !strcmp(out, "img/x.png"),
          "query stripped: %s", out);
    CHECK(image_resolve_url("../css/fonts/a.ttf?v=1#frag", "css/main.css", out, sizeof(out)) &&
              !strcmp(out, "css/fonts/a.ttf"),
          "query+fragment stripped: %s", out);
    CHECK(image_resolve_url("./img/./x.png", "index.html", out, sizeof(out)) && !strcmp(out, "img/x.png"),
          "dot segments: %s", out);
    CHECK(image_resolve_url("../../img/x.png", "css/main.css", out, sizeof(out)) == 0, "escape above root accepted");
    CHECK(image_resolve_url("../x.png", "index.html", out, sizeof(out)) == 0, "escape from root accepted");
    CHECK(image_resolve_url("/../x.png", "index.html", out, sizeof(out)) == 0, "absolute escape accepted");
    CHECK(image_resolve_url("http://example.com/x.png", "index.html", out, sizeof(out)) == 0, "http accepted");
    CHECK(image_resolve_url("data:image/png;base64,AAAA", "index.html", out, sizeof(out)) == 0, "data: accepted");
    CHECK(image_resolve_url("//cdn/x.png", "index.html", out, sizeof(out)) == 0, "protocol-relative accepted");
    CHECK(image_resolve_url("", "index.html", out, sizeof(out)) == 0, "empty accepted");
    CHECK(image_resolve_url("?build=1", "index.html", out, sizeof(out)) == 0, "query-only accepted");
    CHECK(image_resolve_url("img/", "index.html", out, sizeof(out)) == 0, "directory accepted");
    CHECK(image_resolve_url("img/x.png", "index.html", out, 4) == 0, "overflow accepted");
}

static void test_pixels(ImageCache *cache) {
    /* palette + tRNS (color type 3, 4-bit), 8x8 class icon */
    const ImageEntry *icon = image_get(cache, "textures/classes/icon_0.png", "index.html");
    CHECK(!icon->failed && icon->w == 8 && icon->h == 8, "icon_0 load");

    if (!icon->failed) {
        unsigned char *px = read_texture(icon->tex, 8, 8);
        expect_pixel("icon_0", px, 8, 0, 0, 0, 0, 0, 0);
        expect_pixel("icon_0", px, 8, 1, 0, 114, 80, 62, 255);
        expect_pixel("icon_0", px, 8, 2, 2, 207, 188, 143, 255);
        expect_pixel("icon_0", px, 8, 0, 4, 61, 61, 61, 255);
        expect_pixel("icon_0", px, 8, 2, 6, 223, 215, 212, 255);
        expect_pixel("icon_0", px, 8, 0, 7, 61, 61, 61, 255); /* bottom row: distinguishes a flip */
        free(px);
    }

    /* RGBA (color type 6) 8x8 icon with (255,255,255,0) transparent texels */
    const ImageEntry *icon13 = image_get(cache, "textures/classes/icon_13.png", "index.html");
    CHECK(!icon13->failed, "icon_13 load");

    if (!icon13->failed) {
        unsigned char *px = read_texture(icon13->tex, 8, 8);
        expect_pixel("icon_13", px, 8, 0, 0, 255, 255, 255, 0); /* premultiplied -> (0,0,0,0) */
        expect_pixel("icon_13", px, 8, 7, 0, 255, 255, 255, 0);
        expect_pixel("icon_13", px, 8, 0, 7, 255, 255, 255, 255);
        CHECK(px[0] == 0 && px[1] == 0 && px[2] == 0, "icon_13 (0,0) not premultiplied: (%d,%d,%d)", px[0], px[1],
              px[2]);
        free(px);
    }

    /* RGB (color type 2): alpha must be 255 everywhere */
    const ImageEntry *map = image_get(cache, "img/maps/map_10.png", "index.html");
    CHECK(!map->failed && map->w == 200 && map->h == 80, "map_10 load");

    if (!map->failed) {
        unsigned char *px = read_texture(map->tex, 200, 80);
        expect_pixel("map_10", px, 200, 0, 0, 16, 13, 24, 255);
        expect_pixel("map_10", px, 200, 199, 79, 255, 104, 255, 255);

        int opaque = 1;
        for (int i = 0; i < 200 * 80; i++) opaque &= px[i * 4 + 3] == 255;
        CHECK(opaque, "map_10 has non-opaque texels");
        free(px);
    }

    /* logo_4.png: no vertical flip + premultiplication of semi-transparent texels.
     * convert logo_4.png -crop 1x1+0+0 txt:   -> (0,0,0,0)
     * convert logo_4.png -crop 1x1+418+0 txt: -> (16,16,16,77)
     * convert logo_4.png -crop 1x1+276+5 txt: -> (16,16,16,147)
     * convert logo_4.png -crop 1x1+513+301 txt: -> (16,16,16,254)
     * row 0 mean alpha 0.1515, row 301 mean alpha 0.00136 */
    const ImageEntry *logo = image_get(cache, "img/logo_4.png", "index.html");
    CHECK(!logo->failed && logo->w == 1024 && logo->h == 302, "logo_4 load");

    if (!logo->failed) {
        unsigned char *px = read_texture(logo->tex, 1024, 302);
        expect_pixel("logo_4", px, 1024, 0, 0, 0, 0, 0, 0);
        expect_pixel("logo_4", px, 1024, 418, 0, 16, 16, 16, 77);
        expect_pixel("logo_4", px, 1024, 276, 5, 16, 16, 16, 147);
        expect_pixel("logo_4", px, 1024, 513, 301, 16, 16, 16, 254);

        const double top = row_mean_alpha(px, 1024, 0);
        const double bottom = row_mean_alpha(px, 1024, 301);
        CHECK(fabs(top - 0.151517 * 255) < 1.0, "logo_4 row 0 mean alpha %.2f, expected %.2f (flipped?)", top,
              0.151517 * 255);
        CHECK(fabs(bottom - 0.0013557 * 255) < 0.5, "logo_4 row 301 mean alpha %.2f, expected %.2f (flipped?)",
              bottom, 0.0013557 * 255);
        free(px);
    }
}

static void test_texture_state(ImageCache *cache) {
    const ImageEntry *icon = image_get(cache, "textures/classes/icon_1.png", "index.html");
    CHECK(!icon->failed, "icon_1 load");
    if (icon->failed) return;

    GLint v = 0;
    glBindTexture(GL_TEXTURE_2D, icon->tex);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &v);
    CHECK(v == GL_LINEAR_MIPMAP_LINEAR, "default MIN_FILTER 0x%x", v);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &v);
    CHECK(v == GL_LINEAR, "default MAG_FILTER %d", v);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &v);
    CHECK(v == GL_CLAMP_TO_EDGE, "WRAP_S %d", v);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, &v);
    CHECK(v == GL_CLAMP_TO_EDGE, "WRAP_T %d", v);
    glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &v);
    CHECK(v == 3, "MAX_LEVEL %d, expected 3 (8x8 -> 1x1)", v);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_INTERNAL_FORMAT, &v);
    CHECK(v == GL_RGBA8, "internal format 0x%x", v);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &v);
    CHECK(v == 4, "mip level 1 width %d", v);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 3, GL_TEXTURE_WIDTH, &v);
    CHECK(v == 1, "mip level 3 width %d", v);
    glBindTexture(GL_TEXTURE_2D, 0);

    /* mips are built from premultiplied texels: every level keeps rgb <= a,
     * and a non-square NPOT chain (giveaway 1027x578) is complete down to 1x1 */
    {
        const ImageEntry *logo = image_get(cache, "img/logo_4.png", "index.html");
        glBindTexture(GL_TEXTURE_2D, logo->tex);
        GLint lw = 0, lh = 0;
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_WIDTH, &lw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 2, GL_TEXTURE_HEIGHT, &lh);
        CHECK(lw == 256 && lh == 75, "logo_4 level 2 %dx%d", lw, lh);
        unsigned char *px = malloc((size_t) lw * lh * 4);
        glGetTexImage(GL_TEXTURE_2D, 2, GL_RGBA, GL_UNSIGNED_BYTE, px);
        int premul_ok = 1;
        for (int i = 0; i < lw * lh; i++) {
            const unsigned char *p = px + i * 4;
            premul_ok &= p[0] <= p[3] && p[1] <= p[3] && p[2] <= p[3];
        }
        CHECK(premul_ok, "logo_4 level 2 has rgb > alpha (mips not premultiplied)");
        free(px);

        const ImageEntry *give = image_get(cache, "img/giveaway.png", "index.html");
        glBindTexture(GL_TEXTURE_2D, give->tex);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &v);
        CHECK(v == 10, "giveaway MAX_LEVEL %d, expected 10", v);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 10, GL_TEXTURE_WIDTH, &lw);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 10, GL_TEXTURE_HEIGHT, &lh);
        CHECK(lw == 1 && lh == 1, "giveaway level 10 %dx%d", lw, lh);
        glBindTexture(GL_TEXTURE_2D, 0);
    }

    /* image_texture(): linear by default, lazily created nearest twin with identical pixels */
    CHECK(image_texture(icon, 0) == icon->tex, "image_texture(0) != tex");
    CHECK(icon->tex_nearest == 0, "tex_nearest created eagerly");

    const unsigned nearest = image_texture(icon, 1);
    CHECK(nearest != 0 && nearest != icon->tex, "nearest twin %u (tex %u)", nearest, icon->tex);
    CHECK(nearest == icon->tex_nearest, "tex_nearest not stored");
    CHECK(image_texture(icon, 1) == nearest, "nearest twin recreated");

    if (nearest) {
        glBindTexture(GL_TEXTURE_2D, nearest);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, &v);
        CHECK(v == GL_NEAREST, "nearest MIN_FILTER %d", v);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, &v);
        CHECK(v == GL_NEAREST, "nearest MAG_FILTER %d", v);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, &v);
        CHECK(v == GL_CLAMP_TO_EDGE, "nearest WRAP_S %d", v);
        glGetTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, &v);
        CHECK(v == 0, "nearest MAX_LEVEL %d", v);
        glGetTexLevelParameteriv(GL_TEXTURE_2D, 1, GL_TEXTURE_WIDTH, &v);
        CHECK(v == 0, "nearest twin has mip level 1 (width %d)", v);
        glBindTexture(GL_TEXTURE_2D, 0);

        unsigned char *a = read_texture(icon->tex, icon->w, icon->h);
        unsigned char *b = read_texture(nearest, icon->w, icon->h);
        CHECK(!memcmp(a, b, (size_t) icon->w * icon->h * 4), "nearest twin pixels differ");
        free(a);
        free(b);
    }

    /* binding state is restored after a lazy load mid-frame */
    GLuint probe = 0;
    glGenTextures(1, &probe);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, probe);
    const ImageEntry *fresh = image_get(cache, "textures/classes/icon_2.png", "index.html");
    (void) image_texture(fresh, 1);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &v);
    CHECK((GLuint) v == probe, "texture binding not restored (%d != %u)", v, probe);
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &v);
    CHECK(v == 0, "read framebuffer binding not restored (%d)", v);
    glBindTexture(GL_TEXTURE_2D, 0);
    glDeleteTextures(1, &probe);

    CHECK(glGetError() == GL_NO_ERROR, "GL error left behind");
}

static void test_failures(ImageCache *cache) {

    for (size_t i = 0; i < sizeof(k_missing) / sizeof(k_missing[0]); i++) {
        const ImageEntry *e = image_get(cache, k_missing[i].url, k_missing[i].base);
        CHECK(e->failed && e->tex == 0 && e->w == 0 && e->h == 0, "missing %s not failed", k_missing[i].url);
        CHECK(image_texture(e, 0) == 0 && image_texture(e, 1) == 0, "failed entry returned a texture");
    }

    /* undecodable: a stylesheet is not an image */
    const ImageEntry *css = image_get(cache, "css/main.css", "index.html");
    CHECK(css->failed, "css decoded as an image");

    /* rejected URLs */
    CHECK(image_get(cache, "../../etc/passwd", "css/main.css")->failed, "escape loaded");
    CHECK(image_get(cache, "http://example.com/x.png", "index.html")->failed, "http loaded");
    CHECK(image_get(cache, "", "index.html")->failed, "empty loaded");

    /* failures are cached: the same entry comes back, no second log line */
    const int before = image_cache_count(cache);
    const ImageEntry *again = image_get(cache, "../img/world_0.png", "css/main.css");
    CHECK(again->failed && image_cache_count(cache) == before, "failed entry not cached");

    /* the same file through different spellings is one entry */
    const ImageEntry *a = image_get(cache, "img/timer.png", "index.html");
    const ImageEntry *b = image_get(cache, "../img/timer.png?build=7", "css/hud.css");
    const ImageEntry *c = image_get(cache, "/img/./timer.png", "windows/3.html");
    CHECK(a == b && b == c, "cache key not normalised");
}

static void test_geometry(void) {
    float x, y, w, h;

    /* map thumb 200x80 in a 100x100 box */
    image_cover_contain_rect(200, 80, 100, 100, CSS_OBJECT_FIT_COVER, &x, &y, &w, &h);
    CHECK(near(w, 250) && near(h, 100) && near(x, -75) && near(y, 0), "cover 200x80->100x100: %g %g %g %g", x, y, w, h);
    image_cover_contain_rect(200, 80, 100, 100, CSS_OBJECT_FIT_CONTAIN, &x, &y, &w, &h);
    CHECK(near(w, 100) && near(h, 40) && near(x, 0) && near(y, 30), "contain 200x80->100x100: %g %g %g %g", x, y, w, h);

    /* giveaway 1027x578 covering 1920x1080: width-limited */
    image_cover_contain_rect(1027, 578, 1920, 1080, CSS_OBJECT_FIT_COVER, &x, &y, &w, &h);
    CHECK(near(w, 1920) && fabsf(h - 1080.5842f) < 0.01f && near(x, 0) && fabsf(y + 0.2921f) < 0.01f,
          "cover giveaway: %g %g %g %g", x, y, w, h);

    /* menu icon 128x128 contained in 64x80 */
    image_cover_contain_rect(128, 128, 64, 80, CSS_OBJECT_FIT_CONTAIN, &x, &y, &w, &h);
    CHECK(near(w, 64) && near(h, 64) && near(x, 0) && near(y, 8), "contain icon: %g %g %g %g", x, y, w, h);

    /* fill / none / scale-down */
    image_fit_size(128, 128, 64, 80, CSS_OBJECT_FIT_FILL, &w, &h);
    CHECK(near(w, 64) && near(h, 80), "fill: %g %g", w, h);
    image_fit_size(128, 128, 64, 80, CSS_OBJECT_FIT_NONE, &w, &h);
    CHECK(near(w, 128) && near(h, 128), "none: %g %g", w, h);
    image_fit_size(128, 128, 64, 80, CSS_OBJECT_FIT_SCALE_DOWN, &w, &h);
    CHECK(near(w, 64) && near(h, 64), "scale-down (smaller box): %g %g", w, h);
    image_fit_size(8, 8, 64, 80, CSS_OBJECT_FIT_SCALE_DOWN, &w, &h);
    CHECK(near(w, 8) && near(h, 8), "scale-down (larger box): %g %g", w, h);
    image_fit_size(8, 8, 64, 80, CSS_OBJECT_FIT_CONTAIN, &w, &h);
    CHECK(near(w, 64) && near(h, 64), "contain upscales: %g %g", w, h);
    image_fit_size(0, 0, 64, 80, CSS_OBJECT_FIT_COVER, &w, &h);
    CHECK(near(w, 0) && near(h, 0), "cover of a failed image: %g %g", w, h);

    /* background-size */
    CssBgSize size;
    memset(&size, 0, sizeof(size));
    size.kind = CSS_BG_SIZE_CONTAIN;
    image_bg_size(128, 128, 50, 50, &size, &w, &h);
    CHECK(near(w, 50) && near(h, 50), "bg contain: %g %g", w, h);
    size.kind = CSS_BG_SIZE_COVER;
    image_bg_size(80, 92, 40, 40, &size, &w, &h);
    CHECK(near(w, 40) && near(h, 46), "bg cover sprite: %g %g", w, h);

    size.kind = CSS_BG_SIZE_LENGTHS;
    size.w.kind = CSS_LEN_AUTO;
    size.h.kind = CSS_LEN_AUTO;
    image_bg_size(80, 92, 40, 40, &size, &w, &h);
    CHECK(near(w, 80) && near(h, 92), "bg auto auto: %g %g", w, h);

    size.w.kind = CSS_LEN_VALUE;
    size.w.px = 0;
    size.w.pct = 100;
    image_bg_size(80, 92, 40, 40, &size, &w, &h);
    CHECK(near(w, 40) && near(h, 46), "bg 100%% auto: %g %g", w, h);

    size.w.kind = CSS_LEN_AUTO;
    size.h.kind = CSS_LEN_VALUE;
    size.h.px = 23;
    size.h.pct = 0;
    image_bg_size(80, 92, 40, 40, &size, &w, &h);
    CHECK(near(w, 20) && near(h, 23), "bg auto 23px: %g %g", w, h);

    size.w.kind = CSS_LEN_VALUE;
    size.w.px = 10;
    size.w.pct = 50;
    image_bg_size(80, 92, 40, 40, &size, &w, &h);
    CHECK(near(w, 30) && near(h, 23), "bg calc(50%% + 10px) 23px: %g %g", w, h);

    /* background-position: `right 8px center` = {px:-8, pct:100} {pct:50} */
    CssLength px_x = {-8, 100, 0, CSS_LEN_VALUE};
    CssLength px_y = {0, 50, 0, CSS_LEN_VALUE};
    image_resolve_position(&px_x, &px_y, 200, 40, 16, 16, &x, &y);
    CHECK(near(x, 176) && near(y, 12), "right 8px center: %g %g", x, y);

    CssLength center = {0, 50, 0, CSS_LEN_VALUE};
    image_resolve_position(&center, &center, 100, 100, 250, 100, &x, &y);
    CHECK(near(x, -75) && near(y, 0), "center with cover overflow: %g %g", x, y);

    CssLength zero = {0, 0, 0, CSS_LEN_VALUE};
    CssLength bottom = {0, 100, 0, CSS_LEN_VALUE};
    image_resolve_position(&zero, &bottom, 100, 100, 20, 30, &x, &y);
    CHECK(near(x, 0) && near(y, 70), "left bottom: %g %g", x, y);

    CssLength twelve = {12, 0, 0, CSS_LEN_VALUE};
    image_resolve_position(&twelve, NULL, 100, 100, 20, 30, &x, &y);
    CHECK(near(x, 12) && near(y, 0), "12px (y default): %g %g", x, y);

    CHECK(near(image_resolve_position_axis(-8, 100, 200, 16), 176), "axis helper");

    /* background-repeat: round */
    CHECK(image_repeat_round_count(100, 30) == 3 && near(image_repeat_round_size(100, 30), 100.0f / 3), "round 100/30");
    CHECK(image_repeat_round_count(100, 45) == 2 && near(image_repeat_round_size(100, 45), 50), "round 100/45");
    CHECK(image_repeat_round_count(100, 40) == 3, "round 100/40 (2.5 rounds up)");
    CHECK(image_repeat_round_count(10, 30) == 1 && near(image_repeat_round_size(10, 30), 10), "round below one tile");
    CHECK(image_repeat_round_count(100, 0) == 1 && near(image_repeat_round_size(100, 0), 100), "round zero tile");
}

/* ---- main ------------------------------------------------------------------- */

int main(const int argc, char **argv) {
    const char *ui_dir = argc > 1 ? argv[1] : "../ui/";

    test_url_resolution();
    test_geometry();

    if (!glfwInit()) {
        fprintf(stderr, "glfwInit failed\n");
        return 2;
    }

    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 3);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow *window = glfwCreateWindow(64, 64, "test_image", NULL, NULL);
    if (!window) {
        fprintf(stderr, "glfwCreateWindow failed\n");
        return 2;
    }

    glfwMakeContextCurrent(window);
    if (!gladLoadGLLoader((GLADloadproc) glfwGetProcAddress)) {
        fprintf(stderr, "gladLoadGLLoader failed\n");
        return 2;
    }

    printf("GL %s on %s\n", glGetString(GL_VERSION), glGetString(GL_RENDERER));

    /* loaders.c leaves the process-global flip flag set; make sure the engine
     * ignores it (it uses the thread-local override). */
    stbi_set_flip_vertically_on_load(1);

    ImageCache *cache = image_cache_create(ui_dir);
    CHECK(cache != NULL, "image_cache_create");

    check_loaded(cache, k_img_src, (int) (sizeof(k_img_src) / sizeof(k_img_src[0])), "<img src>");
    check_loaded(cache, k_css_url, (int) (sizeof(k_css_url) / sizeof(k_css_url[0])), "url()");
    test_pixels(cache);
    test_texture_state(cache);
    test_failures(cache);

    const int count = image_cache_count(cache);
    image_cache_destroy(cache);
    CHECK(glGetError() == GL_NO_ERROR, "GL error after destroy");

    /* a second cache on the same context must be independent */
    cache = image_cache_create(ui_dir);
    const ImageEntry *e = image_get(cache, "img/timer.png", "index.html");
    CHECK(!e->failed && e->w == 32, "reload after destroy");
    image_cache_destroy(cache);

    glfwDestroyWindow(window);
    glfwTerminate();

    printf("%d checks, %d failures, %d cache entries\n", g_checks, g_failures, count);
    return g_failures ? 1 : 0;
}
