#pragma once
// Krunker Strike's rules: FRVR's Krunker Strike (build 1.37.2) as data, so the Strike flavor plays
// by FRVR's tables rather than invented deltas (doc/v10/krunker-strike.md). Strike is FRVR's fork
// of the Krunker engine; its class, weapon and mode tables are Krunker's with FRVR's values. The
// data is tools/strike/strike_data.json (tools/strike/extract_strike.py, text-only extraction of
// the client bundle); tools/strike/gen_rules.py turns it into shared/src/rules/strike/strike_data.c.
// Regenerate, never hand-edit, when a new Strike build is extracted.
//
// Two layers:
//   - strike_rules(): the generated data (pure tables, Krunker JS units).
//   - strike_ruleset() / strike_game_config(): the engine tables built from it once, on first use
//     (shared/src/rules/strike/strike_rules.c): v10.0.2's weapon / class / mode tables with Strike's
//     values applied. Under FLAVOR_STRIKE ruleset_current() returns strike_ruleset(), so Game.rules,
//     game_configure(), rules_weapon() and rules_classes() all follow (shared/include/flavor_rules.h
//     has the gameplay gates). FLAVOR_BASE never touches any of this.
//
// Pure C; part of the shared core (no GL / net).
#include <stddef.h>
#include <shared.h> // Weapon, ClassConfig, GameModeConfig, GameConfig

// One weaponConfig key and its value, in Krunker's JS units (times in ms, recoil per ms, booleans 0/1).
typedef struct {
    const char *key;
    double value;
} StrikeStat;

// A Strike class: classConfig as Strike ships it, classConfig defaults applied (the effective row).
typedef struct {
    int id;                  // Krunker class id (classConfig index; Strike keeps Krunker's numbering)
    const char *name;        // Strike's name ("Shotgunner" is Krunker's Vince)
    int loadout;             // primary weapon id (Strike classes carry no secondary)
    int health;
    int segments;            // segs: health bar segments
    float regen;
    float speed;
    unsigned char secondary; // carries a secondary weapon (false for every Strike class)
    unsigned char wall_jump; // wallJ
    int wall_jump_max;       // wallJM
    unsigned char leap;
    unsigned char back_slide; // bkSlid
    float melee_rate, slide_time, slide_accel, turn_speed; // melRate, slT, slA, trnSpd
    int throw_charge_ms;     // throwChrg
    // movement (classConfig defaults; Strike's grA default is 0.048 where Krunker's is 0.042)
    float ground_accel, air_accel, ground_decel, air_decel, crouch_speed; // grA, arA, grDec, airDec, crchSpd
    int rarity;              // 0 common .. 3 (store presentation)
    int min_rec;             // minRec: account level gate, 0 = open
    const char *unlockable;  // how it unlocks: "bundle", "rewards_track", "spin_tickets"; NULL = starter
    int colors[6];
    const char *const *texts; // txts: select-class lines
    int text_count;
    const char *description;
} StrikeClass;

// A Strike weapon (id = weaponConfig index, the same ids as Krunker's 0-28): the keys where it
// differs from Krunker v10.0.2's entry. A key Strike's entry leaves out carries the engine default.
typedef struct {
    int id;
    const char *name;
    const StrikeStat *overrides;
    size_t override_count;
} StrikeWeapon;

// A Strike game mode (Strike's gameModes table: v7's without Trade and Impulsed).
typedef struct {
    int index;               // Strike's mode index (its own numbering)
    int mode_id;             // KrunkNative's mode id (modes.h MODE_ID_*)
    const char *alias;
    const char *name;
    unsigned char teams, party;
    unsigned char offered;   // in Strike's menu (it carries a description there): the public rotation
    unsigned char prevent_custom;
    int score_limit;         // scoreLimit, 0 none (Strike: FFA 1500, TDM 2500)
    int game_time_ms;        // gameTime, -1 = the lobby's config.gameTime
    int lives;
    const int *weapon_order; // Gun Game weaponOrder, NULL none
    size_t weapon_order_count;
} StrikeMode;

// A serverConfig default (custom-game / lobby settings), Krunker units.
typedef struct {
    const char *var;
    double def;
} StrikeConfigDefault;

typedef struct StrikeRules_t {
    const char *game_version;   // "1.37.2"
    const char *build_version;  // the bundle's build id

    const StrikeClass *classes; // the six Strike classes, Krunker id order
    size_t class_count;
    int default_class;          // the starter class (Triggerman, 0): any other pick falls back to it

    const StrikeWeapon *weapons; // Strike's weaponConfig: ids 0-28 (no Boulder / Compressor)
    size_t weapon_count;

    const StrikeMode *modes;
    size_t mode_count;
    const int *rotation_modes;  // mode ids of the offered modes, Strike's order
    size_t rotation_mode_count;

    const StrikeConfigDefault *config_defaults;
    size_t config_default_count;

    // lobby limits and timers (Strike's server constants)
    int max_players;            // public lobby cap: constants.maxPlayers (Number(MAX_PLAYERS) || 10)
    int private_capacity;       // private match capacity the client asks the director for (12)
    int max_bots;
    int max_players_total;      // per server process: gamesPerServer x maxPlayers + 40
    int games_per_server;
    float server_tick_ms;       // 1e3 / serverTickFrequency
    int server_send_frequency;
    int game_start_delay_ms;
    int kick_timer_ms;
    int game_time_min;          // match minutes (serverConfig gameTime default)
    int intermission_s;         // intermTmr default
} StrikeRules;

// The generated data (strike_data.c). Never NULL.
const StrikeRules *strike_rules(void);
extern const StrikeRules g_strike_rules; // FlavorConfig.rules of FLAVOR_STRIKE (shared/src/flavor.c)

// Lookups in `rules` (NULL: strike_rules()); NULL when Strike has no such entry.
const StrikeClass *strike_class(const StrikeRules *rules, int class_id);
const StrikeWeapon *strike_weapon(const StrikeRules *rules, int weapon_id);
const StrikeMode *strike_mode(const StrikeRules *rules, int mode_id);
const StrikeConfigDefault *strike_config_default(const StrikeRules *rules, const char *var);

// ---- the engine tables built from the data (strike_rules.c; built once, then read-only) ---------
// Krunker v10.0.2's ruleset (ruleset.h) with Strike's weapon, class and mode tables: the same key /
// version (the wire is v10's), krunker_weapon_count 29 (Boulder / Compressor out), the classes
// Strike lacks hidden. Thread-safe first build. Never NULL.
const struct Ruleset_t *strike_ruleset(void);
// The default lobby config: g_default_game_config with every serverConfig default where Strike's
// differs from Krunker's (match minutes, strafe speed, streaks, team select) and Strike's lobby cap.
const GameConfig *strike_game_config(void);
// Write one weaponConfig key (JS units) into a Weapon. Returns 0 for a key the engine has no field for.
int strike_weapon_apply(Weapon *weapon, const char *key, double value);
