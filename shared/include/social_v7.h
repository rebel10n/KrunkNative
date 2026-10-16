#pragma once
// player-viewer WP-E part 2b (doc/v7/player-viewer.md 1.5.2 and 1.5.8): the v7.1.4 text / voice
// channels, multi-spray and the spray wheel, prop-hunt taunts and quick chat.
//
// Packets: ids 245-249 of NET_PACKET_BLOCK_SOCIAL (240-249; 240 LEADER_STATS, 241-244 the death
// screen / spectate / minimap). The server sends them only to peers that announced
// NET_PROTOCOL_MINOR_V7 (server/src/net.c net_peer_takes), clients only to such servers (or the
// offline sandbox). One id serves both directions where v7 uses one event name for both ("ct" /
// "ch" are two names, but KrunkNative's ids are scarce): the struct depends on the direction.
//
//   NET_PACKET_CHAT_V7  C->S NetChatV7SendPacket  "ct", channel, msg
//                       S->C NetChatV7Packet      "ch", channel, sender, msg, type, color, senderColor, accountId
//   NET_PACKET_VOICE_V7 C->S / S->C NetSocialChunk pieces, framing in shared/include/voice.h
//                       ("vc", dataUrl, channel, micVolume, micQuality /
//                        "vc", dataUrl, name, sid, channel, volume, isFinal, accountId)
//   NET_PACKET_SPRAY_V7 C->S NetSprayRequestPacket "spry"[, wheelIndex]
//                       S->C NetSprayV7Packet      "sp", "<sid>_<n>", sprayIndex, x, y, z, rotY, rotX, rotZ, dyeId
//   NET_PACKET_TAUNT    C->S NetTauntPacket        "prpt", i (players.taunt: a network sound)
//   NET_PACKET_SOCIAL_V7 S->C uint8 version         (KrunkNative: the server takes the four above)
//
// Older peers keep the v3 shapes: NET_PACKET_CHAT_SEND / CHAT, VOICE_SEND / VOICE, the v3 "int 0"
// spray and NET_PACKET_SPRAY (one decal per player). The server bridges both ways.
#include <shared.h>

#define NET_PACKET_CHAT_V7 (NET_PACKET_BLOCK_SOCIAL + 5)  // both ways, see above
#define NET_PACKET_VOICE_V7 (NET_PACKET_BLOCK_SOCIAL + 6) // both ways: chunked voice clips with channel / volume
#define NET_PACKET_SPRAY_V7 (NET_PACKET_BLOCK_SOCIAL + 7) // C->S spray request, S->C a placed spray
#define NET_PACKET_TAUNT (NET_PACKET_BLOCK_SOCIAL + 8)    // C->S: "prpt" index
// S->C, uint8 NET_SOCIAL_V7_VERSION, after every INIT (and after the HELLO) to a v7 peer: this server
// takes ids 245-248. 1.3 servers that predate them ignore them, so a client sends them only after
// this packet (no NET_PROTOCOL bump; client/src/ui/hud_social.c social_server_v7).
#define NET_PACKET_SOCIAL_V7 (NET_PACKET_BLOCK_SOCIAL + 9)
#define NET_SOCIAL_V7_VERSION 1

// The text & voice channel (v7 index.js vLN02023, #chatSwitch): forced to global outside team
// modes; -1 on a received line or clip = every channel (io_addChat outside team modes, and
// server / game lines).
#define CHAT_CHANNEL_ALL (-1)
#define CHAT_CHANNEL_GLOBAL 0
#define CHAT_CHANNEL_TEAM 1

#define QUICK_CHAT_COOLDOWN 1.5f // f2028: Date.now() - lastMsgTime <= 1500 drops the message

#pragma pack(push, 1)
typedef struct {
    uint8_t channel;               // CHAT_CHANNEL_GLOBAL / CHAT_CHANNEL_TEAM (getChatTeam: anything but 1 is 0)
    char text[NET_CHAT_TEXT_SIZE]; // NUL-terminated UTF-8; sent with length 1 + strlen(text) + 1
} NetChatV7SendPacket;

typedef struct {
    int8_t channel;     // CHAT_CHANNEL_*: 1 only to the sender's team
    int32_t account_id; // the sender's account (mute_chat), 0 guest
    NetChatPacket chat; // kind NET_CHAT_PLAYER
} NetChatV7Packet;

typedef struct {
    int32_t index; // the wheel's store.skins index, -1 = none (plain "spry")
} NetSprayRequestPacket;

// players.sprayPosition's broadcast. A player's sprays cycle through `slot` 0..limit-1
// ("<sid>_<spraysPlaced>"): a spray in a used slot moves that decal.
typedef struct {
    int32_t uid;
    uint16_t slot;
    int32_t skin;        // store.skins index of a SPRAY (type 4) item; the client falls back to 2482
    float position[3];   // centre 0.1 in front of the face (round(2))
    int16_t y_rot;       // three.js rotation.y in degrees: 0 +z face, 180 -z, -90 -x, 90 +x
    int16_t x_rot;       // rotation.x: -90 top face, 90 bottom face
    int16_t z_rot;       // rotation.z: top / bottom faces turn with the sprayer's yaw (multiples of 90)
    int32_t dye;         // spray dye store index, -1 none (players.spray never passes one)
} NetSprayV7Packet;

typedef struct {
    uint8_t index; // config.taunts index 0..TAUNT_COUNT-1
} NetTauntPacket;
#pragma pack(pop)

// ---- sprays: store items of type SPRAY (assets/skins.json), v7 src/data/players.js:2011-2072

#define SPRAY_DEFAULT_INDEX 2482   // store.skins[2482] "Krunker", every fallback
#define SPRAY_LEGACY_COUNT 53      // KrunkNative's v3.1.1 Game.sprays: index i is store item 2482 + i
#define SPRAY_ITEM_TYPE 4          // itemTypes.SPRAY
#define SPRAY_MAX_DEFAULT 1        // config.maxSprays
#define SPRAY_MAX_PREMIUM 2        // config.maxSpraysP
#define SPRAY_MAX_FEATURED 3       // config.maxSpraysF (featured premium accounts, and the sandbox)
#define SPRAY_WHEEL_MAX 15         // overlay.sprayWheelMax: owned sprays on the wheel (+ the equipped one)
#define SPRAY_WHEEL_DELAY 0.2f     // overlay.sprayWheelReq (200 ms of holding before it shows)

typedef struct {
    int index;           // store.skins index
    int id;              // textures/sprays/<id>.png
    float scl_w, scl_h;  // sclW / sclH (1 when absent): the size the face must fit
    unsigned char free;  // free: anyone may pick it on the wheel
    int spray_snd;       // spraySnd: the sound "spray_<n>", 0 = "spray"
} StoreSpray;

// A SPRAY item of the store table (loaded from assets/skins.json at the first call), NULL for
// anything else or when the table cannot be read.
const StoreSpray *store_spray(int index);
// ENT sprayIndex of older KrunkNative clients: 0..52 named v3.1.1's Game.sprays, which v7.1.4 sells
// as the free store items 2482..2534 (same textures). Every other index is returned unchanged.
int store_spray_from_legacy(int index);
// The other way, for NET_PACKET_SPRAY to older peers (they resolve 0..52 as Game.sprays).
int store_spray_to_legacy(int index);

// players.spray(player, index) (v10.0.2 v10:7939498): the store item a spray uses. `wheel_index`
// < 0 = none. Premium accounts and the sandbox spray the wheel's index, everyone else (and a plain
// "spry") the equipped sprayIndex (ENT ent[1]). It stays when it is a SPRAY item that is free (or
// any in the sandbox) or that canUseSkin allows (GameHooks.can_use_skin: the account owns it),
// else 2482: a guest sprays free sprays only.
int game_spray_pick(const Game*, const Player*, int wheel_index);
// "spry" from a connection: drains the player's queued inputs, then game_player_spray_pick.
void game_player_spray_request(Game*, Player*, int wheel_index);
// How many sprays of a player stay on the map (maxSprays / maxSpraysP / maxSpraysF).
int game_spray_limit(const Game*, const Player*);

// ---- prop hunt taunts (v7 config.taunts, players.taunt)

#define TAUNT_COUNT 6
#define TAUNT_VOLUME 0.3f
typedef struct {
    const char *sound; // config.networkSounds name
    float time;        // tm (s): the next taunt waits for it
} TauntConfig;
extern const TauntConfig g_taunts[TAUNT_COUNT];

// teamOptions[team - 1] == "Prop": the player is on the prop team of a prop-hunt mode.
int game_player_is_prop(const Game*, const Player*);
// "prpt" i: players.taunt(player, i) on the authority. Returns 1 when the taunt played.
int game_player_taunt(Game*, Player*, int index);
// force (nextTaunt's taunts): no cooldown check (players.taunt's third argument).
int game_player_taunt_ex(Game*, Player*, int index, int force);
