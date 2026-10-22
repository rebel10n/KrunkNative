// vivo_floor_test: a player standing on Vivo's wall-top stack at (-357.726, -96.280). The stack is
// two CUBE slabs: the lower one (y 10..35, its top face switched off) and the upper one (y 35..37,
// top face on). The player's feet are placed at y 34.99, 35 and 35.01 (and 37 as the control) and
// the physics steps with no input. Supported: the feet stay at the stack top (y 37). Falls: the feet
// drop below 35. Pushed: the feet are moved sideways off the spot. Run from the repository root:
// ./<build>/vivo_floor_test
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

#define VIVO_MAP "assets/maps/official/37_vivo.json"
#define SPOT_X (-357.726f)
#define SPOT_Z (-96.280f)

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    (void) uid;
    (void) type;
    (void) payload;
    (void) length;
}

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

static Player *add_player(Game *game, const int uid) {
    Player *player = player_init(game);
    player->uid = uid;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    return player;
}

static void place(Player *player, const float x, const float y, const float z) {
    player->position = (vec3) {x, y, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
}

// One movement frame with no keys at 60 fps.
static void frame(Player *player) {
    const Input input = {.seq = player->input_seq + 1, .move_dir = -1, .delta = 1.0f / 60.0f, .reset = 0};
    player_proc_input(player, &input, 0, 0);
}

typedef struct {
    float min_y, final_y, final_x, final_z;
    int on_ground;
} Result;

// A fresh game per run, so each player starts from the same state.
static Result stand(const cJSON *map, const float feet_y) {
    Game game;
    game_on(&game, map, MODE_ID_FFA);
    Player *p = add_player(&game, 1);
    place(p, SPOT_X, feet_y, SPOT_Z);
    Result r = {feet_y, feet_y, SPOT_X, SPOT_Z, 0};
    for (int i = 0; i < 120; i++) {
        frame(p);
        if (p->position.y < r.min_y) r.min_y = p->position.y;
    }
    r.final_y = p->position.y;
    r.final_x = p->position.x;
    r.final_z = p->position.z;
    r.on_ground = p->on_ground;
    return r;
}

static void report(const char *label, const float feet_y, const Result r) {
    printf("%-8s feet %.2f: final y %.3f, min y %.3f, x %.3f, z %.3f, on_ground %d\n", label, feet_y,
           r.final_y, r.min_y, r.final_x, r.final_z, r.on_ground);
}

// Walk across the spot (velocity set per frame, as ramp_fix_test does) from 10 units before it.
static void walk(const cJSON *map, const float feet_y, const float dx, const float dz, const char *label) {
    Game game;
    game_on(&game, map, MODE_ID_FFA);
    Player *p = add_player(&game, 1);
    place(p, SPOT_X - 10.0f * dx, feet_y, SPOT_Z - 10.0f * dz);
    float min_y = feet_y, max_y = feet_y;
    for (int i = 0; i < 150; i++) {
        p->velocity.x = 0.15f * dx;
        p->velocity.z = 0.15f * dz;
        frame(p);
        if (p->position.y < min_y) min_y = p->position.y;
        if (p->position.y > max_y) max_y = p->position.y;
    }
    printf("walk %-6s feet %.2f: y %.3f..%.3f, end x %.3f z %.3f\n", label, feet_y, min_y, max_y, p->position.x, p->position.z);
}

int main(void) {
    printf("Running Vivo floor probe...\n");
    cJSON *json = NULL;
    if (load_json_file(VIVO_MAP, &json) || !json) {
        printf("Failed to load %s (run from the repository root)\n", VIVO_MAP);
        return 1;
    }
    const float feet[] = {34.99f, 35.0f, 35.01f, 37.0f};
    const char *labels[] = {"below", "at", "above", "control"};
    for (int i = 0; i < 4; i++) {
        const Result r = stand(json, feet[i]);
        report(labels[i], feet[i], r);
        // supported: the feet end on the stack top; the spot does not move
        const int supported = fabsf(r.final_y - 37.0f) < 0.05f;
        CHECK(supported, "%s feet %.2f: supported on the stack top at y 37 (final y %.3f)", labels[i], feet[i], r.final_y);
        CHECK(fabsf(r.final_x - SPOT_X) < 0.05f && fabsf(r.final_z - SPOT_Z) < 0.05f,
              "%s feet %.2f: not pushed off the spot (x %.3f, z %.3f)", labels[i], feet[i], r.final_x, r.final_z);
    }

    {
        const float heights[] = {45.0f, 36.0f, 35.5f};
        for (int i = 0; i < 3; i++) {
            Result r = stand(json, heights[i]);
            report("drop", heights[i], r);
        }
        // one 30 fps frame step
        Game game;
        game_on(&game, json, MODE_ID_FFA);
        Player *p = add_player(&game, 1);
        place(p, SPOT_X, 35.0f, SPOT_Z);
        for (int i = 0; i < 90; i++) {
            const Input in = {.seq = p->input_seq + 1, .move_dir = -1, .delta = 1.0f / 30.0f, .reset = 0};
            player_proc_input(p, &in, 0, 0);
        }
        printf("30fps feet 35.00: final y %.3f\n", p->position.y);
    }
    walk(json, 37.0f, 1.0f, 0.0f, "+x");
    walk(json, 37.0f, 0.0f, 1.0f, "+z");
    walk(json, 35.0f, 1.0f, 0.0f, "+x");
    walk(json, 35.0f, 0.0f, 1.0f, "+z");
    cJSON_Delete(json);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
