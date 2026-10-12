// ksc: KrunkScript command-line tool (doc/v7/krunkscript.md 6.6).
//
//   ksc check [--side client|server] [--json] [--quiet] FILE...
//
// FILE is a .krnk source file or a map .json (its scripts.client / scripts.server,
// base64 or raw, are checked with their own side). `ksc FILE...` = `ksc check`.
// Text output, one line per diagnostic:
//   <file>[:<side>]:<line>:<col>: error: <message>
//   <file>[:<side>]:<line>:<col>: warning: <message>
// --json prints one object: {"ok": bool, "scripts": [{"file", "side", "status":
// "ok"|"error"|"compiled_js"|"missing", "errors": [{"line","col","message"}],
// "warnings": [...]}]}. Exit status: 0 no errors, 1 compile errors, 2 usage/IO.
// (map_compile in tools/editor-mcp calls `ksc check --json <map.json>`.)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <cJSON.h>

#include "ks.h"

#define KSC_MAX_WARNINGS 64

typedef struct {
    int json;
    int quiet;
    int first_json;
    int errors;
} Out;

static char *slurp(const char *path, size_t *len) {
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    size_t cap = 1 << 16, n = 0;
    char *buf = malloc(cap + 1);
    while (buf) {
        size_t r = fread(buf + n, 1, cap - n, fp);
        n += r;
        if (n < cap) break;
        char *b2 = realloc(buf, cap * 2 + 1);
        if (!b2) {
            free(buf);
            buf = NULL;
            break;
        }
        buf = b2;
        cap *= 2;
    }
    fclose(fp);
    if (!buf) return NULL;
    buf[n] = 0;
    *len = n;
    return buf;
}

static void json_str(const char *s) {
    putchar('"');
    for (const unsigned char *p = (const unsigned char *) s; *p; p++) {
        if (*p == '"' || *p == '\\') printf("\\%c", *p);
        else if (*p == '\n') fputs("\\n", stdout);
        else if (*p < 0x20) printf("\\u%04x", *p);
        else putchar(*p);
    }
    putchar('"');
}

static void json_diags(const char *key, const KsDiag *d, int n) {
    printf(",\"%s\":[", key);
    for (int i = 0; i < n; i++) {
        printf("%s{\"line\":%d,\"col\":%d,\"message\":", i ? "," : "", d[i].line, d[i].col);
        json_str(d[i].message);
        putchar('}');
    }
    putchar(']');
}

static void text_diag(const char *file, const char *side, const char *kind, const KsDiag *d) {
    if (side) printf("%s:%s:", file, side);
    else printf("%s:", file);
    if (d->line > 0) printf("%d:%d:", d->line, d->col);
    printf(" %s: %s\n", kind, d->message);
}

// Report one script; status "missing" = the map has no script for that side.
static void report(Out *o, const char *file, const char *side_label, KsSide side, const char *text, size_t len,
                   int decode) {
    const char *status = "ok";
    KsDiag err = {0};
    KsDiag warns[KSC_MAX_WARNINGS];
    int nwarn = 0, failed = 0;
    char *src = NULL;
    size_t slen = len;
    if (!text) {
        status = "missing";
    } else {
        src = decode ? ks_decode_script(text, len, &slen) : NULL;
        const char *s = decode ? src : text;
        if (decode && !src) {
            fprintf(stderr, "ksc: out of memory\n");
            exit(2);
        }
        if (ks_is_compiled_js(s, slen)) {
            status = "compiled_js";
        } else if (ks_validate(s, slen, side, &err, warns, KSC_MAX_WARNINGS, &nwarn) != 0) {
            status = "error";
            failed = 1;
            o->errors++;
        }
    }
    if (nwarn > KSC_MAX_WARNINGS) nwarn = KSC_MAX_WARNINGS;
    if (o->json) {
        printf("%s{\"file\":", o->first_json ? "" : ",");
        o->first_json = 0;
        json_str(file);
        printf(",\"side\":\"%s\",\"status\":\"%s\"", side == KS_SIDE_SERVER ? "server" : "client", status);
        json_diags("errors", &err, failed);
        json_diags("warnings", warns, nwarn);
        putchar('}');
    } else if (!o->quiet) {
        for (int i = 0; i < nwarn; i++) text_diag(file, side_label, "warning", &warns[i]);
        if (failed) text_diag(file, side_label, "error", &err);
        if (strcmp(status, "compiled_js") == 0) {
            KsDiag d = {0};
            snprintf(d.message, sizeof d.message, "%s", KS_COMPILED_JS_MESSAGE);
            text_diag(file, side_label, "note", &d);
        }
    } else if (failed) {
        text_diag(file, side_label, "error", &err);
    }
    free(src);
}

static int has_suffix(const char *s, const char *suf) {
    size_t a = strlen(s), b = strlen(suf);
    return a >= b && strcmp(s + a - b, suf) == 0;
}

static int check_file(Out *o, const char *path, int side_opt) {
    size_t len = 0;
    char *buf = slurp(path, &len);
    if (!buf) {
        fprintf(stderr, "ksc: cannot read %s\n", path);
        return -1;
    }
    if (has_suffix(path, ".json")) {
        cJSON *root = cJSON_ParseWithLength(buf, len);
        if (!root) {
            fprintf(stderr, "ksc: %s: invalid JSON\n", path);
            free(buf);
            return -1;
        }
        cJSON *scripts = cJSON_GetObjectItemCaseSensitive(root, "scripts");
        static const char *const keys[2] = {"client", "server"};
        for (int k = 0; k < 2; k++) {
            if (side_opt >= 0 && side_opt != k) continue;
            cJSON *s = cJSON_IsObject(scripts) ? cJSON_GetObjectItemCaseSensitive(scripts, keys[k]) : NULL;
            const char *text = cJSON_IsString(s) ? s->valuestring : NULL;
            if (!text && !o->json) continue;
            report(o, path, keys[k], (KsSide) k, text, text ? strlen(text) : 0, 1);
        }
        cJSON_Delete(root);
    } else {
        KsSide side = side_opt >= 0 ? (KsSide) side_opt : strstr(path, "server") ? KS_SIDE_SERVER : KS_SIDE_CLIENT;
        report(o, path, NULL, side, buf, len, 0);
    }
    free(buf);
    return 0;
}

static int usage(void) {
    fprintf(stderr,
            "usage: ksc check [--side client|server] [--json] [--quiet] FILE...\n"
            "  FILE: a .krnk script (side: --side, else \"server\" when the path contains \"server\")\n"
            "        or a map .json (checks scripts.client and scripts.server)\n"
            "  exit status: 0 ok, 1 compile errors, 2 usage or I/O error\n"
            "       ksc run [--side client|server] [--hooks start,update:16,...] [--repeat N] [--seed N] [--stats] FILE\n"
            "  compiles FILE and calls the hooks against a minimal host (tools/ks/ksc_run.c)\n");
    return 2;
}

static int cmd_check(int argc, char **argv) {
    Out o = {.first_json = 1};
    int side = -1, nfiles = 0, io_error = 0;
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--json") == 0) o.json = 1;
        else if (strcmp(argv[i], "--quiet") == 0) o.quiet = 1;
        else if (strcmp(argv[i], "--side") == 0 && i + 1 < argc) {
            i++;
            if (strcmp(argv[i], "client") == 0) side = KS_SIDE_CLIENT;
            else if (strcmp(argv[i], "server") == 0) side = KS_SIDE_SERVER;
            else return usage();
        } else if (argv[i][0] == '-' && argv[i][1]) return usage();
    }
    if (o.json) printf("{\"scripts\":[");
    for (int i = 0; i < argc; i++) {
        if (strcmp(argv[i], "--side") == 0) { i++; continue; }
        if (argv[i][0] == '-' && argv[i][1]) continue;
        nfiles++;
        if (check_file(&o, argv[i], side) != 0) io_error = 1;
    }
    if (o.json) printf("],\"ok\":%s}\n", o.errors || io_error ? "false" : "true");
    if (nfiles == 0) return usage();
    return io_error ? 2 : o.errors ? 1 : 0;
}

int ksc_run(int argc, char **argv); // tools/ks/ksc_run.c (WP2)

int main(int argc, char **argv) {
    if (argc < 2) return usage();
    if (strcmp(argv[1], "check") == 0) return cmd_check(argc - 2, argv + 2);
    if (strcmp(argv[1], "run") == 0) return ksc_run(argc - 2, argv + 2);
    if (strcmp(argv[1], "help") == 0 || strcmp(argv[1], "--help") == 0) return usage();
    return cmd_check(argc - 1, argv + 1);
}
