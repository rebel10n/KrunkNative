#pragma once
// knet: KrunkNative's UDP game transport (doc/transport.md). Pure state machines, no
// sockets: the caller feeds received datagrams in and gets datagrams to send out, so the
// same code runs natively (server/src/net_udp.c, client/src/net/net.c), in the browser
// (WebTransport datagrams relayed 1:1 by net-bridge/wt) and in shared/tests/knet_test.c.
//
// Channels on one connection:
//   reliable   ordered byte stream (offset-addressed segments, resent on RTO). It carries
//              the game's framed packets unchanged (1 byte type, 2 bytes length, payload),
//              so any message size is fragmented across datagrams for free.
//   unreliable whole game frames, sequenced (an older one than the newest delivered is
//              dropped); KNET_SEND_REDUNDANT frames are repeated in the next two flushes.
// Every DATA datagram carries seq + ack + a 32-bit ack bitfield; acks drive reliable
// resends and the RTT estimate.
//
// Byte 0 of every knet datagram is a KNET_* type below, never 0xFF: datagrams starting
// with 0xFF 0xFF 0xFF 0xFF are server-info queries (shared/include/server_info.h).
#include <stddef.h>
#include <stdint.h>

#define KNET_VERSION 1
#define KNET_MTU 1000              // max datagram bytes: fits a WebTransport datagram (QUIC 1200-byte floor) with room to spare
#define KNET_CONNECT_REQUEST_SIZE 64 // padded, larger than a CHALLENGE (no amplification)
#define KNET_FORWARDED_MAX 47      // CONNECT_RESPONSE forwarded peer address (trusted from loopback only)
#define KNET_TIMEOUT 10.0          // seconds without a datagram before the connection is dead
#define KNET_KEEPALIVE 0.5         // seconds of silence before an empty DATA goes out
#define KNET_HANDSHAKE_RESEND 0.25 // client handshake retry interval
#define KNET_ACK_DELAY 0.01       // an ack-only datagram waits this long for data to ride on
#define KNET_COOKIE_BUCKET 10      // seconds per cookie key epoch (current + previous accepted)

enum {
    KNET_CONNECT_REQUEST = 0x01,  // C->S [type][ 'K' 'N' 'U' ][ver][salt4] zero padding to 64
    KNET_CHALLENGE = 0x02,        // S->C [type][salt4][cookie8]
    KNET_CONNECT_RESPONSE = 0x03, // C->S [type][salt4][cookie8][fwd_len1][fwd bytes]
    KNET_ACCEPT = 0x04,           // S->C [type][salt4][token4]
    KNET_DENY = 0x05,             // S->C [type][salt4][len1][reason]
    KNET_DATA = 0x06,             // both [type][token4][seq2][ack2][ack_bits4] messages...
    KNET_DISCONNECT = 0x07,       // both [type][token4]
};

#define KNET_DATA_HEADER 13
#define KNET_MSG_RELIABLE 0x10    // [0x10][offset4][len2][bytes]
#define KNET_MSG_UNRELIABLE 0x20  // [0x20][flags1][id2][len2][type1][payload], flags bit0 = sequenced
#define KNET_RELIABLE_HEADER 7
#define KNET_UNRELIABLE_HEADER 7
#define KNET_SEGMENT_MAX (KNET_MTU - KNET_DATA_HEADER - KNET_RELIABLE_HEADER)
#define KNET_UNRELIABLE_MAX (KNET_MTU - KNET_DATA_HEADER - KNET_UNRELIABLE_HEADER)

#define KNET_SENT_RING 256          // remembered sent datagrams (acks older than that are ignored)
#define KNET_SEGS_PER_DATAGRAM 16
#define KNET_MAX_INFLIGHT (192 * 1024) // unacked reliable bytes in flight
#define KNET_MAX_NEW_PER_FLUSH (48 * 1024)
#define KNET_MAX_PENDING (8u * 1024u * 1024u) // reliable bytes queued before the peer is declared stuck
#define KNET_MAX_OOO (1024u * 1024u)    // out-of-order reliable bytes buffered by the receiver
#define KNET_UNRELIABLE_QUEUE (64 * 1024) // queued unreliable bytes per flush (more are dropped)
#define KNET_REDUNDANT_HISTORY 2           // flushes a redundant frame is repeated in
#define KNET_REDUNDANT_HISTORY_MAX 2048    // bytes of redundant frames remembered per flush

// RELIABLE: ordered + resent. UNRELIABLE: sequenced per flush ("generation"): frames of a
// generation older than the newest one delivered are dropped (STATE snapshots).
// REDUNDANT: unordered, repeated in the next KNET_REDUNDANT_HISTORY flushes and
// de-duplicated by id (inputs, ping probes: a single loss costs nothing).
enum { KNET_SEND_RELIABLE = 0, KNET_SEND_UNRELIABLE = 1, KNET_SEND_REDUNDANT = 2 };

typedef void (*KnetEmitFn)(void *user, const uint8_t *datagram, size_t length);
// One complete game frame (type, payload length, payload) from either channel.
typedef void (*KnetDeliverFn)(void *user, uint8_t type, const uint8_t *payload, uint16_t length);

typedef struct {
    uint32_t offset;
    uint16_t length;
    uint8_t acked;
    uint8_t sends;
    uint16_t last_seq; // datagram seq of the last send (fast resend)
    double sent_at;
} KnetSegment;

typedef struct {
    uint16_t seq;
    uint8_t valid;
    uint8_t segment_count;
    double sent_at;
    uint8_t eliciting;
    uint32_t segments[KNET_SEGS_PER_DATAGRAM]; // segment offsets carried
} KnetSentRecord;

typedef struct {
    uint32_t offset;
    uint16_t length;
    uint8_t *data;
} KnetOooChunk;

typedef struct {
    uint64_t datagrams_sent, datagrams_received;
    uint64_t bytes_sent, bytes_received;
    uint64_t segments_resent;
    uint64_t duplicates_received;  // reliable bytes / unreliable frames seen twice
    uint64_t unreliable_dropped;   // stale (older than the newest delivered) frames
    uint64_t malformed;
} KnetStats;

typedef struct {
    uint32_t token;
    int failed;                // stream overflow / malformed reliable data: close the connection
    double last_received_at;
    double last_sent_at;
    double ack_pending_since;  // first ack-eliciting datagram received since our last send, 0 none

    // packet acks
    uint16_t next_seq;
    uint16_t remote_seq;       // newest DATA seq received
    uint32_t remote_bits;      // bit i = remote_seq - 1 - i received
    int remote_any;
    uint16_t highest_acked;    // newest of our seqs the peer acked
    int highest_acked_any;
    KnetSentRecord sent[KNET_SENT_RING];

    // RTT (seconds)
    double srtt, rttvar;
    int rtt_samples;

    // reliable send
    uint8_t *send_buf;         // bytes [send_base, send_end) start at send_buf[send_head]
    size_t send_head, send_cap;
    size_t inflight;           // bytes of sent, unacked segments
    uint32_t send_base;        // stream offset of send_buf[0]
    uint32_t send_end;         // stream offset after the last queued byte
    uint32_t segmented_end;    // bytes below this belong to a segment
    KnetSegment *segments;     // sorted by offset, unacked ones and acked ones not yet popped
    size_t segment_count, segment_cap;

    // reliable receive
    uint32_t recv_offset;      // next expected stream byte
    KnetOooChunk *ooo;
    size_t ooo_count, ooo_cap, ooo_bytes;
    uint8_t frame_buf[3 + 65535]; // game frame being reassembled from the stream
    uint32_t frame_have;

    // unreliable
    uint16_t send_generation;  // id of sequenced frames queued now (+1 per flush that sent any)
    int generation_used;
    uint16_t next_redundant_id;
    uint8_t *ubuf;             // encoded unreliable messages queued for the next flush
    size_t ubuf_length, ubuf_cap;
    uint8_t history[KNET_REDUNDANT_HISTORY][KNET_REDUNDANT_HISTORY_MAX]; // redundant messages of past flushes
    size_t history_length[KNET_REDUNDANT_HISTORY];
    double history_at[KNET_REDUNDANT_HISTORY];   // last time the slot went out
    int history_left[KNET_REDUNDANT_HISTORY];    // repeats still due
    size_t history_next;
    uint16_t newest_generation;
    int generation_any;
    uint16_t newest_redundant;
    uint64_t redundant_bits;   // bit i = newest_redundant - 1 - i delivered
    int redundant_any;

    KnetStats stats;
} KnetConn;

void knet_conn_init(KnetConn *conn, uint32_t token, double now);
void knet_conn_free(KnetConn *conn);

// Queue one game frame (header + payload). Returns 0 when it cannot be queued (too large
// for the unreliable channel, or the reliable backlog overflowed: conn->failed is set).
int knet_conn_send(KnetConn *conn, uint8_t type, const void *payload, uint16_t length, int mode);

// One received datagram (any KNET_* type; only DATA is consumed here, the rest return 0).
// Returns 1 for a DATA datagram of this connection, 0 otherwise. DISCONNECT sets *closed.
int knet_conn_receive(KnetConn *conn, const uint8_t *datagram, size_t length, double now,
                      KnetDeliverFn deliver, void *user, int *closed);

// Emit everything that is due: queued frames, resends, acks, keepalive.
void knet_conn_flush(KnetConn *conn, double now, KnetEmitFn emit, void *user);

int knet_conn_timed_out(const KnetConn *conn, double now);
int knet_conn_rtt_ms(const KnetConn *conn);           // -1 before the first sample
size_t knet_conn_unacked(const KnetConn *conn);       // reliable bytes not yet acked
size_t knet_build_disconnect(uint8_t *out, uint32_t token);

// ------------------------------------------------------------------ handshake
size_t knet_build_connect_request(uint8_t *out, uint32_t salt);
// Client: CHALLENGE -> CONNECT_RESPONSE (forwarded = NULL natively). 0 = not our challenge.
size_t knet_client_answer_challenge(const uint8_t *in, size_t length, uint32_t salt, uint8_t *out);
// Client: 1 = ACCEPT (token set), -1 = DENY (reason copied), 0 = something else.
int knet_client_read_accept(const uint8_t *in, size_t length, uint32_t salt, uint32_t *token, char *reason, size_t reason_size);

typedef struct {
    uint8_t key[16];
} KnetCookieKey;

typedef struct {
    uint32_t salt;
    char forwarded[KNET_FORWARDED_MAX + 1];
} KnetConnectInfo;

// Server, stateless: CONNECT_REQUEST -> CHALLENGE into out (returns its length).
// CONNECT_RESPONSE with a valid cookie -> returns 0 and *accepted = 1 with info filled.
// peer = IPv4 address (network order) + port (network order).
size_t knet_server_handshake(const KnetCookieKey *key, uint32_t peer_ip, uint16_t peer_port, double now,
                             const uint8_t *in, size_t length, uint8_t *out, int *accepted, KnetConnectInfo *info);
size_t knet_build_accept(uint8_t *out, uint32_t salt, uint32_t token);
size_t knet_build_deny(uint8_t *out, uint32_t salt, const char *reason);
uint32_t knet_datagram_token(const uint8_t *in, size_t length); // DATA / DISCONNECT token, 0 otherwise

uint64_t knet_siphash(const uint8_t key[16], const uint8_t *data, size_t length);

// ------------------------------------------------------------------ game glue
// Which knet channel a game packet type rides (shared/src/net_channel.c): STATE is
// UNRELIABLE, INPUT / INPUT_BATCH / PING / PONG are REDUNDANT, everything else RELIABLE.
int net_packet_channel(uint8_t type);

// Virtual descriptors: a ClientConnection.fd >= NET_VIRTUAL_FD_BASE is a knet peer, not a
// socket. net_send_packet() (shared/src/net.c) hands such fds to the registered sender.
#define NET_VIRTUAL_FD_BASE 0x10000000
typedef int (*NetVirtualSendFn)(int fd, uint8_t type, const void *payload, uint16_t length);
void net_set_virtual_send(NetVirtualSendFn fn);
NetVirtualSendFn net_virtual_send(void);

// ------------------------------------------------------------------ loss simulation
// KRUNKNATIVE_NET_DROP / _DUP / _REORDER = percent of inbound datagrams dropped,
// duplicated or held back behind the next one. Off (all 0) by default.
typedef struct {
    int drop, dup, reorder;
    uint64_t rng;
    uint8_t held[KNET_MTU + 64];
    size_t held_length;
} KnetSim;

void knet_sim_from_env(KnetSim *sim, uint64_t seed);
int knet_sim_active(const KnetSim *sim);
// Pass one inbound datagram through the simulation; deliver() gets 0..2 datagrams.
void knet_sim_filter(KnetSim *sim, const uint8_t *datagram, size_t length, KnetEmitFn deliver, void *user);
