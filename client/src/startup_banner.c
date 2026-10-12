// Startup log (client/include/startup_banner.h).
#include <gl.h>
#include <build_info.h>
#include <headless.h>
#include <startup_banner.h>
#include <stdio.h>

#ifdef __EMSCRIPTEN__
#include <emscripten.h>
#elif !defined(_WIN32)
#include <sys/utsname.h>
#endif

static const char *gl_string(const GLenum name) {
    const char *s = (const char *) glGetString(name);
    return s ? s : "?";
}

#ifdef __EMSCRIPTEN__
// v7.1.4 src/index.js (top level, before anything else runs):
//   const L = "padding: 1px; border-radius: 3px 0 0 3px; color: #fff; background: #606060;";
//   const R = "padding: 1px; border-radius: 0 3px 3px 0; color: #fff; background";
//   const ua = navigator.userAgent.toString();
//   console.info("%c Browser %c " + ua.slice(ua.lastIndexOf(") ") + 2, ua.length) + " ", L, R + ": #1475b2;");
// The Browser badge is that line verbatim; the KrunkNative, Renderer and User Agent badges
// below it use the same two styles. The renderer is WebGL's unmasked one when the browser
// exposes WEBGL_debug_renderer_info (GL_RENDERER is the masked "WebKit WebGL").
EM_JS(void, web_console_banner, (const char *build, const char *backend, const char *renderer, const char *version), {
    var L = "padding: 1px; border-radius: 3px 0 0 3px; color: #fff; background: #606060;";
    var R = "padding: 1px; border-radius: 0 3px 3px 0; color: #fff; background";
    var ua = navigator.userAgent.toString();
    console.info("%c Browser %c " + ua.slice(ua.lastIndexOf(") ") + 2, ua.length) + " ", L, R + ": #1475b2;");
    console.info("%c KrunkNative %c " + UTF8ToString(build) + " ", L, R + ": #f2a20d;");
    var gpu = UTF8ToString(renderer);
    try {
        var gl = GL.currentContext && GL.currentContext.GLctx;
        var ext = gl && gl.getExtension("WEBGL_debug_renderer_info");
        if (ext) gpu = gl.getParameter(ext.UNMASKED_RENDERER_WEBGL) || gpu;
    } catch (e) {}
    console.info("%c Renderer %c " + gpu + " | " + UTF8ToString(version) + " | " + UTF8ToString(backend) + " ", L, R + ": #2e9e4f;");
    console.info("%c User Agent %c " + ua + " ", L, R + ": #606060;");
});
#else
static const char *platform_os(void) {
#if defined(__APPLE__)
    return "macOS";
#elif defined(_WIN32)
    return "Windows";
#elif defined(__linux__)
    return "Linux";
#elif defined(__FreeBSD__)
    return "FreeBSD";
#else
    return "unknown OS";
#endif
}

static const char *platform_arch(void) {
#if defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#elif defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__i386__) || defined(_M_IX86)
    return "x86";
#else
    return "unknown arch";
#endif
}
#endif

void startup_banner(void) {
#ifdef __EMSCRIPTEN__
    web_console_banner(kn_build_string(), headless_gl_backend(), gl_string(GL_RENDERER), gl_string(GL_VERSION));
#else
    char kernel[160] = "";
#ifndef _WIN32
    struct utsname u;
    if (uname(&u) == 0) snprintf(kernel, sizeof(kernel), " (%s %s)", u.sysname, u.release);
#endif
    // "v1.0.0-51-g6f0cea9 (6f0cea9e1234, built 2026-10-12T05:00:00Z, protocol 1.3)": describe, commit
    printf("[startup] KrunkNative client %s\n", kn_build_string());
    printf("[startup] platform %s %s%s\n", platform_os(), platform_arch(), kernel);
    printf("[startup] GL backend %s\n", headless_gl_backend());
    printf("[startup] GL_RENDERER %s | GL_VERSION %s\n", gl_string(GL_RENDERER), gl_string(GL_VERSION));
    fflush(stdout);
#endif
}
