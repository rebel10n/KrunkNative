#pragma once
// The launcher window (launcher_ui.c): a small GLFW window with the title, a status line, a
// progress bar, a detail line and, after a failure, Retry / Play / Quit buttons. Text uses
// FreeType over the ui/ fonts; without them only the bar and the window title show progress.
// GL through client/include/gl.h (desktop GL or ANGLE, like client_bin; headless EGL with
// KRUNKNATIVE_HEADLESS=1, plus KRUNKNATIVE_SCREENSHOT=<png> for a capture).

typedef struct {
    char status[160];   // "Downloading game content"
    char detail[200];   // "120 / 1700 MB  -  14.2 MB/s"
    double progress;    // 0..1, < 0 = indeterminate
    int error;          // status is an error: buttons
    int can_play;       // offer Play next to Retry (an older install still works)
} LauncherView;

typedef enum { UI_NONE = 0, UI_RETRY, UI_PLAY, UI_QUIT } LauncherAction;

typedef struct LauncherUi LauncherUi;

// ui_dir: the install's ui/ (fonts). NULL when no window can be opened (no display).
LauncherUi *launcher_ui_open(const char *ui_dir);
// One frame (polls events). Returns the button the user pressed; closing the window is UI_QUIT.
LauncherAction launcher_ui_frame(LauncherUi *ui, const LauncherView *view);
// Hides the window at once (Quit while the worker winds down).
void launcher_ui_hide(LauncherUi *ui);
// Destroys the window and terminates GLFW (before exec'ing the client).
void launcher_ui_close(LauncherUi *ui);
