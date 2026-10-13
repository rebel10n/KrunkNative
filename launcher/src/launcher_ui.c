// The launcher window (launcher_ui.h).
#include "launcher_ui.h"

#include <build_info.h>
#include <gl.h>
#include <headless.h>
#include <png_write.h>
#include <stb_image.h>

#include <ft2build.h>
#include FT_FREETYPE_H

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WIN_W 560
#define WIN_H 210
#define ATLAS_W 1024
#define ATLAS_H 512
#define MAX_VERTS 6000

typedef struct {
    float x0, y0, x1, y1;   // atlas UV
    int w, h, left, top, adv;
    int ok;
} Glyph;

typedef struct {
    Glyph g[96]; // ASCII 32..127
    int ascent, line;
} FontFace;

enum { FONT_TITLE, FONT_BODY, FONT_SMALL, FONT_COUNT };

struct LauncherUi {
    GLFWwindow *window;
    GLuint prog, vao, vbo, tex;
    GLint u_size;
    float scale;           // framebuffer px per window unit
    FontFace fonts[FONT_COUNT];
    int has_text;
    float verts[MAX_VERTS * 8];
    int nverts;
    int mouse_was_down;
    double t;
    int frames;
    char title[256];
};

#ifdef KN_GLES
#define SHADER_HEADER "#version 300 es\nprecision mediump float;\n"
#else
#define SHADER_HEADER "#version 330 core\n"
#endif

static const char *VS = SHADER_HEADER
    "in vec2 a_pos;\nin vec2 a_uv;\nin vec4 a_col;\nuniform vec2 u_size;\nout vec2 v_uv;\nout vec4 v_col;\n"
    "void main() {\n  v_uv = a_uv;\n  v_col = a_col;\n  vec2 p = a_pos / u_size * 2.0 - 1.0;\n"
    "  gl_Position = vec4(p.x, -p.y, 0.0, 1.0);\n}\n";
static const char *FS = SHADER_HEADER
    "in vec2 v_uv;\nin vec4 v_col;\nuniform sampler2D u_tex;\nout vec4 o_col;\n"
    "void main() {\n  o_col = vec4(v_col.rgb, v_col.a * texture(u_tex, v_uv).r);\n}\n";

static GLuint compile(const GLenum type, const char *src) {
    const GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);
    GLint ok = 0;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "[launcher] shader: %s\n", log);
    }
    return s;
}

// ------------------------------------------------------------------ fonts

static int load_face(FT_Library ft, const char *path, const int px, FontFace *f, unsigned char *atlas, int *pen_x,
                     int *pen_y, int *row_h) {
    FT_Face face;
    if (FT_New_Face(ft, path, 0, &face)) return 0;
    FT_Set_Pixel_Sizes(face, 0, (FT_UInt) px);
    f->ascent = (int) (face->size->metrics.ascender >> 6);
    f->line = (int) (face->size->metrics.height >> 6);
    for (int c = 32; c < 128; c++) {
        Glyph *g = &f->g[c - 32];
        if (FT_Load_Char(face, (FT_ULong) c, FT_LOAD_RENDER)) continue;
        const FT_Bitmap *bm = &face->glyph->bitmap;
        if (*pen_x + (int) bm->width + 2 >= ATLAS_W) {
            *pen_x = 2;
            *pen_y += *row_h + 2;
            *row_h = 0;
        }
        if (*pen_y + (int) bm->rows + 2 >= ATLAS_H) break;
        for (unsigned y = 0; y < bm->rows; y++)
            for (unsigned x = 0; x < bm->width; x++)
                atlas[(*pen_y + (int) y) * ATLAS_W + *pen_x + (int) x] = bm->buffer[y * (unsigned) bm->pitch + x];
        g->x0 = (float) *pen_x / ATLAS_W;
        g->y0 = (float) *pen_y / ATLAS_H;
        g->x1 = (float) (*pen_x + (int) bm->width) / ATLAS_W;
        g->y1 = (float) (*pen_y + (int) bm->rows) / ATLAS_H;
        g->w = (int) bm->width;
        g->h = (int) bm->rows;
        g->left = face->glyph->bitmap_left;
        g->top = face->glyph->bitmap_top;
        g->adv = (int) (face->glyph->advance.x >> 6);
        g->ok = 1;
        *pen_x += (int) bm->width + 2;
        if ((int) bm->rows > *row_h) *row_h = (int) bm->rows;
    }
    FT_Done_Face(face);
    return 1;
}

static void load_fonts(LauncherUi *ui, const char *ui_dir) {
    unsigned char *atlas = calloc(ATLAS_W * ATLAS_H, 1);
    if (!atlas) return;
    // a 4x4 white block at the origin: solid rectangles sample it
    for (int y = 0; y < 4; y++)
        for (int x = 0; x < 4; x++) atlas[y * ATLAS_W + x] = 255;
    FT_Library ft;
    if (ui_dir && !FT_Init_FreeType(&ft)) {
        char title[4096], body[4096];
        snprintf(title, sizeof(title), "%s/css/fonts/FrVrKrunkerBold2.ttf", ui_dir);
        snprintf(body, sizeof(body), "%s/css/fonts/ChakraPetch-Medium.ttf", ui_dir);
        int pen_x = 8, pen_y = 2, row_h = 4;
        const int s = (int) lroundf(ui->scale);
        const int ok = load_face(ft, title, 30 * s, &ui->fonts[FONT_TITLE], atlas, &pen_x, &pen_y, &row_h) &&
                       load_face(ft, body, 18 * s, &ui->fonts[FONT_BODY], atlas, &pen_x, &pen_y, &row_h) &&
                       load_face(ft, body, 14 * s, &ui->fonts[FONT_SMALL], atlas, &pen_x, &pen_y, &row_h);
        ui->has_text = ok;
        FT_Done_FreeType(ft);
    }
    glGenTextures(1, &ui->tex);
    glBindTexture(GL_TEXTURE_2D, ui->tex);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_R8, ATLAS_W, ATLAS_H, 0, GL_RED, GL_UNSIGNED_BYTE, atlas);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    free(atlas);
}

// ------------------------------------------------------------------ drawing (window units)

static void vert(LauncherUi *ui, const float x, const float y, const float u, const float v, const float *c) {
    if (ui->nverts >= MAX_VERTS) return;
    float *p = ui->verts + ui->nverts++ * 8;
    p[0] = x * ui->scale;
    p[1] = y * ui->scale;
    p[2] = u;
    p[3] = v;
    memcpy(p + 4, c, 4 * sizeof(float));
}

static void quad(LauncherUi *ui, const float x0, const float y0, const float x1, const float y1, const float u0,
                 const float v0, const float u1, const float v1, const float *c) {
    vert(ui, x0, y0, u0, v0, c);
    vert(ui, x1, y0, u1, v0, c);
    vert(ui, x1, y1, u1, v1, c);
    vert(ui, x0, y0, u0, v0, c);
    vert(ui, x1, y1, u1, v1, c);
    vert(ui, x0, y1, u0, v1, c);
}

static void rect(LauncherUi *ui, const float x, const float y, const float w, const float h, const float *c) {
    const float u = 2.0f / ATLAS_W, v = 2.0f / ATLAS_H;
    quad(ui, x, y, x + w, y + h, u, v, u, v, c);
}

static float text_width(const LauncherUi *ui, const int font, const char *s) {
    const FontFace *f = &ui->fonts[font];
    float w = 0;
    for (; *s; s++) {
        const unsigned c = (unsigned char) *s;
        if (c >= 32 && c < 128 && f->g[c - 32].ok) w += (float) f->g[c - 32].adv / ui->scale;
    }
    return w;
}

// baseline at y; returns the advance
static float text(LauncherUi *ui, const int font, float x, const float y, const char *s, const float *c) {
    if (!ui->has_text) return 0;
    const FontFace *f = &ui->fonts[font];
    const float x_start = x;
    for (; *s; s++) {
        const unsigned ch = (unsigned char) *s;
        if (ch < 32 || ch >= 128 || !f->g[ch - 32].ok) continue;
        const Glyph *g = &f->g[ch - 32];
        const float gx = x + (float) g->left / ui->scale, gy = y - (float) g->top / ui->scale;
        if (g->w && g->h) quad(ui, gx, gy, gx + (float) g->w / ui->scale, gy + (float) g->h / ui->scale, g->x0, g->y0, g->x1, g->y1, c);
        x += (float) g->adv / ui->scale;
    }
    return x - x_start;
}

// Draws s word-wrapped to max_w (at most max_lines lines, the last one cut with "..."); returns
// the number of lines.
static int text_wrapped(LauncherUi *ui, const int font, const float x, float y, const float max_w, const int max_lines,
                        const char *s, const float *c) {
    const float line_h = (float) ui->fonts[font].line / ui->scale + 2;
    int lines = 0;
    char buf[256];
    while (*s && lines < max_lines) {
        size_t fit = 0, last_space = 0;
        for (size_t i = 0; s[i] && i + 1 < sizeof(buf); i++) {
            memcpy(buf, s, i + 1);
            buf[i + 1] = 0;
            if (text_width(ui, font, buf) > max_w) break;
            fit = i + 1;
            if (s[i] == ' ') last_space = i;
        }
        if (!fit) break;
        const int last = lines + 1 == max_lines;
        size_t take = fit;
        if (s[fit] && !last && last_space) take = last_space;
        memcpy(buf, s, take);
        buf[take] = 0;
        if (s[take] && last && take > 3) memcpy(buf + take - 3, "...", 4);
        text(ui, font, x, y, buf, c);
        s += take;
        while (*s == ' ') s++;
        y += line_h;
        lines++;
    }
    return lines;
}

// ------------------------------------------------------------------ window

// The client's window icon: the install's assets/img/icon.png next to ui/ (every package ships
// it; krunker.io's 256 px app icon, tools/branding/fetch_branding.py). macOS windows have no
// icon (the .app's .icns is the Dock's); Windows also has it in the exe (client/win.rc).
static void set_window_icon(GLFWwindow *window, const char *ui_dir) {
    char path[4096];
    snprintf(path, sizeof(path), "%s/../assets/img/icon.png", ui_dir ? ui_dir : ".");
    GLFWimage icon = {0};
    int channels = 0;
    icon.pixels = stbi_load(path, &icon.width, &icon.height, &channels, 4);
    if (icon.pixels) {
#ifndef __APPLE__
        glfwSetWindowIcon(window, 1, &icon);
#endif
        stbi_image_free(icon.pixels);
    }
    printf("[launcher] window \"%s\", icon %s (%dx%d)\n", glfwGetWindowTitle(window), icon.pixels ? path : "none", icon.width,
           icon.height);
}

LauncherUi *launcher_ui_open(const char *ui_dir) {
    int argc = 1;
    char *argv[] = {"krunknative", NULL};
    headless_parse_args(&argc, argv); // KRUNKNATIVE_HEADLESS=1: EGL pbuffer (tests, captures)
    if (!headless_glfw_init()) return NULL;
#if defined(KRUNKNATIVE_GL_ANGLE)
    glfwWindowHint(GLFW_CLIENT_API, GLFW_OPENGL_ES_API);
    glfwWindowHint(GLFW_CONTEXT_CREATION_API, GLFW_EGL_CONTEXT_API);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 3);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);
#elif defined(__APPLE__)
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 1);
    glfwWindowHint(GLFW_OPENGL_FORWARD_COMPAT, GLFW_TRUE);
#else
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
#endif
    glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
    glfwWindowHint(GLFW_SCALE_TO_MONITOR, GLFW_TRUE);
    headless_window_hints();
    LauncherUi *ui = calloc(1, sizeof(*ui));
    if (!ui) return NULL;
    ui->window = glfwCreateWindow(WIN_W, WIN_H, KN_PRODUCT_NAME, NULL, NULL);
    if (!ui->window || !headless_make_context_current(ui->window) || !kn_gl_load(headless_get_proc_address)) {
        if (ui->window) glfwDestroyWindow(ui->window);
        free(ui);
        glfwTerminate();
        return NULL;
    }
    set_window_icon(ui->window, ui_dir);
    int fw = WIN_W, fh = WIN_H, ww = WIN_W, wh = WIN_H;
    glfwGetFramebufferSize(ui->window, &fw, &fh);
    glfwGetWindowSize(ui->window, &ww, &wh);
    ui->scale = ww > 0 ? (float) fw / (float) ww : 1.0f;
    if (ui->scale < 1.0f) ui->scale = 1.0f;
    headless_swap_interval(1);

    const GLuint vs = compile(GL_VERTEX_SHADER, VS), fs = compile(GL_FRAGMENT_SHADER, FS);
    ui->prog = glCreateProgram();
    glAttachShader(ui->prog, vs);
    glAttachShader(ui->prog, fs);
    glBindAttribLocation(ui->prog, 0, "a_pos");
    glBindAttribLocation(ui->prog, 1, "a_uv");
    glBindAttribLocation(ui->prog, 2, "a_col");
    glLinkProgram(ui->prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    ui->u_size = glGetUniformLocation(ui->prog, "u_size");
    glGenVertexArrays(1, &ui->vao);
    glGenBuffers(1, &ui->vbo);
    glBindVertexArray(ui->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui->vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(ui->verts), NULL, GL_DYNAMIC_DRAW);
    glEnableVertexAttribArray(0);
    glEnableVertexAttribArray(1);
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *) 0);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *) (2 * sizeof(float)));
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, 8 * sizeof(float), (void *) (4 * sizeof(float)));
    load_fonts(ui, ui_dir);
    ui->t = glfwGetTime();
    return ui;
}

static int button(LauncherUi *ui, const float x, const float y, const float w, const float h, const char *label,
                  const int primary, const double mx, const double my, const int clicked) {
    const int hover = mx >= x && mx < x + w && my >= y && my < y + h;
    static const float yellow[4] = {0.984f, 0.753f, 0.176f, 1}, yellow_hi[4] = {1, 0.84f, 0.35f, 1};
    static const float grey[4] = {0.27f, 0.27f, 0.27f, 1}, grey_hi[4] = {0.36f, 0.36f, 0.36f, 1};
    static const float dark[4] = {0.1f, 0.1f, 0.1f, 1}, white[4] = {1, 1, 1, 1};
    rect(ui, x, y, w, h, primary ? (hover ? yellow_hi : yellow) : (hover ? grey_hi : grey));
    const float tw = text_width(ui, FONT_BODY, label);
    text(ui, FONT_BODY, x + (w - tw) / 2, y + h / 2 + 6, label, primary ? dark : white);
    return hover && clicked;
}

LauncherAction launcher_ui_frame(LauncherUi *ui, const LauncherView *v) {
    if (!ui) return UI_NONE;
    glfwPollEvents();
    if (glfwWindowShouldClose(ui->window)) return UI_QUIT;
    // the window title mirrors the status: the only text when the fonts are missing
    char title[256];
    if (v->progress >= 0 && !v->error) snprintf(title, sizeof(title), KN_PRODUCT_NAME " - %s (%d%%)", v->status, (int) (v->progress * 100));
    else snprintf(title, sizeof(title), KN_PRODUCT_NAME " - %s", v->status);
    if (strcmp(title, ui->title) != 0) {
        snprintf(ui->title, sizeof(ui->title), "%s", title);
        glfwSetWindowTitle(ui->window, title);
    }
    double mx = -1, my = -1;
    glfwGetCursorPos(ui->window, &mx, &my);
    const int down = glfwGetMouseButton(ui->window, GLFW_MOUSE_BUTTON_LEFT) == GLFW_PRESS;
    const int clicked = ui->mouse_was_down && !down; // on release
    ui->mouse_was_down = down;

    static const float bg_top[4] = {0.13f, 0.13f, 0.14f, 1}, accent[4] = {0.984f, 0.753f, 0.176f, 1};
    static const float white[4] = {1, 1, 1, 1}, grey[4] = {0.67f, 0.67f, 0.67f, 1}, red[4] = {0.93f, 0.26f, 0.26f, 1};
    static const float track[4] = {0.24f, 0.24f, 0.26f, 1};
    ui->nverts = 0;
    rect(ui, 0, 0, WIN_W, 4, accent);
    text(ui, FONT_TITLE, 24, 48, "KRUNKER BOX OFFICE", white);
    LauncherAction action = UI_NONE;
    if (v->error) {
        const int lines = ui->has_text ? text_wrapped(ui, FONT_BODY, 24, 88, WIN_W - 48, 2, v->status, red) : 1;
        if (v->detail[0]) text_wrapped(ui, FONT_SMALL, 24, 88 + (float) lines * 24 + 2, WIN_W - 48, 1, v->detail, grey);
        float x = WIN_W - 24 - 96;
        if (button(ui, x, WIN_H - 60, 96, 36, "Quit", 0, mx, my, clicked)) action = UI_QUIT;
        x -= 96 + 10;
        if (v->can_play && button(ui, x, WIN_H - 60, 96, 36, "Play", 0, mx, my, clicked)) action = UI_PLAY;
        if (v->can_play) x -= 96 + 10;
        if (button(ui, x, WIN_H - 60, 96, 36, "Retry", 1, mx, my, clicked)) action = UI_RETRY;
    } else {
        text_wrapped(ui, FONT_BODY, 24, 92, WIN_W - 48, 1, v->status, white);
        const float bx = 24, by = 108, bw = WIN_W - 48, bh = 12;
        rect(ui, bx, by, bw, bh, track);
        if (v->progress >= 0) {
            const float p = v->progress > 1 ? 1 : (float) v->progress;
            rect(ui, bx, by, bw * p, bh, accent);
        } else { // indeterminate: a segment sweeping across
            const float t = (float) fmod(glfwGetTime() - ui->t, 1.6) / 1.6f;
            const float seg = bw * 0.25f, x0 = bx + (bw + seg) * t - seg;
            const float a = x0 < bx ? bx : x0, b = x0 + seg > bx + bw ? bx + bw : x0 + seg;
            if (b > a) rect(ui, a, by, b - a, bh, accent);
        }
        if (v->detail[0]) text_wrapped(ui, FONT_SMALL, 24, 146, WIN_W - 48, 2, v->detail, grey);
    }
    (void) bg_top;
    int fw = WIN_W, fh = WIN_H;
    glfwGetFramebufferSize(ui->window, &fw, &fh);
    if (headless_enabled()) fw = (int) (WIN_W * ui->scale), fh = (int) (WIN_H * ui->scale);
    glViewport(0, 0, fw, fh);
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.11f, 0.11f, 0.12f, 1);
    glClear(GL_COLOR_BUFFER_BIT);
    glUseProgram(ui->prog);
    glUniform2f(ui->u_size, (float) fw, (float) fh);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, ui->tex);
    glBindVertexArray(ui->vao);
    glBindBuffer(GL_ARRAY_BUFFER, ui->vbo);
    glBufferSubData(GL_ARRAY_BUFFER, 0, (GLsizeiptr) ((size_t) ui->nverts * 8 * sizeof(float)), ui->verts);
    glDrawArrays(GL_TRIANGLES, 0, ui->nverts);

    // KRUNKNATIVE_SCREENSHOT=<png>: capture the window after a few frames (tests, docs)
    const char *shot = getenv("KRUNKNATIVE_SCREENSHOT");
    if (shot && *shot && ++ui->frames == 8) {
        unsigned char *px = malloc((size_t) fw * (size_t) fh * 4), *flip = malloc((size_t) fw * (size_t) fh * 4);
        if (px && flip) {
            glReadPixels(0, 0, fw, fh, GL_RGBA, GL_UNSIGNED_BYTE, px);
            for (int y = 0; y < fh; y++) memcpy(flip + (size_t) y * fw * 4, px + (size_t) (fh - 1 - y) * fw * 4, (size_t) fw * 4);
            png_write_rgba(shot, flip, fw, fh);
        }
        free(px);
        free(flip);
    }
    headless_swap_buffers(ui->window);
    return action;
}

void launcher_ui_hide(LauncherUi *ui) {
    if (ui && ui->window) glfwHideWindow(ui->window);
}

void launcher_ui_close(LauncherUi *ui) {
    if (!ui) return;
    glDeleteTextures(1, &ui->tex);
    glDeleteBuffers(1, &ui->vbo);
    glDeleteVertexArrays(1, &ui->vao);
    glDeleteProgram(ui->prog);
    glfwDestroyWindow(ui->window);
    headless_shutdown();
    glfwTerminate();
    free(ui);
}
