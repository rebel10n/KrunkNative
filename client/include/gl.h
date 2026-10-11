#pragma once

// Single OpenGL entry point for the client renderer.
//
// Native builds load desktop OpenGL 4.5 through the vendored GLAD loader.
// Emscripten builds target WebGL2, which is exposed as OpenGL ES 3.0 through
// the browser; the ES headers ship with the emscripten sysroot and no loader
// is needed (the functions are resolved by the WebGL binding layer).
//
// Renderer code must stay inside the OpenGL ES 3.0 subset that both paths
// share. Desktop-only entry points (direct state access, glPolygonMode, ...)
// have to be guarded with __EMSCRIPTEN__ where they are used.
#ifdef __EMSCRIPTEN__
#include <GLES3/gl3.h>
#include <GLES2/gl2ext.h>
#else
#include <glad/glad.h>
#endif
