#include <server_cli.h>
#include <ruleset.h> // the startup log: Krunker v10.0.2
#include <flavor.h>   // --flavor / server.cfg `flavor`: the product flavor (Krunker Strike)
#include <flavor_rules.h> // the flavor's mode rotation (Krunker Strike)
#include <server_accounts.h>
#include <brain.h>
#include <info_query.h>
#include <rcon.h>
#include <bots.h>
#include <bot_chat.h>
#include <ks_server.h> // KrunkScript server host
#include <director_gate.h> // Krunker Strike director tickets
#include <build_info.h>
#include <map_catalog.h> // map_catalog_load_file
#include <published_maps.h> // published:<creator>/<name> as the map argument
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

// ------------------------------------------------------------------ drain (graceful stop)
//
// First SIGINT / SIGTERM / SIGHUP / console `drain` / Ctrl+C at the prompt: refuse new
// players, tell the coordinator, let the round finish (at most gameserver.shutdown_grace
// seconds), then stop. A second request stops at once.

static volatile sig_atomic_t g_drain_requested;
static char g_drain_why[64];
static int g_drain_active;
static float g_drain_deadline;

void server_request_drain(const char *why) {
    if (g_drain_requested) {
        server_request_stop(SERVER_STOP_SIGNAL, why && *why ? why : "second stop request");
        return;
    }
    snprintf(g_drain_why, sizeof(g_drain_why), "%s", why ? why : "");
    g_drain_requested = 1;
}

// Every connected player gets the shutdown ERROR, then (after ~100 ms, like v7.1.4's
// sock.close timer, so the text is read before the close) the sockets close.
static void server_send_shutdown_message(void) {
    NetErrorPacket error;
    memset(&error, 0, sizeof(error));
    server_shutdown_message(error.message, sizeof(error.message), error.code, sizeof(error.code));
    pthread_mutex_lock(&g_server->lock);
    const size_t count = g_server->client_count;
    for (size_t i = 0; i < count; i++) net_send_packet(g_server->clients[i].fd, NET_PACKET_ERROR, &error, sizeof(error));
    pthread_mutex_unlock(&g_server->lock);
    if (!count) return;
    server_log("Sent \"%s\" (%s) to %lu client%s", error.message, error.code, (unsigned long) count, count == 1 ? "" : "s");
#ifdef WIN32
    Sleep(150);
#else
    const struct timespec pause = {0, 150 * 1000 * 1000};
    nanosleep(&pause, NULL);
#endif
}

// server.cfg bots / bot_quota / bot_difficulty: the v7.1.4 bot manager (shared/src/bots.c)
// keeps that many bot players in the lobby. The difficulty applies like `bot_difficulty`.
static void server_configure_bots(const GsConfig *cfg) {
    bot_chat_set_enabled(&g_server->game, cfg->bot_chat); // bot chat (shared/src/bot_chat.c), also for bot_add bots
    bot_chat_set_ai(&g_server->game, cfg->bot_chat_ai);
    const int quota = cfg->bot_quota > 0 ? cfg->bot_quota : cfg->bots;
    if (quota <= 0) return;
    float difficulty = BOT_DIFFICULTY_NORMAL;
    if (!bot_parse_difficulty(cfg->bot_difficulty, &difficulty)) {
        server_log("bot_difficulty '%s' is not easy|normal|hard|max|<number>: normal", cfg->bot_difficulty);
        difficulty = BOT_DIFFICULTY_NORMAL;
    }
    BotAssignments preset;
    bot_difficulty_assignments(difficulty, &preset);
    game_bots_configure(&g_server->game, &preset);
    game_bots_set_quota(&g_server->game, quota, cfg->bot_quota > 0);
    game_bots_set_quota_grace(&g_server->game, cfg->bot_quota_grace);
    if (cfg->bot_quota > 0)
        server_log("Bots: fill to %d players while a human plays (none in an empty lobby after %g s), difficulty %g (%s)", quota,
                   (double) cfg->bot_quota_grace, (double) difficulty, cfg->bot_difficulty);
    else server_log("Bots: keep %d, difficulty %g (%s)", quota, (double) difficulty, cfg->bot_difficulty);
}

static void server_tick_drain(void) {
    if (!g_drain_requested || g_server->should_quit) return;
    if (!g_drain_active) {
        g_drain_active = 1;
        g_drain_deadline = g_server->now + g_gs_config.shutdown_grace;
        accounts_begin_drain(g_drain_why);
        if (g_gs_config.shutdown_grace > 0.0f && !accounts_drained())
            server_log("Stopping after this round (at most %.0f s; signal again or 'quit' to stop now)", g_gs_config.shutdown_grace);
    }
    if (accounts_drained() || g_server->now >= g_drain_deadline) {
        char detail[96];
        snprintf(detail, sizeof(detail), "%s, drained", g_drain_why[0] ? g_drain_why : "drain");
        server_request_stop(SERVER_STOP_SIGNAL, detail);
    }
}

static void server_poll_signals(void) {
    const int pending = g_pending_signal;
    if (!pending) return;
    g_pending_signal = 0;

    char detail[64];
    snprintf(detail, sizeof(detail), "received %s", server_signal_name(pending));
    server_request_drain(detail);
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
    int no_cli;         // --no-cli: no console thread, stdin is never read
    int help;
    const char *mode;   // --mode: a Krunker mode alias / name / id, or "rotation"
    float round_seconds; // --round-seconds: round length override (debug: quick end screen / rotation tests)
    int perms;          // --perms (or PERMS=1): v3.1.1 PERMS, DEV-only chat commands
    int open_cheats;    // --open-cheats: v3.1.1's perms-off cheats for everyone
    const char *flavor; // --flavor base|strike (flavor.h); NULL = server.cfg `flavor`, else base
    // backend / config (doc/backend.md section 8); NULL / 0 = not given on the command line
    const char *config_path; // --config FILE
    const char *brain_url;   // --brain URL
    const char *region;      // --region ID
    const char *node_id;     // --node-id ID
    const char *name;        // --name TEXT
    const char *public_host; // --public-host HOST
    unsigned short public_port; // --public-port PORT
    int max_players;         // --max-players N
    // dedicated server (doc/backend.md 8.7)
    const char *cfg_path;    // --cfg FILE (server.cfg; "none" = skip the search)
    const char *hostname;    // --hostname TEXT (= --name)
    const char *password;    // --password TEXT (sv_password)
    const char *log_file;    // --log FILE
    int online_mode;         // --online-mode / --offline-mode (1 / 0, -1 not given)
    const char *rcon_target; // --rcon [HOST:]PORT: client mode
    const char *rcon_password; // --rcon-password PW
    int rcon_argc;           // trailing words after --rcon ... = the command
    char **rcon_argv;
    int version;             // --version
} ServerArgs;

// "--flag VALUE" / "--flag=VALUE": 1 matched (value set, NULL when missing), 0 other flag.
static int arg_value(const int argc, char **argv, int *i, const char *flag, const char **value) {
    const size_t n = strlen(flag);
    if (!strncmp(argv[*i], flag, n) && argv[*i][n] == '=') {
        *value = argv[*i] + n + 1;
        return 1;
    }
    if (strcmp(argv[*i], flag)) return 0;
    *value = ++*i < argc ? argv[*i] : NULL;
    return 1;
}

// --gameserver.<key>=<value> / --set gameserver.<key>=<value> (knsvc/config.h style).
// Other sections' overrides (--set brain.port=..., --database.name=...) are accepted and
// ignored: backend-up passes the same arguments to every service.
static int parse_config_override(const char *spec) {
    const char *dot = strchr(spec, '.');
    const char *eq = strchr(spec, '=');
    if (dot && eq && dot < eq && strncmp(spec, "gameserver.", 11)) {
        // brain.host / brain.port feed gameserver.brain_url "auto" (server/src/config.c)
#ifdef WIN32
        if (!strncmp(spec, "brain.host=", 11)) _putenv_s("KN_BRAIN_HOST", eq + 1);
        if (!strncmp(spec, "brain.port=", 11)) _putenv_s("KN_BRAIN_PORT", eq + 1);
#else
        if (!strncmp(spec, "brain.host=", 11)) setenv("KN_BRAIN_HOST", eq + 1, 1);
        if (!strncmp(spec, "brain.port=", 11)) setenv("KN_BRAIN_PORT", eq + 1, 1);
#endif
        return 1;
    }
    if (strncmp(spec, "gameserver.", 11)) return 0;
    if (!eq || eq == spec + 11) return 0;
    char key[64];
    snprintf(key, sizeof(key), "%.*s", (int) (eq - spec - 11), spec + 11);
    return gs_config_set_cli(key, eq + 1) == 0;
}

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

        if (args->rcon_target && arg[0] != '-') { // server_bin --rcon HOST:PORT status
            args->rcon_argc = argc - i;
            args->rcon_argv = argv + i;
            break;
        }
        if (!strcmp(arg, "--online-mode") || !strcmp(arg, "--offline-mode")) {
            args->online_mode = arg[3] == 'n';
            continue;
        }
        if (!strcmp(arg, "--version") || !strcmp(arg, "-v")) {
            args->version = 1;
            continue;
        }
        {
            static const struct {
                const char *flag;
                size_t offset;
            } ds_flags[] = {
                {"--cfg", offsetof(ServerArgs, cfg_path)},           {"--hostname", offsetof(ServerArgs, hostname)},
                {"--password", offsetof(ServerArgs, password)},      {"--log", offsetof(ServerArgs, log_file)},
                {"--rcon", offsetof(ServerArgs, rcon_target)},       {"--rcon-password", offsetof(ServerArgs, rcon_password)},
            };
            const char *v = NULL;
            int hit = 0;
            for (size_t f = 0; f < sizeof(ds_flags) / sizeof(ds_flags[0]) && !hit; f++) {
                if (!arg_value(argc, argv, &i, ds_flags[f].flag, &v)) continue;
                if (!v) return 0;
                *(const char **) ((char *) args + ds_flags[f].offset) = v;
                hit = 1;
            }
            if (hit) continue;
        }

        if (!strcmp(arg, "--open-cheats")) {
            args->open_cheats = 1;
            continue;
        }

        if (arg_value(argc, argv, &i, "--flavor", &args->flavor)) {
            if (!args->flavor || flavor_from_key(args->flavor, FLAVOR_COUNT) == FLAVOR_COUNT) return 0;
            continue;
        }

        const char *value = NULL;
        if (arg_value(argc, argv, &i, "--config", &args->config_path)) {
            if (!args->config_path) return 0;
            continue;
        }
        static const struct {
            const char *flag;
            size_t offset;
        } string_flags[] = {
            {"--brain", offsetof(ServerArgs, brain_url)},     {"--region", offsetof(ServerArgs, region)},
            {"--node-id", offsetof(ServerArgs, node_id)},     {"--name", offsetof(ServerArgs, name)},
            {"--public-host", offsetof(ServerArgs, public_host)},
        };
        int matched = 0;
        for (size_t f = 0; f < sizeof(string_flags) / sizeof(string_flags[0]) && !matched; f++) {
            if (!arg_value(argc, argv, &i, string_flags[f].flag, &value)) continue;
            if (!value) return 0;
            *(const char **) ((char *) args + string_flags[f].offset) = value;
            matched = 1;
        }
        if (matched) continue;
        if (arg_value(argc, argv, &i, "--public-port", &value)) {
            if (!value || !parse_port(value, &args->public_port)) return 0;
            continue;
        }
        if (arg_value(argc, argv, &i, "--max-players", &value)) {
            char *end = NULL;
            const long n = value ? strtol(value, &end, 10) : 0;
            if (!value || !end || *end || n < 1 || n > 31) return 0;
            args->max_players = (int) n;
            continue;
        }
        if (arg_value(argc, argv, &i, "--set", &value)) {
            if (!value || !parse_config_override(value)) return 0;
            continue;
        }
        if (!strncmp(arg, "--", 2) && strchr(arg, '.') && strchr(arg, '=') && strchr(arg, '.') < strchr(arg, '=')) {
            if (!parse_config_override(arg + 2)) return 0; // --gameserver.<key>=v (other sections ignored)
            continue;
        }

        if (!strcmp(arg, "--headless") || !strcmp(arg, "--no-cli")) {
            args->headless = 1;
            if (!strcmp(arg, "--no-cli")) args->no_cli = 1;
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
            KN_PRODUCT_NAME " dedicated server (one lobby per process; doc/backend.md section 8).\n"
            "Usage: %s [options] [map.json | published:<creator>/<name>]\n"
            "       %s --rcon [HOST:]PORT --rcon-password PW [command ...]   (remote console client)\n"
            "\n"
            "Hosting: put a server.cfg next to the binary (or in ./cfg/) and run it with no flags.\n"
            "Example server.cfg (any gameserver key works; \"//\" comments):\n"
            "  hostname \"My Krunker Server\"     port 21015          maxplayers 8\n"
            "  mapcycle Burg,Sandstorm,Kanji     mode ffa,tdm        sv_password \"\"\n"
            "  // a published map from the brain (brain_url / brain_http_url): mapcycle Burg,published:alice/Arena\n"
            "  rcon_password \"change-me\"       rcon_port 27015     region us-nj\n"
            "  online_mode 1                     // verify accounts with the central auth service (guests still join)\n"
            "  coordinator_url ws://coord.example:5050/mm/internal/ws   coordinator_secret \"...\"\n"
            "  finder_url https://matchmaker.krunk.io   // opt-in public listing\n"
            "  public_ip 203.0.113.7   public_port 21015    log_file logs/server.log\n"
            "\n"
            "Dedicated server:\n"
            "  --cfg FILE           server.cfg to load (default ./server.cfg, then ./cfg/server.cfg; 'none' = skip)\n"
            "  --hostname TEXT      server name in lists (server.cfg hostname)\n"
            "  --password TEXT      players need this password to join (sv_password)\n"
            "  --online-mode | --offline-mode   verify accounts by hasJoined (guests welcome) / no hasJoined\n"
            "  --log FILE           also append the console log to FILE\n"
            "  --version            print the version and exit\n"
            "\n"
            "Game:\n"
            "  --port PORT   TCP port to listen on (default %d); the server-info query answers on UDP PORT\n"
            "  --mode MODE   game mode: alias (tdm, point, ctf, infect, gun, ...), name or id;\n"
            "                'rotation' plays Krunker's public rotation (default: ffa)\n"
            "  --round-seconds S  round length in seconds instead of the mode's game time (testing)\n"
            "  --perms       keep DEV chat commands (/kick, /map, /god, ...) for DEV accounts, op'd and\n"
            "                local clients even with --open-cheats (also PERMS=1)\n"
            "  --open-cheats every chat command (moderation and cheats) for everyone while perms are off,\n"
            "                like v3.1.1 (gameserver.cheats \"all\"); by default only DEV accounts, ops,\n"
            "                clients on this machine (and the custom host, for the cheats) get them\n"
            "  --headless    no interactive console (implied when stdin/stdout is not a terminal);\n"
            "                console commands are still read line by line from stdin\n"
            "  --no-cli      no console at all: stdin is never read (systemd, supervisors)\n"
            "\n"
            "Coordinator / config (config/config.json \"gameserver\", doc/backend.md section 8):\n"
            "  --config FILE        config file (default: $KN_CONFIG, else config/config.json)\n"
            "  --brain URL          kn_brain internal socket, e.g. ws://127.0.0.1:5050/mm/internal/ws\n"
            "  --region ID  --node-id ID  --name TEXT  --public-host HOST  --public-port PORT\n"
            "  --max-players N      lobby size (1-31)\n"
            "  --gameserver.KEY=VALUE  any gameserver key (also --set gameserver.KEY=VALUE)\n"
            "  --set gameserver.game.VAR=VALUE  one game setting (gameTime, maxPlayers, canSlide, ...; JSON values)\n"
            "  Precedence: command line > KN_GAMESERVER_* env > server.cfg > config.json > defaults.\n"
            "\n"
            "Console: status, players, kick, ban, map/changelevel, mode, say, round, op, drain, quit, help\n"
            "(also every Krunker chat command without the slash). The same commands work over rcon.\n"
            "Ctrl+C / SIGINT / SIGTERM drain: no new players, the round finishes (up to shutdown_grace\n"
            "seconds, default 10), stats are committed, then the server exits; a second signal or 'quit'\n"
            "stops at once. Players are told why: 'quit update' / 'drain update' (or \"update\" in the\n"
            "shutdown_reason_file a deploy writes) says GAME UPDATING, 'quit restart' Server restarting.\n",
            argv0, argv0, NET_DEFAULT_PORT);
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
        server_core_tick(now, delta, tick_start); // game_tick, accounts, idle kicks, the per-tick broadcast
        info_query_update(now);
        server_tick_drain();

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

    ServerArgs args = {0}; // port 0: from the config (default NET_DEFAULT_PORT)
    args.online_mode = -1;

    if (!parse_args(argc, argv, &args)) {
        print_usage(stderr, argv[0]);
        return 2;
    }

    if (args.help) {
        print_usage(stdout, argv[0]);
        return 0;
    }
    if (args.flavor) { // --flavor wins over server.cfg; before --version and the config defaults
        flavor_set_key(args.flavor);
        gs_config_lock_flavor();
    }
    if (args.version) {
        printf(KN_PRODUCT_NAME " server_bin %d.%d.%d (Krunker v3.1.1 port)\n", g_server_version.major, g_server_version.minor, g_server_version.patch);
        if (flavor_get() != FLAVOR_BASE) printf("flavor %s (%s)\n", flavor_current()->key, flavor_current()->display_name);
        // build identity (shared/include/build_info.h): what the finder / coordinator compare
        printf("build %s\n", kn_build_string());
        return 0;
    }
    if (args.rcon_target) return rcon_client_main(args.rcon_target, args.rcon_password, args.rcon_argc, args.rcon_argv);
    if (args.cfg_path) gs_config_set_cfg_path(args.cfg_path);

    // config/config.json "gameserver" < KN_GAMESERVER_* env < these flags
    gs_config_defaults(&g_gs_config);
    char config_error[256] = "";
    if (gs_config_load(&g_gs_config, args.config_path, args.config_path != NULL, config_error, sizeof(config_error))) {
        fprintf(stderr, "Config error: %s\n", config_error);
        return 2;
    }
    GsConfig *cfg = &g_gs_config;
    if (args.port) cfg->listen_port = args.port;
    if (args.brain_url) snprintf(cfg->brain_url, sizeof(cfg->brain_url), "%s", args.brain_url);
    if (args.region) snprintf(cfg->region, sizeof(cfg->region), "%s", args.region);
    if (args.node_id) snprintf(cfg->node_id, sizeof(cfg->node_id), "%s", args.node_id);
    if (args.name) snprintf(cfg->name, sizeof(cfg->name), "%s", args.name);
    if (args.hostname) snprintf(cfg->name, sizeof(cfg->name), "%s", args.hostname);
    if (args.password) snprintf(cfg->password, sizeof(cfg->password), "%s", args.password);
    if (args.log_file) snprintf(cfg->log_file, sizeof(cfg->log_file), "%s", args.log_file);
    if (args.online_mode >= 0) cfg->online_mode = args.online_mode;
    if (args.public_host) snprintf(cfg->public_host, sizeof(cfg->public_host), "%s", args.public_host);
    if (args.public_port) cfg->public_port = args.public_port;
    // The product flavor (flavor.h): --flavor (applied above) wins; else server.cfg `flavor`; else
    // the compile-time default (base, or Strike for a -DKRUNKNATIVE_FLAVOR=strike build).
    if (!args.flavor && cfg->flavor[0]) flavor_set_key(cfg->flavor);
    if (args.max_players) {
        cfg->max_players = args.max_players;
        memset(cfg->mode_max_players, 0, sizeof(cfg->mode_max_players));
        cfg->team_size = 0;
        memset(cfg->mode_team_size, 0, sizeof(cfg->mode_team_size));
    }
    if (args.round_seconds > 0.0f) {
        cfg->round_seconds = args.round_seconds;
        memset(cfg->mode_round_seconds, 0, sizeof(cfg->mode_round_seconds));
    }
    if (args.perms) cfg->perms = 1;
    if (args.open_cheats) cfg->cheats_open = 1;
    if (strchr(cfg->node_id, ':')) {
        fprintf(stderr, "--node-id '%s' must not contain ':'\n", cfg->node_id);
        return 2;
    }

    cJSON *startup_map = NULL;

    // published:<creator>/<name>: a published map from the brain (published_maps.h). When it
    // cannot be had, the server says so and plays gameserver.maps / Krunker's rotation instead.
    char published_name[96], published_creator[64];
    if (args.map_path && published_map_parse(args.map_path, published_name, sizeof(published_name), published_creator, sizeof(published_creator))) {
        char err[160];
        const cJSON *published = published_map_load(cfg, published_name, published_creator, 10000, err, sizeof(err));
        if (published) startup_map = cJSON_Duplicate(published, 1);
        else server_log_error("Map %s unavailable (%s): playing the configured maps instead", args.map_path, err);
    }
    // map_catalog_load_file: a renamed map's old path (assets/maps/burg.json) loads its 311_ file
    else if (args.map_path && map_catalog_load_file(args.map_path, &startup_map)) {
        fprintf(stderr, "Failed to load map JSON: %s\n", args.map_path);
        return 1;
    }

    server_install_signal_handlers();

    char port_error[128] = "";

    if (!server_port_available(cfg->listen_port, port_error, sizeof(port_error))) {
        fprintf(stderr, "Cannot listen on port %u: %s\n", (unsigned int) cfg->listen_port, port_error);
        cJSON_Delete(startup_map);
        return 1;
    }

    static Server INSTANCE = {0};
    g_server = &INSTANCE;

    g_server->config.tick_rate = cfg->tick_rate;
    g_server->config.listen_port = cfg->listen_port;
    g_server->config.max_players = (unsigned short) cfg->max_players; // per mode: accounts_round_changed()
    g_server->config.idle_timeout = cfg->idle_timeout;
    g_server->config.idle_menu_timeout = cfg->idle_menu_timeout;
    g_server->config.idle_move_threshold = cfg->idle_move_threshold;
    g_server->config.perms = cfg->perms; // --perms, PERMS=1 or gameserver.perms
    g_server->config.cheats_open = cfg->cheats_open;
    g_server->game.lag_comp = (unsigned char) !!cfg->lag_comp; // players.js fetchState (shared/src/player.c)
    g_server->game.lag_comp_anchor = (unsigned char) (cfg->lag_comp_anchor & 3); // LAG_COMP_ANCHOR_*
    g_server->game.lag_comp_scale = cfg->lag_comp_scale; // times each player's ENT lagComp
    pthread_mutex_init(&g_server->lock, NULL);

    // --mode: one mode for every round (Krunker config.modes = [id]), or the public rotation;
    // it replaces gameserver.modes of the config
    if (args.mode) {
        const int rotation = !strcmp(args.mode, "rotation");
        const int mode_id = rotation ? -1 : mode_find(args.mode);

        if (!rotation && mode_id < 0) {
            fprintf(stderr, "Unknown mode '%s' (try ffa, tdm, point, ctf, infect, gun, kc, ...)\n", args.mode);
            cJSON_Delete(startup_map);
            return 2;
        }
        if (!rotation && !flavor_mode_allowed(mode_id)) {
            fprintf(stderr, "Mode '%s' is not a %s mode\n", args.mode, flavor_current()->display_name);
            cJSON_Delete(startup_map);
            return 2;
        }

        cfg->mode_count = 0;
        if (rotation) { // the flavor's public rotation (Krunker Strike: the modes its menu offers)
            const int *rotation_modes = NULL;
            const size_t rotation_count = flavor_rotation_modes(flavor_get(), &rotation_modes);
            for (size_t m = 0; m < rotation_count && cfg->mode_count < GS_MAX_LIST; m++) cfg->modes[cfg->mode_count++] = rotation_modes[m];
        } else {
            cfg->modes[cfg->mode_count++] = mode_id;
        }
    }

    // map list: the command line map.json + Krunker's rotation, else gameserver.maps /
    // custom_map_dirs, else Krunker's rotation; GameConfig from gameserver.game
    g_server->game.server = g_server;
    // The listen port joins the random seed: instances a deploy starts in the same second (one
    // per port) open on different rotation pairs (game_init -> game_pick_rotation_pair).
    pcg32_srandom(((uint64_t) pcg32_random() << 32) ^ (uint64_t) time(NULL), ((uint64_t) cfg->listen_port << 32) ^ (uint64_t) (uintptr_t) &args);
    accounts_configure_public(&g_server->game, startup_map);
    game_init(&g_server->game, startup_map ? 0 : -1, -1, 0);
    server_configure_bots(cfg); // server.cfg bots / bot_quota / bot_difficulty (Krunker v7.1.4 bot players)

    // Interactive only when a human is on both ends; pipes, /dev/null, systemd
    // and redirected logs run headless (no raw mode, no escape sequences).
    const int interactive = !args.headless && SERVER_ISATTY(stdin) && SERVER_ISATTY(stdout);

    // net.c / events.c still print through g_replxx, so it exists in headless
    // mode too; there it is only a printer (replxx_input is never called and
    // the terminal mode is never touched).
    g_replxx = replxx_init();
    if (cfg->log_file[0] && server_log_open_file(cfg->log_file))
        fprintf(stderr, "Cannot open log file %s: %s (logging to the console only)\n", cfg->log_file, strerror(errno));

    server_log("Starting " KN_PRODUCT_NAME " server v%d.%d.%d%s", g_server_version.major, g_server_version.minor, g_server_version.patch,
               interactive ? "" : " (headless)");
    server_log("Build %s", kn_build_string());
    server_log("Plays Krunker v%s", ruleset_current()->version);
    if (flavor_get() != FLAVOR_BASE) server_log("Flavor: %s (%s)", flavor_current()->display_name, flavor_current()->key);

    if (g_server->game.ready) {
        const cJSON *name = cJSON_GetObjectItem(g_server->game.maps[g_server->game.current_map_index], "name");
        server_log("Loaded map %s (%d), mode %s (%d)", cJSON_IsString(name) ? name->valuestring : "?",
                   g_server->game.current_map_index, mode_name(game_mode_id(&g_server->game)), game_mode_id(&g_server->game));
    } else {
        server_log_error("No playable map loaded");
    }

    director_gate_init(cfg); // Krunker Strike director tickets (gameserver.director_secret)
    // backend: lobby id, bans file, per-mode lobby size / round length, brain thread
    pthread_mutex_lock(&g_server->lock);
    ks_server_round_init(); // KrunkScript: the startup map's server script, start()
    accounts_init();
    info_query_update(0.0f); // the query snapshot exists before any transport can answer queries
    pthread_mutex_unlock(&g_server->lock);
    gs_config_log_summary(cfg);

    server_block_signals(1);

    pthread_t net_thread;
    const int net_started = pthread_create(&net_thread, NULL, server_net_thread, NULL) == 0;

    if (!net_started) {
        server_request_stop(SERVER_STOP_ERROR, "could not start network thread");
    } else if (!server_wait_for_net_startup()) {
        info_query_start(); // server-info query (UDP) + optional finder heartbeat
        rcon_start();       // local rcon (rcon_port + rcon_password)
        if (cli_start(interactive, !args.no_cli)) server_log_error("Interactive console unavailable, running headless");
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

    // 3. Close every client socket (the backend first commits what everyone played).
    //    Backend: drain + lobby-down to the brain.
    pthread_mutex_lock(&g_server->lock);
    accounts_shutdown();
    pthread_mutex_unlock(&g_server->lock);

    server_send_shutdown_message(); // ERROR "GAME UPDATING" / "Server shutting down" (v7.1.4 restartForUpdate)
    const size_t client_count = g_server->client_count;
    server_disconnect_all();
    if (client_count) server_log("Disconnected %lu client%s", (unsigned long) client_count, client_count == 1 ? "" : "s");
    ks_server_shutdown(); // KrunkScript onServerClosed (the net thread has stopped)

    net_cleanup();
    rcon_stop();
    info_query_stop(); // final "shutdown" heartbeat to the finder
    brain_stop(3000); // wait for the commits' replies, close the brain socket

    // 4. Save history and restore the terminal before anything else is printed.
    cli_shutdown();

    pthread_mutex_destroy(&g_server->lock);

    const int exit_code = reason == SERVER_STOP_ERROR ? 1 : 0;
    printf(KN_PRODUCT_NAME " server stopped.\n");
    fflush(stdout);
    server_log_open_file(NULL);

#ifdef WIN32
    g_shutdown_complete = 1;
#endif
    return exit_code;
}
