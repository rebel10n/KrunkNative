/*
 * knsvc/msgpack.h - MessagePack encode/decode, cJSON bridge, and Krunker's
 * socket framing (msgpack-lite payload followed by two 0x00 bytes; see
 * Krunker_v3.1.1/server/io.ts encodePacket/decodePacket).
 *
 * Packets are arrays: ["label", arg1, arg2, ...].
 *     cJSON *pkt = cJSON_CreateArray(); ... add label + args
 *     kn_buf out = {0}; kn_krunker_packet_encode(pkt, &out); kn_ws_send_binary(ws, out.data, out.len);
 *     cJSON *in = kn_krunker_packet_decode(data, len);   // NULL if malformed
 *
 * cJSON mapping (encode, same rule as msgpack-lite): integral numbers in the
 * int32 range -> smallest int format, other numbers -> float64, strings -> str,
 * true/false/null, arrays, objects -> maps. Decode: ints/floats -> number,
 * str -> string, bin -> string (bytes, NUL-truncated by cJSON), ext -> null,
 * non-string map keys -> stringified. Nesting depth limited to 64.
 */
#ifndef KNSVC_MSGPACK_H
#define KNSVC_MSGPACK_H

#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "knsvc/buf.h"

/* ---- writer (append to a kn_buf; each returns 0 ok, -1 out of memory) */
int kn_mp_write_nil(kn_buf *b);
int kn_mp_write_bool(kn_buf *b, int v);
int kn_mp_write_int(kn_buf *b, int64_t v);     /* smallest encoding */
int kn_mp_write_uint(kn_buf *b, uint64_t v);
int kn_mp_write_float(kn_buf *b, float v);
int kn_mp_write_double(kn_buf *b, double v);
int kn_mp_write_str(kn_buf *b, const char *s, size_t len);
int kn_mp_write_cstr(kn_buf *b, const char *s);
int kn_mp_write_bin(kn_buf *b, const void *p, size_t len);
int kn_mp_write_array(kn_buf *b, uint32_t count);  /* then count values */
int kn_mp_write_map(kn_buf *b, uint32_t count);    /* then count key/value pairs */

/* ---- reader (pull parser) */
typedef enum kn_mp_type {
    KN_MP_NIL, KN_MP_BOOL, KN_MP_INT, KN_MP_UINT, KN_MP_FLOAT, KN_MP_STR, KN_MP_BIN,
    KN_MP_ARRAY, KN_MP_MAP, KN_MP_EXT
} kn_mp_type;

typedef struct kn_mp_value {
    kn_mp_type type;
    union {
        int b;
        int64_t i;
        uint64_t u;
        double f;
        struct { const char *ptr; uint32_t len; } str; /* STR, BIN, EXT data; not NUL-terminated */
        uint32_t count;                                /* ARRAY elements / MAP pairs that follow */
    } v;
    int8_t ext_type;
} kn_mp_value;

typedef struct kn_mp_reader {
    const uint8_t *p;
    size_t len;
    size_t pos;
    int error;       /* sticky: set on truncation / invalid byte (0xc1) */
} kn_mp_reader;

void kn_mp_reader_init(kn_mp_reader *r, const void *data, size_t len);
int kn_mp_read(kn_mp_reader *r, kn_mp_value *out);   /* 0 ok, -1 error/end */
int kn_mp_skip(kn_mp_reader *r);                     /* skip one complete value (incl. children) */

/* ---- cJSON bridge */
int kn_msgpack_from_json(const cJSON *json, kn_buf *out);              /* 0 ok */
/* Decodes one value; *consumed (optional) = bytes used. NULL on error. */
cJSON *kn_msgpack_to_json(const void *data, size_t len, size_t *consumed);

/* ---- Krunker framing: msgpack(packet) + "\0\0" */
int kn_krunker_packet_encode(const cJSON *packet, kn_buf *out);
cJSON *kn_krunker_packet_decode(const void *data, size_t len);  /* requires the 2-byte trailer */

#endif
