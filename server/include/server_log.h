#pragma once
#include <replxx.h>

// Thread-safe server console logging.
//
// While the interactive CLI owns the terminal, lines are routed through
// replxx_write() so replxx can clear the prompt, print the line and repaint the
// prompt (and whatever the user was typing) underneath it. In headless mode, or
// after the CLI has been torn down, lines go straight to stdout.
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

// Route log output through this replxx instance (NULL = plain stdout/stderr).
// Call with NULL before replxx_end().
void server_log_set_replxx(Replxx *replxx);
