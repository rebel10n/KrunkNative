// A walking AI stays on a flat floor (the floor's top is y 0). Five seconds at 60 Hz for a roaming
// walker and for a walker that chases a player on the same floor; the AI's y is checked every frame.
// Run from the repository root: ./<build>/ai_floor_test
#include <ai.h>
#include <world.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

// The floor: a 2000 x 10 x 2000 block whose top is at y 0 (as ai_test's flat map).
static const char *k_floor_map =
    "{\"name\":\"AI Floor\",\"spawns\":[[0,0,0,0,0,0]],\"objects\":["
    "{\"p\":[0,-10,0],\"s\":[2000,10,2000]}"
    "]}";

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    (void) uid;
    (void) type;
    (void) payload;
    (void) length;
}

static void game_on(Game *game, const cJSON *map) {
    static const cJSON *maps[1];
    maps[0] = map;
    memset(game, 0, sizeof(*game));
    int *modes = malloc(sizeof(int));
    modes[0] = MODE_ID_FFA;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, 1);
}

static Player *add_player(Game *game, const int uid, const vec3 at) {
    Player *player = player_init(game);
    player->uid = uid;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    player->position = at;
    player->last_position = at;
    player->god_mode = 1;
    return player;
}

// One run: spawn a walker at `start`, optionally with a player at `player_at`, tick 5 s at 60 Hz and
// report the lowest y and how many frames it was below the floor (y < -0.5).
typedef struct {
    float min_y;
    int below;
    float max_y;
} FloorRun;

static FloorRun run_walker(const char *label, const char *map_json, const vec3 start, const int with_player, const vec3 player_at, const float roam_radius, const int phys_move) {
    cJSON *map = cJSON_Parse(map_json);
    Game game;
    game_on(&game, map);
    CHECK(game.ready && game.map, "%s: the floor map loads", label);

    if (with_player) add_player(&game, 1, player_at);

    GameAIData d;
    game_ai_data_defaults(&d);
    d.health = 500.0f;
    d.speed = 1.0f;
    d.turn_speed = 10.0f;
    d.gravity = 1.0f;
    d.vision = 500;
    d.targets_players = (unsigned char) (with_player ? 1 : 0);
    d.chase_distance = 30.0f;
    d.roam_radius = roam_radius;
    d.roam_time = 2000.0f;
    d.phys_move = (unsigned char) (phys_move ? 1 : 0);
    GameAI *ai = game_ai_spawn(&game, 0, 0, "Walker", start, &d);
    CHECK(ai != NULL, "%s: the walker spawns", label);

    FloorRun r = {1000.0f, 0, -1000.0f};
    const int frames = 5 * 60;
    for (int f = 0; f < frames && ai; f++) {
        game_ai_tick(&game, 1000.0f / 60.0f);
        if (!ai->active) break;
        if (ai->position.y < r.min_y) r.min_y = ai->position.y;
        if (ai->position.y > r.max_y) r.max_y = ai->position.y;
        if (ai->position.y < -0.5f) r.below++;
    }
    printf("%s: min y %.3f, max y %.3f, frames below the floor %d of %d (end x %.1f z %.1f)\n", label, r.min_y, r.max_y, r.below,
           frames, ai ? ai->position.x : 0.0f, ai ? ai->position.z : 0.0f);
    CHECK(r.below == 0, "%s: the walker stays on the floor (%d frames below, min y %.3f)", label, r.below, r.min_y);
    CHECK(r.min_y > -0.5f, "%s: min y %.3f", label, r.min_y);
    cJSON_Delete(map);
    return r;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("Running walking AI floor test...\n");

    const vec3 origin = {0.0f, 0.0f, 0.0f};
    const vec3 near_player = {150.0f, 0.0f, 0.0f};
    const vec3 high_player = {150.0f, 40.0f, 0.0f};
    const vec3 under = {0.0f, -2.0f, 0.0f};
    run_walker("roaming walker", k_floor_map, origin, 0, origin, 200.0f, 0);
    run_walker("chasing walker", k_floor_map, origin, 1, near_player, 0.0f, 0);
    run_walker("chasing walker, target above", k_floor_map, origin, 1, high_player, 0.0f, 0);
    run_walker("roaming pMov walker", k_floor_map, origin, 0, origin, 200.0f, 1);
    run_walker("chasing pMov walker, target above", k_floor_map, origin, 1, high_player, 0.0f, 1);
    run_walker("spawned under the floor", k_floor_map, under, 1, near_player, 0.0f, 0);
    const vec3 deep = {0.0f, -6.0f, 0.0f};
    run_walker("spawned deep under the floor", k_floor_map, deep, 0, origin, 200.0f, 1);

    // a walker under a floating platform (bottom 3, top 5): the fall-back lift must not carry it up
    // through the platform; it stays at or below the floor (y 0)
    static const char *k_platform_map =
        "{\"name\":\"AI Platform\",\"spawns\":[[0,0,0,0,0,0]],\"objects\":["
        "{\"p\":[0,-10,0],\"s\":[2000,10,2000]},"
        "{\"p\":[0,3,0],\"s\":[200,2,200]}"
        "]}";
    const FloorRun under_platform = run_walker("walker under a platform", k_platform_map, origin, 0, origin, 0.0f, 0);
    CHECK(under_platform.max_y < 1.0f, "walker under a platform is not lifted through it (max y %.3f)", under_platform.max_y);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
