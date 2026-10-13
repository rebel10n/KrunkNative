// Native UDP game transport of the client (shared/include/knet.h, doc/transport.md).
// net_main() (net.c) runs one session per connect attempt here first; a server that never
// answers the UDP handshake (TCP-only / older server_bin, UDP blocked) falls back to TCP.
// KRUNKNATIVE_NET_TRANSPORT=tcp skips UDP, =udp never falls back.
//
// client->net_socket holds a virtual descriptor while the session is up, so every
// client_net_send_packet() caller works unchanged: net_send_packet() hands virtual fds to
// net_udp_client_send() below (any thread; g_cu.lock serialises the KnetConn).
#ifndef __EMSCRIPTEN__
#include <client.h>
#include <knet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#define CU_CLOSE closesocket
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>
#define CU_CLOSE close
#endif

#define CU_VIRTUAL_FD (NET_VIRTUAL_FD_BASE + 1)
#define CU_HANDSHAKE_TIMEOUT 1.5 // seconds without CHALLENGE / ACCEPT before falling back to TCP

static struct {
    pthread_mutex_t lock;
    int fd;
    int active;
    volatile int close_requested;
    KnetConn conn;
    KnetSim sim;
    Client *client;
} g_cu = {.lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1};

static double cu_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

static void cu_emit(void *user, const uint8_t *data, const size_t length) {
    (void) user;
#ifdef WIN32
    send(g_cu.fd, (const char *) data, (int) length, 0);
#else
    send(g_cu.fd, data, length, 0);
#endif
}

// net_send_packet() for the virtual fd: queue, then send now (inputs and pongs must not
// wait for the transport thread's next wake-up; knet packs whatever is queued).
static int net_udp_client_send(const int fd, const uint8_t type, const void *payload, const uint16_t length) {
    if (fd != CU_VIRTUAL_FD) return 0;
    pthread_mutex_lock(&g_cu.lock);
    int ok = 0;
    if (g_cu.active) {
        ok = knet_conn_send(&g_cu.conn, type, payload, length, net_packet_channel(type));
        if (ok) knet_conn_flush(&g_cu.conn, cu_now(), cu_emit, NULL);
    }
    pthread_mutex_unlock(&g_cu.lock);
    return ok;
}

int net_udp_client_is_virtual(const int socket) {
    return socket >= NET_VIRTUAL_FD_BASE && socket < NET_LOOPBACK_FD_BASE; // not the offline sandbox's loopback fds
}

// netinfo (client_net_info): the knet session's counters.
void net_udp_client_info(char *out, const size_t size) {
    out[0] = 0;
    pthread_mutex_lock(&g_cu.lock);
    if (g_cu.active) {
        const KnetStats *st = &g_cu.conn.stats;
        snprintf(out, size, "knet: rtt %d ms, sent %llu / received %llu datagrams, %llu resent segments, %llu duplicate\n",
                 knet_conn_rtt_ms(&g_cu.conn), (unsigned long long) st->datagrams_sent, (unsigned long long) st->datagrams_received,
                 (unsigned long long) st->segments_resent, (unsigned long long) st->duplicates_received);
    }
    pthread_mutex_unlock(&g_cu.lock);
}

// net_socket_shutdown() for the virtual fd: end the session from the transport thread.
void net_udp_client_request_close(void) {
    g_cu.close_requested = 1;
}

// 0 = default (UDP, TCP fallback), 1 = UDP only, -1 = TCP only
int net_udp_client_mode(void) {
    const char *value = getenv("KRUNKNATIVE_NET_TRANSPORT");
    if (!value || !value[0]) return 0;
    if (!strcmp(value, "tcp")) return -1;
    if (!strcmp(value, "udp")) return 1;
    return 0;
}

// Called with g_cu.lock held, on the transport thread.
static void cu_deliver(void *user, const uint8_t type, const uint8_t *payload, const uint16_t length) {
    Client *client = user;
    if (length > NET_MAX_PAYLOAD) return;
    NetPacket packet;
    packet.type = type;
    packet.length = length;
    if (length) memcpy(packet.payload, payload, length);
    client_net_log_packet("<=", packet.type, packet.payload, packet.length);
    // "pi" -> "po" from the receive path, like net_poll() (Krunker answers in onmessage)
    if (packet.type == NET_PACKET_PING && packet.length == sizeof(NetPingPacket)) {
        client_net_log_packet("=>", NET_PACKET_PONG, packet.payload, packet.length);
        if (knet_conn_send(&g_cu.conn, NET_PACKET_PONG, packet.payload, packet.length, KNET_SEND_REDUNDANT)) {
            knet_conn_flush(&g_cu.conn, cu_now(), cu_emit, NULL);
        }
        return;
    }
    client_net_queue_packet(client, &packet);
}

static void cu_sim_receive(void *user, const uint8_t *data, const size_t length) {
    int *closed = user;
    int c = 0;
    knet_conn_receive(&g_cu.conn, data, length, cu_now(), cu_deliver, g_cu.client, &c);
    if (c) *closed = 1;
}

static int cu_wait(const int timeout_ms) {
#ifdef WIN32
    fd_set set;
    FD_ZERO(&set);
    FD_SET((SOCKET) g_cu.fd, &set);
    struct timeval tv = {0, timeout_ms * 1000};
    return select(0, &set, NULL, NULL, &tv) > 0;
#else
    struct pollfd pfd = {.fd = g_cu.fd, .events = POLLIN};
    return poll(&pfd, 1, timeout_ms) > 0;
#endif
}

static long cu_recv(uint8_t *buf, const size_t cap) {
#ifdef WIN32
    return recv(g_cu.fd, (char *) buf, (int) cap, 0);
#else
    return (long) recv(g_cu.fd, buf, cap, 0);
#endif
}

static uint32_t cu_random32(void) {
    uint32_t value = 0;
#ifndef WIN32
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        if (fread(&value, sizeof(value), 1, f) != 1) value = 0;
        fclose(f);
    }
#endif
    if (!value) value = (uint32_t) (cu_now() * 1e6) ^ (uint32_t) (uintptr_t) &value ^ (uint32_t) time(NULL);
    return value ? value : 1u;
}

// Handshake: 1 = accepted (*token), 0 = no answer (fall back), -1 = denied / socket error.
static int cu_handshake(Client *client, uint32_t *token) {
    const uint32_t salt = cu_random32();
    uint8_t out[KNET_MTU], in[2048];
    size_t out_length = knet_build_connect_request(out, salt);
    const double started = cu_now();
    double sent_at = -1.0;

    while (!client->net_should_quit && !client->net_retry_now) {
        const double now = cu_now();
        if (now - started > CU_HANDSHAKE_TIMEOUT) return 0;
        if (now - sent_at >= KNET_HANDSHAKE_RESEND) {
            cu_emit(NULL, out, out_length);
            sent_at = now;
        }
        if (!cu_wait(50)) continue;
        const long got = cu_recv(in, sizeof(in));
        if (got <= 0) {
#ifndef WIN32
            if (got < 0 && (errno == EAGAIN || errno == EWOULDBLOCK || errno == EINTR)) continue;
#endif
            return 0; // ICMP port unreachable (ECONNREFUSED): no UDP listener there
        }
        const size_t answer = knet_client_answer_challenge(in, (size_t) got, salt, out);
        if (answer) {
            out_length = answer; // resend the response (with the cookie) until ACCEPT
            cu_emit(NULL, out, out_length);
            sent_at = cu_now();
            continue;
        }
        char reason[128] = "";
        const int accepted = knet_client_read_accept(in, (size_t) got, salt, token, reason, sizeof(reason));
        if (accepted > 0) return 1;
        if (accepted < 0) {
            char message[160];
            snprintf(message, sizeof(message), "Server refused the connection: %s", reason[0] ? reason : "denied");
            on_client_socket_failed(client, message);
            return -1;
        }
    }
    return -1;
}
// One UDP session to address. Returns 1 when a session ran (and ended), 0 when the server
// did not answer the handshake (the caller falls back to TCP), -1 on a refusal / error.
int net_udp_client_session(Client *client, const struct sockaddr_in *address) {
    const int fd = (int) socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) return 0;
    if (connect(fd, (const struct sockaddr *) address, sizeof(*address)) < 0) {
        CU_CLOSE(fd);
        return 0;
    }
#ifdef WIN32
    unsigned long non_blocking = 1;
    ioctlsocket(fd, FIONBIO, &non_blocking);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    const int buffer = 2 * 1024 * 1024; // a map stream arrives in one burst
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *) &buffer, sizeof(buffer));

    g_cu.fd = fd;
    g_cu.client = client;
    g_cu.close_requested = 0;
    uint32_t token = 0;
    const int handshake = cu_handshake(client, &token);
    if (handshake <= 0) {
        CU_CLOSE(fd);
        g_cu.fd = -1;
        return handshake;
    }

    pthread_mutex_lock(&g_cu.lock);
    knet_conn_init(&g_cu.conn, token, cu_now());
    knet_sim_from_env(&g_cu.sim, ((uint64_t) token << 32) | cu_random32());
    g_cu.active = 1;
    pthread_mutex_unlock(&g_cu.lock);
    net_set_virtual_send(net_udp_client_send);
    if (knet_sim_active(&g_cu.sim)) {
        printf("[udp] loss simulation: drop %d%% dup %d%% reorder %d%% (inbound)\n", g_cu.sim.drop, g_cu.sim.dup, g_cu.sim.reorder);
    }

    pthread_mutex_lock(&client->net_lock);
    client->net_socket = CU_VIRTUAL_FD;
    pthread_mutex_unlock(&client->net_lock);
    client_net_note(NET_NOTE_INFO, "udp session established (token %08x, knet over UDP to %s:%u)", token, client->net_address,
                    (unsigned int) client->net_port);
    on_client_socket_connect(client);

    const NetHelloPacket hello = net_hello_make(); // the minor gates the v7 packets (KrunkScript 200-203), the patch WP-C sizes, the mask the rulesets
    client_net_send_packet(CU_VIRTUAL_FD, NET_PACKET_HELLO, &hello, sizeof(hello));
    client_net_send_join(client, CU_VIRTUAL_FD); // brain ticket / session / guest name (net_join.c)

    uint8_t in[2048];
    int closed = 0;
    const char *why = "closed";
    while (!client->net_should_quit && !g_cu.close_requested && !closed) {
        cu_wait(5); // acks, resends and keepalives need a few-ms cadence
        pthread_mutex_lock(&g_cu.lock);
        for (int budget = 0; budget < 4096 && !closed; budget++) {
            const long got = cu_recv(in, sizeof(in));
            if (got <= 0) break;
            knet_sim_filter(&g_cu.sim, in, (size_t) got, cu_sim_receive, &closed);
        }
        const double now = cu_now();
        if (closed) {
            why = "server closed the session";
            client_net_set_drop(client, "closed", 0, "the server closed the connection");
        } else if (knet_conn_timed_out(&g_cu.conn, now)) {
            closed = 1;
            why = "timed out";
            client_net_set_drop(client, "timeout", 0, "no data from the server for %.0f s", KNET_TIMEOUT);
        } else if (g_cu.conn.failed) {
            closed = 1;
            why = "transport overflow";
            client_net_set_drop(client, "overflow", 0, "the connection fell too far behind (transport overflow)");
        } else {
            knet_conn_flush(&g_cu.conn, now, cu_emit, NULL);
        }
        pthread_mutex_unlock(&g_cu.lock);
    }

    pthread_mutex_lock(&client->net_lock);
    client->net_socket = -1;
    pthread_mutex_unlock(&client->net_lock);

    pthread_mutex_lock(&g_cu.lock);
    uint8_t bye[8];
    const size_t bye_length = knet_build_disconnect(bye, token);
    for (int i = 0; i < 3; i++) cu_emit(NULL, bye, bye_length); // best effort: the server times out otherwise
    client_net_note(NET_NOTE_WARN, "udp session ended (%s; sent %llu / received %llu datagrams, %llu resent segments, rtt %d ms)", why,
           (unsigned long long) g_cu.conn.stats.datagrams_sent, (unsigned long long) g_cu.conn.stats.datagrams_received,
           (unsigned long long) g_cu.conn.stats.segments_resent, knet_conn_rtt_ms(&g_cu.conn));
    g_cu.active = 0;
    knet_conn_free(&g_cu.conn);
    CU_CLOSE(fd);
    g_cu.fd = -1;
    pthread_mutex_unlock(&g_cu.lock);

    on_client_socket_disconnect(client);
    return 1;
}
#endif
