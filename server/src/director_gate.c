// Krunker Strike director tickets on the game server (server/include/director_gate.h).
#include <director_gate.h>
#include <server_log.h>
#include <stdio.h>
#include <string.h>

#ifdef KRUNKNATIVE_HAVE_KNSVC
#include <director_ticket.h>

static struct {
    char key[65];          // "" = gate off
    int require;
    int port;              // the port clients connect to (public_port, else listen_port)
    char code[DIRECTOR_CODE_MAX]; // the lobby's private code ("" = public)
} g_dg;

int director_gate_init(const GsConfig *cfg) {
    memset(&g_dg, 0, sizeof(g_dg));
    g_dg.port = cfg->public_port ? cfg->public_port : cfg->listen_port;
    if (!cfg->director_secret[0]) {
        if (cfg->director_require_ticket) server_log_error("[director] director_require_ticket without director_secret: ignored");
        return 0;
    }
    if (director_ticket_key(cfg->director_secret, g_dg.key) != 0) {
        g_dg.key[0] = 0;
        server_log_error("[director] cannot derive the director ticket key: tickets are not checked");
        return -1;
    }
    g_dg.require = cfg->director_require_ticket != 0;
    server_log("[director] Strike director tickets checked (port %d)%s", g_dg.port,
               g_dg.require ? ", required for every join" : "");
    return 0;
}

int director_gate_enabled(void) { return g_dg.key[0] != 0; }
int director_gate_required(void) { return g_dg.key[0] && g_dg.require; }

DirectorGateResult director_gate_check(const char *ticket, const int lobby_empty, char *msg, const size_t msg_size) {
    DirectorTicket t;
    char err[64] = "";
    if (!g_dg.key[0]) return DIRECTOR_GATE_PASS;
    if (!ticket || !ticket[0]) {
        if (g_dg.require) {
            snprintf(msg, msg_size, "Join through matchmaking");
            return DIRECTOR_GATE_REFUSED;
        }
        if (lobby_empty) {
            g_dg.code[0] = 0; // a guest in an empty lobby: it is public again
            return DIRECTOR_GATE_PASS;
        }
        if (g_dg.code[0]) {
            snprintf(msg, msg_size, "This lobby is private");
            return DIRECTOR_GATE_REFUSED;
        }
        return DIRECTOR_GATE_PASS;
    }
    if (director_ticket_verify(g_dg.key, ticket, 0, &t, err, sizeof(err)) != 0) {
        snprintf(msg, msg_size, "%s", !strcmp(err, "expired") ? "Join key expired" : "Invalid join key");
        return DIRECTOR_GATE_INVALID;
    }
    if (t.port != g_dg.port) {
        snprintf(msg, msg_size, "Join key is for another server");
        return DIRECTOR_GATE_REFUSED;
    }
    if (lobby_empty) {
        // an empty lobby takes the code of whoever the director sends first (a private lobby it
        // opened, or "" = back to public)
        if (strcmp(g_dg.code, t.code) != 0)
            server_log("[director] lobby is now %s%s", t.code[0] ? "private, code " : "public", t.code);
        snprintf(g_dg.code, sizeof(g_dg.code), "%s", t.code);
        return DIRECTOR_GATE_OK;
    }
    if (strcmp(g_dg.code, t.code) != 0) {
        snprintf(msg, msg_size, "%s", g_dg.code[0] ? "This lobby is private" : "Private lobby closed");
        return DIRECTOR_GATE_REFUSED;
    }
    return DIRECTOR_GATE_OK;
}

int director_gate_private(const int players) { return g_dg.key[0] && g_dg.code[0] && players > 0; }

#else // ---------------------------------------------------------------- no knsvc

int director_gate_init(const GsConfig *cfg) {
    if (cfg->director_secret[0]) server_log_error("[director] gameserver.director_secret is set but this server_bin was built without knsvc (OpenSSL): tickets are not checked");
    return cfg->director_secret[0] ? -1 : 0;
}
int director_gate_enabled(void) { return 0; }
int director_gate_required(void) { return 0; }
DirectorGateResult director_gate_check(const char *ticket, const int lobby_empty, char *msg, const size_t msg_size) {
    (void) ticket;
    (void) lobby_empty;
    (void) msg;
    (void) msg_size;
    return DIRECTOR_GATE_PASS;
}
int director_gate_private(const int players) {
    (void) players;
    return 0;
}

#endif
