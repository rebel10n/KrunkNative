#pragma once
// Headless GPU rendering: no X11/Wayland window, nothing on screen.
//
//   KRUNKNATIVE_HEADLESS=1 (or --headless)   enable (client_bin and ui_test)
//   KRUNKNATIVE_EGL_DEVICE=nvidia|mesa|<n>   GPU: an NVIDIA EGL device, a Mesa device (hardware
//                                           DRM first, then llvmpipe) or index <n> of
//                                           eglQueryDevicesEXT; default nvidia when present,
//                                           else mesa, else EGL_MESA_platform_surfaceless
//
// GLFW runs on its null platform, so the GLFWwindow, input callbacks, cursor modes, monitors
// and timers keep working (input simply never arrives: drive the client with
// KRUNKNATIVE_INPUT_SCRIPT, xdotool needs a real X server such as Xvfb). The window has no
// GLFW context (GLFW's null platform cannot create an EGL one); this module creates an EGL
// context on a device display (EGL_EXT_platform_device) with a pbuffer surface at the
// window's framebuffer size, which is the default framebuffer 0: glReadPixels/glReadBuffer
// (GL_BACK) read-backs in debug_capture.c, input_script.c and ui_scenario.c work unchanged.
// Linux builds with EGL, and every ANGLE build (KRUNKNATIVE_GL_BACKEND=angle, macOS included:
// an OpenGL ES 3.0 context on an ANGLE display, see KRUNKNATIVE_ANGLE_BACKEND in headless.c)
// define KRUNKNATIVE_HEADLESS_EGL; elsewhere headless_glfw_init() fails with a message.
//
// ANGLE builds also pick the ANGLE backend here for windowed runs (headless_glfw_init:
// Metal on macOS, else ANGLE's OpenGL backend; GLFW_ANGLE_PLATFORM_TYPE) and log it.
//
// Every function below also works when headless mode is off: it then forwards to GLFW, so
// callers use them unconditionally around window/context creation and presentation.
#include <GLFW/glfw3.h>

// Reads KRUNKNATIVE_HEADLESS and removes `--headless` from argv (argc updated). Returns 1 when
// headless mode is on. Call before headless_glfw_init().
int headless_parse_args(int *argc, char **argv);
int headless_enabled(void);

// glfwInit(), on GLFW's null platform when headless.
int headless_glfw_init(void);
// Window hints after the caller's GL hints: GLFW_NO_API when headless (the context is EGL's).
void headless_window_hints(void);
// Makes a GL 4.5 core (ANGLE: OpenGL ES 3.0) context current for `window`: glfwMakeContextCurrent, or (headless)
// creates the EGL context + pbuffer at the window's framebuffer size and logs GL_RENDERER.
// Returns 0 on failure (message on stderr).
int headless_make_context_current(GLFWwindow *window);
// GL loader for kn_gl_load (gl.h): eglGetProcAddress when headless, else glfwGetProcAddress.
void *headless_get_proc_address(const char *name);
// The window whose context is current (glfwGetCurrentContext() is NULL when headless).
GLFWwindow *headless_current_window(void);
// End of frame: glfwSwapBuffers, or (headless) a glFlush, since a pbuffer has no front
// buffer, plus the framebuffer-size event the null platform skips on fullscreen changes.
void headless_swap_buffers(GLFWwindow *window);
// glfwSwapInterval; no-op when headless (a pbuffer never presents, and GLFW has no context).
void headless_swap_interval(int interval);
// Framebuffer size callback: recreates the pbuffer at the new size (no-op when not headless).
void headless_resize(int width, int height);
// Destroys the EGL context/surface (no-op when not headless). Call before glfwTerminate().
void headless_shutdown(void);
