#pragma once
// Official Krunker socket codec (doc: ~/.cache/kn-official/OFFICIAL-BRIEF.md "The protocol").
// Pure C11 (no GL, no sockets, no allocator other than malloc): the transport
// (client/src/net/krunker_net.c) moves the bytes, this file only frames them.
//
// Wire format of every WebSocket binary frame, from game.deob.js (the live build):
//
//   send(name, ...args):                          socket.send, game.deob.js:98207
//     if (name != C2S['cptR']) ahNum = (ahNum + AHKEY) & 0xff;
//     frame = msgpack.encode([name].concat(args)) ++ [ahNum >> 4 & 0xf, ahNum & 0xf]
//   onmessage(data):                              game.deob.js:24201
//     echo = (data[len-2] << 4) + data[len-1];    // not checked by the client
//     v = msgpack.decode(data[0 .. len-2]);       // exactly one value, extra bytes throw
//     if (!Array.isArray(v) || v.length < 1) throw 'Invalid message';
//     -> [echo, v[0], v.slice(1)]
//
// The msgpack dialect is @msgpack/msgpack's default Encoder/Decoder (game.deob.js:22484 /
// 22991), whose number rule differs from msgpack-lite (knsvc/msgpack.h): every safe integer
// (|n| <= 2^53-1) takes the smallest int format (uint64 / int64 included), every other
// number is float64; strings fixstr / str8 / str16 / str32; arrays and maps as usual.
//
// AHKEY, the exempt packet and the packet name tables are build specific: they come from the
// current KrunkerBuild (shared/include/krunker_build_live.h), the generated krunker_build.h's
// values or a published build.json's, so a newer Krunker build works without a rebuild.
// KrunkerSession takes the AHKEY at init (krunker_build()->ahkey).
//
// Values: a small tagged tree (KrunkerValue) rather than cJSON:
//   - O(1) indexing into the contiguous items of an array. Sync packets ('l', 'k', '0') are
//     long flat arrays of numbers that the transport walks by index; cJSON_GetArrayItem is a
//     linked-list walk (O(n^2) over a packet).
//   - one malloc per decoded frame (an exact-size block, freed by krunker_frame_free), no
//     per-node allocation; outgoing packets are built on the stack with compound literals.
//   - JS-exact: a number is a double (as in JS; the encoder picks int/float from the value,
//     like @msgpack/msgpack), strings carry their length (may contain NUL), bin and ext data
//     survive (cJSON truncates bin at NUL and drops ext).
// krunker_value_to_json() prints a value as JSON for logs.
#include <stddef.h>
#include <stdint.h>
#include <string.h>   // strlen (KRUNKER_V_STR)

#include "krunker_build.h"

// ---------------------------------------------------------------- packet name tables
// krunker_build.h defines the ids: KrunkerC2S (KRUNKER_C2S_ENTER_GAME, ..., KRUNKER_C2S_COUNT)
// and KrunkerS2C, one per X(ident, "wire", "Name") entry of KRUNKER_C2S_PACKETS /
// KRUNKER_S2C_PACKETS, identified by the bundle key ("ENTER_GAME"). The wire names are the current
// build's (krunker_build()): store wire names only for the build that sent them.

// Wire name ("en") of the current build / bundle key ("ENTER_GAME") of a packet id; NULL out of
// range or when the current build has no such packet.
const char *krunker_c2s_wire(KrunkerC2S id);
const char *krunker_c2s_key(KrunkerC2S id);
const char *krunker_s2c_wire(KrunkerS2C id);
const char *krunker_s2c_key(KrunkerS2C id);
// Packet id of a wire name in the current build (len bytes, need not be NUL-terminated); -1 when it has none.
int krunker_c2s_lookup(const char *wire, size_t len);
int krunker_s2c_lookup(const char *wire, size_t len);

// ---------------------------------------------------------------- values

typedef enum KrunkerValueType {
    KRUNKER_NIL,    // nil (JS null / undefined)
    KRUNKER_BOOL,
    KRUNKER_NUM,    // every msgpack int / float: a JS number
    KRUNKER_STR,    // UTF-8 bytes as received (not validated)
    KRUNKER_BIN,    // JS Uint8Array
    KRUNKER_ARRAY,
    KRUNKER_MAP,    // keys are KRUNKER_STR or KRUNKER_NUM (the decoder refuses others, as JS)
    KRUNKER_EXT,    // ext_type + raw bytes (-1 = msgpack timestamp, a JS Date)
} KrunkerValueType;

typedef struct KrunkerValue {
    uint8_t type;      // KrunkerValueType
    int8_t ext_type;   // KRUNKER_EXT only
    uint32_t len;      // STR / BIN / EXT: bytes; ARRAY: items; MAP: key/value pairs
    union {
        int boolean;
        double num;
        const char *str;                    // STR / BIN / EXT bytes; decoded ones are NUL-terminated
        const struct KrunkerValue *items;   // ARRAY: len values; MAP: 2 * len (key, value, ...)
    } u;
} KrunkerValue;

// Builders for outgoing packets (compound literals: no allocation, the referenced storage
// must outlive the encode call).
#define KRUNKER_V_NIL ((KrunkerValue){.type = KRUNKER_NIL})
#define KRUNKER_V_BOOL(b) ((KrunkerValue){.type = KRUNKER_BOOL, .u.boolean = (b) ? 1 : 0})
#define KRUNKER_V_NUM(x) ((KrunkerValue){.type = KRUNKER_NUM, .u.num = (double)(x)})
#define KRUNKER_V_STRN(s, n) ((KrunkerValue){.type = KRUNKER_STR, .len = (uint32_t)(n), .u.str = (s)})
#define KRUNKER_V_STR(s) KRUNKER_V_STRN((s), strlen(s))
#define KRUNKER_V_BIN(p, n) ((KrunkerValue){.type = KRUNKER_BIN, .len = (uint32_t)(n), .u.str = (const char *)(p)})
#define KRUNKER_V_ARRAY(items_, n) ((KrunkerValue){.type = KRUNKER_ARRAY, .len = (uint32_t)(n), .u.items = (items_)})
#define KRUNKER_V_MAP(kv, pairs) ((KrunkerValue){.type = KRUNKER_MAP, .len = (uint32_t)(pairs), .u.items = (kv)})

// Bounds-checked readers: a missing / mistyped value gives NULL or the default.
const KrunkerValue *krunker_at(const KrunkerValue *array, uint32_t i);   // ARRAY item i
const KrunkerValue *krunker_get(const KrunkerValue *map, const char *key); // MAP value of a string key
double krunker_num(const KrunkerValue *v, double def);    // NUM; BOOL as 0 / 1
const char *krunker_str(const KrunkerValue *v);           // STR bytes (NUL-terminated when decoded) or NULL
int krunker_is(const KrunkerValue *v, KrunkerValueType t);

// ---------------------------------------------------------------- byte buffer

typedef struct KrunkerBuf {
    uint8_t *data;
    size_t len;
    size_t cap;
} KrunkerBuf;

void krunker_buf_free(KrunkerBuf *b);

// ---------------------------------------------------------------- errors

typedef enum KrunkerProtoError {
    KRUNKER_PROTO_OK = 0,
    KRUNKER_PROTO_EINVAL = -1,     // bad argument (NULL, value type not encodable, string > 4 GiB)
    KRUNKER_PROTO_ENOMEM = -2,
    KRUNKER_PROTO_ESHORT = -3,     // frame shorter than 1 msgpack byte + the trailer
    KRUNKER_PROTO_EMSGPACK = -4,   // truncated or invalid msgpack (0xc1, counts past the end)
    KRUNKER_PROTO_EEXTRA = -5,     // bytes left after the value (JS: "Extra N of M byte(s)")
    KRUNKER_PROTO_ENOTARRAY = -6,  // payload is not a non-empty array (JS: 'Invalid message')
    KRUNKER_PROTO_ENAME = -7,      // packet name (item 0) is not a string
    KRUNKER_PROTO_EDEPTH = -8,     // nesting deeper than KRUNKER_PROTO_MAX_DEPTH
    KRUNKER_PROTO_EKEY = -9,       // map key neither string nor number (JS throws too)
    KRUNKER_PROTO_ETRAILER = -10,  // KRUNKER_DECODE_STRICT_TRAILER: a trailer byte > 0xf
} KrunkerProtoError;

const char *krunker_proto_strerror(int err);

// @msgpack/msgpack's Encoder maxDepth (100, the outer array is depth 1). The decoder has no
// limit in JS; ours uses the same one so hostile frames cannot exhaust the C stack.
#define KRUNKER_PROTO_MAX_DEPTH 100

// ---------------------------------------------------------------- session (ahNum)

typedef struct KrunkerSession {
    int64_t ahkey;    // the build's AHKEY (krunker_build()->ahkey when the session starts)
    uint8_t ah_num;   // trailer value of the last frame sent; 0 after init / reset
} KrunkerSession;

void krunker_session_init(KrunkerSession *s, int64_t ahkey);
// JS socket.reset() (a new connection): ahNum = 0.
void krunker_session_reset(KrunkerSession *s);
// 1 when sending `name` does not rotate ahNum (the current build's exempt packet, 'cptR').
int krunker_proto_ahnum_exempt(const char *name, size_t name_len);
// The rotation socket.send applies right before each send: ahNum = (ahNum + AHKEY) & 0xff,
// skipped for the exempt packet. Returns the trailer value of the frame being sent. Call it
// once per frame actually written to the socket (frames queued before the socket opens rotate
// when they are flushed, as the JS sendQueue does). krunker_proto_encode calls it for you.
uint8_t krunker_proto_next_ahnum(KrunkerSession *s, const char *name);

// ---------------------------------------------------------------- frames

// Encode one outgoing frame: msgpack([name, ...args]) + the 2-byte trailer of the session's
// next ahNum, replacing out's contents. The session rotates only when the encode succeeds
// (JS rotates before encoding; its encoder cannot fail on real packets).
// Number args follow @msgpack/msgpack (see above); map keys must be STR (or integral NUM,
// written as its decimal string like JS Object keys).
int krunker_proto_encode(KrunkerSession *s, const char *name, const KrunkerValue *args, size_t argc,
                         KrunkerBuf *out);
// Same with an explicit trailer value and no session (tests, replay tools).
int krunker_proto_encode_ah(uint8_t ah_num, const char *name, const KrunkerValue *args, size_t argc,
                            KrunkerBuf *out);

typedef struct KrunkerFrame {
    int ah_echo;               // (trailer[0] << 4) + trailer[1], as the JS decoder (unchecked)
    const char *name;          // packet wire name, NUL-terminated
    uint32_t name_len;
    const KrunkerValue *args;  // argc values (v.slice(1))
    uint32_t argc;
    void *mem_;                // owned block (krunker_frame_free)
} KrunkerFrame;

// Decode flags.
#define KRUNKER_DECODE_STRICT_TRAILER 1u   // also refuse trailer bytes > 0xf (the JS client accepts any)

// Decode one incoming frame into *out (zeroed on failure). Returns KRUNKER_PROTO_OK or a
// negative KrunkerProtoError; never reads outside data[0..len). The frame owns its values:
// release them with krunker_frame_free (safe on a zeroed / failed frame).
int krunker_proto_decode(const void *data, size_t len, unsigned flags, KrunkerFrame *out);
void krunker_frame_free(KrunkerFrame *f);

// Item i of the frame's args (NULL past the end).
const KrunkerValue *krunker_frame_arg(const KrunkerFrame *f, uint32_t i);

// Append v as JSON text (logs / debugging: bin as {"bin":"<hex>"}, ext as {"ext":t,"hex":...},
// non-finite numbers as null, number keys as strings). 0 or KRUNKER_PROTO_ENOMEM / EDEPTH.
int krunker_value_to_json(const KrunkerValue *v, KrunkerBuf *out);
