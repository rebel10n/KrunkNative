// Standalone test for the HTML engine GL backend (gl_backend.c).
//
// Renders a display list exercising every DlOp into a 1280x720 offscreen
// framebuffer under a hidden GLFW window, writes it as a PPM and asserts a
// few pixel colors.
//
// Build (throwaway CMake project in a scratch directory; KN = the repository):
//
//   cmake_minimum_required(VERSION 3.16)
//   project(kn_glb_test C)
//   set(CMAKE_C_STANDARD 11)
//   set(KN /path/to/KrunkNative)
//   foreach(dep ZLIB BZIP2 PNG HARFBUZZ BROTLI)
//     set(FT_DISABLE_${dep} TRUE CACHE BOOL "" FORCE)
//   endforeach()
//   set(GLFW_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
//   set(GLFW_BUILD_TESTS OFF CACHE BOOL "" FORCE)
//   set(GLFW_BUILD_DOCS OFF CACHE BOOL "" FORCE)
//   add_subdirectory(${KN}/client/lib/freetype freetype)
//   add_subdirectory(${KN}/client/lib/glad glad)
//   add_subdirectory(${KN}/client/lib/glfw glfw)
//   add_subdirectory(${KN}/shared/lib/cJSON cJSON)
//   set(E ${KN}/client/src/html/engine)
//   add_executable(test_gl_backend ${E}/tests/test_gl_backend.c ${E}/gl_backend.c ${KN}/client/src/shader.c
//       ${KN}/shared/src/utils.c)
//   target_include_directories(test_gl_backend PRIVATE ${E} ${KN}/client/include ${KN}/shared/include
//       ${KN}/shared/lib/cJSON ${KN}/client/lib/lexbor)
//   target_link_libraries(test_gl_backend PRIVATE freetype glad glfw cJSON m)
//
// Run from a directory named cmake-build-* next to the repository's assets/ so
// assets_path() resolves ../assets/shaders/html_ui.{vert,frag}:
//
//   cd $KN/cmake-build-debug && LIBGL_ALWAYS_SOFTWARE=1 xvfb-run -a <build>/test_gl_backend <scratch>/out.ppm
#include <gl.h>
#include <GLFW/glfw3.h>
#include <headless.h> // ctest: KRUNKNATIVE_HEADLESS=1, an EGL pbuffer instead of a window
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "gl_backend.h"

// client.h globals the backend keeps coherent (defined by main.c in the client).
unsigned int g_blank_texture = 0;
unsigned int g_active_texture = 0;
unsigned int g_active_shader = 0;

#define W 1280
#define H 720

static const CssColor BG = {30, 30, 34, 255, CSS_COLOR_RGBA};
static int g_failures = 0;

static DlItem item(DlOp op) {
    DlItem it;
    memset(&it, 0, sizeof(it));
    it.op = op;
    return it;
}

static void push_rect(DisplayList *dl, DlRect r, CssColor c) {
    DlItem it = item(DL_RECT);
    it.u.rect.rect = r;
    it.u.rect.color = c;
    dl_push(dl, &it);
}

static void push_rrect(DisplayList *dl, DlRect r, DlRadii rad, CssColor c) {
    DlItem it = item(DL_RRECT);
    it.u.rrect.rect = r;
    it.u.rrect.radii = rad;
    it.u.rrect.color = c;
    dl_push(dl, &it);
}

static void push_border(DisplayList *dl, DlRect r, DlRadii rad, const float w[4], const CssColor c[4], const uint8_t style[4]) {
    DlItem it = item(DL_BORDER);
    it.u.border.rect = r;
    it.u.border.radii = rad;
    for (int i = 0; i < 4; i++) {
        it.u.border.width[i] = w[i];
        it.u.border.color[i] = c[i];
        it.u.border.style[i] = style[i];
    }
    dl_push(dl, &it);
}

static void push_box_shadow(DisplayList *dl, DlRect r, DlRadii rad, float dx, float dy, float blur, float spread, CssColor c, int inset, float pad) {
    DlItem it = item(DL_BOX_SHADOW);
    it.u.box_shadow.rect = r;
    it.u.box_shadow.radii = rad;
    it.u.box_shadow.dx = dx;
    it.u.box_shadow.dy = dy;
    it.u.box_shadow.blur = blur;
    it.u.box_shadow.spread = spread;
    it.u.box_shadow.color = c;
    it.u.box_shadow.inset = (uint8_t) inset;
    for (int i = 0; i < 4; i++) it.u.box_shadow.inset_pad[i] = pad;
    dl_push(dl, &it);
}

static void push_op(DisplayList *dl, DlOp op) {
    DlItem it = item(op);
    dl_push(dl, &it);
}

static void push_transform(DisplayList *dl, Mat2D m) {
    DlItem it = item(DL_PUSH_TRANSFORM);
    it.u.transform.m = m;
    dl_push(dl, &it);
}

static void push_group(DisplayList *dl, DlRect r, float opacity, const CssFilterOp *filters, int n) {
    DlItem it = item(DL_PUSH_GROUP);
    it.u.group.rect = r;
    it.u.group.opacity = opacity;
    it.u.group.n_filters = n;
    for (int i = 0; i < n; i++) it.u.group.filters[i] = filters[i];
    dl_push(dl, &it);
}

static CssLength pct(float v) {
    CssLength l = {0.0f, v, 0.0f, CSS_LEN_VALUE};
    return l;
}

static CssGradientStop stop(CssColor c, float pct_pos, int has) {
    CssGradientStop s;
    memset(&s, 0, sizeof(s));
    s.color = c;
    s.pos = pct(pct_pos);
    s.has_pos = (uint8_t) has;
    return s;
}

static Mat2D mat_mul(Mat2D a, Mat2D m) {
    Mat2D r;
    r.a = a.a * m.a + a.c * m.b;
    r.b = a.b * m.a + a.d * m.b;
    r.c = a.a * m.c + a.c * m.d;
    r.d = a.b * m.c + a.d * m.d;
    r.e = a.a * m.e + a.c * m.f + a.e;
    r.f = a.b * m.e + a.d * m.f + a.f;
    return r;
}

static GLuint make_texture(int w, int h, GLenum internal, GLenum format, const void *px) {
    GLuint t;
    glGenTextures(1, &t);
    glBindTexture(GL_TEXTURE_2D, t);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, internal, w, h, 0, format, GL_UNSIGNED_BYTE, px);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return t;
}

static GLuint checkerboard_texture(void) {
    uint8_t px[8 * 8 * 4];
    for (int y = 0; y < 8; y++)
        for (int x = 0; x < 8; x++) {
            const int on = ((x >> 1) + (y >> 1)) & 1;
            uint8_t *p = px + (y * 8 + x) * 4;
            p[0] = on ? 240 : 60;
            p[1] = on ? 200 : 90;
            p[2] = on ? 40 : 200;
            p[3] = 255;
        }
    return make_texture(8, 8, GL_RGBA8, GL_RGBA, px);
}

// 64x64 R8 "glyph": anti-aliased disc, radius 24.
static GLuint disc_texture(void) {
    uint8_t px[64 * 64];
    for (int y = 0; y < 64; y++)
        for (int x = 0; x < 64; x++) {
            const float d = sqrtf((x + 0.5f - 32.0f) * (x + 0.5f - 32.0f) + (y + 0.5f - 32.0f) * (y + 0.5f - 32.0f)) - 24.0f;
            float c = 0.5f - d;
            c = c < 0.0f ? 0.0f : c > 1.0f ? 1.0f : c;
            px[y * 64 + x] = (uint8_t) (c * 255.0f + 0.5f);
        }
    return make_texture(64, 64, GL_R8, GL_RED, px);
}

static const uint8_t *px_at(const uint8_t *img, int x, int y) {
    return img + ((H - 1 - y) * W + x) * 4;   // glReadPixels rows are bottom-up
}

static void expect(const uint8_t *img, int x, int y, int r, int g, int b, int tol, const char *what) {
    const uint8_t *p = px_at(img, x, y);
    const int ok = abs(p[0] - r) <= tol && abs(p[1] - g) <= tol && abs(p[2] - b) <= tol;
    printf("%s  %-44s (%4d,%4d) got (%3d,%3d,%3d) want (%3d,%3d,%3d) +-%d\n", ok ? "PASS" : "FAIL", what, x, y, p[0], p[1], p[2], r, g, b, tol);
    if (!ok) g_failures++;
}

static void expect_darker(const uint8_t *img, int x, int y, int r, int g, int b, const char *what) {
    const uint8_t *p = px_at(img, x, y);
    const int ok = p[0] < r && p[1] < g && p[2] < b;
    printf("%s  %-44s (%4d,%4d) got (%3d,%3d,%3d) want darker than (%3d,%3d,%3d)\n", ok ? "PASS" : "FAIL", what, x, y, p[0], p[1], p[2], r, g, b);
    if (!ok) g_failures++;
}

static int write_ppm(const char *path, const uint8_t *img) {
    FILE *f = fopen(path, "wb");
    if (!f) return -1;
    fprintf(f, "P6\n%d %d\n255\n", W, H);
    for (int y = 0; y < H; y++)
        for (int x = 0; x < W; x++) fwrite(px_at(img, x, y), 1, 3, f);
    fclose(f);
    return 0;
}

static void build_list(DisplayList *dl, GLuint checker, GLuint disc, DlGlyphQuad *glyphs, int n_glyphs, float *poly) {
    const CssColor red = dl_color(229, 57, 53, 255), green = dl_color(67, 160, 71, 160), blue = dl_color(66, 165, 245, 255);
    const CssColor amber = dl_color(255, 179, 0, 255), white = dl_color(255, 255, 255, 255), black = dl_color(0, 0, 0, 255);
    const CssColor transparent = dl_color(0, 0, 0, 0), teal = dl_color(38, 166, 154, 255), orange = dl_color(255, 112, 67, 255);
    const CssColor paper = dl_color(236, 239, 241, 255);

    // --- row 1: solid, rounded, borders, images --------------------------------
    push_rect(dl, dl_rect(20, 20, 100, 60), red);
    push_rect(dl, dl_rect(130, 20, 100, 60), green);                          // semi-transparent over bg
    push_rrect(dl, dl_rect(250, 20, 120, 80), dl_radii(0, 20, 40, 10), blue);  // mixed radii
    push_rrect(dl, dl_rect(390, 20, 80, 80), dl_radii_all(40), amber);         // circle

    {   // solid border, per-edge widths and colors, rounded
        const float w[4] = {2, 6, 10, 4};
        const CssColor c[4] = {white, dl_color(255, 0, 0, 255), dl_color(0, 255, 0, 255), dl_color(0, 128, 255, 255)};
        const uint8_t s[4] = {DL_BORDER_SOLID, DL_BORDER_SOLID, DL_BORDER_SOLID, DL_BORDER_SOLID};
        push_border(dl, dl_rect(500, 20, 120, 80), dl_radii_all(12), w, c, s);
    }
    {   // dashed
        const float w[4] = {3, 3, 3, 3};
        const CssColor c[4] = {white, white, white, white};
        const uint8_t s[4] = {DL_BORDER_DASHED, DL_BORDER_DASHED, DL_BORDER_DASHED, DL_BORDER_DASHED};
        push_border(dl, dl_rect(640, 20, 120, 80), dl_radii_all(0), w, c, s);
    }
    {   // dotted
        const float w[4] = {4, 4, 4, 4};
        const CssColor c[4] = {amber, amber, amber, amber};
        const uint8_t s[4] = {DL_BORDER_DOTTED, DL_BORDER_DOTTED, DL_BORDER_DOTTED, DL_BORDER_DOTTED};
        push_border(dl, dl_rect(780, 20, 120, 80), dl_radii_all(6), w, c, s);
    }
    {   // CSS triangle: width/height 0, border-left/right 30 transparent, border-bottom 40 red
        const float w[4] = {0, 30, 40, 30};
        const CssColor c[4] = {transparent, transparent, red, transparent};
        const uint8_t s[4] = {DL_BORDER_SOLID, DL_BORDER_SOLID, DL_BORDER_SOLID, DL_BORDER_SOLID};
        push_border(dl, dl_rect(930, 20, 60, 40), dl_radii_all(0), w, c, s);
    }
    {   // images: the same 8x8 checkerboard, linear then nearest
        DlItem it = item(DL_IMAGE);
        it.u.image.tex = checker;
        const float uv[4] = {0, 0, 1, 1};
        memcpy(it.u.image.uv, uv, sizeof(uv));
        it.u.image.dst = dl_rect(1020, 20, 100, 100);
        it.u.image.opacity = 1.0f;
        it.u.image.nearest = 0;
        dl_push(dl, &it);
        it.u.image.dst = dl_rect(1140, 20, 100, 100);
        it.u.image.nearest = 1;
        dl_push(dl, &it);
    }

    // --- row 2: gradients and shadows ------------------------------------------
    {   // linear to right with a hard stop at 85%
        DlItem it = item(DL_GRADIENT);
        it.u.gradient.rect = dl_rect(20, 140, 200, 60);
        it.u.gradient.gradient.kind = CSS_GRADIENT_LINEAR;
        it.u.gradient.gradient.angle_deg = 90.0f;
        it.u.gradient.gradient.stops[0] = stop(white, 85, 1);
        it.u.gradient.gradient.stops[1] = stop(black, 85, 1);
        it.u.gradient.gradient.stop_count = 2;
        dl_push(dl, &it);
    }
    {   // linear 135deg red -> blue, three stops (middle auto-positioned)
        DlItem it = item(DL_GRADIENT);
        it.u.gradient.rect = dl_rect(240, 140, 200, 60);
        it.u.gradient.gradient.kind = CSS_GRADIENT_LINEAR;
        it.u.gradient.gradient.angle_deg = 135.0f;
        it.u.gradient.gradient.stops[0] = stop(red, 0, 0);
        it.u.gradient.gradient.stops[1] = stop(amber, 0, 0);
        it.u.gradient.gradient.stops[2] = stop(blue, 0, 0);
        it.u.gradient.gradient.stop_count = 3;
        dl_push(dl, &it);
    }
    {   // radial ellipse farthest-corner at 50% 50%
        DlItem it = item(DL_GRADIENT);
        it.u.gradient.rect = dl_rect(460, 140, 200, 100);
        it.u.gradient.gradient.kind = CSS_GRADIENT_RADIAL;
        it.u.gradient.gradient.shape = CSS_RADIAL_ELLIPSE;
        it.u.gradient.gradient.extent = CSS_RADIAL_FARTHEST_CORNER;
        it.u.gradient.gradient.cx = pct(50);
        it.u.gradient.gradient.cy = pct(50);
        it.u.gradient.gradient.stops[0] = stop(dl_color(255, 235, 59, 255), 0, 0);
        it.u.gradient.gradient.stops[1] = stop(dl_color(30, 136, 229, 255), 0, 0);
        it.u.gradient.gradient.stop_count = 2;
        dl_push(dl, &it);
    }
    {   // box-shadows blur 0 / 4 / 8 with spread 0 / 2 / 4, offset (4, 4)
        const float blur[3] = {0, 4, 8}, spread[3] = {0, 2, 4};
        for (int i = 0; i < 3; i++) {
            const DlRect r = dl_rect(700 + 120 * (float) i, 150, 80, 50);
            push_box_shadow(dl, r, dl_radii_all(8), 4, 4, blur[i], spread[i], dl_color(0, 0, 0, 160), 0, 0);
            push_rrect(dl, r, dl_radii_all(8), paper);
        }
        // inset shadow
        const DlRect r = dl_rect(1060, 150, 120, 60);
        push_rrect(dl, r, dl_radii_all(8), white);
        push_box_shadow(dl, r, dl_radii_all(8), 0, 4, 8, 0, dl_color(0, 0, 0, 140), 1, 0);
    }

    // --- row 3: glyphs + text shadows, nested clips, transforms -----------------
    push_rect(dl, dl_rect(10, 260, 220, 110), dl_color(150, 150, 150, 255));  // panel so shadows show
    {
        DlItem sh = item(DL_TEXT_SHADOW);                     // sharp offset shadow
        sh.u.text_shadow.quads = glyphs;
        sh.u.text_shadow.n = n_glyphs;
        sh.u.text_shadow.dx = 2;
        sh.u.text_shadow.dy = 2;
        sh.u.text_shadow.blur = 0;
        sh.u.text_shadow.color = black;
        sh.u.text_shadow.bounds = dl_rect(20, 270, 200, 32);
        dl_push(dl, &sh);
        DlItem g = item(DL_GLYPHS);
        g.u.glyphs.quads = glyphs;
        g.u.glyphs.n = n_glyphs;
        g.u.glyphs.color = white;
        dl_push(dl, &g);

        // second row: blurred shadow only (no glyph fill), then glyphs in amber
        DlItem sh2 = sh;
        sh2.u.text_shadow.blur = 4;
        sh2.u.text_shadow.dx = 0;
        sh2.u.text_shadow.dy = 50;
        sh2.u.text_shadow.color = dl_color(0, 0, 0, 255);
        dl_push(dl, &sh2);
        DlItem g2 = item(DL_GLYPHS);
        g2.u.glyphs.quads = glyphs + n_glyphs;                 // offset copies made by the caller
        g2.u.glyphs.n = n_glyphs;
        g2.u.glyphs.color = amber;
        dl_push(dl, &g2);
    }
    {   // nested clips: rrect (shader) -> rect (scissor) -> polygon (stencil)
        DlItem c1 = item(DL_PUSH_CLIP_RRECT);
        c1.u.clip_rrect.rect = dl_rect(300, 260, 200, 120);
        c1.u.clip_rrect.radii = dl_radii_all(30);
        dl_push(dl, &c1);
        DlItem c2 = item(DL_PUSH_CLIP_RECT);
        c2.u.clip_rect.rect = dl_rect(310, 275, 250, 150);
        dl_push(dl, &c2);
        poly[0] = 290; poly[1] = 250; poly[2] = 560; poly[3] = 250; poly[4] = 400; poly[5] = 420;
        DlItem c3 = item(DL_PUSH_CLIP_POLY);
        c3.u.clip_poly.pts = poly;
        c3.u.clip_poly.n = 3;
        dl_push(dl, &c3);
        push_rect(dl, dl_rect(250, 230, 400, 250), orange);
        push_op(dl, DL_POP_CLIP);
        push_op(dl, DL_POP_CLIP);
        push_op(dl, DL_POP_CLIP);
        // after the pops nothing is clipped: a small marker rect
        push_rect(dl, dl_rect(300, 390, 200, 6), orange);
    }
    {   // transform: scale(0.8) rotate(45deg) about (700, 320); clip rect inside -> stencil
        const float cx = 700, cy = 320, s = 0.8f, a = 45.0f * 3.14159265f / 180.0f;
        Mat2D m = {1, 0, 0, 1, cx, cy};
        const Mat2D rot = {cosf(a), sinf(a), -sinf(a), cosf(a), 0, 0};
        const Mat2D scl = {s, 0, 0, s, 0, 0};
        const Mat2D back = {1, 0, 0, 1, -cx, -cy};
        m = mat_mul(mat_mul(mat_mul(m, rot), scl), back);
        push_transform(dl, m);
        DlItem c = item(DL_PUSH_CLIP_RECT);
        c.u.clip_rect.rect = dl_rect(640, 280, 120, 80);
        dl_push(dl, &c);
        push_rect(dl, dl_rect(600, 240, 200, 160), teal);
        push_op(dl, DL_POP_CLIP);
        const float w[4] = {3, 3, 3, 3};
        const CssColor bc[4] = {white, white, white, white};
        const uint8_t bs[4] = {DL_BORDER_SOLID, DL_BORDER_SOLID, DL_BORDER_SOLID, DL_BORDER_SOLID};
        push_border(dl, dl_rect(640, 280, 120, 80), dl_radii_all(10), w, bc, bs);
        push_rrect(dl, dl_rect(770, 300, 60, 40), dl_radii_all(6), amber);  // rotated rounded rect
        push_op(dl, DL_POP_TRANSFORM);
    }

    // --- row 4: groups ----------------------------------------------------------
    {   // opacity group 0.5 over two overlapping opaque rects
        push_group(dl, dl_rect(20, 420, 220, 120), 0.5f, NULL, 0);
        push_rect(dl, dl_rect(20, 420, 150, 100), dl_color(255, 0, 0, 255));
        push_rect(dl, dl_rect(90, 460, 150, 80), dl_color(0, 0, 255, 255));
        push_op(dl, DL_POP_GROUP);
    }
    {   // grayscale(1) group
        CssFilterOp f;
        memset(&f, 0, sizeof(f));
        f.kind = CSS_FILTER_GRAYSCALE;
        f.amount = 1.0f;
        push_group(dl, dl_rect(280, 420, 200, 120), 1.0f, &f, 1);
        push_rect(dl, dl_rect(280, 420, 200, 120), dl_color(255, 0, 0, 255));
        push_rrect(dl, dl_rect(300, 440, 60, 60), dl_radii_all(30), dl_color(0, 0, 255, 255));
        push_op(dl, DL_POP_GROUP);
    }
    {   // drop-shadow(8 8 6 rgba(0,0,0,.8)) group
        CssFilterOp f;
        memset(&f, 0, sizeof(f));
        f.kind = CSS_FILTER_DROP_SHADOW;
        f.shadow.x = 8;
        f.shadow.y = 8;
        f.shadow.blur = 6;
        f.shadow.color = dl_color(0, 0, 0, 204);
        push_group(dl, dl_rect(540, 420, 200, 120), 1.0f, &f, 1);
        push_rrect(dl, dl_rect(560, 440, 120, 80), dl_radii_all(12), white);
        push_rrect(dl, dl_rect(640, 430, 60, 40), dl_radii_all(6), amber);
        push_op(dl, DL_POP_GROUP);
    }
    {   // fast path: single primitive group with opacity 0.5
        push_group(dl, dl_rect(800, 420, 100, 100), 0.5f, NULL, 0);
        push_rrect(dl, dl_rect(800, 420, 100, 100), dl_radii_all(16), dl_color(0, 0, 255, 255));
        push_op(dl, DL_POP_GROUP);
    }
    {   // hue-rotate(120deg) + brightness(1.2) + opacity 0.8 group
        CssFilterOp f[2];
        memset(f, 0, sizeof(f));
        f[0].kind = CSS_FILTER_HUE_ROTATE;
        f[0].amount = 120.0f;
        f[1].kind = CSS_FILTER_BRIGHTNESS;
        f[1].amount = 1.2f;
        push_group(dl, dl_rect(920, 420, 120, 120), 0.8f, f, 2);
        push_rect(dl, dl_rect(920, 420, 120, 120), dl_color(255, 0, 0, 255));
        push_rrect(dl, dl_rect(950, 450, 60, 60), dl_radii_all(10), white);
        push_op(dl, DL_POP_GROUP);
    }
    {   // scroll container: clip + SCROLL_OFFSET (0, -40)
        DlItem c = item(DL_PUSH_CLIP_RECT);
        c.u.clip_rect.rect = dl_rect(1080, 420, 160, 120);
        dl_push(dl, &c);
        DlItem s = item(DL_SCROLL_OFFSET);
        s.u.scroll.dx = 0;
        s.u.scroll.dy = -40;
        dl_push(dl, &s);
        for (int i = 0; i < 4; i++)
            push_rect(dl, dl_rect(1080, 420 + 50.0f * (float) i, 160, 40), (i & 1) ? paper : teal);
        push_op(dl, DL_POP_CLIP);
        push_rect(dl, dl_rect(1080, 545, 160, 4), white);     // scroll offset undone by POP_CLIP
    }

    // --- row 5: large rounded panel with shadow + border (menu-like) ---------------
    {
        const DlRect r = dl_rect(20, 580, 600, 110);
        push_box_shadow(dl, r, dl_radii_all(10), 0, 6, 16, 0, dl_color(0, 0, 0, 180), 0, 0);
        push_rrect(dl, r, dl_radii_all(10), dl_color(55, 60, 70, 255));
        const float w[4] = {1, 1, 1, 1};
        const CssColor bc[4] = {dl_color(255, 255, 255, 60), dl_color(255, 255, 255, 60), dl_color(255, 255, 255, 60), dl_color(255, 255, 255, 60)};
        const uint8_t bs[4] = {DL_BORDER_SOLID, DL_BORDER_SOLID, DL_BORDER_SOLID, DL_BORDER_SOLID};
        push_border(dl, r, dl_radii_all(10), w, bc, bs);
    }
}

int main(int argc, char **argv) {
    headless_parse_args(&argc, argv); // KRUNKNATIVE_HEADLESS=1 / --headless
    const char *out = argc > 1 ? argv[1] : "test_gl_backend.ppm";

    if (!headless_glfw_init()) {
        fprintf(stderr, "glfwInit failed\n");
        return 77; // no GL: ctest SKIP_RETURN_CODE
    }
    glfwWindowHint(GLFW_VISIBLE, GLFW_FALSE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    headless_window_hints();
    GLFWwindow *win = glfwCreateWindow(64, 64, "glb test", NULL, NULL);
    if (!win) {
        fprintf(stderr, "glfwCreateWindow failed\n");
        return 77;
    }
    headless_make_context_current(win);
    if (!kn_gl_load(headless_get_proc_address)) {
        fprintf(stderr, "glad failed\n");
        return 2;
    }
    printf("GL: %s / %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    // 1280x720 RGBA8 + depth/stencil framebuffer
    GLuint fbo, color, ds;
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, W, H, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glGenRenderbuffers(1, &ds);
    glBindRenderbuffer(GL_RENDERBUFFER, ds);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, W, H);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ds);
    if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "test framebuffer incomplete\n");
        return 2;
    }
    glViewport(0, 0, W, H);
    glClearColor(BG.r / 255.0f, BG.g / 255.0f, BG.b / 255.0f, 1.0f);
    // The window framebuffer's stencil is never cleared by the client and its
    // initial contents are undefined (NVIDIA: non-zero). Start dirty: the backend
    // must reset it itself, or every stencil clip (nested rounded clips: class
    // cards, attachment/reticle previews) drops its content.
    glClearStencil(0xFF);
    glClear(GL_COLOR_BUFFER_BIT | GL_STENCIL_BUFFER_BIT);
    // the client leaves these on; the backend must cope and restore the blend func
    glEnable(GL_DEPTH_TEST);
    glEnable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

    GlBackend *b = glb_create();
    if (!b) {
        fprintf(stderr, "glb_create failed (run from a cmake-build-* directory so ../assets/shaders resolves)\n");
        return 2;
    }

    // a non-identity gamma table: mild contrast boost, same for every luminance row
    uint8_t lut[8][256];
    for (int r = 0; r < 8; r++)
        for (int i = 0; i < 256; i++) {
            const float c = i / 255.0f;
            const float v = powf(c, 1.0f / 1.2f);
            lut[r][i] = (uint8_t) (v * 255.0f + 0.5f);
        }
    glb_set_text_gamma(b, lut);

    const GLuint checker = checkerboard_texture();
    const GLuint disc = disc_texture();

    // glyph quads: 5 discs, 32 px, 40 px advance; second set 50 px lower
    enum { NG = 5 };
    DlGlyphQuad glyphs[NG * 2];
    for (int i = 0; i < NG * 2; i++) {
        const int k = i % NG;
        glyphs[i].tex = disc;
        glyphs[i].uv[0] = 0; glyphs[i].uv[1] = 0; glyphs[i].uv[2] = 1; glyphs[i].uv[3] = 1;
        glyphs[i].x = 20 + 40.0f * (float) k;
        glyphs[i].y = 270 + (i >= NG ? 50.0f : 0.0f);
        glyphs[i].w = 32;
        glyphs[i].h = 32;
    }

    float poly[6];
    DisplayList dl = {0};
    build_list(&dl, checker, disc, glyphs, NG, poly);
    printf("display list: %d items\n", dl.n);

    glb_begin(b, W, H, 1.0f);
    glb_submit(b, &dl);
    glb_end(b);

    const GlbStats st = glb_stats(b);
    printf("stats: draw_calls=%d vertices=%d fbo_passes=%d\n", st.draw_calls, st.vertices, st.fbo_passes);

    // state restored?
    GLint blend_src = 0, blend_dst = 0, vao = -1, fb = -1;
    glGetIntegerv(GL_BLEND_SRC_ALPHA, &blend_src);
    glGetIntegerv(GL_BLEND_DST_ALPHA, &blend_dst);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &vao);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &fb);
    const int state_ok = blend_src == GL_SRC_ALPHA && blend_dst == GL_ONE_MINUS_SRC_ALPHA && vao == 0 && fb == (GLint) fbo &&
                         !glIsEnabled(GL_SCISSOR_TEST) && !glIsEnabled(GL_STENCIL_TEST) && glIsEnabled(GL_CULL_FACE) && !glIsEnabled(GL_DEPTH_TEST);
    printf("%s  GL state after glb_end (blend %x/%x vao %d fbo %d)\n", state_ok ? "PASS" : "FAIL", blend_src, blend_dst, vao, fb);
    if (!state_ok) g_failures++;

    uint8_t *img = malloc((size_t) W * H * 4);
    glReadBuffer(GL_COLOR_ATTACHMENT0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, img);
    const GLenum err = glGetError();
    printf("%s  no GL error (0x%x)\n", err == GL_NO_ERROR ? "PASS" : "FAIL", err);
    if (err != GL_NO_ERROR) g_failures++;

    // --- assertions ---------------------------------------------------------------
    expect(img, 70, 50, 229, 57, 53, 2, "solid rect");
    // green a=160 over bg: 30 + (67-30)*160/255 ...
    expect(img, 180, 50, 30 + (67 - 30) * 160 / 255, 30 + (160 - 30) * 160 / 255, 34 + (71 - 34) * 160 / 255, 3, "translucent rect blend");
    expect(img, 252, 22, 66, 165, 245, 2, "rrect: square tl corner (radius 0)");
    expect(img, 366, 96, BG.r, BG.g, BG.b, 2, "rrect: br corner radius 40 cut");
    expect(img, 392, 22, BG.r, BG.g, BG.b, 2, "circle: corner outside");
    expect(img, 430, 60, 255, 179, 0, 2, "circle: center");
    expect(img, 560, 95, 0, 255, 0, 2, "border: bottom edge (10px green)");
    expect(img, 617, 60, 255, 0, 0, 2, "border: right edge (6px red)");
    expect(img, 560, 60, BG.r, BG.g, BG.b, 2, "border: interior empty");
    expect(img, 960, 58, 229, 57, 53, 2, "css triangle: bottom center red");
    expect(img, 935, 25, BG.r, BG.g, BG.b, 2, "css triangle: top-left transparent edge");
    expect(img, 1150, 30, 60, 90, 200, 2, "image nearest: crisp dark cell");
    expect(img, 1171, 26, 240, 200, 40, 2, "image nearest: crisp light cell");
    expect(img, 644, 21, 255, 255, 255, 3, "dashed border: top edge dash");
    expect(img, 652, 21, BG.r, BG.g, BG.b, 3, "dashed border: top edge gap");
    expect(img, 784, 22, 255, 179, 0, 6, "dotted border: top edge dot");
    expect(img, 788, 22, BG.r, BG.g, BG.b, 3, "dotted border: top edge gap");
    expect(img, 898, 56, 255, 179, 0, 6, "dotted border: right edge dot");
    expect(img, 180, 170, 255, 255, 255, 3, "gradient hard stop: white side");
    expect(img, 200, 170, 0, 0, 0, 3, "gradient hard stop: black side");
    expect(img, 560, 190, 255, 235, 59, 6, "radial gradient center");
    expect(img, 740, 175, 236, 239, 241, 2, "box-shadow 0: box fill");
    expect_darker(img, 782, 198, BG.r + 1, BG.g + 1, BG.b + 1, "box-shadow 0: hard shadow corner");
    expect_darker(img, 1030, 205, BG.r + 1, BG.g + 1, BG.b + 1, "box-shadow 8 spread 4: soft shadow");
    expect_darker(img, 1120, 157, 250, 250, 250, "inset shadow: darker near top edge");
    expect(img, 1120, 185, 255, 255, 255, 3, "inset shadow: center stays white");
    expect(img, 36, 286, 255, 255, 255, 3, "glyph disc center (gamma LUT)");
    expect(img, 48, 291, 0, 0, 0, 6, "text-shadow offset copy (+2,+2)");
    expect_darker(img, 36, 349, 130, 130, 130, "text-shadow blur 4 just below disc");
    expect(img, 400, 300, 255, 112, 67, 2, "nested clips: inside all three");
    expect(img, 330, 350, BG.r, BG.g, BG.b, 2, "nested clips: outside polygon");
    expect(img, 330, 272, BG.r, BG.g, BG.b, 2, "nested clips: outside scissor rect");
    expect(img, 498, 277, BG.r, BG.g, BG.b, 2, "nested clips: outside rrect corner");
    expect(img, 497, 292, 255, 112, 67, 2, "nested clips: inside rrect past corner");
    expect(img, 400, 393, 255, 112, 67, 2, "clips popped: marker visible");
    expect(img, 700, 320, 38, 166, 154, 2, "transform: rotated clip center");
    expect(img, 640, 282, BG.r, BG.g, BG.b, 2, "transform: original corner now outside");
    // opacity group: overlap pixel = 0.5 * blue + 0.5 * bg (NOT 0.25 red)
    expect(img, 120, 480, BG.r / 2, BG.g / 2, 128 + BG.b / 2, 3, "opacity group 0.5: overlap (group math)");
    expect(img, 50, 440, 128 + BG.r / 2, BG.g / 2, BG.b / 2, 3, "opacity group 0.5: red only");
    expect(img, 450, 500, 54, 54, 54, 4, "grayscale group: red -> gray");
    expect(img, 330, 470, 18, 18, 18, 4, "grayscale group: blue -> dark gray");
    expect(img, 620, 480, 255, 255, 255, 2, "drop-shadow group: content intact");
    expect_darker(img, 686, 525, BG.r, BG.g, BG.b, "drop-shadow group: shadow below right");
    expect(img, 850, 470, BG.r / 2, BG.g / 2, 128 + BG.b / 2, 3, "fast-path opacity group 0.5");
    expect(img, 1100, 440, 236, 239, 241, 2, "scroll offset: second row shifted up 40");
    expect(img, 1100, 547, 255, 255, 255, 2, "scroll offset undone after POP_CLIP");

    if (write_ppm(out, img)) {
        fprintf(stderr, "cannot write %s\n", out);
        g_failures++;
    } else printf("wrote %s\n", out);

    free(img);
    dl_free(&dl);
    glb_destroy(b);
    glDeleteTextures(1, &checker);
    glDeleteTextures(1, &disc);
    glDeleteFramebuffers(1, &fbo);
    glDeleteRenderbuffers(1, &ds);
    glDeleteTextures(1, &color);
    glfwDestroyWindow(win);
    glfwTerminate();

    printf("%d failure(s)\n", g_failures);
    return g_failures ? 1 : 0;
}
