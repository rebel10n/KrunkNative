#define _GNU_SOURCE
#include "proc.h"
#include "util.h"
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static int executable(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode) && access(path, X_OK) == 0;
}

int proc_find_client(const Ctx *ctx, char *out, const size_t size) {
    if (ctx->client_bin[0]) {
        if (!executable(ctx->client_bin)) return -1;
        snprintf(out, size, "%s", ctx->client_bin);
        return 0;
    }
    const char *candidates[] = {"cmake-build-release/client_bin", "cmake-build-debug/client_bin", NULL};
    char path[PATH_MAX];
    double best_time = 0;
    int found = 0;
    for (int i = 0; candidates[i]; i++) {
        snprintf(path, sizeof(path), "%s/%s", ctx->root, candidates[i]);
        struct stat st;
        if (!executable(path) || stat(path, &st) != 0) continue;
        // the newer build wins (a stale release next to a fresh debug build would miss features)
        const double t = (double) st.st_mtime;
        if (!found || t > best_time) {
            snprintf(out, size, "%s", path);
            best_time = t;
            found = 1;
        }
    }
    if (found) return 0;
    char self[PATH_MAX];
    const ssize_t n = readlink("/proc/self/exe", self, sizeof(self) - 1);
    if (n > 0) {
        self[n] = 0;
        char *slash = strrchr(self, '/');
        if (slash) {
            snprintf(slash + 1, sizeof(self) - (size_t) (slash + 1 - self), "client_bin");
            if (executable(self)) {
                snprintf(out, size, "%s", self);
                return 0;
            }
        }
    }
    return -1;
}

int proc_run(const Ctx *ctx, char *const argv[], char *const env[], const char *cwd, const double timeout_s, ProcResult *out) {
    memset(out, 0, sizeof(*out));
    out->exit_code = -1;
    // only binaries kn_editor_mcp found itself
    char client[PATH_MAX];
    if (proc_find_client(ctx, client, sizeof(client)) != 0 || strcmp(argv[0], client) != 0) {
        log_msg(LOG_ERROR, "proc", "refusing to run %s (not KrunkNative's client_bin)", argv[0]);
        return -1;
    }
    char *base[16];
    int nb = 0;
    static char bufs[8][PATH_MAX + 32];
    const char *pass[] = {"PATH", "HOME", "LANG", "XDG_RUNTIME_DIR", "LD_LIBRARY_PATH", "KRUNKNATIVE_EGL_DEVICE", "__EGL_VENDOR_LIBRARY_DIRS", NULL};
    for (int i = 0; pass[i] && nb < 8; i++) {
        const char *v = getenv(pass[i]);
        if (!v) continue;
        snprintf(bufs[nb], sizeof(bufs[nb]), "%s=%s", pass[i], v);
        base[nb] = bufs[nb];
        nb++;
    }
    char *envp[64];
    int ne = 0;
    for (int i = 0; i < nb; i++) envp[ne++] = base[i];
    for (int i = 0; env && env[i] && ne < 63; i++) envp[ne++] = env[i];
    envp[ne] = NULL;

    int pipefd[2];
    if (pipe(pipefd) != 0) return -1;
    const double t0 = now_seconds();
    const pid_t pid = fork();
    if (pid < 0) {
        close(pipefd[0]);
        close(pipefd[1]);
        return -1;
    }
    if (pid == 0) {
        setpgid(0, 0);
        dup2(pipefd[1], 1);
        dup2(pipefd[1], 2);
        close(pipefd[0]);
        close(pipefd[1]);
        const int devnull = open("/dev/null", O_RDONLY);
        if (devnull >= 0) dup2(devnull, 0);
        if (cwd && chdir(cwd) != 0) _exit(126);
        execve(argv[0], argv, envp);
        _exit(127);
    }
    close(pipefd[1]);
    fcntl(pipefd[0], F_SETFL, O_NONBLOCK);
    Sb sb = {0};
    int status = 0, done = 0;
    while (!done) {
        struct pollfd pfd = {.fd = pipefd[0], .events = POLLIN};
        poll(&pfd, 1, 100);
        char buf[8192];
        ssize_t n;
        while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) {
            if (sb.len < (1u << 20)) sb_putn(&sb, buf, (size_t) n);
        }
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) done = 1;
        else if (now_seconds() - t0 > timeout_s) {
            kill(-pid, SIGKILL);
            waitpid(pid, &status, 0);
            out->timed_out = 1;
            done = 1;
        }
    }
    char buf[8192];
    ssize_t n;
    while ((n = read(pipefd[0], buf, sizeof(buf))) > 0) if (sb.len < (1u << 20)) sb_putn(&sb, buf, (size_t) n);
    close(pipefd[0]);
    out->seconds = now_seconds() - t0;
    out->output = sb_take(&sb);
    if (!out->timed_out && WIFEXITED(status)) out->exit_code = WEXITSTATUS(status);
    return 0;
}

void proc_result_free(ProcResult *r) {
    free(r->output);
    r->output = NULL;
}
