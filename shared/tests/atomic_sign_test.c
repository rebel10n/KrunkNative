// Atomic's object 833 (a PREFAB_SIGN, bv 1: no background) was a collidable sign on an invisible surface
// at p [-64.1, 64.1, 137], scale [85, 0.01, 26]: a player could stand on it at (-86.117088, 64.110001,
// 143.965744), where official krunker.io has nothing. The record now carries "l": 1 (the flag v10.0.2's
// addObject reads as noCol = l || col), so the sign loads walk-through. Object indices are unchanged:
// the record stays at index 833 (uids are indices, and nothing references it).
// Fail-before: the player stands at feet height 64.11 on the sign; after the fix he falls.
// Run from the repository root: ./<build>/atomic_sign_test
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

// An authority game (is_local) on `map` in FFA, with the recording send hook.
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

static Player *add_player(Game *game, const int uid) {
    Player *player = player_init(game);
    player->uid = uid;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    player_spawn(player);
    return player;
}

// One movement frame (procInputs with no keys) at 60 fps.
static void frame(Player *player, const int reset) {
    const Input input = {.seq = player->input_seq + 1, .move_dir = -1, .delta = 1.0f / 60.0f, .reset = (unsigned char) reset};
    player_proc_input(player, &input, 0, 0);
}

int main(void) {
    printf("Running Atomic invisible-sign test...\n");

    cJSON *json = load_map("assets/maps/official/23_atomic.json");
    Game game;
    game_on(&game, json);
    CHECK(game.ready && game.map, "Atomic loads");
    if (!game.ready || !game.map) {
        printf("%d checks, %d failures\n", g_checks, g_failures);
        return 1;
    }

    // the record keeps its index: object 833 is still the sign, now walk-through, and the sign after it
    // (834) is untouched; nothing was removed
    CHECK(game.map->object_count == 1252, "Atomic keeps all 1252 objects (%zu)", game.map->object_count);
    if (game.map->object_count > 833) {
        const Object *sign = game.map->objects[833];
        CHECK(sign && sign->raw && cJSON_IsNumber(cJSON_GetObjectItem(sign->raw, "i")) &&
              (int) cJSON_GetNumberValue(cJSON_GetObjectItem(sign->raw, "i")) == 30,
              "object 833 is still the PREFAB_SIGN record");
        CHECK(sign && sign->collision_type == COLLISION_TYPE_NONE,
              "object 833 (the sign) has no collision body (type %d)", sign ? (int) sign->collision_type : -1);
    }

    // the spot: a player placed at the reported position must fall, not stand on the sign
    Player *p = add_player(&game, 1);
    p->position = (vec3) {-86.117088f, 64.110001f, 143.965744f};
    p->last_position = p->position;
    p->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    const float start_y = p->position.y;
    for (int i = 0; i < 240; i++) frame(p, 0);
    CHECK(p->position.y < start_y - 1.0f,
          "the player falls at (-86.117088, 64.110001, 143.965744): feet y %.3f after 4 s, from %.3f", p->position.y, start_y);

    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
