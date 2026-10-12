// Server core glue (server.h): the server clock, the transports flushed every tick and the
// tick step shared by server_bin's loop (main.c) and the in-process sandbox (sandbox.c).
#include <server.h>
#include <server_accounts.h>
#include <bots.h>
#include <ks_server.h> // KrunkScript server host
#include <string.h>
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

// The quota's lobby size: the round's max players; ranked matches get no quota bots.
static void server_bot_slots(void) {
    char tags[4][24];
    const int n = accounts_info_tags(tags, 4);
    int ranked = 0;
    for (int i = 0; i < n; i++) ranked |= !strcmp(tags[i], "ranked");
    g_server->game.bots->slots = ranked ? -1 : g_server->config.max_players;
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
