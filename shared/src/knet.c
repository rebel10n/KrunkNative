// knet: UDP game transport state machines (shared/include/knet.h).
#include <knet.h>
#include <stdlib.h>
#include <string.h>

// ------------------------------------------------------------------ byte helpers
static void put16(uint8_t *p, const uint16_t v) { p[0] = (uint8_t) (v >> 8); p[1] = (uint8_t) v; }
static void put32(uint8_t *p, const uint32_t v) { p[0] = (uint8_t) (v >> 24); p[1] = (uint8_t) (v >> 16); p[2] = (uint8_t) (v >> 8); p[3] = (uint8_t) v; }
static uint16_t get16(const uint8_t *p) { return (uint16_t) ((p[0] << 8) | p[1]); }
static uint32_t get32(const uint8_t *p) { return ((uint32_t) p[0] << 24) | ((uint32_t) p[1] << 16) | ((uint32_t) p[2] << 8) | p[3]; }

// a - b for wrapping 16-bit sequence numbers
static int seq_diff(const uint16_t a, const uint16_t b) { return (int16_t) (uint16_t) (a - b); }
static int32_t off_diff(const uint32_t a, const uint32_t b) { return (int32_t) (a - b); }

// ------------------------------------------------------------------ SipHash-2-4
#define ROTL(x, b) (uint64_t) (((x) << (b)) | ((x) >> (64 - (b))))
#define SIPROUND do { v0 += v1; v1 = ROTL(v1, 13); v1 ^= v0; v0 = ROTL(v0, 32); v2 += v3; v3 = ROTL(v3, 16); v3 ^= v2; \
    v0 += v3; v3 = ROTL(v3, 21); v3 ^= v0; v2 += v1; v1 = ROTL(v1, 17); v1 ^= v2; v2 = ROTL(v2, 32); } while (0)

static uint64_t le64(const uint8_t *p) {
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

uint64_t knet_siphash(const uint8_t key[16], const uint8_t *data, const size_t length) {
    const uint64_t k0 = le64(key), k1 = le64(key + 8);
    uint64_t v0 = 0x736f6d6570736575ULL ^ k0, v1 = 0x646f72616e646f6dULL ^ k1;
    uint64_t v2 = 0x6c7967656e657261ULL ^ k0, v3 = 0x7465646279746573ULL ^ k1;
    const size_t blocks = length / 8;
    for (size_t i = 0; i < blocks; i++) {
        const uint64_t m = le64(data + i * 8);
        v3 ^= m;
        SIPROUND;
        SIPROUND;
        v0 ^= m;
    }
    uint64_t b = (uint64_t) length << 56;
    for (size_t i = 0; i < (length & 7); i++) b |= (uint64_t) data[blocks * 8 + i] << (8 * i);
    v3 ^= b;
    SIPROUND;
    SIPROUND;
    v0 ^= b;
    v2 ^= 0xff;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    SIPROUND;
    return v0 ^ v1 ^ v2 ^ v3;
}

// ------------------------------------------------------------------ handshake
size_t knet_build_connect_request(uint8_t *out, const uint32_t salt) {
    memset(out, 0, KNET_CONNECT_REQUEST_SIZE);
    out[0] = KNET_CONNECT_REQUEST;
    out[1] = 'K';
    out[2] = 'N';
    out[3] = 'U';
    out[4] = KNET_VERSION;
    put32(out + 5, salt);
    return KNET_CONNECT_REQUEST_SIZE;
}

static uint64_t knet_cookie(const KnetCookieKey *key, const uint32_t ip, const uint16_t port, const uint32_t salt, const uint32_t epoch) {
    uint8_t buf[14];
    memcpy(buf, &ip, 4);
    memcpy(buf + 4, &port, 2);
    put32(buf + 6, salt);
    put32(buf + 10, epoch);
    return knet_siphash(key->key, buf, sizeof(buf));
}

size_t knet_server_handshake(const KnetCookieKey *key, const uint32_t peer_ip, const uint16_t peer_port, const double now,
                             const uint8_t *in, const size_t length, uint8_t *out, int *accepted, KnetConnectInfo *info) {
    *accepted = 0;
    const uint32_t epoch = (uint32_t) (now / KNET_COOKIE_BUCKET);

    if (in[0] == KNET_CONNECT_REQUEST) {
        // Padded to 64 bytes: the CHALLENGE (13 bytes) is smaller than the request.
        if (length < KNET_CONNECT_REQUEST_SIZE || memcmp(in + 1, "KNU", 3) || in[4] != KNET_VERSION) return 0;
        const uint32_t salt = get32(in + 5);
        const uint64_t cookie = knet_cookie(key, peer_ip, peer_port, salt, epoch);
        out[0] = KNET_CHALLENGE;
        put32(out + 1, salt);
        for (int i = 0; i < 8; i++) out[5 + i] = (uint8_t) (cookie >> (8 * i));
        return 13;
    }

    if (in[0] == KNET_CONNECT_RESPONSE) {
        if (length < 14) return 0;
        const uint32_t salt = get32(in + 1);
        uint64_t cookie = 0;
        for (int i = 0; i < 8; i++) cookie |= (uint64_t) in[5 + i] << (8 * i);
        if (cookie != knet_cookie(key, peer_ip, peer_port, salt, epoch) &&
            cookie != knet_cookie(key, peer_ip, peer_port, salt, epoch - 1)) return 0;
        const size_t fwd = in[13];
        if (fwd > KNET_FORWARDED_MAX || length < 14 + fwd) return 0;
        info->salt = salt;
        memcpy(info->forwarded, in + 14, fwd);
        info->forwarded[fwd] = 0;
        *accepted = 1;
    }
    return 0;
}

size_t knet_client_answer_challenge(const uint8_t *in, const size_t length, const uint32_t salt, uint8_t *out) {
    if (length < 13 || in[0] != KNET_CHALLENGE || get32(in + 1) != salt) return 0;
    out[0] = KNET_CONNECT_RESPONSE;
    memcpy(out + 1, in + 1, 12);
    out[13] = 0; // forwarded address: only net-bridge/wt fills it
    return 14;
}

size_t knet_build_accept(uint8_t *out, const uint32_t salt, const uint32_t token) {
    out[0] = KNET_ACCEPT;
    put32(out + 1, salt);
    put32(out + 5, token);
    return 9;
}

size_t knet_build_deny(uint8_t *out, const uint32_t salt, const char *reason) {
    size_t n = reason ? strlen(reason) : 0;
    if (n > 200) n = 200;
    out[0] = KNET_DENY;
    put32(out + 1, salt);
    out[5] = (uint8_t) n;
    if (n) memcpy(out + 6, reason, n);
    return 6 + n;
}

int knet_client_read_accept(const uint8_t *in, const size_t length, const uint32_t salt, uint32_t *token, char *reason, const size_t reason_size) {
    if (length >= 9 && in[0] == KNET_ACCEPT && get32(in + 1) == salt) {
        *token = get32(in + 5);
        return *token ? 1 : 0;
    }
    if (length >= 6 && in[0] == KNET_DENY && get32(in + 1) == salt) {
        size_t n = in[5];
        if (n > length - 6) n = length - 6;
        if (reason && reason_size) {
            if (n >= reason_size) n = reason_size - 1;
            memcpy(reason, in + 6, n);
            reason[n] = 0;
        }
        return -1;
    }
    return 0;
}

uint32_t knet_datagram_token(const uint8_t *in, const size_t length) {
    if ((in[0] == KNET_DATA && length >= KNET_DATA_HEADER) || (in[0] == KNET_DISCONNECT && length >= 5)) return get32(in + 1);
    return 0;
}

size_t knet_build_disconnect(uint8_t *out, const uint32_t token) {
    out[0] = KNET_DISCONNECT;
    put32(out + 1, token);
    return 5;
}

size_t knet_build_disconnect_frame(uint8_t *out, const uint32_t token, const uint8_t type, const void *payload, uint16_t length) {
    if (length > KNET_MTU - 8) length = KNET_MTU - 8;
    knet_build_disconnect(out, token);
    out[5] = type;
    out[6] = (uint8_t) (length >> 8);
    out[7] = (uint8_t) length;
    if (length) memcpy(out + 8, payload, length);
    return 8 + (size_t) length;
}
// KNET_PART2

// ------------------------------------------------------------------ connection
#define KNET_RTO_MIN 0.05
#define KNET_RTO_MAX 2.0
#define KNET_RTO_INITIAL 0.25
#define KNET_FAST_RESEND_GAP 3   // a segment is lost once a datagram 3 seqs newer was acked
#define KNET_REDUNDANT_REPEAT 0.02 // a redundant frame repeats alone after 20 ms of silence

void knet_conn_init(KnetConn *conn, const uint32_t token, const double now) {
    memset(conn, 0, sizeof(*conn));
    conn->token = token;
    conn->last_received_at = now;
    conn->last_sent_at = now;
    conn->next_seq = 1;
}

void knet_conn_free(KnetConn *conn) {
    free(conn->send_buf);
    free(conn->segments);
    for (size_t i = 0; i < conn->ooo_count; i++) free(conn->ooo[i].data);
    free(conn->ooo);
    free(conn->ubuf);
    memset(conn, 0, sizeof(*conn));
}

static double knet_rto(const KnetConn *conn) {
    if (!conn->rtt_samples) return KNET_RTO_INITIAL;
    double rto = conn->raw_srtt + 4.0 * conn->raw_rttvar + KNET_ACK_DELAY;
    if (rto < KNET_RTO_MIN) rto = KNET_RTO_MIN;
    if (rto > KNET_RTO_MAX) rto = KNET_RTO_MAX;
    return rto;
}

int knet_conn_rtt_ms(const KnetConn *conn) {
    return conn->rtt_samples ? (int) (conn->srtt * 1000.0 + 0.5) : -1;
}

size_t knet_conn_unacked(const KnetConn *conn) {
    return (size_t) (conn->send_end - conn->send_base);
}

int knet_conn_timed_out(const KnetConn *conn, const double now) {
    return now - conn->last_received_at > KNET_TIMEOUT;
}

static int knet_reserve(uint8_t **buf, size_t *cap, const size_t need) {
    if (need <= *cap) return 1;
    size_t next = *cap ? *cap : 4096;
    while (next < need) next *= 2;
    uint8_t *grown = realloc(*buf, next);
    if (!grown) return 0;
    *buf = grown;
    *cap = next;
    return 1;
}

static int knet_stream_append(KnetConn *conn, const uint8_t *data, const size_t length) {
    const size_t used = (size_t) (conn->send_end - conn->send_base);
    if (used + length > KNET_MAX_PENDING) return 0;
    if (conn->send_head + used + length > conn->send_cap && conn->send_head) {
        memmove(conn->send_buf, conn->send_buf + conn->send_head, used);
        conn->send_head = 0;
    }
    if (!knet_reserve(&conn->send_buf, &conn->send_cap, conn->send_head + used + length)) return 0;
    memcpy(conn->send_buf + conn->send_head + used, data, length);
    conn->send_end += (uint32_t) length;
    return 1;
}

int knet_conn_send(KnetConn *conn, const uint8_t type, const void *payload, const uint16_t length, const int mode) {
    if (conn->failed) return 0;
    uint8_t header[KNET_UNRELIABLE_HEADER];

    if (mode == KNET_SEND_RELIABLE) {
        header[0] = type;
        put16(header + 1, length);
        if (!knet_stream_append(conn, header, 3) || (length && !knet_stream_append(conn, payload, length))) {
            conn->failed = 1; // a peer that stopped acking for megabytes
            return 0;
        }
        return 1;
    }

    if (length > KNET_UNRELIABLE_MAX) return 0;
    const size_t need = KNET_UNRELIABLE_HEADER + length;
    if (conn->ubuf_length + need > KNET_UNRELIABLE_QUEUE) return 0;
    if (!knet_reserve(&conn->ubuf, &conn->ubuf_cap, conn->ubuf_length + need)) return 0;

    header[0] = KNET_MSG_UNRELIABLE;
    if (mode == KNET_SEND_UNRELIABLE) {
        header[1] = 1;
        put16(header + 2, conn->send_generation);
        conn->generation_used = 1;
    } else {
        header[1] = 0;
        put16(header + 2, conn->next_redundant_id++);
    }
    put16(header + 4, length);
    header[6] = type;
    memcpy(conn->ubuf + conn->ubuf_length, header, KNET_UNRELIABLE_HEADER);
    if (length) memcpy(conn->ubuf + conn->ubuf_length + KNET_UNRELIABLE_HEADER, payload, length);
    conn->ubuf_length += need;
    return 1;
}

// ------------------------------------------------------------------ receive
static void knet_stream_deliver(KnetConn *conn, const uint8_t *data, size_t length, const KnetDeliverFn deliver, void *user) {
    while (length) {
        if (conn->frame_have < 3) {
            const size_t take = 3 - conn->frame_have < length ? 3 - conn->frame_have : length;
            memcpy(conn->frame_buf + conn->frame_have, data, take);
            conn->frame_have += (uint32_t) take;
            data += take;
            length -= take;
            if (conn->frame_have < 3) return;
        }
        const uint32_t frame_length = 3u + get16(conn->frame_buf + 1);
        const size_t take = frame_length - conn->frame_have < length ? frame_length - conn->frame_have : length;
        memcpy(conn->frame_buf + conn->frame_have, data, take);
        conn->frame_have += (uint32_t) take;
        data += take;
        length -= take;
        if (conn->frame_have == frame_length) {
            conn->frame_have = 0;
            if (deliver) deliver(user, conn->frame_buf[0], conn->frame_buf + 3, (uint16_t) (frame_length - 3));
        }
    }
}

static void knet_ooo_drain(KnetConn *conn, const KnetDeliverFn deliver, void *user) {
    int progressed = 1;
    while (progressed) {
        progressed = 0;
        for (size_t i = 0; i < conn->ooo_count; i++) {
            KnetOooChunk *chunk = &conn->ooo[i];
            if (off_diff(chunk->offset, conn->recv_offset) > 0) continue;
            const int32_t end = off_diff(chunk->offset + chunk->length, conn->recv_offset);
            if (end > 0) {
                const uint32_t skip = chunk->length - (uint32_t) end;
                conn->recv_offset += (uint32_t) end;
                knet_stream_deliver(conn, chunk->data + skip, (size_t) end, deliver, user);
            }
            conn->ooo_bytes -= chunk->length;
            free(chunk->data);
            conn->ooo[i] = conn->ooo[--conn->ooo_count];
            progressed = 1;
            break;
        }
    }
}

static void knet_receive_segment(KnetConn *conn, const uint32_t offset, const uint8_t *data, const uint16_t length,
                                 const KnetDeliverFn deliver, void *user) {
    const int32_t end = off_diff(offset + length, conn->recv_offset);
    if (end <= 0) {
        conn->stats.duplicates_received += length;
        return;
    }
    if (off_diff(offset, conn->recv_offset) <= 0) {
        const uint32_t skip = length - (uint32_t) end;
        conn->recv_offset += (uint32_t) end;
        knet_stream_deliver(conn, data + skip, (size_t) end, deliver, user);
        knet_ooo_drain(conn, deliver, user);
        return;
    }
    for (size_t i = 0; i < conn->ooo_count; i++) {
        if (conn->ooo[i].offset == offset && conn->ooo[i].length >= length) {
            conn->stats.duplicates_received += length;
            return;
        }
    }
    if (conn->ooo_bytes + length > KNET_MAX_OOO) return; // the sender resends it later
    if (conn->ooo_count == conn->ooo_cap) {
        const size_t cap = conn->ooo_cap ? conn->ooo_cap * 2 : 32;
        KnetOooChunk *grown = realloc(conn->ooo, cap * sizeof(*grown));
        if (!grown) return;
        conn->ooo = grown;
        conn->ooo_cap = cap;
    }
    uint8_t *copy = malloc(length);
    if (!copy) return;
    memcpy(copy, data, length);
    conn->ooo[conn->ooo_count++] = (KnetOooChunk) {offset, length, copy};
    conn->ooo_bytes += length;
}

// 1 = deliver, 0 = duplicate / stale
static int knet_accept_unreliable(KnetConn *conn, const int sequenced, const uint16_t id) {
    if (sequenced) {
        if (conn->generation_any && seq_diff(id, conn->newest_generation) < 0) {
            conn->stats.unreliable_dropped++;
            return 0;
        }
        conn->generation_any = 1;
        conn->newest_generation = id;
        return 1;
    }
    if (!conn->redundant_any) {
        conn->redundant_any = 1;
        conn->newest_redundant = id;
        conn->redundant_bits = 0;
        return 1;
    }
    const int d = seq_diff(id, conn->newest_redundant);
    if (d > 0) {
        conn->redundant_bits = d >= 64 ? 0 : (conn->redundant_bits << d) | (1ULL << (d - 1));
        conn->newest_redundant = id;
        return 1;
    }
    if (d == 0 || -d > 64 || (conn->redundant_bits & (1ULL << (-d - 1)))) {
        conn->stats.duplicates_received++;
        return 0;
    }
    conn->redundant_bits |= 1ULL << (-d - 1);
    return 1;
}

static size_t knet_find_segment(const KnetConn *conn, const uint32_t offset) {
    size_t lo = 0, hi = conn->segment_count;
    while (lo < hi) {
        const size_t mid = (lo + hi) / 2;
        if (off_diff(conn->segments[mid].offset, offset) < 0) lo = mid + 1;
        else hi = mid;
    }
    return lo < conn->segment_count && conn->segments[lo].offset == offset ? lo : SIZE_MAX;
}

static void knet_ack_record(KnetConn *conn, const uint16_t seq, const double now, const int sample_rtt, const double ack_delay) {
    KnetSentRecord *record = &conn->sent[seq % KNET_SENT_RING];
    if (!record->valid || record->seq != seq) return;
    record->valid = 0;
    if (sample_rtt && record->eliciting) {
        const double raw = now - record->sent_at;
        double rtt = raw;
        if (rtt - ack_delay >= 0.0005) rtt -= ack_delay; // like QUIC: never below a plausible floor
        if (raw >= 0.0) {
            if (!conn->rtt_samples) {
                conn->srtt = rtt;
                conn->rttvar = rtt / 2.0;
                conn->raw_srtt = raw;
                conn->raw_rttvar = raw / 2.0;
            } else {
                const double err = rtt > conn->srtt ? rtt - conn->srtt : conn->srtt - rtt;
                conn->rttvar = 0.75 * conn->rttvar + 0.25 * err;
                conn->srtt = 0.875 * conn->srtt + 0.125 * rtt;
                const double raw_err = raw > conn->raw_srtt ? raw - conn->raw_srtt : conn->raw_srtt - raw;
                conn->raw_rttvar = 0.75 * conn->raw_rttvar + 0.25 * raw_err;
                conn->raw_srtt = 0.875 * conn->raw_srtt + 0.125 * raw;
            }
            conn->rtt_samples++;
        }
    }
    for (int i = 0; i < record->segment_count; i++) {
        const size_t index = knet_find_segment(conn, record->segments[i]);
        if (index == SIZE_MAX || conn->segments[index].acked) continue;
        conn->segments[index].acked = 1;
        conn->inflight -= conn->segments[index].length;
    }
}

static void knet_pop_acked(KnetConn *conn) {
    size_t pop = 0;
    uint32_t bytes = 0;
    while (pop < conn->segment_count && conn->segments[pop].acked) bytes += conn->segments[pop++].length;
    if (!pop) return;
    memmove(conn->segments, conn->segments + pop, (conn->segment_count - pop) * sizeof(KnetSegment));
    conn->segment_count -= pop;
    conn->send_base += bytes;
    conn->send_head += bytes;
    if (conn->send_base == conn->send_end) conn->send_head = 0;
}

int knet_conn_receive(KnetConn *conn, const uint8_t *in, const size_t length, const double now,
                      const KnetDeliverFn deliver, void *user, int *closed) {
    if (closed) *closed = 0;
    if (length >= 5 && in[0] == KNET_DISCONNECT && get32(in + 1) == conn->token) {
        // a relay's goodbye can say why: one game frame after the token (knet.h KNET_DISCONNECT)
        if (length >= 8 && deliver) {
            const uint16_t n = (uint16_t) ((in[6] << 8) | in[7]);
            if ((size_t) n + 8 <= length) deliver(user, in[5], in + 8, n);
        }
        if (closed) *closed = 1;
        return 1;
    }
    if (length < KNET_DATA_HEADER || in[0] != KNET_DATA || get32(in + 1) != conn->token) return 0;

    conn->stats.datagrams_received++;
    conn->stats.bytes_received += length;
    conn->last_received_at = now;

    const uint16_t seq = get16(in + 5);
    const uint16_t ack = get16(in + 7);
    const uint32_t ack_bits = get32(in + 9);
    const double ack_delay = get16(in + 13) / 10000.0;

    // our acks of their datagrams
    // A datagram seen before (duplicated by the network) still carries acks, but its
    // messages were already delivered. Older than the 32-bit window: processed (rare,
    // and every channel tolerates it: offsets / generations / redundant ids).
    int duplicate = 0;
    if (!conn->remote_any) {
        conn->remote_any = 1;
        conn->remote_seq = seq;
        conn->remote_seq_at = now;
        conn->remote_bits = 0;
    } else {
        const int d = seq_diff(seq, conn->remote_seq);
        if (d > 0) {
            conn->remote_bits = d >= 32 ? 0 : (conn->remote_bits << d) | (1u << (d - 1));
            conn->remote_seq = seq;
            conn->remote_seq_at = now;
        } else if (d == 0) {
            duplicate = 1;
        } else if (-d <= 32) {
            duplicate = (conn->remote_bits >> (-d - 1)) & 1u;
            conn->remote_bits |= 1u << (-d - 1);
        }
    }

    // their acks of ours
    knet_ack_record(conn, ack, now, 1, ack_delay);
    for (int i = 0; i < 32; i++) {
        if (ack_bits & (1u << i)) knet_ack_record(conn, (uint16_t) (ack - 1 - i), now, 0, 0.0);
    }
    if (!conn->highest_acked_any || seq_diff(ack, conn->highest_acked) > 0) {
        conn->highest_acked = ack;
        conn->highest_acked_any = 1;
    }
    knet_pop_acked(conn);

    size_t at = KNET_DATA_HEADER;
    int eliciting = 0;
    if (duplicate) {
        conn->stats.duplicates_received++;
        if (length > KNET_DATA_HEADER && conn->ack_pending_since <= 0.0) conn->ack_pending_since = now;
        return 1;
    }
    while (at < length) {
        const uint8_t kind = in[at];
        if (kind == KNET_MSG_RELIABLE && at + KNET_RELIABLE_HEADER <= length) {
            const uint32_t offset = get32(in + at + 1);
            const uint16_t n = get16(in + at + 5);
            if (at + KNET_RELIABLE_HEADER + n > length) break;
            knet_receive_segment(conn, offset, in + at + KNET_RELIABLE_HEADER, n, deliver, user);
            at += KNET_RELIABLE_HEADER + n;
            eliciting = 1;
        } else if (kind == KNET_MSG_UNRELIABLE && at + KNET_UNRELIABLE_HEADER <= length) {
            const int sequenced = in[at + 1] & 1;
            const uint16_t id = get16(in + at + 2);
            const uint16_t n = get16(in + at + 4);
            if (at + KNET_UNRELIABLE_HEADER + n > length) break;
            if (knet_accept_unreliable(conn, sequenced, id) && deliver) deliver(user, in[at + 6], in + at + KNET_UNRELIABLE_HEADER, n);
            at += KNET_UNRELIABLE_HEADER + n;
            eliciting = 1;
        } else {
            break;
        }
    }
    if (at != length) conn->stats.malformed++;
    if (eliciting && conn->ack_pending_since <= 0.0) conn->ack_pending_since = now;
    return 1;
}

// ------------------------------------------------------------------ flush
typedef struct {
    KnetConn *conn;
    double now;
    KnetEmitFn emit;
    void *user;
    uint8_t data[KNET_MTU];
    size_t length;
    int eliciting;
    uint8_t segment_count;
    uint32_t segments[KNET_SEGS_PER_DATAGRAM];
} KnetBuilder;

static void knet_builder_begin(KnetBuilder *b) {
    KnetConn *conn = b->conn;
    b->data[0] = KNET_DATA;
    put32(b->data + 1, conn->token);
    put16(b->data + 5, conn->next_seq);
    put16(b->data + 7, conn->remote_any ? conn->remote_seq : (uint16_t) (conn->next_seq - 1000));
    put32(b->data + 9, conn->remote_any ? conn->remote_bits : 0);
    put16(b->data + 13, 0); // ack_delay: set by knet_builder_finish
    b->length = KNET_DATA_HEADER;
    b->eliciting = 0;
    b->segment_count = 0;
}

static void knet_builder_finish(KnetBuilder *b) {
    KnetConn *conn = b->conn;
    const uint16_t seq = conn->next_seq++;
    if (!conn->next_seq) conn->next_seq = 1;
    double delay = conn->remote_any ? (b->now - conn->remote_seq_at) * 10000.0 : 0.0;
    if (delay < 0.0) delay = 0.0;
    put16(b->data + 13, (uint16_t) (delay > 65535.0 ? 65535.0 : delay));
    KnetSentRecord *record = &conn->sent[seq % KNET_SENT_RING];
    record->valid = 1; // a never-acked record it overwrites leaves its segments to the RTO
    record->seq = seq;
    record->sent_at = b->now;
    record->eliciting = (uint8_t) b->eliciting;
    record->segment_count = b->segment_count;
    memcpy(record->segments, b->segments, b->segment_count * sizeof(uint32_t));
    for (int i = 0; i < b->segment_count; i++) {
        const size_t index = knet_find_segment(conn, b->segments[i]);
        if (index != SIZE_MAX) conn->segments[index].last_seq = seq;
    }
    b->emit(b->user, b->data, b->length);
    conn->stats.datagrams_sent++;
    conn->stats.bytes_sent += b->length;
    conn->last_sent_at = b->now;
    conn->ack_pending_since = 0.0;
    knet_builder_begin(b);
}

static int knet_builder_room(const KnetBuilder *b, const size_t need, const int segment) {
    if (b->length + need > KNET_MTU) return 0;
    return !segment || b->segment_count < KNET_SEGS_PER_DATAGRAM;
}

static void knet_builder_raw(KnetBuilder *b, const uint8_t *msg, const size_t length) {
    if (!knet_builder_room(b, length, 0)) knet_builder_finish(b);
    if (!knet_builder_room(b, length, 0)) return; // cannot happen: messages are <= KNET_MTU - header
    memcpy(b->data + b->length, msg, length);
    b->length += length;
    b->eliciting = 1;
}

static void knet_builder_segment(KnetBuilder *b, KnetSegment *segment) {
    KnetConn *conn = b->conn;
    const size_t need = KNET_RELIABLE_HEADER + segment->length;
    if (!knet_builder_room(b, need, 1)) knet_builder_finish(b);
    uint8_t *p = b->data + b->length;
    p[0] = KNET_MSG_RELIABLE;
    put32(p + 1, segment->offset);
    put16(p + 5, segment->length);
    memcpy(p + KNET_RELIABLE_HEADER, conn->send_buf + conn->send_head + (segment->offset - conn->send_base), segment->length);
    b->length += need;
    b->eliciting = 1;
    b->segments[b->segment_count++] = segment->offset;
    if (!segment->sends) conn->inflight += segment->length;
    else conn->stats.segments_resent++;
    segment->sent_at = b->now;
    segment->sends++;
}

// Walk the queued unreliable messages; redundant ones are also copied into history.
static void knet_flush_unreliable(KnetBuilder *b, uint8_t *history, size_t *history_length) {
    KnetConn *conn = b->conn;
    size_t at = 0;
    *history_length = 0;
    while (at + KNET_UNRELIABLE_HEADER <= conn->ubuf_length) {
        const size_t n = KNET_UNRELIABLE_HEADER + get16(conn->ubuf + at + 4);
        knet_builder_raw(b, conn->ubuf + at, n);
        if (!(conn->ubuf[at + 1] & 1) && *history_length + n <= KNET_REDUNDANT_HISTORY_MAX) {
            memcpy(history + *history_length, conn->ubuf + at, n);
            *history_length += n;
        }
        at += n;
    }
    conn->ubuf_length = 0;
    if (conn->generation_used) {
        conn->send_generation++;
        conn->generation_used = 0;
    }
}

void knet_conn_flush(KnetConn *conn, const double now, const KnetEmitFn emit, void *user) {
    KnetBuilder b;
    KnetBuilder *builder = &b;
    builder->conn = conn;
    builder->now = now;
    builder->emit = emit;
    builder->user = user;
    knet_builder_begin(builder);
    const size_t datagrams_before = (size_t) conn->stats.datagrams_sent;

    // 1. resends: RTO expiry, or three newer datagrams acked meanwhile (fast resend)
    const double rto = knet_rto(conn);
    for (size_t i = 0; i < conn->segment_count; i++) {
        KnetSegment *segment = &conn->segments[i];
        if (segment->acked || !segment->sends) continue;
        const int fast = conn->highest_acked_any && seq_diff(conn->highest_acked, segment->last_seq) >= KNET_FAST_RESEND_GAP &&
                         now - segment->sent_at >= (conn->rtt_samples ? conn->raw_srtt : 0.0);
        const double backoff = rto * (double) (1u << (segment->sends > 4 ? 4 : segment->sends - 1));
        if (fast || now - segment->sent_at >= (backoff > KNET_RTO_MAX ? KNET_RTO_MAX : backoff)) knet_builder_segment(builder, segment);
    }

    // 2. queued unreliable frames, then repeats of the last flushes' redundant ones
    uint8_t fresh_history[KNET_REDUNDANT_HISTORY_MAX];
    size_t fresh_length = 0;
    const int have_unreliable = conn->ubuf_length > 0;
    knet_flush_unreliable(builder, fresh_history, &fresh_length);
    for (int i = 0; i < KNET_REDUNDANT_HISTORY; i++) {
        if (conn->history_left[i] <= 0) continue;
        if (!have_unreliable && now - conn->history_at[i] < KNET_REDUNDANT_REPEAT) continue;
        size_t at = 0;
        while (at + KNET_UNRELIABLE_HEADER <= conn->history_length[i]) {
            const size_t n = KNET_UNRELIABLE_HEADER + get16(conn->history[i] + at + 4);
            knet_builder_raw(builder, conn->history[i] + at, n);
            at += n;
        }
        conn->history_left[i]--;
        conn->history_at[i] = now;
    }
    if (fresh_length) {
        const size_t slot = conn->history_next++ % KNET_REDUNDANT_HISTORY;
        memcpy(conn->history[slot], fresh_history, fresh_length);
        conn->history_length[slot] = fresh_length;
        conn->history_left[slot] = KNET_REDUNDANT_HISTORY;
        conn->history_at[slot] = now;
    }

    // 3. new reliable bytes, as far as the window allows
    size_t fresh = 0;
    while (conn->segmented_end != conn->send_end && conn->inflight < KNET_MAX_INFLIGHT && fresh < KNET_MAX_NEW_PER_FLUSH) {
        uint32_t n = conn->send_end - conn->segmented_end;
        if (n > KNET_SEGMENT_MAX) n = KNET_SEGMENT_MAX;
        if (conn->segment_count == conn->segment_cap) {
            const size_t cap = conn->segment_cap ? conn->segment_cap * 2 : 64;
            KnetSegment *grown = realloc(conn->segments, cap * sizeof(*grown));
            if (!grown) break;
            conn->segments = grown;
            conn->segment_cap = cap;
        }
        KnetSegment *segment = &conn->segments[conn->segment_count++];
        *segment = (KnetSegment) {.offset = conn->segmented_end, .length = (uint16_t) n};
        conn->segmented_end += n;
        knet_builder_segment(builder, segment);
        fresh += n;
    }

    if (builder->length > KNET_DATA_HEADER) {
        knet_builder_finish(builder);
        return;
    }
    // 4. nothing to carry: an ack-only datagram when one is due, or a keepalive
    const int ack_due = conn->ack_pending_since > 0.0 && now - conn->ack_pending_since >= KNET_ACK_DELAY;
    if ((size_t) conn->stats.datagrams_sent == datagrams_before && (ack_due || now - conn->last_sent_at >= KNET_KEEPALIVE)) {
        builder->eliciting = 0;
        knet_builder_finish(builder);
    }
}

static NetVirtualSendFn g_net_virtual_send;
void net_set_virtual_send(const NetVirtualSendFn fn) { g_net_virtual_send = fn; }
NetVirtualSendFn net_virtual_send(void) { return g_net_virtual_send; }
static NetVirtualSendFn g_net_loopback_send;
void net_set_loopback_send(const NetVirtualSendFn fn) { g_net_loopback_send = fn; }
NetVirtualSendFn net_loopback_send(void) { return g_net_loopback_send; }

// ------------------------------------------------------------------ loss simulation
static int knet_env_percent(const char *name) {
    const char *value = getenv(name);
    if (!value || !value[0]) return 0;
    const int percent = atoi(value);
    return percent < 0 ? 0 : percent > 100 ? 100 : percent;
}

static uint32_t knet_sim_next(KnetSim *sim) {
    sim->rng ^= sim->rng << 13;
    sim->rng ^= sim->rng >> 7;
    sim->rng ^= sim->rng << 17;
    return (uint32_t) (sim->rng >> 16) % 100u;
}

void knet_sim_from_env(KnetSim *sim, const uint64_t seed) {
    memset(sim, 0, sizeof(*sim));
    sim->drop = knet_env_percent("KRUNKNATIVE_NET_DROP");
    sim->dup = knet_env_percent("KRUNKNATIVE_NET_DUP");
    sim->reorder = knet_env_percent("KRUNKNATIVE_NET_REORDER");
    sim->rng = seed ? seed : 0x9e3779b97f4a7c15ULL;
}

int knet_sim_active(const KnetSim *sim) {
    return sim->drop || sim->dup || sim->reorder;
}

void knet_sim_filter(KnetSim *sim, const uint8_t *datagram, const size_t length, const KnetEmitFn deliver, void *user) {
    if (!knet_sim_active(sim)) {
        deliver(user, datagram, length);
        return;
    }
    if (sim->drop && knet_sim_next(sim) < (uint32_t) sim->drop) return;
    if (sim->reorder && !sim->held_length && length <= sizeof(sim->held) && knet_sim_next(sim) < (uint32_t) sim->reorder) {
        memcpy(sim->held, datagram, length); // goes out after the next one
        sim->held_length = length;
        return;
    }
    deliver(user, datagram, length);
    if (sim->dup && knet_sim_next(sim) < (uint32_t) sim->dup) deliver(user, datagram, length);
    if (sim->held_length) {
        const size_t held = sim->held_length;
        sim->held_length = 0;
        deliver(user, sim->held, held);
    }
}
