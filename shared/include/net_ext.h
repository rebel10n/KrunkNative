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
    NET_EXT_PLAYER_SPEED_BOOST = 0x0202, // "spb" speedBoost, silent (NetSpeedBoostPacket; official lobbies, krunker_link.c)
    NET_EXT_PLAYER_NAME = 0x0203,        // "00" a player's new name (NetPlayerNamePacket; official lobbies, krunker_link.c)

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

    // 0x8000 live account (KrunkNative): signing in, switching accounts or signing out from the menu
    // during a session re-verifies the live connection, no rejoin (server/src/accounts.c "live sign-in")
    NET_EXT_KN_ACCOUNT_AUTH = 0x8000,   // C -> S NetAccountAuthPacket
    NET_EXT_KN_ACCOUNT_RIGHTS = 0x8001, // S -> C NetAccountRightsPacket

    // 0x8010 KrunkNative gameplay (shared/include/drank.h): the Purple Drank's laced event
    NET_EXT_KN_LACED = 0x8010, // S -> C NetLacedPacket (everyone, the laced player included)

    // 0x8011 bots (KrunkNative): what a spectator following a bot sees of its AI (0x8010 is nettspend's NET_EXT_KN_LACED)
    NET_EXT_KN_BOT_PATH = 0x8011, // S -> C NetBotPathPacket (variable size, NET_BOT_PATH_SIZE(n))
} NetExtSubtype;

// NET_EXT_KN_ACCOUNT_AUTH (C -> S): the identity the player now has in the menu. The server verifies
// it exactly as a JOIN's (a session token through the coordinator's token auth, a "hj1:" through
// hasJoined), never trusting the claim, then re-derives the connection's account, DEV, op and host
// rights and answers with NET_EXT_KN_ACCOUNT_RIGHTS (the same seq). GUEST signs out: the
// connection drops to a guest at once. A client only sends it after this connection's server
// announced the kind in a RIGHTS push (live_kinds); others keep the rejoin (servers_ui.c).
enum {
    NET_ACCOUNT_AUTH_GUEST = 0,      // signed out: back to Guest_N
    NET_ACCOUNT_AUTH_SESSION = 1,    // key = the coordinator session token (brain-handed lobbies only, as JOIN.session)
    NET_ACCOUNT_AUTH_HAS_JOINED = 2, // key = "hj1:" + a fresh client nonce after POST /auth/join, name = the account
};
typedef struct {
    uint8_t kind;  // NET_ACCOUNT_AUTH_*
    uint8_t seq;   // echoed by the answer
    char name[32]; // HAS_JOINED: the account name /auth/join was told
    char key[64];  // SESSION: the token (NetJoinPacket.session's size); HAS_JOINED: "hj1:" + 32 hex
} NetAccountAuthPacket;

// NET_EXT_KN_ACCOUNT_RIGHTS (S -> C): the connection's identity and rights. Pushed once its join
// is settled (status STATE: tells the client this server takes live sign-ins), and as the answer
// to every NET_EXT_KN_ACCOUNT_AUTH. REFUSED / BUSY / UNAVAILABLE changed nothing.
enum {
    NET_ACCOUNT_RIGHTS_STATE = 0,       // push (join settled), seq 0
    NET_ACCOUNT_RIGHTS_OK = 1,          // verified and applied (or signed out)
    NET_ACCOUNT_RIGHTS_REFUSED = 2,     // the token / hasJoined did not verify, or not allowed here (ranked)
    NET_ACCOUNT_RIGHTS_BUSY = 3,        // rate limited, or another verification is still running
    NET_ACCOUNT_RIGHTS_UNAVAILABLE = 4, // this server cannot verify that kind (no coordinator / auth service)
};
#define NET_RIGHTS_DEV 1u  // passes the DEV checks (verified DEV account, op, the server's own machine)
#define NET_RIGHTS_HOST 2u // hosts this custom lobby (premium custom host)
#define NET_RIGHTS_OP 4u   // a config / CLI op
#define NET_RIGHTS_MOD 8u  // a moderator account
#define NET_ACCOUNT_LIVE_SESSION (1u << NET_ACCOUNT_AUTH_SESSION)       // live_kinds: takes session tokens
#define NET_ACCOUNT_LIVE_HAS_JOINED (1u << NET_ACCOUNT_AUTH_HAS_JOINED) // live_kinds: takes hasJoined
typedef struct {
    uint8_t status;     // NET_ACCOUNT_RIGHTS_*
    uint8_t seq;        // the AUTH answered, 0 for a push
    uint8_t rights;     // NET_RIGHTS_*
    uint8_t live_kinds; // NET_ACCOUNT_LIVE_* (always takes GUEST)
    int32_t conn_id;    // the connection (NetInitPacket.client_id): the same one before and after
    int32_t account_id; // 0 guest
    char name[32];      // the connection's name (the next spawn's)
    char message[64];   // why, for REFUSED / BUSY / UNAVAILABLE
} NetAccountRightsPacket;

// NET_EXT_KN_LACED (S -> C, to everyone): the authority rolled a Purple Drank pour-up (reload) as
// laced (shared/src/drank.c drank_pour): `uid` is high for `duration_ms` from now. The client's
// own player gets the distortion post-effect; others may show an indicator. Replaces any spell
// still running (the timer restarts). drank_apply_laced reads it into Player.laced_timer.
#pragma pack(push, 1)
typedef struct {
    int32_t uid;          // the laced player (Player.uid / NetInitPacket client id)
    uint32_t duration_ms; // the spell from now (DRANK: WeaponStream.laced_ms, 8000)
} NetLacedPacket;
#pragma pack(pop)

// NET_EXT_PLAYER_SPEED_BOOST (S -> C, to the player): Krunker "spb" [speedBoost, silent]
// (game.deob.js:220498): Player.speed_boost (the movement acceleration x it, 0 = none); unless
// silent the chat shows "SpeedBoost - <value>".
// NET_EXT_PLAYER_NAME (S -> C): one rename of Krunker "00" UPDATE_NAMES [[id, name, alias]..., keepOpen]
// (game.deob.js:220322), the player's display name from now on.
#pragma pack(push, 1)
typedef struct {
    float boost;    // speedBoost (the server clamps it to 0.1..10; 0 = none)
    uint8_t silent; // arg 1: no chat line
} NetSpeedBoostPacket;

typedef struct {
    int32_t uid;   // the player's sid
    char name[32]; // PLAYER_NAME_SIZE, NUL-terminated
} NetPlayerNamePacket;
#pragma pack(pop)
// NET_EXT_KN_BOT_PATH (S -> C): the spectated bot's AI state for the show_navmeshes overlay
// (client/src/nav_overlay.c) and the spectator HUD's state line (hud_spectate.c). The server
// sends it only to a spectator following a bot (NET_PACKET_SPECTATE_TARGET), for that bot,
// NET_BOT_PATH_INTERVAL_S apart (server/src/net.c net_broadcast_bot_paths), from
// bot_path_current() / bot_path_goal() (shared/include/bot_path.h) and the bot's target memory.
// The body is the struct cut after the last point: NET_BOT_PATH_SIZE(n) bytes
// (shared/src/bot_path_net.c fills and decodes it). Positions are feet, world units.
#define NET_BOT_PATH_POINTS_MAX 28        // 60-byte head + 28 x 16 = 508 <= NET_EXT_BODY_MAX
#define NET_BOT_PATH_STATE_SIZE 12        // state_name: bot_state_name() ("Hunt", "Attack", ...)
#define NET_BOT_PATH_INTERVAL_S 0.2f      // the server's send interval per spectator
#define NET_BOT_PATH_HAS_GOAL 1u          // flags: goal[] is where the bot is heading
#define NET_BOT_PATH_HAS_TARGET 2u        // target_sid / target[]: the bot has a target player
#define NET_BOT_PATH_SEES_TARGET 4u       // ... and has line of sight to it right now
#define NET_BOT_PATH_HAS_LAST_KNOWN 8u    // last_known[]: where it last saw its target
#define NET_BOT_PATH_PARTIAL 16u          // the route does not reach the goal
#define NET_BOT_PATH_STRANDED 32u         // the goal is unreachable: the bot waits for a new one
#define NET_BOT_PATH_TRUNCATED 64u        // the route has more waypoints than fit (NET_BOT_PATH_POINTS_MAX)
#define NET_BOT_PATH_ACTIVE 128u          // the bot's player is alive (dead: state only, no points)
typedef struct {
    float x, y, z;       // feet
    uint8_t kind;        // BotPathKind of the segment ending here (0 for point 0)
    uint8_t flags;       // BOT_PATH_FLAG_* of that segment
    uint8_t reserved[2];
} NetBotPathPoint;
typedef struct {
    int32_t bot_sid;     // the bot's Player uid (the spectator's target)
    int32_t target_sid;  // its target's uid, -1 none
    uint8_t state;       // BotState
    uint8_t flags;       // NET_BOT_PATH_*
    uint8_t n;           // points that follow: 0 = no route; else point 0 is the bot's feet
    uint8_t reserved;
    char state_name[NET_BOT_PATH_STATE_SIZE]; // bot_state_name(state), so new states need no client change
    float goal[3];       // NET_BOT_PATH_HAS_GOAL
    float target[3];     // NET_BOT_PATH_HAS_TARGET: the target's feet when sent (the client follows the live player)
    float last_known[3]; // NET_BOT_PATH_HAS_LAST_KNOWN
    NetBotPathPoint points[NET_BOT_PATH_POINTS_MAX];
} NetBotPathPacket;
#define NET_BOT_PATH_SIZE(n) (offsetof(NetBotPathPacket, points) + (size_t) (n) * sizeof(NetBotPathPoint))

// Bodies fit one NetPacket (NET_MAX_PAYLOAD 512) after the subtype.
#define NET_EXT_BODY_MAX (512 - NET_EXT_HEADER_SIZE)

// Writes the envelope (subtype + body) into out (at least NET_EXT_HEADER_SIZE + body_length
// bytes); returns its length, 0 when the body is too long.
uint16_t net_ext_pack(uint8_t *out, size_t out_size, uint16_t subtype, const void *body, uint16_t body_length);
// Splits an EXT payload; 0 when shorter than the header.
int net_ext_unpack(const void *payload, uint16_t length, uint16_t *subtype, const uint8_t **body, uint16_t *body_length);
