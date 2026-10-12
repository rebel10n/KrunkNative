// Whole-frame offscreen target: the web build draws each frame offscreen and copies it to the
// canvas once, so a mid-frame ASYNCIFY yield (a spray texture or a joining player's assets
// being fetched) never lets the browser composite a half-drawn frame. See frame_target.h.
#include <gl.h>
#include <GLFW/glfw3.h>
#include <frame_target.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct {
    int configured;
    int enabled;                        // web: on; native: KRUNKNATIVE_FRAME_TARGET=1
    int active;                         // between begin() and present()
    unsigned int fbo, color_rb, depth_rb;
    int width, height;                  // allocated size

    // KRUNKNATIVE_YIELD_CHECK: native emulation of the browser compositing at a yield
    int check;
    Client *client;
    int game_frames;                    // frames presented in a running game, in a row
    int yields, bad;
} ft;

static void ft_delete(void) {
    if (ft.fbo) glDeleteFramebuffers(1, &ft.fbo);
    if (ft.color_rb) glDeleteRenderbuffers(1, &ft.color_rb);
    if (ft.depth_rb) glDeleteRenderbuffers(1, &ft.depth_rb);
    ft.fbo = ft.color_rb = ft.depth_rb = 0;
    ft.width = ft.height = 0;
}

static int ft_allocate(const int width, const int height) {
    if (ft.fbo && ft.width == width && ft.height == height) return 1;
    ft_delete();

    GLint previous_rb = 0;
    glGetIntegerv(GL_RENDERBUFFER_BINDING, &previous_rb);
    glGenRenderbuffers(1, &ft.color_rb);
    glBindRenderbuffer(GL_RENDERBUFFER, ft.color_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_RGBA8, width, height);
    glGenRenderbuffers(1, &ft.depth_rb); // the default framebuffer's depth 24 + stencil 8 (GLFW defaults)
    glBindRenderbuffer(GL_RENDERBUFFER, ft.depth_rb);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, width, height);
    glBindRenderbuffer(GL_RENDERBUFFER, (GLuint) previous_rb);

    glGenFramebuffers(1, &ft.fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, ft.fbo);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_RENDERBUFFER, ft.color_rb);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, ft.depth_rb);
    const GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "frame_target: %dx%d framebuffer incomplete (0x%x), drawing to the window\n", width, height, status);
        ft_delete();
        return 0;
    }
    ft.width = width;
    ft.height = height;
    return 1;
}

// What a browser would composite if it got control now: framebuffer 0. "No world" when the
// middle of the picture is (nearly) all the sky clear colour or flat.
static void ft_yield_check(const char *path) {
    Client *client = ft.client;
    if (!client || !client->window || ft.game_frames < 5) return; // a running game, already shown a few times

    int width = 0, height = 0;
    glfwGetFramebufferSize(client->window, &width, &height);
    const int x0 = width / 10, y0 = height / 4, w = width * 8 / 10, h = height / 2;
    if (w <= 0 || h <= 0) return;
    unsigned char *px = malloc((size_t) w * (size_t) h * 4);
    if (!px) return;

    GLint read_fbo = 0, pack = 4;
    glGetIntegerv(GL_READ_FRAMEBUFFER_BINDING, &read_fbo);
    glGetIntegerv(GL_PACK_ALIGNMENT, &pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glReadPixels(x0, y0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px);
    glPixelStorei(GL_PACK_ALIGNMENT, pack);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, (GLuint) read_fbo);

    const int sky[3] = {
        (int) lroundf(g_render_lighting.sky_color.x * 255.0f),
        (int) lroundf(g_render_lighting.sky_color.y * 255.0f),
        (int) lroundf(g_render_lighting.sky_color.z * 255.0f),
    };
    size_t n = 0, clear = 0;
    double sum = 0.0, sum_sq = 0.0;
    for (int y = 0; y < h; y += 4) {
        for (int x = 0; x < w; x += 4) {
            const unsigned char *p = px + ((size_t) y * w + x) * 4;
            if (abs(p[0] - sky[0]) <= 3 && abs(p[1] - sky[1]) <= 3 && abs(p[2] - sky[2]) <= 3) clear++;
            const double luma = 0.299 * p[0] + 0.587 * p[1] + 0.114 * p[2];
            sum += luma;
            sum_sq += luma * luma;
            n++;
        }
    }
    free(px);
    if (!n) return;

    const double mean = sum / (double) n;
    const double variance = sum_sq / (double) n - mean * mean;
    const double clear_share = (double) clear / (double) n;
    const int missing = clear_share > 0.9 || variance < 4.0;
    ft.yields++;
    if (missing) ft.bad++;
    fprintf(stderr, "[yield-check] %s %s (sky clear %.0f%%, variance %.0f)\n", missing ? "NO WORLD" : "ok", path,
            clear_share * 100.0, variance);
}

static void ft_configure(Client *client) {
    if (ft.configured) return;
    ft.configured = 1;
#ifdef __EMSCRIPTEN__
    ft.enabled = 1; // the browser composites the canvas at every ASYNCIFY yield
#else
    const char *env = getenv("KRUNKNATIVE_FRAME_TARGET");
    ft.enabled = env && *env && strcmp(env, "0") != 0;
#endif
    const char *check = getenv("KRUNKNATIVE_YIELD_CHECK");
    ft.check = check && *check && strcmp(check, "0") != 0;
    if (ft.check) {
        ft.client = client;
        asset_set_yield_hook(ft_yield_check);
        fprintf(stderr, "[yield-check] on (frame target %s)\n", ft.enabled ? "on" : "off");
    }
}

void frame_target_begin(Client *client) {
    ft_configure(client);
    ft.active = 0;
    if (!ft.enabled || !client || !client->window) return;

    int width = 0, height = 0;
    glfwGetFramebufferSize(client->window, &width, &height);
    if (width <= 0 || height <= 0 || !ft_allocate(width, height)) return;

    glBindFramebuffer(GL_FRAMEBUFFER, ft.fbo);
    ft.active = 1;
}

void frame_target_present(Client *client) {
    if (ft.check) {
        const int in_game = client && client->game.ready && client->me && client->me->active;
        ft.game_frames = in_game ? ft.game_frames + 1 : 0;
    }
    if (!ft.active) return;
    ft.active = 0;

    // glBlitFramebuffer obeys the scissor test (not the colour/depth masks)
    const GLboolean scissor = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor) glDisable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_READ_FRAMEBUFFER, ft.fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glBlitFramebuffer(0, 0, ft.width, ft.height, 0, 0, ft.width, ft.height, GL_COLOR_BUFFER_BIT, GL_NEAREST);
    // until the next begin(): anything drawn in between (event callbacks) stays off the canvas too
    glBindFramebuffer(GL_FRAMEBUFFER, ft.fbo);
    if (scissor) glEnable(GL_SCISSOR_TEST);
}

unsigned int frame_target_framebuffer(void) {
    return ft.active ? ft.fbo : 0;
}

void frame_target_fini(void) {
    if (ft.check) {
        fprintf(stderr, "[yield-check] %d asset yields in a running game, %d would have shown a frame without the world\n", ft.yields, ft.bad);
        asset_set_yield_hook(NULL);
        ft.check = 0;
    }
    ft_delete();
    ft.active = 0;
    ft.configured = 0;
}
