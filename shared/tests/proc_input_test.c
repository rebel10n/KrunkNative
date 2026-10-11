#include <math.h>
#include <shared.h>
#include <stdio.h>

#ifdef WIN32
#include <direct.h>
#else
#include <unistd.h>
#include <string.h>
#endif

// most of the differences can be attributed to cumulative floating point errors
#define EPSILON 1e-3

// First-person feel values a tick can also carry (state[7..13]): the deterministic,
// per-input part of Krunker's local-player animation (players.js procInputs/playerStep).
#define FEEL_COUNT 7
static const char *feel_names[FEEL_COUNT] = {"step_val", "step_chase", "bob_anim_y", "bob_anim_z", "lean_anim_x", "lean_anim_y", "lean_anim_z"};

typedef struct {
    vec3 position;
    vec3 velocity;
    float *ramp_fix;
    int has_feel;
    float feel[FEEL_COUNT];
} PlayerState;

static void player_feel_values(const Player *player, float out[FEEL_COUNT]) {
    out[0] = player->step_val;
    out[1] = player->step_chase;
    out[2] = player->bob_anim.y;
    out[3] = player->bob_anim.z;
    out[4] = player->lean_anim.x;
    out[5] = player->lean_anim.y;
    out[6] = player->lean_anim.z;
}

typedef struct {
    Input input;
    PlayerState expected;
} Tick;

float get_num(const cJSON *array, const int i) {
    return (float) cJSON_GetNumberValue(cJSON_GetArrayItem(array, i));
}

int parse_tick(const cJSON *raw, Tick *out) {
    if (!cJSON_IsArray(raw) || cJSON_GetArraySize(raw) < 2) return -1;

    const cJSON *raw_input = cJSON_GetArrayItem(raw, 0);
    const cJSON *raw_state = cJSON_GetArrayItem(raw, 1);

    if (!cJSON_IsArray(raw_input) || cJSON_GetArraySize(raw_input) < 7 || !cJSON_IsArray(raw_state) || cJSON_GetArraySize(raw_state) < 7) return -1;

    out->input.delta = get_num(raw_input, 0) / 1000.0f;
    out->input.y_dir = get_num(raw_input, 1) / 1000.0f;
    out->input.x_dir = get_num(raw_input, 2) / 1000.0f;

    out->input.move_dir = (int) get_num(raw_input, 3);
    out->input.jump = (int) get_num(raw_input, 4);
    out->input.crouch = (int) get_num(raw_input, 5);
    out->input.scope = (int) get_num(raw_input, 6);

    if (out->input.move_dir != -1) {
        out->input.move_dir += 7;
    }

    out->expected.position.x = get_num(raw_state, 0);
    out->expected.position.y = get_num(raw_state, 1);
    out->expected.position.z = get_num(raw_state, 2);
    out->expected.velocity.x = get_num(raw_state, 3);
    out->expected.velocity.y = get_num(raw_state, 4);
    out->expected.velocity.z = get_num(raw_state, 5);

    if (cJSON_IsNumber(cJSON_GetArrayItem(raw_state, 6))) {
        out->expected.ramp_fix = calloc(1, sizeof(float));
        if (out->expected.ramp_fix) *out->expected.ramp_fix = get_num(raw_state, 6);
    }

    if (cJSON_GetArraySize(raw_state) >= 7 + FEEL_COUNT) {
        out->expected.has_feel = 1;
        for (int i = 0; i < FEEL_COUNT; i++) out->expected.feel[i] = get_num(raw_state, 7 + i);
    }

    return 0;
}

void print_field_delta(const char *name, const float expected, const float actual) {
    const float delta = actual - expected;
    const int bad = fabsf(delta) > EPSILON;

    const char *red = "\x1b[31m";
    const char *green = "\x1b[32m";
    const char *yellow = "\x1b[33m";
    const char *reset = "\x1b[0m";

    printf(
        "| %-30s | %s%-30f%s | %s%-30f%s | %s%-30f%s |\n",
        name,
        bad ? green : reset, expected, reset,
        bad ? red : reset, actual, reset,
        bad ? yellow : reset, delta, reset
    );
}

int validate_tick(const Player *player, const PlayerState *expected, const int i) {
    const float dx = player->position.x - expected->position.x;
    const float dy = player->position.y - expected->position.y;
    const float dz = player->position.z - expected->position.z;

    const int x = fabsf(dx) > EPSILON;
    const int y = fabsf(dy) > EPSILON;
    const int z = fabsf(dz) > EPSILON;

    const float dvx = player->velocity.x - expected->velocity.x;
    const float dvy = player->velocity.y - expected->velocity.y;
    const float dvz = player->velocity.z - expected->velocity.z;

    const int vx = fabsf(dvx) > EPSILON;
    const int vy = fabsf(dvy) > EPSILON;
    const int vz = fabsf(dvz) > EPSILON;

    const int ramp_fix = !!expected->ramp_fix ^ !!player->ramp_fix || (expected->ramp_fix && player->ramp_fix && fabsf(*player->ramp_fix - *expected->ramp_fix) > EPSILON);

    float feel[FEEL_COUNT];
    player_feel_values(player, feel);
    int feel_bad = 0;
    for (int f = 0; expected->has_feel && f < FEEL_COUNT; f++) feel_bad |= fabsf(feel[f] - expected->feel[f]) > EPSILON;

    const int bad = x || y || z || vx || vy || vz || ramp_fix || feel_bad;

    if (bad) {
        printf("Failed at tick #%d! \n", i);

        printf("|--------------------------------|--------------------------------|--------------------------------|--------------------------------|\n");
        printf("| %-30s | %-30s | %-30s | %-30s |\n", "field", "expected", "actual", "delta");
        printf("|--------------------------------|--------------------------------|--------------------------------|--------------------------------|\n");

        print_field_delta("x", expected->position.x, player->position.x);
        print_field_delta("y", expected->position.y, player->position.y);
        print_field_delta("z", expected->position.z, player->position.z);

        print_field_delta("vel_x", expected->velocity.x, player->velocity.x);
        print_field_delta("vel_y", expected->velocity.y, player->velocity.y);
        print_field_delta("vel_z", expected->velocity.z, player->velocity.z);

        if (expected->ramp_fix) {
            print_field_delta("ramp_fix", *expected->ramp_fix, player->ramp_fix ? *player->ramp_fix : 0.0f);
        }

        for (int f = 0; expected->has_feel && f < FEEL_COUNT; f++) print_field_delta(feel_names[f], expected->feel[f], feel[f]);

        printf("|--------------------------------|--------------------------------|--------------------------------|--------------------------------|\n");
    }

    return !bad;
}

const cJSON *load_json(const char *filename) {
    size_t file_length;
    unsigned char *file_data;

    if (read_file(filename, &file_data, &file_length)) {
        return NULL;
    }

    const cJSON *parsed = cJSON_Parse((char *) file_data);

    free(file_data);
    return parsed;
}

const char *get_project_root() {
    char path[256];
    if (!getcwd(path, sizeof(path))) return NULL;

#ifdef WIN32
    const char slash = '\\';
    const char *cmake_debug_suffix = "\\cmake-build-debug";
    const char *cmake_release_suffix = "\\cmake-build-release";
#else
    const char slash = '/';
    const char *cmake_debug_suffix = "/cmake-build-debug";
    const char *cmake_release_suffix = "/cmake-build-release";
#endif

    const size_t path_length = strlen(path);
    const size_t debug_length = strlen(cmake_debug_suffix);
    const size_t release_length = strlen(cmake_release_suffix);

    int devel = 0;

    if (path_length >= debug_length && !strcmp(path + path_length - debug_length, cmake_debug_suffix)) devel = 1;
    if (path_length > debug_length && !strcmp(path + path_length - debug_length - 1, cmake_debug_suffix) && path[path_length-1] == slash) devel = 1;

    if (path_length >= release_length && !strcmp(path + path_length - release_length, cmake_release_suffix)) devel = 1;
    if (path_length > release_length && !strcmp(path + path_length - release_length - 1, cmake_release_suffix) && path[path_length-1] == slash) devel = 1;

    const char *devel_assets = "../";
    const char *assets = "./";

    return devel ? devel_assets : assets;
}

static int g_failures = 0;

void run_test(const char *name, Player *player) {
    // Absolute paths (CLI replay of recorded/synthetic v3.1.1 runs) are used as is.
    char *test_base_path = concat(get_project_root(), "shared/tests/inputs/");
    char *test_path = name[0] == '/' ? concat(name, "") : concat(test_base_path, name);

    const cJSON *raw_ticks = load_json(test_path);

    free(test_base_path);
    free(test_path);

    if (!raw_ticks) {
        printf("Failed to parse %s, skipping... \n", name);
        g_failures++;
        return;
    }

    const int tick_count = cJSON_GetArraySize(raw_ticks);
    Tick *ticks = calloc(tick_count, sizeof(Tick));

    if (!ticks) {
        printf("Failed to allocate ticks array for %s, skipping... (bad input?) \n", name);
        return;
    }

    printf("Running test %s (%d ticks)... \n", name, tick_count);

    for (int i = 0; i < tick_count; i++) {
        if (!parse_tick(cJSON_GetArrayItem(raw_ticks, i), &ticks[i])) continue;
        printf("Failed to parse tick #%d! \n", i);

        free(ticks);
        g_failures++;
        return;
    }

    player_spawn(player);

    player->position = ticks[0].expected.position;
    player->wall_jump = !strcmp(name, "wall_jump.json");
    // Recorded/synthetic runs start from a long-alive v3.1.1 player: no spawn swap delay.
    if (name[0] == '/') player->swap_timer = 0.0f;

    // Ticks carrying feel values were recorded from the local (rendered) player.
    player->is_you = player->render_you = tick_count > 0 && ticks[0].expected.has_feel;
    if (player->is_you) {
        player->direction.x = ticks[0].input.x_dir;
        player->direction.y = ticks[0].input.y_dir;
    }

    for (int i = 0; i < tick_count; i++) {
        if (!validate_tick(player, &ticks[i].expected, i)) {
            free(ticks);
            g_failures++;
            return;
        }

        player_proc_input(player, &ticks[i].input, 0, 0);
    }

    printf("Test OK\n");
    free(ticks);
}

// proc_input_test <map.json> <ticks.json>...: replay recorded/synthetic v3.1.1 input
// runs (tools/kref/movement-synth.js / movement-rec.js) on one map.
static int replay_main(const int argc, char **argv) {
    const cJSON *map = load_json(argv[1]);
    if (!map) {
        printf("Failed to load %s! \n", argv[1]);
        return 1;
    }

    Game game = {};
    game_configure(&game, NULL, &map, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1);

    Player *player = player_init(&game);
    if (!player) return 1;
    game_players_add(&game, player);

    for (int i = 2; i < argc; i++) {
        run_test(argv[i], player);
        printf("\n");
    }

    return g_failures ? 1 : 0;
}

int main(const int argc, char **argv) {
    if (argc > 2) return replay_main(argc, argv);

    const char *tests[] = {
        "collide.json",
        "crouch.json",
        "jump.json",
        "scope.json",
        "slide.json",
        "spawn.json",
        "walk_forwards.json",
        "walk_ladder.json",
        "walk_ramp.json",
        "wall_jump.json",
        // Generated from the real v3.1.1 client by tools/kref/movement-synth.js at fixed
        // frame rates; these also check the first-person feel values (step/bob/lean).
        "v3_walk_diag_60fps.json",     // diagonal strafe speed (config.strafeSpd)
        "v3_aim_walk_60fps.json",      // aim slow while walking
        "v3_crouch_slide_144fps.json", // sprint -> jump -> crouch slide, slide timer expiry
        "v3_sprint_jump_30fps.json",   // jump push, landing, step bob
        "v3_look_60fps.json",          // lean sway signs (yaw/pitch), strafe step
        "v3_ladder_60fps.json",        // ladder climb, ladder step x1.4
        "v3_turn_jump_144fps.json",    // turning in the air
        "v3_crouch_walk_30fps.json",   // crouch speed, crouch bob, crouchVal.round(3)
    };

    char *sandstorm_path = concat(get_project_root(), "assets/maps/sandstorm.json");
    const cJSON *sandstorm = load_json(sandstorm_path);

    free(sandstorm_path);

    if (!sandstorm) {
        printf("Failed to load sandstorm! \n");
        return 1;
    }

    Game game = {};
    game_configure(&game, NULL, &sandstorm, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1);

    Player *player = player_init(&game);

    if (!player) {
        printf("Failed to create test player! \n");
        return 1;
    }

    game_players_add(&game, player);

    for (size_t i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        run_test(tests[i], player);
        printf("\n");
    }

    char *kanji_path = concat(get_project_root(), "assets/maps/kanji.json");
    const cJSON *kanji = load_json(kanji_path);

    free(kanji_path);

    if (!kanji) {
        printf("Failed to load sandstorm! \n");
        return 1;
    }

    game.map_count = 0; // hack: game_configure free()'s maps (assumes maps is malloc()'d), so to prevent that we set map_count to 0

    game_configure(&game, NULL, &kanji, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1);

    run_test("kanji_jump_pad.json", player);
    printf("\n");
    run_test("v3_kanji_jump_pad_30fps.json", player);

    return g_failures ? 1 : 0;
}
