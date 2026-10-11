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

#define RULE_CHECK(cond, ...) do { if (!(cond)) { printf("FAIL: " __VA_ARGS__); printf("\n"); g_failures++; } } while (0)

// Replace the spawned loadout (player_spawn frees these on the next spawn).
static void set_loadout(Player *player, const int *ids, const int count, const int index) {
    free(player->loadout);
    free(player->ammo);
    free(player->reloads);
    player->loadout = calloc((size_t) count, sizeof(int));
    player->ammo = calloc((size_t) count, sizeof(int));
    player->reloads = calloc((size_t) count, sizeof(float));
    player->loadout_size = (size_t) count;
    for (int i = 0; i < count; i++) {
        player->loadout[i] = ids[i];
        player->ammo[i] = (int) player->game->weapons[ids[i]]->ammo;
    }
    player_swap_weapon(player, index, 1, 1, 0);
}

static int rule_input(Player *player, const int swap, const int scroll) {
    Input input = {0};
    input.move_dir = -1;
    input.delta = 0.016f;
    input.x_dir = player->direction.x;
    input.y_dir = player->direction.y;
    input.swap = (unsigned char) swap;
    input.scroll = (signed char) scroll;
    player_proc_input(player, &input, 0, 1);
    return player->loadout_index;
}

// 95.js pY2 / swapMelee / swapSecondary and the procInputs rounding (input-audit #3, #4, #6).
static void run_input_rule_tests(Player *player) {
    printf("Running input rule tests (swap / scroll / rounding)... \n");
    const int failures = g_failures;
    player_spawn(player);
    player->direction = (vec2) {0.0f, 0.0f};

    // scroll wrap: ammos.length - (length > 3 ? 3 : 2)
    const int two[] = {1, WEAPON_ID_KNIFE};
    set_loadout(player, two, 2, 0);
    RULE_CHECK(rule_input(player, 0, 1) == 0, "2 weapons: scroll up from 0 wraps to 0 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 0, -1) == 1, "2 weapons: scroll down from 0 -> min(1, 2) = 1 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 0, -1) == 0, "2 weapons: scroll down from 1 -> 0 (got %d)", player->loadout_index);

    const int three[] = {1, WEAPON_ID_PISTOL, WEAPON_ID_KNIFE};
    set_loadout(player, three, 3, 0);
    RULE_CHECK(rule_input(player, 0, 1) == 1, "3 weapons: scroll up 0 -> 1 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 0, 1) == 0, "3 weapons: scroll up 1 wraps to 0, melee skipped (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 0, -1) == 2, "3 weapons: scroll down 0 -> 2 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 0, -1) == 0, "3 weapons: scroll down 2 -> 0 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 1, 1) == 1, "3 weapons: a swap key wins over the wheel (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 1, 0) == 0, "3 weapons: secondary key on secondary -> 0 (got %d)", player->loadout_index);

    const int four[] = {1, WEAPON_ID_PISTOL, 17 /* Bomb, equipment */, WEAPON_ID_KNIFE};
    set_loadout(player, four, 4, 0);
    RULE_CHECK(rule_input(player, 0, 1) == 1, "4 weapons: scroll up 0 -> 1 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 0, 1) == 0, "4 weapons: scroll up 1 wraps to 0 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 0, -1) == 2, "4 weapons: scroll down 0 -> min(3, 2) = 2 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 0, 1) == 0, "4 weapons: scroll up 2 wraps to 0 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 4, 0) == 2, "4 weapons: equipment key -> 2 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 4, 0) == 0, "4 weapons: equipment key on equipment -> 0 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 2, 0) == 3, "4 weapons: melee key -> 3 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 2, 0) == 0, "4 weapons: melee key on melee -> reload(+1) wraps to 0 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 3, 0) == 0 && rule_input(player, 1, 0) == 1 && rule_input(player, 3, 0) == 0, "4 weapons: primary key -> 0 (got %d)", player->loadout_index);

    // melee key on melee steps to the next slot instead of going back to 0
    const int melee_first[] = {WEAPON_ID_KNIFE, 1, WEAPON_ID_PISTOL};
    set_loadout(player, melee_first, 3, 0);
    RULE_CHECK(rule_input(player, 2, 0) == 1, "melee in slot 0: melee key on melee -> reload(+1) = 1 (got %d)", player->loadout_index);
    RULE_CHECK(rule_input(player, 2, 0) == 0, "melee in slot 0: melee key from 1 -> 0 (got %d)", player->loadout_index);

    const int one[] = {WEAPON_ID_KNIFE};
    set_loadout(player, one, 1, 0);
    RULE_CHECK(rule_input(player, 0, 1) == 0 && rule_input(player, 2, 0) == 0, "1 weapon: no scroll / swap (got %d)", player->loadout_index);

    // rounding: yaw/pitch to 0.001 rad, delta to whole ms (crouchVal is rounded to 3 decimals after)
    player_spawn(player);
    Input input = {0};
    input.move_dir = -1;
    input.x_dir = 0.12345f;
    input.y_dir = -2.71828f;
    input.delta = 0.0166f;
    input.crouch = 1;
    player->crouch_val = 0.0f;
    player_proc_input(player, &input, 0, 1);
    RULE_CHECK(fabsf(player->direction.x - 0.123f) < 1e-6f && fabsf(player->direction.y - -2.718f) < 1e-6f,
               "yaw/pitch round(3): got %.6f %.6f", player->direction.x, player->direction.y);
    const float crouch_17 = player->crouch_val;
    player->crouch_val = 0.0f;
    input.delta = 0.017f;
    player_proc_input(player, &input, 0, 1);
    RULE_CHECK(player->crouch_val == crouch_17, "delta 16.6 ms runs as 17 ms: crouch %.6f vs %.6f", crouch_17, player->crouch_val);
    RULE_CHECK(fabsf(crouch_17 - roundf(game_constants.crouch_speed * 0.017f * 1000.0f) / 1000.0f) < 1e-6f,
               "crouchVal round(3): got %.6f", crouch_17);
    player->crouch_val = 0.0f;
    input.delta = -0.01f;
    player_proc_input(player, &input, 0, 1);
    RULE_CHECK(player->crouch_val == 0.0f, "negative delta runs as 0 (crouch %.6f)", player->crouch_val);
    input.delta = 0.0164f; // 16 ms
    input.x_dir = 0.0004f; // below 0.0005 rad: rounds to 0
    player_proc_input(player, &input, 0, 1);
    RULE_CHECK(player->direction.x == 0.0f, "pitch 0.0004 rounds to 0 (got %.6f)", player->direction.x);

    // the wheel field survives the INPUT packet; shorter packets read as no scroll
    NetInputPacket net = {0};
    Input in = {0}, out = {0};
    in.scroll = -1;
    in.swap = 4;
    net_packet_from_input(&net, &in);
    NetPacket packet = {0};
    packet.length = sizeof(net);
    memcpy(packet.payload, &net, sizeof(net));
    NetInputPacket read = {0};
    RULE_CHECK(net_packet_read_input(&read, &packet), "24-byte INPUT reads");
    net_packet_to_input(&out, &read);
    RULE_CHECK(out.scroll == -1 && out.swap == 4, "INPUT scroll/swap round trip (got %d %d)", out.scroll, out.swap);
    packet.length = NET_INPUT_PACKET_V2_SIZE;
    RULE_CHECK(net_packet_read_input(&read, &packet) && read.scroll == 0, "23-byte INPUT reads with scroll 0");

    player_spawn(player);
    if (g_failures == failures) printf("Test OK\n");
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

    run_input_rule_tests(player);
    printf("\n");

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
    printf("\n");

    char *soul_path = concat(get_project_root(), "assets/maps/soul_sanctum.json");
    const cJSON *soul = load_json(soul_path);

    free(soul_path);

    if (!soul) {
        printf("Failed to load soul_sanctum! \n");
        return 1;
    }

    game.map_count = 0; // see kanji above
    game_configure(&game, NULL, &soul, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, 1);

    // Soul Sanctum's spawn stands on a cylinder top (y 3): v3.1.1 collides with it inclusively,
    // so the player stays grounded (no onGround flicker, no step sound every other tick).
    run_test("v3_soul_spawn_60fps.json", player);
    printf("\n");
    run_test("v3_soul_spawn_30fps.json", player);
    printf("\n");

    printf("Running Soul Sanctum spawn landing test...\n");
    const int failures = g_failures;
    player_spawn(player);
    player->position = (vec3) {1.0f, 4.0f, 0.0f};
    player->velocity = (vec3) {0};
    player->is_you = player->render_you = 1;
    player->land_events = 0;
    Input idle = {0};
    idle.move_dir = -1;
    idle.delta = 0.017f;
    int grounded_since = -1, ungrounded_after = 0;
    for (int i = 0; i < 180; i++) {
        player_proc_input(player, &idle, 0, 0);
        if (player->on_ground && grounded_since < 0) grounded_since = i;
        else if (!player->on_ground && grounded_since >= 0) ungrounded_after++;
    }
    RULE_CHECK(grounded_since >= 0 && fabsf(player->position.y - 3.0f) < 1e-4f, "lands on the spawn disk (y %.4f)", player->position.y);
    RULE_CHECK(ungrounded_after == 0, "stays grounded on the disk (%d airborne ticks after landing)", ungrounded_after);
    RULE_CHECK(player->land_events == 1, "one landing, got %d", player->land_events);
    player->is_you = player->render_you = 0;
    if (g_failures == failures) printf("Test OK\n");

    return g_failures ? 1 : 0;
}
