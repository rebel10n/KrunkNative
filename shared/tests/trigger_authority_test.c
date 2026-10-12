// Map logic runs on the authority only (game_is_authority: is_local or a server), the same
// shared code for server_bin, the client's in-process sandbox server and the old local game:
// a TRIGGER's onEnter action (Soul Sanctum's jump scare, its interface teleport) and an
// interact-key gate purchase fire on an authority game, and never on a client mirror
// (is_local 0, no server), whose events only arrive from its server.
// Run from the repository root: ./<build>/trigger_authority_test
#include <shared.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

static int g_sent[256];

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game; (void) uid; (void) payload; (void) length;
    if (type >= 0 && type < 256) g_sent[type]++;
}

// The map object at (x, y, z) (base centre) of a given flag, -1 none.
static Object *object_at(const Game *game, const float x, const float y, const float z) {
    for (size_t i = 0; i < game->map->object_count; i++) {
        Object *o = game->map->objects[i];
        if (fabsf(o->position.x - x) < 0.5f && fabsf(o->position.y - y) < 0.5f && fabsf(o->position.z - z) < 0.5f) return o;
    }
    return NULL;
}

// One input frame standing at feet (x, y, z): the map collision pass (onEnter, zones) runs.
static void stand_at(Game *game, Player *player, const float x, const float y, const float z) {
    player->position = (vec3) {x, y, z};
    player->last_position = player->position;
    player->velocity = (vec3) {0.0f, 0.0f, 0.0f};
    player->in_object = 0;
    const Input input = {.seq = player->input_seq + 1, .move_dir = -1, .delta = 1.0f / 60.0f, .x_dir = 0.0f, .y_dir = 0.0f};
    player_queue_input(player, &input);
    game_tick(game, game->now + 1.0f / 60.0f, 1.0f / 60.0f);
}

typedef enum { AUTH_LOCAL, AUTH_SERVER, MIRROR } Kind;

static void run(const cJSON *map, const Kind kind) {
    static const char *const labels[] = {"is_local", "server", "mirror"};
    static const cJSON *maps[1];
    static int fake_server;
    maps[0] = map;

    Game game = {0};
    if (kind == AUTH_SERVER) game.server = &fake_server;
    game_configure(&game, NULL, maps, 1, NULL, 0, NULL, 0, NULL, 0);
    game_init(&game, 0, -1, kind == AUTH_LOCAL);
    game.hooks.send = test_send;
    CHECK(game.ready, "%s: game ready", labels[kind]);
    if (!game.ready) return;
    CHECK(game_is_authority(&game) == (kind != MIRROR), "%s: authority", labels[kind]);

    Player *player = player_init(&game);
    player->uid = 1;
    snprintf(player->name, sizeof(player->name), "Tester");
    game_players_add(&game, player);
    player_spawn(player);
    player->god_mode = 1; // the AIs and damage triggers must not end the test early

    // onEnter -> Jump Scare (trigger 841, single use, interface 6)
    memset(g_sent, 0, sizeof(g_sent));
    stand_at(&game, player, 454.0f, 220.0f, -105.0f);
    CHECK((g_sent[NET_PACKET_JUMP_SCARE] > 0) == (kind != MIRROR), "%s: jump scare sent %d", labels[kind], g_sent[NET_PACKET_JUMP_SCARE]);

    // onEnter -> Teleport To Interface 6 (trigger 866): the player moves (doRecon "changePosition")
    memset(g_sent, 0, sizeof(g_sent));
    stand_at(&game, player, 358.0f, 220.0f, -102.0f);
    const float moved = hypotf(player->position.x - 358.0f, player->position.z + 102.0f);
    CHECK((moved > 50.0f) == (kind != MIRROR), "%s: teleported %.1f units", labels[kind], (double) moved);
    CHECK((g_sent[NET_PACKET_POSITION] > 0) == (kind != MIRROR), "%s: position sent %d", labels[kind], g_sent[NET_PACKET_POSITION]);

    // the interact key at a score gate (1057: 150 score, paid): bought and opened
    Object *gate = object_at(&game, 705.0f, 314.0f, -239.0f);
    CHECK(gate && gate->gate, "%s: gate 1057 parsed", labels[kind]);
    if (gate) {
        const int was_active = gate->active;
        memset(g_sent, 0, sizeof(g_sent));
        player->score = 300;
        player->position = (vec3) {700.0f, 314.0f, -239.0f};
        player->last_position = player->position;
        game.now += 1.0f; // past interactTimer
        game_player_interact(&game, player, 1, 0);
        const int opened = was_active && !gate->active;
        CHECK(opened == (kind != MIRROR), "%s: gate opened %d (active %d -> %d)", labels[kind], opened, was_active, gate->active);
        CHECK((player->score == 150) == (kind != MIRROR), "%s: score after purchase %d", labels[kind], player->score);
        CHECK((g_sent[NET_PACKET_OBJECT_STATE] > 0) == (kind != MIRROR), "%s: gte sent %d", labels[kind], g_sent[NET_PACKET_OBJECT_STATE]);
        CHECK((g_sent[NET_PACKET_MEDAL] > 0) == (kind != MIRROR), "%s: Purchased medal %d", labels[kind], g_sent[NET_PACKET_MEDAL]);
    }
}

int main(void) {
    cJSON *map = NULL;
    if (load_json_file("assets/maps/311_soul_sanctum.json", &map)) {
        printf("Failed to load assets/maps/311_soul_sanctum.json (run from the repository root)\n");
        return 1;
    }
    run(map, AUTH_LOCAL);
    run(map, AUTH_SERVER);
    run(map, MIRROR);
    if (g_failures) {
        printf("%d of %d checks failed\n", g_failures, g_checks);
        return 1;
    }
    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
