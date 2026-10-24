#pragma once
// Krunker Box Office plays Krunker v10.0.3, the only version it plays (doc/v10/target.md; v10.0.2 is
// the prior version, kept as a reference). There is no version choice: one Ruleset, the v10
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
    const char *version; // "10.0.3": what menus and logs show

    const GameConstants *constants;
    // Hit test: v10.0.2 traces the posed hit boxes (shared/src/hitboxes.c); 0 keeps v7.1.4's padded
    // AABB (players.js lineInRect), as Krunker Strike does. hit_zones_live: head / leg zones measured
    // on the target's live position (v7 / Strike) rather than the lag-compensated box (KrunkNative's fix).
    unsigned char posed_hitboxes;
    unsigned char hit_zones_live;
    // procInputs' delta floor is constants->min_delta x config.deltaMlt when set (v10.0.3: Math.max(1 * deltaMlt,
    // Math.min(input[1], dltMx)) ms, and a delta under the floor becomes the floor, deob-10.0.3.js:191276-191280),
    // else the bare constant (v7.1.4 / Strike: 0.1 ms)
    unsigned char delta_floor_scaled;
    float end_timer;            // config.endTimer: the end screen / map vote, seconds (v10 25, deob-10.0.3.js:22540; Krunker Strike 30)
    float end_anim;             // config.endAnim: the round-end slow motion, seconds (v10 5, deob-10.0.3.js:19427, 22537; Krunker Strike 2.5)
    // a projectile's head band is plain headScale (v7 / Krunker Strike projectiles), not headScale x headMlt
    unsigned char projectile_head_plain;

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
        float kr_mlt;               // endGame reward x (deob-10.0.3.js:221760-221761 `else if (isRanked) reward * 2`)
        unsigned char no_streaks;   // no kill streak count (deob-10.0.3.js:198604 `&& !isRanked`)
        unsigned char no_class_xp;  // saveClassScores skipped (deob-10.0.3.js:197794 `isPubType() && !isRanked`)
        int need_all_ms;            // needAllTimer when ranked (deob-10.0.3.js:219964 60000; 0 = the server's own)
        int shutdown_ms;            // shutdown after the end (deob-10.0.3.js:221814-221815 120000; 0 = the end timer)
    } ranked;

    int trigger_action_limit;   // config.triggerActionLimit (8, deob-10.0.3.js:22957)
    int entity_limit;           // config.entityLimit (64, deob-10.0.3.js:22543; validateMap "Too many entities." :216658)
    int archetype_limit;        // config.archetypeLimit (32, deob-10.0.3.js:22397; validateMap :216534)
    float light_unit_factor;    // config.lightIntensityScaling: map light intensity x PI (deob-10.0.3.js:21272, 22682)
    // getPointSpread's spread for a comp spawn (warmup, the intermission; game.js getSpawnPoint useCompSpawns):
    // the point is the spawn itself, rounded to whole units (spread 10 less 10 = 0). v10.0.3: 10 (deob line
    // 217148); v7.1.4: 40 (players up to 30 units off)
    float comp_spawn_spread;
} Ruleset;

// The ruleset: Krunker v10.0.3. Never NULL. Under a flavor with rules (Krunker Strike, flavor.h)
// it is v10's with that flavor's weapon / class / mode tables (strike_rules.h strike_ruleset()).
const Ruleset *ruleset_current(void);
// Krunker v10.0.2's own tables, whatever the flavor (what a flavor's tables are built from).
const Ruleset *ruleset_v10(void);

// Short forms for code without a Game in scope (menus, viewer, catalogues).
const Weapon *const *rules_weapons(void);
size_t rules_weapon_count(void);
int rules_krunker_weapon_count(void);
const Weapon *rules_weapon(int id); // bounds-checked, NULL for holes / out of range
const ClassConfig *rules_classes(void);
size_t rules_class_count(void);

// The ruleset a game was configured with (Game.rules), else ruleset_current(). Never NULL.
const Ruleset *game_ruleset(const struct Game_t *game);
// Its physics constants (Ruleset.constants). The global game_constants is v10's: code reading a
// constant a flavor changes (crouch_lean, min_delta under Krunker Strike) goes through this.
const GameConstants *game_constants_of(const struct Game_t *game);
// 1 in a ranked match (Game.is_ranked, set by the server's ranked lobby setup).
int game_is_ranked(const struct Game_t *game);
// The end-game KR multiplier of `game`: ranked.kr_mlt in a ranked match, else 1.
float rules_reward_mlt(const struct Game_t *game);
// The ranked kill-streak rule: 1 when `game` is ranked (no kill streaks there).
int rules_ranked_no_streaks(const struct Game_t *game);
// The ranked class-XP rule: 1 when `game` is ranked (no class score saved there).
int rules_ranked_no_class_xp(const struct Game_t *game);
