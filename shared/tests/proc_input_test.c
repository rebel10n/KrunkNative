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

typedef struct {
    vec3 position;
    vec3 velocity;
    float *ramp_fix;
} PlayerState;

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
    const int bad = x || y || z || vx || vy || vz || ramp_fix;

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

void run_test(const char *name, Player *player) {
    char *test_base_path = concat(get_project_root(), "shared/tests/inputs/");
    char *test_path = concat(test_base_path, name);

    const cJSON *raw_ticks = load_json(test_path);

    free(test_base_path);
    free(test_path);

    if (!raw_ticks) {
        printf("Failed to parse %s, skipping... \n", name);
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
        return;
    }

    player_spawn(player);

    player->position = ticks[0].expected.position;
    player->wall_jump = !strcmp(name, "wall_jump.json");

    for (int i = 0; i < tick_count; i++) {
        if (!validate_tick(player, &ticks[i].expected, i)) {
            free(ticks);
            return;
        }

        player_proc_input(player, &ticks[i].input, 0, 0);
    }

    printf("Test OK\n");
    free(ticks);
}

int main() {
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

    return 0;
}
