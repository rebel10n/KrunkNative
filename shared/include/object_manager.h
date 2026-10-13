#pragma once
// KrunkNative Object Manager: a Garry's Mod style spawn menu (F1). Not a Krunker v7.1.4 feature
// (doc/v7/decisions.md "Object Manager"). Players carry extra loadout weapons and drop weapon
// pickups at the crosshair; devs, the game host and the offline sandbox also spawn objects,
// entities and NPCs. Everything is server-authoritative, rate limited and capped.
//
// Packets: NET_PACKET_BLOCK_SANDBOX (250-254), sent only to peers that announced
// NET_PROTOCOL_MINOR_V7 (server/src/net.c net_peer_takes); clients send SPAWN_REQ only to such
// servers (or the offline sandbox).
//
//   NET_PACKET_SPAWN_REQ     C->S NetSpawnReqPacket: an Object Manager action
//   NET_PACKET_SPAWN_RESULT  S->C NetSpawnResultPacket: its outcome + what this player may do now
//   NET_PACKET_LOADOUT_EXTRA S->C NetLoadoutExtraPacket: a player's extra loadout weapons, to
//                            everyone after each change and after every SPAWN of such a player
//   NET_PACKET_SPAWNED_OBJ   S->C NetSpawnedObjPacket [+ the map object record JSON]: a spawned
//                            thing appears / goes; replayed to joiners (object_manager_send_state).
//                            One id for every kind (OM_KIND_*): 254 stays spare.
//
// Spawned map objects live in Map.objects after the map's own (uid = index, like the builds),
// flagged Object.spawned, and go at the next round start with the builds (world_remove_builds).
#include <shared.h>

#define NET_PACKET_SPAWN_REQ (NET_PACKET_BLOCK_SANDBOX + 0)
#define NET_PACKET_SPAWN_RESULT (NET_PACKET_BLOCK_SANDBOX + 1)
#define NET_PACKET_LOADOUT_EXTRA (NET_PACKET_BLOCK_SANDBOX + 2)
#define NET_PACKET_SPAWNED_OBJ (NET_PACKET_BLOCK_SANDBOX + 3)

// The menu's tabs (client/src/ui/object_manager_ui.c), in the order they are shown.
typedef enum {
    OM_TAB_OBJECTS = 0,
    OM_TAB_ENTITIES,
    OM_TAB_NPCS,
    OM_TAB_WEAPONS,
    OM_TAB_COUNT
} ObjectManagerTab;

typedef enum {
    OM_ACTION_QUERY = 0,     // no-op: answers with the caps (the menu asks when it opens)
    OM_ACTION_ADD_WEAPON,    // item = weapon id: an extra loadout slot
    OM_ACTION_REMOVE_WEAPON, // item = weapon id: drop that extra slot
    OM_ACTION_CLEAR_WEAPONS, // every extra slot
    OM_ACTION_DROP_WEAPON,   // item = weapon id: a weapon pickup on the surface at the crosshair
    OM_ACTION_SPAWN_ENTITY,  // item = entity catalogue index, at the crosshair
    OM_ACTION_SPAWN_NPC,     // item = NPC catalogue index, at the crosshair
    OM_ACTION_SPAWN_OBJECT,  // item = object catalogue index, at the crosshair
    OM_ACTION_REMOVE_AIM,    // remove the spawned thing at the crosshair
    OM_ACTION_UNDO,          // remove this player's last spawned thing
    OM_ACTION_COUNT
} ObjectManagerAction;

typedef enum {
    OM_STATUS_OK = 0,
    OM_STATUS_DENIED,    // not allowed for this player in this game
    OM_STATUS_LIMIT,     // a cap (NetSpawnResultPacket.limit)
    OM_STATUS_RATE,      // too many requests: wait a moment
    OM_STATUS_INVALID,   // unknown item / action
    OM_STATUS_NO_TARGET, // nothing to place it on at the crosshair
    OM_STATUS_NOTHING,   // nothing to undo / remove
    OM_STATUS_DEAD,      // not spawned
    OM_STATUS_COUNT
} ObjectManagerStatus;

#define OM_CAP_WEAPONS 1u // extra loadout weapons and weapon pickups
#define OM_CAP_SPAWN 2u   // objects, entities, NPCs (dev, host, sandbox)
#define OM_CAP_DEV 4u     // the player passes the dev checks

// What a spawned thing is (NetSpawnedObjPacket.kind, the undo list).
typedef enum {
    OM_KIND_OBJECT = 0, // a map object (uid)
    OM_KIND_ENTITY,     // reserved: entities replicate through their own packets
    OM_KIND_NPC,        // reserved: NPCs replicate through the AI / bot packets
} ObjectManagerKind;

#define OM_SPAWNED_ADD 1
#define OM_SPAWNED_REMOVE 2

// Caps and rates (server-side): one table so hosts can be tuned in one place.
#define OM_RATE_BURST 10.0f        // request bucket size
#define OM_RATE_PER_SECOND 4.0f    // refill
#define OM_PICKUPS_PER_PLAYER 16   // live weapon pickups a player spawned
#define OM_OBJECTS_PER_PLAYER 64   // live spawned map objects (pickups included) per player
#define OM_OBJECTS_MAX 256         // live spawned map objects in the game
#define OM_AIM_RANGE 400.0f        // crosshair placement range (units)
#define OM_RECORD_MAX 480          // the longest record JSON a SPAWNED_OBJ carries

#pragma pack(push, 1)
typedef struct {
    uint8_t action;  // ObjectManagerAction
    uint8_t reserved;
    int16_t item;    // weapon id / catalogue index (by action)
    float pitch;     // the client's view at the request: the crosshair (x_dir)
    float yaw;       // (y_dir)
} NetSpawnReqPacket;

typedef struct {
    uint8_t action;    // the request answered (OM_ACTION_QUERY for the caps)
    uint8_t status;    // ObjectManagerStatus
    uint8_t caps;      // OM_CAP_* for this player now
    uint8_t extra_max; // PLAYER_EXTRA_WEAPONS_MAX
    int16_t item;      // the request's item
    uint16_t limit;    // the cap that was hit (OM_STATUS_LIMIT), else 0
} NetSpawnResultPacket;

typedef struct {
    int32_t uid;
    uint8_t count;
    uint8_t weapons[PLAYER_EXTRA_WEAPONS_MAX]; // weapon ids, count of them used
} NetLoadoutExtraPacket;

typedef struct {
    uint8_t op;      // OM_SPAWNED_ADD / OM_SPAWNED_REMOVE
    uint8_t kind;    // ObjectManagerKind
    uint16_t reserved;
    int32_t id;      // OM_KIND_OBJECT: the map object uid
    int32_t owner;   // the spawning player's uid
    // ADD of an object: the map object record (v7 map JSON, NUL-terminated) follows
} NetSpawnedObjPacket;
#pragma pack(pop)

// Who is asking (the server fills it from the connection; the shared rules decide).
typedef struct {
    unsigned char dev:1;    // server ops, DEV accounts, the server's own machine
    unsigned char host:1;   // the custom game's host account
    unsigned char ranked:1; // a ranked / comp match
} ObjectManagerPerms;

// ---- catalogue (both sides)
int object_manager_weapon_listed(int weapon_id); // the Weapons tab: every weapon but the knife and the bomb

// ---- rules
unsigned int object_manager_caps(const Game *game, const ObjectManagerPerms *perms); // OM_CAP_*
const char *object_manager_status_text(int status); // the menu's message for a refusal

// ---- authority
// Runs one request for `player` (NULL: not spawned) and fills `out`. Sends LOADOUT_EXTRA /
// SPAWNED_OBJ / PICKUP to everyone as needed; the caller sends `out` back.
void object_manager_request(Game *game, Player *player, const NetSpawnReqPacket *req, const ObjectManagerPerms *perms,
                            NetSpawnResultPacket *out);
void object_manager_send_extras(Game *game, const Player *player, int target); // LOADOUT_EXTRA of `player`
void object_manager_send_state(Game *game, int uid);   // a joiner: every live spawned object
void object_manager_round_start(Game *game);           // after world_remove_builds: drop the records
// The extra weapons `player` may keep (a spawn with caps lacking OM_CAP_WEAPONS clears them).
int object_manager_keep_extras(const Game *game, const ObjectManagerPerms *perms);

// ---- both sides
// Replace the extra weapon list (invalid / duplicate / over-cap ids dropped) and rebuild the
// loadout's tail. The held slot stays when it still exists.
void player_set_extra_weapons(Player *player, const int *weapon_ids, int count);
// Client: a SPAWNED_OBJ (`length` bytes). 1 applied.
int object_manager_apply_spawned(Game *game, const void *payload, uint16_t length);

// The surface at the crosshair: the player's eye along (pitch, yaw), map objects and terrain.
typedef struct {
    vec3 point;   // where the ray hit
    vec3 normal;  // the face it hit (axis aligned), terrain +y
    int object;   // map object uid, -1 terrain
} ObjectManagerAim;
int object_manager_aim(const Game *game, const Player *player, float pitch, float yaw, ObjectManagerAim *out);
