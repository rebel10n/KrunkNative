#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ruleset.h>
#include <flavor.h>
#include <map_catalog.h>
#include <strike_rules.h>
#include <triggers.h>

// The ruleset (shared/include/ruleset.h): Krunker v10.0.3, the only version it plays (doc/v10/target.md). Its tables are in
// rules/v10/ (doc/v10/*.md). TODO(v10-maps): the map catalogue is v7.1.4's 32 official maps.
extern const Weapon *const g_weapons_v10[WEAPON_TABLE_COUNT];
extern const ClassConfig g_classes_v10[CLASS_TABLE_COUNT];

static const Ruleset k_v10 = {
    .key = "v10",
    .version = "10.0.3",
    .constants = &game_constants,
    .posed_hitboxes = 1, // v10.0.2: every mode but Prop Hunt traces the posed boxes (hitboxes.c)
    .hit_zones_live = 0, // KrunkNative measures the zones on the rewound box (player.c combat)
    .delta_floor_scaled = 1, // procInputs max(1 * deltaMlt, ...) (v10:169582)
    .end_timer = GAME_END_TIMER, // config.endTimer 25000
    .end_anim = 5.0f,            // config.endAnim 5000
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

const Ruleset *ruleset_v10(void) {
    return &k_v10;
}

// Krunker Strike (flavor.h, strike_rules.h) plays v10's ruleset with FRVR's weapon / class / mode
// tables; FLAVOR_BASE (rules NULL) is v10 itself.
const Ruleset *ruleset_current(void) {
    return flavor_current()->rules ? strike_ruleset() : &k_v10;
}

const Weapon *const *rules_weapons(void) { return ruleset_current()->weapons; }
size_t rules_weapon_count(void) { return ruleset_current()->weapon_count; }
int rules_krunker_weapon_count(void) { return ruleset_current()->krunker_weapon_count; }
const ClassConfig *rules_classes(void) { return ruleset_current()->classes; }
size_t rules_class_count(void) { return ruleset_current()->class_count; }

const Weapon *rules_weapon(const int id) {
    const Ruleset *rules = ruleset_current();
    return id >= 0 && (size_t) id < rules->weapon_count ? rules->weapons[id] : NULL;
}

const Ruleset *game_ruleset(const Game *game) { return game && game->rules ? game->rules : ruleset_current(); }
int game_is_ranked(const Game *game) { return game && game->is_ranked; }
const GameConstants *game_constants_of(const Game *game) {
    const GameConstants *k = game_ruleset(game)->constants;
    return k ? k : &game_constants;
}

float rules_reward_mlt(const Game *game) {
    const float mlt = game_ruleset(game)->ranked.kr_mlt;
    return game_is_ranked(game) && mlt > 0.0f ? mlt : 1.0f;
}

int rules_ranked_no_streaks(const Game *game) { return game_is_ranked(game) && game_ruleset(game)->ranked.no_streaks; }
int rules_ranked_no_class_xp(const Game *game) { return game_is_ranked(game) && game_ruleset(game)->ranked.no_class_xp; }
