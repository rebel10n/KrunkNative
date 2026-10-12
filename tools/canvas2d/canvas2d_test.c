// canvas2d_test: replays a GAME.OVERLAY scene (tools/canvas2d/scenes/*.txt) through
// ks_overlay_call() on Canvas2D into an offscreen framebuffer and writes a PNG. run.sh renders
// the same scene with headless Chromium's Canvas 2D (ref.js: the v7 OVERLAY code) and diffs.
//
//   canvas2d_test [--headless] [--bench N] <scene.txt> <out.png>    (from the repo root)
//
// Scene grammar: ks_overlay.h (call scripts). Calls with a return value print
// "<line>: <method> -> <value>" on stdout.
// Exit status: 0 ok, 1 usage/setup error, 2 GL state was not restored by the canvas.
#include <gl.h>
#include <GLFW/glfw3.h>
#include <canvas2d.h>
#include <headless.h>
#include <ks_overlay.h>
#include <png_write.h>
#include <shared.h>

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

unsigned int g_active_texture; // client.h globals used by shader.c's callers
unsigned int g_active_shader;

typedef struct {
    int fbo, viewport[4], program, vao, tex, blend, src, dst, stencil, depth;
} StateProbe;

static void probe(StateProbe *p) {
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &p->fbo);
    glGetIntegerv(GL_VIEWPORT, p->viewport);
    glGetIntegerv(GL_CURRENT_PROGRAM, &p->program);
    glGetIntegerv(GL_VERTEX_ARRAY_BINDING, &p->vao);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &p->tex);
    p->blend = glIsEnabled(GL_BLEND);
    glGetIntegerv(GL_BLEND_SRC_RGB, &p->src);
    glGetIntegerv(GL_BLEND_DST_RGB, &p->dst);
    p->stencil = glIsEnabled(GL_STENCIL_TEST);
    p->depth = glIsEnabled(GL_DEPTH_TEST);
}

static double now_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6;
}

int main(int argc, char **argv) {
    headless_parse_args(&argc, argv);
    int bench = 0;
    const char *scene = NULL, *out = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--bench") && i + 1 < argc) bench = atoi(argv[++i]);
        else if (!scene) scene = argv[i];
        else if (!out) out = argv[i];
    }
    if (!scene || !out) {
        fprintf(stderr, "usage: %s [--headless] [--bench N] <scene.txt> <out.png>\n", argv[0]);
        return 1;
    }
    char *probe_path = concat(assets_path() ? assets_path() : "", "shaders/canvas2d.vert");
    FILE *probe_file = probe_path ? fopen(probe_path, "rb") : NULL;
    if (!probe_file) {
        fprintf(stderr, "canvas2d_test: %s not found: run from the repo root (assets_path() is ./assets/)\n", probe_path ? probe_path : "assets");
        return 1;
    }
    fclose(probe_file);
    free(probe_path);
    KsOverlayScene *sc = ks_overlay_scene_load(scene);
    if (!sc) {
        fprintf(stderr, "canvas2d_test: cannot read %s\n", scene);
        return 1;
    }
    const KsOverlaySceneInfo *S = ks_overlay_scene_info(sc);
    const int dw = (int) lround(S->width * S->dpr), dh = (int) lround(S->height * S->dpr);

    if (!headless_glfw_init()) {
        fprintf(stderr, "canvas2d_test: glfwInit failed (use --headless / KRUNKNATIVE_HEADLESS=1)\n");
        return 1;
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
    GLFWwindow *window = glfwCreateWindow(64, 64, "canvas2d_test", NULL, NULL);
    if (!window || !headless_make_context_current(window) || !kn_gl_load(headless_get_proc_address)) {
        fprintf(stderr, "canvas2d_test: no GL context\n");
        return 1;
    }
    fprintf(stderr, "canvas2d_test: GL_RENDERER %s\n", (const char *) glGetString(GL_RENDERER));

    // the target: an RGBA8 framebuffer like the window (no stencil: the canvas brings its own)
    unsigned int fbo, color;
    glGenTextures(1, &color);
    glBindTexture(GL_TEXTURE_2D, color);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, dw, dh, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, color, 0);
    glViewport(0, 0, dw, dh);
    glEnable(GL_BLEND); // the client's global blend state (main.c)
    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glEnable(GL_DEPTH_TEST);

    ks_overlay_scene_use_images(sc);
    // load images up front (the web loads them in the background: v7 skips until isReady)
    for (int i = 0; i < S->image_count; i++) c2d_image(ks_overlay_canvas(), S->image_path[i]);

    StateProbe before, after;
    glClearColor(S->background[0], S->background[1], S->background[2], 1);
    glClear(GL_COLOR_BUFFER_BIT);
    probe(&before);
    ks_overlay_begin_frame(S->width, S->height, dw, dh, S->scale);
    ks_overlay_scene_replay(sc, 0, stdout);
    ks_overlay_composite();
    probe(&after);
    int cmds, verts, draws;
    c2d_last_flush_stats(ks_overlay_canvas(), &cmds, &verts, &draws);
    fprintf(stderr, "canvas2d_test: %d calls -> %d commands, %d vertices, %d draw calls\n", ks_overlay_scene_call_count(sc), cmds, verts, draws);
    int state_ok = !memcmp(&before, &after, sizeof(before));
    if (!state_ok) fprintf(stderr, "canvas2d_test: GL state changed by the canvas\n");

    unsigned char *px = malloc((size_t) dw * dh * 4), *flip = malloc((size_t) dw * dh * 4);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(0, 0, dw, dh, GL_RGBA, GL_UNSIGNED_BYTE, px);
    for (int y = 0; y < dh; y++) memcpy(flip + (size_t) y * dw * 4, px + (size_t) (dh - 1 - y) * dw * 4, (size_t) dw * 4);
    for (size_t i = 0; i < (size_t) dw * dh; i++) flip[i * 4 + 3] = 255;
    const int written = png_write_rgba(out, flip, dw, dh);

    if (bench > 0) {
        glFinish();
        const double t0 = now_ms();
        for (int i = 0; i < bench; i++) {
            glClear(GL_COLOR_BUFFER_BIT);
            ks_overlay_begin_frame(S->width, S->height, dw, dh, S->scale);
            ks_overlay_scene_replay(sc, 0, NULL);
            ks_overlay_composite();
        }
        glFinish();
        const double t1 = now_ms();
        fprintf(stderr, "canvas2d_test: bench %d frames, %.3f ms/frame (record + flush + composite, GPU finished)\n",
                bench, (t1 - t0) / bench);
    }

    free(px);
    free(flip);
    ks_overlay_shutdown();
    ks_overlay_scene_free(sc);
    glDeleteFramebuffers(1, &fbo);
    glDeleteTextures(1, &color);
    headless_shutdown();
    glfwTerminate();
    if (!written) return 1;
    return state_ok ? 0 : 2;
}
