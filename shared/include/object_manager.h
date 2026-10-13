#pragma once
// KrunkNative Object Manager: a Garry's Mod style spawn menu (F1). Not a Krunker v7.1.4 feature
// (doc/v7/decisions.md "Object Manager"). Players carry extra loadout weapons and drop weapon
// pickups at the crosshair; devs, the game host and the offline sandbox also spawn objects,
// entities and NPCs, and take them back (undo, remove at the crosshair). Everything is
// server-authoritative, rate limited and capped.
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
//                            One id for every kind (OM_KIND_*). 254 is NET_PACKET_EXT (net_ext.h).
//
// Console (shared/src/chat_commands.c, doc/console.md): `give <weapon|entity> [player]` runs
// OM_ACTION_ADD_WEAPON / OM_ACTION_SPAWN_ENTITY for a player, `drop` OM_ACTION_DROP_HELD (or v7's
// drpw for a picked-up weapon). The Tool Gun (WEAPON_ID_TOOL_GUN, the window's Tools pane) sends
// each shot as OM_ACTION_TOOL with the tool picked there. The Physics Gun (WEAPON_ID_PHYS_GUN) and
// the Move / Resize tools (btools) send OM_ACTION_EDIT: a NetSpawnEditPacket (the request with a
// pose) that moves, resizes or turns a spawned object in place, replicated as SPAWNED_OBJ
// OM_SPAWNED_UPDATE; Undo takes edits back too (the previous pose is kept).
//
// Spawned map objects live in Map.objects after the map's own (uid = index, like the builds),
// flagged Object.spawned, and go at the next round start with the builds (world_remove_builds).
//
// Map edits: devs and the host (OM_CAP_MAP) also delete, duplicate, turn, move, resize and carry
// the map's own objects with the Tool Gun and the Physics Gun. The map's Object leaves its uid for a
// stand-in (Object.map_edit): one built from the map record with the edited "p" / "s" / "r" / "d",
// or an inactive placeholder for a delete; the original waits detached and comes back with Undo, at
// the next round start and before a map change (object_manager_restore_map). Everyone hears
// SPAWNED_OBJ OM_SPAWNED_MAP_EDIT (the pose keys only: every client has the map's record) /
// MAP_DELETE / MAP_RESTORE, replayed to joiners. Triggers and KrunkScript keep addressing the uid:
// a moved object's trigger fires where it went (its trigger state starts over), a deleted one's
// stops and actions aimed at it find the inactive placeholder. Pickups, zombies machines, flags,
// objectives, zones of the modes, nodes, complex-collision and animated (GLTF) assets stay as they
// are (object_manager_map_editable). The bot grid is rebuilt at most once a second after map edits
// (bot_nav_invalidate); the client re-renders the sun's shadow map on each edit; ambient-occlusion
// planes stay where the map built them.
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
    OM_ACTION_DROP_HELD,     // the extra weapon in hand: a weapon pickup a step in front, out of the loadout (drop, Z)
    OM_ACTION_TOOL,          // item = ObjectManagerTool: a Tool Gun shot at the crosshair (the Tool Gun in hand)
    OM_ACTION_EDIT,          // NetSpawnEditPacket only (item = ObjectManagerEditSource): a spawned object's new pose
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
    OM_STATUS_KEEP,      // DROP_HELD: not an extra weapon (class / melee weapons stay; v7's drpw takes picked ones)
    OM_STATUS_NO_TOOL,   // TOOL: the Tool Gun is not in hand
    OM_STATUS_UNSUPPORTED, // TOOL / EDIT: the tool does not work on that (a pickup, a mode object, an animated asset ...)
    OM_STATUS_BLOCKED,   // EDIT: the object would end up inside a player
    OM_STATUS_BOUNDS,    // EDIT: out of reach, or past the size limits (OM_EDIT_SIZE_*)
    OM_STATUS_COUNT
} ObjectManagerStatus;

#define OM_CAP_WEAPONS 1u // extra loadout weapons and weapon pickups
#define OM_CAP_SPAWN 2u   // objects, entities, NPCs (dev, host, sandbox)
#define OM_CAP_DEV 4u     // the player passes the dev checks
#define OM_CAP_MAP 8u     // the Tool Gun / Physics Gun also change the map's own objects (devs, the host outside ranked)

// What a spawned thing is (NetSpawnedObjPacket.kind, the undo list). Only map objects (and the
// Piss's puddles) travel in SPAWNED_OBJ: spawnables, airdrops, AIs and bots replicate through
// their own v7 packets.
typedef enum {
    OM_KIND_OBJECT = 0, // a map object (uid): pickups, scraps, machines, prefabs
    OM_KIND_ENTITY,     // a spawnable (sid) or an airdrop (sid)
    OM_KIND_NPC,        // an AI (sid) or a bot (GameBot.id)
    OM_KIND_PUDDLE,     // KrunkNative Piss: a puddle (GamePuddle.id) + NetPuddlePayload (shared/include/piss.h); never in the undo list
} ObjectManagerKind;

// ---- catalogue (both sides): the Entities and NPCs tabs (the Weapons tab lists the ruleset's weapons)
typedef enum {
    OM_ENTRY_AI = 0,      // a built-in AI kind (ai.c ai_builtin), data = its index
    OM_ENTRY_AI_TEMPLATE, // a Soul Sanctum AI (game_ai_resolve_template by name)
    OM_ENTRY_ZOMBIE,      // a zombies template (mode_zombie_template), data = its kind
    OM_ENTRY_BOT,         // a v7 bot player (bots.c), pinned to the spot
    OM_ENTRY_SPAWNABLE,   // a medals.js spawnable, data = SpawnableType (they despawn on their own timer)
    OM_ENTRY_AIRDROP,     // an airdrop crate falling onto the spot, data = package (-1 random)
    OM_ENTRY_RECORD,      // a map object record (json, "p" added; "s" defaults to the prefab's size)
} ObjectManagerEntryKind;

typedef struct {
    const char *name;
    const char *icon;     // a Material Icons ligature for the grid, or a page image ("img/props/prop_1.png")
    unsigned char kind;   // ObjectManagerEntryKind
    short data;
    const char *json;     // OM_ENTRY_RECORD
} ObjectManagerEntry;

// The entries of a tab (OM_TAB_OBJECTS / OM_TAB_ENTITIES / OM_TAB_NPCS; NULL and 0 for Weapons).
// The index is the request's item.
const ObjectManagerEntry *object_manager_entries(int tab, int *count);

// ---- the Tool Gun's tools (the window's Tools pane; one function each in object_manager.c)
typedef enum {
    OM_TOOL_DELETER = 0, // remove the spawned thing at the crosshair (OM_ACTION_REMOVE_AIM)
    OM_TOOL_DUPLICATOR,  // copy the spawned object at the crosshair; the next shot places the copy
    OM_TOOL_ROTATOR,     // turn the spawned object at the crosshair a quarter turn
    OM_TOOL_INFO,        // what is at the crosshair: kind, owner, uid (a NOTIFY to the shooter)
    OM_TOOL_MOVE,        // btools: select a spawned object, drag its X / Y / Z arrows (OM_ACTION_EDIT, the client's handles)
    OM_TOOL_RESIZE,      // btools: drag a face handle to resize along its axis (shift: both faces)
    OM_TOOL_COUNT
} ObjectManagerTool;

typedef struct {
    const char *name;  // "Deleter"
    const char *icon;  // Material Icons ligature
    const char *hint;  // the pane's line under the name
    unsigned char mutates; // ownership applies (your own spawns unless dev / host)
    unsigned char handles; // btools: driven by the client's handles (OM_ACTION_EDIT), no OM_ACTION_TOOL shots
} ObjectManagerToolInfo;
const ObjectManagerToolInfo *object_manager_tool_info(int tool); // NULL out of range

// OM_ACTION_TOOL answered OK: NetSpawnResultPacket.limit says what happened
#define OM_TOOL_DONE 0   // removed / rotated
#define OM_TOOL_COPIED 1 // Duplicator: copied, the next shot places it
#define OM_TOOL_PASTED 2 // Duplicator: the copy placed
#define OM_TOOL_SHOWN 3  // Info: the text came as a NET_PACKET_NOTIFY

#define OM_DROP_TOSS 8.0f // DROP_HELD: how far in front of the player the pickup lands (units)

#define OM_SPAWNED_ADD 1
#define OM_SPAWNED_REMOVE 2
#define OM_SPAWNED_UPDATE 3 // an edited object: the record (JSON) follows, it replaces the object at its uid
// a map object (id = its uid, owner = the editor): MAP_EDIT carries the edited keys of its record
// ({"p", "s", "r" / "d"}, JSON) applied to the map's own record; MAP_DELETE / MAP_RESTORE nothing
#define OM_SPAWNED_MAP_EDIT 4
#define OM_SPAWNED_MAP_DELETE 5
#define OM_SPAWNED_MAP_RESTORE 6

// ---- edits (OM_ACTION_EDIT): the Physics Gun and the Move / Resize tools
typedef enum {
    OM_EDIT_SOURCE_PHYSGUN = 0, // the Physics Gun in hand
    OM_EDIT_SOURCE_BTOOLS,      // the Tool Gun in hand (Move / Resize)
} ObjectManagerEditSource;

#define OM_EDIT_BEGIN 1        // a new undo step: a grab or a drag starts (its first edit)
#define OM_EDIT_DROP 2         // the Physics Gun's release with "Drop": down onto the surface below
#define OM_EDIT_INTERVAL 0.05f // seconds between one player's edits (clients send at most 10 / s)
#define OM_EDIT_SIZE_MIN 1.0f  // per axis (units)
#define OM_EDIT_SIZE_MAX 200.0f // (a map object bigger than this may keep its size, never grow)
#define OM_EDIT_RANGE 420.0f   // the edited box's nearest point from the editor's eye
#define OM_EDITS_MAX 256       // the undo history of edits in a round (the oldest go)
#define OM_PHYSGUN_DIST_MIN 8.0f // the grab distance the wheel changes (units)
#define OM_PHYSGUN_DIST_MAX 300.0f

// A spawned object's pose as edits see it: the feet (bottom centre), the world-axis box, the
// quarter turns about y (map.js "d" for ramps and ladders, `directed`).
typedef struct {
    vec3 position;
    vec3 size;
    int quarter;
    int directed;
} ObjectManagerPose;

// Caps and rates (server-side): one table so hosts can be tuned in one place.
#define OM_RATE_BURST 10.0f        // request bucket size
#define OM_RATE_PER_SECOND 4.0f    // refill
#define OM_PICKUPS_PER_PLAYER 16   // live weapon pickups a player spawned
#define OM_OBJECTS_PER_PLAYER 64   // live spawned map objects (pickups included) per player
#define OM_OBJECTS_MAX 256         // live spawned map objects in the game
#define OM_NPCS_PER_PLAYER 12      // live AIs + bots a player spawned
#define OM_NPCS_MAX 32             // live spawned AIs + bots in the game (GAME_AI_LIMIT / GAME_BOTS_MAX still apply)
#define OM_ENTITIES_PER_PLAYER 24  // live spawnables + airdrops a player spawned
#define OM_ENTITIES_MAX 64         // in the game (MODE_MAX_SPAWNABLES, AIRDROP_MAX still apply)
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
    NetSpawnReqPacket req; // action OM_ACTION_EDIT, item ObjectManagerEditSource, the view (pitch, yaw)
    int32_t uid;           // the spawned object
    float position[3];     // its new pose (ObjectManagerPose)
    float size[3];
    uint8_t quarter;
    uint8_t flags;         // OM_EDIT_*
    uint16_t reserved;
} NetSpawnEditPacket;

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
int object_manager_weapon_listed(int weapon_id); // the Weapons tab: every v7 weapon but the knife and the bomb, + the KN Piss
int object_manager_weapon_allowed(int weapon_id); // an extra slot may hold it: the listed ones and the Tool Gun
int object_manager_entity_giveable(int index);    // `give` takes this Entities entry: spawnables and scrap pickups

// `give`: a weapon by id or name, else an Entities entry by name. Names compare normalised
// (letters and digits, case-insensitive): exact, else a unique prefix, else a unique substring
// (the Object Manager search). OM_GIVE_NONE: unknown, or *matches > 1 (ambiguous).
typedef enum { OM_GIVE_NONE = 0, OM_GIVE_WEAPON, OM_GIVE_ENTITY } ObjectManagerGiveKind;
int object_manager_give_lookup(const char *query, int *id, int *matches);
const char *object_manager_give_name(int kind, int id); // the weapon / entry name, NULL unknown

// ---- rules
unsigned int object_manager_caps(const Game *game, const ObjectManagerPerms *perms); // OM_CAP_*
// OM_ACTION_REMOVE_AIM: the spawned thing nearest along the crosshair (objects, AIs, bots,
// spawnables, airdrops) goes when it is the player's own, or the player is a dev or the host.
const char *object_manager_status_text(int status); // the menu's message for a refusal

// ---- authority
int object_manager_holds_extra(const Player *player); // the slot in hand is an extra one (DROP_HELD's case)
// Runs one request for `player` (NULL: not spawned) and fills `out`. Sends LOADOUT_EXTRA /
// SPAWNED_OBJ / PICKUP to everyone as needed; the caller sends `out` back.
void object_manager_request(Game *game, Player *player, const NetSpawnReqPacket *req, const ObjectManagerPerms *perms,
                            NetSpawnResultPacket *out);
void object_manager_send_extras(Game *game, const Player *player, int target); // LOADOUT_EXTRA of `player`
void object_manager_send_state(Game *game, int uid);   // a joiner: every live spawned object
void object_manager_round_start(Game *game);           // after world_remove_builds: drop the records (map edits restored)
// The extra weapons `player` may keep (a spawn with caps lacking OM_CAP_WEAPONS clears them).
int object_manager_keep_extras(const Game *game, const ObjectManagerPerms *perms);
// The kept extra weapons in place (the Tool Gun goes without OM_CAP_SPAWN); returns the new count.
int object_manager_filter_extras(const Game *game, const ObjectManagerPerms *perms, int *weapon_ids, int count);

// OM_ACTION_EDIT (a NetSpawnEditPacket): the same rules as the other actions (OM_CAP_SPAWN, Remove's
// ownership), the editor's own rate (OM_EDIT_INTERVAL), reach and size limits, no box into a
// player; applied in place and sent to everyone as OM_SPAWNED_UPDATE.
void object_manager_edit(Game *game, Player *player, const NetSpawnEditPacket *edit, const ObjectManagerPerms *perms,
                         NetSpawnResultPacket *out);

// ---- edits (both sides): the pose of a spawned object the edits take (active, not a pickup or a
// zombies machine), and the btools' handle math (pure: the client drags, the tests check)
int object_manager_editable(const Game *game, int uid); // a spawned object, or a map object object_manager_map_editable takes
int object_manager_pose(const Game *game, int uid, ObjectManagerPose *out);
// ---- map edits (both sides)
int object_manager_is_map_object(const Game *game, int uid);   // one of the map's own uids (edited or not)
int object_manager_map_editable(const Game *game, int uid);    // a map object the tools may change now (not deleted)
int object_manager_map_edited(const Game *game, int uid);      // 1 changed, 2 deleted, 0 as the map has it
// Puts every edited map object back (the map's own Objects at their uids). Round start, map change.
void object_manager_restore_map(Game *game);
float object_manager_snap(float value, float step); // to the nearest multiple of step (step <= 0: as is)
// Move: `delta` units along axis 0 x / 1 y / 2 z, snapped
void object_manager_pose_move(const ObjectManagerPose *from, int axis, float delta, float snap, ObjectManagerPose *out);
// Resize: the face on `axis` facing `sign` (+1 / -1) pushed out by `delta` (snapped; inward when
// negative), the opposite face kept, or both faces moved when `symmetric`; OM_EDIT_SIZE_MIN at least
// (growth stops at OM_EDIT_SIZE_MAX, or at the size it had when that is bigger)
void object_manager_pose_resize(const ObjectManagerPose *from, int axis, int sign, float delta, float snap, int symmetric,
                                ObjectManagerPose *out);
// `turns` quarter turns: w and l swap on odd ones (the AABB follows the model), not for ramps / ladders
void object_manager_pose_turn(const ObjectManagerPose *from, int turns, ObjectManagerPose *out);

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
// What the tools pick at the crosshair: the nearest object of any kind (solid or not: jump pads,
// teleporters, zones, triggers, planes, at least a unit thick), terrain in the way; object -1 none
// (normal is +y). 0: nothing in reach.
int object_manager_pick(const Game *game, const Player *player, float pitch, float yaw, ObjectManagerAim *out);
