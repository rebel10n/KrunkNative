// Server core glue (server.h): the server clock, the transports flushed every tick and the
// tick step shared by server_bin's loop (main.c) and the in-process sandbox (sandbox.c).
#include <server.h>
#include <server_accounts.h>
#include <bots.h>
#include <ks_server.h> // KrunkScript server host
#include <server_config.h> // g_gs_config.shutdown_reason_file
#include <server_log.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <strings.h> // strcasecmp
#include <sys/stat.h>
#include <time.h>

Server *g_server; // server_bin's one lobby, or the client's sandbox (sandbox.c)

// ------------------------------------------------------------------ clock

static double (*g_server_clock)(void);

static double server_monotonic(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

double server_time(void) {
    return g_server_clock ? g_server_clock() : server_monotonic();
}

void server_set_clock(double (*clock)(void)) {
    g_server_clock = clock;
}

// ------------------------------------------------------------------ transports

#define SERVER_TRANSPORTS_MAX 4

static const ServerTransport *g_transports[SERVER_TRANSPORTS_MAX];

void server_transport_register(const ServerTransport *transport) {
    for (int i = 0; i < SERVER_TRANSPORTS_MAX; i++) {
        if (g_transports[i] == transport) return;
    }
    for (int i = 0; i < SERVER_TRANSPORTS_MAX; i++) {
        if (g_transports[i]) continue;
        g_transports[i] = transport;
        return;
    }
}

void server_transport_unregister(const ServerTransport *transport) {
    for (int i = 0; i < SERVER_TRANSPORTS_MAX; i++) {
        if (g_transports[i] == transport) g_transports[i] = NULL;
    }
}

void server_transport_flush_all(void) {
    for (int i = 0; i < SERVER_TRANSPORTS_MAX; i++) {
        if (g_transports[i] && g_transports[i]->flush) g_transports[i]->flush();
    }
}

// ------------------------------------------------------------------ tick

// The quota's lobby size: the round's max players. Ranked matches and hosted custom games get no
// quota bots (a host adds its own with bot_add); the quota bots leave when one starts.
static void server_bot_slots(void) {
    char tags[4][24];
    const int n = accounts_info_tags(tags, 4);
    int no_bots = 0;
    for (int i = 0; i < n; i++) no_bots |= !strcmp(tags[i], "ranked") || !strcmp(tags[i], "custom");
    g_server->game.bots->slots = no_bots ? -1 : g_server->config.max_players;
}

void server_core_tick(const float now, const float delta, const double tick_wall) {
    g_server->now = now;
    g_server->tick_wall = tick_wall; // net_input_arrival: wall clock -> game.state_now
    if (g_server->game.bots) server_bot_slots(); // bot quota: the lobby size (per mode), none in ranked
    game_tick(&g_server->game, now, delta);
    ks_server_tick(delta); // KrunkScript update() after the game update (GJS:4960), custom sync values
    accounts_tick(now); // brain events, join timeouts, stats commits, lobby pushes
    net_check_idle_clients(now);
    net_broadcast_states();
}

// ------------------------------------------------------------------ shutdown reason (server.h)
//
// What the players read when the server goes away. v7.1.4's server (server/index.ts
// restartForUpdate) sends every socket ["error", "GAME UPDATING"] and closes it 100 ms later
// when an update starts; its client shows the "Game Updating" screen for that text. Without
// a message the native client only saw its socket drop, retried with its spent join ticket and
// showed the restarted server's "Join key expired". KrunkNative sends ERROR on every stop: the
// v7 text for an update, a plain description otherwise (server_shutdown_message).

static pthread_mutex_t g_reason_lock = PTHREAD_MUTEX_INITIALIZER;
static char g_shutdown_reason[64];
static int g_shutdown_reason_known; // set explicitly, or the reason file was read

// A reason file older than this is a leftover of an interrupted deploy, not this stop.
#define SERVER_REASON_FILE_MAX_AGE 900

static void server_trim_reason(char *out, const size_t size, const char *text) {
    while (*text == ' ' || *text == '\t' || *text == '\r' || *text == '\n') text++;
    size_t n = 0;
    while (text[n] && text[n] != '\r' && text[n] != '\n' && n + 1 < size) n++; // the first line
    while (n && (text[n - 1] == ' ' || text[n - 1] == '\t')) n--;
    memcpy(out, text, n);
    out[n] = 0;
}

void server_set_shutdown_reason(const char *reason) {
    if (!reason) return;
    char trimmed[sizeof(g_shutdown_reason)];
    server_trim_reason(trimmed, sizeof(trimmed), reason);
    if (!trimmed[0]) return;
    pthread_mutex_lock(&g_reason_lock);
    snprintf(g_shutdown_reason, sizeof(g_shutdown_reason), "%s", trimmed);
    g_shutdown_reason_known = 1;
    pthread_mutex_unlock(&g_reason_lock);
}

const char *server_shutdown_reason(void) {
    pthread_mutex_lock(&g_reason_lock);
    if (!g_shutdown_reason_known) {
        g_shutdown_reason_known = 1;
        const char *path = g_gs_config.shutdown_reason_file;
        struct stat st;
        FILE *file = path[0] && stat(path, &st) == 0 && time(NULL) - st.st_mtime <= SERVER_REASON_FILE_MAX_AGE ? fopen(path, "r") : NULL;
        if (file) {
            char line[sizeof(g_shutdown_reason)] = "";
            if (fgets(line, sizeof(line), file)) server_trim_reason(g_shutdown_reason, sizeof(g_shutdown_reason), line);
            fclose(file);
            if (g_shutdown_reason[0]) server_log("Shutdown reason from %s: %s", path, g_shutdown_reason);
        }
    }
    pthread_mutex_unlock(&g_reason_lock);
    return g_shutdown_reason; // written once, before it is read
}

void server_shutdown_message(char *message, const size_t message_size, char *code, const size_t code_size) {
    const char *reason = server_shutdown_reason();
    if (!strcasecmp(reason, "update")) {
        snprintf(message, message_size, "GAME UPDATING"); // v7.1.4 UPDATING_SHUTDOWN_MESSAGE
        snprintf(code, code_size, NET_ERROR_CODE_UPDATING);
        return;
    }
    if (!strcasecmp(reason, "restart")) snprintf(message, message_size, "Server restarting");
    else if (reason[0]) snprintf(message, message_size, "Server shutting down: %s", reason);
    else snprintf(message, message_size, "Server shutting down");
    snprintf(code, code_size, NET_ERROR_CODE_SHUTDOWN);
}

const char *server_shutdown_notice(void) {
    const char *reason = server_shutdown_reason();
    if (!strcasecmp(reason, "update")) return "Server is restarting for an update after this round";
    if (!strcasecmp(reason, "restart")) return "Server is restarting after this round";
    return "Server is shutting down after this round";
}
