/*
 * knsvc/log.h - leveled logging for the services.
 *
 * Lines go to stderr (or a file) as
 *   2026-10-10T12:00:00.123Z INFO  [brain] http: listening on 0.0.0.0:8080
 * or, with JSON output, one object per line:
 *   {"ts":"...","level":"info","svc":"brain","mod":"http","msg":"..."}
 *
 * Environment: KN_LOG_LEVEL=trace|debug|info|warn|error, KN_LOG_FORMAT=text|json,
 * KN_LOG_FILE=<path>. kn_log_init() reads them; config "log.level"/"log.format"/
 * "log.file" can be applied with kn_log_configure() (knsvc/config.h).
 * Thread-safe (one mutex around each line). Never log secrets: use
 * kn_config_dump_redacted() for config dumps.
 */
#ifndef KNSVC_LOG_H
#define KNSVC_LOG_H

#include <stdarg.h>
#include "knsvc/buf.h"

typedef enum kn_log_level {
    KN_LOG_TRACE = 0,
    KN_LOG_DEBUG,
    KN_LOG_INFO,
    KN_LOG_WARN,
    KN_LOG_ERROR,
    KN_LOG_FATAL,
    KN_LOG_OFF
} kn_log_level;

struct kn_config;

/* service: short name printed on every line ("brain", "game", "migrate"). */
void kn_log_init(const char *service);
void kn_log_configure(const struct kn_config *cfg); /* log.level / log.format / log.file */
void kn_log_set_level(kn_log_level level);
kn_log_level kn_log_get_level(void);
void kn_log_set_json(int enabled);
int kn_log_set_file(const char *path);              /* NULL -> stderr; 0 ok, -1 error */
kn_log_level kn_log_level_parse(const char *s, kn_log_level def);
int kn_log_enabled(kn_log_level level);

void kn_log_write(kn_log_level level, const char *module, const char *fmt, ...) KN_PRINTF(3, 4);
void kn_log_vwrite(kn_log_level level, const char *module, const char *fmt, va_list ap);

#define KN_TRACE(mod, ...) do { if (kn_log_enabled(KN_LOG_TRACE)) kn_log_write(KN_LOG_TRACE, mod, __VA_ARGS__); } while (0)
#define KN_DEBUG(mod, ...) do { if (kn_log_enabled(KN_LOG_DEBUG)) kn_log_write(KN_LOG_DEBUG, mod, __VA_ARGS__); } while (0)
#define KN_INFO(mod, ...)  kn_log_write(KN_LOG_INFO, mod, __VA_ARGS__)
#define KN_WARN(mod, ...)  kn_log_write(KN_LOG_WARN, mod, __VA_ARGS__)
#define KN_ERROR(mod, ...) kn_log_write(KN_LOG_ERROR, mod, __VA_ARGS__)
#define KN_FATAL(mod, ...) kn_log_write(KN_LOG_FATAL, mod, __VA_ARGS__)

#endif
