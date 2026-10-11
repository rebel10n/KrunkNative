#pragma once
#include <server.h>
#include <server_log.h>

// Interactive server console (replxx) and process lifecycle helpers shared by
// main.c and cli.c.
//
// Threads: main (game tick + shutdown), net (net_main), and, when stdin/stdout
// are a terminal, cli (replxx_input loop). Only the main thread performs
// shutdown; every other path just records a stop reason and sets
// g_server->should_quit.

typedef enum {
    SERVER_STOP_NONE = 0,
    SERVER_STOP_COMMAND,   // quit / exit / stop typed at the prompt
    SERVER_STOP_EOF,       // Ctrl+D on an empty prompt
    SERVER_STOP_INTERRUPT, // Ctrl+C typed at the prompt (terminal in raw mode, no SIGINT)
    SERVER_STOP_SIGNAL,    // SIGINT / SIGTERM / SIGHUP / Windows console control event
    SERVER_STOP_ERROR,     // network thread failure or another fatal error
} ServerStopReason;

typedef struct {
    float measured_tick_rate; // ticks per second over the last measurement window
    float max_tick_ms;        // slowest tick in the last measurement window
} ServerTickStats;

// Implemented in net.c with external linkage but not declared in server.h.
// Caller holds g_server->lock.
void net_remove_client(ClientConnection *client);

extern const ServerVersion g_server_version;
extern ServerTickStats g_server_tick_stats; // guarded by g_server->lock

// Record why the server is stopping (first reason wins) and set should_quit.
void server_request_stop(ServerStopReason reason, const char *detail);
ServerStopReason server_stop_reason(const char **detail);

// Start the console. interactive != 0 starts the replxx prompt thread (stdin and
// stdout must be terminals); otherwise the server runs headless and the console
// only logs. Returns 0 on success.
int cli_start(int interactive);
int cli_is_interactive(void);

// Wake the prompt (if it is blocked in replxx_input) and join the CLI thread.
// Safe to call more than once and in headless mode. The terminal is back in
// cooked mode when this returns.
void cli_stop(void);

// Save history and release replxx (replxx_end). Call after every thread that
// may print through g_replxx has been joined.
void cli_shutdown(void);
