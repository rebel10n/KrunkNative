#define _GNU_SOURCE
#include "util.h"
#include <errno.h>
#include <limits.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

// ---------------------------------------------------------------- Sb

static int sb_grow(Sb *sb, const size_t extra) {
    if (sb->oom) return 0;
    if (sb->len + extra + 1 <= sb->cap) return 1;
    size_t cap = sb->cap ? sb->cap : 256;
    while (cap < sb->len + extra + 1) cap *= 2;
    char *b = realloc(sb->buf, cap);
    if (!b) {
        sb->oom = 1;
        return 0;
    }
    sb->buf = b;
    sb->cap = cap;
    return 1;
}

void sb_putn(Sb *sb, const char *s, const size_t n) {
    if (!sb_grow(sb, n)) return;
    memcpy(sb->buf + sb->len, s, n);
    sb->len += n;
    sb->buf[sb->len] = 0;
}

void sb_putc(Sb *sb, const char c) { sb_putn(sb, &c, 1); }
void sb_puts(Sb *sb, const char *s) { if (s) sb_putn(sb, s, strlen(s)); }

void sb_printf(Sb *sb, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    char small[512];
    const int n = vsnprintf(small, sizeof(small), fmt, ap);
    va_end(ap);
    if (n < 0) return;
    if ((size_t) n < sizeof(small)) {
        sb_putn(sb, small, (size_t) n);
        return;
    }
    if (!sb_grow(sb, (size_t) n)) return;
    va_start(ap, fmt);
    vsnprintf(sb->buf + sb->len, (size_t) n + 1, fmt, ap);
    va_end(ap);
    sb->len += (size_t) n;
}

char *sb_take(Sb *sb) {
    if (sb->oom) {
        free(sb->buf);
        memset(sb, 0, sizeof(*sb));
        return NULL;
    }
    char *b = sb->buf ? sb->buf : strdup("");
    memset(sb, 0, sizeof(*sb));
    return b;
}

void sb_free(Sb *sb) {
    free(sb->buf);
    memset(sb, 0, sizeof(*sb));
}

// ---------------------------------------------------------------- base64

static const char B64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

char *base64_encode(const unsigned char *data, const size_t len, size_t *out_len) {
    const size_t n = 4 * ((len + 2) / 3);
    char *out = malloc(n + 1);
    if (!out) return NULL;
    size_t o = 0;
    for (size_t i = 0; i < len; i += 3) {
        const unsigned v = (unsigned) data[i] << 16 | (i + 1 < len ? (unsigned) data[i + 1] << 8 : 0) |
                           (i + 2 < len ? data[i + 2] : 0);
        out[o++] = B64[(v >> 18) & 63];
        out[o++] = B64[(v >> 12) & 63];
        out[o++] = i + 1 < len ? B64[(v >> 6) & 63] : '=';
        out[o++] = i + 2 < len ? B64[v & 63] : '=';
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}

static int b64_value(const char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+' || c == '-') return 62;
    if (c == '/' || c == '_') return 63;
    return -1;
}

unsigned char *base64_decode(const char *text, size_t *out_len) {
    const size_t n = strlen(text);
    unsigned char *out = malloc(n / 4 * 3 + 4);
    if (!out) return NULL;
    size_t o = 0;
    unsigned acc = 0;
    int bits = 0;
    for (size_t i = 0; i < n; i++) {
        const char c = text[i];
        if (c == '=' || c == '\n' || c == '\r' || c == ' ') continue;
        const int v = b64_value(c);
        if (v < 0) {
            free(out);
            return NULL;
        }
        acc = acc << 6 | (unsigned) v;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out[o++] = (unsigned char) (acc >> bits & 0xff);
        }
    }
    out[o] = 0;
    if (out_len) *out_len = o;
    return out;
}

// ---------------------------------------------------------------- files

char *file_read(const char *path, size_t *len) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    Sb sb = {0};
    char buf[65536];
    size_t n;
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) sb_putn(&sb, buf, n);
    const int err = ferror(f);
    fclose(f);
    if (err) {
        sb_free(&sb);
        return NULL;
    }
    if (len) *len = sb.len;
    return sb_take(&sb);
}

int file_write(const char *path, const char *data, const size_t len) {
    char tmp[PATH_MAX + 32];
    snprintf(tmp, sizeof(tmp), "%s.tmp%d", path, (int) getpid());
    FILE *f = fopen(tmp, "wb");
    if (!f) return -1;
    const size_t w = fwrite(data, 1, len, f);
    const int bad = w != len || fflush(f) != 0;
    fclose(f);
    if (bad || rename(tmp, path) != 0) {
        unlink(tmp);
        return -1;
    }
    return 0;
}

int file_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISREG(st.st_mode);
}

int dir_exists(const char *path) {
    struct stat st;
    return stat(path, &st) == 0 && S_ISDIR(st.st_mode);
}

int mkdir_p(const char *path) {
    char buf[PATH_MAX];
    snprintf(buf, sizeof(buf), "%s", path);
    for (char *p = buf + 1; *p; p++) {
        if (*p != '/') continue;
        *p = 0;
        if (mkdir(buf, 0755) && errno != EEXIST) return -1;
        *p = '/';
    }
    if (mkdir(buf, 0755) && errno != EEXIST) return -1;
    return 0;
}

double now_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

double wall_seconds(void) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec * 1e-9;
}

int path_resolve(const char *path, char *out, const size_t size) {
    char tmp[PATH_MAX];
    if (realpath(path, tmp)) {
        snprintf(out, size, "%s", tmp);
        return 0;
    }
    // not existing yet: resolve the parent directory
    char parent[PATH_MAX];
    snprintf(parent, sizeof(parent), "%s", path);
    char *slash = strrchr(parent, '/');
    const char *base = path;
    if (slash) {
        *slash = 0;
        base = slash + 1;
        if (!parent[0]) snprintf(parent, sizeof(parent), "/");
    } else {
        snprintf(parent, sizeof(parent), ".");
    }
    if (!base[0] || !strcmp(base, ".") || !strcmp(base, "..")) return -1;
    if (!realpath(parent, tmp)) return -1;
    const int n = snprintf(out, size, "%s/%s", strcmp(tmp, "/") ? tmp : "", base);
    return n < 0 || (size_t) n >= size ? -1 : 0;
}

int path_inside(const char *p, const char *root) {
    const size_t n = strlen(root);
    if (!n) return 0;
    if (strncmp(p, root, n)) return 0;
    return p[n] == 0 || p[n] == '/' || root[n - 1] == '/';
}

const char *path_basename(const char *p) {
    const char *s = strrchr(p, '/');
    return s ? s + 1 : p;
}

// ---------------------------------------------------------------- log ring

#define LOG_RING 2000

typedef struct {
    double t;
    LogLevel level;
    char source[16];
    char *msg;
} LogEntry;

static LogEntry g_log[LOG_RING];
static size_t g_log_next, g_log_count;
static int g_log_stderr = 1;
static void (*g_log_sink)(LogLevel, const char *, const char *);
static int g_in_sink;

void log_set_sink(void (*sink)(LogLevel, const char *, const char *)) { g_log_sink = sink; }

void log_set_stderr(const int enabled) { g_log_stderr = enabled; }

const char *log_level_name(const LogLevel level) {
    static const char *names[] = {"debug", "info", "warning", "error"};
    return level >= LOG_DEBUG && level <= LOG_ERROR ? names[level] : "info";
}

int log_level_parse(const char *s) {
    if (!s) return -1;
    if (!strcmp(s, "debug")) return LOG_DEBUG;
    if (!strcmp(s, "info") || !strcmp(s, "notice")) return LOG_INFO;
    if (!strcmp(s, "warning") || !strcmp(s, "warn")) return LOG_WARN;
    if (!strcmp(s, "error") || !strcmp(s, "critical") || !strcmp(s, "alert") || !strcmp(s, "emergency")) return LOG_ERROR;
    return -1;
}

static void log_push(const LogLevel level, const char *source, const char *msg) {
    LogEntry *e = &g_log[g_log_next];
    free(e->msg);
    e->t = wall_seconds();
    e->level = level;
    snprintf(e->source, sizeof(e->source), "%s", source ? source : "mcp");
    e->msg = strdup(msg);
    g_log_next = (g_log_next + 1) % LOG_RING;
    if (g_log_count < LOG_RING) g_log_count++;
    if (g_log_sink && !g_in_sink) {
        g_in_sink = 1;
        g_log_sink(level, e->source, msg);
        g_in_sink = 0;
    }
    if (g_log_stderr && level >= LOG_INFO)
        fprintf(stderr, "[kn_editor_mcp:%s] %s%s%s\n", e->source, level >= LOG_WARN ? log_level_name(level) : "", level >= LOG_WARN ? ": " : "", msg);
}

void log_msg(const LogLevel level, const char *source, const char *fmt, ...) {
    char buf[2048];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    log_push(level, source, buf);
}

void log_lines(const char *source, const LogLevel level, const char *text) {
    if (!text) return;
    const int saved = g_log_stderr;
    g_log_stderr = 0; // subprocess output: ring only
    const char *s = text;
    while (*s) {
        const char *e = strchr(s, '\n');
        const size_t n = e ? (size_t) (e - s) : strlen(s);
        if (n) {
            char line[1024];
            snprintf(line, sizeof(line), "%.*s", (int) (n < sizeof(line) - 1 ? n : sizeof(line) - 1), s);
            log_push(level, source, line);
        }
        s += n + (e ? 1 : 0);
    }
    g_log_stderr = saved;
}

cJSON *log_tail(const char *source, const LogLevel min_level, int max) {
    cJSON *arr = cJSON_CreateArray();
    if (max <= 0) max = 100;
    // walk newest -> oldest, collect, then reverse
    const LogEntry *pick[LOG_RING];
    int n = 0;
    for (size_t i = 0; i < g_log_count && n < max; i++) {
        const size_t idx = (g_log_next + LOG_RING - 1 - i) % LOG_RING;
        const LogEntry *e = &g_log[idx];
        if (e->level < min_level) continue;
        if (source && source[0] && strcmp(source, "all") && strcmp(e->source, source)) continue;
        pick[n++] = e;
    }
    for (int i = n - 1; i >= 0; i--) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "t", js_round(pick[i]->t, 3));
        cJSON_AddStringToObject(o, "level", log_level_name(pick[i]->level));
        cJSON_AddStringToObject(o, "source", pick[i]->source);
        cJSON_AddStringToObject(o, "msg", pick[i]->msg ? pick[i]->msg : "");
        cJSON_AddItemToArray(arr, o);
    }
    return arr;
}

// ---------------------------------------------------------------- JSON getters

const cJSON *js_get(const cJSON *obj, const char *key) {
    return obj && cJSON_IsObject(obj) ? cJSON_GetObjectItemCaseSensitive(obj, key) : NULL;
}

double js_num(const cJSON *obj, const char *key, const double def) {
    const cJSON *v = js_get(obj, key);
    if (cJSON_IsNumber(v)) return v->valuedouble;
    if (cJSON_IsBool(v)) return cJSON_IsTrue(v);
    if (cJSON_IsString(v) && v->valuestring[0]) {
        char *end;
        const double d = strtod(v->valuestring, &end);
        if (end != v->valuestring) return d;
    }
    return def;
}

int js_int(const cJSON *obj, const char *key, const int def) {
    const double d = js_num(obj, key, def);
    return isfinite(d) ? (int) d : def;
}

int js_bool(const cJSON *obj, const char *key, const int def) {
    const cJSON *v = js_get(obj, key);
    if (!v || cJSON_IsNull(v)) return def;
    return js_truthy(v);
}

const char *js_str(const cJSON *obj, const char *key, const char *def) {
    const cJSON *v = js_get(obj, key);
    return cJSON_IsString(v) ? v->valuestring : def;
}

int js_vec3(const cJSON *arr, double out[3]) {
    if (!cJSON_IsArray(arr) || cJSON_GetArraySize(arr) < 3) return 0;
    for (int i = 0; i < 3; i++) {
        const cJSON *c = cJSON_GetArrayItem(arr, i);
        if (cJSON_IsNumber(c)) out[i] = c->valuedouble;
        else if (cJSON_IsString(c)) out[i] = strtod(c->valuestring, NULL);
        else return 0;
    }
    return 1;
}

cJSON *js_vec3_new(const double v[3]) {
    cJSON *a = cJSON_CreateArray();
    for (int i = 0; i < 3; i++) cJSON_AddItemToArray(a, cJSON_CreateNumber(v[i]));
    return a;
}

int js_truthy(const cJSON *v) {
    if (!v || cJSON_IsNull(v) || cJSON_IsFalse(v)) return 0;
    if (cJSON_IsNumber(v)) return v->valuedouble != 0 && !isnan(v->valuedouble);
    if (cJSON_IsString(v)) return v->valuestring[0] != 0;
    return 1;
}

double js_round(const double v, const int decimals) {
    const double m = pow(10.0, decimals);
    const double r = round(v * m) / m;
    return r == 0.0 ? 0.0 : r; // no -0
}
