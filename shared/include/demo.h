#pragma once
// K.P.D demos (doc/v10/replays.md): server_bin's recording of what a spectator receives, the client's
// replay viewer reads it. Krunker v10.0.7's demo is a msgpack ["demo", {v, meta, frames}] of the socket
// messages a spectator got, with `_kf` keyframes {init, roster} (deob-10.0.7.js:201618-201646); Box
// Office's is the same idea over its own framed packets:
//
//   "KNDEMO\0" | u8 version | u32 meta length | meta JSON
//   records:  u32 t_ms | u8 kind (DEMO_REC_*) | u32 length | bytes
//
// little-endian; a record's bytes are framed packets exactly as the TCP protocol carries them (u8 type,
// u16 big-endian length, payload). A keyframe record holds the join burst a fresh spectator would get at
// that moment (MAP_INFO, INIT, SPAWNs, infos, round state), a stream record what the recorder received
// at t. Records are in time order and the first one is a keyframe.
//
// meta: {v, created (epoch ms), map, mode, typ, server, duration (ms), calls: [{token, suspect}],
// suspects: [uid]}. No GL, no I/O: shared by server_bin, kn_brain's tests and the client.
#include <stddef.h>
#include <stdint.h>

#define DEMO_MAGIC "KNDEMO" // 7 bytes with its NUL
#define DEMO_MAGIC_SIZE 7
#define DEMO_VERSION 1
#define DEMO_HEADER_SIZE (DEMO_MAGIC_SIZE + 1 + 4)
#define DEMO_RECORD_HEADER_SIZE 9
#define DEMO_META_MAX (64u * 1024u)
// The largest demo a server uploads and a client opens: under the client's 4 MiB HTTP response cap
// (backend_native.c HTTP_MAX_BODY). Box Office choice; Krunker's limit is server-side.
#define DEMO_MAX_BYTES (3584u * 1024u)
// computeSuspectSegments merges spans of the suspect closer than 10 s (deob-10.0.7.js:201733)
#define DEMO_SEGMENT_MERGE_MS 10000u
#define DEMO_SEGMENTS_MAX 64

typedef enum {
    DEMO_REC_STREAM = 0,
    DEMO_REC_KEYFRAME = 1,
} DemoRecordKind;

typedef struct {
    uint8_t *data;
    size_t length, capacity;
} DemoBuf;

int demo_buf_append(DemoBuf *buf, const void *data, size_t length); // 1 ok, 0 out of memory
void demo_buf_free(DemoBuf *buf);

// Writer: the header (meta: a JSON object's text), then records in time order. 1 ok, 0 failed.
int demo_write_header(DemoBuf *out, const char *meta_json);
int demo_write_record(DemoBuf *out, uint32_t t_ms, uint8_t kind, const uint8_t *bytes, uint32_t length);

typedef struct {
    uint32_t t_ms;
    uint8_t kind;
    uint32_t length;
    uint8_t *bytes; // into Demo.data
} DemoRecord;

typedef struct {
    uint8_t *data;   // the file (owned copy, writable: demo_anonymize edits it in place)
    size_t length;
    uint8_t version;
    char *meta;      // NUL-terminated meta JSON (owned)
    DemoRecord *records;
    size_t count;
    uint32_t duration_ms; // the last record's t
} Demo;

// Parses a demo file (copied). 1 ok; 0 with err ("Invalid Demo File" cases: magic, version, truncation,
// records out of order, a record whose bytes are not whole frames, no keyframe first).
int demo_parse(Demo *demo, const uint8_t *data, size_t length, char *err, size_t err_size);
void demo_free(Demo *demo);

int demo_first_keyframe(const Demo *demo);                  // record index, -1 none
int demo_keyframe_at(const Demo *demo, uint32_t t_ms);      // the last keyframe at or before t (-1 none)

// One framed packet of a record's bytes. Return 0 from fn to stop.
typedef int (*DemoPacketFn)(void *user, uint8_t type, uint8_t *payload, uint16_t length);
// Walks bytes as frames; 1 when they were whole frames (and fn never stopped), 0 otherwise.
int demo_frames_each(uint8_t *bytes, uint32_t length, DemoPacketFn fn, void *user);

// The suspect's spans (computeSuspectSegments, deob-10.0.7.js:201693-201737): a SPAWN of the suspect
// makes it present (and alive as its `active`), its KILL as the victim dead, its LEAVE gone; a keyframe
// sets presence from its SPAWNs. Spans where it is present and alive, closer than 10 s merged. No suspect
// (uid < 0), no duration or no span: the whole demo [0, duration]. Returns the count (<= max).
typedef struct {
    uint32_t start, end;
} DemoSegment;
size_t demo_suspect_segments(const Demo *demo, int32_t suspect_uid, DemoSegment *out, size_t max);
// clampToSegments (:201739-201751): t inside a span stays, else the nearest span edge.
uint32_t demo_clamp_to_segments(const DemoSegment *segments, size_t count, uint32_t t_ms);

// hideIdentities (:201752-201782): every SPAWN name becomes "Player_<uid>"; every chat line's sender (CHAT,
// SCRIPT_CHAT, CHAT_V7, a bot's CHAT_NAMES; server lines too) becomes "Player_<uid>" of the player whose
// PLAYER_INFO had the sender's account, or "Player" when none did (a server line, a guest), and a CHAT_V7
// loses its account id; chat text is left as it is. Every PLAYER_INFO loses its account id, level, clan and
// account flags (a bot keeps its bot bit). In place.
void demo_anonymize(Demo *demo);

// The meta's suspect (resolveSuspectSid, :201666-201690): the call whose token is call_token, else the
// first of suspects; -1 none.
int32_t demo_resolve_suspect(const Demo *demo, const char *call_token);
