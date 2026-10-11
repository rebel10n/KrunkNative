// Headless GPU rendering through EGL, without a display server (client/include/headless.h).
#include <gl.h>
#include <headless.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_headless;

int headless_parse_args(int *argc, char **argv) {
    const char *env = getenv("KRUNKNATIVE_HEADLESS");
    if (env && *env && strcmp(env, "0") != 0) g_headless = 1;

    if (*argc < 1) return g_headless;
    int out = 1;
    for (int i = 1; i < *argc; i++) {
        if (!strcmp(argv[i], "--headless")) {
            g_headless = 1;
            continue;
        }
        argv[out++] = argv[i];
    }
    if (out < *argc) argv[out] = NULL;
    *argc = out;
    return g_headless;
}

int headless_enabled(void) {
    return g_headless;
}

#if defined(KRUNKNATIVE_HEADLESS_EGL) && !defined(__EMSCRIPTEN__)

#define EGL_NO_X11 // no Xlib types in eglplatform.h: headless never touches X
#include <EGL/egl.h>
#include <EGL/eglext.h>

#define HEADLESS_MAX_DEVICES 16

typedef struct {
    EGLDeviceEXT handle;
    const char *extensions;
    const char *drm_path; // "" when the device has no DRM node (llvmpipe)
    int nvidia;           // NVIDIA's own EGL device (EGL_NV_device_cuda)
    int software;         // Mesa's llvmpipe device (EGL_MESA_device_software)
} HeadlessDevice;

static struct {
    EGLDisplay display;
    EGLConfig config;
    EGLContext context;
    EGLSurface surface;
    GLFWwindow *window;
    int width, height;
} g_egl = {EGL_NO_DISPLAY, NULL, EGL_NO_CONTEXT, EGL_NO_SURFACE, NULL, 0, 0};

static PFNEGLQUERYDEVICESEXTPROC p_eglQueryDevicesEXT;
static PFNEGLQUERYDEVICESTRINGEXTPROC p_eglQueryDeviceStringEXT;
static PFNEGLGETPLATFORMDISPLAYEXTPROC p_eglGetPlatformDisplayEXT;

static int has_extension(const char *list, const char *name) {
    const size_t length = strlen(name);
    for (const char *p = list; p && (p = strstr(p, name)); p += length) {
        if ((p == list || p[-1] == ' ') && (p[length] == ' ' || p[length] == 0)) return 1;
    }
    return 0;
}

static int enumerate_devices(HeadlessDevice *devices) {
    const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
    if (!has_extension(client, "EGL_EXT_device_enumeration") || !has_extension(client, "EGL_EXT_platform_device")) return 0;

    p_eglQueryDevicesEXT = (PFNEGLQUERYDEVICESEXTPROC) eglGetProcAddress("eglQueryDevicesEXT");
    p_eglQueryDeviceStringEXT = (PFNEGLQUERYDEVICESTRINGEXTPROC) eglGetProcAddress("eglQueryDeviceStringEXT");
    if (!p_eglQueryDevicesEXT || !p_eglQueryDeviceStringEXT) return 0;

    EGLDeviceEXT handles[HEADLESS_MAX_DEVICES];
    EGLint count = 0;
    if (!p_eglQueryDevicesEXT(HEADLESS_MAX_DEVICES, handles, &count)) return 0;

    for (EGLint i = 0; i < count; i++) {
        HeadlessDevice *d = &devices[i];
        const char *extensions = p_eglQueryDeviceStringEXT(handles[i], EGL_EXTENSIONS);
        const char *drm = has_extension(extensions, "EGL_EXT_device_drm") ? p_eglQueryDeviceStringEXT(handles[i], EGL_DRM_DEVICE_FILE_EXT) : NULL;

        d->handle = handles[i];
        d->extensions = extensions ? extensions : "";
        d->drm_path = drm ? drm : "";
        d->nvidia = has_extension(extensions, "EGL_NV_device_cuda");
        d->software = has_extension(extensions, "EGL_MESA_device_software");
    }
    return (int) count;
}

// Mesa also enumerates the DRM node NVIDIA's driver owns; it cannot drive it (and logs
// "failed to create dri2 screen"), so "mesa" skips nodes an NVIDIA device already claims.
static int claimed_by_nvidia(const HeadlessDevice *devices, const int count, const HeadlessDevice *d) {
    if (d->nvidia || !d->drm_path[0]) return 0;
    for (int i = 0; i < count; i++) {
        if (devices[i].nvidia && !strcmp(devices[i].drm_path, d->drm_path)) return 1;
    }
    return 0;
}

static const char *device_kind(const HeadlessDevice *d) {
    return d->nvidia ? "nvidia" : d->software ? "mesa llvmpipe" : "mesa";
}

static EGLDisplay open_device(const HeadlessDevice *d) {
    EGLDisplay display = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_DEVICE_EXT, d->handle, NULL);
    if (display == EGL_NO_DISPLAY) return EGL_NO_DISPLAY;
    if (!eglInitialize(display, NULL, NULL)) return EGL_NO_DISPLAY;
    return display;
}

// Fills `order` with the device indices to try for KRUNKNATIVE_EGL_DEVICE (`want`).
static int candidate_order(const char *want, const HeadlessDevice *devices, const int count, int *order) {
    int n = 0;
    if (want[0] >= '0' && want[0] <= '9') {
        const int index = atoi(want);
        if (index < count) order[n++] = index;
        return n;
    }

    const int nvidia = !want[0] || !strcmp(want, "nvidia");
    const int mesa = !want[0] || !strcmp(want, "mesa");
    if (nvidia) {
        for (int i = 0; i < count; i++) if (devices[i].nvidia) order[n++] = i;
    }
    if (mesa) {
        for (int i = 0; i < count; i++) {
            if (!devices[i].nvidia && !devices[i].software && !claimed_by_nvidia(devices, count, &devices[i])) order[n++] = i;
        }
        for (int i = 0; i < count; i++) if (devices[i].software) order[n++] = i;
    }
    return n;
}

static int open_display(void) {
    const char *want = getenv("KRUNKNATIVE_EGL_DEVICE");
    if (!want) want = "";
    if (want[0] && strcmp(want, "nvidia") != 0 && strcmp(want, "mesa") != 0 && !(want[0] >= '0' && want[0] <= '9')) {
        fprintf(stderr, "[headless] KRUNKNATIVE_EGL_DEVICE=%s: expected nvidia, mesa or a device index\n", want);
        return 0;
    }

    p_eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!p_eglGetPlatformDisplayEXT) {
        fprintf(stderr, "[headless] libEGL has no eglGetPlatformDisplayEXT (EGL_EXT_platform_base)\n");
        return 0;
    }

    HeadlessDevice devices[HEADLESS_MAX_DEVICES];
    const int count = enumerate_devices(devices);
    for (int i = 0; i < count; i++) {
        fprintf(stderr, "[headless] EGL device %d: %s%s%s%s\n", i, device_kind(&devices[i]),
                devices[i].drm_path[0] ? " " : "", devices[i].drm_path,
                claimed_by_nvidia(devices, count, &devices[i]) ? " (NVIDIA's node: mesa cannot drive it)" : "");
    }

    int order[HEADLESS_MAX_DEVICES * 2];
    const int candidates = candidate_order(want, devices, count, order);
    for (int i = 0; i < candidates; i++) {
        const HeadlessDevice *d = &devices[order[i]];
        g_egl.display = open_device(d);
        if (g_egl.display != EGL_NO_DISPLAY) {
            fprintf(stderr, "[headless] using EGL device %d (%s)\n", order[i], device_kind(d));
            return 1;
        }
        fprintf(stderr, "[headless] EGL device %d (%s) failed to initialize (0x%x)\n", order[i], device_kind(d), eglGetError());
    }

    // No usable device (no EGL_EXT_device_enumeration, or none matched): Mesa's surfaceless
    // platform. Through libglvnd it may land on any vendor; GL_RENDERER below tells which.
    if (!want[0] || !strcmp(want, "mesa")) {
        const char *client = eglQueryString(EGL_NO_DISPLAY, EGL_EXTENSIONS);
        if (has_extension(client, "EGL_MESA_platform_surfaceless")) {
            g_egl.display = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, EGL_DEFAULT_DISPLAY, NULL);
            if (g_egl.display != EGL_NO_DISPLAY && eglInitialize(g_egl.display, NULL, NULL)) {
                fprintf(stderr, "[headless] using EGL_MESA_platform_surfaceless (%s)\n", eglQueryString(g_egl.display, EGL_VENDOR));
                return 1;
            }
            g_egl.display = EGL_NO_DISPLAY;
        }
    }

    fprintf(stderr, "[headless] no EGL device matches KRUNKNATIVE_EGL_DEVICE=%s (%d enumerated)\n", want[0] ? want : "(default)", count);
    return 0;
}

static EGLSurface create_pbuffer(const int width, const int height) {
    const EGLint attribs[] = {EGL_WIDTH, width > 0 ? width : 1, EGL_HEIGHT, height > 0 ? height : 1, EGL_NONE};
    return eglCreatePbufferSurface(g_egl.display, g_egl.config, attribs);
}

static int create_context(const int width, const int height) {
    if (!open_display()) return 0;

    if (!eglBindAPI(EGL_OPENGL_API)) {
        fprintf(stderr, "[headless] eglBindAPI(EGL_OPENGL_API) failed (0x%x)\n", eglGetError());
        return 0;
    }

    // What GLFW asks a window system for by default: RGBA8, depth 24, stencil 8, no MSAA.
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_BIT,
        EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };
    EGLint config_count = 0;
    if (!eglChooseConfig(g_egl.display, config_attribs, &g_egl.config, 1, &config_count) || config_count < 1) {
        fprintf(stderr, "[headless] no RGBA8/D24S8 pbuffer EGLConfig with desktop OpenGL\n");
        return 0;
    }

    const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 5,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
    g_egl.context = eglCreateContext(g_egl.display, g_egl.config, EGL_NO_CONTEXT, context_attribs);
    if (g_egl.context == EGL_NO_CONTEXT) {
        fprintf(stderr, "[headless] eglCreateContext (OpenGL 4.5 core) failed (0x%x)\n", eglGetError());
        return 0;
    }

    g_egl.surface = create_pbuffer(width, height);
    if (g_egl.surface == EGL_NO_SURFACE) {
        fprintf(stderr, "[headless] eglCreatePbufferSurface %dx%d failed (0x%x)\n", width, height, eglGetError());
        return 0;
    }
    if (!eglMakeCurrent(g_egl.display, g_egl.surface, g_egl.surface, g_egl.context)) {
        fprintf(stderr, "[headless] eglMakeCurrent failed (0x%x)\n", eglGetError());
        return 0;
    }
    g_egl.width = width;
    g_egl.height = height;

    // glad is not loaded yet: resolve glGetString directly for the log line.
    typedef const GLubyte *(*GetStringFn)(GLenum);
    const GetStringFn get_string = (GetStringFn) eglGetProcAddress("glGetString");
    if (get_string) {
        fprintf(stderr, "[headless] GL_RENDERER %s | GL_VERSION %s | %dx%d pbuffer, no window\n",
                (const char *) get_string(GL_RENDERER), (const char *) get_string(GL_VERSION), width, height);
    }
    return 1;
}

int headless_glfw_init(void) {
    if (g_headless) glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
    return glfwInit();
}

void headless_window_hints(void) {
    if (g_headless) glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
}

int headless_make_context_current(GLFWwindow *window) {
    if (!g_headless) {
        glfwMakeContextCurrent(window);
        return glfwGetCurrentContext() == window;
    }
    if (g_egl.context != EGL_NO_CONTEXT) return g_egl.window == window;

    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    g_egl.window = window;
    if (create_context(width, height)) return 1;
    headless_shutdown();
    return 0;
}

void *headless_get_proc_address(const char *name) {
    return g_headless ? (void *) eglGetProcAddress(name) : (void *) glfwGetProcAddress(name);
}

GLFWwindow *headless_current_window(void) {
    return g_headless ? g_egl.window : glfwGetCurrentContext();
}

void headless_swap_buffers(GLFWwindow *window) {
    if (!g_headless) {
        glfwSwapBuffers(window);
        return;
    }
    glFlush();

    // GLFW's null platform resizes a window that enters or leaves a monitor (fullscreen /
    // borderless) without a framebuffer-size event. Deliver it like a window system would
    // before the next glfwPollEvents: the callback (main.c resize_viewport) resizes the
    // pbuffer through headless_resize, the viewport and the HTML layer.
    int width = 0, height = 0;
    glfwGetFramebufferSize(window, &width, &height);
    if (window != g_egl.window || width <= 0 || height <= 0 || (width == g_egl.width && height == g_egl.height)) return;
    const GLFWframebuffersizefun callback = glfwSetFramebufferSizeCallback(window, NULL);
    glfwSetFramebufferSizeCallback(window, callback);
    if (callback) callback(window, width, height);
    else headless_resize(width, height);
}

void headless_swap_interval(const int interval) {
    if (!g_headless) glfwSwapInterval(interval);
}

void headless_resize(const int width, const int height) {
    if (!g_headless || g_egl.surface == EGL_NO_SURFACE || width <= 0 || height <= 0) return;
    if (width == g_egl.width && height == g_egl.height) return;

    const EGLSurface surface = create_pbuffer(width, height);
    if (surface == EGL_NO_SURFACE || !eglMakeCurrent(g_egl.display, surface, surface, g_egl.context)) {
        fprintf(stderr, "[headless] resize to %dx%d failed (0x%x); keeping %dx%d\n", width, height, eglGetError(), g_egl.width, g_egl.height);
        if (surface != EGL_NO_SURFACE) eglDestroySurface(g_egl.display, surface);
        return;
    }
    eglDestroySurface(g_egl.display, g_egl.surface);
    g_egl.surface = surface;
    g_egl.width = width;
    g_egl.height = height;
    fprintf(stderr, "[headless] pbuffer resized to %dx%d\n", width, height);
}

void headless_shutdown(void) {
    if (g_egl.display == EGL_NO_DISPLAY) return;
    eglMakeCurrent(g_egl.display, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
    if (g_egl.surface != EGL_NO_SURFACE) eglDestroySurface(g_egl.display, g_egl.surface);
    if (g_egl.context != EGL_NO_CONTEXT) eglDestroyContext(g_egl.display, g_egl.context);
    eglTerminate(g_egl.display);
    eglReleaseThread();
    g_egl.display = EGL_NO_DISPLAY;
    g_egl.surface = EGL_NO_SURFACE;
    g_egl.context = EGL_NO_CONTEXT;
    g_egl.window = NULL;
}

#else // no EGL (web, macOS, Windows, or Linux without libEGL): GLFW only

int headless_glfw_init(void) {
    if (g_headless) {
        fprintf(stderr, "[headless] KRUNKNATIVE_HEADLESS needs a Linux build with EGL (KRUNKNATIVE_HEADLESS_EGL)\n");
        return 0;
    }
    return glfwInit();
}

void headless_window_hints(void) {}

int headless_make_context_current(GLFWwindow *window) {
    glfwMakeContextCurrent(window);
    return glfwGetCurrentContext() == window;
}

void *headless_get_proc_address(const char *name) {
    return (void *) glfwGetProcAddress(name);
}

GLFWwindow *headless_current_window(void) {
    return glfwGetCurrentContext();
}

void headless_swap_buffers(GLFWwindow *window) {
    glfwSwapBuffers(window);
}

void headless_swap_interval(const int interval) {
    glfwSwapInterval(interval);
}

void headless_resize(const int width, const int height) {
    (void) width;
    (void) height;
}

void headless_shutdown(void) {}

#endif
