#pragma once
// NET_PACKET_EXT (254): the envelope for packets added from Krunker v10.0.2 on (coordinator
// decision, doc/v10/net.md "KrunkNative packet ids"). The 1-byte NetPacket type stays; every new
// packet is an EXT whose payload starts with a little-endian u16 subtype, followed by the
// subtype's body (at most NET_EXT_BODY_MAX bytes):
//
//   NetPacket { type = NET_PACKET_EXT, length = 2 + n, payload = [subtype lo, subtype hi, body[n]] }
//
// EXT packets go only to peers whose HELLO / INIT announced at least NET_PROTOCOL_MINOR_EXT;
// receivers ignore subtypes they do not know (and a short or malformed envelope). Subtype ranges
// are owned per feature like the 1-byte blocks (doc/v7/decisions.md "Packet id blocks"); a body
// layout is the owner's, documented next to its struct. KrunkNative-only packets use 0x8000 up.
//
// Not ported, on purpose: v10 CHEAT_CHECK "cc" / CHEAT_CHECK_RESPONSE "ccr" (server-sent JS the
// client evals; doc/v10/net.md section 6). No subtype is reserved for them.
#include <stddef.h>
#include <stdint.h>

#define NET_PACKET_EXT 254
#define NET_EXT_HEADER_SIZE 2

typedef enum {
    NET_EXT_NONE = 0x0000,

    // 0x0100 entities (v10 KrunkScript entities; doc/v10/net.md "Entities"), S -> C
    NET_EXT_ENTITY_SPAWN = 0x0100,  // "e+"  [[eid, archId, tick, state{}, tag, mode, hp]]
    NET_EXT_ENTITY_REMOVE = 0x0101, // "e-"  eid, reason
    NET_EXT_ENTITY_STATE = 0x0102,  // "es"  [[eid, tick, state{}], ...]
    NET_EXT_ENTITY_EVENT = 0x0103,  // "ee"  eid, name, a, b
    NET_EXT_ENTITY_ATTACH = 0x0104, // "ea+" playerSid, eid, {o, lk, ctl, soft, nd}
    NET_EXT_ENTITY_DETACH = 0x0105, // "ea-" playerSid, eid

    // 0x0200 player state, S -> C
    NET_EXT_PLAYER_POSE = 0x0200,  // "pz"  sid, pose | 0
    NET_EXT_PLAYER_LOCKS = 0x0201, // "plk" disableShooting, disableMelee

    // 0x0300 store / live service, S -> C
    NET_EXT_SKIN_DEFINITION_CHANGED = 0x0300, // "sdc" {skinId, skin}
    NET_EXT_SPIN_WHEELS_UPDATED = 0x0301,     // "swu" spin data

    // 0x0400 Discord link
    NET_EXT_DISCORD_LINK_WAIT = 0x0400,   // "dlnkW" C -> S
    NET_EXT_DISCORD_LINK_RESULT = 0x0401, // "dlnkR" S -> C

    // 0x0500 charge consent (KrunkScript purchases)
    NET_EXT_CHARGE_CONSENT_REQUEST = 0x0500,  // "chreq" S -> C: requestId, value, flag
    NET_EXT_CHARGE_CONSENT_RESPONSE = 0x0501, // "chrsp" C -> S: requestId[, 1]

    // 0x0600 ranked
    NET_EXT_JOINED_RANK = 0x0600, // "joinedRank" C -> S, no body (v7 key, first used by v10)

    // 0x0700 protocol / ruleset (KrunkNative, the version toggle)
    NET_EXT_BLOCK_PROTOCOL = 0x0700,

    // 0x8000.. KrunkNative-only
    NET_EXT_BLOCK_KN = 0x8000,
} NetExtSubtype;

// Bodies fit one NetPacket (NET_MAX_PAYLOAD 512) after the subtype.
#define NET_EXT_BODY_MAX (512 - NET_EXT_HEADER_SIZE)

// Writes the envelope (subtype + body) into out (at least NET_EXT_HEADER_SIZE + body_length
// bytes); returns its length, 0 when the body is too long.
uint16_t net_ext_pack(uint8_t *out, size_t out_size, uint16_t subtype, const void *body, uint16_t body_length);
// Splits an EXT payload; 0 when shorter than the header.
int net_ext_unpack(const void *payload, uint16_t length, uint16_t *subtype, const uint8_t **body, uint16_t *body_length);
