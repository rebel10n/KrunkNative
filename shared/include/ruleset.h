#pragma once
// Krunker Box Office plays Krunker v10.0.2, the only version (user, 2026-10-13; doc/v7/decisions.md
// "Krunker v10.0.2 is the only version"). There is no version choice: one Ruleset, the v10.0.2
// tables in shared/src/rules/v10/, bundles what Krunker's version defines: physics constants, the
// weapon / class / mode tables, the map catalogue, trigger and entity limits, light units and the
// ranked rules. A Game keeps a pointer to it (Game.rules); code without a Game reads
// ruleset_current().
//
// The weapon definitions themselves stay in shared/src/weapons/ (v10's ten rebalanced entries are
// copies in rules/v10/weapons_rebalance_v10.c) and the mode table is shared/src/modes/modes.c.
#include <stddef.h>
#include <stdint.h>
#include <shared.h> // GameConstants, ClassConfig, Weapon (weapons.h), GameModeConfig (modes.h), cJSON

typedef struct Ruleset_t { // shared.h forward-declares the tag for Game.rules
    const char *key;     // "v10": the brain lobby meta / hello, the server-info query
    const char *version; // "10.0.2": what menus and logs show

    const GameConstants *constants;

    // Weapon ids index this table: 0 .. krunker_weapon_count-1 are Krunker's own (wire ids),
    // WEAPON_ID_KN_BASE.. KrunkNative's; the ids in between are NULL.
    const Weapon *const *weapons;
    size_t weapon_count;         // table length (WEAPON_TABLE_COUNT)
    int krunker_weapon_count;    // Krunker's weaponConfig length (31): menus, skins, host config

    const ClassConfig *classes;
    size_t class_count;

    const GameModeConfig *modes; // indexed by mode id
    size_t mode_count;

    const cJSON *const *maps; // the official map catalogue (mapIndex)
    size_t map_count;
    const int *rotation_maps;
    size_t rotation_map_count;

    // Ranked match rules (doc/v10/gameplay.md section 7, ranked.md): applied while Game.is_ranked
    // (server/src/accounts.c acc_ranked_setup).
    struct {
        float kr_mlt;               // endGame reward x (v10:191171 `isRanked ? 2 : 1`)
        unsigned char no_streaks;   // no kill streak count (v10:174076 `|| isRanked`)
        unsigned char no_class_xp;  // saveClassScores skipped (v10:173631 `isPubType() && !isRanked`)
        int need_all_ms;            // needAllTimer when ranked (v10:189820 60000; 0 = the server's own)
        int shutdown_ms;            // shutdown after the end (v10:191217 120000; 0 = the end timer)
    } ranked;

    int trigger_action_limit;   // config.triggerActionLimit (8)
    int entity_limit;           // config.entityLimit (64)
    int archetype_limit;        // config.archetypeLimit (32)
    float light_unit_factor;    // config.lightIntensityScaling: map light intensity x PI
} Ruleset;

// The ruleset: Krunker v10.0.2. Never NULL.
const Ruleset *ruleset_current(void);

// Short forms for code without a Game in scope (menus, viewer, catalogues).
const Weapon *const *rules_weapons(void);
size_t rules_weapon_count(void);
int rules_krunker_weapon_count(void);
const Weapon *rules_weapon(int id); // bounds-checked, NULL for holes / out of range
const ClassConfig *rules_classes(void);
size_t rules_class_count(void);

// The ruleset a game was configured with (Game.rules), else ruleset_current(). Never NULL.
const Ruleset *game_ruleset(const struct Game_t *game);
// 1 in a ranked match (Game.is_ranked, set by the server's ranked lobby setup).
int game_is_ranked(const struct Game_t *game);
// The end-game KR multiplier of `game`: ranked.kr_mlt in a ranked match, else 1.
float rules_reward_mlt(const struct Game_t *game);
// The ranked kill-streak rule: 1 when `game` is ranked (no kill streaks there).
int rules_ranked_no_streaks(const struct Game_t *game);
// The ranked class-XP rule: 1 when `game` is ranked (no class score saved there).
int rules_ranked_no_class_xp(const struct Game_t *game);
