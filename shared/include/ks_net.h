// KrunkScript network messages (doc/v7/krunkscript.md 2.5, 2.6, 2.8): the wire helpers of
// NET_PACKET_SCRIPT_MSG / SCRIPT_LOG / SCRIPT_SYNC (shared/include/shared.h, KrunkScript
// block 200-239). Both the server host (server/src/ks_server_host.c) and the client host
// (client/src/ks) use them. Shared code: no client / server headers.
//
// SCRIPT_MSG ("sd", both directions): KsNetChunkHeader + body bytes per packet; the body of
// one message is `uint8 id_len, id bytes, ks_serialize(data)`, at most KS_NET_MAX_BODY bytes.
// SCRIPT_LOG ("ksL", S->C): uint8 is_error + UTF-8 text (no NUL), <= KS_NET_LOG_TEXT_MAX.
// SCRIPT_SYNC (custom sync values, S->C to that player): ks_serialize([name, value, ...]).
#ifndef KS_NET_H
#define KS_NET_H

#include <shared.h>
#include <ks.h>
#include <krunker_proto.h> // KrunkerValue (the official link's msgpack values; types only)

#ifdef __cplusplus
extern "C" {
#endif

#define KS_NET_ID_MAX 64              // id bytes (UTF-8) of a message [KN]
#define KS_NET_JSON_MAX 2000          // JSON.stringify(data).length > 2000 -> rejected (API:1350-1392)
// Binary bodies are larger than the JSON for number-heavy data (9 bytes per number against
// one JSON digit), so the reassembled limit covers any message that passes the 2000-char rule.
#define KS_NET_MAX_BODY 16384
#define KS_NET_LOG_TEXT_MAX 480       // SCRIPT_LOG text bytes (longer lines end with "...")

#pragma pack(push, 1)
typedef struct {
    uint16_t msg_seq;                 // per-sender message counter
    uint8_t chunk;                    // 0..chunk_count-1, sent in order
    uint8_t chunk_count;              // >= 1
} KsNetChunkHeader;
#pragma pack(pop)
#define KS_NET_CHUNK_DATA (NET_MAX_PAYLOAD - (int) sizeof(KsNetChunkHeader))

// Body of one message: returns its length, or -1 when the message is rejected (id longer
// than KS_NET_ID_MAX bytes, prefixed JSON longer than KS_NET_JSON_MAX or not serialisable
// (cycles), body over cap).
int ks_net_encode_msg(KsScript*, const char *id, KsValue data, uint8_t *body, size_t cap);
// 1 with id (NUL-terminated) and data decoded into the script's heap; 0 on a bad body.
int ks_net_decode_msg(KsScript*, const uint8_t *body, size_t len, char *id, size_t id_cap, KsValue *data);

int ks_net_chunk_count(size_t body_len);
// Writes chunk `i` of `body` (header + data) into out (NET_MAX_PAYLOAD bytes); returns the
// payload length.
uint16_t ks_net_make_chunk(const uint8_t *body, size_t len, uint16_t seq, int i, uint8_t *out);

// In-order reassembly of one sender's messages (a new msg_seq or chunk 0 restarts).
typedef struct KsNetReassembler {
    uint16_t seq;
    uint8_t next, count;
    uint8_t *buf;
    size_t len, cap;
} KsNetReassembler;
// 1 when a message is complete (*body valid until the next call or free), 0 while
// incomplete, -1 on a bad chunk (the partial message is dropped).
int ks_net_reassemble(KsNetReassembler*, const void *payload, uint16_t len, const uint8_t **body, size_t *body_len);
void ks_net_reassembler_free(KsNetReassembler*);

// Sends one message through `send` (one call per chunk, type NET_PACKET_SCRIPT_MSG). Returns
// 1 when sent, 0 when rejected (encode rules above) or a send failed. *seq advances.
typedef int (*KsNetSendFn)(void *user, int type, const void *payload, uint16_t len);
int ks_net_send_msg(KsScript*, uint16_t *seq, const char *id, KsValue data, KsNetSendFn send, void *user);

// The official krunker.io "sd" packets (shared/src/ks/ks_net_official.c; deob-10.0.3.js GAME.NETWORK :209434-209469,
// onNetworkMessage :241493): [id, data] in msgpack, `data` the script's JS value whose KrunkScript object keys are
// compiled as "V_" + name. ks_net_body_from_official turns an S2C "sd" into our SCRIPT_MSG body (keys lose "V_",
// a number id / key becomes its String(); nil, Uint8Array and Date values are null): its length, -1 when the id is
// longer than KS_NET_ID_MAX or the body over cap. ks_net_body_to_official decodes a SCRIPT_MSG body for the C2S
// "sd" (keys get "V_", undefined / null are nil) into a value tree owned by `out` (ks_net_official_free); 0 on a
// bad body.
typedef struct {
    KrunkerValue data;
    void **blocks;       // the tree's allocations
    size_t block_count, block_cap;
} KsNetOfficial;
int ks_net_body_from_official(const KrunkerValue *id, const KrunkerValue *data, uint8_t *body, size_t cap);
int ks_net_body_to_official(const uint8_t *body, size_t len, char *id, size_t id_cap, KsNetOfficial *out);
void ks_net_official_free(KsNetOfficial*);

// SCRIPT_LOG payload of one console line (truncated to KS_NET_LOG_TEXT_MAX with "..."):
// returns the payload length (out >= 1 + KS_NET_LOG_TEXT_MAX bytes).
uint16_t ks_net_encode_log(int is_error, const char *text, uint8_t *out);
// 1 with *is_error and text (NUL-terminated, truncated to cap) on a valid payload.
int ks_net_decode_log(const void *payload, uint16_t len, int *is_error, char *text, size_t cap);

// SCRIPT_SYNC on the client: writes each [name, value] into `self` (the local player's
// wrapper) only when that property already exists there with a non-object value
// (IDX:14769-14775). Returns the number of values written, -1 on a bad payload.
int ks_net_apply_sync(KsScript*, KsValue self, const void *payload, uint16_t len);

#ifdef __cplusplus
}
#endif

#endif
