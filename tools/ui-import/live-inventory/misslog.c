// LD_PRELOAD shim: logs every fopen/open/stat/access of a path containing "assets/" that fails with
// ENOENT to $MISSLOG ("<call>\t<path>"). Build: gcc -shared -fPIC -O2 -o misslog.so misslog.c -ldl
#define _GNU_SOURCE
#include <dlfcn.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <sys/stat.h>
#include <pthread.h>
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static void logm(const char *how, const char *p, int err) {
    if (!p || err != ENOENT) return;
    if (!strstr(p, "assets/")) return;
    const char *f = getenv("MISSLOG"); if (!f) return;
    pthread_mutex_lock(&mu);
    int sv = errno;
    static int (*ropen)(const char *, int, ...);
    if (!ropen) ropen = dlsym(RTLD_NEXT, "open");
    int fd = ropen(f, O_WRONLY|O_APPEND|O_CREAT, 0644);
    if (fd >= 0) { char b[2048]; int n = snprintf(b, sizeof b, "%s\t%s\n", how, p); if (n > 0) { ssize_t w = write(fd, b, n); (void)w; } close(fd); }
    errno = sv;
    pthread_mutex_unlock(&mu);
}
#define WRAPF(name) FILE *name(const char *p, const char *m) { static FILE *(*r)(const char*, const char*); if(!r) r = dlsym(RTLD_NEXT, #name); FILE *x = r(p, m); if(!x) logm(#name, p, errno); return x; }
WRAPF(fopen)
WRAPF(fopen64)
#define WRAPO(name) int name(const char *p, int fl, ...) { static int (*r)(const char*, int, ...); if(!r) r = dlsym(RTLD_NEXT, #name); mode_t md = 0; if (fl & O_CREAT) { va_list a; va_start(a, fl); md = va_arg(a, int); va_end(a);} int x = r(p, fl, md); if (x < 0) logm(#name, p, errno); return x; }
WRAPO(open)
WRAPO(open64)
int stat(const char *p, struct stat *s) { static int (*r)(const char*, struct stat*); if(!r) r = dlsym(RTLD_NEXT, "stat"); int x = r(p, s); if (x < 0) logm("stat", p, errno); return x; }
int access(const char *p, int m) { static int (*r)(const char*, int); if(!r) r = dlsym(RTLD_NEXT, "access"); int x = r(p, m); if (x < 0) logm("access", p, errno); return x; }
