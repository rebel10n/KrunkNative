// UDP game transport of server_bin (server/include/server_net_udp.h, shared/include/knet.h).
#include <server_net_udp.h>
#include <server_log.h>
#include <info_query.h>
#include <knet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifdef WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
typedef int socklen_t;
#define NET_UDP_CLOSE closesocket
#else
#include <poll.h>
#include <unistd.h>
#define NET_UDP_CLOSE close
#endif

#define NET_UDP_MAX_PEERS 256
#define NET_UDP_INBOX_MAX 4096        // queued inbound game packets per peer before it is dropped
#define NET_UDP_LINGER 2.0            // seconds a closed peer keeps resending its last reliable bytes
#define NET_UDP_FLUSH_INTERVAL 0.002  // net thread flush cadence (acks, resends, keepalives)

typedef struct {
    int used;
    int added;                    // handed to net_add_client()
    int fd;                       // virtual descriptor (NET_VIRTUAL_FD_BASE + slot, generation in the high bits)
    int state;                    // PEER_*
    struct sockaddr_in addr;
    uint32_t salt;
    KnetConn conn;
    NetPacket *inbox;             // complete game packets for the game handlers
    size_t inbox_count, inbox_head, inbox_cap;
    int overflow;
    double pong_received_at;      // arrival of the datagram that carried the last PONG
    double closed_at;
    char forwarded[KNET_FORWARDED_MAX + 1];
} UdpPeer;

enum { PEER_PENDING_ADD = 1, PEER_ACTIVE, PEER_DEAD, PEER_LINGER };

static struct {
    pthread_mutex_t lock;
    int fd;
    KnetCookieKey key;
    UdpPeer peers[NET_UDP_MAX_PEERS];
    unsigned int generation;
    KnetSim sim;
    double last_flush;
} g_udp = {.lock = PTHREAD_MUTEX_INITIALIZER, .fd = -1};

static double udp_now(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double) ts.tv_sec + (double) ts.tv_nsec / 1e9;
}

int net_udp_enabled(void) {
    const char *value = getenv("KRUNKNATIVE_NET_UDP");
    return !value || strcmp(value, "0") != 0;
}

int net_udp_fd(void) { return g_udp.fd; }

static const ServerTransport g_udp_transport;

static UdpPeer *udp_peer_by_fd(const int fd) {
    if (fd < NET_VIRTUAL_FD_BASE) return NULL;
    const int slot = (fd - NET_VIRTUAL_FD_BASE) & 0xffff;
    if (slot >= NET_UDP_MAX_PEERS) return NULL;
    UdpPeer *peer = &g_udp.peers[slot];
    return peer->used && peer->fd == fd ? peer : NULL;
}

static UdpPeer *udp_peer_by_addr(const struct sockaddr_in *addr) {
    for (int i = 0; i < NET_UDP_MAX_PEERS; i++) {
        UdpPeer *peer = &g_udp.peers[i];
        if (peer->used && peer->addr.sin_addr.s_addr == addr->sin_addr.s_addr && peer->addr.sin_port == addr->sin_port) return peer;
    }
    return NULL;
}

static void udp_sendto(const struct sockaddr_in *addr, const uint8_t *data, const size_t length) {
#ifdef WIN32
    sendto(g_udp.fd, (const char *) data, (int) length, 0, (const struct sockaddr *) addr, sizeof(*addr));
#else
    sendto(g_udp.fd, data, length, 0, (const struct sockaddr *) addr, sizeof(*addr));
#endif
}

static void udp_emit(void *user, const uint8_t *data, const size_t length) {
    const UdpPeer *peer = user;
    udp_sendto(&peer->addr, data, length);
}

static void udp_peer_free(UdpPeer *peer) {
    knet_conn_free(&peer->conn);
    free(peer->inbox);
    memset(peer, 0, sizeof(*peer));
}

// Called with g_udp.lock held (transport -> game packets).
static void udp_deliver(void *user, const uint8_t type, const uint8_t *payload, const uint16_t length) {
    UdpPeer *peer = user;
    if (peer->state != PEER_ACTIVE && peer->state != PEER_PENDING_ADD) return;
    if (length > NET_MAX_PAYLOAD) {
        peer->overflow = 1; // the TCP framing refuses it too (net_buffer_next)
        return;
    }
    if (type == NET_PACKET_PONG) peer->pong_received_at = udp_now();
    if (peer->inbox_count >= NET_UDP_INBOX_MAX) {
        peer->overflow = 1;
        return;
    }
    if (peer->inbox_head + peer->inbox_count == peer->inbox_cap) {
        if (peer->inbox_head) {
            memmove(peer->inbox, peer->inbox + peer->inbox_head, peer->inbox_count * sizeof(NetPacket));
            peer->inbox_head = 0;
        } else {
            const size_t cap = peer->inbox_cap ? peer->inbox_cap * 2 : 16;
            NetPacket *grown = realloc(peer->inbox, cap * sizeof(NetPacket));
            if (!grown) {
                peer->overflow = 1;
                return;
            }
            peer->inbox = grown;
            peer->inbox_cap = cap;
        }
    }
    NetPacket *packet = &peer->inbox[peer->inbox_head + peer->inbox_count++];
    packet->type = type;
    packet->length = length;
    if (length) memcpy(packet->payload, payload, length);
}

static int udp_send(const int fd, const uint8_t type, const void *payload, const uint16_t length) {
    pthread_mutex_lock(&g_udp.lock);
    UdpPeer *peer = udp_peer_by_fd(fd);
    int ok = 0;
    if (peer && (peer->state == PEER_ACTIVE || peer->state == PEER_PENDING_ADD)) {
        ok = knet_conn_send(&peer->conn, type, payload, length, net_packet_channel(type));
        // Krunker answers a "pi" from onmessage at once: probes and their results leave now
        // instead of with the next flush.
        if (ok && (type == NET_PACKET_PING || type == NET_PACKET_PING_RESULT)) knet_conn_flush(&peer->conn, udp_now(), udp_emit, peer);
    }
    pthread_mutex_unlock(&g_udp.lock);
    return ok;
}

static void udp_random_bytes(uint8_t *out, const size_t length) {
#ifndef WIN32
    FILE *f = fopen("/dev/urandom", "rb");
    if (f) {
        const size_t got = fread(out, 1, length, f);
        fclose(f);
        if (got == length) return;
    }
#endif
    uint64_t x = (uint64_t) time(NULL) ^ ((uint64_t) (uintptr_t) out << 17) ^ (uint64_t) (udp_now() * 1e9);
    for (size_t i = 0; i < length; i++) {
        x ^= x << 13;
        x ^= x >> 7;
        x ^= x << 17;
        out[i] = (uint8_t) (x >> 24);
    }
}

int net_udp_open(const char *host, const unsigned short port) {
    // The info-query responder (info_query.c) must not bind the same UDP port: this
    // socket answers its 0xFFFFFFFF datagrams (contract with dedicated-server).
    g_info_query_external = 1;

    const int fd = (int) socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (fd < 0) {
        server_log_error("[udp] socket() failed: %s", strerror(errno));
        g_info_query_external = 0;
        return -1;
    }
    struct sockaddr_in addr = {0};
    addr.sin_family = AF_INET;
    addr.sin_addr.s_addr = INADDR_ANY;
    if (host && host[0] && strcmp(host, "0.0.0.0")) inet_pton(AF_INET, host, &addr.sin_addr);
    addr.sin_port = htons(port);
    const int buffer = 4 * 1024 * 1024; // bursts (map streams to several joiners) without kernel drops
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, (const char *) &buffer, sizeof(buffer));
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, (const char *) &buffer, sizeof(buffer));
    if (bind(fd, (struct sockaddr *) &addr, sizeof(addr)) < 0) {
        server_log_error("[udp] cannot bind UDP port %u: %s (UDP clients and server-info queries disabled)", (unsigned int) port, strerror(errno));
        NET_UDP_CLOSE(fd);
        g_info_query_external = 0;
        return -1;
    }
#ifdef WIN32
    unsigned long non_blocking = 1;
    ioctlsocket(fd, FIONBIO, &non_blocking);
#else
    fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK);
#endif
    udp_random_bytes(g_udp.key.key, sizeof(g_udp.key.key));
    uint64_t seed = 0;
    udp_random_bytes((uint8_t *) &seed, sizeof(seed));
    knet_sim_from_env(&g_udp.sim, seed);
    if (knet_sim_active(&g_udp.sim)) {
        server_log("[udp] loss simulation: drop %d%% dup %d%% reorder %d%% (inbound)", g_udp.sim.drop, g_udp.sim.dup, g_udp.sim.reorder);
    }
    g_udp.fd = fd;
    net_set_virtual_send(udp_send);
    server_transport_register(&g_udp_transport); // net_broadcast_states flushes it every tick
    server_log("Listening on UDP port %u (game + server-info query)", (unsigned int) port);
    return fd;
}

static UdpPeer *udp_peer_alloc(void) {
    for (int i = 0; i < NET_UDP_MAX_PEERS; i++) {
        UdpPeer *peer = &g_udp.peers[i];
        if (peer->used) continue;
        memset(peer, 0, sizeof(*peer));
        peer->used = 1;
        g_udp.generation = (g_udp.generation + 1) & 0xfff;
        peer->fd = NET_VIRTUAL_FD_BASE + i + (int) (g_udp.generation << 16);
        return peer;
    }
    return NULL;
}

static uint32_t udp_token(void) {
    uint32_t token = 0;
    while (!token) udp_random_bytes((uint8_t *) &token, sizeof(token));
    return token;
}

// The forwarded address of a net-bridge/wt relay is trusted from this machine only, the
// same rule as NET_PACKET_FORWARDED over TCP (net_handle_forwarded): it is replayed as
// that packet, first in the inbox.
static void udp_handshake(const struct sockaddr_in *from, const uint8_t *in, const size_t length, const double now) {
    uint8_t out[KNET_MTU];
    int accepted = 0;
    KnetConnectInfo info;
    const size_t n = knet_server_handshake(&g_udp.key, from->sin_addr.s_addr, from->sin_port, now, in, length, out, &accepted, &info);
    if (n) {
        udp_sendto(from, out, n);
        return;
    }
    if (!accepted) return;

    UdpPeer *peer = udp_peer_by_addr(from);
    if (peer && peer->salt == info.salt && peer->state != PEER_DEAD && peer->state != PEER_LINGER) {
        udp_sendto(from, out, knet_build_accept(out, info.salt, peer->conn.token)); // our ACCEPT was lost
        return;
    }
    if (peer) {
        // the same address:port with a new salt: the old session is over
        if (peer->state == PEER_ACTIVE) peer->state = PEER_DEAD;
        else if (peer->state != PEER_DEAD) udp_peer_free(peer);
    }
    peer = udp_peer_alloc();
    if (!peer) {
        udp_sendto(from, out, knet_build_deny(out, info.salt, "Server full"));
        return;
    }
    peer->state = PEER_PENDING_ADD;
    peer->addr = *from;
    peer->salt = info.salt;
    knet_conn_init(&peer->conn, udp_token(), now);
    const int loopback = (ntohl(from->sin_addr.s_addr) >> 24) == 127;
    if (info.forwarded[0] && loopback) snprintf(peer->forwarded, sizeof(peer->forwarded), "%s", info.forwarded);
    if (peer->forwarded[0]) udp_deliver(peer, NET_PACKET_FORWARDED, (const uint8_t *) peer->forwarded, (uint16_t) strlen(peer->forwarded));
    udp_sendto(from, out, knet_build_accept(out, info.salt, peer->conn.token));
}

typedef struct {
    struct sockaddr_in from;
    double now;
} UdpDatagramContext;

static void udp_handle_datagram(void *user, const uint8_t *in, const size_t length) {
    const UdpDatagramContext *ctx = user;
    if (!length) return;
    if (server_info_is_query(in, length)) {
        uint8_t reply[1400];
        pthread_mutex_unlock(&g_udp.lock); // the snapshot has its own lock; keep ours short
        const size_t n = server_info_handle_datagram(in, length, reply, sizeof(reply));
        pthread_mutex_lock(&g_udp.lock);
        if (n) udp_sendto(&ctx->from, reply, n);
        return;
    }
    switch (in[0]) {
        case KNET_CONNECT_REQUEST:
        case KNET_CONNECT_RESPONSE:
            udp_handshake(&ctx->from, in, length, ctx->now);
            return;
        case KNET_DATA:
        case KNET_DISCONNECT: {
            UdpPeer *peer = udp_peer_by_addr(&ctx->from);
            if (!peer || knet_datagram_token(in, length) != peer->conn.token) return;
            int closed = 0;
            knet_conn_receive(&peer->conn, in, length, ctx->now, udp_deliver, peer, &closed);
            if (closed) {
                if (peer->state == PEER_LINGER) udp_peer_free(peer);
                else peer->state = PEER_DEAD;
            }
            return;
        }
        default:
            return;
    }
}

static void udp_flush_locked(const double now) {
    for (int i = 0; i < NET_UDP_MAX_PEERS; i++) {
        UdpPeer *peer = &g_udp.peers[i];
        if (!peer->used || peer->state == PEER_DEAD) continue;
        if (knet_conn_timed_out(&peer->conn, now) || peer->conn.failed || peer->overflow) {
            if (peer->state == PEER_LINGER) {
                udp_peer_free(peer);
                continue;
            }
            if (peer->state == PEER_ACTIVE) server_log("[udp] %s:%u %s", inet_ntoa(peer->addr.sin_addr), (unsigned int) ntohs(peer->addr.sin_port),
                                                         peer->overflow ? "flooded its inbox" : peer->conn.failed ? "stopped acking" : "timed out");
            peer->state = PEER_DEAD;
            continue;
        }
        knet_conn_flush(&peer->conn, now, udp_emit, peer);
        if (peer->state == PEER_LINGER && (!knet_conn_unacked(&peer->conn) || now - peer->closed_at > NET_UDP_LINGER)) {
            uint8_t out[8];
            udp_emit(peer, out, knet_build_disconnect(out, peer->conn.token));
            udp_peer_free(peer);
        }
    }
    g_udp.last_flush = now;
}

void net_udp_receive(void) {
    if (g_udp.fd < 0) return;
    uint8_t in[2048];
    UdpDatagramContext ctx;
    pthread_mutex_lock(&g_udp.lock);
    for (int budget = 0; budget < 4096; budget++) {
        socklen_t from_length = sizeof(ctx.from);
#ifdef WIN32
        const int got = recvfrom(g_udp.fd, (char *) in, (int) sizeof(in), 0, (struct sockaddr *) &ctx.from, &from_length);
#else
        const ssize_t got = recvfrom(g_udp.fd, in, sizeof(in), 0, (struct sockaddr *) &ctx.from, &from_length);
#endif
        if (got <= 0) break; // EAGAIN (or ICMP errors on Windows): nothing more now
        ctx.now = udp_now();
        knet_sim_filter(&g_udp.sim, in, (size_t) got, udp_handle_datagram, &ctx);
    }
    const double now = udp_now();
    if (now - g_udp.last_flush >= NET_UDP_FLUSH_INTERVAL) udp_flush_locked(now);
    pthread_mutex_unlock(&g_udp.lock);
}

void net_udp_flush_all(void) {
    if (g_udp.fd < 0) return;
    pthread_mutex_lock(&g_udp.lock);
    udp_flush_locked(udp_now());
    pthread_mutex_unlock(&g_udp.lock);
}

void net_udp_service(void) {
    if (g_udp.fd < 0) return;
    // net_add_client() sends (INIT, ...) through udp_send, which takes g_udp.lock: collect first.
    ClientConnection joiners[16];
    int joiner_count = 0;
    pthread_mutex_lock(&g_udp.lock);
    for (int i = 0; i < NET_UDP_MAX_PEERS && joiner_count < 16; i++) {
        UdpPeer *peer = &g_udp.peers[i];
        if (!peer->used) continue;
        if (peer->state == PEER_DEAD && !peer->added) {
            udp_peer_free(peer); // died before net_add_client took it
            continue;
        }
        if (peer->state != PEER_PENDING_ADD) continue;
        peer->state = PEER_ACTIVE;
        peer->added = 1; // owned by a ClientConnection now: net_remove_client -> net_udp_close frees it
        ClientConnection conn = {0};
        conn.fd = peer->fd;
        conn.addr = peer->addr;
        conn.transport = &g_udp_transport;
        joiners[joiner_count++] = conn;
    }
    pthread_mutex_unlock(&g_udp.lock);
    for (int i = 0; i < joiner_count; i++) net_add_client(&joiners[i]);
}

int net_udp_next_packet(const int fd, NetPacket *packet) {
    pthread_mutex_lock(&g_udp.lock);
    UdpPeer *peer = udp_peer_by_fd(fd);
    int result = -1;
    if (peer && peer->inbox_count) {
        *packet = peer->inbox[peer->inbox_head++];
        if (!--peer->inbox_count) peer->inbox_head = 0;
        result = 1;
    } else if (peer && (peer->state == PEER_ACTIVE || peer->state == PEER_PENDING_ADD)) {
        result = 0;
    }
    pthread_mutex_unlock(&g_udp.lock);
    return result;
}

double net_udp_pong_received_at(const int fd) {
    pthread_mutex_lock(&g_udp.lock);
    const UdpPeer *peer = udp_peer_by_fd(fd);
    const double at = peer ? peer->pong_received_at : 0.0;
    pthread_mutex_unlock(&g_udp.lock);
    return at;
}

void net_udp_close(const int fd) {
    pthread_mutex_lock(&g_udp.lock);
    UdpPeer *peer = udp_peer_by_fd(fd);
    if (peer) {
        if (peer->state == PEER_DEAD || g_udp.fd < 0) {
            udp_peer_free(peer);
        } else {
            peer->state = PEER_LINGER; // the ERROR / LEAVE just queued still reaches it
            peer->closed_at = udp_now();
            peer->inbox_count = peer->inbox_head = 0;
            knet_conn_flush(&peer->conn, peer->closed_at, udp_emit, peer);
        }
    }
    pthread_mutex_unlock(&g_udp.lock);
}

void net_udp_shutdown(void) {
    if (g_udp.fd < 0) return;
    // Up to 300 ms for the lingering peers' last reliable bytes (kick reasons, "server stopping").
    for (int i = 0; i < 60; i++) {
        net_udp_receive();
        net_udp_flush_all();
        int lingering = 0;
        pthread_mutex_lock(&g_udp.lock);
        for (int p = 0; p < NET_UDP_MAX_PEERS; p++) lingering += g_udp.peers[p].used && g_udp.peers[p].state == PEER_LINGER;
        pthread_mutex_unlock(&g_udp.lock);
        if (!lingering) break;
#ifdef WIN32
        Sleep(5);
#else
        const struct timespec ts = {0, 5000000L};
        nanosleep(&ts, NULL);
#endif
    }
    pthread_mutex_lock(&g_udp.lock);
    for (int p = 0; p < NET_UDP_MAX_PEERS; p++) {
        UdpPeer *peer = &g_udp.peers[p];
        if (!peer->used) continue;
        uint8_t out[8];
        udp_emit(peer, out, knet_build_disconnect(out, peer->conn.token));
        udp_peer_free(peer);
    }
    net_set_virtual_send(NULL);
    server_transport_unregister(&g_udp_transport);
    NET_UDP_CLOSE(g_udp.fd);
    g_udp.fd = -1;
    pthread_mutex_unlock(&g_udp.lock);
}

// ------------------------------------------------------------------ server core transport

// The connection's knet inbox: whole packets (server.h ServerTransport.poll).
static int udp_transport_poll(ClientConnection *client) {
    NetPacket packet;
    int result;
    while ((result = net_udp_next_packet(client->fd, &packet)) > 0) {
        if (!net_handle_packet(client, &packet)) return 0;
    }
    if (result < 0) {
        net_remove_client(client);
        return 0;
    }
    return 1;
}

// One knet message per frame (STATE rides the unreliable channel).
static void udp_transport_send_frames(const int fd, const uint8_t *frames, const size_t length) {
    for (size_t at = 0; at + NET_PACKET_HEADER_SIZE <= length;) {
        const uint16_t size = (uint16_t) ((frames[at + 1] << 8) | frames[at + 2]);
        net_send_packet(fd, (NetPacketType) frames[at], frames + at + NET_PACKET_HEADER_SIZE, size);
        at += NET_PACKET_HEADER_SIZE + size;
    }
}

static double udp_transport_input_received_at(const int fd) {
    (void) fd;
    return 0.0; // inputs are stamped when the tick handles them
}

static const ServerTransport g_udp_transport = {
    .name = "udp",
    .poll = udp_transport_poll,
    .send_frames = udp_transport_send_frames,
    .close = net_udp_close,
    .pong_received_at = net_udp_pong_received_at,
    .input_received_at = udp_transport_input_received_at,
    .flush = net_udp_flush_all,
};
