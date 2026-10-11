// Web (emscripten) transport. The browser cannot open raw TCP sockets, so the
// client talks to a WebSocket endpoint (net-bridge proxies it to the real TCP
// server). Binary frames carry the exact same byte stream as the TCP protocol:
// 1 byte type, 2 bytes big-endian payload length, payload. Received bytes are
// pushed through a NetPacketBuffer exactly like net_poll() does natively and
// queued for client_tick_net(). Everything runs on the main thread; the
// callbacks below are invoked by the browser event loop between frames.
//
// URL resolution (first match wins):
//   1. `?ws=<url>` query parameter
//   2. `Module.krunkNativeWsUrl` set by the hosting page
//   3. `(https: ? wss : ws)://<location.host>/native`
// The brain matchmaker (servers_ui.c) overrides all of them with Client.net_ws_url.
#include <client.h>
#include <emscripten.h>
#include <emscripten/websocket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Fake descriptor stored in client->net_socket while the WebSocket is open.
#define NET_WEB_FAKE_FD 1

// Reconnect backoff after a failed/dropped WebSocket (same as the native thread).
#define NET_WEB_RETRY_MILLIS 2000

static EMSCRIPTEN_WEBSOCKET_T g_web_socket;
static NetPacketBuffer g_web_read_buffer;
static char g_web_url[512];          // resolved endpoint, for error messages
static int g_web_reconnect_pending;  // an emscripten_async_call reconnect is scheduled
static int g_web_switching;          // client_net_switch_server closed the socket: reconnect at once

static void net_web_reconnect_later(void *user_data) {
    Client *client = user_data;

    g_web_reconnect_pending = 0;
    if (client->net_should_quit || client->offline || g_web_socket) return;

    net_web_connect(client);
}

static void net_web_schedule_reconnect(Client *client) {
    if (g_web_reconnect_pending || client->net_should_quit || client->offline) return;
    if (client->net_hold_retry) return; // server ERROR: only the page's `reconnect` tries again

    g_web_reconnect_pending = 1;
    emscripten_async_call(net_web_reconnect_later, client, NET_WEB_RETRY_MILLIS);
}

static char *net_web_resolve_url(Client *client) {
    // 0. the brain matchmaker's target (servers_ui.c: seek webUrl or /native?target=host:port)
    if (client->net_ws_url[0]) return strdup(client->net_ws_url);
    return (char *) EM_ASM_PTR({
        var url = null;
        try { url = new URLSearchParams(location.search).get('ws'); } catch (e) {}
        if (!url && typeof Module !== 'undefined' && Module['krunkNativeWsUrl']) url = Module['krunkNativeWsUrl'];
        if (!url) url = (location.protocol === 'https:' ? 'wss' : 'ws') + '://' + location.host + '/native';
        return stringToNewUTF8(url);
    });
}

static bool net_web_on_open(int event_type, const EmscriptenWebSocketOpenEvent *event, void *user_data) {
    (void) event_type;
    (void) event;

    Client *client = user_data;

    if (client->offline) {
        // The user went offline (embedded server) while this socket was still
        // connecting: this session is local now, drop the late connection.
        emscripten_websocket_close(g_web_socket, 1000, "offline");
        return true;
    }

    pthread_mutex_lock(&client->net_lock);
    client->net_socket = NET_WEB_FAKE_FD;
    pthread_mutex_unlock(&client->net_lock);

    on_client_socket_connect(client);

    const NetHelloPacket hello = {1, 0, 0};
    client_net_send_packet(NET_WEB_FAKE_FD, NET_PACKET_HELLO, &hello, sizeof(hello));
    client_net_send_join(client, NET_WEB_FAKE_FD); // brain ticket / session / guest name (net_join.c)

    return true;
}

static bool net_web_on_message(int event_type, const EmscriptenWebSocketMessageEvent *event, void *user_data) {
    (void) event_type;

    Client *client = user_data;

    if (event->isText) return true; // the protocol is binary only

    // net-bridge forwards whatever one TCP read returned, which can be far more than the
    // NetPacketBuffer holds (a burst of map or voice chunks): feed it in slices, draining the
    // complete packets in between (each frame is at most NET_PACKET_HEADER_SIZE + NET_MAX_PAYLOAD).
    const uint8_t *data = event->data;
    size_t left = event->numBytes;
    NetPacket packet;
    int result = 0;

    while (left) {
        const size_t room = sizeof(g_web_read_buffer.data) - g_web_read_buffer.length;
        const size_t n = left < room ? left : room;
        if (!n || !net_buffer_push(&g_web_read_buffer, data, n)) {
            fprintf(stderr, "websocket receive buffer overflow, closing\n");
            emscripten_websocket_close(g_web_socket, 1009, "buffer overflow");
            return true;
        }
        data += n;
        left -= n;

        while ((result = net_buffer_next(&g_web_read_buffer, &packet)) > 0) {
            client_net_log_packet("<=", packet.type, packet.payload, packet.length);
            // "pi" -> "po" from onmessage like Krunker (not after the next frame's drain, net.c)
            if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) {
                client_net_send_packet(NET_WEB_FAKE_FD, NET_PACKET_PONG, packet.payload, packet.length);
                continue;
            }
            client_net_queue_packet(client, &packet);
        }

        if (result < 0) {
            fprintf(stderr, "malformed packet from server, closing\n");
            emscripten_websocket_close(g_web_socket, 1002, "protocol error");
            return true;
        }
    }

    return true;
}

static bool net_web_on_error(int event_type, const EmscriptenWebSocketErrorEvent *event, void *user_data) {
    (void) event_type;
    (void) event;
    (void) user_data;

    // The browser gives no reason here; the close event that follows reports
    // the failure (net_web_on_close) with the endpoint URL.
    fprintf(stderr, "websocket error (%s)\n", g_web_url);
    return true;
}

static bool net_web_on_close(int event_type, const EmscriptenWebSocketCloseEvent *event, void *user_data) {
    (void) event_type;

    Client *client = user_data;

    pthread_mutex_lock(&client->net_lock);
    client->net_socket = -1;
    pthread_mutex_unlock(&client->net_lock);

    g_web_read_buffer.length = 0;

    if (g_web_socket) {
        emscripten_websocket_delete(g_web_socket);
        g_web_socket = 0;
    }

    printf("websocket closed (code %d%s%s)\n", event->code, event->reason[0] ? ": " : "", event->reason);

    if (client->net_session_connected) {
        on_client_socket_disconnect(client);
    } else if (!client->offline) {
        char message[160];
        snprintf(message, sizeof(message), "Could not connect to %s", g_web_url);
        on_client_socket_failed(client, message);
    }

    if (g_web_switching) {
        // client_net_switch_server: the old session is gone, open the new one now
        g_web_switching = 0;
        net_web_connect(client);
        return true;
    }
    net_web_schedule_reconnect(client); // no-op once the embedded server runs or on quit
    return true;
}

// Opens the WebSocket; the connection completes asynchronously and
// on_client_socket_connect() fires from net_web_on_open(). A failed or dropped
// connection is reported through on_client_socket_failed()/_disconnect() and
// retried every NET_WEB_RETRY_MILLIS until the client quits or goes offline.
// Returns 0 only when the socket could not even be created.
int net_web_connect(Client *client) {
    if (g_web_socket) return 1; // already connecting/connected

    if (!emscripten_websocket_is_supported()) {
        on_client_socket_failed(client, "WebSockets are not supported by this browser");
        return 0;
    }

    char *url = net_web_resolve_url(client);
    if (!url) return 0;

    strncpy(g_web_url, url, sizeof(g_web_url) - 1);
    g_web_url[sizeof(g_web_url) - 1] = 0;
    free(url);

    EmscriptenWebSocketCreateAttributes attributes;
    emscripten_websocket_init_create_attributes(&attributes);
    attributes.url = g_web_url;
    attributes.createOnMainThread = true;

    g_web_socket = emscripten_websocket_new(&attributes);
    printf("connecting to %s\n", g_web_url);

    if (g_web_socket <= 0) {
        char message[160];
        snprintf(message, sizeof(message), "Could not open a websocket to %s (%d)", g_web_url, (int) g_web_socket);
        g_web_socket = 0;
        on_client_socket_failed(client, message);
        net_web_schedule_reconnect(client);
        return 0;
    }

    g_web_read_buffer.length = 0;

    emscripten_websocket_set_onopen_callback(g_web_socket, client, net_web_on_open);
    emscripten_websocket_set_onmessage_callback(g_web_socket, client, net_web_on_message);
    emscripten_websocket_set_onerror_callback(g_web_socket, client, net_web_on_error);
    emscripten_websocket_set_onclose_callback(g_web_socket, client, net_web_on_close);

    return 1;
}

void net_web_disconnect(Client *client) {
    (void) client;

    if (!g_web_socket) return;
    emscripten_websocket_close(g_web_socket, 1000, "client quit");
}

// Page `reconnect`: skip the backoff and try again now (no-op while a socket is
// open/connecting or the embedded server runs).
void net_web_request_reconnect(Client *client) {
    if (g_web_socket || client->net_should_quit || client->offline) return;
    net_web_connect(client);
}

// Frames the packet exactly like shared/src/net.c net_send_packet() and sends
// it as a single binary WebSocket message. `socket` is the fake descriptor and
// is ignored; the WebSocket handle is module state.
int client_net_send_packet(const int socket, const NetPacketType type, const void *payload, const uint16_t length) {
    (void) socket;

    if (!g_web_socket || length > NET_MAX_PAYLOAD) return 0;

    client_net_log_packet("=>", type, payload, length);

    uint8_t frame[NET_PACKET_HEADER_SIZE + NET_MAX_PAYLOAD];
    frame[0] = (uint8_t) type;
    frame[1] = (uint8_t) (length >> 8);
    frame[2] = (uint8_t) (length & 0xff);

    if (payload && length) memcpy(frame + NET_PACKET_HEADER_SIZE, payload, length);

    return emscripten_websocket_send_binary(g_web_socket, frame, NET_PACKET_HEADER_SIZE + length) == EMSCRIPTEN_RESULT_SUCCESS;
}

// Brain matchmaking: move the session to client->net_ws_url (servers_ui.c). An open
// socket is closed and the new one opened from its close callback; an idle transport
// (the brain startup seek) connects now.
void client_net_switch_server(Client *client) {
    if (client->offline) local_game_stop(client); // leave the in-process game
    if (client->game.player_count) client_clear_players(client);
    client->in_game = 0;
    client->menu_play_pending = 0;
    client->net_should_quit = 0;
    client->net_hold_retry = 0;
    if (g_web_socket) {
        g_web_switching = 1;
        emscripten_websocket_close(g_web_socket, 1000, "switch server");
        return;
    }
    net_web_connect(client);
}
