// kn_editor_mcp helpers: growable strings, base64, files, paths, the log ring, JSON getters.
#pragma once
#include <cJSON.h>
#include <stddef.h>
#include <stdarg.h>

typedef struct {
    char *buf;
    size_t len, cap;
    int oom;
} Sb;

void sb_putc(Sb *sb, char c);
void sb_puts(Sb *sb, const char *s);
void sb_putn(Sb *sb, const char *s, size_t n);
void sb_printf(Sb *sb, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
char *sb_take(Sb *sb); // malloc'd, "" when empty; NULL on OOM. Resets sb.
void sb_free(Sb *sb);

char *base64_encode(const unsigned char *data, size_t len, size_t *out_len);
unsigned char *base64_decode(const char *text, size_t *out_len); // NULL on bad input

// Whole file into a malloc'd NUL-terminated buffer; NULL on error (errno kept).
char *file_read(const char *path, size_t *len);
// Writes atomically (temp file + rename). 0 on success.
int file_write(const char *path, const char *data, size_t len);
int file_exists(const char *path);
int dir_exists(const char *path);
int mkdir_p(const char *path);
double now_seconds(void);   // monotonic
double wall_seconds(void);  // epoch

// realpath() of path, or of its parent + basename when the file does not exist yet.
// 0 on success.
int path_resolve(const char *path, char *out, size_t size);
// 1 when resolved path `p` is `root` or inside it.
int path_inside(const char *p, const char *root);
const char *path_basename(const char *p);

// log ring (`logs` tool, stderr mirror)
typedef enum { LOG_DEBUG, LOG_INFO, LOG_WARN, LOG_ERROR } LogLevel;
void log_msg(LogLevel level, const char *source, const char *fmt, ...) __attribute__((format(printf, 3, 4)));
void log_lines(const char *source, LogLevel level, const char *text); // one entry per line
// Newest-last JSON array of {t, level, source, msg} for entries at >= level from source ("" any).
cJSON *log_tail(const char *source, LogLevel min_level, int max);
const char *log_level_name(LogLevel level);
int log_level_parse(const char *s); // -1 unknown
void log_set_stderr(int enabled);
// Optional forwarder (MCP notifications/message); called for every entry.
void log_set_sink(void (*sink)(LogLevel level, const char *source, const char *msg));

// JSON getters with defaults
double js_num(const cJSON *obj, const char *key, double def);
int js_int(const cJSON *obj, const char *key, int def);
int js_bool(const cJSON *obj, const char *key, int def);
const char *js_str(const cJSON *obj, const char *key, const char *def);
const cJSON *js_get(const cJSON *obj, const char *key); // case-sensitive
int js_vec3(const cJSON *arr, double out[3]);           // 1 when a 3-number array
cJSON *js_vec3_new(const double v[3]);
int js_truthy(const cJSON *v);                          // JS truthiness
double js_round(double v, int decimals);
