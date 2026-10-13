#pragma once
// Krunker Box Office rulesets (doc/v10/version-toggle.md "Recommended architecture"): one binary
// plays Krunker v7.1.4 or v10.0.2 (v3.1.1 later), one ruleset per process. A server process runs
// the ruleset it was started with; the client adopts the server's on join, and the offline sandbox
// (client/src/net/offline.c, in-process) runs the client's. Everything that differs per Krunker
// version hangs off a Ruleset: physics constants, the weapon / class / mode tables, the map
// catalogue and dialect, trigger limits, the KrunkScript feature set, light units and the UI page.
//
// Tables live in shared/src/rules/<ver>/ (the weapon and class definitions themselves stay in
// shared/src/weapons/ and shared/src/classes.c; a ruleset's table points at them, or at its own
// copies where a version changes a value).
#include <stddef.h>
#include <stdint.h>
#include <shared.h> // GameConstants, ClassConfig, Weapon (weapons.h), GameModeConfig (modes.h), cJSON

typedef enum {
    RULESET_V3_1_1 = 0, // TODO(v3 ruleset): not built yet; ruleset_get() returns NULL (doc/v10/version-toggle.md)
    RULESET_V7_1_4 = 1,
    RULESET_V10_0_2 = 2,
    RULESET_COUNT
} RulesetId;

#define RULESET_DEFAULT RULESET_V7_1_4

// How map JSON fields are read (doc/v10/map-format.md). Today map.c reads every map the v7 way.
typedef enum {
    MAP_DIALECT_V3 = 0,
    MAP_DIALECT_V7 = 1,
    MAP_DIALECT_V10 = 2,
} MapDialect;

// KrunkScript API features a ruleset exposes (ks_feature_mask).
#define RULES_KS_V7 0x1u          // the v7.1.4 API
#define RULES_KS_ENTITIES 0x2u    // v10 entities / archetypes (entityLimit, e+ e- es ...)
#define RULES_KS_PLAYER_POSE 0x4u // v10 player pose (pz) and locks (plk)

typedef struct Ruleset_t { // shared.h forward-declares the tag for Game.rules
    RulesetId id;
    const char *key;     // "v7", "v10": server --ruleset, server.cfg / config `ruleset`, ui/<key>/
    const char *version; // "7.1.4": what menus show

    const GameConstants *constants;

    // Weapon ids index this table: 0 .. krunker_weapon_count-1 are Krunker's own (wire ids),
    // WEAPON_ID_KN_BASE.. KrunkNative's; the ids in between are NULL.
    const Weapon *const *weapons;
    size_t weapon_count;         // table length (WEAPON_TABLE_COUNT)
    int krunker_weapon_count;    // Krunker's weaponConfig length (v7 29): menus, skins, host config

    const ClassConfig *classes;
    size_t class_count;

    const GameModeConfig *modes; // indexed by mode id
    size_t mode_count;

    const cJSON *const *maps; // the official map catalogue (mapIndex)
    size_t map_count;
    const int *rotation_maps;
    size_t rotation_map_count;
    MapDialect map_dialect;

    int trigger_action_limit;   // config.triggerActionLimit (v7 5, v10 8)
    int entity_limit;           // config.entityLimit (v10 64; 0 = no entities)
    int archetype_limit;        // config.archetypeLimit (v10 32)
    uint32_t ks_feature_mask;   // RULES_KS_*
    float light_unit_factor;    // config.lightIntensityScaling: map light intensity x (v7 1, v10 PI)
    const char *ui_page_dir;    // page tree under ui/ ("" = ui/ itself; v10 "v10" once it exists)
} Ruleset;

// The ruleset of `id`; NULL for one not built (v3.1.1) or out of range.
const Ruleset *ruleset_get(RulesetId id);
// "v7" / "7.1.4" / "v7.1.4" / "7" (and v10 likewise) -> the ruleset; NULL unknown or not built.
const Ruleset *ruleset_find(const char *name);
// This process's ruleset (RULESET_DEFAULT until ruleset_set_current). Never NULL.
const Ruleset *ruleset_current(void);
// Switch the process ruleset (game_constants follows). 0 when `id` is not built. Only switch
// between games: a running Game keeps the tables it was configured with (Game.rules).
int ruleset_set_current(RulesetId id);

// Short forms for code without a Game in scope (menus, viewer, catalogues).
const Weapon *const *rules_weapons(void);
size_t rules_weapon_count(void);
int rules_krunker_weapon_count(void);
const Weapon *rules_weapon(int id); // bounds-checked, NULL for holes / out of range
const ClassConfig *rules_classes(void);
size_t rules_class_count(void);
