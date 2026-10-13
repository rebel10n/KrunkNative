#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>
#include <map_catalog.h>
#include <triggers.h>

// The ruleset tables (shared/include/ruleset.h). The per-version data is in rules/v7/ and rules/v10/.
extern const GameConstants g_constants_v7, g_constants_v10;
extern const Weapon *const g_weapons_v7[WEAPON_TABLE_COUNT], *const g_weapons_v10[WEAPON_TABLE_COUNT];
extern const ClassConfig g_classes_v7[CLASS_COUNT], g_classes_v10[CLASS_COUNT];

static const Ruleset k_v7 = {
    .id = RULESET_V7_1_4,
    .key = "v7",
    .version = "7.1.4",
    .constants = &g_constants_v7,
    .weapons = g_weapons_v7,
    .weapon_count = WEAPON_TABLE_COUNT,
    .krunker_weapon_count = WEAPON_COUNT,
    .classes = g_classes_v7,
    .class_count = CLASS_COUNT,
    .modes = g_mode_configs,
    .mode_count = MODE_COUNT,
    .maps = g_maps,
    .map_count = MAP_CATALOG_OFFICIAL_COUNT,
    .rotation_maps = g_rotation_maps,
    .rotation_map_count = sizeof(g_rotation_maps) / sizeof(g_rotation_maps[0]),
    .map_dialect = MAP_DIALECT_V7,
    .trigger_action_limit = TRIGGER_V7_ACTION_LIMIT,
    .entity_limit = 0,
    .archetype_limit = 0,
    .ks_feature_mask = RULES_KS_V7,
    .light_unit_factor = 1.0f,
    .ui_page_dir = "",
};

// v10.0.2 (doc/v10/*.md). TODO(v10-gameplay / v10-maps / live-v10): the tables are v7's copies
// until v10's data lands; modes and the map catalogue are v7's own for now, the page is ui/ until
// ui/v10/ exists (the client falls back when ui/<dir>/index.html is missing).
static const Ruleset k_v10 = {
    .id = RULESET_V10_0_2,
    .key = "v10",
    .version = "10.0.2",
    .constants = &g_constants_v10,
    .weapons = g_weapons_v10,
    .weapon_count = WEAPON_TABLE_COUNT,
    .krunker_weapon_count = WEAPON_COUNT, // TODO(v10-gameplay): 31 once Boulder and Compressor exist
    .classes = g_classes_v10,
    .class_count = CLASS_COUNT,
    .modes = g_mode_configs,
    .mode_count = MODE_COUNT,
    .maps = g_maps,
    .map_count = MAP_CATALOG_OFFICIAL_COUNT,
    .rotation_maps = g_rotation_maps,
    .rotation_map_count = sizeof(g_rotation_maps) / sizeof(g_rotation_maps[0]),
    .map_dialect = MAP_DIALECT_V10,
    .trigger_action_limit = 8,    // config.triggerActionLimit 5 -> 8 (overview.md "Config diff")
    .entity_limit = 64,           // config.entityLimit
    .archetype_limit = 32,        // config.archetypeLimit
    .ks_feature_mask = RULES_KS_V7 | RULES_KS_ENTITIES | RULES_KS_PLAYER_POSE,
    .light_unit_factor = (float) M_PI, // config.lightIntensityScaling
    .ui_page_dir = "v10",
};

// TODO(v3 ruleset): RULESET_V3_1_1 (tables from branch v3.1.1, MAP_DIALECT_V3, ui page from the branch).
static const Ruleset *const k_rulesets[RULESET_COUNT] = {
    [RULESET_V3_1_1] = NULL,
    [RULESET_V7_1_4] = &k_v7,
    [RULESET_V10_0_2] = &k_v10,
};

static const Ruleset *g_current = &k_v7;
const GameConstants *g_rules_constants = &g_constants_v7;

const Ruleset *ruleset_get(const RulesetId id) {
    return (int) id >= 0 && id < RULESET_COUNT ? k_rulesets[id] : NULL;
}

const Ruleset *ruleset_find(const char *name) {
    if (!name || !name[0]) return NULL;
    if (name[0] == 'v' || name[0] == 'V') name++;
    for (int i = 0; i < RULESET_COUNT; i++) {
        const Ruleset *r = k_rulesets[i];
        if (r && (!strcmp(name, r->key + 1) || !strcmp(name, r->version))) return r; // "7" / "7.1.4"
    }
    return NULL;
}

const Ruleset *ruleset_current(void) {
    return g_current;
}

int ruleset_set_current(const RulesetId id) {
    const Ruleset *r = ruleset_get(id);
    if (!r) return 0;
    g_current = r;
    g_rules_constants = r->constants;
    return 1;
}

const Weapon *const *rules_weapons(void) { return g_current->weapons; }
size_t rules_weapon_count(void) { return g_current->weapon_count; }
int rules_krunker_weapon_count(void) { return g_current->krunker_weapon_count; }
const ClassConfig *rules_classes(void) { return g_current->classes; }
size_t rules_class_count(void) { return g_current->class_count; }

const Weapon *rules_weapon(const int id) {
    return id >= 0 && (size_t) id < g_current->weapon_count ? g_current->weapons[id] : NULL;
}
