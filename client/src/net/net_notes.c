// Transport notes and the transport summary of the console's `netinfo` / the menu ping tooltip.
//
// client_net_note(): one line about the game transport (which one was tried, why it failed,
// which fallback was taken). It goes to stdout at once (the browser console on the web) and
// into a small queue that ui_console_tick() empties into the in-game console on the main
// thread, so the native transport thread may call it too.
#include <client.h>
#include "klog.h"
#include <pthread.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#define NOTE_MAX 32
#define NOTE_LEN 320

static struct {
    pthread_mutex_t lock;
    char lines[NOTE_MAX][NOTE_LEN];
    int levels[NOTE_MAX];
    int head, count;
} g_notes = {.lock = PTHREAD_MUTEX_INITIALIZER};

void client_net_note(const int level, const char *fmt, ...) {
    char line[NOTE_LEN];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    KN_LOG_VERBOSE2("[net] %s\n", line);
    fflush(stdout);

    pthread_mutex_lock(&g_notes.lock);
    if (g_notes.count == NOTE_MAX) { // drop the oldest
        g_notes.head = (g_notes.head + 1) % NOTE_MAX;
        g_notes.count--;
    }
    const int slot = (g_notes.head + g_notes.count) % NOTE_MAX;
    snprintf(g_notes.lines[slot], NOTE_LEN, "%s", line);
    g_notes.levels[slot] = level;
    g_notes.count++;
    pthread_mutex_unlock(&g_notes.lock);
}

int client_net_note_next(int *level, char *out, const size_t size) {
    pthread_mutex_lock(&g_notes.lock);
    const int have = g_notes.count > 0;
    if (have) {
        snprintf(out, size, "%s", g_notes.lines[g_notes.head]);
        if (level) *level = g_notes.levels[g_notes.head];
        g_notes.head = (g_notes.head + 1) % NOTE_MAX;
        g_notes.count--;
    }
    pthread_mutex_unlock(&g_notes.lock);
    return have;
}

const char *client_net_transport_name(const Client *client) {
    if (!client) return "-";
    if (client->offline || client->game.is_local) return "offline";
#ifdef __EMSCRIPTEN__
    const char *label = net_wt_transport_label();
    if (label) return label;
    return client->net_socket >= 0 ? "WebSocket" : "-";
#else
    if (client->net_socket < 0) return "-";
    return net_udp_client_is_virtual(client->net_socket) ? "UDP" : "TCP";
#endif
}

void client_net_info(const Client *client, char *out, const size_t size) {
    size_t n = 0;
#define NI_ADD(...)                                                           \
    do {                                                                      \
        if (n < size) n += (size_t) snprintf(out + n, size - n, __VA_ARGS__); \
    } while (0)
    out[0] = 0;
    NI_ADD("transport: %s\n", client_net_transport_name(client));
    if (client && client->net_socket >= 0 && !client->offline)
        NI_ADD("server: %s:%u, ping %d ms\n", client->net_address, (unsigned int) client->net_port, client->net_ping_ms);
#ifdef __EMSCRIPTEN__
    if (client && client->net_ws_url[0]) NI_ADD("websocket endpoint: %s\n", client->net_ws_url);
    if (n < size) net_wt_info(out + n, size - n);
#else
    if (n < size) net_udp_client_info(out + n, size - n);
#endif
#undef NI_ADD
}
