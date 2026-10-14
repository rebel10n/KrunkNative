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
//   3. `Module.krunkNativeBridgeUrl` (loader.js, <meta name="kn-bridge">: the deployed
//      net-bridge, e.g. wss://play.krunk.io/native; deploy/pages/stage.py)
//   4. `(https: ? wss : ws)://<location.host>/native`
// The brain matchmaker (servers_ui.c) overrides all of them with Client.net_ws_url, built
// by net_web_target_url() from the same chain plus `?target=host:port`.
#include <client.h>
#include <krunker_net.h>
#include <emscripten.h>
#include <emscripten/websocket.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Fake descriptor stored in client->net_socket while the WebSocket is open.
#define NET_WEB_FAKE_FD 1

// Reconnect backoff after a failed/dropped WebSocket (same as the native thread, net.c):
// 2 s doubling per consecutive failure (a short session counts) up to 30 s.
#define NET_WEB_RETRY_MILLIS 2000
#define NET_WEB_RETRY_MAX_MILLIS 30000
#define NET_WEB_SESSION_STABLE_MILLIS 10000.0

static EMSCRIPTEN_WEBSOCKET_T g_web_socket;
static NetPacketBuffer g_web_read_buffer;
static char g_web_url[512];          // resolved endpoint, for error messages
static int g_web_reconnect_pending;  // an emscripten_async_call reconnect is scheduled
static int g_web_switching;          // client_net_switch_server closed the socket: reconnect at once
static int g_web_failures;           // consecutive failed connects / short sessions (backoff)
static double g_web_opened_at;       // emscripten_get_now() of the last open

static void net_web_reconnect_later(void *user_data) {
    Client *client = user_data;

    g_web_reconnect_pending = 0;
    if (client->net_should_quit || client->offline || g_web_socket) return;

    net_web_connect(client);
}

static void net_web_schedule_reconnect(Client *client) {
    if (g_web_reconnect_pending || client->net_should_quit || client->offline) return;
    if (client->net_hold_retry) return; // server ERROR: only the page's `reconnect` tries again

    g_web_failures++;
    int delay = NET_WEB_RETRY_MILLIS;
    for (int i = 1; i < g_web_failures && delay < NET_WEB_RETRY_MAX_MILLIS; i++) delay *= 2;
    if (delay > NET_WEB_RETRY_MAX_MILLIS) delay = NET_WEB_RETRY_MAX_MILLIS;
    printf("[net] retrying in %d s (failure %d)\n", delay / 1000, g_web_failures);
    g_web_reconnect_pending = 1;
    emscripten_async_call(net_web_reconnect_later, client, delay);
}

static char *net_web_resolve_url(Client *client) {
    // 0. the brain matchmaker's target (servers_ui.c: seek webUrl or /native?target=host:port)
    if (client->net_ws_url[0]) return strdup(client->net_ws_url);
    return (char *) EM_ASM_PTR({
        var url = null;
        try { url = new URLSearchParams(location.search).get('ws'); } catch (e) {}
        if (!url && typeof Module !== 'undefined' && Module['krunkNativeWsUrl']) url = Module['krunkNativeWsUrl'];
        if (!url && typeof Module !== 'undefined' && Module['krunkNativeBridgeUrl']) url = Module['krunkNativeBridgeUrl'];
        if (!url) url = (location.protocol === 'https:' ? 'wss' : 'ws') + '://' + location.host + '/native';
        return stringToNewUTF8(url);
    });
}

// The bridge URL for the game server host:port (brain matchmaking, direct connect): the
// endpoint of the chain above with `target=host:port` added. A `?ws=` URL that already
// names a target is used as it is (pins every session to that server).
void net_web_target_url(char *out, const size_t size, const char *host, const int port) {
    char *url = (char *) EM_ASM_PTR({
        var base = null; // (no comma-separated declarations: EM_ASM is a macro)
        var target = UTF8ToString($0) + ':' + $1;
        try { base = new URLSearchParams(location.search).get('ws'); } catch (e) {}
        if (base && /[?&]target=/.test(base)) return stringToNewUTF8(base);
        if (!base && typeof Module !== 'undefined' && Module['krunkNativeWsUrl']) base = Module['krunkNativeWsUrl'];
        if (!base && typeof Module !== 'undefined' && Module['krunkNativeBridgeUrl']) base = Module['krunkNativeBridgeUrl'];
        if (!base) base = (location.protocol === 'https:' ? 'wss' : 'ws') + '://' + location.host + '/native';
        return stringToNewUTF8(base + (base.indexOf('?') < 0 ? '?' : '&') + 'target=' + target);
    }, host ? host : "", port);
    snprintf(out, size, "%s", url ? url : "");
    free(url);
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

    g_web_opened_at = emscripten_get_now();
    on_client_socket_connect(client);

    const NetHelloPacket hello = net_hello_make(); // the minor gates the v7 packets (KrunkScript 200-203), the patch WP-C sizes, the mask the rulesets
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

// 1 = the browser believes it is online (navigator.onLine; no API = assume so).
EM_JS(int, kn_net_online, (void), { return typeof navigator === 'undefined' || navigator.onLine !== false ? 1 : 0; });

// Why a live WebSocket session ended, for the error screen (client_net_set_drop). The codes: RFC
// 6455; the reasons are net-bridge's (webBridge.ts endSession / shutdown).
static void net_web_explain_close(Client *client, const int code, const char *reason) {
    if (!kn_net_online()) client_net_set_drop(client, "offline", code, "your network is offline (the browser reports no connection)");
    else if (code == 1012 || code == 1001)
        client_net_set_drop(client, "gateway", code, "the connection gateway is restarting%s%s", reason[0] ? ": " : "", reason);
    else if (code == 1006)
        client_net_set_drop(client, "lost", code, "the connection was lost without a close (a network drop, or a proxy closed it)");
    else if (strstr(reason, "native tcp closed")) client_net_set_drop(client, "closed", code, "the server closed the connection");
    else if (strstr(reason, "idle timeout")) client_net_set_drop(client, "timeout", code, "the connection gateway closed it after no data for a while");
    else if (strstr(reason, "native tcp")) client_net_set_drop(client, "unreachable", code, "the game server is unreachable (%s)", reason);
    else client_net_set_drop(client, "closed", code, "the WebSocket closed (code %d%s%s)", code, reason[0] ? ": " : "", reason);
}

static bool net_web_on_close(int event_type, const EmscriptenWebSocketCloseEvent *event, void *user_data) {
    (void) event_type;

    Client *client = user_data;
    if (krunker_net_active()) { // closed by client_net_release: the session was reported then, an official one may be up now
        if (g_web_socket == event->socket) g_web_socket = 0;
        emscripten_websocket_delete(event->socket);
        g_web_read_buffer.length = 0;
        return true;
    }
    if (client->net_session_connected && !g_web_switching && !client->net_should_quit) net_web_explain_close(client, event->code, event->reason);

    pthread_mutex_lock(&client->net_lock);
    if (client->net_socket == NET_WEB_FAKE_FD) client->net_socket = -1; // (an official krunker.io socket may own it now)
    pthread_mutex_unlock(&client->net_lock);

    g_web_read_buffer.length = 0;

    if (g_web_socket) {
        emscripten_websocket_delete(g_web_socket);
        g_web_socket = 0;
    }

    client_net_note(client->net_should_quit || g_web_switching ? NET_NOTE_INFO : NET_NOTE_WARN, "websocket closed (code %d%s%s)", event->code,
                    event->reason[0] ? ": " : "", event->reason);

    if (client->net_session_connected) {
        if (emscripten_get_now() - g_web_opened_at >= NET_WEB_SESSION_STABLE_MILLIS) g_web_failures = 0; // a session that lasted
        on_client_socket_disconnect(client);
    } else if (!client->offline && !client->net_should_quit) { // (client_net_stop already said why)
        char message[160]; // the bridge's close reason (e.g. "target not allowed") when it gave one
        snprintf(message, sizeof(message), "Could not connect to %s%s%s", g_web_url, event->reason[0] ? ": " : "", event->reason);
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
    if (krunker_net_active()) return 1; // an official krunker.io game owns the connection (krunker_net.c)
    if (g_web_socket) return 1; // already connecting/connected
    if (net_wt_connect(client)) return 1; // UDP-style transport first: knet over WebTransport (net_wt.c)

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
    client_net_note(NET_NOTE_INFO, "connecting to %s (game transport WebSocket)", g_web_url); // (wt-e2e.js: "connecting to ws")

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
    net_wt_close(0);

    if (!g_web_socket) return;
    emscripten_websocket_close(g_web_socket, 1000, "client quit");
}

// Page `reconnect`: skip the backoff and try again now (no-op while a socket is
// open/connecting or the embedded server runs).
void net_web_request_reconnect(Client *client) {
    if (g_web_socket || net_wt_busy() || client->net_should_quit || client->offline) return;
    g_web_failures = 0;
    net_web_connect(client);
}

void client_net_stop(Client *client, const char *reason) {
    krunker_net_leave(client, reason ? reason : "Disconnected"); // an official krunker.io game ends too
    const int had_session = client->net_session_connected;
    pthread_mutex_lock(&client->net_lock);
    if (client->net_socket >= 0) { // net_web_on_close -> on_client_socket_disconnect shows it
        snprintf(client->net_disconnect_reason, sizeof(client->net_disconnect_reason), "%s", reason ? reason : "Disconnected");
        snprintf(client->net_disconnect_code, sizeof(client->net_disconnect_code), "000000");
    }
    pthread_mutex_unlock(&client->net_lock);
    client->net_should_quit = 1; // no net_web_schedule_reconnect / wt_reconnect_later until switch_server
    g_web_failures = 0;
    net_web_disconnect(client);
    client->net_connected = 0;
    if (!had_session) on_client_socket_failed(client, reason ? reason : "Disconnected"); // stopped while (re)trying
}

// Frames the packet exactly like shared/src/net.c net_send_packet() and sends
// it as a single binary WebSocket message. `socket` is the fake descriptor and
// is ignored; the WebSocket handle is module state.
int client_net_send_packet(const int socket, const NetPacketType type, const void *payload, const uint16_t length) {
    (void) socket;

    if (krunker_net_active()) return krunker_net_send_packet(type, payload, length); // an official krunker.io game
    if (net_wt_active()) return net_wt_send(type, payload, length);
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
    krunker_net_leave(client, NULL); // from an official krunker.io game back to ours
    if (client->offline) local_game_stop(client); // leave the in-process game
    if (client->game.player_count) client_clear_players(client);
    client->in_game = 0;
    client->menu_play_pending = 0;
    client->net_should_quit = 0;
    client->net_hold_retry = 0;
    g_web_failures = 0;
    if (net_wt_busy()) {
        net_wt_close(1); // and connect to the new target
        return;
    }
    if (g_web_socket) {
        g_web_switching = 1;
        if (client->net_session_connected) client->net_close_intended = 1; // a switch, not a drop (events.c)
        emscripten_websocket_close(g_web_socket, 1000, "switch server");
        return;
    }
    net_web_connect(client);
}

// KN_KRUNKER_OFFICIAL takes the connection (krunker_net.c): close ours as an intended switch and
// keep it closed (net_should_quit) until client_net_switch_server brings it back.
void client_net_release(Client *client) {
    if (client->offline) local_game_stop(client);
    client->net_should_quit = 1;
    g_web_failures = 0;
    if (net_wt_busy()) net_wt_close(0);
    if (g_web_socket) {
        pthread_mutex_lock(&client->net_lock);
        if (client->net_socket == NET_WEB_FAKE_FD) client->net_socket = -1;
        pthread_mutex_unlock(&client->net_lock);
        if (client->net_session_connected) { // reported now: the close callback lands when krunker.io may own the session
            client->net_close_intended = 1;
            on_client_socket_disconnect(client);
        }
        emscripten_websocket_close(g_web_socket, 1000, "krunker.io");
    }
}
