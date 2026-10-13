// billboard_ad_test: the server's billboard ad (client/include/billboard_ad.h).
//
//   billboard_ad_test [--headless] [out.png]      (from the repo root)
//
// 1. NET_PACKET_BILLBOARD / _IMAGE -> INIT: an INIT applies what arrived since the previous one
//    (map change, late join), an INIT without any (another server, offline) clears the ad, bad
//    image paths are refused.
// 2. Maps with different billboard kinds (v7 Kanji: random + fixed posters, v7 Burg: random,
//    Krunk Plaza: bb 7 clamped to 6, Lagoon: v3 "sad" ad billboards, the v3.1.1 Sandstorm):
//    every BILLBOARD object gets the ad, nothing else does (SIGN, PLANE, ...), and without an
//    ad every BILLBOARD keeps its poster.
// 3. The createCanvasAD canvas (headless GL): layout of "FRVR please hire me", background and
//    text colours, written to out.png when given.
#include <billboard_ad.h>
#include <canvas2d.h>
#include <gl.h>
#include <GLFW/glfw3.h>
#include <headless.h>
#include <png_write.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
#define CHECK(cond, ...)                                                                                                         \
    do {                                                                                                                         \
        if (!(cond)) {                                                                                                           \
            g_failures++;                                                                                                        \
            printf("FAIL %s:%d: ", __FILE__, __LINE__);                                                                         \
            printf(__VA_ARGS__);                                                                                                 \
            printf("\n");                                                                                                        \
        }                                                                                                                        \
    } while (0)

static cJSON *load_json(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    const long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    char *text = malloc((size_t) size + 1);
    if (!text) {
        fclose(f);
        return NULL;
    }
    const size_t n = fread(text, 1, (size_t) size, f);
    fclose(f);
    text[n] = 0;
    cJSON *json = cJSON_Parse(text);
    free(text);
    return json;
}

static void send_text(const char *txt, const char *txt_col, const char *bg_col) {
    NetBillboardPacket p;
    memset(&p, 0, sizeof(p));
    snprintf(p.txt, sizeof(p.txt), "%s", txt);
    snprintf(p.txt_col, sizeof(p.txt_col), "%s", txt_col);
    snprintf(p.bg_col, sizeof(p.bg_col), "%s", bg_col);
    billboard_ad_on_packet(&p);
}

static void send_image(const char *path) {
    NetBillboardImagePacket p;
    memset(&p, 0, sizeof(p));
    snprintf(p.path, sizeof(p.path), "%s", path);
    billboard_ad_on_image_packet(&p);
}

static void test_packets(void) {
    printf("Running billboard packets...\n");
    billboard_ad_on_init();
    CHECK(!billboard_ad_active(), "no packet: no ad (the maps keep their posters)");

    send_text("FRVR please hire me", "", "");
    CHECK(!billboard_ad_active(), "the ad waits for INIT");
    billboard_ad_on_init();
    const BillboardAd *ad = billboard_ad_active();
    CHECK(ad && !strcmp(ad->txt, "FRVR please hire me"), "INIT applies the text");
    CHECK(ad && !strcmp(ad->txt_col, "#e3e3e3") && !strcmp(ad->bg_col, "#000000"), "default colours %s %s", ad ? ad->txt_col : "", ad ? ad->bg_col : "");
    CHECK(ad && !ad->image[0], "no image");

    // the next map (the server resends before every INIT): still there
    send_text("FRVR please hire me", "#ff0000", "#00ff00");
    billboard_ad_on_init();
    ad = billboard_ad_active();
    CHECK(ad && !strcmp(ad->txt_col, "#ff0000") && !strcmp(ad->bg_col, "#00ff00"), "map change keeps the ad with its colours");

    // an INIT without a billboard (offline, another server): gone
    billboard_ad_on_init();
    CHECK(!billboard_ad_active(), "INIT without BILLBOARD clears the ad");

    // images: asset paths only
    static const char *const bad[] = {"https://example.com/ad.png", "/etc/passwd.png", "../ad.png", "textures//a.png", "textures/a.gif", "a b.png", ""};
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++) {
        CHECK(!billboard_image_path_ok(bad[i]), "'%s' refused", bad[i]);
        send_image(bad[i]);
        billboard_ad_on_init();
        CHECK(!billboard_ad_active(), "bad image '%s' does not set an ad", bad[i]);
    }
    send_image("textures/pubs/b_3.png");
    billboard_ad_on_init();
    ad = billboard_ad_active();
    CHECK(ad && !strcmp(ad->image, "textures/pubs/b_3.png") && !ad->txt[0], "image-only ad");
    send_text("Hello", "", "");
    send_image("user-assets/30462/texture.PNG");
    billboard_ad_on_init();
    ad = billboard_ad_active();
    CHECK(ad && !strcmp(ad->txt, "Hello") && !strcmp(ad->image, "user-assets/30462/texture.PNG"), "text + image ad");
    billboard_ad_on_init();
}

static void test_maps(void) {
    static const struct {
        const char *file;
        int billboards; // BILLBOARD objects ("i": 11)
    } maps[] = {
        {"assets/maps/kanji.json", 6},          // v7: 2 random, 4 fixed posters (bb 1 / 6)
        {"assets/maps/burg_new.json", 2},       // v7 Burg: random posters
        {"assets/maps/krunk_plaza_v2.json", 5}, // bb 7 (clamped to 6), next to SIGNs and poster PLANEs
        {"assets/maps/lagoon.json", 6},         // the v3 "sad" ad billboards
        {"assets/maps/311_sandstorm.json", 9},  // the v3.1.1 format (size index "si")
    };
    printf("Running billboard surfaces on maps...\n");
    for (size_t m = 0; m < sizeof(maps) / sizeof(maps[0]); m++) {
        cJSON *json = load_json(maps[m].file);
        CHECK(json != NULL, "%s: missing (run from the repo root)", maps[m].file);
        if (!json) continue;
        Map *map = map_init_ex(json, 0);
        CHECK(map != NULL, "%s: does not load", maps[m].file);
        if (!map) {
            cJSON_Delete(json);
            continue;
        }

        billboard_ad_on_init(); // no ad
        int surfaces = 0, kept = 0;
        for (size_t i = 0; i < map->object_count; i++) {
            surfaces += billboard_ad_is_surface(map->objects[i]);
            kept += billboard_ad_for_object(map->objects[i]) == NULL;
        }
        CHECK(surfaces == maps[m].billboards, "%s: %d billboard surfaces, expected %d", maps[m].file, surfaces, maps[m].billboards);
        CHECK(kept == (int) map->object_count, "%s: without an ad every object keeps its texture", maps[m].file);

        send_text("FRVR please hire me", "", "");
        billboard_ad_on_init();
        const BillboardAd *ad = billboard_ad_active();
        int overridden = 0, others = 0;
        for (size_t i = 0; i < map->object_count; i++) {
            const Object *o = map->objects[i];
            const BillboardAd *got = billboard_ad_for_object(o);
            if (o->prefab == PREFAB_BILLBOARD) {
                overridden += got == ad && got;
            } else {
                others += got != NULL;
            }
        }
        CHECK(overridden == maps[m].billboards, "%s: %d of %d billboards show the ad", maps[m].file, overridden, maps[m].billboards);
        CHECK(others == 0, "%s: %d non-billboard objects (signs, planes, ...) would show the ad", maps[m].file, others);
        printf("  %s: %d billboards -> ad\n", maps[m].file, overridden);
        billboard_ad_on_init();
        map_fini(map);
        free(map);
        cJSON_Delete(json);
    }
}

static int pixel_near(const unsigned char *p, const int r, const int g, const int b, const int tolerance) {
    return abs(p[0] - r) <= tolerance && abs(p[1] - g) <= tolerance && abs(p[2] - b) <= tolerance;
}

static void test_canvas(const char *out) {
    printf("Running the createCanvasAD canvas...\n");
    if (!headless_glfw_init()) { // make test on a box without any GL: the GL-free parts still ran
        printf("SKIP: glfwInit failed (use --headless / KRUNKNATIVE_HEADLESS=1)\n");
        return;
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
#ifdef KRUNKNATIVE_GL_ANGLE
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
#ifdef __APPLE__
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
#else
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
#endif
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#endif
    headless_window_hints();
    GLFWwindow *window = glfwCreateWindow(64, 64, "billboard_ad_test", NULL, NULL);
    if (!window || !headless_make_context_current(window) || !kn_gl_load(headless_get_proc_address)) {
        printf("SKIP: no GL context\n");
        glfwTerminate();
        return;
    }

    const int w = BILLBOARD_AD_WIDTH, h = BILLBOARD_AD_HEIGHT;
    Canvas2D *c = c2d_create();
    c2d_set_size(c, w, h, w, h);

    // layout: two lines at the largest size that fits 1968 x 944 (drawMultilineText)
    static BillboardAdLayout layout;
    billboard_ad_layout(c, "FRVR please hire me", w, h, &layout);
    CHECK(layout.line_count == 2, "%d lines", layout.line_count);
    CHECK(layout.line_count == 2 && !strcmp(layout.lines[0], "FRVR please") && !strcmp(layout.lines[1], "hire me"), "lines '%s' / '%s'", layout.lines[0], layout.lines[1]);
    CHECK(layout.layout_px == layout.font_px + 1, "layout size %d = font size %d + 1", layout.layout_px, layout.font_px);
    CHECK(layout.font_px > 150 && layout.font_px < 400, "font %d px", layout.font_px);
    billboard_ad_layout(c, "Ad", w, h, &layout);
    CHECK(layout.line_count == 1 && layout.font_px > 400, "one short word fills the height: %d lines, %d px", layout.line_count, layout.font_px);
    printf("  'FRVR please hire me': 2 lines; 'Ad': %d px\n", layout.font_px);

    unsigned int fbo, color;
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glViewport(0, 0, w, h);
    glClearColor(0, 0, 0, 0);
    glClear(GL_COLOR_BUFFER_BIT);

    BillboardAd ad = {"FRVR please hire me", "#ffd400", "#102030", ""};
    c2d_reset(c);
    billboard_ad_draw(c, &ad);
    c2d_composite(c);
    unsigned char *px = malloc((size_t) w * h * 4), *flip = malloc((size_t) w * h * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < h; y++) memcpy(flip + (size_t) y * w * 4, px + (size_t) (h - 1 - y) * w * 4, (size_t) w * 4);

    CHECK(pixel_near(flip, 0x10, 0x20, 0x30, 1), "corner = bgCol (%d %d %d)", flip[0], flip[1], flip[2]);
    int text = 0, top = h, bottom = 0;
    for (int y = 0; y < h; y++) {
        for (int x = 0; x < w; x++) {
            if (pixel_near(flip + ((size_t) y * w + x) * 4, 0xff, 0xd4, 0x00, 8)) {
                text++;
                if (y < top) top = y;
                if (y > bottom) bottom = y;
            }
        }
    }
    CHECK(text > w * h / 20, "txtCol covers %d px", text);
    CHECK(top > 40 && bottom < h - 40 && top < h / 2 && bottom > h / 2, "text block %d..%d inside the 40 px margin, around the middle", top, bottom);
    printf("  canvas: %d text px, rows %d..%d\n", text, top, bottom);
    if (out) {
        for (size_t i = 0; i < (size_t) w * h; i++) flip[i * 4 + 3] = 255;
        CHECK(png_write_rgba(out, flip, w, h), "write %s", out);
    }
    free(px);
    free(flip);
    c2d_destroy(c);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color);
    headless_shutdown();
    glfwTerminate();
}

int main(int argc, char **argv) {
    headless_parse_args(&argc, argv);
    const char *out = argc > 1 ? argv[1] : NULL;
    test_packets();
    test_maps();
    test_canvas(out);
    if (g_failures) {
        printf("billboard_ad_test: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("billboard_ad_test: all passed\n");
    return 0;
}
