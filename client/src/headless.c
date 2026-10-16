// Headless GPU rendering through EGL, without a display server (client/include/headless.h).
#include <gl.h>
#include <headless.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_headless;

// KRUNKNATIVE_HEADLESS turns headless on for any value except empty, 0, false, no or off (any case).
// Before, only "0" was off, so KRUNKNATIVE_HEADLESS=false gave a windowless run that ui_render_settings.c
// then capped at 120 fps.
static int env_flag_on(const char *value) {
    static const char *const off[] = {"0", "false", "no", "off"};
    if (!value || !*value) return 0;
    for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
        const char *a = value, *b = off[i];
        while (*a && *b && tolower((unsigned char) *a) == *b) a++, b++;
        if (!*a && !*b) return 0;
    }
    return 1;
}

int headless_parse_args(int *argc, char **argv) {
    if (env_flag_on(getenv("KRUNKNATIVE_HEADLESS"))) g_headless = 1;

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
    // Headless runs (tests, agents, render scenarios) never open the desktop's sound devices:
    // playback and voice capture both use miniaudio's null backend, whatever the environment or
    // the Client tab's audioBackend setting asks for (it only applies when the variable is unset).
    // Nor do they reach the desktop keyring: the alt vault's master key goes to the run's own config
    // directory (the 0600 file keystore, client/src/alt/alt_keystore.c) unless a run forces another one.
    // A private XDG_CONFIG_HOME does not isolate the Secret Service or the Keychain, so without this
    // every e2e client would leave a key item in the user's login keyring.
    if (g_headless) {
#ifdef _WIN32
        _putenv_s("KRUNKNATIVE_AUDIO_BACKEND", "null");
        if (!getenv("KRUNKNATIVE_ALT_KEYSTORE")) _putenv_s("KRUNKNATIVE_ALT_KEYSTORE", "file");
#else
        setenv("KRUNKNATIVE_AUDIO_BACKEND", "null", 1);
        setenv("KRUNKNATIVE_ALT_KEYSTORE", "file", 0);
#endif
    }
    return g_headless;
}

int headless_enabled(void) {
    return g_headless;
}

#if defined(KRUNKNATIVE_HEADLESS_EGL) && !defined(__EMSCRIPTEN__)

#define EGL_NO_X11 // no Xlib types in eglplatform.h: headless never touches X
#include <EGL/egl.h>
#include <EGL/eglext.h>
#ifdef KRUNKNATIVE_GL_ANGLE
#include <EGL/eglext_angle.h>
#endif

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

static PFNEGLGETPLATFORMDISPLAYEXTPROC p_eglGetPlatformDisplayEXT;

#ifndef KRUNKNATIVE_GL_ANGLE // desktop GL: EGL device enumeration (KRUNKNATIVE_EGL_DEVICE)
static char g_egl_device_desc[96]; // headless_gl_backend(): the EGL device open_display picked
static PFNEGLQUERYDEVICESEXTPROC p_eglQueryDevicesEXT;
static PFNEGLQUERYDEVICESTRINGEXTPROC p_eglQueryDeviceStringEXT;

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
            snprintf(g_egl_device_desc, sizeof(g_egl_device_desc), "EGL device %d, %s", order[i], device_kind(d));
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
                snprintf(g_egl_device_desc, sizeof(g_egl_device_desc), "EGL_MESA_platform_surfaceless");
                return 1;
            }
            g_egl.display = EGL_NO_DISPLAY;
        }
    }

    fprintf(stderr, "[headless] no EGL device matches KRUNKNATIVE_EGL_DEVICE=%s (%d enumerated)\n", want[0] ? want : "(default)", count);
    return 0;
}
#endif // !KRUNKNATIVE_GL_ANGLE

#ifdef KRUNKNATIVE_GL_ANGLE
// ANGLE backends (KRUNKNATIVE_GL_BACKEND=angle, gl.h KN_GLES). KRUNKNATIVE_ANGLE_BACKEND=<name>
// forces one; otherwise the platform's list is tried in order and the first whose EGL display
// initializes wins: macOS Metal, then ANGLE's OpenGL (CGL) backend, which also runs where no
// Metal device exists (VMs: Apple's software renderer); Linux Vulkan, then OpenGL. SwiftShader
// (CPU Vulkan, only in ANGLE builds with --swiftshader) is the last resort and headless only:
// GLFW cannot ask for it. ANGLE_PREFERRED_DEVICE=nvidia|amd|intel picks ANGLE's Vulkan GPU.
typedef struct {
    const char *name;
    EGLint type;        // EGL_PLATFORM_ANGLE_TYPE_ANGLE
    EGLint device_type; // EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE, 0 = ANGLE's default (hardware)
    int glfw_type;      // GLFW_ANGLE_PLATFORM_TYPE for windows, 0 = headless only
} AngleBackend;

static const AngleBackend ANGLE_BACKENDS[] = {
#ifdef __APPLE__
    {"metal", EGL_PLATFORM_ANGLE_TYPE_METAL_ANGLE, 0, GLFW_ANGLE_PLATFORM_TYPE_METAL},
    {"opengl", EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE, 0, GLFW_ANGLE_PLATFORM_TYPE_OPENGL},
    {"vulkan", EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE, 0, 0}, // forced only (MoltenVK-less builds fail)
#else
    {"vulkan", EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE, 0, GLFW_ANGLE_PLATFORM_TYPE_VULKAN},
    {"opengl", EGL_PLATFORM_ANGLE_TYPE_OPENGL_ANGLE, 0, GLFW_ANGLE_PLATFORM_TYPE_OPENGL},
    {"opengles", EGL_PLATFORM_ANGLE_TYPE_OPENGLES_ANGLE, 0, GLFW_ANGLE_PLATFORM_TYPE_OPENGLES},
#endif
    {"swiftshader", EGL_PLATFORM_ANGLE_TYPE_VULKAN_ANGLE, EGL_PLATFORM_ANGLE_DEVICE_TYPE_SWIFTSHADER_ANGLE, 0},
};
#define ANGLE_BACKEND_COUNT ((int) (sizeof(ANGLE_BACKENDS) / sizeof(ANGLE_BACKENDS[0])))

static const AngleBackend *g_angle_backend; // the one that initialized (logged, kept for headless)

// Default order: every backend except the forced-only ones (vulkan on Apple).
static int angle_backend_default(const AngleBackend *b) {
#ifdef __APPLE__
    if (!strcmp(b->name, "vulkan")) return 0;
#endif
    return 1;
}

// Opens + initializes an ANGLE display for `b` (EGL_NO_DISPLAY + *error on failure). Headless
// Linux displays are surfaceless (no X11/Wayland connection); windows use the default native
// display, as GLFW does.
static EGLDisplay angle_open(const AngleBackend *b, const int headless, EGLint *error) {
    *error = EGL_SUCCESS;
    if (!p_eglGetPlatformDisplayEXT) {
        p_eglGetPlatformDisplayEXT = (PFNEGLGETPLATFORMDISPLAYEXTPROC) eglGetProcAddress("eglGetPlatformDisplayEXT");
        if (!p_eglGetPlatformDisplayEXT) return EGL_NO_DISPLAY;
    }
    EGLint attribs[8];
    int n = 0;
    attribs[n++] = EGL_PLATFORM_ANGLE_TYPE_ANGLE;
    attribs[n++] = b->type;
    if (b->device_type) {
        attribs[n++] = EGL_PLATFORM_ANGLE_DEVICE_TYPE_ANGLE;
        attribs[n++] = b->device_type;
    }
#ifndef __APPLE__
    if (headless) {
        attribs[n++] = EGL_PLATFORM_ANGLE_NATIVE_PLATFORM_TYPE_ANGLE;
        attribs[n++] = EGL_PLATFORM_SURFACELESS_MESA;
    }
#else
    (void) headless;
#endif
    attribs[n] = EGL_NONE;
    EGLDisplay display = p_eglGetPlatformDisplayEXT(EGL_PLATFORM_ANGLE_ANGLE, NULL /* EGL_DEFAULT_DISPLAY, an int on Apple */, attribs);
    if (display == EGL_NO_DISPLAY || !eglInitialize(display, NULL, NULL)) {
        *error = eglGetError();
        return EGL_NO_DISPLAY;
    }
    return display;
}

// Picks the first ANGLE backend that initializes (see ANGLE_BACKENDS). `headless` = 0 skips the
// headless-only ones (GLFW then opens its own display on the winner); 1 keeps it in g_egl.
static int angle_select_backend(const int headless) {
    const char *want = getenv("KRUNKNATIVE_ANGLE_BACKEND");
    if (want && !*want) want = NULL;
    int matched = 0;
    for (int i = 0; i < ANGLE_BACKEND_COUNT; i++) {
        const AngleBackend *b = &ANGLE_BACKENDS[i];
        if (want ? strcmp(want, b->name) != 0 : !angle_backend_default(b)) continue;
        matched = 1;
        if (!headless && !b->glfw_type) {
            if (want) fprintf(stderr, "[gl] ANGLE backend %s is headless-only (GLFW cannot create it); use KRUNKNATIVE_HEADLESS=1\n", b->name);
            continue;
        }
        EGLint error;
        const EGLDisplay display = angle_open(b, headless, &error);
        if (display == EGL_NO_DISPLAY) {
            char code[32] = ""; // ANGLE leaves EGL_SUCCESS when a backend has no device (Metal in a VM)
            if (error != EGL_SUCCESS) snprintf(code, sizeof(code), " (EGL error 0x%x)", error);
            fprintf(stderr, "[gl] ANGLE %s backend unavailable%s%s\n", b->name, code,
                    !strcmp(b->name, "metal") ? ": no Metal device, or an ANGLE build without Metal" : "");
            continue;
        }
        g_angle_backend = b;
        fprintf(stderr, "[gl] ANGLE backend: %s (%s)\n", b->name, eglQueryString(display, EGL_VERSION));
        // Windows: GLFW opens its own display on this backend. The probe display stays
        // initialized: ANGLE keeps displays for the process lifetime anyway, and terminating
        // its OpenGL-backend display broke GLFW's XOpenDisplay right after (Linux/X11).
        if (headless) g_egl.display = display;
        return 1;
    }
    if (want && !matched) {
        fprintf(stderr, "[gl] KRUNKNATIVE_ANGLE_BACKEND=%s: unknown; expected one of", want);
        for (int i = 0; i < ANGLE_BACKEND_COUNT; i++) fprintf(stderr, " %s", ANGLE_BACKENDS[i].name);
        fprintf(stderr, "\n");
    } else {
        fprintf(stderr, "[gl] no ANGLE backend initialized: nothing to render with (KRUNKNATIVE_ANGLE_BACKEND to force one)\n");
    }
    return 0;
}
#endif

static EGLSurface create_pbuffer(const int width, const int height) {
    const EGLint attribs[] = {EGL_WIDTH, width > 0 ? width : 1, EGL_HEIGHT, height > 0 ? height : 1, EGL_NONE};
    return eglCreatePbufferSurface(g_egl.display, g_egl.config, attribs);
}

static int create_context(const int width, const int height) {
#ifdef KRUNKNATIVE_GL_ANGLE
    // OpenGL ES 3.0 on ANGLE (gl.h KN_GLES), same as the windowed GLFW context.
    if (!angle_select_backend(1)) return 0;
    const EGLenum api = EGL_OPENGL_ES_API;
    const EGLint renderable = EGL_OPENGL_ES3_BIT;
    const char *api_name = "OpenGL ES 3.0";
    const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE,
    };
#else
    if (!open_display()) return 0;
    const EGLenum api = EGL_OPENGL_API;
    const EGLint renderable = EGL_OPENGL_BIT;
    const char *api_name = "OpenGL 4.5 core";
    const EGLint context_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 4,
        EGL_CONTEXT_MINOR_VERSION, 5,
        EGL_CONTEXT_OPENGL_PROFILE_MASK, EGL_CONTEXT_OPENGL_CORE_PROFILE_BIT,
        EGL_NONE,
    };
#endif

    if (!eglBindAPI(api)) {
        fprintf(stderr, "[headless] eglBindAPI(%s) failed (0x%x)\n", api_name, eglGetError());
        return 0;
    }

    // What GLFW asks a window system for by default: RGBA8, depth 24, stencil 8, no MSAA.
    const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE, EGL_PBUFFER_BIT,
        EGL_RENDERABLE_TYPE, renderable,
        EGL_COLOR_BUFFER_TYPE, EGL_RGB_BUFFER,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8,
        EGL_DEPTH_SIZE, 24, EGL_STENCIL_SIZE, 8,
        EGL_NONE,
    };
    EGLint config_count = 0;
    if (!eglChooseConfig(g_egl.display, config_attribs, &g_egl.config, 1, &config_count) || config_count < 1) {
        fprintf(stderr, "[headless] no RGBA8/D24S8 pbuffer EGLConfig with %s\n", api_name);
        return 0;
    }

    g_egl.context = eglCreateContext(g_egl.display, g_egl.config, EGL_NO_CONTEXT, context_attribs);
    if (g_egl.context == EGL_NO_CONTEXT) {
        fprintf(stderr, "[headless] eglCreateContext (%s) failed (0x%x)\n", api_name, eglGetError());
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

static const char *glfw_platform_name(void) {
    switch (glfwGetPlatform()) {
    case GLFW_PLATFORM_X11: return "X11";
    case GLFW_PLATFORM_WAYLAND: return "Wayland";
    case GLFW_PLATFORM_WIN32: return "Win32";
    case GLFW_PLATFORM_COCOA: return "Cocoa";
    case GLFW_PLATFORM_NULL: return "no window";
    default: return "?";
    }
}

const char *headless_gl_backend(void) {
    static char desc[192];
#ifdef KRUNKNATIVE_GL_ANGLE
    if (g_headless) snprintf(desc, sizeof(desc), "ANGLE %s (OpenGL ES 3.0), headless EGL pbuffer", g_angle_backend ? g_angle_backend->name : "?");
    else snprintf(desc, sizeof(desc), "ANGLE %s (OpenGL ES 3.0), GLFW %s window", g_angle_backend ? g_angle_backend->name : "?", glfw_platform_name());
#else
    if (g_headless) snprintf(desc, sizeof(desc), "desktop OpenGL (glad), headless EGL pbuffer on %s", g_egl_device_desc[0] ? g_egl_device_desc : "?");
    else snprintf(desc, sizeof(desc), "desktop OpenGL (glad), GLFW %s window", glfw_platform_name());
#endif
    return desc;
}

int headless_glfw_init(void) {
    if (g_headless) glfwInitHint(GLFW_PLATFORM, GLFW_PLATFORM_NULL);
#ifdef KRUNKNATIVE_GL_ANGLE
    // Windows: probe the ANGLE backend now (Metal on macOS, falling back to ANGLE's OpenGL)
    // and hand it to GLFW, which creates the EGL context on it. Headless picks its own in
    // create_context (it may also fall back to SwiftShader).
    if (!g_headless) {
        if (!angle_select_backend(0)) return 0;
        glfwInitHint(GLFW_ANGLE_PLATFORM_TYPE, g_angle_backend->glfw_type);
    }
#endif
    if (glfwInit()) return 1;
    const char *reason = NULL;
    glfwGetError(&reason);
    fprintf(stderr, "glfwInit failed: %s\n", reason ? reason : "unknown error");
    return 0;
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

// Frames in flight. A window's swap blocks once the driver has queued a frame or two ahead of
// the GPU; a pbuffer's flush never does. An uncapped headless client (no vsync, no frame cap:
// ~700 frames per second of menu) on a GPU that other processes keep busy therefore queued
// seconds of work, and the next call that waits for the GPU (a texture upload: the menu's
// lazily loaded window images, a map's textures; glReadPixels for a screenshot) waited for all
// of it inside one frame: Find Game froze the main thread for 3-6 s. Each swap now fences its
// frame and waits for the frame HEADLESS_FRAMES_IN_FLIGHT swaps back, like a window's swap.
#define HEADLESS_FRAMES_IN_FLIGHT 2
#define HEADLESS_FENCE_TIMEOUT_NS 1000000000ull // 1 s: never hang on a lost fence
static GLsync g_frame_fence[HEADLESS_FRAMES_IN_FLIGHT];
static unsigned g_frame_fence_next;

static void frame_fences_release(void) {
    for (int i = 0; i < HEADLESS_FRAMES_IN_FLIGHT; i++) {
        if (g_frame_fence[i]) glDeleteSync(g_frame_fence[i]);
        g_frame_fence[i] = NULL;
    }
}

void headless_swap_buffers(GLFWwindow *window) {
    if (!g_headless) {
        glfwSwapBuffers(window);
        return;
    }
    GLsync *fence = &g_frame_fence[g_frame_fence_next++ % HEADLESS_FRAMES_IN_FLIGHT];
    if (*fence) {
        glClientWaitSync(*fence, GL_SYNC_FLUSH_COMMANDS_BIT, HEADLESS_FENCE_TIMEOUT_NS);
        glDeleteSync(*fence);
    }
    *fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
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
    frame_fences_release(); // context objects: while the context is still current
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

#else // no EGL (web, macOS/Windows desktop GL, or Linux without libEGL): GLFW only

const char *headless_gl_backend(void) {
#ifdef __EMSCRIPTEN__
    return "WebGL 2 (OpenGL ES 3.0), browser canvas";
#else
    return "desktop OpenGL (glad), GLFW window";
#endif
}

int headless_glfw_init(void) {
    if (g_headless) {
        fprintf(stderr, "[headless] KRUNKNATIVE_HEADLESS needs EGL: a Linux build with libEGL or an ANGLE build (KRUNKNATIVE_HEADLESS_EGL)\n");
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
