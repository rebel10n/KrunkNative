// Native TCP transport: a background thread connects to the server, frames
// outgoing packets with net_send_packet() and feeds received bytes through a
// NetPacketBuffer into the client's inbound queue. The emscripten build uses
// net_web.c instead (WebSocket, no threads).
#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define NET_ERRNO WSAGetLastError()
#define SLEEP_MILLIS(ms) Sleep(ms)
#else
#include <netinet/in.h>
#include <arpa/inet.h>
#include <sys/socket.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/select.h>
#include <netdb.h>
#include <netinet/tcp.h>
#include <poll.h>
#define NET_ERRNO errno
#define CLOSE_SOCKET close
#define WOULD_BLOCK(err) ((err) == EAGAIN || (err) == EWOULDBLOCK)
#define SLEEP_MILLIS(ms) do { struct timespec ts = {0, (ms) * 1000000L}; nanosleep(&ts, NULL); } while (0)
#endif

#include <client.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// The main thread (input, chat, ...) and this transport thread (PONG, HELLO/JOIN) both send:
// the lock keeps their frames from interleaving on the stream. It covers the send only.
static pthread_mutex_t g_net_send_lock = PTHREAD_MUTEX_INITIALIZER;

int client_net_send_packet(const int socket, const NetPacketType type, const void *payload, const uint16_t length) {
    client_net_log_packet("=>", type, payload, length);
    pthread_mutex_lock(&g_net_send_lock);
    const int sent = net_send_packet(socket, type, payload, length);
    pthread_mutex_unlock(&g_net_send_lock);
    return sent;
}

void net_socket_shutdown(const int socket) {
    if (net_udp_client_is_virtual(socket)) { // UDP session (net_udp.c): its loop ends and says goodbye
        net_udp_client_request_close();
        return;
    }
#ifdef WIN32
    shutdown(socket, SD_BOTH);
#else
    shutdown(socket, SHUT_RDWR);
#endif
}

// Wait up to timeout_ms for the socket to become readable (instead of a fixed 1 ms sleep
// between recv() calls, which added up to a millisecond plus timer slack to every packet).
static void net_wait_readable(const int socket, const int timeout_ms) {
#ifdef WIN32
    (void) socket;
    SLEEP_MILLIS(timeout_ms > 1 ? 1 : timeout_ms);
#else
    struct pollfd pfd = {.fd = socket, .events = POLLIN};
    poll(&pfd, 1, timeout_ms);
#endif
}

int net_poll(Client *client, const int socket, NetPacketBuffer *read_buffer) {
    char buffer[NET_BUFFER_SIZE];
    int read;

    if ((read = recv(socket, buffer, sizeof(buffer), 0)) <= 0) {
#ifdef WIN32
        if (read < 0 && NET_ERRNO == WSAEWOULDBLOCK) return 1;
#else
        if (read < 0 && WOULD_BLOCK(NET_ERRNO)) return 1;
#endif

        return 0;
    }

    // recv() never returns more than the buffer has room for, so a large read cannot overflow it.
    if (!net_buffer_push(read_buffer, buffer, (size_t) read)) return 0;

    NetPacket packet;
    int result;

    while ((result = net_buffer_next(read_buffer, &packet)) > 0) {
        client_net_log_packet("<=", packet.type, packet.payload, packet.length);
        // "pi" -> "po" right here: Krunker answers from the socket's onmessage, not the render
        // loop. Through the per-frame queue the RTT included the wait for the next frame and,
        // on connect, the whole map load (the first sample read ~4 s and the EWMA took a
        // minute to come back down).
        if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) {
            client_net_send_packet(socket, NET_PACKET_PONG, packet.payload, packet.length);
            continue;
        }
        client_net_queue_packet(client, &packet);
    }

    if (result < 0) return 0;

    return 1;
}

// Retry backoff between failed connects / after a dropped session, sliced so
// quitting (net_should_quit) and the page's `reconnect` (net_retry_now) are
// honoured promptly.
#define NET_RETRY_MILLIS 2000
// Give up on a single connect attempt after this long (unreachable hosts).
#define NET_CONNECT_TIMEOUT_MILLIS 5000

static void net_retry_sleep(Client *client) {
    // After a server ERROR (kick) wait for the page's `reconnect` instead of the backoff.
    for (int waited = 0; (client->net_hold_retry || waited < NET_RETRY_MILLIS) && !client->net_should_quit; waited += 100) {
        if (client->net_retry_now) break;
        SLEEP_MILLIS(100);
    }

    client->net_hold_retry = 0;

    client->net_retry_now = 0;
}

static void net_set_non_blocking(const int client_socket) {
#ifdef WIN32
    unsigned long non_blocking = 1;
    ioctlsocket(client_socket, FIONBIO, &non_blocking);
#else
    int flags = fcntl(client_socket, F_GETFL, 0);
    fcntl(client_socket, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Non-blocking connect with a timeout, polled in 100 ms slices so a quit does
// not wait on an unreachable host. Returns 0 when connected, otherwise the
// socket error (errno / WSA code).
static int net_connect_with_timeout(Client *client, const int client_socket, const struct sockaddr_in *address) {
    net_set_non_blocking(client_socket);

    if (connect(client_socket, (const struct sockaddr *) address, sizeof(*address)) == 0) return 0;

    const int connect_error = NET_ERRNO;
#ifdef WIN32
    if (connect_error != WSAEWOULDBLOCK) return connect_error;
#else
    if (connect_error != EINPROGRESS) return connect_error;
#endif

    for (int waited = 0; waited < NET_CONNECT_TIMEOUT_MILLIS; waited += 100) {
#ifdef WIN32
        if (client->net_should_quit) return WSAECONNABORTED;
#else
        if (client->net_should_quit) return ECONNABORTED;
#endif

        fd_set write_set, error_set;
        FD_ZERO(&write_set);
        FD_ZERO(&error_set);
        FD_SET(client_socket, &write_set);
        FD_SET(client_socket, &error_set);

        struct timeval timeout = {0, 100 * 1000};
        const int ready = select(client_socket + 1, NULL, &write_set, &error_set, &timeout);

        if (ready < 0) {
            const int select_error = NET_ERRNO;
#ifndef WIN32
            if (select_error == EINTR) continue;
#endif
            return select_error;
        }

        if (ready == 0) continue;

        int socket_error = 0;
        socklen_t length = sizeof(socket_error);
        if (getsockopt(client_socket, SOL_SOCKET, SO_ERROR, (void *) &socket_error, &length) < 0) return NET_ERRNO;
        return socket_error;
    }

#ifdef WIN32
    return WSAETIMEDOUT;
#else
    return ETIMEDOUT;
#endif
}

static void net_close_socket(const int client_socket) {
#ifdef WIN32
    closesocket(client_socket);
#else
    CLOSE_SOCKET(client_socket);
#endif
}

// Thread entry. Connects to client->net_address:net_port (NetMainArgs.address
// overrides the address when set) and keeps retrying every NET_RETRY_MILLIS
// until net_should_quit, so a server started after the client is picked up and
// a dropped session reconnects. Every failure is reported through
// on_client_socket_error() for the menu's connection error screen.
void *net_main(void *user_data) {
    NetMainArgs *args = user_data;
    Client *client = args->client;

#ifdef WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    if (args->address && args->address[0] && args->address != client->net_address) {
        strncpy(client->net_address, args->address, sizeof(client->net_address) - 1);
        client->net_address[sizeof(client->net_address) - 1] = 0;
    }

    if (!client->net_port) client->net_port = NET_DEFAULT_PORT;

    struct sockaddr_in server_address;

    while (!client->net_should_quit) {
        // Resolved per attempt: the brain's matchmaker (client_net_switch_server) can
        // point the session at another host (names or dotted quads) between attempts.
        memset(&server_address, 0, sizeof(server_address));
        server_address.sin_family = AF_INET;
        pthread_mutex_lock(&client->net_lock);
        char host[sizeof(client->net_address)];
        memcpy(host, client->net_address, sizeof(host));
        server_address.sin_port = htons(client->net_port);
        pthread_mutex_unlock(&client->net_lock);
        server_address.sin_addr.s_addr = inet_addr(host);
#ifndef WIN32
        if (server_address.sin_addr.s_addr == INADDR_NONE) {
            struct addrinfo hints = {0}, *res = NULL;
            hints.ai_family = AF_INET;
            hints.ai_socktype = SOCK_STREAM;
            if (getaddrinfo(host, NULL, &hints, &res) == 0 && res) {
                server_address.sin_addr = ((struct sockaddr_in *) res->ai_addr)->sin_addr;
                freeaddrinfo(res);
            }
        }
#endif

        // UDP first (net_udp.c, doc/transport.md); a server that does not answer the UDP
        // handshake gets the TCP session below. KRUNKNATIVE_NET_TRANSPORT=tcp|udp forces one.
        const int udp_mode = net_udp_client_mode();
        if (udp_mode >= 0 && server_address.sin_addr.s_addr != INADDR_NONE) {
            const int udp = net_udp_client_session(client, &server_address);
            if (udp != 0 || udp_mode > 0) {
                if (udp == 0 && !client->net_should_quit) on_client_socket_failed(client, "No UDP answer from the server");
                if (!client->net_should_quit) net_retry_sleep(client);
                continue;
            }
        }

        const int client_socket = (int) socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);

        if (client_socket < 0) {
            on_client_socket_error(client, NET_ERRNO);
            net_retry_sleep(client);
            continue;
        }

        const int connect_error = net_connect_with_timeout(client, client_socket, &server_address);

        if (connect_error) {
            net_close_socket(client_socket);

            if (!client->net_should_quit) {
                on_client_socket_error(client, connect_error);
                net_retry_sleep(client);
            }

            continue;
        }

        // Krunker's ws sockets run with Nagle off: without this an input or PONG written while
        // the previous segment is unacked waits for the server's delayed ACK (40 ms on Linux).
        const int nodelay = 1;
        setsockopt(client_socket, IPPROTO_TCP, TCP_NODELAY, (const char *) &nodelay, sizeof(nodelay));

        pthread_mutex_lock(&client->net_lock);
        client->net_socket = client_socket;
        pthread_mutex_unlock(&client->net_lock);

        on_client_socket_connect(client);

        const NetHelloPacket hello = {1, 0, 0};
        client_net_send_packet(client_socket, NET_PACKET_HELLO, &hello, sizeof(hello));
        client_net_send_join(client, client_socket); // brain ticket / session / guest name (net_join.c)

        NetPacketBuffer read_buffer = {};

        while (!client->net_should_quit) {
            if (!net_poll(client, client_socket, &read_buffer)) break;
            net_wait_readable(client_socket, 50); // 50 ms: net_should_quit / switch is still honoured promptly
        }

        pthread_mutex_lock(&client->net_lock);
        client->net_socket = -1;
        pthread_mutex_unlock(&client->net_lock);

        net_close_socket(client_socket);
        on_client_socket_disconnect(client);

        if (!client->net_should_quit) net_retry_sleep(client);
    }

    return NULL;
}

// Brain matchmaking: move the session to client->net_address:net_port (set by the
// caller under net_lock). A running transport drops its socket and reconnects at
// once; an idle one (brain startup, or after an offline game) is started.
void client_net_switch_server(Client *client) {
    static NetMainArgs args;

    if (client->offline) local_game_stop(client); // leave the in-process game
    if (client->game.player_count) client_clear_players(client);
    client->in_game = 0;
    client->menu_play_pending = 0;
    if (client->net_thread_running && client->net_should_quit) {
        // an offline game told the thread to quit: let it finish before starting anew
        pthread_join(client->net_thread, NULL);
        client->net_thread_running = 0;
    }
    client->net_should_quit = 0;
    client->net_hold_retry = 0;

    if (client->net_thread_running) {
        pthread_mutex_lock(&client->net_lock);
        const int socket = client->net_socket;
        pthread_mutex_unlock(&client->net_lock);
        client->net_retry_now = 1;
        if (socket >= 0) net_socket_shutdown(socket);
        return;
    }

    args.client = client;
    args.address = NULL; // client->net_address as set by the matchmaker
    if (pthread_create(&client->net_thread, NULL, net_main, &args)) {
        on_client_socket_failed(client, "Failed to start network thread");
        return;
    }
    client->net_thread_running = 1;
}
