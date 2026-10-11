/*
 * kn_fakenode - a fake game server node speaking the brain's internal WS
 * protocol (doc/backend.md section 5). For integration tests and local demos
 * without server_bin.
 *
 *   kn_fakenode --brain ws://127.0.0.1:5050/mm/internal/ws --secret S --node NY
 *               [--region us-nj] [--host 127.0.0.1] [--port 21015] [--lobbies 1]
 *               [--players 0] [--max 8] [--custom] [--map Burg] [--mode ffa]
 *               [--web-url URL] [--version 1.0.0] [--quiet]
 * Signals: SIGUSR1 = +1 player in the first lobby, SIGUSR2 = send drain.
 * Every received frame is printed to stdout as "<- {json}".
 */
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "knsvc/crypto.h"
#include "knsvc/json.h"
#include "knsvc/log.h"
#include "knsvc/loop.h"
#include "knsvc/ws.h"

typedef struct Lobby {
    char id[16];
    int players;
    int custom;
} Lobby;

static struct {
    const char *brain, *secret, *node, *region, *host, *web_url, *map, *mode, *version;
    int port, max, custom, quiet;
    Lobby lobbies[16];
    int lobby_count;
    kn_loop *loop;
    kn_ws *ws;
    int welcomed;
    int backoff_ms;
    char auth[300];
} g;

static void send_json(cJSON *f) {
    char *s = kn_json_print(f);
    if (s && g.ws) kn_ws_send_text(g.ws, s, (size_t) -1);
    if (s && !g.quiet) printf("-> %s\n", s);
    fflush(stdout);
    free(s);
    cJSON_Delete(f);
}

static cJSON *tuple(const Lobby *l) {
    cJSON *t = cJSON_CreateArray();
    cJSON_AddItemToArray(t, cJSON_CreateString(l->id));
    cJSON_AddItemToArray(t, cJSON_CreateString(g.region));
    cJSON_AddItemToArray(t, cJSON_CreateNumber(l->players));
    cJSON_AddItemToArray(t, cJSON_CreateNumber(g.max));
    cJSON *m = cJSON_CreateObject();
    char label[96];
    if (l->custom) snprintf(label, sizeof(label), "%s_%s", g.mode, g.map);
    else snprintf(label, sizeof(label), "%s", g.map);
    cJSON_AddStringToObject(m, "i", label);
    cJSON_AddStringToObject(m, "v", g.version);
    cJSON_AddBoolToObject(m, "cs", l->custom);
    if (l->custom) cJSON_AddStringToObject(m, "m", g.map);
    else cJSON_AddNullToObject(m, "m");
    cJSON_AddBoolToObject(m, "pr", 0);
    cJSON_AddBoolToObject(m, "earnKR", 0);
    cJSON_AddNumberToObject(m, "mode", 0);
    cJSON_AddStringToObject(m, "modeName", g.mode);
    cJSON_AddStringToObject(m, "map", g.map);
    cJSON_AddStringToObject(m, "host", g.host);
    cJSON_AddNumberToObject(m, "port", g.port);
    cJSON_AddNumberToObject(m, "timeLeft", 240);
    cJSON_AddItemToArray(t, m);
    return t;
}

static void send_snapshot(void) {
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "type", "snapshot");
    cJSON *arr = cJSON_AddArrayToObject(f, "lobbies");
    for (int i = 0; i < g.lobby_count; i++) cJSON_AddItemToArray(arr, tuple(&g.lobbies[i]));
    send_json(f);
}

static void send_lobby_up(const Lobby *l) {
    cJSON *f = cJSON_CreateObject();
    cJSON_AddStringToObject(f, "type", "lobby-up");
    cJSON_AddItemToObject(f, "lobby", tuple(l));
    send_json(f);
}

static void new_lobby_id(char *out) {
    static const char cs[] = "abcdefghijklmnopqrstuvwxyz0123456789";
    unsigned char rnd[5];
    kn_random_bytes(rnd, sizeof(rnd));
    for (int i = 0; i < 5; i++) out[i] = cs[rnd[i] % 36];
    out[5] = 0;
}

static void handle(const cJSON *f) {
    const char *type = kn_json_str(f, "type", "");
    if (!strcmp(type, "welcome")) {
        g.welcomed = 1;
        send_snapshot();
    } else if (!strcmp(type, "ping")) {
        cJSON *p = cJSON_CreateObject();
        cJSON_AddStringToObject(p, "type", "pong");
        cJSON_AddNumberToObject(p, "ts", kn_json_num(f, "ts", 0));
        send_json(p);
    } else if (!strcmp(type, "host")) {
        cJSON *r = cJSON_CreateObject();
        cJSON_AddStringToObject(r, "type", "host-reply");
        cJSON_AddNumberToObject(r, "rid", kn_json_num(f, "rid", 0));
        Lobby *l = NULL;
        for (int i = 0; i < g.lobby_count; i++) {
            if (!g.lobbies[i].custom && g.lobbies[i].players == 0) l = &g.lobbies[i];
        }
        if (!l && g.lobby_count < 16) l = &g.lobbies[g.lobby_count++];
        if (!l || !g.custom) {
            cJSON_AddBoolToObject(r, "ok", 0);
            cJSON_AddStringToObject(r, "err", "NoCapacity");
            send_json(r);
            return;
        }
        new_lobby_id(l->id);
        l->custom = 1;
        l->players = 0;
        cJSON_AddBoolToObject(r, "ok", 1);
        cJSON_AddStringToObject(r, "lobbyId", l->id);
        send_json(r);
        send_snapshot();
    } else if (!strcmp(type, "drain")) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "type", "drain");
        send_json(d);
    }
}

static void on_open(kn_ws *ws, void *ud) {
    (void) ws;
    (void) ud;
    g.backoff_ms = 1000;
    cJSON *h = cJSON_CreateObject();
    cJSON_AddStringToObject(h, "type", "hello");
    cJSON_AddStringToObject(h, "nodeId", g.node);
    cJSON_AddStringToObject(h, "region", g.region);
    cJSON_AddStringToObject(h, "name", g.node);
    cJSON_AddStringToObject(h, "publicHost", g.host);
    cJSON_AddNumberToObject(h, "publicPort", g.port);
    if (g.web_url) cJSON_AddStringToObject(h, "webUrl", g.web_url);
    cJSON_AddStringToObject(h, "version", g.version);
    cJSON_AddNumberToObject(h, "maxPlayers", g.max);
    cJSON_AddBoolToObject(h, "custom", g.custom);
    cJSON_AddNumberToObject(h, "maxLobbies", g.lobby_count > 0 ? g.lobby_count : 1);
    send_json(h);
}

static void on_message(kn_ws *ws, const void *data, size_t len, int bin, void *ud) {
    (void) ws;
    (void) bin;
    (void) ud;
    if (!g.quiet) printf("<- %.*s\n", (int) len, (const char *) data);
    fflush(stdout);
    cJSON *f = kn_json_parse(data, len);
    if (f) handle(f);
    cJSON_Delete(f);
}

static void connect_brain(kn_loop *loop, void *ud);

static void on_close(kn_ws *ws, int code, const char *reason, void *ud) {
    (void) ws;
    (void) ud;
    g.ws = NULL;
    g.welcomed = 0;
    if (!g.quiet) printf("closed %d %s; reconnect in %d ms\n", code, reason ? reason : "", g.backoff_ms);
    fflush(stdout);
    if (kn_loop_stopped(g.loop)) return;
    kn_loop_add_timer(g.loop, (uint64_t) g.backoff_ms, 0, connect_brain, NULL);
    g.backoff_ms = g.backoff_ms * 2 > 30000 ? 30000 : g.backoff_ms * 2;
}

static void connect_brain(kn_loop *loop, void *ud) {
    (void) ud;
    static const kn_ws_callbacks cbs = {.on_open = on_open, .on_message = on_message, .on_close = on_close};
    const char *hdrs[1] = {g.auth};
    kn_ws_options o = {.headers = hdrs, .header_count = 1};
    g.ws = kn_ws_connect(loop, g.brain, &cbs, &o, NULL);
}

static void snapshot_tick(kn_loop *loop, void *ud) {
    (void) loop;
    (void) ud;
    if (g.welcomed) send_snapshot();
}

static void on_usr(kn_loop *loop, int signo, void *ud) {
    (void) loop;
    (void) ud;
    if (signo == SIGUSR1 && g.lobby_count > 0) {
        if (g.lobbies[0].players < g.max) g.lobbies[0].players++;
        if (g.welcomed) send_lobby_up(&g.lobbies[0]);
    } else if (signo == SIGUSR2) {
        cJSON *d = cJSON_CreateObject();
        cJSON_AddStringToObject(d, "type", "drain");
        cJSON_AddStringToObject(d, "reason", "SIGUSR2");
        send_json(d);
    }
}

int main(int argc, char **argv) {
    kn_log_init("fakenode");
    g.brain = "ws://127.0.0.1:5050/mm/internal/ws";
    g.secret = "kn-dev-secret";
    g.node = "FAKE";
    g.region = "local";
    g.host = "127.0.0.1";
    g.map = "Burg";
    g.mode = "ffa";
    g.version = "1.0.0";
    g.port = 21015;
    g.max = 8;
    int lobbies = 1, players = 0;
    for (int i = 1; i < argc; i++) {
        const char *a = argv[i], *v = i + 1 < argc ? argv[i + 1] : NULL;
#define OPT(name) (!strcmp(a, name) && v && (i++, 1))
        if (OPT("--brain")) g.brain = v;
        else if (OPT("--secret")) g.secret = v;
        else if (OPT("--node")) g.node = v;
        else if (OPT("--region")) g.region = v;
        else if (OPT("--host")) g.host = v;
        else if (OPT("--port")) g.port = atoi(v);
        else if (OPT("--lobbies")) lobbies = atoi(v);
        else if (OPT("--players")) players = atoi(v);
        else if (OPT("--max")) g.max = atoi(v);
        else if (OPT("--map")) g.map = v;
        else if (OPT("--mode")) g.mode = v;
        else if (OPT("--web-url")) g.web_url = v;
        else if (OPT("--version")) g.version = v;
        else if (!strcmp(a, "--custom")) g.custom = 1;
        else if (!strcmp(a, "--quiet")) g.quiet = 1;
        else {
            fprintf(stderr, "unknown argument %s\n", a);
            return 2;
        }
#undef OPT
    }
    if (lobbies < 0) lobbies = 0;
    if (lobbies > 16) lobbies = 16;
    g.lobby_count = lobbies;
    for (int i = 0; i < lobbies; i++) {
        new_lobby_id(g.lobbies[i].id);
        g.lobbies[i].players = i == 0 ? players : 0;
    }
    snprintf(g.auth, sizeof(g.auth), "Authorization: Bearer %s", g.secret);
    g.backoff_ms = 1000;
    g.loop = kn_loop_new();
    kn_loop_stop_on_signals(g.loop);
    kn_loop_on_signal(g.loop, SIGUSR1, on_usr, NULL);
    kn_loop_on_signal(g.loop, SIGUSR2, on_usr, NULL);
    connect_brain(g.loop, NULL);
    kn_loop_add_timer(g.loop, 5000, 5000, snapshot_tick, NULL);
    kn_loop_run(g.loop);
    if (g.ws) kn_ws_abort(g.ws);
    kn_loop_free(g.loop);
    return 0;
}
