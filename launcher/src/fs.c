// Launcher file system layer (launcher/include/kn_fs.h).
#include <kn_fs.h>

#include <auth_join.h> // KnSha256

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <limits.h>
#include <sys/stat.h>
#include <unistd.h>
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif
#endif

int kn_fs_join(char *out, const size_t n, const char *a, const char *b) {
    const size_t la = strlen(a);
    const int slash = la && a[la - 1] != '/' && a[la - 1] != '\\';
    const int w = snprintf(out, n, "%s%s%s", a, slash ? "/" : "", b);
    return w > 0 && (size_t) w < n;
}

void kn_fs_dirname(char *path) {
    char *slash = strrchr(path, '/');
#ifdef _WIN32
    char *bs = strrchr(path, '\\');
    if (!slash || (bs && bs > slash)) slash = bs;
#endif
    if (!slash) {
        strcpy(path, ".");
        return;
    }
    if (slash == path) slash[1] = 0;
    else *slash = 0;
}

int kn_fs_mkdirs_parent(const char *file_path) {
    char dir[4096];
    snprintf(dir, sizeof(dir), "%s", file_path);
    kn_fs_dirname(dir);
    return kn_fs_mkdirs(dir);
}

#ifdef _WIN32
// ------------------------------------------------------------------ Windows (wide calls)

static wchar_t *wide(const char *s) {
    const int n = MultiByteToWideChar(CP_UTF8, 0, s, -1, NULL, 0);
    if (n <= 0) return NULL;
    wchar_t *w = malloc((size_t) n * sizeof(wchar_t));
    if (w) MultiByteToWideChar(CP_UTF8, 0, s, -1, w, n);
    for (wchar_t *p = w; p && *p; p++)
        if (*p == L'/') *p = L'\\';
    return w;
}

static int narrow(const wchar_t *w, char *out, const size_t n) {
    const int r = WideCharToMultiByte(CP_UTF8, 0, w, -1, out, (int) n, NULL, NULL);
    if (r <= 0) return 0;
    for (char *p = out; *p; p++)
        if (*p == '\\') *p = '/';
    return 1;
}

static DWORD attrs(const char *path) {
    wchar_t *w = wide(path);
    const DWORD a = w ? GetFileAttributesW(w) : INVALID_FILE_ATTRIBUTES;
    free(w);
    return a;
}

int kn_fs_exists(const char *path) { return attrs(path) != INVALID_FILE_ATTRIBUTES; }

int kn_fs_is_dir(const char *path) {
    const DWORD a = attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

int kn_fs_is_symlink(const char *path) {
    const DWORD a = attrs(path);
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_REPARSE_POINT);
}

int64_t kn_fs_size(const char *path) {
    wchar_t *w = wide(path);
    WIN32_FILE_ATTRIBUTE_DATA d;
    const int ok = w && GetFileAttributesExW(w, GetFileExInfoStandard, &d);
    free(w);
    if (!ok || (d.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) return -1;
    return (int64_t) d.nFileSizeHigh << 32 | d.nFileSizeLow;
}

static int mkdir_one(const char *path) {
    wchar_t *w = wide(path);
    const int ok = w && (CreateDirectoryW(w, NULL) || GetLastError() == ERROR_ALREADY_EXISTS);
    free(w);
    return ok;
}

int kn_fs_remove(const char *path) {
    wchar_t *w = wide(path);
    if (!w) return 0;
    const DWORD a = GetFileAttributesW(w);
    int ok = 1;
    if (a != INVALID_FILE_ATTRIBUTES) {
        if (a & FILE_ATTRIBUTE_READONLY) SetFileAttributesW(w, a & ~FILE_ATTRIBUTE_READONLY);
        ok = (a & FILE_ATTRIBUTE_DIRECTORY) ? RemoveDirectoryW(w) : DeleteFileW(w);
    }
    free(w);
    return ok;
}

int kn_fs_rename(const char *from, const char *to) {
    wchar_t *a = wide(from), *b = wide(to);
    const int ok = a && b && MoveFileExW(a, b, MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED);
    free(a);
    free(b);
    return ok;
}

FILE *kn_fs_fopen(const char *path, const char *mode) {
    wchar_t *w = wide(path), wm[8];
    MultiByteToWideChar(CP_UTF8, 0, mode, -1, wm, 8);
    FILE *f = w ? _wfopen(w, wm) : NULL;
    free(w);
    return f;
}

int kn_fs_set_executable(const char *path, const int on) {
    (void) path;
    (void) on;
    return 1;
}

int kn_fs_symlink(const char *target, const char *link_path) {
    (void) target;
    (void) link_path;
    return 0;
}

static int walk_rec(const char *root, const char *rel, KnFsWalkFn fn, void *user) {
    char dir[4096], pattern[4200];
    if (rel[0]) kn_fs_join(dir, sizeof(dir), root, rel);
    else snprintf(dir, sizeof(dir), "%s", root);
    kn_fs_join(pattern, sizeof(pattern), dir, "*");
    wchar_t *w = wide(pattern);
    WIN32_FIND_DATAW fd;
    HANDLE h = w ? FindFirstFileW(w, &fd) : INVALID_HANDLE_VALUE;
    free(w);
    if (h == INVALID_HANDLE_VALUE) return 1;
    int ok = 1;
    do {
        if (!wcscmp(fd.cFileName, L".") || !wcscmp(fd.cFileName, L"..")) continue;
        char name[1024], child[4096];
        if (!narrow(fd.cFileName, name, sizeof(name))) continue;
        if (rel[0]) kn_fs_join(child, sizeof(child), rel, name);
        else snprintf(child, sizeof(child), "%s", name);
        const int is_dir = (fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) && !(fd.dwFileAttributes & FILE_ATTRIBUTE_REPARSE_POINT);
        if (!fn(child, is_dir, user)) ok = 0;
        else if (is_dir && !walk_rec(root, child, fn, user)) ok = 0;
    } while (ok && FindNextFileW(h, &fd));
    FindClose(h);
    return ok;
}

int kn_fs_self_exe(char *out, const size_t n) {
    wchar_t w[MAX_PATH * 4];
    const DWORD len = GetModuleFileNameW(NULL, w, MAX_PATH * 4);
    return len > 0 && len < MAX_PATH * 4 && narrow(w, out, n);
}

int kn_fs_absolute(const char *path, char *out, const size_t n) {
    wchar_t *w = wide(path), full[MAX_PATH * 4];
    const DWORD len = w ? GetFullPathNameW(w, MAX_PATH * 4, full, NULL) : 0;
    free(w);
    return len > 0 && len < MAX_PATH * 4 && narrow(full, out, n);
}

#else
// ------------------------------------------------------------------ POSIX

FILE *kn_fs_fopen(const char *path, const char *mode) {
    return fopen(path, mode);
}

int kn_fs_exists(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0;
}

int kn_fs_is_dir(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int kn_fs_is_symlink(const char *path) {
    struct stat st;
    return lstat(path, &st) == 0 && S_ISLNK(st.st_mode);
}

int64_t kn_fs_size(const char *path) {
    struct stat st;
    if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) return -1;
    return (int64_t) st.st_size;
}

static int mkdir_one(const char *path) {
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

int kn_fs_remove(const char *path) {
    struct stat st;
    if (lstat(path, &st) != 0) return errno == ENOENT;
    return S_ISDIR(st.st_mode) ? rmdir(path) == 0 : unlink(path) == 0;
}

int kn_fs_rename(const char *from, const char *to) {
    return rename(from, to) == 0;
}

int kn_fs_set_executable(const char *path, const int on) {
    struct stat st;
    if (stat(path, &st) != 0) return 0;
    mode_t mode = st.st_mode & 07777;
    mode = on ? (mode | ((mode & 0444) >> 2)) : (mode & ~(mode_t) 0111);
    return chmod(path, mode) == 0;
}

int kn_fs_symlink(const char *target, const char *link_path) {
    return symlink(target, link_path) == 0;
}

static int walk_rec(const char *root, const char *rel, KnFsWalkFn fn, void *user) {
    char dir[4096];
    if (rel[0]) kn_fs_join(dir, sizeof(dir), root, rel);
    else snprintf(dir, sizeof(dir), "%s", root);
    DIR *d = opendir(dir);
    if (!d) return 1;
    int ok = 1;
    struct dirent *e;
    while (ok && (e = readdir(d))) {
        if (!strcmp(e->d_name, ".") || !strcmp(e->d_name, "..")) continue;
        char child[4096], full[4096];
        if (rel[0]) kn_fs_join(child, sizeof(child), rel, e->d_name);
        else snprintf(child, sizeof(child), "%s", e->d_name);
        kn_fs_join(full, sizeof(full), root, child);
        struct stat st;
        const int is_dir = lstat(full, &st) == 0 && S_ISDIR(st.st_mode);
        if (!fn(child, is_dir, user)) ok = 0;
        else if (is_dir && !walk_rec(root, child, fn, user)) ok = 0;
    }
    closedir(d);
    return ok;
}

int kn_fs_self_exe(char *out, const size_t n) {
#ifdef __APPLE__
    char raw[4096];
    uint32_t size = sizeof(raw);
    if (_NSGetExecutablePath(raw, &size) != 0) return 0;
    char real[PATH_MAX];
    if (!realpath(raw, real)) return 0;
    return snprintf(out, n, "%s", real) < (int) n;
#else
    const ssize_t len = readlink("/proc/self/exe", out, n - 1);
    if (len <= 0 || (size_t) len >= n - 1) return 0;
    out[len] = 0;
    return 1;
#endif
}

int kn_fs_absolute(const char *path, char *out, const size_t n) {
    char tmp[PATH_MAX * 2];
    if (path[0] == '/') {
        snprintf(tmp, sizeof(tmp), "%s", path);
    } else {
        char cwd[PATH_MAX];
        if (!getcwd(cwd, sizeof(cwd))) return 0;
        snprintf(tmp, sizeof(tmp), "%s/%s", cwd, path);
    }
    // collapse "." / ".." / "//" (no symlink resolution: the path may not exist yet)
    char res[PATH_MAX * 2];
    size_t rl = 0;
    for (const char *p = tmp; *p;) {
        while (*p == '/') p++;
        if (!*p) break;
        const char *seg = p;
        while (*p && *p != '/') p++;
        const size_t sl = (size_t) (p - seg);
        if (sl == 1 && seg[0] == '.') continue;
        if (sl == 2 && seg[0] == '.' && seg[1] == '.') {
            while (rl > 0 && res[rl - 1] != '/') rl--;
            if (rl > 0) rl--;
            continue;
        }
        if (rl + 1 + sl + 1 >= sizeof(res)) return 0;
        res[rl++] = '/';
        memcpy(res + rl, seg, sl);
        rl += sl;
    }
    if (!rl) res[rl++] = '/';
    res[rl] = 0;
    return snprintf(out, n, "%s", res) < (int) n;
}
#endif

// ------------------------------------------------------------------ common

#define FOPEN kn_fs_fopen

int kn_fs_mkdirs(const char *path) {
    if (!path || !*path) return 0;
    if (kn_fs_is_dir(path)) return 1;
    char buf[4096];
    if (snprintf(buf, sizeof(buf), "%s", path) >= (int) sizeof(buf)) return 0;
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        if (p[-1] == ':' ) continue; // C:/
        const char c = *p;
        *p = 0;
        if (!kn_fs_is_dir(buf)) mkdir_one(buf);
        *p = c;
    }
    return mkdir_one(buf) && kn_fs_is_dir(buf);
}

typedef struct {
    const char *root;
    char **dirs;
    size_t ndirs;
    int ok;
} RmCtx;

static int rm_visit(const char *rel, const int is_dir, void *user) {
    RmCtx *c = user;
    char full[4096];
    kn_fs_join(full, sizeof(full), c->root, rel);
    if (is_dir) {
        char **grown = realloc(c->dirs, (c->ndirs + 1) * sizeof(*grown));
        if (!grown) return 0;
        c->dirs = grown;
        c->dirs[c->ndirs++] = strdup(full);
    } else if (!kn_fs_remove(full)) {
        c->ok = 0;
    }
    return 1;
}

int kn_fs_rmtree(const char *path) {
    if (!kn_fs_exists(path)) return 1;
    if (!kn_fs_is_dir(path)) return kn_fs_remove(path);
    RmCtx c = {.root = path, .ok = 1};
    if (!walk_rec(path, "", rm_visit, &c)) c.ok = 0;
    for (size_t i = c.ndirs; i-- > 0;) { // deepest first (pre-order list reversed)
        if (c.dirs[i] && !kn_fs_remove(c.dirs[i])) c.ok = 0;
        free(c.dirs[i]);
    }
    free(c.dirs);
    return kn_fs_remove(path) && c.ok;
}

int kn_fs_walk(const char *root, const KnFsWalkFn fn, void *user) {
    return walk_rec(root, "", fn, user);
}

int kn_fs_dir_writable(const char *dir) {
    char probe[4096];
    if (!kn_fs_join(probe, sizeof(probe), dir, ".kn-write-probe")) return 0;
    FILE *f = FOPEN(probe, "wb");
    if (!f) return 0;
    fclose(f);
    kn_fs_remove(probe);
    return 1;
}

int kn_fs_read(const char *path, char **data, size_t *len, const size_t max) {
    *data = NULL;
    if (len) *len = 0;
    FILE *f = FOPEN(path, "rb");
    if (!f) return 0;
    size_t cap = 65536, n = 0;
    char *buf = malloc(cap + 1);
    while (buf) {
        if (n == cap) {
            if (cap >= max) break;
            cap = cap * 2 > max ? max : cap * 2;
            char *grown = realloc(buf, cap + 1);
            if (!grown) {
                free(buf);
                buf = NULL;
                break;
            }
            buf = grown;
        }
        const size_t r = fread(buf + n, 1, cap - n, f);
        if (!r) break;
        n += r;
    }
    const int truncated = buf && n == cap && !feof(f) && fgetc(f) != EOF;
    fclose(f);
    if (!buf || truncated) {
        free(buf);
        return 0;
    }
    buf[n] = 0;
    *data = buf;
    if (len) *len = n;
    return 1;
}

int kn_fs_write_atomic(const char *path, const void *data, const size_t len) {
    char tmp[4096];
    if (snprintf(tmp, sizeof(tmp), "%s.tmp", path) >= (int) sizeof(tmp)) return 0;
    FILE *f = FOPEN(tmp, "wb");
    if (!f) return 0;
    const int ok = fwrite(data, 1, len, f) == len;
    if (fclose(f) != 0 || !ok) {
        kn_fs_remove(tmp);
        return 0;
    }
    if (!kn_fs_rename(tmp, path)) {
        kn_fs_remove(tmp);
        return 0;
    }
    return 1;
}

int kn_fs_sha256(const char *path, char out_hex[65]) {
    FILE *f = FOPEN(path, "rb");
    if (!f) return 0;
    KnSha256 s;
    kn_sha256_init(&s);
    const size_t cap = 1 << 16;
    unsigned char *buf = malloc(cap);
    if (!buf) {
        fclose(f);
        return 0;
    }
    size_t r;
    while ((r = fread(buf, 1, cap, f)) > 0) kn_sha256_update(&s, buf, r);
    const int err = ferror(f);
    fclose(f);
    free(buf);
    if (err) return 0;
    uint8_t digest[32];
    kn_sha256_final(&s, digest);
    kn_sha256_hex(digest, out_hex);
    return 1;
}
