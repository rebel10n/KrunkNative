// Camera-matched world captures (see client/include/debug_capture.h and
// doc/ref/krunker-v3.1.1/world/README.md). Driven only by environment variables so the
// normal client is untouched when they are absent.
#include <gl.h>
#include <GLFW/glfw3.h>
#include <debug_capture.h>
#include <png_write.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct {
    int initialized;
    int pinned;          // KRUNKNATIVE_CAMERA parsed
    vec3 position;
    float pitch;
    float yaw;
    float fov;           // radians, 0 = keep the client's FOV
    int hide_ui;
    const char *screenshot;
    int frames_wanted;
    int frames_drawn;
    int done;
} DebugCapture;

static DebugCapture g_capture;

void debug_capture_startup(void) {
    const char *camera = getenv("KRUNKNATIVE_CAMERA");
    const char *screenshot = getenv("KRUNKNATIVE_SCREENSHOT");
    if (!(camera && *camera) && !(screenshot && *screenshot)) return;

    // A capture must reproduce its pose with v3.1.1 defaults: the user's stored settings
    // (fov, aspectRatio, resolution, lighting, ...) live under $XDG_CONFIG_HOME/krunknative,
    // so unless the caller chose a config dir (setting runs do) use an empty private one.
    const char *xdg = getenv("XDG_CONFIG_HOME");
    if (xdg && *xdg) return;

#ifdef _WIN32
    _putenv_s("XDG_CONFIG_HOME", "krunknative-capture-config");
#else
    char dir[] = "/tmp/krunknative-capture-XXXXXX";
    if (mkdtemp(dir)) setenv("XDG_CONFIG_HOME", dir, 1);
#endif
}

static void debug_capture_init(void) {
    if (g_capture.initialized) return;
    g_capture.initialized = 1;

    const char *camera = getenv("KRUNKNATIVE_CAMERA");
    if (camera && *camera) {
        float x, y, z, pitch, yaw;

        if (sscanf(camera, "%f,%f,%f,%f,%f", &x, &y, &z, &pitch, &yaw) == 5) {
            g_capture.pinned = 1;
            g_capture.position = (vec3) {x, y, z};
            g_capture.pitch = pitch;
            g_capture.yaw = yaw;
        } else {
            fprintf(stderr, "KRUNKNATIVE_CAMERA: expected \"x,y,z,pitch,yaw\", got \"%s\"\n", camera);
        }
    }

    const char *fov = getenv("KRUNKNATIVE_FOV");
    if (fov && *fov) g_capture.fov = (float) atof(fov) * (float) M_PI / 180.0f;

    const char *hide_ui = getenv("KRUNKNATIVE_HIDE_UI");
    g_capture.hide_ui = hide_ui && !strcmp(hide_ui, "1");

    const char *screenshot = getenv("KRUNKNATIVE_SCREENSHOT");
    if (screenshot && *screenshot) g_capture.screenshot = screenshot;

    const char *frames = getenv("KRUNKNATIVE_CAPTURE_FRAMES");
    g_capture.frames_wanted = frames && *frames ? atoi(frames) : 30;
    if (g_capture.frames_wanted < 1) g_capture.frames_wanted = 1;
}

void debug_capture_apply_camera(Client *client) {
    debug_capture_init();

    if (g_capture.fov > 0.0f) client->camera.fov = g_capture.fov;
    if (!g_capture.pinned) return;

    client->camera.position = g_capture.position;
    client->camera.rotation.x = g_capture.pitch;
    client->camera.rotation.y = g_capture.yaw;
    client->camera.rotation.z = 0.0f;
    client->camera.zoom = 1.0f;
}

int debug_capture_hide_ui(void) {
    debug_capture_init();
    return g_capture.hide_ui;
}

static void debug_capture_write(Client *client) {
    int width, height;
    glfwGetFramebufferSize(client->window, &width, &height);
    if (width <= 0 || height <= 0) return;

    unsigned char *pixels = malloc((size_t) width * (size_t) height * 4);
    unsigned char *flipped = malloc((size_t) width * (size_t) height * 4);

    if (pixels && flipped) {
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        glPixelStorei(GL_PACK_ALIGNMENT, 1);
        glReadPixels(0, 0, width, height, GL_RGBA, GL_UNSIGNED_BYTE, pixels);

        const size_t row = (size_t) width * 4;
        for (int y = 0; y < height; y++) {
            memcpy(flipped + (size_t) y * row, pixels + (size_t) (height - 1 - y) * row, row);
        }

        // the default framebuffer may carry translucent alpha from blended draws
        for (size_t i = 3; i < row * (size_t) height; i += 4) flipped[i] = 255;

        if (png_write_rgba(g_capture.screenshot, flipped, width, height)) {
            fprintf(stderr, "debug capture: wrote %s (%dx%d)\n", g_capture.screenshot, width, height);
        }
    }

    free(pixels);
    free(flipped);
}

void debug_capture_frame_end(Client *client) {
    debug_capture_init();
    if (!g_capture.screenshot || g_capture.done) return;
    if (!client->game.ready || !client->map_loaded) return;

    if (++g_capture.frames_drawn < g_capture.frames_wanted) return;

    debug_capture_write(client);
    g_capture.done = 1;
    glfwSetWindowShouldClose(client->window, GLFW_TRUE);
}
