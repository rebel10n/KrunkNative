#pragma once
#include <stdlib.h>
#include <stdint.h>
#include <stddef.h> // offsetof (NET_ENT_PACKET_V3_SIZE)
#include <modes.h>
#include <weapons.h>
#include <cJSON.h>
#include <common.h>

typedef struct {
    int billboard_count;
    float mouse_sensitivity;
    float world_uv_scale;
    float player_height;
    float border_height;
    float camera_height;
    float climb_height;
    float min_delta;
    float max_delta;
    float jump_velocity;
    float jump_aim_slow;
    float jump_push;
    float crouch_jump;
    float crouch_speed;
    float crouch_lean;
    float player_speed;
    float slipping_speed;
    float air_speed;
    float aim_slow;
    float crouch_slow;
    float player_slipping_jump_cooldown;
    float terrain_slide_threshold;
    float terrain_slip_decel;
    float terrain_slide_decel;
    float terrain_decel;
    float slide_decel;
    float air_decel;
    float ground_decel;
    float ladder_decel;
    float crouch_distance;
    float gravity;
    float arm_scale;
    float arm_inset;
    float chest_width;
    float chest_scale;
    float leg_scale;
    float leg_height;
    float head_scale;
    float body_height;
    float player_scale;
    float min_decel;
    float slide_time;
    float player_slide_velocity_mlt;
    float player_terrain_slide_velocity_mlt;
    float idle_anim_speed;
    float interpolation;
    float lean_sensitivity;
    float lean_max;
    float lean_pull;
    float lean_pull_z;
    float lean_mlt_z;
    float bob_pull_y;
    float bob_pull_z;
    float bob_mlt_y;
    float bob_mlt_z;
    float crouch_anim;
    float collision_padding;
    float booster_speed;
    float ladder_speed;
    float ladder_scale;
    float ladder_width;
    float death_y;
    float recoil_mlt;
    float spread_adjustment;
    float upper_arm_length;
    float lower_arm_length;
    float arm_offset;
    float anim_mlt;
    float aim_anim_mlt;
    float crouch_anim_mlt;
    float step_anim;
    float step_pull;
    float disk_spin;
} GameConstants;

// Krunker v10.0.2's physics and animation constants (shared/src/rules/v10/constants_v10.c).
extern const GameConstants game_constants;

// Krunker v7.1.4 data/prefabs.js prefab ids (map object "i"; doc/v7/map-logic.md §1.4).
// Ids 0-59 keep their v3.1.1 index; 33 and 55 were renamed (the v3 names stay as aliases);
// 60-96 are new; v10.0.2 adds 97-98 and renames 41 (doc/v10/map-format.md §1). The per-prefab data is shared/include/prefabs.h (g_prefab_data, prefab_info).
typedef enum {
    PREFAB_CUBE,
    PREFAB_CRATE,
    PREFAB_BARREL,
    PREFAB_LADDER,
    PREFAB_PLANE,
    PREFAB_SPAWN_POINT,
    PREFAB_CAMERA_POSITION,
    PREFAB_VEHICLE,
    PREFAB_STACK,
    PREFAB_RAMP,
    PREFAB_SCORE_ZONE,
    PREFAB_BILLBOARD,
    PREFAB_DEATH_ZONE,
    PREFAB_PARTICLES,
    PREFAB_OBJECTIVE,
    PREFAB_TREE,
    PREFAB_CONE,
    PREFAB_CONTAINER,
    PREFAB_GRASS,
    PREFAB_CONTAINER_R,
    PREFAB_ACID_BARREL,
    PREFAB_DOOR,
    PREFAB_WINDOW,
    PREFAB_FLAG,
    PREFAB_GATE,
    PREFAB_CHECK_POINT,
    PREFAB_WEAPON_PICKUP,
    PREFAB_TELEPORTER,
    PREFAB_TEDDY,
    PREFAB_TRIGGER,
    PREFAB_SIGN,
    PREFAB_DEPOSIT_BOX,
    PREFAB_LIGHT_CONE,
    PREFAB_CAMERA,              // 33 (v3.1.1 SPECTATE_CAM)
    PREFAB_SPHERE,
    PREFAB_PLACEHOLDER,
    PREFAB_CARD_B,
    PREFAB_PALLET,
    PREFAB_LIQUID,
    PREFAB_SOUND_EMITTER,
    PREFAB_EVENT,
    PREFAB_BOT_PATH_NODE,       // 41: v7 TERMINAL, v10 BOT_PATH_NODE; builds nothing in both (no object, no uid)
    PREFAB_PREMIUM_ZONE,
    PREFAB_VERIFIED_ZONE,
    PREFAB_CUSTOM_ASSET,
    PREFAB_BOMB_SITE,
    PREFAB_BOOST_PAD,
    PREFAB_TEAM_ZONE,
    PREFAB_CYLINDER,
    PREFAB_POLICE,
    PREFAB_CAGE,
    PREFAB_E_BARREL,
    PREFAB_SHOWCASE,
    PREFAB_POINT_LIGHT,
    PREFAB_GHOST,
    PREFAB_AI_SPAWNER,          // 55 (v3.1.1 BOT)
    PREFAB_PUMPKIN,
    PREFAB_RUNE,
    PREFAB_SKELETON,
    PREFAB_KNIGHT,
    PREFAB_GIFT,                // 60
    PREFAB_CHRISTMASTREE,       // 61
    PREFAB_CANE,                // 62
    PREFAB_LIGHT_BAR,           // 63
    PREFAB_PATH_NODE,           // 64
    PREFAB_DIRECTIONAL_LIGHT,   // 65
    PREFAB_RECTAREA_LIGHT,      // 66
    PREFAB_SPOT_LIGHT,          // 67
    PREFAB_PRIZE_WHEEL,         // 68
    PREFAB_KR_ZONE,             // 69
    PREFAB_NOPVP_ZONE,          // 70
    PREFAB_PVP_ZONE,            // 71
    PREFAB_LEADERBOARD,         // 72
    PREFAB_TILES,               // 73
    PREFAB_CLOUDS,              // 74
    PREFAB_PARTNER_ZONE,        // 75
    PREFAB_SPRITE,              // 76
    PREFAB_DOMINATION_ZONE,     // 77
    PREFAB_ROPE_LADDER,         // 78
    PREFAB_ENVIRONMENT_ZONE,    // 79
    PREFAB_KPD_ZONE,            // 80
    PREFAB_DUMPSTER,            // 81
    PREFAB_SERVER_NODE,         // 82
    PREFAB_LAMP,                // 83
    PREFAB_WATER_TOWER,         // 84
    PREFAB_AC_UNIT,             // 85
    PREFAB_SHEET_METAL,         // 86
    PREFAB_ANTENNA,             // 87
    PREFAB_SATELLITE_DISH,      // 88
    PREFAB_SPRAY,               // 89
    PREFAB_WEAPONBOX,           // 90
    PREFAB_MYSTERY_BOX,         // 91
    PREFAB_PERK_MACHINE,        // 92
    PREFAB_AI_REDIRECTION,      // 93
    PREFAB_KRUM_MACHINE,        // 94
    PREFAB_BOT_SPAWNER,         // 95
    PREFAB_MAP_ANCHOR,          // 96
    PREFAB_V7_COUNT,            // 97: the v7.1.4 table ended here
    PREFAB_SCRAPCRATE = PREFAB_V7_COUNT, // 97 (v10.0.2): a generic model prop like CRATE (scale 5)
    PREFAB_ENTITY,              // 98 (v10.0.2): an entity instance (Map.entity_instances), no map object
    PREFAB_COUNT,               // 99: every id of v10.0.2's table
    PREFAB_TERMINAL = PREFAB_BOT_PATH_NODE, // v3.1.1 / v7.1.4 name of 41
    PREFAB_SPECTATE_CAM = PREFAB_CAMERA,  // v3.1.1 name of 33
    PREFAB_BOT = PREFAB_AI_SPAWNER,       // v3.1.1 name of 55
} Prefab;

typedef struct {
    float boost;
    vec2 start;
    vec2 end;
} Ramp;

typedef struct {
    float move;
    int move_direction;

    int frames;
    float frame_time;
} TextureAnimation;

typedef enum {
    COLLISION_TYPE_BOX,
    COLLISION_TYPE_CYLINDER,
    COLLISION_TYPE_NONE,
} CollisionType;

// Krunker triggers (triggers.js events / actions): TriggerEvent, TriggerAction, TriggerData.
#include <triggers.h>

typedef struct {
    unsigned int prefab;
    unsigned int texture;

    unsigned char active:1;
    unsigned char is_border:1;
    unsigned char premium:1;
    unsigned char verified:1;
    unsigned char jump_pad:1;
    unsigned char team_zone:1;
    unsigned char score_zone:1;
    unsigned char teleporter:1;
    unsigned char checkpoint:1;
    unsigned char pickup:1;
    unsigned char flag:1;
    unsigned char trigger:1;
    unsigned char kill:1;
    unsigned char objective:1;
    unsigned char bomb_site:1;
    unsigned char spawnable:1;
    unsigned char crouch:1;
    unsigned char ladder:1;
    unsigned char wall_jumpable:1;
    unsigned char ambient_enabled:1;
    unsigned char fog_enabled:1;
    unsigned char penetrable:1; // map "pe": bullets pass through (wallbang), Krunker penetrable
    unsigned char no_shoot:1;   // Krunker noShoot: hitscan ignores the object
    unsigned char force_col:1;  // /forcecol made this non-colliding object collide (restored on OFF)
    unsigned char terminal:1;           // TERMINAL prefab: "ot" opens #terminalHolder (objects.js addTerminal, tRadius 7)
    unsigned char no_msg:1;             // terminal "nm": no "Open Terminal" interact prompt
    // Map interfaces (objects.js GameObject, shared/src/triggers.c): destructibles, gates,
    // deposit boxes, check points and teleporters.
    unsigned char can_destroy:1;        // "cdy" (or a health): triggers / actions may destroy it
    unsigned char start_destroyed:1;    // "sd" (canDestroy only): starts destroyed each round
    unsigned char destroyed:1;          // Krunker destroyed (in destObjs while it waits to respawn)
    unsigned char respawn_random:1;     // "rr": respawns after randInt(1, 5000) ms
    unsigned char gate:1;               // GATE prefab: "gte" toggles active (players.interact, Toggle Interface Gate)
    unsigned char closeable:1;          // gate "cl": an open gate can be closed again
    unsigned char start_closed:1;       // gate "stc" (closeable only)
    unsigned char bank:1;               // DEPOSIT_BOX prefab: "bnk" deposited score
    unsigned char single_use:1;         // check point "su": only once per life of the player list
    unsigned char stop_momentum:1;      // teleporter "m": telStopMo
    unsigned char force_dir:1;          // teleporter "fd": the receiver sets the view direction
    unsigned char no_vis:1;             // noVis ("v", every build; the client also sets it for meshes created hidden): not rendered, no spray, interface state never shows it

    // "d" (ladders, ramps, check points, teleporters 0..3; v7 BOOST_PAD 0..5: 0 up, 1 down, 2-5 sideways)
    unsigned char direction:3;

    // ---- WP-A: Krunker v7.1.4 per-object keys (doc/v7/map-logic.md §1.2-§1.5, shared/src/map.c)
    unsigned char asset_video:1;        // "at" contains a "v": "<id>/video" (else "<id>/texture")
    unsigned char force_trans:1;        // "ft" (fTrans)
    unsigned char sprite:1;             // "msp": rendered as a camera-facing sprite
    unsigned char no_ai_col:1;          // "nai" (noAICol); SERVER_NODE always
    unsigned char no_grapple:1;         // "gp"
    unsigned char no_impulse:1;         // "ip"
    unsigned char no_player_col:1;      // TRIGGER "pl" (noPCol)
    unsigned char complex_coll:1;       // "cpx": triangle collision (top-level "tris"); off for destructibles
    unsigned char no_minimap:1;         // "smm"
    unsigned char no_cast:1;            // "cst" (cast = cst == null)
    unsigned char no_receive:1;         // "rcv"
    unsigned char force_render:1;       // "fcr"
    unsigned char can_script:1;         // "sci" (any truthy value: the v7 serializer writes this.sci)
    unsigned char start_inactive:1;     // MYSTERY_BOX: built with active false (switchMysteryBox turns one on)
    unsigned char legacy_trigger:1;     // TRIGGER without "tas": its single-action keys were wrapped into tas[0]
    unsigned char v3_plane_rotation:1;  // PLANE / BILLBOARD / LIQUID / RUNE of a map flagged MAP_KEY_V3_PLANES: v3.1.1 rotation
    // v7 collision kinds (player.js collision chain, map-logic.md §1.6). The pass-through volumes
    // (no_pvp, force_pvp, redirection, server_node) are COLLISION_TYPE_NONE: non-solid for players,
    // bullets, AIs, bot navigation and spawn checks; whoever implements their behaviour tests the
    // object's AABB (position = bottom centre, scale = full size) directly.
    unsigned char rope_ladder:1;        // ROPE_LADDER: climbable from any side (collT cylinder, w = l = min(sx, sz) / 2)
    unsigned char no_pvp:1;             // NOPVP_ZONE: player.noPVP while inside
    unsigned char force_pvp:1;          // PVP_ZONE: player.forcePVP while inside
    unsigned char kr_zone:1;            // KR_ZONE: solid unless the player has >= kr_amount KR
    unsigned char partner_zone:1;       // PARTNER_ZONE: solid unless account.partnerApp >= 2
    unsigned char kpd_zone:1;           // KPD_ZONE: solid unless account.canFlag
    unsigned char redirection:1;        // AI_REDIRECTION: steers AIs along direction / bounce (boostDr)
    unsigned char server_node:1;        // SERVER_NODE: tagged spawn node (node_tag, spread), pass-through
    // v10.0.2 (doc/v10/map-format.md §2; v10 map dialect, the canDynamic prefabs CUBE / TRIGGER /
    // CUSTOM_ASSET): "dyn" = !!dyn. A dynamic object forces noAmb (no AO planes) and keeps its
    // GameObject (health, interface) even with noCol. TODO(v10 KS / entities): GAME.OBJECTS
    // position setters move dyn objects at run time (game.moveObject: re-quad, carry riders, "mo").
    unsigned char dyn:1;

    unsigned int team;
    unsigned int score_points;          // "sp": score zone points / gate price
    int interface_id;                   // "in" (0 none): Krunker interface, listed in map interfaces
    int gate_method;                    // gate "md": 0 score (paid), 1 score (kept), 2 kills
    int channel;                        // teleporter "ch" 0..100
    int node;                           // teleporter "n": 0 sending, 1 receiving, 2 both
    float tel_cooldown;                 // telCooldown (ms) of a node-2 receiver
    float health;                       // "h" / "hp": destructible health (0 none)
    float start_health;
    float respawn_time;                 // respawnT (ms) left / of the next respawn (0: stays destroyed)
    float respawn_time_start;           // "rt" * 1000 (>= 100 when set)
    int destroyed_by;                   // destroyedBy: player uid, -1 none
    int deposited;                      // deposit box: deposited score
    int withdraw_amount;                // "wd" (absent: 1000, 0 = all)
    int deposit_amount;                 // "dp" (absent: 1000, 0 = all)
    TriggerData *trig;                  // TRIGGER prefab: event / action / state (owned by the map)

    float spin;
    float bounce;

    // ---- WP-A: Krunker v7.1.4 per-object values (defaults / clamps of map.js + MapManager builders)
    const cJSON *raw;                   // the map record (owned by Map.raw_data): prefab-specific keys,
                                        // the AI / bot key bags (WP-F), sign / leaderboard text (WP-G)
    vec3 rotation;                      // "r" radians as authored: (r0, r1, r2); map_object_rotation()
                                        // gives the per-prefab v7 builder order
    float penetration;                  // "pe": limitMM(parseFloat(pe) || 0, 0, 1) wallbang fraction
    int tex_variant;                    // "tv" (parseInt || 0; the variant id appended to the texture src)
    int asset_texture;                  // "at": user asset id (digits of at), -1 none
    float tex_rotation;                 // "tro" degrees 0..360 (legacy "tr" radians -> toDeg)
    float tex_scale_mult;               // "tsm"
    int tex_encoding;                   // "ten" 0..2 (config.encodingTypes)
    int frames;                         // "fct" 0..100 sprite-sheet frames
    float frame_time;                   // "fs" 0..10 -> ms per frame (round(1) * 100)
    int render_side;                    // "rs" 0 front / 1 back / 2 double, -1 = prefab default
    float alpha_test;                   // "alt" 0..1
    float darken;                       // "da"
    int step_sound;                     // "sat" 0..4 (config.stepSounds)
    float spawn_chance;                 // "rsc" 0..100 (%) for a start-destroyed object at round reset
    float hitbox_h, hitbox_w;           // "hbh" / "hbw" 0.1..10 (1): CUSTOM_ASSET / TRIGGER box, object_box_size()
    int spin_axis;                      // "spa" 0 y / 1 x
    float anim_speed;                   // "as" (parseFloat || 0)
    const char *anim_start;             // "ast" <= 32 chars of the trigger text charset (in raw), NULL none
    const char *interact_msg;           // "inm" <= 70 chars (in raw), NULL none
    int objective_uid;                  // OBJECTIVE "obi" 1..10 -> uid obi - 1, -1 = auto (file order)
    int obj_link;                       // CAMERA "obl" (v3 SPECTATE_CAM "obi" is not read: links to 0)
    int camera_type;                    // CAMERA "camt" 0 spectate / 1 player
    int node_id;                        // PATH_NODE "nid" (-1 none)
    int kr_amount;                      // KR_ZONE "krs" 0..1e7
    int anchor_type;                    // MAP_ANCHOR "ant" 0 corner / 1 center
    int zombie_perk;                    // PERK_MACHINE "zp" 0..6
    int spray_id;                       // SPRAY "spt" (2482)
    float spread;                       // SERVER_NODE "ssp" 0..50
    const char *node_tag;               // SERVER_NODE "nt" <= 70 chars (in raw), NULL none
    // v10.0.2 CUSTOM_ASSET / asset TRIGGER "mp" (mapProps, in raw): the companion maps' settings,
    // {<n e s a m r l p b>: {ch: uv channel, fy: flipY, i: intensity}} (applyAssetCompanionMaps). NULL none.
    const cJSON *mp;

    // ---- map-logic WP-B (shared/src/world.c, build.c)
    unsigned char tel_checkpoint:1;     // TELEPORTER "tcp" (telCheckPoint): to the player's check point
    unsigned char dynamic:1;            // a player build (game.buildObj): removed by removeBuilds
    int build_index;                    // buildObjs.js index of a build (typeInd; dynamic objects only)
    // KrunkNative client prediction (build.c world_predict_build): ms this client's own build, shown
    // ahead of the server's "bo", stays before it gives up on it (the server refused); 0 = a real build
    float predicted_ms;
    // KrunkNative Object Manager (shared/src/object_manager.c): spawned at run time from a menu
    // record (raw points into the game's ObjectManager records); removed with the builds at round start
    unsigned char spawned:1;
    // a map object's stand-in while the Object Manager has it edited (a replacement built from the
    // edited record, or the deleted placeholder); the map's own Object waits detached (object_manager.c)
    unsigned char map_edit:1;
    int spawn_owner;                    // uid of the player who spawned it (spawned objects only)
    // ---- end WP-B

    CollisionType collision_type;

    vec3 position;
    vec3 scale;

    TextureAnimation *tex_anim;
    Ramp *ramp;
    void *mesh;
} Object;

// Full size of a map object's v7 GameObject box (bottom centre `position`). `scale` is the
// authored "s", the render size. Only addAsset (CUSTOM_ASSET) and addTrigger (TRIGGER, plain or
// asset) apply the hit box multipliers (v7 MapManager.js:2846-2849 / 2045-2048):
// w = l = s/2 * hbw, h = s/2 * hbh, y = p.y + h, so the bottom stays at p.y. Every other builder
// (addMesh model props, addCylinder, the machines...) takes the full s, which v7 also does. A Lumber
// tree (CUSTOM_ASSET hbw = hbh = 0.1) collides as its trunk, not as its 64 x 154 x 65 canopy.
// Use this, not `scale`, wherever the code stands in for the GameObject: movement, shots, AIs, bots.
static inline vec3 object_box_size(const Object *object) {
    const int hit_box = object->prefab == PREFAB_CUSTOM_ASSET || object->prefab == PREFAB_TRIGGER;
    const float hbw = hit_box && object->hitbox_w > 0.0f ? object->hitbox_w : 1.0f;
    const float hbh = hit_box && object->hitbox_h > 0.0f ? object->hitbox_h : 1.0f;
    return (vec3) {object->scale.x * hbw, object->scale.y * hbh, object->scale.z * hbw};
}

typedef struct {
    vec3 position;
    int team;
    int direction;
    int comp;
    int team_only;  // [6] v7 teamOnly (!!spawn[6])
    float spread;   // [7] v7 spread: limitMM(parseFloat(spawn[7]) || 0, 0, 50)
} Spawn;

typedef enum {
    CAMERA_TYPE_NORMAL,
    CAMERA_TYPE_TOP_DOWN,
    CAMERA_TYPE_PLATFORMER_X,
    CAMERA_TYPE_PLATFORMER_Z,
} CameraType;

typedef enum {
    MODEL_TYPE_NORMAL,
    MODEL_TYPE_SPRITE,
} ModelType;

typedef struct {
    CameraType cam_type;
    unsigned char cam_rotation:1;
    vec3 cam_offset;

    ModelType model;

    vec3 speed;

    float ladder_accel;
    float air_accel;
    float slide_time;
    float slide_accel;
    float jump_cooldown;

    unsigned char infinite_jump:1;
} MapConfig;

// ---- WP-A: Krunker v7.1.4 top-level map keys (doc/v7/map-logic.md §1.1, §1.8), parsed by map.c.
#define MAP_COLOR_SIZE 8 // "#rrggbb"

// The lighting / sky / fog / output environment of the map (render.js initEnvironment), also the
// data of an ENVIRONMENT_ZONE (MapManager.addEnvironmentZone, its "ambientC" / "fogC" keys).
typedef struct {
    unsigned char has_ambient:1;  // map: a missing "ambient" means no ambient light (zones: always)
    unsigned char has_light:1;    // map: a missing "light" means no sun
    unsigned char sky_dome:1;
    unsigned char sky_dome_tex:1;
    unsigned char correct_lights:1;
    char ambient[MAP_COLOR_SIZE], light[MAP_COLOR_SIZE], sky[MAP_COLOR_SIZE], fog[MAP_COLOR_SIZE];
    float ambient_i, light_i, light_d;  // 1 / 1.3 / 500
    float sun_ang_x, sun_ang_y;         // 90 / 54
    int shadow_r, shadow_d;             // 1024 / 1200
    float fog_d;                        // 2000
    char sky_dome_col[3][MAP_COLOR_SIZE], sky_dome_emis[MAP_COLOR_SIZE];
    int sky_dome_tex_asset;             // "skyDomeTexA" asset id, 0 none
    int sky_dome_emis_tex;              // "skyDomeEmisTex" asset id, 0 none
    float sky_dome_mov_t, sky_dome_mov_d;
    int amb_ind;                        // soundscape (config.soundScapes value), 1
    int amb_ind_custom;                 // "ambIndC" asset id, 0 none
    int tone_mapping;                   // 0..4
    float tone_mapping_exposure;        // 1
    int output_encoding;                // 0..2
    float shadow_nb;                    // v10 "shadowNB": the sun's shadow normalBias 0..5 (0; v7 maps / zones: 0)
} MapEnvironment;

typedef struct {
    vec3 position;   // bottom centre (p)
    vec3 size;       // full size (s)
    MapEnvironment env;
} MapEnvironmentZone;

typedef struct {     // Terrain.CONFIG_PROPS (built only when seed, width and height are all truthy)
    unsigned char enabled:1;
    char seed[64], tree_seed[64];       // "terrainSeed" / "terrainTSeed" (numbers kept as text)
    float width, height, mountain_mlt;  // 3000 / 3000 / 1
    int ease;                           // 12
    char mountain_col[MAP_COLOR_SIZE], snow_col[MAP_COLOR_SIZE], grass_col[MAP_COLOR_SIZE],
         dirt_col[MAP_COLOR_SIZE], sand_col[MAP_COLOR_SIZE];
} MapTerrainConfig;

typedef struct {     // Zone.CONFIG_PROPS (built only when zone && zoneSize && zoneSpeed)
    unsigned char enabled:1;
    unsigned char random:1;             // "zoneRnd"
    float size, speed, height, wait;    // 500 / 1 / 1000 / 5 (s)
    int stages;                         // 0..10
    char col[3][MAP_COLOR_SIZE];
} MapZoneConfig;

typedef struct {     // Ocean.js (client only)
    unsigned char enabled:1;
    char col[MAP_COLOR_SIZE];           // #001e0f
    float opacity, distortion, speed, y_offset, width, length; // .9 / 3.7 / 1 / -1 / 1000 / 1000
    int reflection_res;                 // 3
} MapOceanConfig;

typedef struct {
    int nid;          // PATH_NODE "nid"
    size_t object;    // index into Map.objects
} MapPathNode;

// "botPaths" [{p: [[x, y, z], ...], r: reversible}] (map.js: parseInt(x) || 0 per coordinate, entries
// without an array p are skipped). v10 bots with usePathing walk them (shared/src/bots.c).
typedef struct {
    vec3 *nodes;
    size_t node_count;
    unsigned char reversible:1;
} MapBotPath;

// ---- v10.0.2 Entities (doc/v10/map-format.md §4): the map data only. TODO(entities package): the
// manager (spawnPending, onEntityInit / simulateEntity, bodies, packets e+ e- es ee ea+ ea-) reads
// Map.archetypes / Map.entity_instances and Map.shared_script; nothing runs them yet.
#define MAP_ARCHETYPE_LIMIT 32          // config.archetypeLimit
#define MAP_ARCHETYPE_OBJECT_LIMIT 40   // config.archetypeObjectLimit
#define MAP_ENTITY_LIMIT 64             // config.entityLimit (map.js keeps the first 64 instances)
#define MAP_ENTITY_TAG_MAX 32
#define MAP_ARCHETYPE_NAME_MAX 24

typedef struct {     // a soft zone part (soft: 1): the rider glue box, not a body
    vec3 centre, half;
} MapArchetypeSoftZone;

typedef struct {     // entities.loadMapData archetype record
    int id;                             // 1..9999
    char name[MAP_ARCHETYPE_NAME_MAX + 1]; // name || "archetype_<id>"
    int coll;                           // 0 none, 1 solid, 2 bullets only (noPCol)
    float hp;
    const cJSON *data;                  // "data" object (raw), NULL = {}
    const cJSON *objects;               // "objects" parts (raw array: i p s c o aid n soft as ...)
    int object_count;
    float foot_cx, foot_cz, foot_hx, foot_hz, foot_h; // footprint of the solid parts (soft ones excluded)
    MapArchetypeSoftZone *soft_zones;
    int soft_zone_count;
} MapArchetype;

typedef struct {     // an ENTITY (98) object: manager.entityInstances
    int eid;                            // parseInt(eid)
    int archetype;                      // parseInt(ar)
    vec3 position;                      // p || [0, 0, 0]
    vec3 rotation;                      // r || [0, 0, 0] (only r[1], the yaw, is applied)
    char tag[MAP_ENTITY_TAG_MAX + 1];   // (tag + "").slice(0, 32), "" none
    unsigned char has_tag:1;
    unsigned char auto_spawn:1;         // as !== 0 (spawnPending skips as: 0)
    const cJSON *spawn_params;          // "sp" when an object (raw), NULL none
    const cJSON *raw;
} MapEntityInstance;

typedef struct {
    const cJSON *raw_data;

    MapConfig config;
    float death_y;

    size_t spawn_count;
    Spawn **spawns;

    size_t object_count;
    Object **objects;

    struct {
        vec3 min;
        vec3 max;
    } dimensions;

    vec3 camera_position;

    // ---- WP-A: v7 top-level keys (absent keys take the v7 defaults; unknown keys stay in raw_data)
    const char *name;                   // "name" (raw_data string), NULL none
    const char *welcome;                // "welMsg" when it passes the v7 charset check, else NULL
    unsigned char camera_static:1;      // camPos[3]
    unsigned char no_ambient:1;         // "nAmb"
    unsigned char old_ao:1;             // "oldAO"
    unsigned char has_lightmaps:1;      // "hasLM"
    vec3 camera_rotation;               // camPos[4..6] (rx, ry, rz): rotateCam(p5, p4, p6)
    int minimap_asset;                  // "mmId" digits, 0 none
    MapEnvironment env;
    MapTerrainConfig terrain;
    MapZoneConfig zone;
    MapOceanConfig ocean;

    size_t mode_count;                  // top-level "modes" (official map startup / vote modes)
    int *modes;

    float *tris;                        // "tris": 9 floats per world-space triangle (cpx collision)
    size_t tri_count;

    // raw references into raw_data (owned by it): parsed by the owning packages
    const cJSON *game_config;           // "config" {modes, classes, settings, weps, cls, prks} (WP-E)
    const cJSON *gui;                   // "gui" custom HUD elements (WP-C / WP-G)
    const cJSON *scripts;               // "scripts" {client, server} base64 KrunkScript
    const cJSON *bot_paths;             // "botPaths" raw (the v7 runtime ignores it; kept for round-trip)
    MapBotPath *bot_path_list;          // botPaths parsed (v10 bots with usePathing walk them)
    size_t bot_path_count;
    const cJSON *shared_script;         // v10 "scripts.shared" (the entity simulation script, run on both sides), NULL none
    MapArchetype *archetypes;           // v10 "archetypes" (map dialect v10 only)
    size_t archetype_count;
    MapEntityInstance *entity_instances; // v10 ENTITY objects, <= MAP_ENTITY_LIMIT
    size_t entity_instance_count;
    const cJSON *editor_player;         // "player" {pos, rot} (editor test spawn)

    // object lists (indices into objects) and pending spawners (raw records, no map object)
    MapPathNode *path_nodes;            // PATH_NODE, in file order (manager.pathNodes[nid])
    size_t path_node_count;
    size_t *server_nodes, server_node_count;   // SERVER_NODE
    size_t *map_anchors, map_anchor_count;     // MAP_ANCHOR
    size_t *dom_zones, dom_zone_count;         // DOMINATION_ZONE (uid = position in this list)
    size_t *objectives, objective_count;       // OBJECTIVE, in file order
    MapEnvironmentZone *env_zones;             // ENVIRONMENT_ZONE
    size_t env_zone_count;
    const cJSON **ai_spawners;                 // AI_SPAWNER records (map.js addAI: pending AIs, WP-F)
    size_t ai_spawner_count;
    const cJSON **bot_spawners;                // BOT_SPAWNER records (addBot: pending map bots, WP-F)
    size_t bot_spawner_count;

    // ---- map-logic WP-B (shared/include/terrain.h, world.h): runtime world data of the map
    struct Terrain_t *world_terrain;           // map.terrain, built lazily by map_terrain(), freed by map_fini
    unsigned char world_terrain_failed:1;      // generation ran out of memory: don't retry
    // ---- end WP-B
} Map;

extern const MapConfig g_default_map_config;
extern const cJSON *g_maps[32];           // the official maps (MAP_CATALOG_OFFICIAL_COUNT, v7.1.4 mapIndex)
extern const int g_rotation_maps[24];     // v7.1.4 data/map.js rotationMaps

void load_default_maps();
const cJSON **map_list_with_custom(const cJSON*, size_t*);

Map *map_init(const cJSON*);
// build_meshes 0: no meshes even in the client build (the client's in-process sandbox server,
// server/src/sandbox.c, plays the map exactly like server_bin: game_init passes !game->server).
Map *map_init_ex(const cJSON*, int build_meshes);
// The prefab id of a raw map object, as v7 map.js generate() resolves it:
// prefabs.prefabs[i || id || 0] || prefabs.prefabs[0], so ids outside 0..96 and prefab *names*
// become CUBE (numeric strings index like numbers). Validation rejects unknown ids first
// (map_validate.h); the name lookup is prefab_id_from_name (prefabs.h).
int map_raw_object_prefab(const cJSON *raw_obj);
// 1 when map_init makes a map->objects entry for this raw object: it has p and s arrays and its
// prefab builds a map object (not TERMINAL, AI_SPAWNER, BOT_SPAWNER or EVENT; prefab_info()
// PREFAB_R_NO_OBJECT). Code pairing raw_data objects with map->objects in order must skip
// exactly these.
int map_raw_object_kept(const cJSON *raw_obj);

// The v7 builder rotation of an object (map.js generate -> MapManager builders), as a row-major
// 3x3 matrix applied to the prefab's unit geometry (three.js conventions, y up):
//   CUBE / models / cylinders / triggers ...: yR = r0, xR = r1, zR = r2, Euler "XYZ"
//   BOOST_PAD: xR = r0, yR = r1 (v3.1.1 had yR = r0, xR = r1)
//   PLANE / BILLBOARD / SIGN / LIQUID / SPRITE / LIGHT_BAR / SPRAY / LEADERBOARD (addFixedPlane):
//     Rx(r0) * Ry(r1) * Rz(r2) * Rx(-PI/2) of the XY unit plane (v3: Euler YXZ (r2, r0 - PI/2, r1))
//   RECTAREA_LIGHT: (r2, r0, r1)
// Old v3 maps go through the same table: rotated v3 planes look different, exactly as in v7.
// KrunkNative difference: the catalogue's v3.1.1 / v1.5.2 maps ("311_*", "152_*", which v10 does
// not ship) carry MAP_KEY_V3_PLANES (map_catalog_json), and their PLANE / BILLBOARD / LIQUID / RUNE
// objects (Object.v3_plane_rotation) keep v3.1.1's addPlane rotation, Ry(r2) * Rx(r0 - PI/2) * Rz(r1)
// of the XY unit plane: with v7's a billboard turned by r2 = +-PI/2 faced the wall, its poster
// mirrored (Krunker's own v7 re-saves of those maps negate r2 for that reason).
#define MAP_KEY_V3_PLANES "knV3Planes"
void map_object_rotation(const Object *object, float out[9]);
// A custom texture id (skyDomeTexA, skyDomeEmisTex, mmId) as v10.0.2 accepts it: the digits of a
// number or of a string made only of digits and the letters "tlnesacdgxufqmrpbhv" (v7.1.4 lacked
// "mrpbhv"), else 0.
int map_texture_id(const cJSON *item);
// The v10 archetype with that id, NULL none.
const MapArchetype *map_archetype(const Map *map, int id);
void map_reset(Map*);
void map_fini(Map*); // NOTE: ensure no meshes from the map are part of the scene, they are free()'d!
// KrunkNative Object Manager: an Object for one raw map object record (`raw_obj` must outlive it:
// object->raw points into it), parsed exactly like map_init does with `map`'s ambient setting;
// not added to the map (no side lists, no dimensions). NULL: not a kept object / out of memory.
Object *map_object_from_json(const Map *map, const cJSON *raw_obj, int build_meshes);
// Frees one object made by map_init / map_object_from_json (client: its mesh leaves the scene first).
void map_object_free(Object *object);

// ---- WP-A: v7.1.4 per-class movement knobs (config.js classConfig "Movement", player.js,
// movement.js). No official v7 class sets one; custom maps can (config.cls). A field left 0 reads
// as the v7 default named in its comment, so the class table lists only changes. v7 defaults
// reproduce the v3.1.1 constants. Accel/decel are in v7 units (per ms; grA/arA per 100 ms).
typedef struct ClassMovement {
    int scheme;             // movSch: game.movement index (0 Default; the others are not ported yet)
    float speed_x;          // speedX, 0 = 1: x step multiplier in the integration
    float speed_y;          // speedY, 0 = 1
    float speed_z;          // speedZ, 0 = 1
    float move_dir_offset;  // movDrP (x PI) added to the movement direction, default 0
    float turn_rate;        // trnSp, 0 = 1: below 1 the look turns at most trnSp * 0.01 rad/ms
    float ground_accel;     // grA, 0 = 0.042 (/100 per ms)
    float ground_decel;     // grDec, 0 = 0.99 per ms
    float air_accel;        // arA, 0 = 0.0047 (/100 per ms)
    float air_decel;        // airDec, 0 = 0.9996 per ms
    float crouch_speed;     // crchSpd, 0 = 0.3: crouched acceleration multiplier
    float ladder_accel;     // ldA, 0 = 1
    float wall_jump_decel;  // wallJD, 0 = 0.3: gravity multiplier while wall sliding
    float wall_jump_vel;    // wallJV, 0 = 0.03: horizontal wall-jump push
    float step_freq;        // stpFr, 0 = 1: step period multiplier (feel)
    unsigned char no_crouch:1;        // !canCrch
    unsigned char disable_jump:1;     // disJump
    unsigned char infinite_jump:1;    // infJ: jump while airborne
    unsigned char override_jump:1;    // ovrJump: the jump sets velocity.y instead of adding to it
    unsigned char free_slide:1;       // frSlid: slides follow the held movement key
    unsigned char player_collision:1; // pColl: players are solid boxes (not ported yet)
    unsigned char lock_pitch:1;       // lckCmYR: input pitch forced to 0
} ClassMovement;

// ---- WP-E: the v7.1.4 classConfig keys (doc/v7/map-logic.md §4.3.3) that neither ClassConfig nor
// its movement block holds. A map's or a host's cls[id] sets them (shared/src/game_config.c).
// `set` = 0 means untouched: read them through class_overrides() (game_config.h), which then
// returns the v7 defaults. ----
typedef struct {
    unsigned char set;            // 1 once a cls[id] override wrote this class
    char name[25];                // name (len 24): ClassConfig.name points here when overridden
    int cam_type;                 // camType: 0 FPS, 1 3rd person, 2 platformer X, 3 platformer Z, 4 orthographic
    float cam_offset[3];          // camOffX/Y/Z 0..500
    float ortho_f;                // orthoF 1..1000 (50)
    float lock_cam_pitch;         // lockCamP 0..1
    float y_cam_limit;            // yCmLim 0..1
    unsigned char cam_rotation;   // camRot (1)
    unsigned char fp_lock;        // fPLock
    int model;                    // mdl: 0 default, 1 sprite, 2 model
    char asset_id[13];            // aID (len 12)
    unsigned char model_fp;       // mdlFP (1)
    float anim_speed;             // animS 0.1..10 (1)
    float model_scale;            // mdlS 0.1..100 (1)
    float model_pos_scale;        // mdPS 0.01..10 (1)
    float model_pos_y;            // mdPY -50..50
    float model_dir;              // mdlDr 0..360 (degrees)
    float model_y_offset;         // mdlYO -50..50
    float speed_length;           // spdLnTh 1..500 (500)
    float step_sd;                // stpSD 0..1 (step sound dynamic)
    float vehicle_drag;           // vhdr 0.001..0.999 (.97)
    float vehicle_air_drag;       // vhaDr 0.001..0.999 (.94)
    float vehicle_tilt;           // vhtL -100..100 (-2.7)
    float vehicle_turn_h;         // vhtrsh 0..0.01 (.0025)
    float vehicle_turn_v;         // vhtrsv 0..1 (0)
    float vehicle_move_power;     // vhmP 0..1 (.12)
    float vehicle_power_f;        // vhpF 0..0.1 (.0012)
    float vehicle_move_rot;       // vhmR 0..1 (.0375)
    float vehicle_rot_f;          // vhrF 0..0.1 (.0005)
    float jump_cooldown;          // jpC 0.1..3 (1)
    unsigned char can_shoot;      // canShoot (1)
    unsigned char shoot_to_cam;   // shtToCam
    float shoot_drag;             // shtDrg 0.01..1 (1)
    float shot_x, shot_y;         // shotX / shotY 0..360
    unsigned char hip_fire;       // hipF (1)
    unsigned char crouch_shoot;   // croSh (1)
    unsigned char air_shoot;      // airSh (1)
    unsigned char can_melee;      // canMelee (1)
    char crosshair_id[13];        // cID (len 12)
    char crosshair_color_id[13];  // ccID (len 12)
    float regen_delay;            // regenDelay (s; v7 ms 0..10000), 0 = config.regenDelay
} ClassOverrides;
// ---- end WP-E ClassOverrides ----

// Krunker v7.1.4 class (src/data/classes.js, shared/src/classes.c). Fields marked "0 = 1" read
// as 1 when unset, so custom/mode classes only set what they change.
typedef struct {
    const char *name;
    int icon_index;
    const char **texts;   // txts: selectClass speech lines
    int text_count;
    const int *loadout;
    unsigned char secondary:1;
    unsigned char wall_jump:1;
    unsigned char hide:1;       // hide: not in the class picker (Deagler, Survivor)
    unsigned char no_mobile:1;  // noMobile
    unsigned char no_sleeve:1;  // noSleeve: no arm sleeves on the viewmodel
    unsigned char leap:1;       // leap: melee lunge
    unsigned char back_slide:1; // bkSlid
    int kn_secondary;           // KrunkNative: weapon id + 1 the class always carries as its secondary (0 = the Select Secondary pick)
    int colors[6];
    int health;
    int health_segments;
    float regen;
    float speed;
    int wall_jump_max;     // wallJM: wall jumps per airborne period (0 with wall_jump = none)
    float throw_charge;    // throwChrg (s): knife-throw charge time (0 = config.throwCharge 0.9 s)
    float slide_time_mlt;  // slT, 0 = 1
    float slide_accel_mlt; // slA, 0 = 1
    float turn_speed;      // trnSpd, 0 = 1
    float melee_rate;      // melRate, 0 = 1
    int min_rec;           // minRec: account level gate (req); 0 = open
    int face;              // face: class face store item
    float hitbox_pad_w;    // hbS: added to the game's hitBoxPad sideways (player.hitboxPadW)
    float hitbox_pad_h;    // hbH: added to the game's hitBoxPad on top (player.hitboxPadH)
    float hurtbox_scale;   // v10.0.2 cls hurtboxScale (1..2, 0 = 1): the posed hit boxes' scale (shared/src/hitboxes.c)
    ClassMovement movement; // WP-A
    ClassOverrides ext;    // WP-E: the other cls[id] keys (camera, model, vehicle, shooting), read via class_overrides()
} ClassConfig;

#define CLASS_COUNT 16 // Krunker v10.0.2's classConfig: what the menus, challenges and class stats list
#define CLASS_ID_SURVIVOR 14
#define CLASS_ID_INFILTRATOR 15
// KrunkNative-only classes sit after Krunker's (doc/v7/decisions.md "Nettspend class"): in the
// ruleset's class table (Game.classes, rules_class_count() = CLASS_TABLE_COUNT), so spawns,
// packets and the picker take them, but outside CLASS_COUNT.
#define CLASS_ID_NETTSPEND 16
#define CLASS_TABLE_COUNT 17
// The class tables are the rulesets' (shared/include/ruleset.h; rules_classes() / Game.classes).

// ---- WP-B: Krunker v7.1.4 status effects, perks and killstreaks (shared/src/effects.c) ----

// effects.js: stackable effects; player.effects[i] is the stack count and execute() writes the
// player.attributes value below. Timed stacks wear off one at a time (attributeTimers).
typedef enum {
    EFFECT_BOOSTED,         // speed: +10% move speed per stack
    EFFECT_SLEIGHT_OF_HAND, // reload: -10% reload time per stack
    EFFECT_RAPID_FIRE,      // fRate: -5% fire interval per stack
    EFFECT_SIPHON,          // siphon: the killer heals 10 HP per stack
    EFFECT_ROOTED,          // rooted: -25% move speed per stack, 25 s
    EFFECT_DIZZY,           // drunk: camera sway, 35 s
    EFFECT_POISONED,        // enraged: 10 damage every 4 s, 40 s
    EFFECT_JUMP_BOOST,      // jump: +10% jump per stack
    EFFECT_ZAPPED,          // zap: 1 damage per stack every 1 s to zappedBy, 5 s
    EFFECT_STRENGTH,        // dmg: +10% damage per stack
    EFFECT_COUNT
} EffectId;

#define EFFECT_STACK_MAX 10 // the largest effects.js `max`

typedef struct {
    const char *name;
    const char *key;   // the player.attributes key
    float timer;       // seconds per stack, 0 = until death
    float def;         // the attribute with no stacks
    int max;           // stack cap
    float mlt;         // per stack
    float hp;          // siphon / poison / zap: HP per tick (0 = none)
    int medal;         // mid: the "am" medal icon
} EffectConfig;

extern const EffectConfig g_effects[EFFECT_COUNT];

// player.attributes (effects.js keys). effects_reset() sets every value to its `def`.
typedef struct {
    float speed, reload, fire_rate, siphon, rooted, drunk, enraged, jump, zap, damage;
} PlayerAttributes;

// Perks (player.perks: PERK_MACHINE map objects and the Perk Bottle pickup, players.givePerk).
enum {
    PERK_HEALTH = 0,        // maxHealth / health 250
    PERK_RAPID_FIRE = 1,    // fire interval x0.66
    PERK_FAST_RELOAD = 2,   // reload time x0.5
    PERK_SPEED = 3,         // move speed x1.15
    PERK_SLIDE = 4,         // may slide in noSliding modes
    PERK_SLIDE_EXPLODE = 5, // may slide; sliding blasts AIs every 750 ms
    PERK_WEAPON_SLOT = 6,   // weaponLimit 2 -> 3
};
#define PLAYER_PERK_MAX 8
#define GAME_MAX_ZOMBIE_PERKS 4 // config.maxZombiePerks

// streaks.js: one pick per slot (streakIndex1..3, ENT streakIndexes), earned at realKillStreak == kills.
typedef enum {
    STREAK_NUKE,
    STREAK_AIRDROP,
    STREAK_REFILL,      // devOnly
    STREAK_BOMBARDMENT, // devOnly
    STREAK_SLIMER,
    STREAK_JUGGERNAUT,
    STREAK_WAR_MACHINE,
    STREAK_COUNT
} StreakId;

#define STREAK_SLOTS 3

typedef struct {
    const char *name;
    int slot;              // the loadout slot it can be picked in
    int kills;             // realKillStreak needed
    unsigned char req_account:1; // reqAccount: needs player.account, which every v7.1.4 server player has (effects.c streak_add)
    unsigned char no_customs:1;  // noCustoms: not in custom games
    unsigned char dev_only:1;    // devOnly
} StreakConfig;

extern const StreakConfig g_streaks[STREAK_COUNT];
// index.js streakIndex1..3 defaults (f1795): slots 0 and 1 none, slot 2 the Nuke.
extern const int g_default_streak_indexes[STREAK_SLOTS];
// ---- end WP-B effects/perks/streaks types ----

typedef enum {
    TEAM_OPTIONS_NONE,
    TEAM_OPTIONS_HIDERS,
    TEAM_OPTIONS_SEEKERS,
    TEAM_OPTIONS_SURVIVORS,
    TEAM_OPTIONS_INFECTED,
    TEAM_OPTIONS_FOLLOWERS,
    TEAM_OPTIONS_SIMON,
    TEAM_OPTIONS_PROP,
    TEAM_OPTIONS_HEROES,
    TEAM_OPTIONS_BOSS,
    TEAM_OPTIONS_LIVING,
    TEAM_OPTIONS_STALKER,
    TEAM_OPTIONS_INNOCENT,
    TEAM_OPTIONS_TRAITOR
} TeamOptions;

// v7.1.4 player.grapple (players.js tryGrapple, player.js procInputs): the hook point on the map
// object the Grappler's ray hit (rounded to 0.1) and the rope length, which reels in by 0.04
// units/ms down to 35 while the player is held on a sphere of that radius round the point.
typedef struct {
    unsigned char active;
    vec3 point;
    float range;
} PlayerGrapple;

typedef struct {
    int seq;
    int move_dir;

    float delta;
    float x_dir;
    float y_dir;
    // input[12] (index.js b9): 1 - CONTROLS.speedLmt.round(1), the controller's Gradual
    // Speed (gradSpeed); procInputs scales the acceleration by limitMM(1 - it, 0, 1).
    // 0 = full speed.
    float slowdown;

    unsigned char shoot:1;
    unsigned char scope:1;
    unsigned char jump:1;
    unsigned char crouch:1;
    unsigned char reload:1;
    // input[11] (CONTROLS.aba6): 1 swapSecondary, 2 swapMelee, 3 primary (slot 0), 4 swapEquipment.
    unsigned char swap:3;
    // KrunkNative Object Manager: 0 none, n = equip loadout slot n - 1 (the number keys with extra
    // loadout weapons). Sent in NetInputPacket.scroll as n + 1 (>= 2); see net_packet_from_input.
    unsigned char select_slot;
    // input[10]: the wheel, wDG6 * scrollDirM (-1 / 0 / 1), only when no swap key was
    // pressed; procInputs steps the weapon index with reload/pY2 (player_scroll_weapon).
    signed char scroll;
    // input.velObj (players.js shoot, client only, never sent): the physPow knockback this
    // frame's shot gave the local player; a reconciliation replay (recon) applies it again,
    // since replays skip shooting (index.js reconcilePlayer -> procInputs(recon = true)).
    unsigned char has_vel_obj;
    vec3 vel_obj;
    // input.grapObj (players.js tryGrapple, client only, never sent): the grapple this frame's
    // Grappler shot hooked; a replay sets player.grapple from it (procInputs recon branch).
    unsigned char has_grap_obj;
    PlayerGrapple grap_obj;
    // Server only, never sent: when this frame reached the server on the Game.state_now clock,
    // pulled back by the deltas of the frames after it in the same "q" batch (the client
    // generated it that much earlier). The anchor of anchored lag compensation
    // (player_get_state_const); 0 = unknown (offline, tests): the shot counts as arriving now.
    double arrival;
    // Server only, never sent: exact anchored lag compensation (LagCompClock, player.c).
    // received: when the frame's packet reached the server (state_now clock, not pulled back);
    // client_time: the client's own clock for this frame (the running sum of the deltas of its
    // timeline, s), -1 = none; clock_epoch: the LagCompClock timeline it belongs to.
    double received;
    double client_time;
    unsigned short clock_epoch;
    // ---- map-logic WP-B: v7.1.4 input[13] (CONTROLS "resetLast" 2 / "reset" 1: the Parkour
    // resetToLast / resetToStart, players.js) and input[14] (the "interact" key held: airdrop
    // claims, hold-interact prompts). Sent in NetInputPacket.flags (NET_INPUT_RESET_* / _INTERACT).
    unsigned char reset:2;
    unsigned char interact_held:1;
    // ---- end WP-B
} Input;

// Exact anchored lag compensation (server.cfg lag_comp_anchor 1, shared/src/player.c): the
// client's input timeline (the summed frame deltas) mapped onto the server clock with a
// min-filtered offset, per connection, rebuilt at every new life / seq restart / lost frame.
#define LAG_COMP_CLOCK_SAMPLES 256
typedef struct {
    double client_time;        // running sum of the accepted frames' (clamped) deltas, s
    int last_seq;              // seq of the last frame fed, -1 none
    unsigned short epoch;      // bumps at every timeline break (Input.clock_epoch must match)
    int head, count;           // packet samples, newest at head
    double sample_received[LAG_COMP_CLOCK_SAMPLES];
    double sample_skew[LAG_COMP_CLOCK_SAMPLES]; // received - client_time (the least of one packet's frames)
    double begin;              // received of the timeline's first sample
    double baseline;           // the least skew of the calibration (anomaly reference)
    double offset;             // the least skew of the window: client_time -> server time of a least-delayed packet
    double excess;             // the window's median skew - offset: the typical uplink delay above the least
    unsigned char calibrated:1;
    unsigned char anomaly:1;      // this life's client clock ran faster / slower than real time: arrival fallback
    unsigned char anomaly_new:1;  // set with anomaly, cleared by whoever logs it (the server)
} LagCompClock;
const char *lag_comp_anchor_name(int anchor);                          // "v3.1.1" / "exact" / "arrival"
int lag_comp_anchor_parse(const char *text);                          // 0/1/2, on/off/true/false, names; -1 = unknown
void lag_comp_clock_reset(LagCompClock*);                              // a new life: empty, no anomaly
void lag_comp_clock_frame(LagCompClock*, Input*, float raw_delta);   // an accepted frame (received > 0): sets client_time / clock_epoch
double lag_comp_clock_shot_time(const LagCompClock*, const Input*, double *excess); // server time the frame was sent, 0 = fall back

struct Game_t;

#define PLAYER_NAME_SIZE 32
#define PLAYER_DAMAGE_SLOTS 8
#define PLAYER_MEDAL_SLOTS 56 // overlay.medalsList (PROGRESSION_MEDAL_COUNT, shared/include/progression.h)
#define PLAYER_CHECKPOINT_LIST_MAX 256 // single-use check points one player can remember
#define PLAYER_SCORE_OBJS_MAX 256      // map-logic WP-B: score zones one player can have taken (scoreObjs)
#define PLAYER_COLLIDING_OBJS_MAX 64   // map-logic WP-B: enter / exit trigger objects a player is inside (collidingObjs)
#define PLAYER_PICKED_MAX 64           // WP-C: lastPicked slots (weapon ids 0..63)
#define PLAYER_RESERVE_MAX 8           // map-logic WP-H: finiteAmmo reserves (one per loadout weapon)
#define PLAYER_EXTRA_WEAPONS_MAX 24    // KrunkNative Object Manager: extra loadout weapons per player

typedef struct {
    int uid;      // dealer uid, -1 = free slot
    float value;  // damage dealt by that player
    float time;   // game time of the last hit (seconds)
} PlayerDamage;

// The store items on a player's weapons (Krunker player.skins = [primary, secondary] and meleeIndex,
// ENT ent[2] / ent[9]): store.skins indices (the array position in assets/skins.json), -1 = none.
// A weapon skin only shows on the loadout slot whose weapon it is for (skin.weapon - 1), the melee
// one only on a melee weapon. Only int32 members, so it embeds unchanged in the packed ENT / SKINS.
typedef struct {
    int32_t primary;   // player.skins[0]: worn on loadout slot 0
    int32_t secondary; // player.skins[1]: worn on loadout slot 1
    int32_t melee;     // meleeIndex: a type-3 item on the melee weapon
} PlayerSkins;

#define PLAYER_SKINS_NONE ((PlayerSkins) {-1, -1, -1})

// First-person feel tweens (Krunker v3.1.1 TWEEN.Tween on landBobY / jumpBobY / jumpRot,
// started by players.js jump/resetStep/wall jump). Client-only animation state: only the
// local, non-recon player starts them, and they advance in real (unclamped) frame time
// like TWEEN.update() does. `via` is TWEEN's array target ([via, end] path, jumpRot).
typedef enum {
    PLAYER_EASE_LINEAR,
    PLAYER_EASE_BACK_OUT,
    PLAYER_EASE_ELASTIC_OUT,
    PLAYER_EASE_BACK_IN_OUT,
    PLAYER_EASE_CUBIC_OUT,      // meleeAnim swing / throw
    PLAYER_EASE_SINUSOIDAL_OUT, // meleeAnim equip flick
} PlayerEase;

typedef struct {
    float start, via, end;
    float time, duration;    // seconds
    unsigned char ease;      // PlayerEase
    unsigned char active:1;
    unsigned char has_via:1;
    unsigned char then_settle:1; // resetStep's onComplete: landBobY -> 0 over 1.1 s, Elastic.Out
} PlayerTween;

// Player.meleeAnim (players.js resetMeleeAnim / updateMsh): first-person melee arm and weapon
// offsets the weapon's `anim` (weapon->melee_anim) and swapWeapon's equip flick tween.
typedef enum {
    MELEE_ANIM_WEA_R,  // weaR: melee mesh rotation.z
    MELEE_ANIM_WEA_M,  // weaM: melee mesh position.x
    MELEE_ANIM_ARM_Y,  // armY: right arm position.y
    MELEE_ANIM_ARM_R,  // armR: right arm rotation.z
    MELEE_ANIM_L_ARM,  // lArm: left arm reach / punch
    MELEE_ANIM_R_ARM,  // rArm: right arm punch
    MELEE_ANIM_FLIP_W, // flipW: melee mesh rotation.y
    MELEE_ANIM_ARM_T,  // armT: right arm rotation.y
    MELEE_ANIM_ARM_M,  // armM: right arm position.x
    MELEE_ANIM_ARM_E,  // armE: right arm position.z
    MELEE_ANIM_ARM_S,  // armS: right arm rotation.x
    MELEE_ANIM_CHANNELS
} MeleeAnimChannel;

#define MELEE_ANIM_BIT(channel) (1u << MELEE_ANIM_##channel)

// One TWEEN.Tween(meleeAnim).to({...}, duration).delay(delay).easing(ease): the channels in
// `mask` go from their values at start() to `to` (seconds).
typedef struct {
    unsigned short mask;
    float to[MELEE_ANIM_CHANNELS];
    float duration, delay;
    unsigned char ease; // PlayerEase
} MeleeAnimStep;

// meleeAnim.anim: a step and the step its onComplete starts (has_then).
typedef struct {
    MeleeAnimStep step, then;
    float start[MELEE_ANIM_CHANNELS];
    float time;          // seconds since the running step's start()
    unsigned char phase; // 0 idle, 1 `step`, 2 `then`
    unsigned char has_then:1;
} MeleeAnimTween;

// Krunker's per-player look (jLrR6): skinColIndex / hairCol and the hat / body / waist / face / dye store items.
// Only int32 members, so it embeds unchanged in the packed net packets (ENT, SPAWN). The v7.1.4
// slots added after v3.1.1 ride in PlayerCosmeticsV7 at the end of those packets.
typedef struct {
    int32_t skin_color; // 0xRRGGBB (config.skinColors[skinColIndex].col), -1 = the class colour
    int32_t hair_color; // 0xRRGGBB, -1 = the class colour
    int32_t hat;        // store.skins index, -1 = none (hatIndex, type 1)
    int32_t body;       // bodyIndex (type 2): v3.1.1 called this slot backIndex; v7 ent[4]
    int32_t waist;      // waistIndex
    int32_t face;       // faceIndex
    int32_t dye;        // dyeIndex
} PlayerCosmetics;

#define PLAYER_COSMETICS_NONE ((PlayerCosmetics) {-1, -1, -1, -1, -1, -1, -1})

// v7.1.4's other cosmetic slots (players.js init / sync, server/index.ts parseEnterPlayerData):
// store.skins indices, -1 = none. ENT V9 and SPAWN V4 (protocol 1.3.1, WP-F) end with it.
typedef struct {
    int32_t head;        // headIndex (type 15, ent[32])
    int32_t back;        // backIndex (type 14, ent[31])
    int32_t shoe;        // shoeIndex (type 8, ent[15])
    int32_t pet;         // petIndex (type 9, ent[21])
    int32_t wrist;       // wristIndex (type 11, ent[26])
    int32_t charms[2];   // charms (type 12, ent[30]): the primary's and the secondary's
    int32_t player_card; // playerCardIndex (type 16, ent[33])
    int32_t badge;       // badgeIndex (badges.js): the account's custom badge, -1 none / hidden. ENT: ignored (the server takes the account's)
} PlayerCosmeticsV7;

#define PLAYER_COSMETICS_V7_NONE ((PlayerCosmeticsV7) {-1, -1, -1, -1, -1, {-1, -1}, -1, -1})

// ENT showBadges (ent[22..25], settings verifiedBadge / premiumBadge / partnerBadge / customBadge):
// what the player lets others see. Absent (shorter ENTs) = all shown, the settings' default.
enum {
    PLAYER_SHOW_BADGE_VERIFIED = 1 << 0,
    PLAYER_SHOW_BADGE_PREMIUM = 1 << 1,
    PLAYER_SHOW_BADGE_PARTNER = 1 << 2,
    PLAYER_SHOW_BADGE_CUSTOM = 1 << 3,
    PLAYER_SHOW_BADGES_ALL = 0x0F,
};

// Lag compensation: v3.1.1 players.js storeState / fetchState (shared/src/player.c). One
// stateHistory entry {time, h, x, y, z} per server update; entries older than
// config.stateHistory are dropped. The ring holds 500 ms at the 240 Hz tick_rate maximum.
#define PLAYER_STATE_HISTORY_MAX 128
#define GAME_STATE_HISTORY 0.5f       // config.stateHistory (500 ms)
#define LAG_COMP_ANCHOR_V3 0      // server.cfg lag_comp_anchor 0: v3.1.1 ping * 2 * lagComp from processing
#define LAG_COMP_ANCHOR_EXACT 1   // 1 (default): the shot input's client time on the server clock + 1 ping
#define LAG_COMP_ANCHOR_ARRIVAL 2 // 2: the shot input's arrival + 1 ping (the exact mode's fallback)
#define GAME_LAG_COMP_SEND_RATE 10.0f // player.cSendRate default (ent[16] servSendR off; = NET_STATE_SEND_HZ)
typedef struct {
    double time;       // Game.state_now when stored
    vec3 position;     // x, y, z (feet)
    float height;      // h
    float adjusted_height; // ah (v7.1.4): height with the head scaled by headMlt, the hit box top
    // ---- v10.0.2 stateHistory (players.storeState, v10:170708): the fields the posed hit boxes
    // need (shared/src/hitboxes.c). v7 states leave them 0 (the v7 AABB path does not read them).
    float step_val;    // sV (stepVal)
    float step_chase;  // stepChase
    float x_dir;       // xD (xDire)
    float y_dir;       // yD (yDire)
    float crouch_val;  // cV, rounded to 0 / 1 in v10
    int weapon_index;  // loadout[loadoutIndex] (-2 none: v10 stores undefined)
} PlayerStoredState;

// v10.0.2 Stalker camping (stalk.update, shared/src/modes/mode_rules.c mode_update_stalk)
typedef struct {
    unsigned char has_origin:1;   // campOriginPos set
    unsigned char has_effect:1;   // hasCampEffect (Dizzy given)
    vec3 origin;                  // campOriginPos
    float timer;                  // campTimer (s)
    float remove_timer;           // campEffectRemoveTimer (s)
    double left_at;               // leftCampAreaTime (game time, < 0 none)
} PlayerStalkerCamp;

typedef struct Player_t {
    struct Game_t* game;

    int uid;

    unsigned char is_you:1;
    unsigned char render_you:1;
    unsigned char left_handed:1;

    unsigned char active:1;
    unsigned char noclip:1;
    unsigned char noclip_prev:1; // the noclip flag of the previous input (player.c: a flip off is not ground)
    unsigned char god_mode:1;
    unsigned char unlimited_ammo:1;
    unsigned char force_hide:1;
    // Krunker "frz" (SET_FROZEN, game.deob.js:220495): freeze = !defaultMovement, !defaultVelocity,
    // disableShooting, disableMelee (ks_player_allows answers 0 for those while set). Only official
    // lobbies send it (krunker_link.c); it stays until "frz" false.
    unsigned char frozen:1;
    // Krunker "plk" PLAYER_LOCKS (iìïîììî :220492): player.disableShooting / disableMelee set by the
    // lobby's KrunkScript; ks_player_allows answers 0 for KS_PLAYER_CAN_SHOOT / _CAN_MELEE while set
    unsigned char lock_shooting:1;
    unsigned char lock_melee:1;
    unsigned char smite:1;      // /smite: every hit this player deals does 10000 damage (Krunker player.smite)
    unsigned char notarget:1;   // /notarget (KrunkNative, Source's notarget): bots and AIs never pick this player as a target
    unsigned char interpolate:1;
    unsigned char force_pos:1;  // Krunker forcePos: the next remote state snaps instead of interpolating (spawn / first sighting)
    unsigned char inspecting:1;

    unsigned char is_hidden:1;
    unsigned char on_ground:1;
    unsigned char on_ladder:1;
    unsigned char on_ramp:1; // added instead of the didCollideRamp local in procInputs
    unsigned char on_terrain:1;
    unsigned char on_wall:3;
    unsigned char terrain_slipping:1;
    unsigned char wall_jump:1;

    unsigned char did_act:1;
    unsigned char did_shoot:1;
    unsigned char did_jump:1;
    unsigned char did_wall_jump:1;
    unsigned char can_slide:1;
    unsigned char can_throw:1;
    int in_object;                // Krunker inObject: map object index + 1 of the onEnter trigger stood in, 0 = none
    unsigned char making_progress:1; // Krunker makingProg: an onInteract hold advanced this tick
    unsigned char doing_interact:1;  // Krunker doingInter: holding M1 in an onInteract trigger (no weapon use)
    int checkpoint;               // Krunker checkPoint: map object index + 1 of the last check point, 0 = none
    int checkpoint_list_count;    // checkPointList: single-use check points already taken
    int checkpoint_list[PLAYER_CHECKPOINT_LIST_MAX];

    unsigned int kills;
    unsigned int deaths;
    unsigned int death_streak;

    // Match state (server-authoritative, mirrored to clients through KILL/SCORE/LEADERBOARD packets).
    char name[PLAYER_NAME_SIZE];  // display name (server-assigned Guest_N, synced by SPAWN)
    int secondary_index;          // chosen secondary weapon id (Select Secondary), -1 = Pistol
    PlayerCosmetics cosmetics;    // skin colour + cosmetic items (ENT -> SPAWN), PLAYER_COSMETICS_NONE by default
    PlayerCosmeticsV7 cosmetics_v7; // v7.1.4 head / back / shoe / pet / wrist / charms / card / badge, PLAYER_COSMETICS_V7_NONE
    unsigned char show_badges;    // ENT showBadges (PLAYER_SHOW_BADGE_*), PLAYER_SHOW_BADGES_ALL by default
    int score;                    // Krunker player.score
    int reward;                   // Krunker player.reward: KR of the last endGame (END "reward" column)
    int additional_reward;        // Krunker player.additionalReward: KR from claimed KR Packages, added
                                  // to reward at endGame past the cap (airDropPackages.js / game.js:191329)
    unsigned int assists;
    unsigned int streak;          // Krunker player.streak: multi-kill counter (reset after feedTimer)
    unsigned int kill_streak;     // Krunker player.killStreak: kills since the last death
    float last_kill_time;         // game time of the last kill (seconds)
    float death_time;             // game time of the last death (seconds)
    float hit_timer;              // Krunker hitTimer: regen delay left after taking damage (seconds)
    float regen_ticker;           // Krunker ticker: 0.5 s regen tick
    float spread_plus;            // Krunker spreadPlus: movement/fall spread
    float damage_dealt;
    int last_killer_uid;          // Krunker lastKiller (Revenge), -1 none
    PlayerDamage damage_received[PLAYER_DAMAGE_SLOTS]; // Krunker dmgReceived (assists)
    unsigned int shot_count;      // Krunker shotCntP: picks config.projSpreads for projectiles

    int input_seq;                // inputSN: the last input procInputs ran (-1 after a spawn: none this life)
    // Reconciliation (net-recon): recon_snap asks the server for a NET_RECON_SEQ_ALL "l" this
    // tick (io_tp's lastSNBySocketId override). shot_vel_obj is the client's input.velObj: the
    // physPow knockback of this input's shot, copied onto the pending input for replays.
    unsigned char recon_snap:1;
    unsigned char recon_noclip:1; // the noclip the last "l" told the owner (a flip snaps at once)
    unsigned char has_shot_vel_obj:1;
    unsigned char shot_aim:1;       // v10 shoot(..., rMouse): the aim input held at this shot (physPull, Compressor)
    vec3 shot_vel_obj;
    unsigned char has_shot_grap_obj:1; // shot_grap_obj: this input's input.grapObj (tryGrapple, client)
    PlayerGrapple shot_grap_obj;
    PlayerGrapple grapple_sent;   // authority: the grapple the others last heard (NET_PACKET_GRAPPLE)

    float dt;
    float send_rate;

    vec3 interp_pos_start;
    vec3 interp_pos_end;
    vec2 interp_dir_start;
    vec2 interp_dir_end;

    float health;
    int max_health;
    int team;

    int class_index;

    float scale;
    float height;
    float speed;

    float spread;
    float crouch_val;
    float aim_val;
    float aim_time;

    int slid_cont;

    float air_time;
    float covered_distance;

    float swap_timer;
    float reload_timer;
    float laced_timer;            // KrunkNative Drank (shared/include/drank.h): s of "laced" left, 0 sober (both sides)
    float laced_duration;         // ... the whole spell (s), for the effect's ramp
    float slide_timer;
    float jump_timer;
    float interact_progress;
    float last_interact;          // players.interact: game.now of the last "int" (config.interactTimer)
    int spray_index;              // player.sprayIndex (ENT ent[1]): sprays.js index or a type-4 store.skins index
    float last_spray;             // players.spray: game.now of the last spray (config.sprayTimer)
    int sprays_placed;            // WP-E: spraysPlaced, the next "<sid>_<n>" slot (cycles at game_spray_limit)
    float taunt_ready_at;         // WP-E: lastTaunt (game.now + taunt tm), 0 = never taunted (players.taunt)

    float *ramp_fix;

    float idle_anim;
    float hp_chase;

    vec3 position;
    vec3 last_position;
    vec3 velocity;
    vec2 direction;
    vec2 shot_look;               // the pitch / yaw the last ray left with (recoil and spread included): the line of fire

    float x_vel_cylinder;
    float z_vel_cylinder;

    // ---- WP-A: v7.1.4 movement state (player.js procInputs, players.js resetStep / jump) ----
    int wall_jump_count;          // wallJumpC: wall jumps since the last landing
    int wall_jump_max;            // wallJumpMax: mode wallJM, else class wallJM (setClass)
    float can_slide_timer;        // canSlideT (s): a landing cannot start a slide for 100 ms after one
    float height_old;             // heightOld: height before this tick's crouch change (headClipFix)
    double slide_ms;              // slideTimer in v7's double ms (its exact last slide tick); slide_timer mirrors it in s
    unsigned char on_pad:1;       // onPad: a jump pad launched the player this tick
    unsigned char was_on_pad:1;   // wasOnPad: onPad of the previous tick (top/side contacts keep velocity.y)
    unsigned char was_on_ground:1;// wasOnGround: grounded and not jumping when this tick's collisions began
    unsigned char stop_crouch:1;  // stopCrouch: hit a ceiling this tick
    unsigned char was_stop_crouch:1; // wasStopCrouch: headClipFix blocks crouch and jump for one tick
    // ---- end WP-A movement state ----

    float jump_rotate;

    float land_bob_y;
    float land_bob_yr;
    float jump_bob_y;
    float jump_rotate_mlt;
    float step_val;
    float step_chase;
    unsigned char step_dir_neg:1; // Krunker stepDir: 0 = +1, 1 = -1 (flips at +-PI/2)

    // ---- v10.0.2 Stalker rework (GameModeConfig.stalker_v10; v10:168968-169434, 169664-169685,
    // 169754-169764, 170078, 170153-170154): the stalker's leap, wall stick and coyote time
    unsigned char stuck_to_wall:1;     // stuckToWall: no gravity, no movement
    unsigned char did_stick_to_wall:1; // didStickToWall: the jump key must be released between sticks
    unsigned char stuck_to_wall_dir;   // stuckToWallDir: the onWall side (1..4) it stuck to
    float stalker_coyote_time;         // stalkerCoyoteTime (s): 0.15 after leaving the ground or a wall
    float last_stalker_leap;           // lastStalkerLeap (game time, s); leaps 3 s apart
    PlayerStalkerCamp stalker_camp;    // stalk.update camping (Living players)

    PlayerTween land_tween;     // landBobY (landTween)
    PlayerTween jump_bob_tween; // jumpBobY (jumpTween, and the landing tween's jumpBobY -> 0)
    PlayerTween jump_rot_tween; // jumpRot
    float swap_tween_r;         // swapTweenR: weapon swap wiggle (viewmodel)
    PlayerTween swap_tween;     // swapTweenAnim
    float inspect_x;            // inspectX: weapon inspect swing (viewmodel)
    float inspect_anim[9];      // inspectAn (arZ, arX, arY, apZ, apX, apY, wpZ, wpY, wrY): melee inspect pose
    float inspect_anim_start[9];
    float inspect_anim_time;    // seconds into the 400 ms inspectTween
    unsigned char inspect_anim_active;
    float melee_anim[MELEE_ANIM_CHANNELS]; // meleeAnim: first-person melee swing / throw / equip pose
    MeleeAnimTween melee_anim_tween;       // meleeAnim.anim
    signed char hand_anim_ind;             // handAnimInd: 0 (unset), then -1 / 1 alternating per swing

    // shoot(): recoilTween on recoilTweenY/R/YM/Z (weapon.recoilAnim): phase 1 kicks the
    // masked values to the weapon's targets linearly, phase 2 eases all four back to 0.
    float recoil_tween[4];      // Y, R, YM, Z
    float recoil_tween_start[4];
    float recoil_tween_time, recoil_tween_duration, recoil_tween_return;
    unsigned char recoil_tween_phase; // 0 idle, 1 kick, 2 return
    unsigned char recoil_tween_mask;

    // Local first-person events for client systems (sound_events.c): bumped per Krunker
    // playerStep step (stepDir flip) and per resetStep landing with a vertical speed.
    unsigned short step_events;
    unsigned short land_events;

    vec3 bob_anim;
    vec3 lean_anim;
    vec3 recoil;

    float recoil_force;
    float recoil_anim;
    float recoil_anim_y;

    int *loadout;
    unsigned int *ammo;
    float *reloads;
    int burst_count;

    int loadout_size;
    int loadout_index;
    // KrunkNative Object Manager (shared/src/object_manager.c): the extra loadout weapons follow the
    // class / mode loadout ("base", slots [0, loadout_base)) in slots [loadout_base, loadout_size).
    // 0 = the whole loadout is base (player_loadout_base). Packets and v7 loadout logic see the base.
    int loadout_base;
    int extra_weapons[PLAYER_EXTRA_WEAPONS_MAX]; // weapon ids, in the order they were added
    int extra_weapon_count;
    float om_tokens;              // Object Manager request bucket (OM_RATE_*), refilled by game time
    float om_tokens_at;           // game->now of the last refill
    int om_copy;                  // Tool Gun Duplicator: the copied spawned object's uid + 1, a map object's -(uid + 1), 0 none (authority)
    float om_edit_at;             // game->now of the last accepted Object Manager edit (OM_EDIT_INTERVAL, authority)
    // KrunkNative Physics Gun on a player (object_manager.h OM_EDIT_PLAYER, devs only): held by uid
    // om_held_by - 1 until om_held_until (game->now), at om_hold_pos; its input moves nothing
    // (mode_input_locked) on the authority and, after NET_CHEAT_HELD, on its own client
    unsigned char om_held:1;
    int om_held_by;
    float om_held_until;
    vec3 om_hold_pos;

    Input *input_queue;
    size_t input_queue_size;

    const Weapon *weapon;

    PlayerSkins skins;            // weapon / melee store skins (ENT -> SKINS), PLAYER_SKINS_NONE by default
    void *mesh;

    ModePlayerState mode;         // game mode stats (lives, caps, tier, ...), shared/src/modes

    // Lag compensation (players.js storeState / fetchState / getStateConst, player.c).
    float ping;                   // player.ping: smoothed RTT in ms (server: ClientConnection.ping_ms)
    float lag_comp;               // player.lagComp: ENT lag_comp (ent[11] slider, server-clamped to 0.1..1), default 1
    unsigned char chall_mode:1;   // WP-C player.challMode (ENT V8 ent[7]): no health regen (game_tick_regen)
    unsigned char anon_mode:1;    // WP-C player.anonMode (ENT V8 ent[19]): PLAYER_INFO hides the account from others
    float c_send_rate;            // player.cSendRate (ent[16] servSendR: 20, else 10)
    double input_arrival;         // Input.arrival of the input procInputs is running (0 = now)
    Input input_running;          // a copy of that input (exact anchored lag compensation reads its clock fields)
    LagCompClock lag_clock;       // the connection's input timeline (server; reset by player_spawn)
    // Bot players (shared/src/bots.c, v7.1.4 bots.js): players.add isBot, player.dmgMlt (0 = 1).
    unsigned char is_bot:1;
    float dmg_mlt;
    PlayerStoredState state_history[PLAYER_STATE_HISTORY_MAX]; // stateHistory ring, newest at state_history_head
    int state_history_head;
    int state_history_count;
    // ---- WP-B: v7.1.4 combat state (shared/src/player.c combat region, shared/src/effects.c) ----
    float charge_time;            // chargeTime (s): Crossbow / knife throw / Zapper / Charge Rifle charge
    unsigned char full_charge:1;  // fullCharge: held to chrgTime on a weapon without ovrChrg
    unsigned char over_charged:1; // overCharged: held past chrgTime * 1.2 (ovrChrg); un-aim to charge again
    unsigned char shot_blast:1;   // the last shot was a full-charge one (shoot(..., fullCharge)): "_blast" sound
    PlayerGrapple grapple;        // grapple (Grappler): the hook point and rope length, active 0 = null
    unsigned char is_juggernaut:1;// isJuggernaut (Juggernaut streak): 2000 HP, Minigun, no regen
    float speed_boost;            // speedBoost (admin "spb", 0 = none): the movement acceleration x it (player.js:169777)
    float head_mlt;               // headMlt (0 = 1): head hitbox scale (Big Head FFA), spawn "headMlt"
    float adjusted_height;        // adjustedHeight: height with the head scaled by head_mlt (hit tests)
    PlayerAttributes attributes;  // effects.js attributes (effects_reset at spawn)
    unsigned char effects[EFFECT_COUNT];          // stacks per effect
    float effect_timers[EFFECT_COUNT][EFFECT_STACK_MAX]; // attributeTimers (s), one per timed stack
    unsigned char effect_timer_count[EFFECT_COUNT];
    int zapped_by;                // zappedBy: uid credited with Zapped damage, -1 none
    unsigned int regen_tick_count;// tickerC: 500 ms ticks (poison every 8th, zap every 2nd)
    int perks[PLAYER_PERK_MAX];   // player.perks
    int perk_count;
    int streak_indexes[STREAK_SLOTS];      // streakIndexs: the picked streak per slot, -1 none
    unsigned int real_kill_streak;         // realKillStreak: resets at the highest picked streak
    unsigned char streaks_owned[STREAK_COUNT]; // player.streaks[i].cnt: earned, not yet called in
    int start_health;             // startHealth: maxHealth before Juggernaut
    float throw_charge;           // throwCharge (s): class throwChrg || config.throwCharge
    float throw_speed_mlt;        // v (max(0.3, d)) of the last knife throw: the owner's physObj flies at spd * v too
    int attach_index;             // attachIndex (ENT / spawn): the sight on attach weapons, 0 Reflex, 1 Acog, -1 none
    unsigned char upgraded_slots; // player.upgrades: bit per loadout slot holding its upgradeWeapons entry (krumMachine)
    int weapon_streak;            // weaponStreak: kills in a row with streak_weapon ("wstk")
    uint64_t unique_weapon_kills; // uniqueWeaponKills: bit = a weapon id this player killed with ("uwk"; v7 keeps it for the player's lifetime)
    int streak_weapon;            // currentStreakWeapon, -1 none
    unsigned char has_account:1;  // player.account (server: a backend account or the host's own machine,
                                  // which covers the offline sandbox, decisions.md): reqAccount streaks
    // ---- end WP-B combat state ----
    // ---- map-logic WP-B: v7.1.4 world state of the player (shared/src/world.c)
    unsigned char no_pvp:1;            // noPVP: inside a NOPVP_ZONE this tick (no damage given or taken)
    unsigned char force_pvp:1;         // forcePVP: inside a PVP_ZONE (team undefined: only forcePVP pairs fight)
    unsigned char has_init_pos:1;      // setInitPos ran (the first spawn of the round)
    unsigned char has_checkpoint_data:1; // checkPointData = [timeLog, gameTime] stored by the last check point
    vec3 init_pos;                     // initPos: resetToStart target
    float reset_timer;                 // resetTimer (ms): 250 after a parkour reset, input [13] ignored meanwhile
    int time_log;                      // timeLog: score zones logged this round ("gt")
    float game_time;                   // gameTime (ms): the ownTime speed-run clock (Parkour)
    int last_timer_sent;               // lastTimer: the last clock value sent (tenths of a second), -1 none
    int checkpoint_time_log;           // checkPointData[0]
    float checkpoint_game_time;        // checkPointData[1] (ms)
    int score_objs[PLAYER_SCORE_OBJS_MAX];         // scoreObjs: score zones (object index) taken this round
    int score_obj_count;
    int colliding_objs[PLAYER_COLLIDING_OBJS_MAX]; // collidingObjs: onEnterObject / onExitObject objects inside
    int colliding_obj_count;
    uint64_t colliding_seen;           // bit i: colliding_objs[i] was touched this tick (vA541)
    int build_index;                   // buildIndex: the Build Tool's buildObjs.js pick (0..3)
    // KrunkNative deviation (doc/v7/decisions.md "Build Tool cost"): builds cost no score where the
    // Object Manager grants spawn rights (OM_CAP_SPAWN: the offline sandbox, devs, a custom
    // game's host). The authority sets it at every spawn, the client from its OM caps.
    unsigned char build_free:1;
    // players.build's client half (buildHelper / buildHelperN): where the next build would go and
    // whether it may (blue) or not (red); active 0 = the look ray hits nothing (both hidden).
    struct {
        unsigned char active:1;
        unsigned char can:1;
        vec3 feet;  // x, y (the bottom), z
        vec3 size;  // w, h, l as placed (rotated)
    } build_preview;
    unsigned char select_build:1;      // selectBuild: aiming with the Build Tool (the pick wheel is up)
    // ---- end WP-B
    // ---- WP-C: v7.1.4 trigger / interaction state (shared/src/triggers_v7.c, interact.c) ----
    TriggerValueSet custom_values; // custVals: Set / Inc Player Value, condition 1, value type 2; never reset ("cv")
    int pending_trigger;          // pendingTrigger: map uid + 1 of the KR-cost onInteractKey awaiting "cnfm", 0 none
    int pending_spin;             // pendingSpin: PRIZE_WHEEL uid + 1 awaiting "cnfm" 2, 0 none
    int last_picked[PLAYER_PICKED_MAX]; // lastPicked[weapon id]: uid + 1 of the pickup it came from, 0 none
    // ---- end WP-C ----
    // ---- map-logic WP-H: zombies economy (shared/src/zombies.c, shared/include/zombies.h)
    // finiteAmmo reserveAmmos. v7 keeps one per loadout slot and carries it along when the
    // loadout changes; KN keys them by weapon id, which is the same thing for a loadout that
    // never holds a weapon twice (player_reserve_ammo).
    int reserve_weapons[PLAYER_RESERVE_MAX];
    int reserve_ammo[PLAYER_RESERVE_MAX];
    int reserve_count;
    int shared_boxes;                  // sharedBoxes: mystery box weapons shared with a melee hit
    unsigned char sharing_is_caring:1; // sharingIsCaring (5 shared): half price boxes, no teddy
    // ---- end WP-H
    // ---- player-viewer WP-E: the v7.1.4 death screen ("3" death stats, NET_PACKET_DEATH_INFO).
    // The life counters reset at every spawn (player.js reset: totShotsL, hitShotsL, killsL, scoreL)
    // and count only while active, like v7; the account values are the server's (accounts.c), set
    // at spawn: guests and bots keep class_stat 0 and pc_stat_index -1.
    float life_start;                  // spawnTime (game->now of the spawn, seconds)
    int life_shots, life_hits;         // totShotsL / hitShotsL: hitscan rays fired, rays that hit a player / AI first
    int life_kills, life_score;        // killsL / scoreL
    int class_stat;                    // account.stats["c" + classIndex] (0 hidden by anonMode / no account)
    int pc_stat_index;                 // pcStatIndex (NET_PACKET_PC_STAT), -1 none / no account
    char pc_stat_value[16];            // config.playerCardStats[pc_stat_index].stat(player), as v7 prints it
    // ---- end WP-E
    // player-viewer WP-E part 2c: v7 player.medals (game.incMedal) of this match, by
    // overlay.medalsList index (progression.h): the end screen's Medals tab (NET_PACKET_END_MEDALS)
    // and the account's medal counts (the stats commit).
    uint16_t medal_counts[PLAYER_MEDAL_SLOTS];
} Player;

// KrunkNative: a connection's player state that outlives its Player (the menu before the first spawn,
// death, spectating, a map change). Chat commands write it whether or not the player is spawned in
// (god, noclip ... from the menu, chat_commands.c), the server keeps it on the connection and
// applies it at every spawn (player_keep_apply). Only the fields in `set` hold a value.
#define PLAYER_KEEP_GOD 1u
#define PLAYER_KEEP_NOCLIP 2u
#define PLAYER_KEEP_NOTARGET 4u
#define PLAYER_KEEP_SMITE 8u
#define PLAYER_KEEP_UNLIMITED_AMMO 16u
#define PLAYER_KEEP_FORCE_HIDE 32u
typedef struct {
    unsigned char set;            // PLAYER_KEEP_* bits
    unsigned char god_mode, noclip, notarget, smite, unlimited_ammo, force_hide;
    unsigned char has_spawn;      // `tp` while not spawned in: the next spawn's feet (once)
    vec3 spawn_at;
    unsigned char has_view;       // the last life's eye and look (Object Manager spawns from the menu)
    vec3 view_eye;
    vec2 view_direction;          // pitch, yaw
} PlayerKeep;
// The kept cheats onto `player` (a spawn): returns PLAYER_KEEP_* of the ones that changed
unsigned int player_keep_apply(Player*, const PlayerKeep*);

typedef struct {
    int streak;         // v7 info.stk + 1 (STREAK_NUKE + 1: a Nuke kill), 0 = not a streak kill
    int weapon_id;      // index into Game.weapons, -1 = none (fall/nuke)
    unsigned char headshot:1;
    unsigned char wallbang:1;
    unsigned char fists:1;  // Krunker fs: melee without the knife
    unsigned char thrown:1; // Krunker pj: projectile kill (thrown knife -> Bullseye)
    unsigned char legshot:1; // WP-E part 2c: Krunker ls (the "ls" stat / challenge of a killing hit)
    float distance;     // Krunker dst: fraction of the weapon range
} PlayerKillInfo;

Player *player_init(struct Game_t*);
void player_spawn(Player*);
int player_switch_class(Player*, int class_index, int secondary_index); // KrunkNative console class: the loadout now, position kept (player.c)
void player_swap_weapon(Player*, int, int, int, int);
void player_update_height(Player*); // players.updateHeight: height from crouch_val
void player_movement_spawn(Player*); // WP-A setClass: wall-jump limits (class / mode) and the v7 movement state
void player_kill(Player*, Player*, PlayerKillInfo*, int); // victim, killer (NULL = suicide/fall), info, skip_rewards
int player_change_health(Player*, Player*, float, int); // victim, dealer, damage (<0 heals), crit; 1 = victim died (authority only)
void player_score(Player*, int, int); // player, amount, is_kill_reward
void player_queue_input(Player*, const Input*);
int player_accept_input(Player*, Input*); // a network frame: v3.1.1 inputDecode checks (finite, delta clamp, seq floor, 240 cap); 1 = queued
void player_drain_inputs(Player*);         // procInputs every queued frame now (player_update; io_int before its action)
void player_proc_input(Player*, const Input*, int, int);
float player_round3(float v); // JS v.round(3); procInputs rounds delta (ms), yaw and pitch (0.001 rad)
float player_wrap_yaw(float yaw); // controls.js xDr = rotation.y % Math.PI2: (-2pi, 2pi), non-finite -> 0
float player_clamp_pitch(float pitch); // controls.js pitchObj clamp [-pi/2, pi/2], non-finite -> 0
int player_look_follows_camera(const Player*); // class (movSch || 0) == 0 && trnSp == 1: frames without an input frame turn it with the camera
void player_scroll_weapon(Player*, int dir, int recon); // manager.reload(player, +-1): wheel / melee on melee
// KrunkNative Object Manager: the class / mode part of the loadout (slots [0, base)); the extra
// weapons (shared/src/object_manager.c) follow it. The whole loadout for a player without extra
// weapons (every v7 rule then sees exactly v7's loadout) or when no base is recorded.
static inline int player_loadout_base(const Player *player) {
    return player->extra_weapon_count > 0 && player->loadout_base > 0 && player->loadout_base <= player->loadout_size
        ? player->loadout_base : player->loadout_size;
}
void player_loadout_append_extras(Player *player); // object_manager.c: base -> base + extras (loadout_base set)
void player_update(Player*, float);
float player_lerp_angle(float a, float b, float t); // index.js Math.lerpAngle (remote yaw/pitch interpolation), shortest arc
void player_reset_feel(Player*); // spawn: step/bob/lean/recoil animation state back to rest
void player_inspect_weapon(Player*); // wInspect: toggle the weapon inspect (local player, inspKey)
void player_cancel_inspect(Player*);
void player_reload(Player*); // players.js reload: starts the reload timer (the authority rolls the KrunkNative Drank's laced pour-up, drank.h)
void player_end_grapple(Player*); // manager.endGrapple: grapple = null (Grappler)
void game_sync_grapples(struct Game_t*, int uid); // NET_PACKET_GRAPPLE: NET_TARGET_ALL the changes, a uid every hook (joiner)
void player_melee_anim_reset(Player*); // resetMeleeAnim (stop meleeAnim.anim first with player_melee_anim_stop)
void player_melee_anim_stop(Player*);  // meleeAnim.anim.stop()
void player_melee_anim_start(Player*, const MeleeAnimStep *step, const MeleeAnimStep *then); // then: onComplete step or NULL
void player_send_event();
// Lag compensation (v3.1.1 players.js): storeState once per server update, getStateConst
// (the shooter's rewind in ms), fetchState (the target's stored state that far back, plus the
// remote-player interpolation delay). fetchState returns 0 when no state may be hit (empty
// history); without lag compensation (offline, lag_comp 0) it gives the current state.
void player_store_state(Player*);
float player_get_state_const(const Player*);
int players_fetch_state(const Player *target, float ping_ms, PlayerStoredState *out);

// ---- WP-B: v7.1.4 effects, perks, streaks and hit maths (shared/src/effects.c, player.c) ----
void effects_reset(Player*);                       // spawn: no effects, attributes at their def
#define PLAYER_EFFECTS_V7 1 // player_give_effect & co. exist (modes guard their effect calls on it)
void player_give_effect(Player*, int effect, int count); // players.giveEffect (authority sends "pk")
void player_remove_effect(Player*, int effect, int one); // removeEffect (one stack) / clearEffect
void player_set_effect_stacks(Player*, int effect, int count); // client mirror of "pk": stacks + attribute, no packet
void player_tick_effects(Player*, float delta);    // attributeTimers countdown (authority)
void player_effects_regen_tick(Player*);           // the 500 ms ticker's poison / zap damage (authority)
float player_attr_speed(const Player*);  // attributes.speed (Boosted), 1 by default
float player_attr_rooted(const Player*); // attributes.rooted, 1 by default
float player_attr_jump(const Player*);   // attributes.jump (Jump Boost), 1 by default
float player_attr_reload(const Player*); // attributes.reload * perk 2 (x0.5)
float player_attr_fire_rate(const Player*); // attributes.fRate * perk 1 (x0.66)
float player_attr_damage(const Player*); // attributes.dmg (Strength)
int player_has_perk(const Player*, int perk);
void player_give_perk(Player*, int perk);           // players.givePerk (authority broadcasts "upk")
int player_has_equipment(const Player*);            // hasEquipment: an equipment weapon in the loadout
const Weapon *player_slot_weapon(const Player*, int slot); // players.getWeapon: the upgraded entry for an upgraded slot
void player_upgrade_weapon(Player*, int slot);      // krumMachine: upgrade a slot, full upgraded ammo ("upgr", authority)
void player_set_upgrades(Player*, unsigned char slots); // client mirror of NET_PACKET_UPGRADES
void player_send_upgrades(Player*);                 // NET_PACKET_UPGRADES of the current slots (map-logic WP-H)
void player_give_weapon(Player*, int weapon_id);    // players.giveWeapon (streak equipment), authority
void player_remove_weapon(Player*, int weapon_id);  // players.removeWeapon
void player_set_juggernaut(Player*, int on);        // game.setJuggernaut
void player_set_streak_indexes(Player*, const int indexes[STREAK_SLOTS]); // ENT streakIndexes (sanitised)
void player_update_streaks(Player*);                // updateStreaks after a kill (authority)
int player_use_streak(Player*, int streak);         // useStreak: 1 when it activated
void player_check_streak(Player*, int slot);        // checkStreak: the streak key / "k<slot>"
int game_start_nuke(struct Game_t*, Player*);       // streaks.js Nuke.start: 0 when one is running
float player_hitbox_pad_w(const Player*);           // hitboxPadW: game hitBoxPad + class hbS
float player_hitbox_pad_h(const Player*);           // hitboxPadH: game hitBoxPad + class hbH
float player_hurtbox_scale(const Player*);          // v10 hurtboxScale: game hurtboxScale x class hurtboxScale (0 reads as 1)
int player_class_model(const Player*);              // the class config mdl (0 default)
// Hit zones of a hit at world height `hit_y` on `target` (players.js shoot): head when within
// headScale * headMlt of the top of the box `base_y + adjusted_height`, legs below legHeight
// minus the crouch drop. Used by hitscan, melee and projectiles.
int player_hit_is_head(const Player *target, float base_y, float adjusted_height, float hit_y);
int player_hit_is_leg(const Player *target, float base_y, float hit_y);
// The zones' reference box of a hit: the lag-compensated `state` box (KrunkNative), or the target's
// live position and adjustedHeight where the ruleset says so (Ruleset.hit_zones_live: v7 / Krunker Strike).
void player_hit_zone_base(const Player *target, const PlayerStoredState *state, float *base_y, float *adjusted_height);
// Damage of one weapon hit after head / leg multipliers (noHeadShot, per-weapon lsMlt, no leg
// multiplier on a headshot) and headshotOnly; `damage` is after falloff.
float player_hit_damage(const struct Game_t*, const Weapon*, float damage, int head, int leg);
int player_can_damage(const Player *dealer, const Player *victim); // teams, friendly fire, noPVP / forcePVP (KrunkNative Piss puddles)
// ---- end WP-B ----

// WP-E: GameConfig carries every v7.1.4 serverConfig key (GAME_CONFIG_V7, shared/include/game_config.h).
#define GAME_CONFIG_V7 1
#define GAME_CONFIG_HAS_LOG_TIME 1      // log_time / last_checkpoint_time (map-B)
#define GAME_CONFIG_TEAMS 6             // team 0 + config.teamLimit (5) teams
#define GAME_CONFIG_CLASS_LIMITS 16     // classLim0..15, config.classes

typedef struct {
    int max_players;
    int min_players;
    int game_time;
    float warmup_time;
    int auto_respawn;
    int lives;
    int score_limit;
    float gravity_mlt;
    float jump_mlt;
    float delta_mlt;
    float slide_time;
    float impulse_mlt;
    float wall_jump;
    float strafe_speed;
    // WP-A: v7.1.4 movement settings (config.js serverConfig; same names and types as map-E's
    // serverConfig block, so either patch can carry them)
    float slide_speed;                // slidSpd 0..2 (1): slide boost multiplier, 0 stops the slide
    float fall_damage;                // fallDmg 0..10 (0 = off): landing damage |vy| * 10 * fallDmg (authority)
    float fall_damage_threshold;      // fallDmgThr 0..10: landing |vy| (units per ms) above which it applies
    unsigned char fixed_movement;     // fixMov: exact exponential integration (movement.js)
    unsigned char head_clip_fix;      // headClipFix (comp 1): a ceiling hit blocks crouch / jump for a tick
    unsigned int can_slide:1;
    unsigned int air_strafe:1;
    unsigned int auto_jump:1;
    unsigned int bullet_drop:1;
    unsigned int health_regen:1;
    unsigned int kill_rewards:1;
    unsigned int headshots_only:1;
    unsigned int no_secondaries:1;
    unsigned int no_streaks:1;
    unsigned int disable_borders:1;
    unsigned int throwable_melees:1;
    unsigned int select_team:1;
    unsigned int allow_spectate:1;
    unsigned int third_person:1;
    unsigned int hide_nametags:1;
    char *team1_name;
    char *team2_name;
    float team1_damage;
    float team2_damage;
    float health_mlt;
    float fire_rate;
    float reload_speed;
    // WP-B (v7.1.4 game settings): hitBoxPad (def 0.6, defC 0.1 on competitive hosts) and
    // chrgWeps (def true). hitbox_pad < 0 is valid (-2..2), so `hitbox_pad_set` marks a value.
    float hitbox_pad;
    unsigned int hitbox_pad_set:1;
    float hurtbox_scale;              // v10.0.2 hurtboxScale (game setting, def 1, 1..2; 0 = 1) x the class value
    unsigned int no_charge_weapons:1; // !chrgWeps

    // ---- WP-E: the rest of the v7.1.4 serverConfig (doc/v7/map-logic.md §4.3.1). shared/src/
    // game_config.c fills every field (v7 def, defC on comp hosts, clamps) from host settings, a
    // map's "config" or server.cfg game.<varN>. New bools are unsigned char (addressable). ----
    int host_type;                    // hostTypes index ("override"): GAME_HOST_* (game_config.h), 0 custom
    unsigned char comp;               // isComp: a comp host (hostTypes[1].comp)
    unsigned char mimic_pubs;         // mimicPubs (zombies / raid hosts)
    unsigned char is_private;         // private
    int game_rounds;                  // gamRounds 1..10 (def 1, comp 3)
    float intermission_time;          // intermTmr (s) 5..120 (30)
    float objective_time;             // objtvTime (min) 0.1..10 (1)
    unsigned char keep_team_score;    // keepTScore
    unsigned char no_assists;         // noAssists
    unsigned char force_classes;      // forceC (1): the mode's noWeap / forceClass / teamClass apply
    unsigned char log_time;           // logTim (1): Race / Parkour log their times
    unsigned char last_checkpoint_time; // lstChkT: a checkpoint reset rewinds the time
    unsigned char friendly_fire;      // frFire
    unsigned char hide_nametags_allies; // nameTagsFR (nameTags = hide_nametags: enemies)
    unsigned char kill_cams;          // kCams (1)
    unsigned char allow_anonymous;    // aAnon (1; nothing reads it in v7)
    unsigned char om_weapons;         // KrunkNative knOmWeps (1): the Object Manager's extra loadout weapons in custom games
    unsigned char tp_cam;             // KrunkNative knTPCam (0): the host lets everyone use the third-person camera
    // KrunkNative knClanWar / knClanA / knClanB: v10's mode.clanWar (players.setTeam team = account
    // clan) as a host option of team modes: team 1 = clan A, team 2 = clan B, everyone else
    // spectates. Empty tags: the first two clans to join claim the teams (server/src/accounts.c).
    unsigned char clan_war;
    char clan_war_tags[2][17];
    int spec_slots;                   // specSlots 0..4 (2)
    int team_size;                    // tmSize: select 0..3 = 1v1..4v4 (3)
    unsigned char no_cosmetics;       // noCosm (comp 1)
    unsigned char test_mode;          // tstCmp (comp 1)
    unsigned char no_draws;           // noDraws (comp 1)
    unsigned char best_of;            // bstOfR (comp 1)
    unsigned char minimaps;           // minimaps (1)
    unsigned char limit_classes;      // limitClasses (comp 1)
    int class_limits[GAME_CONFIG_CLASS_LIMITS]; // classLim0..15 0..20 (comp: 1 for config.rankedClasses)
    // nameTeam1..5 / t1Dmg..t5Dmg by team ([0] unused). team1_name / team2_name point at [1] / [2]
    // and team1_damage / team2_damage equal [1] / [2] (game_config.c and game_configure keep them so).
    char team_names[GAME_CONFIG_TEAMS][17];
    float team_damage[GAME_CONFIG_TEAMS];
    // config.classes: the classes a player may pick, classes[0] the fallback (setClass). 0 = every
    // class (v7 resetClasses).
    int class_list[GAME_CONFIG_CLASS_LIMITS];
    int class_list_count;
    // ---- end WP-E ----

    int *maps;
    int map_count;

    int *modes;
    int mode_count;
} GameConfig;

extern const GameConfig g_default_game_config;

// game.voteToKick: the one vote kick a lobby runs at a time (Krunker v10.0.2 config voteKickReq 4,
// voteKickTimer 35 s, voteKickBan 30 min, voteKickMaxLvl 20; server/src/votekick.c owns the
// rules, shared/src/game.c runs the timer and calls hooks.vote_kick_expired when it runs out).
#define VOTE_KICK_VOTERS_MAX 64
typedef struct {
    float timer;     // seconds left

    int votes_for;   // yes votes (the initiator's counts)
    int votes_req;   // votes needed
    int votes_no;    // no votes (the [2] key)
    int eligible;    // how many connections may vote (accounts other than the target)

    Player *player;  // the target
    int target_uid;
    char target[32];    // the target's alias as broadcast
    char initiator[32]; // who opened it
    int voters[VOTE_KICK_VOTERS_MAX]; // uids that voted (yes or no), once each
    int voter_count;
} VoteKick;

// Server-side match events leave the shared game through these hooks; the native
// server (server/src/net.c) and the offline client's in-process game (client/src/net/local_game.c)
// route them to their connections. A NULL hook drops the event (tests, client mirror).
#define NET_TARGET_ALL (-1)
// Every connection without a player in Game.players (spectators, clients still in the menu).
#define NET_TARGET_NON_PLAYERS (-2)
// Every connection except the one owning player `uid` (players, spectators and menu clients):
// Krunker's broadcasts that skip the source (tracers, others' health).
#define NET_TARGET_EXCEPT(uid) (-3 - (uid))
#define NET_TARGET_EXCEPT_UID(target) (-3 - (target)) // decode: target <= -3

struct AccountInfo_t; // shared/include/account.h (GameHooks.account_info)
struct BotChatRequest_t; // shared/include/bot_chat.h (GameHooks.bot_banter)
typedef struct {
    void *user;
    // Send one framed packet to the connection owning player `uid`, or to every
    // connection for NET_TARGET_ALL, NET_TARGET_NON_PLAYERS or NET_TARGET_EXCEPT(uid).
    // `type` is a NetPacketType.
    void (*send)(struct Game_t*, int uid, int type, const void *payload, uint16_t length);
    // The end screen timer ran out: rotate to the next map (the callee re-inits the
    // game; called last in game_tick, nothing touches the old players afterwards).
    void (*next_round)(struct Game_t*);
    // A mode respawns `uid` on its own (forceSpawn): the callee spawns it like an ENT
    // request. NULL: the player respawns when its client asks.
    void (*respawn)(struct Game_t*, int uid);
    // A mode replaces a player's loadout or outfit in place (Gun Game / Sharp Shooter /
    // Stalker loadouts, Infected / Stalker outfits): called with done = 0 right before the
    // change and done = 1 right after. The in-process offline client (client/src/net/
    // local_game.c), whose game is also its render state, rebuilds that player's meshes
    // around it. NULL elsewhere (server, client mirror: the LOADOUT/TEAM events do it).
    void (*player_changing)(const struct Game_t*, Player*, int done);
    // endGame(): player.reward, the KR this round earns (v3.1.1 game.js: accounts only,
    // min(premiumT ? 40 : 30, floor(score * rewardMlt / 100)), none in hosted games). The
    // server knows the accounts (server/src/accounts.c); NULL = 0 (guests / offline).
    int (*reward)(struct Game_t*, const Player*);
    // Bot players (shared/src/bots.c): the game sends their SKINS / SPAWN / PLAYER_INFO / LEAVE
    // itself through `send`. The in-process offline client, whose game is its render state,
    // also needs their meshes: bot_spawning(done = 0) right before player_spawn (drop the old
    // ones), (done = 1) right after (build them); bot_removing before the Player is freed.
    void (*bot_spawning)(struct Game_t*, Player*, int done);
    void (*bot_removing)(struct Game_t*, Player*);
    // A uid for a new bot that no connection will ever get (server: its connection id
    // counter). NULL: the game counts from GAME_BOT_UID_BASE past every uid in use.
    int (*alloc_uid)(struct Game_t*);
    // WP-A (shared/include/account.h): the account facts of a player for the v7 zones, trigger
    // conditions / methods and class locks. NULL = guest. Returns 1 when *out was filled.
    int (*account_info)(const struct Game_t*, const Player*, struct AccountInfo_t *out);
    // WP-C: KR-cost triggers (tam 0 + tac, v7 DB call 138): spend `amount` KR of the player's
    // account; 1 = spent. NULL (offline, no kn_brain wallet): KR-cost triggers are unaffordable.
    int (*spend_kr)(struct Game_t*, const Player*, int amount);
    // WP6 (KrunkScript persistence & services): the account backends of map triggers, NULL = none
    // (the actions do nothing: decisions.md "Backend-only triggers"). The server routes them to
    // kn_brain or the offline sandbox's file store (server/src/ks_storage.c).
    // Save Data (49): storageUpdt(account, null, type, "private", raw "key,value") of the player.
    void (*storage_save)(struct Game_t*, const Player*, const char *key, int type, const char *value);
    // Load Data (57): getStoragePlayer(account, null); the rows arrive later through
    // trigger_load_data_apply.
    void (*storage_load)(struct Game_t*, const Player*);
    // Reward KR (32): game.rewardKR(player, amount) (the callee checks game.krRewards).
    void (*reward_kr)(struct Game_t*, const Player*, int amount);
    // player-viewer WP-E part 2c: v7 game.incStat (stat = 1: key is a progression.h stat key,
    // value 1) and challenges.update (stat = 0: a challenge key, its value, the weapon and streak
    // context, -1 none; item: the {item} context of "spry" / "find", -1 none) in public games. The
    // server tallies them per account for the stats commit (server/src/accounts.c); NULL =
    // nothing (offline, guests).
    void (*progress)(struct Game_t*, const Player*, int stat, const char *key, int value, int weapon, int streak, int item);
    // season-system: v7 game.giveSkin(player, skin, reason) -> sendUserSkin (the account backend
    // gives the skin; the server then tallies "find" {item} and sends NET_PACKET_SKIN_FOUND).
    // NULL = nothing (offline, servers without rewards).
    void (*give_skin)(struct Game_t*, const Player*, int skin, const char *reason);
    // v10 Turf Wars (territorialWars.onPlayerDeath): a player died, killer NULL / the victim for
    // a suicide. The server tallies clan kills (server/src/accounts.c); NULL = nothing.
    void (*kill)(struct Game_t*, const Player *victim, const Player *killer);
    // v10 players.spray's canUseSkin(index, store.skins, player.account, ...): 1 when the player's
    // account may use store item `skin` (the server: the coordinator's inventory, accounts.c).
    // NULL = no account (guests, the client mirror): only free items (shared/src/social_v7.c).
    int (*can_use_skin)(const struct Game_t*, const Player*, int skin);
    // Bot chat (shared/include/bot_chat.h): a bot wants an AI-written chat line. The server asks
    // kn_brain (`bot-banter`, server/src/bot_banter.c) and the answer comes back through
    // bot_chat_deliver. 1 = asked (an answer or a timeout follows), 0 = not available (offline,
    // no brain, a guests-only lobby): the bot's template line goes out instead. NULL = 0.
    int (*bot_banter)(struct Game_t*, const struct BotChatRequest_t*);
    // Vote kick (shared.h VoteKick): game.update's timer ran out before the votes came in. The
    // server (server/src/votekick.c) tells every client the vote failed; the game frees it right
    // after. NULL = nothing.
    void (*vote_kick_expired)(struct Game_t*);
} GameHooks;

// Server projectiles (Krunker v3.1.1 src/data/projectile.js). Units per second
// (Krunker spd is units/ms); `range` counts down with the distance flown.
#define GAME_MAX_PROJECTILES 64

// ProjectileConfig: shared/include/weapons.h (every Weapon carries its v7 pj_* block).

typedef struct {
    int id;             // Krunker m.sid: slot index, stable while active
    int owner_uid;      // -1 none
    int weapon_index;
    unsigned char active:1;
    unsigned char skip_move:1;
    unsigned char thrown_melee:1; // Krunker pj kill info (Bullseye)
    vec3 position;
    vec3 velocity;      // units/ms
    float speed;
    float yaw_dir;      // xD (yaw - PI) and yD (pitch) of the flight
    float pitch_dir;
    float range;
    int ai_kind;        // projectiles.stored index of an AI shot (owner -1, "orb_0"), 0 = a player's weapon
    float state_const;  // stateConst: the owner's getStateConst at launch (ms), 0 dontPred / AI
    ProjectileConfig config;
    int effect;         // WP-B effectInd: EffectId given on a hit, -1 none (Zapper)
    int effect_count;   // effectC: stacks (floor(charge * 10))
} GameProjectile;

// WP-B: one shot's projectiles.init options (v7.1.4 players.js shoot / melee): the charge
// scales speed (knife throw: v = max(0.3, d)) and damage (ceil(dmg * d) * dmgMlt).
typedef struct {
    float speed_mlt;    // v (0 = 1)
    float damage_d;     // d: ceil(dmg * d) (0 = 1)
    float damage_mlt;   // dmgMlt (0 = 1)
    int effect;         // effectInd, -1 none
    int effect_count;   // effectC
} ProjectileShot;

// Krunker v3.1.1 match constants (src/config.js) in seconds.
#define GAME_END_TIMER 25.0f        // config.endTimer (end screen / map vote)
#define GAME_DEATH_DELAY 2.8f       // config.deathDelay (death card before the menu returns)
#define GAME_ASSIST_TIME 5.0f       // config.assistTime
#define GAME_ASSIST_SCORE 25        // config.assistScore
#define GAME_ASSIST_MIN 20.0f       // config.assistMin
#define GAME_FEED_TIMER 2.0f        // config.feedTimer (multi-kill window)
#define GAME_REGEN_DELAY 5.0f       // config.regenDelay
#define GAME_REGEN_TICK 0.5f        // players.tickPlayer ticker
#define GAME_SCORE_CAP 13000        // players.score: public games cap the score
#define GAME_LEADERBOARD_MAX 16     // players.syncLeaders: 16 rows

// ---- map-logic WP-B: v7.1.4 world runtime state of a game (shared/include/world.h)
// The battle-royale zone (libs/Zone.js). The authority runs the circle; clients tween to each
// NET_PACKET_ZONE over 900 ms (zone_circle() gives the value to draw).
typedef struct {
    unsigned char active:1;      // map.zone exists (zone && zoneSize && zoneSpeed)
    unsigned char waiting:1;     // a stage wait ("Zone closing in ") is running
    unsigned char visible:1;     // client: hidden once a received size reaches 0
    unsigned char synced:1;      // client: a NET_PACKET_ZONE arrived this round
    float size, height;          // _size / _height (limits applied)
    float shrink_speed;          // circleShrinkSpeed (units/s)
    float wait_time;             // waitTimer (ms) of one stage, _waitTimer
    int stages_start;            // _stages
    int stages;                  // stages left
    float wait_timer;            // ms left of the running stage wait
    float broadcast_timer;       // zoneTimer (ms) until the next "zn" + outside damage
    vec3 position, scale;        // circlePosition / circleSize (x, z used; scale.y = height)
    vec3 target_position, target_scale; // targetCirclePosition / targetCircleSize
    // client tween (Zone.update with a value: TWEEN 900 ms linear from the current values)
    float tween_from[4], tween_to[4], tween_time;
} WorldZone;

typedef struct {
    int object;                  // map object index (uid)
    int node;                    // PATH_NODE nid it was moved to
    vec3 origin;                 // orgX / orgY / orgZ: the object's position before the first move
} WorldMovedObject;

typedef struct {
    WorldZone zone;
    WorldMovedObject *moved;     // manager.movedObjs (uid, node pairs), in move order
    size_t moved_count, moved_cap;
    int build_count;             // game.buildCount (config.buildMax 1200)
    size_t static_object_count;  // map objects before any build (builds are appended after them)
    unsigned char save_colls:1;  // game.saveColls: some object listens to onEnterObject / onExitObject
    unsigned char predicted_builds:1; // client: some build may be predicted (Object.predicted_ms, build.c)
} GameWorld;
// ---- end WP-B

// ---- map-logic WP-H: zombies machines, power-ups, waves and airdrops (shared/include/zombies.h)
#define ZOMBIES_MAX_MACHINES 16  // mystery boxes / krum machines / perk machines per map
#define ZOMBIES_MAX_PENDING 64   // zWaveData.tmpCnts (wave caps 30 + 10 + 5 + 4)
#define ZOMBIES_POWERUPS 3       // instaKill, fireSale, doublePoints ("pwup" index)
#define AIRDROP_MAX 16
#define ZOMBIES_NONE (-2)        // pendingWeapon / purchasedBy undefined

typedef struct {
    int uid;                     // map object index
    int state;                   // 0 idle, 1 rolling (mystery box) / upgrading (krum), 2 ready to collect
    float timer;                 // ms left of the state
    int pending_weapon;          // pendingWeapon: weapon id, -1 the teddy (mystery box), ZOMBIES_NONE
    int purchased_by;            // purchasedBy: player uid, -1 anyone (a shared box), ZOMBIES_NONE
} ZombieMachine;

typedef struct {
    int sid;                     // slot + 1
    int owner_uid;               // the caller, -1 none
    vec3 position;               // x, z; y falls from the call height + 500
    float fall;                  // fallDist left
    int index;                   // airDropPackages index
    int imitate;                 // a trapped package's look for everyone but the owner, -1 none
    float claim_time;            // ms of interact hold
    unsigned char active:1;
    unsigned char can_claim:1;   // landed
} AirDrop;

typedef struct {
    float powerups[ZOMBIES_POWERUPS]; // instaKillTimer / fireSaleTimer / doublePointsTimer (ms)
    ZombieMachine boxes[ZOMBIES_MAX_MACHINES]; // map.manager.mysteryBoxes (Object.active is the box's active)
    int box_count;
    ZombieMachine krums[ZOMBIES_MAX_MACHINES]; // map.manager.krumMachines
    int krum_count;
    int perk_machines[ZOMBIES_MAX_MACHINES];   // PERK_MACHINE object indexes
    unsigned char perk_free_taken[ZOMBIES_MAX_MACHINES]; // gotFreeP: the crouch +50 was taken
    int perk_machine_count;
    // zom.js (authority)
    int wave;                    // zWave (clients: the last "upz")
    float spawn_timer;           // zombieSpawnTim (ms)
    unsigned char skip_check;    // skipZCheck
    int conc;                    // zWaveData.concZomb
    int counts[4];               // zWaveData.zombCnts: Walker, Brute, Flesh Colossal, Runner
    unsigned char pending[ZOMBIES_MAX_PENDING]; // tmpCnts: template per zombie left to spawn
    int pending_count;
    int left;                    // zLeft
    float hp_mlt, dmg_mlt, spd_mlt;
    int frog_round, drone_round; // zFrogSpawnRound / zDroneSpawnRound
    unsigned char pap_active, pap_completed; // eePAPActive / eePAPCompleted (the Zombie Portal easter egg)
    int pap_order[3];            // eePAPOrder: template indexes (Walker, Brute, Runner shuffled)
    int pap_progress;
    float dance_timer;           // eeDanceTimer (ms)
    unsigned char dance_completed;
    int dance_skulls;
    char special[24];            // the last "upz" special wave name (clients)
    // airDrop.js
    AirDrop drops[AIRDROP_MAX];
    int drop_count;              // slots used
} GameZombies;
// ---- end WP-H

// KrunkNative Piss (shared/include/piss.h, shared/src/piss.c): the puddles of the stream.
#define GAME_PUDDLES_MAX 64
typedef struct {
    int id;         // SPAWNED_OBJ id (OM_KIND_PUDDLE)
    int owner;      // the owner's uid
    int weapon_id;  // the kill feed's weapon
    vec3 position;  // centre on the ground (+ PISS_PUDDLE_LIFT)
    float radius;
    float dps;      // authority
    float life;     // ms left (authority)
    float age;      // ms since it appeared (client fade in)
    int color;      // 0xRRGGBB
} GamePuddle;

typedef struct {
    GamePuddle items[GAME_PUDDLES_MAX]; // oldest first
    int count;
    int next_id;
    float tick;        // ms since the last damage tick (authority)
    float tick_period; // the latest puddle's weapon puddle_tick (ms)
} GamePuddles;

struct Ruleset_t;

typedef struct Game_t {
    GameConfig config;
    // 1 in a ranked match (server/src/accounts.c acc_ranked_setup; v10 game.isRanked): the
    // ruleset's ranked rules apply (ruleset.h Ruleset.ranked, game_is_ranked).
    unsigned char is_ranked;
    const struct Ruleset_t *rules; // the ruleset game_configure took the tables from (ruleset.h); NULL before
    GameMode *mode;
    Map *map;

    Weapon **weapons;
    ClassConfig *classes;
    const cJSON **maps;
    int *modes;

    size_t map_count;
    size_t mode_count;
    size_t class_count;
    size_t weapon_count;

    int current_map_index;
    int current_mode_index;

    size_t player_count;
    Player **players;

    unsigned char move_lock:1;
    unsigned char is_local:1;
    unsigned char ready:1;
    unsigned char is_sandbox:1;   // WP-E: v7 game.isSandbox (the offline sandbox server, server/src/sandbox.c): spray wheel, maxSpraysF
    // 1 while this client plays an OFFICIAL krunker.io game (client/src/net/krunker_net.c sets it on join and
    // clears it on leave): KrunkNative's own classes are not playable there (game_class_allowed). Never set
    // on a server, in the offline sandbox or in a KrunkNative lobby.
    unsigned char is_official:1;

    float end_timer;              // seconds of end screen left (> 0 = round over)

    // Round state (authority): Krunker game.gameTimer / lastTimer / kills.
    float round_timer;            // seconds left in the round
    float round_length;           // > 0: round length in seconds instead of config.game_time (server --round-seconds, CLI `round`)
    int round_seconds_sent;       // last whole second broadcast (TIMER packet), -1 none
    int kill_count;               // Krunker game.kills (First Blood)
    float now;                    // game time of the current tick (seconds)
    unsigned char round_endless:1;// /endless: Krunker endTimer = "inf", the round clock is paused at round_timer (game_set_endless)
    unsigned char leaders_dirty:1;// Krunker leadersUpdated: LEADERBOARD due this tick
    // The in-process offline client's game (client/src/net/local_game.c): game_end_round()
    // keeps the players active behind the end screen (v3.1.1's client plays the endAnim slow
    // motion with them) instead of players.clear(); only their input queues are dropped.
    unsigned char end_keep_players:1;
    // Lag compensation (players.js storeState / fetchState): the server turns it on (server.cfg
    // lag_comp, lag_comp_anchor). Off in the offline local game, which has no network delay and
    // shows its players without interpolation, so the current state is what the shooter saw.
    unsigned char lag_comp:1;
    unsigned char lag_comp_anchor:2; // LAG_COMP_ANCHOR_*: getStateConst from the shot input's time (player.c)
    float lag_comp_scale;         // server.cfg lag_comp_scale: multiplies every player's lagComp (0 = 1)
    double state_now;             // Krunker game.now for stateHistory (s); double: `now` is a float
                                  // that loses millisecond precision after a day of server uptime
    GameHooks hooks;
    // WP-A (shared/include/triggers_v7.h): the KrunkScript hooks of the trigger runtime, NULL none
    const struct TriggerScriptHooks *script_hooks;
    struct KsGame *ks;            // KrunkScript instance of this Game (shared/include/ks_game.h), NULL none
    GameProjectile projectiles[GAME_MAX_PROJECTILES];
    struct GameAIs_t *ai;         // Krunker game.AI (shared/include/ai.h): NULL until the first AI
    struct GameBots_t *bots;      // Krunker v7.1.4 game.bots (shared/include/bots.h): NULL until the first bot
    struct BotChat_t *bot_chat;   // KrunkNative bot chat (shared/include/bot_chat.h): NULL until the first use

    const EffectConfig *effects;  // WP-B: per-game effects.js table (custom-game prks overrides), NULL = g_effects
    // WP-E (shared/src/game_config.c): host request, weps / prks override copies, match votes,
    // network rates. Created on demand, freed by game_config_free(); NULL = public defaults.
    struct GameConfigState *config_state;
    float nuke_timer;
    Player *nuke_player;

    VoteKick *vote_kick;

    void *server; // TODO

    GameWorld world; // map-logic WP-B (shared/include/world.h): zone, moved objects, builds
    // ---- WP-C: the v7.1.4 trigger runtime (shared/src/triggers_v7.c): globalVals, triggerQueue,
    // trigger chains, per-object custVals, playerSounds, pickups. Allocated on first use, NULL none.
    struct TriggerRuntime_t *trigger_rt;
    // v7.1.4 game.lobbyPlayers of a comp lobby (shared/include/comp_lobby.h), NULL until used
    struct CompLobby_t *comp_lobby;
    GameZombies zombies; // map-logic WP-H (shared/include/zombies.h): machines, power-ups, waves, airdrops
    // KrunkNative Object Manager (shared/include/object_manager.h): spawned object records and the
    // per-player spawn lists. Allocated on first use, NULL none.
    struct ObjectManager_t *object_manager;
    GamePuddles puddles; // KrunkNative Piss (shared/include/piss.h): live puddles (authority; clients a copy)
} Game;

void game_configure(Game*, const GameConfig*, const cJSON**, size_t, int*, size_t, Weapon**, size_t, ClassConfig*, size_t);
void game_init(Game*, int, int, unsigned char);
void game_tick(Game*, float, float);
// game_tick with the world (player simulation) on its own step: the in-process offline
// client slows its world for the endAnim while the round/end clocks keep real time.
void game_tick_world(Game*, float now, float delta, float world_delta);
void game_players_add(Game*, Player*);
// game.js getSpawnPoint(player.team, player): the point a (re)spawning player is placed at
// (shared/src/modes/mode_round.c). 0: the map has no spawns.
int game_get_spawn_point(Game*, const Player*, vec3 *position, int *direction);
// KrunkNative: 1 when a player added now (the team players.add would give it) gets a spawn point
// nobody stands on (game_get_spawn_point's first tier), so the bot fill never stacks bodies.
int game_spawn_point_free(const Game*);
void game_broadcast();
int game_is_authority(const Game*);      // server or local game: runs damage/kills/score
// io_ent spawn rules: a ready round, not the end screen, not alive, config.deathDelay since
// the last death, lives left (`player` NULL: first spawn). Server and offline client alike.
int game_can_spawn(const Game*, const Player*);
int game_end_late_spawn(const Game*, const Player*); // ENT while the round is over (vote window): spawn anyway
void game_send_end_late(Game*, int uid);              // then the "end" with didWin -1 for that uid
void game_player_changing(const Game*, Player*, int done); // GameHooks.player_changing, when set
void game_send(Game*, int uid, int type, const void*, uint16_t); // through hooks.send
// player-viewer WP-E part 2c (shared/src/game.c): v7 game.isPubType() (no host, or mimicPubs; the
// offline sandbox counts), game.incStat(key, player), challenges.update(game, key, value,
// {wep, stk}, player) through GameHooks.progress, and game.incMedal(name, player) (accounts and
// the sandbox: Player.medal_counts, icon medals only).
int game_is_pub_type(const Game*);
void game_inc_stat(Game*, const Player*, const char *key);
void game_challenge(Game*, const Player*, const char *key, int value, int weapon, int streak);
// challenges.update(game, key, value, {item}, player): "spry" (the spray), "find" (a found skin)
void game_challenge_item(Game*, const Player*, const char *key, int value, int item);
// game.giveSkin(player, skin, reason): an account's found skin (GameHooks.give_skin), public games
void game_give_skin(Game*, const Player*, int skin, const char *reason);
void game_inc_medal(Game*, Player*, const char *name);
Player *game_find_player(Game*, int uid);
void game_sync_leaders(Game*, int uid);  // LEADERBOARD to `uid` (or NET_TARGET_ALL)
void game_start_round(Game*);            // fresh round timer + stats (after game_init)
void game_end_round(Game*);              // Krunker endGame(3): END packets, players cleared
void game_set_round_time(Game*, float seconds); // debug: seconds left in the running round (TIMER goes out next tick)
// Interaction (shared/src/interact.c): v7.1.4 players.interact ("int" type, uid / "intSec"),
// confirmInteract ("cnfm"), dropWeapon ("drpw") and Player's changeInteract prompts. Terminals
// are gone in v7 (TERMINAL builds nothing); weapon pickups (WEAPON_PICKUP) are interact type 6.
#define GAME_INTERACT_TIMER 0.1f       // config.interactTimer (s)
#define GAME_SPRAY_TIMER 1.0f          // config.sprayTimer (s)
#define GAME_SPRAY_RANGE 25.0f         // config.sprayRange
#define GAME_SPRAY_SCALE 15.0f         // config.sprayScale: the decal's width and height
// v3-shaped "int": 0 spray (game_player_spray), 1 interact (the server resolves the target from
// its own prompt scan, game_interact_scan), 2 drop weapon. Authority only.
void game_player_interact(Game*, Player*, int action, int secondary);
// v7 "int" type, uid (lastInteractData) / "intSec" type, uid (secondary): authority only.
void game_player_interact_v7(Game*, Player*, int type, int uid, int secondary);
void game_player_confirm_interact(Game*, Player*, int kind); // "cnfm": kind 0 trigger (pendingTrigger), 2 prize wheel
void game_player_drop_weapon(Game*, Player*);  // "drpw": players.dropWeapon(player) of the weapon in hand
void game_player_drop_all(Game*, Player*);     // dropWeapon(player, true): at a death / leave, picked weapons go back to their pickups
void game_player_spray(Game*, Player*); // players.spray: NET_PACKET_SPRAY_V7 / NET_PACKET_SPRAY + the "spray" sound when the view ray hits a wide enough face
// WP-E "spry"[, index] (shared/include/social_v7.h): game_spray_pick(wheel_index), then players.spray
void game_player_spray_pick(Game*, Player*, int wheel_index);
// players.js changeInteract types (the #interactMsg line and lastInteractData.type).
typedef enum {
    GAME_INTERACT_NONE = -1,
    GAME_INTERACT_GATE = 0,        // "Press [key] to <inm || Open/Close gate> [price]"
    GAME_INTERACT_BANK = 1,        // deposit box: deposit / withdraw
    GAME_INTERACT_BOMB = 3,        // bomb site plant / equip / defuse (data 1..3; modes)
    GAME_INTERACT_TRIGGER = 4,     // onHoldMouse: "Hold [M1] to <inm || Interact>" (priority 1)
    GAME_INTERACT_PICKUP = 6,      // WEAPON_PICKUP: "Press [key] to <inm || pickup NAME> [price]"
    GAME_INTERACT_KEY = 7,         // onInteractKey trigger: "Press [key] to <inm || Interact> [cost]"
    GAME_INTERACT_SPINNER = 9,     // PRIZE_WHEEL (KR store wheels: no store natively)
    GAME_INTERACT_CLAIM = 10,      // airdrop claim (hold interact)
    GAME_INTERACT_SPAWNABLE = 12,  // spawnable pickup message
    GAME_INTERACT_MYSTERY_BOX = 13,
    GAME_INTERACT_PERK = 14,
    GAME_INTERACT_KRUM = 15,
} GameInteractType;
#define GAME_INTERACT_RADIUS 7.0f      // addGate / addBank tRadius (and onInteractKey triggers)
// One tick of changeInteract calls for a player: the prompt that wins (vO422: type, object,
// priority rules, noMsg skipped) and lastInteractData (vO423: the last call whose type is not
// 3 / 10 / 11 / 12), which is what the interact key sends.
typedef struct {
    int type;        // GameInteractType of the prompt, GAME_INTERACT_NONE = no prompt
    int index;       // its map object index, -1 none
    int data;        // the prompt's data (bomb 1..3)
    int last_type;   // lastInteractData.type, GAME_INTERACT_NONE none
    int last_uid;    // lastInteractData.id (map object uid), -1 none
} GameInteractScan;
void game_interact_scan(const Game*, const Player*, GameInteractScan *out);
int game_interact_prompt(const Game*, const Player*, int *object_index); // the scan's prompt type and object index (or -1)
// WEAPON_PICKUP runtime (game.updatePickup / resetPickups): the weapon id it holds now (-1 empty),
// whether it shows, its price / method; 0 when `uid` is no pickup.
typedef struct {
    int weapon;      // pickup: weapon id, -1 none (a pickupRep pickup emptied by a swap)
    int ammo;        // pickupAmmo
    int score;       // scoreP ("sp")
    int method;      // "md" 0..7 (as gates)
    int team;        // "tm"
    unsigned char active:1;
    unsigned char replace:1;   // pickupRep (!"wr"): a pickup swaps the old weapon in
    unsigned char random:1;    // "rw": a random weapon at round reset
    unsigned char no_msg:1;    // "nm"
    int random_group;          // "rwg" 1..10000, 0 none
    vec3 position;             // x, y, z (moved by drops), bottom
    vec3 origin;               // orgX/Y/Z
} GamePickup;
int game_pickup_state(const Game*, int uid, GamePickup *out);
void game_pickup_added(Game*, int uid);              // KrunkNative Object Manager: a pickup object appended mid-round
void game_pickup_disable(Game*, int uid);            // KrunkNative Object Manager: a spawned pickup removed
void game_pickup_send(Game*, int target, int uid);   // NET_PACKET_PICKUP of one pickup
void game_pickup_set(Game*, int uid, int weapon, int active, const vec3 *position, int ammo); // client mirror of "inat" pickups

// Map triggers, interfaces, check points, teleporters (shared/src/triggers.c).
typedef enum {
    GAME_HIT_SHOT,          // hitscan: onShoot / onDamage
    GAME_HIT_MELEE,         // melee swing: onMelee / onDamage / onPunch (knife thrown)
    GAME_HIT_PROJECTILE,    // projectile: onShoot / onDamage / onProjectile (+ onThrowingMelee for the knife)
} GameHitKind;
struct GameAI_t;
// game.onTrigger(player, obj, chain) (v7.1.4, shared/src/triggers_v7.c, authority): `chain` points to the
// chain key of the calling action (triggerChains[key]); NULL starts a chain keyed by the object's uid.
void game_on_trigger(Game*, Player *source, Object *object, int *chain);
void game_trigger_ai_death(Game*, Player *source, struct GameAI_t *ai); // AI.kill: onAIDeath -> game.onTrigger(source, ai)
int  game_object_event(const Object*);                  // the object's v7 TriggerEvent 0..31 (TRIGGER_EVENT_NONE without one)
void game_object_hit(Game*, Object*, Player *dealer, float damage, int thrown_melee, int kind); // a shot / swing / projectile reached a map object
void game_destroy_object(Game*, Object*, Player *by);   // health 0: active = false, destObjs, "do"
void game_respawn_object(Game*, Object*, int *chain);   // "ro": active again, onRespawn
void game_set_gate(Game*, Object*, int active);         // "gte"
void game_set_checkpoint(Game*, Player*, Object*);      // setCheckPoint ("chp" from the authority)
void game_check_teleport(Game*, Player*, Object*);      // checkTeleport (client prediction and authority)
// players.changePosition(player, x, y, z, recon, stopMomentum, dir): recon sends NET_PACKET_POSITION
void game_change_position(Game*, Player*, vec3 feet, int recon, int stop_momentum, int has_direction, int direction);
void game_tick_map_objects(Game*, float delta_ms);      // teleporter cooldowns; authority: onTimer triggers, destructible respawns
void game_send_object_state(Game*, int uid);            // init dest / gates / banks for a joiner
int  game_spawn_at_checkpoint(Game*, Player*);          // respawn at the player's check point (authority); 1 = moved
// game.playSound -> "s" to every connection: `uid` >= 0 plays at that player (playerSound
// sid), else at `position`. Names: config.emitterSounds / networkSounds, "asset:<id>" user assets.
void game_play_sound(Game*, const char *name, int uid, vec3 position, float volume, float rate);
// game.sendUserJunk's "jnk" reply (io_foundJunk): "You found [x.xx] Junk" to the finder only
// (NET_CHAT_FOUND_JUNK). KN has no junk wallet: nothing is credited (doc/v7/decisions.md).
void game_found_junk(Game*, const Player*, double amount);
void game_set_endless(Game*, int on);   // /endless: pause the round clock (TIMER keeps the frozen value) / resume it from there
void game_send_round_state(Game*, int uid); // TIMER (+ END while the end screen is up) for a joiner
void game_player_left(Game*, Player*);   // drop references (nuke, damage) to a leaving player
int game_pick_next_map(const Game*);     // next round's map: random rotation map (no map votes)
int game_mode_index_for(Game*, int mode_id); // game->modes index of a Krunker mode id (appended when missing), -1 unknown
int game_mode_id(const Game*);           // the current round's Krunker mode id, -1 none
// Projectiles (shared/src/projectile.c), authority only.
const ProjectileConfig *projectile_config_for(const Weapon*, int thrown_melee); // NULL: the weapon fires no projectile
void game_spawn_projectile(Game*, Player *owner, int weapon_index, const ProjectileConfig*, vec3 origin, float yaw, float pitch, int thrown_melee);
// WP-B: game_spawn_projectile with the v7 shot options (NULL = none).
void game_spawn_projectile_ex(Game*, Player *owner, int weapon_index, const ProjectileConfig*, vec3 origin, float yaw, float pitch,
                              int thrown_melee, const ProjectileShot *shot);
void game_tick_projectiles(Game*, float delta);
void game_explosion(Game*, vec3 position, float radius, float damage, Player *owner, int weapon_index);

#define NET_DEFAULT_PORT 21015
#define NET_MAX_PAYLOAD 512
#define NET_PACKET_HEADER_SIZE 3
#define NET_BUFFER_SIZE 4096

typedef enum {
    NET_PACKET_HELLO = 1,
    NET_PACKET_INIT = 2,
    NET_PACKET_ENT = 3,
    NET_PACKET_SPAWN = 4,
    NET_PACKET_INPUT = 5,
    NET_PACKET_STATE = 6,
    NET_PACKET_CYCLE = 7,
    NET_PACKET_CHAT_SEND = 8,    // C->S: NetChatSendPacket (Krunker "ct")
    NET_PACKET_CHAT = 9,         // S->C: NetChatPacket (Krunker "ch")
    NET_PACKET_KILL = 10,        // S->C: NetKillPacket (Krunker "3")
    NET_PACKET_HIT = 11,         // S->C to the dealer: NetHitPacket (Krunker "4")
    NET_PACKET_HEALTH = 12,      // S->C: NetHealthPacket (Krunker "h")
    NET_PACKET_SCORE = 13,       // S->C to the player: NetScorePacket (Krunker "5")
    NET_PACKET_LEADERBOARD = 14, // S->C: NetLeaderboardPacket (Krunker "7")
    NET_PACKET_TIMER = 15,       // S->C: NetTimerPacket (Krunker "t")
    NET_PACKET_END = 16,         // S->C: NetEndPacket (Krunker "end")
    NET_PACKET_PING = 17,        // S->C: NetPingPacket (Krunker "pi")
    NET_PACKET_PONG = 18,        // C->S: NetPingPacket echo (Krunker "po")
    NET_PACKET_PING_RESULT = 19, // S->C: NetPingResultPacket (Krunker "l" ping field)
    NET_PACKET_LEAVE = 20,       // S->C: NetLeavePacket, the player disconnected (Krunker "2")
    NET_PACKET_SHOT = 21,        // S->C to the others: NetShotPacket, one hitscan ray (Krunker "9")
    NET_PACKET_PROJECTILE = 22,  // S->C: NetProjectilePacket, a projectile was fired (Krunker "pr")
    NET_PACKET_PROJECTILE_END = 23, // S->C: NetProjectileEndPacket (Krunker "pre")
    NET_PACKET_EXPLOSION = 24,   // S->C: NetExplosionPacket (Krunker "ex")
    NET_PACKET_MAP_INFO = 25,    // S->C before every INIT: NetMapInfoPacket, which map INIT.map_index means
    NET_PACKET_MAP_REQUEST = 26, // C->S: NetMapRequestPacket, the client lacks that map
    NET_PACKET_MAP_DATA = 27,    // S->C: NetMapDataPacket chunks of the map JSON (Krunker sends custom maps in "init")
    NET_PACKET_SPECTATE = 28,    // S->C: NetSpectatePacket, an ENT spectate request was accepted (Krunker "start" spectating arg)
    NET_PACKET_SPECTATE_TARGET = 29, // C->S: NetSpectateTargetPacket, the player a spectator follows (Krunker "fpSp")
    NET_PACKET_ERROR = 30,       // S->C right before the server closes the connection: NetErrorPacket (Krunker "error")
    NET_PACKET_VOICE_SEND = 31,  // C->S: NetSocialChunk pieces of one voice clip, a data URL (Krunker "vc", shared/include/voice.h)
    NET_PACKET_VOICE = 32,       // S->C: NetSocialChunk pieces of one relayed clip + alias + sid (Krunker "vc")
    NET_PACKET_BILLBOARD = 33,   // S->C before every INIT: NetBillboardPacket (Krunker init "bill" -> game.updateBillboards)
    NET_PACKET_PLAYER_INFO = 34, // S->C after SPAWN: NetPlayerInfoPacket, a player's account (level, clan, flags) for HUD/nametags
    NET_PACKET_SKINS = 35,       // S->C right before every SPAWN: NetSkinsPacket, the player's weapon / melee skins (Krunker spawn skins, meleeIndex)
    NET_PACKET_BILLBOARD_IMAGE = 36, // S->C before every INIT, after BILLBOARD: NetBillboardImagePacket (server.cfg billboard_image)
    // Game mode packets (shared/src/modes, Krunker v3.1.1 modes.js), numbered from 40 so the
    // core protocol above can grow without colliding.
    NET_PACKET_MODE_STATE = 40,  // S->C: NetModeStatePacket, full mode snapshot (round start, joiners)
    NET_PACKET_GAME_MESSAGE = 41,// S->C: NetGameMessagePacket (Krunker "gmsg")
    NET_PACKET_TEAM = 42,        // S->C: NetTeamPacket, a player's team / role (Krunker "tm" + its "h")
    NET_PACKET_TEAM_SCORE = 43,  // S->C: NetTeamScorePacket (Krunker "ts")
    NET_PACKET_FLAG = 44,        // S->C: NetFlagPacket (Krunker "ufl" / "ufp" / "ufi")
    NET_PACKET_OBJECTIVE = 45,   // S->C: NetObjectivePacket (Krunker "obj" / "wep")
    NET_PACKET_LIVES = 46,       // S->C to the player: NetLivesPacket (Krunker "lv")
    NET_PACKET_ACTION = 47,      // S->C: NetActionPacket, an action feed line (Krunker "ac")
    NET_PACKET_LOADOUT = 48,     // S->C: NetLoadoutPacket, a mode changed a loadout (Krunker "inat" / "uamo")
    NET_PACKET_LOCK = 49,        // S->C: NetLockPacket (Krunker "lock" / "lm" / "ulm")
    NET_PACKET_SPAWNABLE = 50,   // S->C: NetSpawnablePacket (Krunker "crsp" / "dsp")
    NET_PACKET_MODE_END = 51,    // S->C after every END: NetModeEndPacket (endData.tms, winner team, mode stats)
    NET_PACKET_MEDAL = 52,       // S->C to the player: NetMedalPacket (Krunker "am")
    NET_PACKET_REFILL = 53,      // S->C to the player, no payload: the thrown knife is back (Krunker "rfl" -> refillKnife)
    NET_PACKET_KILL_MEDALS = 54, // S->C to the killer: NetKillMedalsPacket, kills.js reward list (Krunker "6")
    NET_PACKET_ASSIST = 55,      // S->C to the assister, no payload: the "Assist" medal (Krunker "10")
    NET_PACKET_KILL_STREAK = 56, // S->C to all: NetKillStreakPacket, a 5/10/15.. kill streak chat line (Krunker "kst")
    // Chat command effects (shared/src/chat_commands.c), numbered from 60.
    NET_PACKET_CHEAT = 60,       // S->C: NetCheatPacket (Krunker "noc" / "unl" / "fh" to the target, "borders" / "forcecol" to all)
    // Interaction (shared/src/interact.c)
    NET_PACKET_INTERACT = 61,    // C->S: NetInteractPacket (v3 "int" action [, secondary]) or NetInteractV7Packet (v7 "int" type, uid)
    // 62 / 63: the v3.1.1 terminal packets ("ot" / "tmc"); v7.1.4 has no terminals (retired, WP-C)
    NET_PACKET_SPRAY = 64,       // S->C to all: NetSprayPacket (Krunker "sp": players.spray -> addSpray)
    // Social hub (shared/include/social.h; Krunker's /api/ws socket + /api/* routes), numbered from 70.
    NET_PACKET_SOCIAL_REQUEST = 70, // C->S: NetSocialChunk pieces of one JSON request frame
    NET_PACKET_SOCIAL_REPLY = 71,   // S->C: NetSocialChunk pieces of one JSON array of reply frames
    // Backend (doc/backend.md "Game server"): who is joining, sent by the client right after HELLO.
    NET_PACKET_JOIN = 72,           // C->S: NetJoinPacket (brain seek-game key and/or session token; none = guest)
    NET_PACKET_AUTH_CHALLENGE = 73, // S->C right after HELLO: NetAuthChallengePacket (online_mode join, shared/include/auth_join.h)
    NET_PACKET_REDIRECT = 74,       // S->C: NetRedirectPacket, move to another server (doc/backend.md 8.10)
    // The v3.1.1 offline build's developer-tool packets (mod/mods.ts keys, server/packetHandlers.ts), from 80.
    NET_PACKET_TELEPORT = 80,      // C->S: NetTeleportPacket (Krunker "tp" x, y, z: Ctrl TP's camera-ray hit, y + player height)
    NET_PACKET_TELEPORT_ACK = 81,  // S->C to the sender: NetTeleportPacket, the feet position io_tp moved it to (its "l" ack)
    NET_PACKET_NOTIFY = 82,        // S->C: NetNotifyPacket (Krunker "notify": the mod's notify() toast, e.g. "DEV only")
    NET_PACKET_CLEAR_LOG = 83,     // C->S, no payload (Krunker "clearLog", the mod's F1: the server clears its terminal)
    NET_PACKET_PREMIUM_ACTION = 84, // C->S: uint8 index 0..3 (Krunker "prmA": premiumKeys noclip / god / kill / smite, requirePremiumCheatPerm)
    NET_PACKET_FORWARDED = 85,     // C->S, first packet of a net-bridge connection only: NUL-terminated real peer address (NET_FORWARDED_ADDRESS_SIZE)
    // AI (shared/src/ai.c, shared/include/ai.h), numbered from 90.
    NET_PACKET_AI_ADD = 90,        // S->C: NetAIAddPacket (Krunker "aai": AI.cSpawn)
    NET_PACKET_AI_SYNC = 91,       // S->C: NetAISyncPacket (Krunker "ai": AI.syncAll)
    // Map triggers, interfaces, check points (shared/src/triggers.c), numbered from 100.
    NET_PACKET_OBJECT_STATE = 100, // S->C: NetObjectStatePacket (Krunker "do" / "ro" / "gte" / "bnk", init dest/gates/banks)
    NET_PACKET_CHECKPOINT = 101,   // S->C to the player, no payload (Krunker "chp" -> checkpointSet: "Checkpoint!")
    NET_PACKET_SOUND = 102,        // S->C: NetSoundPacket (Krunker "s" from game.playSound: Broadcast Sound)
    NET_PACKET_JUMP_SCARE = 103,   // S->C to the player: NetJumpScarePacket (Krunker "scr" -> jumpScare)
    NET_PACKET_POSITION = 104,     // S->C to the player: NetPositionPacket (players.changePosition with doRecon)
    NET_PACKET_POPUP = 105,        // S->C: NetPopupPacket (Krunker "am" [text, null] of Broadcast / Send Popup: 70 chars)
    // Client prediction / server reconciliation (net-recon), numbered from 110.
    NET_PACKET_RECON = 110,        // S->C to the player: NetReconPacket (Krunker "l" [lPacket, true]: reconcilePlayer)
    NET_PACKET_INPUT_BATCH = 111,  // C->S: NetInputBatchPacket, the frames of one clientSendRate window (Krunker "q")
} NetPacketType;

#define NET_MAP_MAX_SIZE (4u * 1024u * 1024u) // largest map JSON streamed to clients
#define NET_MAP_CHUNK_SIZE 496                // NetMapDataPacket payload bytes per frame

// Chat (Krunker v3.1.1: #chatInput maxlength=70, server burst limit 3 messages / 1 s).
#define NET_CHAT_MAX_CHARS 70       // code points kept from a message
#define NET_CHAT_TEXT_SIZE 288      // bytes incl. NUL: 70 code points of UTF-8 always fit
#define NET_CHAT_BURST_WINDOW 1.0f  // seconds
#define NET_CHAT_BURST_MAX 3        // messages per window; more closes the connection (4008)
#define NET_PING_INTERVAL 5.0f      // seconds between ping probes (io_po setTimeout 5e3)
#define NET_RECON_SEND_HZ 1         // v3.1.1 config.syncFrequency: the "l" reconciliation rate
#define NET_RECON_SEQ_ALL INT32_MAX // "l" sn of a server-driven snap (MAX_SAFE_INTEGER): drop every pending input
#define NET_INPUT_SEND_INTERVAL (1.0f / 30.0f) // v3.1.1 config.clientSendRate (1000 / 30 ms): one "q" per window
#define NET_INPUT_BATCH_MAX 20      // frames per NetInputBatchPacket (a 1000+ fps window flushes early)
#define NET_RECON_PENDING_MAX 2000  // reconcilePlayer: tmpInpts.length >= 2000 drops pending inputs
#define NET_STATE_SEND_HZ 10        // v3.1.1 gameserver.ts K_PACKET_FREQUENCY ("k" sendRateA); not applied yet: STATE still goes out every tick
#define NET_PING_EWMA_ALPHA 0.35f   // packetHandlers.ts PING_EWMA_ALPHA
#define NET_PING_MAX_MS 5000        // PING_MAX_REASONABLE_MS

typedef enum {
    NET_CHAT_PLAYER = 0,  // "<name>: <text>"
    NET_CHAT_GAME = 1,    // plain game line (gB type 1)
    NET_CHAT_SERVER = 2,  // server message, #fc03ec (gB type 2)
    // v7 "jnk" -> io_foundJunk: the finder's "You found [x.xx] Junk" (text: that plain line; the
    // client colours "You" and the amount). Older clients show it as a plain game line.
    NET_CHAT_FOUND_JUNK = 3,
    // KrunkNative: a ranked queue's notice (v10 server.message.queued), in the ranked UI's green
    // (#5ce05a, the client's RANKED_CHAT_COLOR). Older clients show it as a plain game line.
    NET_CHAT_RANKED = 4,
} NetChatKind;

enum {
    NET_INPUT_SHOOT = 1 << 0,
    NET_INPUT_SCOPE = 1 << 1,
    NET_INPUT_JUMP = 1 << 2,
    NET_INPUT_CROUCH = 1 << 3,
    NET_INPUT_RELOAD = 1 << 4,
    // map-logic WP-B: input[13] reset (1 resetToStart, 2 resetToLast) and input[14] interact held
    NET_INPUT_RESET_START = 1 << 5,
    NET_INPUT_RESET_LAST = 1 << 6,
    NET_INPUT_INTERACT = 1 << 7,
};

#pragma pack(push, 1)
typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    // low nibble: the Krunker versions this client plays (NET_HELLO_RULESETS, v10.0.2 only; kept for
    // wire compatibility with the version toggle's servers). High nibble: the client's product
    // flavor (NET_WIRE_FLAVOR; 0 = base / a HELLO without it). The server refuses a peer whose
    // flavor is non-base and differs from its own; a base / absent nibble is accepted.
    uint8_t rulesets;
} NetHelloPacket;

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    int32_t map_index;
    int32_t mode_index;
    uint16_t tick_rate;
    int32_t client_id;
    // low nibble: NET_INIT_RULESET (v10.0.2), kept for wire compatibility with the version toggle's
    // clients. High nibble: the server's product flavor (NET_WIRE_FLAVOR; 0 = base). Optional: a
    // 17-byte INIT (a server from before the toggle) has no byte here; clients prefix-read it as 0
    // (base). A client whose flavor differs from this adopted server flavor refuses the server.
    uint8_t ruleset;
} NetInitPacket;

#define NET_INIT_PACKET_V1_SIZE (offsetof(NetInitPacket, ruleset)) // 17 bytes: before the ruleset
// The version toggle's wire ids (doc/v7/decisions.md "Krunker v10.0.2 is the only version"): it
// numbered v3.1.1 0, v7.1.4 1, v10.0.2 2. Only v10.0.2 is sent now: HELLO bit 2, INIT 2 + 1.
#define NET_HELLO_RULESETS (1u << 2)
#define NET_INIT_RULESET 3
// Krunker Strike: the product flavor (shared/include/flavor.h) rides beside the ruleset id, in the
// high nibble (bits 4-7) of the HELLO `rulesets` byte and the INIT `ruleset` byte; the low nibble
// stays the ruleset wire id. Flavor 0 (FLAVOR_BASE) is a zero nibble, so a base build is
// byte-identical on the wire (HELLO 0x04, INIT 3) and net-bridge / the server-list probe / an old
// peer (nibble 0) read as base. The server advertises its flavor in INIT and refuses a peer whose
// HELLO announces a non-base flavor different from its own (net_send_init / server net.c HELLO);
// the client refuses a server whose INIT advertises a different flavor (client_apply_init path).
// So every cross-flavor pair is turned away by whichever side is non-base, never silently mixed.
#define NET_FLAVOR_WIRE_SHIFT 4
#define NET_FLAVOR_WIRE_MASK 0xF0u
#define NET_WIRE_FLAVOR(byte) (((unsigned) (byte) & NET_FLAVOR_WIRE_MASK) >> NET_FLAVOR_WIRE_SHIFT)
#define NET_WIRE_RULESET_BYTE(id, flavor) \
    ((uint8_t) (((unsigned) (id) & 0x0Fu) | (((unsigned) (flavor) << NET_FLAVOR_WIRE_SHIFT) & NET_FLAVOR_WIRE_MASK)))
NetHelloPacket net_hello_make(void); // this build's HELLO: protocol major.minor.patch + NET_HELLO_RULESETS + the flavor nibble

// INIT major.minor: the protocol the server speaks. 1.0 servers drop ENTs longer than V4 (38 bytes)
// without spawning anyone, so a client only sends the longer ENT (V5 spray, V6 weapon skins) to a
// server that announces at least 1.1 (client_enter_game).
#define NET_PROTOCOL_MAJOR 1
// 1.1 servers accept ENTs up to V6 and drop V7 (1.2: + lagComp) the same way.
// 1.3: the Krunker v7.1.4 map schema (WP-A, doc/v7/map-logic.md §7.2): prefabs 0-96, no TERMINAL
// objects, so object uids differ from a 1.2 peer's on the same map. New v7 packets (blocks below)
// are only sent to peers that announced at least NET_PROTOCOL_MINOR_V7.
// 1.4: NET_PACKET_EXT (254, shared/include/net_ext.h), the u16-subtyped envelope of every packet
// added from Krunker v10.0.2 on; it goes only to peers that announced NET_PROTOCOL_MINOR_EXT. A 1.3
// peer gets everything it got before.
#define NET_PROTOCOL_MINOR 4
#define NET_PROTOCOL_MINOR_V7 3
#define NET_PROTOCOL_MINOR_EXT 4
// HELLO / INIT patch (player-viewer WP-C, doc/v7/decisions.md): 1.3.1 peers read and send the longer
// ENT V8 (attachIndex, challMode, anonMode), SPAWN V3 (attachIndex, headMlt) and RECON V2 (wallJumpC,
// canSlideT, the exact slide ms). A 1.3.0 peer checks exact sizes and drops those, so they only go
// to peers net_peer_has_wpc accepts; every reader takes the shorter and longer forms (prefix read).
// A missing patch byte (2-byte HELLO) reads as 0.
// Patch 2: NetKillPacket.fists carries NET_KILL_NUKE; servers clear it for older peers, which read
// any non-zero fists as a fists kill.
#define NET_PROTOCOL_PATCH 2
#define NET_PROTOCOL_PATCH_KILL_NUKE 2
int net_peer_has_wpc(int minor, int patch); // the peer's HELLO / INIT minor.patch (same major) takes WP-C sizes

// Packet id blocks of the v7.1.4 upgrade (doc/v7/decisions.md "Packet id blocks", map-logic.md
// §5.3). Each block is owned by one work package; ids inside a block are the owner's to assign.
#define NET_PACKET_BLOCK_TRIGGERS 120   // 120-129 map-logic WP-C: cv, gv, tgui, camb, sso, asnd/rsnd, cnfm, chgp, int v7
#define NET_PACKET_BLOCK_WORLD 130      // 130-139 map-logic WP-B: zn, bo, gt, checkpoint reset, env / terrain sync
#define NET_PACKET_BLOCK_MODES 140      // 140-149 map-logic WP-D: obj v7, uobj, uct, cct, rds, lkt, tx, krt/chrg/bh, crsp v7
#define NET_PACKET_BLOCK_ZOMBIES 150    // 150-159 map-logic zombies (§5.3 "G", plan WP-H stage 2): mybx/smybx, krm, pwup, upk, upz
#define NET_PACKET_BLOCK_AI 160         // 160-169 map-logic AI / bots (§5.3 "E", plan WP-F): rai, ai v7, aai v7
#define NET_PACKET_BLOCK_EFFECTS 170    // 170-176 player-viewer WP-B: EFFECT (pk), STREAK (wstk/jg/bbm)
#define NET_PACKET_BLOCK_AIRDROP 177    // 177-179 map-logic WP-H: AIRDROP (ad/rad/krp)
#define NET_PACKET_BLOCK_LOBBY 180      // 180-189 map-logic lobby / maps (§5.3 "A", plan WP-E): maVote, mv, networkData
#define NET_PACKET_BLOCK_COMBAT 190     // 190-194 player-viewer WP-B: other combat packets
#define NET_PACKET_BLOCK_PROTOCOL 195   // 195-199 player-viewer WP-C: protocol / input
#define NET_PACKET_BLOCK_KRUNKSCRIPT 200 // 200-239 KrunkScript (doc/v7/krunkscript.md)
#define NET_PACKET_BLOCK_SOCIAL 240     // 240-249 player-viewer WP-E: HUD, death screen, social
#define NET_PACKET_BLOCK_SANDBOX 250    // 250-253 KrunkNative Object Manager (shared/include/object_manager.h)
// 254 NET_PACKET_EXT: the v10+ envelope with a u16 subtype (shared/include/net_ext.h); new packets go there
#define NET_PACKET_BLOCK_END 255
uint16_t net_ent_size_for(int major, int minor, int patch); // the ENT a client sends to an INIT major.minor.patch server: V8 from 1.3.1, V7 from 1.2, V6 for 1.1, else V4

typedef struct {
    int32_t class_index;
    int32_t secondary_index; // Krunker ent[5] selectedSecondaryIndex: weapon id, -1 = class default (optional: 4-byte ENT = class only)
    PlayerCosmetics cosmetics; // optional (8-byte ENT = class + secondary): none
    uint8_t spectate;          // Krunker ent[6] (toggleSpect): join as a spectator, no spawn (optional: only sent when 1)
    uint8_t slid_cont;         // Krunker ent[15] settings.tmp.slidCont ("Non FPS Sliding"): 1 = the slide follows the
                               // view (players.js procInputs). Optional: shorter ENTs read as 1 (the setting's default).
    int32_t spray_index;       // Krunker ent[1] selectedSprayIndex (-2 Random already resolved). Optional: shorter ENTs read as 0.
    PlayerSkins skins;         // Krunker ent[2] [primary, secondary] weapon skins + ent[9] meleeIndex (Random resolved).
                               // Optional: shorter ENTs read as PLAYER_SKINS_NONE.
    float lag_comp;            // Krunker ent[11] settings.tmp.lagCompensation (Settings > Network "Lag Compensation"
                               // slider, 0..1): player.lagComp. Optional: shorter ENTs read as 1 (the setting's default).
    // ---- WP-C (protocol 1.3.1, NET_ENT_PACKET_V8_SIZE): v7.1.4 ent[12] attachIndex, ent[7] challengeMode,
    // ent[19] anonMode (server/index.ts parseEnterData). Shorter ENTs read as 0 / 0 / 0.
    int32_t attach_index;      // the sight on attach weapons: 0 Reflex, 1 Acog, -1 none
    uint8_t chall_mode;        // settings challMode: no health regen (players.js tickPlayer), x1.5 KR reward
    uint8_t anon_mode;         // settings anonMode: others see no account, clan, level or badges
    // ---- WP-F (protocol 1.3.1, NET_ENT_PACKET_V9_SIZE): the v7.1.4 cosmetic slots and ent[22..25]
    // showBadges. A 1.3.1 peer without them reads the V8 prefix; shorter ENTs read as none / all shown.
    PlayerCosmeticsV7 cosmetics_v7;
    uint8_t show_badges;       // PLAYER_SHOW_BADGE_*
} NetEntPacket;

#define NET_ENT_PACKET_MIN_SIZE 4 // class_index only (older clients / bridges)
#define NET_ENT_PACKET_V2_SIZE 8  // class_index + secondary_index (before cosmetics)
#define NET_ENT_PACKET_V3_SIZE (offsetof(NetEntPacket, spectate)) // up to cosmetics (older clients)
#define NET_ENT_PACKET_V4_SIZE (offsetof(NetEntPacket, slid_cont) + 1) // + spectate + slid_cont
#define NET_ENT_PACKET_V5_SIZE (offsetof(NetEntPacket, skins)) // + spray_index
#define NET_ENT_PACKET_V6_SIZE (offsetof(NetEntPacket, lag_comp)) // + weapon / melee skins (protocol 1.1)
#define NET_ENT_PACKET_V7_SIZE (offsetof(NetEntPacket, attach_index)) // + lagComp (protocol 1.2)
#define NET_ENT_PACKET_V8_SIZE (offsetof(NetEntPacket, cosmetics_v7)) // + attachIndex, challMode, anonMode (1.3.1, WP-C)
#define NET_ENT_PACKET_V9_SIZE (sizeof(NetEntPacket)) // + the v7 cosmetic slots and showBadges (1.3.1, WP-F): what a client sends

typedef struct {
    uint8_t spectating;      // 1: this connection spectates now (no player; every player's state is streamed to it)
} NetSpectatePacket;

typedef struct {
    int32_t uid;             // followed player, -1 = free cam
} NetSpectateTargetPacket;

typedef struct {
    int32_t uid;
    uint8_t is_you;
    uint8_t active;
    int32_t class_index;
    int32_t team;
    float health;
    int32_t max_health;
    float position[3];
    float direction[2];
    char name[32];
    int32_t secondary_index; // the spawned loadout's secondary weapon id, -1 class default
    PlayerCosmetics cosmetics; // optional: a 78-byte SPAWN (older servers / net-bridge) means none
    // ---- WP-C (protocol 1.3.1, sent to net_peer_has_wpc peers): v7.1.4 players.js sync attachIndex and
    // headMlt (players.js:5002). Shorter SPAWNs read as DEFAULT_ATTACHMENT_INDEX and 1.
    int32_t attach_index;
    float head_mlt;
    // ---- WP-F (protocol 1.3.1): the v7.1.4 cosmetic slots (players.js sync: shoeIndex, petIndex,
    // badgeIndex, wristIndex, charms, backIndex, headIndex, playerCardIndex). badge is -1 unless the
    // player shows its custom badge. Shorter SPAWNs read as PLAYER_COSMETICS_V7_NONE.
    PlayerCosmeticsV7 cosmetics_v7;
} NetSpawnPacket;

#define NET_SPAWN_PACKET_V1_SIZE 78 // up to secondary_index (before cosmetics)
#define NET_SPAWN_PACKET_V2_SIZE (offsetof(NetSpawnPacket, attach_index)) // + cosmetics (1.0 - 1.3.0)
#define NET_SPAWN_PACKET_V3_SIZE (offsetof(NetSpawnPacket, cosmetics_v7)) // + attachIndex, headMlt (1.3.1, WP-C)
#define NET_SPAWN_PACKET_V4_SIZE (sizeof(NetSpawnPacket))                // + the v7 cosmetic slots (1.3.1, WP-F)

typedef struct {
    int32_t seq;
    int32_t move_dir;
    float delta;
    float x_dir;
    float y_dir;
    uint8_t flags;
    uint8_t swap;
    uint8_t slowdown; // Input.slowdown in tenths, clamped to 0..10 (outside [0, 1] it scales like the bound)
    int8_t scroll;    // Input.scroll (input[10]): -1 / 0 / 1; absent (22/23-byte senders) = 0
} NetInputPacket;

#define NET_INPUT_PACKET_V1_SIZE (offsetof(NetInputPacket, slowdown)) // 22 bytes: no slowdown (older senders, net-bridge)
#define NET_INPUT_PACKET_V2_SIZE (offsetof(NetInputPacket, scroll)) // 23 bytes: no scroll

typedef struct {
    int32_t uid;
    int32_t ack_seq;
    uint8_t active;
    int32_t loadout_index;
    float position[3];
    float velocity[3];
    float direction[2];
    float health;
    float aim_val;
    float crouch_val;
} NetStatePacket;

// Krunker "l" (packetHandlers.ts buildLPacket, index.js reconcilePlayer): the owner's
// authoritative movement state right after the server processed input `ack_seq`; the client
// snaps to it and replays its pending (unacked) inputs. Sent at NET_RECON_SEND_HZ and right
// after server-driven position changes (spawn, teleport, changePosition recon, noclip flips)
// with ack_seq NET_RECON_SEQ_ALL. noclip is a KrunkNative field (v3.1.1 sends "noc" alone).
#define NET_RECON_ON_GROUND  0x01
#define NET_RECON_DID_JUMP   0x02
#define NET_RECON_DID_JUMP_W 0x04
#define NET_RECON_ON_LADDER  0x08
#define NET_RECON_CAN_SLIDE  0x10
#define NET_RECON_ON_TERRAIN 0x20
#define NET_RECON_NOCLIP     0x40
typedef struct {
    int32_t ack_seq;        // [0] inputSN
    float position[3];      // [2..4] feet
    float velocity[3];      // [6] velX, [5] velY, [7] velZ
    float yaw;              // [8]
    float aim_val;          // [14]
    float crouch_val;       // [15]
    int32_t loadout_index;  // [16] weaponIndex (players.reload set)
    float slide_timer;      // [17]
    float x_vel_cylinder;   // [21] xVelC
    float z_vel_cylinder;   // [22] zVelC
    uint8_t on_wall;        // [12]
    uint8_t flags;          // NET_RECON_*: [9] onGround [10] didJump [11] didJumpW [13] onLadder [18] canSlide [19] onTerrain
    // ---- WP-C (protocol 1.3.1, sent to net_peer_has_wpc peers): v7.1.4 io_syncPlayer [12] wallJumpC and
    // [24] canSlideT, plus slideTimer as v7's double ms ([18] is a JS number; slide_timer above is its
    // float s). A V1 RECON keeps the client's own values (net_packet_read_recon).
    uint16_t wall_jump_count; // wallJumpC
    float can_slide_timer;    // canSlideT (s)
    double slide_ms;          // slideTimer (ms, exact)
    // ---- v7.1.4 io_syncPlayer [23] grapple || 0 (the Grappler's hook and rope length): the
    // client snaps its grapple with the rest, then replays (input.grapObj). Sent with V2 to the
    // same peers; a 1.3.1 reader takes the longer form (prefix read), a shorter RECON keeps the
    // client's own predicted grapple (net_packet_read_recon).
    uint8_t grapple_active;   // 1 hooked, 0 none; NET_RECON_GRAPPLE_KEEP (read side only): not sent
    float grapple[4];         // x, y, z, range
} NetReconPacket;
#define NET_RECON_GRAPPLE_KEEP 0xFF // net_packet_read_recon of a shorter RECON: the client keeps its own grapple

#define NET_RECON_PACKET_V1_SIZE (offsetof(NetReconPacket, wall_jump_count)) // 58 bytes (1.0 - 1.3.0, net-bridge)
#define NET_RECON_PACKET_V2_SIZE (offsetof(NetReconPacket, grapple_active))  // + the v7 movement locks (1.3.1)
#define NET_RECON_PACKET_V3_SIZE (sizeof(NetReconPacket))                    // + the grapple (1.3.1 readers prefix-read it)

// Krunker "q": consecutive input frames (seq ascending) as whole NetInputPackets.
typedef struct {
    uint8_t count;
    NetInputPacket frames[NET_INPUT_BATCH_MAX];
} NetInputBatchPacket;
_Static_assert(sizeof(NetInputBatchPacket) <= NET_MAX_PAYLOAD, "NetInputBatchPacket exceeds NET_MAX_PAYLOAD");

typedef struct {
    char text[NET_CHAT_TEXT_SIZE];  // NUL-terminated UTF-8
} NetChatSendPacket;

typedef struct {
    int32_t uid;                    // sender, -1 for server/game lines
    uint8_t kind;                   // NetChatKind
    char name[PLAYER_NAME_SIZE];    // sender name ("" for server lines)
    char text[NET_CHAT_TEXT_SIZE];  // raw text; the HUD escapes it
} NetChatPacket;

// ---- KrunkScript packets (doc/v7/krunkscript.md 2.5-2.8), ids in NET_PACKET_BLOCK_KRUNKSCRIPT
// (200-239). Payload layouts and helpers: shared/include/ks_net.h. The server sends them only to
// connections whose HELLO announced NET_PROTOCOL_MINOR_V7; clients send SCRIPT_MSG only to such
// servers (INIT minor) or offline.
#define NET_PACKET_SCRIPT_MSG (NET_PACKET_BLOCK_KRUNKSCRIPT + 0)  // both ways: KsNetChunkHeader + message body (v7 "sd")
#define NET_PACKET_SCRIPT_LOG (NET_PACKET_BLOCK_KRUNKSCRIPT + 1)  // S->C: uint8 is_error + text (v7 "ksL", GAME.log of the server script)
#define NET_PACKET_SCRIPT_SYNC (NET_PACKET_BLOCK_KRUNKSCRIPT + 2) // S->C to the player: its custom sync values (registerSyncValues)
#define NET_PACKET_SCRIPT_CHAT (NET_PACKET_BLOCK_KRUNKSCRIPT + 3) // S->C: NetScriptChatPacket, GAME.CHAT.send / broadcast (kind 2 + colour)

typedef struct {
    uint32_t color;                 // 0xRRGGBB of the line (v7 default #fc03ec)
    NetChatPacket chat;             // kind NET_CHAT_SERVER, uid -1
} NetScriptChatPacket;

typedef struct {
    int32_t victim_uid;
    int32_t killer_uid;    // -1: suicide / fall / death zone
    int32_t weapon_index;  // Game.weapons index, -1 none
    uint8_t headshot;
    uint8_t wallbang;
    uint8_t fists;         // NET_KILL_FISTS (Krunker fs) | NET_KILL_NUKE (patch 2 peers only, net.c strips it for older ones)
    int32_t victim_deaths;
    int32_t killer_kills;
    int32_t killer_class;  // kill card icon
    int32_t score;         // kill reward the killer earned (kill card "+N"), 0 none
    float respawn_delay;   // seconds before the victim may respawn (config.deathDelay)
} NetKillPacket;
_Static_assert(sizeof(NetKillPacket) == 35, "NetKillPacket: older peers check this exact size");
#define NET_KILL_FISTS 0x01 // NetKillPacket.fists bit 0: melee without the knife (Krunker fs)
#define NET_KILL_NUKE 0x02  // bit 1: a Nuke kill, v7 info {stk: 0} (the kill feed's img/streaks/0.png)
// Bits only the official-Krunker link sets (client/src/net/krunker_link.c, never on the wire: our
// servers send 0 there and net.c keeps only NET_KILL_FISTS for older peers):
#define NET_KILL_THROWN 0x04    // killInfo.pj: a thrown melee (the feed's img/thrown_0.png before the melee icon)
#define NET_KILL_HIDE_FEED 0x08 // "3" hideFeed: no kill-feed row (player.killedBy still counts)
#define NET_KILL_STREAK_SHIFT 4 // bits 4-7: killInfo.stk + 1 (0 = not a streak kill): img/streaks/<stk>.png
#define NET_KILL_STREAK(fists) ((int) ((fists) >> NET_KILL_STREAK_SHIFT) - 1)

typedef struct {
    int32_t target_uid;
    int32_t damage;        // Math.round(dmg) (official lobbies: Math.ceil, v10's default "Rounded" dmgType)
    uint8_t crit;          // headshot
    uint8_t kill;
    uint8_t ai;            // target_uid is an AI sid (Krunker "4" fifth value isAI)
} NetHitPacket;
// NET_PACKET_HIT to uid, preceded by NET_EXT_KN_HIT_DAMAGE when `damage` (the exact value) has a fraction
void game_send_hit(Game*, int uid, const NetHitPacket *hit, float damage);

typedef struct {
    int32_t uid;
    float health;          // Math.ceil(health)
    uint8_t has_source;    // the victim's own packet carries the dealer position
    float source_x;
    float source_z;
} NetHealthPacket;

typedef struct {
    int32_t delta;         // score gained (Krunker "5" value)
    int32_t score;         // total after the change
} NetScorePacket;

typedef struct {
    int32_t uid;
    int32_t score;
    int32_t kills;
    int32_t deaths;
} NetLeaderboardEntry;

typedef struct {
    uint8_t count;
    NetLeaderboardEntry entries[GAME_LEADERBOARD_MAX]; // sorted by score, high first
} NetLeaderboardPacket;

typedef struct {
    int32_t seconds;       // seconds left
    uint8_t end;           // 1: end screen countdown to the next round (fD(time, 1))
} NetTimerPacket;

typedef struct {
    int8_t result;         // 1 victory, 0 defeat, -1 draw (no winner)
    int32_t next_round;    // seconds until the next round
    int32_t winner_uid;    // -1 none
    NetLeaderboardPacket board;
    // endData.ed "reward" column (config.endStats): KR of board.entries[i], 0 = "NONE"
    // (guests, hosted games, noReward modes). Optional: a NET_END_PACKET_V1_SIZE END has none.
    int32_t rewards[GAME_LEADERBOARD_MAX];
} NetEndPacket;

#define NET_END_PACKET_V1_SIZE (offsetof(NetEndPacket, rewards))

typedef struct {
    uint32_t seq;
} NetPingPacket;

typedef struct {
    int32_t ping_ms;       // smoothed RTT
} NetPingResultPacket;

typedef struct {
    int32_t uid;
} NetLeavePacket;

typedef struct {
    int32_t uid;           // shooter
    int32_t weapon_index;
    float hit[3];          // impact point (or the end of the range)
    uint8_t has_normal;    // hit a map object: normal angles below are valid
    float normal_yaw;      // aM: 0 = +z, PI/2 = +x
    float normal_pitch;    // aN: PI/2 = up
    uint8_t trail;         // weapon.trail
    uint8_t first;         // first ray of the shot (shotguns send one per pellet): play the sound once;
                           // 2: a full-charge shot (v7 weapon.sound + "_blast"; older clients play the plain one)
} NetShotPacket;

typedef struct {
    int32_t id;            // projectile slot
    int32_t owner_uid;
    int32_t weapon_index;  // 12 = thrown knife
    float position[3];
    float velocity[3];     // units/ms
    float gravity;         // units/ms^2
    float range;
    uint8_t ai_kind;       // AI shot: projectiles.stored[ai_kind] drawn as "orb_0" (weapon_index unused), 0 none
} NetProjectilePacket;

typedef struct {
    int32_t id;            // the server's projectile slot; <= NET_PROJECTILE_END_OWN: our own predicted one (below)
    float position[3];
    uint8_t impact;        // impact particles at the end point
} NetProjectileEndPacket;
// Official krunker.io sessions (client/src/net/krunker_link.c): the owner gets no "pr" for its own
// predicted projectile, and the "pre" ending it names the owner's shotCntP instead of a slot. The
// link sends that as id = NET_PROJECTILE_END_OWN - shotCntP; the client ends its prediction tagged
// with that count (projectiles_render.c). Our own server never sends a negative id.
#define NET_PROJECTILE_END_OWN (-2)

typedef struct {
    float position[3];
    float radius;
} NetExplosionPacket;

typedef struct {
    int32_t map_index;     // the server's index, as in the INIT that follows
    uint32_t hash;         // net_map_hash() of the map JSON
    uint32_t size;         // bytes of its unformatted JSON (what MAP_DATA streams)
    char name[32];         // map "name" (logs / errors)
} NetMapInfoPacket;

typedef struct {
    uint32_t hash;
} NetMapRequestPacket;

typedef struct {
    uint32_t hash;
    uint32_t offset;
    uint16_t length;
    uint8_t data[NET_MAP_CHUNK_SIZE];
} NetMapDataPacket;     // sent with length 10 + data length

#define NET_MAP_DATA_HEADER_SIZE 10

typedef struct {
    char message[96];      // shown on the client's connection error screen
    char code[8];          // Krunker error code ("000005" = idle kick)
} NetErrorPacket;

// NetErrorPacket codes the client acts on (doc/backend.md 8.3 / 8.5; the rest are shown only):
// a refused join ticket makes a matchmade client seek a fresh one (once); the two shutdown
// codes make it rejoin through the matchmaker (fresh ticket) once the server is gone. 004004 on
// a live session is the explicit "restarting for an update, reconnect after": the client restarts
// itself (the web page reloads) and the new process rejoins the same lobby (game_reload.h).
#define NET_ERROR_CODE_TICKET "004001"   // invalid / expired join key or session, guests refused
#define NET_ERROR_CODE_UPDATING "004004" // "GAME UPDATING": draining (a refused connect), or stopping for an update (rejoin after)
#define NET_ERROR_CODE_SHUTDOWN "004006" // the server stopped (restart / shutdown / console quit)
#define NET_ERROR_CODE_PASSWORD "004005" // the server password (gameserver.password) is missing / wrong / locked out
#define NET_ERROR_CODE_FLAVOR "004007"   // a cross-flavor peer (Krunker Strike vs Krunker Box Office); shown, not acted on

// NET_PACKET_BILLBOARD: the server's billboard ad (server.cfg billboard, the v3.1.1 init "bill":
// txt, txtCol, bgCol). While a server sends one, every BILLBOARD map object of every map shows
// it instead of its poster, drawn like utils.js createCanvasAD (client/src/billboard_ad.c,
// doc/v7/decisions.md). Sent only when the server has billboard text; empty colours = the
// engine defaults (#e3e3e3 on #000000).
typedef struct {
    char txt[64];
    char txt_col[8];       // "#rrggbb"
    char bg_col[8];
} NetBillboardPacket;

// NET_PACKET_BILLBOARD_IMAGE: the server's billboard image (server.cfg billboard_image), an
// asset path under assets/ (billboard_image_path_ok). Shown on every BILLBOARD instead of the
// text ad when it loads. A separate packet so older clients (which ignore unknown types) and
// the fixed NetBillboardPacket size stay compatible.
#define NET_BILLBOARD_IMAGE_MAX 128
typedef struct {
    char path[NET_BILLBOARD_IMAGE_MAX];
} NetBillboardImagePacket;

// A billboard_image value both sides accept: a relative path under assets/ (letters, digits,
// "_-./"), no "..", "//" or leading "/", ending in .png / .jpg / .jpeg. URLs are refused (the
// native client has no HTTP fetch outside the asset host). 1 when acceptable.
int billboard_image_path_ok(const char *path);

// NET_PACKET_PLAYER_INFO: account data of a player (uid) for the scoreboard, nametags
// and kill feed. account_id / level 0 = guest.
enum {
    NET_PLAYER_INFO_PREMIUM = 1 << 0,
    NET_PLAYER_INFO_VERIFIED = 1 << 1, // featured / verified account
    NET_PLAYER_INFO_DEV = 1 << 2,
    NET_PLAYER_INFO_MOD = 1 << 3,
    NET_PLAYER_INFO_HACKER = 1 << 4, // v10 player.isHacker: a flagged account (accounts.player_hack): the red name
    NET_PLAYER_INFO_HOST = 1 << 5,   // v10 game.host == this player's id: the custom lobby's host (the list's star)
    NET_PLAYER_INFO_BOT = 1 << 6,    // KrunkNative: a bot player (bots.c bot_send_info); older clients ignore the bit
    NET_PLAYER_INFO_CREATOR = 1 << 7, // v10 creatorBadge (account creator_badge): the purple verified icon
};

typedef struct {
    int32_t uid;
    int32_t account_id;
    int32_t level;
    uint8_t flags;         // NET_PLAYER_INFO_*
    char clan[8];          // clan tag, "" none
} NetPlayerInfoPacket;

// The validated (owned) weapon skins of a player, sent before its SPAWN: a separate packet so older
// clients, which only accept the fixed SPAWN sizes, keep working (they ignore unknown types).
typedef struct {
    int32_t uid;
    PlayerSkins skins;
} NetSkinsPacket;

// NET_PACKET_JOIN (C->S right after HELLO, before the first ENT). Every field is a
// NUL-terminated string, "" = not given. game_id + client_key: what the brain's
// /mm/seek-game returned (one-time key the brain also sent to this server); session:
// the social login session token. No JOIN at all (offline, --connect, old clients) or
// all empty = guest. name: guest nickname wish (ignored for accounts). A JOIN without
// the trailing name (NET_JOIN_PACKET_V1_SIZE bytes) is accepted.
typedef struct {
    char game_id[48];
    char client_key[352];  // the brain's signed join ticket (JWS HS256, ~250 chars)
    char session[64];
    char name[32];
    char password[16];     // server password (gameserver.password / server.cfg sv_password), "" none
} NetJoinPacket;           // 512 bytes = NET_MAX_PAYLOAD

#define NET_JOIN_PACKET_V1_SIZE (offsetof(NetJoinPacket, name))
#define NET_JOIN_PACKET_V2_SIZE (offsetof(NetJoinPacket, password)) // before the password field

// NET_PACKET_REDIRECT (S->C, console `send`): leave this server for another one of the
// network. game_id set (coordinator lobby id "<node>:<lobby>"): the client seeks it through
// its matchmaker (/mm/seek-game?game=, a fresh ticket); else host:port is a direct connect.
typedef struct {
    char game_id[48];
    char host[128];
    uint16_t port;
    char reason[64];       // shown while switching ("Moving to Sandstorm...")
} NetRedirectPacket;

// NET_PACKET_AUTH_CHALLENGE (S->C right after HELLO; online_mode servers always send it,
// offline ones may). Mojang-style join (shared/include/auth_join.h): the client picks a
// client nonce, POSTs <auth>/auth/join {token, serverHash = kn_server_hash(server_nonce,
// public_key, client_nonce)} and then sends JOIN with name = account username and
// client_key = "hj1:" + client nonce; the server asks <auth>/auth/hasJoined. A client without
// an account answers with client_key = AUTH_JOIN_GUEST_KEY and plays as a guest.
// Strings NUL-terminated; public_key "" while the transport has no key.
typedef struct {
    char server_nonce[33];  // 32 lowercase hex, random per server boot
    uint8_t online_mode;    // 1 = accounts verified through the auth service (guests answer AUTH_JOIN_GUEST_KEY); 0 = no handshake
    char public_key[65];    // lowercase hex of the transport public key, "" = none
    char auth_url[128];     // auth service base the server verifies against ("" = its default)
} NetAuthChallengePacket;   // 227 bytes

// NET_PACKET_CHEAT: what a chat command changed (shared/src/chat_commands.c). The
// client applies it to its mirror and prints Krunker's line ("Noclip - Enabled", ...).
typedef enum {
    NET_CHEAT_NOCLIP = 0,          // "noc" -> me->noclip
    NET_CHEAT_UNLIMITED_AMMO = 1,  // "unl" -> me->unlimited_ammo
    NET_CHEAT_FORCE_HIDE = 2,      // "fh"  -> me->force_hide
    NET_CHEAT_BORDERS = 3,         // "borders" -> !game.config.disable_borders
    NET_CHEAT_FORCE_COLLIDE = 4,   // "forcecol" -> map objects forced collidable
    NET_CHEAT_THIRD_PERSON = 5,    // KrunkNative /thirdperson: the third-person camera granted (1) / taken (0)
    NET_CHEAT_HELD = 6,            // KrunkNative Physics Gun: a dev carries you (1: your input moves nothing) / let go (0)
    NET_CHEAT_FROZEN = 7,          // "frz" -> me->frozen (official lobbies, krunker_link.c; our own server never sends it)
} NetCheatFlag;

typedef struct {
    uint8_t flag;   // NetCheatFlag
    uint8_t on;
} NetCheatPacket;

// v3 "int": 0 spray, 1 interact (secondary = interactSecKey), 2 drop. v7 peers send NetInteractV7Packet.
typedef struct {
    uint8_t action;
    uint8_t secondary;
} NetInteractPacket;

// "sp" [sid, sprayIndex, x, y, z, xDr, yDr] (players.spray): the decal's centre (0.1 in front of
// the face) and its plane rotation in degrees: y_rot (three.js rotation.y) 0 = +z face, 180 = -z,
// -90 = -x, 90 = +x; x_rot (rotation.x) -90 = top face, 90 = bottom face.
typedef struct {
    int32_t uid;          // the sprayer (one spray per player: a new one moves it)
    int32_t spray_index;  // player.sprayIndex: the client resolves it (sprays.js, store type 4, else spray 0)
    float position[3];
    int16_t y_rot;
    int16_t x_rot;
} NetSprayPacket;

// io_tp: callers pass eye-height coordinates; the feet go EYE_OFFSET_Y below (playerHeight
// 11 - cameraHeight 1.5).
#define NET_TELEPORT_EYE_OFFSET (11.0f - 1.5f)
typedef struct {
    float x, y, z;
} NetTeleportPacket;

// NET_PACKET_FORWARDED: net-bridge writes it before any byte of its browser peer, so the server
// can tell a bridged web client (whose TCP peer is the bridge on 127.0.0.1) from a client on its
// own machine. Honoured only as the very first packet of a loopback connection.
#define NET_FORWARDED_ADDRESS_SIZE 48

#define NET_NOTIFY_TEXT_SIZE 96
typedef struct {
    char text[NET_NOTIFY_TEXT_SIZE]; // NUL-terminated; the client shows it as notify() innerHTML
} NetNotifyPacket;

// ---- map trigger packets (shared/src/triggers.c builds them)
typedef enum {
    NET_OBJECT_DESTROY = 0,     // "do" uid: active = false, destroyed (mesh hidden)
    NET_OBJECT_RESPAWN = 1,     // "ro" uid: active again, health back (mesh shown)
    NET_OBJECT_GATE = 2,        // "gte" uid, active: a gate opened (0) / closed (1)
    NET_OBJECT_BANK = 3,        // "bnk" uid, deposited
    // map-logic WP-B (shared/src/world.c): object node moves
    NET_OBJECT_MOVE_TO_NODE = 4,   // "mo" uid, node: teleportToNode (feet on PATH_NODE nid `value`)
    NET_OBJECT_MOVE_FROM_NODE = 5, // "mf" uid: teleportFromNode (back to its original position)
} NetObjectStateKind;

typedef struct {
    int32_t uid;                // map object index (Krunker GameObject uid)
    uint8_t kind;               // NetObjectStateKind
    int32_t value;              // gate: active; bank: deposited
} NetObjectStatePacket;

// ---- map-logic WP-B packets (NET_PACKET_BLOCK_WORLD 130-139; shared/src/world.c, zone.c, build.c).
// Sent only by v7 servers (NET_PROTOCOL_MINOR_V7); clients handle them in client/src/world_events.c.
#define NET_PACKET_ZONE (NET_PACKET_BLOCK_WORLD + 0)        // S->C all: NetZonePacket ("zn" every 1 s, init syncData.zone)
#define NET_PACKET_BUILD (NET_PACKET_BLOCK_WORLD + 1)       // S->C all: NetBuildPacket ("bo"; init syncData.dyno)
#define NET_PACKET_LOG_TIME (NET_PACKET_BLOCK_WORLD + 2)    // S->C to the player: NetLogTimePacket ("gt" / the speed-run "t")
#define NET_PACKET_OBJECT_ANIM (NET_PACKET_BLOCK_WORLD + 3) // S->C: NetObjectAnimPacket ("ana" with isObj true)
// NET_PACKET_CHECKPOINT (101) keeps its id; v7 servers add a NetCheckpointPacket payload ("chp" / "chp" true).

typedef struct {
    float x, z;                 // circlePosition x / z
    float size_x, size_z;       // circleSize x / z (radius; 0 hides the zone)
} NetZonePacket;

typedef struct {
    int32_t uid;                // the new map object's index
    uint8_t build_index;        // buildObjs.js index (0 Build Block, 1 Platform, 2 Wall, 3 Stairs)
    int8_t direction;           // dir (rotDir / ramp direction 0..3)
    float position[3];          // x, y (the build's feet), z
    float size[3];              // w, h, l (0 = the build object's own size)
} NetBuildPacket;

enum {
    NET_LOG_TIME_ZONE = 0,      // "gt" time, zoneIndex: a score zone was logged
    NET_LOG_TIME_CLOCK = 1,     // "t" of an ownTime mode: the player's own speed-run clock
    NET_LOG_TIME_FINISH = 2,    // "t" after the last score zone (the final time)
};
typedef struct {
    int32_t time_ms;            // game.gameTimer (ms) or the player's gameTime (ownTime)
    int32_t time_log;           // timeLog (zone index + 1)
    uint8_t kind;               // NET_LOG_TIME_*
    uint8_t show_ms;            // the mode shows milliseconds (showMS)
} NetLogTimePacket;

#define NET_ANIM_CLIP_SIZE 33   // trigger "ast" / Play Animation clip names are <= 32 chars
typedef struct {
    int32_t uid;                // map object index (isObj) or AI sid
    uint8_t is_object;          // 4th "ana" argument: a map object (1) or an AI (0)
    uint8_t raw;                // 1: clip is a custom clip name; 0: one of config.clipNames
    char clip[NET_ANIM_CLIP_SIZE];
} NetObjectAnimPacket;

typedef struct {
    uint8_t reset;              // "chp" true: the player's check points were cleared
} NetCheckpointPacket;
// ---- end WP-B packets

#define NET_SOUND_NAME_SIZE 24
typedef struct {
    char name[NET_SOUND_NAME_SIZE]; // sound name (config.emitterSounds / networkSounds entry)
    int32_t uid;                // playerSound sid: play at that player (-1: at the position)
    float position[3];
    float volume;
    float rate;
} NetSoundPacket;

typedef struct {
    int32_t index;              // jumpScares[index]: 0 = image + sound
} NetJumpScarePacket;

typedef struct {
    char text[TRIGGER_TEXT_MAX + 1]; // medal popup text, no score
} NetPopupPacket;

typedef struct {
    float x, y, z;              // feet position
    uint8_t stop_momentum;      // changePosition U: velocities 0, stepVal 0
    uint8_t has_direction;      // changePosition V != null: yaw = -(dir + 1) * PI / 2
    int32_t direction;
} NetPositionPacket;

// ---- game mode packets (shared/src/modes/mode_net.c builds them)
#define NET_MODE_TEXT_SIZE 32
#define NET_GAME_MESSAGE_SIZE 96
#define NET_GAME_MESSAGE_WAITING "wt" // gmsg "wt": the client shows i18n player.waiting
#define NET_MODE_END_MAX 16

typedef struct {
    float position[3];          // crystal centre (the carrier's ping while carried)
    int32_t carrier_uid;        // -1 at base / dropped
    int32_t team;               // 1 / 2
} NetModeFlag;

typedef struct {
    int32_t mode_id;            // GameModeId
    int32_t team_scores[2];     // teams 1 and 2
    char flag_info[2][NET_MODE_TEXT_SIZE]; // CTF teamFlags 1 / 2 ("AT BASE", "AWAY", carrier name)
    int32_t active_objective;   // Hardpoint / KOTH objective index, -1 none
    int32_t objective_weapon;   // Sharp Shooter weapon id, -1 none
    uint8_t move_lock;          // game.moveLock
    uint8_t locked;             // the recipient is locked (lockPlayer)
    int32_t lives;              // the recipient's lives, -1: the mode has none
    uint8_t flag_count;
    NetModeFlag flags[MODE_MAX_FLAGS];
    char message[NET_GAME_MESSAGE_SIZE]; // the current game message ("" none)
} NetModeStatePacket;

typedef struct {
    char text[NET_GAME_MESSAGE_SIZE]; // "" clears, NET_GAME_MESSAGE_WAITING = waiting for players
} NetGameMessagePacket;

typedef struct {
    int32_t uid;
    int32_t team;
    float health;               // the role's health (Boss 10000, Stalker 400, ...)
    int32_t max_health;
} NetTeamPacket;

typedef struct {
    int32_t team;
    int32_t score;
} NetTeamScorePacket;

typedef struct {
    int32_t index;              // flag index (map order)
    NetModeFlag flag;
    char info[NET_MODE_TEXT_SIZE]; // teamFlags[flag.team]
} NetFlagPacket;

typedef enum {
    NET_OBJECTIVE_POINT = 0,    // "obj": active objective index
    NET_OBJECTIVE_WEAPON = 1,   // "wep": Sharp Shooter weapon id
} NetObjectiveKind;

typedef struct {
    int32_t kind;               // NetObjectiveKind
    int32_t value;
} NetObjectivePacket;

typedef struct {
    int32_t lives;
} NetLivesPacket;

typedef struct {
    int32_t uid;                // the actor
    int32_t other_uid;          // -1 none
    char text[NET_MODE_TEXT_SIZE]; // "captured the Flag", "infected", ...
} NetActionPacket;

#define NET_LOADOUT_MAX 4

typedef struct {
    int32_t uid;
    int32_t count;
    int32_t weapons[NET_LOADOUT_MAX];
    int32_t ammo[NET_LOADOUT_MAX];
    int32_t loadout_index;
    int32_t weapon_tier;        // Gun Game tier (the clients rebuild the same loadout on respawn)
} NetLoadoutPacket;

typedef enum {
    NET_LOCK_PLAYER = 0,        // "lock": the recipient may not move or shoot
    NET_LOCK_MOVE = 1,          // "lm" / "ulm": game.moveLock for everyone
} NetLockKind;

typedef struct {
    uint8_t kind;               // NetLockKind
    uint8_t locked;
} NetLockPacket;

typedef struct {
    int32_t sid;
    uint8_t active;             // 0: dispose
    int32_t team;
    float position[3];
} NetSpawnablePacket;

typedef enum {
    NET_MODE_STAT_CAPS,
    NET_MODE_STAT_CONFIRMS,
    NET_MODE_STAT_DENIES,
    NET_MODE_STAT_OBJECTIVE,
    NET_MODE_STAT_WEAPON_TIER,
    NET_MODE_STAT_SETBACKS,
    NET_MODE_STAT_MELEES,
    NET_MODE_STAT_CONVS,
    NET_MODE_STAT_LIVES,
    NET_MODE_STAT_ASSISTS,
    NET_MODE_STAT_TIME,         // endForm.time: player.timer in tenths of a second + 1, 0 = "DNF"
    NET_MODE_STAT_DEPOSITS,     // WP-D v7.1.4 endStats "deposits" (Deposit / Deposit FFA)
    NET_MODE_STAT_PICKUPS,      // WP-D v7.1.4 endStats "pickups" (Mag Dump)
    NET_MODE_STAT_COUNT
} NetModeStat;

typedef struct {
    int32_t uid;
    int16_t stats[NET_MODE_STAT_COUNT];
} NetModeEndEntry;

typedef struct {
    int32_t mode_id;
    int32_t winner_team;        // 1 / 2, -1 none (FFA or draw)
    int32_t team_scores[2];
    uint8_t count;
    NetModeEndEntry entries[NET_MODE_END_MAX]; // in endSort order
} NetModeEndPacket;

typedef struct {
    char text[NET_MODE_TEXT_SIZE]; // "Capture", "Confirmed", ...
    int32_t score;
} NetMedalPacket;

// kills.js reward(): [medalName, score, ...] pairs in push order; "" names are the plain
// kill score (no popup). Sent only when the list is not empty, before the SCORE packet.
#define NET_KILL_MEDALS_MAX 13 // every medal of one kill fits (and the packet stays under NET_MAX_PAYLOAD)
typedef struct {
    uint8_t count;
    NetMedalPacket medals[NET_KILL_MEDALS_MAX];
    int32_t kills;                 // the killer's kills (Krunker "6" third value)
} NetKillMedalsPacket;
_Static_assert(sizeof(NetKillMedalsPacket) <= NET_MAX_PAYLOAD, "NetKillMedalsPacket exceeds NET_MAX_PAYLOAD");

typedef struct {
    int32_t uid;                   // the killer
    int32_t streak;                // killStreak, a multiple of 5
} NetKillStreakPacket;

// ---- WP-B combat packets (v7.1.4), ids in NET_PACKET_BLOCK_EFFECTS (170-176) and
// NET_PACKET_BLOCK_COMBAT (190-194). Old peers ignore unknown ids; a client sends the C->S
// ones only to servers that announced NET_PROTOCOL_MINOR_V7. Handled client-side in
// client/src/net/combat_events.c, server-side in server/src/net.c.
#define NET_PACKET_EFFECT (NET_PACKET_BLOCK_EFFECTS + 0)       // S->C to the player: NetEffectPacket ("pk" index, count)
#define NET_PACKET_STREAK (NET_PACKET_BLOCK_EFFECTS + 1)       // S->C to the player: NetStreakPacket ("st" index, count)
#define NET_PACKET_USE_STREAK (NET_PACKET_BLOCK_EFFECTS + 2)   // C->S: NetUseStreakPacket ("k<slot>": checkStreak)
#define NET_PACKET_NUKE (NET_PACKET_BLOCK_EFFECTS + 3)         // S->C to all: NetNukePacket ("n" 0 incoming / 1 detonated)
#define NET_PACKET_JUGGERNAUT (NET_PACKET_BLOCK_EFFECTS + 4)   // S->C to all: NetJuggernautPacket ("jg" sid, on)
#define NET_PACKET_STREAK_SLOTS (NET_PACKET_BLOCK_EFFECTS + 5) // C->S before ENT: NetStreakSlotsPacket (ENT streakIndexes)
// NET_PACKET_BLOCK_EFFECTS + 6: reserved for "bbm" (Bombardment, devOnly, not ported)
#define NET_PACKET_PERKS (NET_PACKET_BLOCK_COMBAT + 0)         // S->C to all: NetPerksPacket ("upk" sid, perks)
#define NET_PACKET_UPGRADES (NET_PACKET_BLOCK_COMBAT + 1)      // S->C to all: NetUpgradesPacket (v7 inat upgrades[]: krumMachine slots)
#define NET_PACKET_WEAPON_STREAK (NET_PACKET_BLOCK_COMBAT + 2) // S->C to the killer: NetWeaponStreakPacket ("wstk" weapon, count)
#define NET_PACKET_GRAPPLE (NET_PACKET_BLOCK_COMBAT + 3)       // S->C to the others: NetGrapplePacket (the "k" sync's grapple slot)

// ---- WP-E packets (player-viewer: HUD, death screen, social), ids in NET_PACKET_BLOCK_SOCIAL
// (240-249). Sent only to peers that announced NET_PROTOCOL_MINOR_V7 (server/src/net.c
// net_peer_takes); older peers ignore unknown ids. Client side: client/src/net/game_events.c.
#define NET_PACKET_LEADER_STATS (NET_PACKET_BLOCK_SOCIAL + 0) // S->C: NetLeaderStatsPacket (v7 "7" [sid, ...mode.leaderStats])
#define NET_LEADER_STATS_MAX 4                                // the longest v7 leaderStats (Deposit, Zombies)

// players.syncLeaders: the board rows in the mode's leader order (leaderSort / killSort /
// orderByScore, at most 16) with one value per mode.leaderStats key (endForm applied, see
// mode_leader_stat_value). Sent right after NET_PACKET_LEADERBOARD.
typedef struct {
    uint8_t count;
    uint8_t stat_count;                                   // mode.leaderStats length (<= NET_LEADER_STATS_MAX)
    int32_t uid[GAME_LEADERBOARD_MAX];
    int32_t values[GAME_LEADERBOARD_MAX][NET_LEADER_STATS_MAX];
} NetLeaderStatsPacket;

// Part 2a: the death screen. v7's victim-only "3" extras (players.js:4860) ride in DEATH_INFO,
// which the server sends to the victim right before the KILL packet; the KILL stays the same
// for every peer. A client that gets no DEATH_INFO (an older server) samples what it can itself.
#define NET_PACKET_DEATH_INFO (NET_PACKET_BLOCK_SOCIAL + 1) // S->C to the victim: NetDeathInfoPacket ("3" [wid, classStat, pcStat, stats])
#define NET_PACKET_PC_STAT (NET_PACKET_BLOCK_SOCIAL + 2)    // C->S before ENT: NetPcStatPacket (v7 ENT pcStatIndex)
// Part 2c: v7's endData.mdls (game.js:4693, the end screen's Medals tab): the player's
// player.medals, sent to it right before its END. + 5..9 are WP-E part 2b's.
#define NET_PACKET_END_MEDALS (NET_PACKET_BLOCK_SOCIAL + 3) // S->C: NetEndMedalsPacket
// season-system (doc/v7/decisions.md): a skin game.giveSkin gave (sendUserSkin's success): v7
// "skin" (io_skinReward) to the finder and "skinF" (io_skinFoundMessage) to everyone else.
#define NET_PACKET_SKIN_FOUND (NET_PACKET_BLOCK_SOCIAL + 4) // S->C: NetSkinFoundPacket

typedef struct {
    int32_t uid;  // the finder
    int32_t skin; // store.skins index
} NetSkinFoundPacket;

typedef struct {
    uint8_t count;                                // entries used, in overlay.medalsList order
    struct {
        uint8_t index;                            // overlay.medalsList index (progression.h)
        uint16_t count;                           // times earned this match
    } medals[PLAYER_MEDAL_SLOTS];
} NetEndMedalsPacket;

typedef struct {
    int32_t victim_uid;
    int32_t killer_uid;            // -1: no killer ("You Died")
    int32_t weapon_index;          // the killing weapon ("3" [0]), -1 none
    int32_t class_stat;            // the killer's account.stats["c" + classIndex] ("3" [1]): its class level
    int8_t pc_stat_index;          // "3" [2][0]: config.playerCardStats index, -1 = null (no account / none picked)
    char pc_stat_value[16];        // "3" [2][1] as v7 prints it ("1.52", "38.1%", "1203")
    uint8_t thrown;                // a thrown melee (info.pj): "Thrown [name]"
    uint8_t fists;                 // info.fs: "Fists"
    int32_t time_alive_ms;         // "3" [3]: Date.now() - spawnTime
    int32_t shots;                 // totShotsL
    char accuracy[8];              // min(totShotsL, hitShotsL) / totShotsL * 100, .round(1) + "%"
    int32_t kills;                 // killsL
    int32_t score;                 // scoreL
} NetDeathInfoPacket;

typedef struct {
    int8_t index;                  // pcStatIndex (Change Loadout > Player Card stat), -1 none
} NetPcStatPacket;

typedef struct {
    uint8_t effect;                // EffectId
    uint8_t count;                 // stacks, 0 = cleared
} NetEffectPacket;

typedef struct {
    uint8_t streak;                // StreakId
    uint8_t count;                 // earned and not yet called in
} NetStreakPacket;

typedef struct {
    uint8_t slot;                  // 0..STREAK_SLOTS-1
} NetUseStreakPacket;

typedef struct {
    uint8_t state;                 // 0 incoming (10 s siren), 1 detonated
} NetNukePacket;

typedef struct {
    int32_t uid;
    uint8_t on;
    int32_t max_health;            // the new maxHealth (2000, or the class value when off)
} NetJuggernautPacket;

typedef struct {
    int8_t indexes[STREAK_SLOTS];  // streakIndex1..3, -1 none (sanitised by player_set_streak_indexes)
} NetStreakSlotsPacket;

typedef struct {
    int32_t uid;
    uint8_t count;
    int8_t perks[PLAYER_PERK_MAX];
    int32_t max_health;            // maxHealth after the perks (perk 0: 250)
} NetPerksPacket;
typedef struct {
    int32_t weapon;                // currentStreakWeapon (weapon id)
    int32_t count;                 // weaponStreak
} NetWeaponStreakPacket;

// v7.1.4 game.js player sync ("k"): each other player's grapple ? [x, y, z] : 0, which the client
// turns into a rope (index.js PARTICLES.addRope when the point changes, gone at 0). KrunkNative
// sends it when it changes (server/src/net.c net_sync_grapples) instead of in every sync; the
// owner predicts its own and gets the authority's in RECON V3.
typedef struct {
    int32_t uid;
    uint8_t active;
    float point[3];
} NetGrapplePacket;

typedef struct {
    int32_t uid;
    uint8_t slots;                 // bit per loadout slot: upgradeWeapons entry
    int32_t ammo[NET_LOADOUT_MAX]; // the slots' ammo after the upgrade
} NetUpgradesPacket;
// ---- end WP-B combat packets ----

// ---- WP-D: Krunker v7.1.4 mode packets (doc/v7/map-logic.md §4.1, §5.3 block 140-149; built in
// shared/src/modes/mode_net.c). The server sends them only to connections whose HELLO announced
// NET_PROTOCOL_MINOR_V7 (server/src/net.c); the offline sandbox client always gets them.
#define NET_PACKET_OBJECTIVE_V7 (NET_PACKET_BLOCK_MODES + 0)        // S->C: NetObjectiveV7Packet ("obj uid, objtvTime, initial")
#define NET_PACKET_OBJECTIVE_OCCUPANCY (NET_PACKET_BLOCK_MODES + 1) // S->C: NetObjectiveOccupancyPacket ("uobj uid, onZone")
#define NET_PACKET_DOM_UPDATE (NET_PACKET_BLOCK_MODES + 2)          // S->C: NetDomUpdatePacket ("uct", sync.doms)
#define NET_PACKET_DOM_CAPTURE (NET_PACKET_BLOCK_MODES + 3)         // S->C: NetDomCapturePacket ("cct uid, team")
#define NET_PACKET_ROUNDS (NET_PACKET_BLOCK_MODES + 4)              // S->C: NetRoundsPacket ("rds roundsLeft, false, winTeam, teamRoundWins")
#define NET_PACKET_LOCKOUT (NET_PACKET_BLOCK_MODES + 5)             // S->C: NetLockoutPacket ("lkt")
#define NET_PACKET_TEXT_POPUP (NET_PACKET_BLOCK_MODES + 6)          // S->C: NetTextPopupPacket ("tx": "Overtime", "+60 Seconds")
#define NET_PACKET_MODE_COUNTER (NET_PACKET_BLOCK_MODES + 7)        // S->C: NetModeCounterPacket ("krt" / "chrg" / "bh")
#define NET_PACKET_SPAWNABLE_V7 (NET_PACKET_BLOCK_MODES + 8)        // S->C: NetSpawnableV7Packet ("crsp type, sid, x, y, z, team, data" / "dsp sid")
#define NET_PACKET_MODE_SYNC_V7 (NET_PACKET_BLOCK_MODES + 9)        // S->C: NetModeSyncV7Packet (init teamScores 1..5, roundC, lockT, obj)

typedef struct {
    int32_t uid;                // objective uid, -1 none
    int32_t time_ms;            // objtvTime: ms until the next objective
    uint8_t initial;            // time == null (gameStart / a new round)
} NetObjectiveV7Packet;

typedef struct {
    int32_t uid;
    int32_t on_zone;            // players on the hill
} NetObjectiveOccupancyPacket;

typedef struct {
    int32_t uid;                // zone index (map->dom_zones order)
    int32_t captured_by;        // team, 0 none
    float capture_t;            // ms of capture progress
    int32_t last_team_on;       // 0 none
    uint8_t teams_on_count;
    int8_t teams_on[MODE_TEAMS];
    float mlt;                  // capture speed
} NetDomZoneState;

typedef struct {
    uint8_t count;
    NetDomZoneState zones[MODE_MAX_DOM_ZONES];
} NetDomUpdatePacket;

typedef struct {
    int32_t uid;
    int32_t team;
} NetDomCapturePacket;

typedef struct {
    int32_t rounds_left;        // roundCount after this round
    int32_t win_team;           // getWinTeam(): 0 none
    uint8_t has_round_wins;     // teamRoundWins != null (best of)
    int32_t team_round_wins[MODE_TEAMS];
} NetRoundsPacket;

typedef struct {
    int32_t ms;                 // lockoutTimer, 0 = over
} NetLockoutPacket;

typedef struct {
    char title[NET_MODE_TEXT_SIZE]; // "Overtime"
    char text[NET_MODE_TEXT_SIZE];  // "+60 Seconds"
} NetTextPopupPacket;

typedef enum {
    NET_MODE_COUNTER_KR_TAGS = 0, // "krt dogTags" to the carrier
    NET_MODE_COUNTER_CHARGE = 1,  // "chrg sid, krankedTimer ms" (Kranked)
    NET_MODE_COUNTER_HEAD = 2,    // "bh sid, headMlt" (Bighead FFA)
} NetModeCounterKind;

typedef struct {
    uint8_t kind;               // NetModeCounterKind
    int32_t uid;
    float value;
} NetModeCounterPacket;

typedef struct {
    int32_t sid;
    uint8_t type;               // medals.js index (spawnables.h SpawnableType)
    uint8_t active;             // 0: "dsp" dispose
    int32_t team;
    float position[3];          // centre (yOff included)
    int32_t data_c;             // data.c (KR Tag count, ammo), 0 none
    int32_t data_w;             // data.wId (Weapon Box), -1 none
    float data_xd, data_zd;     // data.xD / zD rotation
} NetSpawnableV7Packet;

typedef struct {
    int32_t team_scores[MODE_TEAMS]; // teamScores 1..5 (sdm)
    int32_t rounds_left;        // roundC
    uint8_t has_round_wins;
    int32_t team_round_wins[MODE_TEAMS];
    int32_t lockout_ms;         // lockT
    int32_t objective_uid;      // obj[0], -1 none
    int32_t objective_ms;       // obj[1]
} NetModeSyncV7Packet;
// ---- end WP-D

// ---- WP-E lobby packets (map-logic.md §5.3 block 180-189; shared/src/game_config.c builds them).
// Only sent to / accepted from peers that announced NET_PROTOCOL_MINOR_V7. The server sends
// GAME_CONFIG and NETWORK_DATA right before every INIT. ----
#define NET_PACKET_MATCH_VOTE (NET_PACKET_BLOCK_LOBBY + 0)       // C->S: NetMatchVotePacket ("maVote" "mode,map")
#define NET_PACKET_MATCH_VOTE_STATE (NET_PACKET_BLOCK_LOBBY + 1) // S->C: NetMatchVoteStatePacket (endData.mts + "mv" totals)
#define NET_PACKET_NETWORK_DATA (NET_PACKET_BLOCK_LOBBY + 2)     // S->C: NetNetworkDataPacket (init networkData)
#define NET_PACKET_GAME_CONFIG (NET_PACKET_BLOCK_LOBBY + 3)      // S->C: NetGameConfigChunk (init config + customData, JSON)
#define NET_PACKET_COMP_SET (NET_PACKET_BLOCK_LOBBY + 4)         // C->S: NetCompSetPacket (v7 updSyncDat: "cd" key, value)
#define NET_PACKET_COMP_LOBBY (NET_PACKET_BLOCK_LOBBY + 5)       // S->C: NetCompLobbyPacket (v7 "cd" lobbyPlayers / "cda" id, entry)

#define NET_MATCH_PICKS 4 // config.matchPicks

typedef struct {
    int32_t mode_id;   // Krunker mode id
    int32_t map_index; // index in the server's map list (as INIT map_index)
} NetMatchVotePacket;

typedef struct {
    int32_t mode_id;
    int32_t map_index;
    uint16_t votes;    // summed vote weights
    char map_name[32];
} NetMatchVoteEntry;

typedef struct {
    uint8_t count;     // entries in use; 0 = no vote (clears the vote cards)
    int8_t voted;      // the receiving connection's vote (entry index), -1 none
    NetMatchVoteEntry entries[NET_MATCH_PICKS];
} NetMatchVoteStatePacket;

typedef struct {
    uint16_t send_hz;    // snf: player states ("k") per second (config.serverSendFrequency 10, mode.sendR)
    uint16_t tick_hz;    // tf: server ticks per second
    uint16_t sync_hz;    // sf: reconciliation ("l") per second
    float view_distance; // mode.viewDis: players further away are not streamed to a player, 0 = all
} NetNetworkDataPacket;

#define NET_GAME_CONFIG_HEADER_SIZE 8
#define NET_GAME_CONFIG_CHUNK_SIZE (NET_MAX_PAYLOAD - NET_GAME_CONFIG_HEADER_SIZE)
#define NET_GAME_CONFIG_MAX (64 * 1024)
typedef struct {
    uint32_t total;    // length of the whole JSON text
    uint32_t offset;   // of this chunk; offset 0 starts a new config
    uint8_t data[NET_GAME_CONFIG_CHUNK_SIZE];
} NetGameConfigChunk;

// Comp lobby (shared/include/comp_lobby.h). v7 "cd" (the whole lobbyPlayers table) is a CLEAR
// followed by one SET per entry; "cda" is a SET, or a REMOVE for `cda id null`.
typedef enum {
    NET_COMP_LOBBY_CLEAR = 0,
    NET_COMP_LOBBY_SET = 1,
    NET_COMP_LOBBY_REMOVE = 2,
} NetCompLobbyOp;

typedef struct {
    int32_t uid;           // lobbyId: the player's connection id (player.id)
    uint8_t active;        // a
    uint8_t rejected;      // rej: left the lobby while on a team
    int8_t team;           // t: 0 none, 1 / 2
    int8_t class_index;    // c: -1 null
    int32_t account_id;    // i (0 guest)
    int32_t level;         // l
    uint8_t verified;      // v (account.featured)
    int16_t flag;          // f: account stats.flg, -1 none
    char name[32];         // n ("" = guest)
    char clan[8];          // cl
} NetCompLobbyEntry;

typedef struct {
    uint8_t op;            // NetCompLobbyOp
    NetCompLobbyEntry entry;
} NetCompLobbyPacket;

typedef struct {
    uint8_t key;           // 't' team (0 leave, 1, 2) or 'c' class index
    int32_t value;
} NetCompSetPacket;
// ---- end WP-E lobby packets ----
// ---- WP-C trigger / interaction packets (v7.1.4, doc/v7/map-logic.md §3.9, §5.3), ids in
// NET_PACKET_BLOCK_TRIGGERS (120-129). Old peers ignore unknown ids; a client sends the C->S
// ones (and the v7 INTERACT payload) only to servers that announced NET_PROTOCOL_MINOR_V7 (or
// offline). Built by shared/src/triggers_v7.c / interact.c, handled in client/src/map_triggers.c.
#define NET_PACKET_CUSTOM_VALUE (NET_PACKET_BLOCK_TRIGGERS + 0)     // S->C to the player: NetValuePacket ("cv" key, val)
#define NET_PACKET_GLOBAL_VALUE (NET_PACKET_BLOCK_TRIGGERS + 1)     // S->C to all: NetValuePacket ("gv" key, val; the "gbl" join list)
#define NET_PACKET_GUI_TOGGLE (NET_PACKET_BLOCK_TRIGGERS + 2)       // S->C: NetGuiTogglePacket ("tgui" id, status)
#define NET_PACKET_AMBIENT_CHANGE (NET_PACKET_BLOCK_TRIGGERS + 3)   // S->C to all: NetAmbientPacket ("camb" soundscape)
#define NET_PACKET_STOP_SOUND (NET_PACKET_BLOCK_TRIGGERS + 4)       // S->C to all: NetStopSoundPacket ("sso" name, isAsset)
#define NET_PACKET_PLAYER_SOUND (NET_PACKET_BLOCK_TRIGGERS + 5)     // S->C to all: NetPlayerSoundPacket ("asnd" / "rsnd"; the "asnd" join list)
#define NET_PACKET_CONFIRM_INTERACT (NET_PACKET_BLOCK_TRIGGERS + 6) // S->C to the player: NetConfirmPacket ("cnfm" uid, kind); C->S: NetConfirmPacket (uid ignored)
#define NET_PACKET_CHANGE_POSITION (NET_PACKET_BLOCK_TRIGGERS + 7)  // S->C to all: NetChangePositionPacket ("chgp")
#define NET_PACKET_PICKUP (NET_PACKET_BLOCK_TRIGGERS + 8)           // S->C to all: NetPickupPacket (the pickup half of v7 "inat"; joiners: "pkups")
// NET_PACKET_BLOCK_TRIGGERS + 9: free

#define NET_VALUE_RESET 0x01 // NetValuePacket.flags: clear the client's set first (join list / round start)
typedef struct {
    uint8_t flags;                      // NET_VALUE_*
    char key[TRIGGER_VALUE_KEY_SIZE];   // "" with NET_VALUE_RESET: only the clear
    double value;
} NetValuePacket;

typedef struct {
    int32_t id;       // gui element id
    uint8_t status;   // 0 toggle, 1 show, 2 hide
} NetGuiTogglePacket;

typedef struct {
    int32_t ambient;  // config.soundScapes value: "ambient_<n>"
} NetAmbientPacket;

typedef struct {
    char name[NET_SOUND_NAME_SIZE]; // emitter sound name or the asset id digits
    uint8_t asset;                  // user asset sound (no asset sounds natively)
} NetStopSoundPacket;

typedef struct {
    int32_t uid;                    // the player the loop follows
    uint8_t add;                    // 1 asnd, 0 rsnd
    uint8_t force;                  // rsnd force: every sound of that player
    uint8_t loop;                   // asnd data.loop
    float volume, rate;             // asnd data.vol / data.rate
    char name[NET_SOUND_NAME_SIZE];
} NetPlayerSoundPacket;

typedef struct {
    int32_t uid;      // S->C: the object awaiting confirmation (pendingTrigger / pendingSpin), -1 clears
    uint8_t kind;     // 0 trigger, 2 prize wheel
} NetConfirmPacket;

typedef struct {
    int32_t uid;      // the moved player
    float x, y, z;    // feet
    uint8_t recon, stop_momentum, has_direction;
    int32_t direction;
} NetChangePositionPacket;

typedef struct {
    int32_t uid;      // the pickup's map object uid
    int32_t weapon;   // weapon id it holds now, -1 empty
    uint8_t active;
    uint8_t moved;    // position below is valid (a drop moved it; else it sits at its origin)
    float position[3];
    int32_t ammo;     // pickupAmmo
} NetPickupPacket;

// NET_PACKET_INTERACT from a v7 client (the v3 NetInteractPacket is the first two bytes): action 1
// with lastInteractData {type, uid}; servers tell the two apart by the length.
typedef struct {
    uint8_t action;     // 0 spray, 1 interact, 2 drop weapon ("drpw")
    uint8_t secondary;  // intSec
    int32_t type;       // lastInteractData.type (GameInteractType)
    int32_t uid;        // lastInteractData.id
} NetInteractV7Packet;
// ---- end WP-C packets ----
#pragma pack(pop)

typedef struct {
    uint8_t type;
    uint16_t length;
    uint8_t payload[NET_MAX_PAYLOAD];
} NetPacket;

typedef struct {
    size_t length;
    uint8_t data[NET_BUFFER_SIZE];
} NetPacketBuffer;

int net_send_packet(int, NetPacketType, const void*, uint16_t);
int net_buffer_push(NetPacketBuffer*, const void*, size_t);
int net_buffer_next(NetPacketBuffer*, NetPacket*);
void net_packet_from_input(NetInputPacket*, const Input*);
void net_packet_to_input(Input*, const NetInputPacket*);
int net_packet_read_input(NetInputPacket*, const NetPacket*); // full, 23-byte v2 or 22-byte v1 INPUT; 0 = malformed
void net_packet_from_player(NetStatePacket*, const Player*);
void net_packet_apply_player(Player*, const NetStatePacket*);
void net_packet_from_recon(NetReconPacket*, const Player*, int32_t ack_seq); // buildLPacket
void net_packet_apply_recon(Player*, const NetReconPacket*);                // reconcilePlayer's snap (no replay)
// WP-C: a V1 (58-byte) or V2+ RECON; the v7 fields a V1 lacks come from `keep` (the client's own
// player: no change), or 0 without one. 0 = malformed.
int  net_packet_read_recon(NetReconPacket*, const NetPacket*, const Player *keep);
int  net_packet_read_hello(NetHelloPacket*, const NetPacket*); // 2-byte (no patch: 0) or longer HELLO; 0 = malformed
int  net_packet_read_init(NetInitPacket*, const NetPacket*);   // the full INIT or longer (prefix); 0 = malformed
// WP-C v7.1.4 netRate (settings.js network.netRate, index.js gameLoop): the client samples one input
// frame when UTILS.roundToClos(inputFrameDelta, 1000 / netRate) >= 1000 / netRate, i.e. once the
// frame time summed since the last sample reaches half a period; the frame's delta is that sum.
#define NET_RATE_COUNT 6
#define NET_RATE_DEFAULT_INDEX 3        // 144 Hz (mobile: 1, 60 Hz)
int   net_rate_hz(int index);           // options {30, 60, 120, 144, 240, 360}; out of range: the default
float net_input_rate_take(float *accum, float frame_delta, int rate_hz); // adds frame_delta (s); the input delta (s) to sample now, or -1
int  net_packet_read_input_batch(NetInputBatchPacket*, const NetPacket*);
size_t net_reconcile(Player*, const NetReconPacket*, Input *pending, size_t count, int move_lock); // snap + replay, returns pending left
void net_recon_take_vel_obj(Player*, Input *pending, size_t count); // the last input's shot knockback / grapple -> its velObj / grapObj
int  net_recon_due(Player*, double now, double *next_at); // the server's "l" schedule: 1 = send one now
// io_tp's ack state: feet at x/y/z, no velocity, on the ground, no jump/wall/ladder/crouch/
// slide carry-over, canSlide (server, offline game and the client's own copy).
void net_player_teleport(Player*, float x, float y, float z);
// Chat helpers shared by the native and the local server.
int net_chat_sanitize(char *out, size_t size, const char *in, size_t length); // 1 = non-empty message
typedef struct {
    float window_start;
    int count;
} NetChatLimiter;
int net_chat_allow(NetChatLimiter*, float now); // 0 = burst exceeded (Krunker closes the socket)
void net_packet_chat(NetChatPacket*, int uid, NetChatKind, const char *name, const char *text);
int net_ping_update(int previous_ms, float rtt_seconds); // EWMA smoothing, previous < 0 = first sample
int net_packet_read_ent(NetEntPacket*, const NetPacket*); // 4-byte (class only), 8-byte (no cosmetics) or full ENT; 0 = malformed
int net_packet_read_spawn(NetSpawnPacket*, const NetPacket*); // 78-byte (no cosmetics), V2, V3 or longer SPAWN; 0 = malformed
// FNV-1a of cJSON_PrintUnformatted(map) (cached per map object); json_out/size_out
// optionally receive a malloc'd copy of that text. 0 when the map cannot be printed.
uint32_t net_map_hash(const cJSON *map, char **json_out, size_t *size_out);

char *concat(const char*, const char*);
const char *assets_path();         // the game content: KRUNKNATIVE_ASSETS_DIR, else assets_default_path()
// "./assets/" from the install / repo root, "../assets/" from cmake-build-*: cwd-based, never the
// KRUNKNATIVE_ASSETS_DIR override.
const char *assets_default_path();
const char *ui_default_path();     // "./ui/" or "../ui/": the menu page beside assets_default_path()
int ensure_asset_file(const char*); // web: fetch into MEMFS on demand; native: 1 unless the fetch hook says missing
// The URL an asset path ("/assets/textures/x.png") is downloaded from, malloc'd (caller frees):
// web: web_asset_url (asset host + ?v=<version>), native: a copy of the path. NULL on OOM.
char *asset_url(const char*);
// web: starts a background download of the asset into MEMFS so a later ensure_asset_file()
// does not block the frame on the network; 1 = present, 0 = still downloading, -1 = the
// download failed (ensure_asset_file() reports it). Native: the fetch hook's answer (1 without one).
int asset_prefetch(const char*);
// web: asset_prefetch() every path (full paths under assets_path()) and wait until all have
// arrived or failed, so a load step pays one round trip instead of one per file. Native: the fetch
// hook's downloads of those paths are waited for (KRUNKNATIVE_ASSET_WAIT_MS, default 10 s in all).
void asset_prefetch_wait(const char *const *paths, size_t count);
// Called with the path wherever an asset load may yield to the browser on the web (a blocking
// ensure_asset_file() fetch, asset_prefetch_wait()), before it does; natively at every
// ensure_asset_file() / asset_prefetch_wait() call, so tests can emulate those yields
// (client/src/frame_target.c, KRUNKNATIVE_YIELD_CHECK). NULL (the default) turns it off.
void asset_set_yield_hook(void (*hook)(const char *path));
// Native on-demand game content fetch (client/include/asset_fetch.h): consulted by
// ensure_asset_file() (enqueue 1) and asset_prefetch() (enqueue 1) with every path that is not
// redirected by a resource pack / swapper. Answers 1 = read it (present, or not a managed file),
// 0 = missing or failed its manifest size check: a background download is queued / running,
// -1 = missing and the download gave up. ensure_asset_file() then returns 1 only for 1, so a
// loader can show its placeholder (asset_prefetch() == 0: it pops in later). NULL = off (server,
// tools, tests, the web build).
void asset_set_fetch_hook(int (*hook)(const char *path, int enqueue));
// Bumped by the client when fetched files arrived (asset_fetch_client.c): the HTML engine probes
// its page images that were missing again (html/engine/image.c).
unsigned asset_arrival_generation(void);
void asset_arrival_bump(void);
int read_file(const char*, unsigned char**, size_t*);
// Path redirect consulted by read_file() (and the client's texture loader): the client's
// resource swapper (client/src/ui/ui_client_settings.c) maps a file to its swapped copy.
// The hook returns 1 and writes the replacement path into `out` when it redirects. No
// hook (the server, tools, tests): every path is read as given.
typedef int (*FileRedirectFn)(const char *path, char *out, size_t n);
void file_set_redirect(FileRedirectFn fn);
// A second redirect consulted before that one: the loaded map's resource pack (the client's
// client/src/mod_pack.c, v7 loadModPack). On the web ensure_asset_file() / asset_prefetch()
// follow both, so a redirected file is not downloaded.
void file_set_overlay(FileRedirectFn fn);
const char *file_redirect(const char *path, char *buf, size_t n); // `buf` when redirected, else `path`
int load_json_file(const char*, cJSON**);
int parse_hex_color(const char*, vec4*);
vec4 hex_to_vec(int);
float normalize_angle(float);
float progress_on_line(vec2, vec2, vec2);
float line_in_rect(vec3, vec3, vec3, vec3);
void angles_from_sides(float, float, float, float*);

static inline void mat3x3(const float *a, const float *b, float *out) {
#define MAT3x3 \
    X(0) X(1) X(2) \
    X(3) X(4) X(5) \
    X(6) X(7) X(8)

#define X(i) out[i] = a[(i / 3) * 3] * b[(i % 3)] + a[(i / 3) * 3 + 1] * b[(i % 3) + 3] + a[(i / 3) * 3 + 2] * b[(i % 3) + 6];
    MAT3x3

#undef X
#undef MAT3x3
}

static inline void mat4x4(const float *a, const float *b, float *out) {
#define MAT4x4 \
    X(0) X(1) X(2) X(3) \
    X(4) X(5) X(6) X(7) \
    X(8) X(9) X(10) X(11) \
    X(12) X(13) X(14) X(15)

#define X(i) out[i] = a[(i / 4) * 4] * b[(i % 4)] + a[(i / 4) * 4 + 1] * b[(i % 4) + 4] + a[(i / 4) * 4 + 2] * b[(i % 4) + 8] + a[(i / 4) * 4 + 3] * b[(i % 4) + 12];
    MAT4x4

#undef X
#undef MAT4x4
}

#define CROP(x, lim) (x <= lim && x >= -lim ? 0 : x)
#define CLAMP(x, min, max) (x < min ? min : x > max ? max : x)
#define MIN(a, b) (a < b ? a : b)
#define MAX(a, b) (a > b ? a : b)
