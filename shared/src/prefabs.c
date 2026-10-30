// Krunker v7.1.4 prefab runtime semantics (shared/include/prefabs.h): what v7 data/map.js
// generate() and the MapManager builders make of each prefab id (doc/v7/map-logic.md §1.3, §1.4).
// The generated property table is shared/src/prefab_data.c.
//
// Ids 0-59 keep the collision / interface rules KN applied to them before the v7 upgrade
// (v3.1.1-shaped; the collision changes of v7 belong to WP-B and are made in player.c), except
// where WP-A adopts a v7 parse change: TERMINAL builds nothing, and the zones that v7 made
// destructible / interface-addressable (DEATH, PREMIUM, VERIFIED, TEAM zones) read h / in.
#include <prefabs.h>
#include <shared.h>

#include <stdio.h>
#include <string.h>

_Static_assert(PREFAB_COUNT == PREFAB_TABLE_COUNT, "shared.h Prefab and prefabs.h disagree");

#define G(x) PREFAB_GEO_##x
#define C(x) PREFAB_COL_##x
#define NO_OBJ PREFAB_R_NO_OBJECT
#define RENDER PREFAB_R_RENDER_ONLY
#define MAPDET PREFAB_R_MAP_DETAIL
#define EDITOR PREFAB_R_EDITOR_ONLY
#define NOSHOOT PREFAB_R_NO_SHOOT
#define NOWJ PREFAB_R_NO_WALL_JUMP
#define HP PREFAB_R_HEALTH
#define IN PREFAB_R_INTERFACE
#define INC (PREFAB_R_INTERFACE | PREFAB_R_INTERFACE_CLAMP)
#define BORDER PREFAB_R_BORDER
#define KEEP PREFAB_R_KEEP_NONSOLID
#define LIGHT PREFAB_R_LIGHT
#define ZONE PREFAB_R_ZONE
#define MODEL_PROP {G(MODEL), C(OPTIONAL), HP | INC}

static const PrefabInfo k_prefab_info[PREFAB_TABLE_COUNT] = {
    [PREFAB_CUBE] = {G(CUBE), C(OPTIONAL), HP | IN | BORDER},
    [PREFAB_CRATE] = MODEL_PROP,
    [PREFAB_BARREL] = MODEL_PROP,
    [PREFAB_LADDER] = {G(LADDER), C(FORCED), NOSHOOT},
    [PREFAB_PLANE] = {G(PLANE), C(OPTIONAL), HP | IN},
    [PREFAB_SPAWN_POINT] = {G(NONE), C(OPTIONAL), EDITOR},
    [PREFAB_CAMERA_POSITION] = {G(NONE), C(OPTIONAL), EDITOR},
    [PREFAB_VEHICLE] = MODEL_PROP,
    [PREFAB_STACK] = MODEL_PROP,
    // v10 addRamp (L165176): `noCol || new GameObject`, so a RAMP with l / col has no collision body (optional, like CUBE)
    [PREFAB_RAMP] = {G(RAMP), C(OPTIONAL), NOSHOOT | NOWJ},
    [PREFAB_SCORE_ZONE] = {G(NONE), C(FORCED), NOSHOOT | NOWJ | ZONE},
    [PREFAB_BILLBOARD] = {G(PLANE), C(OPTIONAL), HP | IN},
    [PREFAB_DEATH_ZONE] = {G(NONE), C(FORCED), NOSHOOT | NOWJ | ZONE | HP | INC},
    [PREFAB_PARTICLES] = {G(PARTICLES), C(NONE), RENDER | NOWJ},
    [PREFAB_OBJECTIVE] = {G(SPECIAL), C(FORCED), NOWJ | ZONE},
    [PREFAB_TREE] = MODEL_PROP,
    [PREFAB_CONE] = MODEL_PROP,
    [PREFAB_CONTAINER] = MODEL_PROP,
    [PREFAB_GRASS] = MODEL_PROP,
    [PREFAB_CONTAINER_R] = MODEL_PROP,
    [PREFAB_ACID_BARREL] = MODEL_PROP,
    [PREFAB_DOOR] = MODEL_PROP,
    [PREFAB_WINDOW] = MODEL_PROP,
    [PREFAB_FLAG] = {G(SPECIAL), C(FORCED), NOSHOOT | NOWJ},
    [PREFAB_GATE] = {G(CUBE), C(FORCED), INC | BORDER},
    [PREFAB_CHECK_POINT] = {G(NONE), C(FORCED), NOSHOOT | NOWJ | ZONE},
    [PREFAB_WEAPON_PICKUP] = {G(SPECIAL), C(FORCED), NOSHOOT | NOWJ},
    [PREFAB_TELEPORTER] = {G(NONE), C(FORCED), NOSHOOT | NOWJ | ZONE},
    [PREFAB_TEDDY] = MODEL_PROP,
    [PREFAB_TRIGGER] = {G(CUBE), C(FORCED), HP | INC},
    // v10.0.4 SIGN: noCol (deob-10.0.4.js:157294; forceCollision there is never read) and addCanvas builds only the
    // client plane, no GameObject (:168134-168137): a sign never collides, "l" or not
    [PREFAB_SIGN] = {G(CANVAS), C(NONE), RENDER | NOWJ},
    [PREFAB_DEPOSIT_BOX] = {G(CUBE), C(FORCED), INC | BORDER},
    [PREFAB_LIGHT_CONE] = {G(CYLINDER), C(NONE), RENDER | MAPDET | NOWJ},
    [PREFAB_CAMERA] = {G(NONE), C(NONE), NOWJ},
    [PREFAB_SPHERE] = {G(SPHERE), C(NONE), 0},
    [PREFAB_PLACEHOLDER] = {G(NONE), C(OPTIONAL), EDITOR | HP | INC},
    [PREFAB_CARD_B] = MODEL_PROP,
    [PREFAB_PALLET] = MODEL_PROP,
    [PREFAB_LIQUID] = {G(LIQUID), C(NONE), RENDER | MAPDET | NOWJ},
    [PREFAB_SOUND_EMITTER] = {G(NONE), C(NONE), NOWJ},
    // v10.0.3 EVENT (the egg hunt's eggs, doc/v10/events.md): map.generate builds it as any model (addMesh: a GameObject
    // unless noCol, isEvent, deob-10.0.3.js:187021-187059, :189737) except in a custom game, where it is skipped. KN keeps
    // it a map object in every game (the uids of KN's server and client agree before the client knows the game is
    // custom) and makes it inert in a custom one (live_events_map_objects: inactive, so no collision, shot or mesh).
    [PREFAB_EVENT] = MODEL_PROP,
    // v10.0.3 BOT_PATH_NODE (v7 TERMINAL): map.js skips both names (deob-10.0.3.js:188700), no object either way;
    // its editor data (noCol, noCollision, tool, ...) is prefab_data.c's row 41 (deob-10.0.3.js:174150)
    [PREFAB_BOT_PATH_NODE] = {G(NONE), C(NONE), NO_OBJ},
    [PREFAB_PREMIUM_ZONE] = {G(NONE), C(FORCED), NOSHOOT | ZONE | BORDER | HP | INC},
    [PREFAB_VERIFIED_ZONE] = {G(NONE), C(FORCED), NOSHOOT | NOWJ | ZONE | BORDER | HP | INC},
    [PREFAB_CUSTOM_ASSET] = {G(SPECIAL), C(OPTIONAL), HP | IN},
    [PREFAB_BOMB_SITE] = {G(SPECIAL), C(FORCED), NOWJ | ZONE},
    [PREFAB_BOOST_PAD] = {G(SPECIAL), C(OPTIONAL), 0},
    [PREFAB_TEAM_ZONE] = {G(NONE), C(OPTIONAL), NOSHOOT | NOWJ | ZONE | BORDER | HP | INC},
    [PREFAB_CYLINDER] = {G(CYLINDER), C(OPTIONAL_CYLINDER), HP | IN},
    [PREFAB_POLICE] = MODEL_PROP,
    [PREFAB_CAGE] = MODEL_PROP,
    [PREFAB_E_BARREL] = MODEL_PROP,
    // v10 addShowcase (L164331): a model only, no GameObject, so a showcase never blocks a player
    [PREFAB_SHOWCASE] = {G(SPECIAL), C(NONE), NOWJ},
    [PREFAB_POINT_LIGHT] = {G(LIGHT), C(NONE), RENDER | LIGHT | NOWJ},
    [PREFAB_GHOST] = MODEL_PROP,
    // map.js addAI: a pending AI (shared/src/ai.c spawns it at game init), not a map object
    [PREFAB_AI_SPAWNER] = {G(SPECIAL), C(NONE), NO_OBJ},
    [PREFAB_PUMPKIN] = MODEL_PROP,
    [PREFAB_RUNE] = {G(PLANE), C(OPTIONAL), HP | INC},
    [PREFAB_SKELETON] = MODEL_PROP,
    [PREFAB_KNIGHT] = MODEL_PROP,
    // ---- 60-96, new in v7
    [PREFAB_GIFT] = MODEL_PROP,
    [PREFAB_CHRISTMASTREE] = MODEL_PROP,
    [PREFAB_CANE] = MODEL_PROP,
    [PREFAB_LIGHT_BAR] = {G(PLANE), C(NONE), RENDER | MAPDET | NOWJ},
    [PREFAB_PATH_NODE] = {G(NONE), C(NONE), NOWJ},            // manager.pathNodes[nid]
    [PREFAB_DIRECTIONAL_LIGHT] = {G(LIGHT), C(NONE), RENDER | LIGHT | NOWJ},
    [PREFAB_RECTAREA_LIGHT] = {G(LIGHT), C(NONE), RENDER | LIGHT | NOWJ},
    [PREFAB_SPOT_LIGHT] = {G(LIGHT), C(NONE), RENDER | LIGHT | NOWJ},
    [PREFAB_PRIZE_WHEEL] = {G(CANVAS), C(NONE), NOWJ},       // manager.spinners (interact 9), no GameObject
    [PREFAB_KR_ZONE] = {G(NONE), C(FORCED), NOSHOOT | ZONE | BORDER | HP | INC},
    [PREFAB_NOPVP_ZONE] = {G(NONE), C(NONE), NOSHOOT | NOWJ | ZONE | BORDER | HP | INC | KEEP},
    [PREFAB_PVP_ZONE] = {G(NONE), C(NONE), NOSHOOT | NOWJ | ZONE | BORDER | HP | INC | KEEP},
    [PREFAB_LEADERBOARD] = {G(CANVAS), C(NONE), RENDER | NOWJ},
    [PREFAB_TILES] = MODEL_PROP,
    [PREFAB_CLOUDS] = MODEL_PROP,
    [PREFAB_PARTNER_ZONE] = {G(NONE), C(FORCED), NOSHOOT | ZONE | BORDER | HP | INC},
    [PREFAB_SPRITE] = {G(SPRITE), C(NONE), RENDER | NOWJ},
    [PREFAB_DOMINATION_ZONE] = {G(SPECIAL), C(NONE), NOWJ | ZONE}, // manager.domzones, no GameObject
    [PREFAB_ROPE_LADDER] = {G(LADDER), C(FORCED_CYLINDER), NOSHOOT},
    [PREFAB_ENVIRONMENT_ZONE] = {G(NONE), C(NONE), NOWJ | ZONE},  // manager.envZones (client camera)
    [PREFAB_KPD_ZONE] = {G(NONE), C(FORCED), NOSHOOT | ZONE | BORDER | HP | INC},
    [PREFAB_DUMPSTER] = MODEL_PROP,
    [PREFAB_SERVER_NODE] = {G(NONE), C(NONE), NOSHOOT | NOWJ | ZONE | HP | INC | KEEP},
    [PREFAB_LAMP] = MODEL_PROP,
    [PREFAB_WATER_TOWER] = MODEL_PROP,
    [PREFAB_AC_UNIT] = MODEL_PROP,
    [PREFAB_SHEET_METAL] = MODEL_PROP,
    [PREFAB_ANTENNA] = MODEL_PROP,
    [PREFAB_SATELLITE_DISH] = MODEL_PROP,
    [PREFAB_SPRAY] = {G(PLANE), C(NONE), RENDER | NOWJ},
    [PREFAB_WEAPONBOX] = MODEL_PROP,
    [PREFAB_MYSTERY_BOX] = {G(SPECIAL), C(FORCED), 0},               // starts inactive (switchMysteryBox)
    [PREFAB_PERK_MACHINE] = {G(SPECIAL), C(FORCED), BORDER | HP | INC},
    [PREFAB_AI_REDIRECTION] = {G(NONE), C(NONE), NOSHOOT | NOWJ | ZONE},
    [PREFAB_KRUM_MACHINE] = {G(SPECIAL), C(FORCED), BORDER},
    // map.js addBot: a pending map bot (WP-F), not a map object
    [PREFAB_BOT_SPAWNER] = {G(NONE), C(NONE), NO_OBJ},
    [PREFAB_MAP_ANCHOR] = {G(NONE), C(NONE), NOWJ},           // manager.mapAnchors (minimap bounds)
    // v10.0.2: SCRAPCRATE has no own loader branch (a generic addMesh prop, CRATE's flags at scale
    // 5); ENTITY builds no map object, map.c keeps it as an entity instance (Map.entity_instances)
    [PREFAB_SCRAPCRATE] = MODEL_PROP,
    [PREFAB_ENTITY] = {G(NONE), C(NONE), NO_OBJ},
    // v10.0.5 COLLISION_MESH (deob-10.0.5.js:158847-158862): a tool, noExport. Its triangles reach the game only
    // as the map's "tris" (the editor's export, editor-10.0.5.js:16454-16460), which map.c loads as cpx collision;
    // game.js reads no "cmt", so a saved object of it builds nothing here
    [PREFAB_COLLISION_MESH] = {G(NONE), C(NONE), NO_OBJ},
};

const PrefabInfo *prefab_info(const int id) {
    return id >= 0 && id < PREFAB_TABLE_COUNT ? &k_prefab_info[id] : NULL;
}

const PrefabData *prefab_data(const int id) {
    return id >= 0 && id < PREFAB_TABLE_COUNT ? &g_prefab_data[id] : NULL;
}

int prefab_has_flag(const int id, const PrefabFlag flag) {
    if (id < 0 || id >= PREFAB_TABLE_COUNT || (int) flag < 0 || flag >= PF_FLAG_COUNT) return 0;
    return (g_prefab_data[id].flags[flag / 64] >> (flag % 64)) & 1u;
}

int prefab_has_role(const int id, const unsigned roles) {
    const PrefabInfo *info = prefab_info(id);
    return info && (info->roles & roles) != 0;
}

int prefab_id_from_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < PREFAB_TABLE_COUNT; i++) {
        if (!strcmp(name, g_prefab_data[i].name)) return i;
    }
    if (!strcmp(name, "SPECTATE_CAM")) return PREFAB_CAMERA; // v3.1.1 names
    if (!strcmp(name, "BOT")) return PREFAB_AI_SPAWNER;
    if (!strcmp(name, "TERMINAL")) return PREFAB_BOT_PATH_NODE; // v3.1.1 / v7.1.4 name of 41 (v10.0.3: BOT_PATH_NODE)
    return -1;
}

const char *prefab_name(const int id) {
    return id >= 0 && id < PREFAB_TABLE_COUNT ? g_prefab_data[id].name : NULL;
}

int prefab_texture_resolve(const int prefab, const int has_t, const int t) {
    const int fallback = prefab == PREFAB_LADDER || prefab == PREFAB_ROPE_LADDER ? 2 : 0;
    if (!has_t) return fallback;
    return t >= 0 && t < PREFAB_TEXTURE_COUNT ? t : fallback;
}

const char *prefab_texture_src(const int texture, const int tv, char *out, const size_t size) {
    if (texture < 0 || texture >= PREFAB_TEXTURE_COUNT || !out || !size) return NULL;
    const PrefabTextureData *t = &g_prefab_textures[texture];
    // map.js: if (tv && variants[tv]) src += "_" + variants[tv].id
    if (tv > 0 && tv < t->variant_count) snprintf(out, size, "%s_%d", t->src, t->variants[tv].id);
    else snprintf(out, size, "%s", t->src);
    return out;
}

int prefab_texture_id_from_name(const char *name) {
    if (!name) return -1;
    for (int i = 0; i < PREFAB_TEXTURE_COUNT; i++) {
        if (!strcmp(name, g_prefab_textures[i].name)) return i;
        if (g_prefab_textures[i].alias && !strcmp(name, g_prefab_textures[i].alias)) return i;
    }
    return -1;
}
