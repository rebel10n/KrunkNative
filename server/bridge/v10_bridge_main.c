/*
 * v10_bridge_main.c - the v10_bridge process (server/bridge/v10_bridge.h).
 *
 *   v10_bridge --v10-listen=<port> [--bind=<addr>] [--native=<host:port>] [--lobby=<id>]
 *
 * The official Krunker v10 client connects to ws://<bind>:<port>/ws. The native server is a KrunkerBox
 * game server (server_bin, TCP, the knet frames). Environment fallbacks, as the TS bridge reads them:
 * KRUNK_NATIVE_HOST, KRUNK_NATIVE_PORT (21015), KRUNK_NATIVE_LOBBY_ID ("KN:local").
 * SIGINT / SIGTERM: every session closes (1001), then the process exits.
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/loop.h"
#include "v10_bridge.h"

static void usage(FILE *out) {
    fprintf(out,
            "usage: v10_bridge --v10-listen=<port> [--bind=<addr>] [--native=<host:port>] [--lobby=<id>]\n"
            "  --v10-listen   WebSocket port of the v10 client (ws://<bind>:<port>/ws); 0 = ephemeral\n"
            "  --bind         listen address (default 127.0.0.1)\n"
            "  --native       KrunkerBox game server (default KRUNK_NATIVE_HOST:KRUNK_NATIVE_PORT, 127.0.0.1:21015)\n"
            "  --lobby        the lobby id the client shows (default KRUNK_NATIVE_LOBBY_ID, KN:local)\n");
}

/* "host:port" -> host and port; false when the port is not a number. */
static int parse_native(const char *value, char *host, size_t host_cap, int *port) {
    const char *colon = strrchr(value, ':');
    if (!colon || colon == value) return 0;
    const size_t hl = (size_t)(colon - value);
    if (hl >= host_cap) return 0;
    memcpy(host, value, hl);
    host[hl] = 0;
    char *end = NULL;
    const long p = strtol(colon + 1, &end, 10);
    if (end == colon + 1 || *end || p <= 0 || p > 65535) return 0;
    *port = (int)p;
    return 1;
}

static int g_listen_port = -1;
static char g_bind[64] = "127.0.0.1";
static char g_native_host[256] = "";
static int g_native_port = 0;
static char g_lobby[64] = "";

static void on_signal(kn_loop *loop, int signo, void *ud) {
    (void)signo;
    (void)ud;
    kn_loop_stop(loop);
}

int main(int argc, char **argv) {
    for (int i = 1; i < argc; i++) {
        const char *arg = argv[i];
        if (!strncmp(arg, "--v10-listen=", 13)) {
            char *end = NULL;
            const long p = strtol(arg + 13, &end, 10);
            if (end == arg + 13 || *end || p < 0 || p > 65535) {
                fprintf(stderr, "v10_bridge: bad port in %s\n", arg);
                return 2;
            }
            g_listen_port = (int)p;
        } else if (!strncmp(arg, "--bind=", 7)) {
            snprintf(g_bind, sizeof g_bind, "%s", arg + 7);
        } else if (!strncmp(arg, "--native=", 9)) {
            if (!parse_native(arg + 9, g_native_host, sizeof g_native_host, &g_native_port)) {
                fprintf(stderr, "v10_bridge: --native wants host:port, got %s\n", arg + 9);
                return 2;
            }
        } else if (!strncmp(arg, "--lobby=", 8)) {
            snprintf(g_lobby, sizeof g_lobby, "%s", arg + 8);
        } else if (!strcmp(arg, "-h") || !strcmp(arg, "--help")) {
            usage(stdout);
            return 0;
        } else {
            fprintf(stderr, "v10_bridge: unknown argument %s\n", arg);
            usage(stderr);
            return 2;
        }
    }
    if (g_listen_port < 0) {
        usage(stderr);
        return 2;
    }
    if (!g_native_port) {
        const char *env_host = getenv("KRUNK_NATIVE_HOST");
        const char *env_port = getenv("KRUNK_NATIVE_PORT");
        snprintf(g_native_host, sizeof g_native_host, "%s", env_host && *env_host ? env_host : "127.0.0.1");
        g_native_port = env_port && *env_port ? atoi(env_port) : 21015;
        if (g_native_port <= 0) g_native_port = 21015;
    }
    if (!g_lobby[0]) {
        const char *env_lobby = getenv("KRUNK_NATIVE_LOBBY_ID");
        snprintf(g_lobby, sizeof g_lobby, "%s", env_lobby && *env_lobby ? env_lobby : "KN:local");
    }

    kn_loop *loop = kn_loop_new();
    if (!loop) return 1;
    V10BridgeConfig cfg = {
        .bind_host = g_bind,
        .listen_port = g_listen_port,
        .native_host = g_native_host,
        .native_port = g_native_port,
        .lobby_id = g_lobby,
    };
    V10Bridge *bridge = v10_bridge_new(loop, &cfg);
    if (!bridge) {
        fprintf(stderr, "v10_bridge: cannot create the listener\n");
        kn_loop_free(loop);
        return 1;
    }
    if (v10_bridge_listen(bridge) != 0) {
        v10_bridge_free(bridge);
        kn_loop_free(loop);
        return 1;
    }
    kn_loop_on_signal(loop, SIGINT, on_signal, NULL);
    kn_loop_on_signal(loop, SIGTERM, on_signal, NULL);
    fprintf(stderr, "[v10-bridge] v10 client ws://%s:%d/ws -> native %s:%d (lobby %s)\n", g_bind, v10_bridge_port(bridge),
            g_native_host, g_native_port, g_lobby);
    kn_loop_run(loop);
    v10_bridge_close_all(bridge);
    for (int i = 0; i < 20; i++) kn_loop_run_once(loop, 50); /* let the close handshakes finish */
    v10_bridge_free(bridge);
    kn_loop_free(loop);
    return 0;
}
