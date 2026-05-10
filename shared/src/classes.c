#include <shared.h>

const ClassConfig triggerman = {
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
};

const ClassConfig hunter = {
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
    .speed = 1.05f,
};

const ClassConfig run_n_gun = {
    .name = "Run N Gun",
    .icon_index = 2,
    .texts = NULL,
    .loadout = (int[]) {3},
    .secondary = 0,
    .wall_jump = 1,
    .colors = {0xA77860, 0x3E6382, 0x2F4B63, 0x282828, 0x634732, 0x1A2B3A},
    .health = 100,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.18f,
};

const ClassConfig spray_n_pray = {
    .name = "Spray N Pray",
    .icon_index = 3,
    .texts = (const char*[]) {"Calling in the Big Guns?", "Remember - No Russian.", "Pesky Snipers..."},
    .loadout = (int[]) {3},
    .secondary = 0,
    .wall_jump = 0,
    .colors = {0xA77860, 0x586849, 0x49563C, 0x282828, 0x282828, 3160103},
    .health = 170,
    .health_segments = 7,
    .regen = 0.05f,
    .speed = 0.95f,
};

const ClassConfig vince = {
    .name = "Vince",
    .icon_index = 4,
    .texts = (const char*[]) {"..."},
    .loadout = (int[]) {5},
    .secondary = 1,
    .wall_jump = 0,
    .colors = {8412234, 5526119, 4144461, 0x282828, 0x282828, 2697267},
    .health = 90,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.0f,
};

const ClassConfig detective = {
    .name = "Detective",
    .icon_index = 5,
    .texts = (const char*[]) {"I'm onto something"},
    .loadout = (int[]) {4},
    .secondary = 0,
    .wall_jump = 0,
    .colors = {0xA77860, 7360054, 4410462, 0x282828, 0x634732, 4140062},
    .health = 100,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.0f,
};

const ClassConfig marksman = {
    .name = "Marksman",
    .icon_index = 6,
    .texts = NULL,
    .loadout = (int[]) {7},
    .secondary = 1,
    .wall_jump = 0,
    .colors = {0xA77860, 0x586849, 0x49563C, 0x282828, 0x282828, 2699298},
    .health = 90,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.0f,
};

const ClassConfig rocketeer = {
    .name = "Rocketeer",
    .icon_index = 7,
    .texts = (const char*[]) {"..."},
    .loadout = (int[]) {8},
    .secondary = 0,
    .wall_jump = 0,
    .colors = {0xA77860, 0x586849, 0x49563C, 0x282828, 0x6C5042, 0x2B3324},
    .health = 130,
    .health_segments = 7,
    .regen = 0.1f,
    .speed = 0.86f,
};

const ClassConfig agent = {
    .name = "Agent",
    .icon_index = 8,
    .texts = NULL,
    .loadout = (int[]) {9},
    .secondary = 0,
    .wall_jump = 1,
    .colors = {0xA77860, 0x3D3D3D, 0x232323, 0x282828, 0x282828, 0xBFBFBF},
    .health = 100,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.2f,
};

const ClassConfig runner = {
    .name = "Runner",
    .icon_index = 9,
    .texts = (const char*[]) {"You sure about this?", "...", "Oh boy", "I don't know about this...", "Not me again..."},
    .loadout = (int[]) {12},
    .secondary = 0,
    .wall_jump = 1,
    .colors = {0xA77860, 0x3D3D3D, 0x232323, 0x282828, 0x282828, 0x232323},
    .health = 120,
    .health_segments = 6,
    .regen = 0.2f,
    .speed = 1.0f,
};

const ClassConfig deagler = {
    .name = "Deagler",
    .icon_index = 10,
    .texts = NULL,
    .loadout = (int[]) {10},
    .secondary = 0,
    .wall_jump = 0,
    .colors = {0xA77860, 0x3D3D3D, 0x232323, 0x282828, 0x282828, 0x232323},
    .health = 60,
    .health_segments = 5,
    .regen = 0.1f,
    .speed = 1.0f,
};

const ClassConfig bowman = {
    .name = "Bowman",
    .icon_index = 11,
    .texts = NULL,
    .loadout = (int[]) {13},
    .secondary = 1,
    .wall_jump = 0,
    .colors = {0xA77860, 0x916C52, 0x6043E2, 0x282828, 0x282828, 0x473527},
    .health = 100,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.0f,
};

const ClassConfig commando = {
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
};

const ClassConfig trooper = {
    .name = "Trooper",
    .icon_index = 13,
    .texts = NULL,
    .loadout = (int[]) {18},
    .secondary = 0,
    .wall_jump = 0,
    .colors = {0xA77860, 0xBDC2C9, 0xBDC2C9, 0x2E2E2E, 0x282828, 0x2E2E2E},
    .health = 100,
    .health_segments = 6,
    .regen = 0.1f,
    .speed = 1.0f,
};

const ClassConfig g_classes[] = {
    triggerman, hunter, run_n_gun, spray_n_pray,
    vince, detective, marksman, rocketeer, agent,
    runner, deagler, bowman, commando, trooper,
};