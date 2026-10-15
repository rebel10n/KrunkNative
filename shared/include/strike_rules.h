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

// A weapon's projectile tint (pj_color, 0xRRGGBB) where Strike's differs from v10's.
typedef struct {
    int id;
    int color;
} StrikeWeaponColor;

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

// A default key bind (CONTROLS.binds: new ControlBind(name, def, altDef)); keyCodes, 10000 + button
// for the mouse, -1 unbound. `index` is the array element of an array bind (move, streak, ...), -1 scalar.
typedef struct {
    const char *bind;
    int index;
    const char *name;
    int def, alt;
} StrikeBind;

// An input / view setting's default as Strike's settings window ships it (val, slider range, and the
// factor its set() stores: sensitivityXMbl 1.4 stores mblInput.sensX = 1.4 * 4).
typedef struct {
    const char *key;
    double val;
    double min, max;  // 0 / 0: not a slider
    double store_mlt; // 1 when set() stores the value as is
} StrikeSetting;

// The mobile aim assist and auto-fire (gameLoop; client/src/strike_assist.c plays it).
typedef struct {
    float box_scale_mlt, box_height_mlt; // base box half size: playerScale * m / dist * zoom, playerHeight * m / ...
    float grow_width, grow_height;       // the "on target" box: base x grow
    float pull_initial, pull_locked;     // look pull factor before / after the horizontal lock
    float center_threshold;              // |dist.x| below this (and no vertical pull) locks
    float locked_look_mlt;               // every look input while locked
    float locked_vertical_mlt;           // the locked vertical pull unless the target is airborne
    float deadband_up, deadband_down;    // no vertical pull while the centre is within playerHeight * x / dist
    float can_see_near_dist;             // canSee pad: near (< dist) / far
    float can_see_pad_near, can_see_pad_far;
    float aim_point_div;                 // aim at y + (playerHeight - crouchVal * crouchDst) / div
    int autofire_buffer_ms;              // auto-fire releases this long after the last on-target frame
} StrikeAimAssist;

// One control of FRVR's touch layout (640x360 base, scaled by the screen height). -1 = side unset.
typedef struct {
    const char *id;
    const char *type;   // "Button", "Toggle", "Joystick", "HintOnly"
    const char *icon, *icon_active, *action;
    int left, right, top, bottom;
    int size, width, height;
    unsigned char disabled; // in the table but never rendered (crouch, swap)
    unsigned char interact; // 0: drawn only (the fake move joystick)
} StrikeTouchControl;

typedef struct {
    int base_width, base_height;      // 640 x 360
    int move_joystick_size;           // the dynamic move joystick (left half)
    int tap_jump_max_travel;          // right-half touch moving at most this (px) is a jump tap
    int no_forward_back[4];           // |degree - 180| ranges with no forward/back: [lo0, hi0, lo1, hi1]
    int no_strafe[2];                 // ... with no strafe
    const StrikeTouchControl *controls;
    size_t control_count;
} StrikeTouch;

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

    // physics: the engine config keys GameConstants carries, Strike's values (Krunker JS units);
    // strike_constants_apply() converts. Strike is v7.1.4's config (crouchLean -PI*0.1 where v10 has 0).
    const StrikeStat *physics;
    size_t physics_count;
    const StrikeWeaponColor *projectile_colors; // client render table overrides (projectiles_render.c)
    size_t projectile_color_count;
    float delta_floor_ms;       // procInputs: max(floor, min(input[1], dltMx)) (v7: 0.1 ms; v10: 1 * deltaMlt)
    int trigger_action_limit;   // config.triggerActionLimit
    int end_timer_ms;           // config.endTimer (the end screen)
    int end_anim_ms;            // config.endAnim (the round-end slow motion)
    // combat: Strike keeps v7's padded AABB hit test with zones measured on the live target (no v10
    // posed hit boxes)
    unsigned char posed_hitboxes;
    unsigned char hit_zones_live;

    // input and look
    const StrikeBind *binds;    // the default binds (a Krunker bind not listed here is unbound)
    size_t bind_count;
    unsigned char scroll_to_swap; // the wheel swaps weapons (Strike: never)
    float pitch_limit;          // look pitch clamp +-
    int flick_clamp;            // mouseSkipFix flick clamp (px)
    const StrikeSetting *settings;
    size_t setting_count;
    StrikeAimAssist aim_assist;
    StrikeTouch touch;
} StrikeRules;

// The generated data (strike_data.c). Never NULL.
const StrikeRules *strike_rules(void);
extern const StrikeRules g_strike_rules; // FlavorConfig.rules of FLAVOR_STRIKE (shared/src/flavor.c)

// Lookups in `rules` (NULL: strike_rules()); NULL when Strike has no such entry.
const StrikeClass *strike_class(const StrikeRules *rules, int class_id);
const StrikeWeapon *strike_weapon(const StrikeRules *rules, int weapon_id);
const StrikeMode *strike_mode(const StrikeRules *rules, int mode_id);
const StrikeConfigDefault *strike_config_default(const StrikeRules *rules, const char *var);
const StrikeBind *strike_bind(const StrikeRules *rules, const char *bind, int index);
const StrikeSetting *strike_setting(const StrikeRules *rules, const char *key);
// Strike's projectile tint of weapon `id` (0xRRGGBB), or -1 when it is v10's.
int strike_projectile_color(const StrikeRules *rules, int id);

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
// Write one engine config key (JS units: ms, per-ms speeds) into GameConstants (s, per-s, as
// constants_v10.c). Returns 0 for a key GameConstants has no field for.
int strike_constants_apply(GameConstants *constants, const char *key, double value);
// Strike's GameConstants: v10's with every `physics` key applied (Ruleset.constants of strike_ruleset()).
const GameConstants *strike_constants(void);
