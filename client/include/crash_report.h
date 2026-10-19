#pragma once
// Crash capture (client/src/crash_report.c). A native crash leaves a report in
// <config>/krunknative/crash/ (the config root of the flavor, like the settings store):
//   crash-<YYYYMMDD-HHMMSS>-<pid>.txt   the build, the signal and fault address, the last
//                                       CRASH_LOG_LINES lines of the in-game console log, and the
//                                       raw stack of the faulting thread;
//   latest.txt                          the path of the newest report (the next start reads it).
// Frames print as "<module>(+0x<offset>) [0x<address>]" (glibc, Windows) or with the symbol
// names the binary exports (macOS): symbolize with the module's offset, e.g.
//   addr2line -f -e client_bin 0x1234          (Linux; the build of the report's commit)
//   atos -o client_bin -l <load address> 0x...  (macOS)
// Lines that may hold a credential (a token, a password, a proxy, a Krunker account id) are
// withheld from the log part. Nothing else of the account is written.
#include <stddef.h>

#define CRASH_LOG_LINES 50 // the in-game console log lines a report keeps
#define CRASH_FREEZE_SECONDS 5 // the main loop has not ticked for this long: a freeze report

// Installs the crash handlers (SIGSEGV, SIGBUS, SIGILL, SIGABRT, SIGFPE; the unhandled-exception
// filter on Windows) and reads the latest report. Call once on the main thread, after the flavor
// and XDG_CONFIG_HOME are settled (debug_capture_startup), before the first frame.
void crash_report_init(void);
// One in-game console line (ui_console.c append): kept for the report's log part. Newlines and
// control characters become spaces; a line that may hold a credential is replaced by a note.
void crash_report_log_line(const char *text);
// Once per client frame, first thing (before any frame-cap sleep). The first call arms the
// watchdog, so startup and the page loads are not a freeze.
void crash_report_tick(void);
// Clean shutdown, last thing before main returns: stops the watchdog. The handlers stay.
void crash_report_shutdown(void);
