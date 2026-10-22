/*
 * klog.h - the debug output level of client_bin and server_bin.
 *
 *   level 0 (default)  errors, warnings and the lines a player needs (startup banner, connection
 *                      established or lost, a user-visible failure). Plain printf, always on.
 *   level 1 (--debug)  debug: lifecycle and state changes (window opened or closed, account sign-in
 *                      or out, lobby joined, round start and end, backend and probe results, Servers list counts).
 *   level 2 (--debug=2) verbose: everything else (per packet, per frame, per click, per HTTP request,
 *                      layout and paint traces, asset loads, skin and paint details).
 *
 * Set by, in order of precedence: the command line (--debug[=N] or -d N, a bare --debug is 1), the
 * environment (KRUNKNATIVE_DEBUG=N), then the default 0. The brain (kn_brain) has its own knsvc/log.h
 * levels and reads the same flag and variable.
 *
 * Use KN_LOG_DEBUG1(...) for level-1 lines and KN_LOG_VERBOSE2(...) for level-2 lines; both are printf.
 */
#ifndef KN_KLOG_H
#define KN_KLOG_H

#include <stdio.h>

#define KN_DEBUG_LEVEL_MAX 2

extern int g_kn_debug_level;

/* KRUNKNATIVE_DEBUG=N, read once at startup (before the command line). Invalid values are ignored. */
void kn_debug_init_env(void);

/* Parses one command-line argument. Returns 0 when arg is not a debug flag, 1 when arg alone was
 * consumed (--debug, --debug=N), 2 when arg and next were consumed (-d N), and -1 on a bad level
 * (a message goes to stderr). next may be NULL. */
int kn_debug_parse_arg(const char *arg, const char *next);

#define KN_LOG_DEBUG1(...) do { if (g_kn_debug_level >= 1) printf(__VA_ARGS__); } while (0)
#define KN_LOG_VERBOSE2(...) do { if (g_kn_debug_level >= 2) printf(__VA_ARGS__); } while (0)

/* The usage lines both binaries print under --help. */
#define KN_DEBUG_USAGE \
    "  --debug[=N]     debug output: 0 (default) errors, warnings and the player lines; 1 (--debug) adds\n" \
    "                  lifecycle and state changes; 2 (--debug=2, -d 2) adds everything else (packets,\n" \
    "                  frames, clicks, HTTP). Also KRUNKNATIVE_DEBUG=N; the flag wins over the variable\n"

#endif
