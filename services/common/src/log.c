/* knsvc/log.h implementation: one line per call, mutex-serialised. */
#include "knsvc/log.h"
#include "knsvc/config.h"

#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/time.h>
#include <time.h>

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static kn_log_level g_level = KN_LOG_NOTICE;
static int g_json;
static FILE *g_file;
static char g_service[32] = "knsvc";

static const char *const LEVEL_NAMES[] = {"trace", "debug", "info", "notice", "warn", "error", "fatal", "off"};

kn_log_level kn_log_level_parse(const char *s, kn_log_level def) {
    if (!s || !*s) return def;
    for (int i = 0; i <= KN_LOG_OFF; i++)
        if (strcasecmp(s, LEVEL_NAMES[i]) == 0) return (kn_log_level)i;
    if (strcasecmp(s, "warning") == 0) return KN_LOG_WARN;
    return def;
}

// N = 0 (NOTICE, the default), 1 (INFO), 2 (DEBUG and TRACE). Returns 1 or 2 when arg was consumed.
static int debug_level_value(const char *s, int *out) {
    if (!s || !*s) return -1;
    char *end = NULL;
    const long v = strtol(s, &end, 10);
    if (end == s || *end || v < 0 || v > 2) return -1;
    *out = (int) v;
    return 0;
}

static void set_debug_level(int level) {
    g_level = level == 2 ? KN_LOG_DEBUG : level == 1 ? KN_LOG_INFO : KN_LOG_NOTICE;
}

int kn_log_debug_arg(const char *arg, const char *next) {
    int level = 0;
    if (!arg) return 0;
    if (!strcmp(arg, "--debug")) { set_debug_level(1); return 1; }
    if (!strncmp(arg, "--debug=", 8)) {
        if (debug_level_value(arg + 8, &level) != 0) return -1;
        set_debug_level(level);
        return 1;
    }
    if (!strcmp(arg, "-d")) {
        if (debug_level_value(next, &level) != 0) return -1;
        set_debug_level(level);
        return 2;
    }
    return 0;
}

void kn_log_debug_env(void) {
    int level = 0;
    const char *env = getenv("KRUNKNATIVE_DEBUG");
    if (env && *env && debug_level_value(env, &level) == 0) set_debug_level(level);
}

void kn_log_init(const char *service) {
    if (service) snprintf(g_service, sizeof g_service, "%s", service);
    g_level = kn_log_level_parse(getenv("KN_LOG_LEVEL"), g_level);
    const char *fmt = getenv("KN_LOG_FORMAT");
    if (fmt) g_json = strcasecmp(fmt, "json") == 0;
    const char *file = getenv("KN_LOG_FILE");
    if (file && *file) kn_log_set_file(file);
}

void kn_log_configure(const kn_config *cfg) {
    if (!cfg) return;
    g_level = kn_log_level_parse(kn_config_str(cfg, "log.level", NULL), g_level);
    const char *fmt = kn_config_str(cfg, "log.format", NULL);
    if (fmt) g_json = strcasecmp(fmt, "json") == 0;
    const char *file = kn_config_str(cfg, "log.file", NULL);
    if (file && *file) kn_log_set_file(file);
}

void kn_log_set_level(kn_log_level level) { g_level = level; }
kn_log_level kn_log_get_level(void) { return g_level; }
void kn_log_set_json(int enabled) { g_json = enabled != 0; }
int kn_log_enabled(kn_log_level level) { return level >= g_level && level < KN_LOG_OFF; }

int kn_log_set_file(const char *path) {
    FILE *f = NULL;
    if (path) {
        f = fopen(path, "a");
        if (!f) return -1;
        setvbuf(f, NULL, _IOLBF, 0);
    }
    pthread_mutex_lock(&g_log_lock);
    if (g_file) fclose(g_file);
    g_file = f;
    pthread_mutex_unlock(&g_log_lock);
    return 0;
}

static void format_time(char *out, size_t cap) {
    struct timeval tv;
    gettimeofday(&tv, NULL);
    struct tm tm;
    time_t secs = tv.tv_sec;
    gmtime_r(&secs, &tm);
    snprintf(out, cap, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday,
             tm.tm_hour, tm.tm_min, tm.tm_sec, (int)(tv.tv_usec / 1000));
}

static void json_escape(kn_buf *b, const char *s) {
    for (; *s; s++) {
        unsigned char c = (unsigned char)*s;
        if (c == '"' || c == '\\') kn_buf_appendf(b, "\\%c", c);
        else if (c == '\n') kn_buf_append_str(b, "\\n");
        else if (c == '\t') kn_buf_append_str(b, "\\t");
        else if (c < 0x20) kn_buf_appendf(b, "\\u%04x", c);
        else kn_buf_append_byte(b, c);
    }
}

void kn_log_vwrite(kn_log_level level, const char *module, const char *fmt, va_list ap) {
    if (!kn_log_enabled(level)) return;
    char ts[40];
    format_time(ts, sizeof ts);
    kn_buf msg = {0};
    kn_buf_vappendf(&msg, fmt, ap);
    kn_buf line = {0};
    if (g_json) {
        kn_buf_appendf(&line, "{\"ts\":\"%s\",\"level\":\"%s\",\"svc\":\"", ts, LEVEL_NAMES[level]);
        json_escape(&line, g_service);
        kn_buf_append_str(&line, "\",\"mod\":\"");
        json_escape(&line, module ? module : "");
        kn_buf_append_str(&line, "\",\"msg\":\"");
        json_escape(&line, msg.data ? msg.data : "");
        kn_buf_append_str(&line, "\"}\n");
    } else {
        char lvl[8];
        snprintf(lvl, sizeof lvl, "%s", LEVEL_NAMES[level]);
        for (char *p = lvl; *p; p++) *p = (char)(*p - 'a' + 'A');
        kn_buf_appendf(&line, "%s %-5s [%s] %s%s%s\n", ts, lvl, g_service, module ? module : "",
                       module ? ": " : "", msg.data ? msg.data : "");
    }
    pthread_mutex_lock(&g_log_lock);
    FILE *out = g_file ? g_file : stderr;
    fwrite(line.data, 1, line.len, out);
    fflush(out);
    pthread_mutex_unlock(&g_log_lock);
    kn_buf_free(&msg);
    kn_buf_free(&line);
}

void kn_log_write(kn_log_level level, const char *module, const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    kn_log_vwrite(level, module, fmt, ap);
    va_end(ap);
}
