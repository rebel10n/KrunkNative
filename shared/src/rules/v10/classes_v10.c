#include <shared.h>

// Krunker v10.0.2 classes (doc/v10/gameplay.md section 4; v10.js classConfig v10:101961-102300):
// the v7.1.4 table (src/data/classes.js) with v10's changes, each marked "v10:N" (line N
// of the renamed 10.0.2 bundle ~/.cache/kn-v10/v10.js at the class entry):
//   Run N Gun health 90 -> 100, Spray N Pray 180 -> 170, Agent 110 -> 105;
//   minRec 1 + req (_34270, the v7 f791 gate) on every class but Triggerman (v7: only Hunter,
//   Vince, Detective and Bowman). Level 1 is every account's first level, so it gates guests.
// Order, loadouts, speeds and wall-jump values are v7's (extractor diff ~/.cache/kn-v10/ext/
// diff-classes.txt; checked against shared/tests/data/v10/classes.json by weapons_v10_test.c).
// The Stalker rework is a mode change (shared/src/modes/mode_v10.c), not a class.

// From the v7.1.4 classes (src/data/classes.js), generated from shared/tests/data/v7/classes.json
// (tools/kref/v7-dump.js). loadout ids index the ruleset's weapon table. Times in s (throwChrg ms -> s).
// Every class-editor knob v7 sets from config.classConfig that equals the engine constant
// (camera, movement scheme, vehicle, grA/arA/grDec/airDec, ...) is left out: 0 means the default.
// The Infiltrator's 7th colour (0x999999) is dropped: playerMesh reads colours 0-5 only.

static const ClassConfig triggerman = {
    .name = "Triggerman",
    .icon_index = 0,
    .texts = NULL,
    .loadout = (int[]) {1},
    .secondary = 1,
    .wall_jump = 0,
    .colors = {0xA77860, 0x3D3D3D, 0x232323, 0x282828, 0x6C5042, 0xBFBFBF},
    .health = 100,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.05f,
    .throw_charge = 0.8f,
    .face = 4544,
};

static const ClassConfig hunter = {
    .name = "Hunter",
    .icon_index = 1,
    .texts = NULL,
    .loadout = (int[]) {0},
    .secondary = 1,
    .wall_jump = 0,
    .colors = {0xA77860, 0x7B573D, 0x634732, 0x282828, 0x634732, 0x3D2B1D},
    .health = 60,
    .health_segments = 5,
    .regen = 0.1f,
    .speed = 1.0f,
    .throw_charge = 0.8f,
    .min_rec = 1,
    .face = 4545,
};

static const ClassConfig run_n_gun = {
    .name = "Run N Gun",
    .icon_index = 2,
    .texts = NULL,
    .loadout = (int[]) {3},
    .secondary = 0,
    .wall_jump = 1,
    .wall_jump_max = 2,
    .colors = {0xA77860, 0x3E6382, 0x2F4B63, 0x282828, 0x634732, 0x1A2B3A},
    .health = 100, // v10:102001 health 90 -> 100
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.18f,
    .min_rec = 1, // v10:102001 minRec 1 (req _34270)
    .throw_charge = 0.8f,
    .face = 4546,
};

static const ClassConfig spray_n_pray = {
    .name = "Spray N Pray",
    .icon_index = 3,
    .texts = (const char*[]) {"Calling in the Big Guns?", "Remember - No Russian.", "Pesky Snipers..."},
    .text_count = 3,
    .loadout = (int[]) {6},
    .secondary = 0,
    .wall_jump = 0,
    .colors = {0xA77860, 0x586849, 0x49563C, 0x282828, 0x282828, 0x303827},
    .health = 170, // v10:102022 health 180 -> 170
    .health_segments = 7,
    .regen = 0.05f,
    .speed = 0.9f,
    .min_rec = 1, // v10:102022 minRec 1 (req _34270)
    .throw_charge = 0.8f,
    .slide_accel_mlt = 0.98f,
    .face = 4547,
};

static const ClassConfig vince = {
    .name = "Vince",
    .icon_index = 4,
    .texts = (const char*[]) {"...", "Don't ever call me Shotgunner", "Skill issue"},
    .text_count = 3,
    .loadout = (int[]) {5},
    .secondary = 1,
    .wall_jump = 0,
    .colors = {0x805C4A, 0x545267, 0x3F3D4D, 0x282828, 0x282828, 0x292833},
    .health = 90,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.0f,
    .throw_charge = 0.8f,
    .min_rec = 1,
    .face = 4548,
};

static const ClassConfig detective = {
    .name = "Detective",
    .icon_index = 5,
    .texts = (const char*[]) {"I'm onto something"},
    .text_count = 1,
    .loadout = (int[]) {4},
    .secondary = 0,
    .wall_jump = 0,
    .colors = {0xA77860, 0x704E36, 0x434C5E, 0x282828, 0x634732, 0x3F2C1E},
    .health = 100,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.0f,
    .throw_charge = 0.8f,
    .min_rec = 1,
    .face = 4549,
};

static const ClassConfig marksman = {
    .name = "Marksman",
    .icon_index = 6,
    .texts = NULL,
    .loadout = (int[]) {7},
    .secondary = 1,
    .wall_jump = 0,
    .colors = {0xA77860, 0x46472D, 0x2A2A1B, 0x1C1C1C, 0x282828, 0x2A2A1B},
    .health = 90,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.0f,
    .min_rec = 1, // v10:102088 minRec 1 (req _34270)
    .throw_charge = 0.8f,
    .face = 4550,
};

static const ClassConfig rocketeer = {
    .name = "Rocketeer",
    .icon_index = 7,
    .texts = (const char*[]) {"..."},
    .text_count = 1,
    .loadout = (int[]) {8},
    .secondary = 1,
    .wall_jump = 0,
    .colors = {0xA77860, 0x586849, 0x49563C, 0x282828, 0x6C5042, 0x2B3324},
    .health = 130,
    .health_segments = 7,
    .regen = 0.1f,
    .speed = 0.86f,
    .min_rec = 1, // v10:102109 minRec 1 (req _34270)
    .throw_charge = 0.8f,
    .face = 4551,
};

static const ClassConfig agent = {
    .name = "Agent",
    .icon_index = 8,
    .texts = NULL,
    .loadout = (int[]) {9},
    .secondary = 0,
    .wall_jump = 1,
    .wall_jump_max = 3,
    .colors = {0xA77860, 0x3D3D3D, 0x232323, 0x282828, 0x282828, 0xBFBFBF},
    .health = 105, // v10:102131 health 110 -> 105
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.2f,
    .min_rec = 1, // v10:102131 minRec 1 (req _34270)
    .throw_charge = 0.8f,
    .face = 4552,
};

static const ClassConfig runner = {
    .name = "Runner",
    .icon_index = 9,
    .texts = (const char*[]) {"You sure about this?", "...", "Oh boy", "I don't know about this...", "Not me again..."},
    .text_count = 5,
    .loadout = (int[]) {12},
    .secondary = 0,
    .wall_jump = 1,
    .wall_jump_max = 100,
    .colors = {0xA77860, 0x3D3D3D, 0x232323, 0x282828, 0x282828, 0x232323},
    .health = 120,
    .health_segments = 6,
    .regen = 0.2f,
    .speed = 1.0f,
    .min_rec = 1, // v10:102152 minRec 1 (req _34270)
    .throw_charge = 0.6f,
    .face = 4553,
};

static const ClassConfig deagler = {
    .name = "Deagler",
    .icon_index = 10,
    .texts = NULL,
    .loadout = (int[]) {10},
    .secondary = 0,
    .wall_jump = 0,
    .hide = 1,
    .no_mobile = 1,
    .colors = {0xA77860, 0x3D3D3D, 0x232323, 0x282828, 0x282828, 0x232323},
    .health = 60,
    .health_segments = 5,
    .regen = 0.1f,
    .speed = 1.0f,
    .min_rec = 1, // v10:102174 minRec 1 (req _34270)
    .throw_charge = 0.8f,
};

static const ClassConfig bowman = {
    .name = "Bowman",
    .icon_index = 11,
    .texts = NULL,
    .loadout = (int[]) {13},
    .secondary = 1,
    .wall_jump = 0,
    .no_mobile = 1,
    .colors = {0xA77860, 0x916C52, 0x60432E, 0x282828, 0x282828, 0x473527},
    .health = 100,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.0f,
    .throw_charge = 0.8f,
    .min_rec = 1,
    .face = 4554,
};

static const ClassConfig commando = {
    .name = "Commando",
    .icon_index = 12,
    .texts = NULL,
    .loadout = (int[]) {14},
    .secondary = 1,
    .wall_jump = 0,
    .colors = {0xA77860, 0x3D3D3D, 0x232323, 0x282828, 0x995C2C, 0x171717},
    .health = 100,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.0f,
    .min_rec = 1, // v10:102218 minRec 1 (req _34270)
    .throw_charge = 0.8f,
    .face = 4555,
};

static const ClassConfig trooper = {
    .name = "Trooper",
    .icon_index = 13,
    .texts = NULL,
    .loadout = (int[]) {18},
    .secondary = 0,
    .wall_jump = 1,
    .wall_jump_max = 3,
    .colors = {0xA77860, 0xBDC2C9, 0xBDC2C9, 0x2E2E2E, 0x282828, 0x2E2E2E},
    .health = 100,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 0.9f,
    .min_rec = 1, // v10:102239 minRec 1 (req _34270)
    .throw_charge = 0.8f,
    .slide_time_mlt = 1.1f,
    .face = 4556,
};

static const ClassConfig survivor = {
    .name = "Survivor",
    .icon_index = 14,
    .texts = NULL,
    .loadout = (int[]) {19},
    .secondary = 0,
    .wall_jump = 1,
    .wall_jump_max = 100,
    .hide = 1,
    .no_mobile = 1,
    .no_sleeve = 1,
    .colors = {0xA77860, 0xA77860, 0xA77860, 0xA77860, 0x6C5042, 0xA77860},
    .health = 150,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 0.8f,
    .min_rec = 1, // v10:102260 minRec 1 (req _34270)
    .throw_charge = 0.8f,
};

static const ClassConfig infiltrator = {
    .name = "Infiltrator",
    .icon_index = 15,
    .texts = NULL,
    .loadout = (int[]) {28},
    .secondary = 1,
    .wall_jump = 0,
    .colors = {0xB18C78, 0xE4A0B7, 0xE4A0B7, 0x999999, 0xEBC967, 0x999999},
    .health = 90,
    .health_segments = 5,
    .regen = 0.1f,
    .speed = 1.0f,
    .min_rec = 1, // v10:102282 minRec 1 (req _34270)
    .throw_charge = 0.8f,
    .face = 5234,
};

// KrunkNative Nettspend (CLASS_ID_NETTSPEND 16, doc/v7/decisions.md "Nettspend class"; shared/include/
// drank.h): not a Krunker class. Purple Drank (WEAPON_ID_DRANK) as the primary, Money Spread
// (WEAPON_ID_MONEY_SPREAD) always as the secondary (kn_secondary: no Select Secondary pick), the
// knife. A Triggerman baseline (100 health in 6 segments, regen 0.1, speed 1.05, no wall jump,
// throwChrg 0.8) with the Triggerman's colours in purple and cash green. Open to everyone: every
// KrunkNative lobby (public, custom, comp, the offline sandbox) and the bots (user, 2026-10-14: "THE
// NETtspend class should be available to all"); not in an official krunker.io game (game_class_allowed).
// icon_index 16 = textures/classes/icon_16.png (client asset); no face store item, no speech lines.
// Balance (KrunkNative, not v10.0.2): the class's table values are unchanged; the Money Spread's
// weapon entry was rebalanced (kn_money_spread.c; doc/v10/gameplay.md "Nettspend balance"). The Drank
// (kn_drank.c) is unchanged.
static const ClassConfig nettspend = {
    .name = "Nettspend",
    .icon_index = 16,
    .texts = (const char*[]) {"Drank drank drank", "Pour up"},
    .text_count = 2,
    .loadout = (int[]) {WEAPON_ID_DRANK},
    .kn_secondary = WEAPON_ID_MONEY_SPREAD + 1,
    .secondary = 1,
    .wall_jump = 0,
    .colors = {0xA77860, 0x7B2CBF, 0x4A1A7A, 0x282828, 0x3FAF5A, 0x2E1047},
    .health = 100,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.05f,
    .throw_charge = 0.8f,
};

// Krunker's 16 (CLASS_COUNT), then KrunkNative's (CLASS_TABLE_COUNT)
const ClassConfig g_classes_v10[CLASS_TABLE_COUNT] = {
    triggerman, hunter, run_n_gun, spray_n_pray, vince, detective, marksman, rocketeer, agent,
    runner, deagler, bowman, commando, trooper, survivor, infiltrator,
    [CLASS_ID_NETTSPEND] = nettspend,
};
