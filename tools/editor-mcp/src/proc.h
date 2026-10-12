// Subprocesses of kn_editor_mcp: only KrunkNative's own binaries, exec'd directly (no shell),
// with an explicit environment, a working directory, captured output and a timeout.
#pragma once
#include "ctx.h"

typedef struct {
    int exit_code;   // -1 when killed / failed to start
    int timed_out;
    double seconds;
    char *output;    // stdout + stderr (malloc'd, may be NULL)
} ProcResult;

// Resolves client_bin: --client, else <root>/cmake-build-release/client_bin,
// <root>/cmake-build-debug/client_bin, then next to this executable. 0 ok.
int proc_find_client(const Ctx *ctx, char *out, size_t size);
// argv[0] must be a path from proc_find_client. env: NULL-terminated "K=V" list added to a
// minimal base environment (PATH, HOME, LANG, XDG_RUNTIME_DIR, LD_LIBRARY_PATH, KRUNKNATIVE_EGL_DEVICE;
// never DISPLAY / WAYLAND_DISPLAY). 0 when it ran (see exit_code).
int proc_run(const Ctx *ctx, char *const argv[], char *const env[], const char *cwd, double timeout_s, ProcResult *out);
void proc_result_free(ProcResult *r);
