// Crash capture: the report a native crash leaves behind (client/include/crash_report.h).
//
// The handler runs in signal context, so it only uses write(2), open(2), backtrace() (warmed up
// at startup so it never loads a library there) and the strings built at startup: no malloc, no
// stdio, no locks. The log part reads the ring without its lock; a line being written during the
// crash may come out cut short, which is acceptable for a diagnostic.
#define _GNU_SOURCE 1 // dladdr (glibc)
#include <crash_report.h>

#include <build_info.h>
#include <flavor.h>

#ifdef __EMSCRIPTEN__

void crash_report_init(void) {}
void crash_report_log_line(const char *text) { (void) text; }

#else

#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef _WIN32
#include <direct.h>
#include <fcntl.h>
#include <io.h>
#include <process.h>
#include <sys/stat.h>
#include <windows.h>
#else
#include <dlfcn.h>
#include <execinfo.h>
#if defined(__linux__)
#include <link.h>
#endif
#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>
#endif

#define CRASH_PATH_MAX 1024
#define CRASH_LINE_MAX 240   // one log line kept (cut at this length)
#define CRASH_FRAMES_MAX 48  // stack frames per thread
#define CRASH_STACK_BYTES (64 * 1024) // the alternate signal stack (a stack overflow still reports)

// ---------------------------------------------------------------- state (written at startup)

static struct {
    char dir[CRASH_PATH_MAX];        // <config root>/crash
    char crash_path[CRASH_PATH_MAX]; // <dir>/crash-<stamp>-<pid>.txt, created by a crash
    char latest_path[CRASH_PATH_MAX]; // <dir>/latest.txt
    char pid_line[32];               // "pid: N\n"
    const char *build;               // kn_build_string(), static
} g_report;

// The in-game console log: the last CRASH_LOG_LINES lines, oldest first from the head.
static pthread_mutex_t g_log_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_log_lines[CRASH_LOG_LINES][CRASH_LINE_MAX];
static volatile unsigned long g_log_total; // lines ever kept; the newest is at (total - 1) % lines

static int g_in_report; // a crash inside the report itself: the next signal just ends the process

// ---------------------------------------------------------------- async-signal-safe output

static void put_bytes(const int fd, const char *s, const size_t n) {
    size_t left = n;
    while (left > 0) {
#ifdef _WIN32
        const int written = _write(fd, s, (unsigned int) left);
        if (written <= 0) return;
#else
        const ssize_t written = write(fd, s, left);
        if (written < 0 && errno == EINTR) continue;
        if (written <= 0) return;
#endif
        s += written;
        left -= (size_t) written;
    }
}

static void put(const int fd, const char *s) {
    if (s) put_bytes(fd, s, strlen(s));
}

static void put_hex(const int fd, const uintptr_t value) {
    static const char digits[] = "0123456789abcdef";
    char buf[2 + sizeof(uintptr_t) * 2 + 1];
    size_t i = sizeof(buf) - 1;
    buf[i] = 0;
    uintptr_t v = value;
    do {
        buf[--i] = digits[v & 15];
        v >>= 4;
    } while (v && i > 2);
    buf[--i] = 'x';
    buf[--i] = '0';
    put(fd, buf + i);
}

static void put_uint(const int fd, const unsigned long value) {
    char buf[24];
    size_t i = sizeof(buf) - 1;
    buf[i] = 0;
    unsigned long v = value;
    do {
        buf[--i] = (char) ('0' + v % 10);
        v /= 10;
    } while (v);
    put(fd, buf + i);
}

static int open_report(const char *path, const int truncate) {
#ifdef _WIN32
    return _open(path, _O_WRONLY | _O_CREAT | _O_BINARY | (truncate ? _O_TRUNC : _O_APPEND), _S_IREAD | _S_IWRITE);
#else
    return open(path, O_WRONLY | O_CREAT | (truncate ? O_TRUNC : O_APPEND), 0600);
#endif
}

static void close_report(const int fd) {
    if (fd < 0) return;
#ifdef _WIN32
    _close(fd);
#else
    close(fd);
#endif
}

// ---------------------------------------------------------------- the log part

// A word or run that is a credential: the keywords, a Krunker account id ("u_" + 22 id characters)
// and a long run of letters and digits (a key, a hash, a signed URL).
static int id_char(const char c) {
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-';
}

static int may_hold_credential(const char *text) {
    static const char *const words[] = {"token", "pass", "secret", "cookie", "bearer", "authoriz", "credential",
                                        "session", "apikey", "api key", "api_key", "proxy", "account id",
                                        "accountid", "account_id", "private key"};
    char low[CRASH_LINE_MAX];
    size_t n = 0;
    for (const char *p = text; *p && n < sizeof(low) - 1; p++) {
        const char c = *p;
        low[n++] = (c >= 'A' && c <= 'Z') ? (char) (c - 'A' + 'a') : c;
    }
    low[n] = 0;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); i++) {
        if (strstr(low, words[i])) return 1;
    }
    for (const char *p = text; *p; p++) {
        if (p[0] != 'u' || p[1] != '_') continue;
        int ok = 1;
        for (int k = 0; k < 22 && ok; k++) ok = p[2 + k] && id_char(p[2 + k]);
        if (ok) return 1;
    }
    size_t run = 0;
    int digit = 0, letter = 0;
    for (const char *p = text;; p++) {
        const char c = *p;
        if (c && (id_char(c) || c == '+' || c == '/' || c == '=')) {
            run++;
            digit |= c >= '0' && c <= '9';
            letter |= (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            if (run >= 32 && digit && letter) return 1;
        } else {
            run = 0;
            digit = letter = 0;
        }
        if (!c) break;
    }
    return 0;
}

void crash_report_log_line(const char *text) {
    if (!text) return;
    char line[CRASH_LINE_MAX];
    size_t n = 0;
    for (const char *p = text; *p && n < sizeof(line) - 1; p++) {
        const unsigned char c = (unsigned char) *p;
        line[n++] = c < 32 || c == 127 ? ' ' : (char) c;
    }
    line[n] = 0;
    if (may_hold_credential(line)) snprintf(line, sizeof(line), "[line withheld: it may hold a credential]");

    pthread_mutex_lock(&g_log_lock);
    const unsigned long slot = g_log_total % CRASH_LOG_LINES;
    snprintf(g_log_lines[slot], CRASH_LINE_MAX, "%s", line);
    g_log_total++;
    pthread_mutex_unlock(&g_log_lock);
}

static void write_log(const int fd) {
    put(fd, "log (the last lines of the in-game console, oldest first; credential-like lines withheld):\n");
    const unsigned long total = g_log_total;
    const unsigned long kept = total < CRASH_LOG_LINES ? total : CRASH_LOG_LINES;
    if (!kept) put(fd, "  (no console lines yet)\n");
    for (unsigned long i = 0; i < kept; i++) {
        put(fd, "  ");
        put(fd, g_log_lines[(total - kept + i) % CRASH_LOG_LINES]);
        put(fd, "\n");
    }
}

// ---------------------------------------------------------------- the stack

#if defined(__linux__)
// The load bias of the object holding pc (dl_iterate_phdr): 0 for a non-PIE executable, so the
// offset is then the link-time address addr2line wants in both cases.
struct FrameBias {
    uintptr_t pc;
    uintptr_t bias;
    int found;
};

static int frame_bias_cb(struct dl_phdr_info *info, size_t size, void *data) {
    (void) size;
    struct FrameBias *frame = (struct FrameBias *) data;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        const ElfW(Phdr) *ph = &info->dlpi_phdr[i];
        if (ph->p_type != PT_LOAD) continue;
        const uintptr_t start = info->dlpi_addr + ph->p_vaddr;
        if (frame->pc >= start && frame->pc < start + ph->p_memsz) {
            frame->bias = info->dlpi_addr;
            frame->found = 1;
            return 1;
        }
    }
    return 0;
}
#endif

// The calling thread's frames: "<module>(+0x<offset>) [0x<address>]" (offset: from the module's
// link-time base, for addr2line / atos; CaptureStackBackTrace and the module of each address on
// Windows).
static void write_frames(const int fd) {
#ifdef _WIN32
    void *frames[CRASH_FRAMES_MAX];
    const USHORT count = CaptureStackBackTrace(0, CRASH_FRAMES_MAX, frames, NULL);
    for (USHORT i = 0; i < count; i++) {
        HMODULE module = NULL;
        char name[MAX_PATH] = "?";
        uintptr_t base = 0;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                               (LPCSTR) frames[i], &module) && module) {
            GetModuleFileNameA(module, name, sizeof(name));
            base = (uintptr_t) module;
        }
        put(fd, "  ");
        put(fd, name);
        put(fd, "(+");
        put_hex(fd, (uintptr_t) frames[i] - base);
        put(fd, ") [");
        put_hex(fd, (uintptr_t) frames[i]);
        put(fd, "]\n");
    }
#else
    // dladdr gives the module and the offset from its base even when the binary exports no
    // symbols (glibc's backtrace_symbols_fd then prints an empty "()").
    void *frames[CRASH_FRAMES_MAX];
    const int count = backtrace(frames, CRASH_FRAMES_MAX);
    for (int i = 0; i < count; i++) {
        const uintptr_t pc = (uintptr_t) frames[i];
        Dl_info info;
        put(fd, "  ");
        if (dladdr(frames[i], &info) && info.dli_fname && info.dli_fbase) {
            uintptr_t base = (uintptr_t) info.dli_fbase;
#if defined(__linux__)
            struct FrameBias frame = {pc, 0, 0};
            dl_iterate_phdr(frame_bias_cb, &frame);
            if (frame.found) base = frame.bias;
#endif
            put(fd, info.dli_fname);
            put(fd, "(+");
            put_hex(fd, pc - base);
            put(fd, ")");
            if (info.dli_sname) {
                put(fd, " ");
                put(fd, info.dli_sname);
            }
        } else {
            put(fd, "(unknown module)");
        }
        put(fd, " [");
        put_hex(fd, pc);
        put(fd, "]\n");
    }
#endif
}

static const char *const SYMBOLIZE_NOTE =
    "\nsymbolize: a frame \"client_bin(+0x1234) [0x...]\" is at offset 0x1234 of its module. Use the\n"
    "build of the report's commit: addr2line -f -e client_bin 0x1234 (macOS: atos -o client_bin\n"
    "-l <load address of the module> <address>; Windows: the .pdb of the same build).\n";

// ---------------------------------------------------------------- the crash report

static void set_latest(const char *report_path) {
    const int fd = open_report(g_report.latest_path, 1);
    if (fd < 0) return;
    put(fd, report_path);
    put(fd, "\n");
    close_report(fd);
}

// The report of a fatal signal (POSIX) or an exception (Windows: number = the exception code).
static void write_crash_report(const char *name, const unsigned long number, const int has_addr, const void *addr,
                               const char *origin) {
    if (__atomic_exchange_n(&g_in_report, 1, __ATOMIC_SEQ_CST)) return;
    const int fd = open_report(g_report.crash_path, 0);
    if (fd < 0) return;
    put(fd, "KrunkNative crash report\n");
    put(fd, "build: ");
    put(fd, g_report.build);
    put(fd, "\n");
    put(fd, g_report.pid_line);
    put(fd, "signal: ");
    put(fd, name);
    put(fd, " (");
    put_uint(fd, number);
    put(fd, ")\n");
    if (has_addr) {
        put(fd, "fault address: ");
        put_hex(fd, (uintptr_t) addr);
        put(fd, "\n");
    }
    if (origin) {
        put(fd, "origin: ");
        put(fd, origin);
        put(fd, "\n");
    }
    put(fd, "\n");
    write_log(fd);
    put(fd, "\nstack of the thread that crashed (innermost first):\n");
    write_frames(fd);
    put(fd, SYMBOLIZE_NOTE);
    close_report(fd);
    set_latest(g_report.crash_path);
}

#ifndef _WIN32

static const char *signal_name(const int signo) {
    switch (signo) {
        case SIGSEGV: return "SIGSEGV";
        case SIGBUS: return "SIGBUS";
        case SIGILL: return "SIGILL";
        case SIGABRT: return "SIGABRT";
        case SIGFPE: return "SIGFPE";
        default: return "signal";
    }
}

static void crash_signal_handler(const int signo, siginfo_t *info, void *context) {
    (void) context;
    // si_code > 0: raised by the hardware (a fault at si_addr); 0 or less: sent by a process (kill, raise).
    const int fault = (signo == SIGSEGV || signo == SIGBUS || signo == SIGILL || signo == SIGFPE) && info && info->si_code > 0;
    write_crash_report(signal_name(signo), (unsigned long) signo, fault, info ? info->si_addr : NULL,
                       info && info->si_code <= 0 ? "sent to the process by kill, raise or a debugger (no fault)" : NULL);
    // The default action for the same signal (the exit status, a core dump): unblock it, since the
    // handler runs with it blocked.
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, signo);
    sigprocmask(SIG_UNBLOCK, &set, NULL);
    raise(signo);
    _exit(128 + signo);
}

static void install_crash_handlers(void) {
    // A stack overflow leaves no room for the handler: it runs on its own stack.
    stack_t stack;
    memset(&stack, 0, sizeof(stack));
    stack.ss_sp = malloc(CRASH_STACK_BYTES);
    stack.ss_size = CRASH_STACK_BYTES;
    if (stack.ss_sp && sigaltstack(&stack, NULL) != 0) free(stack.ss_sp);

    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_sigaction = crash_signal_handler;
    sigemptyset(&action.sa_mask);
    action.sa_flags = SA_SIGINFO | SA_ONSTACK | SA_RESETHAND;
    sigaction(SIGSEGV, &action, NULL);
    sigaction(SIGBUS, &action, NULL);
    sigaction(SIGILL, &action, NULL);
    sigaction(SIGABRT, &action, NULL);
    sigaction(SIGFPE, &action, NULL);
}

#else

static const char *exception_name(const DWORD code) {
    switch (code) {
        case EXCEPTION_ACCESS_VIOLATION: return "EXCEPTION_ACCESS_VIOLATION";
        case EXCEPTION_IN_PAGE_ERROR: return "EXCEPTION_IN_PAGE_ERROR";
        case EXCEPTION_ILLEGAL_INSTRUCTION: return "EXCEPTION_ILLEGAL_INSTRUCTION";
        case EXCEPTION_INT_DIVIDE_BY_ZERO: return "EXCEPTION_INT_DIVIDE_BY_ZERO";
        case EXCEPTION_STACK_OVERFLOW: return "EXCEPTION_STACK_OVERFLOW";
        default: return "exception";
    }
}

static LONG WINAPI crash_exception_filter(EXCEPTION_POINTERS *info) {
    const DWORD code = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionCode : 0;
    const void *addr = info && info->ExceptionRecord ? info->ExceptionRecord->ExceptionAddress : NULL;
    write_crash_report(exception_name(code), code, 1, addr, NULL);
    return EXCEPTION_CONTINUE_SEARCH; // the system's own handling (the error dialog, WER) follows
}

static void crash_signal_win(const int signo) {
    write_crash_report("signal", (unsigned long) signo, 0, NULL, NULL);
    signal(signo, SIG_DFL);
    raise(signo);
}

static void install_crash_handlers(void) {
    SetUnhandledExceptionFilter(crash_exception_filter);
    signal(SIGABRT, crash_signal_win);
    signal(SIGFPE, crash_signal_win);
    signal(SIGILL, crash_signal_win);
    signal(SIGSEGV, crash_signal_win);
}

#endif

// ---------------------------------------------------------------- startup

// $XDG_CONFIG_HOME/<flavor's storage dir> or ~/.config/<...>: the settings store's directory
// (html_dom_core.c storage_dir).
static int config_root(char *out, const size_t n) {
    char sub[64];
    if (!flavor_storage_subdir(flavor_get(), sub, sizeof(sub))) return 0;
    const char *xdg = getenv("XDG_CONFIG_HOME");
    int written;
    if (xdg && *xdg) {
        written = snprintf(out, n, "%s/%s", xdg, sub);
    } else {
        const char *home = getenv("HOME");
#ifdef _WIN32
        if (!home || !*home) home = getenv("USERPROFILE");
#endif
        if (!home || !*home) home = ".";
        written = snprintf(out, n, "%s/.config/%s", home, sub);
    }
    return written > 0 && (size_t) written < n;
}

// mkdir -p: every missing component (an EEXIST is fine). Startup only, never in a handler.
static void make_dirs(const char *path) {
    char tmp[CRASH_PATH_MAX];
    snprintf(tmp, sizeof(tmp), "%s", path);
    for (char *p = tmp + 1; *p; p++) {
        if (*p != '/' && *p != '\\') continue;
        const char c = *p;
        *p = 0;
#ifdef _WIN32
        _mkdir(tmp);
#else
        mkdir(tmp, 0700);
#endif
        *p = c;
    }
#ifdef _WIN32
    _mkdir(tmp);
#else
    mkdir(tmp, 0700);
#endif
}

void crash_report_init(void) {
    char root[CRASH_PATH_MAX];
    if (!config_root(root, sizeof(root))) return;
    if (snprintf(g_report.dir, sizeof(g_report.dir), "%s/crash", root) <= 0) return;
    make_dirs(g_report.dir);

    char stamp[32] = "00000000-000000";
    const time_t now = time(NULL);
    const struct tm *local = localtime(&now);
    if (local) strftime(stamp, sizeof(stamp), "%Y%m%d-%H%M%S", local);
    const long pid =
#ifdef _WIN32
        (long) _getpid();
#else
        (long) getpid();
#endif
    snprintf(g_report.crash_path, sizeof(g_report.crash_path), "%s/crash-%s-%ld.txt", g_report.dir, stamp, pid);
    snprintf(g_report.latest_path, sizeof(g_report.latest_path), "%s/latest.txt", g_report.dir);
    snprintf(g_report.pid_line, sizeof(g_report.pid_line), "pid: %ld\n", pid);
    g_report.build = kn_build_string();

#ifndef _WIN32
    // Warm backtrace() up now: its first call loads the unwinder, which must not happen in a handler.
    void *warm[1];
    backtrace(warm, 1);
#endif
    install_crash_handlers();
}

#endif // __EMSCRIPTEN__
