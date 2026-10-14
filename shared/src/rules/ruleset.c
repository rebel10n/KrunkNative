#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>
#include <map_catalog.h>
#include <triggers.h>

// The ruleset (shared/include/ruleset.h): Krunker v10.0.2, the only version. Its tables are in
// rules/v10/ (doc/v10/*.md). TODO(v10-maps): the map catalogue is v7.1.4's 32 official maps.
extern const Weapon *const g_weapons_v10[WEAPON_TABLE_COUNT];
extern const ClassConfig g_classes_v10[CLASS_TABLE_COUNT];

static const Ruleset k_v10 = {
    .key = "v10",
    .version = "10.0.2",
    .constants = &game_constants,
    .weapons = g_weapons_v10,
    .weapon_count = WEAPON_TABLE_COUNT,
    .krunker_weapon_count = WEAPON_V10_COUNT, // 29 Boulder, 30 Compressor (weapons_v10.c)
    .classes = g_classes_v10,
    .class_count = CLASS_TABLE_COUNT, // Krunker's 16 + the KrunkNative Nettspend (classes_v10.c)
    .modes = g_mode_configs,
    .mode_count = MODE_COUNT,
    .maps = g_maps,
    .map_count = MAP_CATALOG_OFFICIAL_COUNT,
    .rotation_maps = g_rotation_maps,
    .rotation_map_count = sizeof(g_rotation_maps) / sizeof(g_rotation_maps[0]),
    // ranked (doc/v10/gameplay.md section 7): x2 KR (v10:191171), no kill streaks (v10:174076), no class
    // XP (v10:173631), needAllTimer 60000 (v10:189820), shutdownTimer 120000 (v10:191217)
    .ranked = {.kr_mlt = 2.0f, .no_streaks = 1, .no_class_xp = 1, .need_all_ms = 60000, .shutdown_ms = 120000},
    .trigger_action_limit = TRIGGER_V10_ACTION_LIMIT, // config.triggerActionLimit 8 (v7.1.4: 5)
    .entity_limit = 64,           // config.entityLimit
    .archetype_limit = 32,        // config.archetypeLimit
    .light_unit_factor = (float) M_PI, // config.lightIntensityScaling
};

const Ruleset *ruleset_current(void) {
    return &k_v10;
}

const Weapon *const *rules_weapons(void) { return k_v10.weapons; }
size_t rules_weapon_count(void) { return k_v10.weapon_count; }
int rules_krunker_weapon_count(void) { return k_v10.krunker_weapon_count; }
const ClassConfig *rules_classes(void) { return k_v10.classes; }
size_t rules_class_count(void) { return k_v10.class_count; }

const Weapon *rules_weapon(const int id) {
    return id >= 0 && (size_t) id < k_v10.weapon_count ? k_v10.weapons[id] : NULL;
}

const Ruleset *game_ruleset(const Game *game) { return game && game->rules ? game->rules : &k_v10; }
int game_is_ranked(const Game *game) { return game && game->is_ranked; }

float rules_reward_mlt(const Game *game) {
    const float mlt = game_ruleset(game)->ranked.kr_mlt;
    return game_is_ranked(game) && mlt > 0.0f ? mlt : 1.0f;
}

int rules_ranked_no_streaks(const Game *game) { return game_is_ranked(game) && game_ruleset(game)->ranked.no_streaks; }
int rules_ranked_no_class_xp(const Game *game) { return game_is_ranked(game) && game_ruleset(game)->ranked.no_class_xp; }
