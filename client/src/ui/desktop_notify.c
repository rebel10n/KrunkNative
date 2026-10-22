// Desktop (OS) notifications for the notification centre's items (client/include/desktop_notify.h).
//
// Linux: notify-send (libnotify-bin) started with an argv (no shell); the double fork of
// ui_dialog.c's xdg-open, so the notifier is re-parented and never a zombie. macOS: osascript
// with the title and text as argv items of an `on run argv` script (nothing is quoted or escaped).
// Windows and the web build: no desktop notifier (desktop_notify_backend() is "none").
#include <desktop_notify.h>
#include "klog.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32) && !defined(__EMSCRIPTEN__)
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#define DESKTOP_NOTIFY_POSIX 1
#endif

static int g_enabled = 1;
static DesktopNotifySink g_sink;

void desktop_notify_set_enabled(const int on) { g_enabled = on ? 1 : 0; }
int desktop_notify_enabled(void) { return g_enabled; }
void desktop_notify_set_sink(const DesktopNotifySink sink) { g_sink = sink; }

int desktop_notify_parse(const char *stored) { return !(stored && !strcmp(stored, "false")); }

// ---------------------------------------------------------------- backends

#ifdef DESKTOP_NOTIFY_POSIX
// An executable of PATH (access X_OK), without a shell.
static int on_path(const char *name) {
    const char *path = getenv("PATH");
    if (!path) return 0;
    char full[1024];
    while (*path) {
        const char *end = strchr(path, ':');
        const size_t len = end ? (size_t) (end - path) : strlen(path);
        if (len && len + strlen(name) + 2 < sizeof(full)) {
            memcpy(full, path, len);
            snprintf(full + len, sizeof(full) - len, "/%s", name);
            if (access(full, X_OK) == 0) return 1;
        }
        if (!end) break;
        path = end + 1;
    }
    return 0;
}

// Double fork: the program is re-parented to init and this process never waits on it. argv only.
static void spawn_detached(const char *program, char *const argv[]) {
    const pid_t pid = fork();
    if (pid == 0) {
        if (fork() == 0) {
            setsid();
            execvp(program, argv);
            _exit(127);
        }
        _exit(0);
    }
    if (pid > 0) waitpid(pid, NULL, 0);
}
#endif

#if defined(__EMSCRIPTEN__) || defined(_WIN32)
int desktop_notify_available(void) { return 0; }
const char *desktop_notify_backend(void) { return "none"; }
static void os_post(const char *title, const char *text) { (void) title; (void) text; }
#elif defined(__APPLE__)
int desktop_notify_available(void) { return 1; } // osascript ships with macOS
const char *desktop_notify_backend(void) { return "osascript"; }
static void os_post(const char *title, const char *text) {
    char *const argv[] = {"osascript", "-e", "on run argv", "-e", "display notification (item 2 of argv) with title (item 1 of argv)",
                          "-e", "end run", (char *) title, (char *) text, NULL};
    spawn_detached("osascript", argv);
}
#else
int desktop_notify_available(void) { return on_path("notify-send"); }
const char *desktop_notify_backend(void) { return desktop_notify_available() ? "notify-send" : "none"; }
static void os_post(const char *title, const char *text) {
    char *const argv[] = {"notify-send", "--app-name=Krunker Box Office", "--", (char *) title, (char *) text, NULL};
    spawn_detached("notify-send", argv);
}
#endif

// ---------------------------------------------------------------- caption

void desktop_notify_caption(const int enabled, const int available, char *out, const size_t n) {
    if (!n) return;
    const char *text = !enabled ? "Desktop notifications are off."
                     : !available ? "No desktop notifier found on this system. On Linux, install libnotify (notify-send)."
                                  : "Friend requests, messages, Wild Offers and surveys also show on your desktop.";
    snprintf(out, n, "%s", text);
}

// ---------------------------------------------------------------- posting

// KRUNKNATIVE_HEADLESS / KRUNKNATIVE_NO_BROWSER on (any value but empty, 0, false, no, off).
static int env_on(const char *name) {
    const char *v = getenv(name);
    if (!v || !*v) return 0;
    static const char *const off[] = {"0", "false", "no", "off"};
    for (size_t i = 0; i < sizeof(off) / sizeof(off[0]); i++) {
        size_t k = 0;
        while (v[k] && off[i][k] && (v[k] | 0x20) == off[i][k]) k++;
        if (!v[k] && !off[i][k]) return 0;
    }
    return 1;
}

void desktop_notify_item(const char *title, const char *text) {
    if (!g_enabled) return;
    title = title ? title : "";
    text = text ? text : "";
    if (g_sink) {
        g_sink(title, text);
        return;
    }
    if (env_on("KRUNKNATIVE_HEADLESS") || env_on("KRUNKNATIVE_NO_BROWSER")) return;
    if (!strcmp(desktop_notify_backend(), "none")) return;
    KN_LOG_VERBOSE2("[desktop-notify] %s: %s\n", title, text);
    fflush(stdout);
    os_post(title, text);
}
