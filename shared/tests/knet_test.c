// knet transport test: two KnetConns joined by a simulated lossy link (drop / dup /
// reorder / latency), handshake cookie checks, reliable stream integrity with large
// messages, unreliable sequencing and redundant de-duplication. Prints "Test OK".
#include <knet.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond, ...) do { if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); exit(1); } } while (0)

#define LINK_MAX 20000
typedef struct {
    uint8_t data[KNET_MTU];
    size_t length;
    double at;
} Flight;

typedef struct {
    Flight flights[LINK_MAX];
    size_t count;
    double now;
    double latency;
    KnetSim sim;
} Link;

static void link_push_raw(void *user, const uint8_t *data, const size_t length) {
    Link *link = user;
    if (link->count >= LINK_MAX) return;
    Flight *f = &link->flights[link->count++];
    memcpy(f->data, data, length);
    f->length = length;
    f->at = link->now + link->latency;
}

static void link_emit(void *user, const uint8_t *data, const size_t length) {
    CHECK(length <= KNET_MTU, "datagram of %zu bytes > MTU", length);
    CHECK(data[0] != 0xFF, "knet datagram starts with 0xFF");
    Link *link = user;
    knet_sim_filter(&link->sim, data, length, link_push_raw, link);
}

typedef struct {
    uint32_t expect_seq;      // reliable: frames carry an increasing uint32 + pattern
    size_t reliable_frames;
    size_t reliable_bytes;
    int last_state;           // unreliable: last delivered generation tag
    size_t states;
    size_t stale;
    int seen_inputs[4096];
    size_t inputs, input_dups;
} Sink;

static void sink_deliver(void *user, const uint8_t type, const uint8_t *payload, const uint16_t length) {
    Sink *sink = user;
    if (type == 1) {
        uint32_t seq;
        CHECK(length >= 4, "short reliable frame");
        memcpy(&seq, payload, 4);
        CHECK(seq == sink->expect_seq, "reliable out of order: got %u want %u", seq, sink->expect_seq);
        for (uint16_t i = 4; i < length; i++) CHECK(payload[i] == (uint8_t) (seq * 31 + i), "reliable byte %u corrupted", i);
        sink->expect_seq++;
        sink->reliable_frames++;
        sink->reliable_bytes += length;
    } else if (type == 6) {
        int tag;
        memcpy(&tag, payload, 4);
        if (tag < sink->last_state) sink->stale++;
        sink->last_state = tag;
        sink->states++;
    } else if (type == 5) {
        int seq;
        memcpy(&seq, payload, 4);
        CHECK(seq >= 0 && seq < 4096, "bad input seq");
        if (sink->seen_inputs[seq]) sink->input_dups++;
        sink->seen_inputs[seq] = 1;
        sink->inputs++;
    }
}

static void link_deliver(Link *link, KnetConn *to, Sink *sink) {
    size_t kept = 0;
    for (size_t i = 0; i < link->count; i++) {
        Flight *f = &link->flights[i];
        if (f->at <= link->now) {
            int closed = 0;
            knet_conn_receive(to, f->data, f->length, link->now, sink_deliver, sink, &closed);
        } else {
            if (kept != i) link->flights[kept] = *f;
            kept++;
        }
    }
    link->count = kept;
}

static void send_reliable(KnetConn *conn, const uint32_t seq, const uint16_t length) {
    static uint8_t buf[65535];
    memcpy(buf, &seq, 4);
    for (uint16_t i = 4; i < length; i++) buf[i] = (uint8_t) (seq * 31 + i);
    CHECK(knet_conn_send(conn, 1, buf, length, KNET_SEND_RELIABLE), "reliable send refused");
}

static void test_handshake(void) {
    KnetCookieKey key;
    for (int i = 0; i < 16; i++) key.key[i] = (uint8_t) (i * 7 + 3);
    uint8_t req[KNET_CONNECT_REQUEST_SIZE], out[256], resp[64];
    const uint32_t salt = 0x12345678u, ip = 0x0100007fu;
    const uint16_t port = 0x3412;
    int accepted = 0;
    KnetConnectInfo info;

    const size_t req_len = knet_build_connect_request(req, salt);
    size_t n = knet_server_handshake(&key, ip, port, 100.0, req, req_len, out, &accepted, &info);
    CHECK(n == 13 && !accepted && n < req_len, "challenge size %zu (no amplification)", n);
    CHECK(knet_server_handshake(&key, ip, port, 100.0, req, 20, out, &accepted, &info) == 0, "short request answered");

    size_t rn = knet_client_answer_challenge(out, n, salt, resp);
    CHECK(rn == 14, "response");
    CHECK(knet_client_answer_challenge(out, n, salt + 1, resp) == 0, "foreign salt accepted");

    knet_server_handshake(&key, ip, port, 105.0, resp, rn, out, &accepted, &info);
    CHECK(accepted && info.salt == salt && !info.forwarded[0], "valid cookie refused");
    knet_server_handshake(&key, ip, port, 115.0, resp, rn, out, &accepted, &info);
    CHECK(accepted, "previous-epoch cookie refused");
    knet_server_handshake(&key, ip, port, 135.0, resp, rn, out, &accepted, &info);
    CHECK(!accepted, "expired cookie accepted");
    knet_server_handshake(&key, ip + 1, port, 105.0, resp, rn, out, &accepted, &info);
    CHECK(!accepted, "spoofed source accepted");
    resp[7] ^= 1;
    knet_server_handshake(&key, ip, port, 105.0, resp, rn, out, &accepted, &info);
    CHECK(!accepted, "tampered cookie accepted");

    uint32_t token = 0;
    char reason[64];
    n = knet_build_accept(out, salt, 0xdeadbeefu);
    CHECK(knet_client_read_accept(out, n, salt, &token, reason, sizeof(reason)) == 1 && token == 0xdeadbeefu, "accept");
    n = knet_build_deny(out, salt, "Server full");
    CHECK(knet_client_read_accept(out, n, salt, &token, reason, sizeof(reason)) == -1 && !strcmp(reason, "Server full"), "deny");
    printf("handshake OK\n");
}

// Runs a session: a sends `frames` reliable frames of mixed sizes (up to 20 KB, like the map
// stream) + per-tick STATE + redundant INPUT; b echoes nothing but acks.
static void run_session(const int drop, const int dup, const int reorder, const double latency, const char *label) {
    static Link ab, ba;
    static KnetConn a, b;
    static Sink sink_a, sink_b;
    memset(&ab, 0, sizeof(ab));
    memset(&ba, 0, sizeof(ba));
    memset(&sink_a, 0, sizeof(sink_a));
    memset(&sink_b, 0, sizeof(sink_b));
    sink_b.last_state = -1;
    ab.latency = ba.latency = latency;
    ab.sim = (KnetSim) {.drop = drop, .dup = dup, .reorder = reorder, .rng = 1234567};
    ba.sim = (KnetSim) {.drop = drop, .dup = dup, .reorder = reorder, .rng = 7654321};
    knet_conn_init(&a, 42, 0.0);
    knet_conn_init(&b, 42, 0.0);

    const uint32_t frames = 400;
    uint32_t queued = 0;
    int state_tag = 0, input_seq = 0;
    double now = 0.0;
    const double dt = 1.0 / 120.0;
    double deadline = 60.0;
    for (int tick = 0; now < deadline; tick++) {
        now = tick * dt;
        ab.now = ba.now = now;
        if (queued < frames && (tick % 2) == 0) {
            const uint16_t length = (uint16_t) (queued % 50 == 0 ? 20000 : 4 + (queued * 37) % 600);
            send_reliable(&a, queued++, length);
        }
        if ((tick % 4) == 0 && now < 20.0) {
            knet_conn_send(&a, 6, &state_tag, 4, KNET_SEND_UNRELIABLE);
            state_tag++;
        }
        if ((tick % 4) == 2 && input_seq < 4096 && now < 20.0) {
            knet_conn_send(&a, 5, &input_seq, 4, KNET_SEND_REDUNDANT);
            input_seq++;
        }
        knet_conn_flush(&a, now, link_emit, &ab);
        knet_conn_flush(&b, now, link_emit, &ba);
        link_deliver(&ab, &b, &sink_b);
        link_deliver(&ba, &a, &sink_a);
        if (sink_b.reliable_frames == frames && now > 21.0 && !knet_conn_unacked(&a)) break;
    }
    CHECK(!a.failed && !b.failed, "%s: connection failed", label);
    CHECK(sink_b.reliable_frames == frames, "%s: %zu/%u reliable frames delivered", label, sink_b.reliable_frames, frames);
    CHECK(knet_conn_unacked(&a) == 0, "%s: %zu bytes still unacked", label, knet_conn_unacked(&a));
    CHECK(sink_b.stale == 0, "%s: %zu stale states delivered", label, sink_b.stale);
    CHECK(sink_b.states <= (size_t) state_tag, "%s: duplicated states delivered", label);
    CHECK(sink_b.input_dups == 0, "%s: %zu duplicate inputs delivered", label, sink_b.input_dups);
    const double state_ratio = (double) sink_b.states / state_tag;
    const double input_ratio = (double) sink_b.inputs / input_seq;
    CHECK(!drop || input_ratio > state_ratio || input_ratio > 0.99, "%s: redundancy did not help", label);
    const int rtt = knet_conn_rtt_ms(&a);
    CHECK(rtt >= 0 && rtt < (int) (latency * 2000.0) + 60, "%s: rtt %d ms", label, rtt);
    printf("%-26s done %.2fs  rtt %3d ms  resent %4llu  states %5.1f%%  inputs %5.1f%%  dgrams %llu\n", label, now, rtt,
           (unsigned long long) a.stats.segments_resent, 100.0 * state_ratio, 100.0 * input_ratio,
           (unsigned long long) a.stats.datagrams_sent);
    knet_conn_free(&a);
    knet_conn_free(&b);
}

static void test_timeout(void) {
    KnetConn conn;
    knet_conn_init(&conn, 7, 0.0);
    CHECK(!knet_conn_timed_out(&conn, KNET_TIMEOUT - 0.1), "early timeout");
    CHECK(knet_conn_timed_out(&conn, KNET_TIMEOUT + 0.1), "no timeout");
    uint8_t d[8];
    const size_t n = knet_build_disconnect(d, 7);
    int closed = 0;
    CHECK(knet_conn_receive(&conn, d, n, 1.0, NULL, NULL, &closed) && closed, "disconnect");
    d[4] ^= 1;
    CHECK(!knet_conn_receive(&conn, d, n, 1.0, NULL, NULL, &closed), "foreign token accepted");
    knet_conn_free(&conn);
    printf("timeout OK\n");
}

int main(void) {
    test_handshake();
    test_timeout();
    run_session(0, 0, 0, 0.0005, "clean (loopback)");
    run_session(0, 0, 0, 0.05, "50 ms");
    run_session(5, 0, 0, 0.03, "5% drop 30 ms");
    run_session(20, 0, 0, 0.03, "20% drop 30 ms");
    run_session(0, 20, 0, 0.03, "20% dup");
    run_session(0, 0, 30, 0.03, "30% reorder");
    run_session(10, 10, 10, 0.075, "10/10/10 75 ms");
    run_session(40, 5, 5, 0.02, "40% drop (harsh)");
    printf("Test OK\n");
    return 0;
}
