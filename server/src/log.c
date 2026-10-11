#include <server_log.h>
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static Replxx *g_log_replxx;

void server_log_set_replxx(Replxx *replxx) {
    pthread_mutex_lock(&g_log_lock);
    g_log_replxx = replxx;
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

    pthread_mutex_lock(&g_log_lock);

    if (g_log_replxx) {
        replxx_write(g_log_replxx, buffer, length);
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
