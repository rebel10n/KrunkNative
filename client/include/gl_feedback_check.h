#pragma once

// Debug-build check against framebuffer / texture feedback loops (included by gl.h when
// KN_GL_FEEDBACK_CHECK is set: native debug builds, desktop GL and ANGLE).
//
// WebGL 2 and ANGLE reject a draw (GL_INVALID_OPERATION "Feedback loop formed between
// Framebuffer and active Texture") when a texture unit that one of the current program's
// samplers reads holds a texture attached to the bound draw framebuffer, even when the
// shader never samples it on that path. Desktop GL draws it anyway (undefined results), so
// the bug only shows on the web and on macOS. gl.h routes every glDraw* through
// kn_gl_feedback_check(), which asserts the same rule before the draw:
//
//   for each active sampler uniform of GL_CURRENT_PROGRAM: the texture bound to its unit
//   (for the sampler's target) is not a texture attachment of GL_DRAW_FRAMEBUFFER_BINDING.
//
// The default framebuffer (0) and renderbuffer-only framebuffers return early, so ordinary
// window draws cost two glGet calls. A violation prints the call site, program, unit,
// texture and attachment and aborts. KRUNKNATIVE_GL_FEEDBACK_CHECK=warn logs each call site
// once and keeps going; =0 turns the check off.
//
// The check makes no draw call, so it never re-enters itself, and it restores
// GL_ACTIVE_TEXTURE, the only state it changes. A web build can carry it too
// (-DKN_GL_FEEDBACK_CHECK=1 in CMAKE_C_FLAGS, see gl.h): there it only logs (warn) by default,
// to the browser console, so a page can name the call site behind a WebGL feedback-loop error.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static inline int kn_gl_feedback_mode(void) {
    static int mode = -1; // per translation unit; the environment does not change
    if (mode < 0) {
        const char *env = getenv("KRUNKNATIVE_GL_FEEDBACK_CHECK");
#ifdef __EMSCRIPTEN__
        const int fallback = 1; // the page keeps running; the console names the site
#else
        const int fallback = 2;
#endif
        mode = !env || !*env ? fallback : strcmp(env, "0") == 0 ? 0 : strcmp(env, "warn") == 0 ? 1 : 2;
    }
    return mode;
}

static inline GLenum kn_gl_sampler_binding(const GLenum type) {
    switch (type) {
    case GL_SAMPLER_2D: case GL_SAMPLER_2D_SHADOW: case GL_INT_SAMPLER_2D: case GL_UNSIGNED_INT_SAMPLER_2D:
        return GL_TEXTURE_BINDING_2D;
    case GL_SAMPLER_CUBE: case GL_SAMPLER_CUBE_SHADOW: case GL_INT_SAMPLER_CUBE: case GL_UNSIGNED_INT_SAMPLER_CUBE:
        return GL_TEXTURE_BINDING_CUBE_MAP;
    case GL_SAMPLER_3D: case GL_INT_SAMPLER_3D: case GL_UNSIGNED_INT_SAMPLER_3D:
        return GL_TEXTURE_BINDING_3D;
    case GL_SAMPLER_2D_ARRAY: case GL_SAMPLER_2D_ARRAY_SHADOW: case GL_INT_SAMPLER_2D_ARRAY:
    case GL_UNSIGNED_INT_SAMPLER_2D_ARRAY:
        return GL_TEXTURE_BINDING_2D_ARRAY;
    default:
        return 0;
    }
}

static inline void kn_gl_feedback_check(const char *file, const int line) {
    const int mode = kn_gl_feedback_mode();
    if (!mode) return;
    GLint fbo = 0, program = 0;
    glGetIntegerv(GL_DRAW_FRAMEBUFFER_BINDING, &fbo);
    if (!fbo) return;
    glGetIntegerv(GL_CURRENT_PROGRAM, &program);
    if (!program) return;

    // the draw framebuffer's texture attachments
    static const GLenum points[] = {
        GL_COLOR_ATTACHMENT0, GL_COLOR_ATTACHMENT1, GL_COLOR_ATTACHMENT2, GL_COLOR_ATTACHMENT3,
        GL_DEPTH_ATTACHMENT, GL_STENCIL_ATTACHMENT,
    };
    GLint textures[sizeof(points) / sizeof(points[0])];
    int n = 0;
    for (size_t i = 0; i < sizeof(points) / sizeof(points[0]); i++) {
        GLint type = GL_NONE, name = 0;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, points[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_TYPE, &type);
        if (type != GL_TEXTURE) continue;
        glGetFramebufferAttachmentParameteriv(GL_DRAW_FRAMEBUFFER, points[i], GL_FRAMEBUFFER_ATTACHMENT_OBJECT_NAME, &name);
        textures[n++] = name;
    }
    if (!n) return;

    GLint uniforms = 0, active = GL_TEXTURE0;
    glGetProgramiv((GLuint) program, GL_ACTIVE_UNIFORMS, &uniforms);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active);
    for (GLint u = 0; u < uniforms; u++) {
        char name[128];
        GLint size = 0;
        GLenum type = 0;
        glGetActiveUniform((GLuint) program, (GLuint) u, sizeof(name), NULL, &size, &type, name);
        const GLenum binding = kn_gl_sampler_binding(type);
        if (!binding) continue;
        GLint units[32];
        if (size > 32) size = 32;
        char *bracket = strchr(name, '['); // an array reports "name[0]": each element by name
        if (bracket) *bracket = 0;
        for (GLint e = 0; e < size; e++) {
            char element[160];
            if (bracket) snprintf(element, sizeof(element), "%s[%d]", name, e);
            else snprintf(element, sizeof(element), "%s", name);
            const GLint location = glGetUniformLocation((GLuint) program, element);
            units[e] = 0;
            if (location >= 0) glGetUniformiv((GLuint) program, location, &units[e]);
        }
        for (GLint e = 0; e < size; e++) {
            GLint bound = 0;
            glActiveTexture(GL_TEXTURE0 + (GLenum) units[e]);
            glGetIntegerv(binding, &bound);
            for (int t = 0; bound && t < n; t++) {
                if (bound != textures[t]) continue;
                static char seen[64][96]; // warn mode: each call site once (per translation unit)
                char site[96];
                snprintf(site, sizeof(site), "%s:%d", file, line);
                int known = 0, slot = -1;
                for (int s = 0; s < 64; s++) {
                    if (!seen[s][0]) { if (slot < 0) slot = s; continue; }
                    if (strcmp(seen[s], site) == 0) known = 1;
                }
                if (!known) {
                    fprintf(stderr, "[gl] FEEDBACK LOOP at %s: program %d sampler '%s' (unit %d) reads texture %d, "
                            "attached to draw framebuffer %d (KRUNKNATIVE_GL_FEEDBACK_CHECK=warn|0)\n",
                            site, program, name, units[e], bound, fbo);
                    if (slot >= 0) snprintf(seen[slot], sizeof(seen[slot]), "%s", site);
                }
                if (mode == 2) abort();
            }
        }
    }
    glActiveTexture((GLenum) active);
}

#undef glDrawArrays
#undef glDrawElements
#undef glDrawArraysInstanced
#undef glDrawElementsInstanced
#undef glDrawRangeElements
// the real entry point: glad's function pointer natively; on the web the function itself (a macro's
// own name is not expanded again inside it)
#ifdef __EMSCRIPTEN__
#define KN_GL_RAW(name) name
#else
#define KN_GL_RAW(name) glad_##name
#endif
#define glDrawArrays(...) (kn_gl_feedback_check(__FILE__, __LINE__), KN_GL_RAW(glDrawArrays)(__VA_ARGS__))
#define glDrawElements(...) (kn_gl_feedback_check(__FILE__, __LINE__), KN_GL_RAW(glDrawElements)(__VA_ARGS__))
#define glDrawArraysInstanced(...) (kn_gl_feedback_check(__FILE__, __LINE__), KN_GL_RAW(glDrawArraysInstanced)(__VA_ARGS__))
#define glDrawElementsInstanced(...) (kn_gl_feedback_check(__FILE__, __LINE__), KN_GL_RAW(glDrawElementsInstanced)(__VA_ARGS__))
#define glDrawRangeElements(...) (kn_gl_feedback_check(__FILE__, __LINE__), KN_GL_RAW(glDrawRangeElements)(__VA_ARGS__))
