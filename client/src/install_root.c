// Startup chdir to the install root of a packaged client (install_root.h).
#include <install_root.h>

#ifdef __EMSCRIPTEN__

int install_root_chdir(void) {
    return 0;
}

#else

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#ifdef _WIN32
#include <direct.h>
#include <windows.h>
#define chdir _chdir
#define getcwd _getcwd
#else
#include <limits.h>
#include <unistd.h>
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#ifndef PATH_MAX
#define PATH_MAX 4096
#endif

static int is_dir(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && (st.st_mode & S_IFMT) == S_IFDIR;
}

static int ends_with(const char *s, const char *suffix) {
    const size_t n = strlen(s), m = strlen(suffix);
    return n >= m && !strcmp(s + n - m, suffix);
}

// Absolute path of the running executable, symlinks resolved. 0 on failure.
static int executable_path(char *out, const size_t n) {
#if defined(_WIN32)
    const DWORD len = GetModuleFileNameA(NULL, out, (DWORD) n);
    return len > 0 && len < n;
#elif defined(__APPLE__)
    char raw[PATH_MAX];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) return 0;
    char resolved[PATH_MAX];
    if (!realpath(raw, resolved) || strlen(resolved) >= n) return 0;
    strcpy(out, resolved);
    return 1;
#else
    const ssize_t len = readlink("/proc/self/exe", out, n - 1);
    if (len <= 0) return 0;
    out[len] = 0;
    return 1;
#endif
}

int install_root_chdir(void) {
    char cwd[PATH_MAX];
    if (!getcwd(cwd, sizeof(cwd))) cwd[0] = 0;
    if (is_dir("ui") || is_dir("assets")) return 0;
    if (ends_with(cwd, "cmake-build-debug") || ends_with(cwd, "cmake-build-release")) return 0;

    char root[PATH_MAX];
    if (!executable_path(root, sizeof(root))) return 0;
    char *slash = strrchr(root, '/');
#ifdef _WIN32
    char *backslash = strrchr(root, '\\');
    if (!slash || (backslash && backslash > slash)) slash = backslash;
#endif
    if (!slash) return 0;
    *slash = 0;

#ifdef __APPLE__
    // <name>.app/Contents/MacOS/<exe>: ui/ (and kn-install.json) live in Contents/Resources
    if (ends_with(root, "/Contents/MacOS") && strlen(root) + 16 < sizeof(root)) {
        strcpy(root + strlen(root) - strlen("MacOS"), "Resources");
    }
#endif

    char ui[PATH_MAX + 4];
    snprintf(ui, sizeof(ui), "%s/ui", root);
    if (!is_dir(ui) || chdir(root) != 0) return 0;
    printf("[install] working directory %s (was %s)\n", root, cwd[0] ? cwd : "?");
    return 1;
}

#endif
