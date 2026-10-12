#include <server_log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static ServerLogWriter g_log_writer;
static void *g_log_writer_user;
static FILE *g_log_file;
static _Thread_local char *g_capture_buf;
static _Thread_local size_t g_capture_size;

int server_log_open_file(const char *path) {
    FILE *f = path && path[0] ? fopen(path, "a") : NULL;
    pthread_mutex_lock(&g_log_lock);
    if (g_log_file) fclose(g_log_file);
    g_log_file = f;
    pthread_mutex_unlock(&g_log_lock);
    return path && path[0] && !f ? -1 : 0;
}

void server_log_capture_begin(char *buf, const size_t size) {
    g_capture_buf = buf;
    g_capture_size = size;
    if (buf && size) buf[0] = 0;
}

void server_log_capture_end(void) {
    g_capture_buf = NULL;
    g_capture_size = 0;
}

static void server_log_to_file(const char *line) {
    if (!g_log_file) return;
    char stamp[32];
    const time_t now = time(NULL);
    struct tm tm;
#ifdef _WIN32
    localtime_s(&tm, &now);
#else
    localtime_r(&now, &tm);
#endif
    strftime(stamp, sizeof(stamp), "%Y-%m-%d %H:%M:%S", &tm);
    fprintf(g_log_file, "%s %s", stamp, line);
    fflush(g_log_file);
}

void server_log_set_writer(const ServerLogWriter writer, void *user) {
    pthread_mutex_lock(&g_log_lock);
    g_log_writer = writer;
    g_log_writer_user = writer ? user : NULL;
    pthread_mutex_unlock(&g_log_lock);
}

static void server_log_write(FILE *stream, const char *fmt, va_list args) {
    char stack_buffer[512];
    char *buffer = stack_buffer;
    va_list copy;

    va_copy(copy, args);
    int length = vsnprintf(stack_buffer, sizeof(stack_buffer), fmt, copy);
    va_end(copy);

    if (length < 0) return;

    // +2: room for an appended newline and the terminator.
    if ((size_t) length + 2 > sizeof(stack_buffer)) {
        buffer = malloc((size_t) length + 2);
        if (!buffer) return;
        vsnprintf(buffer, (size_t) length + 1, fmt, args);
    }

    if (!length || buffer[length - 1] != '\n') {
        buffer[length++] = '\n';
        buffer[length] = 0;
    }

    if (g_capture_buf) {
        const size_t used = strlen(g_capture_buf);
        if (used + 1 < g_capture_size) snprintf(g_capture_buf + used, g_capture_size - used, "%s", buffer);
    }

    pthread_mutex_lock(&g_log_lock);
    server_log_to_file(buffer);

    if (g_log_writer) {
        g_log_writer(g_log_writer_user, buffer, length);
    } else {
        fputs(buffer, stream);
        fflush(stream);
    }

    pthread_mutex_unlock(&g_log_lock);

    if (buffer != stack_buffer) free(buffer);
}

void server_log(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    server_log_write(stdout, fmt, args);
    va_end(args);
}

void server_log_error(const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    server_log_write(stderr, fmt, args);
    va_end(args);
}

// io_clearLog (v3.1.1 packetHandlers.ts, the mod's F1): console.clear() on a TTY, the raw
// clear sequence otherwise ("\x1Bc" on win32). Through the writer (replxx) while the CLI owns
// the terminal, so the prompt is repainted under the cleared screen.
void server_log_clear(void) {
#ifdef _WIN32
    static const char clear[] = "\x1B" "c";
#else
    static const char clear[] = "\x1B[2J\x1B[3J\x1B[H";
#endif
    pthread_mutex_lock(&g_log_lock);
    if (g_log_writer) {
        g_log_writer(g_log_writer_user, clear, (int) sizeof(clear) - 1);
    } else {
        fputs(clear, stdout);
        fflush(stdout);
    }
    pthread_mutex_unlock(&g_log_lock);
}
