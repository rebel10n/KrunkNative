#include <shared.h>

// Krunker v10.0.2 classes (doc/v10/gameplay.md section 4). TODO(v10-gameplay): for now a copy of
// the v7.1.4 table (shared/src/rules/v7/classes.c); the v10 HP and stat changes go in here.

// Krunker v7.1.4 classes (src/data/classes.js), generated from shared/tests/data/v7/classes.json
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
    .health = 90,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.18f,
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
    .health = 180,
    .health_segments = 7,
    .regen = 0.05f,
    .speed = 0.9f,
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
    .health = 110,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.2f,
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
    .throw_charge = 0.8f,
    .face = 5234,
};

const ClassConfig g_classes_v10[CLASS_COUNT] = {
    triggerman, hunter, run_n_gun, spray_n_pray, vince, detective, marksman, rocketeer, agent,
    runner, deagler, bowman, commando, trooper, survivor, infiltrator,
};
