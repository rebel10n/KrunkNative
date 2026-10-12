#pragma once
#include <stddef.h>

// Thread-safe server console logging (part of the server core: server_bin and the
// client's in-process sandbox server).
//
// While the interactive CLI owns the terminal, lines are routed through its writer
// (server_cli.h server_log_set_replxx: replxx_write) so replxx can clear the prompt, print
// the line and repaint the prompt (and whatever the user was typing) underneath it. In
// headless mode, or after the CLI has been torn down, lines go straight to stdout.
//
// A trailing newline is appended when the message does not end with one, so
// callers never glue output onto the prompt.

#if defined(__GNUC__) || defined(__clang__)
#define SERVER_LOG_PRINTF(fmt_index, args_index) __attribute__((format(printf, fmt_index, args_index)))
#else
#define SERVER_LOG_PRINTF(fmt_index, args_index)
#endif

void server_log(const char *fmt, ...) SERVER_LOG_PRINTF(1, 2);
void server_log_error(const char *fmt, ...) SERVER_LOG_PRINTF(1, 2);
// Clears the server terminal (Krunker "clearLog", io_clearLog).
void server_log_clear(void);

// Route log output (and server_log_clear) through writer(user, text, length), called with
// the log lock held (NULL = plain stdout/stderr). Call with NULL before the writer's target
// goes away.
typedef void (*ServerLogWriter)(void *user, const char *text, int length);
void server_log_set_writer(ServerLogWriter writer, void *user);

// Also append every line (timestamped) to this file; NULL/"" closes it. Returns 0 or -1.
int server_log_open_file(const char *path);
// Copy the lines this thread logs into buf (NUL-terminated, truncated at size) until
// server_log_capture_end(): rcon replies (server/src/rcon.c) reuse the console commands.
void server_log_capture_begin(char *buf, size_t size);
void server_log_capture_end(void);
