// Native window model for the Krunker pages (app_mode.h): like the v3.1.1 Electron client
// (main/main.js handleNavigation), the game, the social hub, the editor and the viewer
// each get their own window, one window per page kind: opening a page whose window exists
// reloads that window with the new URL query and raises it, otherwise a new window opens.
// Links from a page to the game go to the game window (its "loadURL").
//
// Every window is its own client_bin process (`--game|--hub|--editor|--viewer <query>`,
// same working directory and environment, so the same config dir and server settings):
// the client keeps its state in process-wide singletons (the Client INSTANCE, the HTML
// engine's documents and handler contexts, GL caches, audio), so a second GLFW window in
// one process would mean making all of them per-window; a process per window is what
// Electron does too and keeps a crashing page away from the game.
//
// The process that opened the others is the router (the game window, or a page started on
// its own, which opens a game window on demand, Electron's isSingle). It owns one pipe
// pair per child, handed to the child as fds 3 (router -> child) and 4 (child -> router)
// and announced by KRUNKNATIVE_APP_IPC=3,4:
//   router -> child   "nav <query>\n"          load this query in your page, raise
//   child  -> router  "open <mode> <query>\n"  open/reload that page's window
// A child whose router pipe closes (the game window was closed) closes too, like
// Electron's mainWindow "close" quitting the app; the router terminates its children when
// it exits. Windows builds spawn the window without the pipes (no reuse, no routing back).
#if !defined(__EMSCRIPTEN__)

#include <app_mode.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
extern char **environ;
#endif

#define APP_WINDOW_QUERY_MAX 1024
#define APP_WINDOW_LINE_MAX 2048

// v3.1.1 subWin sizes (main.js): social 1200x600, editor 1200x800, viewer 800x600
static const int k_window_size[APP_MODE_COUNT][2] = {{1280, 720}, {1200, 600}, {1200, 800}, {800, 600}};
static const char *const k_mode_flags[APP_MODE_COUNT] = {"--game", "--hub", "--editor", "--viewer"};

typedef struct {
    int pid;            // 0 = no window
    int to_child;       // write end (router -> child)
    int from_child;     // read end (child -> router)
    char inbuf[APP_WINDOW_LINE_MAX];
    size_t inlen;
} AppChild;

static char g_exe[4096];
static AppChild g_children[APP_MODE_COUNT];
static int g_parent_in = -1;   // child: router -> us
static int g_parent_out = -1;  // child: us -> router
static char g_parent_buf[APP_WINDOW_LINE_MAX];
static size_t g_parent_len;
static int g_initialized;

static void exe_path(const char *argv0) {
#if defined(_WIN32)
    if (!GetModuleFileNameA(NULL, g_exe, sizeof(g_exe))) snprintf(g_exe, sizeof(g_exe), "%s", argv0 ? argv0 : "client_bin");
#elif defined(__APPLE__)
    uint32_t size = sizeof(g_exe);
    if (_NSGetExecutablePath(g_exe, &size) != 0) snprintf(g_exe, sizeof(g_exe), "%s", argv0 ? argv0 : "client_bin");
#else
    const ssize_t n = readlink("/proc/self/exe", g_exe, sizeof(g_exe) - 1);
    if (n > 0) g_exe[n] = 0;
    else snprintf(g_exe, sizeof(g_exe), "%s", argv0 ? argv0 : "client_bin");
#endif
}

#ifndef _WIN32
static void set_nonblock_cloexec(const int fd) {
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL) | O_NONBLOCK);
    fcntl(fd, F_SETFD, fcntl(fd, F_GETFD) | FD_CLOEXEC);
}

static void write_line(const int fd, const char *line) {
    if (fd < 0) return;
    const size_t len = strlen(line);
    size_t done = 0;
    while (done < len) {
        const ssize_t w = write(fd, line + done, len - done);
        if (w <= 0) {
            if (w < 0 && errno == EINTR) continue;
            if (w < 0 && errno == EAGAIN) continue; // tiny messages: the pipe drains quickly
            return;
        }
        done += (size_t) w;
    }
}

// Reads what is available into buf; calls fn(line) per complete line. Returns 0 on EOF.
static int read_lines(const int fd, char *buf, size_t *len, void (*fn)(const char *line, void *user), void *user) {
    for (;;) {
        if (*len + 1 >= APP_WINDOW_LINE_MAX) *len = 0; // overlong line: drop
        const ssize_t r = read(fd, buf + *len, APP_WINDOW_LINE_MAX - 1 - *len);
        if (r == 0) return 0;
        if (r < 0) return errno == EAGAIN || errno == EINTR ? 1 : 0;
        *len += (size_t) r;
        char *nl;
        while ((nl = memchr(buf, '\n', *len))) {
            *nl = 0;
            fn(buf, user);
            const size_t used = (size_t) (nl - buf) + 1;
            memmove(buf, nl + 1, *len - used);
            *len -= used;
        }
    }
}
#endif

void app_window_init(const char *argv0) {
    if (g_initialized) return;
    g_initialized = 1;
    exe_path(argv0);
    for (int i = 0; i < APP_MODE_COUNT; i++) g_children[i].to_child = g_children[i].from_child = -1;
#ifndef _WIN32
    signal(SIGPIPE, SIG_IGN); // a closed window's pipe must not kill the writer
    const char *ipc = getenv("KRUNKNATIVE_APP_IPC");
    int in = -1, out = -1;
    if (ipc && sscanf(ipc, "%d,%d", &in, &out) == 2 && fcntl(in, F_GETFD) != -1 && fcntl(out, F_GETFD) != -1) {
        g_parent_in = in;
        g_parent_out = out;
        set_nonblock_cloexec(in);
        set_nonblock_cloexec(out);
    }
    unsetenv("KRUNKNATIVE_APP_IPC"); // not inherited by the windows this one opens
#endif
}

int app_window_has_router(void) {
    return g_parent_out >= 0;
}

static int child_alive(AppChild *c) {
    if (!c->pid) return 0;
#ifdef _WIN32
    return 0;
#else
    int status;
    const pid_t r = waitpid((pid_t) c->pid, &status, WNOHANG);
    if (r == 0) return 1;
    if (c->to_child >= 0) close(c->to_child);
    if (c->from_child >= 0) close(c->from_child);
    c->pid = 0;
    c->to_child = c->from_child = -1;
    c->inlen = 0;
    return 0;
#endif
}

static int spawn_window(Client *client, const AppMode mode, const char *query) {
    char size[64];
    snprintf(size, sizeof(size), "KRUNKNATIVE_WINDOW_SIZE=%dx%d", k_window_size[mode][0], k_window_size[mode][1]);
    // new windows open centred on the work area of the opener's monitor (its centre point,
    // KRUNKNATIVE_WINDOW_NEAR; client_render_window_created), placed before they are shown
    char pos[64] = "";
    if (client && client->window) {
        int x = 0, y = 0, w = 0, h = 0;
        glfwGetWindowPos(client->window, &x, &y);
        glfwGetWindowSize(client->window, &w, &h);
        snprintf(pos, sizeof(pos), "KRUNKNATIVE_WINDOW_NEAR=%d,%d", x + w / 2, y + h / 2);
    }
    const int offline = client && client->offline;
    fprintf(stderr, "[app] new %s window%s%s%s\n", app_mode_name(mode), query && *query ? " ?" : "", query ? query : "",
            offline ? " (offline)" : "");
#ifdef _WIN32
    char cmd[8192];
    snprintf(cmd, sizeof(cmd), "\"%s\" --windowed%s %s \"%s\"", g_exe, offline ? " --offline" : "", k_mode_flags[mode], query ? query : "");
    SetEnvironmentVariableA("KRUNKNATIVE_WINDOW_SIZE", size + strlen("KRUNKNATIVE_WINDOW_SIZE="));
    if (pos[0]) SetEnvironmentVariableA("KRUNKNATIVE_WINDOW_NEAR", pos + strlen("KRUNKNATIVE_WINDOW_NEAR="));
    STARTUPINFOA si = {0};
    PROCESS_INFORMATION pi = {0};
    si.cb = sizeof(si);
    const BOOL ok = CreateProcessA(NULL, cmd, NULL, NULL, FALSE, 0, NULL, NULL, &si, &pi);
    SetEnvironmentVariableA("KRUNKNATIVE_WINDOW_SIZE", NULL);
    SetEnvironmentVariableA("KRUNKNATIVE_WINDOW_NEAR", NULL);
    if (!ok) return 0;
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return 1;
#else
    int down[2], up[2]; // down: router -> child, up: child -> router
    if (pipe(down) != 0) return 0;
    if (pipe(up) != 0) {
        close(down[0]);
        close(down[1]);
        return 0;
    }
    set_nonblock_cloexec(down[1]);
    set_nonblock_cloexec(up[0]);
    fcntl(down[0], F_SETFD, FD_CLOEXEC);
    fcntl(up[1], F_SETFD, FD_CLOEXEC);

    posix_spawn_file_actions_t actions;
    posix_spawn_file_actions_init(&actions);
    posix_spawn_file_actions_adddup2(&actions, down[0], 3); // dup2 clears FD_CLOEXEC on 3 / 4
    posix_spawn_file_actions_adddup2(&actions, up[1], 4);

    // environment: ours + the window size + the IPC fds
    size_t n = 0;
    while (environ[n]) n++;
    char **envp = calloc(n + 4, sizeof(char *));
    size_t e = 0;
    for (size_t i = 0; i < n; i++) {
        if (!strncmp(environ[i], "KRUNKNATIVE_WINDOW_SIZE=", 24) || !strncmp(environ[i], "KRUNKNATIVE_WINDOW_NEAR=", 24) ||
            !strncmp(environ[i], "KRUNKNATIVE_APP_IPC=", 20)) continue;
        envp[e++] = environ[i];
    }
    envp[e++] = size;
    if (pos[0]) envp[e++] = pos;
    envp[e++] = "KRUNKNATIVE_APP_IPC=3,4";
    envp[e] = NULL;

    char *argv[8];
    int a = 0;
    argv[a++] = g_exe;
    argv[a++] = "--windowed";
    if (offline) argv[a++] = "--offline";
    argv[a++] = (char *) k_mode_flags[mode];
    argv[a++] = (char *) (query ? query : "");
    argv[a] = NULL;

    pid_t pid = 0;
    const int rc = posix_spawn(&pid, g_exe, &actions, NULL, argv, envp);
    posix_spawn_file_actions_destroy(&actions);
    free(envp);
    close(down[0]);
    close(up[1]);
    if (rc != 0) {
        fprintf(stderr, "[app] cannot start %s: %s\n", g_exe, strerror(rc));
        close(down[1]);
        close(up[0]);
        return 0;
    }
    AppChild *c = &g_children[mode];
    c->pid = (int) pid;
    c->to_child = down[1];
    c->from_child = up[0];
    c->inlen = 0;
    return 1;
#endif
}

void app_window_open(Client *client, const AppMode mode, const char *query) {
    if (mode < 0 || mode >= APP_MODE_COUNT) return;
    if (!g_initialized) app_window_init(NULL);
#ifndef _WIN32
    if (g_parent_out >= 0) {
        // a page window: the router owns the windows
        char line[APP_WINDOW_LINE_MAX];
        snprintf(line, sizeof(line), "open %s %s\n", app_mode_name(mode), query ? query : "");
        write_line(g_parent_out, line);
        return;
    }
    AppChild *c = &g_children[mode];
    if (child_alive(c)) {
        char line[APP_WINDOW_LINE_MAX];
        snprintf(line, sizeof(line), "nav %s\n", query ? query : "");
        fprintf(stderr, "[app] %s window: reload%s%s\n", app_mode_name(mode), query && *query ? " ?" : "", query ? query : "");
        write_line(c->to_child, line);
        return;
    }
#endif
    spawn_window(client, mode, query);
}

// ------------------------------------------------------------------ messages

static AppMode mode_named(const char *name, size_t len) {
    for (int m = 0; m < APP_MODE_COUNT; m++) {
        const char *n = app_mode_name((AppMode) m);
        if (strlen(n) == len && !strncmp(n, name, len)) return (AppMode) m;
    }
    return APP_MODE_COUNT;
}

#ifndef _WIN32
static void on_child_line(const char *line, void *user) {
    Client *client = user;
    if (strncmp(line, "open ", 5) != 0) return;
    const char *name = line + 5;
    const char *sp = strchr(name, ' ');
    const size_t len = sp ? (size_t) (sp - name) : strlen(name);
    const AppMode mode = mode_named(name, len);
    if (mode == APP_MODE_COUNT) return;
    app_mode_open_window(client, mode, sp ? sp + 1 : "");
}

static void on_parent_line(const char *line, void *user) {
    Client *client = user;
    if (!strncmp(line, "nav ", 4)) app_mode_navigate_self(client, line + 4);
    else if (!strcmp(line, "nav")) app_mode_navigate_self(client, "");
}
#endif

void app_window_poll(Client *client) {
#ifndef _WIN32
    for (int m = 0; m < APP_MODE_COUNT; m++) {
        AppChild *c = &g_children[m];
        if (!c->pid) continue;
        if (c->from_child >= 0 && !read_lines(c->from_child, c->inbuf, &c->inlen, on_child_line, client)) {
            close(c->from_child);
            c->from_child = -1;
        }
        child_alive(c); // reaps a closed window
    }
    if (g_parent_in >= 0 && !read_lines(g_parent_in, g_parent_buf, &g_parent_len, on_parent_line, client)) {
        // the router (game window) is gone: close with it
        close(g_parent_in);
        g_parent_in = -1;
        if (g_parent_out >= 0) close(g_parent_out);
        g_parent_out = -1;
        fprintf(stderr, "[app] the window that opened this one closed: closing too\n");
        if (client && client->window) glfwSetWindowShouldClose(client->window, GLFW_TRUE);
    }
#else
    (void) client;
#endif
}

void app_window_fini(void) {
#ifndef _WIN32
    for (int m = 0; m < APP_MODE_COUNT; m++) {
        AppChild *c = &g_children[m];
        if (!c->pid) continue;
        if (c->to_child >= 0) close(c->to_child); // EOF: the child closes itself
        if (c->from_child >= 0) close(c->from_child);
        c->to_child = c->from_child = -1;
    }
#endif
}

#else
typedef int app_window_web_build;
#endif
