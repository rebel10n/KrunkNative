#pragma once
// Native window state across restarts: the window mode (windowed / fullscreen / borderless),
// the windowed rect (position + size windowed mode returns to) and the window manager's
// maximized flag.
//
// - F4 (app_reload) and the Settings "*" relaunch hand the exact current state to the new
//   process in KRUNKNATIVE_RESTORE_WINDOW, set right before exec and cleared at startup.
//   It wins over the --windowed/--fullscreen/--borderless flag still in argv (a forced mode
//   carries over as forced: F11 keeps not persisting it), KRUNKNATIVE_WINDOW_SIZE and the
//   Client tab's Resolution.
// - Every normal exit persists the windowed rect + maximized flag (storage key "knWindow",
//   next to knSettings), so a plain restart opens where the window was, like most games.
//   The mode itself stays the "windowMode" setting (F11).
//
// This file is the pure part (text format, startup precedence, apply order) for
// client/tests/window_state_test.c; the GLFW side is in client/src/ui/ui_render_settings.c.

#include <stddef.h>

#define WINDOW_STATE_ENV "KRUNKNATIVE_RESTORE_WINDOW"
#define WINDOW_STATE_STORAGE_KEY "knWindow"
#define WINDOW_STATE_TEXT_MAX 96
#define WINDOW_STATE_DEFAULT_W 1280
#define WINDOW_STATE_DEFAULT_H 720
#define WINDOW_STATE_MIN_SIZE 64      // smaller sizes are rejected as garbage
#define WINDOW_STATE_MAX_COORD 32767  // X11's coordinate range

typedef struct {
    int mode;      // RS_WINDOWED / RS_FULLSCREEN / RS_BORDERLESS (render_settings.h)
    int x, y;      // windowed rect: top-left of the client area (screen coordinates)
    int w, h;      // windowed rect: client area size in screen coordinates
    int maximized; // the windowed rect is maximized by the window manager
    int forced;    // the mode is a session-only --windowed/--fullscreen/--borderless one
} WindowState;

// "mode,x,y,w,h,maximized,forced" (forced optional on decode). Encode returns 0 when the
// buffer is too small; decode returns 0 for malformed or out-of-range text (out untouched).
int window_state_encode(const WindowState *s, char *out, size_t n);
int window_state_decode(const char *text, WindowState *out);

// Startup inputs, in precedence order (window_state_resolve).
typedef struct {
    const WindowState *restore;   // KRUNKNATIVE_RESTORE_WINDOW (F4 / relaunch), NULL if none
    int cli_mode;                 // --windowed/--fullscreen/--borderless, -1 if none
    int stored_mode;              // the "windowMode" setting, -1 if not stored
    int env_w, env_h;             // KRUNKNATIVE_WINDOW_SIZE, 0 if not set
    const WindowState *persisted; // "knWindow" (rect + maximized of the last exit), NULL if none
} WindowStartupIn;

typedef struct {
    WindowState state; // what the window opens as (state.forced: client_render_force_window_mode)
    int has_pos;       // place the window at state.x/y (else the window manager places it)
    int restored;      // from KRUNKNATIVE_RESTORE_WINDOW: its size is exact (skip Resolution)
} WindowStartup;

void window_state_resolve(const WindowStartupIn *in, WindowStartup *out);

// The order the startup applies it in, so the first visible frame is already right: the
// window is created hidden at the windowed rect (the size un-fullscreen / un-maximize
// returns to), placed, switched to its mode (or maximized) and only then shown.
typedef enum {
    WINDOW_STEP_CREATE_HIDDEN, // glfwCreateWindow(state.w, state.h) with GLFW_VISIBLE false
    WINDOW_STEP_POSITION,      // glfwSetWindowPos(state.x, state.y)
    WINDOW_STEP_FULLSCREEN,    // exclusive fullscreen (glfwSetWindowMonitor)
    WINDOW_STEP_BORDERLESS,    // undecorated, covering the monitor
    WINDOW_STEP_MAXIMIZE,      // glfwMaximizeWindow (hidden: it maps maximized)
    WINDOW_STEP_SHOW,          // glfwShowWindow
} WindowStep;

#define WINDOW_STEPS_MAX 5

// Writes the steps for `s` into out (at least WINDOW_STEPS_MAX entries); returns the count.
int window_state_steps(const WindowStartup *s, WindowStep *out);

// Where a window without a kept position opens (launch, page windows, the editor's views): centred on
// a monitor's work area (wx, wy, ww, wh: the desktop minus panels / docks) in screen coordinates.
// w, h: the client area; frame: the decorations around it (left, top, right, bottom; NULL / 0 when
// unknown). The whole frame is centred; one bigger than the work area keeps its title bar inside (the
// top-left corner of the frame at the work area's). *x, *y: the client area's top-left, as
// glfwSetWindowPos takes it.
void window_state_center(int wx, int wy, int ww, int wh, int w, int h, const int frame[4], int *x, int *y);
// The work area among n monitors (each wa[i] = {x, y, w, h}) a new window opens on: the one holding
// the point (px, py) when has_point (the opener's centre), else `primary`. Returns that index, -1 when n <= 0.
int window_state_pick_monitor(const int (*wa)[4], int n, int primary, int has_point, int px, int py);
