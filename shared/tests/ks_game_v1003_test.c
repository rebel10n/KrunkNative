// Krunker v10.0.3 KrunkScript items that KN has the API for (doc/v10/changelog-10.0.3.md):
//   - GAME.CONFIG.getMatch().started is false during the round countdown (and true once it is over);
//   - AI.spawn's alwaysSync option: the AI is synced to every player whatever its distance, where AI.sync
//     only sends the AIs within GAME_AI_SYNC_RANGE (800) of a player.
// The script runs through ks_game_load on a real Game with the server's API table (ks_game_api_fill),
// so the checks cover the host objects the server runs. Run from the repository root:
// ./<build>/ks_game_v1003_test
#include <ai.h>
#include <ks_game.h>
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

// ---------------------------------------------------------------- host: script log lines, sent packets

static char g_log[8][128];
static int g_log_count;

static void host_log(void *user, const int is_error, const char *line) {
    (void) user;
    (void) is_error;
    if (g_log_count < 8) snprintf(g_log[g_log_count++], sizeof(g_log[0]), "%s", line ? line : "");
}

typedef struct {
    int type;
    int uid;
} SentPacket;

static SentPacket g_sent[512];
static int g_sent_count;

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game;
    (void) payload;
    (void) length;
    if (g_sent_count < 512) {
        g_sent[g_sent_count].type = type;
        g_sent[g_sent_count].uid = uid;
        g_sent_count++;
    }
}

static int count_sent(const int type, const int uid) {
    int n = 0;
    for (int i = 0; i < g_sent_count; i++) n += g_sent[i].type == type && g_sent[i].uid == uid;
    return n;
}

// ---------------------------------------------------------------- game setup (as ramp_fix_test.c)

static cJSON *load_map(const char *path) {
    cJSON *map = NULL;
    if (load_json_file(path, &map)) {
        printf("Failed to load %s (run from the repository root)\n", path);
        exit(1);
    }
    return map;
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
    player->position = (vec3) {0.0f, 0.0f, 0.0f};
    player->last_position = player->position;
    return player;
}

// The server's KrunkScript host: the shared GAME.* entries (ks_game_api_fill) and the script log.
static KsApiImpl g_api[KS_API_COUNT];

static KsHost server_host(void) {
    memset(g_api, 0, sizeof(g_api));
    ks_game_api_fill(g_api, KS_SIDE_SERVER);
    KsHost host = {0};
    host.side = KS_SIDE_SERVER;
    host.log = host_log;
    host.api = g_api;
    return host;
}

// Loads `src` as the server script of `game` and runs its start().
static void run_script(Game *game, const char *src) {
    ks_game_unload(game);
    KsHost host = server_host();
    const int rc = ks_game_load(game, KS_SIDE_SERVER, src, strlen(src), &host, NULL);
    CHECK(rc == 0, "the script compiles and loads (rc %d)", rc);
    if (rc == 0) ks_game_start(game);
}

// ---------------------------------------------------------------- getMatch().started

static const char *k_match_script =
    "public action start() {\n"
    "    obj m = GAME.CONFIG.getMatch();\n"
    "    if (m.started) { GAME.log(\"started\"); } else { GAME.log(\"countdown\"); }\n"
    "}\n";

static void test_match_started(Game *game) {
    printf("Running getMatch().started test...\n");
    // the round countdown: a wait timer that is not contTime counts down before the match
    game->mode->wait_timers[0] = (ModeWaitTimer) {.time = 5.0f};
    game->mode->wait_timer_count = 1;
    g_log_count = 0;
    run_script(game, k_match_script);
    CHECK(g_log_count == 1 && !strcmp(g_log[0], "countdown"), "started is false during the countdown (log: %s)",
          g_log_count ? g_log[0] : "none");

    // the countdown is over: the match has started
    game->mode->wait_timer_count = 0;
    g_log_count = 0;
    run_script(game, k_match_script);
    CHECK(g_log_count == 1 && !strcmp(g_log[0], "started"), "started is true once the countdown is over (log: %s)",
          g_log_count ? g_log[0] : "none");
}

// ---------------------------------------------------------------- AI.spawn alwaysSync

static const char *k_spawn_script =
    "public action start() {\n"
    "    GAME.AI.spawn(\"1\", \"Synced\", 2000, 0, 0, {alwaysSync: true});\n"
    "    GAME.AI.spawn(\"1\", \"Plain\", 2000, 0, 0, {});\n"
    "}\n";

static GameAI *ai_named(Game *game, const char *name) {
    for (int i = 0; game->ai && i < game->ai->count; i++)
        if (!strcmp(game->ai->ais[i].name, name)) return &game->ai->ais[i];
    return NULL;
}

static void test_always_sync(Game *game) {
    printf("Running AI.spawn alwaysSync test...\n");
    // authority spawn: an AI 2000 units away from the only player (AI.sync's range is 800)
    add_player(game, 2);
    run_script(game, k_spawn_script);
    const GameAI *synced = ai_named(game, "Synced");
    const GameAI *plain = ai_named(game, "Plain");
    CHECK(synced && plain, "both AIs spawned (%d)", game->ai ? game->ai->count : -1);
    CHECK(synced && synced->data.always_sync, "AI.spawn {alwaysSync: true} sets the spawn option");
    CHECK(plain && !plain->data.always_sync, "AI.spawn without the option leaves it off");

    // the sync: as a server (not the in-process client), the far AI goes to the player and the plain one does not
    game->is_local = 0;
    static int server;
    game->server = &server;
    g_sent_count = 0;
    game_ai_sync(game);
    CHECK(count_sent(NET_PACKET_AI_ADD_V7, 2) == 1, "the alwaysSync AI is sent to the far player (%d aai)",
          count_sent(NET_PACKET_AI_ADD_V7, 2));
    CHECK(count_sent(NET_PACKET_AI_SYNC_V7, 2) == 1, "one ai list to the far player (%d)", count_sent(NET_PACKET_AI_SYNC_V7, 2));
    game->is_local = 1;
    game->server = NULL;
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    cJSON *map = load_map("shared/tests/maps/ramp_fix.json");
    static Game game;
    game_on(&game, map, MODE_ID_FFA);
    CHECK(game.ready && game.mode, "the test game is ready with a mode");

    test_match_started(&game);
    test_always_sync(&game);

    ks_game_unload(&game);
    printf("%d checks, %d failures\n", g_checks, g_failures);
    return g_failures ? 1 : 0;
}
