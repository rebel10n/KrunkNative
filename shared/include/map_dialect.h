#pragma once
// The per-ruleset map values (doc/v10/map-format.md): which prefab ids, trigger conditions and
// actions a map may use, the per-trigger action limit, the custom texture-id charset and the
// light-unit factor. One place reads the ruleset (shared/include/ruleset.h): everything else asks
// these accessors, so a later dialect (v3.1.1) only extends them.
//
// The game-less forms read ruleset_current() (the process ruleset: map parsing, validation, the
// renderer); map_dialect_of(game) prefers the ruleset a configured Game keeps (Game.rules).
#include <ruleset.h>
#include <prefabs.h>
#include <triggers.h>

static inline MapDialect map_dialect_current(void) { return ruleset_current()->map_dialect; }
static inline MapDialect map_dialect_of(const Game *game) {
    return game && game->rules ? game->rules->map_dialect : map_dialect_current();
}

// Prefab ids 0 .. count-1 exist in the dialect: v7 97 (0-96), v10 99 (+ SCRAPCRATE, ENTITY).
static inline int map_dialect_prefab_count(const MapDialect d) { return d >= MAP_DIALECT_V10 ? PREFAB_COUNT : PREFAB_V7_COUNT; }
// triggers.conditions / triggers.actions lengths: v7 12 / 72, v10 13 / 75.
static inline int map_dialect_condition_count(const MapDialect d) {
    return d >= MAP_DIALECT_V10 ? TRIGGER_V10_CONDITION_COUNT : TRIGGER_V7_CONDITION_COUNT;
}
static inline int map_dialect_action_count(const MapDialect d) { return d >= MAP_DIALECT_V10 ? TRIGGER_V10_ACTION_COUNT : TRIGGER_V7_ACTION_COUNT; }
// config.triggerActionLimit (tas per trigger): the ruleset's value (v7 5, v10 8).
static inline int map_dialect_trigger_action_limit(const Ruleset *rules) {
    return rules && rules->trigger_action_limit > 0 ? rules->trigger_action_limit : TRIGGER_V7_ACTION_LIMIT;
}
// The custom texture-id charset of skyDomeTexA / skyDomeEmisTex / mmId (besides digits):
// v7 /[^\dtlnesacdgxufq]+/ (MapManager.js:889), v10 adds m r p b h v (L164733, L159097).
static inline const char *map_dialect_texture_id_letters(const MapDialect d) {
    return d >= MAP_DIALECT_V10 ? "tlnesacdgxufqmrpbhv" : "tlnesacdgxufq";
}
// config.lightIntensityScaling: map light intensities x this (v7 1, v10 PI: THREE r186 lights).
static inline float map_dialect_light_unit_factor(void) {
    const float f = ruleset_current()->light_unit_factor;
    return f > 0.0f ? f : 1.0f;
}
// The prefab's name in the dialect: 41 is TERMINAL in v3 / v7 and BOT_PATH_NODE in v10 (both names
// resolve through prefab_id_from_name in every dialect).
static inline const char *map_dialect_prefab_name(const MapDialect d, const int id) {
    if (id == PREFAB_BOT_PATH_NODE && d >= MAP_DIALECT_V10) return "BOT_PATH_NODE";
    return id >= 0 && id < map_dialect_prefab_count(d) ? prefab_name(id) : NULL;
}
