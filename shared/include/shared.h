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

extern const GameConstants game_constants;

// Krunker v7.1.4 data/prefabs.js prefab ids (map object "i"; doc/v7/map-logic.md §1.4).
// Ids 0-59 keep their v3.1.1 index; 33 and 55 were renamed (the v3 names stay as aliases);
// 60-96 are new. The per-prefab data is shared/include/prefabs.h (g_prefab_data, prefab_info).
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
    PREFAB_TERMINAL,            // 41: v7 builds nothing (no object, no uid)
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
    PREFAB_COUNT,                // 97
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
    float hitbox_h, hitbox_w;           // "hbh" / "hbw" 0.1..10 (1)
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

    CollisionType collision_type;

    vec3 position;
    vec3 scale;

    TextureAnimation *tex_anim;
    Ramp *ramp;
    void *mesh;
} Object;

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
    const cJSON *bot_paths;             // "botPaths" (no v7 runtime consumer; kept for round-trip)
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
void map_object_rotation(const Object *object, float out[9]);
void map_reset(Map*);
void map_fini(Map*); // NOTE: ensure no meshes from the map are part of the scene, they are free()'d!

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
} ClassConfig;

#define CLASS_COUNT 16
#define CLASS_ID_SURVIVOR 14
#define CLASS_ID_INFILTRATOR 15
extern const ClassConfig g_classes[CLASS_COUNT];

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
    unsigned char req_account:1; // reqAccount: guests can't earn it (sandbox and offline can)
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
    // input[10]: the wheel, wDG6 * scrollDirM (-1 / 0 / 1), only when no swap key was
    // pressed; procInputs steps the weapon index with reload/pY2 (player_scroll_weapon).
    signed char scroll;
    // input.velObj (players.js shoot, client only, never sent): the physPow knockback this
    // frame's shot gave the local player; a reconciliation replay (recon) applies it again,
    // since replays skip shooting (index.js reconcilePlayer -> procInputs(recon = true)).
    unsigned char has_vel_obj;
    vec3 vel_obj;
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
#define PLAYER_CHECKPOINT_LIST_MAX 256 // single-use check points one player can remember

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

// Krunker's per-player look (jLrR6): skinColIndex / hairCol and the hat / back / waist / face / dye store items.
// Only int32 members, so it embeds unchanged in the packed net packets (ENT, SPAWN).
typedef struct {
    int32_t skin_color; // 0xRRGGBB (config.skinColors[skinColIndex].col), -1 = the class colour
    int32_t hair_color; // 0xRRGGBB, -1 = the class colour
    int32_t hat;        // store.skins index, -1 = none (hatIndex)
    int32_t back;       // backIndex
    int32_t waist;      // waistIndex
    int32_t face;       // faceIndex
    int32_t dye;        // dyeIndex
} PlayerCosmetics;

#define PLAYER_COSMETICS_NONE ((PlayerCosmetics) {-1, -1, -1, -1, -1, -1, -1})

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
} PlayerStoredState;

typedef struct Player_t {
    struct Game_t* game;

    int uid;

    unsigned char is_you:1;
    unsigned char render_you:1;
    unsigned char left_handed:1;

    unsigned char active:1;
    unsigned char noclip:1;
    unsigned char god_mode:1;
    unsigned char unlimited_ammo:1;
    unsigned char force_hide:1;
    unsigned char smite:1;      // /smite: every hit this player deals does 10000 damage (Krunker player.smite)
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
    int score;                    // Krunker player.score
    int reward;                   // Krunker player.reward: KR of the last endGame (END "reward" column)
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
    vec3 shot_vel_obj;

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
    float slide_timer;
    float jump_timer;
    float interact_progress;
    float last_interact;          // players.interact: game.now of the last "int" (config.interactTimer)
    int spray_index;              // player.sprayIndex (ENT ent[1]): sprays.js index or a type-4 store.skins index
    float last_spray;             // players.spray: game.now of the last spray (config.sprayTimer)

    float *ramp_fix;

    float idle_anim;
    float hp_chase;

    vec3 position;
    vec3 last_position;
    vec3 velocity;
    vec2 direction;

    float x_vel_cylinder;
    float z_vel_cylinder;

    float jump_rotate;

    float land_bob_y;
    float land_bob_yr;
    float jump_bob_y;
    float jump_rotate_mlt;
    float step_val;
    float step_chase;
    unsigned char step_dir_neg:1; // Krunker stepDir: 0 = +1, 1 = -1 (flips at +-PI/2)

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

    Input *input_queue;
    size_t input_queue_size;

    const Weapon *weapon;

    PlayerSkins skins;            // weapon / melee store skins (ENT -> SKINS), PLAYER_SKINS_NONE by default
    void *mesh;

    ModePlayerState mode;         // game mode stats (lives, caps, tier, ...), shared/src/modes

    // Lag compensation (players.js storeState / fetchState / getStateConst, player.c).
    float ping;                   // player.ping: smoothed RTT in ms (server: ClientConnection.ping_ms)
    float lag_comp;               // player.lagComp: ENT lag_comp (ent[11] slider, server-clamped to 0.1..1), default 1
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
    unsigned char is_juggernaut:1;// isJuggernaut (Juggernaut streak): 2000 HP, Minigun, no regen
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
    int attach_index;             // attachIndex (ENT / spawn): the sight on attach weapons, 0 Reflex, 1 Acog, -1 none
    unsigned char upgraded_slots; // player.upgrades: bit per loadout slot holding its upgradeWeapons entry (krumMachine)
    int weapon_streak;            // weaponStreak: kills in a row with streak_weapon ("wstk")
    int streak_weapon;            // currentStreakWeapon, -1 none
    unsigned char has_account:1;  // player.account (server: a backend account or the host's own machine,
                                  // which covers the offline sandbox, decisions.md): reqAccount streaks
    // ---- end WP-B combat state ----
} Player;

typedef struct {
    int streak;
    int weapon_id;      // index into Game.weapons, -1 = none (fall/nuke)
    unsigned char headshot:1;
    unsigned char wallbang:1;
    unsigned char fists:1;  // Krunker fs: melee without the knife
    unsigned char thrown:1; // Krunker pj: projectile kill (thrown knife -> Bullseye)
    float distance;     // Krunker dst: fraction of the weapon range
} PlayerKillInfo;

Player *player_init(struct Game_t*);
void player_spawn(Player*);
void player_swap_weapon(Player*, int, int, int, int);
void player_update_height(Player*); // players.updateHeight: height from crouch_val
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
void player_scroll_weapon(Player*, int dir, int recon); // manager.reload(player, +-1): wheel / melee on melee
void player_update(Player*, float);
float player_lerp_angle(float a, float b, float t); // index.js Math.lerpAngle (remote yaw/pitch interpolation), shortest arc
void player_reset_feel(Player*); // spawn: step/bob/lean/recoil animation state back to rest
void player_inspect_weapon(Player*); // wInspect: toggle the weapon inspect (local player, inspKey)
void player_cancel_inspect(Player*);
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
// Hit zones of a hit at world height `hit_y` on `target` (players.js shoot): head when within
// headScale * headMlt of the top of the box `base_y + adjusted_height`, legs below legHeight
// minus the crouch drop. Used by hitscan, melee and projectiles.
int player_hit_is_head(const Player *target, float base_y, float adjusted_height, float hit_y);
int player_hit_is_leg(const Player *target, float base_y, float hit_y);
// Damage of one weapon hit after head / leg multipliers (noHeadShot, per-weapon lsMlt, no leg
// multiplier on a headshot) and headshotOnly; `damage` is after falloff.
float player_hit_damage(const struct Game_t*, const Weapon*, float damage, int head, int leg);
// ---- end WP-B ----

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
    unsigned int no_charge_weapons:1; // !chrgWeps

    int *maps;
    int map_count;

    int *modes;
    int mode_count;
} GameConfig;

extern const GameConfig g_default_game_config;

typedef struct {
    float timer;

    int votes_for;
    int votes_req;

    Player *player;
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

typedef struct Game_t {
    GameConfig config;
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

    const EffectConfig *effects;  // WP-B: per-game effects.js table (custom-game prks overrides), NULL = g_effects
    float nuke_timer;
    Player *nuke_player;

    VoteKick *vote_kick;

    void *server; // TODO
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
void game_broadcast();
int game_is_authority(const Game*);      // server or local game: runs damage/kills/score
// io_ent spawn rules: a ready round, not the end screen, not alive, config.deathDelay since
// the last death, lives left (`player` NULL: first spawn). Server and offline client alike.
int game_can_spawn(const Game*, const Player*);
int game_end_late_spawn(const Game*, const Player*); // ENT while the round is over (vote window): spawn anyway
void game_send_end_late(Game*, int uid);              // then the "end" with didWin -1 for that uid
void game_player_changing(const Game*, Player*, int done); // GameHooks.player_changing, when set
void game_send(Game*, int uid, int type, const void*, uint16_t); // through hooks.send
Player *game_find_player(Game*, int uid);
void game_sync_leaders(Game*, int uid);  // LEADERBOARD to `uid` (or NET_TARGET_ALL)
void game_start_round(Game*);            // fresh round timer + stats (after game_init)
void game_end_round(Game*);              // Krunker endGame(3): END packets, players cleared
void game_set_round_time(Game*, float seconds); // debug: seconds left in the running round (TIMER goes out next tick)
// Interaction (shared/src/interact.c): players.interact / the terminal checks of Player.
#define GAME_TERMINAL_RADIUS 7.0f      // addTerminal tRadius
#define GAME_INTERACT_TIMER 0.1f       // config.interactTimer (s)
#define GAME_SPRAY_TIMER 1.0f          // config.sprayTimer (s)
#define GAME_SPRAY_RANGE 25.0f         // config.sprayRange
#define GAME_SPRAY_SCALE 15.0f         // config.sprayScale: the decal's width and height
int  game_terminal_in_range(const Game*, const Player*, int prompt); // map object index of a terminal the player can open, -1 none; prompt: skip noMsg ones
void game_player_interact(Game*, Player*, int action, int secondary); // "int": 0 spray (game_player_spray), 1 terminals / gates / banks (authority)
void game_player_spray(Game*, Player*); // players.spray: NET_PACKET_SPRAY + the "spray" sound when the view ray hits a wide enough face
// players.js changeInteract priorities: the #interactMsg line the local player gets this tick.
typedef enum {
    GAME_INTERACT_NONE = -1,
    GAME_INTERACT_GATE = 0,       // "Press [key] to Open/Close gate [price]"
    GAME_INTERACT_BANK = 1,       // deposit box: deposit / withdraw
    GAME_INTERACT_TERMINAL = 2,   // "Press [key] to Open Terminal"
    GAME_INTERACT_TRIGGER = 4,    // onInteract trigger: "Press [M1] to Interact" (priority 1)
} GameInteractType;
#define GAME_INTERACT_RADIUS 7.0f      // addGate / addBank / addTerminal tRadius
int game_interact_prompt(const Game*, const Player*, int *object_index); // GameInteractType, object index (or -1)

// Map triggers, interfaces, check points, teleporters (shared/src/triggers.c).
typedef enum {
    GAME_HIT_SHOT,          // hitscan: onShoot / onDamage
    GAME_HIT_MELEE,         // melee swing: onMelee / onDamage / onPunch (knife thrown)
    GAME_HIT_PROJECTILE,    // projectile: onShoot / onDamage / onProjectile (+ onThrowingMelee for the knife)
} GameHitKind;
struct GameAI_t;
void game_on_trigger(Game*, Player *source, Object *object, int *chain); // game.onTrigger (authority); chain NULL starts one
void game_trigger_ai_death(Game*, Player *source, struct GameAI_t *ai); // AI.kill: onAIDeath -> game.onTrigger(source, ai)
int  game_object_event(const Object*);                  // the object's TriggerEvent (TRIGGER_EVENT_NONE without one)
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
void game_set_endless(Game*, int on);   // /endless: pause the round clock (TIMER keeps the frozen value) / resume it from there
void game_send_round_state(Game*, int uid); // TIMER (+ END while the end screen is up) for a joiner
void game_player_left(Game*, Player*);   // drop references (nuke, damage) to a leaving player
int game_pick_next_map(const Game*);     // next round's map: random rotation map (no map votes)
int game_pick_next_mode(const Game*);    // next round's game->modes index: FFA when rotated, else random (endData.mts[0])
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
    NET_PACKET_INTERACT = 61,    // C->S: NetInteractPacket (Krunker "int" action [, secondary])
    NET_PACKET_TERMINAL = 62,    // S->C to the player: NetTerminalPacket (Krunker "ot" uid: openTerminal)
    NET_PACKET_TERMINAL_MSG = 63,// C->S: NetTerminalMsgPacket (Krunker "tmc" uid, text)
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
} NetChatKind;

enum {
    NET_INPUT_SHOOT = 1 << 0,
    NET_INPUT_SCOPE = 1 << 1,
    NET_INPUT_JUMP = 1 << 2,
    NET_INPUT_CROUCH = 1 << 3,
    NET_INPUT_RELOAD = 1 << 4,
};

#pragma pack(push, 1)
typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
} NetHelloPacket;

typedef struct {
    uint8_t major;
    uint8_t minor;
    uint8_t patch;
    int32_t map_index;
    int32_t mode_index;
    uint16_t tick_rate;
    int32_t client_id;
} NetInitPacket;

// INIT major.minor: the protocol the server speaks. 1.0 servers drop ENTs longer than V4 (38 bytes)
// without spawning anyone, so a client only sends the longer ENT (V5 spray, V6 weapon skins) to a
// server that announces at least 1.1 (client_enter_game).
#define NET_PROTOCOL_MAJOR 1
// 1.1 servers accept ENTs up to V6 and drop V7 (1.2: + lagComp) the same way.
// 1.3: the Krunker v7.1.4 map schema (WP-A, doc/v7/map-logic.md §7.2): prefabs 0-96, no TERMINAL
// objects, so object uids differ from a 1.2 peer's on the same map. New v7 packets (blocks below)
// are only sent to peers that announced at least NET_PROTOCOL_MINOR_V7.
#define NET_PROTOCOL_MINOR 3
#define NET_PROTOCOL_MINOR_V7 3

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
#define NET_PACKET_BLOCK_END 240
uint16_t net_ent_size_for(int major, int minor); // the ENT a client sends to an INIT major.minor server: V7 from 1.2, V6 for 1.1, else V4

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
} NetEntPacket;

#define NET_ENT_PACKET_MIN_SIZE 4 // class_index only (older clients / bridges)
#define NET_ENT_PACKET_V2_SIZE 8  // class_index + secondary_index (before cosmetics)
#define NET_ENT_PACKET_V3_SIZE (offsetof(NetEntPacket, spectate)) // up to cosmetics (older clients)
#define NET_ENT_PACKET_V4_SIZE (offsetof(NetEntPacket, slid_cont) + 1) // + spectate + slid_cont
#define NET_ENT_PACKET_V5_SIZE (offsetof(NetEntPacket, skins)) // + spray_index
#define NET_ENT_PACKET_V6_SIZE (offsetof(NetEntPacket, lag_comp)) // + weapon / melee skins (protocol 1.1)
#define NET_ENT_PACKET_V7_SIZE (sizeof(NetEntPacket)) // + lagComp (protocol 1.2): what a client sends

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
} NetSpawnPacket;

#define NET_SPAWN_PACKET_V1_SIZE 78 // up to secondary_index (before cosmetics)

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
} NetReconPacket;

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
    uint8_t fists;
    int32_t victim_deaths;
    int32_t killer_kills;
    int32_t killer_class;  // kill card icon
    int32_t score;         // kill reward the killer earned (kill card "+N"), 0 none
    float respawn_delay;   // seconds before the victim may respawn (config.deathDelay)
} NetKillPacket;

typedef struct {
    int32_t target_uid;
    int32_t damage;        // Math.round(dmg)
    uint8_t crit;          // headshot
    uint8_t kill;
    uint8_t ai;            // target_uid is an AI sid (Krunker "4" fifth value isAI)
} NetHitPacket;

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
    uint8_t first;         // first ray of the shot (shotguns send one per pellet): play the sound once
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
    int32_t id;
    float position[3];
    uint8_t impact;        // impact particles at the end point
} NetProjectileEndPacket;

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

// NET_PACKET_BILLBOARD: the server's billboard config (v3.1.1 game.updateBillboards: txt,
// txtCol, bgCol) painted on every "sad" BILLBOARD map object. Empty fields = the engine
// defaults ("Your Ad Here", #e3e3e3, #000000).
typedef struct {
    char txt[64];
    char txt_col[8];       // "#rrggbb"
    char bg_col[8];
} NetBillboardPacket;

// NET_PACKET_PLAYER_INFO: account data of a player (uid) for the scoreboard, nametags
// and kill feed. account_id / level 0 = guest.
enum {
    NET_PLAYER_INFO_PREMIUM = 1 << 0,
    NET_PLAYER_INFO_VERIFIED = 1 << 1, // featured / verified account
    NET_PLAYER_INFO_DEV = 1 << 2,
    NET_PLAYER_INFO_MOD = 1 << 3,
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
// client_key = "hj1:" + client nonce; the server asks <auth>/auth/hasJoined.
// Strings NUL-terminated; public_key "" while the transport has no key.
typedef struct {
    char server_nonce[33];  // 32 lowercase hex, random per server boot
    uint8_t online_mode;    // 1 = accounts verified through the auth service; 0 = guests
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
} NetCheatFlag;

typedef struct {
    uint8_t flag;   // NetCheatFlag
    uint8_t on;
} NetCheatPacket;

// "int": 0 spray, 1 interact (gates / pickups / terminals; secondary = interactSecKey), 2 drop.
typedef struct {
    uint8_t action;
    uint8_t secondary;
} NetInteractPacket;

typedef struct {
    int32_t uid;    // the terminal's map object index (Krunker GameObject uid)
} NetTerminalPacket;

typedef struct {
    int32_t uid;
    char text[NET_CHAT_TEXT_SIZE]; // #terminalInput value (maxlength 70), NUL-terminated
} NetTerminalMsgPacket;

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
} NetObjectStateKind;

typedef struct {
    int32_t uid;                // map object index (Krunker GameObject uid)
    uint8_t kind;               // NetObjectStateKind
    int32_t value;              // gate: active; bank: deposited
} NetObjectStatePacket;

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

typedef struct {
    int32_t uid;
    uint8_t slots;                 // bit per loadout slot: upgradeWeapons entry
    int32_t ammo[NET_LOADOUT_MAX]; // the slots' ammo after the upgrade
} NetUpgradesPacket;
// ---- end WP-B combat packets ----
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
int  net_packet_read_input_batch(NetInputBatchPacket*, const NetPacket*);
size_t net_reconcile(Player*, const NetReconPacket*, Input *pending, size_t count, int move_lock); // snap + replay, returns pending left
void net_recon_take_vel_obj(Player*, Input *pending, size_t count); // the last input's shot knockback -> its velObj
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
int net_packet_read_spawn(NetSpawnPacket*, const NetPacket*); // 78-byte (no cosmetics) or full SPAWN; 0 = malformed
// FNV-1a of cJSON_PrintUnformatted(map) (cached per map object); json_out/size_out
// optionally receive a malloc'd copy of that text. 0 when the map cannot be printed.
uint32_t net_map_hash(const cJSON *map, char **json_out, size_t *size_out);

char *concat(const char*, const char*);
const char *assets_path();
int ensure_asset_file(const char*); // web: fetch into MEMFS on demand, native: no-op returning 1
// The URL an asset path ("/assets/textures/x.png") is downloaded from, malloc'd (caller frees):
// web: web_asset_url (asset host + ?v=<version>), native: a copy of the path. NULL on OOM.
char *asset_url(const char*);
// web: starts a background download of the asset into MEMFS so a later ensure_asset_file()
// does not block the frame on the network; 1 = present, 0 = still downloading, -1 = the
// download failed (ensure_asset_file() reports it). Native: always 1.
int asset_prefetch(const char*);
// web: asset_prefetch() every path (full paths under assets_path()) and wait until all have
// arrived or failed, so a load step pays one round trip instead of one per file. Native: no-op.
void asset_prefetch_wait(const char *const *paths, size_t count);
int read_file(const char*, unsigned char**, size_t*);
// Path redirect consulted by read_file() (and the client's texture loader): the client's
// resource swapper (client/src/ui/ui_client_settings.c) maps a file to its swapped copy.
// The hook returns 1 and writes the replacement path into `out` when it redirects. No
// hook (the server, tools, tests): every path is read as given.
typedef int (*FileRedirectFn)(const char *path, char *out, size_t n);
void file_set_redirect(FileRedirectFn fn);
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
