// rampFix ("Solid Ramp Sides", Krunker v10.0.3; Player.blockRampSide in the v10.0.3 bundle): a ramp
// with the flag blocks a player who walks into its side below its surface, and the player is moved
// out to that side; without the flag the ramp lifts the player onto it. The fixture is
// shared/tests/maps/ramp_fix.json (a 20 x 20 ramp, 6 high, direction 0, on the ground); the "on" run
// adds the flag to the parsed map. Run from the repository root: ./<build>/ramp_fix_test
#include <world.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    (void) uid;
    (void) type;
    (void) payload;
    (void) length;
}

static cJSON *load_map(const char *path) {
    cJSON *map = NULL;
    if (load_json_file(path, &map)) {
        printf("Failed to load %s (run from the repository root)\n", path);
        exit(1);
    }
    return map;
}

// An authority game (is_local) on `map` in mode `mode_id`, with the recording send hook.
static void game_on(Game *game, const cJSON *map, const int mode_id) {
    static const cJSON *maps[1];
    maps[0] = map;
    memset(game, 0, sizeof(*game));
    int *modes = malloc(sizeof(int));
    modes[0] = mode_id;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1);
}

static Player *add_player(Game *game, const int uid, const int team) {
    Player *player = player_init(game);
    player->uid = uid;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->team = team;
    return player;
}

static void place(Player *player, const float x, const float y, const float z) {
    player->position = (vec3) {x, y, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
}

// One movement frame (procInputs with no keys) at 60 fps.
static void frame(Player *player, const int reset) {
    const Input input = {.seq = player->input_seq + 1, .move_dir = -1, .delta = 1.0f / 60.0f, .reset = (unsigned char) reset};
    player_proc_input(player, &input, 0, 0);
}

typedef struct {
    float top_y;   // the highest feet height reached
    float z;       // the final z
} Walk;

// Walk +z from z -16 (the ramp's -z side is at z -10) at ground level, x fixed, for 90 frames.
static Walk walk_into_side(const int flag, const float x) {
    cJSON *json = load_map("shared/tests/maps/ramp_fix.json");
    if (flag) cJSON_AddTrueToObject(json, "rampFix");
    Game game;
    game_on(&game, json, MODE_ID_FFA);
    CHECK(game.ready && game.map->object_count == 2, "ramp_fix map: 2 objects (%zu)", game.ready ? game.map->object_count : 0);
    Player *p = add_player(&game, 1, 0);
    place(p, x, 0.0f, -16.0f);
    Walk walk = {0.0f, 0.0f};
    for (int i = 0; i < 90; i++) {
        p->velocity.z = 0.1f;
        frame(p, 0);
        if (p->position.y > walk.top_y) walk.top_y = p->position.y;
    }
    walk.z = p->position.z;
    return walk;
}

int main(void) {
    printf("Running rampFix side-block test...\n");

    // no flag: the ramp lifts the walker onto its surface
    const Walk plain = walk_into_side(0, 5.0f);
    CHECK(plain.top_y > 2.0f, "without rampFix the ramp lifts the player (top y %.3f)", plain.top_y);

    // flag: the high side blocks; the player stays on the ground, outside the ramp's -z face
    const Walk solid = walk_into_side(1, 5.0f);
    CHECK(solid.top_y < 0.5f, "rampFix: the side blocks the player below the surface (top y %.3f)", solid.top_y);
    CHECK(solid.z < -10.0f, "rampFix: the player is held outside the -z face (z %.3f)", solid.z);

    // flag, near the ramp's other end (x 9): the same side block applies across its width
    const Walk wide = walk_into_side(1, 9.0f);
    CHECK(wide.top_y < 0.5f && wide.z < -10.0f, "rampFix: blocked across the width (top y %.3f, z %.3f)", wide.top_y, wide.z);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
