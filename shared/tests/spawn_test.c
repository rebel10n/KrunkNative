// Spawn point selection (shared/src/modes/mode_round.c game_get_spawn_point, Krunker v3.1.1
// game.js getSpawnPoint) on Burg, Kanji and Sandstorm, on the server and the offline
// authority: players spawning in the same tick get distinct points while the map has free
// ones (FFA, and teammates in TDM, which v3.1.1 never spreads), and each pick follows the
// v3.1.1 rules (fewest counted players with sight of it, then the largest summed distance to
// them and in flag modes to the other team's flags; counted = active, not you, enemies when
// teamed; your team's or teamless spawns; comp spawns only in a comp waitTimer, at the comp spawn itself: spread 10).
// Run from the repository root: ./<build>/spawn_test
#include <shared.h>
#include <ai.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

static int g_failures;
static int g_checks;

#define CHECK(cond, ...) do { \
    g_checks++; \
    if (!(cond)) { printf("FAIL %s:%d: ", __FILE__, __LINE__); printf(__VA_ARGS__); printf("\n"); g_failures++; } \
} while (0)

#define MAX_TEST_PLAYERS 32

static void test_send(Game *game, const int uid, const int type, const void *payload, const uint16_t length) {
    (void) game; (void) uid; (void) type; (void) payload; (void) length;
}

static void free_players(Game *game) {
    for (size_t i = 0; i < game->player_count; i++) {
        Player *player = game->players[i];
        free(player->loadout);
        free(player->ammo);
        free(player->reloads);
        free(player->input_queue);
        free(player);
    }
    free(game->players);
    game->players = NULL;
    game->player_count = 0;
}

// server = 1: a dedicated server's Game (game->server), else the offline client (is_local).
static void start(Game *game, const cJSON *map, const int mode_id, const int server) {
    free_players(game);
    int *modes = calloc(1, sizeof(int));
    modes[0] = mode_id;
    const cJSON **maps = calloc(1, sizeof(cJSON *)); // game_configure owns (frees) the lists
    maps[0] = map;
    game_configure(game, NULL, maps, 1, modes, 1, NULL, 0, NULL, 0);
    game->hooks.send = test_send;
    game_init(game, 0, 0, (unsigned char) !server);
    game->server = server ? game : NULL;
    game->now = 0.0f;
}

// players.add (team from the mode) without spawning; `team` > 0 overrides it.
static Player *add_player(Game *game, const int uid, const int team) {
    Player *player = player_init(game);
    player->uid = uid;
    snprintf(player->name, sizeof(player->name), "P%d", uid);
    game_players_add(game, player);
    if (team > 0) player->team = team;
    return player;
}

static int spawn_index_at(const Map *map, const vec3 position) {
    for (size_t s = 0; s < map->spawn_count; s++) {
        const vec3 p = map->spawns[s]->position;
        if (fabsf(p.x - position.x) < 1e-3f && fabsf(p.y - position.y) < 1e-3f && fabsf(p.z - position.z) < 1e-3f) return (int) s;
    }
    return -1;
}

// The spawns a player of `team` may get outside comp waitTimers: not comp, teamless or theirs.
static int eligible_count(const Map *map, const int team) {
    int count = 0;
    for (size_t s = 0; s < map->spawn_count; s++) {
        const Spawn *spawn = map->spawns[s];
        count += !spawn->comp && (!spawn->team || spawn->team == team);
    }
    return count;
}

static float dist3(const vec3 a, const vec3 b) {
    return sqrtf((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z));
}

// N players spawning back to back (one tick, no game_tick between): distinct spawn points
// while there are enough, every one a spawn the rules allow.
static void check_simultaneous(Game *game, const char *label, const int team, const int count) {
    const Map *map = game->map;
    int used[64] = {0};
    int distinct = 1;
    for (int i = 0; i < count; i++) {
        Player *player = add_player(game, 100 + i, team);
        player_spawn(player);
        const int index = spawn_index_at(map, player->position);
        CHECK(index >= 0, "%s: player %d spawned off a spawn point (%.1f %.1f %.1f)", label, i, player->position.x, player->position.y, player->position.z);
        if (index < 0) continue;
        const Spawn *spawn = map->spawns[index];
        CHECK(!spawn->comp, "%s: player %d got comp spawn %d outside a comp waitTimer", label, i, index);
        CHECK(!spawn->team || spawn->team == player->team, "%s: team %d player got team %d spawn %d", label, player->team, spawn->team, index);
        if (used[index]) distinct = 0;
        used[index]++;
    }
    CHECK(distinct, "%s: %d players in one tick shared a spawn point (map has %d eligible)", label, count, eligible_count(map, team > 0 ? team : 0));
}

// The v3.1.1 rule, written out on its own: candidates (free first), fewest counted players
// with sight of spawn + playerHeight, then the largest summed distance (+ other-team flags).
static void expected_spawns(const Game *game, const Player *player, int *out, int *out_count) {
    const Map *map = game->map;
    const int team = player->team;
    const float radius = 2.0f * game_constants.player_scale;
    int candidate[64] = {0}, seen[64] = {0};
    int any = 0;
    for (size_t s = 0; s < map->spawn_count; s++) {
        const Spawn *spawn = map->spawns[s];
        if (spawn->comp || (spawn->team && spawn->team != team)) continue;
        int occupied = 0;
        for (size_t i = 0; i < game->player_count; i++) {
            const Player *o = game->players[i];
            if (!o->active || o == player) continue;
            const float dx = o->position.x - spawn->position.x, dz = o->position.z - spawn->position.z;
            occupied |= dx * dx + dz * dz < radius * radius && fabsf(o->position.y - spawn->position.y) < game_constants.player_height;
        }
        candidate[s] = !occupied;
        any |= !occupied;
    }
    if (!any) for (size_t s = 0; s < map->spawn_count; s++) candidate[s] = !map->spawns[s]->comp && (!map->spawns[s]->team || map->spawns[s]->team == team);

    int min_seen = 1 << 30;
    for (size_t s = 0; s < map->spawn_count; s++) {
        if (!candidate[s]) continue;
        const vec3 target = {map->spawns[s]->position.x, map->spawns[s]->position.y + game_constants.player_height, map->spawns[s]->position.z};
        for (size_t i = 0; i < game->player_count; i++) {
            const Player *o = game->players[i];
            if (o->active && o != player && (!team || o->team != team) && game_ai_player_can_see(game, o, target)) seen[s]++;
        }
        if (seen[s] < min_seen) min_seen = seen[s];
    }

    float best = -1.0f;
    *out_count = 0;
    for (size_t s = 0; s < map->spawn_count; s++) {
        if (!candidate[s] || seen[s] != min_seen) continue;
        float d = 0.0f;
        for (size_t i = 0; i < game->player_count; i++) {
            const Player *o = game->players[i];
            if (o->active && o != player && (!team || o->team != team)) d += dist3(map->spawns[s]->position, o->position);
        }
        if (game->mode && game->mode->config.flags) {
            for (int f = 0; f < game->mode->flag_count; f++) {
                if (game->mode->flags[f].team != team) d += dist3(map->spawns[s]->position, game->mode->flags[f].position);
            }
        }
        if (d > best + 1e-3f) { best = d; *out_count = 0; }
        if (fabsf(d - best) <= 1e-3f) out[(*out_count)++] = (int) s;
    }
}

static int contains(const int *list, const int count, const int value) {
    for (int i = 0; i < count; i++) if (list[i] == value) return 1;
    return 0;
}

// Enemies parked on some spawns, then one more player spawns: it lands on the v3.1.1 pick.
static void check_rule(Game *game, const char *label, const int spawner_team, const int enemy_team) {
    const Map *map = game->map;
    int placed = 0;
    for (size_t s = 0; s < map->spawn_count && placed < 3; s += 3) {
        Player *enemy = add_player(game, 200 + (int) s, enemy_team);
        player_spawn(enemy);
        enemy->position = enemy->last_position = map->spawns[s]->position; // pinned for the check
        placed++;
    }
    Player *player = add_player(game, 299, spawner_team);
    player->active = 0; // not counted against itself (and not occupying) before it spawns
    int expected[64], expected_count = 0;
    expected_spawns(game, player, expected, &expected_count);
    player_spawn(player);
    const int index = spawn_index_at(map, player->position);
    CHECK(index >= 0 && contains(expected, expected_count, index), "%s: picked spawn %d, v3.1.1 rule gives %d (of %d)",
          label, index, expected_count ? expected[0] : -1, expected_count);
}

static void run_map(Game *game, const char *name, const cJSON *json) {
    for (int server = 0; server <= 1; server++) {
        char label[96];
        const char *side = server ? "server" : "offline";

        // FFA: every eligible spawn, one tick; then one more (all full: still a valid spawn)
        start(game, json, MODE_ID_FFA, server);
        CHECK(game->ready && game->map && game->map->spawn_count, "%s: map loaded", name);
        if (!game->ready || !game->map) return;
        const int ffa = eligible_count(game->map, 0);
        snprintf(label, sizeof(label), "%s %s FFA x%d", name, side, ffa);
        check_simultaneous(game, label, 0, ffa);
        Player *extra = add_player(game, 199, 0);
        player_spawn(extra);
        CHECK(spawn_index_at(game->map, extra->position) >= 0, "%s %s FFA: the player past the spawn count still gets a spawn", name, side);

        // TDM: a whole team respawning together (v3.1.1 ignores teammates: same point for all)
        start(game, json, MODE_ID_TDM, server);
        const int team1 = eligible_count(game->map, 1);
        for (int e = 0; e < 2; e++) {
            Player *enemy = add_player(game, 150 + e, 2);
            player_spawn(enemy);
        }
        snprintf(label, sizeof(label), "%s %s TDM team 1 x%d", name, side, team1 - 2);
        check_simultaneous(game, label, 1, team1 - 2); // the 2 enemies may hold 2 of them

        // the v3.1.1 selection rule
        start(game, json, MODE_ID_FFA, server);
        snprintf(label, sizeof(label), "%s %s FFA rule", name, side);
        if (server) check_rule(game, label, 0, 0);
        start(game, json, MODE_ID_TDM, server);
        snprintf(label, sizeof(label), "%s %s TDM rule", name, side);
        if (server) check_rule(game, label, 1, 2);
        start(game, json, MODE_ID_CTF, server);
        snprintf(label, sizeof(label), "%s %s CTF rule (%d flags)", name, side, game->mode ? game->mode->flag_count : 0);
        if (server) check_rule(game, label, 2, 1);

        // useCompSpawns (waitTimers[0].comp, e.g. warmup) in a team mode: your team's comp spawn, put by
        // getPointSpread(.., 10) (Krunker v10.0.3 deob line 217148). Its offset is 10 - 10 = 0, so the point
        // is the comp spawn itself, rounded to whole units (0.7072 at most). v7.1.4's spread 40 put players
        // up to 30 units off at random, so every sample here would fail with it.
        start(game, json, MODE_ID_TDM, server);
        GameMode *mode = game->mode;
        mode->wait_timers[0] = (ModeWaitTimer) {.time = 10.0f, .msg = "warmup ends ", .comp = 1};
        mode->wait_timer_count = 1;
        for (int team = 1; team <= 2; team++) {
            const Spawn *comp = NULL;
            for (size_t s = 0; s < game->map->spawn_count && !comp; s++) {
                if (game->map->spawns[s]->comp && game->map->spawns[s]->team == team) comp = game->map->spawns[s];
            }
            for (int k = 0; k < 6; k++) { // six respawns: each draws its own direction
                Player *player = add_player(game, 300 + team * 10 + k, team);
                player_spawn(player);
                if (!comp) continue;
                const float dx = player->position.x - comp->position.x, dz = player->position.z - comp->position.z;
                CHECK(sqrtf(dx * dx + dz * dz) <= 0.7072f && fabsf(player->position.y - comp->position.y) < 1e-3f,
                      "%s %s comp: team %d respawn %d spawned %.2f from its comp spawn", name, side, team, k, sqrtf(dx * dx + dz * dz));
            }
        }
    }
}

int main(void) {
    setvbuf(stdout, NULL, _IONBF, 0);
    static const char *names[] = {"311_burg", "311_kanji", "311_sandstorm"}; // the v3.1.1 maps
    static Game game;
    for (size_t m = 0; m < sizeof(names) / sizeof(names[0]); m++) {
        char path[256];
        cJSON *json = NULL;
        snprintf(path, sizeof(path), "assets/maps/%s.json", names[m]);
        if (load_json_file(path, &json)) {
            snprintf(path, sizeof(path), "../assets/maps/%s.json", names[m]);
            if (load_json_file(path, &json)) {
                printf("Failed to load assets/maps/%s.json (run from the repository root)\n", names[m]);
                return 1;
            }
        }
        // many rounds: the old pick was a coin toss, collisions need a few tries to show
        const int before = g_failures;
        for (int round = 0; round < 20; round++) run_map(&game, names[m], json);
        printf("  %-10s %s\n", names[m], g_failures == before ? "ok" : "FAILED");
    }

    if (g_failures) {
        printf("spawn_test: %d of %d check(s) failed\n", g_failures, g_checks);
        return 1;
    }
    printf("Test OK (%d checks)\n", g_checks);
    return 0;
}
