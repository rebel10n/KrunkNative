#pragma once

// Single OpenGL entry point for the client renderer. Three GL flavours:
//
//   desktop  (default natively)  OpenGL 4.5 core (4.1 core on macOS with
//                                KRUNKNATIVE_GL_BACKEND=desktop) through the vendored
//                                GLAD loader in client/lib/glad.
//   ANGLE    (KRUNKNATIVE_GL_ANGLE, the macOS default: KRUNKNATIVE_GL_BACKEND=angle)
//                                OpenGL ES 3.0 on ANGLE's libEGL/libGLESv2, which render
//                                through Metal on Apple (Vulkan/OpenGL elsewhere), through
//                                the GLES 3.0 GLAD loader in client/lib/glad_gles.
//   web      (__EMSCRIPTEN__)    WebGL2, exposed as OpenGL ES 3.0 by the browser; the ES
//                                headers ship with the emscripten sysroot and no loader is
//                                needed (the WebGL binding layer resolves the functions).
//
// KN_GLES is defined for both ES flavours (ANGLE and web). Renderer code must stay inside
// the OpenGL ES 3.0 subset that every flavour shares; the few desktop-only entry points
// (glPolygonMode, glGetTexImage, ...) are guarded with `#ifndef KN_GLES` where they are
// used, and GLSL goes through shader.c, which rewrites `#version 450 core` to
// `#version 300 es` (+ default precisions) when KN_GLES is set.
//
// kn_gl_load(loader) loads the function pointers for the current context (no-op on the web).
#if defined(__EMSCRIPTEN__)
#define KN_GLES 1
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#define kn_gl_load(loader) ((void) (loader), 1)
#elif defined(KRUNKNATIVE_GL_ANGLE)
#define KN_GLES 1
#include <glad/glad.h> // client/lib/glad_gles: OpenGL ES 3.0
#define kn_gl_load(loader) gladLoadGLES2Loader((GLADloadproc) (loader))
#else
#include <glad/glad.h> // client/lib/glad: desktop OpenGL 4.5
#define kn_gl_load(loader) gladLoadGLLoader((GLADloadproc) (loader))
#endif
