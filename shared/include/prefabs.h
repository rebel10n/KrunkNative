#pragma once
// Krunker v7.1.4 prefab and texture tables (doc/v7/map-logic.md §1.4, §1.5.1), shared by the map
// loader (shared/src/map.c), validation (map_validate.h), the server, the client renderer and the
// editor tables (tools/editor/gen_editor_data.py).
//
//   g_prefab_data / g_prefab_textures   GENERATED from v7 data/prefabs.js (shared/src/prefab_data.c,
//                                       tools/maps/gen_prefab_data.py): names, sizes, variants and
//                                       every boolean prefab property as a PrefabFlag bit.
//   prefab_info()                       hand-written runtime semantics (shared/src/prefabs.c): what
//                                       v7 map.js generate() / MapManager build for each prefab id.
//
// Dependency-free (no cJSON, no Game) so tools such as kn_editor_mcp can link it.
#include <stdint.h>
#include <stddef.h>
#include <prefab_flags.h>

#define PREFAB_TABLE_COUNT 97  // == PREFAB_COUNT (shared.h Prefab)
#define PREFAB_TEXTURE_COUNT 20

typedef struct {
    const char *name;            // "Classic 2018"
    int id;                      // appended to the src: "<src>_<id>"
    unsigned char diff_model;    // diffM: model *and* texture "<src>_0_<id>"
    unsigned char no_emissive;   // noEmis
    unsigned char complex;       // complex (AO box / collision of the variant mesh)
} PrefabVariant;

typedef struct {
    const char *name;            // "CUBE"
    const char *src;             // prefab src ("cube_0"), NULL none
    const char *model;           // models/<model>.obj of a gen (addMesh) prefab, NULL otherwise
    float default_size[3];       // defaultSize, else the model bounds at `scale`
    int has_default_size;        // 1 defaultSize, 2 computed from the model, 0 none
    float scale;                 // model scale (1 otherwise)
    int line_col;                // editor bounding-box colour, -1 default
    float opacity;               // editor material opacity
    const char *ed_cat;          // editor category ("Lights", "Zones", ...), NULL none
    int tex_range;               // EVENT / GIFT: random texture suffix 0..tex_range when tv is absent
    int variant_count;
    const PrefabVariant *variants; // "tv" values
    uint64_t flags[2];           // PrefabFlag bits
} PrefabData;

typedef struct {
    const char *name;            // "WALL"
    const char *alias;           // editor label ("Stone"), NULL = name
    const char *src;             // "wall_0" (GRASS "grass_1")
    int variant_count;
    const PrefabVariant *variants;
} PrefabTextureData;

extern const PrefabData g_prefab_data[PREFAB_TABLE_COUNT];
extern const PrefabTextureData g_prefab_textures[PREFAB_TEXTURE_COUNT];
extern const char *const g_prefab_flag_names[PF_FLAG_COUNT];

// ---- runtime semantics (map.js generate dispatch; doc/v7/map-logic.md §1.4 table)

typedef enum {
    PREFAB_GEO_NONE,       // tools, zones, editor helpers, sound emitters: nothing rendered in game
    PREFAB_GEO_CUBE,       // generateCube with faces f (CUBE, GATE, DEPOSIT_BOX, TRIGGER)
    PREFAB_GEO_MODEL,      // addMesh models/<src>.obj (+ variants)
    PREFAB_GEO_PLANE,      // addFixedPlane family (PLANE, BILLBOARD, RUNE, SPRAY, LIGHT_BAR)
    PREFAB_GEO_CANVAS,     // canvas text plane (SIGN, LEADERBOARD, PRIZE_WHEEL)
    PREFAB_GEO_SPRITE,     // SPRITE
    PREFAB_GEO_CYLINDER,   // CYLINDER, LIGHT_CONE
    PREFAB_GEO_SPHERE,
    PREFAB_GEO_RAMP,
    PREFAB_GEO_LADDER,     // LADDER, ROPE_LADDER
    PREFAB_GEO_LIQUID,
    PREFAB_GEO_PARTICLES,
    PREFAB_GEO_LIGHT,      // point / directional / spot / rect-area lights
    PREFAB_GEO_SPECIAL,    // own builder: flag, objective, pickup, showcase, asset, pad, machines, AI
} PrefabGeometry;

typedef enum {
    PREFAB_COL_NONE,       // never collides (render-only, editor helpers, pass-through volumes)
    PREFAB_COL_OPTIONAL,   // a solid box unless "l" / "col" (noCol)
    PREFAB_COL_OPTIONAL_CYLINDER, // CYLINDER: a solid cylinder unless noCol
    PREFAB_COL_FORCED,     // always a box (zones, tools, interact objects); behaviour comes from its flags
    PREFAB_COL_FORCED_CYLINDER,   // ROPE_LADDER
} PrefabCollision;

enum {
    PREFAB_R_NO_OBJECT = 1 << 0,     // builds no map object (no uid): TERMINAL, AI_SPAWNER, BOT_SPAWNER, EVENT
    PREFAB_R_RENDER_ONLY = 1 << 1,   // client visual only (lights, light bars, sprites, sprays...)
    PREFAB_R_MAP_DETAIL = 1 << 2,    // built only with the client's "map details" setting (mapDet)
    PREFAB_R_EDITOR_ONLY = 1 << 3,   // exported to other keys / not built (SPAWN_POINT, CAMERA_POSITION, PLACEHOLDER)
    PREFAB_R_NO_SHOOT = 1 << 4,      // noShoot: bullets pass
    PREFAB_R_NO_WALL_JUMP = 1 << 5,  // never wall-jumpable (non-solid or own chain branch)
    PREFAB_R_HEALTH = 1 << 6,        // health / cdy / sd / rt / rr / rsc are read
    PREFAB_R_INTERFACE = 1 << 7,     // "in" is read
    PREFAB_R_INTERFACE_CLAMP = 1 << 8, // "in" clamped to 0..interfaceIDMax
    PREFAB_R_BORDER = 1 << 9,        // "bo" border (borderH above the top)
    PREFAB_R_KEEP_NONSOLID = 1 << 10,// keeps health / interface although its collision is NONE
    PREFAB_R_LIGHT = 1 << 11,        // a light source (client)
    PREFAB_R_ZONE = 1 << 12,         // a behaviour volume (the prefab's zone / tool flag)
};

typedef struct {
    unsigned char geometry;      // PrefabGeometry
    unsigned char collision;     // PrefabCollision
    unsigned short roles;        // PREFAB_R_*
} PrefabInfo;

// NULL for ids outside 0..96.
const PrefabInfo *prefab_info(int id);
const PrefabData *prefab_data(int id);
int prefab_has_flag(int id, PrefabFlag flag);
int prefab_has_role(int id, unsigned roles); // any of `roles`
// v7 name ("CAMERA") or a v3.1.1 alias ("SPECTATE_CAM", "BOT"); case-sensitive. -1 none.
int prefab_id_from_name(const char *name);
const char *prefab_name(int id); // "CUBE", NULL out of range

// ---- textures (§1.5.1)
// map.js: t == null ? (LADDER / ROPE_LADDER ? 2 : 0) : textures[t || 0] || the same default.
// Only numbers (and numeric strings) index; a texture *name* falls back to the default (v7).
int prefab_texture_resolve(int prefab, int has_t, int t);
// "<src>" + ("_" + variants[tv].id when tv is non-zero and that variant exists), into out.
// Returns out, or NULL when the texture index is invalid.
const char *prefab_texture_src(int texture, int tv, char *out, size_t size);
int prefab_texture_id_from_name(const char *name); // "WALL" / alias "Stone" (editor import), -1 none
