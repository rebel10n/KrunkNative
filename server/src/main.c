#include <server_cli.h>
#include <pthread.h>
#include <pcg_basic.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef WIN32
#include <windows.h>
#include <io.h>
#define SERVER_ISATTY(stream) _isatty(_fileno(stream))
#else
#include <unistd.h>
#define SERVER_ISATTY(stream) isatty(fileno(stream))
#endif

const ServerVersion g_server_version = {1, 0, 0};
ServerTickStats g_server_tick_stats;

Replxx *g_replxx;
Server *g_server;

// ------------------------------------------------------------------ signals
//
// Handlers only record the signal; the main loop notices it within one tick
// and performs the shutdown (async-signal-safe).

static volatile sig_atomic_t g_pending_signal;

#ifdef WIN32
static volatile LONG g_shutdown_complete;

static BOOL WINAPI server_console_ctrl_handler(const DWORD type) {
    g_pending_signal = (sig_atomic_t) type + 1;

    // Windows terminates the process as soon as these handlers return: give the
    // main thread a few seconds to disconnect clients and restore the console.
    if (type == CTRL_CLOSE_EVENT || type == CTRL_LOGOFF_EVENT || type == CTRL_SHUTDOWN_EVENT) {
        for (int i = 0; i < 50 && !g_shutdown_complete; i++) Sleep(100);
    }

    return TRUE;
}

static const char *server_signal_name(const int pending) {
    switch (pending - 1) {
        case CTRL_C_EVENT: return "Ctrl+C";
        case CTRL_BREAK_EVENT: return "Ctrl+Break";
        case CTRL_CLOSE_EVENT: return "console closed";
        case CTRL_LOGOFF_EVENT: return "logoff";
        case CTRL_SHUTDOWN_EVENT: return "system shutdown";
        default: return "console event";
    }
}

static void server_install_signal_handlers(void) {
    SetConsoleCtrlHandler(server_console_ctrl_handler, TRUE);
}

static void server_block_signals(const int block) {
    (void) block;
}
#else
static void server_on_signal(const int signo) {
    g_pending_signal = signo;
}

static const char *server_signal_name(const int signo) {
    switch (signo) {
        case SIGINT: return "SIGINT";
        case SIGTERM: return "SIGTERM";
        case SIGHUP: return "SIGHUP";
        default: return "signal";
    }
}

static void server_install_signal_handlers(void) {
    struct sigaction action;
    memset(&action, 0, sizeof(action));
    action.sa_handler = server_on_signal;
    sigemptyset(&action.sa_mask);
    action.sa_flags = 0; // no SA_RESTART: interrupt the tick sleep immediately

    sigaction(SIGINT, &action, NULL);
    sigaction(SIGTERM, &action, NULL);
    sigaction(SIGHUP, &action, NULL);

    // A client vanishing mid-send must not kill the server.
    signal(SIGPIPE, SIG_IGN);
}

// Worker threads inherit the mask at creation: block the stop signals while
// spawning them so they are always delivered to the main thread.
static void server_block_signals(const int block) {
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGINT);
    sigaddset(&set, SIGTERM);
    sigaddset(&set, SIGHUP);
    pthread_sigmask(block ? SIG_BLOCK : SIG_UNBLOCK, &set, NULL);
}
#endif

static void server_poll_signals(void) {
    const int pending = g_pending_signal;
    if (!pending) return;

    char detail[64];
    snprintf(detail, sizeof(detail), "received %s", server_signal_name(pending));
    server_request_stop(SERVER_STOP_SIGNAL, detail);
}

// ------------------------------------------------------------------ arguments

static int parse_port(const char *value, unsigned short *port) {
    if (!value || !value[0]) return 0;

    char *end = NULL;
    errno = 0;
    const long parsed = strtol(value, &end, 10);

    if (errno || !end || *end || parsed <= 0 || parsed > 65535) return 0;

    *port = (unsigned short) parsed;
    return 1;
}

typedef struct {
    const char *map_path;
    unsigned short port;
    int headless;
    int help;
    const char *mode;   // --mode: a Krunker mode alias / name / id, or "rotation"
    float round_seconds; // --round-seconds: round length override (debug: quick end screen / rotation tests)
    int perms;          // --perms (or PERMS=1): v3.1.1 PERMS, DEV-only chat commands
} ServerArgs;

static int parse_args(const int argc, char **argv, ServerArgs *args) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];

        if (!strcmp(arg, "--port")) {
            if (++i >= argc || !parse_port(argv[i], &args->port)) return 0;
            continue;
        }

        if (!strncmp(arg, "--port=", 7)) {
            if (!parse_port(arg + 7, &args->port)) return 0;
            continue;
        }

        if (!strcmp(arg, "--mode")) {
            if (++i >= argc) return 0;
            args->mode = argv[i];
            continue;
        }

        if (!strncmp(arg, "--mode=", 7)) {
            args->mode = arg + 7;
            continue;
        }

        if (!strcmp(arg, "--round-seconds") || !strncmp(arg, "--round-seconds=", 16)) {
            const char *value = arg[15] == '=' ? arg + 16 : ++i < argc ? argv[i] : NULL;
            char *end = NULL;
            args->round_seconds = value ? strtof(value, &end) : 0.0f;
            if (!value || !end || *end || args->round_seconds <= 0.0f) return 0;
            continue;
        }

        if (!strcmp(arg, "--perms")) {
            args->perms = 1;
            continue;
        }

        if (!strcmp(arg, "--headless") || !strcmp(arg, "--no-cli")) {
            args->headless = 1;
            continue;
        }

        if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            args->help = 1;
            continue;
        }

        if (arg[0] == '-') return 0;

        if (args->map_path) return 0;
        args->map_path = arg;
    }

    return 1;
}

static void print_usage(FILE *stream, const char *argv0) {
    fprintf(stream,
            "Usage: %s [--port PORT] [--mode MODE] [--headless] [map.json]\n"
            "  --port PORT   TCP port to listen on (default %d)\n"
            "  --mode MODE   game mode: alias (tdm, point, ctf, infect, gun, ...), name or id;\n"
            "                'rotation' plays Krunker's public rotation (default: ffa)\n"
            "  --round-seconds S  round length in seconds instead of the mode's game time (testing)\n"
            "  --perms       restrict DEV chat commands (/kick, /map, /god, ...) to op'd or local clients\n"
            "                (also PERMS=1); by default everyone may run them, like the v3.1.1 server\n"
            "  --headless    no interactive console (implied when stdin/stdout is not a terminal)\n"
            "Stop with 'quit', Ctrl+C, Ctrl+D or SIGTERM.\n",
            argv0, NET_DEFAULT_PORT);
}

// ------------------------------------------------------------------ startup checks

// Bind (without listening) to report "port in use" before any thread or the
// interactive prompt starts.
// Returns 1 when the port can be bound; otherwise fills error.
static int server_port_available(const unsigned short port, char *error, const size_t error_size) {
#ifdef WIN32
    WSADATA wsa_data;
    if (WSAStartup(MAKEWORD(2, 2), &wsa_data)) return 1; // let net_main report it
#endif

    int available = 1;
    const int fd = (int) socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

    if (fd >= 0) {
        struct sockaddr_in addr;
        memset(&addr, 0, sizeof(addr));
        addr.sin_family = AF_INET;
        addr.sin_addr.s_addr = INADDR_ANY;
        addr.sin_port = htons(port);

#ifndef WIN32
        // Ignore TIME_WAIT leftovers from a previous run (only an active
        // listener is a real conflict). On Windows SO_REUSEADDR would also
        // allow binding over a live listener, so it is not used there.
        const int reuse = 1;
        setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
#endif

        available = bind(fd, (struct sockaddr *) &addr, sizeof(addr)) == 0;

#ifdef WIN32
        if (!available) {
            const int err = WSAGetLastError();
            snprintf(error, error_size, "%s (WSA error %d)", err == WSAEADDRINUSE ? "address already in use" : "bind failed", err);
        }
#else
        if (!available) snprintf(error, error_size, "%s", strerror(errno));
#endif

#ifdef WIN32
        closesocket(fd);
#else
        close(fd);
#endif
    }

#ifdef WIN32
    WSACleanup();
#endif
    return available;
}

// net_main() fails (and sets should_quit) right at thread start if it cannot
// create, bind or listen on its socket. Give it a moment before starting the
// prompt so such a failure is reported on a plain terminal, not under replxx.
// Returns 1 if the network thread already stopped the server (it logged why).
static int server_wait_for_net_startup(void) {
    for (int i = 0; i < 50 && !g_server->should_quit; i++) {
#ifdef WIN32
        Sleep(5);
#else
        const struct timespec ts = {0, 5000000L};
        nanosleep(&ts, NULL);
#endif
    }

    return g_server->should_quit && server_stop_reason(NULL) == SERVER_STOP_NONE;
}

// ------------------------------------------------------------------ tick loop

static double timespec_seconds(const struct timespec *ts) {
    return (double) ts->tv_sec + (double) ts->tv_nsec / 1e9;
}

static void server_sleep_until(const double deadline) {
    while (!g_server->should_quit && !g_pending_signal) {
        struct timespec now_ts;
        clock_gettime(CLOCK_MONOTONIC, &now_ts);

        const double remaining = deadline - timespec_seconds(&now_ts);
        if (remaining <= 0.0) return;

#ifdef WIN32
        // Sleep() granularity is ~1-15 ms; spin the last couple of milliseconds.
        if (remaining > 0.002) Sleep((DWORD) ((remaining - 0.002) * 1000.0));
#else
        struct timespec sleep_ts;
        sleep_ts.tv_sec = (time_t) remaining;
        sleep_ts.tv_nsec = (long) ((remaining - (double) sleep_ts.tv_sec) * 1e9);
        nanosleep(&sleep_ts, NULL); // EINTR on a stop signal: loop re-checks the flags
#endif
    }
}

static void server_run_tick_loop(void) {
    struct timespec start_ts;
    clock_gettime(CLOCK_MONOTONIC, &start_ts);

    double last_tick = timespec_seconds(&start_ts);
    double stats_window_start = last_tick;
    unsigned int stats_ticks = 0;
    float stats_max_ms = 0.0f;
    float now = 0;

    while (!g_server->should_quit) {
        server_poll_signals();
        if (g_server->should_quit) break;

        struct timespec tick_ts;
        clock_gettime(CLOCK_MONOTONIC, &tick_ts);

        const double tick_start = timespec_seconds(&tick_ts);
        const float delta = (float) (tick_start - last_tick);

        now += delta;
        last_tick = tick_start;

        pthread_mutex_lock(&g_server->lock);
        g_server->now = now;
        game_tick(&g_server->game, now, delta);
        net_check_idle_clients(now);
        net_broadcast_states();

        clock_gettime(CLOCK_MONOTONIC, &tick_ts);
        const float tick_ms = (float) ((timespec_seconds(&tick_ts) - tick_start) * 1000.0);
        if (tick_ms > stats_max_ms) stats_max_ms = tick_ms;

        // Rate = ticks started inside [window start, this tick) / window length.
        if (tick_start - stats_window_start >= 1.0) {
            g_server_tick_stats.measured_tick_rate = (float) (stats_ticks / (tick_start - stats_window_start));
            g_server_tick_stats.max_tick_ms = stats_max_ms;
            stats_window_start = tick_start;
            stats_ticks = 0;
            stats_max_ms = 0.0f;
        }

        stats_ticks++;

        pthread_mutex_unlock(&g_server->lock);

        const unsigned short tick_rate = g_server->config.tick_rate ? g_server->config.tick_rate : 30;
        server_sleep_until(tick_start + 1.0 / tick_rate);
    }
}

// ------------------------------------------------------------------ shutdown

static void *server_net_thread(void *arg) {
    (void) arg;
    net_main();
    return NULL;
}

static void server_disconnect_all(void) {
    pthread_mutex_lock(&g_server->lock);

    // net_remove_client() closes the socket, frees the player and compacts the array.
    while (g_server->client_count) net_remove_client(&g_server->clients[0]);

    pthread_mutex_unlock(&g_server->lock);
}

static const char *server_stop_description(const ServerStopReason reason, const char *detail) {
    switch (reason) {
        case SERVER_STOP_COMMAND: return "quit command";
        case SERVER_STOP_EOF: return "end of input";
        case SERVER_STOP_INTERRUPT: return "Ctrl+C";
        case SERVER_STOP_SIGNAL: return detail && *detail ? detail : "signal";
        case SERVER_STOP_ERROR: return detail && *detail ? detail : "fatal error";
        default: return "unknown";
    }
}

int main(int argc, char **argv) {
    char *rand_memory = malloc(1);
    pcg32_srandom(time(NULL), (uint64_t) (uintptr_t) rand_memory);
    free(rand_memory);

    ServerArgs args = {.port = NET_DEFAULT_PORT};

    if (!parse_args(argc, argv, &args)) {
        print_usage(stderr, argv[0]);
        return 2;
    }

    if (args.help) {
        print_usage(stdout, argv[0]);
        return 0;
    }

    cJSON *startup_map = NULL;

    if (args.map_path && load_json_file(args.map_path, &startup_map)) {
        fprintf(stderr, "Failed to load map JSON: %s\n", args.map_path);
        return 1;
    }

    server_install_signal_handlers();

    char port_error[128] = "";

    if (!server_port_available(args.port, port_error, sizeof(port_error))) {
        fprintf(stderr, "Cannot listen on port %u: %s\n", (unsigned int) args.port, port_error);
        cJSON_Delete(startup_map);
        return 1;
    }

    static Server INSTANCE = {0};
    g_server = &INSTANCE;

    g_server->config.tick_rate = 30;
    g_server->config.listen_port = args.port;
    g_server->config.max_players = 8;
    g_server->config.idle_timeout = 60.0f;
    g_server->config.idle_move_threshold = 1.0f;
    g_server->config.perms = args.perms || (getenv("PERMS") && !strcmp(getenv("PERMS"), "1"));
    pthread_mutex_init(&g_server->lock, NULL);

    size_t startup_map_count = 0;
    const cJSON **startup_maps = map_list_with_custom(startup_map, &startup_map_count);

    if (startup_map && !startup_maps) {
        fprintf(stderr, "Failed to allocate startup map list\n");
        cJSON_Delete(startup_map);
        return 1;
    }

    // --mode: one mode for every round (Krunker config.modes = [id]), or the public rotation
    int *startup_modes = NULL;
    size_t startup_mode_count = 0;

    if (args.mode) {
        const int rotation = !strcmp(args.mode, "rotation");
        const int mode_id = rotation ? -1 : mode_find(args.mode);

        if (!rotation && mode_id < 0) {
            fprintf(stderr, "Unknown mode '%s' (try ffa, tdm, point, ctf, infect, gun, kc, ...)\n", args.mode);
            cJSON_Delete(startup_map);
            return 2;
        }

        startup_mode_count = rotation ? sizeof(g_rotation_modes) / sizeof(g_rotation_modes[0]) : 1;
        startup_modes = calloc(startup_mode_count, sizeof(int));
        if (!startup_modes) startup_mode_count = 0;
        else if (rotation) memcpy(startup_modes, g_rotation_modes, sizeof(g_rotation_modes));
        else startup_modes[0] = mode_id;
    }

    g_server->game.server = g_server;
    game_configure(&g_server->game, NULL, startup_maps, startup_map_count, startup_modes, startup_mode_count, NULL, 0, NULL, 0);
    g_server->game.round_length = args.round_seconds; // 0: config.game_time
    game_init(&g_server->game, startup_map ? 0 : -1, -1, 0);

    // Interactive only when a human is on both ends; pipes, /dev/null, systemd
    // and redirected logs run headless (no raw mode, no escape sequences).
    const int interactive = !args.headless && SERVER_ISATTY(stdin) && SERVER_ISATTY(stdout);

    // net.c / events.c still print through g_replxx, so it exists in headless
    // mode too; there it is only a printer (replxx_input is never called and
    // the terminal mode is never touched).
    g_replxx = replxx_init();

    server_log("Starting KrunkNative server v%d.%d.%d%s", g_server_version.major, g_server_version.minor, g_server_version.patch,
               interactive ? "" : " (headless)");

    if (g_server->game.ready) {
        const cJSON *name = cJSON_GetObjectItem(g_server->game.maps[g_server->game.current_map_index], "name");
        server_log("Loaded map %s (%d), mode %s (%d)", cJSON_IsString(name) ? name->valuestring : "?",
                   g_server->game.current_map_index, mode_name(game_mode_id(&g_server->game)), game_mode_id(&g_server->game));
    } else {
        server_log_error("No playable map loaded");
    }

    server_block_signals(1);

    pthread_t net_thread;
    const int net_started = pthread_create(&net_thread, NULL, server_net_thread, NULL) == 0;

    if (!net_started) {
        server_request_stop(SERVER_STOP_ERROR, "could not start network thread");
    } else if (!server_wait_for_net_startup()) {
        if (cli_start(interactive)) server_log_error("Interactive console unavailable, running headless");
        if (cli_is_interactive()) server_log("Type 'help' for commands, 'quit' or Ctrl+C to stop.");
    }

    server_block_signals(0);

    server_run_tick_loop();

    // should_quit without a recorded reason means net_main() bailed out
    // (socket/bind/listen failure); it already logged why.
    if (server_stop_reason(NULL) == SERVER_STOP_NONE) server_request_stop(SERVER_STOP_ERROR, "network thread stopped, see above");

    const char *detail = NULL;
    const ServerStopReason reason = server_stop_reason(&detail);

    // 1. Leave the prompt: the terminal is back in cooked mode after this.
    cli_stop();
    server_log("Shutting down (%s)...", server_stop_description(reason, detail));

    // 2. Stop accepting connections: net_main() exits on should_quit and closes the listen socket.
    if (net_started) pthread_join(net_thread, NULL);

    // 3. Close every client socket.
    const size_t client_count = g_server->client_count;
    server_disconnect_all();
    if (client_count) server_log("Disconnected %lu client%s", (unsigned long) client_count, client_count == 1 ? "" : "s");

    net_cleanup();

    // 4. Save history and restore the terminal before anything else is printed.
    cli_shutdown();

    pthread_mutex_destroy(&g_server->lock);

    const int exit_code = reason == SERVER_STOP_ERROR ? 1 : 0;
    printf("KrunkNative server stopped.\n");
    fflush(stdout);

#ifdef WIN32
    g_shutdown_complete = 1;
#endif
    return exit_code;
}
