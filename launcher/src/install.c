// The launcher's install: location, assets dir, journaled swap, processes (kn_install.h).
#include <kn_fs.h>
#include <kn_install.h>

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#define CLIENT_EXE_NAME "client.exe"
#else
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#define CLIENT_EXE_NAME "client_bin"
extern char **environ;
#endif

static void set_err(char *err, const size_t errn, const char *msg, const char *arg) {
    if (err && errn) snprintf(err, errn, "%s%s%s", msg, arg ? ": " : "", arg ? arg : "");
}

// ------------------------------------------------------------------ location

static int read_marker(const char *dir, KnInstallMarker *m) {
    char path[KN_PATH_MAX], *json = NULL;
    size_t len = 0;
    if (!kn_fs_join(path, sizeof(path), dir, KN_INSTALL_MARKER) || !kn_fs_read(path, &json, &len, 65536)) return 0;
    const int ok = kn_install_marker_parse(json, len, m);
    free(json);
    if (!ok) fprintf(stderr, "[launcher] ignoring malformed %s\n", path);
    return ok;
}

int kn_install_locate(KnInstall *in, const char *root_override) {
    memset(in, 0, sizeof(*in));
    if (root_override && *root_override) {
        if (!kn_fs_absolute(root_override, in->exe_dir, sizeof(in->exe_dir))) return 0;
        kn_fs_join(in->exe, sizeof(in->exe), in->exe_dir, "krunknative");
    } else {
        if (!kn_fs_self_exe(in->exe, sizeof(in->exe))) return 0;
        snprintf(in->exe_dir, sizeof(in->exe_dir), "%s", in->exe);
        kn_fs_dirname(in->exe_dir);
    }
    kn_fs_join(in->client, sizeof(in->client), in->exe_dir, CLIENT_EXE_NAME);
    // kn-install.json: next to the launcher, or in ../Resources of a macOS bundle
    char candidates[2][KN_PATH_MAX];
    snprintf(candidates[0], KN_PATH_MAX, "%s", in->exe_dir);
    kn_fs_join(candidates[1], KN_PATH_MAX, in->exe_dir, "../Resources");
    for (int i = 0; i < 2 && !in->has_marker; i++) {
        if (!read_marker(candidates[i], &in->marker)) continue;
        in->has_marker = 1;
        kn_fs_absolute(candidates[i], in->marker_dir, sizeof(in->marker_dir));
    }
    if (in->has_marker) {
        char root[KN_PATH_MAX];
        kn_fs_join(root, sizeof(root), in->marker_dir, in->marker.root);
        kn_fs_absolute(root, in->root, sizeof(in->root));
    } else {
        snprintf(in->marker_dir, sizeof(in->marker_dir), "%s", in->exe_dir);
        snprintf(in->root, sizeof(in->root), "%s", in->exe_dir);
    }
    snprintf(in->platform, sizeof(in->platform), "%s",
             in->has_marker && in->marker.platform[0] ? in->marker.platform : kn_update_platform_default());
    kn_fs_join(in->state_dir, sizeof(in->state_dir), in->root, ".kn-update");
    in->root_writable = kn_fs_dir_writable(in->root);
    in->self_updatable = in->has_marker && kn_install_self_updatable(&in->marker) && in->root_writable;
    return kn_fs_exists(in->client);
}

int kn_install_user_data_dir(char *out, const size_t n) {
    char base[KN_PATH_MAX] = "";
#ifdef _WIN32
    const wchar_t *w = _wgetenv(L"LOCALAPPDATA");
    if (!w || !WideCharToMultiByte(CP_UTF8, 0, w, -1, base, (int) sizeof(base), NULL, NULL)) return 0;
    for (char *p = base; *p; p++)
        if (*p == '\\') *p = '/';
    if (!kn_fs_join(out, n, base, "KrunkNative")) return 0;
#elif defined(__APPLE__)
    const char *home = getenv("HOME");
    if (!home || !*home) return 0;
    snprintf(base, sizeof(base), "%s/Library/Application Support", home);
    if (!kn_fs_join(out, n, base, "KrunkNative")) return 0;
#else
    const char *xdg = getenv("XDG_DATA_HOME");
    const char *home = getenv("HOME");
    if (xdg && xdg[0] == '/') snprintf(base, sizeof(base), "%s", xdg);
    else if (home && *home) snprintf(base, sizeof(base), "%s/.local/share", home);
    else return 0;
    if (!kn_fs_join(out, n, base, "krunknative")) return 0;
#endif
    return kn_fs_mkdirs(out);
}

static int finish_dir(const char *dir, char *out, const size_t n) {
    char abs[KN_PATH_MAX];
    if (!kn_fs_mkdirs(dir) || !kn_fs_absolute(dir, abs, sizeof(abs))) return 0;
    const size_t l = strlen(abs);
    return snprintf(out, n, "%s%s", abs, l && abs[l - 1] == '/' ? "" : "/") < (int) n;
}

int kn_install_assets_dir(const KnInstall *in, const char *override, char *out, const size_t n) {
    out[0] = 0;
    if (override && *override) return finish_dir(override, out, n);
    if (!in->has_marker) return 0; // dev launcher: the client finds ./assets itself
    char dir[KN_PATH_MAX];
    int user = !kn_install_self_updatable(&in->marker); // deb/rpm: /opt is not ours to write
#ifdef __APPLE__
    user = 1; // never inside the signed bundle
#endif
    if (!user) {
        kn_fs_join(dir, sizeof(dir), in->marker_dir, "assets");
        if ((kn_fs_is_dir(dir) && kn_fs_dir_writable(dir)) || (!kn_fs_exists(dir) && kn_fs_dir_writable(in->marker_dir)))
            return finish_dir(dir, out, n);
    }
    char data[KN_PATH_MAX];
    if (!kn_install_user_data_dir(data, sizeof(data))) return 0;
    kn_fs_join(dir, sizeof(dir), data, "assets");
    return finish_dir(dir, out, n);
}

int kn_install_dir_in_checkout(const char *dir, char *root, const size_t n) {
    char path[KN_PATH_MAX];
#ifdef _WIN32
    if (!kn_fs_absolute(dir, path, sizeof(path))) return 0;
#else
    // resolve symlinks: a worktree's assets/<dir> links into the main checkout
    char *real = realpath(dir, NULL);
    if (real) {
        snprintf(path, sizeof(path), "%s", real);
        free(real);
    } else if (!kn_fs_absolute(dir, path, sizeof(path))) {
        return 0;
    }
#endif
    for (;;) {
        char git[KN_PATH_MAX];
        kn_fs_join(git, sizeof(git), path, ".git");
        if (kn_fs_exists(git)) {
            if (root && n) snprintf(root, n, "%s", path);
            return 1;
        }
        const size_t before = strlen(path);
        kn_fs_dirname(path);
        const size_t after = strlen(path);
        // the top: "/" (POSIX), or "." after a drive ("C:" has no slash): never the cwd's .git,
        // which made the Windows launcher refuse any install started from inside a checkout
        if (!after || after >= before || !strcmp(path, ".")) return 0;
    }
}

// ------------------------------------------------------------------ swap

typedef struct {
    char **items; // "F<rel>" file / symlink, "D<rel>" whole directory (ui/)
    size_t n;
    const char *staged;
    int oom;
} SwapList;

// .kn-* (launcher state, sync temp files) is never part of a package. assets/ files the package
// ships (shaders, maps, img, ...: the code-coupled, git-tracked part) are installed like the
// executables; the rest of assets/ is the game content the asset sync owns and is not touched.
static int skip_component(const char *rel) {
    const char *seg = rel;
    for (const char *p = rel;; p++) {
        if (*p == '/' || !*p) {
            const size_t sl = (size_t) (p - seg);
            if (sl >= 4 && !strncmp(seg, ".kn-", 4)) return 1;
            if (!*p) break;
            seg = p + 1;
        }
    }
    return 0;
}

static int list_add(SwapList *l, const char kind, const char *rel) {
    char **grown = realloc(l->items, (l->n + 1) * sizeof(*grown));
    if (!grown) return 0;
    l->items = grown;
    const size_t len = strlen(rel);
    l->items[l->n] = malloc(len + 2);
    if (!l->items[l->n]) return 0;
    l->items[l->n][0] = kind;
    memcpy(l->items[l->n] + 1, rel, len + 1);
    l->n++;
    return 1;
}

static int swap_visit(const char *rel, const int is_dir, void *user) {
    SwapList *l = user;
    if (skip_component(rel)) return 1;
    // inside a whole-directory item already listed?
    for (size_t i = 0; i < l->n; i++) {
        const char *d = l->items[i] + 1;
        const size_t dl = strlen(d);
        if (l->items[i][0] == 'D' && !strncmp(rel, d, dl) && rel[dl] == '/') return 1;
    }
    if (is_dir) {
        const char *base = strrchr(rel, '/');
        base = base ? base + 1 : rel;
        if (!strcmp(base, "ui")) {
            char marker[KN_PATH_MAX], parent[KN_PATH_MAX];
            snprintf(parent, sizeof(parent), "%s", rel);
            kn_fs_dirname(parent);
            char p2[KN_PATH_MAX];
            if (!strcmp(parent, ".")) snprintf(p2, sizeof(p2), "%s", l->staged);
            else kn_fs_join(p2, sizeof(p2), l->staged, parent);
            kn_fs_join(marker, sizeof(marker), p2, KN_INSTALL_MARKER);
            if (kn_fs_exists(marker) && !list_add(l, 'D', rel)) l->oom = 1;
        }
        return !l->oom;
    }
    if (!list_add(l, 'F', rel)) l->oom = 1;
    return !l->oom;
}

static void list_free(SwapList *l) {
    for (size_t i = 0; i < l->n; i++) free(l->items[i]);
    free(l->items);
}

static FILE *journal_open(const KnInstall *in, const char *mode) {
    char path[KN_PATH_MAX];
    kn_fs_join(path, sizeof(path), in->state_dir, "journal");
    return kn_fs_fopen(path, mode);
}

static void journal_line(FILE *j, const char op, const char *rel) {
    fprintf(j, "%c %s\n", op, rel);
    fflush(j);
#ifndef _WIN32
    fsync(fileno(j));
#endif
}

static int move(const char *from, const char *to) {
    return kn_fs_mkdirs_parent(to) && kn_fs_rename(from, to);
}

// Undoes the journal's operations in reverse. B <rel>: root/rel went to backup/rel.
// N <rel>: staged/rel went to root/rel. Lines may name operations that never happened.
static void rollback(const KnInstall *in, char **ops, const size_t n, const char *staged) {
    char backup[KN_PATH_MAX];
    kn_fs_join(backup, sizeof(backup), in->state_dir, "backup");
    for (size_t i = n; i-- > 0;) {
        const char op = ops[i][0];
        const char *rel = ops[i] + 2;
        char a[KN_PATH_MAX], b[KN_PATH_MAX];
        kn_fs_join(a, sizeof(a), in->root, rel);
        if (op == 'N') {
            kn_fs_join(b, sizeof(b), staged, rel);
            if (!kn_fs_exists(b) && kn_fs_exists(a) && !move(a, b)) fprintf(stderr, "[launcher] rollback: cannot move %s back\n", a);
        } else if (op == 'B') {
            kn_fs_join(b, sizeof(b), backup, rel);
            if (kn_fs_exists(b) && !move(b, a)) fprintf(stderr, "[launcher] rollback: cannot restore %s\n", a);
        }
    }
}

int kn_install_swap(const KnInstall *in, const char *staged, char *err, const size_t errn) {
    if (err && errn) err[0] = 0;
    SwapList l = {.staged = staged};
    if (!kn_fs_walk(staged, swap_visit, &l) || l.oom || !l.n) {
        list_free(&l);
        set_err(err, errn, l.oom ? "out of memory" : "the package is empty", NULL);
        return 0;
    }
    char backup[KN_PATH_MAX];
    kn_fs_join(backup, sizeof(backup), in->state_dir, "backup");
    kn_fs_rmtree(backup);
    if (!kn_fs_mkdirs(backup)) {
        list_free(&l);
        set_err(err, errn, "cannot create", backup);
        return 0;
    }
    FILE *j = journal_open(in, "wb");
    if (!j) {
        list_free(&l);
        set_err(err, errn, "cannot write the update journal in", in->state_dir);
        return 0;
    }
    fprintf(j, "S %s\n", staged);
    char **ops = calloc(l.n * 2, sizeof(*ops));
    size_t nops = 0;
    int ok = ops != NULL;
    for (size_t i = 0; ok && i < l.n; i++) {
        const char *rel = l.items[i] + 1;
        char cur[KN_PATH_MAX], bak[KN_PATH_MAX], neu[KN_PATH_MAX];
        kn_fs_join(cur, sizeof(cur), in->root, rel);
        kn_fs_join(bak, sizeof(bak), backup, rel);
        kn_fs_join(neu, sizeof(neu), staged, rel);
        if (kn_fs_exists(cur) || kn_fs_is_symlink(cur)) {
            ops[nops] = malloc(strlen(rel) + 3);
            sprintf(ops[nops++], "B %s", rel);
            journal_line(j, 'B', rel);
            if (!move(cur, bak)) {
                set_err(err, errn, "cannot move aside (is the game or a server still running?)", cur);
                ok = 0;
                break;
            }
        }
        ops[nops] = malloc(strlen(rel) + 3);
        sprintf(ops[nops++], "N %s", rel);
        journal_line(j, 'N', rel);
        if (!move(neu, cur)) {
            set_err(err, errn, "cannot install", cur);
            ok = 0;
        }
    }
    if (ok) {
        journal_line(j, 'C', "commit");
    } else if (ops) {
        rollback(in, ops, nops, staged);
    }
    fclose(j);
    for (size_t i = 0; i < nops; i++) free(ops[i]);
    free(ops);
    list_free(&l);
    if (!ok && err && errn && !err[0]) set_err(err, errn, "out of memory", NULL);
    return ok;
}

int kn_install_recover(const KnInstall *in) {
    FILE *j = journal_open(in, "rb");
    if (!j) return 0;
    char line[KN_PATH_MAX + 4], staged[KN_PATH_MAX] = "";
    char **ops = NULL;
    size_t n = 0;
    int committed = 0;
    while (fgets(line, sizeof(line), j)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == 'S' && line[1] == ' ') snprintf(staged, sizeof(staged), "%s", line + 2);
        else if (line[0] == 'C') committed = 1;
        else if ((line[0] == 'B' || line[0] == 'N') && line[1] == ' ' && kn_update_path_ok(line + 2)) {
            char **grown = realloc(ops, (n + 1) * sizeof(*grown));
            if (!grown) break;
            ops = grown;
            ops[n++] = strdup(line);
        }
    }
    fclose(j);
    int rolled = 0;
    if (!committed && n && staged[0]) {
        fprintf(stderr, "[launcher] an update was interrupted: rolling back %zu step(s)\n", n);
        rollback(in, ops, n, staged);
        rolled = 1;
    }
    for (size_t i = 0; i < n; i++) free(ops[i]);
    free(ops);
    char path[KN_PATH_MAX];
    kn_fs_join(path, sizeof(path), in->state_dir, "journal");
    kn_fs_remove(path);
    return rolled;
}

void kn_install_cleanup(const KnInstall *in) {
    if (kn_fs_exists(in->state_dir)) kn_fs_rmtree(in->state_dir);
}

// ------------------------------------------------------------------ package assets

typedef struct {
    const char *src, *dst;
    int copied, failed;
    char *err;
    size_t errn;
} PkgAssets;

static int pkg_asset_visit(const char *rel, const int is_dir, void *user) {
    PkgAssets *c = user;
    if (is_dir || !kn_update_path_ok(rel) || !strncmp(rel, ".kn-", 4) || strstr(rel, "/.kn-")) return 1;
    char from[KN_PATH_MAX], to[KN_PATH_MAX];
    kn_fs_join(from, sizeof(from), c->src, rel);
    kn_fs_join(to, sizeof(to), c->dst, rel);
    const int64_t size = kn_fs_size(from);
    if (size < 0) return 1;
    if (kn_fs_size(to) == size) {
        char a[65], b[65];
        if (kn_fs_sha256(from, a) && kn_fs_sha256(to, b) && !strcmp(a, b)) return 1;
    }
    char *data = NULL;
    size_t len = 0;
    if (!kn_fs_read(from, &data, &len, (size_t) 1 << 28) || !kn_fs_mkdirs_parent(to) || !kn_fs_write_atomic(to, data, len)) {
        set_err(c->err, c->errn, "cannot copy", to);
        c->failed = 1;
        free(data);
        return 0;
    }
    free(data);
    c->copied++;
    return 1;
}

int kn_install_package_assets(const KnInstall *in, const char *assets_dir, char *err, const size_t errn) {
    char src[KN_PATH_MAX], a[KN_PATH_MAX], b[KN_PATH_MAX];
    kn_fs_join(src, sizeof(src), in->marker_dir, "assets");
    if (!kn_fs_is_dir(src) || !kn_fs_absolute(src, a, sizeof(a)) || !kn_fs_absolute(assets_dir, b, sizeof(b))) return 0;
    const size_t la = strlen(a), lb = strlen(b);
    if (la && a[la - 1] == '/') a[la - 1] = 0;
    if (lb && b[lb - 1] == '/') b[lb - 1] = 0;
    if (!strcmp(a, b)) return 0; // the package's assets/ is the game content dir itself
    PkgAssets c = {.src = a, .dst = b, .err = err, .errn = errn};
    kn_fs_walk(a, pkg_asset_visit, &c);
    return c.failed ? -1 : c.copied;
}

// ------------------------------------------------------------------ processes

#ifdef _WIN32
static wchar_t *wide(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    wchar_t *w = n > 0 ? malloc((size_t) n * sizeof(wchar_t)) : NULL;
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    return w;
}

// argv -> one command line with the MSVCRT quoting rules (CommandLineToArgvW inverse)
static wchar_t *command_line(char *const argv[]) {
    size_t cap = 64;
    for (int i = 0; argv[i]; i++) cap += strlen(argv[i]) * 2 + 3;
    char *s = malloc(cap), *p = s;
    if (!s) return NULL;
    for (int i = 0; argv[i]; i++) {
        if (i) *p++ = ' ';
        *p++ = '"';
        size_t backslashes = 0;
        for (const char *c = argv[i];; c++) {
            if (*c == '\\') {
                backslashes++;
                continue;
            }
            const size_t times = !*c ? backslashes * 2 : *c == '"' ? backslashes * 2 + 1 : backslashes;
            for (size_t k = 0; k < times; k++) *p++ = '\\';
            backslashes = 0;
            if (!*c) break;
            *p++ = *c;
        }
        *p++ = '"';
    }
    *p = 0;
    wchar_t *w = wide(s);
    free(s);
    return w;
}

int kn_proc_run(const char *exe, char *const argv[], const int timeout_ms, char *out, const size_t outn) {
    if (out && outn) out[0] = 0;
    SECURITY_ATTRIBUTES sa = {sizeof(sa), NULL, TRUE};
    HANDLE rd = NULL, wr = NULL;
    if (!CreatePipe(&rd, &wr, &sa, 1 << 16)) return -1;
    SetHandleInformation(rd, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    si.dwFlags = STARTF_USESTDHANDLES;
    si.hStdOutput = si.hStdError = wr;
    si.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION pi;
    wchar_t *wexe = wide(exe), *cmd = command_line(argv);
    const BOOL started = wexe && cmd && CreateProcessW(wexe, cmd, NULL, NULL, TRUE, CREATE_NO_WINDOW, NULL, NULL, &si, &pi);
    free(wexe);
    free(cmd);
    CloseHandle(wr);
    if (!started) {
        CloseHandle(rd);
        return -1;
    }
    size_t got = 0;
    DWORD waited = 0, code = (DWORD) -1;
    for (;;) {
        DWORD avail = 0;
        while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail) {
            char buf[512];
            DWORD r = 0;
            if (!ReadFile(rd, buf, sizeof(buf), &r, NULL) || !r) break;
            for (DWORD i = 0; i < r && out && got + 1 < outn; i++) out[got++] = buf[i];
            if (out && outn) out[got] = 0;
        }
        if (WaitForSingleObject(pi.hProcess, 50) == WAIT_OBJECT_0) break;
        waited += 50;
        if ((int) waited >= timeout_ms) {
            TerminateProcess(pi.hProcess, 1);
            CloseHandle(pi.hProcess);
            CloseHandle(pi.hThread);
            CloseHandle(rd);
            return -1;
        }
    }
    DWORD avail = 0;
    while (PeekNamedPipe(rd, NULL, 0, NULL, &avail, NULL) && avail) {
        char buf[512];
        DWORD r = 0;
        if (!ReadFile(rd, buf, sizeof(buf), &r, NULL) || !r) break;
        for (DWORD i = 0; i < r && out && got + 1 < outn; i++) out[got++] = buf[i];
        if (out && outn) out[got] = 0;
    }
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    CloseHandle(rd);
    return (int) code;
}

static int start(const char *exe, char *const argv[], const char *cwd, const char *const env[], const DWORD flags) {
    for (int i = 0; env && env[i]; i++) {
        wchar_t *kv = wide(env[i]);
        wchar_t *eq = kv ? wcschr(kv, L'=') : NULL;
        if (eq) {
            *eq = 0;
            SetEnvironmentVariableW(kv, eq + 1);
        }
        free(kv);
    }
    STARTUPINFOW si;
    memset(&si, 0, sizeof(si));
    si.cb = sizeof(si);
    PROCESS_INFORMATION pi;
    wchar_t *wexe = wide(exe), *cmd = command_line(argv), *wcwd = cwd ? wide(cwd) : NULL;
    const BOOL ok = wexe && cmd && CreateProcessW(wexe, cmd, NULL, NULL, FALSE, flags, NULL, wcwd, &si, &pi);
    free(wexe);
    free(cmd);
    free(wcwd);
    if (!ok) return 0;
    CloseHandle(pi.hProcess);
    CloseHandle(pi.hThread);
    return 1;
}

int kn_proc_start_client(const char *exe, char *const argv[], const char *cwd, const char *const env[], char *err, const size_t errn) {
    if (start(exe, argv, cwd, env, 0)) return 1;
    char msg[64];
    snprintf(msg, sizeof(msg), "CreateProcess error %lu", (unsigned long) GetLastError());
    set_err(err, errn, msg, exe);
    return 0;
}

int kn_proc_spawn_detached(const char *exe, char *const argv[], const char *cwd) {
    return start(exe, argv, cwd, NULL, DETACHED_PROCESS | CREATE_NEW_PROCESS_GROUP);
}

#else

int kn_proc_run(const char *exe, char *const argv[], const int timeout_ms, char *out, const size_t outn) {
    if (out && outn) out[0] = 0;
    int fds[2];
    if (pipe(fds) != 0) return -1;
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 1);
    posix_spawn_file_actions_adddup2(&fa, fds[1], 2);
    posix_spawn_file_actions_addclose(&fa, fds[0]);
    pid_t pid;
    const int rc = posix_spawn(&pid, exe, &fa, NULL, argv, environ);
    posix_spawn_file_actions_destroy(&fa);
    close(fds[1]);
    if (rc != 0) {
        close(fds[0]);
        return -1;
    }
    size_t got = 0;
    int elapsed = 0, status = 0, done = 0;
    while (!done) {
        struct pollfd p = {fds[0], POLLIN, 0};
        if (poll(&p, 1, 50) > 0) {
            char buf[512];
            const ssize_t r = read(fds[0], buf, sizeof(buf));
            for (ssize_t i = 0; i < r && out && got + 1 < outn; i++) out[got++] = buf[i];
            if (out && outn) out[got] = 0;
        } else {
            elapsed += 50;
        }
        const pid_t w = waitpid(pid, &status, WNOHANG);
        if (w == pid) done = 1;
        else if (elapsed >= timeout_ms) {
            kill(pid, SIGKILL);
            waitpid(pid, &status, 0);
            close(fds[0]);
            return -1;
        }
    }
    for (;;) { // the rest of the output
        char buf[512];
        const ssize_t r = read(fds[0], buf, sizeof(buf));
        if (r <= 0) break;
        for (ssize_t i = 0; i < r && out && got + 1 < outn; i++) out[got++] = buf[i];
        if (out && outn) out[got] = 0;
    }
    close(fds[0]);
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
}

int kn_proc_start_client(const char *exe, char *const argv[], const char *cwd, const char *const env[], char *err, const size_t errn) {
    if (cwd && chdir(cwd) != 0) {
        set_err(err, errn, "cannot enter", cwd);
        return 0;
    }
    for (int i = 0; env && env[i]; i++) {
        char kv[KN_PATH_MAX];
        snprintf(kv, sizeof(kv), "%s", env[i]);
        char *eq = strchr(kv, '=');
        if (!eq) continue;
        *eq = 0;
        setenv(kv, eq + 1, 1);
    }
    fflush(NULL);
    execv(exe, argv);
    set_err(err, errn, strerror(errno), exe);
    return 0;
}

int kn_proc_spawn_detached(const char *exe, char *const argv[], const char *cwd) {
    posix_spawnattr_t attr;
    posix_spawnattr_init(&attr);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETSID);
#else
    posix_spawnattr_setflags(&attr, POSIX_SPAWN_SETPGROUP);
    posix_spawnattr_setpgroup(&attr, 0);
#endif
    char old[KN_PATH_MAX] = "";
    if (cwd && getcwd(old, sizeof(old)) && chdir(cwd) != 0) old[0] = 0;
    pid_t pid;
    const int rc = posix_spawn(&pid, exe, NULL, &attr, argv, environ);
    if (old[0] && chdir(old) != 0) {}
    posix_spawnattr_destroy(&attr);
    return rc == 0;
}
#endif
