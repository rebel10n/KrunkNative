// `ksc run`: compile a KrunkScript and run hooks against a minimal host (doc/v7/krunkscript.md
// 6.6). Every GAME.* entry outside the shared stdlib (log, UTILS, UTILS.math, TIME.now /
// getReadable) is a gated no-op returning undefined.
//
//   ksc run [--side client|server] [--hooks LIST] [--repeat N] [--seed N] [--stats] FILE
//
// FILE is a .krnk source (raw or base64, as in map JSON). LIST is a comma-separated list of
// hook calls, each `name[:arg[:arg...]]`; an arg is a number, true/false, or a string
// (e.g. `start,update:16,onPlayerSpawn:7`). Default `start`. --repeat runs the list N times.
// GAME.log lines print to stdout; runtime errors print as "!RUNTIME ERROR: ..." on stdout.
// --stats prints the wall time per hook call to stderr. Exit: 0 ok, 1 compile error or the
// script failed to load, 2 usage/IO.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <sys/timeb.h>
#endif

#include "ks.h"
#include "ks_import_js.h"

int ksc_run(int argc, char **argv);

static void run_log(void *user, int is_error, const char *line)
{
    (void)user;
    printf("%s%s\n", is_error ? "!" : "", line);
}

// Wall clock in ms. MinGW's C runtime has no C11 timespec_get/TIME_UTC: _ftime64 there.
static double wall_ms(void)
{
#ifdef _WIN32
    struct __timeb64 tb;
    _ftime64(&tb);
    return (double)tb.time * 1000.0 + (double)tb.millitm;
#else
    struct timespec ts;
    timespec_get(&ts, TIME_UTC);
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1e6;
#endif
}

static double run_now(void *user)
{
    (void)user;
    return (double)(long long)wall_ms(); // whole ms, as before
}

static double mono_ms(void)
{
    return wall_ms();
}

static char *slurp(const char *path, size_t *len)
{
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long n = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (n < 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)n + 1);
    if (!buf) { fclose(fp); return NULL; }
    *len = fread(buf, 1, (size_t)n, fp);
    buf[*len] = 0;
    fclose(fp);
    return buf;
}

static int run_usage(void)
{
    fprintf(stderr, "usage: ksc run [--side client|server] [--hooks start,update:16,...] [--repeat N] [--seed N] [--stats] FILE\n");
    return 2;
}

// Calls one "name:arg:arg" hook spec.
static void call_spec(KsScript *s, const char *spec, int stats)
{
    char buf[256];
    snprintf(buf, sizeof buf, "%s", spec);
    KsValue args[16];
    int argc = 0;
    char *name = buf, *p = strchr(buf, ':');
    if (p) *p++ = 0;
    while (p && argc < 16) {
        char *next = strchr(p, ':');
        if (next) *next++ = 0;
        char *end;
        double d = strtod(p, &end);
        if (*p && !*end) args[argc++] = ks_num(d);
        else if (!strcmp(p, "true")) args[argc++] = ks_bool(1);
        else if (!strcmp(p, "false")) args[argc++] = ks_bool(0);
        else args[argc++] = ks_str(s, p, strlen(p));
        p = next;
    }
    double t0 = mono_ms();
    ks_script_call_hook(s, name, args, argc);
    if (stats) fprintf(stderr, "ksc run: %s %.3f ms\n", name, mono_ms() - t0);
}

int ksc_run(int argc, char **argv)
{
    KsSide side = KS_SIDE_CLIENT;
    const char *hooks = "start", *file = NULL;
    int repeat = 1, stats = 0;
    unsigned long long seed = 0;
    int has_seed = 0;
    for (int i = 0; i < argc; i++) {
        if (!strcmp(argv[i], "--side") && i + 1 < argc) side = strcmp(argv[++i], "server") == 0 ? KS_SIDE_SERVER : KS_SIDE_CLIENT;
        else if (!strcmp(argv[i], "--hooks") && i + 1 < argc) hooks = argv[++i];
        else if (!strcmp(argv[i], "--repeat") && i + 1 < argc) repeat = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--seed") && i + 1 < argc) { seed = strtoull(argv[++i], NULL, 10); has_seed = 1; }
        else if (!strcmp(argv[i], "--stats")) stats = 1;
        else if (argv[i][0] == '-') return run_usage();
        else file = argv[i];
    }
    if (!file) return run_usage();
    size_t len;
    char *raw = slurp(file, &len);
    if (!raw) {
        fprintf(stderr, "ksc: cannot read %s\n", file);
        return 2;
    }
    size_t slen;
    char *src = ks_decode_script(raw, len, &slen);
    free(raw);
    if (!src) return 2;
    if (ks_is_compiled_js(src, slen)) {
        // compiled JS runs through the importer (WP7), as in the game
        KsDiag ierr = {0};
        size_t ilen = 0;
        char *imported = ks_import_js(src, slen, side, &ilen, &ierr);
        free(src);
        if (!imported) {
            char msg[300];
            ks_diag_format(&ierr, msg, sizeof msg);
            fprintf(stderr, "%s: error: %s: %s\n", file, KS_COMPILED_JS_MESSAGE, msg);
            return 1;
        }
        src = imported;
        slen = ilen;
    }
    KsDiag err = {0};
    KsProgram *prog = ks_compile(src, slen, side, &err, NULL, 0, NULL);
    free(src);
    if (!prog) {
        char msg[300];
        ks_diag_format(&err, msg, sizeof msg);
        fprintf(stderr, "%s: error: %s\n", file, msg);
        return 1;
    }
    KsHost host = {side, run_log, run_now, NULL};
    double t0 = mono_ms();
    KsScript *s = ks_script_new(prog, &host, NULL);
    if (stats) fprintf(stderr, "ksc run: load %.3f ms\n", mono_ms() - t0);
    if (has_seed) ks_script_seed(s, seed);
    int rc = ks_script_failed(s) ? 1 : 0;
    for (int r = 0; r < repeat && !ks_script_failed(s); r++) {
        char list[1024];
        snprintf(list, sizeof list, "%s", hooks);
        for (char *spec = strtok(list, ","); spec; spec = strtok(NULL, ",")) {
            call_spec(s, spec, stats);
        }
    }
    ks_script_free(s);
    ks_program_free(prog);
    return rc;
}
