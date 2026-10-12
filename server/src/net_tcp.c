// server_bin's network frontend: the TCP listener, the TCP transport of the server core
// (server.h ServerTransport) and the net thread that also drives the UDP transport
// (net_udp.c). Moved out of net.c unchanged; the packet handling stays in the core.
#include <server_cli.h>
#include <server_log.h>
#include <server_net_udp.h>  // UDP game transport: virtual client fds (net-udp)
#include <server_config.h>   // g_gs_config.listen_host
#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#ifdef WIN32
#include <windows.h>
#include <ws2tcpip.h> // inet_pton
#define NET_ERRNO WSAGetLastError()
#define CLOSE_SOCKET closesocket
#define WOULD_BLOCK(err) ((err) == WSAEWOULDBLOCK)
#define SLEEP_MILLIS(ms) Sleep(ms)
#else
#include <unistd.h>
#include <poll.h>
#include <netinet/tcp.h>
#define NET_ERRNO errno
#define CLOSE_SOCKET close
#define WOULD_BLOCK(err) ((err) == EAGAIN || (err) == EWOULDBLOCK)
#define SLEEP_MILLIS(ms) do { struct timespec ts = {0, (ms) * 1000000L}; nanosleep(&ts, NULL); } while (0)
#endif

#define NET_ERROR(fd, ...) { server_log_error(__VA_ARGS__); if ((fd) >= 0) CLOSE_SOCKET(fd); g_server->should_quit = 1; return; }

// When poll() last saw each client fd readable (net thread). The thread then waits for
// g_server->lock, which the tick holds while it runs: a PONG is stamped with that wake-up so
// the ping measures the connection, not how long the tick kept the lock. (Intentional
// difference: v3.1.1's io_po reads Date.now() when Node's event loop gets to the message,
// so a running tick is included there.)
#define NET_READABLE_FDS 1024
static double g_net_readable_at[NET_READABLE_FDS];

static void net_set_nonblocking(const int fd) {
#ifdef WIN32
    unsigned long non_blocking = 1;
    ioctlsocket(fd, FIONBIO, &non_blocking);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

// Krunker's ws sockets run with Nagle off (ws calls socket.setNoDelay()). With Nagle on, a
// small PING/STATE written while the previous tick's segment is unacked waits for the
// peer's delayed ACK (40 ms on Linux loopback): the random 5-130 ms local pings.
static void net_set_nodelay(const int fd) {
    const int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, (const char *) &one, sizeof(one));
}

// ------------------------------------------------------------------ TCP transport

static int tcp_poll(ClientConnection *client) {
    char buffer[1024];
    int read;

    if ((read = recv(client->fd, buffer, sizeof(buffer), 0)) <= 0) {
        if (read < 0 && WOULD_BLOCK(NET_ERRNO)) return 1;

        net_remove_client(client);
        return 0;
    }

    if (!net_buffer_push(&client->read_buffer, buffer, (size_t) read)) {
        net_remove_client(client);
        return 0;
    }

    NetPacket packet;
    int result;

    while ((result = net_buffer_next(&client->read_buffer, &packet)) > 0) {
        if (!net_handle_packet(client, &packet)) return 0;
    }

    if (result < 0) {
        net_remove_client(client);
        return 0;
    }

    return 1;
}

// Sends the batch. When the socket buffer is full (a stalled peer) nothing is written and the
// tick's snapshots are dropped: the next tick carries fresher ones (intentional: Krunker's ws
// would queue them in user space instead). A partial write is finished, since a cut frame
// would desync the stream; that only waits on a peer that is draining.
static void tcp_send_frames(const int fd, const uint8_t *frames, const size_t length) {
    size_t sent = 0;
    while (sent < length) {
#ifdef WIN32
        const int wrote = send(fd, (const char *) frames + sent, (int) (length - sent), 0);
#else
        const ssize_t wrote = send(fd, frames + sent, length - sent, MSG_NOSIGNAL);
#endif
        if (wrote > 0) {
            sent += (size_t) wrote;
            continue;
        }
        if (wrote < 0 && WOULD_BLOCK(NET_ERRNO)) {
            if (!sent) break;
#ifndef WIN32
            struct pollfd pfd = {.fd = fd, .events = POLLOUT};
            poll(&pfd, 1, 5);
#endif
            continue;
        }
        break; // closed / reset: tcp_poll() removes it on its next recv
    }
}

static void tcp_close(const int fd) {
    CLOSE_SOCKET(fd);
}

static double tcp_readable_at(const int fd) {
    return fd >= 0 && fd < NET_READABLE_FDS ? g_net_readable_at[fd] : 0.0;
}

static const ServerTransport g_tcp_transport = {
    .name = "tcp",
    .poll = tcp_poll,
    .send_frames = tcp_send_frames,
    .close = tcp_close,
    .pong_received_at = tcp_readable_at,
    .input_received_at = tcp_readable_at,
};

// ------------------------------------------------------------------ net thread

// Block until the listen socket or a client socket has data (5 ms at most, so pending kicks
// and should_quit are still seen promptly). A fixed 1 ms sleep here delayed every PONG
// (and input) by up to a millisecond plus timer slack.
#define NET_WAIT_MAX_FDS 128
#define NET_WAIT_TIMEOUT_MS 5
static void net_wait_readable(const int server_socket) {
#ifdef WIN32
    (void) server_socket;
    SLEEP_MILLIS(1);
#else
    struct pollfd fds[NET_WAIT_MAX_FDS];
    nfds_t count = 0;
    fds[count++] = (struct pollfd) {.fd = server_socket, .events = POLLIN};

    const int udp_fd = net_udp_fd();
    if (udp_fd >= 0) fds[count++] = (struct pollfd) {.fd = udp_fd, .events = POLLIN};
    const nfds_t first_client = count;

    pthread_mutex_lock(&g_server->lock);
    for (size_t i = 0; i < g_server->client_count && count < NET_WAIT_MAX_FDS; i++) {
        if (g_server->clients[i].transport != &g_tcp_transport) continue; // UDP peers share udp_fd
        fds[count++] = (struct pollfd) {.fd = g_server->clients[i].fd, .events = POLLIN};
    }
    pthread_mutex_unlock(&g_server->lock);

    // a client closed meanwhile only makes this return early (POLLNVAL)
    if (poll(fds, count, NET_WAIT_TIMEOUT_MS) <= 0) return;

    const double now = server_time();
    for (nfds_t i = first_client; i < count; i++) {
        if ((fds[i].revents & POLLIN) && fds[i].fd >= 0 && fds[i].fd < NET_READABLE_FDS) g_net_readable_at[fds[i].fd] = now;
    }
#endif
}

void net_main() {

#ifdef WIN32
    WSADATA wsa_data;
    WSAStartup(MAKEWORD(2, 2), &wsa_data);
#endif

    const int server_socket = (int) socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (server_socket < 0) NET_ERROR(-1, "Failed to create server socket, quitting...");

    // Rebind right after a restart even while old connections sit in TIME_WAIT.
    // Windows' SO_REUSEADDR would let a second server share a listening port, so
    // it gets SO_EXCLUSIVEADDRUSE instead (TIME_WAIT never blocks a bind there).
    const int reuse = 1;
#ifdef WIN32
    setsockopt(server_socket, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, (const char *) &reuse, sizeof(reuse));
#else
    setsockopt(server_socket, SOL_SOCKET, SO_REUSEADDR, (const char *) &reuse, sizeof(reuse));
#endif

    // Match events leave the shared game through these hooks (see GameHooks).
    pthread_mutex_lock(&g_server->lock);
    net_install_game_hooks();
    pthread_mutex_unlock(&g_server->lock);

    struct sockaddr_in listen_addr;

    listen_addr.sin_family = AF_INET;
    listen_addr.sin_addr.s_addr = INADDR_ANY;
    if (g_gs_config.listen_host[0] && strcmp(g_gs_config.listen_host, "0.0.0.0") &&
        inet_pton(AF_INET, g_gs_config.listen_host, &listen_addr.sin_addr) != 1) {
        NET_ERROR(server_socket, "Invalid gameserver.listen_host '%s' (IPv4 address expected), quitting...", g_gs_config.listen_host);
    }
    listen_addr.sin_port = htons(g_server->config.listen_port);

    if (bind(server_socket, (struct sockaddr *) &listen_addr, sizeof(listen_addr)) < 0) {
        NET_ERROR(server_socket, "Failed to bind to port %d (is it in use?), quitting...", g_server->config.listen_port);
    }

    if (listen(server_socket, 5) < 0) NET_ERROR(server_socket, "Failed to listen on port %d, quitting...", g_server->config.listen_port);

    net_set_nonblocking(server_socket);

    server_log("Listening on port %d", g_server->config.listen_port);

    // UDP game transport on the same port number (KRUNKNATIVE_NET_UDP=0: TCP only). It also
    // answers server-info queries, so info_query.c opens no socket of its own.
    if (net_udp_enabled()) net_udp_open(g_gs_config.listen_host, g_server->config.listen_port);

    while (!g_server->should_quit) {
        net_udp_receive(); // datagrams in (stamps PONG arrival before the lock), acks/resends out

        pthread_mutex_lock(&g_server->lock);

        net_udp_service(); // new UDP peers -> net_add_client, dead ones flagged
        net_poll_clients(); // every connection's packets, then chat /kick, /ban, /banip (and the CLI running them)

        pthread_mutex_unlock(&g_server->lock);
        net_udp_flush_all(); // the handlers' replies leave now, not after the next poll

        struct sockaddr_in client_addr;
#ifdef WIN32
        int client_addr_size = sizeof(client_addr);
#else
        socklen_t client_addr_size = sizeof(client_addr);
#endif

        const int client = (int) accept(server_socket, (struct  sockaddr *) &client_addr, &client_addr_size);

        if (client < 0) {
            if (WOULD_BLOCK(NET_ERRNO)) {
                net_wait_readable(server_socket);
                continue;
            }

            break;
        }

        net_set_nonblocking(client);
        net_set_nodelay(client);

        ClientConnection conn = {};

        conn.fd = client;
        conn.addr = client_addr;
        conn.transport = &g_tcp_transport;

        pthread_mutex_lock(&g_server->lock);
        net_add_client(&conn);
        pthread_mutex_unlock(&g_server->lock);
    }

    CLOSE_SOCKET(server_socket);
}

void net_cleanup() {
    net_udp_shutdown(); // after server_disconnect_all: the last ERROR / LEAVE still reach UDP peers
#ifdef WIN32
    WSACleanup();
#endif
}
